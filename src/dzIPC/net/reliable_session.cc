#include "dzIPC/net/reliable_session.h"
#include "byte_codec.h"
#include <algorithm>
#include <stdexcept>

namespace dzIPC::net {
struct ReliableSession::Impl {
    struct Target {
        ReliableTarget identity;
        std::uint32_t initial = 0;
        std::vector<std::uint64_t> retries;
        std::uint64_t probe_at = 0, backoff = 0, last_nack = 0;
        bool sent = false, acked = false;
    };
    WireHeader base;
    std::vector<Target> targets;
    std::vector<SendFragment> pending;
    std::size_t cursor = 0;
    std::uint64_t deadline, initial_retry, max_retry, nack_interval;
    std::optional<SendResultBody> terminal;
    ReliableStats stats;
    void finish(SendResultCode code) {
        if (terminal) return;
        SendResultBody result; result.publisher_id = base.publisher_id; result.sequence = base.sequence; result.result = code; result.target_count = targets.size();
        for (const auto& target : targets) { result.acked_count += target.acked; result.possible_remote_delivery |= target.sent; }
        terminal = result; pending.clear();
    }
    WireHeader header(std::size_t index) const {
        auto h = base; const auto& target = targets[index]; h.kind = PacketKind::Data; h.delivery = Delivery::Reliable;
        h.target_id = target.identity.peer.admission->hello.gateway_id; h.target_epoch = target.identity.peer.admission->hello.gateway_epoch;
        h.receiver_route_epoch = target.identity.route->descriptor.receiver_route_epoch; return h;
    }
    bool matches(const WireHeader& reply, const WireHeader& sent) const {
        return reply.route == sent.route && reply.source_id == sent.target_id && reply.source_epoch == sent.target_epoch &&
            reply.target_id == sent.source_id && reply.target_epoch == sent.source_epoch && reply.publisher_id == sent.publisher_id &&
            reply.sequence == sent.sequence && reply.receiver_route_epoch == sent.receiver_route_epoch && reply.encoding == sent.encoding &&
            reply.delivery == Delivery::Reliable && reply.schema_hash == sent.schema_hash && reply.message_size == sent.message_size &&
            reply.message_crc == sent.message_crc && reply.fragment_count == sent.fragment_count;
    }
    void tick(std::uint64_t now) {
        if (terminal) return;
        if (now >= deadline) { finish(SendResultCode::TimedOut); return; }
        if (targets.empty()) { finish(SendResultCode::NoSubscribers); return; }
        bool all = true;
        for (auto& target : targets) {
            if (target.acked) continue;
            all = false;
            if (!target.identity.peer.admission->active.load()) { finish(target.identity.peer.admission->retired.load() ? SendResultCode::PeerRestarted : SendResultCode::PeerGone); return; }
            if (!target.identity.route->active.load() || !target.identity.route->subscriber_active.load()) { finish(SendResultCode::Rejected); return; }
            if (base.message_size > target.identity.peer.admission->hello.max_message_bytes) { finish(SendResultCode::Rejected); return; }
            if (!target.acked && target.initial == base.fragment_count && target.probe_at && now >= target.probe_at) {
                target.retries[0] |= 1; target.probe_at = now + target.backoff; target.backoff = std::min(max_retry, target.backoff * 2);
            }
        }
        if (all) finish(SendResultCode::Completed);
    }
};
ReliableSession::ReliableSession(WireHeader base, std::vector<ReliableTarget> targets, std::uint64_t deadline,
    std::uint64_t initial_retry, std::uint64_t max_retry, std::uint64_t nack_interval) : impl_(new Impl) {
    if (!base.fragment_count || base.fragment_count > kMaxMessageBytes / 1024 || !deadline || !initial_retry || max_retry < initial_retry || !nack_interval) throw std::invalid_argument("可靠发送参数无效");
    impl_->base = base; impl_->deadline = deadline; impl_->initial_retry = initial_retry; impl_->max_retry = max_retry; impl_->nack_interval = nack_interval;
    impl_->targets.reserve(targets.size());
    for (auto& target : targets) {
        if (!target.peer.admission || !target.peer.snapshot || !target.route || !(target.route->descriptor.role_flags & 2)) throw std::invalid_argument("可靠目标未登记");
        Impl::Target entry; entry.identity = std::move(target); entry.retries.resize((base.fragment_count + 63) / 64); entry.backoff = initial_retry;
        impl_->targets.push_back(std::move(entry));
    }
}
ReliableSession::~ReliableSession() = default;
const std::vector<SendFragment>& ReliableSession::batch(std::uint64_t now, std::size_t maximum, std::size_t max_retries) {
    impl_->tick(now); if (impl_->terminal || !impl_->pending.empty() || !maximum) return impl_->pending;
    // 不改变已发送进度。临时位置只用于形成一个有限批次；失败后原批次保持原样。
    std::vector<std::uint32_t> initial; initial.reserve(impl_->targets.size());
    std::vector<std::uint32_t> retry_cursor(impl_->targets.size());
    for (const auto& target : impl_->targets) initial.push_back(target.initial);
    std::size_t idle = 0, retries = 0;
    while (impl_->pending.size() < maximum && idle < impl_->targets.size()) {
        const auto index = impl_->cursor++ % impl_->targets.size(); const auto& target = impl_->targets[index];
        if (target.acked) { ++idle; continue; }
        if (initial[index] < impl_->base.fragment_count) {
            impl_->pending.push_back({index, initial[index]++, false}); idle = 0; continue;
        }
        auto& f = retry_cursor[index];
        while (f < impl_->base.fragment_count) {
            const auto bits = target.retries[f / 64] & (~0ull << (f % 64));
            if (bits) { f = (f / 64) * 64 + __builtin_ctzll(bits); break; }
            f = (f / 64 + 1) * 64;
        }
        if (f < impl_->base.fragment_count && retries < max_retries) { impl_->pending.push_back({index, f++, true}); ++retries; idle = 0; }
        else ++idle;
    }
    return impl_->pending;
}
void ReliableSession::accepted(std::size_t prefix, std::uint64_t now) {
    if (prefix > impl_->pending.size()) throw std::invalid_argument("发送前缀超出批次");
    for (std::size_t n = 0; n < prefix; ++n) {
        const auto sent = impl_->pending[n]; auto& target = impl_->targets[sent.target]; target.sent = true;
        if (sent.retry) { target.retries[sent.fragment / 64] &= ~(1ull << (sent.fragment % 64)); ++impl_->stats.retry_packets; }
        else { ++target.initial; ++impl_->stats.original_packets; if (target.initial == impl_->base.fragment_count) target.probe_at = now + impl_->initial_retry; }
    }
    impl_->pending.erase(impl_->pending.begin(), impl_->pending.begin() + prefix);
}
void ReliableSession::control(const ReceivedDatagram& packet, std::uint64_t now) {
    impl_->tick(now); if (impl_->terminal) return;
    WireHeader header; ByteView body;
    if (packet.status != IoStatus::Data || packet.size > packet.bytes.size() || !decode_packet(packet.view(), header, body) || header.kind == PacketKind::Data) { ++impl_->stats.ignored_controls; return; }
    for (std::size_t i = 0; i < impl_->targets.size(); ++i) {
        auto& target = impl_->targets[i]; const auto& peer = target.identity.peer.admission;
        if (header.source_id != peer->hello.gateway_id) continue;
        if (!target.sent || packet.source.host != peer->ipv4 || packet.source.port != peer->hello.control_port || !impl_->matches(header, impl_->header(i))) { ++impl_->stats.ignored_controls; return; }
        if (target.acked) return;
        if (header.kind == PacketKind::Ack) {
            target.acked = true; ++impl_->stats.acks;
            // ACK 使待发的该目标后缀失效，其他目标的批次身份保持不变。
            auto& pending = impl_->pending;
            pending.erase(std::remove_if(pending.begin(), pending.end(), [&](const auto& f) { return f.target == i; }), pending.end());
        } else if (header.kind == PacketKind::Reject) { impl_->finish(SendResultCode::Rejected); return; }
        else {
            if (target.last_nack && now - target.last_nack < impl_->nack_interval) return;
            target.last_nack = now; ++impl_->stats.nacks;
            for (std::size_t offset = 0; offset < body.size; offset += 4) { const auto f = codec::get(body.data + offset, 4); if (f < target.initial) target.retries[f / 64] |= 1ull << (f % 64); }
            target.backoff = impl_->initial_retry; target.probe_at = now + impl_->initial_retry;
        }
        impl_->tick(now); return;
    }
    ++impl_->stats.ignored_controls;
}
void ReliableSession::cancel(SendResultCode code) { if (!impl_->terminal) impl_->finish(code); }
bool ReliableSession::has_initial_pending() const {
    if (impl_->terminal) return false;
    for (const auto& target : impl_->targets) if (!target.acked && target.initial < impl_->base.fragment_count) return true;
    return false;
}
void ReliableSession::tick(std::uint64_t now) { impl_->tick(now); }
WireHeader ReliableSession::header(const SendFragment& f) const { auto h = impl_->header(f.target); h.fragment_index = f.fragment; return h; }
Ipv4Address ReliableSession::destination(const SendFragment& f) const { const auto& peer = impl_->targets[f.target].identity.peer.admission; return {peer->ipv4, data_port(impl_->base.route, peer->hello.data_base_port, peer->hello.data_shards)}; }
std::optional<SendResultBody> ReliableSession::result() const { return impl_->terminal; }
ReliableStats ReliableSession::stats() const { return impl_->stats; }
} // namespace dzIPC::net

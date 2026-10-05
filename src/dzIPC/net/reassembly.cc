#include "dzIPC/net/reassembly.h"
#include "byte_codec.h"
#include <algorithm>
#include <map>
#include <mutex>
#include <optional>
#include <tuple>

namespace dzIPC::net {
namespace {
constexpr std::uint64_t lifetime_ns = 5000000000ull, best_effort_idle_ns = 500000000ull;
std::uint64_t after(std::uint64_t now, std::uint64_t delay) { return now > UINT64_MAX - delay ? UINT64_MAX : now + delay; }
struct StreamKey {
    RouteKey route; Identity source, publisher; std::uint64_t epoch, route_epoch;
    bool operator<(const StreamKey& b) const { return std::tie(route, source, epoch, publisher, route_epoch) < std::tie(b.route, b.source, b.epoch, b.publisher, b.route_epoch); }
};
struct Key {
    StreamKey stream; std::uint64_t sequence;
    bool operator<(const Key& b) const { return stream < b.stream || (!(b.stream < stream) && sequence < b.sequence); }
};
Key key_of(const WireHeader& h) { return {{h.route, h.source_id, h.publisher_id, h.source_epoch, h.receiver_route_epoch}, h.sequence}; }
bool same_message(const WireHeader& a, const WireHeader& b) {
    return a.route == b.route && a.source_id == b.source_id && a.source_epoch == b.source_epoch && a.target_id == b.target_id && a.target_epoch == b.target_epoch &&
        a.publisher_id == b.publisher_id && a.sequence == b.sequence && a.message_size == b.message_size && a.fragment_count == b.fragment_count &&
        a.message_crc == b.message_crc && a.schema_hash == b.schema_hash && a.receiver_route_epoch == b.receiver_route_epoch && a.encoding == b.encoding && a.delivery == b.delivery;
}
ReceiveFeedback feedback(const WireHeader& original, ReceiveDisposition disposition, PacketKind kind, const Bytes& payload = {}) {
    ReceiveFeedback result; result.original = original; result.disposition = disposition;
    if (original.delivery == Delivery::Reliable) {
        auto reply = original; std::swap(reply.source_id, reply.target_id); std::swap(reply.source_epoch, reply.target_epoch);
        reply.kind = kind; reply.fragment_index = 0; encode_packet(reply, ByteView(payload), result.control_packet);
    }
    return result;
}
ReceiveFeedback reject(const WireHeader& h, RejectReason reason) {
    Bytes body; codec::append(body, static_cast<std::uint32_t>(reason), 4); return feedback(h, ReceiveDisposition::Rejected, PacketKind::Reject, body);
}
}
struct ReassemblyBudget::Impl : std::enable_shared_from_this<Impl> {
    struct Peer { std::uint64_t bytes = 0, receipts = 0; };
    struct Claim {
        std::shared_ptr<Impl> pool; WireHeader header;
        std::uint64_t bytes = 0, bitmap = 0, stream_bytes = 0;
        bool buffer = false, receipt = false, pending = false, stream = false;
        ~Claim() { release_buffer(); release_receipt(); if (stream) { std::lock_guard<std::mutex> lock(pool->mutex); --pool->used.streams; pool->used.stream_bytes -= stream_bytes; } }
        void release_buffer() {
            if (!buffer) return;
            std::lock_guard<std::mutex> lock(pool->mutex); buffer = false;
            pool->used.bytes -= bytes; pool->used.bitmap_bytes -= bitmap; --pool->used.assemblies;
            if (pending) { pool->used.pending_bytes -= bytes; pending = false; }
            auto p = pool->peers.find(header.source_id); p->second.bytes -= bytes;
            if (!p->second.bytes && !p->second.receipts) pool->peers.erase(p);
            auto r = pool->routes.find(header.route); r->second -= bytes; if (!r->second) pool->routes.erase(r);
        }
        void release_receipt() {
            if (!receipt) return;
            std::lock_guard<std::mutex> lock(pool->mutex); receipt = false; --pool->used.receipts;
            auto p = pool->peers.find(header.source_id); --p->second.receipts;
            if (!p->second.bytes && !p->second.receipts) pool->peers.erase(p);
        }
        bool enter_pending() {
            std::lock_guard<std::mutex> lock(pool->mutex);
            if (pending) return true;
            if (bytes > pool->limits.commit_pending_bytes - pool->used.pending_bytes) { ++pool->used.quota_rejected; return false; }
            pending = true; pool->used.pending_bytes += bytes; return true;
        }
    };
    Limits limits; mutable std::mutex mutex; ReassemblyUsage used;
    std::map<Identity, Peer> peers; std::map<RouteKey, std::uint64_t> routes;
    std::shared_ptr<Claim> stream_claim() {
        auto c = std::make_shared<Claim>(); c->pool = shared_from_this();
        std::lock_guard<std::mutex> lock(mutex);
        if (used.streams >= limits.streams) { ++used.quota_rejected; return {}; }
        c->stream_bytes = 2 * ((limits.stream_window + 63) / 64) * 8; c->stream = true;
        ++used.streams; used.stream_bytes += c->stream_bytes; return c;
    }
    std::shared_ptr<Claim> message_claim(const WireHeader& h) {
        auto c = std::make_shared<Claim>(); c->pool = shared_from_this(); c->header = h;
        c->bytes = ((std::uint64_t(h.message_size) + 63) / 64) * 64; c->bitmap = ((std::uint64_t(h.fragment_count) + 63) / 64) * 8;
        const bool reliable = h.delivery == Delivery::Reliable;
        std::lock_guard<std::mutex> lock(mutex);
        const auto p = peers.find(h.source_id), end = peers.end(); const auto r = routes.find(h.route);
        const auto peer_bytes = p == end ? 0 : p->second.bytes, peer_receipts = p == end ? 0 : p->second.receipts;
        const auto route_bytes = r == routes.end() ? 0 : r->second;
        if (used.assemblies >= limits.assemblies || c->bytes > limits.reassembly_bytes - used.bytes ||
            c->bytes > limits.peer_reassembly_bytes - peer_bytes || c->bytes > limits.route_reassembly_bytes - route_bytes ||
            (reliable && (used.receipts >= limits.receipts || peer_receipts >= limits.peer_receipts))) { ++used.quota_rejected; return {}; }
        try { peers.try_emplace(h.source_id); routes.try_emplace(h.route); }
        catch (...) { auto pi = peers.find(h.source_id); if (pi != peers.end() && !pi->second.bytes && !pi->second.receipts) peers.erase(pi); throw; }
        c->buffer = true; c->receipt = reliable;
        ++used.assemblies; used.bytes += c->bytes; used.peak_bytes = std::max(used.peak_bytes, used.bytes); used.bitmap_bytes += c->bitmap;
        peers.at(h.source_id).bytes += c->bytes; routes.at(h.route) += c->bytes;
        if (reliable) { ++used.receipts; ++peers.at(h.source_id).receipts; }
        return c;
    }
};
ReassemblyBudget::ReassemblyBudget(Limits limits) : impl_(std::make_shared<Impl>()) {
    if (!limits.stream_window || limits.stream_window > 4096 || !limits.message_bytes || limits.message_bytes > kMaxMessageBytes) throw std::invalid_argument("重组配额无效");
    impl_->limits = limits;
}
ReassemblyBudget::~ReassemblyBudget() = default;
ReassemblyUsage ReassemblyBudget::usage() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->used; }

struct ReassemblyShard::Impl {
    using Pool = ReassemblyBudget::Impl;
    enum class Stage { Collecting, Pending };
    enum class Terminal { Unknown, Committed, Rejected };
    struct Stream {
        std::uint64_t highest = 0, width;
        std::shared_ptr<Pool::Claim> claim;
        std::vector<std::uint64_t> committed, rejected;
        Stream(std::uint64_t width, std::shared_ptr<Pool::Claim> claim) : width(width), claim(std::move(claim)), committed((width + 63) / 64), rejected((width + 63) / 64) {}
        bool stale(std::uint64_t sequence) const { return highest >= width && sequence <= highest - width; }
        void advance(std::uint64_t sequence) {
            if (sequence <= highest) return;
            const auto difference = sequence - highest;
            if (difference >= width) { std::fill(committed.begin(), committed.end(), 0); std::fill(rejected.begin(), rejected.end(), 0); }
            else for (std::uint64_t i = 1; i <= difference; ++i) { const auto slot = (highest + i) % width; committed[slot / 64] &= ~(1ull << (slot % 64)); rejected[slot / 64] &= ~(1ull << (slot % 64)); }
            highest = sequence;
        }
        Terminal state(std::uint64_t sequence) const {
            if (sequence > highest) return Terminal::Unknown;
            if (stale(sequence)) return Terminal::Rejected;
            const auto slot = sequence % width; const auto bit = 1ull << (slot % 64);
            if (committed[slot / 64] & bit) return Terminal::Committed;
            if (rejected[slot / 64] & bit) return Terminal::Rejected;
            return Terminal::Unknown;
        }
        void mark(std::uint64_t sequence, Terminal state) {
            if (stale(sequence)) return;
            const auto slot = sequence % width; const auto bit = 1ull << (slot % 64);
            (state == Terminal::Committed ? committed : rejected)[slot / 64] |= bit;
        }
    };
    struct Assembly {
        WireHeader header; ReceiveAdmission admission; std::shared_ptr<Pool::Claim> claim;
        std::unique_ptr<std::uint8_t[]> storage; WireBlob blob; std::vector<std::uint64_t> bits;
        std::uint64_t first, progress, deadline, next_nack, next_commit = 0;
        std::uint32_t count = 0, nack_cursor = 0; Stage stage = Stage::Collecting;
    };
    struct Receipt {
        WireHeader header; std::uint64_t expires = 0, last_reply = 0;
        Terminal state = Terminal::Unknown; RejectReason reason = RejectReason::BadMetadata;
        std::shared_ptr<Pool::Claim> claim;
    };
    Identity local; std::uint64_t epoch, nack_delay, nack_interval; unsigned shard, shards;
    std::shared_ptr<ReassemblyBudget> budget;
    std::map<StreamKey, Stream> streams;
    std::map<Key, Assembly> assemblies;
    std::map<Key, Receipt> receipts;
    std::optional<Key> assembly_cursor, receipt_cursor;
    ReassemblyStats stats;
    ReceiveFeedback failure(const WireHeader& h, RejectReason reason) { ++stats.rejected; return reject(h, reason); }
    bool authorized(const WireHeader& h, const ReceivedDatagram& packet, const ReceiveAdmission& a) const {
        if (!a.peer || !a.peer->active.load() || a.peer->hello.gateway_id != h.source_id || a.peer->hello.gateway_epoch != h.source_epoch || a.peer->ipv4 != packet.source.host || !a.peer->hello.data_shards || a.peer->hello.data_shards > 16) return false;
        const auto port = std::uint64_t(a.peer->hello.data_base_port) + route_hash(h.route) % a.peer->hello.data_shards;
        return port <= 65535 && packet.source.port == port;
    }
    bool route_valid(const WireHeader& h, const ReceiveAdmission& a) const {
        if (!a.publisher || !a.subscriber || !a.publisher->active.load() || !a.subscriber->active.load() || !a.publisher->publisher_active.load() || !a.subscriber->subscriber_active.load()) return false;
        const auto& p = a.publisher->descriptor; const auto& s = a.subscriber->descriptor;
        return p.key == h.route && s.key == h.route && p.topic == s.topic && (p.role_flags & 1) && (s.role_flags & 2) && s.receiver_route_epoch == h.receiver_route_epoch &&
            (h.encoding != Encoding::DzFlat || ((!p.schema_hash || p.schema_hash == h.schema_hash) && (!s.schema_hash || s.schema_hash == h.schema_hash)));
    }
    void expire_receipt(std::map<Key, Receipt>::iterator i) { i->second.claim->release_receipt(); receipts.erase(i); }
    ReceiveFeedback terminate(std::map<Key, Assembly>::iterator it, Terminal state, RejectReason reason, std::uint64_t now) {
        const auto h = it->second.header; streams.at(it->first.stream).mark(h.sequence, state);
        if (auto receipt = receipts.find(it->first); receipt != receipts.end()) { receipt->second.state = state; receipt->second.reason = reason; receipt->second.last_reply = now; }
        // 先释放 buffer，再归还其容量；可靠回执另持同一 claim 的回执部分。
        auto claim = it->second.claim; assemblies.erase(it); claim->release_buffer();
        if (state == Terminal::Committed) { ++stats.committed; return feedback(h, ReceiveDisposition::Committed, PacketKind::Ack); }
        return failure(h, reason);
    }
    ReceiveFeedback ingest(const ReceivedDatagram& packet, const ReceiveAdmission& admission, std::uint64_t now) {
        WireHeader h; ByteView payload;
        if (packet.status != IoStatus::Data || packet.size > packet.bytes.size() || !decode_packet(packet.view(), h, payload, budget->impl_->limits.message_bytes) || h.kind != PacketKind::Data) { ++stats.malformed; return {}; }
        if (h.target_id != local || h.target_epoch != epoch || !authorized(h, packet, admission)) return {};
        if (route_hash(h.route) % shards != shard) { ++stats.wrong_shard; return {}; }
        if (!route_valid(h, admission)) return failure(h, RejectReason::UnknownRoute);
        const auto key = key_of(h);
        auto receipt = receipts.find(key);
        if (receipt != receipts.end() && now >= receipt->second.expires) { expire_receipt(receipt); receipt = receipts.end(); }
        if (receipt != receipts.end() && receipt->second.state != Terminal::Unknown) {
            if (!same_message(receipt->second.header, h)) return failure(h, RejectReason::BadMetadata);
            ++stats.duplicates;
            if (now < after(receipt->second.last_reply, nack_interval)) return {ReceiveDisposition::Duplicate, h, {}};
            receipt->second.last_reply = now;
            return receipt->second.state == Terminal::Committed ? feedback(h, ReceiveDisposition::Duplicate, PacketKind::Ack) : reject(h, receipt->second.reason);
        }
        auto existing = assemblies.find(key);
        if (existing != assemblies.end()) {
            auto& assembly = existing->second;
            if (now >= assembly.deadline) return terminate(existing, Terminal::Rejected, RejectReason::ShmUnavailable, now);
            if (!same_message(h, assembly.header)) {
                if (assembly.stage == Stage::Pending) return failure(h, RejectReason::BadMetadata);
                return terminate(existing, Terminal::Rejected, RejectReason::BadMetadata, now);
            }
            if (assembly.stage == Stage::Pending) {
                if (std::memcmp(assembly.blob.view().data + std::size_t(h.fragment_index) * 1024, payload.data, payload.size)) return failure(h, RejectReason::BadMetadata);
                ++stats.duplicates; return {ReceiveDisposition::Duplicate, h, {}};
            }
        } else {
            auto stream = streams.find(key.stream);
            if (stream != streams.end() && stream->second.state(h.sequence) != Terminal::Unknown) { ++stats.duplicates; return h.delivery == Delivery::Reliable ? reject(h, RejectReason::ShmUnavailable) : ReceiveFeedback{ReceiveDisposition::Duplicate, h, {}}; }
            bool inserted_stream = false, inserted_receipt = false;
            try {
                std::shared_ptr<Pool::Claim> stream_claim;
                if (stream == streams.end()) { stream_claim = budget->impl_->stream_claim(); if (!stream_claim) return failure(h, RejectReason::QuotaExceeded); }
                auto claim = budget->impl_->message_claim(h); if (!claim) return failure(h, RejectReason::QuotaExceeded);
                Assembly a; a.header = h; a.admission = admission; a.claim = claim; a.first = a.progress = now;
                a.deadline = after(now, lifetime_ns); a.next_nack = after(now, nack_delay);
                a.storage.reset(new std::uint8_t[claim->bytes]); a.bits.resize((h.fragment_count + 63) / 64);
                if (stream == streams.end()) { stream = streams.try_emplace(key.stream, budget->impl_->limits.stream_window, std::move(stream_claim)).first; inserted_stream = true; }
                if (h.delivery == Delivery::Reliable) { receipts.emplace(key, Receipt{h, a.deadline, 0, Terminal::Unknown, RejectReason::BadMetadata, claim}); inserted_receipt = true; }
                existing = assemblies.emplace(key, std::move(a)).first; stream->second.advance(h.sequence);
            } catch (...) {
                if (inserted_receipt) receipts.erase(key);
                if (inserted_stream) streams.erase(key.stream);
                return failure(h, RejectReason::QuotaExceeded);
            }
        }
        auto& a = existing->second; const auto bit = 1ull << (h.fragment_index % 64); const auto word = h.fragment_index / 64;
        const auto offset = std::size_t(h.fragment_index) * 1024;
        if (a.bits[word] & bit) {
            if (std::memcmp(a.storage.get() + offset, payload.data, payload.size)) return terminate(existing, Terminal::Rejected, RejectReason::BadMetadata, now);
            ++stats.duplicates; return {ReceiveDisposition::Duplicate, h, {}};
        }
        std::memcpy(a.storage.get() + offset, payload.data, payload.size); a.bits[word] |= bit; ++a.count; a.progress = now;
        if (a.count != h.fragment_count) return {ReceiveDisposition::Accepted, h, {}};
        if (crc32c({a.storage.get(), h.message_size}) != h.message_crc) { ++stats.message_crc_fail; return terminate(existing, Terminal::Rejected, RejectReason::BadMetadata, now); }
        if (!a.claim->enter_pending()) return terminate(existing, Terminal::Rejected, RejectReason::QuotaExceeded, now);
        try { if (!WireEncoder::adopt(std::move(a.storage), h.message_size, a.claim->bytes, h.encoding, h.route.msg_id, h.schema_hash, a.blob)) return terminate(existing, Terminal::Rejected, RejectReason::BadMetadata, now); }
        catch (...) { return terminate(existing, Terminal::Rejected, RejectReason::QuotaExceeded, now); }
        a.stage = Stage::Pending; ++stats.completed; return {ReceiveDisposition::CommitPending, h, {}};
    }
    std::vector<ReceiveFeedback> tick(std::uint64_t now, const Committer& commit, std::size_t allowance) {
        std::vector<ReceiveFeedback> output; const auto count = std::min(allowance, assemblies.size()); output.reserve(count);
        for (std::size_t n = 0; n < count && !assemblies.empty(); ++n) {
            auto i = assembly_cursor ? assemblies.upper_bound(*assembly_cursor) : assemblies.begin(); if (i == assemblies.end()) i = assemblies.begin(); assembly_cursor = i->first;
            auto& a = i->second;
            if (now >= a.deadline || (a.header.delivery == Delivery::BestEffort && a.stage == Stage::Collecting && now >= after(a.progress, best_effort_idle_ns)) || !a.admission.peer->active.load() || !a.admission.subscriber->active.load() || !a.admission.publisher->active.load() || !a.admission.subscriber->subscriber_active.load() || !a.admission.publisher->publisher_active.load()) { ++stats.expired; output.push_back(terminate(i, Terminal::Rejected, RejectReason::ShmUnavailable, now)); continue; }
            if (a.stage == Stage::Pending) {
                if (now < a.next_commit) continue;
                ++stats.commit_attempts; SubmitState result;
                try { result = commit(a.header, a.blob); } catch (...) { result = SubmitState::Indeterminate; }
                if (result == SubmitState::Indeterminate) ++stats.commit_indeterminate;
                else if (result != SubmitState::Committed) ++stats.commit_not_submitted;
                if (result == SubmitState::Committed) output.push_back(terminate(i, Terminal::Committed, RejectReason::ShmUnavailable, now));
                else if (result == SubmitState::Indeterminate) output.push_back(terminate(i, Terminal::Rejected, RejectReason::ShmCommitIndeterminate, now));
                else if (a.header.delivery == Delivery::BestEffort) output.push_back(terminate(i, Terminal::Rejected, RejectReason::ShmUnavailable, now));
                else a.next_commit = after(now, nack_interval);
            } else if (a.header.delivery == Delivery::Reliable && now >= a.next_nack) {
                Bytes missing;
                for (std::uint32_t f = a.nack_cursor; f < a.header.fragment_count && missing.size() < 1024; ++f) { a.nack_cursor = f + 1; if (!(a.bits[f / 64] & (1ull << (f % 64)))) codec::append(missing, f, 4); }
                if (a.nack_cursor >= a.header.fragment_count) a.nack_cursor = 0;
                if (!missing.empty()) output.push_back(feedback(a.header, ReceiveDisposition::Accepted, PacketKind::Nack, missing));
                a.next_nack = after(now, nack_interval);
            }
        }
        const auto receipt_count = std::min(allowance, receipts.size());
        for (std::size_t n = 0; n < receipt_count && !receipts.empty(); ++n) {
            auto i = receipt_cursor ? receipts.upper_bound(*receipt_cursor) : receipts.begin(); if (i == receipts.end()) i = receipts.begin(); receipt_cursor = i->first;
            if (now >= i->second.expires) expire_receipt(i);
        }
        return output;
    }
    template<class Predicate> void retire(Predicate predicate) {
        for (auto i = assemblies.begin(); i != assemblies.end();) {
            if (predicate(i->first.stream)) { auto claim = i->second.claim; i = assemblies.erase(i); claim->release_buffer(); } else ++i;
        }
        for (auto i = receipts.begin(); i != receipts.end();) { if (predicate(i->first.stream)) { auto old = i++; expire_receipt(old); } else ++i; }
        for (auto i = streams.begin(); i != streams.end();) if (predicate(i->first)) i = streams.erase(i); else ++i;
    }
};
ReassemblyShard::ReassemblyShard(Identity id, std::uint64_t epoch, unsigned shard, unsigned shards, std::shared_ptr<ReassemblyBudget> budget, std::uint64_t nack_delay, std::uint64_t nack_interval) : impl_(new Impl) {
    if (!nonzero(id) || !epoch || !shards || shards > 16 || shard >= shards || !budget || !nack_delay || !nack_interval) throw std::invalid_argument("重组 shard 配置无效");
    impl_->local = id; impl_->epoch = epoch; impl_->shard = shard; impl_->shards = shards; impl_->budget = std::move(budget); impl_->nack_delay = nack_delay; impl_->nack_interval = nack_interval;
}
ReassemblyShard::~ReassemblyShard() = default;
ReceiveFeedback ReassemblyShard::ingest(const ReceivedDatagram& packet, const ReceiveAdmission& admission, std::uint64_t now) { return impl_->ingest(packet, admission, now); }
std::vector<ReceiveFeedback> ReassemblyShard::tick(std::uint64_t now, const Committer& commit, std::size_t budget) { return impl_->tick(now, commit, budget); }
void ReassemblyShard::retire_route(const std::shared_ptr<RouteAdmission>& route) {
    route->active.store(false); impl_->retire([&](const StreamKey& key) { return key.route == route->descriptor.key && key.route_epoch == route->descriptor.receiver_route_epoch; });
}
void ReassemblyShard::retire_peer_epoch(const std::shared_ptr<PeerAdmission>& peer) {
    peer->active.store(false); impl_->retire([&](const StreamKey& key) { return key.source == peer->hello.gateway_id && key.epoch == peer->hello.gateway_epoch; });
}
ReassemblyStats ReassemblyShard::stats() const { return impl_->stats; }
} // namespace dzIPC::net

#include "dzIPC/net/peer_directory.h"
#include "byte_codec.h"
#include <algorithm>
#include <mutex>
#include <optional>
#include <set>

namespace dzIPC::net {
namespace {
constexpr std::uint64_t lease_ns = 3000000000ull, candidate_ns = 2000000000ull;
bool equal(const RouteDescriptor& a, const RouteDescriptor& b) {
    return a.key == b.key && a.topic == b.topic && a.schema_hash == b.schema_hash &&
        a.role_flags == b.role_flags && a.receiver_route_epoch == b.receiver_route_epoch;
}
std::uint64_t size_of(const std::vector<RouteDescriptor>& routes) {
    std::uint64_t size = 4;
    for (const auto& r : routes) size += 52 + r.topic.size();
    return size;
}
}
bool compatible_routes(const RouteDescriptor& a, const RouteDescriptor& b) noexcept {
    return a.key == b.key && a.topic == b.topic && (!a.schema_hash || !b.schema_hash || a.schema_hash == b.schema_hash);
}
struct DirectoryBudget::Impl : std::enable_shared_from_this<Impl> {
    enum class Kind { Installed, Old, Candidate };
    struct Charge {
        std::shared_ptr<Impl> pool; Kind kind; std::uint64_t bytes;
        ~Charge() { if (bytes) { std::lock_guard<std::mutex> lock(pool->mutex); pool->counter(kind) -= bytes; } }
    };
    Limits limits; mutable std::mutex mutex; DirectoryUsage used;
    std::uint64_t& counter(Kind kind) { return kind == Kind::Installed ? used.installed : kind == Kind::Old ? used.old : used.candidates; }
    std::uint64_t limit(Kind kind) const { return kind == Kind::Installed ? limits.directory_bytes : kind == Kind::Old ? limits.old_directory_bytes : limits.candidate_bytes; }
    std::shared_ptr<Charge> reserve(Kind kind, std::uint64_t bytes) {
        auto c = std::make_shared<Charge>(); c->pool = shared_from_this(); c->kind = kind; c->bytes = 0;
        std::lock_guard<std::mutex> lock(mutex);
        if (bytes > limit(kind) - counter(kind)) return {};
        counter(kind) += bytes; c->bytes = bytes; return c;
    }
    bool retire(const std::shared_ptr<const DirectorySnapshot>& snapshot) {
        if (!snapshot) return true;
        auto charge = std::static_pointer_cast<Charge>(snapshot->charge);
        std::lock_guard<std::mutex> lock(mutex);
        if (charge->kind == Kind::Old) return true;
        if (charge->bytes > limits.old_directory_bytes - used.old) return false;
        used.installed -= charge->bytes; used.old += charge->bytes; charge->kind = Kind::Old; return true;
    }
};
DirectoryBudget::DirectoryBudget(Limits limits) : impl_(std::make_shared<Impl>()) { impl_->limits = limits; }
DirectoryBudget::~DirectoryBudget() = default;
DirectoryUsage DirectoryBudget::usage() const { std::lock_guard<std::mutex> lock(impl_->mutex); return impl_->used; }
std::shared_ptr<const DirectorySnapshot> DirectoryBudget::replace(const std::vector<RouteDescriptor>& entries,
        std::uint64_t version, const std::shared_ptr<const DirectorySnapshot>& old) {
    if (entries.size() > impl_->limits.topics || size_of(entries) > impl_->limits.peer_candidate_bytes ||
        size_of(entries) > 8 * kMiB || (old && version <= old->version)) return {};
    // 保守计费包含编码、解析条目、名称及树节点。新旧同时存在时先预留新表。
    const auto bytes = 2 * size_of(entries) + entries.size() * 512 + sizeof(DirectorySnapshot);
    auto charge = impl_->reserve(Impl::Kind::Installed, bytes); if (!charge) return {};
    auto next = std::make_shared<DirectorySnapshot>(); next->charge = charge; next->version = version;
    if (!encode_directory(entries, next->body)) return {};
    next->body_crc = crc32c(ByteView(next->body));
    std::map<Scope, const RouteDescriptor*> scopes;
    for (const auto& descriptor : entries) {
        // 一条实际 SHM 话题不能混用不同 msg_id/类型；全名始终保留。
        auto [scope, inserted] = scopes.emplace(descriptor.key.scope, &descriptor);
        if (!inserted && !compatible_routes(*scope->second, descriptor)) return {};
        auto previous = old ? old->routes.find(descriptor.key) : next->routes.end();
        if (old && previous != old->routes.end() && previous->second->active.load() && equal(previous->second->descriptor, descriptor))
            next->routes.emplace(descriptor.key, previous->second);
        else { auto route = std::make_shared<RouteAdmission>(); route->descriptor = descriptor; next->routes.emplace(descriptor.key, std::move(route)); }
    }
    if (!impl_->retire(old)) return {};
    if (old) for (const auto& [key, route] : old->routes) {
        auto found = next->routes.find(key);
        if (found == next->routes.end() || found->second != route) route->active.store(false);
    }
    return next;
}

struct PeerDirectory::Impl {
    struct Candidate {
        std::shared_ptr<DirectoryBudget::Impl::Charge> charge;
        CatalogHeader header; std::uint64_t expires = 0, absolute_deadline = 0;
        Bytes bytes, bits; std::uint32_t received = 0, last_size = 0;
    };
    struct Peer {
        PeerView view;
        std::set<std::uint64_t> history;
        std::unique_ptr<Candidate> candidate;
        std::uint64_t seen = 0, desired = 0, next_request = 0, retry = 100000000;
        bool expired = false, needs_snapshot = true;
    };
    Identity local; std::uint64_t epoch, revision = 1;
    std::shared_ptr<DirectoryBudget> budget;
    std::map<Identity, Peer> entries;
    std::optional<Identity> cursor;
    void invalidate(Peer& p) {
        p.view.admission->active.store(false);
        if (p.view.snapshot) for (const auto& [key, route] : p.view.snapshot->routes) route->active.store(false);
        p.view.snapshot.reset(); p.candidate.reset(); ++revision;
    }
    DirectoryCode hello(const DiscoveryHello& h, Ipv4Address source, std::uint64_t now) {
        Bytes checked; if (!source.host || !encode_hello(h, checked)) return DirectoryCode::Invalid;
        if (h.gateway_id == local) return h.gateway_epoch == epoch ? DirectoryCode::Ignored : DirectoryCode::IdentityConflict;
        auto found = entries.find(h.gateway_id);
        if (found == entries.end()) {
            auto& pool = *budget->impl_;
            if (entries.size() >= pool.limits.peers || pool.used.histories >= pool.limits.peer_history || !pool.limits.gateway_history) return DirectoryCode::HistoryFull;
            Peer p; p.view.admission = std::make_shared<PeerAdmission>(); p.view.admission->active.store(false);
            p.view.admission->hello = h; p.view.admission->ipv4 = source.host; p.history.insert(h.gateway_epoch);
            found = entries.emplace(h.gateway_id, std::move(p)).first;
            { std::lock_guard<std::mutex> lock(pool.mutex); ++pool.used.histories; }
        } else {
            auto& p = found->second; const auto& old = p.view.admission->hello;
            if (source.host != p.view.admission->ipv4) return DirectoryCode::IdentityConflict;
            if (h.gateway_epoch != old.gateway_epoch) {
                if (p.history.count(h.gateway_epoch)) return DirectoryCode::RetiredEpoch;
                auto& pool = *budget->impl_;
                if (p.history.size() >= pool.limits.gateway_history || pool.used.histories >= pool.limits.peer_history) return DirectoryCode::HistoryFull;
                // 新权限对象只在全部必要历史可保留后替换，旧对象绝不重新激活。
                auto admission = std::make_shared<PeerAdmission>(); admission->hello = h; admission->ipv4 = source.host; admission->active.store(false);
                if (!pool.retire(p.view.snapshot)) return DirectoryCode::QuotaExceeded;
                p.history.insert(h.gateway_epoch);
                { std::lock_guard<std::mutex> lock(pool.mutex); ++pool.used.histories; }
                invalidate(p); p.view.admission = std::move(admission); p.desired = 0; p.needs_snapshot = true; p.expired = false; p.next_request = 0;
            } else if (old.data_base_port != h.data_base_port || old.data_shards != h.data_shards || old.control_port != h.control_port || old.max_message_bytes != h.max_message_bytes)
                return DirectoryCode::IdentityConflict;
        }
        auto& p = found->second; p.seen = now;
        // HELLO 可能乱序；旧版本只续租，不回退期望目录版本。
        if (h.snapshot_version > p.desired) { p.desired = h.snapshot_version; p.next_request = 0; p.retry = 100000000; p.needs_snapshot = true; ++revision; }
        if (p.expired) { p.expired = false; p.needs_snapshot = true; p.next_request = 0; p.retry = 100000000; }
        return DirectoryCode::Ok;
    }
    DirectoryCode page(const ReceivedDatagram& packet, std::uint64_t now) {
        CatalogHeader h; ByteView body;
        if (packet.status != IoStatus::Data || packet.size > packet.bytes.size() || !decode_catalog(packet.view(), h, body) || h.kind != CatalogKind::Page) return DirectoryCode::Invalid;
        if (h.target_id != local || h.target_epoch != epoch) return DirectoryCode::Ignored;
        auto found = entries.find(h.source_id); if (found == entries.end()) return DirectoryCode::Ignored;
        auto& p = found->second; const auto& known = p.view.admission->hello;
        if (known.gateway_epoch != h.source_epoch || p.expired || now - p.seen >= lease_ns ||
            packet.source.host != p.view.admission->ipv4 || packet.source.port != known.control_port) return DirectoryCode::Ignored;
        if (h.snapshot_version < p.desired || (p.view.snapshot && h.snapshot_version < p.view.snapshot->version) ||
            (p.candidate && h.snapshot_version < p.candidate->header.snapshot_version)) return DirectoryCode::Stale;
        if (!p.needs_snapshot && p.view.snapshot && h.snapshot_version == p.view.snapshot->version) return DirectoryCode::Ignored;
        if (p.candidate && now >= p.candidate->expires) p.candidate.reset();
        if (!p.candidate || h.snapshot_version > p.candidate->header.snapshot_version) {
            const auto bytes = std::uint64_t(h.page_count) * 1024;
            if (bytes > budget->impl_->limits.peer_candidate_bytes) return DirectoryCode::QuotaExceeded;
            auto charge = budget->impl_->reserve(DirectoryBudget::Impl::Kind::Candidate, bytes + (h.page_count + 7) / 8);
            if (!charge) return DirectoryCode::QuotaExceeded;
            auto c = std::make_unique<Candidate>(); c->charge = std::move(charge); c->header = h; c->expires = now + candidate_ns; c->absolute_deadline = now + 30000000000ull;
            c->bytes.resize(bytes); c->bits.resize((h.page_count + 7) / 8); p.candidate = std::move(c);
        }
        auto& c = *p.candidate;
        if (c.header.page_count != h.page_count || c.header.body_crc != h.body_crc) { p.candidate.reset(); return DirectoryCode::Conflict; }
        const auto offset = std::size_t(h.page_index) * 1024; const auto bit = 1u << (h.page_index % 8);
        if (c.bits[h.page_index / 8] & bit) {
            if ((h.page_index + 1 == h.page_count && c.last_size != body.size) || std::memcmp(c.bytes.data() + offset, body.data, body.size)) { p.candidate.reset(); return DirectoryCode::Conflict; }
            return DirectoryCode::Incomplete;
        }
        std::memcpy(c.bytes.data() + offset, body.data, body.size); c.bits[h.page_index / 8] |= bit; ++c.received;
        c.expires = std::min(c.absolute_deadline, now + candidate_ns);
        if (h.page_index + 1 == h.page_count) c.last_size = body.size;
        if (c.received != h.page_count) return DirectoryCode::Incomplete;
        c.bytes.resize((h.page_count - 1) * 1024 + c.last_size);
        if (crc32c(ByteView(c.bytes)) != h.body_crc) { p.candidate.reset(); return DirectoryCode::Invalid; }
        std::vector<RouteDescriptor> routes;
        // 解析临时索引另计候选费用；先核对 count，再分配字符串/条目。
        const auto count = c.bytes.size() >= 4 ? codec::get(c.bytes.data(), 4) : UINT64_MAX;
        if (count > budget->impl_->limits.topics) { p.candidate.reset(); return DirectoryCode::QuotaExceeded; }
        auto parse_charge = budget->impl_->reserve(DirectoryBudget::Impl::Kind::Candidate, count * 512 + c.bytes.size());
        if (!parse_charge) { p.candidate.reset(); return DirectoryCode::QuotaExceeded; }
        if (!decode_directory(ByteView(c.bytes), routes, budget->impl_->limits.topics)) { p.candidate.reset(); return DirectoryCode::Invalid; }
        // 同版本恢复必须核验内容完全一致；快照版本不能被用来偷换目录。
        if (p.view.snapshot && h.snapshot_version == p.view.snapshot->version) {
            if (p.view.snapshot->body != c.bytes) { p.candidate.reset(); return DirectoryCode::Conflict; }
        } else {
            auto next = budget->replace(routes, h.snapshot_version, p.view.snapshot);
            if (!next) { p.candidate.reset(); return DirectoryCode::QuotaExceeded; }
            p.view.snapshot = std::move(next);
        }
        p.candidate.reset(); p.desired = h.snapshot_version; p.needs_snapshot = false; p.view.admission->active.store(true); ++revision;
        return DirectoryCode::Ok;
    }
};
PeerDirectory::PeerDirectory(Identity id, std::uint64_t epoch, std::shared_ptr<DirectoryBudget> budget) : impl_(new Impl) {
    if (!nonzero(id) || !epoch || !budget) throw std::invalid_argument("目录身份无效");
    impl_->local = id; impl_->epoch = epoch; impl_->budget = std::move(budget);
}
PeerDirectory::~PeerDirectory() {
    std::uint64_t count = 0;
    for (auto& [id, p] : impl_->entries) { p.view.admission->active.store(false); count += p.history.size(); }
    auto& pool = *impl_->budget->impl_; std::lock_guard<std::mutex> lock(pool.mutex); pool.used.histories -= count;
}
DirectoryCode PeerDirectory::hello(const DiscoveryHello& h, Ipv4Address source, std::uint64_t now) { return impl_->hello(h, source, now); }
DirectoryCode PeerDirectory::page(const ReceivedDatagram& p, std::uint64_t now) { return impl_->page(p, now); }
std::vector<CatalogRequest> PeerDirectory::tick(std::uint64_t now, std::size_t allowance) {
    std::vector<CatalogRequest> out;
    for (std::size_t i = 0, count = std::min(allowance, impl_->entries.size()); i < count; ++i) {
        auto it = impl_->cursor ? impl_->entries.upper_bound(*impl_->cursor) : impl_->entries.begin();
        if (it == impl_->entries.end()) it = impl_->entries.begin(); impl_->cursor = it->first;
        auto& p = it->second;
        if (now - p.seen >= lease_ns) {
            if (!p.expired) { p.expired = true; p.view.admission->active.store(false); p.candidate.reset(); p.needs_snapshot = true; ++impl_->revision; }
            continue;
        }
        if (p.candidate && now >= p.candidate->expires) p.candidate.reset();
        if (!p.needs_snapshot || now < p.next_request) continue;
        CatalogHeader h; h.source_id = impl_->local; h.source_epoch = impl_->epoch; h.target_id = it->first;
        h.target_epoch = p.view.admission->hello.gateway_epoch; h.snapshot_version = p.desired;
        CatalogRequest r; r.destination = {p.view.admission->ipv4, p.view.admission->hello.control_port};
        encode_catalog(h, {}, r.packet); out.push_back(std::move(r));
        p.next_request = now + p.retry; p.retry = std::min<std::uint64_t>(1000000000, p.retry * 2);
    }
    return out;
}
PeerView PeerDirectory::peer(const Identity& id) const { auto i = impl_->entries.find(id); return i == impl_->entries.end() ? PeerView{} : i->second.view; }
bool PeerDirectory::reachable(const Identity& id, std::uint64_t now) const { auto i = impl_->entries.find(id); return i != impl_->entries.end() && !i->second.expired && now - i->second.seen < lease_ns; }
std::vector<PeerView> PeerDirectory::peers() const { std::vector<PeerView> result; for (const auto& [id, p] : impl_->entries) result.push_back(p.view); return result; }
std::vector<PeerView> PeerDirectory::targets(const RouteDescriptor& route) const {
    std::vector<PeerView> result;
    for (const auto& [id, p] : impl_->entries) if (p.view.admission->active.load() && p.view.snapshot) {
        auto found = p.view.snapshot->routes.find(route.key);
        if (found != p.view.snapshot->routes.end() && (found->second->descriptor.role_flags & 2) && compatible_routes(route, found->second->descriptor)) result.push_back(p.view);
    }
    return result;
}
bool PeerDirectory::synchronized() const { for (const auto& [id, p] : impl_->entries) if (!p.expired && p.needs_snapshot) return false; return true; }
std::uint64_t PeerDirectory::revision() const { return impl_->revision; }
Bytes catalog_page(const DirectorySnapshot& snapshot, CatalogHeader h, std::uint32_t page) {
    Bytes result; const auto count = (snapshot.body.size() + 1023) / 1024; if (page >= count) return result;
    h.kind = CatalogKind::Page; h.snapshot_version = snapshot.version; h.page_index = page; h.page_count = count; h.body_crc = snapshot.body_crc;
    const auto offset = std::size_t(page) * 1024;
    encode_catalog(h, {snapshot.body.data() + offset, std::min<std::size_t>(1024, snapshot.body.size() - offset)}, result); return result;
}
} // namespace dzIPC::net

#include "dzIPC/net/local_directory.h"
#include <set>
#include <algorithm>
#include <stdexcept>

namespace dzIPC::net {
bool LocalRegistration::accept_sequence(std::uint64_t sequence) {
    if (!sequence || (highest_sequence >= 4096 && sequence <= highest_sequence - 4096)) return false;
    if (sequence > highest_sequence) {
        if (sequence - highest_sequence >= 4096) sequences.fill(0);
        else for (auto n = highest_sequence + 1; n <= sequence; ++n) { const auto slot = n % 4096; sequences[slot / 64] &= ~(1ull << (slot % 64)); if (n == UINT64_MAX) break; }
        highest_sequence = sequence;
    }
    const auto slot = sequence % 4096; const auto bit = 1ull << (slot % 64);
    if (sequences[slot / 64] & bit) return false; sequences[slot / 64] |= bit; return true;
}
struct LocalDirectory::Impl {
    struct Topic {
        std::shared_ptr<LocalBinding> binding;
        std::size_t pubs = 0, subs = 0, ready = 0;
        std::uint64_t epoch = 0;
    };
    std::shared_ptr<DirectoryBudget> budget; Limits limits;
    NetworkVersion wire_version = NetworkVersion::V1;
    std::function<void(RouteDescriptor&)> endpoint_resolver;
    std::map<Identity, std::shared_ptr<LocalRegistration>> handles;
    std::map<std::uint64_t, std::set<Identity>> used_ids;
    std::map<Scope, Topic> topics;
    std::shared_ptr<const DirectorySnapshot> current;
    std::uint64_t next_epoch = 1;
    bool healthy = true;
    void publish() {
        if (current->version == UINT64_MAX) throw std::runtime_error("目录版本耗尽");
        std::vector<RouteDescriptor> routes;
        for (const auto& [scope, topic] : topics) if (topic.pubs || topic.ready) {
            auto d = topic.binding->descriptor; d.role_flags = (topic.pubs ? 1 : 0) | (topic.ready ? 2 : 0);
            if (endpoint_resolver) endpoint_resolver(d);
            d.receiver_route_epoch = topic.ready ? topic.epoch : 0; routes.push_back(std::move(d));
        }
        Bytes checked;
        if (wire_version == NetworkVersion::V2 ? !encode_directory_v2(routes, checked)
                                                : !encode_directory(routes, checked))
            throw std::runtime_error("目录不可公告");
        if (checked == current->body) return;
        auto next = budget->replace(routes, current->version + 1, current, wire_version);
        if (!next) throw std::runtime_error("目录配额不足");
        current = std::move(next);
    }
};
LocalDirectory::LocalDirectory(std::shared_ptr<DirectoryBudget> budget, Limits limits,
                               NetworkVersion wire_version,
                               std::function<void(RouteDescriptor&)> endpoint_resolver) : impl_(new Impl) {
    impl_->budget = std::move(budget); impl_->limits = limits;
    impl_->wire_version = wire_version; impl_->endpoint_resolver = std::move(endpoint_resolver);
    impl_->current = impl_->budget->replace({}, 1, {}, wire_version);
    if (!impl_->current) throw std::runtime_error("空目录配额不足");
}
LocalDirectory::~LocalDirectory() { for (const auto& [id, registration] : impl_->handles) registration->active.store(false); for (const auto& [key, route] : impl_->current->routes) route->active.store(false); }
std::shared_ptr<LocalRegistration> LocalDirectory::add(std::uint64_t session, Identity id, RouteDescriptor descriptor, bool publisher) {
    if (!impl_->healthy) throw std::runtime_error("路由目录已失效");
    Bytes check;
    if (!session || !nonzero(id) || !encode_descriptor(descriptor, true, check)) throw std::invalid_argument("登记描述无效");
    const auto& limits = impl_->limits;
    auto& history = impl_->used_ids[session];
    if (impl_->handles.count(id) || history.count(id)) throw std::runtime_error("句柄已使用");
    if (impl_->handles.size() >= limits.handles) { impl_->budget->metrics()->reject(NetQuota::handles); throw std::runtime_error("登记配额不足"); }
    if (history.size() >= limits.session_handles) { impl_->budget->metrics()->reject(NetQuota::session_handles); throw std::runtime_error("登记配额不足"); }
    auto topic = impl_->topics.find(descriptor.key.scope); const bool fresh = topic == impl_->topics.end();
    if (fresh && impl_->topics.size() >= limits.topics) { impl_->budget->metrics()->reject(NetQuota::topics); throw std::runtime_error("话题配额不足"); }
    if (!fresh && !compatible_routes(topic->second.binding->descriptor, descriptor)) throw std::runtime_error("话题名称或类型冲突");
    std::uint64_t advertised = 4;
    for (const auto& [scope, value] : impl_->topics) advertised += 52 + value.binding->descriptor.topic.size();
    if (fresh) advertised += 52 + descriptor.topic.size();
    if (advertised > limits.peer_candidate_bytes || advertised > 8 * kMiB) { impl_->budget->metrics()->reject(NetQuota::peer_candidate_bytes); throw std::runtime_error("目录公告超过容量"); }
    if (fresh) { Impl::Topic entry; entry.binding = std::make_shared<LocalBinding>(); entry.binding->descriptor = descriptor; topic = impl_->topics.emplace(descriptor.key.scope, std::move(entry)).first; }
    auto& t = topic->second; const auto old_schema = t.binding->descriptor.schema_hash;
    if (!old_schema && descriptor.schema_hash) t.binding->descriptor.schema_hash = descriptor.schema_hash;
    auto registration = std::make_shared<LocalRegistration>(); registration->id = id; registration->session = session;
    registration->publisher = publisher; registration->binding = t.binding;
    bool added = false;
    try {
        history.insert(id); impl_->handles.emplace(id, registration); added = true;
        publisher ? ++t.pubs : ++t.subs;
        impl_->publish();
    } catch (...) {
        if (added) { publisher ? --t.pubs : --t.subs; impl_->handles.erase(id); }
        history.erase(id); t.binding->descriptor.schema_hash = old_schema;
        if (fresh) impl_->topics.erase(topic); throw;
    }
    impl_->budget->metrics()->peak(NetQuota::handles, impl_->handles.size());
    impl_->budget->metrics()->peak(NetQuota::session_handles, history.size());
    impl_->budget->metrics()->peak(NetQuota::topics, impl_->topics.size()); return registration;
}
bool LocalDirectory::set_bridge(const std::shared_ptr<LocalBinding>& binding, std::shared_ptr<ShmWireBridge> bridge) {
    auto i = impl_->topics.find(binding->descriptor.key.scope);
    if (i == impl_->topics.end() || i->second.binding != binding || !i->second.subs || !bridge || !bridge->generation()) return false;
    binding->generation = bridge->generation(); binding->bridge = std::move(bridge); return true;
}
std::uint64_t LocalDirectory::ready(std::uint64_t session, Identity id, std::uint32_t generation) {
    if (!impl_->healthy) throw std::runtime_error("路由目录已失效");
    auto registration = find(session, id);
    if (!registration || registration->publisher || !registration->binding->bridge || !generation || registration->binding->generation != generation || registration->binding->bridge->generation() != generation) throw std::runtime_error("订阅 generation 未就绪");
    auto& topic = impl_->topics.at(registration->binding->descriptor.key.scope);
    if (registration->ready) return topic.epoch;
    if (!topic.ready && impl_->next_epoch == UINT64_MAX) throw std::runtime_error("路由代次耗尽");
    const auto previous = topic.epoch;
    if (!topic.ready) topic.epoch = impl_->next_epoch++;
    ++topic.ready; registration->ready = true;
    try { impl_->publish(); }
    catch (...) { --topic.ready; registration->ready = false; topic.epoch = previous; throw; }
    return topic.epoch;
}
bool LocalDirectory::remove(std::uint64_t session, Identity id, bool publisher) {
    auto registration = find(session, id);
    if (!registration) return true;
    if (registration->publisher != publisher) return false;
    registration->active.store(false);
    auto i = impl_->topics.find(registration->binding->descriptor.key.scope); auto& topic = i->second;
    publisher ? --topic.pubs : --topic.subs;
    if (registration->ready) --topic.ready;
    try { if (impl_->healthy) impl_->publish(); }
    catch (...) {
        // 撤销不能因旧目录配额不足而留下有效订阅。失败封住整个目录，由运行时停止网关。
        impl_->healthy = false;
        for (const auto& [key, route] : impl_->current->routes) route->active.store(false);
    }
    impl_->handles.erase(id);
    if (!topic.ready) topic.epoch = 0;
    if (!topic.subs) { topic.binding->bridge.reset(); topic.binding->generation = 0; }
    if (!topic.pubs && !topic.subs) impl_->topics.erase(i);
    return true;
}
void LocalDirectory::close_session(std::uint64_t session) {
    std::vector<std::shared_ptr<LocalRegistration>> entries;
    for (const auto& [id, entry] : impl_->handles) if (entry->session == session) entries.push_back(entry);
    for (const auto& entry : entries) remove(session, entry->id, entry->publisher);
    impl_->used_ids.erase(session);
}
std::shared_ptr<LocalRegistration> LocalDirectory::find(std::uint64_t session, Identity id) const {
    auto i = impl_->handles.find(id); return i == impl_->handles.end() || i->second->session != session ? nullptr : i->second;
}
std::shared_ptr<LocalBinding> LocalDirectory::binding(const RouteKey& route) const {
    auto i = impl_->topics.find(route.scope); return i == impl_->topics.end() || i->second.binding->descriptor.key != route ? nullptr : i->second.binding;
}
std::vector<RouteKey> LocalDirectory::session_routes(std::uint64_t session) const {
    std::vector<RouteKey> result;
    for (const auto& [id, registration] : impl_->handles)
        if (registration->session == session && registration->binding)
            result.push_back(registration->binding->descriptor.key);
    return result;
}
std::vector<std::shared_ptr<LocalRegistration>> LocalDirectory::publishers() const {
    std::vector<std::shared_ptr<LocalRegistration>> out; for (const auto& [id, entry] : impl_->handles) if (entry->publisher) out.push_back(entry); return out;
}
std::vector<LocalRouteStatus> LocalDirectory::route_statuses() const {
    std::vector<LocalRouteStatus> out;
    out.reserve(impl_->topics.size());
    for (const auto& [scope, topic] : impl_->topics) {
        if (!topic.binding || (!topic.pubs && !topic.subs && !topic.ready)) continue;
        auto descriptor = topic.binding->descriptor;
        if (impl_->current) {
            const auto found = impl_->current->routes.find(descriptor.key);
            if (found != impl_->current->routes.end()) descriptor = found->second->descriptor;
        }
        if (impl_->endpoint_resolver) impl_->endpoint_resolver(descriptor);
        out.push_back({std::move(descriptor), topic.pubs, topic.subs, topic.ready});
    }
    return out;
}
std::shared_ptr<const DirectorySnapshot> LocalDirectory::snapshot() const { return impl_->current; }
std::size_t LocalDirectory::publisher_count() const { std::size_t count = 0; for (const auto& [key, t] : impl_->topics) count += t.pubs; return count; }
std::size_t LocalDirectory::ready_count() const { std::size_t count = 0; for (const auto& [key, t] : impl_->topics) count += t.ready; return count; }
std::size_t LocalDirectory::handle_count() const { return impl_->handles.size(); }
bool LocalDirectory::healthy() const { return impl_->healthy; }
} // namespace dzIPC::net

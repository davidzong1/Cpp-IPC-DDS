#include "gateway_data.h"
#include "local_control_linux.h"
#include <algorithm>
#include <deque>
#include <mutex>
#include <poll.h>
#include <thread>

namespace dzIPC::net {
struct GatewayData::Impl {
    struct Fence {
        std::atomic<unsigned> remaining;
        std::promise<void> promise;
        explicit Fence(unsigned count) : remaining(count) {}
        void arrive() { if (remaining.fetch_sub(1) == 1) promise.set_value(); }
    };
    struct Target { PeerView peer; std::shared_ptr<RouteAdmission> route; std::uint32_t fragment = 0; };
    struct Tx {
        OutboxRecord record;
        std::vector<Target> targets;
        std::size_t cursor = 0;
        std::uint64_t expires = 0;
        std::uint32_t crc = 0;
        bool sent = false;
    };
    struct Command {
        std::uint64_t charged_bytes = 0;
        std::shared_ptr<const GatewayDataView> view;
        std::shared_ptr<Fence> fence;
        std::unique_ptr<Tx> tx;
    };
    struct Shard {
        Impl& parent; unsigned index;
        local::Fd wake = local::event(); std::unique_ptr<DatagramEndpoint> endpoint;
        std::mutex mutex; std::deque<Command> commands;
        std::thread thread;
        std::shared_ptr<const GatewayDataView> view;
        ReassemblyShard receive;
        std::deque<std::unique_ptr<Tx>> sends;
        std::uint64_t send_blocked_until = 0;
        Shard(Impl& p, unsigned i, std::unique_ptr<DatagramEndpoint> e) : parent(p), index(i), endpoint(std::move(e)),
            receive(p.identity, p.epoch, i, p.config.data_shards, p.receive_budget, p.config.nack_delay_ms * 1000000, p.config.nack_interval_ms * 1000000) {}
        void update(std::shared_ptr<const GatewayDataView> next) {
            if (view) {
                for (const auto& [id, peer] : view->peers) {
                    auto now = next->peers.find(id);
                    if (now == next->peers.end() || now->second.admission != peer.admission) receive.retire_peer_epoch(peer.admission);
                }
                for (const auto& [key, route] : view->local->routes) if (route->descriptor.role_flags & 2) {
                    auto now = next->local->routes.find(key);
                    if (now == next->local->routes.end() || now->second->descriptor.receiver_route_epoch != route->descriptor.receiver_route_epoch)
                        receive.retire_route(route);
                }
            }
            view = std::move(next); // 旧 bridge 的最后引用必须先释放，再确认屏障。
        }
        void feedback(ReceiveFeedback f) {
            if (f.disposition == ReceiveDisposition::Committed) ++parent.committed_messages;
            if (f.control_packet.empty() || !view) return;
            auto peer = view->peers.find(f.original.source_id);
            if (peer == view->peers.end() || !peer->second.admission->active.load()) return;
            GatewayDataEvent event; event.kind = GatewayDataEvent::Kind::Feedback;
            event.destination = {peer->second.admission->ipv4, peer->second.admission->hello.control_port};
            event.packet = std::move(f.control_packet); parent.emit(std::move(event));
        }
        void incoming(const ReceivedDatagram& packet) {
            if (!view) return;
            WireHeader h; ByteView bytes;
            if (packet.status != IoStatus::Data || !decode_packet(packet.view(), h, bytes, parent.config.limits.message_bytes)) return;
            const auto p = view->peers.find(h.source_id); const auto s = view->local->routes.find(h.route);
            if (p == view->peers.end() || s == view->local->routes.end() || !p->second.snapshot) return;
            const auto pub = p->second.snapshot->routes.find(h.route); if (pub == p->second.snapshot->routes.end()) return;
            ReceiveAdmission admission{p->second.admission, pub->second, s->second, p->second.snapshot, view->local};
            feedback(receive.ingest(packet, admission, local::monotonic_ns()));
        }
        void finish(std::unique_ptr<Tx> tx, SendResultCode result) {
            GatewayDataEvent event; event.header = tx->record.header; event.result.publisher_id = event.header.publisher_id;
            event.result.sequence = event.header.sequence; event.result.result = result;
            event.result.target_count = tx->targets.size(); event.result.possible_remote_delivery = tx->sent;
            const auto targets = tx->targets.size(); tx.reset(); parent.target_states.fetch_sub(targets);
            if (result == SendResultCode::Completed) ++parent.sent_messages; else ++parent.rejected_records;
            parent.emit(std::move(event));
        }
        void sending() {
            const auto deadline = local::monotonic_ns() + parent.config.io_round_us * 1000;
            std::size_t packets = 0, bytes = 0;
            for (std::size_t visit = 0, count = sends.size(); visit < count && !sends.empty() && packets < parent.config.io_round_packets && bytes < parent.config.io_round_bytes && local::monotonic_ns() < deadline; ++visit) {
                auto tx = std::move(sends.front()); sends.pop_front();
                if (tx->record.header.delivery == Delivery::Reliable) { finish(std::move(tx), SendResultCode::Rejected); continue; }
                if (local::monotonic_ns() >= tx->expires) { finish(std::move(tx), SendResultCode::TimedOut); continue; }
                if (tx->targets.empty()) { finish(std::move(tx), SendResultCode::NoSubscribers); continue; }
                auto& target = tx->targets[tx->cursor];
                if (!target.peer.admission->active.load() || !target.route->active.load()) { finish(std::move(tx), SendResultCode::PeerGone); continue; }
                if (tx->record.blob.size() > target.peer.admission->hello.max_message_bytes) { finish(std::move(tx), SendResultCode::Rejected); continue; }
                WireHeader h; const auto& record = tx->record.header; const auto& blob = tx->record.blob;
                h.route = record.route; h.source_id = parent.identity; h.source_epoch = parent.epoch; h.publisher_id = record.publisher_id; h.sequence = record.sequence;
                h.target_id = target.peer.admission->hello.gateway_id; h.target_epoch = target.peer.admission->hello.gateway_epoch;
                h.receiver_route_epoch = target.route->descriptor.receiver_route_epoch; h.message_size = blob.size(); h.message_crc = tx->crc;
                h.fragment_count = (blob.size() + 1023) / 1024; h.encoding = blob.encoding(); h.schema_hash = blob.schema_hash();
                const auto batch_size = std::min<std::uint64_t>({parent.config.io_batch_max, h.fragment_count - target.fragment,
                    parent.config.io_round_packets - packets, std::max<std::uint64_t>(1, (parent.config.io_round_bytes - bytes) / 1184)});
                std::vector<Bytes> storage(batch_size); std::vector<OutgoingDatagram> outgoing(batch_size);
                for (unsigned n = 0; n < batch_size; ++n) {
                    h.fragment_index = target.fragment + n; const auto offset = std::size_t(h.fragment_index) * 1024;
                    encode_packet(h, {blob.view().data + offset, std::min<std::size_t>(1024, blob.size() - offset)}, storage[n]);
                    outgoing[n] = {{target.peer.admission->ipv4, data_port(h.route, target.peer.admission->hello.data_base_port, target.peer.admission->hello.data_shards)}, ByteView(storage[n])};
                }
                const auto result = endpoint->send_batch(outgoing.data(), outgoing.size());
                if (!result.count && result.status == IoStatus::WouldBlock) send_blocked_until = local::monotonic_ns() + 1000000;
                target.fragment += result.count; packets += result.count; parent.sent_packets += result.count;
                for (unsigned n = 0; n < result.count; ++n) bytes += storage[n].size();
                tx->sent |= result.count != 0;
                if (result.status == IoStatus::Fatal) { finish(std::move(tx), SendResultCode::Rejected); continue; }
                if (target.fragment == h.fragment_count && ++tx->cursor == tx->targets.size()) finish(std::move(tx), SendResultCode::Completed);
                else sends.push_back(std::move(tx));
            }
        }
        void run() noexcept {
            try {
                while (!parent.stopped.load()) {
                    for (unsigned count = 0; count < 64; ++count) {
                        Command command;
                        { std::lock_guard<std::mutex> lock(mutex); if (commands.empty()) break; command = std::move(commands.front()); commands.pop_front(); }
                        --parent.queued_commands;
                        parent.queued_bytes -= command.charged_bytes;
                        if (command.view) { update(std::move(command.view)); command.fence->arrive(); }
                        else if (command.tx) { command.tx->crc = crc32c(command.tx->record.blob.view()); sends.push_back(std::move(command.tx)); }
                    }
                    pollfd fds[]{{wake.get(), POLLIN, 0}, {endpoint->native_handle(), POLLIN, 0}};
                    // 不可因 EAGAIN 忙转；定时轮次保持短上界，不等待凑批。
                    const bool deferred = !sends.empty() && local::monotonic_ns() >= send_blocked_until;
                    if (::poll(fds, 2, deferred ? 0 : 1) < 0 && errno != EINTR) throw std::runtime_error("数据 shard 轮询失败");
                    if (fds[0].revents) local::drain_event(wake.get());
                    if (fds[1].revents & POLLIN) {
                        std::array<ReceivedDatagram, 32> input; const auto until = local::monotonic_ns() + parent.config.io_round_us * 1000;
                        std::size_t packets = 0, bytes = 0;
                        while (packets < parent.config.io_round_packets && bytes < parent.config.io_round_bytes && local::monotonic_ns() < until) {
                            const auto result = endpoint->receive_batch(input.data(), std::min<std::size_t>({input.size(), parent.config.io_batch_max, parent.config.io_round_packets - packets}));
                            if (!result.count) break;
                            for (unsigned n = 0; n < result.count; ++n) { incoming(input[n]); bytes += input[n].size; }
                            packets += result.count;
                        }
                    }
                    for (auto& result : receive.tick(local::monotonic_ns(), [&](const WireHeader& h, const WireBlob& blob) {
                        if (!view) return SubmitState::NotSubmitted;
                        const auto bridge = view->bridges->find(h.route); const auto route = view->local->routes.find(h.route);
                        if (bridge == view->bridges->end() || route == view->local->routes.end() || !route->second->active.load() || route->second->descriptor.receiver_route_epoch != h.receiver_route_epoch) return SubmitState::NotSubmitted;
                        return bridge->second->try_commit(blob);
                    })) feedback(std::move(result));
                    if (local::monotonic_ns() >= send_blocked_until) sending();
                }
            } catch (...) { parent.stopped.store(true); local::notify(parent.control_wake); }
            while (!sends.empty()) { auto tx = std::move(sends.front()); sends.pop_front(); finish(std::move(tx), SendResultCode::GatewayLost); }
            { std::lock_guard<std::mutex> lock(mutex); for (auto& command : commands) { --parent.queued_commands; parent.queued_bytes -= command.charged_bytes; if (command.tx) finish(std::move(command.tx), SendResultCode::GatewayLost); } commands.clear(); }
            view.reset();
        }
    };
    GatewayConfig config; Identity identity; std::uint64_t epoch; int control_wake;
    std::shared_ptr<ReassemblyBudget> receive_budget;
    std::vector<std::unique_ptr<Shard>> shards;
    std::atomic<bool> stopped{false};
    std::atomic<std::uint64_t> queued_commands{0}, target_states{0}, sent_messages{0}, sent_packets{0}, committed_messages{0}, rejected_records{0}, dropped_feedback{0};
    std::atomic<std::uint64_t> queued_bytes{0};
    std::mutex events_mutex; std::deque<GatewayDataEvent> events;
    std::uint64_t event_bytes = 0;
    void emit(GatewayDataEvent event) noexcept {
        try {
            std::lock_guard<std::mutex> lock(events_mutex);
            const auto bytes = sizeof(GatewayDataEvent) + event.packet.capacity();
            if (events.size() >= config.limits.command_records || bytes > config.limits.command_bytes - event_bytes) {
                if (event.kind == GatewayDataEvent::Kind::Feedback) ++dropped_feedback;
                else stopped.store(true);
            } else { events.push_back(std::move(event)); event_bytes += bytes; }
        } catch (...) { stopped.store(true); }
        local::notify(control_wake);
    }
    bool enqueue(unsigned index, Command& command) {
        if (stopped.load()) return false;
        auto count = queued_commands.load();
        do { if (count >= config.limits.command_records) return false; } while (!queued_commands.compare_exchange_weak(count, count + 1));
        const auto bytes = sizeof(Command) + (command.view ? command.view->peers.size() * 192 : sizeof(Tx) + command.tx->targets.size() * sizeof(Target));
        auto used = queued_bytes.load();
        do { if (bytes > config.limits.command_bytes - used) { --queued_commands; return false; } } while (!queued_bytes.compare_exchange_weak(used, used + bytes));
        command.charged_bytes = bytes;
        auto& shard = shards[index];
        try { std::lock_guard<std::mutex> lock(shard->mutex); shard->commands.push_back(std::move(command)); }
        catch (...) { --queued_commands; queued_bytes -= bytes; throw; }
        local::notify(shard->wake.get()); return true;
    }
};
GatewayData::GatewayData(const GatewayConfig& c, Identity id, std::uint64_t epoch, int wake, std::vector<std::unique_ptr<DatagramEndpoint>> endpoints) : impl_(new Impl) {
    impl_->config = c; impl_->identity = id; impl_->epoch = epoch; impl_->control_wake = wake;
    impl_->receive_budget = std::make_shared<ReassemblyBudget>(c.limits);
    if (endpoints.size() != c.data_shards) throw std::invalid_argument("数据端点数不匹配");
    for (unsigned i = 0; i < endpoints.size(); ++i) impl_->shards.push_back(std::make_unique<Impl::Shard>(*impl_, i, std::move(endpoints[i])));
    try { for (auto& shard : impl_->shards) shard->thread = std::thread([p = shard.get()] { p->run(); }); }
    catch (...) { stop(); throw; }
}
GatewayData::~GatewayData() { stop(); }
void GatewayData::stop() {
    impl_->stopped.store(true);
    for (auto& shard : impl_->shards) local::notify(shard->wake.get());
    for (auto& shard : impl_->shards) if (shard->thread.joinable()) shard->thread.join();
    for (auto& shard : impl_->shards) shard->endpoint.reset();
}
std::shared_future<void> GatewayData::synchronize(std::shared_ptr<const GatewayDataView> view) {
    auto fence = std::make_shared<Impl::Fence>(impl_->shards.size()); auto future = fence->promise.get_future().share();
    for (unsigned i = 0; i < impl_->shards.size(); ++i) {
        Impl::Command command; command.view = view; command.fence = fence;
        if (!impl_->enqueue(i, command)) { impl_->stopped.store(true); throw std::runtime_error("shard 目录命令达到上限"); }
    }
    return future;
}
bool GatewayData::submit(OutboxRecord& record, std::vector<PeerView> peers) {
    const auto size = peers.size(); auto used = impl_->target_states.load();
    do { if (size > impl_->config.limits.target_states - used) return false; } while (!impl_->target_states.compare_exchange_weak(used, used + size));
    try {
        auto tx = std::make_unique<Impl::Tx>(); tx->targets.reserve(size);
        for (auto& peer : peers) { auto route = peer.snapshot->routes.at(record.header.route); tx->targets.push_back({std::move(peer), std::move(route), 0}); }
        tx->expires = local::monotonic_ns() + 500000000ull;
        tx->record = std::move(record); Impl::Command command; command.tx = std::move(tx);
        const auto index = route_hash(command.tx->record.header.route) % impl_->config.data_shards;
        if (!impl_->enqueue(index, command)) { record = std::move(command.tx->record); impl_->target_states -= size; return false; }
        return true;
    } catch (...) { impl_->target_states -= size; throw; }
}
bool GatewayData::pop(GatewayDataEvent& event) { std::lock_guard<std::mutex> lock(impl_->events_mutex); if (impl_->events.empty()) return false; impl_->event_bytes -= sizeof(GatewayDataEvent) + impl_->events.front().packet.capacity(); event = std::move(impl_->events.front()); impl_->events.pop_front(); return true; }
bool GatewayData::healthy() const { return !impl_->stopped.load(); }
GatewayDataStats GatewayData::stats() const { return {impl_->sent_messages.load(), impl_->sent_packets.load(), impl_->committed_messages.load(), impl_->rejected_records.load(), impl_->dropped_feedback.load(), impl_->target_states.load(), impl_->receive_budget->usage()}; }
} // namespace dzIPC::net

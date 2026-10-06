#include "gateway_data.h"
#include "local_control_linux.h"
#include "dzIPC/threepools/socket_wait_set.h"
#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <thread>
#include <unordered_map>
#include <unordered_set>

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
        Impl* quota_owner = nullptr;
        Identity charged_publisher{};
        bool publisher_charged = false;
        std::uint64_t target_charge = 0;
        ~Tx() {
            if (!quota_owner) return;
            quota_owner->target_states.fetch_sub(target_charge);
            if (publisher_charged) {
                std::lock_guard<std::mutex> lock(quota_owner->quota_mutex);
                auto found = quota_owner->publisher_reliable.find(charged_publisher);
                if (found != quota_owner->publisher_reliable.end() && !--found->second)
                    quota_owner->publisher_reliable.erase(found);
            }
        }
        OutboxRecord record;
        std::shared_ptr<LocalRegistration> source;
        std::vector<Target> targets;
        unsigned socket_index = 0;
        std::size_t cursor = 0;
        std::uint64_t expires = 0, first_send_ns = 0;
        std::uint32_t crc = 0;
        bool sent = false;
        std::unique_ptr<ReliableSession> reliable;
    };
    struct Command {
        std::uint64_t charged_bytes = 0;
        std::shared_ptr<const GatewayDataView> view;
        std::shared_ptr<Fence> fence;
        std::shared_ptr<std::promise<bool>> endpoint_result;
        std::unique_ptr<Tx> tx;
        std::unique_ptr<ReceivedDatagram> control;
        enum class EndpointOperation { Add, Remove } endpoint_operation = EndpointOperation::Add;
        std::unique_ptr<DatagramEndpoint> endpoint;
        std::uint16_t endpoint_port = 0;
        std::size_t endpoint_slot = 0;
        bool endpoint_command = false;
    };
    struct Shard {
        Impl& parent; unsigned index;
        local::Fd wake = local::event();
        threepools::SocketWaitSet wait_set;
        threepools::SocketWaitToken wake_token;
        std::vector<std::unique_ptr<DatagramEndpoint>> endpoints;
        std::vector<unsigned> socket_indices;
        std::unordered_map<const DatagramEndpoint*, std::size_t> endpoint_slots_by_owner;
        std::deque<std::size_t> ready_sockets;
        std::unordered_set<std::size_t> ready_socket_set;
        mutable std::mutex endpoint_metrics_mutex;
        std::atomic<std::size_t> active_socket_count{0};
        std::mutex mutex; std::deque<Command> commands;
        std::thread thread;
        mutable std::mutex stats_mutex; ReassemblyStats snapshot;
        std::atomic<std::uint64_t> rx_bytes{0}, tx_bytes{0}, wakeups{0}, budget_yields{0}, queued{0}, queued_peak{0};
        void capture() {
            std::lock_guard<std::mutex> lock(stats_mutex);
            snapshot = {};
            for (const auto& receive : receives) if (receive) {
                const auto current = receive->stats();
                snapshot.malformed += current.malformed; snapshot.wrong_shard += current.wrong_shard;
                snapshot.rejected += current.rejected; snapshot.duplicates += current.duplicates;
                snapshot.completed += current.completed; snapshot.committed += current.committed;
                snapshot.commit_attempts += current.commit_attempts;
                snapshot.commit_not_submitted += current.commit_not_submitted;
                snapshot.commit_indeterminate += current.commit_indeterminate;
                snapshot.expired += current.expired; snapshot.message_crc_fail += current.message_crc_fail;
            }
        }
        std::shared_ptr<const GatewayDataView> view;
        std::vector<std::unique_ptr<ReassemblyShard>> receives;
        std::map<RouteKey, std::deque<std::unique_ptr<Tx>>> sends;
        std::map<unsigned, std::deque<RouteKey>> send_routes_by_socket;
        std::deque<unsigned> ready_send_sockets;
        std::set<unsigned> ready_send_socket_set;
        std::set<RouteKey> scheduled_send_routes;
        std::size_t send_count = 0;
        struct DeferredReceive { ReceivedDatagram packet; WireHeader header; unsigned socket_index = 0; };
        std::map<RouteKey, std::deque<DeferredReceive>> deferred_receive;
        std::deque<RouteKey> ready_receive_routes;
        std::set<RouteKey> scheduled_receive_routes;
        std::size_t deferred_receive_count = 0, deferred_receive_bytes = 0;
        std::multimap<std::uint64_t, std::pair<std::size_t, std::uint64_t>> receive_tick_queue;
        std::vector<std::uint64_t> receive_tick_generation, receive_tick_deadline;
        std::map<unsigned, std::uint64_t> send_blocked_until;
        std::uint64_t send_progress_at = 0;
        void queue_send(std::unique_ptr<Tx> tx) {
            const auto key = tx->record.header.route; const auto socket = tx->socket_index; auto& queue = sends[key];
            const bool empty = queue.empty();
            try {
                queue.push_back(std::move(tx));
                try { if (empty) schedule_send_route(socket, key); }
                catch (...) { queue.pop_back(); throw; }
            } catch (...) { if (queue.empty()) sends.erase(key); throw; }
            ++send_count; queued.store(send_count); auto peak = queued_peak.load(); if (send_count > peak) queued_peak.store(send_count);
        }
        void schedule_send_socket(unsigned socket) {
            if (ready_send_socket_set.count(socket)) return;
            ready_send_sockets.push_back(socket);
            try { ready_send_socket_set.insert(socket); }
            catch (...) { ready_send_sockets.pop_back(); throw; }
        }
        void schedule_send_route(unsigned socket, const RouteKey& key) {
            if (scheduled_send_routes.count(key)) return;
            auto& routes = send_routes_by_socket[socket];
            routes.push_back(key);
            try {
                scheduled_send_routes.insert(key);
                schedule_send_socket(socket);
            } catch (...) {
                scheduled_send_routes.erase(key);
                routes.pop_back();
                if (routes.empty()) send_routes_by_socket.erase(socket);
                throw;
            }
        }
        std::unique_ptr<Tx> pop_send(std::uint64_t now) {
            const auto visits = ready_send_sockets.size();
            for (std::size_t n = 0; n < visits; ++n) {
                const auto socket = ready_send_sockets.front(); ready_send_sockets.pop_front();
                ready_send_socket_set.erase(socket);
                const auto blocked = send_blocked_until.find(socket);
                if (blocked != send_blocked_until.end() && blocked->second > now) {
                    schedule_send_socket(socket);
                    continue;
                }
                auto routes = send_routes_by_socket.find(socket);
                if (routes == send_routes_by_socket.end() || routes->second.empty()) {
                    if (routes != send_routes_by_socket.end()) send_routes_by_socket.erase(routes);
                    continue;
                }
                const auto key = routes->second.front(); routes->second.pop_front();
                scheduled_send_routes.erase(key);
                if (!routes->second.empty()) schedule_send_socket(socket);
                else send_routes_by_socket.erase(routes);
                const auto found = sends.find(key);
                if (found == sends.end() || found->second.empty()) continue;
                auto tx = std::move(found->second.front()); found->second.pop_front();
                --send_count; queued.store(send_count);
                if (found->second.empty()) sends.erase(found);
                else schedule_send_route(socket, key);
                return tx;
            }
            return {};
        }
        void rebuild_send_schedule() {
            send_routes_by_socket.clear(); ready_send_sockets.clear(); ready_send_socket_set.clear(); scheduled_send_routes.clear();
            for (const auto& [key, queue] : sends) if (!queue.empty())
                schedule_send_route(queue.front()->socket_index, key);
        }
        void enqueue_ready_socket(std::size_t slot) {
            if (slot >= endpoints.size() || !endpoints[slot] || ready_socket_set.count(slot)) return;
            ready_sockets.push_back(slot);
            try { ready_socket_set.insert(slot); }
            catch (...) { ready_sockets.pop_back(); throw; }
        }
        void enqueue_received(ReceivedDatagram&& packet, const WireHeader& header, unsigned socket_index) {
            const auto key = header.route;
            const auto max_records = std::max<std::uint64_t>(parent.config.io_round_packets, parent.config.io_batch_max);
            const auto max_bytes = max_records * kMaxDatagramBytesV2;
            if (deferred_receive_count >= max_records || packet.size > max_bytes - deferred_receive_bytes)
                throw std::runtime_error("数据 worker deferred 收包队列超过预算");
            auto& queue = deferred_receive[key];
            const bool empty = queue.empty();
            queue.push_back({std::move(packet), header, socket_index});
            try {
                if (empty) {
                    scheduled_receive_routes.insert(key);
                    ready_receive_routes.push_back(key);
                }
            }
            catch (...) {
                scheduled_receive_routes.erase(key);
                queue.pop_back(); if (queue.empty()) deferred_receive.erase(key); throw;
            }
            ++deferred_receive_count;
            deferred_receive_bytes += queue.back().packet.size;
        }
        std::size_t process_deferred_receive(std::size_t allowance, std::uint64_t deadline) {
            std::size_t processed = 0, bytes = 0;
            while (!ready_receive_routes.empty() && processed < allowance && bytes < parent.config.io_round_bytes && local::monotonic_ns() < deadline) {
                const auto key = ready_receive_routes.front(); ready_receive_routes.pop_front();
                scheduled_receive_routes.erase(key);
                auto found = deferred_receive.find(key);
                if (found == deferred_receive.end() || found->second.empty()) continue;
                auto item = std::move(found->second.front()); found->second.pop_front();
                --deferred_receive_count; deferred_receive_bytes -= item.packet.size;
                incoming(item.packet, item.socket_index, &item.header);
                ++processed; bytes += item.packet.size;
                if (found->second.empty()) deferred_receive.erase(found);
                else if (scheduled_receive_routes.insert(key).second) ready_receive_routes.push_back(key);
            }
            if (!ready_receive_routes.empty() && (processed >= allowance || bytes >= parent.config.io_round_bytes)) {
                ++budget_yields; parent.metrics->add(NetMetric::shard_budget_yields);
            }
            return processed;
        }
        void refresh_receive_tick(std::size_t slot, std::uint64_t now) {
            if (slot >= receives.size() || !receives[slot]) return;
            if (receive_tick_generation.size() <= slot) {
                receive_tick_generation.resize(slot + 1);
                receive_tick_deadline.resize(slot + 1);
            }
            const auto wait_ms = receives[slot]->idle_wait_ms();
            if (wait_ms < 0) {
                if (receive_tick_deadline[slot]) ++receive_tick_generation[slot];
                receive_tick_deadline[slot] = 0;
                return;
            }
            const auto due = now + static_cast<std::uint64_t>(wait_ms) * 1000000;
            if (receive_tick_deadline[slot] && receive_tick_deadline[slot] <= due) return;
            receive_tick_deadline[slot] = due;
            const auto generation = ++receive_tick_generation[slot];
            receive_tick_queue.emplace(due, std::make_pair(slot, generation));
        }
        bool prune_receive_tick_queue() {
            while (!receive_tick_queue.empty()) {
                const auto& first = *receive_tick_queue.begin();
                const auto slot = first.second.first;
                if (slot < receive_tick_generation.size() &&
                    receive_tick_generation[slot] == first.second.second &&
                    receive_tick_deadline[slot] == first.first && slot < receives.size() && receives[slot])
                    return true;
                receive_tick_queue.erase(receive_tick_queue.begin());
            }
            return false;
        }
        void tick_receive_shard(std::size_t slot, std::uint64_t now) {
            if (slot >= receives.size() || !receives[slot]) return;
            for (auto& result : receives[slot]->tick(now, [&](const WireHeader& h, const WireBlob& blob) {
                if (!view) return SubmitState::NotSubmitted;
                const auto bridge = view->bridges->find(h.route); const auto route = view->local->routes.find(h.route);
                if (bridge == view->bridges->end() || route == view->local->routes.end() || !route->second->active.load() || route->second->descriptor.receiver_route_epoch != h.receiver_route_epoch) return SubmitState::NotSubmitted;
                return bridge->second->try_commit(blob);
            })) feedback(std::move(result));
            refresh_receive_tick(slot, local::monotonic_ns());
        }
        void process_due_receive_ticks(std::uint64_t now, std::uint64_t deadline) {
            std::size_t processed = 0;
            while (processed < parent.config.io_round_packets && local::monotonic_ns() < deadline && prune_receive_tick_queue()) {
                auto entry = receive_tick_queue.begin();
                if (entry->first > now) break;
                const auto slot = entry->second.first;
                receive_tick_deadline[slot] = 0;
                receive_tick_queue.erase(entry);
                tick_receive_shard(slot, now);
                ++processed;
            }
        }
        void stage_received(ReceivedDatagram&& packet, unsigned socket_index) {
            if (packet.status != IoStatus::Data) { incoming(packet, socket_index); return; }
            WireHeader header; ByteView payload;
            const auto decoded = parent.config.network_version == NetworkVersion::V2
                ? decode_packet_v2(packet.view(), header, payload, parent.config.limits.message_bytes)
                : decode_packet(packet.view(), header, payload, parent.config.limits.message_bytes);
            if (!decoded) { incoming(packet, socket_index); return; }
            enqueue_received(std::move(packet), header, socket_index);
        }
        std::map<std::pair<Identity, std::uint64_t>, Tx*> active;
        Shard(Impl& p, unsigned i, std::vector<std::unique_ptr<DatagramEndpoint>> e, std::vector<unsigned> slots)
            : parent(p), index(i), endpoints(std::move(e)), socket_indices(std::move(slots)) {
            if (!threepools::SocketWaitSet::backend_available())
                throw std::runtime_error("数据 worker 缺少可阻塞多 FD 等待后端");
            wake_token = {this, static_cast<std::uintptr_t>(wake.get())};
            if (!wait_set.add(wake_token)) throw std::runtime_error("数据 worker 注册命令唤醒 FD 失败");
            receives.reserve(socket_indices.size());
            for (std::size_t slot = 0; slot < socket_indices.size(); ++slot) {
                if (endpoints[slot]) {
                    const auto token = threepools::SocketWaitToken{endpoints[slot].get(), static_cast<std::uintptr_t>(endpoints[slot]->native_handle())};
                    if (!wait_set.add(token)) throw std::runtime_error("数据 worker 注册数据 FD 失败");
                    endpoint_slots_by_owner.emplace(endpoints[slot].get(), slot);
                }
                const auto global = socket_indices[slot];
                receives.push_back(std::make_unique<ReassemblyShard>(p.identity, p.epoch,
                    p.config.network_version == NetworkVersion::V2 ? 0u : global,
                    p.config.network_version == NetworkVersion::V2 ? 1u : std::max<std::uint64_t>(1, p.config.data_shards),
                    p.receive_budget, p.config.nack_delay_ms * 1000000,
                    p.config.nack_interval_ms * 1000000, p.config.network_version));
            }
        }
        void update(std::shared_ptr<const GatewayDataView> next) {
            if (view) {
                for (const auto& [id, peer] : view->peers) {
                    auto now = next->peers.find(id);
                    if (now == next->peers.end() || now->second.admission != peer.admission)
                        for (auto& receive : receives) receive->retire_peer_epoch(peer.admission);
                }
                for (const auto& [key, route] : view->local->routes) if (route->descriptor.role_flags & 2) {
                    auto now = next->local->routes.find(key);
                    if (now == next->local->routes.end() || now->second->descriptor.receiver_route_epoch != route->descriptor.receiver_route_epoch)
                        for (auto& receive : receives) receive->retire_route(route);
                }
            }
            // 确认注销屏障前，取消该逻辑发布者尚未终结的网络事务。
            for (auto route = sends.begin(); route != sends.end();) {
                auto& queue = route->second;
                for (auto item = queue.begin(); item != queue.end();) {
                    if ((*item)->source && !(*item)->source->active.load()) {
                        auto tx = std::move(*item); item = queue.erase(item); --send_count; queued.store(send_count);
                        finish(std::move(tx), SendResultCode::Cancelled);
                    } else ++item;
                }
                if (queue.empty()) route = sends.erase(route); else ++route;
            }
            rebuild_send_schedule();
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
        void cancel_endpoint(std::size_t slot) {
            for (auto route = sends.begin(); route != sends.end();) {
                auto& queue = route->second;
                for (auto item = queue.begin(); item != queue.end();) {
                    if ((*item)->socket_index == slot) {
                        auto tx = std::move(*item); item = queue.erase(item);
                        --send_count; queued.store(send_count);
                        finish(std::move(tx), SendResultCode::Cancelled);
                    } else {
                        ++item;
                    }
                }
                if (queue.empty()) route = sends.erase(route); else ++route;
            }
            rebuild_send_schedule();
        }
        void clear_deferred_receive_socket(std::size_t slot) {
            for (auto route = deferred_receive.begin(); route != deferred_receive.end();) {
                auto& queue = route->second;
                for (auto item = queue.begin(); item != queue.end();) {
                    if (item->socket_index == slot) {
                        deferred_receive_bytes -= item->packet.size;
                        --deferred_receive_count;
                        item = queue.erase(item);
                    } else ++item;
                }
                if (queue.empty()) route = deferred_receive.erase(route); else ++route;
            }
            ready_receive_routes.erase(std::remove_if(ready_receive_routes.begin(), ready_receive_routes.end(),
                [&](const RouteKey& key) { return deferred_receive.find(key) == deferred_receive.end(); }), ready_receive_routes.end());
            scheduled_receive_routes.clear();
            for (const auto& key : ready_receive_routes) scheduled_receive_routes.insert(key);
        }
        void incoming(const ReceivedDatagram& packet, unsigned socket_index, const WireHeader* parsed = nullptr) {
            if (!view || socket_index >= receives.size() || !receives[socket_index]) return;
            WireHeader h; ByteView bytes;
            ++parent.rx_packets; parent.rx_bytes += packet.size; rx_bytes += packet.size;
            if (packet.status == IoStatus::Truncated) ++parent.truncated;
            if (packet.status != IoStatus::Data) { ++parent.invalid_packets; parent.metrics->add(NetMetric::bad_header); return; }
            if (parsed) h = *parsed;
            else {
                const auto decoded = parent.config.network_version == NetworkVersion::V2
                    ? decode_packet_v2(packet.view(), h, bytes, parent.config.limits.message_bytes)
                    : decode_packet(packet.view(), h, bytes, parent.config.limits.message_bytes);
                if (!decoded) { ++parent.invalid_packets; record_protocol_error(*parent.metrics, decoded.code); return; }
            }
            const auto p = view->peers.find(h.source_id); const auto s = view->local->routes.find(h.route);
            if (p == view->peers.end() || s == view->local->routes.end() || !p->second.snapshot) { ++parent.unverified_route; parent.metrics->add(NetMetric::source_route_unverified); return; }
            const auto pub = p->second.snapshot->routes.find(h.route); if (pub == p->second.snapshot->routes.end()) { ++parent.unverified_route; parent.metrics->add(NetMetric::source_route_unverified); return; }
            const auto local_port = endpoints[socket_index] ? endpoints[socket_index]->local_address().port : std::uint16_t(0);
            ReceiveAdmission admission{p->second.admission, pub->second, s->second, p->second.snapshot, view->local,
                                       parent.config.network_version, local_port};
            feedback(receives[socket_index]->ingest(packet, admission, local::monotonic_ns()));
            refresh_receive_tick(socket_index, local::monotonic_ns());
        }
        void finish(std::unique_ptr<Tx> tx, SendResultCode result) {
            GatewayDataEvent event; event.header = tx->record.header; event.result.publisher_id = event.header.publisher_id;
            event.result.sequence = event.header.sequence; event.result.result = result;
            event.result.target_count = tx->targets.size(); event.result.possible_remote_delivery = tx->sent;
            if (tx->reliable) {
                if (!tx->reliable->result()) tx->reliable->cancel(result);
                event.result = *tx->reliable->result();
            }
            if (tx->reliable) { const auto stats = tx->reliable->stats(); parent.retry_packets += stats.retry_packets; parent.nacks += stats.nacks; parent.ignored_controls += stats.ignored_controls; }
            result = event.result.result;
            if (tx->reliable) {
                switch (result) {
                case SendResultCode::Completed: parent.metrics->add(NetMetric::reliable_completed); break;
                case SendResultCode::TimedOut: parent.metrics->add(NetMetric::reliable_timed_out); break;
                case SendResultCode::Cancelled: parent.metrics->add(NetMetric::reliable_cancelled); break;
                case SendResultCode::NoSubscribers: parent.metrics->add(NetMetric::reliable_no_subscribers); break;
                case SendResultCode::PeerGone: case SendResultCode::PeerRestarted: case SendResultCode::GatewayLost: parent.metrics->add(NetMetric::reliable_peer_lost); break;
                default: parent.metrics->add(NetMetric::reliable_rejected); break;
                }
                if (tx->first_send_ns) parent.metrics->observe(NetStage::AckWait, metric_now_ns() - tx->first_send_ns);
            }
            const auto key = std::make_pair(event.header.publisher_id, event.header.sequence);
            const auto found = active.find(key); if (found != active.end() && found->second == tx.get()) active.erase(found);
            tx.reset();
            if (result == SendResultCode::Completed) ++parent.sent_messages; else ++parent.rejected_records;
            parent.emit(std::move(event));
        }
        WireHeader base(const Tx& tx) const {
            WireHeader h; const auto& record = tx.record.header; const auto& blob = tx.record.blob;
            h.route = record.route; h.source_id = parent.identity; h.source_epoch = parent.epoch; h.publisher_id = record.publisher_id; h.sequence = record.sequence;
            if (parent.config.network_version == NetworkVersion::V2 && view) {
                const auto local = view->local->routes.find(record.route);
                if (local != view->local->routes.end()) h.data_source_endpoint_epoch = local->second->descriptor.endpoint_epoch;
            }
            h.message_size = blob.size(); h.message_crc = tx.crc; h.fragment_count = (blob.size() + 1023) / 1024; h.encoding = blob.encoding(); h.schema_hash = blob.schema_hash(); h.delivery = record.delivery;
            return h;
        }
        void prepare(std::unique_ptr<Tx> tx) {
            if (tx->record.pulled_ns) {
                const auto elapsed = metric_now_ns() - tx->record.pulled_ns;
                parent.metrics->observe(NetStage::OutboxWait, elapsed);
                if (view) { const auto route = view->local->routes.find(tx->record.header.route); if (route != view->local->routes.end()) route->second->metrics.queue_wait.observe(elapsed); }
            }
            if (tx->source && !tx->source->active.load()) { finish(std::move(tx), SendResultCode::Cancelled); return; }
            const auto& h = tx->record.header;
            const auto key = std::make_pair(h.publisher_id, h.sequence);
            if (active.count(key)) { finish(std::move(tx), SendResultCode::Rejected); return; }
            tx->crc = crc32c(tx->record.blob.view());
            if (h.delivery == Delivery::Reliable) {
                const auto now = local::monotonic_ns();
                if (h.deadline_monotonic_ns > now && h.deadline_monotonic_ns - now > 5000000000ull) { finish(std::move(tx), SendResultCode::UnsupportedTimeout); return; }
                std::vector<ReliableTarget> targets; targets.reserve(tx->targets.size());
                for (const auto& t : tx->targets) targets.push_back({t.peer, t.route});
                tx->expires = h.deadline_monotonic_ns;
                tx->reliable = std::make_unique<ReliableSession>(base(*tx), std::move(targets), tx->expires,
                    parent.config.retry_initial_ms * 1000000, parent.config.retry_max_ms * 1000000, parent.config.nack_interval_ms * 1000000);
                parent.metrics->add(NetMetric::reliable_started);
            }
            active.emplace(key, tx.get()); queue_send(std::move(tx));
        }
        void sent(Tx& tx, std::uint64_t bytes) {
            tx_bytes += bytes;
            if (!tx.first_send_ns) {
                tx.first_send_ns = metric_now_ns();
                if (tx.record.pulled_ns) parent.metrics->observe(NetStage::FirstSend, tx.first_send_ns - tx.record.pulled_ns);
            }
            if (view) { const auto route = view->local->routes.find(tx.record.header.route); if (route != view->local->routes.end()) route->second->metrics.tx_bytes.fetch_add(bytes, std::memory_order_relaxed); }
        }
        void control(Tx& tx, const ReceivedDatagram& packet) {
            const auto before = tx.reliable->stats(); tx.reliable->control(packet, local::monotonic_ns()); const auto after = tx.reliable->stats();
            parent.metrics->add(NetMetric::acks_rx, after.acks - before.acks); parent.metrics->add(NetMetric::nacks_rx, after.nacks - before.nacks);
        }
        void sending() {
            const auto deadline = local::monotonic_ns() + parent.config.io_round_us * 1000;
            std::size_t packets = 0, bytes = 0, retried = 0;
            const bool initial_pending = std::any_of(active.begin(), active.end(), [](const auto& item) {
                return !item.second->reliable || item.second->reliable->has_initial_pending();
            });
            const auto retry_limit = initial_pending ? std::min(parent.config.io_round_packets / 2, parent.config.io_round_bytes / (2 * 1184)) : parent.config.io_round_packets;
            for (std::size_t visit = 0, count = send_count; visit < count && !ready_send_sockets.empty() && packets < parent.config.io_round_packets && bytes < parent.config.io_round_bytes && local::monotonic_ns() < deadline; ++visit) {
                const auto now = local::monotonic_ns();
                auto tx = pop_send(now);
                if (!tx) break;
                if (tx->socket_index >= endpoints.size() || !endpoints[tx->socket_index]) { finish(std::move(tx), SendResultCode::Cancelled); continue; }
                if (tx->source && !tx->source->active.load()) { finish(std::move(tx), SendResultCode::Cancelled); continue; }
                if (tx->reliable) {
                    const auto now = local::monotonic_ns();
                    const auto allowance = std::min<std::uint64_t>({parent.config.io_batch_max, parent.config.io_round_packets - packets,
                        std::max<std::uint64_t>(1, (parent.config.io_round_bytes - bytes) / 1184)});
                    const auto& batch = tx->reliable->batch(now, allowance, retry_limit - retried);
                    if (tx->reliable->result()) { finish(std::move(tx), SendResultCode::Completed); continue; }
                    auto count = std::min<std::size_t>(batch.size(), allowance);
                    std::size_t retry_prefix = 0;
                    for (std::size_t n = 0; n < count; ++n) {
                        if (batch[n].retry && ++retry_prefix > retry_limit - retried) { count = n; break; }
                    }
                    if (!count) {
                        send_blocked_until[tx->socket_index] = local::monotonic_ns() + 1000000;
                        queue_send(std::move(tx)); continue;
                    }
                    std::vector<Bytes> storage(count); std::vector<OutgoingDatagram> outgoing(count);
                    for (unsigned n = 0; n < count; ++n) {
                        const auto h = tx->reliable->header(batch[n]); const auto offset = std::size_t(h.fragment_index) * 1024;
                        if (parent.config.network_version == NetworkVersion::V2) encode_packet_v2(h, {tx->record.blob.view().data + offset, std::min<std::size_t>(1024, tx->record.blob.size() - offset)}, storage[n]);
                        else encode_packet(h, {tx->record.blob.view().data + offset, std::min<std::size_t>(1024, tx->record.blob.size() - offset)}, storage[n]);
                        outgoing[n] = {tx->reliable->destination(batch[n]), ByteView(storage[n])};
                    }
                    tx->reliable->tick(local::monotonic_ns());
                    if (tx->reliable->result()) { finish(std::move(tx), SendResultCode::TimedOut); continue; }
                    const auto result = endpoints[tx->socket_index]->send_batch(outgoing.data(), outgoing.size());
                    if (result.status == IoStatus::WouldBlock) ++parent.send_eagain;
                    if (result.status == IoStatus::Fatal) ++parent.send_error;
                    if (!result.count && result.status == IoStatus::WouldBlock) send_blocked_until[tx->socket_index] = local::monotonic_ns() + 1000000;
                    else send_blocked_until.erase(tx->socket_index);
                    std::uint64_t accepted_bytes = 0;
                    for (unsigned n = 0; n < result.count; ++n) accepted_bytes += storage[n].size();
                    parent.tx_bytes += accepted_bytes; if (result.count) sent(*tx, accepted_bytes);
                    std::uint64_t retry_bytes = 0;
                    for (unsigned n = 0; n < result.count; ++n) { retried += batch[n].retry; if (batch[n].retry) retry_bytes += storage[n].size(); }
                    parent.metrics->add(NetMetric::retransmitted_bytes, retry_bytes);
                    tx->reliable->accepted(result.count, local::monotonic_ns()); tx->sent |= result.count != 0;
                    packets += result.count; parent.sent_packets += result.count;
                    for (unsigned n = 0; n < result.count; ++n) bytes += storage[n].size();
                    if (result.status == IoStatus::Fatal) finish(std::move(tx), SendResultCode::Rejected);
                    else queue_send(std::move(tx));
                    continue;
                }
                if (local::monotonic_ns() >= tx->expires) { finish(std::move(tx), SendResultCode::TimedOut); continue; }
                if (tx->targets.empty()) { finish(std::move(tx), SendResultCode::NoSubscribers); continue; }
                auto& target = tx->targets[tx->cursor];
                if (!target.peer.admission->active.load() || !target.route->active.load() || !target.route->subscriber_active.load()) { finish(std::move(tx), SendResultCode::PeerGone); continue; }
                if (tx->record.blob.size() > target.peer.admission->hello.max_message_bytes) { finish(std::move(tx), SendResultCode::Rejected); continue; }
                WireHeader h; const auto& record = tx->record.header; const auto& blob = tx->record.blob;
                h.route = record.route; h.source_id = parent.identity; h.source_epoch = parent.epoch; h.publisher_id = record.publisher_id; h.sequence = record.sequence;
                if (parent.config.network_version == NetworkVersion::V2) {
                    const auto local = view->local->routes.find(record.route);
                    if (local == view->local->routes.end()) { finish(std::move(tx), SendResultCode::Rejected); continue; }
                    h.data_source_endpoint_epoch = local->second->descriptor.endpoint_epoch;
                    h.data_target_endpoint_epoch = target.route->descriptor.endpoint_epoch;
                }
                h.target_id = target.peer.admission->hello.gateway_id; h.target_epoch = target.peer.admission->hello.gateway_epoch;
                h.receiver_route_epoch = target.route->descriptor.receiver_route_epoch; h.message_size = blob.size(); h.message_crc = tx->crc;
                h.fragment_count = (blob.size() + 1023) / 1024; h.encoding = blob.encoding(); h.schema_hash = blob.schema_hash();
                const auto batch_size = std::min<std::uint64_t>({parent.config.io_batch_max, h.fragment_count - target.fragment,
                    parent.config.io_round_packets - packets, std::max<std::uint64_t>(1, (parent.config.io_round_bytes - bytes) / 1184)});
                std::vector<Bytes> storage(batch_size); std::vector<OutgoingDatagram> outgoing(batch_size);
                for (unsigned n = 0; n < batch_size; ++n) {
                    h.fragment_index = target.fragment + n; const auto offset = std::size_t(h.fragment_index) * 1024;
                    if (parent.config.network_version == NetworkVersion::V2) {
                        encode_packet_v2(h, {blob.view().data + offset, std::min<std::size_t>(1024, blob.size() - offset)}, storage[n]);
                        outgoing[n] = {{target.peer.admission->ipv4, target.route->descriptor.data_port}, ByteView(storage[n])};
                    } else {
                        encode_packet(h, {blob.view().data + offset, std::min<std::size_t>(1024, blob.size() - offset)}, storage[n]);
                        outgoing[n] = {{target.peer.admission->ipv4, data_port(h.route, target.peer.admission->hello.data_base_port, target.peer.admission->hello.data_shards)}, ByteView(storage[n])};
                    }
                }
                if (local::monotonic_ns() >= tx->expires) { finish(std::move(tx), SendResultCode::TimedOut); continue; }
                const auto result = endpoints[tx->socket_index]->send_batch(outgoing.data(), outgoing.size());
                    if (result.status == IoStatus::WouldBlock) ++parent.send_eagain;
                    if (result.status == IoStatus::Fatal) ++parent.send_error;
                    if (!result.count && result.status == IoStatus::WouldBlock) send_blocked_until[tx->socket_index] = local::monotonic_ns() + 1000000;
                    else send_blocked_until.erase(tx->socket_index);
                    std::uint64_t accepted_bytes = 0;
                    for (unsigned n = 0; n < result.count; ++n) accepted_bytes += storage[n].size();
                    parent.tx_bytes += accepted_bytes; if (result.count) sent(*tx, accepted_bytes);
                target.fragment += result.count; packets += result.count; parent.sent_packets += result.count;
                for (unsigned n = 0; n < result.count; ++n) bytes += storage[n].size();
                tx->sent |= result.count != 0;
                if (result.status == IoStatus::Fatal) { finish(std::move(tx), SendResultCode::Rejected); continue; }
                if (target.fragment == h.fragment_count && ++tx->cursor == tx->targets.size()) finish(std::move(tx), SendResultCode::Completed);
                else queue_send(std::move(tx));
            }
            if (!ready_send_sockets.empty() && (packets >= parent.config.io_round_packets || bytes >= parent.config.io_round_bytes || local::monotonic_ns() >= deadline)) { ++budget_yields; parent.metrics->add(NetMetric::shard_budget_yields); }
        }
        void run() noexcept {
            std::uint64_t next_capture_ns = local::monotonic_ns() + 100000000;
            try {
                while (!parent.stopped.load()) {
                    for (unsigned count = 0; count < 64; ++count) {
                        Command command;
                        { std::lock_guard<std::mutex> lock(mutex); if (commands.empty()) break; command = std::move(commands.front()); commands.pop_front(); }
                        --parent.queued_commands;
                        parent.queued_bytes -= command.charged_bytes;
                        if (command.view) { update(std::move(command.view)); command.fence->arrive(); }
                        else if (command.endpoint_command) {
                            bool success = false;
                            try {
                                if (command.endpoint_operation == Command::EndpointOperation::Add) {
                                    if (command.endpoint_slot > endpoints.size() ||
                                        (command.endpoint_slot < endpoints.size() && endpoints[command.endpoint_slot]))
                                        throw std::runtime_error("数据端点 slot 已占用");
                                    const auto socket_index = command.endpoint_slot == endpoints.size()
                                        ? parent.next_socket_index.fetch_add(1, std::memory_order_relaxed)
                                        : socket_indices[command.endpoint_slot];
                                    auto receive = std::make_unique<ReassemblyShard>(parent.identity, parent.epoch,
                                        parent.config.network_version == NetworkVersion::V2 ? 0u : static_cast<unsigned>(socket_index),
                                        parent.config.network_version == NetworkVersion::V2 ? 1u : std::max<std::uint64_t>(1, parent.config.data_shards),
                                        parent.receive_budget, parent.config.nack_delay_ms * 1000000,
                                        parent.config.nack_interval_ms * 1000000, parent.config.network_version);
                                    if (command.endpoint_slot == endpoints.size()) {
                                        endpoints.reserve(endpoints.size() + 1);
                                        receives.reserve(receives.size() + 1);
                                        receive_tick_generation.reserve(receive_tick_generation.size() + 1);
                                        receive_tick_deadline.reserve(receive_tick_deadline.size() + 1);
                                        std::lock_guard<std::mutex> lock(endpoint_metrics_mutex);
                                        socket_indices.reserve(socket_indices.size() + 1);
                                    }
                                    const auto* owner = command.endpoint.get();
                                    const threepools::SocketWaitToken token{owner, static_cast<std::uintptr_t>(command.endpoint->native_handle())};
                                    const auto mapped = endpoint_slots_by_owner.emplace(owner, command.endpoint_slot);
                                    if (!mapped.second) throw std::runtime_error("数据端点 owner 已注册");
                                    bool wait_added = false;
                                    try { wait_added = wait_set.add(token); }
                                    catch (...) { endpoint_slots_by_owner.erase(mapped.first); throw; }
                                    if (!wait_added) {
                                        endpoint_slots_by_owner.erase(mapped.first);
                                        throw std::runtime_error("数据 worker 注册动态端点失败");
                                    }
                                    if (command.endpoint_slot == endpoints.size()) {
                                        endpoints.push_back(std::move(command.endpoint));
                                        {
                                            std::lock_guard<std::mutex> lock(endpoint_metrics_mutex);
                                            socket_indices.push_back(static_cast<unsigned>(socket_index));
                                        }
                                        receives.push_back(std::move(receive));
                                    } else {
                                        endpoints[command.endpoint_slot] = std::move(command.endpoint);
                                        receives[command.endpoint_slot] = std::move(receive);
                                    }
                                    ++active_socket_count;
                                } else {
                                    if (command.endpoint_slot >= endpoints.size() || !endpoints[command.endpoint_slot])
                                        throw std::runtime_error("数据端点 slot 不存在");
                                    cancel_endpoint(command.endpoint_slot);
                                    clear_deferred_receive_socket(command.endpoint_slot);
                                    if (command.endpoint_slot < receive_tick_generation.size()) {
                                        ++receive_tick_generation[command.endpoint_slot];
                                        receive_tick_deadline[command.endpoint_slot] = 0;
                                    }
                                    auto* owner = endpoints[command.endpoint_slot].get();
                                    const threepools::SocketWaitToken token{owner, static_cast<std::uintptr_t>(owner->native_handle())};
                                    if (!wait_set.remove(token)) throw std::runtime_error("数据 worker 注销等待 token 失败");
                                    endpoint_slots_by_owner.erase(owner);
                                    ready_sockets.erase(std::remove(ready_sockets.begin(), ready_sockets.end(), command.endpoint_slot), ready_sockets.end());
                                    ready_socket_set.erase(command.endpoint_slot);
                                    send_blocked_until.erase(static_cast<unsigned>(command.endpoint_slot));
                                    receives[command.endpoint_slot].reset();
                                    endpoints[command.endpoint_slot].reset();
                                    --active_socket_count;
                                }
                                success = true;
                            } catch (...) {
                                success = false;
                            }
                            if (command.endpoint_result) command.endpoint_result->set_value(success);
                            if (command.fence) command.fence->arrive();
                        }
                        else if (command.tx) prepare(std::move(command.tx));
                        else if (command.control) {
                            WireHeader h; ByteView body;
                            const auto decoded = parent.config.network_version == NetworkVersion::V2
                                ? decode_packet_v2(command.control->view(), h, body)
                                : decode_packet(command.control->view(), h, body);
                            if (decoded) {
                                const auto found = active.find({h.publisher_id, h.sequence});
                                if (found != active.end() && found->second->reliable) control(*found->second, *command.control);
                            }
                        }
                    }
                    const auto round_deadline = local::monotonic_ns() + parent.config.io_round_us * 1000;
                    std::size_t processed_packets = process_deferred_receive(parent.config.io_round_packets, round_deadline);
                    process_due_receive_ticks(local::monotonic_ns(), round_deadline);

                    std::size_t read_packets = 0, read_bytes = 0;
                    const auto max_deferred_records = std::max<std::uint64_t>(parent.config.io_round_packets, parent.config.io_batch_max);
                    std::array<ReceivedDatagram, 64> input{};
                    while (processed_packets < parent.config.io_round_packets && !ready_sockets.empty() &&
                           read_packets < parent.config.io_round_packets && read_bytes < parent.config.io_round_bytes &&
                           local::monotonic_ns() < round_deadline) {
                        const auto slot = ready_sockets.front(); ready_sockets.pop_front(); ready_socket_set.erase(slot);
                        if (slot >= endpoints.size() || !endpoints[slot] || !receives[slot]) continue;
                        const auto ready_count = std::max<std::size_t>(1, ready_sockets.size() + 1);
                        const auto socket_quantum = std::max<std::size_t>(1, parent.config.io_round_packets / ready_count);
                        const auto pending_capacity = max_deferred_records > deferred_receive_count
                            ? static_cast<std::size_t>(max_deferred_records - deferred_receive_count) : 0;
                        const auto byte_quantum = std::max<std::size_t>(1, parent.config.io_round_bytes / ready_count);
                        const auto byte_packets = std::max<std::size_t>(1, byte_quantum / kMaxDatagramBytesV2);
                        const auto allowance = std::min<std::size_t>({input.size(), parent.config.io_batch_max,
                            socket_quantum, parent.config.io_round_packets - read_packets,
                            pending_capacity, byte_packets});
                        if (!allowance) { enqueue_ready_socket(slot); break; }
                        const auto result = endpoints[slot]->receive_batch(input.data(), allowance);
                        if (result.status == IoStatus::Fatal) throw std::runtime_error("数据端点批量接收失败");
                        for (std::size_t n = 0; n < result.count; ++n) {
                            read_bytes += input[n].size;
                            stage_received(std::move(input[n]), static_cast<unsigned>(slot));
                        }
                        read_packets += result.count;
                        if (result.count == allowance) enqueue_ready_socket(slot);
                        if (result.count) {
                            const auto remaining = parent.config.io_round_packets > processed_packets
                                ? parent.config.io_round_packets - processed_packets : 0;
                            if (remaining) processed_packets += process_deferred_receive(remaining, round_deadline);
                        }
                    }
                    if (read_packets >= parent.config.io_round_packets || read_bytes >= parent.config.io_round_bytes ||
                        processed_packets >= parent.config.io_round_packets || local::monotonic_ns() >= round_deadline) {
                        ++budget_yields; parent.metrics->add(NetMetric::shard_budget_yields);
                    }
                    process_due_receive_ticks(local::monotonic_ns(), round_deadline);
                    sending();

                    bool pending_commands = false;
                    { std::lock_guard<std::mutex> lock(mutex); pending_commands = !commands.empty(); }
                    const auto now = local::monotonic_ns();
                    bool immediate = pending_commands || !ready_sockets.empty() || !ready_receive_routes.empty();
                    bool unblocked_send = false;
                    std::uint64_t next_send_ns = UINT64_MAX;
                    for (const auto socket : ready_send_sockets) {
                        const auto blocked = send_blocked_until.find(socket);
                        if (blocked == send_blocked_until.end() || blocked->second <= now) unblocked_send = true;
                        else next_send_ns = std::min(next_send_ns, blocked->second);
                    }
                    immediate = immediate || unblocked_send;
                    std::uint64_t next_deadline = now + 50000000;
                    if (prune_receive_tick_queue()) next_deadline = std::min(next_deadline, receive_tick_queue.begin()->first);
                    next_deadline = std::min(next_deadline, next_send_ns);
                    int timeout_ms = 0;
                    if (!immediate) {
                        const auto delay = next_deadline > now ? next_deadline - now : 0;
                        timeout_ms = static_cast<int>(std::min<std::uint64_t>(50, std::max<std::uint64_t>(1, (delay + 999999) / 1000000)));
                    }
                    if (wait_set.wait(std::chrono::milliseconds(timeout_ms))) {
                        ++wakeups; parent.metrics->add(NetMetric::shard_wakeups);
                        for (const auto& token : wait_set.consume_ready()) {
                            if (token == wake_token) { local::drain_event(wake.get()); continue; }
                            const auto owner = static_cast<const DatagramEndpoint*>(token.owner);
                            const auto found = endpoint_slots_by_owner.find(owner);
                            if (found != endpoint_slots_by_owner.end() && found->second < endpoints.size() &&
                                endpoints[found->second].get() == owner &&
                                static_cast<std::uintptr_t>(owner->native_handle()) == token.handle)
                                enqueue_ready_socket(found->second);
                        }
                    }
                    if (local::monotonic_ns() >= next_capture_ns) {
                        capture(); next_capture_ns = local::monotonic_ns() + 100000000;
                    }
                }
            } catch (...) { parent.stopped.store(true); local::notify(parent.control_wake); }
            while (!sends.empty()) {
                auto found = sends.begin(); auto tx = std::move(found->second.front()); found->second.pop_front();
                if (found->second.empty()) sends.erase(found);
                finish(std::move(tx), SendResultCode::GatewayLost);
            }
            ready_send_sockets.clear(); ready_send_socket_set.clear(); send_routes_by_socket.clear(); scheduled_send_routes.clear();
            ready_sockets.clear(); ready_socket_set.clear(); deferred_receive.clear(); ready_receive_routes.clear(); scheduled_receive_routes.clear();
            deferred_receive_count = deferred_receive_bytes = 0; send_count = 0; queued.store(0);
            { std::lock_guard<std::mutex> lock(mutex); for (auto& command : commands) { --parent.queued_commands; parent.queued_bytes -= command.charged_bytes; if (command.tx) finish(std::move(command.tx), SendResultCode::GatewayLost); if (command.endpoint_result) command.endpoint_result->set_value(false); if (command.fence && command.endpoint_command) command.fence->arrive(); } commands.clear(); }
            for (const auto& endpoint : endpoints) if (endpoint)
                wait_set.remove({endpoint.get(), static_cast<std::uintptr_t>(endpoint->native_handle())});
            endpoint_slots_by_owner.clear(); wait_set.remove(wake_token); wait_set.stop();
            capture(); active.clear(); receives.clear(); endpoints.clear(); view.reset();
        }
    };
    std::shared_ptr<NetMetrics> metrics;
    GatewayConfig config; Identity identity; std::uint64_t epoch; int control_wake;
    std::shared_ptr<ReassemblyBudget> receive_budget;
    std::vector<std::unique_ptr<Shard>> shards;
    std::atomic<std::uint64_t> next_socket_index{0};
    mutable std::mutex endpoint_mutex;
    std::map<std::uint16_t, std::pair<unsigned, std::size_t>> endpoint_slots;
    // v1 的端口由 RouteKey 哈希得到全局 socket 下标；该表把全局下标映射到实际 worker 的本地槽位。
    std::vector<std::pair<unsigned, std::size_t>> socket_slots;
    std::map<RouteKey, unsigned> route_workers;
    std::map<RouteKey, std::pair<unsigned, std::size_t>> route_slots;
    std::atomic<bool> stopped{false};
    std::atomic<std::uint64_t> queued_commands{0}, target_states{0}, sent_messages{0}, sent_packets{0}, committed_messages{0}, rejected_records{0}, dropped_feedback{0};
    std::atomic<std::uint64_t> queued_bytes{0};
    std::atomic<std::uint64_t> rx_packets{0}, rx_bytes{0}, tx_bytes{0}, send_eagain{0}, send_error{0}, truncated{0}, invalid_packets{0}, unverified_route{0};
    std::atomic<std::uint64_t> retry_packets{0}, nacks{0}, ignored_controls{0};
    std::mutex stop_mutex;
    std::mutex events_mutex; std::deque<GatewayDataEvent> events;
    std::uint64_t event_bytes = 0;
    std::mutex quota_mutex; std::map<Identity, std::uint64_t> publisher_reliable;
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
        do { if (count >= config.limits.command_records) { metrics->reject(NetQuota::command_records); return false; } } while (!queued_commands.compare_exchange_weak(count, count + 1));
        const auto bytes = sizeof(Command) + (command.view ? command.view->peers.size() * 192 : command.tx ? sizeof(Tx) + command.tx->targets.size() * sizeof(Target) : command.endpoint_command ? sizeof(DatagramEndpoint) : sizeof(ReceivedDatagram));
        auto used = queued_bytes.load();
        do { if (bytes > config.limits.command_bytes - used) { metrics->reject(NetQuota::command_bytes); --queued_commands; return false; } } while (!queued_bytes.compare_exchange_weak(used, used + bytes));
        command.charged_bytes = bytes; metrics->peak(NetQuota::command_bytes, used + bytes); metrics->peak(NetQuota::command_records, count + 1);
        if (index >= shards.size()) { --queued_commands; queued_bytes -= bytes; return false; }
        auto& shard = shards[index];
        try {
            std::lock_guard<std::mutex> lock(shard->mutex);
            if (stopped.load()) { --queued_commands; queued_bytes -= bytes; return false; }
            shard->commands.push_back(std::move(command));
        }
        catch (...) { --queued_commands; queued_bytes -= bytes; throw; }
        local::notify(shard->wake.get()); return true;
    }
};
namespace {
std::vector<unsigned> default_socket_workers(const GatewayConfig& config) {
    const auto count = config.data_mode == DataMode::PerTopic ? std::size_t(0) : static_cast<std::size_t>(config.data_shards);
    std::vector<unsigned> workers; workers.reserve(count);
    for (std::size_t socket = 0; socket < count; ++socket)
        workers.push_back(static_cast<unsigned>(socket % std::max<std::uint64_t>(1, config.data_workers)));
    return workers;
}
}
GatewayData::GatewayData(const GatewayConfig& c, Identity id, std::uint64_t epoch, int wake,
                         std::vector<std::unique_ptr<DatagramEndpoint>> endpoints, std::shared_ptr<NetMetrics> metrics)
    : GatewayData(c, id, epoch, wake, std::move(endpoints), default_socket_workers(c), std::move(metrics)) {}
GatewayData::GatewayData(const GatewayConfig& c, Identity id, std::uint64_t epoch, int wake, std::vector<std::unique_ptr<DatagramEndpoint>> endpoints, std::vector<unsigned> socket_workers, std::shared_ptr<NetMetrics> metrics) : impl_(new Impl) {
    impl_->config = c; impl_->identity = id; impl_->epoch = epoch; impl_->control_wake = wake;
    impl_->metrics = metrics ? std::move(metrics) : std::make_shared<NetMetrics>();
    impl_->receive_budget = std::make_shared<ReassemblyBudget>(c.limits, impl_->metrics);
    const auto pool_sockets = c.data_mode == DataMode::PerTopic ? std::size_t(0) : c.data_shards;
    if (endpoints.size() != pool_sockets || socket_workers.size() != pool_sockets)
        throw std::invalid_argument("数据端点或 worker 映射数不匹配");
    std::vector<std::vector<std::unique_ptr<DatagramEndpoint>>> owned(c.data_workers);
    std::vector<std::vector<unsigned>> slots(c.data_workers);
    for (unsigned global = 0; global < endpoints.size(); ++global) {
        const auto worker = socket_workers[global];
        if (worker >= c.data_workers) throw std::invalid_argument("池 socket worker 超出 data-workers");
        owned[worker].push_back(std::move(endpoints[global]));
        slots[worker].push_back(global);
    }
    for (unsigned worker = 0; worker < c.data_workers; ++worker)
        impl_->shards.push_back(std::make_unique<Impl::Shard>(*impl_, worker, std::move(owned[worker]), std::move(slots[worker])));
    impl_->socket_slots.resize(endpoints.size());
    for (unsigned worker = 0; worker < impl_->shards.size(); ++worker) {
        impl_->shards[worker]->active_socket_count.store(impl_->shards[worker]->endpoints.size());
        for (std::size_t local = 0; local < impl_->shards[worker]->endpoints.size(); ++local) {
            const auto port = impl_->shards[worker]->endpoints[local]->local_address().port;
            impl_->endpoint_slots.emplace(port, std::make_pair(worker, local));
            const auto global = impl_->shards[worker]->socket_indices[local];
            if (global < impl_->socket_slots.size()) impl_->socket_slots[global] = {worker, local};
            ++impl_->next_socket_index;
        }
    }
    try { for (auto& shard : impl_->shards) shard->thread = std::thread([p = shard.get()] { p->run(); }); }
    catch (...) { stop(); throw; }
}
GatewayData::~GatewayData() { stop(); }
bool GatewayData::add_endpoint(std::unique_ptr<DatagramEndpoint> endpoint, std::uint16_t port, unsigned worker) {
    if (!endpoint || !port || worker >= impl_->shards.size()) return false;
    std::size_t slot = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->endpoint_mutex);
        if (impl_->endpoint_slots.count(port)) return false;
        for (;;) {
            const bool occupied = std::any_of(impl_->endpoint_slots.begin(), impl_->endpoint_slots.end(),
                [&](const auto &entry) { return entry.second.first == worker && entry.second.second == slot; });
            if (!occupied) break;
            ++slot;
        }
        impl_->endpoint_slots.emplace(port, std::make_pair(worker, slot));
    }
    Impl::Command command; command.endpoint_command = true; command.endpoint_operation = Impl::Command::EndpointOperation::Add;
    command.endpoint = std::move(endpoint); command.endpoint_port = port; command.endpoint_slot = slot;
    command.fence = std::make_shared<Impl::Fence>(1);
    command.endpoint_result = std::make_shared<std::promise<bool>>();
    auto ready = command.endpoint_result->get_future().share();
    bool queued = false;
    try { queued = impl_->enqueue(worker, command); } catch (...) {
        std::lock_guard<std::mutex> lock(impl_->endpoint_mutex); impl_->endpoint_slots.erase(port); throw;
    }
    if (!queued) { std::lock_guard<std::mutex> lock(impl_->endpoint_mutex); impl_->endpoint_slots.erase(port); return false; }
    try { if (!ready.get()) { std::lock_guard<std::mutex> lock(impl_->endpoint_mutex); impl_->endpoint_slots.erase(port); return false; } }
    catch (...) { std::lock_guard<std::mutex> lock(impl_->endpoint_mutex); impl_->endpoint_slots.erase(port); return false; }
    return true;
}
bool GatewayData::remove_endpoint(std::uint16_t port) {
    std::pair<unsigned, std::size_t> slot;
    { std::lock_guard<std::mutex> lock(impl_->endpoint_mutex); auto found = impl_->endpoint_slots.find(port); if (found == impl_->endpoint_slots.end()) return true; slot = found->second; }
    Impl::Command command; command.endpoint_command = true; command.endpoint_operation = Impl::Command::EndpointOperation::Remove;
    command.endpoint_slot = slot.second; command.fence = std::make_shared<Impl::Fence>(1);
    command.endpoint_result = std::make_shared<std::promise<bool>>();
    auto ready = command.endpoint_result->get_future().share();
    if (!impl_->enqueue(slot.first, command)) return false;
    try { if (!ready.get()) return false; } catch (...) { return false; }
    std::lock_guard<std::mutex> lock(impl_->endpoint_mutex); impl_->endpoint_slots.erase(port); return true;
}
void GatewayData::stop() {
    std::lock_guard<std::mutex> lock(impl_->stop_mutex);
    impl_->stopped.store(true);
    for (auto& shard : impl_->shards) local::notify(shard->wake.get());
    for (auto& shard : impl_->shards) if (shard->thread.joinable()) shard->thread.join();
    for (auto& shard : impl_->shards) shard->endpoints.clear();
    std::lock_guard<std::mutex> events(impl_->events_mutex); impl_->events.clear(); impl_->event_bytes = 0;
}
std::shared_future<void> GatewayData::synchronize(std::shared_ptr<const GatewayDataView> view) {
    auto fence = std::make_shared<Impl::Fence>(impl_->shards.size()); auto future = fence->promise.get_future().share();
    {
        std::lock_guard<std::mutex> lock(impl_->endpoint_mutex);
        impl_->route_workers.clear();
        impl_->route_slots.clear();
        for (const auto& [key, route] : view->local->routes) {
            unsigned worker = 0;
            if (impl_->config.network_version == NetworkVersion::V2) {
                const auto endpoint = impl_->endpoint_slots.find(route->descriptor.data_port);
                if (endpoint == impl_->endpoint_slots.end()) continue;
                worker = endpoint->second.first;
                impl_->route_slots[key] = endpoint->second;
            } else {
                const auto global = route_hash(key) % impl_->config.data_shards;
                if (global >= impl_->socket_slots.size()) continue;
                worker = impl_->socket_slots[global].first;
                impl_->route_slots[key] = impl_->socket_slots[global];
            }
            impl_->route_workers[key] = worker;
        }
    }
    for (unsigned i = 0; i < impl_->shards.size(); ++i) {
        Impl::Command command; command.view = view; command.fence = fence;
        if (!impl_->enqueue(i, command)) { impl_->stopped.store(true); throw std::runtime_error("shard 目录命令达到上限"); }
    }
    return future;
}
bool GatewayData::submit(OutboxRecord& record, std::vector<PeerView> peers, std::shared_ptr<LocalRegistration> source) {
    auto tx = std::make_unique<Impl::Tx>();
    tx->source = std::move(source);
    tx->quota_owner = impl_.get(); tx->charged_publisher = record.header.publisher_id;
    if (record.header.delivery == Delivery::Reliable) {
        std::lock_guard<std::mutex> lock(impl_->quota_mutex);
        auto& count = impl_->publisher_reliable[tx->charged_publisher];
        if (count >= impl_->config.limits.publisher_reliable) { impl_->metrics->reject(NetQuota::publisher_reliable); if (!count) impl_->publisher_reliable.erase(tx->charged_publisher); return false; }
        ++count; impl_->metrics->peak(NetQuota::publisher_reliable, count); tx->publisher_charged = true;
    }
    const auto size = peers.size(); auto used = impl_->target_states.load();
    do { if (size > impl_->config.limits.target_states - used) { impl_->metrics->reject(NetQuota::target_states); return false; } }
    while (!impl_->target_states.compare_exchange_weak(used, used + size));
    impl_->metrics->peak(NetQuota::target_states, used + size);
    tx->target_charge = size; tx->targets.reserve(size);
    for (auto& peer : peers) { auto route = peer.snapshot->routes.at(record.header.route); tx->targets.push_back({std::move(peer), std::move(route), 0}); }
    tx->expires = local::monotonic_ns() + 500000000ull;
    tx->record = std::move(record); Impl::Command command; command.tx = std::move(tx);
    unsigned worker = 0;
    if (impl_->config.network_version == NetworkVersion::V2) {
        std::lock_guard<std::mutex> lock(impl_->endpoint_mutex);
        const auto found = impl_->route_slots.find(command.tx->record.header.route);
        if (found == impl_->route_slots.end()) { record = std::move(command.tx->record); return false; }
        worker = found->second.first; command.tx->socket_index = found->second.second;
    } else {
        const auto global = route_hash(command.tx->record.header.route) % impl_->config.data_shards;
        if (global >= impl_->socket_slots.size()) { record = std::move(command.tx->record); return false; }
        worker = impl_->socket_slots[global].first; command.tx->socket_index = impl_->socket_slots[global].second;
    }
    try {
        if (impl_->enqueue(worker, command)) { impl_->metrics->add(NetMetric::remote_target_copies, size); if (!size) impl_->metrics->add(NetMetric::no_target_dropped); return true; }
    } catch (...) { record = std::move(command.tx->record); throw; }
    record = std::move(command.tx->record); return false;
}
bool GatewayData::control(const ReceivedDatagram& packet) {
    WireHeader h; ByteView body;
    const auto decoded = impl_->config.network_version == NetworkVersion::V2
        ? decode_packet_v2(packet.view(), h, body) : decode_packet(packet.view(), h, body);
    if (packet.status != IoStatus::Data || packet.size > packet.bytes.size() || !decoded || h.kind == PacketKind::Data) return false;
    Impl::Command command; command.control = std::make_unique<ReceivedDatagram>(packet);
    unsigned worker = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->endpoint_mutex);
        const auto found = impl_->route_workers.find(h.route);
        if (found != impl_->route_workers.end()) worker = found->second;
        else if (impl_->config.network_version == NetworkVersion::V1) {
            const auto global = route_hash(h.route) % impl_->config.data_shards;
            if (global >= impl_->socket_slots.size()) return false;
            worker = impl_->socket_slots[global].first;
        }
        else return false;
    }
    return impl_->enqueue(worker, command);
}
bool GatewayData::pop(GatewayDataEvent& event) { std::lock_guard<std::mutex> lock(impl_->events_mutex); if (impl_->events.empty()) return false; impl_->event_bytes -= sizeof(GatewayDataEvent) + impl_->events.front().packet.capacity(); event = std::move(impl_->events.front()); impl_->events.pop_front(); return true; }
std::string GatewayData::shard_metrics() const {
    std::ostringstream out; out << "{\"shards\":["; bool first = true;
    for (const auto& shard : impl_->shards) {
        if (!first) out << ','; first = false;
        std::lock_guard<std::mutex> lock(shard->endpoint_metrics_mutex);
        out << "{\"index\":" << shard->index << ",\"socket_count\":" << shard->active_socket_count.load() << ",\"sockets\":[";
        for (std::size_t n = 0; n < shard->socket_indices.size(); ++n) { if (n) out << ','; out << shard->socket_indices[n]; }
        out << "],\"rx_bytes\":" << shard->rx_bytes.load() << ",\"tx_bytes\":" << shard->tx_bytes.load()
            << ",\"wakeups\":" << shard->wakeups.load() << ",\"budget_yields\":" << shard->budget_yields.load()
            << ",\"queued\":" << shard->queued.load() << ",\"queued_peak\":" << shard->queued_peak.load() << '}';
    }
    out << "]}"; return out.str();
}
bool GatewayData::healthy() const { return !impl_->stopped.load(); }
GatewayDataStats GatewayData::stats() const {
    GatewayDataStats result{impl_->sent_messages.load(), impl_->sent_packets.load(), impl_->committed_messages.load(), impl_->rejected_records.load(), impl_->dropped_feedback.load(), impl_->target_states.load(), impl_->receive_budget->usage()};
    result.queued_commands = impl_->queued_commands.load(); result.queued_bytes = impl_->queued_bytes.load();
    result.rx_packets = impl_->rx_packets.load(); result.rx_bytes = impl_->rx_bytes.load(); result.tx_bytes = impl_->tx_bytes.load();
    result.send_eagain = impl_->send_eagain.load(); result.send_error = impl_->send_error.load(); result.truncated = impl_->truncated.load();
    result.invalid_packets = impl_->invalid_packets.load(); result.unverified_route = impl_->unverified_route.load();
    for (const auto& shard : impl_->shards) {
        std::lock_guard<std::mutex> lock(shard->stats_mutex);
#define SUM(field) result.receive_stats.field += shard->snapshot.field;
        SUM(malformed) SUM(wrong_shard) SUM(rejected) SUM(duplicates) SUM(completed) SUM(committed) SUM(commit_attempts) SUM(commit_not_submitted) SUM(commit_indeterminate) SUM(expired) SUM(message_crc_fail)
#undef SUM
    }
    result.retry_packets = impl_->retry_packets.load(); result.nacks = impl_->nacks.load(); result.ignored_controls = impl_->ignored_controls.load(); return result;
}
} // namespace dzIPC::net

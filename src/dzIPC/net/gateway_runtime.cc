#include "dzIPC/net/gateway_runtime.h"
#include "byte_codec.h"
#include "dzIPC/net/datagram_endpoint.h"
#include "dzIPC/net/local_directory.h"
#include "local_control_linux.h"
#include "outbox_service.h"
#include "gateway_data.h"
#include <algorithm>
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <future>
#include <optional>
#include <unistd.h>

namespace dzIPC::net
{
namespace
{
std::string json_string(const std::string &value)
{
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : value)
    {
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += c;
        }
        else if (c < 32)
        {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        }
        else
            out += c;
    }
    return out + '"';
}
} // namespace
struct GatewayRuntime::Impl
{
    struct Cached
    {
        Bytes request, response;
    };
    struct Session
    {
        local::Fd fd;
        local::Credentials peer;
        std::uint64_t id = 0, last_request = 0, connected_at = 0;
        bool welcomed = false;
        WelcomeBody welcome;
        std::shared_ptr<SendAccount> account;
        std::shared_ptr<OutboxAttachment> outbox;
        std::map<std::uint64_t, Cached> cache;
        std::size_t cache_bytes = 0, output_bytes = 0;
        std::deque<Bytes> output;
        std::map<Identity, std::uint64_t> route_versions;
        std::map<std::uint64_t, OutboxHeader> pending_sends;
        std::map<std::uint64_t, SendResultBody> send_results;
        std::deque<std::uint64_t> result_order;
    };
    struct Initializing {
        std::shared_ptr<LocalBinding> binding;
        std::shared_future<std::shared_ptr<ShmWireBridge>> result;
    };
    struct PendingSub { std::uint64_t session, request; Identity id; Bytes packet; };
    struct PendingFence { std::uint64_t session, request; Bytes packet, reply; std::shared_future<void> fence; };
    struct PageSend { PeerView peer; std::shared_ptr<const DirectorySnapshot> snapshot; std::uint32_t page = 0; };
    GatewayConfig config;
    Identity identity;
    std::uint64_t epoch = 0, clock = 0, next_session = 1;
    std::unique_ptr<local::Listener> listener;
    local::Fd wake;
    std::vector<std::unique_ptr<DatagramEndpoint>> endpoints;
    std::map<int, Session> sessions;
    std::unique_ptr<SendBudget> send_budget;
    std::unique_ptr<OutboxService> outboxes;
    std::shared_ptr<DirectoryBudget> directory_budget;
    std::unique_ptr<PeerDirectory> peer_directory;
    std::unique_ptr<LocalDirectory> local_directory;
    std::unique_ptr<GatewayData> data;
    std::shared_future<void> last_data_fence;
    std::shared_ptr<const DirectorySnapshot> bridge_snapshot;
    std::shared_ptr<const GatewayDataView::Bridges> bridge_map;
    std::deque<PendingFence> pending_fences;
    std::vector<Initializing> initializing;
    std::deque<PendingSub> pending_subs;
    std::uint64_t pending_sub_bytes = 0;
    std::map<Identity, PageSend> page_sends;
    std::optional<Identity> page_cursor;
    std::deque<CatalogRequest> catalog_requests;
    std::uint64_t catalog_bytes = 0, next_hello = 0, last_local_version = 0, last_peer_revision = 0, route_revision = 1;
    std::size_t hint_cursor = 0;
    double control_tokens = 0; unsigned non_catalog_opportunities = 0;
    std::uint64_t token_time = 0;
    std::map<Identity, std::pair<double, std::uint64_t>> peer_tokens;
    std::map<Identity, std::pair<double, std::uint64_t>> receive_tokens;
    double receive_control_tokens = 0;
    std::uint64_t receive_token_time = 0;
    std::atomic<std::uint64_t> snapshot_version{1}, active_peers{0}, registered_handles{0};
    std::atomic<std::uint64_t> logical_publishers{0}, ready_subscribers{0}, active_routes{0};
    std::vector<std::pair<int, int>> socket_buffers;
    std::thread thread;
    std::mutex stop_mutex;
    std::atomic<bool> running{true};
    std::atomic<std::uint64_t> session_count{0}, rejected{0}, outbox_records{0}, credit_requests{0};
    std::size_t control_memory = 0;
    mutable std::mutex error_mutex; std::uint32_t last_error_code = 0; std::string last_error;
    std::string status() const
    {
        std::ostringstream s;
        const auto occupied = send_budget->occupied();
        const auto inflight = send_budget->inflight();
        const auto traffic = data ? data->stats() : GatewayDataStats{};
        const auto directory = directory_budget->usage();
        s << "{\"protocol_version\":1,\"local_protocol_version\":2,\"gateway_id\":"
          << json_string(local::hex(identity)) << ",\"gateway_epoch\":\"" << epoch
          << "\",\"state\":\"" << (running.load() ? "Ready" : "Stopped")
          << "\",\"data_plane_ready\":" << (running.load() ? "true" : "false") << ",\"listen_ip\":" << json_string(config.listen_ip)
          << ",\"interface\":" << json_string(config.interface)
          << ",\"udp_sockets\":" << (running.load() ? config.data_shards + 2 : 0)
          << ",\"client_sessions\":" << session_count.load() << ",\"allocated_send_bytes\":\""
          << occupied.bytes << "\",\"send_inflight_bytes\":\"" << inflight.bytes << "\",\"send_inflight_records\":\"" << inflight.records
          << "\",\"allocated_send_records\":\"" << occupied.records
          << "\",\"outbox_records\":\"" << outbox_records.load() << "\",\"credit_requests\":\""
          << credit_requests.load() << "\",\"rejected\":\"" << rejected.load()
          << "\",\"snapshot_version\":\"" << snapshot_version.load() << "\",\"active_peers\":" << active_peers.load()
          << ",\"registered_handles\":" << registered_handles.load()
          << ",\"sent_messages\":" << traffic.sent_messages << ",\"sent_packets\":" << traffic.sent_packets
          << ",\"committed_messages\":" << traffic.committed_messages << ",\"source_injections\":0"
          << ",\"reassembly_bytes\":" << traffic.receive_usage.bytes << ",\"target_states\":" << traffic.target_states
          << ",\"retry_packets\":" << traffic.retry_packets << ",\"nacks\":" << traffic.nacks << ",\"ignored_controls\":" << traffic.ignored_controls
          << ",\"data_datagrams_rx\":" << traffic.rx_packets << ",\"data_bytes_rx\":" << traffic.rx_bytes << ",\"data_bytes_tx\":" << traffic.tx_bytes
          << ",\"send_eagain\":" << traffic.send_eagain << ",\"send_error\":" << traffic.send_error << ",\"truncated\":" << traffic.truncated
          << ",\"invalid_packets\":" << traffic.invalid_packets << ",\"source_route_unverified\":" << traffic.unverified_route
          << ",\"message_crc_fail\":" << traffic.receive_stats.message_crc_fail
          << ",\"wrong_shard\":" << traffic.receive_stats.wrong_shard << ",\"duplicate_suppressed\":" << traffic.receive_stats.duplicates
          << ",\"shm_commit_attempts\":" << traffic.receive_stats.commit_attempts << ",\"shm_commit_not_submitted\":" << traffic.receive_stats.commit_not_submitted
          << ",\"shm_commit_indeterminate\":" << traffic.receive_stats.commit_indeterminate << ",\"assembly_expired_or_revoked\":" << traffic.receive_stats.expired
          << ",\"reassembly_peak_bytes\":" << traffic.receive_usage.peak_bytes << ",\"receive_quota_rejected\":" << traffic.receive_usage.quota_rejected
          << ",\"queued_commands\":" << traffic.queued_commands << ",\"queued_command_bytes\":" << traffic.queued_bytes
          << ",\"data_ports\":[";
        for (std::uint64_t i = 0; i < config.data_shards; ++i)
        {
            if (i)
                s << ',';
            s << config.data_base_port + i;
        }
        s << "],\"socket_buffers\":[";
        for (std::size_t n = 0; n < socket_buffers.size(); ++n) {
            if (n) s << ',';
            s << "{\"receive_bytes\":" << socket_buffers[n].first << ",\"send_bytes\":" << socket_buffers[n].second << '}';
        }
        s << "],\"logical_publishers\":" << logical_publishers.load()
          << ",\"ready_subscribers\":" << ready_subscribers.load() << ",\"active_routes\":" << active_routes.load()
          << ",\"directory_bytes\":" << directory.installed << ",\"old_directory_bytes\":" << directory.old
          << ",\"candidate_bytes\":" << directory.candidates << ",\"peer_history\":" << directory.histories
          << ",\"assemblies_active\":" << traffic.receive_usage.assemblies
          << ",\"bitmap_bytes\":" << traffic.receive_usage.bitmap_bytes << ",\"commit_pending_bytes\":" << traffic.receive_usage.pending_bytes
          << ",\"dedup_streams\":" << traffic.receive_usage.streams << ",\"dedup_bytes\":" << traffic.receive_usage.stream_bytes
          << ",\"receipts\":" << traffic.receive_usage.receipts
          << ",\"local_path\":\"由应用持有\",\"network_path\":\"" << (running.load() ? "Ready" : "Stopped") << "\""
          << ",\"io_batch_max\":" << config.io_batch_max << ",\"io_round_packets\":" << config.io_round_packets
          << ",\"io_round_bytes\":" << config.io_round_bytes << ",\"io_round_us\":" << config.io_round_us
          << ",\"nack_delay_ms\":" << config.nack_delay_ms << ",\"nack_interval_ms\":" << config.nack_interval_ms
          << ",\"retry_initial_ms\":" << config.retry_initial_ms << ",\"retry_max_ms\":" << config.retry_max_ms
          << ",\"control_rate\":" << config.control_rate << ",\"peer_control_rate\":" << config.peer_control_rate
          << ",\"control_burst\":" << config.control_burst
          << ",\"gateway_threads\":" << (running.load() ? config.data_shards + 3 : 0)
          << ",\"thread_scope\":\"控制、drain、初始化和 K 个 shard；SHM 公共调度线程另由 /proc 采样\""
          << ",\"limits\":{";
        bool first_limit = true;
#define SHOW_LIMIT(field) if (!first_limit) s << ','; first_limit = false; s << "\"" #field "\":" << config.limits.field;
        SHOW_LIMIT(topics) SHOW_LIMIT(sessions) SHOW_LIMIT(peers) SHOW_LIMIT(handles) SHOW_LIMIT(session_handles) SHOW_LIMIT(message_bytes) SHOW_LIMIT(reassembly_bytes) SHOW_LIMIT(peer_reassembly_bytes) SHOW_LIMIT(route_reassembly_bytes) SHOW_LIMIT(assemblies) SHOW_LIMIT(send_bytes) SHOW_LIMIT(send_records) SHOW_LIMIT(publisher_reliable) SHOW_LIMIT(target_states) SHOW_LIMIT(commit_pending_bytes) SHOW_LIMIT(streams) SHOW_LIMIT(stream_window) SHOW_LIMIT(receipts) SHOW_LIMIT(peer_receipts) SHOW_LIMIT(outbox_bytes) SHOW_LIMIT(outbox_records) SHOW_LIMIT(session_send_bytes) SHOW_LIMIT(session_send_records) SHOW_LIMIT(initial_send_bytes) SHOW_LIMIT(initial_send_records) SHOW_LIMIT(result_history) SHOW_LIMIT(pending_credit_requests) SHOW_LIMIT(init_tasks) SHOW_LIMIT(candidate_bytes) SHOW_LIMIT(peer_candidate_bytes) SHOW_LIMIT(directory_bytes) SHOW_LIMIT(old_directory_bytes) SHOW_LIMIT(peer_history) SHOW_LIMIT(gateway_history) SHOW_LIMIT(session_control_bytes) SHOW_LIMIT(local_control_bytes) SHOW_LIMIT(network_control_bytes) SHOW_LIMIT(command_records) SHOW_LIMIT(command_bytes)
#undef SHOW_LIMIT
        { std::lock_guard<std::mutex> lock(error_mutex); s << "},\"last_error_code\":" << last_error_code << ",\"last_error\":" << json_string(last_error); }
        s << ",\"control_port\":" << config.control_port
          << ",\"discovery_port\":" << config.discovery_port
          << ",\"discovery_group\":" << json_string(config.discovery_group) << '}';
        return s.str();
    }
    std::string query(ByteView body) const {
        if (!body.data[0]) return status();
        RouteKey key; codec::copy(key.scope, body.data + 1); key.msg_id = codec::get(body.data + 33, 4);
        auto snapshot = local_directory->snapshot(); bool verified = true, reachable = true;
        Identity peer_id = identity; std::uint64_t peer_epoch = epoch;
        if (body.data[0] == 2) {
            codec::copy(peer_id, body.data + 37); const auto peer = peer_directory->peer(peer_id);
            snapshot = peer.snapshot; verified = bool(peer.admission && snapshot);
            reachable = peer.admission && peer.admission->active.load();
            peer_epoch = peer.admission ? peer.admission->hello.gateway_epoch : 0;
        }
        std::shared_ptr<RouteAdmission> route;
        if (snapshot) { const auto found = snapshot->routes.find(key); if (found != snapshot->routes.end()) route = found->second; }
        std::ostringstream s;
        s << "{\"gateway_id\":" << json_string(local::hex(peer_id)) << ",\"gateway_epoch\":\"" << peer_epoch
          << "\",\"snapshot_version\":\"" << (snapshot ? snapshot->version : 0) << "\",\"source_verified\":" << (verified ? "true" : "false")
          << ",\"reachable\":" << (reachable ? "true" : "false") << ",\"ready\":" << (route && route->active.load() && reachable ? "true" : "false")
          << ",\"domain\":\"" << codec::get(key.scope.data() + 8, 8) << "\",\"msg_id\":" << key.msg_id;
        if (route) {
            const auto& d = route->descriptor;
            s << ",\"topic\":" << json_string(d.topic) << ",\"schema_hash\":" << d.schema_hash << ",\"roles\":" << d.role_flags
              << ",\"receiver_route_epoch\":\"" << d.receiver_route_epoch << "\"";
        }
        s << '}'; return s.str();
    }
    void queue(Session &session, Bytes packet)
    {
        if (session.output.size() >= 256 ||
            session.output_bytes + session.cache_bytes + packet.size() >
                config.limits.session_control_bytes ||
            control_memory + packet.size() > config.limits.local_control_bytes)
            throw std::runtime_error("控制输出达到配额");
        const auto size = packet.size();
        session.output.push_back(std::move(packet));
        session.output_bytes += size;
        control_memory += size;
    }
    Bytes response(Session &session, LocalKind kind, std::uint64_t request_id, const Bytes &body)
    {
        Bytes result;
        if (!encode_local({kind, request_id, session.id, epoch}, ByteView(body), result))
            throw std::runtime_error("控制响应编码失败");
        return result;
    }
    Bytes error(Session &session, std::uint64_t request_id, std::uint32_t code,
                const std::string &text)
    {
        Bytes body;
        codec::append(body, code, 4);
        codec::append(body, text.size(), 2);
        body.insert(body.end(), text.begin(), text.end());
        ++rejected;
        { std::lock_guard<std::mutex> lock(error_mutex); last_error_code = code; std::size_t length = std::min<std::size_t>(160, text.size()); while (length < text.size() && length && (static_cast<unsigned char>(text[length]) & 0xc0) == 0x80) --length; last_error = text.substr(0, length); }
        return response(session, LocalKind::Error, request_id, body);
    }
    void close_session(std::map<int, Session>::iterator i)
    {
        local_directory->close_session(i->second.id);
        if (running.load() && data && data->healthy()) synchronize_data();
        outboxes->cancel(i->second.outbox);
        if (i->second.account)
            i->second.account->close();
        control_memory -= i->second.output_bytes + i->second.cache_bytes;
        if (i->second.welcomed)
            --session_count;
        sessions.erase(i);
    }
    void remember(Session &session, std::uint64_t id, const Bytes &request, const Bytes &reply)
    {
        if (auto old = session.cache.find(id); old != session.cache.end())
        {
            const auto old_size = old->second.request.size() + old->second.response.size();
            session.cache_bytes -= old_size;
            control_memory -= old_size;
            session.cache.erase(old);
        }
        const auto size = request.size() + reply.size();
        while (!session.cache.empty() &&
               (session.cache.size() >= 64 || session.cache_bytes + size > 64 * 1024 ||
                session.cache_bytes + session.output_bytes + size >
                    config.limits.session_control_bytes))
        {
            auto i = session.cache.begin();
            const auto bytes = i->second.request.size() + i->second.response.size();
            session.cache_bytes -= bytes;
            control_memory -= bytes;
            session.cache.erase(i);
        }
        if (size + control_memory > config.limits.local_control_bytes ||
            size + session.output_bytes > config.limits.session_control_bytes)
            throw std::runtime_error("请求历史达到配额");
        session.cache.emplace(id, Cached{request, reply});
        session.cache_bytes += size;
        control_memory += size;
    }
    void process(Session &session, local::Packet packet)
    {
        LocalHeader h;
        ByteView body;
        if (!decode_local(ByteView(packet.bytes), h, body) ||
            (h.kind == LocalKind::AttachTx ? packet.descriptors.size() != 1
                                           : !packet.descriptors.empty()))
            throw std::runtime_error("无效控制包或未支持的附带 FD");
        if (session.welcomed && h.kind != LocalKind::Hello &&
            (h.session_id != session.id || h.gateway_epoch != epoch))
            throw std::runtime_error("会话代次不匹配");
        if (h.request_id <= session.last_request)
        {
            const auto cached = session.cache.find(h.request_id);
            if (cached == session.cache.end() || cached->second.request != packet.bytes)
                queue(session, error(session, h.request_id, 2, "RequestConflict"));
            else if (!cached->second.response.empty())
                queue(session, cached->second.response);
            return;
        }
        Bytes reply;
        if (!session.welcomed)
        {
            if (h.kind != LocalKind::Hello || h.request_id != 1 ||
                !std::equal(identity.begin(), identity.end(), body.data) ||
                codec::get(body.data + 16, 8) != local::process_start(session.peer.pid) ||
                codec::get(body.data + 24, 8) != clock ||
                local::clock_domain(session.peer.pid) != clock)
                throw std::runtime_error("HELLO 身份或时钟域不匹配");
            session.welcomed = true;
            ++session_count;
            session.account = send_budget->open(
                {config.limits.initial_send_bytes, config.limits.initial_send_records});
            const auto granted = session.account->granted();
            auto &welcome = session.welcome;
            welcome.locality = identity;
            welcome.max_message_bytes = config.limits.message_bytes;
            welcome.outbox_limit_bytes = config.limits.outbox_bytes;
            welcome.outbox_record_limit = config.limits.outbox_records;
            welcome.granted_bytes = granted.bytes;
            welcome.granted_records = granted.records;
            welcome.tx_name = outbox_name(identity, epoch, session.id);
            Bytes encoded;
            if (!encode_welcome(welcome, encoded))
                throw std::runtime_error("WELCOME 配额不合法");
            reply = response(session, LocalKind::Welcome, h.request_id, encoded);
        }
        else if (h.kind == LocalKind::Ping)
        {
            reply = response(session, LocalKind::Pong, h.request_id,
                             Bytes(body.data, body.data + body.size));
        }
        else if (h.kind == LocalKind::QueryState)
        {
            const auto text = query(body);
            Bytes encoded;
            codec::append(encoded, text.size(), 4);
            encoded.insert(encoded.end(), text.begin(), text.end());
            reply = response(session, LocalKind::State, h.request_id, encoded);
        }
        else if (h.kind == LocalKind::CreditRequest)
        {
            ++credit_requests;
            const auto bytes = codec::get(body.data, 4), records = codec::get(body.data + 4, 4);
            if (!session.account->grant({bytes, records}))
                reply = error(session, h.request_id, 3, "Busy");
            else
                reply = response(session, LocalKind::CreditGrant, h.request_id,
                                 encode_credit_grant(session.account->granted()));
        }
        else if (h.kind == LocalKind::AttachTx)
        {
            if (session.outbox)
                reply = error(session, h.request_id, 2, "AlreadyAttached");
            else
            {
                auto attachment = std::make_shared<OutboxAttachment>();
                attachment->welcome = session.welcome;
                attachment->session = session.id;
                attachment->epoch = epoch;
                attachment->request = h.request_id;
                attachment->account = session.account;
                attachment->event = std::move(packet.descriptors.front());
                if (!outboxes->attach(attachment))
                    reply = error(session, h.request_id, 3, "Busy");
                else
                {
                    session.outbox = std::move(attachment);
                    session.last_request = h.request_id;
                    remember(session, h.request_id, packet.bytes, {});
                    return;
                }
            }
        }
        else if (h.kind == LocalKind::RegisterPub || h.kind == LocalKind::RegisterSub)
        {
            Identity id; codec::copy(id, body.data); RouteDescriptor descriptor;
            decode_descriptor({body.data + 16, body.size - 16}, true, descriptor);
            try {
                if (pending_fences.size() >= config.limits.command_records) throw std::runtime_error("撤销屏障队列已满");
                const bool publisher = h.kind == LocalKind::RegisterPub;
                if (!publisher && (pending_subs.size() >= config.limits.command_records || pending_sub_bytes + packet.bytes.size() > config.limits.command_bytes)) throw std::runtime_error("登记等待队列已满");
                auto registration = local_directory->add(session.id, id, descriptor, publisher);
                if (publisher) reply = response(session, LocalKind::PubRegistered, h.request_id, Bytes(id.begin(), id.end()));
                else {
                    try {
                        const auto binding = registration->binding;
                        if (!binding->bridge && std::none_of(initializing.begin(), initializing.end(), [&](const auto& task) { return task.binding == binding; })) {
                            if (initializing.size() >= config.limits.init_tasks) throw std::runtime_error("bridge 初始化队列已满");
                            const auto fence = last_data_fence;
                            auto task = std::make_shared<std::packaged_task<std::shared_ptr<ShmWireBridge>()>>([descriptor, fence] {
                                if (fence.valid()) fence.get();
                                return std::make_shared<ShmWireBridge>(descriptor);
                            });
                            auto future = task->get_future().share();
                            if (!outboxes->initialize([task] { (*task)(); })) throw std::runtime_error("初始化队列已满");
                            initializing.push_back({binding, std::move(future)});
                        }
                        pending_subs.push_back({session.id, h.request_id, id, packet.bytes}); pending_sub_bytes += packet.bytes.size();
                        session.last_request = h.request_id; remember(session, h.request_id, packet.bytes, {}); return;
                    } catch (...) { local_directory->remove(session.id, id, false); throw; }
                }
                synchronize_data();
                pending_fences.push_back({session.id, h.request_id, packet.bytes, reply, last_data_fence});
                session.last_request = h.request_id; remember(session, h.request_id, packet.bytes, {}); return;
            } catch (const std::exception& e) { reply = error(session, h.request_id, 3, e.what()); }
        }
        else if (h.kind == LocalKind::SubReady || h.kind == LocalKind::Unregister)
        {
            Identity id; codec::copy(id, body.data);
            try {
                if (pending_fences.size() >= config.limits.command_records) throw std::runtime_error("撤销屏障队列已满");
                Bytes encoded(id.begin(), id.end());
                if (h.kind == LocalKind::SubReady) {
                    const auto route_epoch = local_directory->ready(session.id, id, codec::get(body.data + 16, 4));
                    codec::append(encoded, route_epoch, 8); reply = response(session, LocalKind::SubReadyAck, h.request_id, encoded);
                } else {
                    if (!local_directory->remove(session.id, id, body.data[16] == 1)) throw std::runtime_error("句柄角色不匹配");
                    session.route_versions.erase(id); reply = response(session, LocalKind::Unregistered, h.request_id, encoded);
                }
                synchronize_data();
                pending_fences.push_back({session.id, h.request_id, packet.bytes, reply, last_data_fence});
                session.last_request = h.request_id; remember(session, h.request_id, packet.bytes, {}); return;
            } catch (const std::exception& e) { reply = error(session, h.request_id, 3, e.what()); }
        }
        else
            reply = error(session, h.request_id, 1, "NotImplemented");
        session.last_request = h.request_id;
        remember(session, h.request_id, packet.bytes, reply);
        queue(session, std::move(reply));
    }
    void process_registrations()
    {
        for (auto it = initializing.begin(); it != initializing.end();) {
            if (it->result.wait_for(std::chrono::seconds(0)) != std::future_status::ready) { ++it; continue; }
            try { local_directory->set_bridge(it->binding, it->result.get()); } catch (...) { ++rejected; }
            it = initializing.erase(it);
        }
        const auto count = std::min<std::size_t>(64, pending_subs.size());
        for (std::size_t n = 0; n < count; ++n) {
            auto pending = std::move(pending_subs.front()); pending_subs.pop_front(); pending_sub_bytes -= pending.packet.size();
            auto found = std::find_if(sessions.begin(), sessions.end(), [&](const auto& entry) { return entry.second.id == pending.session; });
            if (found == sessions.end()) continue;
            auto registration = local_directory->find(pending.session, pending.id);
            if (registration && !registration->binding->bridge && std::any_of(initializing.begin(), initializing.end(), [&](const auto& task) { return task.binding == registration->binding; })) {
                pending_sub_bytes += pending.packet.size(); pending_subs.push_back(std::move(pending)); continue;
            }
            try {
                Bytes reply;
                if (!registration || !registration->binding->bridge) {
                    local_directory->remove(pending.session, pending.id, false);
                    reply = error(found->second, pending.request, 3, "BridgeUnavailable");
                } else {
                    Bytes body(pending.id.begin(), pending.id.end()); codec::append(body, registration->binding->generation, 4);
                    const auto snapshot = local_directory->snapshot(); const auto route = snapshot->routes.find(registration->binding->descriptor.key);
                    codec::append(body, route == snapshot->routes.end() ? 0 : route->second->descriptor.receiver_route_epoch, 8);
                    reply = response(found->second, LocalKind::SubRegistered, pending.request, body);
                }
                remember(found->second, pending.request, pending.packet, reply); queue(found->second, std::move(reply));
            } catch (...) { ++rejected; close_session(found); }
        }
    }
    void synchronize_data()
    {
        auto view = std::make_shared<GatewayDataView>(); view->local = local_directory->snapshot();
        if (bridge_snapshot != view->local) {
            auto bridges = std::make_shared<GatewayDataView::Bridges>();
            for (const auto& [key, route] : view->local->routes) if (route->descriptor.role_flags & 2) {
                auto binding = local_directory->binding(key); if (binding && binding->bridge) bridges->emplace(key, binding->bridge);
            }
            bridge_map = std::move(bridges); bridge_snapshot = view->local;
        }
        view->bridges = bridge_map;
        for (const auto& peer : peer_directory->peers()) view->peers.emplace(peer.admission->hello.gateway_id, peer);
        logical_publishers.store(local_directory->publisher_count()); ready_subscribers.store(local_directory->ready_count());
        active_routes.store(view->local->routes.size());
        last_data_fence = data->synchronize(std::move(view));
    }
    void process_data()
    {
        if (!data->healthy()) throw std::runtime_error("数据 shard 失效");
        for (unsigned n = 0; n < 64 && !pending_fences.empty(); ++n) {
            auto& pending = pending_fences.front();
            if (pending.fence.wait_for(std::chrono::seconds(0)) != std::future_status::ready) break;
            pending.fence.get();
            auto session = std::find_if(sessions.begin(), sessions.end(), [&](const auto& entry) { return entry.second.id == pending.session; });
            if (session != sessions.end()) { remember(session->second, pending.request, pending.packet, pending.reply); queue(session->second, std::move(pending.reply)); }
            pending_fences.pop_front();
        }
        for (unsigned n = 0; n < 64; ++n) {
            GatewayDataEvent event; if (!data->pop(event)) break;
            if (event.kind == GatewayDataEvent::Kind::Feedback) {
                WireHeader h; ByteView payload;
                if (decode_packet(ByteView(event.packet), h, payload) && allow_control(h.target_id, local::monotonic_ns())) endpoints[config.data_shards]->send(ByteView(event.packet), event.destination);
                continue;
            }
            auto session = std::find_if(sessions.begin(), sessions.end(), [&](const auto& entry) { return entry.second.id == event.header.session_id; });
            if (session == sessions.end()) continue;
            if (event.header.delivery == Delivery::Reliable) finish_send(session->second, event.header, event.result);
            queue(session->second, response(session->second, LocalKind::CreditGrant, 0, encode_credit_grant(session->second.account->granted())));
        }
    }
    void finish_send(Session& session, const OutboxHeader& header, const SendResultBody& result)
    {
        if (!session.pending_sends.erase(header.request_id)) return;
        if (config.limits.result_history) {
            while (session.result_order.size() >= config.limits.result_history) { session.send_results.erase(session.result_order.front()); session.result_order.pop_front(); }
            session.send_results.emplace(header.request_id, result); session.result_order.push_back(header.request_id);
        }
        Bytes body; encode_send_result(result, body); queue(session, response(session, LocalKind::SendResult, header.request_id, body));
    }
    bool allow_control(const Identity& id, std::uint64_t now, bool incoming = false, bool catalog = false)
    {
        if (!incoming && !catalog && (!page_sends.empty() || !catalog_requests.empty()) && non_catalog_opportunities >= 9) return false;
        auto& stamp = incoming ? receive_token_time : token_time;
        auto& global = incoming ? receive_control_tokens : control_tokens;
        if (!stamp) { stamp = now; global = config.control_burst; }
        global = std::min<double>(config.control_burst, global + (now - stamp) * 1e-9 * config.control_rate); stamp = now;
        auto& token = (incoming ? receive_tokens : peer_tokens)[id];
        const auto peer_burst = std::min<std::uint64_t>(16, config.control_burst);
        if (!token.second) { token.first = peer_burst; token.second = now; }
        token.first = std::min<double>(peer_burst, token.first + (now - token.second) * 1e-9 * config.peer_control_rate); token.second = now;
        if (global < 1 || token.first < 1) return false;
        --global; --token.first;
        if (!incoming) { if (catalog) non_catalog_opportunities = 0; else if (non_catalog_opportunities < 9) ++non_catalog_opportunities; }
        return true;
    }
    void process_network_control()
    {
        const auto now = local::monotonic_ns(); const auto own = local_directory->snapshot();
        if (now >= next_hello || own->version != last_local_version) {
            DiscoveryHello hello; hello.gateway_id = identity; hello.gateway_epoch = epoch; hello.snapshot_version = own->version;
            hello.data_base_port = config.data_base_port; hello.data_shards = config.data_shards; hello.control_port = config.control_port; hello.max_message_bytes = config.limits.message_bytes;
            Bytes packet; encode_hello(hello, packet); endpoints.back()->send(ByteView(packet), Ipv4Address::parse(config.discovery_group, config.discovery_port));
            next_hello = now + 1000000000ull;
        }
        for (unsigned which = 0; which < 2; ++which) {
            const auto end = local::monotonic_ns() + config.io_round_us * 1000;
            auto& endpoint = endpoints[config.data_shards + which];
            for (std::uint64_t n = 0; n < config.io_round_packets && local::monotonic_ns() < end; ++n) {
                ReceivedDatagram packet; const auto result = endpoint->receive(packet);
                if (result.status == IoStatus::WouldBlock) break;
                if (result.status != IoStatus::Data) { ++rejected; continue; }
                if (which) {
                    DiscoveryHello h;
                    if (!decode_hello(packet.view(), h)) { ++rejected; continue; }
                    const auto code = peer_directory->hello(h, packet.source, now);
                    if (code != DirectoryCode::Ok && code != DirectoryCode::Ignored) ++rejected;
                } else {
                    if (packet.size >= 4 && !std::memcmp(packet.bytes.data(), "DZMX", 4)) {
                        WireHeader h; ByteView body;
                        if (!decode_packet(packet.view(), h, body) || h.target_id != identity || h.target_epoch != epoch) { ++rejected; continue; }
                        const auto peer = peer_directory->peer(h.source_id);
                        if (!peer.admission || !peer.admission->active.load() || peer.admission->hello.gateway_epoch != h.source_epoch ||
                            peer.admission->ipv4 != packet.source.host || peer.admission->hello.control_port != packet.source.port) { ++rejected; continue; }
                        if (allow_control(h.source_id, now, true)) data->control(packet);
                        continue;
                    }
                    CatalogHeader h; ByteView payload;
                    if (!decode_catalog(packet.view(), h, payload)) { ++rejected; continue; }
                    if (h.kind == CatalogKind::Page) { peer_directory->page(packet, now); continue; }
                    auto peer = peer_directory->peer(h.source_id);
                    if (!peer.admission || !peer_directory->reachable(h.source_id, now) || peer.admission->hello.gateway_epoch != h.source_epoch || h.target_id != identity || h.target_epoch != epoch ||
                        packet.source.host != peer.admission->ipv4 || packet.source.port != peer.admission->hello.control_port) { ++rejected; continue; }
                    if (!page_sends.count(h.source_id) && page_sends.size() < config.limits.peers) page_sends.emplace(h.source_id, PageSend{peer, own, 0});
                }
            }
        }
        for (auto& request : peer_directory->tick(now)) {
            if (catalog_requests.size() >= config.limits.peers || catalog_bytes + request.packet.size() > config.limits.network_control_bytes) break;
            catalog_bytes += request.packet.size(); catalog_requests.push_back(std::move(request));
        }
        auto& control = endpoints[config.data_shards];
        for (unsigned n = 0, count = std::min<std::size_t>(32, catalog_requests.size()); n < count; ++n) {
            auto request = std::move(catalog_requests.front()); catalog_requests.pop_front();
            CatalogHeader h; ByteView body; decode_catalog(ByteView(request.packet), h, body);
            if (!peer_directory->reachable(h.target_id, now)) { catalog_bytes -= request.packet.size(); continue; }
            if (!allow_control(h.target_id, now, false, true)) { catalog_requests.push_back(std::move(request)); continue; }
            const auto result = control->send(ByteView(request.packet), request.destination);
            if (result.status == IoStatus::WouldBlock) { catalog_requests.push_back(std::move(request)); continue; }
            catalog_bytes -= request.packet.size();
        }
        for (unsigned sent = 0; sent < 32 && !page_sends.empty(); ++sent) {
            auto it = page_cursor ? page_sends.upper_bound(*page_cursor) : page_sends.begin();
            if (it == page_sends.end()) it = page_sends.begin(); page_cursor = it->first;
            auto& work = it->second; const auto peer = peer_directory->peer(it->first);
            if (peer.admission != work.peer.admission || !peer_directory->reachable(it->first, now)) { page_sends.erase(it); continue; }
            if (!allow_control(it->first, now, false, true)) continue;
            CatalogHeader h; h.source_id = identity; h.source_epoch = epoch; h.target_id = it->first; h.target_epoch = work.peer.admission->hello.gateway_epoch;
            auto packet = catalog_page(*work.snapshot, h, work.page);
            const auto result = control->send(ByteView(packet), {work.peer.admission->ipv4, work.peer.admission->hello.control_port});
            if (result.status == IoStatus::WouldBlock) continue;
            if (++work.page == (work.snapshot->body.size() + 1023) / 1024) page_sends.erase(it);
        }
        if (own->version != last_local_version || peer_directory->revision() != last_peer_revision) {
            ++route_revision; last_local_version = own->version; last_peer_revision = peer_directory->revision();
            synchronize_data();
        }
        const auto publishers = local_directory->publishers();
        for (std::size_t n = 0; n < std::min<std::size_t>(64, publishers.size()); ++n) {
            const auto& pub = publishers[hint_cursor++ % publishers.size()];
            auto found = std::find_if(sessions.begin(), sessions.end(), [&](const auto& entry) { return entry.second.id == pub->session; });
            if (found == sessions.end()) continue; auto& session = found->second;
            if (session.route_versions[pub->id] == route_revision || session.output.size() >= 128) continue;
            RouteStateBody hint; hint.publisher_id = pub->id; hint.state_version = route_revision;
            hint.synchronized = peer_directory->synchronized(); hint.remote_ready_count = peer_directory->targets(pub->binding->descriptor).size();
            Bytes body; encode_route_state(hint, body);
            queue(session, response(session, LocalKind::RouteState, 0, body)); session.route_versions[pub->id] = route_revision;
        }
        std::uint64_t active = 0; for (const auto& peer : peer_directory->peers()) active += peer.admission->active.load();
        active_peers.store(active); snapshot_version.store(own->version); registered_handles.store(local_directory->handle_count());
    }
    void process_outboxes()
    {
        if (!outboxes->healthy())
            throw std::runtime_error("出站消费线程异常退出");
        unsigned count = 0;
        for (; count < 64; ++count)
        {
            OutboxEvent event;
            if (!outboxes->pop(event))
                break;
            auto found = std::find_if(sessions.begin(), sessions.end(), [&](const auto &item) {
                return item.second.id == event.attachment->session;
            });
            if (found == sessions.end())
                continue;
            auto &session = found->second;
            try
            {
                if (event.attachment->cancelled.load())
                    throw std::runtime_error("出站通道失效");
                if (event.kind == OutboxEvent::Kind::Ready)
                {
                    Bytes request;
                    encode_local(
                        {LocalKind::AttachTx, event.attachment->request, session.id, epoch}, {},
                        request);
                    auto reply =
                        response(session, LocalKind::TxReady, event.attachment->request, {});
                    remember(session, event.attachment->request, request, reply);
                    queue(session, std::move(reply));
                }
                else if (event.kind == OutboxEvent::Kind::Progress)
                {
                    queue(session, response(session, LocalKind::TxProgress, 0,
                                            encode_tx_progress(event.progress)));
                }
                else if (event.kind == OutboxEvent::Kind::Record)
                {
                    ++outbox_records;
                    const auto h = event.record.header;
                    auto failure = SendResultCode::Rejected;
                    const auto publisher = local_directory->find(session.id, h.publisher_id);
                    if (h.delivery == Delivery::Reliable) {
                        const auto pending = session.pending_sends.find(h.request_id); const auto completed = session.send_results.find(h.request_id);
                        if (pending != session.pending_sends.end() || completed != session.send_results.end()) {
                            const auto id = pending != session.pending_sends.end() ? pending->second.publisher_id : completed->second.publisher_id;
                            const auto sequence = pending != session.pending_sends.end() ? pending->second.sequence : completed->second.sequence;
                            if (id != h.publisher_id || sequence != h.sequence) throw std::runtime_error("可靠请求 ID 冲突");
                            event.record.release();
                            queue(session, response(session, LocalKind::CreditGrant, 0, encode_credit_grant(session.account->granted())));
                            continue; // 同一请求只终结一次，重复 DZTX 不重发也不重新结算旧事务。
                        }
                        if (session.pending_sends.size() >= config.limits.session_send_records) throw std::runtime_error("可靠等待队列达到上限");
                        session.pending_sends.emplace(h.request_id, h);
                    }
                    if (publisher && publisher->publisher && publisher->binding->descriptor.key == h.route &&
                        (!publisher->binding->descriptor.schema_hash || h.encoding == Encoding::Tlv || publisher->binding->descriptor.schema_hash == h.schema_hash) &&
                        publisher->accept_sequence(h.sequence)) {
                        failure = SendResultCode::Busy;
                        if (data->submit(event.record, peer_directory->targets(publisher->binding->descriptor), publisher)) continue;
                    }
                    event.record.release();
                    // 未登记或无法接管的记录明确拒绝，不伪报 NoSubscribers/成功。
                    if (h.delivery == Delivery::Reliable)
                    {
                        SendResultBody result;
                        result.publisher_id = h.publisher_id;
                        result.sequence = h.sequence;
                        result.result = failure;
                        finish_send(session, h, result);
                    }
                    queue(session, response(session, LocalKind::CreditGrant, 0,
                                            encode_credit_grant(session.account->granted())));
                }
                else
                    throw std::runtime_error("出站初始化或消费失败");
            }
            catch (...)
            {
                ++rejected;
                close_session(found);
            }
        }
        if (count == 64)
            local::notify(wake.get());
        for (auto it = sessions.begin(); it != sessions.end();)
        {
            auto current = it++;
            if (current->second.outbox && current->second.outbox->cancelled.load())
                close_session(current);
        }
    }
    void run() noexcept
    {
        try
        {
            while (running.load())
            {
                process_outboxes();
                process_registrations();
                process_data();
                if (!local_directory->healthy()) throw std::runtime_error("路由撤销时目录资源耗尽");
                process_network_control();
                std::vector<pollfd> fds{{wake.get(), POLLIN, 0}, {listener->fd(), POLLIN, 0}};
                std::vector<std::uint64_t> generations{0, 0};
                for (const auto &entry : sessions)
                {
                    fds.push_back(
                        {entry.first,
                         static_cast<short>(POLLIN | (entry.second.output.empty() ? 0 : POLLOUT)),
                         0});
                    generations.push_back(entry.second.id);
                }
                const auto session_end = fds.size();
                fds.push_back({endpoints[config.data_shards]->native_handle(), POLLIN, 0});
                fds.push_back({endpoints.back()->native_handle(), POLLIN, 0});
                if (::poll(fds.data(), fds.size(), 10) < 0)
                {
                    if (errno == EINTR)
                        continue;
                    throw std::runtime_error("网关控制轮询失败");
                }
                if (fds[0].revents)
                    local::drain_event(wake.get());
                if (!running.load())
                    break;
                if (fds[1].revents & POLLIN)
                    for (unsigned accepted = 0; accepted < 16; ++accepted)
                    {
                        local::Fd fd(::accept4(listener->fd(), nullptr, nullptr,
                                               SOCK_NONBLOCK | SOCK_CLOEXEC));
                        if (!fd)
                        {
                            if (errno == EAGAIN || errno == EINTR)
                                break;
                            throw std::runtime_error("接纳控制连接失败");
                        }
                        auto peer = local::credentials(fd.get());
                        if (peer.uid != geteuid() || sessions.size() >= config.limits.sessions ||
                            next_session == UINT64_MAX)
                        {
                            ++rejected;
                            continue;
                        }
                        const int raw = fd.get();
                        Session session;
                        session.fd = std::move(fd);
                        session.peer = peer;
                        session.id = next_session++;
                        session.connected_at = local::monotonic_ns();
                        sessions.emplace(raw, std::move(session));
                    }
                for (std::size_t index = 2; index < session_end; ++index)
                {
                    auto found = sessions.find(fds[index].fd);
                    if (found == sessions.end() || found->second.id != generations[index])
                        continue;
                    auto &session = found->second;
                    try
                    {
                        if ((!session.welcomed &&
                             local::monotonic_ns() - session.connected_at > 2000000000ull) ||
                            (fds[index].revents & (POLLHUP | POLLERR | POLLNVAL)))
                            throw std::runtime_error("会话已关闭");
                        if (fds[index].revents & POLLIN)
                            for (unsigned received = 0; received < 16; ++received)
                            {
                                local::Packet packet;
                                const auto state = local::receive(session.fd.get(), packet);
                                if (state == local::Receive::WouldBlock)
                                    break;
                                if (state != local::Receive::Packet)
                                    throw std::runtime_error("控制接收异常");
                                process(session, std::move(packet));
                            }
                        for (unsigned sent = 0; sent < 16 && !session.output.empty(); ++sent)
                        {
                            if (!local::send(session.fd.get(), ByteView(session.output.front())))
                                break;
                            const auto bytes = session.output.front().size();
                            session.output_bytes -= bytes;
                            control_memory -= bytes;
                            session.output.pop_front();
                        }
                    }
                    catch (...)
                    {
                        ++rejected;
                        close_session(found);
                    }
                }
            }
        }
        catch (...)
        {
            ++rejected;
        }
        running.store(false);
        while (!sessions.empty())
            close_session(sessions.begin());
    }
};
GatewayRuntime::GatewayRuntime(GatewayConfig config) : impl_(new Impl)
{
    auto status = validate_host_interface(config);
    if (!status)
        throw ConfigError(status);
    impl_->config = std::move(config);
    impl_->identity = local::locality();
    impl_->epoch = local::random_epoch();
    impl_->clock = local::clock_domain(getpid());
    impl_->listener = std::make_unique<local::Listener>(impl_->config.control_path);
    impl_->endpoints = open_gateway_endpoints(impl_->config);
    impl_->wake = local::event();
    impl_->send_budget = std::make_unique<SendBudget>(
        CreditCounters{impl_->config.limits.send_bytes, impl_->config.limits.send_records},
        CreditCounters{impl_->config.limits.session_send_bytes,
                       impl_->config.limits.session_send_records});
    impl_->outboxes = std::make_unique<OutboxService>(
        impl_->wake.get(), impl_->config.limits.init_tasks, impl_->config.limits.command_records);
    impl_->directory_budget = std::make_shared<DirectoryBudget>(impl_->config.limits);
    impl_->peer_directory = std::make_unique<PeerDirectory>(impl_->identity, impl_->epoch, impl_->directory_budget);
    impl_->local_directory = std::make_unique<LocalDirectory>(impl_->directory_budget, impl_->config.limits);
    for (const auto& endpoint : impl_->endpoints) impl_->socket_buffers.emplace_back(endpoint->receive_buffer_bytes(), endpoint->send_buffer_bytes());
    std::vector<std::unique_ptr<DatagramEndpoint>> data_endpoints;
    for (unsigned i = 0; i < impl_->config.data_shards; ++i) data_endpoints.push_back(std::move(impl_->endpoints[i]));
    impl_->data = std::make_unique<GatewayData>(impl_->config, impl_->identity, impl_->epoch, impl_->wake.get(), std::move(data_endpoints));
    impl_->synchronize_data();
    impl_->thread = std::thread([p = impl_.get()] { p->run(); });
}
GatewayRuntime::~GatewayRuntime()
{
    stop();
}
void GatewayRuntime::stop()
{
    std::lock_guard<std::mutex> lock(impl_->stop_mutex);
    impl_->running.store(false);
    local::notify(impl_->wake.get());
    if (impl_->thread.joinable())
        impl_->thread.join();
    impl_->data->stop();
    impl_->outboxes.reset();
    impl_->pending_subs.clear(); impl_->pending_sub_bytes = 0;
    impl_->initializing.clear(); impl_->pending_fences.clear();
    impl_->page_sends.clear(); impl_->catalog_requests.clear(); impl_->catalog_bytes = 0;
    impl_->peer_tokens.clear(); impl_->receive_tokens.clear();
    impl_->bridge_map.reset(); impl_->bridge_snapshot.reset();
    impl_->local_directory.reset(); impl_->peer_directory.reset();
    impl_->registered_handles.store(0); impl_->active_peers.store(0);
    impl_->logical_publishers.store(0); impl_->ready_subscribers.store(0); impl_->active_routes.store(0);
    impl_->endpoints.clear();
    impl_->listener.reset();
}
bool GatewayRuntime::running() const noexcept
{
    return impl_->running.load();
}
std::string GatewayRuntime::status_json() const
{
    return impl_->status();
}
} // namespace dzIPC::net

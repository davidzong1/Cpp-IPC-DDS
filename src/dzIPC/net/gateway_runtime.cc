#include "dzIPC/net/gateway_runtime.h"
#include "byte_codec.h"
#include "dzIPC/net/datagram_endpoint.h"
#include "dzIPC/net/local_directory.h"
#include "local_control_linux.h"
#include "outbox_service.h"
#include "gateway_data.h"
#include <algorithm>
#include <atomic>
#include <charconv>
#include <cctype>
#include <deque>
#include <fstream>
#include <map>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <future>
#include <optional>
#include <set>
#include <limits>
#include <tuple>
#include <iterator>
#include <iostream>
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

// 策略文件只在网关启动时解析。解析器故意只接受本方案定义的 JSON 子集，
// 这样未知字段会立即失败，不会悄悄退回默认端点。
struct PolicyJson {
    enum class Kind { Object, Array, String, Number, Boolean, Null } kind = Kind::Null;
    std::map<std::string, PolicyJson> object;
    std::vector<PolicyJson> array;
    std::string string;
    std::uint64_t number = 0;
    bool boolean = false;
};
class PolicyJsonParser {
    const std::string &text; std::size_t pos = 0;
    [[noreturn]] void fail(const std::string &message) const { throw ConfigError({ConfigCode::InvalidOption, "topic-policy-file: " + message}); }
    void whitespace() { while (pos < text.size() && std::isspace(static_cast<unsigned char>(text[pos]))) ++pos; }
    bool consume(char c) { whitespace(); if (pos < text.size() && text[pos] == c) { ++pos; return true; } return false; }
    std::string parse_string() {
        whitespace(); if (pos >= text.size() || text[pos++] != '"') fail("需要字符串");
        std::string value;
        while (pos < text.size()) {
            const char c = text[pos++];
            if (c == '"') return value;
            if (static_cast<unsigned char>(c) < 0x20) fail("字符串包含控制字符");
            if (c != '\\') { value += c; continue; }
            if (pos >= text.size()) fail("字符串转义不完整");
            const char escaped = text[pos++];
            switch (escaped) {
            case '"': value += '"'; break; case '\\': value += '\\'; break;
            case '/': value += '/'; break; case 'b': value += '\b'; break;
            case 'f': value += '\f'; break; case 'n': value += '\n'; break;
            case 'r': value += '\r'; break; case 't': value += '\t'; break;
            default: fail("仅支持基本字符串转义");
            }
        }
        fail("字符串未闭合");
    }
    PolicyJson value() {
        whitespace(); if (pos >= text.size()) fail("值缺失");
        if (text[pos] == '{') return object(); if (text[pos] == '[') return array();
        if (text[pos] == '"') { PolicyJson out; out.kind = PolicyJson::Kind::String; out.string = parse_string(); return out; }
        if (text.compare(pos, 4, "true") == 0) { pos += 4; PolicyJson out; out.kind = PolicyJson::Kind::Boolean; out.boolean = true; return out; }
        if (text.compare(pos, 5, "false") == 0) { pos += 5; PolicyJson out; out.kind = PolicyJson::Kind::Boolean; out.boolean = false; return out; }
        if (text.compare(pos, 4, "null") == 0) { pos += 4; return {}; }
        if (text[pos] < '0' || text[pos] > '9') fail("不支持的值");
        std::uint64_t number = 0; const auto begin = pos;
        while (pos < text.size() && std::isdigit(static_cast<unsigned char>(text[pos]))) {
            const auto digit = static_cast<unsigned>(text[pos++] - '0');
            if (number > (UINT64_MAX - digit) / 10) fail("数字溢出");
            number = number * 10 + digit;
        }
        if (pos != begin && pos < text.size() && (text[pos] == '.' || text[pos] == 'e' || text[pos] == 'E' || text[pos] == '-')) fail("只接受无符号整数");
        PolicyJson out; out.kind = PolicyJson::Kind::Number; out.number = number; return out;
    }
    PolicyJson object() {
        PolicyJson out; out.kind = PolicyJson::Kind::Object; if (!consume('{')) fail("对象缺少左大括号");
        whitespace(); if (consume('}')) return out;
        for (;;) {
            const auto key = parse_string(); if (!consume(':')) fail("对象缺少冒号");
            if (!out.object.emplace(key, value()).second) fail("对象字段重复: " + key);
            if (consume('}')) return out; if (!consume(',')) fail("对象缺少逗号");
        }
    }
    PolicyJson array() {
        PolicyJson out; out.kind = PolicyJson::Kind::Array; if (!consume('[')) fail("数组缺少左中括号");
        whitespace(); if (consume(']')) return out;
        for (;;) { out.array.push_back(value()); if (consume(']')) return out; if (!consume(',')) fail("数组缺少逗号"); }
    }
public:
    explicit PolicyJsonParser(const std::string &input) : text(input) {}
    PolicyJson parse() { auto out = value(); whitespace(); if (pos != text.size()) fail("尾部存在多余内容"); return out; }
};
struct PolicyRouteKey {
    std::string topic; std::uint64_t domain = 0; std::uint32_t msg_id = 0;
    bool operator<(const PolicyRouteKey &other) const { return std::tie(topic, domain, msg_id) < std::tie(other.topic, other.domain, other.msg_id); }
};
struct TopicPolicy {
    bool dedicated = false, worker_set = false, exclusive_worker = false;
    unsigned worker = 0; std::uint64_t receive_buffer = 0, send_buffer = 0;
};
struct TopicPolicySet {
    TopicPolicy default_policy;
    std::map<PolicyRouteKey, TopicPolicy> routes;
    std::set<unsigned> exclusive_workers;
};
const PolicyJson &member(const PolicyJson &object, const std::string &name, bool required = false) {
    if (object.kind != PolicyJson::Kind::Object) throw ConfigError({ConfigCode::InvalidOption, "topic-policy-file: 需要对象"});
    const auto found = object.object.find(name);
    if (found == object.object.end()) { if (required) throw ConfigError({ConfigCode::InvalidOption, "topic-policy-file: 缺少字段 " + name}); static const PolicyJson null; return null; }
    return found->second;
}
void policy_fields(const PolicyJson &object, const std::set<std::string> &allowed) {
    for (const auto &[name, value] : object.object) if (!allowed.count(name)) throw ConfigError({ConfigCode::InvalidOption, "topic-policy-file: 未知字段 " + name});
}
std::string policy_string(const PolicyJson &value, const std::string &name) {
    if (value.kind != PolicyJson::Kind::String || value.string.empty()) throw ConfigError({ConfigCode::InvalidOption, "topic-policy-file: " + name + " 必须为非空字符串"});
    return value.string;
}
std::uint64_t policy_number(const PolicyJson &value, const std::string &name) {
    if (value.kind != PolicyJson::Kind::Number) throw ConfigError({ConfigCode::InvalidOption, "topic-policy-file: " + name + " 必须为无符号整数"});
    return value.number;
}
std::uint64_t policy_domain(const PolicyJson &value, const std::string &name) {
    if (value.kind == PolicyJson::Kind::Number) return value.number;
    if (value.kind != PolicyJson::Kind::String || value.string.empty())
        throw ConfigError({ConfigCode::InvalidOption, "topic-policy-file: " + name + " 必须为十进制域字符串"});
    std::uint64_t result = 0;
    const auto parsed = std::from_chars(value.string.data(), value.string.data() + value.string.size(), result, 10);
    if (parsed.ec != std::errc{} || parsed.ptr != value.string.data() + value.string.size())
        throw ConfigError({ConfigCode::InvalidOption, "topic-policy-file: " + name + " 必须为十进制域字符串"});
    return result;
}
TopicPolicy parse_topic_policy(const PolicyJson &object, const std::string &where, unsigned worker_count) {
    policy_fields(object, {"topic", "domain", "msg_id", "endpoint", "worker", "exclusive_worker", "data_rcvbuf_bytes", "data_sndbuf_bytes"});
    TopicPolicy policy; const auto &endpoint = member(object, "endpoint", true);
    const auto endpoint_name = policy_string(endpoint, where + ".endpoint");
    if (endpoint_name == "dedicated") policy.dedicated = true;
    else if (endpoint_name == "pooled") policy.dedicated = false;
    else throw ConfigError({ConfigCode::InvalidOption, "topic-policy-file: endpoint 必须为 pooled 或 dedicated"});
    const auto &worker = member(object, "worker");
    if (worker.kind != PolicyJson::Kind::Null) {
        const auto value = policy_number(worker, where + ".worker");
        if (value >= worker_count) throw ConfigError({ConfigCode::InvalidOption, "topic-policy-file: worker 超出 data-workers"});
        policy.worker = static_cast<unsigned>(value); policy.worker_set = true;
    }
    const auto &exclusive = member(object, "exclusive_worker");
    if (exclusive.kind != PolicyJson::Kind::Null) { if (exclusive.kind != PolicyJson::Kind::Boolean) throw ConfigError({ConfigCode::InvalidOption, "topic-policy-file: exclusive_worker 必须为布尔值"}); policy.exclusive_worker = exclusive.boolean; }
    for (const auto *field : {"data_rcvbuf_bytes", "data_sndbuf_bytes"}) {
        const auto &value = member(object, field); if (value.kind == PolicyJson::Kind::Null) continue;
        const auto bytes = policy_number(value, std::string(where) + "." + field); if (!bytes || bytes > 64 * kMiB) throw ConfigError({ConfigCode::BufferBudget, "topic-policy-file: 单端点缓冲超出范围"});
        if (std::string(field) == "data_rcvbuf_bytes") policy.receive_buffer = bytes; else policy.send_buffer = bytes;
    }
    if ((policy.worker_set || policy.exclusive_worker || policy.receive_buffer || policy.send_buffer) && !policy.dedicated)
        throw ConfigError({ConfigCode::InvalidOption, "topic-policy-file: pooled 端点不能指定 worker 或缓冲覆盖"});
    if (policy.exclusive_worker && !policy.worker_set) throw ConfigError({ConfigCode::InvalidOption, "topic-policy-file: exclusive_worker 必须同时指定 worker"});
    return policy;
}
TopicPolicySet load_topic_policy(const GatewayConfig &config) {
    TopicPolicySet result; result.default_policy.dedicated = config.data_mode == DataMode::PerTopic;
    if (config.topic_policy_file.empty()) return result;
    std::ifstream stream(config.topic_policy_file, std::ios::binary);
    if (!stream) throw ConfigError({ConfigCode::InvalidOption, "无法读取 topic-policy-file"});
    const std::string text((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    const auto root = PolicyJsonParser(text).parse(); policy_fields(root, {"default", "routes"});
    const auto &default_object = member(root, "default", true); result.default_policy = parse_topic_policy(default_object, "default", static_cast<unsigned>(config.data_workers));
    if (config.data_mode == DataMode::Pooled && result.default_policy.dedicated)
        throw ConfigError({ConfigCode::InvalidOption, "pooled 的 default 必须为 pooled"});
    if (config.data_mode == DataMode::Hybrid && result.default_policy.dedicated) throw ConfigError({ConfigCode::InvalidOption, "hybrid 的 default 必须为 pooled"});
    if (config.data_mode == DataMode::PerTopic && !result.default_policy.dedicated) throw ConfigError({ConfigCode::InvalidOption, "per-topic 的 default 必须为 dedicated"});
    const auto &routes = member(root, "routes", true); if (routes.kind != PolicyJson::Kind::Array) throw ConfigError({ConfigCode::InvalidOption, "topic-policy-file: routes 必须为数组"});
    for (std::size_t i = 0; i < routes.array.size(); ++i) {
        const auto &entry = routes.array[i]; policy_fields(entry, {"topic", "domain", "msg_id", "endpoint", "worker", "exclusive_worker", "data_rcvbuf_bytes", "data_sndbuf_bytes"});
        const auto topic = policy_string(member(entry, "topic", true), "routes.topic");
        const auto domain = policy_domain(member(entry, "domain", true), "routes.domain");
        const auto msg = policy_number(member(entry, "msg_id", true), "routes.msg_id"); if (msg > UINT32_MAX) throw ConfigError({ConfigCode::InvalidOption, "topic-policy-file: msg_id 超出范围"});
        const auto key = PolicyRouteKey{topic, domain, static_cast<std::uint32_t>(msg)}; const auto policy = parse_topic_policy(entry, "routes", static_cast<unsigned>(config.data_workers));
        if (!result.routes.emplace(key, policy).second) throw ConfigError({ConfigCode::InvalidOption, "topic-policy-file: RouteKey 重复"});
        if (config.data_mode == DataMode::Pooled && policy.dedicated) throw ConfigError({ConfigCode::InvalidOption, "pooled 模式禁止 dedicated 策略"});
        if (config.data_mode == DataMode::PerTopic && !policy.dedicated) throw ConfigError({ConfigCode::InvalidOption, "per-topic 模式禁止 pooled 策略"});
        if (policy.exclusive_worker && !result.exclusive_workers.insert(policy.worker).second) throw ConfigError({ConfigCode::InvalidOption, "exclusive worker 被重复指定"});
    }
    if (result.default_policy.exclusive_worker || result.default_policy.worker_set) throw ConfigError({ConfigCode::InvalidOption, "default 不能指定 worker"});
    for (const auto& [key, policy] : result.routes) {
        (void)key;
        if (policy.worker_set && !policy.exclusive_worker && result.exclusive_workers.count(policy.worker))
            throw ConfigError({ConfigCode::InvalidOption, "普通 dedicated 话题不能使用 exclusive_worker"});
    }
    if (result.exclusive_workers.size() >= config.data_workers)
        throw ConfigError({ConfigCode::OwnerFailed, "exclusive_worker 必须为未指定 worker 的话题保留普通 worker"});
    return result;
}
} // namespace
struct GatewayRuntime::Impl
{
    std::shared_ptr<NetMetrics> metrics = std::make_shared<NetMetrics>("gateway");
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
    std::vector<std::uint16_t> socket_buffer_ports;
    EndpointResourceAudit resource_audit;
    std::uint64_t next_endpoint_epoch = 1;
    TopicPolicySet topic_policy;
    std::vector<std::uint16_t> pooled_ports;
    std::vector<std::uint64_t> pooled_epochs;
    std::vector<unsigned> pooled_workers;
    std::vector<unsigned> ordinary_workers;
    struct DedicatedEndpoint { std::uint16_t port = 0; std::uint64_t epoch = 0; unsigned worker = 0; unsigned refs = 0; bool ready = false; };
    std::map<RouteKey, DedicatedEndpoint> dedicated_endpoints;
    std::set<std::uint16_t> closing_ports;
    std::atomic<std::uint64_t> creating_endpoints{0};
    std::thread thread;
    std::mutex stop_mutex;
    std::atomic<bool> running{true};
    std::atomic<std::uint64_t> session_count{0}, rejected{0}, outbox_records{0}, credit_requests{0};
    std::size_t control_memory = 0;
    mutable std::mutex error_mutex; std::uint32_t last_error_code = 0; std::string last_error;
    std::uint32_t last_endpoint_failure_code = 0; std::string last_endpoint_failure;
    std::string runtime_failure;
    void note_runtime_failure(const char *where, const std::exception *error = nullptr) noexcept {
        try {
            std::lock_guard<std::mutex> lock(error_mutex);
            runtime_failure = where;
            if (error) {
                runtime_failure += ": ";
                runtime_failure += error->what();
            }
        } catch (...) {
            // Failure reporting must not turn an already stopping gateway into terminate().
        }
    }
    void note_endpoint_failure(ConfigCode code, const std::string &detail) {
        std::lock_guard<std::mutex> lock(error_mutex);
        last_endpoint_failure_code = static_cast<std::uint32_t>(code);
        last_endpoint_failure = std::string(config_code_name(code)) + ": " + detail;
    }
    unsigned route_owner(const RouteDescriptor &descriptor) const {
        if ((descriptor.endpoint_flags & 1u) != 0) {
            const auto found = dedicated_endpoints.find(descriptor.key);
            if (found != dedicated_endpoints.end()) return found->second.worker;
        }
        if (!pooled_ports.empty()) {
            auto found = std::find(pooled_ports.begin(), pooled_ports.end(), descriptor.data_port);
            const auto index = found == pooled_ports.end() ? route_hash(descriptor.key) % pooled_ports.size()
                                                            : static_cast<std::size_t>(found - pooled_ports.begin());
            return pooled_workers.empty() ? 0 : pooled_workers[index];
        }
        return 0;
    }
    std::string status() const
    {
        std::ostringstream s;
        const auto occupied = send_budget->occupied();
        const auto inflight = send_budget->inflight();
        const auto traffic = data ? data->stats() : GatewayDataStats{};
        const auto directory = directory_budget->usage();
        s << "{\"protocol_version\":" << static_cast<unsigned>(config.network_version)
          << ",\"network_version\":" << static_cast<unsigned>(config.network_version)
          << ",\"data_mode\":" << json_string(config.data_mode == DataMode::Pooled ? "pooled" : config.data_mode == DataMode::Hybrid ? "hybrid" : "per-topic")
          << ",\"local_protocol_version\":2,\"gateway_id\":"
          << json_string(local::hex(identity)) << ",\"gateway_epoch\":\"" << epoch
          << "\",\"state\":\"" << (running.load() ? "Ready" : "Stopped")
          << "\",\"data_plane_ready\":" << (running.load() ? "true" : "false") << ",\"listen_ip\":" << json_string(config.listen_ip)
          << ",\"interface\":" << json_string(config.interface)
          << ",\"udp_sockets\":" << (running.load() ? config.data_ports.size() + 2 : 0)
          << ",\"data_sockets\":" << (running.load() ? config.data_ports.size() : 0)
          << ",\"data_workers\":" << (running.load() ? config.data_workers : 0)
          << ",\"data_socket_cap\":" << config.data_socket_cap
          << ",\"socket_buffer_budget_bytes\":" << config.socket_buffer_budget_bytes
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
        for (std::size_t i = 0; i < config.data_ports.size(); ++i)
        {
            if (i)
                s << ',';
            s << config.data_ports[i];
        }
        s << "],\"pooled_socket_workers\":[";
        for (std::size_t i = 0; i < pooled_workers.size(); ++i) { if (i) s << ','; s << pooled_workers[i]; }
        s << "],\"exclusive_workers\":[";
        bool first_exclusive = true;
        for (const auto worker : topic_policy.exclusive_workers) { if (!first_exclusive) s << ','; first_exclusive = false; s << worker; }
        s << "],\"socket_buffers\":[";
        for (std::size_t n = 0; n < socket_buffers.size(); ++n) {
            if (n) s << ',';
            s << "{\"receive_bytes\":" << socket_buffers[n].first << ",\"send_bytes\":" << socket_buffers[n].second << '}';
        }
        s << "],\"endpoint_states\":{\"creating\":" << creating_endpoints.load()
          << ",\"ready\":" << (pooled_ports.size() + std::count_if(dedicated_endpoints.begin(), dedicated_endpoints.end(),
              [](const auto &entry) { return entry.second.ready; }))
          << ",\"closing\":" << closing_ports.size() << "},\"resource_budget\":{\"fd_soft_limit\":" << resource_audit.fd_soft_limit
          << ",\"fd_hard_limit\":" << resource_audit.fd_hard_limit
          << ",\"fd_count\":" << resource_audit.fd_count
          << ",\"fd_budget_limit\":" << resource_audit.fd_budget_limit
          << ",\"fd_reserve\":" << resource_audit.fd_reserve
          << ",\"candidate_ports\":" << resource_audit.candidate_ports
          << ",\"reserved_ports\":" << resource_audit.reserved_ports
          << ",\"actual_receive_buffer_bytes\":" << resource_audit.actual_receive_buffer_bytes
          << ",\"actual_send_buffer_bytes\":" << resource_audit.actual_send_buffer_bytes
          << ",\"buffer_budget_bytes\":" << resource_audit.buffer_budget_bytes << '}';
        s << ",\"logical_publishers\":" << logical_publishers.load()
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
          << ",\"gateway_threads\":" << (running.load() ? config.data_workers + 3 : 0)
          << ",\"thread_scope\":\"控制、drain、初始化和 W 个 data worker；每个 worker 可持有多个数据 socket；SHM 公共调度线程另由 /proc 采样\""
          << ",\"limits\":{";
        bool first_limit = true;
#define SHOW_LIMIT(field) if (!first_limit) s << ','; first_limit = false; s << "\"" #field "\":" << config.limits.field;
        SHOW_LIMIT(topics) SHOW_LIMIT(sessions) SHOW_LIMIT(peers) SHOW_LIMIT(handles) SHOW_LIMIT(session_handles) SHOW_LIMIT(message_bytes) SHOW_LIMIT(reassembly_bytes) SHOW_LIMIT(peer_reassembly_bytes) SHOW_LIMIT(route_reassembly_bytes) SHOW_LIMIT(assemblies) SHOW_LIMIT(send_bytes) SHOW_LIMIT(send_records) SHOW_LIMIT(publisher_reliable) SHOW_LIMIT(target_states) SHOW_LIMIT(commit_pending_bytes) SHOW_LIMIT(streams) SHOW_LIMIT(stream_window) SHOW_LIMIT(receipts) SHOW_LIMIT(peer_receipts) SHOW_LIMIT(outbox_bytes) SHOW_LIMIT(outbox_records) SHOW_LIMIT(session_send_bytes) SHOW_LIMIT(session_send_records) SHOW_LIMIT(initial_send_bytes) SHOW_LIMIT(initial_send_records) SHOW_LIMIT(result_history) SHOW_LIMIT(pending_credit_requests) SHOW_LIMIT(init_tasks) SHOW_LIMIT(candidate_bytes) SHOW_LIMIT(peer_candidate_bytes) SHOW_LIMIT(directory_bytes) SHOW_LIMIT(old_directory_bytes) SHOW_LIMIT(peer_history) SHOW_LIMIT(gateway_history) SHOW_LIMIT(session_control_bytes) SHOW_LIMIT(local_control_bytes) SHOW_LIMIT(network_control_bytes) SHOW_LIMIT(command_records) SHOW_LIMIT(command_bytes)
#undef SHOW_LIMIT
        s << "},\"routes\":[";
        bool first_route = true;
        const auto routes = local_directory ? local_directory->route_statuses()
                                             : std::vector<LocalRouteStatus>{};
        for (const auto &route : routes) {
            if (!first_route) s << ',';
            first_route = false;
            auto descriptor = route.descriptor;
            if (config.network_version == NetworkVersion::V1 && !descriptor.data_port)
                descriptor.data_port = data_port(descriptor.key, static_cast<std::uint16_t>(config.data_base_port), static_cast<std::uint16_t>(config.data_shards));
            const bool dedicated = (descriptor.endpoint_flags & 1u) != 0;
            const auto endpoint = dedicated ? dedicated_endpoints.find(descriptor.key) : dedicated_endpoints.end();
            const auto endpoint_refs = endpoint == dedicated_endpoints.end() ? 0u : endpoint->second.refs;
            const bool ready = route.descriptor.role_flags != 0 &&
                (!dedicated || (endpoint != dedicated_endpoints.end() && endpoint->second.ready));
            const char *state = ready ? "Ready" : (dedicated && endpoint != dedicated_endpoints.end() ? "Closing" : "Creating");
            const auto domain = codec::get(descriptor.key.scope.data() + 8, 8);
            s << "{\"topic\":" << json_string(descriptor.topic) << ",\"domain\":\"" << domain
              << "\",\"msg_id\":" << descriptor.key.msg_id << ",\"data_port\":" << descriptor.data_port
              << ",\"endpoint_epoch\":\"" << descriptor.endpoint_epoch << "\",\"endpoint_flags\":" << descriptor.endpoint_flags
              << ",\"endpoint\":" << json_string(dedicated ? "dedicated" : "pooled")
              << ",\"owner\":" << route_owner(descriptor) << ",\"endpoint_refs\":" << endpoint_refs
              << ",\"state\":" << json_string(state)
              << ",\"publisher_refs\":" << route.publishers << ",\"subscriber_refs\":" << route.subscribers
              << ",\"ready_subscribers\":" << route.ready_subscribers << '}';
        }
        s << "]";
        { std::lock_guard<std::mutex> lock(error_mutex); s << ",\"last_error_code\":" << last_error_code << ",\"last_error\":" << json_string(last_error)
          << ",\"last_endpoint_failure_code\":" << last_endpoint_failure_code
          << ",\"last_endpoint_failure\":" << json_string(last_endpoint_failure)
          << ",\"runtime_failure\":" << json_string(runtime_failure); }
        s << ",\"control_port\":" << config.control_port
          << ",\"discovery_port\":" << config.discovery_port
          << ",\"discovery_group\":" << json_string(config.discovery_group) << '}';
        return s.str();
    }
    std::string query(ByteView body) const {
        if (!body.data[0]) return status();
        if (body.data[0] == 3) {
            if (body.data[1] == 3) return data->shard_metrics();
            if (body.data[1] == 4) return metrics->trace_page(codec::get(body.data + 2, 4)).json;
            return metrics->json(body.data[1]);
        }
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
              << ",\"receiver_route_epoch\":\"" << d.receiver_route_epoch << "\",\"metrics\":" << route->metrics.json();
        }
        s << '}'; return s.str();
    }
    void queue(Session &session, Bytes packet)
    {
        if (session.output.size() >= 256 ||
            session.output_bytes + session.cache_bytes + packet.size() >
                config.limits.session_control_bytes ||
            control_memory + packet.size() > config.limits.local_control_bytes)
            { if (session.output_bytes + session.cache_bytes + packet.size() > config.limits.session_control_bytes) metrics->reject(NetQuota::session_control_bytes);
              if (control_memory + packet.size() > config.limits.local_control_bytes) metrics->reject(NetQuota::local_control_bytes);
              throw std::runtime_error("控制输出达到配额"); }
        const auto size = packet.size();
        session.output.push_back(std::move(packet));
        session.output_bytes += size;
        control_memory += size; metrics->peak(NetQuota::local_control_bytes, control_memory); metrics->peak(NetQuota::session_control_bytes, session.output_bytes + session.cache_bytes);
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
        const auto routes = local_directory->session_routes(i->second.id);
        local_directory->close_session(i->second.id);
        if (running.load() && data && data->healthy()) synchronize_data();
        for (const auto &route : routes) release_dedicated(route);
        outboxes->cancel(i->second.outbox);
        if (i->second.account)
            i->second.account->close();
        control_memory -= i->second.output_bytes + i->second.cache_bytes;
        if (i->second.welcomed)
            --session_count;
        sessions.erase(i);
    }
    TopicPolicy policy_for(const RouteKey &route, const std::string &topic) const {
        const auto domain = codec::get(route.scope.data() + 8, 8);
        const auto found = topic_policy.routes.find({topic, domain, route.msg_id});
        if (found != topic_policy.routes.end()) return found->second;
        return topic_policy.default_policy;
    }
    std::uint16_t allocate_data_port(std::uint64_t first_candidate) const {
        const auto split = config.data_port_range.find(':'); const auto first = std::stoull(config.data_port_range.substr(0, split)); const auto last = std::stoull(config.data_port_range.substr(split + 1));
        for (std::uint64_t port = std::max<std::uint64_t>(first, first_candidate); port <= last; ++port) {
            if (port == config.control_port || port == config.discovery_port || port > UINT16_MAX || closing_ports.count(static_cast<std::uint16_t>(port))) continue;
            bool used = false; for (const auto &entry : dedicated_endpoints) if (entry.second.port == port) { used = true; break; }
            if (std::find(config.data_ports.begin(), config.data_ports.end(), port) != config.data_ports.end()) used = true;
            if (!used) return static_cast<std::uint16_t>(port);
        }
        return 0;
    }
    void create_dedicated(const RouteKey &route, const RouteDescriptor &descriptor) {
        if (config.network_version != NetworkVersion::V2 || !policy_for(route, descriptor.topic).dedicated) return;
        if (dedicated_endpoints.count(route)) { ++dedicated_endpoints[route].refs; return; }
        creating_endpoints.fetch_add(1, std::memory_order_relaxed);
        struct CreatingGuard {
            std::atomic<std::uint64_t> &count;
            ~CreatingGuard() { count.fetch_sub(1, std::memory_order_relaxed); }
        } creating_guard{creating_endpoints};
        if (config.data_mode == DataMode::Pooled && policy_for(route, descriptor.topic).dedicated) throw ConfigError({ConfigCode::InvalidOption, "pooled 模式不允许专用端点"});
        if (config.data_ports.size() >= config.data_socket_cap) throw ConfigError({ConfigCode::SocketCap, "数据 socket 达到 data-socket-cap"});
        if (resource_audit.fd_budget_limit <= resource_audit.fd_count ||
            resource_audit.fd_budget_limit - resource_audit.fd_count <= resource_audit.fd_reserve)
            throw ConfigError({ConfigCode::FdBudget, "专用端点将超过 FD 半数预算"});
        if (resource_audit.candidate_ports && resource_audit.candidate_ports / 2 <= config.data_ports.size())
            throw ConfigError({ConfigCode::PortBudget, "候选端口的一半不足以容纳专用端点"});
        const auto split = config.data_port_range.find(':');
        const auto first_candidate = std::stoull(config.data_port_range.substr(0, split));
        const auto last_candidate = std::stoull(config.data_port_range.substr(split + 1));
        if (next_endpoint_epoch == UINT64_MAX) throw ConfigError({ConfigCode::OwnerFailed, "数据端点代次耗尽"});
        const auto epoch_value = next_endpoint_epoch++;
        const auto policy = policy_for(route, descriptor.topic);
        unsigned worker = policy.worker_set ? policy.worker
            : policy.exclusive_worker ? policy.worker
            : ordinary_workers[route_hash(route) % ordinary_workers.size()];
        const auto receive_requested = policy.receive_buffer ? policy.receive_buffer : config.data_rcvbuf_bytes;
        const auto send_requested = policy.send_buffer ? policy.send_buffer : config.data_sndbuf_bytes;
        const auto requested_receive = static_cast<std::uint64_t>(receive_requested);
        const auto requested_send = static_cast<std::uint64_t>(send_requested);
        if (requested_receive > UINT64_MAX - requested_send ||
            resource_audit.actual_receive_buffer_bytes > config.socket_buffer_budget_bytes ||
            resource_audit.actual_send_buffer_bytes > config.socket_buffer_budget_bytes - resource_audit.actual_receive_buffer_bytes ||
            requested_receive + requested_send > config.socket_buffer_budget_bytes - resource_audit.actual_receive_buffer_bytes - resource_audit.actual_send_buffer_bytes)
            throw ConfigError({ConfigCode::BufferBudget, "专用端点请求缓冲将超过总预算"});
        std::unique_ptr<DatagramEndpoint> endpoint;
        std::uint16_t port = 0;
        auto candidate = first_candidate;
        while (candidate <= last_candidate) {
            port = allocate_data_port(candidate);
            if (!port) break;
            try {
                endpoint = std::make_unique<DatagramEndpoint>(Ipv4Address::parse(config.listen_ip, port), false,
                    receive_requested, send_requested);
                break;
            } catch (const std::system_error &error) {
                if (error.code().value() != EADDRINUSE) throw;
                candidate = static_cast<std::uint64_t>(port) + 1;
            }
        }
        if (!endpoint) throw ConfigError({ConfigCode::PortBudget, "网络 v2 专用端点端口范围耗尽或均被占用"});
        const auto receive = endpoint->receive_buffer_bytes(), send = endpoint->send_buffer_bytes();
        const auto current_receive = resource_audit.actual_receive_buffer_bytes;
        const auto current_send = resource_audit.actual_send_buffer_bytes;
        const auto added_receive = receive > 0 ? static_cast<std::uint64_t>(receive) : 0;
        const auto added_send = send > 0 ? static_cast<std::uint64_t>(send) : 0;
        const auto current_total = current_receive > UINT64_MAX - current_send ? UINT64_MAX : current_receive + current_send;
        const auto added_total = added_receive > UINT64_MAX - added_send ? UINT64_MAX : added_receive + added_send;
        if (receive <= 0 || send <= 0 || current_total > config.socket_buffer_budget_bytes ||
            added_total > config.socket_buffer_budget_bytes - current_total)
            throw ConfigError({ConfigCode::BufferBudget, "专用端点实际 UDP 缓冲超过总预算"});
        if (!data->add_endpoint(std::move(endpoint), port, worker))
            throw ConfigError({ConfigCode::OwnerFailed, "数据 worker 注册专用端点失败"});
        const auto old_ports = config.data_ports.size();
        const auto old_epochs = config.data_endpoint_epochs.size();
        const auto old_buffers = socket_buffers.size();
        const auto old_buffer_ports = socket_buffer_ports.size();
        bool accounted = false;
        try {
            dedicated_endpoints.emplace(route, DedicatedEndpoint{port, epoch_value, worker, 1, true});
            config.data_ports.push_back(port); config.data_endpoint_epochs.push_back(epoch_value);
            socket_buffers.emplace_back(receive, send); socket_buffer_ports.push_back(port);
            resource_audit.actual_receive_buffer_bytes += static_cast<std::uint64_t>(receive);
            resource_audit.actual_send_buffer_bytes += static_cast<std::uint64_t>(send);
            ++resource_audit.fd_count;
            ++resource_audit.reserved_ports;
            accounted = true;
        } catch (...) {
            if (accounted) {
                resource_audit.actual_receive_buffer_bytes -= static_cast<std::uint64_t>(receive);
                resource_audit.actual_send_buffer_bytes -= static_cast<std::uint64_t>(send);
                if (resource_audit.fd_count) --resource_audit.fd_count;
                if (resource_audit.reserved_ports) --resource_audit.reserved_ports;
            }
            socket_buffer_ports.resize(old_buffer_ports);
            socket_buffers.resize(old_buffers);
            config.data_endpoint_epochs.resize(old_epochs);
            config.data_ports.resize(old_ports);
            dedicated_endpoints.erase(route);
            if (!data->remove_endpoint(port)) throw ConfigError({ConfigCode::OwnerFailed, "专用端点回滚失败"});
            throw;
        }
    }
    void release_dedicated(const RouteKey &route) {
        auto found = dedicated_endpoints.find(route); if (found == dedicated_endpoints.end()) return;
        if (found->second.refs > 1) { --found->second.refs; return; }
        found->second.ready = false; closing_ports.insert(found->second.port);
        if (!data->remove_endpoint(found->second.port)) {
            closing_ports.erase(found->second.port);
            found->second.ready = true;
            throw ConfigError({ConfigCode::OwnerFailed, "数据 worker 注销专用端点失败"});
        }
        closing_ports.erase(found->second.port);
        const auto port = found->second.port;
        const auto data_port = std::find(config.data_ports.begin(), config.data_ports.end(), port);
        if (data_port != config.data_ports.end()) {
            const auto index = static_cast<std::size_t>(data_port - config.data_ports.begin());
            config.data_ports.erase(data_port);
            if (index < config.data_endpoint_epochs.size())
                config.data_endpoint_epochs.erase(config.data_endpoint_epochs.begin() + static_cast<std::ptrdiff_t>(index));
        }
        for (std::size_t i = 0; i < socket_buffer_ports.size(); ++i) if (socket_buffer_ports[i] == port) {
            resource_audit.actual_receive_buffer_bytes -= static_cast<std::uint64_t>(std::max(0, socket_buffers[i].first));
            resource_audit.actual_send_buffer_bytes -= static_cast<std::uint64_t>(std::max(0, socket_buffers[i].second));
            socket_buffer_ports.erase(socket_buffer_ports.begin() + static_cast<std::ptrdiff_t>(i));
            socket_buffers.erase(socket_buffers.begin() + static_cast<std::ptrdiff_t>(i)); break;
        }
        if (resource_audit.fd_count) --resource_audit.fd_count;
        if (resource_audit.reserved_ports) --resource_audit.reserved_ports;
        dedicated_endpoints.erase(found);
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
            { if (size + session.output_bytes > config.limits.session_control_bytes) metrics->reject(NetQuota::session_control_bytes);
              if (size + control_memory > config.limits.local_control_bytes) metrics->reject(NetQuota::local_control_bytes);
              throw std::runtime_error("请求历史达到配额"); }
        session.cache.emplace(id, Cached{request, reply});
        session.cache_bytes += size;
        control_memory += size; metrics->peak(NetQuota::local_control_bytes, control_memory); metrics->peak(NetQuota::session_control_bytes, session.output_bytes + session.cache_bytes);
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
            const bool dedicated = config.network_version == NetworkVersion::V2 && policy_for(descriptor.key, descriptor.topic).dedicated;
            try {
                if (pending_fences.size() >= config.limits.command_records) throw std::runtime_error("撤销屏障队列已满");
                const bool publisher = h.kind == LocalKind::RegisterPub;
                if (!publisher && (pending_subs.size() >= config.limits.command_records || pending_sub_bytes + packet.bytes.size() > config.limits.command_bytes)) throw std::runtime_error("登记等待队列已满");
                const auto route = descriptor.key;
                std::shared_ptr<LocalRegistration> registration;
                if (dedicated) create_dedicated(route, descriptor);
                registration = local_directory->add(session.id, id, descriptor, publisher);
                if (publisher) reply = response(session, LocalKind::PubRegistered, h.request_id, Bytes(id.begin(), id.end()));
                else {
                    const auto binding = registration->binding;
                    if (!binding->bridge && std::none_of(initializing.begin(), initializing.end(), [&](const auto& task) { return task.binding == binding; })) {
                        if (initializing.size() >= config.limits.init_tasks) { metrics->reject(NetQuota::init_tasks); throw std::runtime_error("bridge 初始化队列已满"); }
                        const auto fence = last_data_fence;
                        auto task = std::make_shared<std::packaged_task<std::shared_ptr<ShmWireBridge>()>>([descriptor, fence] {
                            if (fence.valid()) fence.get();
                            return std::make_shared<ShmWireBridge>(descriptor);
                        });
                        auto future = task->get_future().share();
                        if (!outboxes->initialize([task] { (*task)(); })) throw std::runtime_error("初始化队列已满");
                        initializing.push_back({binding, std::move(future)}); metrics->peak(NetQuota::init_tasks, initializing.size());
                    }
                    pending_subs.push_back({session.id, h.request_id, id, packet.bytes}); pending_sub_bytes += packet.bytes.size();
                    session.last_request = h.request_id; remember(session, h.request_id, packet.bytes, {}); return;
                }
                synchronize_data();
                pending_fences.push_back({session.id, h.request_id, packet.bytes, reply, last_data_fence});
                session.last_request = h.request_id; remember(session, h.request_id, packet.bytes, {}); return;
            } catch (const std::exception& e) {
                // add/bridge/synchronize 任一步失败都回滚句柄和专用端点引用，避免端点孤儿化。
                if (dedicated) {
                    const auto *config_error = dynamic_cast<const ConfigError *>(&e);
                    note_endpoint_failure(config_error ? config_error->code() : ConfigCode::BindFailed, e.what());
                }
                const auto registration = local_directory->find(session.id, id);
                if (registration) {
                    try { local_directory->remove(session.id, id, registration->publisher); } catch (...) {}
                }
                if (running.load() && data && data->healthy()) {
                    try { synchronize_data(); } catch (...) {}
                }
                if (config.network_version == NetworkVersion::V2 && policy_for(descriptor.key, descriptor.topic).dedicated) {
                    try { release_dedicated(descriptor.key); } catch (...) {}
                }
                metrics->add(NetMetric::registration_rejected); reply = error(session, h.request_id, 3, e.what());
            }
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
                    const auto registration = local_directory->find(session.id, id);
                    if (!registration) throw std::runtime_error("句柄不存在");
                    const auto route = registration->binding ? registration->binding->descriptor.key : RouteKey{};
                    const auto dedicated = config.network_version == NetworkVersion::V2 && route != RouteKey{} &&
                        policy_for(route, registration->binding->descriptor.topic).dedicated;
                    if (!local_directory->remove(session.id, id, body.data[16] == 1)) throw std::runtime_error("句柄角色不匹配");
                    synchronize_data();
                    if (dedicated) release_dedicated(route);
                    session.route_versions.erase(id); reply = response(session, LocalKind::Unregistered, h.request_id, encoded);
                }
                if (h.kind == LocalKind::SubReady) synchronize_data();
                pending_fences.push_back({session.id, h.request_id, packet.bytes, reply, last_data_fence});
                session.last_request = h.request_id; remember(session, h.request_id, packet.bytes, {}); return;
            } catch (const std::exception& e) {
                if (h.kind == LocalKind::Unregister) {
                    const auto *config_error = dynamic_cast<const ConfigError *>(&e);
                    note_endpoint_failure(config_error ? config_error->code() : ConfigCode::OwnerFailed, e.what());
                }
                metrics->add(NetMetric::registration_rejected); reply = error(session, h.request_id, 3, e.what());
            }
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
                    const auto route = registration && registration->binding ? registration->binding->descriptor.key : RouteKey{};
                    const auto dedicated = registration && route != RouteKey{} && config.network_version == NetworkVersion::V2 &&
                        policy_for(route, registration->binding->descriptor.topic).dedicated;
                    local_directory->remove(pending.session, pending.id, false);
                    if (running.load() && data && data->healthy()) synchronize_data();
                    if (dedicated) release_dedicated(route);
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
                const auto decoded = config.network_version == NetworkVersion::V2
                    ? decode_packet_v2(ByteView(event.packet), h, payload)
                    : decode_packet(ByteView(event.packet), h, payload);
                if (decoded && allow_control(h.target_id, local::monotonic_ns())) {
                    const auto sent = endpoints[config.data_shards]->send(ByteView(event.packet), event.destination);
                    if (sent.status == IoStatus::Data) {
                        if (h.kind == PacketKind::Ack) metrics->add(NetMetric::acks_tx);
                        if (h.kind == PacketKind::Nack) metrics->add(NetMetric::nacks_tx);
                        if (h.kind == PacketKind::Ack && metrics->trace_enabled()) {
                            auto key = trace_key(h); key.gateway_epoch = h.target_epoch;
                            key.session_known = false; key.session_id = 0;
                            metrics->trace(key, MessageTracePoint::AckSent);
                        }
                    }
                }
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
        if (metrics->trace_enabled()) metrics->trace(trace_key(header), MessageTracePoint::ReliableResultQueued);
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
            Bytes packet;
            if (config.network_version == NetworkVersion::V2) {
                DiscoveryHelloV2 hello; hello.gateway_id = identity; hello.gateway_epoch = epoch; hello.snapshot_version = own->version;
                hello.control_port = config.control_port; hello.max_message_bytes = config.limits.message_bytes;
                encode_hello_v2(hello, packet);
            } else {
                DiscoveryHello hello; hello.gateway_id = identity; hello.gateway_epoch = epoch; hello.snapshot_version = own->version;
                hello.data_base_port = config.data_base_port; hello.data_shards = config.data_shards; hello.control_port = config.control_port; hello.max_message_bytes = config.limits.message_bytes;
                encode_hello(hello, packet);
            }
            endpoints.back()->send(ByteView(packet), Ipv4Address::parse(config.discovery_group, config.discovery_port));
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
                    DirectoryCode code;
                    if (config.network_version == NetworkVersion::V2) {
                        DiscoveryHelloV2 h;
                        if (!decode_hello_v2(packet.view(), h)) { ++rejected; continue; }
                        code = peer_directory->hello_v2(h, packet.source, now);
                    } else {
                        DiscoveryHello h;
                        if (!decode_hello(packet.view(), h)) { ++rejected; continue; }
                        code = peer_directory->hello(h, packet.source, now);
                    }
                    if (code != DirectoryCode::Ok && code != DirectoryCode::Ignored) ++rejected;
                } else {
                    if (packet.size >= 4 && !std::memcmp(packet.bytes.data(), "DZMX", 4)) {
                        WireHeader h; ByteView body;
                        const auto decoded = config.network_version == NetworkVersion::V2
                            ? decode_packet_v2(packet.view(), h, body)
                            : decode_packet(packet.view(), h, body);
                        if (!decoded || h.target_id != identity || h.target_epoch != epoch) { ++rejected; continue; }
                        const auto peer = peer_directory->peer(h.source_id);
                        if (!peer.admission || !peer.admission->active.load() || peer.admission->hello.gateway_epoch != h.source_epoch ||
                            peer.admission->ipv4 != packet.source.host || peer.admission->hello.control_port != packet.source.port) { ++rejected; continue; }
                        if (allow_control(h.source_id, now, true)) data->control(packet);
                        continue;
                    }
                    CatalogHeader h; ByteView payload;
                    const auto decoded = config.network_version == NetworkVersion::V2
                        ? decode_catalog_v2(packet.view(), h, payload)
                        : decode_catalog(packet.view(), h, payload);
                    if (!decoded) { ++rejected; continue; }
                    if (h.kind == CatalogKind::Page) { peer_directory->page(packet, now); continue; }
                    auto peer = peer_directory->peer(h.source_id);
                    if (!peer.admission || !peer_directory->reachable(h.source_id, now) || peer.admission->hello.gateway_epoch != h.source_epoch || h.target_id != identity || h.target_epoch != epoch ||
                        packet.source.host != peer.admission->ipv4 || packet.source.port != peer.admission->hello.control_port) { ++rejected; continue; }
                    if (!page_sends.count(h.source_id) && page_sends.size() < config.limits.peers) page_sends.emplace(h.source_id, PageSend{peer, own, 0});
                }
            }
        }
        for (auto& request : peer_directory->tick(now)) {
            if (catalog_requests.size() >= config.limits.peers || catalog_bytes + request.packet.size() > config.limits.network_control_bytes) { metrics->reject(catalog_requests.size() >= config.limits.peers ? NetQuota::peers : NetQuota::network_control_bytes); break; }
            catalog_bytes += request.packet.size(); metrics->peak(NetQuota::network_control_bytes, catalog_bytes); catalog_requests.push_back(std::move(request));
        }
        auto& control = endpoints[config.data_shards];
        for (unsigned n = 0, count = std::min<std::size_t>(32, catalog_requests.size()); n < count; ++n) {
            auto request = std::move(catalog_requests.front()); catalog_requests.pop_front();
            CatalogHeader h; ByteView body;
            if (config.network_version == NetworkVersion::V2) decode_catalog_v2(ByteView(request.packet), h, body);
            else decode_catalog(ByteView(request.packet), h, body);
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
                    event.record.trace(MessageTracePoint::ControlDequeued);
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
                        event.record.trace(MessageTracePoint::ControlAccepted);
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
                        if (sessions.size() >= config.limits.sessions) metrics->reject(NetQuota::sessions);
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
                        sessions.emplace(raw, std::move(session)); metrics->peak(NetQuota::sessions, sessions.size());
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
                    catch (const std::exception &error)
                    {
                        std::cerr << "GatewaySessionFailure: " << error.what() << '\n';
                        ++rejected;
                        close_session(found);
                    }
                    catch (...)
                    {
                        std::cerr << "GatewaySessionFailure: non-standard exception\n";
                        ++rejected;
                        close_session(found);
                    }
                }
            }
        }
        catch (const std::exception &error)
        {
            note_runtime_failure("gateway runtime", &error);
            std::cerr << "GatewayRuntimeFailure: " << error.what() << '\n';
            ++rejected;
        }
        catch (...)
        {
            note_runtime_failure("gateway runtime: non-standard exception");
            std::cerr << "GatewayRuntimeFailure: non-standard exception\n";
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
    if (impl_->config.network_version == NetworkVersion::V2 && impl_->config.data_mode == DataMode::PerTopic)
        impl_->config.data_shards = 0;
    impl_->endpoints = open_gateway_endpoints(impl_->config);
    impl_->config.data_ports.clear();
    impl_->config.data_endpoint_epochs.clear();
    for (std::size_t i = 0; i < impl_->config.data_shards; ++i) {
        impl_->config.data_ports.push_back(impl_->endpoints[i]->local_address().port);
        if (impl_->config.network_version == NetworkVersion::V2) {
            if (impl_->next_endpoint_epoch == UINT64_MAX)
                throw ConfigError({ConfigCode::OwnerFailed, "数据端点代次耗尽"});
            impl_->config.data_endpoint_epochs.push_back(impl_->next_endpoint_epoch++);
        } else {
            impl_->config.data_endpoint_epochs.push_back(0);
        }
    }
    impl_->resource_audit = endpoint_resource_audit(impl_->config, impl_->endpoints.size());
    impl_->wake = local::event();
    impl_->send_budget = std::make_unique<SendBudget>(
        CreditCounters{impl_->config.limits.send_bytes, impl_->config.limits.send_records},
        CreditCounters{impl_->config.limits.session_send_bytes,
                       impl_->config.limits.session_send_records}, impl_->metrics);
    impl_->outboxes = std::make_unique<OutboxService>(
        impl_->wake.get(), impl_->config.limits.init_tasks, impl_->config.limits.command_records);
    impl_->directory_budget = std::make_shared<DirectoryBudget>(impl_->config.limits, impl_->metrics);
    impl_->peer_directory = std::make_unique<PeerDirectory>(impl_->identity, impl_->epoch, impl_->directory_budget, impl_->config.network_version);
    impl_->topic_policy = load_topic_policy(impl_->config);
    for (unsigned worker = 0; worker < impl_->config.data_workers; ++worker)
        if (!impl_->topic_policy.exclusive_workers.count(worker)) impl_->ordinary_workers.push_back(worker);
    if (impl_->ordinary_workers.empty())
        throw ConfigError({ConfigCode::OwnerFailed, "没有可用于普通话题的数据 worker"});
    auto endpoint_resolver = [runtime = impl_.get()](RouteDescriptor &descriptor) {
        if (runtime->config.network_version != NetworkVersion::V2) return;
        const auto dedicated = runtime->dedicated_endpoints.find(descriptor.key);
        if (dedicated != runtime->dedicated_endpoints.end()) {
            descriptor.data_port = dedicated->second.port; descriptor.endpoint_epoch = dedicated->second.epoch; descriptor.endpoint_flags = 1; return;
        }
        if (runtime->pooled_ports.empty()) return;
        const auto index = route_hash(descriptor.key) % runtime->pooled_ports.size();
        descriptor.data_port = runtime->pooled_ports[index];
        descriptor.endpoint_epoch = runtime->pooled_epochs[index];
        descriptor.endpoint_flags = 0;
    };
    impl_->local_directory = std::make_unique<LocalDirectory>(impl_->directory_budget, impl_->config.limits,
                                                               impl_->config.network_version, endpoint_resolver);
    impl_->pooled_ports.assign(impl_->config.data_ports.begin(), impl_->config.data_ports.end());
    impl_->pooled_epochs.assign(impl_->config.data_endpoint_epochs.begin(), impl_->config.data_endpoint_epochs.end());
    impl_->pooled_workers.reserve(impl_->pooled_ports.size());
    for (std::size_t socket = 0; socket < impl_->pooled_ports.size(); ++socket)
        impl_->pooled_workers.push_back(impl_->ordinary_workers[socket % impl_->ordinary_workers.size()]);
    for (const auto& endpoint : impl_->endpoints) {
        const auto receive = endpoint->receive_buffer_bytes();
        const auto send = endpoint->send_buffer_bytes();
        impl_->socket_buffers.emplace_back(receive, send);
        impl_->socket_buffer_ports.push_back(endpoint->local_address().port);
        impl_->resource_audit.actual_receive_buffer_bytes += receive > 0 ? static_cast<std::uint64_t>(receive) : 0;
        impl_->resource_audit.actual_send_buffer_bytes += send > 0 ? static_cast<std::uint64_t>(send) : 0;
    }
    std::vector<std::unique_ptr<DatagramEndpoint>> data_endpoints;
    for (unsigned i = 0; i < impl_->config.data_shards; ++i) data_endpoints.push_back(std::move(impl_->endpoints[i]));
    impl_->data = std::make_unique<GatewayData>(impl_->config, impl_->identity, impl_->epoch, impl_->wake.get(),
                                                std::move(data_endpoints), impl_->pooled_workers, impl_->metrics);
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

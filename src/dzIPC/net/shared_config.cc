#include "dzIPC/net/shared_config.h"
#include "dzIPC/common/shm_mpmc_config.h"

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <set>
#include <utility>
#if defined(__linux__)
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <sys/socket.h>
#endif

namespace dzIPC::net
{
namespace
{
ConfigStatus fail(ConfigCode code, const std::string &detail)
{
    return {code, detail};
}
bool number(const std::string &s, std::uint64_t &n)
{
    if (s.empty())
        return false;
    n = 0;
    for (const unsigned char c : s)
    {
        if (c < '0' || c > '9' || n > (UINT64_MAX - (c - '0')) / 10)
            return false;
        n = n * 10 + c - '0';
    }
    return true;
}
bool ipv4(const std::string &s, std::uint32_t &addr)
{
    addr = 0;
    std::size_t begin = 0;
    for (unsigned i = 0; i != 4; ++i)
    {
        const auto end = s.find('.', begin);
        if ((end == std::string::npos) != (i == 3))
            return false;
        const auto part = s.substr(begin, end == std::string::npos ? end : end - begin);
        std::uint64_t n;
        if (!number(part, n) || n > 255 || (part.size() > 1 && part[0] == '0'))
            return false;
        addr = (addr << 8) | static_cast<std::uint32_t>(n);
        begin = end + 1;
    }
    return true;
}
struct LimitOption
{
    const char *name;
    std::uint64_t Limits::*member;
};
const LimitOption limit_options[] = {
#define LIMIT(name, field)                                                                         \
    {                                                                                              \
        "--" name, &Limits::field                                                                  \
    }
    LIMIT("topics", topics),
    LIMIT("sessions", sessions),
    LIMIT("peers", peers),
    LIMIT("handles", handles),
    LIMIT("session-handles", session_handles),
    LIMIT("max-message-bytes", message_bytes),
    LIMIT("reassembly-bytes", reassembly_bytes),
    LIMIT("peer-reassembly-bytes", peer_reassembly_bytes),
    LIMIT("route-reassembly-bytes", route_reassembly_bytes),
    LIMIT("assemblies", assemblies),
    LIMIT("send-bytes", send_bytes),
    LIMIT("send-records", send_records),
    LIMIT("publisher-reliable", publisher_reliable),
    LIMIT("target-states", target_states),
    LIMIT("commit-pending-bytes", commit_pending_bytes),
    LIMIT("streams", streams),
    LIMIT("stream-window", stream_window),
    LIMIT("receipts", receipts),
    LIMIT("peer-receipts", peer_receipts),
    LIMIT("outbox-bytes", outbox_bytes),
    LIMIT("outbox-records", outbox_records),
    LIMIT("session-send-bytes", session_send_bytes),
    LIMIT("session-send-records", session_send_records),
    LIMIT("initial-send-bytes", initial_send_bytes),
    LIMIT("initial-send-records", initial_send_records),
    LIMIT("result-history", result_history),
    LIMIT("pending-credit-requests", pending_credit_requests),
    LIMIT("init-tasks", init_tasks),
    LIMIT("candidate-bytes", candidate_bytes),
    LIMIT("peer-candidate-bytes", peer_candidate_bytes),
    LIMIT("directory-bytes", directory_bytes),
    LIMIT("old-directory-bytes", old_directory_bytes),
    LIMIT("peer-history", peer_history),
    LIMIT("gateway-history", gateway_history),
    LIMIT("session-control-bytes", session_control_bytes),
    LIMIT("local-control-bytes", local_control_bytes),
    LIMIT("network-control-bytes", network_control_bytes),
    LIMIT("command-records", command_records),
    LIMIT("command-bytes", command_bytes)
#undef LIMIT
};
struct NumberOption
{
    const char *name;
    std::uint64_t GatewayConfig::*member;
};
const NumberOption number_options[] = {
#define OPTION(name, field)                                                                        \
    {                                                                                              \
        "--" name, &GatewayConfig::field                                                           \
    }
    OPTION("data-base-port", data_base_port),     OPTION("data-shards", data_shards),
    OPTION("control-port", control_port),         OPTION("discovery-port", discovery_port),
    OPTION("io-batch-max", io_batch_max),         OPTION("nack-delay-ms", nack_delay_ms),
    OPTION("io-round-packets", io_round_packets), OPTION("io-round-bytes", io_round_bytes),
    OPTION("io-round-us", io_round_us),           OPTION("nack-interval-ms", nack_interval_ms),
    OPTION("retry-initial-ms", retry_initial_ms), OPTION("retry-max-ms", retry_max_ms),
    OPTION("control-rate", control_rate),         OPTION("peer-control-rate", peer_control_rate),
    OPTION("control-burst", control_burst)
#undef OPTION
};
} // namespace

const char *config_code_name(ConfigCode c) noexcept
{
    switch (c)
    {
#define CODE(x)                                                                                    \
    case ConfigCode::x:                                                                            \
        return #x
        CODE(Ok);
        CODE(InvalidBackend);
        CODE(UnsupportedPlatform);
        CODE(BackendNotBuilt);
        CODE(MpmcRequired);
        CODE(InvalidOption);
        CODE(InvalidNumber);
        CODE(InvalidAddress);
        CODE(InvalidInterface);
        CODE(InvalidControlPath);
        CODE(PortConflict);
        CODE(InvalidLimit);
        CODE(NotImplemented);
#undef CODE
    }
    return "Unknown";
}
ConfigError::ConfigError(ConfigStatus s)
    : std::runtime_error(std::string(config_code_name(s.code)) + ": " + s.detail), code_(s.code)
{
}
ConfigStatus parse_backend(const char *value, Backend &out)
{
    if (!value || std::string(value) == "legacy")
        out = Backend::Legacy;
    else if (std::string(value) == "shared_v1")
        out = Backend::SharedV1;
    else
        return fail(ConfigCode::InvalidBackend, "DZIPC_NET_BACKEND 仅接受 legacy 或 shared_v1");
    return {};
}
ConfigStatus backend_availability(Backend mode, bool supported, bool built, bool mpmc)
{
    if (mode == Backend::Legacy)
        return {};
    if (!supported)
        return fail(ConfigCode::UnsupportedPlatform, "共享网络模式要求 64 位 Linux");
    if (!built)
        return fail(ConfigCode::BackendNotBuilt, "构建时未启用 DZIPC_BUILD_SHARED_NET");
    if (!mpmc)
        return fail(ConfigCode::MpmcRequired, "共享网络模式要求 DZIPC_SHM_MPMC=1");
    return {};
}
bool shared_net_supported() noexcept
{
#if defined(__linux__)
    return sizeof(void *) == 8 && sizeof(std::size_t) == 8;
#else
    return false;
#endif
}
bool shared_net_built() noexcept
{
    return DZIPC_SHARED_NET_BUILT != 0;
}
const ProcessConfig &process_config()
{
    static const ProcessConfig result = [] {
        ProcessConfig c;
        c.status = parse_backend(std::getenv("DZIPC_NET_BACKEND"), c.backend);
        if (const auto *path = std::getenv("DZIPC_GATEWAY_CONTROL"))
            c.control_path = path;
        return c;
    }();
    return result;
}
void require_network_backend(bool socket_only)
{
    const auto &c = process_config();
    if (!c.status)
        throw ConfigError(c.status);
    if (socket_only || c.backend == Backend::Legacy)
        return;
    auto s = backend_availability(c.backend, shared_net_supported(), shared_net_built(),
                                  shm_mpmc_enabled());
    if (!s)
        throw ConfigError(s);
    s = validate_control_path(c.control_path);
    if (!s)
        throw ConfigError(s);
}
ConfigStatus validate_control_path(const std::string &s)
{
    // sockaddr_un.sun_path 在目标 Linux 平台为 108B，必须留结尾 NUL。
    if (s.empty() || s[0] != '/' || s.size() > 107 || s.back() == '/' ||
        s.find('\0') != std::string::npos || s.find("/../") != std::string::npos ||
        s.find("/./") != std::string::npos || s.find("//") != std::string::npos ||
        s.substr(s.find_last_of('/') + 1) == "." || s.substr(s.find_last_of('/') + 1) == "..")
        return fail(ConfigCode::InvalidControlPath, "控制路径须为不超过 107 字节的绝对文件路径");
    return {};
}
ConfigStatus validate_config(const GatewayConfig &c)
{
    const auto in = [](std::uint64_t n, std::uint64_t a, std::uint64_t b) {
        return n >= a && n <= b;
    };
    if (!in(c.data_shards, 1, 16) || !in(c.data_base_port, 1, 65535) ||
        c.data_shards - 1 > 65535 - c.data_base_port || !in(c.control_port, 1, 65535) ||
        !in(c.discovery_port, 1, 65535))
        return fail(ConfigCode::InvalidNumber, "数据分片须为 1～16，端口须在 1～65535 内");
    const auto is_data = [&](std::uint64_t p) {
        return p >= c.data_base_port && p - c.data_base_port < c.data_shards;
    };
    if (is_data(c.control_port) || is_data(c.discovery_port) || c.control_port == c.discovery_port)
        return fail(ConfigCode::PortConflict, "数据、控制和发现端口不能重叠");
    std::uint32_t addr;
    if (!ipv4(c.listen_ip, addr) || (addr >> 24) == 0 || (addr >> 24) >= 224)
        return fail(ConfigCode::InvalidAddress, "listen-ip 须为明确的 IPv4 单播地址");
    if (!ipv4(c.discovery_group, addr) || (addr >> 28) != 14)
        return fail(ConfigCode::InvalidAddress, "discovery-group 须为 IPv4 组播地址");
    if (c.interface.empty() || c.interface.size() > 15 ||
        c.interface.find_first_of(" /:\t\r\n") != std::string::npos ||
        c.interface.find('\0') != std::string::npos)
        return fail(ConfigCode::InvalidInterface, "interface 须为 1～15 字节的网卡名称");
    auto status = validate_control_path(c.control_path);
    if (!status)
        return status;
    if (!in(c.io_batch_max, 1, 64) || !in(c.io_round_packets, 1, 4096) ||
        !in(c.io_round_bytes, 1, 16 * kMiB) || !in(c.io_round_us, 1, 1000000) ||
        !in(c.nack_delay_ms, 1, 20) || !in(c.nack_interval_ms, 1, 20) ||
        !in(c.retry_initial_ms, 1, 20) || !in(c.retry_max_ms, c.retry_initial_ms, 5000) ||
        !in(c.control_rate, 1, 1000000) || !in(c.peer_control_rate, 1, c.control_rate) ||
        !in(c.control_burst, 1, 65536))
        return fail(ConfigCode::InvalidLimit, "批量、重传或控制速率超出允许范围");
    const Limits ceiling;
    const auto &l = c.limits;
    for (const auto &opt : limit_options)
    {
        const bool initial = opt.member == &Limits::initial_send_bytes ||
                             opt.member == &Limits::initial_send_records;
        if ((!initial && l.*(opt.member) == 0) || l.*(opt.member) > ceiling.*(opt.member))
            return fail(ConfigCode::InvalidLimit, std::string(opt.name) + " 超出首版硬上限");
    }
    // 在硬上限检查之后进行容量算术，排除加法/取整溢出。
    std::uint64_t loan_capacity = 1024;
    const auto request = l.message_bytes + 112;
    if (request <= 65536)
        loan_capacity = ((request + 1023) / 1024) * 1024;
    else
    {
        loan_capacity = 131072;
        while (loan_capacity < request)
            loan_capacity *= 2;
    }
    const auto wire_capacity = ((l.message_bytes + 63) / 64) * 64;
    if (l.session_handles > l.handles || l.peer_reassembly_bytes > l.reassembly_bytes ||
        l.route_reassembly_bytes > l.peer_reassembly_bytes ||
        l.message_bytes > l.route_reassembly_bytes || l.message_bytes > l.commit_pending_bytes ||
        l.session_send_bytes > l.send_bytes || l.session_send_records > l.send_records ||
        l.session_send_bytes < wire_capacity || l.outbox_bytes < loan_capacity ||
        l.initial_send_bytes > l.session_send_bytes ||
        l.initial_send_records > l.session_send_records || l.peer_receipts > l.receipts ||
        l.peer_candidate_bytes > l.candidate_bytes || l.gateway_history > l.peer_history ||
        l.session_control_bytes > l.local_control_bytes ||
        l.publisher_reliable > l.session_send_records || l.command_records > l.command_bytes / 64)
        return fail(ConfigCode::InvalidLimit, "配额层级或最大消息容量不一致");
    return {};
}
ConfigStatus parse_gateway_options(const std::vector<std::string> &args, GatewayConfig &out)
{
    GatewayConfig candidate = out;
    std::set<std::string> seen;
    for (std::size_t i = 0; i < args.size(); i += 2)
    {
        if (i + 1 == args.size() || !seen.insert(args[i]).second)
            return fail(ConfigCode::InvalidOption, "选项缺少值或重复：" + args[i]);
        const auto &key = args[i];
        const auto &value = args[i + 1];
        if (key == "--listen-ip")
            candidate.listen_ip = value;
        else if (key == "--interface")
            candidate.interface = value;
        else if (key == "--control")
            candidate.control_path = value;
        else if (key == "--discovery-group")
            candidate.discovery_group = value;
        else
        {
            std::uint64_t *dest = nullptr;
            for (const auto &opt : number_options)
                if (key == opt.name)
                    dest = &(candidate.*(opt.member));
            for (const auto &opt : limit_options)
                if (key == opt.name)
                    dest = &(candidate.limits.*(opt.member));
            if (!dest)
                return fail(ConfigCode::InvalidOption, "未知选项：" + key);
            if (!number(value, *dest))
                return fail(ConfigCode::InvalidNumber, "需要无符号十进制整数：" + key);
        }
    }
    auto status = validate_config(candidate);
    if (status)
        out = std::move(candidate);
    return status;
}
ConfigStatus validate_host_interface(const GatewayConfig &c)
{
    auto status = validate_config(c);
    if (!status)
        return status;
#if defined(__linux__)
    if (!if_nametoindex(c.interface.c_str()))
        return fail(ConfigCode::InvalidInterface, "指定网卡不存在");
    ifaddrs *list = nullptr;
    if (getifaddrs(&list))
        return fail(ConfigCode::InvalidInterface, "无法查询本机网卡地址");
    in_addr expected{};
    inet_pton(AF_INET, c.listen_ip.c_str(), &expected);
    bool found = false;
    for (auto *p = list; p; p = p->ifa_next)
    {
        if (p->ifa_addr && p->ifa_addr->sa_family == AF_INET && c.interface == p->ifa_name &&
            (p->ifa_flags & IFF_UP) &&
            reinterpret_cast<sockaddr_in *>(p->ifa_addr)->sin_addr.s_addr == expected.s_addr)
            found = true;
    }
    freeifaddrs(list);
    if (!found)
        return fail(ConfigCode::InvalidInterface, "listen-ip 不属于指定的已启用网卡");
    return {};
#else
    return fail(ConfigCode::UnsupportedPlatform, "共享网络模式要求 64 位 Linux");
#endif
}
} // namespace dzIPC::net

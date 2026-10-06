#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "dzIPC/net/datagram_endpoint.h"
#include "dzIPC/threepools/socket_wait_set.h"
#include <algorithm>
#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <climits>
#include <deque>
#include <fcntl.h>
#include <limits>
#include <map>
#include <net/if.h>
#include <filesystem>
#include <sys/resource.h>
#include <set>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>

namespace dzIPC::net
{
namespace
{
sockaddr_in native(Ipv4Address a)
{
    sockaddr_in s{};
    s.sin_family = AF_INET;
    s.sin_addr.s_addr = htonl(a.host);
    s.sin_port = htons(a.port);
    return s;
}
Ipv4Address portable(const sockaddr_in &a)
{
    return {ntohl(a.sin_addr.s_addr), ntohs(a.sin_port)};
}
IoResult syscall_error() noexcept
{
    return {errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR ? IoStatus::WouldBlock
                                                                      : IoStatus::Fatal,
            0, errno};
}
[[noreturn]] void system_failure(const std::string &action)
{
    throw std::system_error(errno, std::generic_category(), action);
}
int socket_buffer(int fd, int kind) noexcept
{
    int value = 0;
    socklen_t size = sizeof(value);
    return getsockopt(fd, SOL_SOCKET, kind, &value, &size) ? -1 : value;
}
} // namespace
Ipv4Address Ipv4Address::parse(const std::string &ip, std::uint16_t port)
{
    in_addr addr{};
    if (ip.find('\0') != std::string::npos || inet_pton(AF_INET, ip.c_str(), &addr) != 1)
        throw std::invalid_argument("无效 IPv4 地址：" + ip);
    return {ntohl(addr.s_addr), port};
}
std::string Ipv4Address::ip() const
{
    char text[INET_ADDRSTRLEN]{};
    in_addr address{htonl(host)};
    if (!inet_ntop(AF_INET, &address, text, sizeof(text)))
        system_failure("转换 IPv4 地址失败");
    return text;
}
DatagramEndpoint::DatagramEndpoint(Ipv4Address address, bool discovery_reuse,
                                   std::uint64_t receive_buffer_bytes,
                                   std::uint64_t send_buffer_bytes)
{
    fd_ = ::socket(AF_INET, SOCK_DGRAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);
    if (fd_ < 0)
        system_failure("创建 UDP 端点失败");
    try
    {
        // 兼容既有 SocketWaitSet 的非零 handle 契约，包括 stdin 已关闭的进程。
        if (fd_ < 3)
        {
            const int replacement = fcntl(fd_, F_DUPFD_CLOEXEC, 3);
            if (replacement < 0)
                system_failure("复制 UDP 端点失败");
            ::close(fd_);
            fd_ = replacement;
        }
        if (discovery_reuse)
        {
            int one = 1;
            if (setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one)))
                system_failure("设置发现 socket 复用失败");
        }
        if (receive_buffer_bytes)
        {
            if (receive_buffer_bytes > static_cast<std::uint64_t>(INT_MAX))
                throw std::invalid_argument("接收缓冲超出系统接口范围");
            const int value = static_cast<int>(receive_buffer_bytes);
            if (setsockopt(fd_, SOL_SOCKET, SO_RCVBUF, &value, sizeof(value)))
                system_failure("设置 UDP 接收缓冲失败");
        }
        if (send_buffer_bytes)
        {
            if (send_buffer_bytes > static_cast<std::uint64_t>(INT_MAX))
                throw std::invalid_argument("发送缓冲超出系统接口范围");
            const int value = static_cast<int>(send_buffer_bytes);
            if (setsockopt(fd_, SOL_SOCKET, SO_SNDBUF, &value, sizeof(value)))
                system_failure("设置 UDP 发送缓冲失败");
        }
        auto addr = native(address);
        if (::bind(fd_, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)))
            system_failure("绑定 " + address.ip() + ':' + std::to_string(address.port) + " 失败");
        socklen_t size = sizeof(addr);
        if (getsockname(fd_, reinterpret_cast<sockaddr *>(&addr), &size))
            system_failure("读取 UDP 地址失败");
        local_ = portable(addr);
    }
    catch (...)
    {
        ::close(fd_);
        fd_ = -1;
        throw;
    }
}
DatagramEndpoint::~DatagramEndpoint()
{
    if (fd_ >= 0)
        ::close(fd_);
}
int DatagramEndpoint::receive_buffer_bytes() const noexcept
{
    return socket_buffer(fd_, SO_RCVBUF);
}
int DatagramEndpoint::send_buffer_bytes() const noexcept
{
    return socket_buffer(fd_, SO_SNDBUF);
}
void DatagramEndpoint::join_discovery(const std::string &group, const std::string &interface,
                                      const std::string &ip)
{
    ip_mreqn membership{};
    membership.imr_multiaddr.s_addr = htonl(Ipv4Address::parse(group, 0).host);
    membership.imr_address.s_addr = htonl(Ipv4Address::parse(ip, 0).host);
    membership.imr_ifindex = if_nametoindex(interface.c_str());
    if (!membership.imr_ifindex)
        throw std::invalid_argument("发现网卡不存在：" + interface);
    if (setsockopt(fd_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &membership, sizeof(membership)))
        system_failure("加入发现组播失败");
    if (setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_IF, &membership, sizeof(membership)))
        system_failure("设置发现组播网卡失败");
    unsigned char ttl = 1;
    if (setsockopt(fd_, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl)))
        system_failure("设置发现 TTL 失败");
}
IoResult DatagramEndpoint::send(ByteView b, Ipv4Address destination) noexcept
{
    if ((!b.data && b.size) || b.size > 1184 || !destination.port)
        return {IoStatus::Fatal, 0, EINVAL};
    auto addr = native(destination);
    const auto sent = sendto(fd_, b.data, b.size, MSG_DONTWAIT | MSG_NOSIGNAL,
                             reinterpret_cast<sockaddr *>(&addr), sizeof(addr));
    if (sent < 0)
        return syscall_error();
    return {IoStatus::Data, 1, 0};
}
IoResult DatagramEndpoint::receive(ReceivedDatagram &out) noexcept
{
    return receive_batch(&out, 1);
}
IoResult DatagramEndpoint::send_batch(const OutgoingDatagram *items, std::size_t count) noexcept
{
    if (!items || !count || count > 64)
        return {IoStatus::Fatal, 0, EINVAL};
    std::array<mmsghdr, 64> messages{};
    std::array<iovec, 64> iov{};
    std::array<sockaddr_in, 64> addresses{};
    for (std::size_t i = 0; i < count; ++i)
    {
        const auto &item = items[i];
        if ((!item.payload.data && item.payload.size) || item.payload.size > 1184 ||
            !item.destination.port)
            return {IoStatus::Fatal, 0, EINVAL};
        addresses[i] = native(item.destination);
        iov[i] = {const_cast<std::uint8_t *>(item.payload.data), item.payload.size};
        auto &h = messages[i].msg_hdr;
        h.msg_name = &addresses[i];
        h.msg_namelen = sizeof(sockaddr_in);
        h.msg_iov = &iov[i];
        h.msg_iovlen = 1;
    }
    const auto n = sendmmsg(fd_, messages.data(), count, MSG_DONTWAIT | MSG_NOSIGNAL);
    if (n < 0)
        return syscall_error();
    return {IoStatus::Data, static_cast<std::size_t>(n), 0};
}
IoResult DatagramEndpoint::receive_batch(ReceivedDatagram *items, std::size_t count) noexcept
{
    if (!items || !count || count > 64)
        return {IoStatus::Fatal, 0, EINVAL};
    std::array<mmsghdr, 64> messages{};
    std::array<iovec, 64> iov{};
    std::array<sockaddr_in, 64> sources{};
    for (std::size_t i = 0; i < count; ++i)
    {
        items[i].size = 0;
        items[i].status = IoStatus::WouldBlock;
        items[i].source = {};
        iov[i] = {items[i].bytes.data(), items[i].bytes.size()};
        auto &h = messages[i].msg_hdr;
        h.msg_name = &sources[i];
        h.msg_namelen = sizeof(sockaddr_in);
        h.msg_iov = &iov[i];
        h.msg_iovlen = 1;
    }
    const auto n = recvmmsg(fd_, messages.data(), count, MSG_DONTWAIT | MSG_TRUNC, nullptr);
    if (n < 0)
        return syscall_error();
    for (int i = 0; i < n; ++i)
    {
        auto &out = items[i];
        out.source = portable(sources[i]);
        const bool truncated = (messages[i].msg_hdr.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
                               messages[i].msg_len > out.bytes.size();
        out.status = truncated ? IoStatus::Truncated : IoStatus::Data;
        out.size = truncated ? 0 : messages[i].msg_len; // 截断数据不交给协议解析器
    }
    return {n == 1 ? items[0].status : IoStatus::Data, static_cast<std::size_t>(n), 0};
}
std::vector<std::unique_ptr<DatagramEndpoint>> open_gateway_endpoints(const GatewayConfig &c)
{
    auto status = validate_host_interface(c);
    if (!status)
        throw ConfigError(status);
    const auto audit = endpoint_resource_audit(c, c.data_shards + 2);
    if (c.data_shards > c.data_socket_cap)
        throw ConfigError({ConfigCode::SocketCap, "数据 socket 数超过 data-socket-cap"});
    if (audit.fd_budget_limit <= audit.fd_count ||
        audit.fd_budget_limit - audit.fd_count <= audit.fd_reserve ||
        c.data_shards + 2 > audit.fd_budget_limit - audit.fd_count - audit.fd_reserve)
        throw ConfigError({ConfigCode::FdBudget, "数据端点创建将超过 FD 半数预算"});
    if (c.data_shards > audit.candidate_ports / 2)
        throw ConfigError({ConfigCode::PortBudget, "候选端口的一半不足以容纳数据 socket"});
    std::vector<std::unique_ptr<DatagramEndpoint>> endpoints;
    endpoints.reserve(c.data_shards + 2);
    for (std::uint64_t shard = 0; shard < c.data_shards; ++shard)
        endpoints.push_back(std::make_unique<DatagramEndpoint>(
            Ipv4Address::parse(c.listen_ip, c.data_base_port + shard), false,
            c.data_rcvbuf_bytes, c.data_sndbuf_bytes));
    endpoints.push_back(
        std::make_unique<DatagramEndpoint>(Ipv4Address::parse(c.listen_ip, c.control_port), false,
                                           c.data_rcvbuf_bytes, c.data_sndbuf_bytes));
    auto discovery =
        std::make_unique<DatagramEndpoint>(Ipv4Address::parse("0.0.0.0", c.discovery_port), true,
                                           c.data_rcvbuf_bytes, c.data_sndbuf_bytes);
    discovery->join_discovery(c.discovery_group, c.interface, c.listen_ip);
    endpoints.push_back(std::move(discovery));
    std::uint64_t receive = 0, send = 0;
    for (const auto &endpoint : endpoints)
    {
        const auto rcv = endpoint->receive_buffer_bytes();
        const auto snd = endpoint->send_buffer_bytes();
        if (rcv <= 0 || snd <= 0 || receive > UINT64_MAX - static_cast<std::uint64_t>(rcv) ||
            send > UINT64_MAX - static_cast<std::uint64_t>(snd))
            throw ConfigError({ConfigCode::BufferBudget, "无法读取 UDP socket 实际缓冲"});
        receive += static_cast<std::uint64_t>(rcv);
        send += static_cast<std::uint64_t>(snd);
        if (receive > c.socket_buffer_budget_bytes || send > c.socket_buffer_budget_bytes ||
            receive > c.socket_buffer_budget_bytes - send)
            throw ConfigError({ConfigCode::BufferBudget, "UDP socket 实际缓冲超过总预算"});
    }
    return endpoints;
}
EndpointResourceAudit endpoint_resource_audit(const GatewayConfig &c, std::uint64_t endpoint_count)
{
    EndpointResourceAudit result;
    result.buffer_budget_bytes = c.socket_buffer_budget_bytes;
#if defined(__linux__)
    rlimit limit{};
    if (getrlimit(RLIMIT_NOFILE, &limit) == 0)
    {
        result.fd_soft_limit = limit.rlim_cur == RLIM_INFINITY ? UINT64_MAX : limit.rlim_cur;
        result.fd_hard_limit = limit.rlim_max == RLIM_INFINITY ? UINT64_MAX : limit.rlim_max;
        const auto scaled = static_cast<long double>(result.fd_soft_limit) * c.socket_fd_fraction;
        result.fd_budget_limit = scaled >= static_cast<long double>(UINT64_MAX)
                                     ? UINT64_MAX
                                     : static_cast<std::uint64_t>(scaled);
    }
    std::error_code error;
    result.fd_count = std::distance(std::filesystem::directory_iterator("/proc/self/fd", error),
                                    std::filesystem::directory_iterator{});
#endif
    std::uint64_t first = 0, last = 0;
    const auto split = c.data_port_range.find(':');
    if (split != std::string::npos)
    {
        try
        {
            first = std::stoull(c.data_port_range.substr(0, split));
            last = std::stoull(c.data_port_range.substr(split + 1));
        }
        catch (...) { first = last = 0; }
    }
    if (first && last >= first)
    {
        result.candidate_ports = last - first + 1;
        if (c.control_port >= first && c.control_port <= last) --result.candidate_ports;
        if (c.discovery_port >= first && c.discovery_port <= last && result.candidate_ports) --result.candidate_ports;
    }
    result.reserved_ports = endpoint_count;
    return result;
}
std::uint16_t data_port(const RouteKey &key, std::uint16_t base, std::uint16_t shards)
{
    if (!valid_scope(key.scope) || !base || !shards || shards > 16 ||
        unsigned(base) + shards - 1 > 65535)
        throw std::invalid_argument("数据分片参数无效");
    return base + route_hash(key) % shards;
}
bool matches_data_shard(const RouteKey &key, Ipv4Address observed, std::uint16_t base,
                        std::uint16_t shards) noexcept
{
    return valid_scope(key.scope) && base && shards && shards <= 16 &&
           unsigned(base) + shards - 1 <= 65535 && observed.port == base + route_hash(key) % shards;
}
struct DatagramLoop::Impl
{
    struct Entry
    {
        std::shared_ptr<DatagramEndpoint> socket;
        threepools::SocketWaitToken wait;
        // recvmmsg 已从内核取走的后缀，处理预算到期后留给下一轮，最多 batch_max 项。
        std::shared_ptr<std::deque<ReceivedDatagram>> pending =
            std::make_shared<std::deque<ReceivedDatagram>>();
    };
    threepools::SocketWaitSet wait_set;
    std::map<Token, Entry> entries;
    std::deque<Token> deferred;
    std::set<Token> queued;
    std::atomic<bool> stopped{false};
    Token next = 1;
    IoBudget budget;
    void defer(Token token)
    {
        if (entries.count(token) && queued.insert(token).second)
            deferred.push_back(token);
    }
};
DatagramLoop::DatagramLoop(IoBudget budget) : impl_(new Impl)
{
    if (!budget.packets || budget.packets > 4096 || !budget.bytes ||
        budget.bytes > 16 * 1024 * 1024 || !budget.batch_max || budget.batch_max > 64 ||
        budget.time.count() <= 0 || budget.time.count() > 1000000)
        throw std::invalid_argument("IO 轮次预算超出范围");
    if (!threepools::SocketWaitSet::backend_available())
        throw std::runtime_error("epoll 后端不可用");
    impl_->budget = budget;
}
DatagramLoop::~DatagramLoop()
{
    request_stop();
    for (const auto &entry : impl_->entries)
        impl_->wait_set.remove(entry.second.wait);
    impl_->entries.clear();
}
DatagramLoop::Token DatagramLoop::add(std::unique_ptr<DatagramEndpoint> endpoint)
{
    if (!endpoint || stopped() || impl_->next == UINT64_MAX || impl_->entries.size() >= 18)
        throw std::runtime_error("IO 注册不可用或达到 K+2 硬上限");
    const auto token = impl_->next++;
    Impl::Entry entry{std::move(endpoint), {}};
    entry.wait = {entry.socket.get(), static_cast<std::uintptr_t>(entry.socket->native_handle())};
    const auto inserted = impl_->entries.emplace(token, std::move(entry));
    if (!impl_->wait_set.add(inserted.first->second.wait))
    {
        impl_->entries.erase(inserted.first);
        throw std::runtime_error("注册 UDP 等待失败");
    }
    return token;
}
bool DatagramLoop::remove(Token token)
{
    const auto found = impl_->entries.find(token);
    if (found == impl_->entries.end())
        return true;
    if (!impl_->wait_set.remove(found->second.wait))
        return false;
    impl_->entries.erase(found);
    impl_->queued.erase(token);
    impl_->deferred.erase(std::remove(impl_->deferred.begin(), impl_->deferred.end(), token),
                          impl_->deferred.end());
    return true;
}
std::size_t DatagramLoop::size() const noexcept
{
    return impl_->entries.size();
}
void DatagramLoop::request_stop() noexcept
{
    impl_->stopped.store(true, std::memory_order_release);
    impl_->wait_set.stop();
}
bool DatagramLoop::stopped() const noexcept
{
    return impl_->stopped.load(std::memory_order_acquire);
}
IoRound DatagramLoop::run_once(std::chrono::milliseconds timeout, const Handler &handler)
{
    IoRound round;
    if (stopped())
        return round;
    impl_->wait_set.wait(impl_->deferred.empty() ? timeout : std::chrono::milliseconds(0));
    for (const auto &ready : impl_->wait_set.consume_ready())
        for (const auto &entry : impl_->entries)
            if (entry.second.wait == ready)
            {
                impl_->defer(entry.first);
                break;
            }
    const auto turns = impl_->deferred.size();
    std::array<ReceivedDatagram, 64> batch;
    for (std::size_t turn = 0; turn < turns && !impl_->deferred.empty() && !stopped(); ++turn)
    {
        const auto token = impl_->deferred.front();
        impl_->deferred.pop_front();
        impl_->queued.erase(token);
        const auto found = impl_->entries.find(token);
        if (found == impl_->entries.end())
            continue;
        auto socket = found->second.socket;
        auto pending = found->second.pending;
        std::size_t packets = 0, bytes = 0;
        const auto deadline = std::chrono::steady_clock::now() + impl_->budget.time;
        while (!stopped() && impl_->entries.count(token))
        {
            // 最坏报文长度预留本轮余量；至少处理一包以保证极小预算仍能前进。
            const auto remaining_bytes =
                bytes < impl_->budget.bytes ? impl_->budget.bytes - bytes : 0;
            const auto count = std::min({impl_->budget.batch_max, impl_->budget.packets - packets,
                                         std::max<std::size_t>(1, remaining_bytes / 1184)});
            if (pending->empty())
            {
                const auto result = socket->receive_batch(batch.data(), count);
                if (result.status == IoStatus::WouldBlock)
                    break;
                if (result.status == IoStatus::Fatal)
                    throw std::system_error(result.error, std::generic_category(), "UDP 接收失败");
                for (std::size_t i = 0; i < result.count; ++i)
                    pending->push_back(std::move(batch[i]));
            }
            if (pending->empty())
                break;
            auto packet = std::move(pending->front());
            pending->pop_front();
            ++packets;
            bytes += packet.size;
            ++round.packets;
            try
            {
                handler(token, packet);
            }
            catch (...)
            {
                impl_->defer(token);
                throw;
            }
            if (packets >= impl_->budget.packets || bytes >= impl_->budget.bytes ||
                std::chrono::steady_clock::now() >= deadline)
            {
                impl_->defer(token);
                break;
            }
        }
        round.bytes += bytes;
    }
    round.deferred = !impl_->deferred.empty();
    return round;
}
} // namespace dzIPC::net

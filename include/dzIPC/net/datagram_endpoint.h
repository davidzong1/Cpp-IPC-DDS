#pragma once
#include "dzIPC/net/shared_config.h"
#include "dzIPC/net/wire_protocol.h"
#include <array>
#include <chrono>
#include <functional>
#include <memory>
#include <vector>

namespace dzIPC::net
{
struct Ipv4Address
{
    std::uint32_t host = 0;
    std::uint16_t port = 0;
    static Ipv4Address parse(const std::string &ip, std::uint16_t port);
    std::string ip() const;
    bool operator==(const Ipv4Address &b) const noexcept
    {
        return host == b.host && port == b.port;
    }
};
enum class IoStatus
{
    Data,
    WouldBlock,
    Truncated,
    Fatal
};
struct IoResult
{
    IoStatus status = IoStatus::WouldBlock;
    std::size_t count = 0;
    int error = 0;
};
struct ReceivedDatagram
{
    std::array<std::uint8_t, 1184> bytes{};
    std::size_t size = 0;
    Ipv4Address source;
    IoStatus status = IoStatus::WouldBlock;
    ByteView view() const noexcept
    {
        return {bytes.data(), size};
    }
};
struct OutgoingDatagram
{
    Ipv4Address destination;
    ByteView payload;
};

// 单一 IO owner；注册到 DatagramLoop 后由 loop 先摘除、再关闭。
class DatagramEndpoint
{
  public:
    explicit DatagramEndpoint(Ipv4Address bind_address, bool discovery_reuse = false);
    ~DatagramEndpoint();
    DatagramEndpoint(const DatagramEndpoint &) = delete;
    DatagramEndpoint &operator=(const DatagramEndpoint &) = delete;
    int native_handle() const noexcept
    {
        return fd_;
    }
    Ipv4Address local_address() const noexcept
    {
        return local_;
    }
    int receive_buffer_bytes() const noexcept;
    int send_buffer_bytes() const noexcept;
    void join_discovery(const std::string &group, const std::string &interface,
                        const std::string &local_ip);
    IoResult send(ByteView bytes, Ipv4Address destination) noexcept;
    IoResult receive(ReceivedDatagram &out) noexcept;
    // 返回已处理的前缀长度；部分发送后仅重试剩余后缀，不重发前缀。
    IoResult send_batch(const OutgoingDatagram *items, std::size_t count) noexcept;
    IoResult receive_batch(ReceivedDatagram *items, std::size_t count) noexcept;

  private:
    int fd_ = -1;
    Ipv4Address local_;
};

std::vector<std::unique_ptr<DatagramEndpoint>> open_gateway_endpoints(const GatewayConfig &config);
std::uint16_t data_port(const RouteKey &, std::uint16_t base, std::uint16_t shards);
bool matches_data_shard(const RouteKey &, Ipv4Address observed, std::uint16_t base,
                        std::uint16_t shards) noexcept;

struct IoBudget
{
    std::size_t packets = 64, bytes = 64 * 1024, batch_max = 32;
    std::chrono::microseconds time{200};
};
struct IoRound
{
    std::size_t packets = 0, bytes = 0;
    bool deferred = false;
};
// 仅 request_stop 可由其他线程调用；析构前须 stop 并 join 调用 run_once 的线程。
// 注册令牌单调递增，永不将旧事件分配给复用同一 FD 的新对象。
class DatagramLoop
{
  public:
    using Token = std::uint64_t;
    using Handler = std::function<void(Token, const ReceivedDatagram &)>;
    explicit DatagramLoop(IoBudget budget = {});
    ~DatagramLoop();
    DatagramLoop(const DatagramLoop &) = delete;
    DatagramLoop &operator=(const DatagramLoop &) = delete;
    Token add(std::unique_ptr<DatagramEndpoint> endpoint);
    bool remove(Token token);
    std::size_t size() const noexcept;
    IoRound run_once(std::chrono::milliseconds timeout, const Handler &handler);
    void request_stop() noexcept;
    bool stopped() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};
} // namespace dzIPC::net

#pragma once
#include "dzIPC/net/peer_directory.h"
#include "dzIPC/net/local_protocol.h"
#include <optional>

namespace dzIPC::net {
struct SendFragment { std::size_t target = 0; std::uint32_t fragment = 0; bool retry = false; };
struct ReliableTarget { PeerView peer; std::shared_ptr<RouteAdmission> route; };
struct ReliableStats { std::uint64_t original_packets = 0, retry_packets = 0, ignored_controls = 0, nacks = 0, acks = 0; };
// 单 shard 纯发送状态机。批次在 accepted() 确认前不改变身份；ACK 不直接释放外部 WireBlob。
class ReliableSession {
public:
    ReliableSession(WireHeader base, std::vector<ReliableTarget>, std::uint64_t deadline_ns,
                    std::uint64_t initial_retry_ns = 2000000, std::uint64_t max_retry_ns = 100000000,
                    std::uint64_t nack_interval_ns = 2000000);
    ~ReliableSession();
    const std::vector<SendFragment>& batch(std::uint64_t now_ns, std::size_t max_packets, std::size_t max_retries = static_cast<std::size_t>(-1));
    void accepted(std::size_t prefix, std::uint64_t now_ns);
    void control(const ReceivedDatagram&, std::uint64_t now_ns);
    void tick(std::uint64_t now_ns);
    bool has_initial_pending() const;
    void cancel(SendResultCode);
    WireHeader header(const SendFragment&) const;
    Ipv4Address destination(const SendFragment&) const;
    std::optional<SendResultBody> result() const;
    ReliableStats stats() const;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
} // namespace dzIPC::net

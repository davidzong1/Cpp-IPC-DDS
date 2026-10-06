#pragma once
#include "dzIPC/net/local_directory.h"
#include "dzIPC/net/outbox.h"
#include "dzIPC/net/reliable_session.h"
#include <future>

namespace dzIPC::net {
struct GatewayDataView {
    std::shared_ptr<const DirectorySnapshot> local;
    using Bridges = std::map<RouteKey, std::shared_ptr<ShmWireBridge>>;
    std::shared_ptr<const Bridges> bridges;
    std::map<Identity, PeerView> peers;
};
struct GatewayDataEvent {
    enum class Kind { Result, Feedback } kind = Kind::Result;
    OutboxHeader header;
    SendResultBody result;
    Ipv4Address destination;
    Bytes packet;
};
struct GatewayDataStats {
    std::uint64_t sent_messages = 0, sent_packets = 0, committed_messages = 0,
                  rejected_records = 0, dropped_feedback = 0, target_states = 0;
    ReassemblyUsage receive_usage;
    ReassemblyStats receive_stats;
    std::uint64_t rx_packets = 0, rx_bytes = 0, tx_bytes = 0, send_eagain = 0, send_error = 0, truncated = 0, invalid_packets = 0, unverified_route = 0;
    std::uint64_t queued_commands = 0, queued_bytes = 0;
    std::uint64_t retry_packets = 0, nacks = 0, ignored_controls = 0;
};
// 固定 K 个 shard，各自独占数据 socket、重组、发送队列和 SHM commit。
class GatewayData {
public:
    GatewayData(const GatewayConfig&, Identity, std::uint64_t epoch, int control_wake,
                std::vector<std::unique_ptr<DatagramEndpoint>> endpoints, std::shared_ptr<NetMetrics> metrics = {});
    ~GatewayData();
    std::shared_future<void> synchronize(std::shared_ptr<const GatewayDataView>);
    // 动态端点由网关控制线程创建/撤销；返回时 worker 已完成 owner 注册或摘除。
    bool add_endpoint(std::unique_ptr<DatagramEndpoint>, std::uint16_t port, unsigned worker);
    bool remove_endpoint(std::uint16_t port);
    bool submit(OutboxRecord&, std::vector<PeerView> targets, std::shared_ptr<LocalRegistration> source = {});
    bool control(const ReceivedDatagram&);
    bool pop(GatewayDataEvent&);
    bool healthy() const;
    GatewayDataStats stats() const;
    std::string shard_metrics() const;
    void stop();
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
} // namespace dzIPC::net

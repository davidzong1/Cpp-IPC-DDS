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
    std::uint64_t retry_packets = 0, nacks = 0, ignored_controls = 0;
};
// 固定 K 个 shard，各自独占数据 socket、重组、发送队列和 SHM commit。
class GatewayData {
public:
    GatewayData(const GatewayConfig&, Identity, std::uint64_t epoch, int control_wake,
                std::vector<std::unique_ptr<DatagramEndpoint>> endpoints);
    ~GatewayData();
    std::shared_future<void> synchronize(std::shared_ptr<const GatewayDataView>);
    bool submit(OutboxRecord&, std::vector<PeerView> targets);
    bool control(const ReceivedDatagram&);
    bool pop(GatewayDataEvent&);
    bool healthy() const;
    GatewayDataStats stats() const;
    void stop();
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
} // namespace dzIPC::net

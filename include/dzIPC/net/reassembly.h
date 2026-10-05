#pragma once
#include "dzIPC/net/datagram_endpoint.h"
#include "dzIPC/net/client_runtime.h"
#include "dzIPC/net/wire_blob.h"
#include <atomic>
#include <functional>

namespace dzIPC::net {
// 由目录层签发；租约暂停只改变 active，不能清除同 epoch 的去重历史。
struct PeerAdmission {
    DiscoveryHello hello;
    std::uint32_t ipv4 = 0;
    std::atomic<bool> active{true};
};
struct RouteAdmission {
    RouteDescriptor descriptor;
    std::atomic<bool> active{true};
};
struct ReceiveAdmission {
    std::shared_ptr<PeerAdmission> peer;
    std::shared_ptr<RouteAdmission> publisher, subscriber;
};
enum class RejectReason : std::uint32_t { UnknownRoute = 1, RouteEpochMismatch, QuotaExceeded, BadMetadata, ShmUnavailable, ShmCommitIndeterminate, UnsupportedEncoding };
enum class ReceiveDisposition { Dropped, Accepted, CommitPending, Duplicate, Rejected, Committed };
struct ReceiveFeedback {
    ReceiveDisposition disposition = ReceiveDisposition::Dropped;
    WireHeader original;
    Bytes control_packet; // 已反转方向；只发送到经目录确认的控制地址。
};
struct ReassemblyUsage {
    std::uint64_t bytes = 0, bitmap_bytes = 0, pending_bytes = 0, assemblies = 0, streams = 0, stream_bytes = 0, receipts = 0;
};
class ReassemblyBudget {
public:
    explicit ReassemblyBudget(Limits limits = {});
    ~ReassemblyBudget();
    ReassemblyUsage usage() const;
private:
    friend class ReassemblyShard;
    struct Impl; std::shared_ptr<Impl> impl_;
};
struct ReassemblyStats { std::uint64_t malformed = 0, wrong_shard = 0, rejected = 0, duplicates = 0, completed = 0, committed = 0, commit_attempts = 0; };
class ReassemblyShard {
public:
    using Committer = std::function<SubmitState(const WireHeader&, const WireBlob&)>;
    ReassemblyShard(Identity local_id, std::uint64_t epoch, unsigned shard, unsigned shards,
                    std::shared_ptr<ReassemblyBudget>, std::uint64_t nack_delay_ns = 2000000,
                    std::uint64_t nack_interval_ns = 2000000);
    ~ReassemblyShard();
    // 所有方法由所属 shard 单线程调用；committer 不可重入本对象。
    ReceiveFeedback ingest(const ReceivedDatagram&, const ReceiveAdmission&, std::uint64_t now_ns);
    std::vector<ReceiveFeedback> tick(std::uint64_t now_ns, const Committer&, std::size_t budget = 64);
    // 仅永久失效时调用；普通 peer 租约过期不能调用 retire_peer_epoch。
    void retire_route(const std::shared_ptr<RouteAdmission>&);
    void retire_peer_epoch(const std::shared_ptr<PeerAdmission>&);
    ReassemblyStats stats() const;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
} // namespace dzIPC::net

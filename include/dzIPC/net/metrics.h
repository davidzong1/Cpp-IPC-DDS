#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <sstream>
#include <string>

#include "dzIPC/net/wire_protocol.h"

namespace dzIPC::net {
inline std::uint64_t metric_now_ns() noexcept {
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
// 固定64个log2桶；分位数明确为桶上界，原始性能CSV另保留精确样本。
class LatencyHistogram {
    std::array<std::atomic<std::uint64_t>, 64> buckets_{};
    std::atomic<std::uint64_t> sum_{0}, maximum_{0};
public:
    void observe(std::uint64_t ns) noexcept {
        unsigned bucket = 0;
#if defined(__GNUC__) || defined(__clang__)
        bucket = ns ? 64u - static_cast<unsigned>(__builtin_clzll(ns)) : 0u;
#else
        for (auto value = ns; value; value >>= 1) ++bucket;
#endif
        buckets_[std::min(63u, bucket)].fetch_add(1, std::memory_order_relaxed);
        sum_.fetch_add(ns, std::memory_order_relaxed);
        auto old = maximum_.load(std::memory_order_relaxed);
        while (ns > old && !maximum_.compare_exchange_weak(old, ns, std::memory_order_relaxed)) {}
    }
    std::string json() const {
        std::array<std::uint64_t,64> values{}; std::uint64_t count = 0;
        for (unsigned i=0;i<values.size();++i) count += values[i]=buckets_[i].load(std::memory_order_relaxed);
        const auto quantile = [&](unsigned p) {
            const auto rank = count / 100 * p + (count % 100 * p + 99) / 100; std::uint64_t total=0;
            if (!count) return std::uint64_t{0};
            for (unsigned i=0;i<values.size();++i) if ((total+=values[i])>=rank) return i==63 ? UINT64_MAX : ((std::uint64_t{1}<<i)-1);
            return UINT64_MAX;
        };
        std::ostringstream out; out << "{\"count\":"<<count<<",\"sum_ns\":"<<sum_.load()<<",\"max_ns\":"<<maximum_.load()
            <<",\"p50_upper_ns\":"<<quantile(50)<<",\"p95_upper_ns\":"<<quantile(95)<<",\"p99_upper_ns\":"<<quantile(99)<<'}'; return out.str();
    }
};
#define DZIPC_NET_METRICS(X) \
 X(bad_magic) X(bad_version) X(bad_header) X(packet_crc_fail) X(foreign_route) X(source_route_unverified) \
 X(duplicate_fragment) X(conflicting_fragment) X(duplicate_message_suppressed) X(assembly_timeout) X(assembly_revoked) \
 X(reliable_started) X(reliable_completed) X(reliable_timed_out) X(reliable_cancelled) X(reliable_rejected) X(reliable_peer_lost) X(reliable_no_subscribers) X(acks_rx) X(acks_tx) X(nacks_rx) X(nacks_tx) X(retransmitted_bytes) \
 X(peer_expired) X(snapshot_complete) X(snapshot_rejected) X(snapshot_timeout) X(peer_epoch_retired) X(peer_history_full) X(identity_conflict) \
 X(registration_rejected) X(gateway_lost) X(route_recreated) X(remote_target_copies) X(no_target_dropped) X(credit_grant_rejected) \
 X(local_copy_bytes) X(encode_copy_bytes) X(outbox_copy_bytes) X(reassembly_copy_bytes) X(shm_committed_bytes) X(shard_wakeups) X(shard_budget_yields)
#define DZIPC_NET_QUOTAS(X) \
 X(topics) X(sessions) X(peers) X(handles) X(session_handles) X(message_bytes) X(reassembly_bytes) X(peer_reassembly_bytes) X(route_reassembly_bytes) X(assemblies) \
 X(send_bytes) X(send_records) X(session_send_bytes) X(session_send_records) X(publisher_reliable) X(target_states) X(commit_pending_bytes) X(streams) X(receipts) X(peer_receipts) \
 X(outbox_bytes) X(outbox_records) X(init_tasks) X(candidate_bytes) X(peer_candidate_bytes) X(directory_bytes) X(old_directory_bytes) X(peer_history) X(gateway_history) \
 X(session_control_bytes) X(local_control_bytes) X(network_control_bytes) X(command_records) X(command_bytes)
#define DZIPC_METRIC_ENUM(name) name,
enum class NetMetric { DZIPC_NET_METRICS(DZIPC_METRIC_ENUM) Count };
enum class NetQuota { DZIPC_NET_QUOTAS(DZIPC_METRIC_ENUM) Count };
#undef DZIPC_METRIC_ENUM
enum class NetStage { Encode, LocalCommit, CreditWait, OutboxSubmit, OutboxWait, FirstSend, RemoteCommit, AckWait, ApiReturn, Count };
inline constexpr const char* stage_names[]{"encode","local_commit","credit_wait","outbox_submit","gateway_queue_wait","network_first_send","remote_commit","ack_wait","api_return"};
class NetMetrics {
    std::array<std::atomic<std::uint64_t>,static_cast<unsigned>(NetMetric::Count)> counters_{};
    struct Quota { std::atomic<std::uint64_t> peak{0}, rejected{0}; };
    std::array<Quota,static_cast<unsigned>(NetQuota::Count)> quotas_{};
public:
    std::array<LatencyHistogram,static_cast<unsigned>(NetStage::Count)> stages;
    void add(NetMetric id, std::uint64_t n=1) noexcept { counters_[static_cast<unsigned>(id)].fetch_add(n,std::memory_order_relaxed); }
    std::uint64_t get(NetMetric id) const noexcept { return counters_[static_cast<unsigned>(id)].load(std::memory_order_relaxed); }
    void reject(NetQuota id) noexcept { quotas_[static_cast<unsigned>(id)].rejected.fetch_add(1,std::memory_order_relaxed); }
    void peak(NetQuota id, std::uint64_t n) noexcept { auto& value=quotas_[static_cast<unsigned>(id)].peak; auto old=value.load(std::memory_order_relaxed); while(n>old&&!value.compare_exchange_weak(old,n,std::memory_order_relaxed)) {} }
    void observe(NetStage stage,std::uint64_t ns) noexcept { stages[static_cast<unsigned>(stage)].observe(ns); }
    std::string json(unsigned category) const {
        std::ostringstream out; out << '{'; bool first=true;
        auto key=[&](const char* name){if(!first)out<<',';first=false;out<<'"'<<name<<"\":";};
        if(category==0) {
#define DZIPC_METRIC_JSON(name) key(#name); out<<get(NetMetric::name);
            DZIPC_NET_METRICS(DZIPC_METRIC_JSON)
#undef DZIPC_METRIC_JSON
        } else if(category==1) {
#define DZIPC_QUOTA_JSON(name) key(#name); out<<"{\"peak\":"<<quotas_[static_cast<unsigned>(NetQuota::name)].peak.load()<<",\"rejected\":"<<quotas_[static_cast<unsigned>(NetQuota::name)].rejected.load()<<'}';
            DZIPC_NET_QUOTAS(DZIPC_QUOTA_JSON)
#undef DZIPC_QUOTA_JSON
        } else if(category==2) for(unsigned i=0;i<static_cast<unsigned>(NetStage::Count);++i){key(stage_names[i]);out<<stages[i].json();}
        out<<'}';return out.str();
    }
};
inline void record_protocol_error(NetMetrics& m, ProtocolCode code) {
    switch (code) {
    case ProtocolCode::BadMagic: m.add(NetMetric::bad_magic); break;
    case ProtocolCode::BadVersion: m.add(NetMetric::bad_version); break;
    case ProtocolCode::BadCrc: m.add(NetMetric::packet_crc_fail); break;
    case ProtocolCode::TooLarge: m.reject(NetQuota::message_bytes); break;
    default: m.add(NetMetric::bad_header); break;
    }
}
struct MetricTimer {
    NetMetrics* metrics; NetStage stage; std::uint64_t start;
    MetricTimer(NetMetrics* metrics,NetStage stage):metrics(metrics),stage(stage),start(metric_now_ns()){}
    ~MetricTimer(){if(metrics)metrics->observe(stage,metric_now_ns()-start);}
};
// 仅由已核验的目录条目持有；没有按外来packet创建统计表。
struct RouteMetrics {
    LatencyHistogram queue_wait;
    std::atomic<std::uint64_t> tx_bytes{0}, rx_bytes{0}, commits{0};
    std::string json() const { std::ostringstream out;out<<"{\"tx_bytes\":"<<tx_bytes.load()<<",\"rx_bytes\":"<<rx_bytes.load()<<",\"commits\":"<<commits.load()<<",\"queue_wait\":"<<queue_wait.json()<<'}';return out.str(); }
};
}

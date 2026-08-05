#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include "dzIPC/common/srv_data.h"
#include "dzIPC/common/topic_data.h"
#include "libipc/export.h"
#include "libipc/udp.h"

namespace dzIPC {
namespace socket {
enum class SocketDeliveryMode
{
    BestEffort,
    Reliable
};

enum class SocketIntegrityMode
{
    None,
    CRC32C
};

enum class SocketSendStatus
{
    SentUnconfirmed,
    DeliveredAcked,
    FailedTimeout,
    FailedLocalSend,
    FailedIntegrity,
    FailedInvalidArgument
};

struct SocketSendOptions
{
    SocketDeliveryMode delivery{SocketDeliveryMode::BestEffort};
    SocketIntegrityMode integrity{SocketIntegrityMode::None};
    uint64_t ack_timeout_ms{ipc::invalid_value};

    /* 发送节流 (阶段 1): 分片发送速率上限, 单位 B/s, 0 = 不限速。
     *
     * 大包瞬时灌满对端 SO_RCVBUF 是组播丢包的主要嫌疑: 1 MB 消息 = 713 个
     * 分片在几百微秒内发完, 而默认 net.core.rmem_max 只有 208 KB, 订阅端
     * 又是 50ms 轮询取一条 —— 缓冲溢出后到应用线程排空之前的分片全部丢弃。
     *
     * 实测空洞分布证实丢包是缓冲溢出型突发(255 片连续、空洞数只有 1~2 个),
     * 不是随机丢包。正解是扩大 rmem_max (如 64 MB) + 可选限速保护。 */
    std::size_t rate_limit_bps{0};
};

struct SocketSendReport
{
    SocketSendStatus status{SocketSendStatus::FailedInvalidArgument};
    uint32_t crc32c{0};
    uint32_t ack_crc32c{0};
    uint32_t sequence{0};

    bool ok() const
    {
        return status == SocketSendStatus::SentUnconfirmed || status == SocketSendStatus::DeliveredAcked;
    }
};

IPC_EXPORT bool chunk_rev_topic(std::shared_ptr<ipc::socket::UDPNode>& node, std::shared_ptr<TopicData>& rev_msg,
                                uint64_t tm);
IPC_EXPORT bool chunk_rev_server(std::shared_ptr<ipc::socket::UDPNode>& node, std::shared_ptr<ServiceData>& rev_msg,
                                 uint64_t tm, bool ser_or_cli);
IPC_EXPORT SocketSendReport chunk_send_ex(std::shared_ptr<ipc::socket::UDPNode>& node, ipc::buffer& publish_data,
                                          const SocketSendOptions& options);
IPC_EXPORT bool chunk_send(std::shared_ptr<ipc::socket::UDPNode>& node, ipc::buffer& publish_data);

/* 可靠发送: Reliable + CRC32C, 等待 ACK, 丢片时按 NACK 重传。
 * ack_timeout_ms 传 ipc::invalid_value 时由 RTT 自适应算法推导超时。 */
IPC_EXPORT bool chunk_send_reliable(std::shared_ptr<ipc::socket::UDPNode>& node, ipc::buffer& publish_data,
                                    uint64_t ack_timeout_ms = ipc::invalid_value);

IPC_EXPORT ipc::buffer chunk_rev_sniff(ipc::socket::UDPNode& node, uint64_t tm);

/* ---------------- 分片丢失诊断 (阶段 0) ----------------
 *
 * 目的: 区分随机丢包与突发丢包，指导对策选择。
 *   - 随机独立丢包: 链路质量问题，需要重传或降速
 *   - 缓冲溢出突发: 接收端容量问题，需要扩缓冲或发送限速
 *
 * 实测结果(1 MB @ 默认 208KB 缓冲): 255 片连续丢失、空洞数只有 1~2 个。
 * 确认为缓冲溢出型突发，不是随机丢包。正解是扩大 net.core.rmem_max。
 *
 * 位图只在 recv_chunk_common 的栈上存在, 消息组装失败时直接丢弃, 因此
 * 统计必须由库内部采集。默认关闭, 零开销; 由 benchmark 显式打开。 */
struct FragmentLossStats
{
    /* 空洞 = 一段连续缺失的分片, 按长度分桶。
     * 桶上界(含): [1] [2-5] [6-15] [16-31] [32-63] [64-255] [256+]
     * 桶 0 占比高 = 随机丢包; 高位桶有量 = 突发丢包(缓冲溢出的特征)。 */
    static constexpr std::size_t kGapBuckets = 7;
    std::uint64_t gap_hist[kGapBuckets]{};

    std::uint64_t messages_complete{0};     // 组装成功的多分片消息数
    std::uint64_t messages_incomplete{0};   // 组装失败被丢弃的消息数
    std::uint64_t fragments_expected{0};    // 上述消息的分片总数
    std::uint64_t fragments_missing{0};     // 其中缺失的分片总数
    std::uint64_t gaps_total{0};            // 空洞总数
    std::uint64_t gap_max{0};               // 观察到的最长空洞

    /* 平均空洞长度。接近 1 说明随机丢包, 显著大于 1 说明突发。 */
    double mean_gap_len() const
    {
        return (gaps_total == 0) ? 0.0 : static_cast<double>(fragments_missing) / static_cast<double>(gaps_total);
    }
};

/* 打开/关闭采集。关闭时 recv 路径不做任何额外工作。 */
IPC_EXPORT void set_fragment_loss_tracking(bool enabled);
IPC_EXPORT bool fragment_loss_tracking_enabled();
/* 读取累计值快照 (跨线程安全)。 */
IPC_EXPORT FragmentLossStats get_fragment_loss_stats();
IPC_EXPORT void reset_fragment_loss_stats();

/* ---------------- 发送节流全局配置 (阶段 1) ----------------
 *
 * 默认 0 = 不限速。设为非零时, 所有多分片消息的发送速率被限制在该值以下。
 * 仅对 socket 有意义: SHM 传输没有网络分片, 也没有接收缓冲溢出问题。
 *
 * 典型用法: benchmark 在发布端子进程 pin_to_cpu 之后、InitChannel 之前设置。
 * 线程安全: 并发 get/set 彼此不互斥, set 的生效顺序不保证; 适用于启动期设置
 * 一次后不再修改的场景。 */
IPC_EXPORT void set_socket_rate_limit_bps(std::size_t bps);
IPC_EXPORT std::size_t get_socket_rate_limit_bps();
}   // namespace socket
}   // namespace dzIPC

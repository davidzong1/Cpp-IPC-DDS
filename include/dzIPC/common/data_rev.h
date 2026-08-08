#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>
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

    /* ---------------- 端点分离 ----------------
     *
     * 在此节点上等待 ACK/NACK, 而不是在发数据的那条 socket 上。
     *
     * 为什么需要: 数据 socket 加入了组播组且 IP_MULTICAST_LOOP=1(它必须为 1,
     * 否则同机其他进程收不到), 于是自己发出的每一个分片都会回绕进自己的接收
     * 队列。1 MB = 713 片, 对端的 ACK 排在这 713 片后面, 等待窗口必然先超时。
     * 实测表现为吞吐塌到 1 msg/s 而丢包率 0.00% —— 数据面是通的, 坏的是确认面。
     *
     * ack_node 应当是一条 NodeRole::RecvOnly 的独立 socket, 绑在不同端口上,
     * 那里只有对端的 ACK/NACK, 没有自己的数据。
     *
     * 为 nullptr 时退化为在数据节点上等 —— 即历史行为, 保证既有调用方不受影响。*/
    std::shared_ptr<ipc::socket::UDPNode> ack_node{nullptr};

    /* 发完分片后是否发 Heartbeat 通告。
     *
     * 除了让接收端提前收敛, 它还是**唯一**能把 sequence 送到接收端的载体:
     * 12 字节 tail 里没有 sequence 字段, 接收端只能填 0, 而发送端逐条自增,
     * 于是 ACK 的 sequence 校验只有进程发出的第一条消息能通过。详见
     * IpcRtpsHeartbeatMsg 的注释。 */
    bool send_heartbeat{true};
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

/* 端点分离版本: ACK/NACK 从 ack_node 发出, 而不是回到收数据的那条 socket。
 *
 * ack_node 应当是 NodeRole::SendOnly 的独立 socket, 绑在发送端监听的 ACK 端口上。
 * 传 nullptr 则退化为上面的三/四参数版本, 行为逐字节一致。
 *
 * 刻意做成重载而非默认参数: 保留原符号, 已链接的二进制不受影响。 */
IPC_EXPORT bool chunk_rev_topic(std::shared_ptr<ipc::socket::UDPNode>& node, std::shared_ptr<TopicData>& rev_msg,
                                uint64_t tm, const std::shared_ptr<ipc::socket::UDPNode>& ack_node);
IPC_EXPORT bool chunk_rev_server(std::shared_ptr<ipc::socket::UDPNode>& node, std::shared_ptr<ServiceData>& rev_msg,
                                 uint64_t tm, bool ser_or_cli,
                                 const std::shared_ptr<ipc::socket::UDPNode>& ack_node);

IPC_EXPORT SocketSendReport chunk_send_ex(std::shared_ptr<ipc::socket::UDPNode>& node, ipc::buffer& publish_data,
                                          const SocketSendOptions& options);
IPC_EXPORT bool chunk_send(std::shared_ptr<ipc::socket::UDPNode>& node, ipc::buffer& publish_data);

/* 可靠发送: Reliable + CRC32C, 等待 ACK, 丢片时按 NACK 重传。
 * ack_timeout_ms 传 ipc::invalid_value 时由 RTT 自适应算法推导超时。
 *
 * 注意: 本重载在发数据的同一条 socket 上等 ACK。组播 + IP_MULTICAST_LOOP=1 下
 * 自己的分片会回绕堵在 ACK 前面, 多分片消息基本注定超时 —— 用下面带 ack_node
 * 的重载。 */
IPC_EXPORT bool chunk_send_reliable(std::shared_ptr<ipc::socket::UDPNode>& node, ipc::buffer& publish_data,
                                    uint64_t ack_timeout_ms = ipc::invalid_value);

/* 端点分离版本: ACK/NACK 从 ack_node 收, 那条 socket 上没有自己的数据回绕。
 * ack_node 应为 NodeRole::RecvOnly, 绑在接收端回 ACK 的那个端口上。 */
IPC_EXPORT bool chunk_send_reliable(std::shared_ptr<ipc::socket::UDPNode>& node, ipc::buffer& publish_data,
                                    const std::shared_ptr<ipc::socket::UDPNode>& ack_node,
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

    /* ---- NACK 编码选择 ----
     * 没有这几个计数就无法回答"位图到底有没有被用上" —— 零丢包时两个计数都是 0,
     * 与"位图坏了从不触发"在外部观感上完全一样。诊断丢包问题时先看这里:
     *   bitmap_sent 恒为 0 而 explicit_sent 有量 -> 对端没通告 kFlagBitmapNack,
     *     或缺片一直很稀疏(择优判据认为显式列表更省), 都属正常。
     *   bitmap_truncated 有量 -> 缺片跨度超过单帧容量(11496 位), 剩余部分要多花
     *     一轮 round_wait_ms。持续出现说明该考虑分多帧发而不是等下一轮。 */
    std::uint64_t nack_explicit_sent{0};      // 发出的显式列表 NACK 帧数
    std::uint64_t nack_bitmap_sent{0};        // 发出的位图 NACK 帧数
    std::uint64_t nack_explicit_truncated{0};   // 因 256 片上限被截断的次数
    std::uint64_t nack_bitmap_truncated{0};     // 因跨度超出单帧容量被截断的次数

    /* ---- 跨轮 NACK 抑制 (阶段 3, sender 侧采集) ----
     * 上面 4 个 NACK 计数都在接收端 (send_nack_*, 发 NACK 的一方), 量不到
     * 发送端抑制的效果, 补这两个 (采集点在 chunk_send_ex 的重传批):
     *   nack_suppressed            = 因 K=1 抑制窗口被压掉的重传页数, 抑制
     *     生效的直接证据。压力场景 A (多订阅者错峰) 期望 > 0; 场景 B (单
     *     订阅者) 期望 ≈ 0 (单 peer 时抑制整体关闭)。
     *   pages_retransmitted_again  = 同一条消息内被重传 ≥2 次的页数。无抑制
     *     基线 > 0 —— NACK 延迟/重复到达会触发重复重传; 抑制后此类场景
     *     趋近 0, 重传本身又丢 (Case D) 时如实 > 0。 */
    std::uint64_t nack_suppressed{0};
    std::uint64_t pages_retransmitted_again{0};

    /* ---- 闭环自适应限速 (段3 任务2b, sender 侧采集) ----
     * rate_reductions / rate_increments 是 AIMD 减半/增倍动作计数, 比值 +
     * rate_bps_now 的 trace 是场景 C 标定观察点 (K 与 kOverflowSpanPages);
     * rate_floor_warns = 下限告警次数 (每节点一次, 场景 E 判据);
     * retransmit_bytes = 重传批实际发出的字节数 (场景 C 判据 ③)。
     * 计数全走 frag_track_enabled() 门; 自适应功能本身不受门控。 */
    std::uint64_t rate_reductions{0};
    std::uint64_t rate_increments{0};
    std::uint64_t rate_floor_warns{0};
    std::uint64_t retransmit_bytes{0};
    std::size_t rate_bps_now{0};   // 当前生效速率快照 (采样周期读, 非自旋)

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

/* ---------------- 对端身份表 (阶段 2, DECISIONS.md D-1 选项 B) ----------------
 *
 * 发送端按 receiver_id 记录每个对端(订阅者)的确认进度 —— 段3 跨轮 NACK
 * 抑制与 WHC 流控的前置数据。表内字段按 D-1 裁定: 只有最高确认序号 + 两个
 * 时间戳; ack_count 是任务验收要求的可观测出口 (导出"确认数") 才加的。
 *
 * 用法 (多订阅者验证): 每条 Reliable 消息, chunk_send_ex 只观察**第一个**
 * 匹配 ACK 就返回 (data_rev.cc 的 got_ack 分支, 其余 ACK 被 drain 丢弃),
 * 因此验证 N 个订阅者要连发多条消息, 检查各条目的 ack_count 是否都在增长、
 * 且各 highest_acked_seq 接近发送序号。注意 receiver_id 是进程级身份
 * (local_node_id, 每进程一个), 同进程内多个订阅者会合并为同一条目。 */
struct PeerAckInfo
{
    uint32_t receiver_id{0};         // 对端自报身份 (data_rev.cc local_node_id)
    uint32_t highest_acked_seq{0};   // 该对端确认过的最高消息序号
    uint64_t ack_count{0};           // 该对端累计确认次数
};

/* 读取指定发送节点的对端确认表快照, 按 receiver_id 升序 (跨线程安全)。 */
IPC_EXPORT std::vector<PeerAckInfo> get_peer_ack_info(const ipc::socket::UDPNode* node);

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

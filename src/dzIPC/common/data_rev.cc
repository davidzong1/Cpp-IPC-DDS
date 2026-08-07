#include "dzIPC/common/data_rev.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <memory>
#include <mutex>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "dzIPC/common/crc32c.h"
#include "ipc_msg/ipc_msg_base/udp_rtps_ack_msg.hpp"

namespace dzIPC {
namespace socket {
namespace {
constexpr std::size_t UDP_MAX_SIZE = 1'472;
constexpr std::size_t TAIL_SIZE = 12;
constexpr std::size_t MAX_RECV_TOTAL_SIZE = 64 * 1'024 * 1'024;
constexpr int SEND_RETRY_MAX = 10;
constexpr int RTPS_MAX_NACK_ROUND = 5;
constexpr int SEND_BURST_BEFORE_YIELD = 64;
constexpr uint64_t ACK_FAST_WAIT_MS = 5;
constexpr uint8_t INTEGRITY_FLAG_CRC32C = 0x01;

/* Heartbeat 相关 (端点分离 + Heartbeat) */
constexpr int kMaxHeartbeatRepeat = 3;   // 静默轮里最多补发几次 HB
/* HB 与最后一个分片背靠背发出, 可能被重排到最后几片之前。收到 HB 后不立刻定论,
 * 给在途分片一点排空时间再判缺片。 */
constexpr uint64_t kHbGraceMs = 2;

/* BestEffort 下发完成通告的最小分片数。
 *
 * 权衡的是"每条消息多一个包"与"丢片时少空等一轮"。
 *   收益: 丢片时接收端立刻放弃, 而不是空等 kFruitlessRoundLimit × round_wait_ms
 *         (下限 20ms, 即最坏 40ms)。这段时间订阅线程什么都干不了, 可能连累下一条。
 *   成本: 多一个 33 字节的包。字节开销可以忽略, 但**包数**开销是 1/page_cnt ——
 *         3 分片的消息要多付 33% 的包, 而 UDP 吞吐在本机/局域网上常常是包率受限的。
 *
 * 取 16: 包数开销降到 6% 以下, 而这个尺寸(~23 KB)的消息丢了再重来代价已经不小。
 * 低于这个门槛的 BestEffort 消息不发 HB, 回退到原有的 fruitless 启发式。
 *
 * Reliable 不受此限制 —— 它必须发, 因为 sequence 只能由 HB 携带。 */
constexpr uint16_t kBestEffortHeartbeatMinPages = 16;

struct chunk_meta
{
    uint16_t page_cnt{0};
    uint32_t total_size{0};
    uint32_t msg_id{0};
    uint32_t sequence{0};
};

std::atomic<uint32_t>& msg_sequence_counter()
{
    static std::atomic<uint32_t> counter{0};
    return counter;
}

/* ---------------- 分片丢失诊断 (阶段 0) ---------------- */

std::atomic<bool>& frag_track_enabled()
{
    static std::atomic<bool> enabled{false};
    return enabled;
}

/* 用 atomic 数组而非加锁: 采集点在收包热路径上, 且各计数彼此独立,
 * 不需要跨字段一致的快照 —— 直方图允许存在瞬时的轻微不自洽。 */
struct FragTrackState
{
    std::atomic<std::uint64_t> gap_hist[FragmentLossStats::kGapBuckets]{};
    std::atomic<std::uint64_t> messages_complete{0};
    std::atomic<std::uint64_t> messages_incomplete{0};
    std::atomic<std::uint64_t> fragments_expected{0};
    std::atomic<std::uint64_t> fragments_missing{0};
    std::atomic<std::uint64_t> gaps_total{0};
    std::atomic<std::uint64_t> gap_max{0};
    std::atomic<std::uint64_t> nack_explicit_sent{0};
    std::atomic<std::uint64_t> nack_bitmap_sent{0};
    std::atomic<std::uint64_t> nack_explicit_truncated{0};
    std::atomic<std::uint64_t> nack_bitmap_truncated{0};
};

FragTrackState& frag_track_state()
{
    static FragTrackState state;
    return state;
}

/* 发送节流全局开关 (阶段 1)。SocketSendOptions::rate_limit_bps 为 0 时回退到此值,
 * 使得 pub-sub 这类不暴露 options 的上层接口也能被节流。 */
std::atomic<std::size_t>& global_rate_limit_bps()
{
    static std::atomic<std::size_t> bps{0};
    return bps;
}

std::size_t gap_bucket_of(std::uint64_t len)
{
    if (len <= 1) return 0;
    if (len <= 5) return 1;
    if (len <= 15) return 2;
    if (len <= 31) return 3;
    if (len <= 63) return 4;
    if (len <= 255) return 5;
    return 6;
}

/* 扫描 received 位图, 把连续缺失段记入直方图。
 * received 是 1-based (下标 0 不用), 长度 page_cnt + 1。
 * complete 为 false 时说明消息最终被丢弃, 单独计数。 */
void record_fragment_gaps(const std::vector<uint8_t>& received, uint16_t page_cnt, bool complete)
{
    if (!frag_track_enabled().load(std::memory_order_relaxed))
    {
        return;
    }
    FragTrackState& st = frag_track_state();
    st.fragments_expected.fetch_add(page_cnt, std::memory_order_relaxed);
    if (complete)
    {
        st.messages_complete.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        st.messages_incomplete.fetch_add(1, std::memory_order_relaxed);
    }

    std::uint64_t run = 0;
    std::uint64_t missing = 0;
    std::uint64_t local_max = 0;
    for (std::size_t i = 1; i <= static_cast<std::size_t>(page_cnt); ++i)
    {
        if (received[i] == 0)
        {
            ++run;
            ++missing;
            continue;
        }
        if (run > 0)
        {
            st.gap_hist[gap_bucket_of(run)].fetch_add(1, std::memory_order_relaxed);
            st.gaps_total.fetch_add(1, std::memory_order_relaxed);
            local_max = std::max(local_max, run);
            run = 0;
        }
    }
    if (run > 0)   // 末尾未闭合的空洞
    {
        st.gap_hist[gap_bucket_of(run)].fetch_add(1, std::memory_order_relaxed);
        st.gaps_total.fetch_add(1, std::memory_order_relaxed);
        local_max = std::max(local_max, run);
    }
    st.fragments_missing.fetch_add(missing, std::memory_order_relaxed);

    std::uint64_t prev = st.gap_max.load(std::memory_order_relaxed);
    while (local_max > prev && !st.gap_max.compare_exchange_weak(prev, local_max, std::memory_order_relaxed))
    {
    }
}

uint32_t local_node_id()
{
    static const uint32_t id = []
    {
        const auto now = static_cast<uint64_t>(std::chrono::steady_clock::now().time_since_epoch().count());
        return static_cast<uint32_t>((now >> 32) ^ now ^ 0xA5'3C'9E'17u);
    }();
    return id;
}

uint64_t elapsed_ms(const std::chrono::steady_clock::time_point& begin)
{
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - begin).count());
}

bool parse_tail(const ipc::buffer& buf, ipc_tail_msg& tail)
{
    if (buf.size() < TAIL_SIZE)
    {
        return false;
    }

    const auto* p = static_cast<const uint8_t*>(buf.data());
    const std::size_t n = buf.size();
    tail.page_cnt = static_cast<uint16_t>(p[n - 12]) << 8 | static_cast<uint16_t>(p[n - 11]);
    tail.now_page = static_cast<uint16_t>(p[n - 10]) << 8 | static_cast<uint16_t>(p[n - 9]);
    tail.total_size = static_cast<uint32_t>(p[n - 8]) << 24 | static_cast<uint32_t>(p[n - 7]) << 16
                      | static_cast<uint32_t>(p[n - 6]) << 8 | static_cast<uint32_t>(p[n - 5]);
    tail.dz_ipc_msg_id = static_cast<uint32_t>(p[n - 4]) << 24 | static_cast<uint32_t>(p[n - 3]) << 16
                         | static_cast<uint32_t>(p[n - 2]) << 8 | static_cast<uint32_t>(p[n - 1]);
    return true;
}

void write_now_page(ipc::buffer& chunk, uint16_t now_page)
{
    if (chunk.size() < TAIL_SIZE)
    {
        return;
    }
    auto* p = static_cast<uint8_t*>(chunk.data());
    const std::size_t n = chunk.size();
    p[n - 10] = static_cast<uint8_t>(now_page >> 8);
    p[n - 9] = static_cast<uint8_t>(now_page & 0xFF);
}

bool valid_chunk_meta(const ipc_tail_msg& tail)
{
    if (tail.page_cnt == 0 || tail.total_size < TAIL_SIZE)
    {
        return false;
    }
    if (tail.total_size > MAX_RECV_TOTAL_SIZE)
    {
        return false;
    }

    const std::size_t page_cnt = static_cast<std::size_t>(tail.page_cnt);
    const std::size_t total_size = static_cast<std::size_t>(tail.total_size);
    const std::size_t max_total = page_cnt * UDP_MAX_SIZE;
    const std::size_t min_total = (page_cnt == 0) ? TAIL_SIZE : ((page_cnt - 1) * UDP_MAX_SIZE + TAIL_SIZE);
    return total_size >= min_total && total_size <= max_total;
}

std::size_t expected_page_size(const chunk_meta& meta, uint16_t now_page)
{
    if (now_page == meta.page_cnt)
    {
        const std::size_t offset = static_cast<std::size_t>(now_page - 1) * UDP_MAX_SIZE;
        return static_cast<std::size_t>(meta.total_size) - offset;
    }
    return UDP_MAX_SIZE;
}

bool place_page(const ipc::buffer& page_buf, const chunk_meta& meta, std::vector<uint8_t>& assembled,
                std::vector<uint8_t>& received, std::size_t& received_cnt)
{
    ipc_tail_msg tail;
    if (!parse_tail(page_buf, tail))
    {
        return false;
    }
    if (tail.page_cnt != meta.page_cnt || tail.total_size != meta.total_size || tail.dz_ipc_msg_id != meta.msg_id)
    {
        return false;
    }
    if (tail.now_page == 0 || tail.now_page > meta.page_cnt)
    {
        return false;
    }

    const std::size_t payload = page_buf.size();
    const std::size_t expected = expected_page_size(meta, tail.now_page);
    if (payload != expected)
    {
        return false;
    }

    if (received[tail.now_page] != 0)
    {
        return true;
    }

    const std::size_t offset = static_cast<std::size_t>(tail.now_page - 1) * UDP_MAX_SIZE;
    std::memcpy(assembled.data() + offset, page_buf.data(), payload);
    received[tail.now_page] = 1;
    ++received_cnt;
    return true;
}

/* 限速下 page_cnt 个分片"在线上跑完"至少需要多久。
 *
 * 节流是发送端的本地配置, 线格式里没有它。接收端只能读本进程的全局值 ——
 * 同机测试(收发同一台机器、同一份配置)成立; 跨机部署时两端必须配一致的限速,
 * 否则接收端算出的窗口会偏小。这是当前实现的已知边界。
 *
 * 返回 0 表示不限速, 调用方按原逻辑走。 */
uint64_t rate_limited_transit_ms(uint16_t page_cnt)
{
    const std::size_t bps = global_rate_limit_bps().load(std::memory_order_relaxed);
    if (bps == 0)
    {
        return 0;
    }
    const uint64_t bytes = static_cast<uint64_t>(page_cnt) * static_cast<uint64_t>(UDP_MAX_SIZE);
    return (bytes * 1'000ULL) / static_cast<uint64_t>(bps);
}

uint64_t calc_round_wait_ms(uint16_t page_cnt, uint64_t tm)
{
    const uint64_t by_pages = std::max<uint64_t>(20, std::min<uint64_t>(200, static_cast<uint64_t>(page_cnt) * 4));

    /* 修正 1: 组装窗口必须覆盖限速下的实际传输时间。
     *
     * 原来的 200ms 上限是按"发送端瞬时灌完, 窗口只需覆盖 RTT + 抖动"推导的。
     * 限速后传输时间本身就成了主要项: 1 MB @ 20 MB/s 需要 50ms, 而 tm 预算
     * 一旦把 by_budget 压到 16ms(ser-cli tm=100ms 时), 首轮必然收不全, 后续
     * NACK 重传同样被限速拖累, 5 轮跑完仍然失败 —— 这就是 sercli_socket 三个
     * 尺寸全部 "Failed to send" 的原因。
     *
     * 给传输时间留 1.5 倍余量(覆盖 sleep_for 超调与调度抖动), 并作为窗口下限:
     * 传输时间是物理下界, 任何小于它的窗口都注定失败, 不能被 tm 预算压穿。 */
    const uint64_t transit_ms = rate_limited_transit_ms(page_cnt);
    const uint64_t floor_ms = transit_ms + transit_ms / 2;

    if (tm == ipc::invalid_value)
    {
        return std::max(by_pages, floor_ms);
    }
    const uint64_t by_budget = std::max<uint64_t>(10, tm / static_cast<uint64_t>(RTPS_MAX_NACK_ROUND + 1));
    return std::max(std::min(by_pages, by_budget), floor_ms);
}

uint64_t calc_nack_wait_ms(uint16_t page_cnt)
{
    const uint64_t by_pages = std::max<uint64_t>(20, std::min<uint64_t>(200, static_cast<uint64_t>(page_cnt) * 4));
    /* 同修正 1: NACK 重传的等待窗口也要覆盖限速传输时间。 */
    const uint64_t transit_ms = rate_limited_transit_ms(page_cnt);
    return std::max(by_pages, transit_ms + transit_ms / 2);
}

/* ------------------------------------------------------------------------ *
 * 方案 B'：ACK 等待时间按实测 RTT 自适应 (RFC 6298)
 *
 * 原先首轮 ACK 固定等 ACK_FAST_WAIT_MS = 5ms。这个值对三种链路差了几个数量级:
 *   同机 loopback  RTT  20-50 us   -> 5ms 过大 100 倍
 *   同交换机千兆   RTT 0.1-0.5 ms  -> 5ms 略大, 合理
 *   跨机房         RTT   5-50 ms   -> 5ms 已经不够, 会触发无谓重传
 * 固定值无法同时服务这三者, 所以不猜链路类型, 直接测量: ACK 回来的时间就是
 * RTT 的观测值, 获取成本为零。
 *
 * 采用 TCP 的平滑算法:
 *   srtt   = (1-1/8)*srtt   + (1/8)*sample
 *   rttvar = (1-1/4)*rttvar + (1/4)*|srtt - sample|
 *   rto    = clamp(srtt + 4*rttvar, kMinRtoUs, kMaxRtoUs)
 *
 * 冷启动仍用 5ms: 未知链路按保守值起步, 观测到实际 RTT 后再收敛。这样跨主机
 * 首包行为与改动前完全一致, 不会引入回归。
 *
 * 上界取 20ms 而非原来的 200ms: 目标部署是同交换机 (RTT 0.1-0.5ms), 20ms 已是
 * 其 40 倍余量。若将来要支持跨机房, 这个值需要放大。
 * ------------------------------------------------------------------------ */
constexpr uint64_t kMinRtoUs = 200;        // loopback 下限, 低于此值 CPU 空转不划算
constexpr uint64_t kMaxRtoUs = 20'000;     // 同交换机上限 (20ms)
constexpr uint64_t kInitialRtoUs = ACK_FAST_WAIT_MS * 1'000;   // 冷启动 = 原固定值

class RttEstimator
{
public:
    /* 当前的重传超时, 微秒 */
    uint64_t rto_us() const
    {
        std::lock_guard<std::mutex> lock(mtx_);
        return rto_us_;
    }

    void observe(uint64_t sample_us)
    {
        if (sample_us == 0)
        {
            return;   // 时钟精度不足, 忽略
        }
        std::lock_guard<std::mutex> lock(mtx_);
        if (!has_sample_)
        {
            /* RFC 6298 (2.2): 首个样本直接初始化, 不做平滑 */
            srtt_us_ = static_cast<double>(sample_us);
            rttvar_us_ = static_cast<double>(sample_us) / 2.0;
            has_sample_ = true;
        }
        else
        {
            constexpr double kAlpha = 1.0 / 8.0;
            constexpr double kBeta = 1.0 / 4.0;
            const double s = static_cast<double>(sample_us);
            rttvar_us_ = (1.0 - kBeta) * rttvar_us_ + kBeta * std::abs(srtt_us_ - s);
            srtt_us_ = (1.0 - kAlpha) * srtt_us_ + kAlpha * s;
        }
        const double rto = srtt_us_ + 4.0 * rttvar_us_;
        rto_us_ = std::min(kMaxRtoUs, std::max(kMinRtoUs, static_cast<uint64_t>(rto)));
    }

private:
    mutable std::mutex mtx_;
    double srtt_us_{0.0};
    double rttvar_us_{0.0};
    uint64_t rto_us_{kInitialRtoUs};
    bool has_sample_{false};
};

/* 每个 UDPNode 一份估计器 —— RTT 是链路属性, 同一节点上的所有消息共享。
 * 用裸指针做 key: UDPNode 由上层 shared_ptr 持有, 生命周期覆盖所有收发调用。
 * 条目不回收 (节点数是有限的 topic 数, 不会无界增长)。 */
RttEstimator& rtt_of(const ipc::socket::UDPNode* node)
{
    static std::mutex map_mtx;
    static std::unordered_map<const ipc::socket::UDPNode*, std::unique_ptr<RttEstimator>> map;
    std::lock_guard<std::mutex> lock(map_mtx);
    auto it = map.find(node);
    if (it == map.end())
    {
        it = map.emplace(node, std::make_unique<RttEstimator>()).first;
    }
    return *it->second;
}

/* 首轮 ACK 等待: 由实测 RTT 决定, 向上取整到毫秒 (receive() 的粒度是 ms)。 */
uint64_t ack_first_wait_ms(const ipc::socket::UDPNode* node)
{
    const uint64_t rto_us = rtt_of(node).rto_us();
    return std::max<uint64_t>(1, (rto_us + 999) / 1'000);
}

bool send_chunk_with_retry(ipc::socket::UDPNode& node, ipc::buffer& chunk)
{
    int retry = 0;
    while (!node.send(chunk))
    {
        if (++retry >= SEND_RETRY_MAX)
        {
            return false;
        }
        if (retry <= 2)
        {
            std::this_thread::yield();
        }
        else
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
    }
    return true;
}

bool send_all_chunks(ipc::socket::UDPNode& node, std::vector<ipc::buffer>& chunks, std::size_t rate_limit_bps)
{
    const auto send_start = std::chrono::steady_clock::now();
    std::size_t bytes_sent = 0;

    for (std::size_t i = 0; i < chunks.size(); ++i)
    {
        /* 节流: 每片发送前等到令牌桶允许发送的时刻。
         * 已发字节数 / 速率上限 = 已用时间预算。若实际用时小于预算, 差值即等待时长。
         *
         * 注意这里是相对 send_start 的绝对时间表, 不是"每片睡 X us"的增量延迟。
         * 20 MB/s 下单片预算只有 73 us, 而非实时内核的 sleep_for 通常超调几十
         * 微秒 —— 绝对时间表能自然吸收超调(睡过头则下一片 budget < elapsed,
         * 直接不等), 增量式则会让误差逐片累加, 实际速率显著低于设定值。
         * 不要改成增量式。 */
        if (rate_limit_bps > 0 && i > 0)
        {
            bytes_sent += chunks[i - 1].size();
            const auto now = std::chrono::steady_clock::now();
            const std::uint64_t elapsed_us =
                std::chrono::duration_cast<std::chrono::microseconds>(now - send_start).count();
            const std::uint64_t budget_us = (bytes_sent * 1'000'000ULL) / rate_limit_bps;
            if (budget_us > elapsed_us)
            {
                const std::uint64_t wait_us = budget_us - elapsed_us;
                std::this_thread::sleep_for(std::chrono::microseconds(wait_us));
            }
        }

        if (!send_chunk_with_retry(node, chunks[i]))
        {
            return false;
        }
        if (((i + 1) % SEND_BURST_BEFORE_YIELD) == 0 && (i + 1) < chunks.size())
        {
            std::this_thread::yield();
        }
    }
    return true;
}

/* Clean buffer after receiving */
void drain_self_loopback(ipc::socket::UDPNode& node)
{
    constexpr int kMaxDrain = 8'192;
    for (int i = 0; i < kMaxDrain; ++i)
    {
        ipc::buffer buf = node.receive_nowait();
        if (buf.empty())
        {
            return;
        }
    }
}

void send_ack(ipc::socket::UDPNode& node, const chunk_meta& meta, uint32_t payload_crc32c)
{
    IpcRtpsAckMsg ack_msg;
    ack_msg.page_cnt = meta.page_cnt;
    ack_msg.total_size = meta.total_size;
    ack_msg.data_msg_id = meta.msg_id;
    ack_msg.receiver_id = local_node_id();
    ack_msg.sequence = meta.sequence;
    ack_msg.integrity_flags = INTEGRITY_FLAG_CRC32C;
    ack_msg.payload_crc32c = payload_crc32c;
    ipc::buffer ack_buf = ack_msg.serialize();
    send_chunk_with_retry(node, ack_buf);
}

/* 发一帧 Heartbeat。必须在**数据通道**上发 —— 接收端只在数据通道上收包。 */
void send_heartbeat(ipc::socket::UDPNode& node, const chunk_meta& meta, bool reliable, bool final_hb, uint16_t round)
{
    IpcRtpsHeartbeatMsg hb;
    hb.page_cnt = meta.page_cnt;
    hb.total_size = meta.total_size;
    hb.data_msg_id = meta.msg_id;
    hb.sender_id = local_node_id();
    hb.sequence = meta.sequence;
    /* kFlagBitmapNack 恒置: 本版本的 chunk_send_ex 一定认识 DZNB。接收端据此
     * 决定敢不敢用位图格式 —— 对旧版发送端(不发 HB 或 HB 里没这一位)它会退回
     * 显式列表, 那是唯一能被对方解析的编码。 */
    hb.flags = static_cast<uint8_t>((reliable ? IpcRtpsHeartbeatMsg::kFlagReliable : 0)
                                    | (final_hb ? IpcRtpsHeartbeatMsg::kFlagFinal : 0)
                                    | IpcRtpsHeartbeatMsg::kFlagBitmapNack);
    hb.round = round;
    ipc::buffer hb_buf = hb.serialize();
    send_chunk_with_retry(node, hb_buf);
}

bool wait_first_data_chunk(ipc::socket::UDPNode& node, std::shared_ptr<IpcMsgBase>& msg_ptr, chunk_meta& meta,
                           ipc::buffer& first_page, const std::chrono::steady_clock::time_point& begin, uint64_t tm,
                           IpcRtpsHeartbeatMsg& pending_hb, bool& have_pending_hb)
{
    IpcRtpsHeartbeatMsg hb_probe;
    while (true)
    {
        uint64_t wait_ms = tm;
        if (tm != ipc::invalid_value)
        {
            const uint64_t used = elapsed_ms(begin);
            if (used >= tm)
            {
                return false;
            }
            wait_ms = tm - used;
        }

        ipc::buffer buf = node.receive(wait_ms);
        if (buf.empty())
        {
            return false;
        }

        /* Reliable 发送端会在数据之前先发一帧前导 HB。必须在 check_id 过滤之前
         * 截住它 —— 它带着 sequence, 而 sequence 无法从数据分片的 tail 里得到。
         * 单页消息尤其依赖这一条: 首片一到就要立刻 ACK, 没有第二次机会。 */
        if (hb_probe.check_hb_id(buf))
        {
            hb_probe.deserialize(buf);
            pending_hb = hb_probe;
            have_pending_hb = true;
            continue;
        }

        if (!msg_ptr->check_id(buf))
        {
            continue;
        }

        ipc_tail_msg tail;
        if (!parse_tail(buf, tail) || !valid_chunk_meta(tail))
        {
            continue;
        }

        if (tail.now_page == 0 || tail.now_page > tail.page_cnt)
        {
            continue;
        }

        chunk_meta tentative{tail.page_cnt, tail.total_size, tail.dz_ipc_msg_id};
        if (buf.size() != expected_page_size(tentative, tail.now_page))
        {
            continue;
        }

        /* 前导 HB 与本条消息对得上, 就采纳它的 sequence。对不上说明那是上一条
         * 消息的残留, 丢弃。 */
        if (have_pending_hb && pending_hb.data_msg_id == tentative.msg_id && pending_hb.page_cnt == tentative.page_cnt
            && pending_hb.total_size == tentative.total_size)
        {
            tentative.sequence = pending_hb.sequence;
        }

        meta = tentative;
        first_page = std::move(buf);
        return true;
    }
}

void send_nack_for_missing(ipc::socket::UDPNode& node, const chunk_meta& meta, const std::vector<uint8_t>& received,
                           IpcRtpsNackMsg& nack_msg)
{
    nack_msg.page_cnt = meta.page_cnt;
    nack_msg.total_size = meta.total_size;
    nack_msg.data_msg_id = meta.msg_id;
    nack_msg.receiver_id = local_node_id();
    nack_msg.sequence = meta.sequence;
    nack_msg.missing_pages.clear();

    for (uint16_t i = 1; i <= meta.page_cnt; ++i)
    {
        if (received[i] == 0)
        {
            nack_msg.missing_pages.push_back(i);
            if (nack_msg.missing_pages.size() >= IpcRtpsNackMsg::kMaxMissingPages)
            {
                /* 后面还缺的片这一轮报不出去, 要多等一个 round_wait_ms。
                 * 计入统计: 这个数持续有量就说明该让对端支持位图了。 */
                if (i < meta.page_cnt && frag_track_enabled().load(std::memory_order_relaxed))
                {
                    frag_track_state().nack_explicit_truncated.fetch_add(1, std::memory_order_relaxed);
                }
                break;
            }
        }
    }

    if (nack_msg.missing_pages.empty())
    {
        return;
    }

    if (frag_track_enabled().load(std::memory_order_relaxed))
    {
        frag_track_state().nack_explicit_sent.fetch_add(1, std::memory_order_relaxed);
    }
    ipc::buffer nack_buf = nack_msg.serialize();
    send_chunk_with_retry(node, nack_buf);
}

/* 位图 NACK。窗口从第一个缺失页开始, 覆盖到最后一个缺失页(或单页容量上限)。
 *
 * 返回是否真的发出去了。false 表示"没有缺片", 调用方不必再走显式列表。 */
bool send_nack_bitmap_for_missing(ipc::socket::UDPNode& node, const chunk_meta& meta,
                                  const std::vector<uint8_t>& received, IpcRtpsNackBitmapMsg& nb_msg)
{
    uint16_t first_missing = 0;
    uint16_t last_missing = 0;
    for (uint16_t i = 1; i <= meta.page_cnt; ++i)
    {
        if (received[i] == 0)
        {
            if (first_missing == 0)
            {
                first_missing = i;
            }
            last_missing = i;
        }
    }

    if (first_missing == 0)
    {
        return false;
    }

    nb_msg.page_cnt = meta.page_cnt;
    nb_msg.total_size = meta.total_size;
    nb_msg.data_msg_id = meta.msg_id;
    nb_msg.receiver_id = local_node_id();
    nb_msg.sequence = meta.sequence;

    const std::size_t span = static_cast<std::size_t>(last_missing) - first_missing + 1;
    nb_msg.reset_window(first_missing, span);

    for (uint16_t i = first_missing; i <= last_missing; ++i)
    {
        if (received[i] == 0)
        {
            nb_msg.set_missing(i);   // 窗口外的页会被 set_missing 自行忽略
        }
    }

    if (frag_track_enabled().load(std::memory_order_relaxed))
    {
        FragTrackState& st = frag_track_state();
        st.nack_bitmap_sent.fetch_add(1, std::memory_order_relaxed);
        /* reset_window 会把超出单帧容量的跨度截断, 被截掉的缺片这一轮报不出去。
         * 与显式列表的 256 上限是同一类事, 一并计数。 */
        if (span > IpcRtpsNackBitmapMsg::kMaxBitmapBits)
        {
            st.nack_bitmap_truncated.fetch_add(1, std::memory_order_relaxed);
        }
    }

    ipc::buffer nb_buf = nb_msg.serialize();
    send_chunk_with_retry(node, nb_buf);
    return true;
}

/* 在两种 NACK 编码之间选择。
 *
 * 判据是**线格式字节数**, 不是文档 §8.1 写的 "缺失片数 > page_cnt/8"。后者在
 * 稀疏丢包时会误判: 713 片丢 100 片(密度 14% > 12.5%)会切到位图, 但那 100 片
 * 若散布在全域, 窗口跨度就是 713 位 = 90 B, 而显式列表只要 200 B —— 差别不大;
 * 真正的分水岭是 span/8 与 2*N 谁小, 直接算就是了, 不必用密度去近似。
 *
 * 对端不支持位图(HB 没带 kFlagBitmapNack)时无条件用显式列表。 */
void send_nack_auto(ipc::socket::UDPNode& node, const chunk_meta& meta, const std::vector<uint8_t>& received,
                    IpcRtpsNackMsg& nack_msg, IpcRtpsNackBitmapMsg& nb_msg, bool peer_supports_bitmap)
{
    if (!peer_supports_bitmap)
    {
        send_nack_for_missing(node, meta, received, nack_msg);
        return;
    }

    std::size_t miss_cnt = 0;
    uint16_t first_missing = 0;
    uint16_t last_missing = 0;
    for (uint16_t i = 1; i <= meta.page_cnt; ++i)
    {
        if (received[i] == 0)
        {
            ++miss_cnt;
            if (first_missing == 0)
            {
                first_missing = i;
            }
            last_missing = i;
        }
    }

    if (miss_cnt == 0)
    {
        return;
    }

    /* 两种编码各自"这一轮能报多少片"与"要花多少字节"。
     *
     * 判据首先是**覆盖率**, 其次才是字节数 —— 少报一片就要多等一整个
     * round_wait_ms(713 片时 200 ms), 而多花几百字节只是一次 MTU 内的传输。
     *
     * 两种编码都会截断, 且截断方式不同, 所以不能只看其中一个:
     *   显式列表: 报**任意位置**的前 256 片
     *   位图:     报 [first, first + 11496) 这个**连续窗口**内的全部缺片
     * 缺片密集时位图完胜(255 片连续空洞: 32 B vs 510 B); 但缺片稀疏地散布在
     * 一条超大消息上时, 位图窗口只能覆盖消息的前一段, 反而不如显式列表 ——
     * 例如 45000 片的消息丢了 300 片均匀散布, 位图窗口只罩得住约 77 片,
     * 显式列表能报满 256 片。 */
    const std::size_t span = static_cast<std::size_t>(last_missing) - first_missing + 1;
    const std::size_t bitmap_bits = std::min(span, IpcRtpsNackBitmapMsg::kMaxBitmapBits);
    const std::size_t bitmap_bytes = (bitmap_bits + 7) / 8;

    std::size_t bitmap_cover = miss_cnt;
    if (span > IpcRtpsNackBitmapMsg::kMaxBitmapBits)
    {
        /* 窗口装不下整个跨度, 得实际数一遍窗口内有多少片。 */
        bitmap_cover = 0;
        const std::size_t window_end = static_cast<std::size_t>(first_missing) + bitmap_bits;
        for (std::size_t i = first_missing; i < window_end && i <= meta.page_cnt; ++i)
        {
            if (received[i] == 0)
            {
                ++bitmap_cover;
            }
        }
    }

    const std::size_t list_cover = std::min(miss_cnt, IpcRtpsNackMsg::kMaxMissingPages);
    const std::size_t list_bytes = list_cover * 2;

    const bool use_bitmap =
        (bitmap_cover > list_cover) || (bitmap_cover == list_cover && bitmap_bytes < list_bytes);

    if (use_bitmap && send_nack_bitmap_for_missing(node, meta, received, nb_msg))
    {
        return;
    }
    send_nack_for_missing(node, meta, received, nack_msg);
}

ipc::buffer make_owned_copy(const void* src, std::size_t n)
{
    auto* mem = new uint8_t[n];
    std::memcpy(mem, src, n);
    return ipc::buffer(mem, n, [](void* p, std::size_t) { delete[] static_cast<uint8_t*>(p); });
}

bool recv_chunk_common(ipc::socket::UDPNode& node, ipc::socket::UDPNode* ack_node,
                       std::shared_ptr<IpcMsgBase>& msg_ptr, uint64_t tm)
{
    const auto begin = std::chrono::steady_clock::now();

    /* ack_node 为空 -> ACK/NACK 回到数据通道, 即端点分离之前的行为。
     * 这一行是"缺省参数下行为完全不变"的全部实现。 */
    ipc::socket::UDPNode& ack_out = (ack_node != nullptr) ? *ack_node : node;

    chunk_meta meta{};
    ipc::buffer first_page;
    IpcRtpsHeartbeatMsg pending_hb;
    bool have_pending_hb = false;
    if (!wait_first_data_chunk(node, msg_ptr, meta, first_page, begin, tm, pending_hb, have_pending_hb))
    {
        return false;
    }

    if (meta.page_cnt == 1)
    {
        if (first_page.size() != meta.total_size)
        {
            return false;
        }
        const uint32_t payload_crc32c = dzIPC::common::crc32c(first_page.data(), first_page.size());
        msg_ptr->deserialize(first_page);
        send_ack(ack_out, meta, payload_crc32c);
        return true;
    }

    std::vector<uint8_t> assembled(meta.total_size);
    std::vector<uint8_t> received(static_cast<std::size_t>(meta.page_cnt) + 1, 0);
    std::size_t received_cnt = 0;
    if (!place_page(first_page, meta, assembled, received, received_cnt))
    {
        return false;
    }

    /* 首片到位后重置预算时钟。否则若首片正好在 tm 末尾才到达
       （例如 200ms 等待循环刚要超时时 client 才发包），剩余 27
       片就只剩 0ms 可以收，必然组装失败 → 进入下一次
       chunk_rev_server 后才看到 chunk #2，而 chunk #1 已被消费，
       永远缺一片。*/
    const auto assembly_begin = std::chrono::steady_clock::now();

    /* 修正 1(续): 限速下把调用方的 tm 预算放宽到至少能覆盖传输时间。
     *
     * 各调用方的 tm 都是按"发送端瞬时灌完"选的常量:
     *   socket_pub_sub_ipc.cc  订阅循环   tm = 50ms
     *   socket_ser_cli_ipc.cc  ServerRevTime = 200ms
     * 而 1 MB @ 20 MB/s 光传输就要 50ms —— tm=50 的订阅端在最后一片到达的同一
     * 时刻就判超时, 必然组装失败, 这是 pubsub_socket_1048576B 握手拿不到任何
     * 探测帧的直接原因(发送端 probes ok=325, 接收端一帧都没组装成功)。
     *
     * page_cnt 只有在首片解析后才知道, 所以放宽只能做在这里。留 1.5 倍余量再
     * 加 20ms 固定量, 覆盖 sleep_for 超调、调度抖动和末片的 ACK 往返。
     * 不限速时 transit_ms 为 0, effective_tm 恒等于 tm, 行为与改动前完全一致。 */
    uint64_t effective_tm = tm;
    if (tm != ipc::invalid_value)
    {
        const uint64_t transit_ms = rate_limited_transit_ms(meta.page_cnt);
        if (transit_ms > 0)
        {
            effective_tm = std::max(tm, transit_ms + transit_ms / 2 + 20);
        }
    }

    const uint64_t round_wait_ms = calc_round_wait_ms(meta.page_cnt, effective_tm);
    IpcRtpsNackMsg nack_msg;

    /* 线格式里没有 delivery mode 标志，接收端无法知道发送端是 BestEffort 还是
     * Reliable，因此 NACK 照发（对 Reliable 有用，对 BestEffort 会被忽略）。
     *
     * 但方案 A 之后 BestEffort 发送端不再重传，如果仍固定跑满 RTPS_MAX_NACK_ROUND
     * 轮，每条丢包的消息都要空等 5 × round_wait（713 分片时 5 × 200ms = 1s）才丢弃，
     * 接收侧延迟反而比方案 A 之前更差。
     *
     * 用"连续两轮 NACK 没换回任何新分片"作为判据提前放弃：
     *   - Reliable 发送端会响应 NACK，新分片到达 → 继续重试
     *   - BestEffort 发送端不响应 → 两轮后立即放弃，不再空等
     * 容忍两轮而非一轮，是为了给 RTT 偏大的链路留一次机会。 */
    constexpr int kFruitlessRoundLimit = 2;
    int fruitless_rounds = 0;

    /* ---------------- Heartbeat 状态 ----------------
     * 收到匹配的 HB 之后, "对端是否会重传"就从猜测变成了已知事实,
     * 上面那套 fruitless 启发式随之被精确信号取代(它仍保留, 用于对端不发 HB
     * 的旧版本)。 */
    IpcRtpsHeartbeatMsg hb_msg;
    IpcRtpsNackBitmapMsg nb_msg;
    bool hb_seen = false;             // 本轮收到了与本消息匹配的 HB
    bool sender_reliable = false;     // HB 声明发送端会响应 NACK
    bool hb_final = false;            // 发送端已放弃, 不会再重传
    bool sender_bitmap_ok = false;    // HB 声明发送端认识 DZNB(位图 NACK)

    /* 前导 HB 已经在 wait_first_data_chunk 里采纳过 sequence, 这里同步一下
     * 发送端的可靠性声明, 免得第一轮还按"未知"处理。 */
    if (have_pending_hb && pending_hb.data_msg_id == meta.msg_id && pending_hb.page_cnt == meta.page_cnt
        && pending_hb.total_size == meta.total_size)
    {
        sender_reliable = (pending_hb.flags & IpcRtpsHeartbeatMsg::kFlagReliable) != 0;
        sender_bitmap_ok = (pending_hb.flags & IpcRtpsHeartbeatMsg::kFlagBitmapNack) != 0;
    }

    for (int round = 0; round < RTPS_MAX_NACK_ROUND && received_cnt < meta.page_cnt; ++round)
    {
        const std::size_t received_before_round = received_cnt;
        const auto round_begin = std::chrono::steady_clock::now();
        /* HB 到达后把本轮窗口压缩到 kHbGraceMs; 在此之前用完整的 round_wait_ms。 */
        uint64_t round_budget_ms = round_wait_ms;
        hb_seen = false;
        while (received_cnt < meta.page_cnt)
        {
            const uint64_t round_used = elapsed_ms(round_begin);
            if (round_used >= round_budget_ms)
            {
                break;
            }

            uint64_t wait_ms = round_budget_ms - round_used;
            if (effective_tm != ipc::invalid_value)
            {
                const uint64_t used = elapsed_ms(assembly_begin);
                if (used >= effective_tm)
                {
                    record_fragment_gaps(received, meta.page_cnt, false);
                    return false;
                }
                wait_ms = std::min(wait_ms, effective_tm - used);
            }

            ipc::buffer page = node.receive(wait_ms);
            if (page.empty())
            {
                break;
            }

            /* HB 必须在 check_id 之前截住 —— 它的 msg_id 是 DZHB, 过不了数据
             * 消息的类型过滤。 */
            if (hb_msg.check_hb_id(page))
            {
                hb_msg.deserialize(page);
                if (hb_msg.data_msg_id == meta.msg_id && hb_msg.page_cnt == meta.page_cnt
                    && hb_msg.total_size == meta.total_size)
                {
                    /* 权威 sequence: 数据分片的 tail 里没有这个字段, 只能从 HB 拿。
                     * 不采纳的话 ACK 会带着 0 发回去, 发送端的校验永远不匹配。 */
                    meta.sequence = hb_msg.sequence;
                    sender_reliable = (hb_msg.flags & IpcRtpsHeartbeatMsg::kFlagReliable) != 0;
                    hb_final = (hb_msg.flags & IpcRtpsHeartbeatMsg::kFlagFinal) != 0;
                    sender_bitmap_ok = (hb_msg.flags & IpcRtpsHeartbeatMsg::kFlagBitmapNack) != 0;
                    if (!hb_seen)
                    {
                        hb_seen = true;
                        /* 不立刻定论: HB 与最后几片背靠背发出, 可能被重排到它们
                         * 前面。给在途分片 kHbGraceMs 的排空时间。 */
                        const uint64_t used_now = elapsed_ms(round_begin);
                        round_budget_ms = std::min(round_budget_ms, used_now + kHbGraceMs);
                    }
                }
                continue;
            }

            if (!msg_ptr->check_id(page))
            {
                continue;
            }

            place_page(page, meta, assembled, received, received_cnt);
        }

        if (received_cnt >= meta.page_cnt)
        {
            break;
        }

        if (effective_tm != ipc::invalid_value && elapsed_ms(assembly_begin) >= effective_tm)
        {
            record_fragment_gaps(received, meta.page_cnt, false);
            return false;
        }

        /* 收到 HB 就有了确定的分片全集, 不必再靠"连续两轮无果"去猜。 */
        if (hb_seen)
        {
            if (!sender_reliable || hb_final)
            {
                /* BestEffort 不会重传, 已放弃的 Reliable 也不会。继续等只是空耗
                 * 剩余轮次(713 分片时最坏 5 × 200ms = 1s)。 */
                record_fragment_gaps(received, meta.page_cnt, false);
                return false;
            }
            send_nack_auto(ack_out, meta, received, nack_msg, nb_msg, sender_bitmap_ok);
            continue;   // 跳过 fruitless 判据, 立即进入下一轮等重传
        }

        /* 没收到 HB —— 对端可能是不发 HB 的旧版本, 退回原来的启发式。
         * round 0 收的是发送端的首轮突发，此时还没发过 NACK，不计入判据。
         * 从 round 1 起，本轮没有任何新分片就说明对端没有响应 NACK。 */
        if (round >= 1)
        {
            if (received_cnt == received_before_round)
            {
                if (++fruitless_rounds >= kFruitlessRoundLimit)
                {
                    record_fragment_gaps(received, meta.page_cnt, false);
                    return false;   // 对端不重传，放弃组装
                }
            }
            else
            {
                fruitless_rounds = 0;
            }
        }

        /* 本轮没收到 HB, 但前导 HB 可能已经声明过对端认识位图 —— sender_bitmap_ok
         * 是跨轮粘住的, 交给 send_nack_auto 判断即可。对端确实是旧版时它退回显式
         * 列表, 那是唯一能被对方解析的编码。 */
        send_nack_auto(ack_out, meta, received, nack_msg, nb_msg, sender_bitmap_ok);
    }

    if (received_cnt < meta.page_cnt)
    {
        record_fragment_gaps(received, meta.page_cnt, false);
        return false;
    }

    record_fragment_gaps(received, meta.page_cnt, true);
    const uint32_t payload_crc32c = dzIPC::common::crc32c(assembled.data(), meta.total_size);
    ipc::buffer assembled_view(assembled.data(), meta.total_size);
    msg_ptr->deserialize(assembled_view);
    send_ack(ack_out, meta, payload_crc32c);
    return true;
}
}   // namespace

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool chunk_rev_topic(std::shared_ptr<ipc::socket::UDPNode>& node, std::shared_ptr<TopicData>& rev_msg, uint64_t tm)
{
    std::shared_ptr<IpcMsgBase> msg_ptr = rev_msg->topic();
    return recv_chunk_common(*node, nullptr, msg_ptr, tm);
}

bool chunk_rev_topic(std::shared_ptr<ipc::socket::UDPNode>& node, std::shared_ptr<TopicData>& rev_msg, uint64_t tm,
                     const std::shared_ptr<ipc::socket::UDPNode>& ack_node)
{
    std::shared_ptr<IpcMsgBase> msg_ptr = rev_msg->topic();
    return recv_chunk_common(*node, ack_node ? ack_node.get() : nullptr, msg_ptr, tm);
}

bool chunk_rev_server(std::shared_ptr<ipc::socket::UDPNode>& node, std::shared_ptr<ServiceData>& rev_msg, uint64_t tm,
                      bool ser_or_cli)
{
    std::shared_ptr<IpcMsgBase> msg_ptr = ser_or_cli ? rev_msg->request() : rev_msg->response();
    return recv_chunk_common(*node, nullptr, msg_ptr, tm);
}

bool chunk_rev_server(std::shared_ptr<ipc::socket::UDPNode>& node, std::shared_ptr<ServiceData>& rev_msg, uint64_t tm,
                      bool ser_or_cli, const std::shared_ptr<ipc::socket::UDPNode>& ack_node)
{
    std::shared_ptr<IpcMsgBase> msg_ptr = ser_or_cli ? rev_msg->request() : rev_msg->response();
    return recv_chunk_common(*node, ack_node ? ack_node.get() : nullptr, msg_ptr, tm);
}

ipc::buffer chunk_rev_sniff(ipc::socket::UDPNode& node, uint64_t tm)
{
    const auto first_begin = std::chrono::steady_clock::now();

    chunk_meta meta{};
    bool have_meta = false;
    std::vector<uint8_t> assembled;
    std::vector<uint8_t> received;
    std::size_t received_cnt = 0;
    std::chrono::steady_clock::time_point assembly_begin{};
    uint64_t assembly_budget_ms = 0;
    ipc::buffer pending;

    auto begin_assembly = [&](const ipc::buffer& first_page)
    {
        assembled.assign(meta.total_size, 0);
        received.assign(static_cast<std::size_t>(meta.page_cnt) + 1, 0);
        received_cnt = 0;
        assembly_begin = std::chrono::steady_clock::now();
        // Bounded internal deadline so a stuck assembly doesn't hold the sniffer
        // thread past the next sender cycle. Tied to page_cnt so big frames get
        // proportionally more time, but capped to keep stop-responsiveness sane.
        assembly_budget_ms = std::min<uint64_t>(500, std::max<uint64_t>(50, static_cast<uint64_t>(meta.page_cnt) * 2));
        place_page(first_page, meta, assembled, received, received_cnt);
    };

    while (true)
    {
        uint64_t wait_ms;
        if (have_meta)
        {
            const uint64_t used = elapsed_ms(assembly_begin);
            if (used >= assembly_budget_ms)
            {
                return ipc::buffer{};
            }
            wait_ms = assembly_budget_ms - used;
        }
        else
        {
            if (tm == ipc::invalid_value)
            {
                wait_ms = ipc::invalid_value;
            }
            else
            {
                const uint64_t used = elapsed_ms(first_begin);
                if (used >= tm)
                {
                    return ipc::buffer{};
                }
                wait_ms = tm - used;
            }
        }

        ipc::buffer page;
        if (pending.size() > 0)
        {
            page = std::move(pending);
        }
        else
        {
            try
            {
                page = node.receive(wait_ms);
            }
            catch (...)
            {
                return ipc::buffer{};
            }
            if (page.empty())
            {
                return ipc::buffer{};
            }
        }

        ipc_tail_msg tail;
        if (!parse_tail(page, tail) || !valid_chunk_meta(tail))
        {
            continue;
        }
        /* 控制帧与数据同走一条通道, 而 sniff 路径不按 msg_id 过滤(它要嗅探任意
         * 类型的用户消息)。33 字节的 Heartbeat 恰好能通过 valid_chunk_meta 的
         * 单页校验, 不显式排除就会被当成一条合法消息输出。
         * ACK/NACK 目前因尺寸恰好不匹配而侥幸落选, 这里一并显式挡掉, 不再依赖巧合。*/
        if (is_rtps_control_frame(tail.dz_ipc_msg_id))
        {
            continue;
        }
        if (tail.now_page == 0 || tail.now_page > tail.page_cnt)
        {
            continue;
        }

        chunk_meta tentative{tail.page_cnt, tail.total_size, tail.dz_ipc_msg_id};
        if (page.size() != expected_page_size(tentative, tail.now_page))
        {
            continue;
        }

        if (!have_meta)
        {
            // Only kick off assembly on a true first page. This is what keeps us
            // from latching onto the tail of a message whose head we missed —
            // i.e., the case where the previous frame was still being verified
            // on the wire while we were already moving on to "the next packet".
            if (tail.now_page != 1)
            {
                continue;
            }
            meta = tentative;
            have_meta = true;
            if (meta.page_cnt == 1)
            {
                if (page.size() != meta.total_size)
                {
                    have_meta = false;
                    continue;
                }
                return make_owned_copy(page.data(), page.size());
            }
            begin_assembly(page);
            if (received_cnt >= meta.page_cnt)
            {
                return make_owned_copy(assembled.data(), meta.total_size);
            }
            continue;
        }

        if (tail.dz_ipc_msg_id == meta.msg_id && tail.page_cnt == meta.page_cnt && tail.total_size == meta.total_size)
        {
            place_page(page, meta, assembled, received, received_cnt);
            if (received_cnt >= meta.page_cnt)
            {
                return make_owned_copy(assembled.data(), meta.total_size);
            }
            continue;
        }

        // A chunk with a different msg_id arrived mid-assembly — the sender has
        // already moved on, so the in-flight frame is unrecoverable.
        if (tail.now_page == 1)
        {
            // Fresh first page of a new message: discard the old frame and
            // restart cleanly on this one.
            meta = tentative;
            if (meta.page_cnt == 1)
            {
                if (page.size() != meta.total_size)
                {
                    have_meta = false;
                    continue;
                }
                return make_owned_copy(page.data(), page.size());
            }
            begin_assembly(page);
            if (received_cnt >= meta.page_cnt)
            {
                return make_owned_copy(assembled.data(), meta.total_size);
            }
            continue;
        }
        // Mid-page of a message whose start we never saw — we missed the boat.
        // Drop and let the caller try again from a clean state.
        return ipc::buffer{};
    }
}

SocketSendReport chunk_send_ex(std::shared_ptr<ipc::socket::UDPNode>& node, ipc::buffer& publish_data,
                               const SocketSendOptions& options)
{
    SocketSendReport report;
    report.sequence = msg_sequence_counter().fetch_add(1, std::memory_order_relaxed);

    if (publish_data.empty() || publish_data.size() < TAIL_SIZE)
    {
        report.status = SocketSendStatus::FailedInvalidArgument;
        return report;
    }

    std::vector<ipc::buffer> chunks;
    chunks.reserve((publish_data.size() + UDP_MAX_SIZE - 1) / UDP_MAX_SIZE);
    for (std::size_t offset = 0; offset < publish_data.size(); offset += UDP_MAX_SIZE)
    {
        const std::size_t sz = std::min(UDP_MAX_SIZE, publish_data.size() - offset);
        chunks.emplace_back(static_cast<uint8_t*>(publish_data.data()) + offset, sz);
    }

    for (std::size_t i = 0; i < chunks.size(); ++i)
    {
        if (chunks[i].size() < TAIL_SIZE)
        {
            report.status = SocketSendStatus::FailedInvalidArgument;
            return report;
        }
        write_now_page(chunks[i], static_cast<uint16_t>(i + 1));
    }

    /* CRC 必须在 write_now_page **之后**算。
     *
     * chunks 是 publish_data 的非拥有视图(上面用 buffer(ptr, size) 构造), 所以
     * write_now_page 是就地改写 publish_data 里每片 tail 的 now_page 字段。
     * 而且它确实会改动字节: IpcMsgBase::adapt_memcpy_tos 是先 ++page 再
     * add_tail_msg, 于是 serialize() 写出来的页号是 [2,3,...,N,N] —— 差一,
     * write_now_page 的职责正是纠正成 [1,2,...,N]。
     *
     * 接收端 place_page 把整片(含 tail)原样拼进 assembled, CRC 的是线上真实
     * 字节。若在纠正前算 CRC, 两端必然在每片 2 个字节上不同(N 片差 N-1 处),
     * Reliable+CRC32C 的多页消息 100% 报 FailedIntegrity。
     *
     * 这个错位在端点分离之前不可见 —— ACK 根本到不了发送端, 代码在比较 CRC
     * 之前就 FailedTimeout 返回了。单页消息也不受影响(page 从未自增, 无需纠正),
     * 所以 test_socket_reliable_crc 一直是绿的。 */
    if (options.integrity == SocketIntegrityMode::CRC32C)
    {
        report.crc32c = dzIPC::common::crc32c(publish_data.data(), publish_data.size());
    }

    ipc_tail_msg first_tail;
    if (!parse_tail(chunks.front(), first_tail) || !valid_chunk_meta(first_tail))
    {
        report.status = SocketSendStatus::FailedInvalidArgument;
        return report;
    }
    if (static_cast<std::size_t>(first_tail.page_cnt) != chunks.size())
    {
        report.status = SocketSendStatus::FailedInvalidArgument;
        return report;
    }

    /* 端点分离: 在 ack_node 上等确认, 而不是在刚发完几百个分片的数据 socket 上。
     * 为空则退回数据节点 —— 历史行为。 */
    ipc::socket::UDPNode& data_out = *node;
    ipc::socket::UDPNode& ack_in = options.ack_node ? *options.ack_node : *node;

    /* 入口排空：清掉上一轮 chunk_send 退出时还没来得及到达 / 处理的残留包
       （晚到的 ACK/NACK 或 self-loopback 数据片）。否则与本轮发出的包混杂后，
       由于多轮共用同一份 meta，下面 ACK 等待循环会拿旧 ACK 当本轮 ACK 用。

       端点分离后数据通道已是 SendOnly(receive_nowait 直接返回空), 这里排空的
       实际是 ack 通道上的迟到 ACK —— 仍然必要。 */
    drain_self_loopback(ack_in);

    const chunk_meta meta{first_tail.page_cnt, first_tail.total_size, first_tail.dz_ipc_msg_id, report.sequence};
    const bool reliable = (options.delivery == SocketDeliveryMode::Reliable);

    /* 前导 HB: 数据之前先发, 让接收端在**首片到达时**就知道 sequence。
     *
     * 单页消息只有这一次机会 —— recv 端收到首片立刻 ACK 并返回, 不会再有第二轮
     * 去等 HB。多页消息虽然事后补发的 HB 也能赶上, 但前导 HB 让它第一轮就拿到
     * 正确的 sequence, 少一次无效 ACK。 */
    if (options.send_heartbeat && reliable)
    {
        send_heartbeat(data_out, meta, /*reliable=*/true, /*final_hb=*/false, /*round=*/0);
    }

    /* options 未显式指定时回退到全局设置 —— pub-sub 的 publish 接口不暴露
     * SocketSendOptions, 只能靠进程级开关生效。 */
    const std::size_t rate_limit =
        (options.rate_limit_bps != 0) ? options.rate_limit_bps : global_rate_limit_bps().load(std::memory_order_relaxed);
    if (!send_all_chunks(data_out, chunks, rate_limit))
    {
        report.status = SocketSendStatus::FailedLocalSend;
        return report;
    }

    /* 完成通告。Reliable 无论几页都发(sequence 必须送达); BestEffort 只在分片
     * 数够多时才发, 见 kBestEffortHeartbeatMinPages 的权衡说明。 */
    const bool want_completion_hb = reliable || (meta.page_cnt >= kBestEffortHeartbeatMinPages);
    if (options.send_heartbeat && want_completion_hb)
    {
        send_heartbeat(data_out, meta, reliable, /*final_hb=*/!reliable, /*round=*/1);
    }

    /* 方案 A：BestEffort 真正 fire-and-forget。
     *
     * 之前的实现只对单包免等 ACK，多分片强制走 Reliable 路径（等 ACK/NACK），
     * 导致 payload > 1472 B 时性能塌到 195 msg/s（4 KB）甚至完全失败（1 MB）。
     * BestEffort 的语义是"可以丢、不等确认"，发送端不应该为大包改变这个契约。
     *
     * 现在：无论多少分片，BestEffort 都不等 ACK 立即返回。接收端仍会组装分片，
     * 超时丢弃不完整的消息（见 recv_chunk_common）。对同机 loopback（丢包率
     * 几乎为 0）和低丢包链路（< 1%），这消除了 1472 B 的性能断崖；对高丢包链路，
     * 用户应该用 Reliable 而不是 BestEffort。 */
    if (options.delivery == SocketDeliveryMode::BestEffort)
    {
        drain_self_loopback(ack_in);
        report.status = SocketSendStatus::SentUnconfirmed;
        return report;
    }

    const uint64_t nack_wait_ms = calc_nack_wait_ms(meta.page_cnt);
    /* 方案 B': 首轮 ACK 等待由实测 RTT 决定, 而非固定 ACK_FAST_WAIT_MS。
     * key 恒取数据节点 —— RTT 是链路属性, 若一处用 node、另一处用 ack_node,
     * 估计器会被拆成两份各自样本不足的状态, 症状是偶发超时而非崩溃, 极难查。 */
    const ipc::socket::UDPNode* const rtt_key = node.get();
    uint64_t first_wait_ms = ack_first_wait_ms(rtt_key);

    /* 接收端的 ACK 里包含了组装 page_cnt 个分片 + 对整条消息做 CRC32C 的时间,
     * 这部分是 CPU 开销, 不是链路 RTT。端点分离之后 RTT 估计会真正收敛到
     * loopback 的百微秒量级, 不补这一项的话大包首轮必然误判超时。 */
    first_wait_ms += static_cast<uint64_t>(meta.page_cnt) / 64;

    /* 修正 1(续 2): 首轮 ACK 窗口必须覆盖限速下的传输时间。
     *
     * ack_first_wait_ms 推的是 RTT —— "包发出去到对端回话"的往返时间, 同机
     * loopback 下收敛到 ~200us 下限。这个推导隐含了"最后一片几乎和第一片同时
     * 到达"的前提, 限速后不再成立: 接收端要等 transit_ms 才收到最后一片, ACK
     * 不可能早于此发出。
     *
     * 于是 200us 窗口必然超时 -> 发送端 NACK 重传整批 -> 接收端此时刚收全并回
     * ACK, 但发送端已进入下一轮重传, 双方永远错位, 表现为 "Failed to send"。
     * 这就是 sercli_socket 三个尺寸(含限速前本来通过的 65536B)全部失败的原因。
     *
     * ACK 到达时刻 ≈ transit_ms + RTT, 所以窗口取两者之和再留半倍余量。 */
    const uint64_t send_transit_ms = rate_limited_transit_ms(meta.page_cnt);
    if (send_transit_ms > 0)
    {
        first_wait_ms += send_transit_ms + send_transit_ms / 2;
    }

    const uint64_t ack_timeout_ms =
        (options.ack_timeout_ms == ipc::invalid_value)
            ? (first_wait_ms + nack_wait_ms * static_cast<uint64_t>(RTPS_MAX_NACK_ROUND))
            : options.ack_timeout_ms;
    IpcRtpsNackMsg nack_msg;
    IpcRtpsNackBitmapMsg nb_msg;
    IpcRtpsAckMsg ack_msg;

    const auto ack_begin = std::chrono::steady_clock::now();
    int round = 0;
    int hb_repeat = 0;   // 静默轮补发 HB 的次数, 上限 kMaxHeartbeatRepeat
    while (true)
    {
        bool got_nack = false;
        bool got_ack = false;
        std::unordered_set<uint16_t> missing_union;
        const auto round_begin = std::chrono::steady_clock::now();
        const uint64_t round_wait_ms = (round == 0) ? first_wait_ms : nack_wait_ms;

        while (true)
        {
            if (options.delivery == SocketDeliveryMode::Reliable && elapsed_ms(ack_begin) >= ack_timeout_ms)
            {
                drain_self_loopback(ack_in);
                report.status = SocketSendStatus::FailedTimeout;
                return report;
            }

            const uint64_t used = elapsed_ms(round_begin);
            if (used >= round_wait_ms)
            {
                break;
            }
            uint64_t wait_ms = round_wait_ms - used;
            if (options.delivery == SocketDeliveryMode::Reliable)
            {
                const uint64_t total_used = elapsed_ms(ack_begin);
                if (total_used >= ack_timeout_ms)
                {
                    drain_self_loopback(ack_in);
                    report.status = SocketSendStatus::FailedTimeout;
                    return report;
                }
                wait_ms = std::min(wait_ms, ack_timeout_ms - total_used);
            }

            ipc::buffer recv_buf = ack_in.receive(wait_ms);
            if (recv_buf.empty())
            {
                break;
            }
            if (ack_msg.check_ak_id(recv_buf))
            {
                ack_msg.deserialize(recv_buf);
                if (ack_msg.page_cnt == meta.page_cnt && ack_msg.total_size == meta.total_size
                    && ack_msg.data_msg_id == meta.msg_id && ack_msg.sequence == meta.sequence)
                {
                    got_ack = true;
                    report.ack_crc32c = ack_msg.payload_crc32c;
                    /* 方案 B': 只用首轮的 ACK 更新 RTT 估计。后续轮次的 ACK 前面
                     * 夹了 NACK 重传, 测到的不是链路 RTT (Karn 算法: 重传过的
                     * 样本必须丢弃, 否则超时会被自身的重传延迟推高而失控)。 */
                    if (round == 0)
                    {
                        const auto rtt = std::chrono::duration_cast<std::chrono::microseconds>(
                                             std::chrono::steady_clock::now() - ack_begin)
                                             .count();
                        if (rtt > 0)
                        {
                            rtt_of(rtt_key).observe(static_cast<uint64_t>(rtt));
                        }
                    }
                    break;
                }
                continue;
            }
            if (!nack_msg.check_id(recv_buf))
            {
                /* 位图 NACK。与显式列表汇入同一个 missing_union, 下面的重传逻辑
                 * 不必关心接收端用了哪种编码。 */
                if (!nb_msg.check_nb_id(recv_buf))
                {
                    continue;
                }
                nb_msg.deserialize(recv_buf);
                if (nb_msg.page_cnt != meta.page_cnt || nb_msg.total_size != meta.total_size
                    || nb_msg.data_msg_id != meta.msg_id || nb_msg.sequence != meta.sequence)
                {
                    continue;
                }
                got_nack = true;
                for (std::size_t bit = 0; bit < nb_msg.bit_cnt; ++bit)
                {
                    const std::size_t page_id = static_cast<std::size_t>(nb_msg.base_page) + bit;
                    if (page_id > chunks.size())
                    {
                        break;   // 窗口尾部的填充位, 越界即可停
                    }
                    if (page_id > 0 && nb_msg.is_missing(static_cast<uint16_t>(page_id)))
                    {
                        missing_union.insert(static_cast<uint16_t>(page_id));
                    }
                }
                continue;
            }

            nack_msg.deserialize(recv_buf);
            if (nack_msg.page_cnt != meta.page_cnt || nack_msg.total_size != meta.total_size
                || nack_msg.data_msg_id != meta.msg_id || nack_msg.sequence != meta.sequence)
            {
                continue;
            }

            got_nack = true;
            for (uint16_t page_id : nack_msg.missing_pages)
            {
                if (page_id > 0 && page_id <= chunks.size())
                {
                    missing_union.insert(page_id);
                }
            }
        }

        if (got_ack)
        {
            /* ACK 命中后立刻 return 会把队列里剩余的 self-loopback 数据片留给下一轮，
               下一轮会把它们当作本轮 self-loopback 误处理（meta 完全相同）。 */
            drain_self_loopback(ack_in);
            if (options.integrity == SocketIntegrityMode::CRC32C
                && ((ack_msg.integrity_flags & INTEGRITY_FLAG_CRC32C) == 0 || report.ack_crc32c != report.crc32c))
            {
                report.status = SocketSendStatus::FailedIntegrity;
                return report;
            }
            report.status = SocketSendStatus::DeliveredAcked;
            return report;
        }

        if (!got_nack || missing_union.empty())
        {
            if (options.delivery == SocketDeliveryMode::BestEffort)
            {
                break;
            }
            /* 这一轮既没 ACK 也没 NACK。可能是完成通告本身丢了, 也可能接收端压根
             * 没看到最后一片、还没开始组装。补发 HB 把它推进到能做判断的状态,
             * 比干等下一轮超时有效。 */
            if (options.send_heartbeat && hb_repeat < kMaxHeartbeatRepeat)
            {
                ++hb_repeat;
                send_heartbeat(data_out, meta, /*reliable=*/true, /*final_hb=*/false,
                               static_cast<uint16_t>(round + 2));
            }
            ++round;
            continue;
        }

        /* 重传批次也要限速。
         *
         * 位图 NACK 之前, 一轮最多重传 256 片(显式列表上限), 不限速也就 377 KB
         * 的突发。位图把上限提到 11496 位, 713 片的消息现在可以一轮全报 —— 若仍
         * 无节制地灌回去, 就会把当初造成丢包的那个接收缓冲(§7.1 的溢出型突发)
         * 再撑爆一次, 重传本身变成下一轮丢包的成因。
         *
         * 复用首轮的 rate_limit: 同一条链路, 首轮能承受的速率重传也能承受。
         * 用相对 resend_start 的绝对时间表, 理由同 send_all_chunks —— 增量式
         * sleep 的超调会逐片累加。 */
        std::size_t resent = 0;
        std::size_t resent_bytes = 0;
        const auto resend_start = std::chrono::steady_clock::now();
        for (uint16_t miss : missing_union)
        {
            if (rate_limit > 0 && resent > 0)
            {
                const auto now = std::chrono::steady_clock::now();
                const std::uint64_t elapsed_us =
                    std::chrono::duration_cast<std::chrono::microseconds>(now - resend_start).count();
                const std::uint64_t budget_us = (resent_bytes * 1'000'000ULL) / rate_limit;
                if (budget_us > elapsed_us)
                {
                    std::this_thread::sleep_for(std::chrono::microseconds(budget_us - elapsed_us));
                }
            }
            resent_bytes += chunks[miss - 1].size();
            if (!send_chunk_with_retry(data_out, chunks[miss - 1]))
            {
                report.status = SocketSendStatus::FailedLocalSend;
                return report;
            }
            if ((++resent % SEND_BURST_BEFORE_YIELD) == 0 && resent < missing_union.size())
            {
                std::this_thread::yield();
            }
        }
        /* 重传批次之后补一帧 HB: 接收端据此知道这批补片已经发完, 可以立刻判断
         * 还缺不缺, 而不必等满一个 round_wait_ms。 */
        if (options.send_heartbeat)
        {
            send_heartbeat(data_out, meta, /*reliable=*/true, /*final_hb=*/false, static_cast<uint16_t>(round + 2));
        }
        ++round;

        if (options.delivery == SocketDeliveryMode::BestEffort && round >= RTPS_MAX_NACK_ROUND)
        {
            break;
        }
    }

    /* 所有 ACK round 走完仍未拿到 ACK（典型 5ms 静默退出）。本轮发出去的
       self-loopback 数据片此时还在队列里，必须清掉再返回。 */
    /* 终止通告: 告诉还在组装的接收端"我放弃了, 别再等重传"。带 Final 而不带
     * Reliable —— 接收端看到就会立即丢弃, 不再空耗自己剩余的轮次。 */
    if (options.send_heartbeat && options.delivery == SocketDeliveryMode::Reliable)
    {
        send_heartbeat(data_out, meta, /*reliable=*/false, /*final_hb=*/true, static_cast<uint16_t>(round + 2));
    }
    drain_self_loopback(ack_in);
    report.status = (options.delivery == SocketDeliveryMode::Reliable) ? SocketSendStatus::FailedTimeout
                                                                        : SocketSendStatus::SentUnconfirmed;
    return report;
}

bool chunk_send(std::shared_ptr<ipc::socket::UDPNode>& node, ipc::buffer& publish_data)
{
    SocketSendOptions options;
    options.delivery = SocketDeliveryMode::BestEffort;
    options.integrity = SocketIntegrityMode::None;
    return chunk_send_ex(node, publish_data, options).ok();
}

bool chunk_send_reliable(std::shared_ptr<ipc::socket::UDPNode>& node, ipc::buffer& publish_data,
                         uint64_t ack_timeout_ms)
{
    /* 单节点版本 —— 在发数据的同一条 socket 上等 ACK。
     *
     * 这个前提在组播 + IP_MULTICAST_LOOP=1 下不成立: 自己发出的分片会全部回绕进
     * 自己的接收队列(1 MB = 713 片), 对端的 ACK 排在它们后面, 等待窗口必然先超时。
     * 所以本重载对多分片消息基本注定 FailedTimeout, 只适合单分片, 或有独立反向
     * 通道的传输(如单播 TCP/UDP)。
     *
     * 要在组播上真正用 Reliable, 走下面那个带 ack_node 的重载: 传一条独立的
     * RecvOnly socket, ACK 从那里收, 那上面没有自己的数据回绕。
     * 接线方式参见 socket_pub_ipc::publish_blocking()。 */
    SocketSendOptions options;
    options.delivery = SocketDeliveryMode::Reliable;
    /* 多分片消息启用 CRC32C: 分片重组后校验整体完整性。单分片时开销可忽略,
     * 多分片时能挡住"分片都到齐但内容错位"这类静默损坏。 */
    options.integrity = SocketIntegrityMode::CRC32C;
    options.ack_timeout_ms = ack_timeout_ms;
    return chunk_send_ex(node, publish_data, options).ok();
}

bool chunk_send_reliable(std::shared_ptr<ipc::socket::UDPNode>& node, ipc::buffer& publish_data,
                         const std::shared_ptr<ipc::socket::UDPNode>& ack_node, uint64_t ack_timeout_ms)
{
    SocketSendOptions options;
    options.delivery = SocketDeliveryMode::Reliable;
    options.integrity = SocketIntegrityMode::CRC32C;
    options.ack_timeout_ms = ack_timeout_ms;
    options.ack_node = ack_node;
    return chunk_send_ex(node, publish_data, options).ok();
}

/* ---------------- 分片丢失诊断 API (阶段 0) ---------------- */

void set_fragment_loss_tracking(bool enabled)
{
    frag_track_enabled().store(enabled, std::memory_order_relaxed);
}

bool fragment_loss_tracking_enabled()
{
    return frag_track_enabled().load(std::memory_order_relaxed);
}

FragmentLossStats get_fragment_loss_stats()
{
    const FragTrackState& st = frag_track_state();
    FragmentLossStats out;
    for (std::size_t i = 0; i < FragmentLossStats::kGapBuckets; ++i)
    {
        out.gap_hist[i] = st.gap_hist[i].load(std::memory_order_relaxed);
    }
    out.messages_complete = st.messages_complete.load(std::memory_order_relaxed);
    out.messages_incomplete = st.messages_incomplete.load(std::memory_order_relaxed);
    out.fragments_expected = st.fragments_expected.load(std::memory_order_relaxed);
    out.fragments_missing = st.fragments_missing.load(std::memory_order_relaxed);
    out.gaps_total = st.gaps_total.load(std::memory_order_relaxed);
    out.gap_max = st.gap_max.load(std::memory_order_relaxed);
    out.nack_explicit_sent = st.nack_explicit_sent.load(std::memory_order_relaxed);
    out.nack_bitmap_sent = st.nack_bitmap_sent.load(std::memory_order_relaxed);
    out.nack_explicit_truncated = st.nack_explicit_truncated.load(std::memory_order_relaxed);
    out.nack_bitmap_truncated = st.nack_bitmap_truncated.load(std::memory_order_relaxed);
    return out;
}

void reset_fragment_loss_stats()
{
    FragTrackState& st = frag_track_state();
    for (std::size_t i = 0; i < FragmentLossStats::kGapBuckets; ++i)
    {
        st.gap_hist[i].store(0, std::memory_order_relaxed);
    }
    st.messages_complete.store(0, std::memory_order_relaxed);
    st.messages_incomplete.store(0, std::memory_order_relaxed);
    st.fragments_expected.store(0, std::memory_order_relaxed);
    st.fragments_missing.store(0, std::memory_order_relaxed);
    st.gaps_total.store(0, std::memory_order_relaxed);
    st.gap_max.store(0, std::memory_order_relaxed);
    st.nack_explicit_sent.store(0, std::memory_order_relaxed);
    st.nack_bitmap_sent.store(0, std::memory_order_relaxed);
    st.nack_explicit_truncated.store(0, std::memory_order_relaxed);
    st.nack_bitmap_truncated.store(0, std::memory_order_relaxed);
}

/* ---------------- 发送节流全局配置 (阶段 1) ---------------- */

void set_socket_rate_limit_bps(std::size_t bps)
{
    global_rate_limit_bps().store(bps, std::memory_order_relaxed);
}

std::size_t get_socket_rate_limit_bps()
{
    return global_rate_limit_bps().load(std::memory_order_relaxed);
}
}   // namespace socket
}   // namespace dzIPC

#include "dzIPC/common/data_rev.h"
#include <algorithm>
#include <cassert>
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

/* ---------------- 闭环自适应限速 (段3 任务2b 起, 段4 方案E) 常量 ----------------
 *
 * v1 初值, 全部标注「待 tester 标定」(reviewer 方案 §7 参数表)。段4 方案E 移除
 * 了二值跨度判据 kOverflowSpanPages (被连续丢包分数 F 取代) 与 ×1.5 增倍
 * (被加性升 +Δ 取代), 本块同步更新:
 *   kCleanMessagesPerIncrement  连续 K 条干净 (F==0 且页数够大) 消息后加性升
 *                         +Δ (Δ = initial/16)。K=3 ≈ 3 条消息恢复。
 *   kRateFloorDivisor     静态下限 = initial/32 (5 次减半 = 部署意图的 1/32,
 *                         相对值, 与部署速度解耦)。
 *   kLeaseTimeoutMultiple  liveness 租期 = 3 × 本条消息 ack_timeout: 1× 消息
 *                         预算 + 1× last_ack_ts 最坏滞后 + 1× 抖动余量。
 *   kMinPagesForRateSignal 段4 方案E: F==0 计入 clean 的最小页数。单页/小消息
 *                         没压到接收缓冲, F=0 是弱证据 ("链路有余量"不可信) ——
 *                         全小消息负载下若小消息的 F=0 也计 clean, 速率会靠弱
 *                         证据一路爬到 initial, 然后一条大消息就溢出。不对称
 *                         规则 (leader 裁定): 拥塞证据永远采信 (F>0 小消息也
 *                         降速), 余量证据要求消息够大; 小消息 F==0 既不计
 *                         clean 也不清零 clean_count (中性, 跳过)。 */
constexpr uint32_t kCleanMessagesPerIncrement = 3;
constexpr uint32_t kRateFloorDivisor = 32;
constexpr uint32_t kLeaseTimeoutMultiple = 3;
constexpr uint16_t kMinPagesForRateSignal = 8;
/* ---- 段5 R1 新增 (推荐档 T=1 / Δ=initial/320) ---- */
constexpr uint32_t kRateRecoverAlpha = 1;           // α 门阈值 (alpha_scaled 单位 = α×256; 1 ⇒ α≤0.39%)
constexpr uint32_t kRateRecoverAlphaSentinel = 256; // 回滚哨兵: =256 退回旧 3-clean 门 (可取值远小于 256, 不冲突)
constexpr uint32_t kRateRecoverStepDivisor = 320;   // 升步长 = initial / 320 (替代 initial/16)
/* ---- 段5 任务2h (方案3 前置): runs 判别信号采集 (纯诊断, 不进 bps= 赋值) ----
 * 回滚开关沿方案2/变更集 v2 哨兵惯用法: 判据只留一处 (kRatePlan3RunsOn <
 * kRatePlan3Sentinel), 关闭 = kRatePlan3RunsOn 改 4096 → kRunsCaptureEnabled
 * 折叠 false, 采集全旁路, 运行行为与落笔前逐字相同 (只加眼睛不动手脚)。
 * 判别器本体 (阈值 + 判别落点) 见段5 任务2j, 于下方与 drop 分支落笔。 */
constexpr uint32_t kRatePlan3RunsOn   = 1;     // 采集开关 (1=开; ≥哨兵=关)
constexpr uint32_t kRatePlan3Sentinel = 4096;  // 回滚哨兵
const bool kRunsCaptureEnabled = (kRatePlan3RunsOn < kRatePlan3Sentinel);  // 判据只此一处

/* ---- 段5 任务2j (P-3 判别器本体): runs/lost 判拥塞 vs 随机阈值 (O.3 放行) ----
 * tester 实测标定 (seg5_task3g_runs_calibration): 标准(真拥塞) p90=0.158 <
 * 1% 注入(真随机) p10=1.000, 阈值 = 间隙中点 (0.158+1.000)/2 = 0.579。三态单调
 * (饿死 0.002 < 标准 0.07 < 1% 1.0)。runs/lost > 0.579 判真随机, ≤0.579 判真拥塞。
 * 定点 ×1000 交叉相乘避免除法 (同 T.1 惯用法): ratio>0.579 ⇔ runs×1000 > 579×lost。
 * 单阈值 (O.3): 两常量取同值, ⛔ 不得自设迟滞 band (自设 = 自行改判据)。 */
constexpr uint32_t kRatePlan3RunsCongest = 579;  // 0.579×1000 (判拥塞侧; 缺数据保守按拥塞)
constexpr uint32_t kRatePlan3RunsRandom  = 579;  // 0.579×1000 (判随机侧; 与 Congest 同值)
static_assert(kRatePlan3RunsCongest == kRatePlan3RunsRandom,
              "P-3 单阈值 (O.3): 两常量必须同值, 禁止迟滞 band");

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
    /* sender 侧 (跨轮 NACK 抑制, 阶段 3): 上面 4 个 NACK 计数全在接收端
     * (send_nack_*), 量不到发送端抑制的效果。nack_suppressed = 因 K=1 抑制
     * 窗口被压掉的重传页数 (抑制生效的直接证据); pages_retransmitted_again
     * = 同一条消息内被重传 ≥2 次的页数 (无抑制基线 > 0, 抑制后应趋近 0)。
     * 采集点在 chunk_send_ex 的重传批。 */
    std::atomic<std::uint64_t> nack_suppressed{0};
    std::atomic<std::uint64_t> pages_retransmitted_again{0};
    /* 闭环自适应限速 (段3 任务2b) 计数。自适应**功能**本身不受门控 (它是
     * 功能不是诊断), 这些计数才受 frag_track_enabled() 门控 (与现有 6 个
     * 同模式) —— 报告时别把"计数为 0"说成"功能未生效"。 */
    std::atomic<std::uint64_t> rate_reductions{0};
    std::atomic<std::uint64_t> rate_increments{0};
    std::atomic<std::uint64_t> rate_floor_warns{0};
    std::atomic<std::uint64_t> retransmit_bytes{0};
    std::atomic<std::size_t> rate_bps_now{0};   // 当前生效速率快照, 供 rate trace 采样
    std::atomic<std::size_t> observed_bps_now{0};   // 最近 DZA2 的接收观测速率 (方案E, 仅诊断)
    /* 段5 任务2h: runs 判别信号快照 (方案3 前置, 仅诊断不进 bps= 赋值)。 */
    std::atomic<std::size_t> runs_now{0};   // 最近消息 runs 首捕获 (T.3+T.1)
    std::atomic<std::size_t> lost_now{0};   // 同一 pattern 缺页数
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

/* 就地改写调用方 buffer 里本片的 now_page 字段 (tail 的 [n-10, n-9] 两字节)。
 *
 * 三件事必须说清:
 * ① 这里纠正的差一是 serialize() 的固有行为: IpcMsgBase::adapt_memcpy_tos
 *    先 ++page 再 add_tail_msg (ipc_msg_base.hpp:114-116), 于是 serialize()
 *    产出的页号序列是 [2,3,...,N,N] —— 页 1 从不出现, 页 N 出现两次
 *    (tester 实测 N=6 时为 [2,3,4,5,6,6])。本函数把它纠正成 [1..N]。
 * ② 隐式契约: chunk 是调用方 buffer 的非拥有视图 (buffer(ptr, size)),
 *    本函数就地改写, 调用方 buffer 之后就是线上字节。
 * ③ CRC 必须在本函数**之后**算: 接收端把整片(含 tail)原样拼进 assembled 后
 *    才做 CRC (§7.5 的教训), 若在纠正前算, 两端必然在每片 2 字节上不同。 */
void write_now_page(ipc::buffer& chunk, uint16_t now_page)
{
    if (chunk.size() < TAIL_SIZE)
    {
        return;
    }
    auto* p = static_cast<uint8_t*>(chunk.data());
    const std::size_t n = chunk.size();
    const uint16_t page_cnt = static_cast<uint16_t>(p[n - 12]) << 8 | static_cast<uint16_t>(p[n - 11]);
    /* 防御断言: 纠正的目标页号必须落在 [1, page_cnt]。差一是 serialize() 的
     * 固有行为, 本函数正是它的纠正点 —— 若传入的 now_page 越界, 是调用方
     * 传错了页号, 越早炸越容易定位 (Release 下编译为 no-op, 零功能变化)。 */
    assert(now_page >= 1 && now_page <= page_cnt);
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
 * bps 参数 (段3 任务2b): 发送端传**当前自适应速率** —— 若恒读全局, 自适应把
 * 速率降到全局以下后, 发送端的两处窗口 (calc_nack_wait_ms / first_wait 的
 * transit 项) 按全局速率算 < 实际传输时间 → 首轮必然提前超时, 整批误重传,
 * 重传又撑爆缓冲 → 降速本身制造丢包。接收端 (:331/:1000) 不传, 保持读全局
 * (它不知道发送端进程的自适应状态, 且它的 effective_tm 物理约束恰是
 * window_floor 的由来)。
 *
 * bps == 0 时读全局。返回 0 表示不限速, 调用方按原逻辑走。 */
uint64_t rate_limited_transit_ms(uint16_t page_cnt, std::size_t bps = 0)
{
    if (bps == 0)
    {
        bps = global_rate_limit_bps().load(std::memory_order_relaxed);
    }
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

uint64_t calc_nack_wait_ms(uint16_t page_cnt, std::size_t bps = 0)
{
    const uint64_t by_pages = std::max<uint64_t>(20, std::min<uint64_t>(200, static_cast<uint64_t>(page_cnt) * 4));
    /* 同修正 1: NACK 重传的等待窗口也要覆盖限速传输时间。
     * bps (段3 任务2b): 发送端传当前自适应速率, 见 rate_limited_transit_ms。 */
    const uint64_t transit_ms = rate_limited_transit_ms(page_cnt, bps);
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

/* ---------------- 对端身份表 (阶段 2, DECISIONS.md D-1 选项 B) ----------------
 *
 * 发送端按 receiver_id 记录每个对端(订阅者)的确认进度 —— 段3 跨轮 NACK 抑制
 * 与 WHC 流控的前置数据。字段按 D-1 裁定: 只有最高确认序号 + 两个时间戳,
 * 不做 locator / inline QoS / 请求变更表。ack_count 是唯一例外: 任务验收
 * 要求可观测出口导出"确认数", 不存则无法导出。 */

struct PeerState
{
    uint32_t highest_acked_seq{0};
    std::chrono::steady_clock::time_point last_ack_ts{};
    std::chrono::steady_clock::time_point last_nack_ts{};
    uint64_t ack_count{0};
};

/* 每个 UDPNode 一份对端确认表 —— 发送链路属性, 同一节点上的所有消息共享。
 * 组播 1:N 下, 每个订阅者各占一条 receiver_id 条目。
 * mtx: 保护本表。同 rtt_of 的锁粒度结论 —— ACK/NACK 每条消息仅几个, 一个
 * mutex 的开销可忽略; 但它还要兜底"同一节点被并发发送"的场景。 */
struct PeerTable
{
    std::mutex mtx;
    std::unordered_map<uint32_t, PeerState> by_receiver;
};

/* 复刻 rtt_of 的查找范式 (见上): static mutex + unordered_map, key = node.get(),
 * 进程生命周期, 条目不回收 (节点数是有限的 topic 数, 不会无界增长)。 */
PeerTable& peers_of(const ipc::socket::UDPNode* node)
{
    static std::mutex map_mtx;
    static std::unordered_map<const ipc::socket::UDPNode*, std::unique_ptr<PeerTable>> map;
    std::lock_guard<std::mutex> lock(map_mtx);
    auto it = map.find(node);
    if (it == map.end())
    {
        it = map.emplace(node, std::make_unique<PeerTable>()).first;
    }
    return *it->second;
}

void record_peer_ack(const ipc::socket::UDPNode* node, uint32_t receiver_id, uint32_t sequence)
{
    PeerTable& table = peers_of(node);
    std::lock_guard<std::mutex> lock(table.mtx);
    PeerState& peer = table.by_receiver[receiver_id];
    if (sequence > peer.highest_acked_seq)
    {
        peer.highest_acked_seq = sequence;
    }
    peer.last_ack_ts = std::chrono::steady_clock::now();
    ++peer.ack_count;
}

void record_peer_nack(const ipc::socket::UDPNode* node, uint32_t receiver_id)
{
    PeerTable& table = peers_of(node);
    std::lock_guard<std::mutex> lock(table.mtx);
    table.by_receiver[receiver_id].last_nack_ts = std::chrono::steady_clock::now();
}

/* 已知 peer 判定 + 条目计数 —— 段3 跨轮 NACK 抑制专用 (见 chunk_send_ex 的
 * 抑制注释块):
 *   - peer_is_known: NACK 的 receiver_id 在表里首见 (新订阅者中途加入) 时,
 *     本轮整体跳过抑制、立即补发, 防止它被"最近发过的页"饿一轮。
 *   - peer_count: 单 peer 场景 (条目 < 2) 抑制净收益为负 (reviewer P5:
 *     Case B 无害、Case D 纯亏 1 轮), 关闭抑制, 由 tester 场景 B 验证。
 * 两个都是只读查询, 不扩展 D-1 的表字段。 */
bool peer_is_known(const ipc::socket::UDPNode* node, uint32_t receiver_id)
{
    PeerTable& table = peers_of(node);
    std::lock_guard<std::mutex> lock(table.mtx);
    return table.by_receiver.count(receiver_id) > 0;
}

std::size_t peer_count(const ipc::socket::UDPNode* node)
{
    PeerTable& table = peers_of(node);
    std::lock_guard<std::mutex> lock(table.mtx);
    return table.by_receiver.size();
}

/* 首轮 ACK 等待: 由实测 RTT 决定, 向上取整到毫秒 (receive() 的粒度是 ms)。 */
uint64_t ack_first_wait_ms(const ipc::socket::UDPNode* node)
{
    const uint64_t rto_us = rtt_of(node).rto_us();
    return std::max<uint64_t>(1, (rto_us + 999) / 1'000);
}

/* ---------------- 闭环自适应限速 (段3 任务2b 起, 段4 方案E 重构为 DCTCP) ----
 *
 * WHC 重定标: rate_limit_bps 从常量变 per-node 自适应。同步单消息模型下在飞
 * 可靠消息恒 ≤1, 控制周期 = 消息生命周期。状态是**发送链路属性** (per-node),
 * 与对端无关 → 不进 PeerState (D-1 纪律, 表仍 3 字段 + ack_count)。
 *
 * 段3 (MIMD 盲探测, 已由段4 方案E 替换): 二值降速信号 (跨度 ≥ 4 的 NACK) →
 * 减半, 连续 K 条 clean → ×1.5。只有 {floor, initial} 两个边界点, 开区间内
 * 无不动点, 真实容量落在 (floor, initial) 时极限环振荡是结构必然 —— 这是 E
 * 的立项理由。文档里曾写 "AIMD" 是错的 (两边都乘性, 是 MIMD)。
 *
 * 段4 方案E (DCTCP 式比例控制, 替代 MIMD): 接收端每条消息回送 lost_pages /
 * page_cnt = 丢包分数 F (DZA2, 与 ACK 同行; NACK 派生 F 仅作混合部署应急),
 * 多对端取最差。控制律:
 *   α ← (1−g)·α + g·F            EWMA, g=1/16, 定点 ×256
 *   F > 0   → bps × (1−α/2), 钳到 floor        (比例降, 降幅随拥塞连续变化)
 *   F == 0 连续 K 条 (且页数足够) → bps + Δ     (加性升, 升幅恒为 initial/16)
 * 降幅 ∝ α ∝ F, 升幅恒为 Δ ⇒ 速率稳定在"比例降幅恰好抵消加性升幅"那一点,
 * 开区间内有真实平衡点, 极限环消失。
 *
 * 采样截断残余: 迟到 NACK (晚于首匹配 ACK 的 return) 会被下一条消息的入口
 * drain 丢弃 —— 只漏报不误报; DZA2 随 ACK 同行, 不依赖 NACK 是否赶上。 */

struct RateLimitState
{
    std::mutex mtx;                  // 保护本状态 (同 rtt_of 锁粒度结论)
    std::size_t initial_bps{0};      // 播种值 (配置), 增长上限
    std::size_t bps{0};              // 当前生效速率; 0 = 未启用
    std::size_t static_floor_bps{0}; // initial / kRateFloorDivisor
    std::uint32_t clean_count{0};    // 连续干净消息数 (够大的消息 F==0)
    bool floor_warned{false};        // 下限告警每节点只打一次 (复刻 udp.h 哲学)
    /* ---- 段4 方案E 新增 ---- */
    std::uint32_t alpha_scaled{0};    // α×256, DCTCP EWMA (干净启动, 首次丢包后爬升)
    std::size_t additive_step_bps{0}; // Δ = initial/16, 播种时算一次 (加性升步长)
    std::size_t observed_bps_last{0}; // 诊断快照 (最近 DZA2 值), 不参与控制 (D-7)
    std::size_t runs_last{0};         // 诊断快照 (最近消息 runs 首捕获), 不参与控制
    std::size_t lost_last{0};         // 诊断快照 (同一 pattern 缺页数), 不参与控制
    /* ---- 段5 R1 新增: 活 α 的高位累积器 ---- */
    std::uint32_t alpha_accum{0};     // α×4096, EWMA 在 ×16 高位累积, 消除 /16 整数截断死区
};

/* 复刻 rtt_of / peers_of 的查找范式: static mutex + unordered_map,
 * key = node.get(), 进程生命周期, 条目不回收。 */
RateLimitState& rate_state_of(const ipc::socket::UDPNode* node)
{
    static std::mutex map_mtx;
    static std::unordered_map<const ipc::socket::UDPNode*, std::unique_ptr<RateLimitState>> map;
    std::lock_guard<std::mutex> lock(map_mtx);
    auto it = map.find(node);
    if (it == map.end())
    {
        it = map.emplace(node, std::make_unique<RateLimitState>()).first;
    }
    return *it->second;
}

/* 有效速率 (播种规则):
 *   options.rate_limit_bps != 0 → 直接用 options 值, 不建状态不自适应
 *     (显式意图优先, 与旧行为逐字节一致);
 *   否则:
 *     global == 0          → 返回 0, 不建状态 (不限速 = 无状态无开销, 全链旁路);
 *     global != initial    → 重新播种: {initial, bps, static_floor=global/32,
 *                              clean_count=0, floor_warned=false} —— 运行时改
 *                              全局旋钮立即生效 (下一条消息起), 已自适应的
 *                              中间态被重置 (§5.2 行为变化①)。 */
std::size_t effective_rate_bps(ipc::socket::UDPNode* node, std::size_t options_bps)
{
    if (options_bps != 0)
    {
        return options_bps;
    }
    const std::size_t global = global_rate_limit_bps().load(std::memory_order_relaxed);
    if (global == 0)
    {
        return 0;
    }
    RateLimitState& st = rate_state_of(node);
    std::lock_guard<std::mutex> lock(st.mtx);
    if (st.initial_bps != global)
    {
        st.initial_bps = global;
        st.bps = global;
        st.static_floor_bps = global / kRateFloorDivisor;
        st.clean_count = 0;
        st.floor_warned = false;
        /* 段4 方案E 播种: α 干净启动; Δ = initial/16 (相对播种值, 与绝对速率
         * 解耦); 诊断快照清零。 */
        st.alpha_scaled = 0;
        st.alpha_accum = 0;   // 段5 R1: ×4096 累积器同步清零
        st.additive_step_bps = std::max<std::size_t>(1, global / 16);
        st.observed_bps_last = 0;
        st.runs_last = 0;
        st.lost_last = 0;
    }
    return st.bps;
}

/* 丢包分数 F 的定点表示 (×256, ∈ [0,256])。count 夹取到 page_cnt 防分母错配
 * (畸形帧的越界缺页号已被调用方过滤, 这里再钳一道)。 */
uint32_t scaled_loss_fraction(uint32_t count, uint16_t page_cnt)
{
    if (page_cnt == 0)
    {
        return 0;
    }
    const uint64_t cnt = std::min<uint64_t>(count, page_cnt);
    return static_cast<uint32_t>(std::min<uint64_t>(256, (cnt * 256) / page_cnt));
}

/* 物理下限 (reviewer 方案 §3.2, 必须):
 *
 * 接收端组装 deadline 是硬截止: recv 侧 effective_tm 用**全局**速率算 (:1000),
 * 超时直接 return false 且不发 NACK (:1060-1068, :1116-1120) —— 自适应速率一旦
 * 低到实际传输时间 > effective_tm, 消息静默死在接收端, 连降速信号都没有。
 * 发送端无法知道接收端进程的速率, 但双方同一代码库, effective_tm 的公式是
 * 确定的, 取最小的订阅端 tm=50ms (pub-sub, 最紧) 估算, 留 1.5 倍余量。
 * 必须用**全局**速率算 (transit_g 不传 bps): 模拟的正是接收端看到的 deadline。 */
uint64_t window_floor_bps(uint16_t page_cnt)
{
    const uint64_t bytes = static_cast<uint64_t>(page_cnt) * UDP_MAX_SIZE;
    const uint64_t transit_g = rate_limited_transit_ms(page_cnt);   // 全局速率下的传输
    const uint64_t deadline_est_ms = std::max<uint64_t>(50, transit_g + transit_g / 2 + 20);
    return (bytes * 1'500ULL) / deadline_est_ms;    // ×1.5 余量 + ms→s
}

/* live 判定 (reviewer 方案 §4): 表里有条目, 且最近一次 ACK 在租期内。
 * 僵尸 (从不 ACK) 恒非 live; 新加入者 (尚无 ACK) 同理。last_nack_ts 不参与
 * —— 僵尸永远在 NACK, 它不能证明活。只读 last_ack_ts, 不加字段、不写表。 */
bool peer_is_live(const ipc::socket::UDPNode* node, uint32_t receiver_id, uint64_t lease_ms)
{
    PeerTable& table = peers_of(node);
    std::lock_guard<std::mutex> lock(table.mtx);
    auto it = table.by_receiver.find(receiver_id);
    if (it == table.by_receiver.end())
    {
        return false;
    }
    if (it->second.last_ack_ts == std::chrono::steady_clock::time_point{})
    {
        return false;
    }
    const auto age_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                            std::chrono::steady_clock::now() - it->second.last_ack_ts)
                            .count();
    return age_ms >= 0 && static_cast<uint64_t>(age_ms) < lease_ms;
}

/* 转移函数 (每条消息结束时调用一次, 段4 方案E DCTCP 式比例控制, 替换段3
 * MIMD):
 *
 * f_max_scaled = 本条消息收集到的最大丢包分数 F×256 (多对端取最差, D-8:
 *   DZA2 的 lost_pages 权威, NACK 派生 F 只作混合部署应急 —— 取 max 后只应用
 *   一次, 天然防双源重复计数)。
 * F > 0 → 比例降: bps × (1−α/2), 钳到 floor —— 降幅 ∝ α ∝ F, 连续变化。
 *   到下限仍拥塞 → 告警 ① (短语逐字保留自段3)。clean_count 清零。
 * F == 0 → 连续 K 条**够大的**消息 (page_cnt ≥ kMinPagesForRateSignal, 裁定②:
 *   小消息 F=0 是弱证据, 中性跳过不清零) 后加性升 +Δ, 上限 initial —— 升幅
 *   恒为 Δ 不过冲, 这是 E 消除极限环的关键。
 * at_floor_timeout (超时返回点传 true): 速率已在下限仍 FailedTimeout → 告警
 *   ② —— 覆盖「接收端静默放弃所以没有 NACK」的盲区 (window_floor 描述的静默
 *   丢弃型失效)。两条告警共用固定短语 `rate limit floor`, 每节点一次
 *   (floor_warned), 不刷屏。超时不算 clean 不降速 (段3 :685-689 裁定逐字)。
 * 完整性失败: 调用方已拦截, 不进入本函数。
 * 计数器 (rate_reductions/rate_increments) 只在速率实际变化时 ++ (段4 任务0
 * 纪律: 钳到 floor / initial 的 no-op 不计数); 计数受 frag_track 门控, 功能
 * 不受门控。 */
void adapt_rate_dctcp(ipc::socket::UDPNode* node, uint16_t page_cnt, uint32_t f_max_scaled, bool clean,
                      bool at_floor_timeout = false, std::size_t observed_bps = 0,
                      std::size_t runs_first = 0, std::size_t lost_first = 0)
{
    RateLimitState& st = rate_state_of(node);
    std::lock_guard<std::mutex> lock(st.mtx);
    if (st.bps == 0)
    {
        return;   // 未启用
    }
    const std::size_t floor_bps = std::max(st.static_floor_bps, static_cast<std::size_t>(window_floor_bps(page_cnt)));
    const bool at_floor = st.bps <= floor_bps;
    const bool track = frag_track_enabled().load(std::memory_order_relaxed);
    const uint32_t f_scaled = std::min<uint32_t>(f_max_scaled, 256);
    /* EWMA 每条消息都更新: F=0 时 α 按 (1−g) 衰减, 无拥塞余震。 */
    /* 段5 R1 修死区 (§4.3): α EWMA 改 ×4096 高位累积器, 消除 /16 整数截断死区。
     * 显式 cast 必须保留: 无 cast 时 f_scaled*16 若被提升为 unsigned 会改变负数语义 (§8.3)。 */
    st.alpha_accum += (static_cast<int32_t>(f_scaled) * 16 - static_cast<int32_t>(st.alpha_accum)) / 16;
    st.alpha_scaled = static_cast<uint32_t>(st.alpha_accum >> 4);
    if (f_scaled > 0)
    {
        const std::size_t old_bps = st.bps;
        /* 降幅 = bps × α/2, 向上取整; α 至少按 1 (≈0.4%) 计 —— F>0 而 α=0
         * (首次丢包) 或除法截断为 0 时, 降幅恒 0 会让拥塞信号静默 (出错点③)。 */
        /* 段5 任务2j (P-3 判别器本体, O.3 放行): runs/lost 判拥塞 vs 随机。
         * 阈值 0.579 (tester 实测标定: 标准 p90=0.158 < 1% p10=1.000 间隙中点)。
         * 判随机 (runs/lost>0.579) → α_eff 压到结构下限 1: 降速减弱但不清零,
         * 最小降仍 = (bps×1+511)/512 ≥ bps/512 (裁定1(c), ⛔ 不许清零)。
         * 判拥塞 / 缺数据 (无首捕获 pattern 或 lost=0 分母退化) → 维持 R1 全量降
         * (裁定1(b) 保守按拥塞, 不 fail-open 成随机)。判别量只进 α_eff 增益,
         * ⛔ 不进 bps= 赋值 (裁定1(a)); runs 从位图 NACK 派生 (liveness 仅 NACK 派生)。 */
        const bool runs_random = kRunsCaptureEnabled && runs_first > 0 && lost_first > 0
                                 && runs_first * 1000 > static_cast<std::size_t>(kRatePlan3RunsRandom) * lost_first;
        const uint32_t alpha_eff = runs_random ? 1u : std::max<uint32_t>(st.alpha_scaled, 1);
        const std::size_t drop = (st.bps * alpha_eff + 511) / 512;
        st.bps = std::max(floor_bps, st.bps - drop);
        st.clean_count = 0;
        if (st.bps == floor_bps && !st.floor_warned)   // 到下限仍拥塞 → 告警 ①
        {
            st.floor_warned = true;
            if (track)
            {
                frag_track_state().rate_floor_warns.fetch_add(1, std::memory_order_relaxed);
            }
            std::fprintf(stderr, "\033[33m[dzIPC][warn] rate limit floor reached: "
                         "node=%p rate=%zu B/s; overflow NACKs persist — check "
                         "net.core.rmem_max and subscriber health\033[0m\n",
                         static_cast<const void*>(node), st.bps);
        }
        if (track && st.bps != old_bps)   // 已在下限时钳制是 no-op: 速率未变, 不计数
        {
            frag_track_state().rate_reductions.fetch_add(1, std::memory_order_relaxed);
        }
        /* 段5 任务2k (P.1 回升门, 本轮唯一新增授权): 判随机时允许回升 tick 触发 ——
         * 打开 P-3 第二头 (S.3 病征 inc=0 零回升)。判别器只改「是否发生」(布尔门):
         * runs_random=true → 在削弱降速 (α_eff=1, 2j) 之后追加一次回升 tick。
         * ⛔ 幅度逐字不动: inc_step 表达式与下方 rise 分支同式 (Δ=initial/320 或
         * 回滚态 additive_step_bps, 上限 initial), 判别量不写进 bps 函数项 (W 硬禁令
         * 针对 rise=Δ·g(判别量) 形式)。缺数据 / 真拥塞 (runs_random=false) → 门关闭,
         * 维持 R1 全量降 (judge_p3ii 第6条「真拥塞降速不弱于 R1」自动保持)。 */
        if (runs_random)
        {
            const std::size_t old_bps_rise = st.bps;
            const std::size_t inc_step = (kRateRecoverAlpha >= kRateRecoverAlphaSentinel)
                                             ? st.additive_step_bps
                                             : std::max<std::size_t>(1, st.initial_bps / kRateRecoverStepDivisor);
            st.bps = std::min(st.initial_bps, st.bps + inc_step);   // 加性升 +Δ, 上限 initial
            st.clean_count = 0;
            if (track && st.bps != old_bps_rise)   // 已在天花板 initial 时 min() 是 no-op: 速率未变, 不计数
            {
                frag_track_state().rate_increments.fetch_add(1, std::memory_order_relaxed);
            }
        }
    }
    else if (clean && page_cnt >= kMinPagesForRateSignal
             && (kRateRecoverAlpha >= kRateRecoverAlphaSentinel
                     ? (++st.clean_count >= kCleanMessagesPerIncrement)   // 回滚态: 旧 K 连续门
                     : (st.alpha_accum >> 4 <= kRateRecoverAlpha)))       // 新态: α 门 (α_scaled≤T 才放行)
    {
        const std::size_t old_bps = st.bps;
        /* 段5 R1 Δ 收缩 (§4.5): 回滚态用 additive_step_bps(=initial/16), 新态用 initial/320。 */
        const std::size_t inc_step = (kRateRecoverAlpha >= kRateRecoverAlphaSentinel)
                                         ? st.additive_step_bps
                                         : std::max<std::size_t>(1, st.initial_bps / kRateRecoverStepDivisor);
        st.bps = std::min(st.initial_bps, st.bps + inc_step);   // 加性升 +Δ, 上限 initial
        st.clean_count = 0;
        if (track && st.bps != old_bps)   // 已在天花板 initial 时 min() 是 no-op: 速率未变, 不计数
        {
            frag_track_state().rate_increments.fetch_add(1, std::memory_order_relaxed);
        }
    }
    else if (at_floor_timeout && at_floor && !st.floor_warned)   // 下限仍超时 → 告警 ②
    {
        st.floor_warned = true;
        if (track)
        {
            frag_track_state().rate_floor_warns.fetch_add(1, std::memory_order_relaxed);
        }
        std::fprintf(stderr, "\033[33m[dzIPC][warn] rate limit floor reached: "
                     "node=%p rate=%zu B/s; timed out at floor — subscriber may be "
                     "silently dropping (effective_tm deadline) — check "
                     "net.core.rmem_max and subscriber health\033[0m\n",
                     static_cast<const void*>(node), st.bps);
    }
    st.observed_bps_last = observed_bps;   // 诊断快照, 不参与控制 (D-7)
    st.runs_last = runs_first;             // 诊断快照 (T.3 首捕获 + T.1 最拥塞), 不参与控制
    st.lost_last = lost_first;
    if (track)
    {
        frag_track_state().rate_bps_now.store(st.bps, std::memory_order_relaxed);
        frag_track_state().observed_bps_now.store(st.observed_bps_last, std::memory_order_relaxed);
        frag_track_state().runs_now.store(st.runs_last, std::memory_order_relaxed);
        frag_track_state().lost_now.store(st.lost_last, std::memory_order_relaxed);
    }
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

/* ---- 段5 任务2h: runs 判别信号 (方案3 前置, 纯诊断, 不进 bps= 赋值) ----
 * runs = 位图 NACK 里连续缺失段的段数 (真拥塞≈1, 真随机≈lost)。RunsFirst 是
 * 每消息每对端的 T.3 首捕获记录 (见 drain_record_acks / chunk_send_ex)。 */
struct RunsFirst
{
    bool captured{false};
    std::size_t runs{0};
    std::size_t lost{0};
};

/* 与 F 采集同一边界 (page_id==0 || >page_cnt 即停) 数连续缺失段数。lost 沿用
 * 调用方已算好的 miss_cnt (同源, 不重算 —— runs 与 lost 必须出自同一 pattern)。 */
std::size_t count_bitmap_runs(const IpcRtpsNackBitmapMsg& nb, std::size_t page_cnt)
{
    std::size_t runs = 0;
    bool prev_missing = false;
    for (std::size_t bit = 0; bit < nb.bit_cnt; ++bit)
    {
        const std::size_t page_id = static_cast<std::size_t>(nb.base_page) + bit;
        if (page_id == 0 || page_id > page_cnt)
        {
            break;
        }
        const bool missing = nb.is_missing(static_cast<uint16_t>(page_id));
        if (missing && !prev_missing)
        {
            ++runs;   // 0→1 跳变 = 新 run 起点
        }
        prev_missing = missing;
    }
    return runs;
}

/* T.1 跨对端聚合: 取最拥塞 (runs/lost 最小, 交叉相乘避免除法) 对端, 只输出该
 * 对端原始 runs/lost —— 比值留给判读侧算 (P.3: 分离度标定要看分布)。无首捕获
 * pattern → (0,0)。 */
void aggregate_runs_first(const std::unordered_map<uint32_t, RunsFirst>& m, std::size_t& runs_out,
                          std::size_t& lost_out)
{
    runs_out = 0;
    lost_out = 0;
    std::size_t best_runs = 0;
    std::size_t best_lost = 0;
    bool any = false;
    for (const auto& kv : m)
    {
        const RunsFirst& rf = kv.second;
        if (!rf.captured || rf.lost == 0)
        {
            continue;
        }
        if (!any || rf.runs * best_lost < best_runs * rf.lost)
        {
            any = true;
            best_runs = rf.runs;
            best_lost = rf.lost;
        }
    }
    if (any)
    {
        runs_out = best_runs;
        lost_out = best_lost;
    }
}

/* 段3 任务2: ACK 命中后非阻塞收干记账 (裁定 段3_裁定_抑制触发窗口与drain.md §二,
 * 拍板1 的落法)。首匹配 ACK 已满足返回条件 (at-least-one 语义不变), 这里只把
 * 内核缓冲里已到达的其他对端 ACK 一并记入身份表再丢弃 —— 不额外等待, 只是
 * 不再丢弃。与 drain_self_loopback 的纯丢弃不同: 仅对**当前消息**四字段匹配的
 * ACK 记账, 其余帧 (self-loopback 数据片、HB、NACK、其他消息的迟到帧) 一律
 * 丢弃 —— 逐帧消费行为与旧 drain 等价 (只看 empty, 不改变队列状态), 原有
 * 「清掉剩余 self-loopback 防下轮误处理」职责保持。
 *
 * 三条纪律 (写时自查):
 *   - 用**局部** ack_msg 实例, 不得覆盖调用方 ack_msg —— got_ack 分支的
 *     完整性校验在 drain 之后还要读外层 ack_msg 的 CRC, 覆盖即读错 peer。
 *   - 不参与 RTT 估计: Karn + ACK 含对端组装耗时, 迟到样本会系统性推高 RTO。
 *   - 上限 1024 只是防御性兜底, 正常出口 = receive_nowait() 收到空包即退。
 * seen: 本消息已记账对端集合 (含循环内首匹配的 receiver_id), 保证每消息
 * 每对端至多计 1 次 —— ack_count 语义 = 确认该消息的对端数。
 *
 * 段3 任务2b → 段4 方案E 扩展 (reviewer 方案 §8 改 5 + 设计 §7.2): 非 ACK 帧里
 * 顺带采集丢包分数 F —— 位图/显式 NACK 计数**通过范围检查**的缺页数 / page_cnt,
 * 发送者 live → 并入 *f_max_scaled (取 max; 僵尸/新加入者不驱动降速, D-3)。
 * **DZA2 分支**: 与 DZAK 同记账 (record_peer_ack, 无 liveness 过滤 —— DZA2
 * 本身即"本消息已 ACK"的证明, 僵尸从构造上发不出), 并入 F 与 observed_bps
 * (仅诊断)。**用局部 nack 实例 (nack_msg/nb_msg/ack2_msg), 不写身份表**
 * (record_peer_nack 不调, last_nack_ts 不更新 —— 与循环内的记录语义分离)。
 * lease_ms == 0 或 f_max_scaled 为 nullptr 时退化为纯记账 (保持旧行为)。
 * F 是"取 max 聚合"而非短路布尔 —— 每条非 ACK 帧都要解析完。
 * 段5 任务2h: runs_first_map != nullptr 时, 顺带对每对端第一条位图 NACK 做 T.3
 * 首捕获 (迟到 NACK 也采 —— 对端首条 NACK 可能晚于 got_ack 才入 drain 窗口)。 */
void drain_record_acks(ipc::socket::UDPNode& node, const chunk_meta& meta,
                       const ipc::socket::UDPNode* rtt_key,
                       std::unordered_set<uint32_t>& seen,
                       uint64_t lease_ms = 0, uint32_t* f_max_scaled = nullptr,
                       uint32_t* observed_bps_out = nullptr,
                       std::unordered_map<uint32_t, RunsFirst>* runs_first_map = nullptr)
{
    constexpr int kMaxAckDrain = 1'024;
    IpcRtpsAckMsg ack_msg;   // 局部实例, 不碰调用方 ack_msg
    /* 信号采集的局部实例 —— 与循环内 (chunk_send_ex 的 nack_msg/nb_msg/ack_msg)
     * 语义分离: 这里只读不写身份表。 */
    IpcRtpsNackMsg nack_msg;
    IpcRtpsNackBitmapMsg nb_msg;
    IpcRtpsAck2Msg ack2_msg;
    for (int i = 0; i < kMaxAckDrain; ++i)
    {
        ipc::buffer buf = node.receive_nowait();
        if (buf.empty())
        {
            return;
        }
        if (ack2_msg.check_ak2_id(buf))
        {
            /* DZA2 (段4 方案E): 与 DZAK 同记账 + 采 F。四字段匹配后无需
             * liveness 过滤 (DZA2 自证活, 设计 §6.2)。 */
            ack2_msg.deserialize(buf);
            if (ack2_msg.page_cnt == meta.page_cnt && ack2_msg.total_size == meta.total_size
                && ack2_msg.data_msg_id == meta.msg_id && ack2_msg.sequence == meta.sequence)
            {
                if (seen.insert(ack2_msg.receiver_id).second)
                {
                    record_peer_ack(rtt_key, ack2_msg.receiver_id, meta.sequence);
                }
                if (f_max_scaled != nullptr)
                {
                    *f_max_scaled = std::max(*f_max_scaled, scaled_loss_fraction(ack2_msg.lost_pages, meta.page_cnt));
                }
                if (observed_bps_out != nullptr)
                {
                    *observed_bps_out = ack2_msg.observed_bps;
                }
            }
            continue;
        }
        if (!ack_msg.check_ak_id(buf))
        {
            /* 非 ACK 帧 (数据片/HB/显式 NACK/位图 NACK): 照旧丢弃, 但先做
             * F 采集 (NACK 派生 F 只作混合部署应急, 主从关系见设计 §6.2)。 */
            if (lease_ms > 0 && f_max_scaled != nullptr)
            {
                if (!nack_msg.check_nk_id(buf))
                {
                    if (!nb_msg.check_nb_id(buf))
                    {
                        continue;
                    }
                    nb_msg.deserialize(buf);
                    if (nb_msg.page_cnt == meta.page_cnt && nb_msg.total_size == meta.total_size
                        && nb_msg.data_msg_id == meta.msg_id && nb_msg.sequence == meta.sequence)
                    {
                        std::size_t miss_cnt = 0;
                        for (std::size_t bit = 0; bit < nb_msg.bit_cnt; ++bit)
                        {
                            const std::size_t page_id = static_cast<std::size_t>(nb_msg.base_page) + bit;
                            if (page_id == 0 || page_id > meta.page_cnt)
                            {
                                break;   // 窗口尾部填充位, 越界即可停
                            }
                            if (nb_msg.is_missing(static_cast<uint16_t>(page_id)))
                            {
                                ++miss_cnt;
                            }
                        }
                        if (miss_cnt > 0 && peer_is_live(rtt_key, nb_msg.receiver_id, lease_ms))
                        {
                            *f_max_scaled =
                                std::max(*f_max_scaled, scaled_loss_fraction(static_cast<uint32_t>(miss_cnt),
                                                                              meta.page_cnt));
                            /* 段5 任务2h: T.3 首捕获 (每对端只采第一条位图 NACK)。 */
                            if (runs_first_map != nullptr && kRunsCaptureEnabled
                                && runs_first_map->find(nb_msg.receiver_id) == runs_first_map->end())
                            {
                                runs_first_map->emplace(nb_msg.receiver_id,
                                                        RunsFirst{true, count_bitmap_runs(nb_msg, meta.page_cnt),
                                                                  miss_cnt});
                            }
                        }
                    }
                    continue;
                }
                nack_msg.deserialize(buf);
                if (nack_msg.page_cnt == meta.page_cnt && nack_msg.total_size == meta.total_size
                    && nack_msg.data_msg_id == meta.msg_id && nack_msg.sequence == meta.sequence)
                {
                    std::size_t miss_cnt = 0;
                    for (uint16_t page_id : nack_msg.missing_pages)
                    {
                        if (page_id == 0 || page_id > meta.page_cnt)
                        {
                            continue;
                        }
                        ++miss_cnt;
                    }
                    if (miss_cnt > 0 && peer_is_live(rtt_key, nack_msg.receiver_id, lease_ms))
                    {
                        *f_max_scaled =
                            std::max(*f_max_scaled, scaled_loss_fraction(static_cast<uint32_t>(miss_cnt),
                                                                          meta.page_cnt));
                    }
                }
            }
            continue;
        }
        ack_msg.deserialize(buf);
        if (ack_msg.page_cnt != meta.page_cnt || ack_msg.total_size != meta.total_size
            || ack_msg.data_msg_id != meta.msg_id || ack_msg.sequence != meta.sequence)
        {
            continue;   // 其他消息的迟到 ACK: 丢弃不记账
        }
        if (!seen.insert(ack_msg.receiver_id).second)
        {
            continue;   // 本消息已计过该对端, 防重复
        }
        record_peer_ack(rtt_key, ack_msg.receiver_id, meta.sequence);
    }
}

/* 段4 方案E 扩展: use_dza2=true 时发 DZA2 (DZAK 超集 + lost_pages/observed_bps),
 * 否则与段3 逐字节一致。**发送端只有在 HB 通告过 kFlagRateFeedback 时才会收到
 * DZA2** —— 旧版发送端会整帧丢弃 DZA2, 该消息永远等不到 ACK (成功路径致命),
 * 所以 use_dza2 必须由协商位 sender_feedback_ok 驱动, 绝不无条件发。 */
void send_ack(ipc::socket::UDPNode& node, const chunk_meta& meta, uint32_t payload_crc32c, bool use_dza2 = false,
              uint16_t lost_pages = 0, uint32_t observed_bps = 0)
{
    if (use_dza2)
    {
        IpcRtpsAck2Msg ack2_msg;
        ack2_msg.page_cnt = meta.page_cnt;
        ack2_msg.total_size = meta.total_size;
        ack2_msg.data_msg_id = meta.msg_id;
        ack2_msg.receiver_id = local_node_id();
        ack2_msg.sequence = meta.sequence;
        ack2_msg.integrity_flags = INTEGRITY_FLAG_CRC32C;
        ack2_msg.payload_crc32c = payload_crc32c;
        ack2_msg.lost_pages = lost_pages;
        ack2_msg.observed_bps = observed_bps;
        ipc::buffer ack2_buf = ack2_msg.serialize();
        send_chunk_with_retry(node, ack2_buf);
        return;
    }
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
     * 显式列表, 那是唯一能被对方解析的编码。
     * kFlagRateFeedback 恒置 (段4 方案E): 本版本的 chunk_send_ex 一定认识 DZA2
     * (解析 DZAK 与 DZA2 两条路径都在)。接收端只有看到这一位才发 DZA2 —— 旧版
     * 发送端若收到 DZA2 会整帧丢弃 (成功路径致命, 见 udp_rtps_ack_msg.hpp 的
     * DZA2 注释)。旧端解析只做 & 掩码, 多余位安全忽略, 置位无损。 */
    hb.flags = static_cast<uint8_t>((reliable ? IpcRtpsHeartbeatMsg::kFlagReliable : 0)
                                    | (final_hb ? IpcRtpsHeartbeatMsg::kFlagFinal : 0)
                                    | IpcRtpsHeartbeatMsg::kFlagBitmapNack
                                    | IpcRtpsHeartbeatMsg::kFlagRateFeedback);
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
        /* 单页路径无组装轮, 协商位只能来自前导 HB (与多页路径 :1351 处判定
         * 语义一致)。**单页也发 DZA2** (leader 裁定②): 不发的话全小消息负载
         * 永远产生不了 clean 样本, 降速后加性升永不触发 —— 但 F=0 来自单页
         * 消息不构成"链路有余量"的证据 (1 页没压到缓冲), 发送端以
         * kMinPagesForRateSignal 门控 clean 计数。observed_bps 无组装窗, 显式
         * 置 0 —— 否则接近 0 的 elapsed 除出垃圾值。 */
        bool fb_ok = false;
        if (have_pending_hb && pending_hb.data_msg_id == meta.msg_id && pending_hb.page_cnt == meta.page_cnt
            && pending_hb.total_size == meta.total_size)
        {
            fb_ok = (pending_hb.flags & IpcRtpsHeartbeatMsg::kFlagRateFeedback) != 0;
        }
        const uint32_t payload_crc32c = dzIPC::common::crc32c(first_page.data(), first_page.size());
        msg_ptr->deserialize(first_page);
        send_ack(ack_out, meta, payload_crc32c, fb_ok, /*lost_pages=*/0, /*observed_bps=*/0);
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
    bool sender_feedback_ok = false;  // HB 声明发送端认识 DZA2(段4 方案E, 显式拥塞反馈)

    /* 段4 方案E 测量: lost_pages = 本条消息首次发 NACK 时的缺片数 = 去重重传
     * 需求 (received[] 逐页幂等去重), 精确无截断, 是权威 F 的分子。首次捕获后
     * 不再改 —— 消息收全后才算的 received_cnt 恒为 page_cnt, 那会让 F 恒 0,
     * 复刻段3 "信号系统性偏低" 的缺陷。消息无缺片 ⇒ 永不调 send_nack_auto ⇒
     * 保持 0 ⇒ F=0 ✓。 */
    uint16_t lost_pages = 0;

    /* 前导 HB 已经在 wait_first_data_chunk 里采纳过 sequence, 这里同步一下
     * 发送端的可靠性声明, 免得第一轮还按"未知"处理。 */
    if (have_pending_hb && pending_hb.data_msg_id == meta.msg_id && pending_hb.page_cnt == meta.page_cnt
        && pending_hb.total_size == meta.total_size)
    {
        sender_reliable = (pending_hb.flags & IpcRtpsHeartbeatMsg::kFlagReliable) != 0;
        sender_bitmap_ok = (pending_hb.flags & IpcRtpsHeartbeatMsg::kFlagBitmapNack) != 0;
        sender_feedback_ok = (pending_hb.flags & IpcRtpsHeartbeatMsg::kFlagRateFeedback) != 0;
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
                    sender_feedback_ok = (hb_msg.flags & IpcRtpsHeartbeatMsg::kFlagRateFeedback) != 0;
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
            if (lost_pages == 0)
            {
                /* 首次发 NACK 前捕获缺片数 (见上方声明处注释)。 */
                lost_pages = static_cast<uint16_t>(meta.page_cnt - received_cnt);
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
        if (lost_pages == 0)
        {
            lost_pages = static_cast<uint16_t>(meta.page_cnt - received_cnt);
        }
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
    /* 段4 方案E: observed_bps = 已组装字节 / 组装期 (assembly_begin 起, 含 NACK
     * 重传轮), 仅诊断不参与控制 (D-7 红线 —— 发送端是瓶颈时它恒等于发送速率,
     * 拿它设速率会永远不上涨)。elapsed < 1ms 时置 0 防除出垃圾值。 */
    const uint64_t asm_ms = elapsed_ms(assembly_begin);
    const uint32_t observed_bps = (asm_ms >= 1) ? static_cast<uint32_t>(meta.total_size * 1000 / asm_ms) : 0;
    send_ack(ack_out, meta, payload_crc32c, sender_feedback_ok, lost_pages, observed_bps);
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
     * SocketSendOptions, 只能靠进程级开关生效。段3 任务2b: 走 effective_rate_bps
     * 播种 per-node 自适应状态 (options 显式值优先不建状态; global==0 返回 0
     * 全链旁路)。 */
    const std::size_t rate_limit = effective_rate_bps(node.get(), options.rate_limit_bps);
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

    /* 段3 任务2b: 传当前自适应速率 —— 若恒读全局, 降速后窗口按全局速率算 <
     * 实际传输时间, 首轮提前超时 → 整批误重传 → 重传撑爆缓冲 → 降速本身制造
     * 丢包 (硬约束 1)。 */
    const uint64_t nack_wait_ms = calc_nack_wait_ms(meta.page_cnt, rate_limit);
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
     * ACK 到达时刻 ≈ transit_ms + RTT, 所以窗口取两者之和再留半倍余量。
     * (段3 任务2b: 传当前自适应速率, 理由同上。) */
    const uint64_t send_transit_ms = rate_limited_transit_ms(meta.page_cnt, rate_limit);
    if (send_transit_ms > 0)
    {
        first_wait_ms += send_transit_ms + send_transit_ms / 2;
    }

    const uint64_t ack_timeout_ms =
        (options.ack_timeout_ms == ipc::invalid_value)
            ? (first_wait_ms + nack_wait_ms * static_cast<uint64_t>(RTPS_MAX_NACK_ROUND))
            : options.ack_timeout_ms;
    /* 段3 任务2b: liveness 租期 = 3 × 本条消息的 effective ack_timeout (用户
     * 显式 override 就用 override, 否则推导值)。段4 方案E: f_max_scaled = 本条
     * 消息收集到的最大丢包分数 F×256 (DZA2 权威 + NACK 派生应急, 多对端取最差,
     * 取 max 后消息结束时只应用一次 —— 双源不会重复计数); observed_bps_last =
     * 最近 DZA2 的接收速率 (仅诊断, D-7 不参与控制)。 */
    const uint64_t lease_ms = kLeaseTimeoutMultiple * ack_timeout_ms;
    uint32_t f_max_scaled = 0;
    uint32_t observed_bps_last = 0;
    IpcRtpsNackMsg nack_msg;
    IpcRtpsNackBitmapMsg nb_msg;
    IpcRtpsAckMsg ack_msg;
    IpcRtpsAck2Msg ack2_msg;
    /* 本消息已记账 (含循环内首匹配) 的对端 receiver_id 集合 ——
     * drain_record_acks 据此去重, 每消息每对端至多计 1 次。 */
    std::unordered_set<uint32_t> acked_receivers;
    /* ---- 段5 任务2h: runs 判别信号 (纯诊断, 不进 bps=) ----
     * T.3 首捕获去重: 每消息每对端只取第一条位图 NACK (主循环 + drain 共享此表);
     * T.1 最拥塞聚合在消息结束时做 (aggregate_runs_first, 只上报原始 runs/lost)。 */
    std::unordered_map<uint32_t, RunsFirst> runs_first_map;

    const auto ack_begin = std::chrono::steady_clock::now();
    int round = 0;
    int hb_repeat = 0;   // 静默轮补发 HB 的次数, 上限 kMaxHeartbeatRepeat

    /* ================ 跨轮 NACK 抑制 (段3 任务1, DECISIONS.md) ================
     *
     * 键是 (message, page), 不是 receiver 维度: 重传走组播 (data_out),
     * receiver 维度不减少任何发送字节, 只增加 peers×pages 的 state, 纯开销。
     * 因此抑制状态是**一次 chunk_send_ex 调用内的局部数组** (serving
     * history, 长度 chunks.size()+1, 下标 1-based), 记录每页最近一次被
     * **重传**时的轮次, -1 = 从未重传。首轮 send_all_chunks 不算 —— 若算,
     * 第 1 轮重传整批会被全部压掉, 消息必然超时。无跨消息状态、无无界增长
     * (调用结束即销毁); 不进身份表 (D-1 纪律: 表仍 3 字段 + 已放行的
     * ack_count), 身份表在此的唯一作用 = 未知 peer 旁路 (见下)。
     *
     * 抑制判据 (K=1 轮): suppressed(P) ⇔ last_retransmit_round[P] == round - 1
     * —— 上一轮重传过的页, 本轮不再发。round==0 是首次重传批, 无"上一轮",
     * 永不抑制。窗口严格 ≤1 轮: 接收端 5 轮封顶 (RTPS_MAX_NACK_ROUND=5),
     * 最多压掉 1 轮, 恢复仍在预算内; 加大窗口会吃掉恢复余量。
     *
     * 旁路与开关: ①本轮 NACK 若来自身份表里首见的 receiver_id (新订阅者
     * 中途加入), 旁路抑制、立即补发, 避免它被"最近发过的页"饿一轮; 已知
     * peer 才走抑制。②单 peer 场景 (reviewer P5) 抑制净收益为负, 身份表
     * 条目 < 2 时整体关闭 (tester 场景 B 期望 nack_suppressed ≈ 0)。
     *
     * 🔴 P1 致死路径: 抑制轮必须"抑制数据、照发 HB"。接收端 fruitless 判据
     * 在 hb_seen 时整段跳过 (recv 侧 :970-982, :984 注释: 该启发式只服务不
     * 发 HB 的旧版本); 若抑制实现成"这一轮什么都不做", 接收端该轮无 HB →
     * hb_seen=false → fruitless 连续两轮 → 提前放弃、消息被丢。所以: 即使
     * 本轮所有缺页都被抑制, 重传批之后的补 HB 也照发 —— 它无条件执行、
     * 与抑制无关, 禁止"优化"掉。
     *
     * 与 missing_union 正交: missing_union 是轮内跨订阅者去重 (每轮清空),
     * 抑制是跨轮去重。第 N+1 轮重传判据 = P ∈ missing_union(N+1) && !suppressed(P)。 */
    std::vector<int> last_retransmit_round(chunks.size() + 1, -1);
    while (true)
    {
        bool got_nack = false;
        bool got_ack = false;
        /* 本收集窗口内命中四字段匹配的 ACK 帧 (DZAK 或 DZA2) —— 经基类引用
         * 统一访问公共前缀, 完整性检查读它 (段4 方案E)。 */
        const IpcRtpsAckMsg* matched_ack = nullptr;
        /* 本收集窗口内是否见过未知 peer 的 NACK —— 有则本轮整体跳过抑制、
         * 立即补发 (见上方抑制注释: 新订阅者中途加入的旁路)。每轮重置:
         * 旁路只作用于出现该 NACK 的那一轮。 */
        bool nack_from_unknown_peer = false;
        std::unordered_set<uint16_t> missing_union;
        const auto round_begin = std::chrono::steady_clock::now();
        const uint64_t round_wait_ms = (round == 0) ? first_wait_ms : nack_wait_ms;

        while (true)
        {
            if (options.delivery == SocketDeliveryMode::Reliable && elapsed_ms(ack_begin) >= ack_timeout_ms)
            {
                /* 段4 方案E: 超时返回前转移一次 (clean=false; at_floor_timeout=true
                 * → 若速率已在下限, 告警条件②覆盖「接收端静默放弃所以没有 NACK」
                 * 的盲区; 超时前收到的 NACK 仍按 F 降速)。 */
                std::size_t runs_f = 0, lost_f = 0;   // 段5 任务2h: T.1 最拥塞聚合 (诊断)
                aggregate_runs_first(runs_first_map, runs_f, lost_f);
                adapt_rate_dctcp(node.get(), meta.page_cnt, f_max_scaled, /*clean=*/false, /*at_floor_timeout=*/true,
                                 /*observed_bps=*/0, runs_f, lost_f);
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
                    /* 段4 方案E: 同第一处超时返回。 */
                    std::size_t runs_f = 0, lost_f = 0;   // 段5 任务2h: T.1 最拥塞聚合 (诊断)
                    aggregate_runs_first(runs_first_map, runs_f, lost_f);
                    adapt_rate_dctcp(node.get(), meta.page_cnt, f_max_scaled, /*clean=*/false, /*at_floor_timeout=*/true,
                                     /*observed_bps=*/0, runs_f, lost_f);
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
                    matched_ack = &ack_msg;
                    report.ack_crc32c = ack_msg.payload_crc32c;
                    /* 阶段 2: 记录确认对端。receiver_id 由接收端自报
                     * (local_node_id), ACK 已按四字段匹配, 身份可信。 */
                    record_peer_ack(rtt_key, ack_msg.receiver_id, meta.sequence);
                    /* 记入本消息 seen 集合: 首匹配对端已计过账, 出口 drain
                     * (drain_record_acks) 对它去重, 不重复计数。 */
                    acked_receivers.insert(ack_msg.receiver_id);
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
            /* DZA2 (段4 方案E 显式拥塞反馈 ACK): 紧跟 check_ak_id 之后、NACK
             * 检查之前 (出错点① —— 漏并这里, 反馈永不到达, DCTCP 主信号丢失)。
             * 与 DZAK 走同一段四字段匹配/记账/RTT (超集: 前 7 字段偏移一致),
             * 只多采 F 与 observed_bps。无需 liveness 过滤: DZA2 本身即"本条
             * 消息已 ACK"的证明 (record_peer_ack 更新 last_ack_ts), 僵尸从不
             * ACK 所以发不出 DZA2, 从构造上不可能拉低全组 (设计 §6.2)。 */
            if (ack2_msg.check_ak2_id(recv_buf))
            {
                ack2_msg.deserialize(recv_buf);
                if (ack2_msg.page_cnt == meta.page_cnt && ack2_msg.total_size == meta.total_size
                    && ack2_msg.data_msg_id == meta.msg_id && ack2_msg.sequence == meta.sequence)
                {
                    got_ack = true;
                    matched_ack = &ack2_msg;
                    report.ack_crc32c = ack2_msg.payload_crc32c;
                    record_peer_ack(rtt_key, ack2_msg.receiver_id, meta.sequence);
                    acked_receivers.insert(ack2_msg.receiver_id);
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
                    /* DZA2 是权威 F (lost_pages = 接收端逐页去重计数, 不随 NACK
                     * 截断低报); observed_bps 仅诊断 (D-7)。 */
                    f_max_scaled = std::max(f_max_scaled, scaled_loss_fraction(ack2_msg.lost_pages, meta.page_cnt));
                    observed_bps_last = ack2_msg.observed_bps;
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
                /* 未知 peer 旁路 (见上方抑制注释): 表里首见的 receiver_id
                 * 使本轮整体跳过抑制。必须先于 record_peer_nack 判定 ——
                 * 后者会把条目插进表, 之后"首见"就查不到了。 */
                if (!peer_is_known(rtt_key, nb_msg.receiver_id))
                {
                    nack_from_unknown_peer = true;
                }
                record_peer_nack(rtt_key, nb_msg.receiver_id);
                /* 段4 方案E 丢包分数 F: 数出**通过范围检查**的置位页数 (与
                 * missing_union 同一过滤, 防畸形帧越界页号污染 F) / page_cnt,
                 * 发送者 live → 并入 f_max_scaled (取最差对端, D-8)。NACK 派生
                 * F 是应急 (位图 ≤11496 位截断时低报, 预期降级) —— DZA2 权威
                 * F 会覆盖它 (去重计数单调、不截断)。 */
                std::size_t nack_miss_cnt = 0;
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
                        ++nack_miss_cnt;
                    }
                }
                if (nack_miss_cnt > 0 && peer_is_live(rtt_key, nb_msg.receiver_id, lease_ms))
                {
                    f_max_scaled = std::max(
                        f_max_scaled, scaled_loss_fraction(static_cast<uint32_t>(nack_miss_cnt), meta.page_cnt));
                    /* 段5 任务2h: T.3 首捕获 —— 主循环第一条位图 NACK 即采 (runs 与
                     * lost 同 pattern、同一边界, lost 沿用 nack_miss_cnt)。 */
                    if (kRunsCaptureEnabled
                        && runs_first_map.find(nb_msg.receiver_id) == runs_first_map.end())
                    {
                        runs_first_map.emplace(nb_msg.receiver_id,
                                               RunsFirst{true, count_bitmap_runs(nb_msg, chunks.size()),
                                                         nack_miss_cnt});
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
            /* 未知 peer 旁路 (见上方抑制注释), 同位图 NACK 处, 必须先于
             * record_peer_nack 判定。 */
            if (!peer_is_known(rtt_key, nack_msg.receiver_id))
            {
                nack_from_unknown_peer = true;
            }
            record_peer_nack(rtt_key, nack_msg.receiver_id);
            /* 段4 方案E 丢包分数 F: 同位置图 NACK 分支 (计数通过范围检查的
             * 缺页, live 才并入; 显式列表 ≤256 截断时低报, 应急信号)。 */
            std::size_t nack_miss_cnt = 0;
            for (uint16_t page_id : nack_msg.missing_pages)
            {
                if (page_id > 0 && page_id <= chunks.size())
                {
                    missing_union.insert(page_id);
                    ++nack_miss_cnt;
                }
            }
            if (nack_miss_cnt > 0 && peer_is_live(rtt_key, nack_msg.receiver_id, lease_ms))
            {
                f_max_scaled = std::max(f_max_scaled,
                                        scaled_loss_fraction(static_cast<uint32_t>(nack_miss_cnt), meta.page_cnt));
            }
        }

        if (got_ack)
        {
            /* ACK 命中后立刻 return 会把队列里剩余的 self-loopback 数据片留给下一轮，
               下一轮会把它们当作本轮 self-loopback 误处理（meta 完全相同）。
               这里顺带把内核缓冲里已到达的其他对端 ACK 记入身份表 (段3 任务2,
               drain_record_acks): 不额外等待, 只是不再丢弃; 清队列职责不变。
               段4 方案E: 同时把迟到 NACK/DZA2 的 F 采进 f_max_scaled (最差对端
               的反馈晚于 return 到达 → 本条欠反应, 下一条消息必然补采 —— 一条
               消息的滞后, 不是盲区, D-8)。 */
            drain_record_acks(ack_in, meta, rtt_key, acked_receivers, lease_ms, &f_max_scaled, &observed_bps_last,
                              &runs_first_map);
            if (options.integrity == SocketIntegrityMode::CRC32C
                && ((matched_ack->integrity_flags & INTEGRITY_FLAG_CRC32C) == 0 || report.ack_crc32c != report.crc32c))
            {
                /* 完整性失败: 不 adapt, 直接返回 (reviewer 方案 §8 改 5)。 */
                report.status = SocketSendStatus::FailedIntegrity;
                return report;
            }
            /* 段4 方案E: 消息结束, 转移一次。clean = 拿到 ACK 且 F==0 —— F 来自
             * DZA2 权威 (与 ACK 同行) 或 NACK 派生应急; 完整性失败已在上方拦截。
             * 带丢包的消息按 F 比例降 (含 1-2 页小丢失: 连续信号, 降幅小)。 */
            std::size_t runs_f = 0, lost_f = 0;   // 段5 任务2h: T.1 最拥塞聚合 (诊断)
            aggregate_runs_first(runs_first_map, runs_f, lost_f);
            adapt_rate_dctcp(node.get(), meta.page_cnt, f_max_scaled, /*clean=*/f_max_scaled == 0,
                             /*at_floor_timeout=*/false, observed_bps_last, runs_f, lost_f);
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
        /* 每轮重传批前评估抑制开关: 身份表条目在本条消息的 NACK 处理过程中
         * 才增长 (首条消息的 NACK 首见即填充), 调用开始时快照会恒为 false,
         * 第一条消息永不抑制。条目 < 2 = 单 peer 场景, 抑制净收益为负
         * (reviewer P5), 关闭, 由 tester 场景 B 验证。 */
        const bool suppression_enabled = (peer_count(rtt_key) >= 2);
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
            /* 跨轮抑制 (K=1): 上一轮重传过的页本轮不再发 (判据与理由见上方
             * 注释块)。未知 peer 旁路与单 peer 关闭已折叠进
             * nack_from_unknown_peer / suppression_enabled。被压掉的页计入
             * nack_suppressed。round==0 首次重传批永不命中 (round-1 == -1)。 */
            if (suppression_enabled && !nack_from_unknown_peer && round > 0
                && last_retransmit_round[miss] == round - 1)
            {
                if (frag_track_enabled().load(std::memory_order_relaxed))
                {
                    frag_track_state().nack_suppressed.fetch_add(1, std::memory_order_relaxed);
                }
                continue;
            }
            resent_bytes += chunks[miss - 1].size();
            /* 段3 任务2b: 重传实际发出字节计数 (门控, 场景 C 判据 ③ 的
             * retransmit_bytes / 载荷比)。 */
            if (frag_track_enabled().load(std::memory_order_relaxed))
            {
                frag_track_state().retransmit_bytes.fetch_add(chunks[miss - 1].size(), std::memory_order_relaxed);
            }
            /* 同一条消息内被重传 ≥2 次的页 (此前已有重传记录)。无抑制基线
             * > 0 —— NACK 延迟/重复到达会触发重复重传; 抑制后此类场景趋近 0,
             * 重传本身又丢 (Case D) 时如实 > 0。 */
            if (last_retransmit_round[miss] != -1
                && frag_track_enabled().load(std::memory_order_relaxed))
            {
                frag_track_state().pages_retransmitted_again.fetch_add(1, std::memory_order_relaxed);
            }
            last_retransmit_round[miss] = round;
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
    /* 段4 方案E: 终局超时前转移一次 (仅 Reliable 的 FailedTimeout 算失败;
     * BestEffort 的 SentUnconfirmed 不是失败, 不 adapt)。 */
    if (options.delivery == SocketDeliveryMode::Reliable)
    {
        std::size_t runs_f = 0, lost_f = 0;   // 段5 任务2h: T.1 最拥塞聚合 (诊断)
        aggregate_runs_first(runs_first_map, runs_f, lost_f);
        adapt_rate_dctcp(node.get(), meta.page_cnt, f_max_scaled, /*clean=*/false, /*at_floor_timeout=*/true,
                         /*observed_bps=*/0, runs_f, lost_f);
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
    out.nack_suppressed = st.nack_suppressed.load(std::memory_order_relaxed);
    out.pages_retransmitted_again = st.pages_retransmitted_again.load(std::memory_order_relaxed);
    out.rate_reductions = st.rate_reductions.load(std::memory_order_relaxed);
    out.rate_increments = st.rate_increments.load(std::memory_order_relaxed);
    out.rate_floor_warns = st.rate_floor_warns.load(std::memory_order_relaxed);
    out.retransmit_bytes = st.retransmit_bytes.load(std::memory_order_relaxed);
    out.rate_bps_now = st.rate_bps_now.load(std::memory_order_relaxed);
    out.observed_bps_now = st.observed_bps_now.load(std::memory_order_relaxed);
    out.runs_now = st.runs_now.load(std::memory_order_relaxed);
    out.lost_now = st.lost_now.load(std::memory_order_relaxed);
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
    st.nack_suppressed.store(0, std::memory_order_relaxed);
    st.pages_retransmitted_again.store(0, std::memory_order_relaxed);
    st.rate_reductions.store(0, std::memory_order_relaxed);
    st.rate_increments.store(0, std::memory_order_relaxed);
    st.rate_floor_warns.store(0, std::memory_order_relaxed);
    st.retransmit_bytes.store(0, std::memory_order_relaxed);
    st.rate_bps_now.store(0, std::memory_order_relaxed);
    st.observed_bps_now.store(0, std::memory_order_relaxed);
    st.runs_now.store(0, std::memory_order_relaxed);
    st.lost_now.store(0, std::memory_order_relaxed);
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

/* ---------------- 对端身份表诊断 (阶段 2, D-1 选项 B) ---------------- */

std::vector<PeerAckInfo> get_peer_ack_info(const ipc::socket::UDPNode* node)
{
    std::vector<PeerAckInfo> out;
    PeerTable& table = peers_of(node);
    std::lock_guard<std::mutex> lock(table.mtx);
    out.reserve(table.by_receiver.size());
    for (const auto& [receiver_id, st] : table.by_receiver)
    {
        out.push_back(PeerAckInfo{receiver_id, st.highest_acked_seq, st.ack_count});
    }
    /* 升序输出: 快照要可断言, 迭代序 (unordered_map) 不可用。 */
    std::sort(out.begin(), out.end(),
              [](const PeerAckInfo& a, const PeerAckInfo& b) { return a.receiver_id < b.receiver_id; });
    return out;
}
}   // namespace socket
}   // namespace dzIPC

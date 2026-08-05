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

bool send_chunk_with_retry(std::shared_ptr<ipc::socket::UDPNode>& node, ipc::buffer& chunk)
{
    int retry = 0;
    while (!node->send(chunk))
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

bool send_all_chunks(std::shared_ptr<ipc::socket::UDPNode>& node, std::vector<ipc::buffer>& chunks,
                     std::size_t rate_limit_bps)
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
void drain_self_loopback(std::shared_ptr<ipc::socket::UDPNode>& node)
{
    constexpr int kMaxDrain = 8'192;
    for (int i = 0; i < kMaxDrain; ++i)
    {
        ipc::buffer buf = node->receive_nowait();
        if (buf.empty())
        {
            return;
        }
    }
}

void send_ack(std::shared_ptr<ipc::socket::UDPNode>& node, const chunk_meta& meta, uint32_t payload_crc32c)
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

bool wait_first_data_chunk(std::shared_ptr<ipc::socket::UDPNode>& node, std::shared_ptr<IpcMsgBase>& msg_ptr,
                           chunk_meta& meta, ipc::buffer& first_page,
                           const std::chrono::steady_clock::time_point& begin, uint64_t tm)
{
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

        ipc::buffer buf = node->receive(wait_ms);
        if (buf.empty())
        {
            return false;
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

        meta = tentative;
        first_page = std::move(buf);
        return true;
    }
}

void send_nack_for_missing(std::shared_ptr<ipc::socket::UDPNode>& node, const chunk_meta& meta,
                           const std::vector<uint8_t>& received, IpcRtpsNackMsg& nack_msg)
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
                break;
            }
        }
    }

    if (nack_msg.missing_pages.empty())
    {
        return;
    }

    ipc::buffer nack_buf = nack_msg.serialize();
    send_chunk_with_retry(node, nack_buf);
}

ipc::buffer make_owned_copy(const void* src, std::size_t n)
{
    auto* mem = new uint8_t[n];
    std::memcpy(mem, src, n);
    return ipc::buffer(mem, n, [](void* p, std::size_t) { delete[] static_cast<uint8_t*>(p); });
}

bool recv_chunk_common(std::shared_ptr<ipc::socket::UDPNode>& node, std::shared_ptr<IpcMsgBase>& msg_ptr, uint64_t tm)
{
    const auto begin = std::chrono::steady_clock::now();

    chunk_meta meta{};
    ipc::buffer first_page;
    if (!wait_first_data_chunk(node, msg_ptr, meta, first_page, begin, tm))
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
        send_ack(node, meta, payload_crc32c);
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

    for (int round = 0; round < RTPS_MAX_NACK_ROUND && received_cnt < meta.page_cnt; ++round)
    {
        const std::size_t received_before_round = received_cnt;
        const auto round_begin = std::chrono::steady_clock::now();
        while (received_cnt < meta.page_cnt)
        {
            const uint64_t round_used = elapsed_ms(round_begin);
            if (round_used >= round_wait_ms)
            {
                break;
            }

            uint64_t wait_ms = round_wait_ms - round_used;
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

            ipc::buffer page = node->receive(wait_ms);
            if (page.empty())
            {
                break;
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

        /* round 0 收的是发送端的首轮突发，此时还没发过 NACK，不计入判据。
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

        if (effective_tm != ipc::invalid_value && elapsed_ms(assembly_begin) >= effective_tm)
        {
            record_fragment_gaps(received, meta.page_cnt, false);
            return false;
        }

        send_nack_for_missing(node, meta, received, nack_msg);
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
    send_ack(node, meta, payload_crc32c);
    return true;
}
}   // namespace

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool chunk_rev_topic(std::shared_ptr<ipc::socket::UDPNode>& node, std::shared_ptr<TopicData>& rev_msg, uint64_t tm)
{
    std::shared_ptr<IpcMsgBase> msg_ptr = rev_msg->topic();
    return recv_chunk_common(node, msg_ptr, tm);
}

bool chunk_rev_server(std::shared_ptr<ipc::socket::UDPNode>& node, std::shared_ptr<ServiceData>& rev_msg, uint64_t tm,
                      bool ser_or_cli)
{
    std::shared_ptr<IpcMsgBase> msg_ptr = ser_or_cli ? rev_msg->request() : rev_msg->response();
    return recv_chunk_common(node, msg_ptr, tm);
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
    if (options.integrity == SocketIntegrityMode::CRC32C)
    {
        report.crc32c = dzIPC::common::crc32c(publish_data.data(), publish_data.size());
    }

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

    /* 入口排空：清掉上一轮 chunk_send 退出时还没来得及到达 / 处理的残留包
       （晚到的 ACK/NACK 或 self-loopback 数据片）。否则与本轮发出的包混杂后，
       由于多轮共用同一份 meta，下面 ACK 等待循环会拿旧 ACK 当本轮 ACK 用。 */
    drain_self_loopback(node);

    /* options 未显式指定时回退到全局设置 —— pub-sub 的 publish 接口不暴露
     * SocketSendOptions, 只能靠进程级开关生效。 */
    const std::size_t rate_limit =
        (options.rate_limit_bps != 0) ? options.rate_limit_bps : global_rate_limit_bps().load(std::memory_order_relaxed);
    if (!send_all_chunks(node, chunks, rate_limit))
    {
        report.status = SocketSendStatus::FailedLocalSend;
        return report;
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
        drain_self_loopback(node);
        report.status = SocketSendStatus::SentUnconfirmed;
        return report;
    }

    const chunk_meta meta{first_tail.page_cnt, first_tail.total_size, first_tail.dz_ipc_msg_id, report.sequence};
    const uint64_t nack_wait_ms = calc_nack_wait_ms(meta.page_cnt);
    /* 方案 B': 首轮 ACK 等待由实测 RTT 决定, 而非固定 ACK_FAST_WAIT_MS */
    uint64_t first_wait_ms = ack_first_wait_ms(node.get());

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
    IpcRtpsAckMsg ack_msg;

    const auto ack_begin = std::chrono::steady_clock::now();
    int round = 0;
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
                drain_self_loopback(node);
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
                    drain_self_loopback(node);
                    report.status = SocketSendStatus::FailedTimeout;
                    return report;
                }
                wait_ms = std::min(wait_ms, ack_timeout_ms - total_used);
            }

            ipc::buffer recv_buf = node->receive(wait_ms);
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
                            rtt_of(node.get()).observe(static_cast<uint64_t>(rtt));
                        }
                    }
                    break;
                }
                continue;
            }
            if (!nack_msg.check_id(recv_buf))
            {
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
            drain_self_loopback(node);
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
            ++round;
            continue;
        }

        std::size_t resent = 0;
        for (uint16_t miss : missing_union)
        {
            if (!send_chunk_with_retry(node, chunks[miss - 1]))
            {
                report.status = SocketSendStatus::FailedLocalSend;
                return report;
            }
            if ((++resent % SEND_BURST_BEFORE_YIELD) == 0 && resent < missing_union.size())
            {
                std::this_thread::yield();
            }
        }
        ++round;

        if (options.delivery == SocketDeliveryMode::BestEffort && round >= RTPS_MAX_NACK_ROUND)
        {
            break;
        }
    }

    /* 所有 ACK round 走完仍未拿到 ACK（典型 5ms 静默退出）。本轮发出去的
       self-loopback 数据片此时还在队列里，必须清掉再返回。 */
    drain_self_loopback(node);
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
    /* 当前没有调用方 —— dzIPC 现有的两种拓扑都不满足它的前提。
     *
     * 前提: 发送端等 ACK 的那条 socket 上, 不能有自己发出的分片回绕。
     *
     * 但 pub-sub 和 ser-cli 用的都是组播 socket 且 IP_MULTICAST_LOOP=1:
     *   - pub-sub: 收发共用一条组播 socket, 自己的分片全部回绕堵在 ACK 之前;
     *              且 1:N 下"收到某一个 ACK"无法表达全体订阅者的接收状态。
     *   - ser-cli: 虽然请求/响应分了两条通道(port_hash_ / port_hash_+1), 但每条
     *              通道内部仍是收发共用。客户端在 ipc_r_ptr_ 上发请求又在同一个
     *              ipc_r_ptr_ 上等 ACK, 同样撞上 loopback 回绕。
     *              (曾据此判断 ser-cli"双通道所以安全"并改用本函数, 实测全尺寸
     *               失败 —— 双通道分的是方向, 不是收发。)
     *
     * 保留本函数是为了将来接入真正有独立反向通道的传输(如单播 TCP/UDP)。
     * 在组播拓扑上要提高大包可靠性, 正确手段是发送端限速(见 rate_limit_bps),
     * 避免瞬时灌满对端 SO_RCVBUF —— 实测丢包是缓冲溢出型突发, 不是随机丢包。 */
    SocketSendOptions options;
    options.delivery = SocketDeliveryMode::Reliable;
    /* 多分片消息启用 CRC32C: 分片重组后校验整体完整性。单分片时开销可忽略,
     * 多分片时能挡住"分片都到齐但内容错位"这类静默损坏。 */
    options.integrity = SocketIntegrityMode::CRC32C;
    options.ack_timeout_ms = ack_timeout_ms;
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

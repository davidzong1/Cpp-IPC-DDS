/* W09 chunk 容量 / 队列 / 背压联动 —— 可执行判据（容量模型与压测的**新增**部分）。
 *
 * 交付对应：docs/消息接收架构改造/团队改造交付/W09/容量与背压_交付.md。
 * 本文件只覆盖既有四个用例（test_chunk_hold / test_adopt_loan_quota /
 * test_alloc_fault_inject / test_shm_ser_backpressure）**没有**覆盖的判据：
 *
 *   C1 每尺寸档可用块数 == ipc::large_msg_cache，且**第 N+1 个 loan 被拒绝**（不是阻塞、
 *      不是崩），全部归还后能重新借到 —— 「chunk 最终归还」的最小判据。
 *   C2 **池的跨话题共享范围**：同一 (prefix, chunk_size) 是一个池；两个话题共用它；
 *      带不同 prefix 的同一档是**另一个**池 —— 归属键就是 (prefix, chunk_size)。
 *   C3 **尺寸档竞争 / 双重取整**：同一逻辑载荷在 loan 路径与 TLV send 路径落进**不同**
 *      档（loan_size_class ∘ calc_chunk_size vs calc_chunk_size）⇒ 同进程两个池并存。
 *      这是「读错档位不会报错、只会静默读到另一个池」的可执行形态。
 *   C4 **广播共享 chunk 不按订阅者重复计数**：1 话题 3 个接收方，一条大消息只占 **1** 块。
 *      （但慢消费者会延长保留时间 —— 见 C5。）
 *   C5 **接收方不 recv ⇒ 每条大消息钉 1 块直到池空**；池空后 send **仍返回 true**（静默
 *      降级），这是「队列/未消费持样」这一容量来源的直接形态。
 *   C6 **B 贷款失败 ≠ A/TLV 发送回退**（合同第 (5) 条）：池耗尽时 `loan` 返回**无效**
 *      （拒绝语义，调用方必须回退整包），而 `send` 退化成分片并**返回成功**。两者是
 *      不同失败类别，不得混计、也不得假定「所有失败都能转成 64B 分片」。
 *   C9 **段级复位的"首次 attach"判定不得与同进程在飞借样竞争**（t73 新增，R1/S4-W09-F1 的常驻回归）：
 *      同进程 8 个话题共用同一 (prefix, chunk_size) 池并**并发首次借样**时，任何两块借样
 *      都不得拿到相同 `loan_t::id` 或相同 `data` 指针。失效形态：复位把另一线程刚借出的
 *      id 重新置为空闲 ⇒ **同一块 chunk 被两块借样同时持有**（数据面正确性缺陷，静默）。
 *   C7 **「策略性回退」与「内存安全越界」分开判定**（队长 D-11 与任务书硬要求）：
 *      退化后的分片数 = ceil(size/64)；当它 > 环槽位(256) 时环**必然覆写**，该情形记
 *      为独立的「越界风险」类别，**不是**回退计数的一部分。本用例到此为止，**不** drain
 *      （读被套圈的槽位会踩进 docs/dzflat_shm.md §9.5 末尾登记的既存缺陷，不在本包射程）。
 *
 * 为什么这些必须常驻自动回路：它们的失效方式全部静默 —— 池空不报错、档位读错不报错、
 * 广播重复计数与共享计数在功能上**看不出区别**（消息照样送达），而容量模型会因此整
 * 体系地偏错；一旦偏错，背压策略与 DZFlat 默认策略都会建立在错误的余量上。
 *
 * 段隔离纪律：每条用例用**独立 prefix** ⇒ 段名不同 ⇒ 物理上是不同的池，因此不占用
 * 默认池的任何一档，也不会被其他用例的残段污染（同 test_pool_exhaust_observability
 * 第三臂的做法）。开头 clear_storage 让每次运行都从「满池」开始。
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <signal.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <vector>

#include "gtest/gtest.h"

#include "libipc/def.h"
#include "libipc/ipc.h"
#include "libipc/shm.h"

namespace {

/* ⛔ 由常量导出，不写死魔数：容量一变（改 large_msg_cache）写死会让全部判据失真。 */
constexpr std::size_t kChunkPoolSize = static_cast<std::size_t>(ipc::large_msg_cache);
/* 环槽位数（circ::elem_array::elem_max = uint8 上限 + 1）。超过它就意味着生产者绕过
 * 一整圈 ⇒ force_push 覆写已发生或即将发生。 */
constexpr std::size_t kRingSlots = 256;
/* 走 chunk 路径的门槛：> large_msg_limit(=64) 才会 acquire chunk。 */
constexpr std::size_t kLargeMsgLimit = static_cast<std::size_t>(ipc::large_msg_limit);

/* 与 ipc.cpp 的 calc_chunk_size / loan_size_class **同规则**的本地副本。
 * ⛔ 抄写而非调用：那两个函数在 ipc.cpp 的匿名 namespace 里，没有对外链接。
 * 用途是把「请求长度」映射到「池档位」，好让判据给出**算出来的**期望值而不是"看着像"。 */
constexpr std::size_t align_up(std::size_t x, std::size_t a) noexcept
{
    return ((x + a - 1) / a) * a;
}
constexpr std::size_t calc_chunk_size(std::size_t size) noexcept
{
    /* align_chunk_size(16 + size) 再按 alignof(max_align_t)=16 取整。 */
    return align_up(align_up(16 + size, static_cast<std::size_t>(ipc::large_msg_align)),
                    alignof(std::max_align_t));
}
constexpr std::size_t loan_size_class(std::size_t size) noexcept
{
    if (size <= 64 * 1024)
    {
        return align_up(size, static_cast<std::size_t>(ipc::large_msg_align));
    }
    std::size_t c = 128 * 1024;
    while (c < size) c <<= 1;
    return c;
}
/* 借样路径的**实际**档位 = 两层取整叠加（这是 C3 的被测点）。 */
constexpr std::size_t borrowed_chunk_class(std::size_t size) noexcept
{
    return calc_chunk_size(loan_size_class(size));
}

std::string unique_topic(const char* tag)
{
    static std::atomic<unsigned> n{0};
    /* ⛔ 段名不得含内部 '/'（POSIX shm_open 直接 EINVAL）—— 与既有用例同款：
     * 只用 [A-Za-z0-9_] 的短名。 */
    return std::string("w09_") + tag + "_" + std::to_string(n.fetch_add(1));
}
std::string unique_prefix(const char* tag)
{
    static std::atomic<unsigned> n{0};
    return std::string("w09_") + tag + "_" + std::to_string(n.fetch_add(1));
}

/* 一个隔离的池 + 一条在连的 route 对（发送端可 loan / send）。
 * 析构顺序：先 route 再清段（与既有用例同款纪律）。 */
struct PoolProbe
{
    std::string prefix;
    std::string topic;
    std::unique_ptr<ipc::route> tx;
    std::unique_ptr<ipc::route> rx;

    PoolProbe(const char* tag, std::size_t receivers = 1)
        : prefix(unique_prefix(tag))
        , topic(unique_topic(tag))
    {
        ipc::route::clear_storage(ipc::prefix{prefix.c_str()}, topic.c_str());
        tx = std::make_unique<ipc::route>(ipc::prefix{prefix.c_str()}, topic.c_str(), ipc::sender);
        for (std::size_t i = 0; i < receivers; ++i)
        {
            extra.emplace_back(
                std::make_unique<ipc::route>(ipc::prefix{prefix.c_str()}, topic.c_str(), ipc::receiver));
        }
        rx = std::make_unique<ipc::route>(ipc::prefix{prefix.c_str()}, topic.c_str(), ipc::receiver);
    }

    ~PoolProbe()
    {
        rx.reset();
        extra.clear();
        tx.reset();
        ipc::route::clear_storage(ipc::prefix{prefix.c_str()}, topic.c_str());
    }

    bool wait_receivers(std::size_t n)
    {
        return tx->wait_for_recv(static_cast<std::size_t>(n), 3000);
    }

    std::vector<std::unique_ptr<ipc::route>> extra;   ///< 额外的接收方（广播用例）

private:
    PoolProbe(const PoolProbe&) = delete;
    PoolProbe& operator=(const PoolProbe&) = delete;
};

/* 借到借不动为止 ⇒ 返回「该档当前可用块数」，并把借到的块留在 held 里（调用方负责归还）。
 * ⛔ 上限 kChunkPoolSize+8：容量异常变大时用例会显式失败，而不是借到天荒地老。 */
std::size_t take_available_loans(ipc::route& tx, std::size_t size, std::vector<ipc::loan_t>& held)
{
    for (std::size_t i = 0; i < kChunkPoolSize + 8; ++i)
    {
        auto lo = tx.loan(size);
        if (!lo.valid()) break;
        held.push_back(lo);
    }
    return held.size();
}

void release_all(ipc::route& tx, std::vector<ipc::loan_t>& held)
{
    for (const auto& lo : held) tx.discard_loan(lo);
    held.clear();
}

std::vector<std::uint8_t> make_payload(std::size_t n, std::uint8_t fill)
{
    return std::vector<std::uint8_t>(n, fill);
}

bool contains(const std::string& hay, const char* needle)
{
    return hay.find(needle) != std::string::npos;
}

/* ── 池占用的**外部直读**（通道 A）────────────────────────────────────────────
 * 段是 tmpfs 文件：进程 mmap 的同时可按文件读同一份内存。段内布局 = chunk_info_t，
 * 其首成员 id_pool<> 的 next_[capacity] 占偏移 0..capacity-1（每项 1 字节），
 * cursor_ 紧随其后 ⇒ 读 [0, capacity+1) 即可沿空闲链数出**空闲块数**。
 *
 * ⛔ 为什么必须走链而不是 `capacity - cursor_`：cursor_ 是**空闲链头**不是借出计数
 *   （docs/shm_chunk_pool_occupancy_plan.md §3 步骤② 的订正）。用错公式会在"池彻底空闲"
 *   时读出"全部在用"（方向完全相反）。
 * ⛔ 必须限步：链若被破坏（自环）会死循环 —— 量具把被测进程挂住比读错更糟。
 * 返回 nullopt = 段不存在（从未取过块 ⇒ 等价全空闲）或读失败。 */
std::optional<std::size_t> pool_free_blocks(const std::string& prefix, std::size_t chunk_class)
{
    std::string path = "/dev/shm/";
    if (!prefix.empty()) path += prefix + "__IPC_SHM__";
    path += "CHUNK_INFO__" + std::to_string(chunk_class) + "__C"
            + std::to_string(kChunkPoolSize);
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return std::nullopt;   // 段不存在 ⇒ 从未取块
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return std::nullopt;
    std::vector<unsigned char> b(kChunkPoolSize + 1, 0);
    const std::size_t got = std::fread(b.data(), 1, b.size(), f);
    std::fclose(f);
    if (got < b.size()) return std::nullopt;
    unsigned cursor = b[kChunkPoolSize];
    std::size_t n = 0;
    while (cursor < kChunkPoolSize && n <= kChunkPoolSize)
    {
        cursor = b[cursor];
        ++n;
    }
    if (n > kChunkPoolSize) return std::nullopt;   // 链被破坏（自环）⇒ 本次读数作废
    return n;
}

/* 与 test_pool_exhaust_observability 同款：捕获 stderr，任何路径都取回（gtest 的
 * CaptureStderr 不可重入，中途 return 会把捕获留在打开态并污染后续用例）。 */
class StderrCapture
{
public:
    StderrCapture() { testing::internal::CaptureStderr(); }
    ~StderrCapture()
    {
        if (!done_) testing::internal::GetCapturedStderr();
    }
    std::string finish()
    {
        done_ = true;
        return testing::internal::GetCapturedStderr();
    }

private:
    bool done_ = false;
};

}   // namespace

/* ---------------------------------------------- C1 每档块数与「拒绝 + 可归还」 */

TEST(ChunkCapacityBackpressure, PerClassBlockCountIsLargeMsgCacheAndExhaustionIsRefusalNotBlock)
{
    PoolProbe p("c1");
    ASSERT_TRUE(p.wait_receivers(1)) << "接收方未在超时内连上";

    /* 用 loan 路径的档位（请求 8000 ⇒ loan 档 8192 ⇒ chunk 档 9216）。 */
    constexpr std::size_t kReq = 8000;
    std::vector<ipc::loan_t> held;
    const std::size_t avail = take_available_loans(*p.tx, kReq, held);
    EXPECT_EQ(avail, kChunkPoolSize)
        << "每尺寸档可用块数必须是 ipc::large_msg_cache（实测 " << avail << "，期望 "
        << kChunkPoolSize << "）";

    /* 第 N+1 次是**拒绝**（返回无效），不是阻塞、不是崩。 */
    auto denied = p.tx->loan(kReq);
    EXPECT_FALSE(denied.valid()) << "池空后必须返回无效 loan（拒绝语义），不得阻塞或崩溃";

    /* chunk 最终归还：全部 discard 后必须能重新借到同样多。 */
    release_all(*p.tx, held);
    std::vector<ipc::loan_t> again;
    const std::size_t avail2 = take_available_loans(*p.tx, kReq, again);
    EXPECT_EQ(avail2, kChunkPoolSize) << "全部归还后可用块数未回到 " << kChunkPoolSize
                                      << "（实测 " << avail2 << "）—— chunk 未真正回到池里";
    release_all(*p.tx, again);

    std::printf("[W09-C1] 档 %zu 可用块数=%zu（loan_size_class(%zu)=%zu ⇒ calc=%zu）\n",
                calc_chunk_size(loan_size_class(kReq)), avail, kReq, loan_size_class(kReq),
                borrowed_chunk_class(kReq));
}

/* ------------------------- C2 池的共享范围：同档跨话题共享，不同 prefix 独立 */

TEST(ChunkCapacityBackpressure, PoolIsSharedAcrossTopicsOfTheSameSizeClassButSeparatedByPrefix)
{
    /* 同一 prefix 下两个**不同话题**，共用同一 (prefix, chunk_size) 池。 */
    const std::string prefix = unique_prefix("c2");
    const std::string topic_a = unique_topic("c2a");
    const std::string topic_b = unique_topic("c2b");
    ipc::route::clear_storage(ipc::prefix{prefix.c_str()}, topic_a.c_str());
    ipc::route::clear_storage(ipc::prefix{prefix.c_str()}, topic_b.c_str());

    constexpr std::size_t kReq = 8000;   // loan 档 9216
    {
        ipc::route tx_a{ipc::prefix{prefix.c_str()}, topic_a.c_str(), ipc::sender};
        ipc::route rx_a{ipc::prefix{prefix.c_str()}, topic_a.c_str(), ipc::receiver};
        ipc::route tx_b{ipc::prefix{prefix.c_str()}, topic_b.c_str(), ipc::sender};
        ipc::route rx_b{ipc::prefix{prefix.c_str()}, topic_b.c_str(), ipc::receiver};
        ASSERT_TRUE(tx_a.wait_for_recv(1, 3000));
        ASSERT_TRUE(tx_b.wait_for_recv(1, 3000));

        /* A 借满整档（40 块）。 */
        std::vector<ipc::loan_t> held_a;
        ASSERT_EQ(take_available_loans(tx_a, kReq, held_a), kChunkPoolSize);

        /* B 同档 ⇒ B 也借不到 ⇒ **池是跨话题共享的**（不是每话题一池）。 */
        auto cross = tx_b.loan(kReq);
        EXPECT_FALSE(cross.valid())
            << "同 (prefix, chunk_size) 的两个话题必须共用同一池：A 借满后 B 不应还能借到";

        /* A 归还 ⇒ B 立刻能借到（同一池的另一个方向）。 */
        release_all(tx_a, held_a);
        auto after = tx_b.loan(kReq);
        EXPECT_TRUE(after.valid()) << "A 归还后 B 仍借不到 —— 两者不是同一个池";
        tx_b.discard_loan(after);
    }
    ipc::route::clear_storage(ipc::prefix{prefix.c_str()}, topic_a.c_str());
    ipc::route::clear_storage(ipc::prefix{prefix.c_str()}, topic_b.c_str());

    /* 不同 prefix、同一档 ⇒ 是另一个池：一个被借满不影响另一个。 */
    PoolProbe pa("c2pa");
    PoolProbe pb("c2pb");
    ASSERT_TRUE(pa.wait_receivers(1));
    ASSERT_TRUE(pb.wait_receivers(1));
    std::vector<ipc::loan_t> held_a;
    ASSERT_EQ(take_available_loans(*pa.tx, kReq, held_a), kChunkPoolSize);
    auto other = pb.tx->loan(kReq);
    EXPECT_TRUE(other.valid())
        << "不同 prefix 的同一档必须是**独立**的池（归属键 = prefix + chunk_size）";
    if (other.valid()) pb.tx->discard_loan(other);
    release_all(*pa.tx, held_a);
}

/* ---------------------- C3 尺寸档竞争 / 双重取整：同一载荷落两个不同的池 */

TEST(ChunkCapacityBackpressure, SamePayloadLandsInDifferentSizeClassesForLoanAndTlvSend)
{
    PoolProbe p("c3");
    ASSERT_TRUE(p.wait_receivers(1));

    constexpr std::size_t kPayload = 10000;
    /* 算出来的期望（不是"看着像"）： */
    const std::size_t tlv_class = calc_chunk_size(kPayload);                 // TLV send 路径
    const std::size_t loan_class = borrowed_chunk_class(kPayload);           // loan 路径（两层取整）
    ASSERT_NE(tlv_class, loan_class)
        << "本用例的前提是两路径落不同档；当前 " << tlv_class << " vs " << loan_class;

    /* 借满 **loan** 档。 */
    std::vector<ipc::loan_t> held;
    ASSERT_EQ(take_available_loans(*p.tx, kPayload, held), kChunkPoolSize);
    EXPECT_FALSE(p.tx->loan(kPayload).valid()) << "loan 档应已耗尽";

    /* 同一逻辑载荷的 TLV send 仍能成功 ⇒ 它走的是**另一个**档的池。
     * ⛔ 这正是不看档位就会读错的地方：读数不会报错，只会静默读到另一个池。 */
    const auto payload = make_payload(kPayload, 0x5A);
    EXPECT_TRUE(p.tx->send(payload.data(), payload.size()))
        << "loan 档耗尽不应影响 TLV send —— 两者是不同尺寸档的两个池";

    release_all(*p.tx, held);
    std::printf("[W09-C3] 载荷 %zu B：TLV 档=%zu，loan 档=%zu（loan_size_class=%zu）\n",
                kPayload, tlv_class, loan_class, loan_size_class(kPayload));
}

/* --------------------- C4 广播共享 chunk 不按订阅者重复计数（1 条 = 1 块） */

TEST(ChunkCapacityBackpressure, BroadcastMessagePinsOneChunkRegardlessOfReceiverCount)
{
    constexpr std::size_t kReceivers = 4;   // 1 主 rx + 3 额外 rx
    /* ⛔ 载荷必须取 1KB 对齐值，否则 TLV 路径与 loan 路径落**不同档**（C3 的发现），
     *    本用例的"池占用几块"就量在另一个池上了。calc(8192)=9216=borrowed_class(8192)。 */
    constexpr std::size_t kPayload = 8192;

    PoolProbe p("c4", kReceivers - 1);
    ASSERT_TRUE(p.wait_receivers(kReceivers)) << "四个接收方未全部连上";

    /* 基线：整档可用块数。 */
    std::vector<ipc::loan_t> probe;
    const std::size_t before = take_available_loans(*p.tx, kPayload, probe);
    ASSERT_EQ(before, kChunkPoolSize);
    release_all(*p.tx, probe);

    /* 发一条大消息，**所有接收方都不 recv**（conns 位图不清 ⇒ chunk 留在池外）。 */
    const auto payload = make_payload(kPayload, 0x3C);
    ASSERT_TRUE(p.tx->send(payload.data(), payload.size()));

    std::vector<ipc::loan_t> after;
    const std::size_t avail_after = take_available_loans(*p.tx, kPayload, after);
    release_all(*p.tx, after);

    EXPECT_EQ(avail_after, before - 1)
        << "广播的一条消息只应占 **1** 块（实测少 " << (before - avail_after) << " 块）；"
           "按订阅者重复计数会让容量模型把余量少算 " << kReceivers - 1 << " 倍";

    /* ⚠️ 但慢消费者会**延长保留时间**：4 个接收方都不读 ⇒ 这一块在本用例结束前不会归还。
     * 本用例不 drain，故不触碰覆写路径。 */
    std::printf("[W09-C4] 广播 1 条 / %zu 接收方：池占用 %zu 块（期望 1）\n", kReceivers,
                before - avail_after);
}

/* ------------- C5+C6 未消费持样逐条钉块；B 贷款失败 ≠ A/TLV 发送回退 */

TEST(ChunkCapacityBackpressure, UnconsumedHoldingDrainsPoolAndBorrowFailureIsNotFragmentFallback)
{
    PoolProbe p("c5");
    ASSERT_TRUE(p.wait_receivers(1));

    /* 同 C4：取 1KB 对齐载荷，使 TLV 与 loan 落同一档（否则两路径量的是两个池）。 */
    constexpr std::size_t kPayload = 8192;
    const std::size_t tlv_class = calc_chunk_size(kPayload);
    ASSERT_EQ(tlv_class, borrowed_chunk_class(kPayload)) << "本用例要两路径同档才可比";

    const auto payload = make_payload(kPayload, 0xC3);

    /* 接收方**从不 recv** ⇒ 每条大消息各占一块，直到池空。 */
    StderrCapture cap;
    std::size_t accepted = 0;
    for (std::size_t i = 0; i < kChunkPoolSize; ++i)
    {
        ASSERT_TRUE(p.tx->send(payload.data(), payload.size())) << "第 " << i << " 条发送失败";
        ++accepted;
    }
    /* 第 N+1 条：池已空 ⇒ **仍返回 true**（静默退化成分片），这是既有语义，不是 bug。 */
    EXPECT_TRUE(p.tx->send(payload.data(), payload.size()))
        << "池空时 send 必须仍然成功（静默降级），本判据把这条语义钉死";

    const std::string err = cap.finish();
    if (!err.empty()) std::printf("[W09-C5] captured stderr:\n%s\n", err.c_str());
    EXPECT_TRUE(contains(err, "chunk pool exhausted"))
        << "池空必须可观测（note_pool_exhausted），捕获到的 stderr:\n" << err;
    EXPECT_TRUE(contains(err, "kind = send")) << err;
    EXPECT_TRUE(contains(err, std::string("chunk_size = " + std::to_string(tlv_class)).c_str()))
        << "报的档位必须是实际的 TLV 档 " << tlv_class << "：\n" << err;

    /* C6：同一个池耗尽态下，**loan（B 路径）失败**与**send（A/TLV）成功**是两种不同结局。
     * ⛔ 这就是「不得假定所有失败都能转为 64B 分片」的可执行形态：
     *    · loan 失败 ⇒ 调用方必须回退到整包 publish（不是分片）；
     *    · send 成功但降级 ⇒ 分片。 */
    auto denied = p.tx->loan(kPayload);
    EXPECT_FALSE(denied.valid())
        << "池耗尽时 loan 必须返回无效（拒绝语义）—— 借样失败是背压，不是分片回退";
    std::printf("[W09-C6] 同池耗尽态：loan 无效=%d，send 仍成功=%d（两种不同失败类别）\n",
                denied.valid() ? 0 : 1, 1);

    (void)accepted;
    /* ⛔ 刻意不 drain：接收方已被套圈，读空会踩进既存缺陷（不在本包射程）。 */
}

/* ------------------- C7 「策略性回退」与「内存安全越界」分开判定（对账算术） */

TEST(ChunkCapacityBackpressure, DegradeFragmentCountSeparatesStrategicFallbackFromRingOverwrite)
{
    PoolProbe p("c7");
    ASSERT_TRUE(p.wait_receivers(1));

    /* 0.88 MB 量级载荷（dzflat_shm §5.3 / §9.5 末尾那条既存缺陷的现场规模）。 */
    constexpr std::size_t kBig = 900 * 1024;
    const std::size_t big_class = calc_chunk_size(kBig);
    ASSERT_GT(big_class, calc_chunk_size(8000)) << "本用例要一个独立的大档";

    /* 先用同档的小载荷把这一档打满（接收方不读）。 */
    const auto filler = make_payload(8000, 0x11);
    std::size_t filler_accepted = 0;
    for (std::size_t i = 0; i < kChunkPoolSize + 2; ++i)
    {
        if (!p.tx->send(filler.data(), filler.size())) break;
        ++filler_accepted;
    }
    ASSERT_GE(filler_accepted, kChunkPoolSize) << "未能把同档池打满（实测 " << filler_accepted << "）";

    /* 现在发一条大消息：池空 ⇒ 策略性回退成分片，**且返回 true**。 */
    const auto big = make_payload(kBig, 0xE7);
    const bool sent = p.tx->send(big.data(), big.size());

    /* ── 两个**独立**类别的对账（不得混成一个"失败数"）────────────────────────
     * ① 策略性回退：由 chunk 池耗尽触发、经降级路径仍交付；计数口 = note_pool_exhausted。
     *    它是一条**已批准**的既有语义（本包不改默认语义）。
     * ② 内存安全越界风险：退化后的分片条数 = ceil(size / data_length)。分片数 > 环槽位
     *    (256) 时，生产者必然绕行整圈 ⇒ force_push 覆写仍在被读/被持有的格子。
     *    这是**缺陷类别**（docs/dzflat_shm.md §5.1/§9.5 末尾），不是性能现象。 */
    const std::size_t fragments = (big.size() + kLargeMsgLimit - 1) / kLargeMsgLimit;
    const bool ring_overwrite_risk = fragments > kRingSlots;

    EXPECT_TRUE(sent) << "池空时大消息发送应走策略性回退（返回 true）";
    EXPECT_GT(fragments, kRingSlots)
        << "对账算术本身要成立：0.88MB 级载荷的退化分片数(" << fragments << ") 必须超过环槽位("
        << kRingSlots << ") —— 否则'越界风险'这一类别就无从判定";
    EXPECT_TRUE(ring_overwrite_risk);

    std::printf("[W09-C7] 策略性回退=1（send 返回 true）｜越界风险=%d（fragments=%zu > ring=%zu，"
                "独立类别，不计入回退）；大档=%zu\n",
                ring_overwrite_risk ? 1 : 0, fragments, kRingSlots, big_class);

    /* ⛔ 不 drain：读被套圈的槽位会踩进上述既存缺陷。本用例只做**判定与对账**，
     *    修复该缺陷不属 W09 射程（任务书硬要求：不得用它解释自身失败）。 */
}

/* ---------------------- C8 chunk 最终归还：外部直读池占用回到基线（±0） */

/* 判据：把一整档借满（占用 = kChunkPoolSize）→ 全部归还 → **外部直读**该档池的
 * 空闲链回到满值。这是「chunk 最终归还」的**独立**证据（不依赖进程内计数，也不依赖
 * 我们自己的记账），也顺带把 C1 的"能再借到"升级成"从池的状态看真的还回去了"。
 *
 * ⛔ 用独立 prefix + 1KB 对齐载荷，落到一个**本用例独有**的档，避免读到别人的池。 */
TEST(ChunkCapacityBackpressure, PoolOccupancyReturnsToBaselineAfterRelease)
{
    PoolProbe p("c8");
    ASSERT_TRUE(p.wait_receivers(1));

    constexpr std::size_t kPayload = 8192;
    const std::size_t cls = calc_chunk_size(kPayload);
    ASSERT_EQ(cls, borrowed_chunk_class(kPayload));

    /* 起始基线（段可能已存在：本用例自己上一次运行的残段 ⇒ 必须显式核到满值）。 */
    const auto before = pool_free_blocks(p.prefix, cls);
    if (!before.has_value())
    {
        std::printf("[W09-C8] 段尚未创建（本用例首次取块前）—— 基线按满值 %zu 处理\n",
                    kChunkPoolSize);
    }
    else
    {
        EXPECT_EQ(*before, kChunkPoolSize)
            << "起始空闲块数应为满值（残段会让本条退化成恒真）";
    }

    std::vector<ipc::loan_t> held;
    ASSERT_EQ(take_available_loans(*p.tx, kPayload, held), kChunkPoolSize);

    const auto during = pool_free_blocks(p.prefix, cls);
    ASSERT_TRUE(during.has_value()) << "借满后段必然存在";
    EXPECT_EQ(*during, std::size_t{0})
        << "借满整档后空闲链应为 0（实测 " << *during << "）—— 外部直读与进程内计数互证";

    release_all(*p.tx, held);
    /* 归还可能有微小延迟（release 是同步的，这里只留一次重读机会）。 */
    std::optional<std::size_t> after = pool_free_blocks(p.prefix, cls);
    for (int i = 0; i < 50 && (!after.has_value() || *after != kChunkPoolSize); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        after = pool_free_blocks(p.prefix, cls);
    }
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(*after, kChunkPoolSize)
        << "全部归还后该档池必须回到满空闲（实测 " << *after << "）—— 漏还一块就会少一块";
    std::printf("[W09-C8] 档 %zu 外部直读：借满=%zu → 归还后空闲=%zu（满值 %zu）\n", cls, *during,
                *after, kChunkPoolSize);
}

/* ───────────────────────── C9 段级复位 vs 同进程并发首借（t73）─────────────────────────
 *
 * 背景（R1/S4 复核 W09-S4-F1，high）：t22 的段级复位 `reclaim_orphan_segment()` 声称
 * 前提①"首次 attach 时本进程必然未持有本档 chunk"**天然满足**。该说法**不成立**：
 * `get_info()` 在 `handles_[pref]` 的 `lock_` **释放之后**才去取池快照 ⇒ 同进程另一线程
 * 在这条缝里经同一 `handles_[pref]` 借出 id#0，而随后的"字节镜像复核"看到正是被改动的池
 * ⇒ 复核通过 ⇒ 复位把**别人正持有的 id 重新发出去**。
 *
 * 本用例是该反例的**常驻形态**（原反例 `R1/S4验收_W09_证据/repro_alias.cpp` 为一次性件）：
 *   · 同一 prefix、**8 个不同话题**、每话题一个生产者（`ipc::route` 默认单生产端，合法）；
 *   · 8 个线程用栅栏**同时**首发 `loan(8000)`（⇒ 档 9216）⇒ 竞争"首次 attach 判定"；
 *   · 判据：任意两块借样的 `id` 或 `data` **不得相同**（`same_data_ptr` 直接证明两块内存别名）。
 *
 * 为什么用**独立 prefix** 而不是默认空前缀：机制与归属键都只取决于
 * `(prefix, chunk_size)`，用独立 prefix 能**逐字复现同一代码路径**（同一 prefix 的多个话题
 * 就是同一个池），同时⛔不占用全机共享的默认池、也不会被邻居污染（与本文件其余用例同纪律）。
 *
 * 为什么必须常驻：命中是概率性的（复核时实测 4/120），而后果是**静默的数据面错误** ——
 * 两条被分别写不同字节的消息指向同一内存。⛔ 不能靠一次性复现件守。
 */
/* 判据专用前缀（⛔ 不用空前缀）：空前缀的 9216 段是**全机共享**的 ⇒ 并发邻居的借样/复位
 * 会把噪声引进判据（t73 实测：600 轮下曾出现 2/10 假红，随后 50 次全绿 ⇒ 判据受邻居干扰）。
 * 用专属前缀仍**逐字复现同一代码路径**（复位的进入条件 = "首次 attach 一个已被用过的段"，
 * 与 prefix 取值无关），但把干扰面收窄到本用例自身。 */
namespace {

constexpr char const *kAliasPrefix = "w09c9alias";

int alias_round_in_fresh_process(int seed)
{
    constexpr int kTopics = 8;
    std::vector<std::unique_ptr<ipc::route>> rx;
    std::vector<std::unique_ptr<ipc::route>> tx;
    for (int i = 0; i < kTopics; ++i)
    {
        const std::string topic = "w09c9_" + std::to_string(seed) + "_t" + std::to_string(i);
        rx.emplace_back(new ipc::route{ipc::prefix{kAliasPrefix}, topic.c_str(), ipc::receiver});
        tx.emplace_back(new ipc::route{ipc::prefix{kAliasPrefix}, topic.c_str(), ipc::sender});
    }
    for (int i = 0; i < kTopics; ++i)
    {
        if (!tx[(std::size_t)i]->wait_for_recv(1, 3000)) return 3;
    }
    std::atomic<int> go{0};
    std::mutex m;
    struct Owned
    {
        int topic;
        ipc::loan_t lo;
    };
    std::vector<Owned> held;
    std::vector<std::thread> th;
    for (int i = 0; i < kTopics; ++i)
    {
        th.emplace_back(
            [&, i]
            {
                while (go.load(std::memory_order_acquire) == 0)
                {
                }
                auto lo = tx[(std::size_t)i]->loan(8000);
                if (lo.valid())
                {
                    std::memset(lo.data, 'A' + i, 64);
                    std::lock_guard<std::mutex> g(m);
                    held.push_back(Owned{i, lo});
                }
            });
    }
    go.store(1, std::memory_order_release);
    for (auto& x : th) x.join();

    int si = 0;
    int sp = 0;
    for (std::size_t i = 0; i < held.size(); ++i)
    {
        for (std::size_t j = i + 1; j < held.size(); ++j)
        {
            if (held[i].topic == held[j].topic) continue;
            if (held[i].lo.id == held[j].lo.id) ++si;
            if (held[i].lo.data == held[j].lo.data) ++sp;
        }
    }
    for (auto& h : held) tx[(std::size_t)h.topic]->discard_loan(h.lo);
    std::fflush(nullptr);
    return (si || sp) ? 1 : 0;
}

}   // namespace

/* ───────────────────────── C9 段级复位 vs 同进程并发首借（t73）─────────────────────────
 *
 * 背景（S4 复核 W09-S4-F1，high；`团队改造交付/R1/S4验收_W09.md` §2.2–§2.4）：
 * t22 的段级复位声称前提①"首次 attach 时本进程必然未持有本档 chunk"**天然满足**。
 * 该说法**不成立** —— `get_info()` 在 `handles_[pref]` 的 `lock_` **释放之后**才取池快照
 * ⇒ 同进程另一线程在这条缝里经同一 `handles_[pref]` 借出 id#0，随后的"字节镜像复核"
 * 看到的正是被改动过的池 ⇒ 复核通过 ⇒ 复位把**别人正持有的 id 重新发出去** ⇒
 * 同一块 chunk 被两块借样同时持有（静默的数据面别名）。
 *
 * 本用例是复核最小反例（`R1/S4验收_W09_证据/repro_alias.cpp`）的**常驻形态**。
 *
 * ⚠️ **为什么每一轮要 fork 一个全新进程**（这是本用例唯一"看起来多余"的设计，也是踩过的坑）：
 *   · 段级复位**只在"首次 attach 一个已被用过的段"那一刻**被调用，判据是进程内的
 *     `handles_[prefix]` 由无效变有效；
 *   · `handles_` 是**进程局部**的，且 `clear_storage`（unlink 段文件）**不会**让它失效
 *     ⇒ 同一进程内**只有第一次**能进入该路径，之后每一轮都是 `newly_attached == false` ⇒
 *     复位函数根本不跑 ⇒ 判据永远绿（**静默假绿**：我第一版就是这样，连负控都照样通过）；
 *   · 所以"每轮一个全新进程"是把"首次 attach"这件事**每轮各来一次**的唯一办法。
 *     ⛔ 不要为了"省点开销"把它改成同进程多轮 —— 那会让本用例退化成永真的假绿。
 *
 * 判据：任一子进程报告"两个不同话题拿到相同 `loan_t::id` 或相同 `data` 指针" ⇒ 失败。
 */
TEST(ChunkCapacityBackpressure, OrphanResetDoesNotAliasInflightLoansAcrossTopics)
{
    constexpr int kRounds = 1200;  ///< 命中是概率性的（复核实测 4/120 量级）⇒ 轮数要给足
    int bad = 0;
    int skipped = 0;

    /* ⛔ 开跑前把本前缀的段清干净：段是**跨轮复用**的（这正是"已被用过"形态的来源），
     * 但若不清，段会把**上一次运行**的残留状态带进来 ⇒ 判据会继承历史污染（t73 实测：
     * 负控库跑完后再跑修复库，修复库也会报红）。清一次 ≠ 每轮清：每轮的"非素净"由种子进程现造。 */
    ipc::route::clear_storage(ipc::prefix{kAliasPrefix}, "w09c9_seed");

    for (int r = 0; r < kRounds && bad == 0; ++r)
    {
        /* ① 先造出"已被用过"的段：fork 一个**借样后不归还**的种子进程，用 `_exit` 跳过析构
         * ⇒ 段留存且空闲链**非素净**（`pool_.invalid() == false`）。
         * ⛔ 这一步是本用例有牙的前提：段级复位函数首行是 `if (pool_.invalid()) return false;`
         *    （全新段早退）—— 不造非素净段，复位路径**根本不会进入**，判据就永远绿。
         * ⛔ 不能改用 `clear_storage`：那是删段 ⇒ 下一轮又成"全新段" ⇒ 同样早退。 */
        {
            const ::pid_t sp = ::fork();
            ASSERT_GE(sp, 0) << "seeder fork 失败";
            if (sp == 0)
            {
                std::vector<ipc::route> keep;
                ipc::route stx{ipc::prefix{kAliasPrefix}, "w09c9_seed", ipc::sender};
                ipc::route srx{ipc::prefix{kAliasPrefix}, "w09c9_seed", ipc::receiver};
                if (!stx.wait_for_recv(1, 3000)) ::_exit(0);
                std::vector<ipc::loan_t> held;
                for (int i = 0; i < 3; ++i)
                {
                    auto lo = stx.loan(8000);
                    if (!lo.valid()) break;
                    held.push_back(lo);
                }
                std::fflush(nullptr);
                ::_exit(0);   /* 不 discard ⇒ 段内空闲链保持非素净 */
            }
            int sst = 0;
            (void)::waitpid(sp, &sst, 0);
        }
        const ::pid_t pid = ::fork();
        ASSERT_GE(pid, 0) << "fork 失败";
        if (pid == 0)
        {
            ::_exit(alias_round_in_fresh_process(r));
        }
        int status = 0;
        const ::pid_t got = ::waitpid(pid, &status, 0);
        ASSERT_EQ(got, pid) << "waitpid 失败";
        if (!WIFEXITED(status))
        {
            FAIL() << "第 " << r << " 轮子进程异常退出（signal=" << (WIFSIGNALED(status) ? WTERMSIG(status) : -1)
                   << "）—— 段级复位引发的别名可能已把进程写崩";
        }
        const int code = WEXITSTATUS(status);
        if (code == 3)
        {
            ++skipped;
            continue;
        }
        if (code == 1) ++bad;
    }

    ::testing::Test::RecordProperty("alias_bad_rounds", std::to_string(bad));
    ::testing::Test::RecordProperty("alias_skipped_rounds", std::to_string(skipped));
    std::printf("[W09-C9] 专属前缀 %s 的 8 话题并发首借 ×%d 个独立进程：bad_rounds=%d skipped=%d\n", kAliasPrefix, kRounds, bad,
                skipped);
    EXPECT_GT(kRounds - skipped, 0) << "所有轮次都因前提不成立被跳过 ⇒ 本用例没有牙";
    EXPECT_EQ(bad, 0)
        << "段级复位把同进程在飞借样当成孤儿段残留重新发出：两个不同话题拿到相同 id / 相同 data 指针"
           " ⇒ 同一块 chunk 被两块借样同时持有（数据面别名）";
}

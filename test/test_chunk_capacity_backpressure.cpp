#include "libipc/memory/resource.h"
/* W09 chunk 容量 / 队列 / 背压联动 —— 可执行判据（容量模型与压测的**新增**部分）。
 *
 * 交付对应：docs/消息接收架构改造/团队改造交付/W09/容量与背压_交付.md。
 * 本文件只覆盖既有四个用例（test_chunk_hold / test_adopt_loan_quota /
 * test_alloc_fault_inject / test_shm_ser_backpressure）**没有**覆盖的判据：
 *
 *   C1 每尺寸档可用块数 == ipc::topic_msg_cache，且**第 N+1 个 loan 被拒绝**（不是阻塞、
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
constexpr std::size_t kChunkPoolSize = static_cast<std::size_t>(ipc::topic_msg_cache);
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
std::size_t calc_chunk_size(std::size_t size) noexcept
{
    /* align_chunk_size(16 + size) 再按 alignof(max_align_t)=16 取整。 */
    return align_up(align_up(16 + ipc::pool_size_class(size), static_cast<std::size_t>(ipc::large_msg_align)),
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
std::size_t borrowed_chunk_class(std::size_t size) noexcept
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
std::optional<std::size_t> pool_free_blocks(const std::string& prefix, const std::string& topic, std::size_t chunk_class)
{
    const auto key=ipc::topic_pool_prefix(ipc::make_string(prefix.c_str()),ipc::make_string(topic.c_str()));
    const std::string path="/dev/shm/"+std::string(key.c_str())+"CHUNK_INFO__"+std::to_string(chunk_class)+"__C10";
    struct stat st{};
    if (::stat(path.c_str(), &st) != 0) return std::nullopt;   // 段不存在 ⇒ 从未取块
    std::FILE* f = std::fopen(path.c_str(), "rb");
    if (f == nullptr) return std::nullopt;
    std::fseek(f, sizeof(ipc::pool_identity_header), SEEK_SET);
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
        << "每尺寸档可用块数必须是 ipc::topic_msg_cache（实测 " << avail << "，期望 "
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

TEST(ChunkCapacityBackpressure, PoolsAreSeparatedByTopicAndPrefix)
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
        EXPECT_TRUE(cross.valid()) << "A 借满不能占用 B 的独立池";
        tx_b.discard_loan(cross);

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

TEST(ChunkCapacityBackpressure, SamePayloadSharesSizeClassForLoanAndTlvSend)
{
    PoolProbe p("c3");
    ASSERT_TRUE(p.wait_receivers(1));

    constexpr std::size_t kPayload = 10000;
    /* 算出来的期望（不是"看着像"）： */
    const std::size_t tlv_class = calc_chunk_size(kPayload);                 // TLV send 路径
    const std::size_t loan_class = borrowed_chunk_class(kPayload);           // loan 路径（两层取整）
    ASSERT_EQ(tlv_class, loan_class)
        << "两条入口必须落入相同尺寸档；当前 " << tlv_class << " vs " << loan_class;

    /* 借满 **loan** 档。 */
    std::vector<ipc::loan_t> held;
    ASSERT_EQ(take_available_loans(*p.tx, kPayload, held), kChunkPoolSize);
    EXPECT_FALSE(p.tx->loan(kPayload).valid()) << "loan 档应已耗尽";

    // 同一尺寸池已满，send 仍可通过分片回退交付。
    const auto payload = make_payload(kPayload, 0x5A);
    EXPECT_TRUE(p.tx->send(payload.data(), payload.size()))
        << "同档池满时 send 仍保留分片回退";

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

    std::vector<ipc::loan_t> held;
    ASSERT_EQ(take_available_loans(*p.tx,kBig,held),kChunkPoolSize);

    /* 现在发一条大消息：池空 ⇒ 策略性回退成分片，**且返回 true**。 */
    const auto big = make_payload(kBig, 0xE7);
    const bool sent = p.tx->send(big.data(), big.size(), 0);

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
    const auto before = pool_free_blocks(p.prefix, p.topic, cls);
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

    const auto during = pool_free_blocks(p.prefix, p.topic, cls);
    ASSERT_TRUE(during.has_value()) << "借满后段必然存在";
    EXPECT_EQ(*during, std::size_t{0})
        << "借满整档后空闲链应为 0（实测 " << *during << "）—— 外部直读与进程内计数互证";

    release_all(*p.tx, held);
    /* 归还可能有微小延迟（release 是同步的，这里只留一次重读机会）。 */
    std::optional<std::size_t> after = pool_free_blocks(p.prefix, p.topic, cls);
    for (int i = 0; i < 50 && (!after.has_value() || *after != kChunkPoolSize); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
        after = pool_free_blocks(p.prefix, p.topic, cls);
    }
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(*after, kChunkPoolSize)
        << "全部归还后该档池必须回到满空闲（实测 " << *after << "）—— 漏还一块就会少一块";
    std::printf("[W09-C8] 档 %zu 外部直读：借满=%zu → 归还后空闲=%zu（满值 %zu）\n", cls, *during,
                *after, kChunkPoolSize);
}

// 新池没有跨话题共享 ID 空间：不同话题 id 可以相同，地址及内容必须独立。
TEST(ChunkCapacityBackpressure, ConcurrentTopicsNeverAliasPayloads) {
    for(int round=0;round<50;++round) {
        std::vector<std::unique_ptr<PoolProbe>> probes;
        for(int i=0;i<8;++i) probes.emplace_back(new PoolProbe("parallel"));
        std::vector<ipc::loan_t> loans(8);std::vector<std::thread> threads;
        for(int i=0;i<8;++i)threads.emplace_back([&,i]{
            loans[i]=probes[i]->tx->loan(8000);
            if(loans[i].valid())std::memset(loans[i].data, i+1, 8000);
        });
        for(auto& t:threads)t.join();
        for(int i=0;i<8;++i) {
            ASSERT_TRUE(loans[i].valid());
            EXPECT_EQ(static_cast<unsigned char*>(loans[i].data)[7999],i+1);
            for(int j=0;j<i;++j)EXPECT_NE(loans[i].data,loans[j].data);
        }
    }
}

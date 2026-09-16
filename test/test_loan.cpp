/* libipc 借样原语回归 (docs/dzflat_shm.md §5.2)
 *
 * loan/publish_loan/discard_loan 的契约:
 *   ① loan 成功 → 调用方就地写入 → publish_loan → 接收方 recv 到的就是那块共享内存,
 *      全程没有 send() 那次 "调用方缓冲 → chunk" 的 memcpy;
 *   ② loan 返回的是**容量**而非请求长度(按尺寸档位取整), 接收方 recv 到的 buff_t
 *      大小等于该容量 —— 真实负载长度由负载自身的头部承载;
 *   ③ 失败是常态而非异常: 无接收方、chunk 池耗尽(32 块/档位)都会返回无效 loan,
 *      调用方必须能回退到 send();
 *   ④ discard_loan 与 publish_loan 都必须归还 chunk, 否则池子会被耗干。
 *      publish_loan 失败时由它自己归还, 调用方不得重复 discard。
 */
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

#include "gtest/gtest.h"

#include "libipc/ipc.h"

namespace {

/* ipc::id_pool<>::max_count = ipc::large_msg_cache */
constexpr int kChunkPoolSize = 32;

void fill(void* p, std::size_t n, std::uint8_t v)
{
    std::memset(p, v, n);
}

bool all_bytes_are(const void* p, std::size_t n, std::uint8_t v)
{
    const auto* b = static_cast<const std::uint8_t*>(p);
    for (std::size_t i = 0; i < n; ++i)
    {
        if (b[i] != v) return false;
    }
    return true;
}

}   // namespace

TEST(Loan, RoundTripDeliversTheLoanedChunkItself)
{
    const std::string name = "loan_roundtrip";
    ipc::route::clear_storage(name.c_str());
    ipc::route tx{name.c_str(), ipc::sender};
    ipc::route rx{name.c_str(), ipc::receiver};
    ASSERT_TRUE(tx.wait_for_recv(1, 2000));

    constexpr std::size_t kWant = 4096;
    auto lo = tx.loan(kWant);
    ASSERT_TRUE(lo.valid()) << "有接收方且池子空闲时 loan 不应失败";
    EXPECT_GE(lo.size, kWant) << "借到的容量不得小于请求";

    fill(lo.data, lo.size, 0x7E);
    ASSERT_TRUE(tx.publish_loan(lo, 1000));

    ipc::buff_t got = rx.recv(2000);
    ASSERT_FALSE(got.empty());
    /* 接收到的是容量, 不是请求长度 —— 这是 §5.2 的档位取整在 wire 上的可见后果。 */
    EXPECT_EQ(got.size(), lo.size);
    EXPECT_TRUE(all_bytes_are(got.data(), got.size(), 0x7E));
}

/* 借样必须落在 storage 路径上: 即使请求的字节数 <= large_msg_limit(64), 也要被
 * 提升到 chunk 语义, 否则接收侧的 msg.storage_ 分支不成立, 拿到的会是槽内数据。 */
TEST(Loan, TinyRequestIsPromotedToChunkPath)
{
    const std::string name = "loan_tiny";
    ipc::route::clear_storage(name.c_str());
    ipc::route tx{name.c_str(), ipc::sender};
    ipc::route rx{name.c_str(), ipc::receiver};
    ASSERT_TRUE(tx.wait_for_recv(1, 2000));

    auto lo = tx.loan(8);
    ASSERT_TRUE(lo.valid());
    EXPECT_GT(lo.size, static_cast<std::size_t>(64)) << "须被提升过 large_msg_limit";
    fill(lo.data, lo.size, 0x33);
    ASSERT_TRUE(tx.publish_loan(lo, 1000));

    ipc::buff_t got = rx.recv(2000);
    ASSERT_FALSE(got.empty());
    EXPECT_EQ(got.size(), lo.size);
    EXPECT_TRUE(all_bytes_are(got.data(), got.size(), 0x33));
}

/* 尺寸档位: 相近的长度必须落进同一容量, 否则每个长度开一个共享段(§5.2)。 */
TEST(Loan, NearbySizesShareOneCapacityClass)
{
    const std::string name = "loan_class";
    ipc::route::clear_storage(name.c_str());
    ipc::route tx{name.c_str(), ipc::sender};
    ipc::route rx{name.c_str(), ipc::receiver};
    ASSERT_TRUE(tx.wait_for_recv(1, 2000));

    /* 大消息档位是 2 的幂: 200KB 与 250KB 都应落到 256KB。 */
    auto a = tx.loan(200 * 1024);
    auto b = tx.loan(250 * 1024);
    ASSERT_TRUE(a.valid());
    ASSERT_TRUE(b.valid());
    EXPECT_EQ(a.size, b.size) << "相近的大消息长度必须共用一个容量档位";
    EXPECT_EQ(a.size, static_cast<std::size_t>(256 * 1024));
    tx.discard_loan(a);
    tx.discard_loan(b);

    /* 小消息档位是 1KB 台阶, 与既有 send 路径同粒度, 不引入新段。 */
    auto c = tx.loan(5000);
    ASSERT_TRUE(c.valid());
    EXPECT_EQ(c.size, static_cast<std::size_t>(5 * 1024));
    tx.discard_loan(c);
}

/* 无接收方时必须失败(而不是借出一块没人回收的 chunk), 让调用方回退 send()。 */
TEST(Loan, FailsWithoutReceiverSoCallerCanFallBack)
{
    const std::string name = "loan_noreceiver";
    ipc::route::clear_storage(name.c_str());
    ipc::route tx{name.c_str(), ipc::sender};

    auto lo = tx.loan(4096);
    EXPECT_FALSE(lo.valid()) << "无接收方时不得借出 chunk";

    /* 回退路径仍然可用。 */
    std::vector<std::uint8_t> payload(4096, 0x5A);
    EXPECT_TRUE(tx.no_member_try_send(payload.data(), payload.size(), 0));
}

/* 池子耗尽是背压信号而非错误: 必须返回无效 loan, 且已借出的块归还后能再借到。 */
TEST(Loan, PoolExhaustionIsRecoverable)
{
    const std::string name = "loan_exhaust";
    ipc::route::clear_storage(name.c_str());
    ipc::route tx{name.c_str(), ipc::sender};
    ipc::route rx{name.c_str(), ipc::receiver};
    ASSERT_TRUE(tx.wait_for_recv(1, 2000));

    /* 独占一个档位(9216 B 类), 借空它。 */
    constexpr std::size_t kSize = 8192;
    std::vector<ipc::loan_t> held;
    for (int i = 0; i < kChunkPoolSize + 4; ++i)
    {
        auto lo = tx.loan(kSize);
        if (!lo.valid()) break;
        held.push_back(lo);
    }
    EXPECT_EQ(static_cast<int>(held.size()), kChunkPoolSize)
        << "每个尺寸档位应恰有 " << kChunkPoolSize << " 块";

    auto denied = tx.loan(kSize);
    EXPECT_FALSE(denied.valid()) << "池空时必须返回无效 loan, 而不是崩溃或阻塞";

    for (const auto& lo : held) tx.discard_loan(lo);
    held.clear();

    auto again = tx.loan(kSize);
    EXPECT_TRUE(again.valid()) << "全部归还后必须能重新借到 —— discard 未真正回收";
    tx.discard_loan(again);
}

/* discard 之后 chunk 必须回到池子: 反复借还不得耗尽。 */
TEST(Loan, DiscardReturnsChunkEveryTime)
{
    const std::string name = "loan_discard";
    ipc::route::clear_storage(name.c_str());
    ipc::route tx{name.c_str(), ipc::sender};
    ipc::route rx{name.c_str(), ipc::receiver};
    ASSERT_TRUE(tx.wait_for_recv(1, 2000));

    for (int i = 0; i < kChunkPoolSize * 5; ++i)
    {
        auto lo = tx.loan(16384);
        ASSERT_TRUE(lo.valid()) << "第 " << i << " 次借样失败 —— discard 漏还了 chunk";
        tx.discard_loan(lo);
    }
}

/* publish 之后 chunk 归接收方: 接收方读完释放 buff_t 才回池。反复收发不得耗尽。 */
TEST(Loan, PublishedChunksRecycleAfterReceiverReleases)
{
    const std::string name = "loan_recycle";
    ipc::route::clear_storage(name.c_str());
    ipc::route tx{name.c_str(), ipc::sender};
    ipc::route rx{name.c_str(), ipc::receiver};
    ASSERT_TRUE(tx.wait_for_recv(1, 2000));

    for (int i = 0; i < kChunkPoolSize * 3; ++i)
    {
        auto lo = tx.loan(16384);
        ASSERT_TRUE(lo.valid()) << "第 " << i << " 轮借样失败 —— 已投递的 chunk 未回收";
        fill(lo.data, lo.size, static_cast<std::uint8_t>(i));
        ASSERT_TRUE(tx.publish_loan(lo, 1000));

        ipc::buff_t got = rx.recv(2000);
        ASSERT_FALSE(got.empty()) << "第 " << i << " 轮未收到";
        EXPECT_TRUE(all_bytes_are(got.data(), got.size(), static_cast<std::uint8_t>(i)));
        /* got 在此析构 → recycle_storage → 回池 */
    }
}

/* 多接收方: 每个都应收到同一块 chunk 的内容, 且全部释放后才回池。 */
TEST(Loan, BroadcastToMultipleReceivers)
{
    const std::string name = "loan_broadcast";
    ipc::route::clear_storage(name.c_str());
    ipc::route tx{name.c_str(), ipc::sender};
    ipc::route rx1{name.c_str(), ipc::receiver};
    ipc::route rx2{name.c_str(), ipc::receiver};
    ASSERT_TRUE(tx.wait_for_recv(2, 2000));

    auto lo = tx.loan(4096);
    ASSERT_TRUE(lo.valid());
    fill(lo.data, lo.size, 0xC7);
    ASSERT_TRUE(tx.publish_loan(lo, 1000));

    ipc::buff_t g1 = rx1.recv(2000);
    ipc::buff_t g2 = rx2.recv(2000);
    ASSERT_FALSE(g1.empty());
    ASSERT_FALSE(g2.empty());
    EXPECT_TRUE(all_bytes_are(g1.data(), g1.size(), 0xC7));
    EXPECT_TRUE(all_bytes_are(g2.data(), g2.size(), 0xC7));
    /* 两个接收方拿到的是同一块共享内存(零拷贝广播), 地址应当一致。 */
    EXPECT_EQ(g1.data(), g2.data());
}

/* 借样与常规 send 在同一通道上混用不得互相破坏。 */
TEST(Loan, InterleavesWithRegularSend)
{
    const std::string name = "loan_mixed";
    ipc::route::clear_storage(name.c_str());
    ipc::route tx{name.c_str(), ipc::sender};
    ipc::route rx{name.c_str(), ipc::receiver};
    ASSERT_TRUE(tx.wait_for_recv(1, 2000));

    std::vector<std::uint8_t> regular(3000, 0x11);
    for (int i = 0; i < 10; ++i)
    {
        ASSERT_TRUE(tx.try_send(regular.data(), regular.size(), 1000));
        ipc::buff_t a = rx.recv(2000);
        ASSERT_FALSE(a.empty());
        EXPECT_EQ(a.size(), regular.size());
        EXPECT_TRUE(all_bytes_are(a.data(), a.size(), 0x11));

        auto lo = tx.loan(3000);
        ASSERT_TRUE(lo.valid());
        fill(lo.data, lo.size, 0x22);
        ASSERT_TRUE(tx.publish_loan(lo, 1000));
        ipc::buff_t b = rx.recv(2000);
        ASSERT_FALSE(b.empty());
        EXPECT_EQ(b.size(), lo.size);
        EXPECT_TRUE(all_bytes_are(b.data(), b.size(), 0x22));
    }
}

TEST(Loan, InvalidLoanOperationsAreNoOps)
{
    const std::string name = "loan_invalid";
    ipc::route::clear_storage(name.c_str());
    ipc::route tx{name.c_str(), ipc::sender};
    ipc::route rx{name.c_str(), ipc::receiver};
    ASSERT_TRUE(tx.wait_for_recv(1, 2000));

    ipc::loan_t bad;   /* 默认构造 = 无效 */
    EXPECT_FALSE(bad.valid());
    EXPECT_FALSE(tx.publish_loan(bad, 0));
    tx.discard_loan(bad);   /* 不得崩溃 */

    /* 无效 loan 不应污染池子。 */
    auto lo = tx.loan(4096);
    EXPECT_TRUE(lo.valid());
    tx.discard_loan(lo);
}

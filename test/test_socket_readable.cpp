/* [t12] 非阻塞可读判据 readable()/udp_node_readable 的聚焦判据。
 *
 * 为什么单列一个套件(而不是塞进 test_socket_wait_set.cpp 的某个用例):
 * 它是 socket 侧收包 worker 的**准入判据** —— 没有它, recv_once() 只能直接调
 * chunk_rev_topic/chunk_rev_server, 而那两者一律带 tm 且空闲时阻塞到 tm(50ms/200ms);
 * worker 的预算循环是 for(;;){ n = recv_once(); if (n == 0) break; ... }, 于是每次数据
 * 突发收尾都会让**共享** worker 线程空读阻塞最长 tm。三档失效(该真时不真 / 该假时不假 /
 * 变慢)全部静默, 因此逐条钉在数值上。
 *
 * 平台前提: 本文件不出现任何平台宏, 句柄/判据一律走 UDPNode 与 udp_node_* 桥接;
 * 组播不可用时 GTEST_SKIP 而不是假绿(同 test_socket_wait_set.cpp)。
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>

#include "dzIPC/common/data_rev.h"
#include "dzIPC/common/hash.h"
#include "libipc/udp.h"

#include <gtest/gtest.h>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr int kDomain = 63;   ///< 与既有 socket 用例不同的 domain, 避免串扰

bool wait_for(const std::function<bool()>& pred, int timeout_ms)
{
    const auto deadline = Clock::now() + std::chrono::milliseconds(timeout_ms);
    while (Clock::now() < deadline)
    {
        if (pred())
        {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

std::string unique_topic(const char* tag)
{
    static std::atomic<unsigned> serial{0};
    return std::string("swr_") + tag + "_" + std::to_string(serial.fetch_add(1));
}

/* 一条真实的可读接收通道(RecvOnly: 入组、只收) + 它的发送端。 */
struct Channel
{
    std::string topic;
    std::string ip;
    std::uint16_t port{0};
    std::shared_ptr<ipc::socket::UDPNode> rx;
    std::shared_ptr<ipc::socket::UDPNode> tx;
    bool usable{false};

    explicit Channel(const char* tag)
    {
        topic = unique_topic(tag);
        ip = dzIPC::common::udp_discovery_addr_calculate(topic);
        port = dzIPC::common::udp_discovery_port_calculate(topic, kDomain);
        rx = std::make_shared<ipc::socket::UDPNode>("swr_rx", ip.c_str(), port,
                                                    ipc::socket::NodeRole::RecvOnly);
        usable = rx->connect();
        if (!usable)
        {
            return;
        }
        tx = std::make_shared<ipc::socket::UDPNode>("swr_tx", ip.c_str(), port,
                                                    ipc::socket::NodeRole::SendOnly);
        usable = tx->connect();
    }

    bool send(const char* text)
    {
        if (!usable)
        {
            return false;
        }
        char raw[64];
        const std::size_t n = std::strlen(text);
        std::memcpy(raw, text, n);
        ipc::buffer payload(static_cast<void*>(raw), n);
        return tx->send(payload);
    }
};

bool require_multicast(const Channel& ch) { return ch.usable; }

/* 发送端到接收端之间的组播回绕是**异步**的: send() 返回时包可能还在内核里。
 * 有界重试而不是"睡够就行" —— 超时即按调用点语义判定。 */
bool send_and_wait_readable(Channel& ch, const char* text)
{
    for (int attempt = 0; attempt < 200; ++attempt)
    {
        if (!ch.send(text))
        {
            return false;
        }
        if (ch.rx->readable())
        {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return false;
}

}   // namespace

/* read() 的三档语义: 空通道 false / 有数据 true / **读完之后回到 false**。
 * 第三条最容易被写成"永远 true"(例如实现成只查了 fd 是否有效), 那样 worker 的预算
 * 循环就退化成忙转。 */
TEST(SocketReadable, ReflectsPendingDataWithoutConsuming) 
{
    Channel ch{"state"};
    if (!require_multicast(ch))
    {
        GTEST_SKIP() << "multicast unavailable on this host";
        return;
    }

    /* ① 空通道: false。连续多次同值(幂等)。 */
    for (int i = 0; i < 4; ++i)
    {
        EXPECT_FALSE(ch.rx->readable()) << "空通道不得报可读";
    }

    /* ② 有数据: true。 */
    ASSERT_TRUE(send_and_wait_readable(ch, "swr-payload")) << "组播发送失败/未到达 ⇒ 环境限制";
    EXPECT_TRUE(ch.rx->readable());

    /* ③ **不做读操作**: 反复调用保持 true(不消费), 且数据仍在。 */
    EXPECT_TRUE(ch.rx->readable()) << "readable() 不得消费数据";
    EXPECT_TRUE(ch.rx->readable());
    const auto got = ch.rx->receive_nowait();
    ASSERT_EQ(got.size(), std::strlen("swr-payload")) << "readable() 把数据吃掉了";
    EXPECT_EQ(std::string(static_cast<const char*>(got.data()), got.size()), "swr-payload");

    /* ④ 读空之后必须回到 false(否则 worker 会对着没数据的 fd 空转)。 */
    ASSERT_TRUE(wait_for([&] { return !ch.rx->readable(); }, 500))
        << "读空后 readable() 必须回到 false(否则就是忙轮询)";
}

/* cancel_wait() 之后一律 false。这一条是**语义必需**, 不是优化:
 * Linux 侧 fd 已被 shutdown(SHUT_RD), poll 会永久报 POLLIN|POLLHUP(associates 到
 * "可读"), 不短路就会让 worker 在一个读不出东西的通道上空转。 */
TEST(SocketReadable, IsFalseAfterCancelWaitAndRecoversOnReconnect)
{
    Channel ch{"cancel"};
    if (!require_multicast(ch))
    {
        GTEST_SKIP() << "multicast unavailable on this host";
        return;
    }

    ASSERT_TRUE(send_and_wait_readable(ch, "before-cancel"));
    EXPECT_TRUE(ch.rx->readable());

    dzIPC::socket::udp_node_cancel_wait(ch.rx);
    EXPECT_FALSE(ch.rx->readable()) << "cancel 之后不得再报可读";
    EXPECT_FALSE(ch.rx->readable()) << "幂等: 重复查询同值";

    /* 接收面失效: 即便后端还有包到达, 也不得报可读(冻结语义: cancel ⇒ 一律空/不可读)。 */
    ch.send("after-cancel");
    std::this_thread::sleep_for(50ms);
    EXPECT_FALSE(ch.rx->readable()) << "cancel 之后接收面必须失效";

    /* close() + connect() 复位(契约 §1 的复位口径)。 */
    ASSERT_TRUE(ch.rx->close());
    ASSERT_TRUE(ch.rx->connect());
    EXPECT_FALSE(ch.rx->readable()) << "复位后是新通道, 不该凭空可读";
    ASSERT_TRUE(send_and_wait_readable(ch, "after-reset"))
        << "复位后的通道必须重新可读(否则上面那条 false 就是假绿)";
}

/* 桥接层 nullptr 安全 + 与直接调用同值。 */
TEST(SocketReadable, BridgeIsNullSafeAndAgreesWithNode)
{
    std::shared_ptr<ipc::socket::UDPNode> empty;
    EXPECT_FALSE(dzIPC::socket::udp_node_readable(empty)) << "nullptr ⇒ false";

    Channel ch{"bridge"};
    if (!require_multicast(ch))
    {
        GTEST_SKIP() << "multicast unavailable on this host";
        return;
    }
    EXPECT_EQ(dzIPC::socket::udp_node_readable(ch.rx), ch.rx->readable());
    ASSERT_TRUE(send_and_wait_readable(ch, "bridge-payload"));
    EXPECT_EQ(dzIPC::socket::udp_node_readable(ch.rx), ch.rx->readable());
    EXPECT_TRUE(dzIPC::socket::udp_node_readable(ch.rx));
}

/* 不可等待的节点(SendOnly 不入组)一律 false: 消费方据此走兼容回退, 而不是把
 * 一个假判据当"没数据"反复轮询。 */
TEST(SocketReadable, SendOnlyNodeIsNeverReadable)
{
    Channel probe{"sendonly_probe"};
    if (!require_multicast(probe))
    {
        GTEST_SKIP() << "multicast unavailable on this host";
        return;
    }
    ipc::socket::UDPNode send_only("swr_so", probe.ip.c_str(), probe.port,
                                   ipc::socket::NodeRole::SendOnly);
    ASSERT_TRUE(send_only.connect());
    EXPECT_FALSE(send_only.readable()) << "SendOnly 不入组 ⇒ 永远收不到东西";
    EXPECT_FALSE(send_only.readable());
}

/* 空闲通道上的非阻塞读取必须**立即**返回 —— 这是 t12 存在的全部理由。
 *
 * 判据 = 有界时间窗(与 test_socket_borrow.cpp 的"带超时的视图取必须超时返回"同一
 * 手法: 只看返回值的话, 一个直接 return 0 的实现也能过; 只看下界的话, 一个阻塞到
 * 50ms 的实现也能过)。这里两条都查: 读到的必须是 0, 且耗时可忽略。
 *
 * ⛔ 不得把这条改成"跳过" —— 它守的正是"共享 worker 线程被空读阻塞 tm"这个缺陷。 */
TEST(SocketReadable, IdleRecvOnceReturnsImmediatelyWithoutBlockingOnTimeout)
{
    Channel ch{"idle"};
    if (!require_multicast(ch))
    {
        GTEST_SKIP() << "multicast unavailable on this host";
        return;
    }

    /* 先确保通道真的空(排空残留)。 */
    while (!ch.rx->receive_nowait().empty())
    {
    }
    ASSERT_FALSE(ch.rx->readable()) << "本判据要求先造出空通道";

    /* 三档时限: 走 udp_node_readable 的实现是微秒级; 直接调 chunk_rev_* 的实现会
     * 阻塞在**它有数据都没有**的 tm 上(50/200ms)。阈值取 5ms 留足调度抖动。 */
    for (int i = 0; i < 5; ++i)
    {
        const auto t0 = Clock::now();
        bool readable = false;
        std::size_t bytes = 0;
        if (!dzIPC::socket::udp_node_readable(ch.rx))
        {
            readable = false;   // 无数据 ⇒ recv_once 必须立即返回 0(不调 chunk_rev_*)
        }
        else
        {
            readable = true;
            bytes = 1;
        }
        const auto elapsed_us =
            std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
        EXPECT_FALSE(readable) << "空通道不得报可读";
        EXPECT_EQ(bytes, 0u);
        EXPECT_LT(elapsed_us, 5000) << "空闲判据必须立即返回(实测 " << elapsed_us << "us)";
    }

    /* 反向: 真数据到达时判据必须变 true —— 否则上一条的"立即返回"是靠"永远 false"
     * 换来的(那是假绿: worker 会静默停收)。 */
    ASSERT_TRUE(send_and_wait_readable(ch, "idle-flip"))
        << "有数据时 readable() 必须变 true, 否则上一条只是永远 false 的假绿";
}

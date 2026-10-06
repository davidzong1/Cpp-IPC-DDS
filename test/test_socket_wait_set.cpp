/* 阶段 5 `SocketWaitSet` 聚焦验收：可等待 backend（Linux epoll）的 add/remove/stop
 * 与 wait 并发、fd 复用不误触发、真通道端到端就绪。
 *
 * 接口契约：`ipc-transport-phase5-shared-wait-layer-contract-*.md`（owner: ipc-transport）；
 * 消费方：docs/消息接收架构改造/{socket_pub_sub,socket_ser_cli}线程池移植方案.md §3。
 *
 * ── 为什么这些判据是承重的（不是"跑绿即过"）────────────────────────────────
 * socket 侧的失效方式同样静默：
 *   ① remove/stop 不唤醒阻塞中的 wait ⇒ worker 睡过注销 ⇒ 宿主按协议释放通道后
 *      worker 才醒 ⇒ use-after-free 或"话题永久停收"（契约注销协议第 2 步）。
 *   ② 摘除不彻底（不清 epoll ready 残留、不清用户态 ready 缓存）⇒ **fd 复用**时
 *      旧事件算到新通道上 ⇒ 新通道被凭空报告就绪 ⇒ worker 对着一个没数据的 socket
 *      空转（本机实测：close 后新 socket 复用同一 fd 号，A=3、B=3）。
 *   ③ 只消费事件不真读 ⇒ epoll LT 下 wait 立刻返回 ⇒ 忙轮询吃满 CPU。
 * 故本套件以"能否唤醒"、"fd 复用后是否误报"、"真通道能否被点名"三类判据守门。
 *
 * ── 平台前提 ────────────────────────────────────────────────────────────
 * 本文件不出现平台宏：句柄一律 std::uintptr_t，由 libipc 给出（Linux = 接收 fd，
 * Windows = WSAEVENT）。组播不可用时用例 GTEST_SKIP 而不是假绿（同
 * test_socket_borrow.cpp 的手法）。
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#if defined(__linux__)
#include <sys/eventfd.h>
#include <time.h>
#include <unistd.h>
#endif

#include "dzIPC/common/data_rev.h"
#include "dzIPC/common/hash.h"
#include "dzIPC/threepools/socket_wait_set.h"
#include "libipc/udp.h"

#include <gtest/gtest.h>

namespace {

using namespace dzIPC::threepools;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr int kDomain = 61;   ///< 与既有 socket 用例不同的 domain，避免串扰

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
    return std::string("sws_") + tag + "_" + std::to_string(serial.fetch_add(1));
}

struct Endpoint
{
    std::string ip;
    std::uint16_t port{0};
};

Endpoint endpoint_for(const std::string& topic)
{
    return Endpoint{dzIPC::common::udp_discovery_addr_calculate(topic),
                    dzIPC::common::udp_discovery_port_calculate(topic, kDomain)};
}

/* 一条可等待的接收通道（RecvOnly：入组、只收）+ 它的发送端。
 * 析构顺序：sender 先、receiver 后（成员逆序），与真实模块一致。 */
struct Channel
{
    std::string topic;
    Endpoint ep;
    /* shared_ptr 而不是 unique_ptr：消费方（socket_pub_sub / socket_ser_cli）
     * 真实持有的就是 shared_ptr，桥接函数也按 shared_ptr 判空。 */
    std::shared_ptr<ipc::socket::UDPNode> rx;
    std::shared_ptr<ipc::socket::UDPNode> tx;
    bool usable{false};

    explicit Channel(const char* tag)
    {
        topic = unique_topic(tag);
        ep = endpoint_for(topic);
        rx = std::make_shared<ipc::socket::UDPNode>("sws_rx", ep.ip.c_str(), ep.port,
                                                   ipc::socket::NodeRole::RecvOnly);
        usable = rx->connect();
        if (!usable)
        {
            return;
        }
        tx = std::make_shared<ipc::socket::UDPNode>("sws_tx", ep.ip.c_str(), ep.port,
                                                    ipc::socket::NodeRole::SendOnly);
        usable = tx->connect();
    }

    /* 返回 false = 本机没有组播（环境限制），用例应跳过。 */
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

    std::uintptr_t handle() { return rx->wait_handle(); }
};

/* 组播不可用 ⇒ 整套跳过（不是失败）。 */
bool require_multicast(const Channel& ch)
{
    return ch.usable;
}

}   // namespace

/* ------------------------------------------------------------ 能力探测 */

/* backend_available() 必须进程内缓存、只探测一次；false 时消费方必须显式回退
 * 到兼容的每通道线程（**不得**用 receive_nowait 忙轮询降级）。 */
TEST(SocketWaitSet, BackendIsProbedAndNamed)
{
    const bool available = SocketWaitSet::backend_available();
    EXPECT_EQ(available, SocketWaitSet::backend_available()) << "探测结果必须进程内缓存（同值）";
    if (!available)
    {
        GTEST_SKIP() << "SocketWaitSet backend unavailable; callers must keep one receive thread per channel";
        return;
    }
    EXPECT_STRNE(SocketWaitSet::backend_name(), "none");
    EXPECT_GT(SocketWaitSet::max_channels(), 0u);

    /* 不可等待的通道（SendOnly 不入组）句柄必须是 0：消费方据此走回退，而不是把
     * 一个假句柄塞进 wait-set。 */
    Channel ch{"sendonly_probe"};
    if (!require_multicast(ch))
    {
        GTEST_SKIP() << "multicast unavailable on this host";
        return;
    }
    ipc::socket::UDPNode send_only("sws_so", ch.ep.ip.c_str(), ch.ep.port,
                                   ipc::socket::NodeRole::SendOnly);
    ASSERT_TRUE(send_only.connect());
    EXPECT_FALSE(send_only.waitable()) << "SendOnly 不入组 ⇒ 收不到东西 ⇒ 不构成可等待通道";
    EXPECT_EQ(send_only.wait_handle(), 0u);

    EXPECT_TRUE(ch.rx->waitable());
    EXPECT_NE(ch.handle(), 0u);
    EXPECT_EQ(ch.rx->wait_handle(), ch.handle()) << "同一通道重复取句柄必须稳定";
}

/* dzIPC 桥接（socket 两模块唯一的句柄入口）必须 nullptr 安全：模块在 stop /
 * 通道重建的窗口里持有空 shared_ptr，判空漏一处就是空指针解引用。 */
TEST(SocketWaitSet, DataRevBridgeIsNullSafe)
{
    std::shared_ptr<ipc::socket::UDPNode> empty;
    EXPECT_FALSE(dzIPC::socket::udp_node_waitable(empty));
    EXPECT_EQ(dzIPC::socket::udp_node_wait_handle(empty), 0u);
    dzIPC::socket::udp_node_cancel_wait(empty);   // 不得崩
    dzIPC::socket::udp_node_clear_wait(empty);    // 不得崩
}

/* ------------------------------------------------------------ add/remove */

TEST(SocketWaitSet, AddIsIdempotentAndRejectsAmbiguousIdentities)
{
    Channel ch{"add"};
    if (!require_multicast(ch))
    {
        GTEST_SKIP() << "multicast unavailable on this host";
        return;
    }
    SocketWaitSet set;
    if (!SocketWaitSet::backend_available())
    {
        GTEST_SKIP() << "SocketWaitSet backend unavailable on this platform";
        return;
    }

    const SocketWaitToken token{ch.rx.get(), ch.handle()};
    ASSERT_TRUE(set.add(token));
    EXPECT_EQ(set.size(), 1u);
    EXPECT_TRUE(set.add(token)) << "同一 token 重复 add 是幂等成功";
    EXPECT_EQ(set.size(), 1u) << "重复 add 不得产生第二份条目";

    /* 同 owner 不同 handle：宿主换了底层 socket 却没 remove —— 拒绝（而不是覆盖）。 */
    EXPECT_FALSE(set.add(SocketWaitToken{ch.rx.get(), ch.handle() + 1u}));
    /* 不同 owner 同 handle：事件 → token 的映射会二义 —— 拒绝。 */
    EXPECT_FALSE(set.add(SocketWaitToken{&set, ch.handle()}));
    /* 无效 token：owner/handle 任一为空都拒绝。 */
    EXPECT_FALSE(set.add(SocketWaitToken{nullptr, ch.handle()}));
    EXPECT_FALSE(set.add(SocketWaitToken{ch.rx.get(), 0u}));
    EXPECT_EQ(set.size(), 1u) << "被拒的 add 不得改变已有集合";

    EXPECT_TRUE(set.remove(token));
    EXPECT_EQ(set.size(), 0u);
    EXPECT_TRUE(set.remove(token)) << "未注册的 token remove 是幂等成功";
    EXPECT_TRUE(set.remove(SocketWaitToken{nullptr, 0u})) << "无效 token remove 也必须是成功（幂等）";
}

/* ------------------------------------------------------------ wait 语义 */

TEST(SocketWaitSet, WaitTimesOutWithoutDataAndReportsRealChannel)
{
    Channel ch{"wait"};
    if (!require_multicast(ch))
    {
        GTEST_SKIP() << "multicast unavailable on this host";
        return;
    }
    SocketWaitSet set;
    if (!SocketWaitSet::backend_available())
    {
        GTEST_SKIP() << "SocketWaitSet backend unavailable on this platform";
        return;
    }
    const SocketWaitToken token{ch.rx.get(), ch.handle()};
    ASSERT_TRUE(set.add(token));

    EXPECT_FALSE(set.wait(120ms)) << "没有数据时必须超时返回 false（超时不是错误）";
    EXPECT_TRUE(set.consume_ready().empty());

    ASSERT_TRUE(ch.send("sws-payload")) << "组播发送失败 ⇒ 环境限制";
    ASSERT_TRUE(set.wait(2000ms));
    const auto ready = set.consume_ready();
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready.front(), token) << "ready 必须能回到注册它的通道（不能只返回「有人醒了」）";

    /* level-triggered：**必须真读**。没读走的事件会让下一次 wait 立刻返回。 */
    auto got = ch.rx->receive_nowait();
    ASSERT_EQ(got.size(), std::strlen("sws-payload"));
    EXPECT_EQ(std::string(static_cast<const char*>(got.data()), got.size()), "sws-payload");
    EXPECT_FALSE(set.wait(120ms)) << "读空之后不得再报告就绪（否则就是忙轮询）";

    EXPECT_TRUE(set.remove(token));
    EXPECT_TRUE(set.consume_ready().empty()) << "remove 之后该 token 不得再出现在 ready 里";
}

#if defined(__linux__)
TEST(SocketWaitSet, LargeIdleSetAndHighTokenReuse)
{
    if (!SocketWaitSet::backend_available()) GTEST_SKIP() << "epoll unavailable";
    constexpr std::size_t count = 256;
    if (SocketWaitSet::max_channels() < count) GTEST_SKIP() << "backend channel limit is below this case";
    SocketWaitSet set;
    std::vector<int> fds;
    struct FdCleanup { std::vector<int>& values; ~FdCleanup() { for (const auto fd : values) if (fd >= 0) ::close(fd); } } cleanup{fds};
    std::vector<std::unique_ptr<unsigned>> owners;
    std::vector<SocketWaitToken> tokens;
    fds.reserve(count); owners.reserve(count); tokens.reserve(count);
    for (std::size_t i = 0; i < count; ++i)
    {
        const int fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
        ASSERT_GE(fd, 0);
        fds.push_back(fd);
        owners.push_back(std::make_unique<unsigned>(static_cast<unsigned>(i)));
        tokens.push_back({owners.back().get(), static_cast<std::uintptr_t>(fd)});
        ASSERT_TRUE(set.add(tokens.back()));
    }
    ASSERT_EQ(set.size(), count);
    timespec cpu_before{}, cpu_after{};
    ASSERT_EQ(::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_before), 0);
    const auto wall_before = Clock::now();
    EXPECT_FALSE(set.wait(200ms)) << "256 路空闲 FD 必须阻塞到超时，不应被报告为就绪";
    const auto wall_elapsed = Clock::now() - wall_before;
    ASSERT_EQ(::clock_gettime(CLOCK_PROCESS_CPUTIME_ID, &cpu_after), 0);
    const auto cpu_ns = (cpu_after.tv_sec - cpu_before.tv_sec) * 1000000000ll + cpu_after.tv_nsec - cpu_before.tv_nsec;
    EXPECT_GE(wall_elapsed, 180ms);
    EXPECT_LT(cpu_ns, 50000000ll) << "多 FD 空闲等待不应忙轮询";
    EXPECT_TRUE(set.consume_ready().empty());

    std::uint64_t one = 1;
    ASSERT_EQ(::write(fds[count / 2], &one, sizeof(one)), static_cast<ssize_t>(sizeof(one)));
    ASSERT_TRUE(set.wait(100ms));
    auto ready = set.consume_ready();
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready.front(), tokens[count / 2]);
    std::uint64_t drained = 0;
    ASSERT_EQ(::read(fds[count / 2], &drained, sizeof(drained)), static_cast<ssize_t>(sizeof(drained)));

    const auto old_high = tokens.back();
    const int old_fd = fds.back();
    auto old_owner = std::move(owners.back());
    ASSERT_TRUE(set.remove(old_high));
    ::close(old_fd);
    fds.back() = -1; fds.pop_back(); tokens.pop_back(); owners.pop_back();
    const int replacement_fd = ::eventfd(0, EFD_NONBLOCK | EFD_CLOEXEC);
    ASSERT_GE(replacement_fd, 0);
    fds.push_back(replacement_fd);
    auto replacement_owner = std::make_unique<unsigned>(999u);
    const SocketWaitToken replacement{replacement_owner.get(), static_cast<std::uintptr_t>(replacement_fd)};
    ASSERT_TRUE(set.add(replacement));
    ASSERT_EQ(::write(replacement_fd, &one, sizeof(one)), static_cast<ssize_t>(sizeof(one)));
    ASSERT_TRUE(set.wait(100ms));
    ready = set.consume_ready();
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready.front(), replacement);
    EXPECT_NE(replacement, old_high);
    ASSERT_TRUE(set.remove(replacement));
    (void)old_owner;
    fds.back() = -1; ::close(replacement_fd);
    for (std::size_t i = 0; i < tokens.size(); ++i)
    {
        ASSERT_TRUE(set.remove(tokens[i]));
    }
    EXPECT_EQ(set.size(), 0u);
}
#endif

/* --------------------------------------------- remove/stop 唤醒并发 wait */

/* 注销协议第 2 步：remove 必须唤醒阻塞中的 wait。若只摘除不唤醒，worker 会一直
 * 睡到超时（甚至更久），而宿主已经按"remove 返回即安全"释放了通道。 */
TEST(SocketWaitSet, RemoveWakesBlockedWait)
{
    Channel ch{"remove_wake"};
    if (!require_multicast(ch))
    {
        GTEST_SKIP() << "multicast unavailable on this host";
        return;
    }
    SocketWaitSet set;
    if (!SocketWaitSet::backend_available())
    {
        GTEST_SKIP() << "SocketWaitSet backend unavailable on this platform";
        return;
    }
    const SocketWaitToken token{ch.rx.get(), ch.handle()};
    ASSERT_TRUE(set.add(token));

    std::atomic<bool> returned{false};
    std::atomic<bool> woke{false};
    std::thread waiter([&] {
        woke.store(set.wait(10'000ms));   // 长超时：唤醒不生效就是 10s 红灯
        returned.store(true);
    });
    std::this_thread::sleep_for(50ms);   // 让 wait 真的睡下去
    const auto begin = Clock::now();
    ASSERT_TRUE(set.remove(token));
    waiter.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin);

    EXPECT_TRUE(returned.load());
    EXPECT_TRUE(woke.load()) << "被 remove 唤醒时 wait 必须返回 true（调用方据此 consume_ready）";
    EXPECT_LT(elapsed.count(), 2000) << "remove 必须立刻唤醒阻塞中的 wait（实测 " << elapsed.count() << "ms）";
    EXPECT_TRUE(set.consume_ready().empty()) << "被唤醒但集合已摘除 ⇒ ready 应为空集";
    EXPECT_EQ(set.size(), 0u);
}

TEST(SocketWaitSet, StopWakesBlockedWaitAndIsIdempotent)
{
    Channel ch{"stop_wake"};
    if (!require_multicast(ch))
    {
        GTEST_SKIP() << "multicast unavailable on this host";
        return;
    }
    SocketWaitSet set;
    if (!SocketWaitSet::backend_available())
    {
        GTEST_SKIP() << "SocketWaitSet backend unavailable on this platform";
        return;
    }
    const SocketWaitToken token{ch.rx.get(), ch.handle()};
    ASSERT_TRUE(set.add(token));

    std::atomic<bool> woke{false};
    std::thread waiter([&] { woke.store(set.wait(10'000ms)); });
    std::this_thread::sleep_for(50ms);
    const auto begin = Clock::now();
    set.stop();
    waiter.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin);

    EXPECT_TRUE(woke.load());
    EXPECT_LT(elapsed.count(), 2000) << "stop 必须立刻唤醒阻塞中的 wait（实测 " << elapsed.count() << "ms）";
    set.stop();   // 幂等
    EXPECT_TRUE(set.wait(1ms)) << "stop 之后 wait 立即返回 true，由 worker 循环据此退出";
    EXPECT_FALSE(set.add(token)) << "stop 之后不得再接受新通道";
}

/* add/remove 与 wait 并发 churn：契约要求"add/remove 与 wait 无竞态"。
 * 判据 = 不崩、不死锁、最终能停下来（remove 都能唤醒）。 */
TEST(SocketWaitSet, ConcurrentAddRemoveDuringWaitDoesNotHang)
{
    Channel first{"churn_a"};
    Channel second{"churn_b"};
    if (!require_multicast(first) || !require_multicast(second))
    {
        GTEST_SKIP() << "multicast unavailable on this host";
        return;
    }
    SocketWaitSet set;
    if (!SocketWaitSet::backend_available())
    {
        GTEST_SKIP() << "SocketWaitSet backend unavailable on this platform";
        return;
    }
    const SocketWaitToken token_a{first.rx.get(), first.handle()};
    const SocketWaitToken token_b{second.rx.get(), second.handle()};
    ASSERT_TRUE(set.add(token_a));
    ASSERT_TRUE(set.add(token_b));

    std::atomic<bool> stop{false};
    std::atomic<unsigned> waits{0};
    std::thread waiter([&] {
        while (!stop.load(std::memory_order_acquire))
        {
            set.wait(20ms);
            set.consume_ready();
            waits.fetch_add(1, std::memory_order_relaxed);
        }
    });

    for (int i = 0; i < 60; ++i)
    {
        if (i % 2 == 0)
        {
            set.remove(token_b);
        }
        else
        {
            set.add(token_b);
        }
        std::this_thread::sleep_for(2ms);
    }
    stop.store(true, std::memory_order_release);
    waiter.join();
    EXPECT_GT(waits.load(), 0u) << "wait 线程必须真的跑起来（否则本用例没造出并发）";

    /* 收尾：两条都摘掉，集合回到空且不再报告任何东西。 */
    EXPECT_TRUE(set.remove(token_a));
    EXPECT_TRUE(set.remove(token_b));
    EXPECT_EQ(set.size(), 0u);
    set.wait(1ms);
    EXPECT_TRUE(set.consume_ready().empty());
}

/* ------------------------------------------------ fd 复用不误触发（实测） */

/* 本机实测（Linux 6.8）：
 *   · close(fd) 之后新建 socket 复用同一 fd 号（A=3、B=3）；
 *   · epoll_ctl(EPOLL_CTL_DEL) 同步清除该 fd 在 ready list 里的残留；
 *   · DEL 本身**不唤醒**阻塞中的 epoll_wait —— 唤醒必须走独立通道（remove/stop 的
 *     eventfd），这也是上面两个唤醒用例守的东西。
 * 三条合起来 ⇒ 摘除必须显式 DEL + 清用户态 ready 缓存，且 consume_ready 按当前
 * 在册集合过滤。缺任何一条，旧 fd 的事件就会算到复用该 fd 号的新通道上。
 *
 * 判据：把 A 造到"有未读数据 + 已进过 ready"之后 remove/关闭，再让 B 复用同一
 * fd 号注册；B 不得被凭空报告就绪，而 B 真收到数据时必须被点名。 */
TEST(SocketWaitSet, ReusedHandleDoesNotInheritStaleReadiness)
{
    Channel first{"reuse_a"};
    Channel second{"reuse_b"};
    if (!require_multicast(first) || !require_multicast(second))
    {
        GTEST_SKIP() << "multicast unavailable on this host";
        return;
    }
    SocketWaitSet set;
    if (!SocketWaitSet::backend_available())
    {
        GTEST_SKIP() << "SocketWaitSet backend unavailable on this platform";
        return;
    }

    const std::uintptr_t handle_a = first.handle();
    const SocketWaitToken token_a{first.rx.get(), handle_a};
    ASSERT_TRUE(set.add(token_a));

    /* 让 A 真的就绪（有未读数据），并把它收进 ready 缓存 —— 不读走。 */
    ASSERT_TRUE(first.send("stale")) << "组播发送失败 ⇒ 环境限制";
    ASSERT_TRUE(set.wait(2000ms));
    ASSERT_EQ(set.consume_ready().size(), 1u) << "A 必须被报告一次（否则本用例没造出残留）";
    ASSERT_TRUE(set.add(token_a));   // 重新在册（consume_ready 不清注册）

    /* 摘除 + 关闭：ready 缓存与 epoll 残留都必须被清掉。 */
    ASSERT_TRUE(set.remove(token_a));
    EXPECT_TRUE(set.consume_ready().empty()) << "remove 必须同时清掉用户态 ready 缓存";
    first.rx->close();

    /* B 复用同一个 fd 号（实测成立；若本机不复用，本用例退化为"新通道不误报"）。 */
    second.rx->close();
    ASSERT_TRUE(second.rx->connect());
    const std::uintptr_t handle_b = second.handle();
    ASSERT_NE(handle_b, 0u);
    const bool reused = (handle_b == handle_a);
    const SocketWaitToken token_b{second.rx.get(), handle_b};
    ASSERT_TRUE(set.add(token_b));

    /* 关键判据：B 的 socket 接收队列是空的，不得因为 fd 号复用而被报告就绪。 */
    EXPECT_FALSE(set.wait(150ms))
        << "fd 复用后旧事件不得算到新通道上（handle_a=" << handle_a << ", handle_b=" << handle_b
        << ", reused=" << reused << "）";
    EXPECT_TRUE(set.consume_ready().empty());

    /* 反向：B 自己真收到数据时必须被点名（证明上一条不是"永远不报告"的假绿）。 */
    ASSERT_TRUE(second.send("fresh"));
    ASSERT_TRUE(set.wait(2000ms));
    const auto ready = set.consume_ready();
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready.front(), token_b);
    EXPECT_EQ(ready.front().handle, handle_b);

    EXPECT_TRUE(set.remove(token_b));
}

/* -------------------------------------------------- cancel_wait 唤醒协议 */

/* `udp_node_cancel_wait` 是"宿主主动让等待方立刻返回"的唯一合法手段（不能靠
 * close 别的线程正在 epoll_wait 的 fd —— 实测那会让 epoll_wait 睡到超时）。
 * 判据：阻塞中的 wait 被唤醒，且此后该节点接收面失效（不再返回数据）。 */
TEST(SocketWaitSet, CancelWaitWakesBlockedWaitAndDisablesNode)
{
    Channel ch{"cancel"};
    if (!require_multicast(ch))
    {
        GTEST_SKIP() << "multicast unavailable on this host";
        return;
    }
    SocketWaitSet set;
    if (!SocketWaitSet::backend_available())
    {
        GTEST_SKIP() << "SocketWaitSet backend unavailable on this platform";
        return;
    }
    const SocketWaitToken token{ch.rx.get(), ch.handle()};
    ASSERT_TRUE(set.add(token));

    std::atomic<bool> woke{false};
    std::thread waiter([&] { woke.store(set.wait(10'000ms)); });
    std::this_thread::sleep_for(50ms);

    const auto begin = Clock::now();
    dzIPC::socket::udp_node_cancel_wait(ch.rx);
    waiter.join();
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin);

    EXPECT_TRUE(woke.load()) << "cancel_wait 必须唤醒阻塞在该句柄上的等待";
    EXPECT_LT(elapsed.count(), 2000) << "cancel_wait 必须立刻唤醒（实测 " << elapsed.count() << "ms）";
    EXPECT_FALSE(ch.rx->waitable()) << "cancel 之后不得再声明可等待";
    EXPECT_EQ(ch.rx->wait_handle(), 0u);

    ASSERT_TRUE(ch.send("after-cancel"));
    EXPECT_TRUE(ch.rx->receive_nowait().empty()) << "cancel 之后接收面必须失效（不得返回数据）";
    EXPECT_TRUE(ch.rx->receive(50).empty());

    dzIPC::socket::udp_node_cancel_wait(ch.rx);   // 幂等
    EXPECT_TRUE(set.remove(token));
}

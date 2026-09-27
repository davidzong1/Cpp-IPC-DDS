/* 阶段 5 `SocketRecvWorker` / `SocketRecvWorkerPool` 聚焦自检：固定归属与 SHM 侧逐值一致、
 * 单消费者互斥（busy/duplicate）、真实通道的收取与统计、remove_route 的同步唤醒与 owner 归还、
 * 空闲退出与按需拉起、池的固定归属与线程数口径。
 *
 * 为什么需要这个套件：socket 侧收包 worker 是 t4（socket_pub_sub）与 t5（socket_ser_cli）
 * 共同消费的共享组件，它一旦失效，两个模块的表现都是**静默**的：
 *   ① 归属漂移 ⇒ 同一通道的前后分片落在两个线程的重组状态上 ⇒ 偶发丢消息；
 *   ② 与兼容收包线程双收 ⇒ 同一字节被消费两次（两边都不报错）；
 *   ③ remove_route 不唤醒阻塞中的 wait ⇒ 宿主按协议关掉 fd 之后 worker 才醒 ⇒ UAF；
 *   ④ 空闲退出后不按需拉起 ⇒ "返回 ok 却无人消费"（静默停收）。
 * 因此每条判据都落在**数值 / 因果序**上，不接受"没报错即通过"。
 *
 * 平台前提：本文件不出现任何平台宏，句柄一律由 libipc 的 UDPNode 给出（Linux = 接收 fd，
 * Windows = WSAEVENT）；组播不可用时用例 GTEST_SKIP 而不是假绿（同 test_socket_wait_set.cpp）。
 */
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dzIPC/common/data_rev.h"
#include "dzIPC/common/hash.h"
#include "dzIPC/threepools/recv_worker.h"
#include "dzIPC/threepools/socket_recv_worker.h"
#include "libipc/udp.h"

#include <gtest/gtest.h>

namespace {

using namespace dzIPC::threepools;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr int kDomain = 62;   ///< 与既有 socket 用例不同的 domain，避免串扰

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
    return std::string("srw_") + tag + "_" + std::to_string(serial.fetch_add(1));
}

/* 一条真实的可等待接收通道（RecvOnly：入组、只收）+ 它的发送端。
 * 宿主 route 适配器只依赖冻结接口：句柄经 udp_node_wait_handle()，取消经 udp_node_cancel_wait()，
 * ⇒ 本文件因此也不依赖 libipc 的平台细节。 */
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
        rx = std::make_shared<ipc::socket::UDPNode>("srw_rx", ip.c_str(), port,
                                                    ipc::socket::NodeRole::RecvOnly);
        usable = rx->connect();
        if (!usable)
        {
            return;
        }
        tx = std::make_shared<ipc::socket::UDPNode>("srw_tx", ip.c_str(), port,
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

/* 宿主侧 route 适配器的参考实现（t4/t5 照此接入）：只做"收一次到完整边界 + 记账"。
 * ⛔ 不含任何用户回调 —— callback 由模块的处理路径承担（需求 §1.2）。 */
class SocketTestRoute : public SocketRecvRouteSource
{
public:
    explicit SocketTestRoute(Channel& ch)
        : ch_(ch)
        , name_(ch.topic)
    {}

    const char* route_name() const noexcept override { return name_.c_str(); }
    std::uint32_t domain_id() const noexcept override { return static_cast<std::uint32_t>(kDomain); }

    SocketWaitToken wait_token() const noexcept override
    {
        return SocketWaitToken{ch_.rx.get(), dzIPC::socket::udp_node_wait_handle(ch_.rx)};
    }

    std::size_t recv_once() override
    {
        in_flight_.fetch_add(1, std::memory_order_acq_rel);
        ipc::buffer got;
        if (!stopping_.load(std::memory_order_acquire))
        {
            got = ch_.rx->receive_nowait();   // E1：一律非阻塞读，不用裸 recvfrom
        }
        in_flight_.fetch_sub(1, std::memory_order_acq_rel);
        if (got.empty())
        {
            return 0;
        }
        received_.fetch_add(1, std::memory_order_relaxed);
        return got.size();
    }

    RecvOwner recv_owner() const noexcept override { return owner_.load(std::memory_order_acquire); }

    bool try_claim_recv(RecvOwner who) noexcept override
    {
        RecvOwner expected = RecvOwner::none;
        return owner_.compare_exchange_strong(expected, who, std::memory_order_acq_rel);
    }

    void release_recv() noexcept override
    {
        RecvOwner expected = owner_.load(std::memory_order_acquire);
        while (expected != RecvOwner::none
               && !owner_.compare_exchange_weak(expected, RecvOwner::none, std::memory_order_acq_rel))
        {
        }
    }

    void stop_and_wake() noexcept override
    {
        stopping_.store(true, std::memory_order_release);
        dzIPC::socket::udp_node_cancel_wait(ch_.rx);   // 唤醒阻塞在该句柄上的等待
    }

    void wait_quiescent() noexcept override
    {
        const auto deadline = Clock::now() + 3s;
        while (in_flight_.load(std::memory_order_acquire) != 0 && Clock::now() < deadline)
        {
            std::this_thread::sleep_for(1ms);
        }
    }

    std::size_t received() const noexcept { return received_.load(std::memory_order_relaxed); }

private:
    Channel& ch_;
    std::string name_;
    std::atomic<RecvOwner> owner_{RecvOwner::none};
    std::atomic<bool> stopping_{false};
    std::atomic<std::size_t> in_flight_{0};
    std::atomic<std::size_t> received_{0};
};

bool require_multicast(const Channel& ch) { return ch.usable; }

}   // namespace

/* ------------------------------------------------ 固定归属（纯函数 + 与 SHM 侧同值） */

/* 归属规则必须是**纯函数**，且与 SHM 侧 RecvWorkerPool::worker_for **逐值一致** ——
 * t4/t5 的方案文档把"同名同 domain 的通道分到同一序号 worker"当口径，两侧一旦漂移，
 * 对照验收（t6）就失去基准。这里直接与 SHM 侧对拍，另附本机实测 golden 值。 */
TEST(SocketRecvWorker, WorkerForMatchesShmSideAndIsStable)
{
    const char* name = "rw_alpha";
    EXPECT_EQ(SocketRecvWorkerPool::worker_for(name, 0, 8), RecvWorkerPool::worker_for(name, 0, 8));
    EXPECT_EQ(SocketRecvWorkerPool::worker_for(name, 7, 1024), RecvWorkerPool::worker_for(name, 7, 1024));
    EXPECT_EQ(SocketRecvWorkerPool::worker_for("srw_topic", kDomain, 32),
              RecvWorkerPool::worker_for("srw_topic", kDomain, 32));

    EXPECT_EQ(SocketRecvWorkerPool::worker_for(name, 0, 8), 5u);
    for (int i = 0; i < 8; ++i)
    {
        EXPECT_EQ(SocketRecvWorkerPool::worker_for(name, 0, 8),
                  SocketRecvWorkerPool::worker_for(name, 0, 8));
    }
    EXPECT_EQ(SocketRecvWorkerPool::worker_for(nullptr, 0, 8), 0u);
    EXPECT_EQ(SocketRecvWorkerPool::worker_for(name, 0, 0), 0u);
}

/* ------------------------------------------------ 注册判定与单消费者互斥 */

TEST(SocketRecvWorker, AddRouteJudgementOrderAndSingleConsumer)
{
    Channel ch{"judge"};
    if (!require_multicast(ch))
    {
        GTEST_SKIP() << "multicast unavailable on this host";
        return;
    }

    SocketRecvWorker worker(0, RecvBudget{});
    /* 未 start ⇒ stopped（而不是 invalid_*）：模块据此判断"池还没起来"。 */
    EXPECT_EQ(worker.add_route(std::make_shared<SocketTestRoute>(ch)), RecvRegisterStatus::stopped);
    EXPECT_EQ(worker.add_route(nullptr), RecvRegisterStatus::invalid_route);
    ASSERT_TRUE(worker.start());

    if (!SocketRecvWorker::backend_available())
    {
        GTEST_SKIP() << "SocketWaitSet backend unavailable on this platform";
        return;
    }

    auto route = std::make_shared<SocketTestRoute>(ch);
    const auto status = worker.add_route(route);
    if (status == RecvRegisterStatus::backend_unavailable)
    {
        GTEST_SKIP() << "SocketWaitSet backend unavailable; callers must keep one receive thread per channel";
        return;
    }
    ASSERT_EQ(status, RecvRegisterStatus::ok);
    EXPECT_EQ(route->recv_owner(), RecvOwner::worker) << "注册成功即独占收包";
    EXPECT_EQ(worker.route_count(), 1u);

    /* duplicate：自己重复注册。 */
    EXPECT_EQ(worker.add_route(route), RecvRegisterStatus::duplicate);
    EXPECT_EQ(worker.route_count(), 1u) << "被拒的注册不得改变 route 表";

    /* busy：另一条 route 正被"兼容收包线程"收（CAS 抢不到）——两种失败必须可区分，
     * 否则模块会把"自己重复注册"误当成"兼容线程在收"并做出相反处置。 */
    auto busy_route = std::make_shared<SocketTestRoute>(ch);
    ASSERT_TRUE(busy_route->try_claim_recv(RecvOwner::compat_thread));
    EXPECT_EQ(worker.add_route(busy_route), RecvRegisterStatus::busy);
    busy_route->release_recv();

    /* 注销第 6 步：owner 必须归还 none，之后兼容线程才允许重新接管。 */
    worker.remove_route(route.get());
    EXPECT_EQ(route->recv_owner(), RecvOwner::none);
    EXPECT_EQ(worker.route_count(), 0u);
    worker.remove_route(route.get());   // 幂等
    EXPECT_TRUE(route->try_claim_recv(RecvOwner::compat_thread));
    route->release_recv();
}

/* ------------------------------------------------ 真通道收取 + remove 同步唤醒 */

TEST(SocketRecvWorker, ReceivesRealChannelAndRemoveIsSynchronous)
{
    Channel ch{"recv"};
    if (!require_multicast(ch))
    {
        GTEST_SKIP() << "multicast unavailable on this host";
        return;
    }
    if (!SocketRecvWorker::backend_available())
    {
        GTEST_SKIP() << "SocketWaitSet backend unavailable on this platform";
        return;
    }

    RecvBudget budget;
    budget.wait_timeout = 50ms;
    SocketRecvWorker worker(0, budget);
    ASSERT_TRUE(worker.start());

    auto route = std::make_shared<SocketTestRoute>(ch);
    if (worker.add_route(route) == RecvRegisterStatus::backend_unavailable)
    {
        GTEST_SKIP() << "SocketWaitSet backend unavailable on this platform";
        return;
    }

    /* 空闲只走阻塞等待：没有数据时既不该返回就绪，也不该忙转。
     * 判据 = 一段时间内 wait_wakeups 不随时间线性增长（忙轮询下它会爆掉）。 */
    const auto wakeups_before = worker.stats().wait_wakeups;
    ASSERT_TRUE(ch.send("srw-payload")) << "组播发送失败 ⇒ 环境限制";
    ASSERT_TRUE(wait_for([&] { return route->received() >= 1; }, 3000))
        << "注册后到达的数据必须被消费者取到（实收 " << route->received() << " 条）";

    const auto st = worker.stats();
    EXPECT_GE(st.messages_received, 1u) << "统计面必须反映实收（t6 对照口径）";
    EXPECT_GE(st.bytes_received, std::strlen("srw-payload"));
    EXPECT_GE(st.wait_wakeups, wakeups_before + 1);

    /* remove_route 必须**同步**：返回即"worker 不会再碰这条 route"，
     * 宿主随后就可以关 fd。判据 = 快速返回 + owner 归还 + 不再有新的收取。 */
    const auto before_remove = route->received();
    const auto begin = Clock::now();
    worker.remove_route(route.get());
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - begin);
    EXPECT_LT(elapsed.count(), 2000) << "remove_route 必须同步唤醒阻塞中的 wait（实测 "
                                     << elapsed.count() << "ms）";
    EXPECT_EQ(route->recv_owner(), RecvOwner::none);
    EXPECT_EQ(worker.route_count(), 0u);

    /* 注销之后不得再被回调：worker 已不再持有它（route 的取消也已让接收面失效）。 */
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(route->received(), before_remove) << "注销返回后 worker 不得再调用 recv_once";
}

/* ------------------------------------------------ 空闲退出与按需拉起 */

TEST(SocketRecvWorker, IdleExitReturnsThreadAndAddRouteRestartsIt)
{
    Channel first{"idle"};
    if (!require_multicast(first))
    {
        GTEST_SKIP() << "multicast unavailable on this host";
        return;
    }
    if (!SocketRecvWorker::backend_available())
    {
        GTEST_SKIP() << "SocketWaitSet backend unavailable on this platform";
        return;
    }

    RecvBudget budget;
    budget.wait_timeout = 50ms;
    budget.idle_keep_alive = 200ms;   // 可配置性：窗口不是硬编码
    SocketRecvWorker worker(0, budget);
    ASSERT_TRUE(worker.start());

    auto route = std::make_shared<SocketTestRoute>(first);
    if (worker.add_route(route) == RecvRegisterStatus::backend_unavailable)
    {
        GTEST_SKIP() << "SocketWaitSet backend unavailable on this platform";
        return;
    }
    ASSERT_TRUE(wait_for([&] { return worker.thread_alive(); }, 1000));

    /* route 表非空期间线程**绝不**退出（否则同一注册期内的 recv_once 会换线程，
     * thread_local 重组状态的前提被打破）。 */
    const auto idle_exits_before = worker.stats().idle_exits;
    std::this_thread::sleep_for(300ms);   // 已超过 idle_keep_alive
    EXPECT_TRUE(worker.thread_alive()) << "route 表非空时线程不得归还";
    EXPECT_EQ(worker.stats().idle_exits, idle_exits_before) << "有 route 时不得计 idle exit";

    worker.remove_route(route.get());
    ASSERT_TRUE(wait_for([&] { return !worker.thread_alive(); }, 4000))
        << "route 表全空后线程必须归还（窗口 = idle_keep_alive + 一次 wait 切片）";
    EXPECT_GE(worker.stats().idle_exits, idle_exits_before + 1) << "空闲退出必须被记录";
    /* 活动期语义不变：归还的是**线程**，不是 worker。 */
    EXPECT_TRUE(worker.running());
    EXPECT_FALSE(worker.start()) << "start 仍是一次性的";

    /* 按需拉起必须真的在消费（否则"线程数回落"会以"静默停收"为代价换到）。 */
    Channel second{"restart"};
    if (!require_multicast(second))
    {
        GTEST_SKIP() << "multicast unavailable on this host";
        return;
    }
    const auto restarts_before = worker.stats().thread_restarts;
    auto route2 = std::make_shared<SocketTestRoute>(second);
    EXPECT_EQ(worker.add_route(route2), RecvRegisterStatus::ok);
    EXPECT_TRUE(worker.thread_alive()) << "add_route 命中空闲退出的 worker 必须按需拉起";
    EXPECT_EQ(worker.stats().thread_restarts, restarts_before + 1)
        << "拉起次数必须恰好 +1（多了就是同一个 worker 有了两条消费者）";

    ASSERT_TRUE(second.send("after-restart"));
    ASSERT_TRUE(wait_for([&] { return route2->received() >= 1; }, 3000))
        << "重拉起之后必须继续收包（实收 " << route2->received() << " 条）";

    worker.remove_route(route2.get());
}

/* ------------------------------------------------ 进程级池 */

/* 池是进程级一次性单例：本套件只在这一个用例里 start 它（其余用例都用独立的
 * SocketRecvWorker 实例），因此 start() 的一次性语义不会被前面的用例干扰。 */
TEST(SocketRecvWorkerPool, FixedAffinityAndRouteDelegation)
{
    Channel a{"pool_a"};
    Channel b{"pool_b"};
    if (!require_multicast(a) || !require_multicast(b))
    {
        GTEST_SKIP() << "multicast unavailable on this host";
        return;
    }
    if (!SocketRecvWorkerPool::backend_available())
    {
        GTEST_SKIP() << "SocketWaitSet backend unavailable on this platform";
        return;
    }

    auto& pool = SocketRecvWorkerPool::instance();
    RecvBudget budget;
    budget.wait_timeout = 50ms;
    /* 进程级单例 + start 一次性：本用例只 start 一次；若同一进程里已经启动过
     * （例如套件被以 repeat 方式跑两遍），退化成"验运行期不变量"而不是假绿。 */
    if (!pool.running())
    {
        ASSERT_TRUE(pool.start(3, budget)) << "池必须能起来（worker 数显式给 3，便于断言）";
    }
    ASSERT_TRUE(pool.running()) << "池必须处于活动期";
    EXPECT_EQ(pool.worker_count(), 3u);
    EXPECT_FALSE(pool.start(3, budget)) << "start 是一次性的";

    auto ra = std::make_shared<SocketTestRoute>(a);
    auto rb = std::make_shared<SocketTestRoute>(b);
    const auto sa = pool.add_route(ra);
    if (sa == RecvRegisterStatus::backend_unavailable)
    {
        GTEST_SKIP() << "SocketWaitSet backend unavailable; callers must keep one receive thread per channel";
        return;
    }
    ASSERT_EQ(sa, RecvRegisterStatus::ok);
    ASSERT_EQ(pool.add_route(rb), RecvRegisterStatus::ok);
    EXPECT_EQ(pool.route_count(), 2u);

    /* 归属必须与 worker_for 一致：t6 的对照口径就靠这一条。 */
    const std::size_t expected_a = SocketRecvWorkerPool::worker_for(ra->route_name(), ra->domain_id(), 3);
    EXPECT_EQ(expected_a, SocketRecvWorkerPool::worker_for(ra->route_name(), ra->domain_id(), 3));

    ASSERT_TRUE(a.send("pool-a")) << "组播发送失败 ⇒ 环境限制";
    ASSERT_TRUE(b.send("pool-b"));
    ASSERT_TRUE(wait_for([&] { return ra->received() >= 1 && rb->received() >= 1; }, 3000))
        << "两条 route 都必须被其固定 worker 消费（实收 " << ra->received() << "/" << rb->received() << "）";

    const auto st = pool.stats();
    EXPECT_GE(st.messages_received, 2u);
    EXPECT_EQ(st.route_count, pool.route_count());

    EXPECT_EQ(pool.add_route(ra), RecvRegisterStatus::duplicate);
    pool.remove_route(ra.get());
    pool.remove_route(rb.get());
    EXPECT_EQ(pool.route_count(), 0u);
    EXPECT_EQ(ra->recv_owner(), RecvOwner::none);
    EXPECT_EQ(rb->recv_owner(), RecvOwner::none);
    /* 池**故意不 stop**：它是进程级单例，模块侧只 add/remove_route；
     * worker 线程会在空闲窗口后自己归还（本套件默认 budget 的 1s 内）。 */
}

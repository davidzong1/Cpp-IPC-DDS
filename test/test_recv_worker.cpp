/* 阶段 5 `RecvWorker` / `RecvWorkerPool` 聚焦验收：固定归属、预算安全边界、
 * 注销协议、单消费者互斥、显式回退信号。
 *
 * 接口契约：`ipc-transport-phase5-shared-wait-layer-contract-*.md`（owner: ipc-transport）。
 * 设计依据：docs/消息接收架构改造/{事件驱动线程池需求.md §3.2/§5/§6,
 * shm_sub_thread_consolidation_plan.md 阶段 5}。
 *
 * ── 为什么这些判据是承重的（不是"跑绿即过"）────────────────────────────────
 * 本层是三个模块（shm_ser_cli / socket_pub_sub / socket_ser_cli）共同接入的
 * 共享收包层，它的失效方式全部是**静默**的：
 *   ① 归属不稳定（同一 route 换 worker）⇒ thread_local 分片缓存换槽 ⇒ 多片消息
 *      重组失败 ⇒ 偶发丢消息，功能测「最终又能收到」抓不到（需求 §2.1）。
 *   ② 注销不等在途 recv_once ⇒ worker 在宿主已 release 的 route 上继续 recv ⇒
 *      偶发 SIGSEGV；或反过来"注销返回了但 worker 还在调 recv_once"⇒ 宿主按
 *      协议释放资源后被打。
 *   ③ 预算耗尽当丢弃处理 ⇒ 热话题下**静默丢消息**（用户侧只能看到"偶尔少一条"）。
 *   ④ 单消费者互斥漏掉 ⇒ 兼容 subscribe_thread_ 与 worker 双收同一条 route，
 *      同一字节被消费两次（消息丢一半，且两边都"没报错"）。
 *   ⑤ 后端不可用却静默走 try_recv 轮询 ⇒ 空转吃满 CPU，而"功能正常"。
 *   ⑥ recv_once 抛异常逃出 worker 线程 ⇒ 整个进程 terminate（不是该 route 坏，
 *      是**进程**死）。
 * 故每条都以**数值边界 / 因果序**断言，不接受"没报错即通过"。
 *
 * ── 为什么宿主用真实 ipc::route + 真 RouteSession，而不是替身 ──────────────
 * `RecvRouteSource` 是宿主（模块）实现的接口，替身只能证明"接口能被调用"，
 * 证明不了"worker 的调用时序与阶段 2 的 RouteSession 协议合得上"。这里用真实
 * `ipc::route`（receiver）+ 真 `dzIPC::shm::RouteSession` 做 lease 层，于是
 * "recv_once 内部 lease 配对"这件事由 RouteSession 自己的 I4 计数（漏配对 ⇒
 * wait_quiescent 永不返回）间接守门 —— remove_route 会调 wait_quiescent，若
 * 宿主漏配对，用例会**挂住**而不是假绿（ctest 侧按超时红灯处理）。
 *
 * ── 段名纪律 ────────────────────────────────────────────────────────────
 * 名字不得含 `/`（POSIX shm 名只允许一个前导斜杠）；每个用例用 RAII 在所有
 * route 对象析构**之后** clear_storage（test_sercli_auto_path 的历史教训）。
 */
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#include "dzIPC/shm_route_session.h"
#include "dzIPC/threepools/recv_worker.h"
#include "libipc/ipc.h"

#include <gtest/gtest.h>

namespace {

using namespace dzIPC::threepools;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

/* 有界等待：所有等待都有上界，超时即按调用点语义判定（不靠"睡够就行"）。 */
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

/* 唯一段名 + 析构时清段。声明在 route 对象**之前**，析构逆序保证 route 先释放。 */
struct RouteName
{
    std::string name;

    explicit RouteName(const char* tag)
    {
        static std::atomic<int> n{0};
        name = std::string("rw_") + tag + "_" + std::to_string(n.fetch_add(1));
    }

    ~RouteName() { ipc::route::clear_storage(name.c_str()); }

    const char* c_str() const { return name.c_str(); }
};

std::shared_ptr<ipc::route> make_sender(const RouteName& rn)
{
    return std::make_shared<ipc::route>(rn.c_str(), ipc::sender, /*verbose=*/false);
}

std::shared_ptr<ipc::route> make_receiver(const RouteName& rn)
{
    return std::make_shared<ipc::route>(rn.c_str(), ipc::receiver, /*verbose=*/false);
}

/* `disconnect()` 叫醒阻塞中的 recv 时返回的是**零填充**而非空 buffer（实测，
 * 见 test_shm_route_session.cpp 抬头注释）。这里必须按同一事实过滤，否则
 * "断开"会被当成一条 64 字节的假消息收进来，把判据带偏。 */
bool is_wakeup_artifact(const ipc::buff_t& buf)
{
    if (buf.empty())
    {
        return true;
    }
    const auto* p = static_cast<const unsigned char*>(buf.data());
    for (std::size_t i = 0; i < buf.size(); ++i)
    {
        if (p[i] != 0)
        {
            return false;
        }
    }
    return true;
}

/* 宿主侧实现：真 route + 真 RouteSession lease 层 + 显式收包独占状态机。
 *
 * `recv_delay_ms` 用来制造"remove_route 时 recv_once 正在跑"的窗口；它模拟的是
 * socket 侧"一次 chunk_rev_* 持续到整条消息组装完成"那种合法长调用（契约 §4.2：
 * 预算只在完整边界检查，不得在组包中途切走）。 */
class TestRouteSource : public RecvRouteSource
{
public:
    explicit TestRouteSource(RouteName& rn, std::size_t recv_delay_ms = 0)
        : name_(rn.name)
        , recv_delay_ms_(recv_delay_ms)
        , create_([&rn] { return make_receiver(rn); })
    {
        session_.begin_rebuild(1, create_);
    }

    /* ---------------- RecvRouteSource ---------------- */

    const char* route_name() const noexcept override { return name_.c_str(); }
    std::uint32_t domain_id() const noexcept override { return 0; }

    bool has_pending() const noexcept override
    {
        if (after_pending_) after_pending_();
        return false;
    }

    ipc::recv_wait_token read_wait_token() const noexcept override
    {
        if (force_invalid_token_.load(std::memory_order_acquire))
        {
            return ipc::recv_wait_token{};
        }
        const auto route = session_.current_route();
        return route ? route->read_wait_token() : ipc::recv_wait_token{};
    }

    std::size_t recv_once() override
    {
        recv_calls_.fetch_add(1, std::memory_order_acq_rel);
        if (recv_delay_ms_ > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(recv_delay_ms_));
        }
        if (throw_once_.exchange(false, std::memory_order_acq_rel))
        {
            throw std::runtime_error("injected recv_once failure");
        }

        auto lease = session_.acquire_receive();
        if (!lease.has_value())
        {
            return 0;   // 已 stop_and_wake：不得再碰 route
        }
        ipc::buff_t buf = lease->route->recv(0);   // 非阻塞（契约：recv_once 不得长等）
        session_.release_receive();                // 所有出口都配对（含上面的 throw）

        if (is_wakeup_artifact(buf))
        {
            if (after_empty_) after_empty_();
            return 0;
        }
        {
            std::lock_guard<std::mutex> lock(sink_mtx_);
            sink_.emplace_back(static_cast<const char*>(buf.data()), buf.size());
        }
        return buf.size();
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

    void stop_and_wake() noexcept override { session_.stop_and_wake(); }
    void wait_quiescent() noexcept override { session_.wait_quiescent(); }

    /* 宿主义务（阶段 2 协议）：remove_route 内部按契约 §4.4 第 3 步调过
     * `stop_and_wake()`，它把 RouteSession 置成 stopping_ 并 disconnect 当前
     * route。见 shm_route_session.h「接口语义裁定」：stop_and_wake **不是终态**，
     * 但重新注册之前必须 `begin_rebuild` 复位它 —— 不复位则 acquire_receive 永远
     * 返回空 ⇒ recv_once 恒读到 0 ⇒ 该 route 收包**静默停摆**（现象正是
     * "add_route 返回 ok 却收不到消息"）。这里的 rebuild() 就是模块侧重新订阅时
     * 那段义务，用例必须照样走一遍，否则测出来的是宿主的漏配而不是 worker 的行为。 */
    void rebuild() { session_.begin_rebuild(++generation_, create_); }

    /* ---------------- 观测面（只给用例用） ---------------- */

    std::size_t recv_calls() const noexcept { return recv_calls_.load(std::memory_order_acquire); }

    std::size_t received_count() const
    {
        std::lock_guard<std::mutex> lock(sink_mtx_);
        return sink_.size();
    }

    std::vector<std::string> received() const
    {
        std::lock_guard<std::mutex> lock(sink_mtx_);
        return sink_;
    }

    // 仅在注册前设置；钩子在真实空读且 lease 已释放后执行。
    std::function<void()> after_empty_;
    std::function<void()> after_pending_;

    void throw_on_next_recv() { throw_once_.store(true, std::memory_order_release); }
    void force_invalid_token(bool on) { force_invalid_token_.store(on, std::memory_order_release); }

private:
    std::string name_;
    std::size_t recv_delay_ms_{0};
    std::function<std::shared_ptr<ipc::route>()> create_;
    std::uint32_t generation_{0};
    dzIPC::shm::RouteSession session_;
    std::atomic<RecvOwner> owner_{RecvOwner::none};
    std::atomic<std::size_t> recv_calls_{0};
    std::atomic<bool> throw_once_{false};
    std::atomic<bool> force_invalid_token_{false};
    mutable std::mutex sink_mtx_;
    std::vector<std::string> sink_;
};

/* try_send 需要订阅者已连上；有界重试，避免把"握手慢"误判成"发不出去"。 */
bool send_messages(const std::shared_ptr<ipc::route>& pub, const std::vector<std::string>& msgs)
{
    for (const auto& msg : msgs)
    {
        bool sent = false;
        for (int attempt = 0; attempt < 40 && !sent; ++attempt)
        {
            sent = pub->try_send(msg.data(), msg.size(), 50);
            if (!sent)
            {
                std::this_thread::sleep_for(5ms);
            }
        }
        if (!sent)
        {
            return false;
        }
    }
    return true;
}

/* 后端不可用（老内核 / 非 Linux）时本套件整体跳过，而不是假绿。 */
bool require_backend(RecvWorker& worker, const std::shared_ptr<TestRouteSource>& src)
{
    const auto status = worker.add_route(src);
    if (status == RecvRegisterStatus::backend_unavailable)
    {
        return false;
    }
    EXPECT_EQ(status, RecvRegisterStatus::ok);
    return true;
}

}   // namespace

/* ---------------------------------------------------- 固定归属（纯函数） */

/* 归属规则必须是**纯函数**：相同输入恒定输出（路由表可重建、可断言），domain
 * 参与哈希（不同 domain 的同名 route 落到不同 worker），且 nullptr / 0 有定义。
 * 期望值是本机实测的 golden 值 —— 它们一旦变化就说明归属规则被改过，而"改归属
 * 规则"在运行期的表现是"同一 route 换了 worker"⇒ 分片缓存换槽 ⇒ 静默丢消息。 */
TEST(RecvWorker, WorkerForIsStableAndDomainSensitive)
{
    const char* name = "rw_alpha";

    EXPECT_EQ(RecvWorkerPool::worker_for(name, 0, 8), 5u);
    EXPECT_EQ(RecvWorkerPool::worker_for(name, 0, 4), 1u);
    EXPECT_EQ(RecvWorkerPool::worker_for(name, 0, 1024), 573u);
    EXPECT_EQ(RecvWorkerPool::worker_for(name, 7, 1024), 138u);
    EXPECT_EQ(RecvWorkerPool::worker_for("rw_beta", 0, 1024), 161u);
    EXPECT_EQ(RecvWorkerPool::worker_for("rw_gamma", 0, 1024), 976u);

    /* 幂等：同一输入重复调用必须同值（否则归属表重建会漂移）。 */
    for (int i = 0; i < 8; ++i)
    {
        EXPECT_EQ(RecvWorkerPool::worker_for(name, 7, 64), RecvWorkerPool::worker_for(name, 7, 64));
    }

    /* domain 参与哈希：不同 domain 的同名 route 不得恒定撞同一 worker。 */
    bool differs = false;
    for (std::uint32_t d = 1; d <= 16 && !differs; ++d)
    {
        differs = RecvWorkerPool::worker_for(name, d, 64) != RecvWorkerPool::worker_for(name, 0, 64);
    }
    EXPECT_TRUE(differs) << "domain_id 必须参与归属哈希（需求 §3.2）";

    /* 边界：nullptr / worker_count == 0 都返回 0，不崩。 */
    EXPECT_EQ(RecvWorkerPool::worker_for(nullptr, 3, 8), 0u);
    EXPECT_EQ(RecvWorkerPool::worker_for(name, 3, 0), 0u);
}

/* ------------------------------------------------------------ 收包主路径 */

TEST(RecvWorker, AddRouteReceivesPublishedMessages)
{
    RouteName rn{"recv"};
    auto pub = make_sender(rn);
    auto src = std::make_shared<TestRouteSource>(rn);

    RecvWorker worker(0);
    ASSERT_TRUE(worker.start());
    if (!require_backend(worker, src))
    {
        GTEST_SKIP() << "recv_wait_set backend unavailable on this platform/kernel";
        return;
    }
    EXPECT_EQ(worker.route_count(), 1u);
    EXPECT_TRUE(RecvWorker::backend_available()) << "首次成功 add 之后能力探测必须判定为可用";

    ASSERT_TRUE(send_messages(pub, {"msg-1", "msg-2"}));
    ASSERT_TRUE(wait_for([&] { return src->received_count() >= 2; }, 3000))
        << "worker 必须收满两条消息（stats=" << worker.stats().messages_received << "）";

    const auto got = src->received();
    EXPECT_EQ(got[0], "msg-1");
    EXPECT_EQ(got[1], "msg-2");
    EXPECT_GE(worker.stats().messages_received, 2u);
    EXPECT_GE(worker.stats().bytes_received, 10u);

    worker.remove_route(src.get());
    EXPECT_EQ(worker.route_count(), 0u) << "remove_route 返回后该 route 必须已摘除";
}

/* 预算耗尽**不是丢弃**：max_messages_per_route=2 下连发 5 条，必须一条不少地
 * 收到。deferred FIFO 让让出的 route 在下一轮被重新选中 —— 若把预算当丢弃处理，
 * 这里会静默少收（症状是"热话题偶尔丢一条"）。 */
TEST(RecvWorker, BudgetYieldIsNotMessageLoss)
{
    RouteName rn{"budget"};
    auto pub = make_sender(rn);
    auto src = std::make_shared<TestRouteSource>(rn);

    RecvBudget budget;
    budget.max_messages_per_route = 2;
    budget.max_bytes_per_route = 1u << 20;
    budget.max_processing_time_per_route = 50ms;

    RecvWorker worker(0, budget);
    ASSERT_TRUE(worker.start());
    if (!require_backend(worker, src))
    {
        GTEST_SKIP() << "recv_wait_set backend unavailable on this platform/kernel";
        return;
    }

    std::vector<std::string> msgs;
    for (int i = 0; i < 5; ++i)
    {
        msgs.push_back("budget-" + std::to_string(i));
    }
    ASSERT_TRUE(send_messages(pub, msgs));
    ASSERT_TRUE(wait_for([&] { return src->received_count() >= 5; }, 5000))
        << "预算耗尽必须是「让出」而不是「丢弃」（实收 " << src->received_count() << " 条）";
    EXPECT_GE(worker.stats().budget_yields, 1u) << "max_messages_per_route=2 下必须发生过预算让出";
    EXPECT_EQ(src->received().size(), 5u);

    worker.remove_route(src.get());
}

/* ------------------------------------------- 单消费者互斥（与兼容线程） */

/* 兼容 subscribe_thread_ 正在 recv 时 add_route 必须返回 busy 且**不接管**
 * （route_count 保持 0）。若这里返回 ok，两路会同时 recv 同一条 route：同一条
 * 字节被消费两次，两边都不报错。 */
TEST(RecvWorker, AddRouteIsRefusedWhileCompatThreadOwnsRoute)
{
    RouteName rn{"busy"};
    auto src = std::make_shared<TestRouteSource>(rn);

    RecvWorker worker(0);
    ASSERT_TRUE(worker.start());

    ASSERT_TRUE(src->try_claim_recv(RecvOwner::compat_thread)) << "模拟兼容收包线程已接管";
    EXPECT_EQ(worker.add_route(src), RecvRegisterStatus::busy);
    EXPECT_EQ(worker.route_count(), 0u) << "被拒的 route 不得进入归属表";

    src->release_recv();
    EXPECT_EQ(worker.add_route(src), RecvRegisterStatus::ok);
    EXPECT_EQ(worker.route_count(), 1u);

    worker.remove_route(src.get());
    EXPECT_EQ(src->recv_owner(), RecvOwner::none)
        << "remove_route 第 6 步必须把收包独占归还 none，之后兼容线程才允许重新接管";
    EXPECT_TRUE(src->try_claim_recv(RecvOwner::compat_thread));
    src->release_recv();
}

/* 同一 route 重复 add：必须报 duplicate（而不是 busy）—— 消费方要靠这个区分
 * "自己重复注册"与"兼容线程在收"，两者对应的处置完全相反。 */
TEST(RecvWorker, DuplicateAddIsReportedAsDuplicate)
{
    RouteName rn{"dup"};
    auto src = std::make_shared<TestRouteSource>(rn);

    RecvWorker worker(0);
    ASSERT_TRUE(worker.start());
    if (!require_backend(worker, src))
    {
        GTEST_SKIP() << "recv_wait_set backend unavailable on this platform/kernel";
        return;
    }
    EXPECT_EQ(worker.add_route(src), RecvRegisterStatus::duplicate);
    EXPECT_EQ(worker.route_count(), 1u) << "重复 add 不得改变归属表";
    EXPECT_EQ(src->recv_owner(), RecvOwner::worker) << "重复 add 不得把 owner 打回 none";

    worker.remove_route(src.get());
}

/* ---------------------------------------------------------- 注销协议 */

/* remove_route 必须**同步**等到在途 recv_once 归零，且返回后不再对该 route 发起
 * 任何新调用。判据是因果序 + 数值边界：
 *   · 返回耗时 ≥ recv_once 的剩余睡眠（说明真的等了）；
 *   · 返回后 200ms 内 recv_calls 不再增长（说明摘除生效，不是"等了但没摘"）。
 * 若实现不等在途调用，宿主会按协议 release/reset 已被 worker 继续使用的 route。 */
TEST(RecvWorker, RemoveRouteWaitsForInFlightRecvAndStopsCallingIt)
{
    RouteName rn{"quiesce"};
    auto pub = make_sender(rn);
    auto src = std::make_shared<TestRouteSource>(rn, /*recv_delay_ms=*/300);

    RecvWorker worker(0);
    ASSERT_TRUE(worker.start());
    if (!require_backend(worker, src))
    {
        GTEST_SKIP() << "recv_wait_set backend unavailable on this platform/kernel";
        return;
    }

    ASSERT_TRUE(send_messages(pub, {"slow-1"}));
    ASSERT_TRUE(wait_for([&] { return src->recv_calls() >= 1; }, 3000))
        << "worker 必须已经进入 recv_once（否则本用例没造出在途窗口）";

    const auto begin = Clock::now();
    worker.remove_route(src.get());
    const auto elapsed = Clock::now() - begin;

    EXPECT_GE(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 100)
        << "remove_route 必须等在途 recv_once 结束（实测耗时 "
        << std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count() << "ms）";

    const std::size_t calls_after_remove = src->recv_calls();
    std::this_thread::sleep_for(200ms);
    EXPECT_EQ(src->recv_calls(), calls_after_remove)
        << "remove_route 返回后不得再对这条 route 调用 recv_once";
    EXPECT_EQ(worker.route_count(), 0u);

    /* 幂等 + nullptr 安全。 */
    worker.remove_route(src.get());
    worker.remove_route(nullptr);
}

/* ------------------------------------------------------------ 停止语义 */

/* stop() 必须唤醒阻塞在 wait 里的 worker 并在有界时间内 join 返回（幂等）。
 * 若 stop 只置标志不唤醒，这里会挂到 wait 超时甚至永久阻塞 —— 那正是"析构先
 * 唤醒再 join"这条顺序纪律要守的东西（需求 §2.5）。 */
TEST(RecvWorker, StopWakesBlockedWaitAndIsIdempotent)
{
    RouteName rn{"stop"};
    auto src = std::make_shared<TestRouteSource>(rn);

    RecvBudget budget;
    budget.wait_timeout = 5000ms;   // 长超时：stop 若不唤醒，本用例必然超时红灯

    RecvWorker worker(0, budget);
    ASSERT_TRUE(worker.start());
    if (!require_backend(worker, src))
    {
        GTEST_SKIP() << "recv_wait_set backend unavailable on this platform/kernel";
        return;
    }
    std::this_thread::sleep_for(50ms);   // 让 worker 进入 wait

    const auto begin = Clock::now();
    worker.stop();
    const auto elapsed = Clock::now() - begin;
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 1000)
        << "stop 必须在有界时间内返回（wait_timeout=" << budget.wait_timeout.count() << "ms）";
    EXPECT_FALSE(worker.running());

    worker.stop();   // 幂等
    EXPECT_EQ(worker.add_route(src), RecvRegisterStatus::stopped);
    EXPECT_FALSE(worker.start()) << "stop 之后不得重启（归属表与线程所有权已失效）";
}

/* ---------------------------------------------------- 参数与能力回退 */

TEST(RecvWorker, AddRouteRejectsInvalidArguments)
{
    RouteName rn{"invalid"};
    auto src = std::make_shared<TestRouteSource>(rn);

    RecvWorker worker(0);
    ASSERT_TRUE(worker.start());

    EXPECT_EQ(worker.add_route(nullptr), RecvRegisterStatus::invalid_route);

    src->force_invalid_token(true);
    EXPECT_EQ(worker.add_route(src), RecvRegisterStatus::invalid_token)
        << "无效 token 必须显式报 invalid_token（模块据此回退兼容线程），不得静默接管";
    EXPECT_EQ(src->recv_owner(), RecvOwner::none) << "被拒的 add 不得留下 worker owner";

    src->force_invalid_token(false);
    EXPECT_EQ(worker.add_route(src), RecvRegisterStatus::ok);
    worker.remove_route(src.get());

    /* 未 start 的 worker：stopped，不是 invalid_route（消费方据此判断"池没起来"）。 */
    RecvWorker idle(1);
    EXPECT_EQ(idle.add_route(src), RecvRegisterStatus::stopped);
}

/* -------------------------------------------------------- 异常隔离 */

/* 宿主 recv_once 抛出的异常不得逃出 worker 线程（否则整个进程 terminate），
 * 且该 worker 必须继续服务其它 route / 后续消息。 */
TEST(RecvWorker, RecvOnceExceptionDoesNotKillWorker)
{
    RouteName rn{"throw"};
    auto pub = make_sender(rn);
    auto src = std::make_shared<TestRouteSource>(rn);

    RecvWorker worker(0);
    ASSERT_TRUE(worker.start());
    if (!require_backend(worker, src))
    {
        GTEST_SKIP() << "recv_wait_set backend unavailable on this platform/kernel";
        return;
    }

    src->throw_on_next_recv();
    ASSERT_TRUE(send_messages(pub, {"after-throw"}));
    ASSERT_TRUE(wait_for([&] { return src->received_count() >= 1; }, 3000))
        << "抛过一次异常之后 worker 必须还活着并继续收包";
    EXPECT_GE(worker.stats().recv_errors, 1u);

    worker.remove_route(src.get());
}

/* ------------------------------------------------------- 进程级 worker 池 */

/* 池按固定归属把 route 分到 N 个 worker；池是**一次性**的进程级单例（故意泄漏，
 * 与 LocalPubSubRegistry / ShmControlScheduler 同构）。本用例是进程内唯一使用
 * instance() 的地方，结束时 stop 以便干净退出。 */
TEST(RecvWorkerPool, RoutesAreDelegatedByFixedAffinity)
{
    RouteName first{"pool_a"};
    RouteName second{"pool_b"};
    auto pub_a = make_sender(first);
    auto pub_b = make_sender(second);
    auto src_a = std::make_shared<TestRouteSource>(first);
    auto src_b = std::make_shared<TestRouteSource>(second);

    RecvWorkerPool& pool = RecvWorkerPool::instance();
    ASSERT_TRUE(pool.start(2));
    EXPECT_TRUE(pool.running());
    EXPECT_EQ(pool.worker_count(), 2u);
    EXPECT_FALSE(pool.start(2)) << "池只允许 start 一次（stop 之后不可重启）";

    const auto status_a = pool.add_route(src_a);
    if (status_a == RecvRegisterStatus::backend_unavailable)
    {
        pool.stop();
        GTEST_SKIP() << "recv_wait_set backend unavailable on this platform/kernel";
        return;
    }
    EXPECT_EQ(status_a, RecvRegisterStatus::ok);
    EXPECT_EQ(pool.add_route(src_b), RecvRegisterStatus::ok);
    EXPECT_EQ(pool.route_count(), 2u);
    EXPECT_EQ(pool.add_route(src_a), RecvRegisterStatus::duplicate);

    ASSERT_TRUE(send_messages(pub_a, {"pool-msg"}));
    ASSERT_TRUE(wait_for([&] { return src_a->received_count() >= 1; }, 3000));
    EXPECT_GE(pool.stats().messages_received, 1u);

    pool.remove_route(src_a.get());
    pool.remove_route(src_b.get());
    EXPECT_EQ(pool.route_count(), 0u);
    pool.stop();
    EXPECT_FALSE(pool.running());
}

/* ============================================ 空闲退出（idle keep-alive） */

/* 任务 E 的根因：RecvWorkerPool 一次性常驻 hardware_concurrency() 条 worker 线程，
 * 话题全部注销后线程数不回落，既有回归 test/test_sercli_auto_path.cpp:337 的
 * "析构后线程数回落"断言（4000 ms 窗口）因此失败。本节的判据都是**数值边界 +
 * 因果序**，不接受"没报错即通过"，因为这条能力的失效方式全是静默的：
 *   · 窗口没上界 ⇒ 既有断言仍然红灯（修的是同一件事，必须用同一个窗口量）；
 *   · 退出后拉不回来 ⇒ 线程数好看了，但 add_route 返回 ok 却无人消费（静默丢包）；
 *   · 拉回来多条 ⇒ 同一个 worker 两条消费者同时 recv_once（单消费者互斥被打穿）；
 *   · route 表非空时退出 ⇒ 同一 route 的 recv_once 换线程 ⇒ thread_local 分片
 *     缓存换槽 ⇒ 多片消息重组失败（需求 §2.1，症状是偶发丢消息）；
 *   · 空闲用忙转代替阻塞等待 ⇒ 线程数回落了但 CPU 被占满。
 */

/* 判据一：route 表全空之后，工作线程必须在既有断言的 4000 ms 窗口内归还；用
 * **默认 budget**（idle_keep_alive = 1000 ms）—— 池用的就是这个配置，因此这里
 * 的数字与 test_sercli_auto_path 的断言是同一把尺子。 */
TEST(RecvWorker, IdleExitReturnsThreadWithinLegacyAssertionWindow)
{
    RouteName rn{"idle"};
    auto pub = make_sender(rn);
    auto src = std::make_shared<TestRouteSource>(rn);

    RecvWorker worker(0);   // 默认 budget：idle_keep_alive = 1000 ms
    ASSERT_TRUE(worker.start());
    if (!require_backend(worker, src))
    {
        GTEST_SKIP() << "recv_wait_set backend unavailable on this platform/kernel";
        return;
    }
    EXPECT_TRUE(worker.thread_alive()) << "start 之后必须有一条工作线程";

    ASSERT_TRUE(send_messages(pub, {"idle-1"}));
    ASSERT_TRUE(wait_for([&] { return src->received_count() >= 1; }, 3000));

    worker.remove_route(src.get());
    EXPECT_EQ(worker.route_count(), 0u);

    const auto empty_at = Clock::now();
    ASSERT_TRUE(wait_for([&] { return !worker.thread_alive(); }, 4000))
        << "route 表全空后线程必须归还（既有断言窗口 4000 ms）";
    const auto elapsed =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - empty_at).count();
    EXPECT_LT(elapsed, 4000) << "实测退出耗时 " << elapsed << " ms";
    EXPECT_GE(worker.stats().idle_exits, 1u) << "空闲退出必须被记录（诊断面：idle_exits）";

    /* 活动期语义不变：线程归还的是**线程**，不是 worker —— running() 仍为 true，
     * start() 仍然只成功一次（空闲退出不是 stop）。 */
    EXPECT_TRUE(worker.running()) << "空闲退出不是 stop：活动期语义必须保持 true";
    EXPECT_FALSE(worker.start()) << "start 仍是一次性的";
    worker.remove_route(src.get());   // 幂等
}

/* 判据二：空闲退出之后，任何 add_route 必须把线程按需拉回来，并且拉回来之后
 * **真的在消费** —— 否则"线程数回落"会以"功能静默失效"为代价换到。 */
TEST(RecvWorker, AddRouteRestartsIdleExitedThreadAndKeepsReceiving)
{
    RouteName rn{"restart"};
    auto pub = make_sender(rn);
    auto src = std::make_shared<TestRouteSource>(rn);

    RecvBudget budget;
    budget.idle_keep_alive = 200ms;   // 可配置性：窗口不是硬编码

    RecvWorker worker(0, budget);
    ASSERT_TRUE(worker.start());
    if (!require_backend(worker, src))
    {
        GTEST_SKIP() << "recv_wait_set backend unavailable on this platform/kernel";
        return;
    }

    ASSERT_TRUE(send_messages(pub, {"first"}));
    ASSERT_TRUE(wait_for([&] { return src->received_count() >= 1; }, 3000));
    worker.remove_route(src.get());
    ASSERT_TRUE(wait_for([&] { return !worker.thread_alive(); }, 4000))
        << "本用例必须先造出一次空闲退出";

    src->rebuild();   // 宿主义务：注销路径走过 stop_and_wake，重新注册前必须重建 lease
    const auto restarts_before = worker.stats().thread_restarts;
    EXPECT_EQ(worker.add_route(src), RecvRegisterStatus::ok);
    EXPECT_TRUE(worker.thread_alive()) << "add_route 命中空闲退出的 worker 必须按需拉起";
    EXPECT_EQ(worker.stats().thread_restarts, restarts_before + 1)
        << "拉起次数必须恰好 +1（多了就是同一个 worker 有了两条消费者）";

    /* 拉起来的线程必须真的是消费者：不丢就绪、能收新消息。 */
    ASSERT_TRUE(send_messages(pub, {"after-restart"}));
    ASSERT_TRUE(wait_for([&] { return src->received_count() >= 2; }, 3000))
        << "重拉起之后必须继续收包（实收 " << src->received_count() << " 条）";
    EXPECT_GE(worker.stats().idle_exits, 1u);

    worker.remove_route(src.get());
}

/* 判据三：多个 add_route 并发抢同一条空闲线程 ⇒ 只成功拉起一次。若失败一次
 * 建一条，同一个 worker 会有两条线程同时遍历同一张 route 表（同一 route 的两个
 * 消费者 ⇒ 同一条字节被消费两次，且谁都没报错）。 */
TEST(RecvWorker, ConcurrentAddRouteRestartsThreadExactlyOnce)
{
    constexpr int kRoutes = 8;

    RecvBudget budget;
    budget.idle_keep_alive = 50ms;
    budget.wait_timeout = 50ms;

    RecvWorker worker(0, budget);
    ASSERT_TRUE(worker.start());

    /* 先用一条探针 route 把后端能力探明并把线程送回空闲退出态。 */
    RouteName probe{"race_probe"};
    auto probe_src = std::make_shared<TestRouteSource>(probe);
    if (!require_backend(worker, probe_src))
    {
        GTEST_SKIP() << "recv_wait_set backend unavailable on this platform/kernel";
        return;
    }
    worker.remove_route(probe_src.get());
    ASSERT_TRUE(wait_for([&] { return !worker.thread_alive(); }, 4000))
        << "本用例必须先造出一次空闲退出";

    std::vector<std::unique_ptr<RouteName>> names;
    std::vector<std::shared_ptr<ipc::route>> pubs;
    std::vector<std::shared_ptr<TestRouteSource>> srcs;
    names.reserve(kRoutes);
    for (int i = 0; i < kRoutes; ++i)
    {
        names.push_back(std::make_unique<RouteName>("race"));
        pubs.push_back(make_sender(*names.back()));
        srcs.push_back(std::make_shared<TestRouteSource>(*names.back()));
    }

    const auto restarts_before = worker.stats().thread_restarts;
    std::vector<RecvRegisterStatus> statuses(kRoutes, RecvRegisterStatus::invalid_route);
    std::atomic<int> go{0};
    std::vector<std::thread> threads;
    threads.reserve(kRoutes);
    for (int i = 0; i < kRoutes; ++i)
    {
        threads.emplace_back([&, i] {
            while (go.load(std::memory_order_acquire) == 0)
            {
                std::this_thread::yield();
            }
            statuses[i] = worker.add_route(srcs[i]);
        });
    }
    go.store(1, std::memory_order_release);
    for (auto& t : threads) t.join();

    for (int i = 0; i < kRoutes; ++i)
    {
        EXPECT_EQ(statuses[i], RecvRegisterStatus::ok) << "第 " << i << " 条 route 未被接受";
    }
    EXPECT_EQ(worker.stats().thread_restarts, restarts_before + 1)
        << "并发抢拉起必须只成功一次（实测 " << worker.stats().thread_restarts - restarts_before << " 次）";
    EXPECT_EQ(worker.route_count(), static_cast<std::size_t>(kRoutes));

    /* 每条 route 各发一条：全部必须收到 —— 拉起判据写错时这里是静默丢包现场。 */
    for (int i = 0; i < kRoutes; ++i)
    {
        ASSERT_TRUE(send_messages(pubs[i], {"race-" + std::to_string(i)})) << "第 " << i << " 条发不出去";
    }
    ASSERT_TRUE(wait_for(
        [&] {
            std::size_t total = 0;
            for (const auto& s : srcs) total += s->received_count();
            return total >= static_cast<std::size_t>(kRoutes);
        },
        5000))
        << "并发注册之后必须全部被消费（不得有 add 返回 ok 却无人消费的 route）";

    for (int i = 0; i < kRoutes; ++i) worker.remove_route(srcs[i].get());
    EXPECT_EQ(worker.route_count(), 0u);
}

/* 判据四：remove_route 与空闲退出交错。窗口取 0（下一轮空闲检查即退出）把交错
 * 压到最紧：注册/注销与线程起停的判断共用同一把表锁，交错下必须保持
 * "route_count 归零 ⇒ 线程退出"，且过程中不许丢包、不许死锁。 */
TEST(RecvWorker, RemoveRouteInterleavedWithIdleExitKeepsTableConsistent)
{
    constexpr int kRounds = 4;

    RecvBudget budget;
    budget.idle_keep_alive = 0ms;    // 下一轮空闲检查即退出（仅供测试的取值）
    budget.wait_timeout = 20ms;

    RecvWorker worker(0, budget);
    ASSERT_TRUE(worker.start());

    std::vector<std::unique_ptr<RouteName>> names;
    std::vector<std::shared_ptr<ipc::route>> pubs;
    std::vector<std::shared_ptr<TestRouteSource>> srcs;
    names.reserve(kRounds);

    for (int round = 0; round < kRounds; ++round)
    {
        names.push_back(std::make_unique<RouteName>("mix"));
        pubs.push_back(make_sender(*names.back()));
        srcs.push_back(std::make_shared<TestRouteSource>(*names.back()));

        const auto status = worker.add_route(srcs.back());
        if (round == 0 && status == RecvRegisterStatus::backend_unavailable)
        {
            GTEST_SKIP() << "recv_wait_set backend unavailable on this platform/kernel";
            return;
        }
        ASSERT_EQ(status, RecvRegisterStatus::ok) << "第 " << round << " 轮注册失败";

        const std::string msg = "mix-" + std::to_string(round);
        ASSERT_TRUE(send_messages(pubs.back(), {msg}));
        ASSERT_TRUE(wait_for([&] { return srcs.back()->received_count() >= 1; }, 3000))
            << "第 " << round << " 轮：注册成功后必须被消费，不得静默丢包";

        worker.remove_route(srcs.back().get());
        EXPECT_EQ(worker.route_count(), 0u) << "第 " << round << " 轮注销后表必须为空";
    }

    ASSERT_TRUE(wait_for([&] { return !worker.thread_alive(); }, 4000))
        << "route 表长时间为空后线程必须退出";
    EXPECT_GE(worker.stats().idle_exits, 1u);
    EXPECT_GE(worker.stats().thread_restarts, 1u) << "反复注册必须反复按需拉起";
    EXPECT_TRUE(worker.running());
}

/* 判据五：route 表**非空**期间线程不得退出。这条挡住"更激进的退出"被误当成
 * 通过 —— 表里还有 route 就退出，等于把该 route 的 recv_once 交给下一条线程，
 * thread_local 分片缓存换槽（需求 §2.1 的静默丢消息）。顺带验证空闲期是
 * **阻塞等待**：wait_timeouts 的增量必须与墙钟同量级，而不是成千上万次忙转。 */
TEST(RecvWorker, ThreadStaysAliveWhileRouteRegisteredAndIdle)
{
    RouteName rn{"stay"};
    auto pub = make_sender(rn);
    auto src = std::make_shared<TestRouteSource>(rn);

    RecvBudget budget;
    budget.idle_keep_alive = 100ms;
    budget.wait_timeout = 100ms;

    RecvWorker worker(0, budget);
    ASSERT_TRUE(worker.start());
    if (!require_backend(worker, src))
    {
        GTEST_SKIP() << "recv_wait_set backend unavailable on this platform/kernel";
        return;
    }

    const auto timeouts_before = worker.stats().wait_timeouts;
    std::this_thread::sleep_for(600ms);   // 6 倍空闲窗口，期间 route 在册但无数据

    EXPECT_TRUE(worker.thread_alive())
        << "route 表非空期间不得退出（否则该 route 的 recv_once 会换线程 ⇒ 分片缓存换槽）";
    EXPECT_EQ(worker.stats().idle_exits, 0u);
    const auto timeouts_delta = worker.stats().wait_timeouts - timeouts_before;
    EXPECT_LE(timeouts_delta, 20u)
        << "空闲期必须是阻塞等待：600 ms 内 wait 超时 " << timeouts_delta
        << " 次（忙轮询会给出成千上万次）";

    /* 闲过一整个窗口之后仍然能收 —— 线程没被误杀。 */
    ASSERT_TRUE(send_messages(pub, {"after-idle"}));
    ASSERT_TRUE(wait_for([&] { return src->received_count() >= 1; }, 3000));

    worker.remove_route(src.get());
}

/* 判据六：线程已归还（空闲退出态）期间到达的数据不得因为"按需拉起 + seq 基线
 * 已包含它"而静默丢弃。注册那一刻 token 的 seq 基线是**当下**的值，只靠
 * "seq 是否变化"判断的话，注册前已经在队列里的消息永远看不到。 */
TEST(RecvWorker, MessagesArrivingWhileThreadIsIdleExitingAreNotLost)
{
    RecvBudget budget;
    budget.idle_keep_alive = 200ms;

    RouteName first{"preq"};
    auto pub_first = make_sender(first);
    auto src_first = std::make_shared<TestRouteSource>(first);

    RecvWorker worker(0, budget);
    ASSERT_TRUE(worker.start());
    if (!require_backend(worker, src_first))
    {
        GTEST_SKIP() << "recv_wait_set backend unavailable on this platform/kernel";
        return;
    }
    ASSERT_TRUE(send_messages(pub_first, {"warm"}));
    ASSERT_TRUE(wait_for([&] { return src_first->received_count() >= 1; }, 3000));
    worker.remove_route(src_first.get());
    ASSERT_TRUE(wait_for([&] { return !worker.thread_alive(); }, 4000))
        << "本用例必须先让线程归还，才覆盖得到'退出窗口内到达的数据'";

    /* 新 route 的 receiver 已连上、数据先到、注册后到。 */
    RouteName second{"preq2"};
    auto pub_second = make_sender(second);
    auto src_second = std::make_shared<TestRouteSource>(second);
    ASSERT_TRUE(send_messages(pub_second, {"arrived-before-registration"}));
    std::this_thread::sleep_for(100ms);   // 让数据确实落进 route 队列（此刻无人消费）

    const auto restarts_before = worker.stats().thread_restarts;
    EXPECT_EQ(worker.add_route(src_second), RecvRegisterStatus::ok);
    EXPECT_TRUE(worker.thread_alive()) << "add_route 必须按需拉起线程";
    EXPECT_EQ(worker.stats().thread_restarts, restarts_before + 1);
    ASSERT_TRUE(wait_for([&] { return src_second->received_count() >= 1; }, 3000))
        << "注册前已在队列里的消息不得静默丢弃（seq 基线已包含它，只靠 seq 变化判据会漏）";

    worker.remove_route(src_second.get());
}


/* 空读已经完成、worker 尚未确认序号时提交唯一一条消息。
 * 使用真实 route/token；钩子位于 lease 释放之后，不阻塞生命周期。
 * 没有后续业务消息、补发或测试唤醒可以替 worker 恢复进展。 */
TEST(RecvWorker, PublishAfterEmptyReadIsNotAcknowledgedAsConsumed)
{
    RouteName rn{"empty_publish"};
    auto pub = make_sender(rn);
    auto src = std::make_shared<TestRouteSource>(rn);
    std::atomic<bool> injected{false}, sent{false};
    src->after_empty_ = [&] {
        if (!injected.exchange(true)) {
            const std::string msg = "only-message-after-empty";
            sent.store(pub->try_send(msg.data(), msg.size(), 0));
        }
    };
    RecvBudget budget;
    budget.wait_timeout = 1000ms;
    RecvWorker worker(0, budget);
    ASSERT_TRUE(worker.start());
    if (!require_backend(worker, src)) {
        worker.stop();
        GTEST_SKIP() << "等待后端不可用";
        return;
    }
    const bool delivered = wait_for([&] { return src->received_count() == 1; }, 200);
    EXPECT_TRUE(injected.load());
    EXPECT_TRUE(sent.load());
    EXPECT_TRUE(delivered) << "唯一消息的序号不得被空读后的确认吞掉";
    worker.remove_route(src.get());
    worker.stop();
    if (delivered) EXPECT_EQ(src->received().front(), "only-message-after-empty");
}


TEST(RecvWorker, PublishAfterStableEmptyCheckRemainsVisible)
{
    RouteName rn{"stable_empty_publish"};
    auto pub = make_sender(rn);
    auto src = std::make_shared<TestRouteSource>(rn);
    std::atomic<bool> injected{false}, sent{false};
    src->after_pending_ = [&] {
        if (!injected.exchange(true)) {
            const std::string msg = "after-stable-check";
            sent.store(pub->try_send(msg.data(), msg.size(), 0));
        }
    };
    RecvBudget budget;
    budget.wait_timeout = 1000ms;
    RecvWorker worker(0, budget);
    ASSERT_TRUE(worker.start());
    if (!require_backend(worker, src)) {
        worker.stop();
        GTEST_SKIP() << "等待后端不可用";
        return;
    }
    EXPECT_TRUE(wait_for([&] { return src->received_count() == 1; }, 200));
    EXPECT_TRUE(sent.load());
    worker.remove_route(src.get());
    worker.stop();
}

TEST(RecvWorker, EmptyReadPublishAcrossSequenceWrapIsNotLost)
{
    RouteName rn{"empty_sequence_wrap"};
    auto pub = make_sender(rn);
    auto src = std::make_shared<TestRouteSource>(rn);
    auto token = src->read_wait_token();
    ASSERT_TRUE(token.valid());
    // 无接收者运行时设置共享计数，仅用例使用；发布后真实 release 递增到零。
    const_cast<std::atomic<std::uint32_t>*>(token.sequence())->store(UINT32_MAX);
    std::atomic<bool> injected{false}, sent{false};
    src->after_empty_ = [&] {
        if (!injected.exchange(true)) {
            const std::string msg = "wrapped-sequence";
            sent.store(pub->try_send(msg.data(), msg.size(), 0));
        }
    };
    RecvBudget budget;
    budget.wait_timeout = 1000ms;
    RecvWorker worker(0, budget);
    ASSERT_TRUE(worker.start());
    if (!require_backend(worker, src)) {
        worker.stop();
        GTEST_SKIP() << "等待后端不可用";
        return;
    }
    EXPECT_TRUE(wait_for([&] { return src->received_count() == 1; }, 200));
    EXPECT_TRUE(sent.load());
    EXPECT_EQ(token.sequence()->load(), 0u);
    worker.remove_route(src.get());
    worker.stop();
}

/* W04 · SHM 接入契约复审 —— **可执行的竞态与容量偏斜清单**。
 *
 * 交付物对应：docs/消息接收架构改造/团队改造交付/接口与生命周期.md §6/§9。
 * 本文件只覆盖 W04 的**新增判据**（其余竞态条目由既有套件守门，映射见文档 §9）：
 *   L1 固定哈希下每 worker 容量偏斜是**构造性**的，`127 × worker_count` 只是上界；
 *   L2 同一 worker 上第 126 / 127 / 128 个 route 的边界与 `wait_set_full` 语义
 *      （含"失败必须归还收包独占，否则兼容线程永久 busy"）；
 *   L3 满载后摘一条即可重新接入，且已接入 route 仍正常收包（容量满不是后端不可用）；
 *   L4 `has_pending()` 恒真时的**有界**空读（kMaxEmptyPollsPerBudget）与不饿死邻居；
 *   L5 一次耗时的 `recv_once` 不能被时间预算抢占 ⇒ 同 worker 邻居确定性延迟
 *      （方案 §10.3 的"部分分片/长组包队头阻塞"最小可判定构造）；
 *   L6 add/remove churn 下单消费者互斥与 owner 归还契约（每次 remove 后 owner==none）；
 *   L7 `stop()` **不**释放 owner（模块必须先 remove_route）—— 记录契约义务，不是判 bug。
 *
 * 为什么这些必须常驻自动回路：W04 是 W05/W06 的**共同前置**，而它的失效方式全部是
 * 静默的 —— 偏斜导致的 `wait_set_full` 若不归还 owner，重新注册会永久返回 busy
 * （症状是"某几个话题偶尔收不到"）；长组包让邻居延迟若被误判为"性能抖动"，会被
 * 优化动作错误地"修"在容量或线程数上。判据一律落在**数值边界 / 因果序**上。
 *
 * 平台前提：与 test_recv_worker.cpp 同 —— 后端不可用时 GTEST_SKIP 而不是假绿。
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "dzIPC/shm_route_session.h"
#include "dzIPC/threepools/recv_worker.h"
#include "dzIPC/threepools/socket_recv_worker.h"
#include "libipc/ipc.h"

#include <gtest/gtest.h>

namespace {

using namespace dzIPC::threepools;
using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

/* 与 recv_wait_set / RecvWorker 内部的 kMaxRoutes 同源（Linux futex_waitv 128 槽留 1
 * 给中断 ⇒ 127；Windows WaitForMultipleObjects 上限 64 留 1 ⇒ 63）。 */
#if defined(_WIN32)
constexpr std::size_t kPerWorkerAssetCapacity = 63;
#else
constexpr std::size_t kPerWorkerAssetCapacity = 127;
#endif

/* 有界等待：所有等待都有上界，超时即按调用点语义判定。 */
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
        name = std::string("lc_") + tag + "_" + std::to_string(n.fetch_add(1));
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

/* disconnect 叫醒阻塞中的 recv 时返回**零填充**而非空 buffer（实测，见
 * test_shm_route_session.cpp 抬头注释）。必须按同一事实过滤。 */
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

/* 宿主侧实现：真 ipc::route + 真 RouteSession lease 层 + 显式收包独占状态机。
 *
 * 三种可注入行为，分别对应三条 W04 判据：
 *   · recv_delay_ms > 0      → 一次 recv_once 耗时较长（长组包/大消息，§10.3）；
 *   · gate（enable_gate）     → 把 recv_once 停在闸门上（确定性制造"worker 被占住"）；
 *   · stubborn_empty = true  → recv_once 恒返回 0 且 has_pending() 恒真（L4 的有界空读）。 */
class TestRouteSource : public RecvRouteSource
{
public:
    explicit TestRouteSource(RouteName& rn, std::size_t recv_delay_ms = 0, bool stubborn_empty = false)
        : name_(rn.name)
        , recv_delay_ms_(recv_delay_ms)
        , stubborn_empty_(stubborn_empty)
        , create_([&rn] { return make_receiver(rn); })
    {
        session_.begin_rebuild(1, create_);
    }

    void enable_gate()
    {
        std::lock_guard<std::mutex> lock(gate_mtx_);
        gate_enabled_ = true;
    }

    bool gate_entered() const noexcept { return gate_entered_.load(std::memory_order_acquire); }

    void open_gate()
    {
        {
            std::lock_guard<std::mutex> lock(gate_mtx_);
            gate_open_ = true;
        }
        gate_cv_.notify_all();
    }

    /* 宿主义务（阶段 2 协议）：remove_route 第 3 步调用过 stop_and_wake()，它把
     * RouteSession 置成 stopping_ 并 disconnect 当前 route。stop_and_wake **不是终态**，
     * 但重新注册之前必须 begin_rebuild 复位它 —— 不复位则 acquire_receive 永远返回空
     * ⇒ recv_once 恒读到 0 ⇒ 该 route 收包**静默停摆**（现象正是"add_route 返回 ok
     * 却收不到消息"）。这就是模块侧重新订阅时要走的那段义务。 */
    void rebuild() { session_.begin_rebuild(++generation_, create_); }

    /* ---------------- RecvRouteSource ---------------- */

    const char* route_name() const noexcept override { return name_.c_str(); }
    std::uint32_t domain_id() const noexcept override { return 0; }

    ipc::recv_wait_token read_wait_token() const noexcept override
    {
        const auto route = session_.current_route();
        return route ? route->read_wait_token() : ipc::recv_wait_token{};
    }

    std::size_t recv_once() override
    {
        recv_calls_.fetch_add(1, std::memory_order_acq_rel);

        if (stubborn_empty_)
        {
            /* L4：恒空 + has_pending() 恒真 ⇒ worker 只在**有界**次数内重试。 */
            std::this_thread::sleep_for(1ms);
            return 0;
        }

        {
            std::unique_lock<std::mutex> lock(gate_mtx_);
            if (gate_enabled_)
            {
                gate_entered_.store(true, std::memory_order_release);
                /* 有界：最多等 5s，绝不让用例挂死（闸门没开就按"无数据"返回）。 */
                gate_cv_.wait_for(lock, 5s, [this] { return gate_open_; });
                gate_entered_.store(false, std::memory_order_release);
            }
        }
        if (recv_delay_ms_ > 0)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(recv_delay_ms_));
        }

        auto lease = session_.acquire_receive();
        if (!lease.has_value())
        {
            return 0;   // 已 stop_and_wake：不得再碰 route
        }
        ipc::buff_t buf = lease->route->recv(0);   // 非阻塞（契约：recv_once 不得长等）
        session_.release_receive();                // 所有出口都配对

        if (is_wakeup_artifact(buf))
        {
            return 0;
        }
        {
            std::lock_guard<std::mutex> lock(sink_mtx_);
            sink_.emplace_back(static_cast<const char*>(buf.data()), buf.size());
        }
        return buf.size();
    }

    bool has_pending() const noexcept override { return stubborn_empty_; }

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
        session_.stop_and_wake();
        open_gate();   // 不让闸门把注销等成 2s 超时（注销路径不需要等满 5s）
    }
    void wait_quiescent() noexcept override { session_.wait_quiescent(); }

    /* ---------------- 观测面 ---------------- */

    std::size_t recv_calls() const noexcept { return recv_calls_.load(std::memory_order_acquire); }

    std::size_t received_count() const
    {
        std::lock_guard<std::mutex> lock(sink_mtx_);
        return sink_.size();
    }

private:
    std::string name_;
    std::size_t recv_delay_ms_{0};
    bool stubborn_empty_{false};
    std::function<std::shared_ptr<ipc::route>()> create_;
    dzIPC::shm::RouteSession session_;
    std::atomic<RecvOwner> owner_{RecvOwner::none};
    std::atomic<std::size_t> recv_calls_{0};
    std::uint32_t generation_{1};
    mutable std::mutex sink_mtx_;
    std::vector<std::string> sink_;

    std::mutex gate_mtx_;
    std::condition_variable gate_cv_;
    bool gate_enabled_{false};
    bool gate_open_{false};
    std::atomic<bool> gate_entered_{false};
};

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

/* 构造 N 个 route 名，使 `worker_for(name, domain, worker_count) == target`。
 * 这是"构造性偏斜"的实现：固定哈希是纯函数，敌对/不利的命名集合可以全部落到同一个
 * worker —— 因此 `127 × worker_count` 只是容量上界，不是"任意 route 集合都能接入"的保证。 */
std::vector<std::string> names_landing_on(std::size_t target, std::size_t worker_count,
                                          std::size_t count, std::uint32_t domain)
{
    std::vector<std::string> out;
    out.reserve(count);
    for (std::uint64_t i = 0; i < 1000000 && out.size() < count; ++i)
    {
        std::string n = "skew_" + std::to_string(domain) + "_" + std::to_string(i);
        if (RecvWorkerPool::worker_for(n.c_str(), domain, worker_count) == target)
        {
            out.push_back(std::move(n));
        }
    }
    return out;
}

constexpr std::size_t kWorkers = 32;
constexpr std::uint32_t kSkewDomain = 7;

}   // namespace

/* ---------------------------------------------- L1 构造性偏斜（纯函数） */

/* 判据：固定哈希下存在**同一个 worker 上远超平均值**的 route 集合，而总量上界
 * (127 × worker_count) 看起来绰绰有余。本用例把"看起来够"与"真的够"分开：
 *   · 127 × 32 = 4064 ≥ 1000 ⇒ 总量口径不会报警；
 *   · 但构造出的 128 个名字全部落在 worker 0 ⇒ 该 worker 第 128 条必然失败（L2 实测）。
 * 分布数字只记录（RecordProperty + stdout），不作硬断言 —— 断言"平均分布"本身就是
 * 把实现细节（哈希随机性）当成契约，而契约是"固定归属 + 每 worker 容量上限"。 */
TEST(LifecycleContract, ConstructiveSkewShowsTotalCapacityIsOnlyAnUpperBound)
{
    const auto same_worker = names_landing_on(0, kWorkers, 128, kSkewDomain);
    ASSERT_EQ(same_worker.size(), 128u) << "构造性偏斜必须可实现（哈希是纯函数，可枚举）";
    for (const auto& n : same_worker)
    {
        EXPECT_EQ(RecvWorkerPool::worker_for(n.c_str(), kSkewDomain, kWorkers), 0u);
    }

    /* 总量上界看起来够用： */
    EXPECT_GE(kPerWorkerAssetCapacity * kWorkers, 1000u);
    /* 但单 worker 的真实上限是常数，与 worker_count 无关： */
    EXPECT_LT(kPerWorkerAssetCapacity, same_worker.size());

    /* 顺序命名的分布（报告用）：1000 个独立话题在 32 个 worker 上的实测直方图。 */
    std::vector<std::size_t> hist(kWorkers, 0);
    for (std::uint64_t i = 0; i < 1000; ++i)
    {
        const std::string n = "topic_" + std::to_string(i);
        ++hist[RecvWorkerPool::worker_for(n.c_str(), kSkewDomain, kWorkers)];
    }
    const auto max_it = std::max_element(hist.begin(), hist.end());
    const auto min_it = std::min_element(hist.begin(), hist.end());
    ASSERT_TRUE(max_it != hist.end() && min_it != hist.end());
    /* 鸽巢下界：1000 路分到 32 个桶，最满的桶至少 ceil(1000/32) = 32。 */
    EXPECT_GE(*max_it, (1000u + kWorkers - 1) / kWorkers);
    ::testing::Test::RecordProperty("skew_1000_max_per_worker", std::to_string(*max_it));
    ::testing::Test::RecordProperty("skew_1000_min_per_worker", std::to_string(*min_it));
    ::testing::Test::RecordProperty("constructive_same_worker", std::to_string(same_worker.size()));
    std::printf("[W04-L1] 1000 顺序话题直方图 min=%zu max=%zu（worker_count=%zu）；构造性同 worker 集合=%zu\n",
                *min_it, *max_it, kWorkers, same_worker.size());
}

/* ------------------------- L2/L3 同一 worker 第 126/127/128 个 route 的边界 */

/* 这是 W04 第 (4) 条的核心判据，也是**既有套件没有覆盖**的那一段：
 *   · 第 1..126 个：ok（route_count 逐个增长）；
 *   · 第 127 个   ：仍是 ok —— wait-set 上限是 127，不是 126；
 *   · 第 128 个   ：wait_set_full（**不是** backend_unavailable：曾成功过 ⇒ 是容量满）；
 *   · 失败的那条必须归还收包独占（owner 回到 none），否则兼容线程永久 busy ⇒ 静默丢包；
 *   · 摘掉一条之后，第 128 条可重新接入并真的收到消息，且同 worker 其它 route 不受影响。
 * 构造用的 128 个名字全部落在 worker 0（kWorkers=32 下的固定哈希结果），所以本用例同时是
 * "总容量充足 ≠ 每个分片都不会满"的端到端证据。 */
TEST(LifecycleContract, SameWorkerBoundary126_127_128AndWaitSetFullSemantics)
{
    const auto names = names_landing_on(0, kWorkers, 128, kSkewDomain);
    ASSERT_EQ(names.size(), 128u);

    std::vector<std::unique_ptr<RouteName>> route_names;
    route_names.reserve(names.size());
    for (const auto& n : names)
    {
        route_names.emplace_back(new RouteName("cap"));
        route_names.back()->name = n;   // route_name 就是固定归属的 key
        ASSERT_EQ(RecvWorkerPool::worker_for(route_names.back()->c_str(), kSkewDomain, kWorkers), 0u);
    }
    std::vector<std::shared_ptr<ipc::route>> senders;
    senders.reserve(names.size());
    std::vector<std::shared_ptr<TestRouteSource>> srcs;
    srcs.reserve(names.size());
    for (auto& rn : route_names)
    {
        senders.push_back(make_sender(*rn));
        srcs.push_back(std::make_shared<TestRouteSource>(*rn));
    }
    ASSERT_TRUE(senders.size() == 128u && srcs.size() == 128u);

    RecvWorker worker(0);
    ASSERT_TRUE(worker.start());
    {
        const auto first = worker.add_route(srcs.front());
        if (first == RecvRegisterStatus::backend_unavailable)
        {
            worker.stop();
            srcs.clear();
            senders.clear();
            route_names.clear();
            GTEST_SKIP() << "recv_wait_set backend unavailable on this platform/kernel";
            return;
        }
        EXPECT_EQ(first, RecvRegisterStatus::ok);
    }
    for (std::size_t i = 1; i < 126; ++i)
    {
        EXPECT_EQ(worker.add_route(srcs[i]), RecvRegisterStatus::ok) << "第 " << (i + 1) << " 个 route";
    }
    EXPECT_EQ(worker.route_count(), 126u);

    /* 第 127 个：仍必须成功 —— 127 是**可用**上限。 */
    EXPECT_EQ(worker.add_route(srcs[126]), RecvRegisterStatus::ok) << "第 127 个 route 应可用";
    EXPECT_EQ(worker.route_count(), 127u);

    /* 第 128 个：显式容量满，且必须与"后端不可用"区分（消费方据此决定是否回退）。 */
    EXPECT_EQ(worker.add_route(srcs[127]), RecvRegisterStatus::wait_set_full) << "第 128 个 route 应被容量拒绝";
    EXPECT_NE(worker.add_route(srcs[127]), RecvRegisterStatus::backend_unavailable)
        << "曾成功 add 过 ⇒ 失败原因是容量满，不是后端不可用（否则模块会错误回退整条路径）";
    EXPECT_EQ(worker.route_count(), 127u);
    /* 失败路径必须归还收包独占：否则兼容线程/下次注册永久 busy（静默丢包）。 */
    EXPECT_EQ(srcs[127]->recv_owner(), RecvOwner::none)
        << "wait_set_full 之后 owner 必须回到 none（契约 §4.4/§4.6）";
    ASSERT_TRUE(srcs[127]->try_claim_recv(RecvOwner::compat_thread))
        << "满载失败的 route 必须能立刻被兼容线程接管（否则该话题静默丢包）";
    srcs[127]->release_recv();
    EXPECT_EQ(srcs[127]->recv_owner(), RecvOwner::none);

    /* 摘一条 → 第 128 条可接入，并且真的收到消息（容量释放是**可用**的，不是账面数字）。 */
    worker.remove_route(srcs[0].get());
    EXPECT_EQ(srcs[0]->recv_owner(), RecvOwner::none);
    EXPECT_EQ(worker.route_count(), 126u);
    EXPECT_EQ(worker.add_route(srcs[127]), RecvRegisterStatus::ok);
    EXPECT_EQ(worker.route_count(), 127u);

    ASSERT_TRUE(send_messages(senders[127], {"after-slot-freed"}));
    ASSERT_TRUE(wait_for([&] { return srcs[127]->received_count() >= 1; }, 4000))
        << "容量释放后重新接入的 route 必须真的收包";
    /* 同 worker 其它 route 不受容量事件影响。 */
    ASSERT_TRUE(send_messages(senders[1], {"neighbour-still-alive"}));
    ASSERT_TRUE(wait_for([&] { return srcs[1]->received_count() >= 1; }, 4000));

    for (auto& src : srcs)
    {
        worker.remove_route(src.get());
    }
    EXPECT_EQ(worker.route_count(), 0u);
    worker.stop();

    /* 先销毁 route 适配器（worker 已摘除），再清段（RouteName 析构）。 */
    srcs.clear();
    senders.clear();
    route_names.clear();
}

/* ---------------------------- L4 has_pending 恒真 ⇒ 有界空读且不饿死邻居 */

/* 判据：宿主实现"恒空且 has_pending() 恒真"时，worker 单轮只能重试
 * kMaxEmptyPollsPerBudget(4) 次（否则一个坏实现就把整个 worker 钉死在一条 route 上），
 * 且同 worker 的真 route 仍能在有界时间内收到消息（deferred FIFO + 阻塞 wait）。
 * 这条判据的存在理由是**静默失效**：饿死邻居不报错，只表现为"某话题偶尔很慢"。 */
TEST(LifecycleContract, StubbornHasPendingIsBoundedAndDoesNotStarveNeighbour)
{
    RouteName stubborn_rn{"stubborn"};
    RouteName live_rn{"live"};
    auto live_pub = make_sender(live_rn);
    auto stubborn = std::make_shared<TestRouteSource>(stubborn_rn, 0, /*stubborn_empty=*/true);
    auto live = std::make_shared<TestRouteSource>(live_rn);

    RecvWorker worker(0, RecvBudget{});   // 默认预算：wait_timeout = 100ms
    ASSERT_TRUE(worker.start());
    if (!require_backend(worker, stubborn))
    {
        worker.stop();
        GTEST_SKIP() << "recv_wait_set backend unavailable on this platform/kernel";
        return;
    }
    EXPECT_EQ(worker.add_route(live), RecvRegisterStatus::ok);

    /* 恒真 has_pending ⇒ 每轮最多 4 次空读；速率上界因此是 4 × (1s / wait_timeout) 量级。
     * 断言"有上界"（< 200 次/s）而不是精确值：调度抖动不应让用例假红，但"无界忙转"
     * （每秒几十万次）必然越界。 */
    ASSERT_TRUE(wait_for([&] { return stubborn->recv_calls() >= 4; }, 2000))
        << "恒真 has_pending 必须触发重试（否则 level-triggered 重检失效）";
    const auto calls_before = stubborn->recv_calls();
    std::this_thread::sleep_for(1s);
    const auto per_second = stubborn->recv_calls() - calls_before;
    EXPECT_LE(per_second, 200u) << "恒真 has_pending 不得把 worker 变成忙轮询（实测 " << per_second << " 次/s）";
    std::printf("[W04-L4] stubborn has_pending：1s 内 recv_once=%zu 次（上界判据 200）\n", per_second);

    /* 邻居仍然被服务（不饿死）。 */
    ASSERT_TRUE(send_messages(live_pub, {"live-1"}));
    ASSERT_TRUE(wait_for([&] { return live->received_count() >= 1; }, 4000))
        << "同 worker 的真 route 不得因邻居恒真 has_pending 而饿死";

    /* 注销后不得再有调用（悬挂调用的静默形态）。 */
    worker.remove_route(stubborn.get());
    ASSERT_TRUE(wait_for([&] {
        const auto a = stubborn->recv_calls();
        std::this_thread::sleep_for(30ms);
        return stubborn->recv_calls() == a;
    }, 3000)) << "remove_route 返回后不得再对已注销 route 调用 recv_once";
    EXPECT_EQ(stubborn->recv_owner(), RecvOwner::none);
    worker.remove_route(live.get());
    EXPECT_EQ(live->recv_owner(), RecvOwner::none);
    worker.stop();
}

/* ------------------------- L4b W04-F3 修复的判据：假就绪必须退避（有界循环速率） */

/* 修复前（实测 1.31M 次/s，见 W04-F3 与 build/w04/probe10_fruitless.txt）：宿主对
 * "内核报就绪却读不到"的通道只能靠 remove+add 重挂并**清零**计数 ⇒ fd 仍 LT 可读
 * ⇒ 下一轮 wait(0) 立刻再报就绪 ⇒ 周而复始。
 *
 * 本判据用**同一构造**（一个 LT 持续可读、宿主恒返回 0 的 pipe 读端）但走
 * SocketRecvWorker，断言：
 *   ① 循环速率有界（≤ 200 次/s）—— 修复后实测 ≤ 100 次/s；
 *   ② `fruitless_backoffs` / `rearm_events` **都在增长** —— 证明走的是"退避 + 到期重挂"
 *      这条路径，而不是"用例碰巧没触发假就绪"；
 *   ③ 真数据到达后的恢复延迟 ≤ 10ms 量级（不牺牲恢复延迟，方案 §10.2）。
 * ⛔ 这条必须常驻：ω 失效是**静默**的（CPU 被吃满而功能全对），且它会让 W00 门槛 3
 *    "空闲 CPU ≤ 0.20 core" 假失败。 */
TEST(LifecycleContract, SocketFruitlessReadinessBacksOffWithBoundedCycleRateAndFastRecovery)
{
    int p[2];
    if (::pipe(p) != 0)
    {
        GTEST_SKIP() << "pipe unavailable";
        return;
    }
    char seed[8];
    std::memset(seed, 'x', sizeof(seed));
    const ssize_t wr = ::write(p[1], seed, sizeof(seed));
    ASSERT_EQ(wr, static_cast<ssize_t>(sizeof(seed)));

    struct PipeRoute : SocketRecvRouteSource
    {
        explicit PipeRoute(std::string n, int fd) : name_(std::move(n)), fd_(fd) {}
        const char* route_name() const noexcept override { return name_.c_str(); }
        std::uint32_t domain_id() const noexcept override { return 0; }
        SocketWaitToken wait_token() const noexcept override
        {
            return SocketWaitToken{this, static_cast<std::uintptr_t>(fd_)};
        }
        std::size_t recv_once() override
        {
            calls_.fetch_add(1, std::memory_order_relaxed);
            if (!serve_.load(std::memory_order_acquire))
            {
                return 0;   // 恒返回 0：模拟"内核报就绪但读不到"
            }
            char c = 0;
            const ssize_t n = ::read(fd_, &c, 1);   // 真数据：非阻塞 fd（用例前置 O_NONBLOCK）
            if (n <= 0) return 0;
            got_.fetch_add(1, std::memory_order_relaxed);
            return 1;
        }
        RecvOwner recv_owner() const noexcept override { return owner_.load(); }
        bool try_claim_recv(RecvOwner who) noexcept override
        {
            RecvOwner expected = RecvOwner::none;
            return owner_.compare_exchange_strong(expected, who, std::memory_order_acq_rel);
        }
        void release_recv() noexcept override { owner_.store(RecvOwner::none); }
        void stop_and_wake() noexcept override {}
        void wait_quiescent() noexcept override {}
        std::string name_;
        int fd_;
        std::atomic<RecvOwner> owner_{RecvOwner::none};
        std::atomic<std::size_t> calls_{0};
        std::atomic<bool> serve_{false};
        std::atomic<std::size_t> got_{0};
    };

    /* recv_once 必须非阻塞（契约义务）：读端置 O_NONBLOCK，避免第二阶段在共享 worker
     * 线程上空读阻塞。 */
    (void)::fcntl(p[0], F_SETFL, O_NONBLOCK);

    auto route = std::make_shared<PipeRoute>("w04_l4b_fruitless", p[0]);
    SocketRecvWorker worker(0, RecvBudget{});
    ASSERT_TRUE(worker.start());
    const auto st = worker.add_route(route);
    if (st == RecvRegisterStatus::backend_unavailable)
    {
        worker.stop();
        ::close(p[0]);
        ::close(p[1]);
        GTEST_SKIP() << "SocketWaitSet backend unavailable on this platform/kernel";
        return;
    }
    ASSERT_EQ(st, RecvRegisterStatus::ok);

    std::this_thread::sleep_for(1s);
    const auto c0 = route->calls_.load();
    std::this_thread::sleep_for(1s);
    const auto per_second = route->calls_.load() - c0;
    const auto s = worker.stats();

    std::printf("[W04-L4b] 假就绪通道：1s 内 recv_once=%zu 次（判据 ≤200）；"
                "backoffs=%llu rearm=%llu\n",
                per_second, static_cast<unsigned long long>(s.fruitless_backoffs),
                static_cast<unsigned long long>(s.rearm_events));

    EXPECT_LE(per_second, 200u)
        << "假就绪不得把 worker 变成忙转（修复前实测 1.31M 次/s）";
    EXPECT_GE(s.fruitless_backoffs, 1u) << "未观察到退避 —— 说明本用例没触发到该路径（判据失效）";
    EXPECT_GE(s.rearm_events, 1u) << "退避到期后必须重新挂回等待集合（否则该通道永久失聪）";

    /* ③ 真数据到达后的恢复延迟：退避期内该 fd **不在**等待集合里，唤醒只能来自退避
     *    定时器，因此这条直接量"退避是否牺牲了恢复延迟"。判据取 ≤ 200ms（远小于
     *    2000ms 的注销上界、也小于一次 wait_timeout 的 2 倍余量），而实测应在 10–20ms
     *    量级（= kFruitlessBackoffMax + 调度）。 */
    (void)::write(p[1], "ab", 2);
    route->serve_.store(true, std::memory_order_release);
    const auto t0 = Clock::now();
    const bool recovered = wait_for([&] { return route->got_.load() >= 1; }, 2000);
    const auto recovery_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count();
    std::printf("[W04-L4b] 真数据到达后的恢复耗时=%lld ms（实测应 ≈10ms 量级）\n",
                static_cast<long long>(recovery_ms));
    EXPECT_TRUE(recovered) << "退避后必须能恢复收包（否则是用恢复延迟换 CPU）";
    EXPECT_LE(recovery_ms, 200) << "恢复延迟不得被退避拖长（方案 §10.2 禁止为降 CPU 牺牲恢复延迟）";

    worker.remove_route(route.get());
    EXPECT_EQ(route->recv_owner(), RecvOwner::none);
    worker.stop();
    ::close(p[0]);
    ::close(p[1]);
}

/* --------------------------- L5 一次耗时 recv_once 不可被预算抢占（§10.3） */

/* 确定性构造（不靠 sleep 猜时序）：闸门把 worker 停在 gated route 的 recv_once 内部，
 * 于是"一次调用占住整个 worker"成为**可观测状态**（gate_entered）。判据：
 *   ① 被占住期间投递邻居消息 ⇒ 邻居**收不到**：预算只在完整 recv_once 返回后才检查，
 *      即使把 max_processing_time_per_route 压到 1 µs 也无法抢占这次组包；
 *   ② 放行 ⇒ 邻居在有界时间内收到 —— 队头阻塞是"有界但真实"的，不是永久停摆；
 *   ③ 注销被闸门停住的 route 仍是有界的（stop_and_wake 会打断闸门）。
 * 这条判据把方案 §10.3 的"部分分片到达后暂停发送时同 worker 其他 route 的服务延迟"
 * 从推测变成可观测证据，并明确它**不是**可被容量/线程数"修好"的性能抖动。 */
TEST(LifecycleContract, GatedRecvOnceCannotBePreemptedByTinyBudgetAndRecovers)
{
    RouteName gated_rn{"gate"};
    RouteName neighbour_rn{"gate_neighbour"};
    auto neighbour_pub = make_sender(neighbour_rn);
    auto gated = std::make_shared<TestRouteSource>(gated_rn, /*recv_delay_ms=*/0);
    gated->enable_gate();
    auto neighbour = std::make_shared<TestRouteSource>(neighbour_rn);

    /* 1 µs 处理时间预算：若预算能抢占一次 recv_once，本用例的"被占住"状态就不可能出现。 */
    RecvBudget tiny;
    tiny.max_processing_time_per_route = std::chrono::microseconds{1};
    RecvWorker worker(0, tiny);
    ASSERT_TRUE(worker.start());
    if (!require_backend(worker, gated))
    {
        worker.stop();
        GTEST_SKIP() << "recv_wait_set backend unavailable on this platform/kernel";
        return;
    }
    EXPECT_EQ(worker.add_route(neighbour), RecvRegisterStatus::ok);
    ASSERT_TRUE(wait_for([&] { return gated->gate_entered(); }, 3000))
        << "worker 必须进入被闸门停住的 recv_once（1µs 预算不得抢占它）";

    ASSERT_TRUE(send_messages(neighbour_pub, {"held"}));
    std::this_thread::sleep_for(300ms);
    EXPECT_EQ(neighbour->received_count(), 0u)
        << "闸门未放行 ⇒ 邻居不得被服务（worker 被一次 recv_once 占住，预算不可抢占）";
    EXPECT_TRUE(worker.running());
    EXPECT_TRUE(worker.thread_alive());

    /* 放行 ⇒ 邻居必须被补服务（有界恢复，不是只测"空队列快速返回"）。 */
    const auto released = Clock::now();
    gated->open_gate();
    ASSERT_TRUE(wait_for([&] { return neighbour->received_count() >= 1; }, 4000)) << "放行后邻居必须被服务";
    const auto recovery_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - released).count();
    std::printf("[W04-L5] 闸门占住 worker 300ms+，放行后邻居恢复耗时=%lld ms（1µs 预算未抢占）\n",
                static_cast<long long>(recovery_ms));
    EXPECT_LE(recovery_ms, 4000);

    /* ③ 被闸门停住的 route 注销仍有界（stop_and_wake 打断闸门，不靠 2s 超时兜底）。 */
    const auto teardown_begin = Clock::now();
    worker.remove_route(gated.get());
    const auto teardown_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - teardown_begin).count();
    EXPECT_LT(teardown_ms, 2000) << "注销被占住的 route 必须有界（stop_and_wake 打断在途 recv_once）";
    EXPECT_EQ(gated->recv_owner(), RecvOwner::none);
    worker.remove_route(neighbour.get());
    EXPECT_EQ(neighbour->recv_owner(), RecvOwner::none);
    worker.stop();
}

/* -------------------------------- L6 add/remove churn 与 owner 归还契约 */

/* 判据：反复 add → 收包 → remove 的过程中，任何一次 add 都不得返回 busy/duplicate
 * （那是双消费者或残留 owner 的信号），每次 remove 返回后 owner 必须为 none，
 * 且兼容线程随时可以接管（证明"注销后无悬挂独占"）。 */
TEST(LifecycleContract, AddRemoveChurnAlwaysReturnsOwnerToNone)
{
    RouteName rn{"churn"};
    auto pub = make_sender(rn);
    auto src = std::make_shared<TestRouteSource>(rn);

    RecvWorker worker(0, RecvBudget{});
    ASSERT_TRUE(worker.start());
    {
        const auto first = worker.add_route(src);
        if (first == RecvRegisterStatus::backend_unavailable)
        {
            worker.stop();
            GTEST_SKIP() << "recv_wait_set backend unavailable on this platform/kernel";
            return;
        }
        EXPECT_EQ(first, RecvRegisterStatus::ok);
    }

    for (int round = 0; round < 40; ++round)
    {
        ASSERT_TRUE(send_messages(pub, {std::string("churn-") + std::to_string(round)}));
        ASSERT_TRUE(wait_for([&] { return src->received_count() >= static_cast<std::size_t>(round + 1); }, 3000))
            << "第 " << round << " 轮收包失败";

        worker.remove_route(src.get());
        EXPECT_EQ(src->recv_owner(), RecvOwner::none) << "第 " << round << " 轮 remove 后 owner 必须归还";
        ASSERT_TRUE(src->try_claim_recv(RecvOwner::compat_thread))
            << "第 " << round << " 轮后兼容线程必须能接管（否则 owner 泄漏）";
        src->release_recv();

        /* 重新注册前必须复位 lease 层（契约 §4.4 注释：stop_and_wake 不是终态）。 */
        src->rebuild();
        EXPECT_EQ(worker.add_route(src), RecvRegisterStatus::ok) << "第 " << round << " 轮重新注册失败";
    }
    worker.remove_route(src.get());
    EXPECT_EQ(worker.route_count(), 0u);
    worker.stop();
}

/* --------------------- L7 stop() 不释放 owner（记录模块义务，不是判 bug） */

/* 契约义务：worker.stop() 只停线程，**不**摘 route、**不**归还 owner。模块必须
 * 先 remove_route 再 stop（契约 §4.4 析构顺序 1-6 步）；若模块只 stop 不 remove，
 * 之后兼容线程接管会一直 busy ⇒ 该 route 静默不再收包。本用例把这条义务钉成可执行判据：
 *   · stop 之后 owner 仍为 worker、route_count 仍为 1；
 *   · 但 stop 之后 remove_route 仍然可用（幂等路径不依赖活动期）并把 owner 归还。 */
TEST(LifecycleContract, StopDoesNotReleaseOwnerSoModuleMustRemoveRouteFirst)
{
    RouteName rn{"stop_contract"};
    auto src = std::make_shared<TestRouteSource>(rn);

    RecvWorker worker(0, RecvBudget{});
    ASSERT_TRUE(worker.start());
    if (!require_backend(worker, src))
    {
        worker.stop();
        GTEST_SKIP() << "recv_wait_set backend unavailable on this platform/kernel";
        return;
    }
    EXPECT_TRUE(worker.running());
    EXPECT_EQ(worker.route_count(), 1u);

    worker.stop();
    EXPECT_FALSE(worker.running());
    EXPECT_EQ(worker.route_count(), 1u) << "stop() 不摘 route（模块义务：先 remove_route）";
    EXPECT_EQ(src->recv_owner(), RecvOwner::worker)
        << "stop() 不归还 owner（模块只 stop 不 remove ⇒ 兼容线程此后永久 busy）";
    EXPECT_FALSE(src->try_claim_recv(RecvOwner::compat_thread))
        << "此处为 true 说明 stop() 归还了 owner，与该契约不符";

    /* 仍然可注销并把 owner 归还（remove_route 不依赖活动期）。 */
    worker.remove_route(src.get());
    EXPECT_EQ(src->recv_owner(), RecvOwner::none);
    EXPECT_EQ(worker.route_count(), 0u);
    worker.stop();   // 幂等
}

/* ------------------- L8 进程级池的"首次初始化责任方"（方案 §10.4） */

/* 契约义务（写进 接口与生命周期.md §8）：进程级池的 worker 数与预算由**首次成功
 * start() 的调用方**一次性决定，之后任何 start() 都返回 false 且**不得**改变已生效的
 * 配置。因此模块侧不得依赖"我自己那一次 start 一定生效"，也不得让模块加载顺序决定
 * 最终配置 —— 这是必须由测试钉死的可判定行为，而不是注释里的约定（方案 §7.2
 * 「注释即条款必须配机械核对」）。
 *
 * 本用例放在文件最后：它是唯一使用 RecvWorkerPool::instance() 的用例，会把该进程级
 * 单例置为活动期（池随进程存活、stop 后不可重启），因此前面的用例一律只用独立的
 * RecvWorker 实例，避免互相污染。 */
TEST(LifecycleContract, PoolFirstStartDecidesWorkerCountAndBudgetOnce)
{
    RecvWorkerPool& pool = RecvWorkerPool::instance();
    if (!pool.running())
    {
        /* 首次 start 决定配置：本用例是进程内第一个调用者。 */
        RecvBudget first_budget;
        first_budget.wait_timeout = 37ms;   // 非默认值：用于证明"后到者不得改写"
        if (!pool.start(4, first_budget))
        {
            GTEST_SKIP() << "pool start failed (thread creation?) — 无法执行首次初始化判据";
            return;
        }
    }
    EXPECT_TRUE(pool.running());
    const std::size_t decided_workers = pool.worker_count();
    EXPECT_EQ(decided_workers, 4u) << "首次 start(4) 必须生效";

    /* 后到者（另一模块、不同 worker 数、不同预算）：必须失败且不改写已生效配置。 */
    RecvBudget later_budget;
    later_budget.wait_timeout = 999ms;
    EXPECT_FALSE(pool.start(8, later_budget))
        << "池是一次性的：第二个调用者必须拿到 false（不得静默重分片 / 改 worker 数）";
    EXPECT_EQ(pool.worker_count(), decided_workers)
        << "后到者的 worker_count 不得覆盖首次配置（首写者责任方）";
    EXPECT_TRUE(pool.running());

    /* 归属计算随池的 worker_count 一起固定：同一 route_name 在池内外恒一致。 */
    EXPECT_EQ(RecvWorkerPool::worker_for("lc_pool_probe", kSkewDomain, pool.worker_count()),
              RecvWorkerPool::worker_for("lc_pool_probe", kSkewDomain, decided_workers));

    /* 池的 stop 之后不可重启：模块只 add/remove_route，不得由单模块析构调用 stop
     * （那会让其它仍在收包的模块整体静默停收）。 */
    pool.stop();
    EXPECT_FALSE(pool.running());
    EXPECT_FALSE(pool.start(4)) << "stop() 之后不得重启（否则 worker_for 的模数变化 + 统计重复）";
    EXPECT_EQ(pool.add_route(nullptr), RecvRegisterStatus::invalid_route);
}

/* -------- L9 固定归属 key 必须与 generation 无关（否则注销/重注册会漂移） */

/* 判据：归属是 route_name‖domain 的纯函数。若模块把 generation（或任何每次重建都变的
 * 东西）拼进 route_name，同一话题在重建后会落进**另一个** worker —— 而 worker 迁移
 * 会让 thread_local 分片缓存换槽（需求 §2.1），症状是重建后偶发丢消息/重组失败。
 * 本用例用固定算法把这件事量化：给同一批名字加一个"带 generation 的后缀"，
 * 统计有多少比例的归属发生变化（实测 > 90%，因此"加了也不影响"不是可以赌的前提）。 */
TEST(LifecycleContract, RouteKeyMustNotCarryGenerationBecauseAffinityIsPureFunction)
{
    std::size_t moved = 0;
    constexpr std::size_t kProbe = 1000;
    for (std::size_t i = 0; i < kProbe; ++i)
    {
        const std::string stable = "dz_ipc_d3_topic_" + std::to_string(i) + "_topic";
        const std::string drift = stable + "_g2";   // 把 generation 拼进 key 的写法
        if (RecvWorkerPool::worker_for(stable.c_str(), 3, kWorkers)
            != RecvWorkerPool::worker_for(drift.c_str(), 3, kWorkers))
        {
            ++moved;
        }
    }
    ::testing::Test::RecordProperty("generation_suffix_moves_worker", std::to_string(moved));
    std::printf("[W04-L9] 把 generation 拼进 route key：%zu/%zu 条 route 会换 worker（=迁移⇒分片缓存换槽）\n",
                moved, kProbe);
    EXPECT_GT(moved, kProbe / 2)
        << "route key 必须只由稳定量（段名 + domain）构成；带 generation 的后缀会大面积改变归属";
    /* 反面：同一 (name, domain) 的归属恒定 —— 路由表可重建、可断言。 */
    for (std::size_t i = 0; i < 100; ++i)
    {
        const std::string n = "stable_" + std::to_string(i);
        EXPECT_EQ(RecvWorkerPool::worker_for(n.c_str(), 3, kWorkers),
                  RecvWorkerPool::worker_for(n.c_str(), 3, kWorkers));
    }
}

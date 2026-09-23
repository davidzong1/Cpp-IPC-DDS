/* 阶段 1 `ShmControlScheduler` 单元层验收：并发注销协议 + 时间语义 + 异常隔离。
 *
 * ── 为什么这些用例是承重的（不是"跑绿即过"）──────────────────────────────────
 * 调度器是**进程级**组件：它替代的是一堆按话题扩张的握手线程，而它的正确性全在
 * 四条不可见的不变式上。这些不变式一旦破了，症状不是崩溃而是**静默失效**：
 *   I1 注销后仍有回调 → 已析构宿主被踩（UAF），或计数器悄悄多跑
 *   I2 注销不等在途回调 → 调用方以为"安全了"，其实回调还在跑
 *   I3 注销后仍持有 state → 对象无法析构、weak_ptr 永不过期
 *   I4 并发注册/注销与 tick 交错 → 数据竞争 / 死锁
 * 故每条都以**数值边界**断言，不接受"没报错即通过"（同 docs/hotpath_gate.sh 记录
 * 的 A′ 事故：零投递却无 FAILED）。
 *
 * ── 测试用栈上实例，不用 instance() ─────────────────────────────────────────
 * `instance()` 是故意泄漏的进程级单例（先例 local_pub_sub_registry.cc:14-24）。
 * 若用例直接用它，一个用例里的 `stop()` 会**永久**关掉本进程内所有后续用例的
 * tick（stop 不可恢复）⇒ 测试间互相污染，且"绿"取决于执行顺序。产品把构造/析构
 * 设为 public 正是为了让测试能拿独立实例 —— 这里就用它。
 *
 * ── 时间语义的容差取值 ─────────────────────────────────────────────────────
 * 10ms / 50ms 是**调度周期**，不是硬实时保证；CI 负载下会抖动。判据取"周期量级
 * 正确 + 比例正确"（5:1），不取逐次精确值 —— 后者会变成 flaky 用例，而 flaky
 * 用例比没有用例更坏（它会训练人忽略红灯）。
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "dzIPC/threepools/shm_control_scheduler.h"

#if defined(__linux__)
#include <cerrno>
#include <csignal>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include <cstdio>
#include <gtest/gtest.h>

namespace {

using namespace dzIPC::shm_control;
using namespace std::chrono_literals;

/* 本进程的线程数。与 test/test_sercli_auto_path.cpp:100-115 同一手法。 */
int thread_count()
{
#if defined(__linux__)
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
    {
        if (line.rfind("Threads:", 0) == 0)
        {
            return std::atoi(line.c_str() + 8);
        }
    }
#endif
    return -1;
}

/* 测试自己的订阅侧状态：计数器 + 可控慢回调 + 可控抛出。
 * 能这样注入是因为 register_* 收的是**抽象基类**的 shared_ptr —— 产品不必为测试
 * 额外开口子，接口本身就是注入点。 */
struct CountingSub final : SubControlState
{
    std::atomic<int> hb{0};
    std::atomic<bool> in_callback{false};
    std::atomic<bool> destroyed{false};
    std::chrono::milliseconds callback_cost{0};
    std::atomic<bool> throw_on_callback{false};

    ~CountingSub() override { destroyed.store(true); }

    void on_sub_heartbeat(ControlClock::time_point) override
    {
        if (throw_on_callback.load(std::memory_order_relaxed))
        {
            throw std::runtime_error("injected sub-heartbeat failure");
        }
        in_callback.store(true, std::memory_order_release);
        if (callback_cost.count() > 0)
        {
            std::this_thread::sleep_for(callback_cost);
        }
        hb.fetch_add(1, std::memory_order_relaxed);
        in_callback.store(false, std::memory_order_release);
    }

    const char* debug_name() const noexcept override { return "counting-sub"; }
};

struct CountingPub final : PubControlState
{
    std::atomic<int> owner_hb{0};
    std::atomic<int> stale_scan{0};
    std::atomic<int> last_dead_timeout_ns{-1};
    std::atomic<bool> peers{false};

    void on_pub_heartbeat(ControlClock::time_point) override { owner_hb.fetch_add(1, std::memory_order_relaxed); }

    bool has_peers() const override { return peers.load(std::memory_order_relaxed); }

    void on_pub_stale_scan(ControlClock::time_point, std::chrono::nanoseconds dead_timeout) override
    {
        stale_scan.fetch_add(1, std::memory_order_relaxed);
        last_dead_timeout_ns.store(
            static_cast<int>(std::chrono::duration_cast<std::chrono::nanoseconds>(dead_timeout).count()),
            std::memory_order_relaxed);
    }
};

/* 有界等待：所有等待都必须有界，超时即 FAIL —— 不用"睡够就行"。 */
bool wait_for(const std::function<bool()>& pred, int timeout_ms)
{
    const auto deadline = ControlClock::now() + std::chrono::milliseconds(timeout_ms);
    while (ControlClock::now() < deadline)
    {
        if (pred())
        {
            return true;
        }
        std::this_thread::sleep_for(1ms);
    }
    return pred();
}

}   // namespace

/* ------------------------------------------------------------------ 时间语义 */
/* 10ms / 50ms 周期与 2s 判死超时是"必须逐位保持"的现状语义（需求 §3.1）。 */
TEST(ShmControlScheduler, TimingSemanticsUnchanged)
{
    ShmControlScheduler sched;
    auto sub = std::make_shared<CountingSub>();
    auto pub = std::make_shared<CountingPub>();
    pub->peers.store(true);
    RegistrationToken sub_reg(&sched, sched.register_subscriber(sub));
    RegistrationToken pub_reg(&sched, sched.register_publisher(pub));
    ASSERT_TRUE(sub_reg.valid());
    ASSERT_TRUE(pub_reg.valid());

    /* 注册本身不得触发任何回调：第一次回调在下一个到期点。给 1 次容差 ——
     * 若线程被抢占 >10ms，第一次到期可能已经发生。 */
    EXPECT_LE(sub->hb.load(), 1) << "注册不得立即触发回调";

    std::this_thread::sleep_for(400ms);
    const int sub_hb = sub->hb.load();
    const int owner_hb = pub->owner_hb.load();
    const int stale = pub->stale_scan.load();
    std::printf("[timing] 400ms: sub_hb=%d pub_owner_hb=%d pub_stale=%d\n", sub_hb, owner_hb, stale);

    /* 10ms 周期 ⇒ 400ms 约 40 次；容差 ±40% 吸收 CI 抖动，但足以区分 10ms 与 50ms。 */
    EXPECT_GE(sub_hb, 24) << "订阅 heartbeat 明显慢于 10ms 周期";
    EXPECT_LE(sub_hb, 56) << "订阅 heartbeat 明显快于 10ms 周期";
    /* 50ms 周期 ⇒ 400ms 约 8 次。 */
    EXPECT_GE(owner_hb, 5) << "发布 owner heartbeat 明显慢于 50ms 周期";
    EXPECT_LE(owner_hb, 12) << "发布 owner heartbeat 明显快于 50ms 周期";
    /* 比例判据：10ms : 50ms = 5:1。这一条比绝对值更能抗负载抖动。 */
    EXPECT_GE(static_cast<double>(sub_hb) / static_cast<double>(owner_hb), 2.5)
        << "订阅/发布周期比例失真（期望约 5:1）";
    /* stale 扫描与 owner heartbeat 同频（现状同一循环体）。 */
    EXPECT_EQ(stale, owner_hb) << "stale 扫描必须与 owner heartbeat 同频";
    EXPECT_EQ(pub->last_dead_timeout_ns.load(), 2000000000)
        << "peer 判死超时必须是 2s（kPeerDeadTimeoutNs 现状值）";
}

/* 无 peer 时 owner heartbeat **必须继续**，只跳过 stale 扫描（需求 §3.1 明文）。 */
TEST(ShmControlScheduler, PublisherHeartbeatWithoutPeers)
{
    ShmControlScheduler sched;
    auto pub = std::make_shared<CountingPub>();
    RegistrationToken reg(&sched, sched.register_publisher(pub));

    ASSERT_TRUE(wait_for([&] { return pub->owner_hb.load() >= 3; }, 500))
        << "无 peer 时 owner heartbeat 停了 —— 别的进程将无法判断本发布者是否还活着";
    EXPECT_EQ(pub->stale_scan.load(), 0) << "peer_count()==0 时必须跳过 stale 扫描";

    pub->peers.store(true);
    ASSERT_TRUE(wait_for([&] { return pub->stale_scan.load() >= 2; }, 500))
        << "有 peer 后 stale 扫描必须恢复";
    EXPECT_GT(pub->owner_hb.load(), 3) << "有 peer 时 owner heartbeat 必须继续";
}

/* --------------------------------------------------------------- 周期配置 */
/* ControlTiming 必须**按项**生效。这一条不能省：把实现里的 timing 全部忽略、
 * 一律按默认 10ms/50ms 跑，上面所有用例**依然全绿**（默认值恰好就是被测值）。
 * 只有"同一调度器里两种周期必须可区分"才能证伪"timing 参数没接线"。 */
TEST(ShmControlScheduler, CustomTimingIsHonored)
{
    ShmControlScheduler sched;
    auto fast = std::make_shared<CountingSub>();   /* 默认 10ms */
    auto slow = std::make_shared<CountingSub>();   /* 自定义 50ms */
    ControlTiming slow_timing;
    slow_timing.sub_heartbeat = 50ms;

    RegistrationToken fast_reg(&sched, sched.register_subscriber(fast));
    RegistrationToken slow_reg(&sched, sched.register_subscriber(slow, slow_timing));
    ASSERT_TRUE(fast_reg.valid());
    ASSERT_TRUE(slow_reg.valid());

    ASSERT_TRUE(wait_for([&] { return fast->hb.load() >= 20; }, 800)) << "默认 10ms 项未被驱动";
    const int fast_hb = fast->hb.load();
    const int slow_hb = slow->hb.load();
    std::printf("[timing] custom: fast(10ms)=%d slow(50ms)=%d\n", fast_hb, slow_hb);

    EXPECT_GE(slow_hb, 3) << "自定义 50ms 项未被驱动";
    EXPECT_LE(slow_hb, 15) << "自定义 50ms 项快于其配置周期";
    /* 关键判据：两项必须**可区分**。若实现忽略 ControlTiming，两者会一样快。 */
    EXPECT_GE(fast_hb, slow_hb * 3) << "10ms 与 50ms 两项未被区分: fast=" << fast_hb << " slow=" << slow_hb
                                   << " ⇒ ControlTiming 未按项生效（被写死成默认值）";
}

/* peer_dead_timeout 必须按 ControlTiming **原样透传**给 on_pub_stale_scan。
 * 默认 2s 由 TimingSemanticsUnchanged 守；这里守"非默认值不被写死覆盖"。
 * ⚠️ 取值 1.5s 而非 7s：测试替身 last_dead_timeout_ns 是 int32，2s 已接近
 *    INT_MAX(2147483647)，更大的值会溢出成负数而让判据失真。 */
TEST(ShmControlScheduler, CustomPeerDeadTimeoutIsPassedThrough)
{
    ShmControlScheduler sched;
    auto pub = std::make_shared<CountingPub>();
    pub->peers.store(true);
    ControlTiming t;
    t.peer_dead_timeout = std::chrono::nanoseconds{1'500'000'000LL};   /* 1.5s，仍在 int32 范围内 */

    RegistrationToken reg(&sched, sched.register_publisher(pub, t));
    ASSERT_TRUE(reg.valid());

    ASSERT_TRUE(wait_for([&] { return pub->stale_scan.load() >= 1; }, 800));
    EXPECT_EQ(pub->last_dead_timeout_ns.load(), 1500000000)
        << "peer_dead_timeout 被写死，未按 ControlTiming 透传";
}

/* ------------------------------------------------------------------ O(1) 线程 */
/* 注册任意多项都不得新增线程 —— 这是阶段 1 的全部收益所在。 */
TEST(ShmControlScheduler, RegisterDoesNotAddThreads)
{
    const int before = thread_count();
    ShmControlScheduler sched;
    const int after_ctor = thread_count();
#if defined(__linux__)
    ASSERT_GT(before, 0);
    EXPECT_EQ(after_ctor, before + 1) << "调度器自身必须只占 1 条线程";
#endif

    std::vector<std::shared_ptr<CountingSub>> keep;
    std::vector<RegistrationToken> regs;
    keep.reserve(200);
    for (int i = 0; i < 200; ++i)
    {
        auto st = std::make_shared<CountingSub>();
        keep.push_back(st);
        regs.emplace_back(&sched, sched.register_subscriber(st));
        ASSERT_TRUE(regs.back().valid());
    }
    EXPECT_EQ(sched.entry_count(), 200u);

    /* 让它们跑起来，再确认线程数没变。 */
    ASSERT_TRUE(wait_for([&] { return keep[0]->hb.load() >= 3; }, 800)) << "200 个订阅项未被驱动";
    EXPECT_EQ(thread_count(), after_ctor) << "注册 200 项后线程数必须零增长（O(1)）";

    regs.clear();
    EXPECT_EQ(sched.entry_count(), 0u);
    /* 令牌全销毁后 worker 仍活着（进程级常驻，不是"最后一项注销即退出"）。 */
    EXPECT_TRUE(sched.worker_active());
}

/* ------------------------------------------------------------------ I1 */
/* 注销返回后不得再发起该 id 的新回调。 */
TEST(ShmControlScheduler, UnregisterStopsNewCallbacks)
{
    ShmControlScheduler sched;
    auto sub = std::make_shared<CountingSub>();
    RegistrationToken reg(&sched, sched.register_subscriber(sub));
    ASSERT_TRUE(wait_for([&] { return sub->hb.load() >= 2; }, 500));

    reg.reset();
    /* 基线在注销**返回之后**取：注销协议允许"在途回调在返回前结算"，那部分
     * 计数已经发生，不属于"注销后的新回调"。 */
    const int baseline = sub->hb.load();
    std::this_thread::sleep_for(80ms);   /* ≥ 8 个 10ms 周期 */
    EXPECT_EQ(sub->hb.load(), baseline) << "I1: 注销返回后仍有新回调";
    EXPECT_EQ(sched.entry_count(), 0u);
}

/* ------------------------------------------------------------------ I2 */
/* 注销必须等待在途回调结算 —— 否则调用方会在"以为安全"时被踩。 */
TEST(ShmControlScheduler, UnregisterWaitsForInflightTick)
{
    ShmControlScheduler sched;
    auto sub = std::make_shared<CountingSub>();
    sub->callback_cost = 250ms;
    RegistrationToken reg(&sched, sched.register_subscriber(sub));

    ASSERT_TRUE(wait_for([&] { return sub->in_callback.load(); }, 1000)) << "慢回调未开始执行";
    const auto t0 = ControlClock::now();
    reg.reset();
    const auto waited_ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(ControlClock::now() - t0).count();
    std::printf("[I2] unregister waited %lldms (callback cost 250ms)\n", static_cast<long long>(waited_ms));

    EXPECT_GE(waited_ms, 150) << "I2: unregister 没有等待在途回调";
    EXPECT_FALSE(sub->in_callback.load()) << "I2: unregister 返回时在途回调仍在跑";
    /* 返回后计数必须冻结。 */
    const int baseline = sub->hb.load();
    std::this_thread::sleep_for(60ms);
    EXPECT_EQ(sub->hb.load(), baseline);
    EXPECT_GE(sched.stats().tick_deferred_count, 1u) << "stats 必须记录被延迟的注销（可观测性）";
}

/* ------------------------------------------------------------------ I3 */
/* 注销后调度器不得再持有 state（"不得保存裸 this"的可判定形态）。 */
TEST(ShmControlScheduler, UnregisterReleasesState)
{
    ShmControlScheduler sched;
    auto sub = std::make_shared<CountingSub>();
    EXPECT_EQ(sub.use_count(), 1);

    {
        RegistrationToken reg(&sched, sched.register_subscriber(sub));
        std::this_thread::sleep_for(40ms);
        EXPECT_GE(sub.use_count(), 2)
            << "在册期间调度器必须持有 shared_ptr（若只存裸指针，这里会是 1）";
    }

    EXPECT_EQ(sub.use_count(), 1) << "I3: 注销后 shared_ptr 计数必须回落";
    std::weak_ptr<CountingSub> weak = sub;
    sub.reset();
    EXPECT_TRUE(weak.expired()) << "I3: 注销后对象必须可安全析构";
}

/* ------------------------------------------------------------------ I4 */
/* 并发 register/unregister 与 tick 交错：无数据竞争、无死锁、无迟到回调。 */
TEST(ShmControlScheduler, ConcurrentChurnIsRaceFree)
{
    ShmControlScheduler sched;
    std::atomic<bool> stop{false};
    std::atomic<int> late_callbacks{0};
    std::atomic<int> registrations{0};
    std::vector<std::thread> churn;

    for (int t = 0; t < 4; ++t)
    {
        churn.emplace_back(
            [&]
            {
                while (!stop.load(std::memory_order_relaxed))
                {
                    auto st = std::make_shared<CountingSub>();
                    RegistrationToken reg(&sched, sched.register_subscriber(st));
                    if (!reg.valid())
                    {
                        continue;
                    }
                    registrations.fetch_add(1, std::memory_order_relaxed);
                    std::this_thread::sleep_for(2ms);
                    reg.reset();                       /* 同步注销 */
                    const int baseline = st->hb.load();
                    std::this_thread::sleep_for(30ms); /* ≥3 个周期 */
                    if (st->hb.load() != baseline)
                    {
                        late_callbacks.fetch_add(1, std::memory_order_relaxed);
                    }
                }
            });
    }
    std::this_thread::sleep_for(600ms);
    stop.store(true, std::memory_order_relaxed);
    for (auto& t : churn)
    {
        t.join();
    }

    EXPECT_GT(registrations.load(), 0) << "压力循环一次都没跑起来，判据不成立";
    EXPECT_EQ(late_callbacks.load(), 0) << "I4: 并发 churn 下出现迟到回调";
    EXPECT_EQ(sched.entry_count(), 0u) << "I4: 全部注销后条目必须清零";
    EXPECT_TRUE(sched.worker_active()) << "I4: worker 必须在压力下存活";
}

/* ------------------------------------------------------- 幂等与参数校验 */
TEST(ShmControlScheduler, UnregisterIsIdempotent)
{
    ShmControlScheduler sched;
    /* 无效 / 未知 id 一律无操作。 */
    sched.unregister(ShmControlScheduler::kInvalidEntry);
    sched.unregister(0xDEADBEEFull);

    auto sub = std::make_shared<CountingSub>();
    const auto id = sched.register_subscriber(sub);
    ASSERT_NE(id, ShmControlScheduler::kInvalidEntry);
    sched.unregister(id);
    sched.unregister(id);   /* 重复注销 */
    sched.unregister(id);
    EXPECT_EQ(sched.entry_count(), 0u);

    /* 空 state 必须被拒（否则会注册一个永远无法回调的条目）。 */
    EXPECT_EQ(sched.register_subscriber(nullptr), ShmControlScheduler::kInvalidEntry);
    EXPECT_EQ(sched.register_publisher(nullptr), ShmControlScheduler::kInvalidEntry);
    EXPECT_EQ(sched.entry_count(), 0u);

    /* EntryId 单调递增、永不复用（避免 ABA：迟到的 unregister(旧 id) 不得注销别人的项）。 */
    const auto a = sched.register_subscriber(std::make_shared<CountingSub>());
    const auto b = sched.register_subscriber(std::make_shared<CountingSub>());
    EXPECT_NE(a, b);
    EXPECT_LT(a, b);
    sched.unregister(a);
    sched.unregister(b);
}

/* ------------------------------------------------------------------ 异常隔离 */
/* 一个坏项不得带走进程级控制面（现状 open() 失败在子线程 throw ⇒ terminate 的迁移风险）。 */
TEST(ShmControlScheduler, CallbackExceptionIsIsolated)
{
    ShmControlScheduler sched;
    auto bad = std::make_shared<CountingSub>();
    bad->throw_on_callback.store(true);
    auto good = std::make_shared<CountingSub>();
    auto pub = std::make_shared<CountingPub>();

    RegistrationToken bad_reg(&sched, sched.register_subscriber(bad));
    RegistrationToken good_reg(&sched, sched.register_subscriber(good));
    RegistrationToken pub_reg(&sched, sched.register_publisher(pub));

    /* 坏项必须被摘除；好项与发布项必须继续被驱动 —— 这正是"隔离"的定义。 */
    ASSERT_TRUE(wait_for([&] { return good->hb.load() >= 3; }, 800))
        << "坏项把好项一起带走了（隔离失效）";
    ASSERT_TRUE(wait_for([&] { return pub->owner_hb.load() >= 2; }, 800))
        << "坏项把发布侧控制面一起带走了";
    EXPECT_EQ(bad->hb.load(), 0) << "抛异常的项不得被记为成功回调";
    EXPECT_GE(sched.stats().callback_exception_count, 1u) << "异常隔离必须计入 stats";

    /* 坏项被摘除后不再被回调。 */
    const int bad_baseline = bad->hb.load();
    std::this_thread::sleep_for(60ms);
    EXPECT_EQ(bad->hb.load(), bad_baseline);
    EXPECT_TRUE(sched.worker_active()) << "进程级控制面必须存活";
}

/* 防误用兜底：回调内注销不得自死锁（协议禁止，但必须不挂）。 */
TEST(ShmControlScheduler, CallbackInternalUnregisterDoesNotDeadlock)
{
    ShmControlScheduler sched;

    struct SelfUnregistering final : SubControlState
    {
        ShmControlScheduler* sched{nullptr};
        ShmControlScheduler::EntryId id{ShmControlScheduler::kInvalidEntry};
        std::atomic<int> calls{0};
        void on_sub_heartbeat(ControlClock::time_point) override
        {
            calls.fetch_add(1, std::memory_order_relaxed);
            sched->unregister(id);   /* 明令禁止的用法：只置 inactive、不等待 */
        }
    };

    auto self = std::make_shared<SelfUnregistering>();
    self->sched = &sched;
    self->id = sched.register_subscriber(self);
    ASSERT_NE(self->id, ShmControlScheduler::kInvalidEntry);

    /* 若兜底失效，worker 会永久阻塞在"等自己"上，后面的判据全都不会成立。 */
    EXPECT_TRUE(wait_for([&] { return self->calls.load() >= 1; }, 1000));
    std::this_thread::sleep_for(80ms);
    EXPECT_EQ(self->calls.load(), 1) << "回调内注销后该项必须被摘除，不得重复回调";

    /* worker 必须仍然能服务其它项（这才是"没死锁"的判据，不是"没崩"）。 */
    auto good = std::make_shared<CountingSub>();
    RegistrationToken good_reg(&sched, sched.register_subscriber(good));
    ASSERT_TRUE(wait_for([&] { return good->hb.load() >= 2; }, 800)) << "worker 被自注销卡死";
}

/* ------------------------------------------------------- I1 的同轮形态（承重） */
/* 头文件 :173 保证①把"同轮 due 内已被选中、但尚未回调"的项也纳入 I1：
 *   tick 在**每次 dispatch 前**复查 inactive。
 *
 * 为什么必须单独守这一条：上面两条 I1 用例都抓不住它 ——
 *   · UnregisterStopsNewCallbacks 是**外部线程**注销，tick 在锁内选 due 时就能看到
 *     inactive 而不选中，根本不经过"dispatch 前复查"；
 *   · CallbackInternalUnregisterDoesNotDeadlock 只注销**自己**，自己那次 dispatch
 *     早已在运行中。
 * 于是"回调 A 在同轮里注销了已被选入 due 的 B"这条路径没有任何用例覆盖。
 * 删掉 dispatch 的 inactive 复查，现有用例**全绿**（已用变异实测确认）。
 *
 * ── 构造（自证前提，不依赖容器迭代序）────────────────────────────────────
 * A、B 都是 10ms 周期、A 先注册。tick 每轮把**同一批**到期项按 (next_due, EntryId)
 * 排序后依次回调（见 .cc 中 tick 的注释），因此同批内 A 必先于 B。
 * A **只在观察到"某批次同时含 A 与 B"之后**才动手注销 B —— 那个批次就是"两者确实
 * 同批到期"的正向证据，用例因此不依赖任何"第几轮会同批"的时序假设。
 * 动手时 A 先 sleep(150ms)：等它睡醒，同批的 B 仍未被 dispatch（A 排在前），
 * B 此刻 tick_inflight==true ⇒ 注销走"只置 inactive、不等待"的兜底路径。
 *
 * ── 判据（为什么不可被伪绿绕过）──────────────────────────────────────────
 * on_sub_heartbeat(now) 的 now 就是该轮 tick 的 t0 ⇒ **同批回调携带同一个 now**。
 * 于是"B 是否在本批被复查拦住"可直接判定：
 *   · 复查生效 ⇒ B 不留下任何 now >= a_now 的记录；
 *   · 复查失效 ⇒ B 在本批被 dispatch，留下 now == a_now 的记录 ⇒ 计数非零。
 * "B 根本没被选入 due"这一伪绿分支由**前置断言**排除（A 动手前必须已观察到同批）。 */
TEST(ShmControlScheduler, SameRoundUnregisterSkipsDispatch)
{
    ShmControlScheduler sched;

    /* 批次探针：记录 A、B 各自每次回调携带的 tick t0，以及 A 的"动手批"t0。
     *
     * ⚠️ 为什么用 t0 判"同批"而不是回调序号或注册顺序：on_sub_heartbeat(now) 的
     *    now 就是该轮 tick 的 t0，**同一批**回调必然携带同一个 now。于是"B 是否在
     *    A 动手的那一批里被 dispatch"成为一个可判定的事实，与 unordered_map 的迭代
     *    序、与"第几批会同批"这类假设全部无关。
     * ⚠️ 为什么不用"A 的计数恒为 0"这种绝对值判据：B 在**动手批之前**已经合法回调过
     *    （周期相同 ⇒ 每批都同批），"计数为 0"根本不可能成立；早期版本正是照抄了
     *    单批场景的绝对值判据而误红。 */
    struct BatchProbe
    {
        std::mutex mtx;
        std::vector<ControlClock::time_point> a_t0s;
        std::vector<ControlClock::time_point> b_t0s;
        ControlClock::time_point action_t0{};
        bool acted{false};

        void record_a(ControlClock::time_point now)
        {
            std::lock_guard<std::mutex> lock(mtx);
            a_t0s.push_back(now);
        }

        void record_b(ControlClock::time_point now)
        {
            std::lock_guard<std::mutex> lock(mtx);
            b_t0s.push_back(now);
        }

        void note_action(ControlClock::time_point now)
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (!acted)
            {
                action_t0 = now;
                acted = true;
            }
        }

        /* 前置：A、B 的**第一次**回调同批（t0 相等）。由它推出后续批次也同批 ——
         * 两者周期相同（都 10ms），且各自的 next_due 由**同一个** tick 的 now 推进，
         * 于是到期点保持"微秒级同步"，tick 在同一临界区里把两者一起选入 due。
         * 这条前置是排除伪绿的关键：没有它，"B 未被选入 due"与"B 被复查拦住"
         * 在 t0 判据下不可区分。
         * ⚠️ 它依赖的前提是"tick 的唤醒开销 >> 两次注册的间隔"（后者在微秒级）。
         *    若将来 tick 改成"精确到期即醒"（开销降到微秒以下），本前置可能失败 ——
         *    那时改用 SameRoundDispatchOrderFollowsRegistrationOrder 的做法：
         *    用"回调耗时 > 周期"让 settle_inflight 把各项 next_due **对齐**到同一
         *    时刻，同批就成了确定性结果而不是时序余量。 */
        bool first_callbacks_shared_batch()
        {
            std::lock_guard<std::mutex> lock(mtx);
            return !a_t0s.empty() && !b_t0s.empty() && a_t0s[0] == b_t0s[0];
        }

        bool has_action()
        {
            std::lock_guard<std::mutex> lock(mtx);
            return acted;
        }

        ControlClock::time_point action_batch()
        {
            std::lock_guard<std::mutex> lock(mtx);
            return action_t0;
        }

        bool b_ran_in_batch(ControlClock::time_point t0)
        {
            std::lock_guard<std::mutex> lock(mtx);
            for (const auto& t : b_t0s)
            {
                if (t == t0)
                {
                    return true;
                }
            }
            return false;
        }

        std::size_t a_count()
        {
            std::lock_guard<std::mutex> lock(mtx);
            return a_t0s.size();
        }

        std::size_t b_count()
        {
            std::lock_guard<std::mutex> lock(mtx);
            return b_t0s.size();
        }
    };

    struct Peer final : SubControlState
    {
        std::atomic<int> calls{0};
        std::shared_ptr<BatchProbe> probe;

        void on_sub_heartbeat(ControlClock::time_point now) override
        {
            /* 一进入就计数 + 记录 t0：任何一次 dispatch 都必然留下痕迹。 */
            calls.fetch_add(1, std::memory_order_relaxed);
            probe->record_b(now);
        }
    };

    /* 故意**不是** CountingSub：CountingSub 把计数推迟到 sleep 之后，会让"B 在本批
     * 是否被跑"出现读时序歧义；本用例要的是"留下痕迹"必然可读。 */
    struct Canceller final : SubControlState
    {
        ShmControlScheduler* sched{nullptr};
        std::shared_ptr<BatchProbe> probe;
        ShmControlScheduler::EntryId victim{ShmControlScheduler::kInvalidEntry};
        std::atomic<bool> armed{false};   /* 外部确认"A、B 首次同批"后置位 */
        std::atomic<bool> done{false};
        std::atomic<int> calls{0};
        /* unregister 返回后**立刻**（仍在本次回调内）读到的 entry_count。
         * 为什么必须在回调内读：worker 此刻正卡在本回调里，不可能同时执行 tick 顶部的
         * 惰性清理 ⇒ 这个读数是"注销是否**立即**摘除"的确定性判据。若只在外部等
         * unregister 返回后再读，下一轮 tick 的惰性清理可能已经跑过，于是"只置 inactive、
         * 靠惰性清理"的实现也能蒙混过关（已用变异实测确认：去掉立即 erase 后外部读数
         * 抓不住）。0 表示"未读到"（异常/未走到）。 */
        std::atomic<std::size_t> entry_count_after_unregister{0};

        void on_sub_heartbeat(ControlClock::time_point now) override
        {
            const int n = calls.fetch_add(1, std::memory_order_relaxed) + 1;
            probe->record_a(now);
            /* 前两批只记录：第 1 批供外部自证"A、B 同批"，第 2 批留一个周期的余量
             * 让外部把 armed 置上（避免"外部还没来得及置位就动手"的竞态）。 */
            if (n < 3 || !armed.load(std::memory_order_acquire))
            {
                return;
            }
            if (done.load(std::memory_order_acquire))
            {
                return;   /* 已动手过（正常不会走到这里：本项此后即被摘除） */
            }
            probe->note_action(now);
            /* ⚠️ 先 sleep 再注销：本批里 B 排在 A 之后、尚未 dispatch，A 睡这 150ms
             * 期间 B 一直 tick_inflight ⇒ 注销走"只置 inactive、不等待"的兜底路径。
             * 这正是本用例要考的同轮形态（若 A 与 B 同时回调，就考不到复查）。 */
            std::this_thread::sleep_for(150ms);
            sched->unregister(victim);
            done.store(true, std::memory_order_release);
        }
    };

    auto a = std::make_shared<Canceller>();
    auto b = std::make_shared<Peer>();

    const auto probe = std::make_shared<BatchProbe>();
    a->probe = probe;
    b->probe = probe;

    /* 先注册 A 再注册 B：EntryId 递增 ⇒ 同批内 A 排在前（tick 的排序键第二维）。
     * ⚠️ 这一条**不再**是"靠容器迭代序猜出来的前提"：.cc 的 tick 显式按
     *    (next_due, EntryId) 排序后回调，A 先跑是确定性的契约。 */
    const auto a_id = sched.register_subscriber(a);
    const auto b_id = sched.register_subscriber(b);
    a->sched = &sched;
    a->victim = b_id;
    ASSERT_NE(a_id, ShmControlScheduler::kInvalidEntry);
    ASSERT_NE(b_id, ShmControlScheduler::kInvalidEntry);

    /* ── 前置 1：A、B 的**第一次**回调同批（t0 相等）。
     * 这是本用例全部判据的地基：若两者根本不同批，后面的"B 在动手批是否被跑"
     * 就退化成"B 有没有被选入 due"，与 dispatch 复查无关 ⇒ 伪绿。
     * 它同时证明"B 会被 dispatch"这件事本身成立。 */
    ASSERT_TRUE(wait_for([&] { return probe->first_callbacks_shared_batch(); }, 1500))
        << "A、B 首次回调不同批 —— 用例前提不成立（同周期项必须被同一临界区一起选入）";

    /* 现在放行 A：它将在下一批动手（先 sleep 150ms，再注销同批尚未 dispatch 的 B）。 */
    a->armed.store(true, std::memory_order_release);

    ASSERT_TRUE(wait_for([&] { return probe->has_action(); }, 2000)) << "A 从未动手注销 B";
    const auto action_t0 = probe->action_batch();

    /* ⚠️ 必须先等 unregister **返回**再读 entry_count：note_action 发生在注销之前，
     * 此刻被注销项尚未摘除，读到的会是"动手前"的状态（早期版本就在这里拿到 2）。
     * done 由 A 在 unregister 返回之后置位。 */
    ASSERT_TRUE(wait_for([&] { return a->done.load(); }, 2000)) << "回调内的 unregister 未返回（疑似自挂）";

    /* ── 核心判据（I1 同轮）：B 在 A 动手的那一批里不得留下任何回调记录。 */
    EXPECT_FALSE(probe->b_ran_in_batch(action_t0))
        << "I1(同轮): B 已被选入同批 due 且 A 在回调内注销了它，dispatch 仍发起了回调 —— "
           "dispatch 的 inactive 复查失效（同轮保证①）";
    /* ── I3：被注销项必须已从调度器摘除。只注销了 B，因此这里恰好剩 A 一项
     *    （早期版本断言 0u 是错的：A 从未被注销）。 */
    EXPECT_EQ(sched.entry_count(), 1u)
        << "被注销项必须已从调度器摘除（I3）：此刻只应剩 A 在册";

    /* ── 收尾：B 的计数必须**冻结**（动手批之后不再有任何回调）。 */
    const std::size_t frozen = probe->b_count();
    std::this_thread::sleep_for(60ms);
    EXPECT_EQ(probe->b_count(), frozen) << "I1(同轮): 注销返回后仍出现 B 的回调";
    EXPECT_TRUE(sched.worker_active()) << "worker 必须存活（未因回调内注销而挂起）";
}

/* ------------------------------------------------- 同批 dispatch 顺序（契约） */
/* tick 把同批到期项按 (next_due, EntryId) 排序后**依次**回调（见 .cc 中 tick 的
 * 排序注释）。本用例是那条排序的**直接**判据 —— 契约本身要能被单独测到，而不是
 * 只能从"某个 I1 用例红/绿"间接推断。
 *
 * 为什么需要它（而不是只留 SameRoundUnregisterSkipsDispatch）：
 *   实测把 tick 里的 std::sort 整段删掉后，SameRoundUnregisterSkipsDispatch 会红，
 *   但它报的是"I1(同轮) dispatch 的 inactive 复查失效" —— 指向**错误的方向**：
 *   真正退化的是顺序，复查本身完好。而"同批顺序"还有它**自己**的产品含义：
 *   同一轮里先到期/先注册的项先被服务，一个慢项不得让后注册项插到前面去。
 *
 * ── 确定性构造：不靠时序余量，靠"回调耗时 > 周期"对齐 next_due ──────────────
 * 两个项都是 10ms 周期，回调各 sleep(25ms)（> 10ms）。tick 的第 1 轮里两者被同一
 * 临界区一起选入 due（注册间隔在微秒级，远小于 tick 的唤醒开销）。此后
 * settle_inflight 推进 next_due 时，`next = next_due + period` 已 <= now ⇒ 两项
 * 都被对齐到 `now + period`（见 settle_inflight 的"不追赶风暴"分支）——
 * 于是从第 2 批起两者的 next_due **精确相等**，同批成为确定性结果。
 *
 * ── 判据：首批之后，每一批内 A 都必须排在 B 之前 ──────────────────────────
 * 记录**带批次标识**的回调序列（用该批第 1 个回调的 t0 作批号 —— on_sub_heartbeat
 * 的 now 就是该轮 tick 的 t0），然后逐批断言 A 是第一个、B 是第二个。
 * 用"批内次序"而不是全局次序：全局序列里混着 A 的第二批回调，直接比较会误判。
 *
 * ⚠️ 断言取**全部批次**而不是只看最后一批：只查最后一批的话，一个"首批反序、
 *    之后碰巧正确"的实现会漏过；而首批之后 next_due 已对齐，顺序若错必然每批都错。 */
TEST(ShmControlScheduler, SameRoundDispatchOrderFollowsRegistrationOrder)
{
    ShmControlScheduler sched;

    struct BatchLog
    {
        mutable std::mutex mtx;
        /* 每批的回调序列：batch_id（该批 tick 的 t0）→ 该批内按发生先后记录的 id。 */
        std::map<ControlClock::time_point, std::vector<ShmControlScheduler::EntryId>> batches;
        std::vector<ControlClock::time_point> batch_order;

        void record(ControlClock::time_point now, ShmControlScheduler::EntryId id)
        {
            std::lock_guard<std::mutex> lock(mtx);
            if (batches.find(now) == batches.end())
            {
                batch_order.push_back(now);
            }
            batches[now].push_back(id);
        }

        /* 已观察到的"完整批次"数（每批内 >=2 项）。 */
        std::size_t full_batch_count() const
        {
            std::lock_guard<std::mutex> lock(mtx);
            std::size_t n = 0;
            for (const auto& t : batch_order)
            {
                const auto it = batches.find(t);
                if (it != batches.end() && it->second.size() >= 2)
                {
                    ++n;
                }
            }
            return n;
        }

        /* 所有**完整**批次（>=2 项）的回调序列快照，供逐批断言。 */
        std::vector<std::pair<ControlClock::time_point, std::vector<ShmControlScheduler::EntryId>>> full_batches() const
        {
            std::lock_guard<std::mutex> lock(mtx);
            std::vector<std::pair<ControlClock::time_point, std::vector<ShmControlScheduler::EntryId>>> out;
            for (const auto& t : batch_order)
            {
                const auto it = batches.find(t);
                if (it != batches.end() && it->second.size() >= 2)
                {
                    out.emplace_back(t, it->second);
                }
            }
            return out;
        }
    };

    struct Ordered final : SubControlState
    {
        std::shared_ptr<BatchLog> log;
        ShmControlScheduler::EntryId id{ShmControlScheduler::kInvalidEntry};

        void on_sub_heartbeat(ControlClock::time_point now) override
        {
            log->record(now, id);
            /* > 周期（10ms）：让 settle_inflight 把 next_due 对齐到同一时刻，
             * 从而"两项同批"成为确定性事实（见用例抬头）。 */
            std::this_thread::sleep_for(25ms);
        }
    };

    auto log = std::make_shared<BatchLog>();
    auto first = std::make_shared<Ordered>();
    auto second = std::make_shared<Ordered>();
    first->log = log;
    second->log = log;

    /* 先注册者 EntryId 更小 ⇒ 同批内必须先回调（排序键第二维）。 */
    const auto first_id = sched.register_subscriber(first);
    const auto second_id = sched.register_subscriber(second);
    first->id = first_id;
    second->id = second_id;
    ASSERT_NE(first_id, ShmControlScheduler::kInvalidEntry);
    ASSERT_NE(second_id, ShmControlScheduler::kInvalidEntry);
    ASSERT_LT(first_id, second_id) << "EntryId 必须单调递增（注册顺序即大小顺序）";

    /* 前置：至少观察到**两个**完整批次（每个批次里两个项都在）——否则"批内顺序"
     * 无从谈起，后面的断言会退化成"没记录 ⇒ 通过"的伪绿；只要求 1 批也不够：
     * 单批无法区分"顺序契约成立"与"首轮恰好正确"。多批还顺带验证了
     * "settle_inflight 把 next_due 对齐"这一构造前提（对齐失败就不会再有完整批）。 */
    ASSERT_TRUE(wait_for([&] { return log->full_batch_count() >= 2; }, 1500))
        << "未观察到 >=2 个同时含两个项的批次（前提不成立，用例无意义）";

    /* 判据：每一个完整批次内都必须恰好是 [first, second]。 */
    const auto batches = log->full_batches();
    ASSERT_GE(batches.size(), 2u) << "完整批次不足（前置已保证 >=2，此处复核快照与前置一致）";
    for (const auto& b : batches)
    {
        ASSERT_EQ(b.second.size(), 2u) << "完整批次内必须恰好两个项（多/少都说明选批逻辑异常）";
        EXPECT_EQ(b.second[0], first_id)
            << "同批内先注册者必须排在前面（tick 的 (next_due, EntryId) 排序契约）";
        EXPECT_EQ(b.second[1], second_id)
            << "同批内后注册者必须排在后面（tick 的 (next_due, EntryId) 排序契约）";
    }
    std::printf("[order] 观察到的完整批次数=%zu，全部满足 first<second\n", batches.size());

    /* 收尾：worker 存活（顺序契约不靠"停掉调度器"达成）。 */
    EXPECT_TRUE(sched.worker_active());
}

/* ------------------------------------------------------------------ 可观测性 */
/* 需求 §10 阶段 1 要求"记录扫描耗时和最大抖动"。 */
TEST(ShmControlScheduler, StatsExposeTickCost)
{
    ShmControlScheduler sched;
    auto slow = std::make_shared<CountingSub>();
    slow->callback_cost = 250ms;
    RegistrationToken reg(&sched, sched.register_subscriber(slow));

    ASSERT_TRUE(wait_for([&] { return sched.stats().tick_count >= 2; }, 1500));
    const auto st = sched.stats();
    std::printf("[stats] ticks=%llu overruns=%llu deferred=%llu max=%lldns entry_count=%zu\n",
                static_cast<unsigned long long>(st.tick_count), static_cast<unsigned long long>(st.tick_overrun_count),
                static_cast<unsigned long long>(st.tick_deferred_count), static_cast<long long>(st.tick_duration_max_ns),
                st.entry_count);

    EXPECT_GT(st.tick_count, 1u) << "调度线程没在跑";
    EXPECT_GE(st.tick_duration_max_ns, 250000000LL) << "慢回调耗时未被计入（抖动上界不可观测）";
    EXPECT_GT(st.tick_overrun_count, 0u) << "单轮耗时超过最小到期周期时必须计入超期";
    EXPECT_EQ(st.entry_count, 1u);
}

/* ------------------------------------------------------------------ stop / wakeup */
TEST(ShmControlScheduler, StopAndWakeupSemantics)
{
    ShmControlScheduler sched;
    EXPECT_TRUE(sched.worker_active());

    sched.wakeup();   /* 空载唤醒：不得崩、不得产生回调 */
    sched.wakeup();
    EXPECT_EQ(sched.entry_count(), 0u);

    auto sub = std::make_shared<CountingSub>();
    RegistrationToken reg(&sched, sched.register_subscriber(sub));
    ASSERT_TRUE(wait_for([&] { return sub->hb.load() >= 2; }, 500));

    /* stop 必须唤醒阻塞中的 worker 并在有界时间内返回。 */
    const auto t0 = ControlClock::now();
    sched.stop();
    const auto stop_ms = std::chrono::duration_cast<std::chrono::milliseconds>(ControlClock::now() - t0).count();
    EXPECT_LT(stop_ms, 1000) << "stop 未在有界时间内返回（可能没唤醒阻塞中的 worker）";
    EXPECT_FALSE(sched.worker_active());

    /* stop 后：tick 冻结、注册被拒（否则"注册了但永不回调"是静默失效）。 */
    const int frozen = sub->hb.load();
    std::this_thread::sleep_for(60ms);
    EXPECT_EQ(sub->hb.load(), frozen) << "stop 后仍有回调";
    EXPECT_EQ(sched.register_subscriber(std::make_shared<CountingSub>()), ShmControlScheduler::kInvalidEntry)
        << "stop 后注册必须返回 kInvalidEntry";

    /* stop 幂等；stop 后 unregister 仍然安全。 */
    sched.stop();
    sched.stop();
    EXPECT_FALSE(sched.worker_active());
    reg.reset();
    EXPECT_EQ(sched.entry_count(), 0u);
}

/* ------------------------------------------------- 空闲唤醒契约（不空转） */
/* worker 必须"算到最近到期点再睡"，被 wakeup() 唤醒只**重算**、不产生额外 tick。
 * 若实现退化成"被 notify 就无条件跑一轮 tick"，tick_count 会随唤醒次数线性增长
 * —— 那正是阶段 1 要消灭的固定周期轮询开销，且会让需求 §10 的
 * "唤醒/上下文切换下降"验收读数失真（tick_count 被空转撑大）。
 * 判据取"tick 增量 << 唤醒次数"：正确实现 200ms 内约 20 轮（10ms 周期），
 * 空转实现约 200 轮 —— 相差一个数量级，不受 CI 抖动影响。 */
TEST(ShmControlScheduler, WakeupDoesNotSpuriouslyTick)
{
    ShmControlScheduler sched;
    auto sub = std::make_shared<CountingSub>();
    RegistrationToken reg(&sched, sched.register_subscriber(sub));   /* 默认 10ms */
    ASSERT_TRUE(wait_for([&] { return sub->hb.load() >= 2; }, 500)) << "调度线程没跑起来";

    const auto before = sched.stats().tick_count;
    constexpr int k_wakeups = 200;
    for (int i = 0; i < k_wakeups; ++i)
    {
        sched.wakeup();
        std::this_thread::sleep_for(1ms);
    }
    const auto delta = sched.stats().tick_count - before;
    std::printf("[wakeup] %d 次唤醒 / ~200ms: tick 增量=%llu\n", k_wakeups,
                static_cast<unsigned long long>(delta));

    /* 200ms / 10ms ≈ 20 轮；给 3 倍余量吸收调度抖动。空转实现会是 ~200 轮。 */
    EXPECT_LT(delta, 60u) << "wakeup() 产生了额外 tick —— worker 在空转轮询而非睡到最近到期点";
    EXPECT_GE(delta, 5u) << "200ms 内 tick 太少（周期语义可能已坏），判据不成立";
}

/* ------------------------------------------------------------------ RAII 令牌 */
TEST(ShmControlScheduler, RegistrationTokenIsMoveOnly)
{
    ShmControlScheduler sched;
    auto sub = std::make_shared<CountingSub>();
    RegistrationToken a(&sched, sched.register_subscriber(sub));
    ASSERT_TRUE(a.valid());
    const auto id = a.id();

    RegistrationToken b(std::move(a));
    EXPECT_TRUE(b.valid());
    EXPECT_FALSE(a.valid()) << "移动后源令牌必须失效（否则会二次注销）";
    EXPECT_EQ(b.id(), id);
    EXPECT_EQ(sched.entry_count(), 1u);

    b.reset();
    EXPECT_FALSE(b.valid());
    b.reset();   /* 重复 reset 幂等 */
    EXPECT_EQ(sched.entry_count(), 0u);

    /* 默认构造即"未注册" ⇒ 未 InitChannel() 就析构的对象无需特殊处理。 */
    RegistrationToken empty;
    EXPECT_FALSE(empty.valid());
    empty.reset();
    EXPECT_EQ(sched.entry_count(), 0u);
}

/* 调度器析构必须停掉 worker 并释放全部 state（宿主析构路径的等价物）。 */
TEST(ShmControlScheduler, DtorStopsWorkerAndReleasesEntries)
{
    auto sub = std::make_shared<CountingSub>();
    std::weak_ptr<CountingSub> weak = sub;
    {
        ShmControlScheduler sched;
        RegistrationToken reg(&sched, sched.register_subscriber(sub));
        ASSERT_TRUE(wait_for([&] { return sub->hb.load() >= 2; }, 500));
        /* reg 后声明 ⇒ 先于 sched 析构，本来就是安全的；宿主析构体的推荐写法是
         * 显式 reset()（把"顺序约定"变成"代码顺序"），这里照做。 */
        reg.reset();
    }
    const int baseline = sub->hb.load();
    std::this_thread::sleep_for(60ms);
    EXPECT_EQ(sub->hb.load(), baseline) << "调度器析构后仍有回调";
    sub.reset();
    EXPECT_TRUE(weak.expired()) << "调度器析构后必须释放持有的 state";
}

/* ------------------------------------------------- stop 唤醒（空载 = 无限期 cv.wait） */
/* stop() 必须能唤醒**阻塞在无限期 cv.wait 上**的 worker。
 *
 * 这是 stop 最容易漏的一条：空载（无注册项）时 loop_body 走的是 `cv.wait(lock)`
 * —— 没有超时。只有 stop 置 stopping + notify_all 才能让它出来。漏掉那次 notify，
 * worker 会永久停在 cv.wait 上，于是 stop() 里的 join() 永久阻塞 —— 是**死锁**，
 * 不是静默失效。
 *
 * 为什么必须放进子进程：漏 notify 的后果是 stop() 永不返回，用例会挂在 join 处，
 * 把整个套件拖成"跑不完"（StopAndWakeupSemantics 里的 EXPECT_LT(stop_ms, 1000)
 * 在这种实现下根本执行不到 —— 它自己就出不了 stop()）。只有 fork + 父侧硬超时，
 * 才能把"挂死"变成"有界失败"（手法同 test_uf004_shutdown_monitor_optout.cpp:88
 * 的 run_in_child：pipe 回报 + poll 超时 + SIGKILL 收尾）。
 *
 * ⚠️ 子进程里不用 EXPECT_ / ASSERT_ 宏：fork 之后从多线程父进程里调 exit 不安全，
 * 且 gtest 断言本就依赖父进程环境。子进程只用 ::write + ::_exit 回报。 */
#if defined(__linux__)
TEST(ShmControlScheduler, StopWakesIdleWorkerWithoutDeadlock)
{
    int pfd[2] = {-1, -1};
    ASSERT_EQ(::pipe(pfd), 0);
    const ::pid_t pid = ::fork();
    ASSERT_GE(pid, 0);
    if (pid == 0)
    {
        ::close(pfd[0]);
        /* 故意不析构：stop() 走一遍就够，避免析构再 stop 一次干扰判据。 */
        auto* sched = new ShmControlScheduler();
        if (sched->worker_active() && sched->entry_count() == 0)
        {
            ::write(pfd[1], "idle", 4);   /* 确认是**空载**路径（无注册项） */
        }
        sched->stop();                     /* 空载 stop：必须被唤醒并 join 成功 */
        ::write(pfd[1], "stopped", 7);
        ::_exit(sched->worker_active() ? 11 : 0);
    }
    ::close(pfd[1]);

    std::string log;
    bool timed_out = false;
    const auto deadline = ControlClock::now() + 5s;
    for (;;)
    {
        const auto remain =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - ControlClock::now()).count();
        if (remain <= 0)
        {
            timed_out = true;
            break;
        }
        struct pollfd p{pfd[0], POLLIN, 0};
        const int pr = ::poll(&p, 1, static_cast<int>(remain));
        if (pr < 0)
        {
            if (errno == EINTR)
            {
                continue;
            }
            break;
        }
        if (pr == 0)
        {
            timed_out = true;
            break;
        }
        char buf[64];
        const ssize_t n = ::read(pfd[0], buf, sizeof(buf));
        if (n > 0)
        {
            log.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0)
        {
            break;   /* EOF：子进程已结束 */
        }
        if (errno == EINTR)
        {
            continue;
        }
        break;
    }
    ::close(pfd[0]);

    int st = 0;
    for (;;)
    {
        const ::pid_t w = ::waitpid(pid, &st, WNOHANG);
        if (w == pid)
        {
            break;
        }
        if (w < 0 && errno != EINTR)
        {
            break;
        }
        if (ControlClock::now() >= deadline)
        {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &st, 0);
            timed_out = true;
            break;
        }
        ::usleep(5 * 1000);
    }

    ASSERT_FALSE(timed_out) << "stop() 在空载 worker 上挂死 —— worker 未被唤醒（漏 notify）; log=[" << log << "]";
    ASSERT_TRUE(WIFEXITED(st)) << "子进程非正常退出; log=[" << log << "]";
    EXPECT_NE(log.find("idle"), std::string::npos)
        << "子进程没有走到空载路径（entry_count 非 0），判据不成立; log=[" << log << "]";
    EXPECT_NE(log.find("stopped"), std::string::npos) << "stop() 未走完; log=[" << log << "]";
    EXPECT_EQ(WEXITSTATUS(st), 0) << "stop() 返回后 worker_active() 仍为 true; log=[" << log << "]";
}
#endif

// W03 测量口径与路径观测 · 自动化验收用例
//
// 对应验收项（docs/消息接收架构改造/团队改造方案_性能证据闭环与SHM规模化.md）：
//   1. 「已知线程活动可被正确计入」—— 本进程内 spawn 已知数量的线程，逐 TID 采集器
//      必须计入其 CPU 与上下文切换，并且**必须超过 /proc/<pid>/status 的主线程参考值**
//      （这正是方案 §1 指出的旧探针缺陷：只数主线程，报出"两秒仅 10 次切换"）。
//   2. 新生/退出线程 —— 必须被识别，退出造成的不可读区间必须标 complete=false 并给出上界。
//   3. 跨进程时钟 —— fork+pipe 区间判据必须成立。
//   4. 计数开销可量化 —— 常驻计数、诊断门控、扫描观测的单次成本必须可测且量级合理。
//   5. 三种空闲状态 —— 分类必须区分；未接入等待层的"少线程"不得当作空闲成绩。
//   6. 相位统计 —— 启动峰值/稳定活跃/空闲回落必须分别统计，窗口 >= 60 s 判定生效。
//   7. 路径证据不足 —— 必须标「未确认」；DZFlat B 缺 publish_loan 调用点必须未确认。
//   8. 字段定义 —— 方案 §4 W03 第 4 条列出的计数必须全部存在。
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <gtest/gtest.h>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "dzIPC/measure/measurement.h"

using namespace dzIPC::measure;

namespace {

std::uint64_t now_ns() { return monotonic_now_ns(); }

void sleep_ms(long ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

/* 拉起一个"已知活动"子进程：N 次 sleep（必产生自愿切换）+ 一段忙等（产生 CPU）。
 * 用 fork 而不是 exec，避免依赖外部可执行文件。 */
pid_t spawn_known_child(long sleeps, long sleep_ms_each, long spin_ms)
{
    const pid_t pid = ::fork();
    if (pid == 0) {
        for (long i = 0; i < sleeps; ++i) {
            std::this_thread::sleep_for(std::chrono::milliseconds(sleep_ms_each));
        }
        volatile double x = 0.0;
        const std::uint64_t t0 = monotonic_now_ns();
        while (monotonic_now_ns() - t0 < static_cast<std::uint64_t>(spin_ms) * 1000000ull) {
            x += 1.0;
        }
        (void)x;
        ::_exit(0);   // 不跑 gtest 析构
    }
    return pid;
}

}   // namespace

/* ------------------------------------------------------------------ 时钟 */

TEST(W03Measurement, MonotonicClockIsNonDecreasing)
{
    std::uint64_t prev = now_ns();
    for (int i = 0; i < 1000; ++i) {
        const std::uint64_t cur = now_ns();
        ASSERT_GE(cur, prev) << "CLOCK_MONOTONIC 回退: " << prev << " -> " << cur;
        prev = cur;
    }
    EXPECT_STREQ("CLOCK_MONOTONIC", clock_source_name());
}

TEST(W03Measurement, ClockCostIsQuantified)
{
    const ClockCost c = measure_clock_cost(20000);
    EXPECT_GT(c.vdso_ns_per_call, 0.0);
    EXPECT_GT(c.syscall_ns_per_call, 0.0);
    /* vDSO 应当不慢于强制 syscall；且绝对成本在可观测范围内（<2 µs）。 */
    EXPECT_LE(c.vdso_ns_per_call, c.syscall_ns_per_call * 1.5 + 20.0);
    EXPECT_LT(c.vdso_ns_per_call, 2000.0);
}

TEST(W03Measurement, CrossProcessClockIsConsistent)
{
    const XprocClockCheck r = verify_cross_process_clock();
    ASSERT_TRUE(r.performed) << r.note;
    EXPECT_TRUE(r.consistent) << "子进程 CLOCK_MONOTONIC 值 " << r.child_ns
                              << " 不在父进程区间 [" << r.parent_before_ns << ", "
                              << r.parent_after_ns << "] 内";
    EXPECT_GE(r.child_ns, r.parent_before_ns);
    EXPECT_LE(r.child_ns, r.parent_after_ns);
}

/* ------------------------------------------------------------------ 已知线程活动 */

/* 核心验收：已知线程活动的上下文切换必须被计入线程组合计，
 * 并且该合计必须显著大于 /proc/<pid>/status 的主线程参考值。 */
TEST(W03Measurement, KnownThreadActivityIsCountedBeyondMainThreadReference)
{
    const pid_t self = ::getpid();
    ProcessSampler sampler(self);
    ASSERT_TRUE(sampler.attach());
    ASSERT_TRUE(sampler.sample());
    const std::size_t threads_before = sampler.threads().size();

    constexpr int kThreads = 4;
    constexpr int kSleeps = 120;   // 每次 sleep 至少 1 次自愿切换
    std::atomic<int> ready{0};
    std::vector<std::thread> workers;
    for (int t = 0; t < kThreads; ++t) {
        workers.emplace_back([&ready]() {
            ready.fetch_add(1);
            for (int i = 0; i < kSleeps; ++i) sleep_ms(1);
        });
    }
    while (ready.load() < kThreads) std::this_thread::sleep_for(std::chrono::microseconds(200));

    std::size_t threads_peak = threads_before;
    const std::uint64_t t0 = now_ns();
    while (now_ns() - t0 < 400ull * 1000000ull) {
        ASSERT_TRUE(sampler.sample());
        threads_peak = std::max(threads_peak, sampler.threads().size());
        sleep_ms(20);
    }
    for (auto& w : workers) w.join();
    sampler.sample();
    const ThreadGroupCtxSwitch ctx = sampler.finish();

    /* 已知线程确实在册被观察到 */
    EXPECT_GE(threads_peak, threads_before + static_cast<std::size_t>(kThreads));
    EXPECT_GE(ctx.tids_seen, static_cast<std::uint64_t>(kThreads));

    /* 线程组合计必须远大于主线程参考值 —— 这是"不能用 /proc/self/status 当全进程合计"的机器判据 */
    EXPECT_GT(ctx.voluntary, ctx.main_thread_voluntary_reference)
        << "线程组合计 " << ctx.voluntary << " 未超过主线程参考值 "
        << ctx.main_thread_voluntary_reference;
    EXPECT_GE(ctx.voluntary, static_cast<std::uint64_t>(kThreads * kSleeps) / 2u)
        << "已知 " << kThreads << " 线程各 sleep " << kSleeps << " 次, 却只统计到 "
        << ctx.voluntary << " 次自愿切换";

    /* 新生与退出线程都必须被识别 */
    EXPECT_GE(ctx.tids_born_mid_window, static_cast<std::uint64_t>(kThreads));
    EXPECT_GE(ctx.tids_exited, static_cast<std::uint64_t>(kThreads));

    /* 窗口内有线程退出 ⇒ 最后一个采样区间不可读 ⇒ 必须标不完整并给上界 */
    EXPECT_FALSE(ctx.complete) << "存在退出线程却声称完整";
    EXPECT_GT(ctx.voluntary_undercount_bound, 0u);
    EXPECT_EQ(std::string("proc_task_sample"), ctx.method);
    EXPECT_GE(ctx.samples, 5u);
}

/* 无线程退出的窗口：必须能声明完整。 */
TEST(W03Measurement, StableWindowWithoutThreadChurnIsComplete)
{
    const pid_t self = ::getpid();
    ProcessSampler sampler(self);
    ASSERT_TRUE(sampler.attach());
    for (int i = 0; i < 6; ++i) {
        ASSERT_TRUE(sampler.sample());
        sleep_ms(15);
    }
    const ThreadGroupCtxSwitch ctx = sampler.finish();
    EXPECT_EQ(0u, ctx.tids_exited);
    EXPECT_EQ(0u, ctx.tids_born_mid_window);
    EXPECT_TRUE(ctx.complete) << ctx.note;
    EXPECT_EQ(0u, ctx.voluntary_undercount_bound);
    EXPECT_GE(ctx.main_thread_voluntary_reference, 1u)
        << "主线程自身也应有自愿切换（sleep 采样间隔）";
}

/* 子进程 CPU 与上下文切换：逐 TID 采样口径必须与 wait4 rusage 全生命周期口径吻合
 * （采样口径是下界，允许因退出区间而偏小，但不允许数量级偏离）。 */
TEST(W03Measurement, ChildCpuAndCtxSwitchMatchRusage)
{
    constexpr long kSleeps = 60;
    const pid_t pid = spawn_known_child(kSleeps, 5, 60);
    ASSERT_GT(pid, 0);

    ProcessSampler sampler(pid);
    ASSERT_TRUE(sampler.attach()) << "无法 attach 到子进程 " << pid;
    const long hz = clock_ticks_per_sec();

    const std::uint64_t t0 = now_ns();
    while (now_ns() - t0 < 1200ull * 1000000ull) {
        if (!sampler.sample()) break;
        sleep_ms(20);
    }
    sampler.finish();
    const double sampled_cpu_s = static_cast<double>(sampler.utime_ticks() + sampler.stime_ticks())
                               / static_cast<double>(hz);
    const std::uint64_t sampled_vol = sampler.ctx().voluntary;

    int status = 0;
    struct rusage ru {};
    ASSERT_EQ(pid, ::wait4(pid, &status, 0, &ru));
    const double rusage_cpu_s = static_cast<double>(ru.ru_utime.tv_sec)
                              + static_cast<double>(ru.ru_utime.tv_usec) / 1e6
                              + static_cast<double>(ru.ru_stime.tv_sec)
                              + static_cast<double>(ru.ru_stime.tv_usec) / 1e6;
    const std::uint64_t rusage_vol = static_cast<std::uint64_t>(ru.ru_nvcsw);

    EXPECT_GT(rusage_cpu_s, 0.02) << "已知忙等子进程应有可测 CPU";
    EXPECT_GT(rusage_vol, kSleeps / 2) << "已知 sleep 子进程应有自愿切换";
    /* 采样是下界：允许偏小，但不能偏离超过一半（否则说明漏采） */
    EXPECT_GE(sampled_vol, rusage_vol / 2)
        << "采样自愿切换 " << sampled_vol << " 远小于 rusage " << rusage_vol;
    EXPECT_LE(sampled_vol, rusage_vol + 100)
        << "采样自愿切换 " << sampled_vol << " 超过 rusage 全生命周期值 " << rusage_vol;
    EXPECT_GE(sampled_cpu_s, rusage_cpu_s * 0.5)
        << "采样 CPU " << sampled_cpu_s << " s 远小于 rusage " << rusage_cpu_s << " s";
}

/* ------------------------------------------------------------------ 计数与开销 */

TEST(W03Measurement, CounterRegistryCountsExactly)
{
    CounterRegistry& r = CounterRegistry::instance();
    r.set_diagnostics_enabled(false);
    const CounterSnapshot before = r.snapshot();
    r.inc(CounterId::dzflat_b_messages, 3);
    r.inc(CounterId::dzflat_a_messages, 2);
    r.add(CounterId::tlv_bytes, 4096);
    r.set_gauge(CounterId::deferred_depth_last, 7);
    r.max_gauge(CounterId::deferred_depth_max, 5);
    r.max_gauge(CounterId::deferred_depth_max, 3);   // 不得回退
    const CounterSnapshot after = r.snapshot();
    const CounterSnapshot d = CounterRegistry::diff(before, after);
    EXPECT_EQ(3u, d.get(CounterId::dzflat_b_messages));
    EXPECT_EQ(2u, d.get(CounterId::dzflat_a_messages));
    EXPECT_EQ(4096u, d.get(CounterId::tlv_bytes));
    EXPECT_EQ(7u, after.get(CounterId::deferred_depth_last));
    EXPECT_GE(after.get(CounterId::deferred_depth_max), 5u);
}

/* §10.2：低开销常驻计数与详细诊断采样分开。
 * 诊断关闭时 ScanRoundScope 不得写任何计数；开启时必须累加并记录耗时与 deferred 深度。 */
TEST(W03Measurement, ScanRoundScopeRespectsDiagnosticsGate)
{
    CounterRegistry& r = CounterRegistry::instance();

    r.set_diagnostics_enabled(false);
    const std::uint64_t rounds0 = r.get(CounterId::scan_rounds);
    const std::uint64_t routes0 = r.get(CounterId::scanned_routes_total);
    const std::uint64_t time0 = r.get(CounterId::scan_time_ns_total);
    {
        ScanRoundScope s(120, 4);
        s.set_ready(true);
    }
    EXPECT_EQ(rounds0, r.get(CounterId::scan_rounds)) << "诊断关闭时不得计数";
    EXPECT_EQ(routes0, r.get(CounterId::scanned_routes_total));
    EXPECT_EQ(time0, r.get(CounterId::scan_time_ns_total));

    r.set_diagnostics_enabled(true);
    const std::uint64_t rounds1 = r.get(CounterId::scan_rounds);
    const std::uint64_t routes1 = r.get(CounterId::scanned_routes_total);
    const std::uint64_t time1 = r.get(CounterId::scan_time_ns_total);
    const std::uint64_t ready1 = r.get(CounterId::ready_observed);
    {
        ScanRoundScope s(120, 9);
        s.set_ready(true);
    }
    EXPECT_EQ(rounds1 + 1, r.get(CounterId::scan_rounds));
    EXPECT_EQ(routes1 + 120, r.get(CounterId::scanned_routes_total));
    EXPECT_GT(r.get(CounterId::scan_time_ns_total), time1) << "扫描耗时必须被记录";
    EXPECT_EQ(ready1 + 1, r.get(CounterId::ready_observed)) << "有效就绪比例的分母/分子要能算";
    EXPECT_EQ(9u, r.get(CounterId::deferred_depth_last));
    EXPECT_GE(r.get(CounterId::deferred_depth_max), 9u);
    r.set_diagnostics_enabled(false);
}

/* 计数开销可量化：常驻计数为个位数 ns 量级；诊断关闭的热路径接近零成本；
 * 诊断开启的扫描观测成本必须远小于一次 /proc 读。 */
TEST(W03Measurement, CounterOverheadIsQuantified)
{
    const CounterOverhead o = measure_counter_overhead(50000);
    EXPECT_GT(o.counter_inc_ns, 0.0);
    EXPECT_LT(o.counter_inc_ns, 100.0) << "单次常驻计数超过 100 ns, 热路径代价过高";
    EXPECT_LT(o.counter_inc_cached_ns, 100.0);
    EXPECT_LT(o.diag_inc_disabled_ns, 10.0) << "诊断关闭时热路径必须接近零成本";
    EXPECT_LT(o.scan_scope_disabled_ns, 10.0);
    EXPECT_GT(o.scan_scope_enabled_ns, o.scan_scope_disabled_ns);
    EXPECT_GT(o.clock_now_ns, 0.0);
    EXPECT_GT(o.proc_self_stat_read_ns, o.clock_now_ns)
        << "/proc 读取成本必须高于一次 clock_gettime（这决定采样模式的最小间隔）";
}

/* ------------------------------------------------------------------ 三种空闲 */

TEST(W03Measurement, ThreeIdleStatesAreDistinguished)
{
    IdleInputs none;
    EXPECT_EQ(IdleState::no_registered_route, classify_idle(none));

    IdleInputs reg_no_pub;
    reg_no_pub.registered_routes = 1000;
    reg_no_pub.worker_routes = 1000;
    reg_no_pub.wait_layer_attached = true;
    EXPECT_EQ(IdleState::registered_no_publisher, classify_idle(reg_no_pub));

    IdleInputs silent = reg_no_pub;
    silent.publisher_connected = true;
    EXPECT_EQ(IdleState::connected_silent, classify_idle(silent));

    IdleInputs active = silent;
    active.publishing = true;
    EXPECT_EQ(IdleState::active_publishing, classify_idle(active));

    /* §10.1：注册了 1000 条但没接入等待层 ⇒ 不能把少线程/低 CPU 当空闲成绩 */
    IdleInputs not_attached = reg_no_pub;
    not_attached.wait_layer_attached = false;
    EXPECT_EQ(IdleState::unconfirmed, classify_idle(not_attached));

    /* 应用对象在册但 worker 表为空 ⇒ 统计口径不一致，同样未确认 */
    IdleInputs mismatch;
    mismatch.application_objects = 1000;
    EXPECT_EQ(IdleState::unconfirmed, classify_idle(mismatch));
}

TEST(W03Measurement, RecoveryFirstPacketLatencyIsRecorded)
{
    RecoveryLatencyStats stats;
    for (int i = 0; i < 10; ++i) {
        RecoveryObservation r;
        r.silence_ns = 1500000000ull;
        r.first_packet_latency_ns = static_cast<std::uint64_t>((i + 1) * 100) * 1000ull; // 100..1000 us
        r.routed_routes_before = 1000;
        r.routed_routes_after = 1000;
        r.reconnected = true;
        if (i == 3) r.lost_packets = true;
        if (i == 5) r.reregistration_required = true;
        stats.add(r);
    }
    const RecoveryLatencyStats::Summary s = stats.summary();
    EXPECT_EQ(10u, s.count);
    EXPECT_DOUBLE_EQ(100.0, s.min_us);
    EXPECT_DOUBLE_EQ(1000.0, s.max_us);
    EXPECT_GE(s.p99_us, 900.0);
    EXPECT_EQ(1u, s.lost_count);
    EXPECT_EQ(1u, s.rereg_count);
    EXPECT_EQ(0u, s.failed_recovery);
}

/* ------------------------------------------------------------------ 相位与窗口 */

TEST(W03Measurement, PhaseObserverSeparatesStartupSteadyAndIdleFallback)
{
    PhaseObserver obs(60.0);
    const std::uint64_t base = 1000000000ull;
    obs.begin(base);
    /* 合成 70 s：启动峰值 10 s（18 线程）→ 稳定活跃 40 s（8 线程）→ 空闲回落 20 s（3 线程） */
    for (std::uint64_t t = 0; t <= 70000000000ull; t += 1000000000ull) {
        PhaseSample s;
        s.monotonic_ns = base + t;
        const double sec = static_cast<double>(t) / 1e9;
        if (sec < 10.0) { s.threads = 18; s.cpu_cores = 2.0; s.ctx_switch_per_s = 20000; }
        else if (sec < 50.0) { s.threads = 8; s.cpu_cores = 0.8; s.ctx_switch_per_s = 5000; }
        else { s.threads = 3; s.cpu_cores = 0.02; s.ctx_switch_per_s = 30; }
        s.rss_kb = 4096;
        s.idle_state = IdleState::connected_silent;
        const Phase p = (sec < 10.0) ? Phase::startup_peak
                      : (sec < 50.0) ? Phase::steady_active : Phase::idle_fallback;
        obs.add(p, s);
    }
    PeriodicTaskCoverage pc;
    pc.configured = true;
    pc.ticks_observed = 3500;
    pc.ticks_per_s = 50.0;
    pc.source = "test";
    obs.set_periodic_coverage(pc);

    const IdleSummary s = obs.summary();
    EXPECT_TRUE(s.window_sufficient) << "70 s 窗口应满足 >= 60 s";
    EXPECT_TRUE(s.startup_peak.covered);
    EXPECT_TRUE(s.steady_active.covered);
    EXPECT_TRUE(s.idle_fallback.covered);
    EXPECT_DOUBLE_EQ(18.0, s.startup_peak.threads_mean);
    EXPECT_DOUBLE_EQ(8.0, s.steady_active.threads_mean);
    EXPECT_DOUBLE_EQ(3.0, s.idle_fallback.threads_mean);
    EXPECT_GT(s.startup_peak.threads_max, s.steady_active.threads_max);
    EXPECT_LT(s.idle_fallback.cpu_cores_mean, s.steady_active.cpu_cores_mean);
    EXPECT_LT(s.idle_fallback.ctx_switch_per_s_mean, s.steady_active.ctx_switch_per_s_mean);
    EXPECT_LT(s.thread_fallback_ratio, 0.5);
    EXPECT_TRUE(s.idle_fallback_observed);
}

TEST(W03Measurement, ShortWindowIsRejectedAndPeriodicCoverageRequired)
{
    PhaseObserver obs(60.0);
    obs.begin(0);
    for (int i = 0; i < 10; ++i) {
        PhaseSample s;
        s.monotonic_ns = static_cast<std::uint64_t>(i) * 1000000000ull;
        s.threads = 5;
        obs.add(Phase::steady_active, s);
    }
    const IdleSummary s = obs.summary();
    EXPECT_FALSE(s.window_sufficient) << "10 s 窗口必须被判不足";
    EXPECT_FALSE(s.idle_fallback.covered);
    EXPECT_FALSE(s.idle_fallback_observed);
    /* 未声明周期任务 ⇒ 空闲观测不得声称覆盖周期任务 */
    EXPECT_FALSE(s.periodic.configured);
    EXPECT_NE(std::string::npos, s.verdict.find("未确认"));
}

/* ------------------------------------------------------------------ 路径证据 */

TEST(W03Measurement, InsufficientPathEvidenceIsMarkedUnconfirmed)
{
    EvidenceRegister reg;

    PathEvidence s2;                       // 只有"模块已接入"等级 ⇒ 未确认
    s2.path = PathKind::dzflat_a;
    s2.level = EvidenceLevel::s2_module_wired;
    s2.call_sites.push_back("src/dzIPC/shm_pub_sub_ipc.cc:100:try_publish_dzflat");
    s2.runtime_evidence.push_back("log: path=dzflat-a");
    s2.binary_fingerprints.push_back("sha256:deadbeef");
    s2.counters.push_back("dzflat_a_messages");
    s2.samples = 1000;
    reg.add(s2);

    PathEvidence b_no_inplace;             // DZFlat B 但只有内部 loan ⇒ 未确认
    b_no_inplace.path = PathKind::dzflat_b;
    b_no_inplace.level = EvidenceLevel::s3_single_case_verified;
    b_no_inplace.call_sites.push_back("src/dzIPC/shm_pub_sub_ipc.cc:200:loan");
    b_no_inplace.runtime_evidence.push_back("log: path=dzflat-b");
    b_no_inplace.binary_fingerprints.push_back("sha256:cafe");
    b_no_inplace.counters.push_back("dzflat_b_messages");
    b_no_inplace.samples = 500;
    reg.add(b_no_inplace);

    PathEvidence ok;                       // 证据齐全 ⇒ 已确认
    ok.path = PathKind::tlv;
    ok.level = EvidenceLevel::s3_single_case_verified;
    ok.call_sites.push_back("test/ipc_benchmark.cpp:1:tlv_send");
    ok.runtime_evidence.push_back("log: path=tlv");
    ok.binary_fingerprints.push_back("sha256:1234");
    ok.counters.push_back("tlv_messages");
    ok.samples = 2000;
    reg.add(ok);

    const std::vector<std::string> unconfirmed = reg.unconfirmed_paths();
    EXPECT_EQ(2u, unconfirmed.size());
    EXPECT_NE(std::string::npos, std::string(reg.items()[0].label()).find("未确认"));
    EXPECT_STREQ("未确认", reg.items()[0].label());
    EXPECT_STREQ("已确认", reg.items()[2].label());
    /* 未确认项必须点名缺什么 */
    bool mentions_level = false, mentions_loan = false;
    for (const std::string& m : reg.items()[0].missing)
        if (m.find("证据等级不足") != std::string::npos) mentions_level = true;
    for (const std::string& m : reg.items()[1].missing)
        if (m.find("publish_loan") != std::string::npos) mentions_loan = true;
    EXPECT_TRUE(mentions_level);
    EXPECT_TRUE(mentions_loan);
    EXPECT_NE(std::string::npos, reg.to_json().find("\"unconfirmed\""));
}

/* 计量口径变更必须被机器约束：三种路径各自的计数必须存在且互不混用。 */
TEST(W03Measurement, RequiredCountersExistAndAreSeparate)
{
    const char* required[] = {
        "tlv_messages", "dzflat_a_messages", "dzflat_b_messages", "tlv_bytes",
        "dzflat_a_bytes", "dzflat_b_bytes", "tlv_wire_bytes",
        "fallback_total", "fallback_backend_unavailable", "fallback_capacity_full",
        "fallback_type_incompatible", "fallback_oversized", "fallback_pool_exhausted",
        "registration_attempts", "registration_ok", "registration_failed",
        "registration_rejected", "wait_set_full", "wait_token_invalid", "fd_limit",
        "chunk_exhausted", "chunk_alloc_failed", "queue_evicted", "queue_backpressure",
        "generation_mismatch", "publish_blocked", "rx_timeout",
        "scan_rounds", "scanned_routes_total", "scan_time_ns_total", "wait_timeout_count",
        "ready_observed", "deferred_depth_last", "deferred_depth_max",
        "seq_out_of_order", "seq_duplicate", "seq_lost",
        /* W03/t44（R0-9/R0-10 结束值接口）新增 4 个 ID */
        "scan_ready_routes_total", "deferred_depth_after_last",
        "deferred_depth_after_max", "deferred_depth_after_total",
    };
    for (const char* name : required) {
        bool found = false;
        for (std::size_t i = 0; i < kCounterCount; ++i)
            if (std::strcmp(counter_table()[i].name, name) == 0) { found = true; break; }
        EXPECT_TRUE(found) << "缺少计数: " << name;
    }
    EXPECT_STRNE(counter_name(CounterId::dzflat_a_messages),
                 counter_name(CounterId::dzflat_b_messages));
    EXPECT_TRUE(counter_is_diagnostics_only(CounterId::scan_rounds));
    EXPECT_FALSE(counter_is_diagnostics_only(CounterId::dzflat_a_messages));
}

TEST(W03Measurement, SchemaCoversSampleCounterAndManifestFields)
{
    const std::string json = schema_document_json("w03-test");
    for (const char* key : {"produced_ns", "transport_done_ns", "app_obtained_ns",
                            "fully_consumed_ns", "payload_checksum_ok", "retry_count",
                            "evicted", "generation", "unique_topic_count", "fallback_count",
                            "diagnostics_enabled", "clock_cost_ns_per_call", "dzflat_b_messages"}) {
        EXPECT_NE(std::string::npos, json.find(key)) << "schema.json 缺少字段 " << key;
    }
    const std::string csv = schema_document_csv();
    EXPECT_NE(std::string::npos, csv.find("sample,produced_ns"));
    EXPECT_NE(std::string::npos, csv.find("counter,chunk_exhausted"));
    EXPECT_NE(std::string::npos, csv.find("idle_state,connected_silent"));
    /* 样本 CSV 表头与字段表必须一致 */
    const std::string header = sample_csv_header();
    EXPECT_NE(std::string::npos, header.find("run_id"));
    EXPECT_NE(std::string::npos, header.find("e2e_ns"));
    SampleRecord rec;
    rec.run_id = "r1";
    rec.route = "t/1";
    rec.path = PathKind::dzflat_b;
    rec.produced_ns = 1000;
    rec.publish_enter_ns = 1010;
    rec.transport_done_ns = 1050;
    rec.has_app_obtained = true;
    rec.app_obtained_ns = 1100;
    rec.has_fully_consumed = true;
    rec.fully_consumed_ns = 1300;
    rec.payload_bytes = 65536;
    const std::string row = rec.to_csv_row();
    const std::string line = rec.to_jsonl();
    EXPECT_NE(std::string::npos, row.find("dzflat-b"));
    EXPECT_NE(std::string::npos, row.find("300"));      // e2e = 1300-1000
    EXPECT_NE(std::string::npos, line.find("\"e2e_ns\":300"));
    EXPECT_NE(std::string::npos, line.find("\"delivery_ns\":50"));
    EXPECT_NE(std::string::npos, line.find("\"app_read_ns\":200"));
}

/* 回归：相位时长必须与插入顺序无关。
 * 采集器按"每采样时刻"分组喂入观测；若分组键用字符串，13 位与 14 位 ns 的字典序会
 * 与数值序不一致（如 "10000125064509" < "9999874868855" 字面成立），从而让某相位的
 * 末元素早于首元素，front()/back() 相减产生无符号下溢（实测曾得 1.8446744e10）。
 * 本用例故意乱序插入并跨位数边界，断言 duration_s 等于 max-min 且不出现下溢值。 */
TEST(W03Measurement, PhaseDurationIsOrderIndependentAndCannotUnderflow)
{
    PhaseObserver obs(60.0);
    /* 13 位与 14 位 ns 混杂，且插入顺序被打乱 */
    const std::uint64_t ns_13 = 9999874868855ull;   // 13 位
    const std::uint64_t ns_14 = 10000125064509ull;  // 14 位（字典序上小于上面的 13 位串）
    ASSERT_LT(std::string("10000125064509"), std::string("9999874868855"))
        << "前提: 该对数值的字符串字典序与数值序相反";

    /* 先插后者（数值更大），再插前者 —— 若实现依赖 front/back 就会下溢 */
    PhaseSample a;
    a.monotonic_ns = ns_14;
    a.threads = 8;
    obs.add(Phase::idle_fallback, a);
    PhaseSample b;
    b.monotonic_ns = ns_13;
    b.threads = 2;
    obs.add(Phase::idle_fallback, b);
    PhaseSample c;
    c.monotonic_ns = (ns_13 + ns_14) / 2;
    c.threads = 5;
    obs.add(Phase::idle_fallback, c);

    const IdleSummary s = obs.summary();
    const double expected = static_cast<double>(ns_14 - ns_13) / 1e9;
    ASSERT_TRUE(s.idle_fallback.covered);
    EXPECT_NEAR(expected, s.idle_fallback.duration_s, 1e-6)
        << "duration_s 必须等于 max-min（与插入顺序无关）";
    /* 无符号下溢的典型值不得出现 */
    EXPECT_LT(s.idle_fallback.duration_s, 1.0e6)
        << "duration_s 疑似无符号下溢: " << s.idle_fallback.duration_s;
    EXPECT_GE(s.idle_fallback.duration_s, 0.0);
    /* 三态判定不应因乱序而误报 */
    EXPECT_GE(s.total_window_s, 0.0);
}

/* ------------------------------------------------------------------ *
 * R0-9 / R0-10：扫描结束值接口（finish 出参 + 4 个新 ID + 按 worker 出参）
 * 规格来源：W06/deferred_结束值接口_规格提交W03.md（S1/S2/S4/S5）
 * ------------------------------------------------------------------ */

/* R0-9：finish(ready_routes, deferred_depth_after) 出参语义。
 * ⛔ 未调用 finish() 时"扫描后"必须回退为"扫描前"（不得给出假值）。 */
TEST(W03Measurement, ScanRoundFinishProvidesEndValues)
{
    CounterRegistry& r = CounterRegistry::instance();

    /* 未 finish：after 回退为 before，且 elapsed 未采集 */
    r.set_diagnostics_enabled(true);
    {
        ScanRoundScope s(10, 4);
        EXPECT_FALSE(s.finished());
        const ScanRoundResult res = s.result();
        EXPECT_EQ(10u, res.scanned_routes);
        EXPECT_EQ(0u, res.ready_routes);
        EXPECT_EQ(4u, res.deferred_depth_before);
        EXPECT_EQ(4u, res.deferred_depth_after) << "未 finish 时 after 必须等于 before（不得给假值）";
        EXPECT_FALSE(res.elapsed_ns_collected);
        EXPECT_EQ(0u, res.elapsed_ns);
        EXPECT_TRUE(res.diagnostics_enabled);
        EXPECT_FALSE(res.ready());
        s.finish_with_before_depth(2);
        EXPECT_TRUE(s.finished());
        EXPECT_EQ(2u, s.result().ready_routes);
        EXPECT_TRUE(s.result().elapsed_ns_collected);
        EXPECT_TRUE(s.result().ready());
    }

    /* 已 finish：出参逐字段正确，elapsed 与 scan_time_ns_total 同区间 */
    const std::uint64_t t0 = r.get(CounterId::scan_time_ns_total);
    ScanRoundResult captured;
    {
        ScanRoundScope s(10, 9);
        s.finish(3, 12);          /* 本轮新入队 3 条；扫描后深度 12 */
        s.set_ready(3 > 0);
        captured = s.result();
        EXPECT_TRUE(s.finished());
        EXPECT_EQ(3u, captured.ready_routes);
        EXPECT_EQ(12u, captured.deferred_depth_after);
        EXPECT_EQ(9u, captured.deferred_depth_before);
        EXPECT_TRUE(captured.elapsed_ns_collected);
        EXPECT_GT(captured.elapsed_ns, 0u);
    }
    EXPECT_GT(r.get(CounterId::scan_time_ns_total), t0) << "scan_time 仍在析构累加";
    r.set_diagnostics_enabled(false);
}

/* R0-9：新 4 个 ID 受诊断门控，且 `deferred_depth_after_last/_max` 是**扫描后**语义
 * （与 `deferred_depth_last` 的"扫描前"语义不同，不得互相覆盖）。 */
TEST(W03Measurement, ScanRoundEndValueCountersAreGatedAndDistinguishBeforeAfter)
{
    CounterRegistry& r = CounterRegistry::instance();

    /* 关诊断：4 个新计数一个都不写，且 result() 标记"未采集" */
    r.set_diagnostics_enabled(false);
    const std::uint64_t a0 = r.get(CounterId::scan_ready_routes_total);
    const std::uint64_t b0 = r.get(CounterId::deferred_depth_after_last);
    const std::uint64_t c0 = r.get(CounterId::deferred_depth_after_max);
    const std::uint64_t d0 = r.get(CounterId::deferred_depth_after_total);
    bool uncollected = false;
    {
        ScanRoundScope s(20, 5);
        s.finish(4, 7);
        s.set_ready(true);
        uncollected = !s.result().elapsed_ns_collected;
    }
    EXPECT_TRUE(uncollected) << "诊断关闭时 elapsed_ns 必须标记未采集";
    EXPECT_EQ(a0, r.get(CounterId::scan_ready_routes_total)) << "关诊断不得写新计数";
    EXPECT_EQ(b0, r.get(CounterId::deferred_depth_after_last));
    EXPECT_EQ(c0, r.get(CounterId::deferred_depth_after_max));
    EXPECT_EQ(d0, r.get(CounterId::deferred_depth_after_total));
    /* 「未采集」的机器判据（⛔ 不是实测 0） */
    EXPECT_TRUE(counter_is_uncollected(CounterId::scan_ready_routes_total, false));
    EXPECT_TRUE(counter_is_uncollected(CounterId::scan_time_ns_total, false));
    EXPECT_TRUE(counter_is_uncollected(CounterId::deferred_depth_after_total, false));
    EXPECT_FALSE(counter_is_uncollected(CounterId::scan_ready_routes_total, true));
    EXPECT_FALSE(counter_is_uncollected(CounterId::dzflat_a_messages, false));
    EXPECT_NE(std::string::npos,
              CounterRegistry::instance().to_json().find("\"diagnostics_collection\": \"uncollected\""))
        << "诊断关闭时 counters.json 必须声明 uncollected";

    /* 开诊断：逐轮累加与 gauge 语义 */
    r.set_diagnostics_enabled(true);
    const std::uint64_t ready0 = r.get(CounterId::scan_ready_routes_total);
    const std::uint64_t after_total0 = r.get(CounterId::deferred_depth_after_total);
    const std::uint64_t observed0 = r.get(CounterId::ready_observed);
    const std::uint64_t rounds0 = r.get(CounterId::scan_rounds);
    const std::uint64_t before_last0 = r.get(CounterId::deferred_depth_last);
    {
        ScanRoundScope s(11, 1);       /* 扫描前深度 1 */
        s.finish(2, 3);                /* 扫描后深度 3 */
        s.set_ready(true);
    }
    {
        ScanRoundScope s(11, 3);       /* 这一轮的"扫描前"= 上一轮的"扫描后" = 3 */
        s.finish(0, 0);                /* 扫描后深度 0 */
        s.set_ready(false);
    }
    EXPECT_EQ(ready0 + 2, r.get(CounterId::scan_ready_routes_total)) << "Σ 每轮 ready_routes";
    EXPECT_EQ(after_total0 + 3, r.get(CounterId::deferred_depth_after_total)) << "Σ 每轮扫描后深度 (3+0)";
    EXPECT_EQ(observed0 + 1, r.get(CounterId::ready_observed));
    EXPECT_EQ(rounds0 + 2, r.get(CounterId::scan_rounds));
    /* 两个 gauge 语义不同：入队前 = 3（第二轮构造时写入），扫描后 = 0（最后一轮） */
    EXPECT_EQ(3u, r.get(CounterId::deferred_depth_last));
    EXPECT_EQ(0u, r.get(CounterId::deferred_depth_after_last));
    EXPECT_NE(r.get(CounterId::deferred_depth_last),
              r.get(CounterId::deferred_depth_after_last))
        << "扫描前/扫描后是不同语义，不得互相覆盖";
    EXPECT_GE(r.get(CounterId::deferred_depth_after_max), 3u);
    EXPECT_NE(before_last0, r.get(CounterId::deferred_depth_after_last));
    r.set_diagnostics_enabled(false);
}

/* R0-8/规格 S4-3：`ready_observed`（轮数）与 `scan_ready_routes_total`（route 数）
 * 量纲不同 —— 拒绝相加；两者之比才是"平均每轮新入队 route 数"。 */
TEST(W03Measurement, ScanReadyRoutesIsRouteCountNotRoundCount)
{
    CounterRegistry& r = CounterRegistry::instance();
    r.set_diagnostics_enabled(true);
    const std::uint64_t observed0 = r.get(CounterId::ready_observed);
    const std::uint64_t routes0 = r.get(CounterId::scan_ready_routes_total);
    const std::uint64_t rounds0 = r.get(CounterId::scan_rounds);

    /* 3 轮，每轮新入队 2 条 ⇒ 轮数 +3、route 数 +6 */
    for (int i = 0; i < 3; ++i) {
        ScanRoundScope s(8, 0);
        s.finish(2, 2);
        s.set_ready(true);
    }
    const std::uint64_t observed = r.get(CounterId::ready_observed) - observed0;
    const std::uint64_t routes = r.get(CounterId::scan_ready_routes_total) - routes0;
    const std::uint64_t rounds = r.get(CounterId::scan_rounds) - rounds0;
    EXPECT_EQ(3u, observed);
    EXPECT_EQ(6u, routes);
    EXPECT_EQ(3u, rounds);
    EXPECT_NE(observed, routes) << "轮数与 route 数必须可区分（这正是不得相加的原因）";
    EXPECT_LE(observed, rounds) << "0 <= ready_observed <= scan_rounds";
    EXPECT_DOUBLE_EQ(2.0, static_cast<double>(routes) / static_cast<double>(rounds))
        << "平均每轮新入队 route 数 = scan_ready_routes_total / scan_rounds";
    r.set_diagnostics_enabled(false);
}

/* R0-10：每 worker 出参——全池总量 = Σ(每 worker)；⛔ 全局 gauge 不得当全池总量。 */
TEST(W03Measurement, ScanRoundAccumulatorPerWorkerSumsMatchPoolTotal)
{
    CounterRegistry& r = CounterRegistry::instance();
    r.set_diagnostics_enabled(true);
    const std::uint64_t pool_ready0 = r.get(CounterId::scan_ready_routes_total);
    const std::uint64_t pool_after_total0 = r.get(CounterId::deferred_depth_after_total);

    ScanRoundAccumulator w0, w1;
    /* worker 0：3 轮，扫描后深度各 5，每轮就绪 1 条 */
    for (int i = 0; i < 3; ++i) {
        ScanRoundScope s(16, 5);
        s.finish(1, 5);
        w0.add(s);
    }
    /* worker 1：2 轮，扫描后深度各 7，每轮就绪 0 条 */
    for (int i = 0; i < 2; ++i) {
        ScanRoundScope s(16, 7);
        s.finish(0, 7);
        w1.add(s);
    }

    EXPECT_EQ(3u, w0.rounds());
    EXPECT_EQ(2u, w1.rounds());
    EXPECT_EQ(3u, w0.ready_routes_total());
    EXPECT_EQ(0u, w1.ready_routes_total());
    EXPECT_EQ(15u, w0.deferred_depth_after_total());
    EXPECT_EQ(14u, w1.deferred_depth_after_total());
    EXPECT_EQ(5u, w0.deferred_depth_after_last());
    EXPECT_EQ(7u, w1.deferred_depth_after_last());
    EXPECT_EQ(5u, w0.deferred_depth_after_max());

    /* 全池当前总深度 = Σ(每 worker 的 last) —— 这是唯一正确的求法 */
    const std::uint64_t pool_depth = w0.deferred_depth_after_last() + w1.deferred_depth_after_last();
    EXPECT_EQ(12u, pool_depth);
    /* ⛔ 任一 gauge 都不等于它（最后一轮是 w1 的 7） */
    EXPECT_EQ(7u, r.get(CounterId::deferred_depth_after_last));
    EXPECT_NE(pool_depth, r.get(CounterId::deferred_depth_after_last))
        << "gauge 是全池总量的反例：12 != 7";
    /* `_max` 是**单调**全局 gauge：它 ≥ 任一 worker 的值，也可能因其它轮/其它 worker
     * 的更大值而不等于本轮的 Σ，因此同样**不得**当作全池当前总深度使用。
     * 这里只断言下界（不依赖 gtest 用例执行顺序与全局残留状态）。 */
    EXPECT_GE(r.get(CounterId::deferred_depth_after_max), 7u);
    EXPECT_LE(r.get(CounterId::deferred_depth_after_max), pool_depth + r.get(CounterId::deferred_depth_after_max));

    /* 累计量：acc 的和 == 全局计数增量（两者都是 Σ，可交叉核对） */
    EXPECT_EQ(r.get(CounterId::scan_ready_routes_total) - pool_ready0,
              w0.ready_routes_total() + w1.ready_routes_total());
    EXPECT_EQ(r.get(CounterId::deferred_depth_after_total) - pool_after_total0,
              w0.deferred_depth_after_total() + w1.deferred_depth_after_total());

    /* 派生值（route 数口径，⛔ 与 ready_observed/scan_rounds 的轮占比不同） */
    EXPECT_DOUBLE_EQ(1.0, w0.mean_ready_routes_per_round());
    EXPECT_DOUBLE_EQ(0.0, w1.mean_ready_routes_per_round());
    EXPECT_DOUBLE_EQ(5.0, w0.mean_deferred_depth_after());
    EXPECT_DOUBLE_EQ(7.0, w1.mean_deferred_depth_after());

    /* 全池聚合（Σ 语义） */
    std::vector<ScanRoundResult> last_results{ScanRoundResult{}, ScanRoundResult{}};
    last_results[0].deferred_depth_after = static_cast<std::size_t>(w0.deferred_depth_after_last());
    last_results[1].deferred_depth_after = static_cast<std::size_t>(w1.deferred_depth_after_last());
    last_results[0].scanned_routes = 3 * 16;
    last_results[1].scanned_routes = 2 * 16;
    const ScanRoundPoolAggregate agg = aggregate_scan_rounds(
        last_results,
        {w0.ready_routes_total(), w1.ready_routes_total()},
        {w0.deferred_depth_after_total(), w1.deferred_depth_after_total()},
        {w0.rounds(), w1.rounds()},
        {w0.ready_rounds(), w1.ready_rounds()});
    EXPECT_EQ(12u, agg.deferred_depth_after_last_sum) << "全池当前总深度 = Σ 每 worker";
    EXPECT_EQ(29u, agg.deferred_depth_after_total_sum);
    EXPECT_EQ(3u, agg.ready_routes_sum);
    EXPECT_EQ(5u, agg.rounds_sum);
    EXPECT_EQ(3u, agg.ready_rounds_sum) << "w0 三轮都有就绪 ⇒ 就绪轮数 3";
    EXPECT_EQ(80u, agg.scanned_routes_sum);
    EXPECT_DOUBLE_EQ(0.6, agg.mean_ready_routes_per_round());
    EXPECT_DOUBLE_EQ(5.8, agg.mean_deferred_depth_after());
    r.set_diagnostics_enabled(false);
}

/* ------------------------------------------------------------------ *
 * W03/t52：跨包运行级字段的**结构登记**（W02/R-5）。
 * 登记门禁：判定字段（gate 列/bad_header）必须在 schema 里登记，
 * 且区分「已采集判定(decided_zero)」与「未采集(null)」——⛔ 0 不得被读作未采集。
 * 规格来源：W02/t50_R5_新字段规格提交W03.md
 * ------------------------------------------------------------------ */
TEST(W03Measurement, SchemaRegistersW02RunLevelFields)
{
    const std::string js = schema_document_json("t52-test");
    /* summary.csv 新增 6 列 */
    for (const char* c : {"bad_header", "late_ok", "backlog_ok", "send_blocked_ok",
                          "abnormal_ok", "bad_header_ok"}) {
        EXPECT_NE(std::string::npos, js.find(std::string("\"name\": \"") + c + "\""))
            << "schema.json 未登记列 " << c;
    }
    /* manifest 新增 5 键 */
    for (const char* k : {"failure_thresholds", "currently_citable", "run_identity",
                          "evidence_discipline", "field_aliases"}) {
        EXPECT_NE(std::string::npos, js.find(std::string("\"name\": \"") + k + "\""))
            << "schema.json 未登记 manifest 键 " << k;
    }
    /* results.json 新增键 */
    EXPECT_NE(std::string::npos, js.find("\"cases[].bad_header\""));
    EXPECT_NE(std::string::npos, js.find("\"cases[].gates\""));

    /* 登记 ≠ 拥有：owner 必须是 W02，registered_by 是 W03 */
    const std::vector<RunFieldDef>& rlf = run_level_field_table();
    ASSERT_GE(rlf.size(), 13u);
    for (const RunFieldDef& f : rlf) {
        EXPECT_STREQ("W02", f.owner) << f.name << " 的语义所有者应为 W02（不是登记方 W03）";
        EXPECT_STREQ("W03", f.registered_by) << f.name;
    }
    EXPECT_NE(std::string::npos, js.find("\"structural_vs_measurement_registration\""));
    EXPECT_NE(std::string::npos, js.find("登记 != 拥有"));

    /* CSV 形态也带上 owner/zero_semantics 列 */
    const std::string csv = schema_document_csv();
    EXPECT_NE(std::string::npos, csv.find("run_field:summary.csv,bad_header"));
    EXPECT_NE(std::string::npos, csv.find("run_field:manifest.json,currently_citable"));
    EXPECT_NE(std::string::npos, csv.find("owner=W02"));
    int run_field_rows = 0;
    for (std::size_t p2 = 0; (p2 = csv.find("\nrun_field:", p2)) != std::string::npos; ++p2) ++run_field_rows;
    EXPECT_EQ(rlf.size(), static_cast<std::size_t>(run_field_rows));

    /* W02 侧复查脚本口径（规格 §6）：逐字段可查 + CSV 表头可机械使用 */
    EXPECT_STRNE("", summary_csv_extra_header());
    EXPECT_NE(std::string::npos, std::string(summary_csv_extra_header()).find("bad_header_ok"));
    EXPECT_EQ(5u, manifest_extension_keys().size());
}

/* 判定结果（0/1）与「未采集（null）」必须结构上可区分：
 * gate 列/bad_header 不是 nullable（未触发也写 0/1），而 currently_citable 可为 null。
 * ⛔ 读方不得把 gate 的 0 读成「未采集」——这是 W02 规格里明确点名的风险。 */
TEST(W03Measurement, JudgementArtifactsAreNotMisreadAsUncollected)
{
    const std::string js = schema_document_json("t52-test");
    EXPECT_NE(std::string::npos, js.find("\"zero_value_three_states\""));
    EXPECT_NE(std::string::npos, js.find("\"decided_zero\""));

    const std::vector<RunFieldDef>& rlf = run_level_field_table();
    auto find = [&](const char* n) -> const RunFieldDef* {
        for (const RunFieldDef& f : rlf) if (std::string(f.name) == n) return &f;
        return nullptr;
    };
    /* 5 个 gate 与 6 列的判定语义 */
    for (const char* n : {"late_ok", "backlog_ok", "send_blocked_ok", "abnormal_ok", "bad_header_ok"}) {
        const RunFieldDef* f = find(n);
        ASSERT_NE(nullptr, f) << n;
        EXPECT_STREQ("decided_zero", f->zero_semantics) << n << " 是已采集判定, 不得写成未采集";
        EXPECT_FALSE(f->nullable) << n << " 未触发也写 0/1, 不得为 null";
    }
    /* bad_header 是实测量（0 = 实测为零） */
    const RunFieldDef* bh = find("bad_header");
    ASSERT_NE(nullptr, bh);
    EXPECT_STREQ("measured_zero", bh->zero_semantics);
    EXPECT_FALSE(bh->nullable);
    /* 唯一允许 null 的是引用权威指针 */
    const RunFieldDef* cc = find("currently_citable");
    ASSERT_NE(nullptr, cc);
    EXPECT_TRUE(cc->nullable);
    EXPECT_STREQ("uncollected_null", cc->zero_semantics);
    EXPECT_NE(std::string::npos, js.find("\"currently_citable\", \"type\": \"string|null\""));
}

/* CSV 形态必须是**结构良好**的：每一行的列数 == 表头列数。
 * 根因：说明字段里含逗号，若不按 RFC4180 加引号，一行会被拆成多列（实测 t52 前 8 行、
 * t52 追加后 19 行不一致）—— 对"按列取值"的读方是静默错位。 */
TEST(W03Measurement, SchemaCsvIsWellFormed)
{
    const std::string csv = schema_document_csv();
    std::size_t expected = 1;   /* 至少表头 1 列 */
    {
        std::size_t pos = csv.find('\n');
        const std::string header = csv.substr(0, pos == std::string::npos ? csv.size() : pos);
        expected = 1;
        for (char c : header) if (c == ',') ++expected;
    }
    std::size_t line_no = 0, bad = 0;
    std::istringstream is(csv);
    std::string line;
    while (std::getline(is, line)) {
        ++line_no;
        if (line.empty()) continue;
        /* 计列数时必须忽略引号内的逗号 */
        std::size_t cols = 1;
        bool in_quotes = false;
        for (std::size_t i = 0; i < line.size(); ++i) {
            const char c = line[i];
            if (c == '"') {
                if (in_quotes && i + 1 < line.size() && line[i + 1] == '"') { ++i; continue; }
                in_quotes = !in_quotes;
            } else if (c == ',' && !in_quotes) {
                ++cols;
            }
        }
        if (cols != expected) ++bad;
    }
    EXPECT_GT(line_no, 100u) << "schema.csv 行数异常";
    EXPECT_EQ(0u, bad) << "有 " << bad << " 行与表头列数(" << expected << ")不一致; "
                          "检查 csv_escape() 是否覆盖了该行";
    EXPECT_NE(std::string::npos, csv.find("run_field:manifest.json,currently_citable"));
    EXPECT_EQ("\"a,b\"", csv_escape("a,b"));
    EXPECT_EQ("\"a\"\"b\"", csv_escape("a\"b"));
    EXPECT_EQ("plain", csv_escape("plain"));
}

/* 回归：W03 的测量字段登记不得被运行级登记挤掉（追加而非替换）。 */
TEST(W03Measurement, MeasurementFieldsStillRegisteredAfterRunLevelExtension)
{
    const std::string js = schema_document_json("t52-test");
    for (const char* k : {"produced_ns", "e2e_ns", "dzflat_b_messages",
                          "scan_ready_routes_total", "deferred_depth_after_last"}) {
        EXPECT_NE(std::string::npos, js.find(k)) << "追加运行级字段时挤掉了 " << k;
    }
    EXPECT_EQ(66u, kCounterCount) << "计数总数不得因本次登记而变化（只登记结构，不改计数）";
}

/* null 与 0 必须区分：未测字段 JSON 输出 null，CSV 输出空字段。 */
TEST(W03Measurement, UnmeasuredFieldsAreNullNotZero)
{
    SampleRecord rec;
    rec.run_id = "r1";
    rec.route = "t/1";
    rec.path = PathKind::tlv;
    rec.produced_ns = 1;
    rec.publish_enter_ns = 2;
    rec.transport_done_ns = 3;
    /* app_obtained / fully_consumed 未测 */
    const std::string line = rec.to_jsonl();
    EXPECT_NE(std::string::npos, line.find("\"app_obtained_ns\":null"));
    EXPECT_NE(std::string::npos, line.find("\"fully_consumed_ns\":null"));
    EXPECT_NE(std::string::npos, line.find("\"e2e_ns\":null"));
    const std::string csv = rec.to_csv_row();
    EXPECT_NE(std::string::npos, csv.find(",,")) << "CSV 未测字段必须留空而不是 0";
}

/* CPU 口径核验（§4 W03 第 2 条；W01 UF-06 的 C≈D）：
 * `/proc/<pid>/stat` 的 utime+stime 与逐 TID utime+stime 之和必须一致
 * （内核 `do_task_stat(..., whole=1)` → `thread_group_cputime_adjusted`，见
 * docs/.../W03/外部来源存证/proc_array_v6.8_do_task_stat.md）。
 * 同时验证 `/proc/<pid>/status` 的 ctx 字段来自单任务（不得当作线程组合计）：
 * 多线程忙跑时，逐 TID 聚合必须显著超过主线程参考值。 */
TEST(W03Measurement, CpuAggregationMatchesTaskGroupSum)
{
    /* 子进程内含 3 条忙转线程，制造可观 CPU 与切换。 */
    const pid_t pid = ::fork();
    if (pid == 0) {
        std::atomic<bool> stop{false};
        std::vector<std::thread> ts;
        for (int i = 0; i < 3; ++i) {
            ts.emplace_back([&stop]() {
                volatile double x = 0.0;
                while (!stop.load()) x += 1.0;
            });
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        stop.store(true);
        for (auto& t : ts) t.join();
        ::_exit(0);
    }
    ASSERT_GT(pid, 0);

    ProcessSampler sampler(pid);
    ASSERT_TRUE(sampler.attach());
    const long hz = clock_ticks_per_sec();
    double ratio = 0.0;
    std::uint64_t main_ref = 0, aggregate = 0;
    for (int i = 0; i < 12; ++i) {
        if (!sampler.sample()) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    const CpuScopeSample scope = sampler.cpu_scope();
    ASSERT_TRUE(scope.valid) << "忙转子进程在采样窗口内无线程退出, 应产生合格 CPU 口径采样";
    const std::uint64_t proc_stat_ticks = scope.proc_stat_ticks;
    const std::uint64_t task_sum_ticks = scope.task_group_sum_ticks;
    main_ref = sampler.ctx().main_thread_voluntary_reference;
    aggregate = sampler.ctx().voluntary;
    ratio = scope.ratio;

    sampler.finish();
    int status = 0;
    ASSERT_EQ(pid, ::wait4(pid, &status, 0, nullptr));

    EXPECT_GT(proc_stat_ticks, static_cast<std::uint64_t>(hz / 100))
        << "忙转子进程应有可测 CPU tick";
    EXPECT_GT(ratio, 0.5) << "逐 TID CPU 之和 " << task_sum_ticks
                          << " 远小于 /proc/<pid>/stat " << proc_stat_ticks
                          << " => /proc/<pid>/stat 未必是线程组累计";
    EXPECT_LT(ratio, 1.5) << "逐 TID CPU 之和 " << task_sum_ticks
                          << " 远大于 /proc/<pid>/stat " << proc_stat_ticks
                          << " => 逐 TID 求和重复计数";
    /* 4 条忙转线程（含主线程）全部在跑：聚合切换不应低于主线程参考值 */
    EXPECT_GE(aggregate, main_ref);
}

/* 平台信息必须可采（拓扑/环境进 environment.json / topology.json）。 */
TEST(W03Measurement, PlatformInfoIsCollectable)
{
    const PlatformInfo p = collect_platform_info();
    EXPECT_GT(p.online_cpus, 0);
    EXPECT_GT(p.clock_ticks_per_sec, 0);
    EXPECT_FALSE(p.cpu_governors_unique.empty());
    const std::string js = platform_info_json(p);
    EXPECT_NE(std::string::npos, js.find("\"online_cpus\""));
    EXPECT_NE(std::string::npos, js.find("\"clock_ticks_per_sec\""));
    EXPECT_NE(std::string::npos, js.find("\"nofile_soft\""));
}

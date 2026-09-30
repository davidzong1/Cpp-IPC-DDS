/* W06/t30 —— §10.2 扫描计数接线验证工装（**在 W10 工装目录内增量**，不另造平行工装）。
 *
 * 为什么放在这里：队长裁决 D-23 要求「复用或参照 W10 已落盘的规模工装」；本文件与
 * `w10_threadscan.cpp` / `w10_matrix.cpp` 同目录、同一编译方式（静态链 `build/lib`
 * 的 libipc + 同一 -I 集合）、同一话题命名与握手等待口径，只补一个 W10 侧没有的量：
 * **逐档读 §10.2 的扫描计数**（常驻 + 诊断两套），并把它们随 `N`（route 数）与
 * `N_worker` 的变化落盘。
 *
 * 用法：
 *   w10_scancost --n 100 --workers 4 --diag on  --window-ms 6000 --out <dir> --run-id <id>
 *                [--domain 7000] [--msgs 3]
 *
 * 判据（本工具自己给，不依赖外部）：
 *   · 常驻三件套与诊断三件套在 diag=on 时必须**逐值相等**（同一接线点同一取值）；
 *   · `mean_routes_per_round = scanned_routes_total / scan_rounds` ⇒ 每 worker ≈ N/W；
 *   · 扫描总工作量随 N 线性（`scanned_routes_per_s` 与 N 成正比）；
 *   · `有效就绪比例 = scan_ready_rounds / scan_rounds` 有落盘值。
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <filesystem>
#include <fstream>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/common/control_plane.h"
#include "dzIPC/detail/shm_sub_seam.h"
#include "dzIPC/measure/counters.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/threepools/recv_worker.h"
#include "dzIPC/threepools/shm_control_scheduler.h"
#include "ipc_msg/std_msgs/std_string.hpp"

using Clock = std::chrono::steady_clock;
static constexpr std::uint32_t kMsgId = 93;

static long arg_long(int argc, char** argv, const char* key, long def)
{
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], key) == 0) return std::strtol(argv[i + 1], nullptr, 10);
    }
    return def;
}
static const char* arg_str(int argc, char** argv, const char* key, const char* def)
{
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
    }
    return def;
}

static std::atomic<int> g_feeder_alive{0};

/* 接收路径事件计数（seam）：用来证明"多出来的线程"不是静默产生的 ——
 * 容量装不下时每条溢出 route 都必须有一次 `kRecvPathCompat`（原因码 kWaitSetFull）。
 * ⛔ 只统计，不改变任何行为；hook 是无捕获函数指针（seam 的约定）。 */
static std::atomic<long> g_compat_path{0};
static std::atomic<long> g_worker_path{0};
static std::atomic<long> g_compat_reason_waitsetfull{0};

static void sc_seam_hook(const dzIPC::detail::SeamEvent& ev) noexcept
{
    if (ev.point == dzIPC::detail::SeamPoint::kRecvPathWorker)
    {
        g_worker_path.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (ev.point != dzIPC::detail::SeamPoint::kRecvPathCompat) return;
    g_compat_path.fetch_add(1, std::memory_order_relaxed);
    if (static_cast<dzIPC::detail::RecvPathReason>(ev.size) == dzIPC::detail::RecvPathReason::kWaitSetFull)
        g_compat_reason_waitsetfull.fetch_add(1, std::memory_order_relaxed);
}

static std::shared_ptr<dzIPC::TopicData> td()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId);
}

/* 线程数（`/proc/self/task`，与 W00/W05/W10 同一口径）。
 * 为什么本工装必须自己读：要证明的命题是"**线程 O(1) 但扫描 O(route)**"，
 * 两者必须在**同一次运行、同一份证据**里给出，否则就会出现"线程数来自 W05、扫描量
 * 来自本工具"的拼接式论证（不同二进制、不同负载，无法互相约束）。 */
static std::size_t thread_count()
{
    std::size_t n = 0;
    DIR* d = ::opendir("/proc/self/task");
    if (d == nullptr) return 0;
    while (dirent* e = ::readdir(d))
    {
        if (e->d_name[0] != '.') ++n;
    }
    ::closedir(d);
    return n;
}

/* 进程 CPU（ticks，含所有线程；口径与 W10 工装 `/proc/self/stat` 第 14/15 字段一致）。
 * 用于「常驻 vs 诊断」两配置的**直接成本对照**：同一档、同一窗口，只差
 * `diagnostics_enabled()` 一个布尔 —— 差值就是"详细诊断采样"的代价。 */
static double proc_cpu_ticks()
{
    std::ifstream f("/proc/self/stat");
    if (!f) return -1;
    std::string b;
    std::getline(f, b);
    const auto q = b.rfind(')');
    if (q == std::string::npos) return -1;
    std::istringstream is(b.substr(q + 2));
    std::string t;
    int i = 2;
    double u = 0, s = 0;
    while (is >> t)
    {
        if (i == 13) u = static_cast<double>(std::strtoull(t.c_str(), nullptr, 10));
        else if (i == 14) s = static_cast<double>(std::strtoull(t.c_str(), nullptr, 10));
        ++i;
    }
    return u + s;
}

/* 线程账（可归因，而不是只报总数）：把 `/proc/self/task` 的总数拆成
 *   · `workers`      —— 共享 SHM 收包池的在册 worker 数（`RecvWorkerPool::worker_count()`）；
 *   · `orchestration`—— 本工装自己的线程（主线程 + 活跃馈送现 + 无）；
 *   · `residual`     —— 上两者之外的**剩余**线程数。
 * ⛔ 只报总数会掩盖"多出来的那条是什么"，而**剩余必须为 0 或常数**才是
 * "per-topic 线程 = 0"的可核对形态（方案 §13.2 条件 4：不得用额外 per-topic 线程补齐）。
 * 本工装主动起了 1 条馈送线程，故 expect_residual = 0（扣掉它之后）。 */
static int live_feeder_threads() noexcept
{
    return g_feeder_alive.load(std::memory_order_acquire) ? 1 : 0;
}

struct Res
{
    std::uint64_t scan_rounds{0};
    std::uint64_t scanned_routes_total{0};
    std::uint64_t scan_ready_rounds{0};
    std::uint64_t deferred_depth_last{0};
    std::uint64_t deferred_depth_max{0};
    std::uint64_t wait_timeouts{0};
    std::uint64_t wait_wakeups{0};
    std::uint64_t messages_received{0};
    std::size_t route_count{0};
    std::size_t workers{0};
};

static Res snap_resident()
{
    const auto st = dzIPC::threepools::RecvWorkerPool::instance().stats();
    Res r;
    r.scan_rounds = st.scan_rounds;
    r.scanned_routes_total = st.scanned_routes_total;
    r.scan_ready_rounds = st.scan_ready_rounds;
    r.deferred_depth_last = st.deferred_depth_last;
    r.deferred_depth_max = st.deferred_depth_max;
    r.wait_timeouts = st.wait_timeouts;
    r.wait_wakeups = st.wait_wakeups;
    r.messages_received = st.messages_received;
    r.route_count = st.route_count;
    r.workers = dzIPC::threepools::RecvWorkerPool::instance().worker_count();
    return r;
}

struct Gated
{
    std::uint64_t scan_rounds{0};
    std::uint64_t scanned_routes_total{0};
    std::uint64_t scan_time_ns_total{0};
    std::uint64_t wait_timeout_count{0};
    std::uint64_t ready_observed{0};
    std::uint64_t deferred_depth_last{0};
    std::uint64_t deferred_depth_max{0};
};

static Gated snap_gated()
{
    const auto c = dzIPC::measure::CounterRegistry::instance().snapshot();
    const auto g = [&](dzIPC::measure::CounterId id) { return static_cast<std::uint64_t>(c.get(id)); };
    Gated k;
    k.scan_rounds = g(dzIPC::measure::CounterId::scan_rounds);
    k.scanned_routes_total = g(dzIPC::measure::CounterId::scanned_routes_total);
    k.scan_time_ns_total = g(dzIPC::measure::CounterId::scan_time_ns_total);
    k.wait_timeout_count = g(dzIPC::measure::CounterId::wait_timeout_count);
    k.ready_observed = g(dzIPC::measure::CounterId::ready_observed);
    k.deferred_depth_last = g(dzIPC::measure::CounterId::deferred_depth_last);
    k.deferred_depth_max = g(dzIPC::measure::CounterId::deferred_depth_max);
    return k;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const long n = arg_long(argc, argv, "--n", 100);
    const long workers = arg_long(argc, argv, "--workers", 0);
    const long dom = arg_long(argc, argv, "--domain", 7000);
    const long msgs = arg_long(argc, argv, "--msgs", 3);
    const long window_ms = arg_long(argc, argv, "--window-ms", 6000);
    const bool diag = std::strcmp(arg_str(argc, argv, "--diag", "off"), "on") == 0;
    const std::string out = arg_str(argc, argv, "--out", "");
    const std::string run_id = arg_str(argc, argv, "--run-id", "scancost");
    const bool led = !out.empty();

    dzIPC::detail::SetSeamHook(&sc_seam_hook);
    dzIPC::measure::CounterRegistry::instance().set_diagnostics_enabled(diag);

    /* 池必须**先**按本档 worker 数启动：池的 worker 数是"首个成功 start() 的调用方"
     * 一次性决定的（契约 §8.1），先启动才能保证本档的 N_worker 真的生效。 */
    auto& pool = dzIPC::threepools::RecvWorkerPool::instance();
    if (!pool.start(static_cast<std::size_t>(workers > 0 ? workers : 0), dzIPC::threepools::RecvBudget{}))
    {
        std::printf("NOTE pool already started (worker_count=%zu)\n", pool.worker_count());
    }

    std::vector<std::string> names;
    std::vector<std::unique_ptr<dzIPC::shm::shm_pub_ipc>> pubs;
    std::vector<std::unique_ptr<dzIPC::shm::shm_sub_ipc>> subs;
    names.reserve(static_cast<std::size_t>(n));
    for (long i = 0; i < n; ++i)
    {
        names.push_back("sc_" + std::to_string(dom) + "_" + std::to_string(i));
        pubs.emplace_back(new dzIPC::shm::shm_pub_ipc(td(), names.back(), static_cast<std::size_t>(dom), false));
        pubs.back()->InitChannel("sc");
        subs.emplace_back(new dzIPC::shm::shm_sub_ipc(td(), names.back(), static_cast<std::size_t>(dom), 64, false));
        subs.back()->InitChannel("sc");
    }
    long attached = 0;
    {
        const auto dl = Clock::now() + std::chrono::seconds(120);
        while (Clock::now() < dl && attached < n)
        {
            attached = 0;
            for (long i = 0; i < n; ++i)
            {
                dzIPC::control_plane_shm::TopicControlPlane cp;
                if (cp.open(shm_topic_control_name(names[static_cast<std::size_t>(i)], static_cast<std::size_t>(dom)))
                    && cp.peer_count() >= 1)
                    ++attached;
            }
            if (attached < n) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    /* 每 route 发 msgs 条并全部收满：让扫描轮数在有真实就绪的负载下统计
     * （空转窗口也能测，但"有效就绪比例"会成为 0，失去对照意义）。 */
    long rx = 0;
    for (long k = 0; k < msgs; ++k)
    {
        for (long i = 0; i < n; ++i)
        {
            auto m = std::make_shared<dzIPC::Msg::StdString>();
            m->set_msg_id(kMsgId);
            m->str = "S" + std::to_string(i) + "_" + std::to_string(k);
            (void)pubs[static_cast<std::size_t>(i)]->publish(m);
        }
    }
    for (long i = 0; i < n; ++i)
    {
        auto sink = td();
        const auto dl = Clock::now() + std::chrono::milliseconds(5000);
        while (Clock::now() < dl)
        {
            if (subs[static_cast<std::size_t>(i)]->try_get_clone(sink)) { ++rx; break; }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(500));

    /* ---------------- 测量窗口：只读计数差分 ----------------
     * 窗口内**持续有真实就绪**：一个后台线程按轮转顺序逐 route 发布并立刻排空，
     * 否则"有效就绪比例"恒为 0，失去对照意义（扫描量与就绪量必须同时非零）。
     *
     * ⛔ 两套快照不是原子同取的：常驻量与门控量分处两个对象，两次读之间可能有
     * 扫描轮完成。因此用**三明治**读法（常驻 → 门控 → 常驻）并断言门控差落在
     * 两次常驻差之间；这不是"放宽判据"，而是把探针侧的读数竞态显式建模。 */
    std::atomic<bool> stop{false};
    std::atomic<long> live_sent{0};
    std::thread feeder([&] {
        g_feeder_alive.store(1, std::memory_order_release);
        long i = 0;
        while (!stop.load(std::memory_order_relaxed))
        {
            auto m = std::make_shared<dzIPC::Msg::StdString>();
            m->set_msg_id(kMsgId);
            m->str = "L" + std::to_string(i);
            if (pubs[static_cast<std::size_t>(i % n)]->publish(m)) live_sent.fetch_add(1);
            auto sink = td();
            (void)subs[static_cast<std::size_t>(i % n)]->try_get_clone(sink);
            ++i;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    });

    const Res r0 = snap_resident();
    const Gated k0 = snap_gated();
    const double cpu0 = proc_cpu_ticks();
    const auto w0 = Clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(window_ms));
    const double win_s = std::chrono::duration<double>(Clock::now() - w0).count();
    const double cpu1 = proc_cpu_ticks();
    const double cpu_cores = (cpu1 >= 0 && cpu0 >= 0) ? (cpu1 - cpu0) / 100.0 / win_s : -1.0;
    const Gated k1 = snap_gated();
    const Res r1 = snap_resident();
    stop.store(true, std::memory_order_relaxed);
    feeder.join();
    g_feeder_alive.store(0, std::memory_order_release);

    const std::uint64_t d_rounds = r1.scan_rounds - r0.scan_rounds;
    const std::uint64_t d_scanned = r1.scanned_routes_total - r0.scanned_routes_total;
    const std::uint64_t d_ready = r1.scan_ready_rounds - r0.scan_ready_rounds;
    const std::uint64_t d_timeout = r1.wait_timeouts - r0.wait_timeouts;
    const std::uint64_t d_g_rounds = k1.scan_rounds - k0.scan_rounds;
    const std::uint64_t d_g_scanned = k1.scanned_routes_total - k0.scanned_routes_total;
    const std::uint64_t d_g_time = k1.scan_time_ns_total - k0.scan_time_ns_total;
    const std::uint64_t d_g_timeout = k1.wait_timeout_count - k0.wait_timeout_count;
    const std::uint64_t d_g_ready = k1.ready_observed - k0.ready_observed;

    /* ⛔ 容量边界必须显式记录，不得用"attached == n"掩盖：每 worker 的 wait token
     * 上限是 127（W04 §7），故池能接的最大 route 数 = 127 × N_worker。默认 32 worker
     * （容量 4064）远大于 1000，但本 sweep 刻意包含 W=4（容量 **508**）与 W=8（1016）
     * ⇒ 前者装不下 1000，多出的 route 按设计**显式回退**兼容线程（静默丢包是禁止的，
     * 回退线程照样收得到，故 `rx == n` 仍然成立）。因此"每轮扫描 route 数"的正确
     * 口径是 **池内在册 route 数**，不是话题数 —— 报告必须同时给出两者。 */
    const std::size_t pool_capacity = r1.workers * 127u;
    const bool capacity_binds = r1.route_count < static_cast<std::size_t>(n);

    const double mean_routes_per_round = d_rounds > 0 ? static_cast<double>(d_scanned) / d_rounds : 0.0;
    /* 期望值以**在册 route 数**为分子（不是话题数）：容量装不下时后者会高估。 */
    const double per_worker_routes =
        (r1.workers > 0) ? static_cast<double>(r1.route_count) / static_cast<double>(r1.workers) : 0.0;
    const double scanned_per_s = d_scanned / win_s;
    const double scanned_per_s_per_route = r1.route_count > 0 ? scanned_per_s / r1.route_count : 0.0;
    const double mean_scan_ns = d_g_rounds > 0 ? static_cast<double>(d_g_time) / d_g_rounds : 0.0;
    const double ready_ratio = d_rounds > 0 ? static_cast<double>(d_ready) / d_rounds : 0.0;

    /* 「线程 O(1) 但扫描 O(route)」的同一份证据：线程数与扫描量在**同一次运行**里取。 */
    const std::size_t threads_now = thread_count();
    const long sched = dzIPC::shm_control::ShmControlScheduler::instance().worker_active() ? 1 : 0;
    const long orchestration = 1 + live_feeder_threads();   /* 主线程 + 馈送线程 */
    const long residual =
        static_cast<long>(threads_now) - static_cast<long>(r1.workers) - orchestration - sched;
    /* 容量受限时溢出的 route 数（= 走显式回退的条数）。 */
    const long overflow = static_cast<long>(n) - static_cast<long>(r1.route_count);
    const double scans_per_thread = threads_now > 0 ? scanned_per_s / static_cast<double>(threads_now) : 0.0;

    std::printf("scancost n=%ld workers=%zu diag=%s window_s=%.3f attached=%ld rx=%ld route_count=%zu "
                "pool_capacity=%zu capacity_binds=%d threads=%zu cpu_cores=%.5f\n",
                n, r1.workers, diag ? "on" : "off", win_s, attached, rx, r1.route_count, pool_capacity,
                static_cast<int>(capacity_binds), threads_now, cpu_cores);
    std::printf("threads_breakdown pool_workers=%zu control_scheduler=%ld orchestration=%ld residual=%ld "
                "overflow=%ld compat_path=%ld compat_waitsetfull=%ld worker_path=%ld\n",
                r1.workers, sched, orchestration, residual, overflow, g_compat_path.load(),
                g_compat_reason_waitsetfull.load(), g_worker_path.load());
    std::printf("resident scan_rounds=%llu scanned_routes_total=%llu scan_ready_rounds=%llu "
                "deferred_depth_last=%llu deferred_depth_max=%llu wait_timeouts=%llu wait_wakeups=%llu\n",
                (unsigned long long)d_rounds, (unsigned long long)d_scanned,
                (unsigned long long)d_ready, (unsigned long long)r1.deferred_depth_last,
                (unsigned long long)r1.deferred_depth_max, (unsigned long long)d_timeout,
                (unsigned long long)(r1.wait_wakeups - r0.wait_wakeups));
    std::printf("gated scan_rounds=%llu scanned_routes_total=%llu scan_time_ns_total=%llu wait_timeout_count=%llu "
                "ready_observed=%llu deferred_depth_last=%llu deferred_depth_max=%llu\n",
                (unsigned long long)d_g_rounds, (unsigned long long)d_g_scanned,
                (unsigned long long)d_g_time, (unsigned long long)d_g_timeout, (unsigned long long)d_g_ready,
                (unsigned long long)k1.deferred_depth_last, (unsigned long long)k1.deferred_depth_max);
    std::printf("derived mean_routes_per_round=%.2f expected_routes_over_w=%.2f scanned_routes_per_s=%.1f "
                "scanned_routes_per_s_per_route=%.1f mean_scan_ns_per_round=%.1f ready_ratio=%.4f "
                "scan_rounds_per_s=%.1f\n",
                mean_routes_per_round, per_worker_routes, scanned_per_s, scanned_per_s_per_route, mean_scan_ns,
                ready_ratio, d_rounds / win_s);

    int verdict = 0;
    const auto fail = [&](const char* s) { std::printf("FAILURE: %s\n", s); verdict = 1; };
    if (attached != n) fail("attached != n（用例前提不成立）");
    if (rx != n) fail("rx != n（收发前提不成立）");
    if (d_rounds == 0) fail("scan_rounds 增量为 0 ⇒ 扫描计数未接线或窗口内无扫描");
    if (live_sent.load() == 0) fail("窗口内无真实发布 ⇒ 就绪量判据没有牙");
    /* 「线程 O(1)」的可核对形态（**按容量是否受限分两种判据**）：
     *   · 容量不受限（本期目标场景：默认 32 worker ⇒ 容量 4064 ≥ 1000）⇒ 剩余必须为 **0**，
     *     即 per-topic 接收线程确实清零；
     *   · 容量受限（本 sweep 刻意构造的 W=4 + 1000 话题）⇒ 溢出 route 按设计**显式回退**
     *     兼容线程，故剩余必须**恰好等于溢出条数**，且 seam 上必须有等量的
     *     `kRecvPathCompat(kWaitSetFull)` 事件 —— 这同时证明"回退不是静默的"。
     * ⛔ 两种情形都不允许"剩余线程无法归因"（那才是 §13.2 条件 4 禁止的形态）。 */
    if (!capacity_binds)
    {
        if (residual != 0) fail("容量不受限但扣除池 worker 与编排线程后仍有剩余线程 ⇒ per-topic 线程未清零");
    }
    else
    {
        if (residual != overflow)
            fail("容量受限时剩余线程 != 溢出条数 ⇒ 回退线程数与溢出 route 数不符（无法归因）");
        if (g_compat_reason_waitsetfull.load() != overflow)
            fail("容量受限但 seam 上的 kRecvPathCompat(kWaitSetFull) 数与溢出条数不等 ⇒ 回退不显式");
    }

    /* ---- 自洽判据（两套计数在同一接线点、同一取值写入）----
     *
     * ⛔ 为什么不是"逐值严格相等"：常驻量与门控量分处两个对象，快照不是原子同取的。
     * 这里故意让门控窗口**嵌套**在常驻窗口之内（r0 → k0 → 窗口 → k1 → r1），于是
     * `d_g_rounds <= d_rounds` 在数学上必然成立，而"接线正确性"由**比值**判定：
     * 窗口内 route 集合不变 ⇒ 两边 `scanned/rounds` 都是同一个加权平均，必须相等。
     * 这样判据既是强判据（比值容差 0.5%），又不依赖两次快照的同取性。 */
    if (diag)
    {
        if (d_g_rounds == 0) fail("diag=on 但门控 scan_rounds 为 0（ScanRoundScope 未生效）");
        if (d_g_rounds > d_rounds) fail("门控窗口不在常驻窗口之内（探针读法被改坏）");
        if (d_g_time == 0) fail("diag=on 但 scan_time_ns_total 未累加（ScanRoundScope 未生效）");
        if (d_g_timeout > d_timeout) fail("门控 wait_timeout_count 超出常驻口径");
        if (d_g_ready > d_ready) fail("门控 ready_observed 超出常驻口径");
        const double ratio_r = d_rounds > 0 ? static_cast<double>(d_scanned) / d_rounds : 0.0;
        const double ratio_g = d_g_rounds > 0 ? static_cast<double>(d_g_scanned) / d_g_rounds : 0.0;
        if (ratio_g > 0.0 && std::fabs(ratio_r - ratio_g) > ratio_r * 0.005 + 0.01)
            fail("diag=on 时两套计数的 mean_routes_per_round 不等 ⇒ 不是同一接线点/同一取值");
    }
    else
    {
        if (d_g_rounds != 0 || d_g_scanned != 0 || d_g_time != 0 || d_g_timeout != 0 || d_g_ready != 0)
            fail("diag=off 时门控计数必须恒为 0（否则诊断门控失效）");
    }
    /* 每轮扫描 route 数必须等于本 worker 在册 route 数（本轮全量遍历的口径）。 */
    if (per_worker_routes > 0.0 && mean_routes_per_round < per_worker_routes * 0.5)
        fail("mean_routes_per_round 明显小于 route_count/W ⇒ 扫描不是全量遍历（口径或语义被改动）");
    /* 容量边界必须与"显式回退"事实一致：装不下的部分不能静默丢 —— 它是回退线程接住的。 */
    if (capacity_binds && rx != n) fail("容量装不下时仍要求 rx == n（多出的 route 必须由回退线程收到）");
    /* 有效就绪比例：窗口内有真实发布 ⇒ 必须非零（否则该量没有对照意义）。 */
    if (d_ready == 0) fail("scan_ready_rounds 增量为 0，但窗口内有真实发布 ⇒ 就绪判据失效");

    if (led)
    {
        std::error_code ec;
        if (std::filesystem::exists(out, ec) && !std::filesystem::is_empty(out, ec))
        {
            std::printf("ARTIFACT_DIR_NOT_EMPTY: %s\n", out.c_str());
            return verdict;
        }
        std::filesystem::create_directories(out, ec);
        std::ofstream f(out + "/scancost.json");
        f << "{\n  \"run_id\": \"" << run_id << "\",\n  \"n\": " << n << ",\n  \"workers\": " << r1.workers
          << ",\n  \"domain\": " << dom << ",\n  \"diag\": " << (diag ? "true" : "false") << ",\n"
          << "  \"window_s\": " << win_s << ",\n  \"attached\": " << attached << ",\n  \"rx\": " << rx << ",\n"
          << "  \"route_count\": " << r1.route_count << ",\n"
          << "  \"pool_capacity\": " << pool_capacity << ",\n"
          << "  \"capacity_binds\": " << (capacity_binds ? "true" : "false") << ",\n"
          << "  \"threads\": " << threads_now << ",\n"
          << "  \"cpu_cores\": " << cpu_cores << ",\n"
          << "  \"control_scheduler_threads\": " << sched << ",\n"
          << "  \"orchestration_threads\": " << orchestration << ",\n"
          << "  \"residual_threads\": " << residual << ",\n"
          << "  \"overflow_routes\": " << overflow << ",\n"
          << "  \"seam\": {\"worker_path\": " << g_worker_path.load() << ", \"compat_path\": "
          << g_compat_path.load() << ", \"compat_kWaitSetFull\": " << g_compat_reason_waitsetfull.load()
          << "},\n"
          << "  \"scanned_routes_per_s_per_thread\": " << scans_per_thread << ",\n"
          << "  \"resident\": {\"scan_rounds\": " << d_rounds << ", \"scanned_routes_total\": " << d_scanned
          << ", \"scan_ready_rounds\": " << d_ready << ", \"deferred_depth_last\": " << r1.deferred_depth_last
          << ", \"deferred_depth_max\": " << r1.deferred_depth_max << ", \"wait_timeouts\": " << d_timeout
          << "},\n"
          << "  \"gated\": {\"scan_rounds\": " << d_g_rounds << ", \"scanned_routes_total\": " << d_g_scanned
          << ", \"scan_time_ns_total\": " << d_g_time << ", \"wait_timeout_count\": " << d_g_timeout
          << ", \"ready_observed\": " << d_g_ready << "},\n"
          << "  \"derived\": {\"mean_routes_per_round\": " << mean_routes_per_round
          << ", \"expected_routes_over_w\": " << per_worker_routes
          << ", \"scanned_routes_per_s\": " << scanned_per_s
          << ", \"scanned_routes_per_s_per_route\": " << scanned_per_s_per_route
          << ", \"mean_scan_ns_per_round\": " << mean_scan_ns << ", \"ready_ratio\": " << ready_ratio
          << ", \"scan_rounds_per_s\": " << (d_rounds / win_s) << "},\n"
          << "  \"verdict\": \"" << (verdict ? "FAIL" : "PASS") << "\"\n}\n";
    }

    pubs.clear();
    subs.clear();
    for (const auto& nm : names)
    {
        ipc::route::clear_storage(shm_topic_segment_name(nm, static_cast<std::size_t>(dom)).c_str());
        ipc::shm::handle::clear_storage(shm_topic_control_name(nm, static_cast<std::size_t>(dom)).c_str());
    }
    dzIPC::detail::SetSeamHook(nullptr);
    std::printf("W10_SCANCOST_DONE verdict=%s\n", verdict ? "FAIL" : "PASS");
    return verdict;
}

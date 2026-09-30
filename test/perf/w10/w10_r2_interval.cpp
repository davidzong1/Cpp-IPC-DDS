/* R2/t42 缺口 3：把 `scan_time_ns_total` 的**观测区间**做成可核对的量。
 *
 * 区间定义（源码级）：`ScanRoundScope` 构造 → 析构之间。在 `collect_pending()` 里即
 *   [routes.size()/deferred.size() 取值] → [遍历] → [scan.set_ready] → [常驻孪生 5 次写] → 析构
 * 故它 **不止**"遍历"：还含 `set_ready` 与常驻孪生写入（t30 新增）。
 *
 * 本探针分别测两段可分离的成本（不改产品代码）：
 *   A. ScanRoundScope 构造+析构本身（两次 clock_gettime + 2 次门控判断）—— W03 已测 ~37.8 ns
 *   B. 常驻孪生那 5 次原子写（3×relaxed fetch_add + 1×store + 1×CAS）
 *   C. 对照：区间内**空体**（只有 scope）⇒ 用来验证 B 的量级
 * 结论用法：报告里 `scan_time_ns_total / scan_rounds` 应写成
 *   「遍历 + set_ready + 常驻孪生(A+B) 的墙钟累计均值」，⛔ 不得换算 CPU core。
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <cstdio>
#include <vector>
#include "dzIPC/measure/counters.h"
using Clock = std::chrono::steady_clock;
static std::atomic<std::uint64_t> g_rounds{0}, g_scanned{0}, g_ready{0}, g_depth{0}, g_depthmax{0};
static inline void resident_twins(std::size_t scanned, bool ready, std::size_t depth)
{
    g_rounds.fetch_add(1, std::memory_order_relaxed);
    g_scanned.fetch_add(scanned, std::memory_order_relaxed);
    if (ready) g_ready.fetch_add(1, std::memory_order_relaxed);
    g_depth.store(depth, std::memory_order_relaxed);
    std::uint64_t prev = g_depthmax.load(std::memory_order_relaxed);
    while (depth > prev && !g_depthmax.compare_exchange_weak(prev, depth, std::memory_order_relaxed)) {}
}
static double now_run_ns(int iters, const std::function<void()>& f)
{
    f(); /* 预热 */
    const auto t0 = Clock::now();
    for (int i = 0; i < iters; ++i) f();
    const auto t1 = Clock::now();
    return std::chrono::duration<double, std::nano>(t1 - t0).count() / iters;
}
int main()
{
    const int iters = 500000;
    auto& reg = dzIPC::measure::CounterRegistry::instance();
    reg.set_diagnostics_enabled(true);
    const double t_scope_on = now_run_ns(iters, [] {
        dzIPC::measure::ScanRoundScope s(31, 0);
        s.set_ready(true);
    });
    reg.set_diagnostics_enabled(false);
    const double t_scope_off = now_run_ns(iters, [] {
        dzIPC::measure::ScanRoundScope s(31, 0);
        s.set_ready(true);
    });
    const double t_resident = now_run_ns(iters, [] { resident_twins(31, true, 2); });
    const double t_empty = now_run_ns(iters, [] {});
    printf("nop_ns=%.3f\n", t_empty);
    printf("scope_only_diag_on_ns=%.3f\n", t_scope_on - t_empty);
    printf("scope_only_diag_off_ns=%.3f\n", t_scope_off - t_empty);
    printf("resident_twins_ns=%.3f\n", t_resident - t_empty);
    printf("interval_diag_on_ns=%.3f  (= scope_only_on + resident_twins)\n",
           t_scope_on - t_empty + t_resident - t_empty);
    printf("interval_diag_off_ns=%.3f (= scope_only_off + resident_twins)\n",
           t_scope_off - t_empty + t_resident - t_empty);
    printf("PROBE_INTERVAL_DONE\n");
    return 0;
}

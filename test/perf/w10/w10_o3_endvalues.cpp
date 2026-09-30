/* W06/t53 —— R0-9/R0-10 结束值接线的**判决性取证**（两个部分，同一二进制）。
 *
 * 背景（t12 实测的零值语义混淆）：t44 落了 `ScanRoundScope::finish()` 与
 * `ScanRoundAccumulator`，但 `src/` 内**无人调用** ⇒ `scan_ready_routes_total` /
 * `deferred_depth_after_*` 在 diag=on 下恒为 0，`0` 被读作"扫描无深度/无就绪"。
 * t53 完成接线后，本工装负责证明：**四量从 0 变为非 0**、**每 worker 出参且全池 = Σ**、
 * **diag=off 是「未采集」而非「深度为 0」**、**守恒判据 Σ elapsed ≈ scan_time_ns_total**。
 *
 * 两个用例：
 *   --case gauge：**判决性反例**（复现 R0-10 的"单个 gauge 不能表达全池总量"）。
 *       直接建 K 个独立 `RecvWorker`（每个自带一个 `ScanRoundAccumulator`），
 *       让其中 ≥2 个同时有扫描后深度：读**逐 worker** 的 `deferred_depth_after_last`
 *       与门控 gauge `deferred_depth_after_last`，断言 Σ(每 worker) > gauge 且
 *       gauge == 最后写入者之一。⛔ 这条只能在"每 worker 出参"成立时通过。
 *   --case scale：**四档规模**（N=1/100/500/1000）×（diag on/off）。走**真实产品路径**
 *       （`shm_pub_sub_ipc` + 进程池），读池级导出与门控计数，判：
 *         · diag=on：四量增量 > 0（接线生效），且 `scan_ready_routes_total ≥
 *           scan_ready_rounds`（route 数口径 ≥ 轮数口径，量纲差异的机械核对）；
 *         · diag=off：四量为 0 且 `counter_is_uncollected()==true` ⇒ 标 **未采集**，
 *           ⛔ 不得当作深度 0；
 *         · 守恒：`scan_elapsed_ns_total ≈ scan_time_ns_total`（同区间，容差见下）。
 *
 * 用法：w10_o3_endvalues --case gauge|scale [--n N] [--diag on|off] [--domain D]
 *                        [--window-ms M] [--out <dir>] [--run-id <id>]
 * 退出码 0 = 通过；1 = 有失败（逐条打印 FAILURE:）。
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

#include "libipc/ipc.h"
#include <vector>

#include "dzIPC/common/name_operator.h"
#include "dzIPC/shm_route_session.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/measure/counters.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/threepools/recv_worker.h"
#include "ipc_msg/std_msgs/std_string.hpp"

using Clock = std::chrono::steady_clock;
static constexpr std::uint32_t kMsgId = 93;

static long arg_long(int argc, char** argv, const char* key, long def)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], key) == 0) return std::strtol(argv[i + 1], nullptr, 10);
    return def;
}
static const char* arg_str(int argc, char** argv, const char* key, const char* def)
{
    for (int i = 1; i + 1 < argc; ++i)
        if (std::strcmp(argv[i], key) == 0) return argv[i + 1];
    return def;
}
static std::shared_ptr<dzIPC::TopicData> td()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId);
}
static std::uint64_t ctr(dzIPC::measure::CounterId id)
{
    return static_cast<std::uint64_t>(dzIPC::measure::CounterRegistry::instance().snapshot().get(id));
}

static int g_failures = 0;
/* 尾部常数（ns/轮）：由 `--case conserve` 在本进程内标定，供 B5b 判据使用。
 * 未标定时为 0 ⇒ B5b 会失败（⛔ 不允许"没标定就过"）。 */
static double g_tail_ns = 0.0;

/* 在**本进程内**标定尾部常数（与接线同形态：构造 → set_ready → finish → add → 析构）。
 * 为什么放在进程内而不是跨进程传参：`scale` 档的每个进程预算/worker 数都不同，尾部
 * 常数虽与规模无关，但"同进程标定 + 同进程判据"少一个可被质疑的跨进程假设。 */
static double calibrate_tail_ns(int iters, double* lib_tail_out)
{
    std::uint64_t elapsed_sum = 0, span_sum = 0;
    for (int i = 0; i < iters; ++i)
    {
        dzIPC::measure::ScanRoundAccumulator acc;
        const auto t0 = Clock::now();
        {
            dzIPC::measure::ScanRoundScope sc(31, 0);
            sc.set_ready(true);
            sc.finish(2, 1);
            acc.add(sc.result());
            elapsed_sum += sc.result().elapsed_ns;
        }
        span_sum += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
    }
    const double span = static_cast<double>(span_sum) / iters;
    const double el = static_cast<double>(elapsed_sum) / iters;
    if (lib_tail_out != nullptr)
    {
        std::uint64_t lib_sum = 0, lib_el_sum = 0;
        for (int i = 0; i < iters; ++i)
        {
            const auto t0 = Clock::now();
            std::uint64_t e = 0;
            {
                dzIPC::measure::ScanRoundScope sc(31, 0);
                sc.set_ready(true);
                sc.finish(2, 1);
                e = sc.result().elapsed_ns;
            }
            lib_sum += static_cast<std::uint64_t>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
            lib_el_sum += e;
        }
        *lib_tail_out = static_cast<double>(lib_sum) / iters - static_cast<double>(lib_el_sum) / iters;
    }
    return span - el;
}
static std::vector<std::string> g_rows;
static void rec(const char* name, bool ok, const std::string& detail)
{
    std::string line = std::string(name) + " " + (ok ? "PASS" : "FAIL") + "  " + detail;
    g_rows.push_back(line);
    std::printf("%-52s %s  %s\n", name, ok ? "PASS" : "FAIL", detail.c_str());
    if (!ok)
    {
        std::printf("FAILURE: %s —— %s\n", name, detail.c_str());
        ++g_failures;
    }
}

/* ---------------------------------------------------------------------------
 * 用例 A：单个 gauge 不能表达全池总量（R0-10 的判决性反例）
 *
 * ⛔ 造法必须**绕开产品路径**：`shm_sub_ipc` 只能把 route 注册进进程级单例
 * `RecvWorkerPool`（哈希归属，无法人为让某几个 worker 同时有深度），而池一启动就是
 * 32 个 worker。本用例直接建 K 个**独立** `RecvWorker`（各自一份
 * `ScanRoundAccumulator`）并给每个 worker 注册本地 route，使 K 个 worker 在**同一
 * 测量点**各自都有"扫描后深度" ⇒ 才能判决"Σ 每 worker" 与"单个 gauge"的差别。
 *
 * 本地 route 的行为：`recv_once()` 恒返回 0 且 `has_pending()` 恒真 ⇒ 该 worker 每轮
 * 都会把这条 route 重新入 deferred（`run_budget` 的 `more=true` 分支），于是"扫描后
 * 深度"持续 ≥1 —— 这是制造"多 worker 同时有深度"的最短路径。
 * ------------------------------------------------------------------------- */
class O3StubbornRoute final : public dzIPC::threepools::RecvRouteSource
{
public:
    explicit O3StubbornRoute(const std::string& nm)
        : name_(nm)
        , create_([this] { return std::make_shared<ipc::route>(name_.c_str(), ipc::receiver, false); })
    {
        session_.begin_rebuild(1, create_);
    }
    const char* route_name() const noexcept override { return name_.c_str(); }
    std::uint32_t domain_id() const noexcept override { return 0; }
    ipc::recv_wait_token read_wait_token() const noexcept override
    {
        const auto r = session_.current_route();
        return r ? r->read_wait_token() : ipc::recv_wait_token{};
    }
    std::size_t recv_once() override
    {
        /* 恒"无数据"：数据不会被消费 ⇒ 该 route 每轮都被重新入队（制造持续深度）。 */
        std::this_thread::sleep_for(std::chrono::microseconds(200));
        return 0;
    }
    bool has_pending() const noexcept override { return true; }
    dzIPC::threepools::RecvOwner recv_owner() const noexcept override
    {
        return owner_.load(std::memory_order_acquire);
    }
    bool try_claim_recv(dzIPC::threepools::RecvOwner who) noexcept override
    {
        auto e = dzIPC::threepools::RecvOwner::none;
        return owner_.compare_exchange_strong(e, who, std::memory_order_acq_rel);
    }
    void release_recv() noexcept override { owner_.store(dzIPC::threepools::RecvOwner::none); }
    void stop_and_wake() noexcept override { session_.stop_and_wake(); }
    void wait_quiescent() noexcept override { session_.wait_quiescent(); }
    const std::string& name() const { return name_; }

private:
    std::string name_;
    std::function<std::shared_ptr<ipc::route>()> create_;
    dzIPC::shm::RouteSession session_;
    std::atomic<dzIPC::threepools::RecvOwner> owner_{dzIPC::threepools::RecvOwner::none};
};

int run_gauge(long dom)
{
    (void)dom;
    dzIPC::measure::CounterRegistry::instance().set_diagnostics_enabled(true);
    const int K = 3;
    const int ROUTES_PER_WORKER = 2;

    std::vector<std::unique_ptr<dzIPC::threepools::RecvWorker>> workers;
    std::vector<std::vector<std::shared_ptr<O3StubbornRoute>>> routes(K);
    for (int w = 0; w < K; ++w)
    {
        dzIPC::threepools::RecvBudget b{};
        b.max_messages_per_route = 1;
        b.max_bytes_per_route = 1u << 20;
        b.max_processing_time_per_route = std::chrono::microseconds{200000};
        b.wait_timeout = std::chrono::milliseconds{50};
        b.idle_keep_alive = std::chrono::milliseconds{60000};
        workers.emplace_back(new dzIPC::threepools::RecvWorker(static_cast<std::size_t>(w), b));
        workers.back()->start();
    }
    for (int w = 0; w < K; ++w)
    {
        for (int r = 0; r < ROUTES_PER_WORKER; ++r)
        {
            auto src = std::make_shared<O3StubbornRoute>("o3g_" + std::to_string(w) + "_" + std::to_string(r));
            const auto st = workers[static_cast<std::size_t>(w)]->add_route(src);
            if (st != dzIPC::threepools::RecvRegisterStatus::ok)
                std::printf("WARN add_route(%d,%d) status=%d\n", w, r, static_cast<int>(st));
            routes[static_cast<std::size_t>(w)].push_back(src);   /* worker 侧持 shared_ptr，这里再留一份用于取名字 */
        }
    }
    std::size_t total = 0;
    for (auto& w : workers) total += w->route_count();
    std::printf("workers=%d routes_per_worker=%d attached_to_workers=%zu\n", K, ROUTES_PER_WORKER, total);
    rec("A0 前提：每 worker 都接上了 route", total == static_cast<std::size_t>(K * ROUTES_PER_WORKER),
        "Σ route_count=" + std::to_string(total) + "（期望 " + std::to_string(K * ROUTES_PER_WORKER) + "）");

    /* 让每 worker 都至少跑几轮，使"扫描后深度"在各 worker 上稳定出现。 */
    std::this_thread::sleep_for(std::chrono::milliseconds(2500));

    /* 逐 worker 出参（**R0-10 要的形态**）。 */
    std::vector<std::uint64_t> per_last, per_total, per_ready;
    std::uint64_t sum_last = 0, sum_total = 0, sum_ready = 0, max_last = 0;
    for (auto& w : workers)
    {
        const auto s = w->stats();
        per_last.push_back(s.deferred_depth_after_last);
        per_total.push_back(s.deferred_depth_after_total);
        per_ready.push_back(s.scan_ready_routes_total);
        sum_last += s.deferred_depth_after_last;
        sum_total += s.deferred_depth_after_total;
        sum_ready += s.scan_ready_routes_total;
        if (s.deferred_depth_after_last > max_last) max_last = s.deferred_depth_after_last;
    }
    const std::uint64_t gauge_last = ctr(dzIPC::measure::CounterId::deferred_depth_after_last);
    const std::uint64_t gauge_total = ctr(dzIPC::measure::CounterId::deferred_depth_after_total);
    const std::uint64_t gauge_ready = ctr(dzIPC::measure::CounterId::scan_ready_routes_total);

    std::string pw;
    for (std::size_t i = 0; i < per_last.size(); ++i)
        pw += "w" + std::to_string(i) + "=" + std::to_string(per_last[i]) + " ";
    const int nonzero = static_cast<int>(std::count_if(per_last.begin(), per_last.end(),
                                                       [](std::uint64_t v) { return v > 0; }));

    rec("A1 每 worker 有独立出参（≥2 个 worker 同时有深度）", nonzero >= 2,
        "per-worker deferred_depth_after_last: " + pw + "（非零 worker 数=" + std::to_string(nonzero) + "）");
    rec("A2 ⛔ 单个 gauge 表达不了全池总量（Σ 每 worker > gauge）", sum_last > gauge_last,
        "Σ 每 worker=" + std::to_string(sum_last) + " > 门控 gauge=" + std::to_string(gauge_last) +
            "（gauge 最多等于某一个 worker 的值；单 worker 最大=" + std::to_string(max_last) + "）");
    rec("A3 gauge <= 单 worker 最大值（它只是最后写入者）", gauge_last <= max_last,
        "gauge=" + std::to_string(gauge_last) + " ≤ max=" + std::to_string(max_last));
    rec("A4 累计量可安全求和（Σ 每 worker _total ≥ gauge _total 的任一单点值）",
        sum_total >= gauge_total && sum_total > 0,
        "Σ 每 worker deferred_depth_after_total=" + std::to_string(sum_total) +
            "；门控 _total=" + std::to_string(gauge_total));
    /* A5 的期望值必须是**语义正确**的那个，而不是"越大越好"：本用例的 route 靠
     * `run_budget` 的 requeue 留在 deferred 里（`entry->queued` 仍为 true），**不由**
     * `collect_pending()` 本轮新入队 ⇒ `scan_ready_routes_total` 正确结果就是 **0**，
     * 而 `deferred_depth_after_*` 非零。这恰好是一条"两量语义不同、⛔ 不得互相替代"的
     * 机械核对（真实新消息到达时的 >0 由用例 B 的四档给出）。
     * ⛔ 若把这条写成"必须 >0"，就是在用错误的期望值掩盖语义。 */
    rec("A5 ready_routes 与 depth_after 语义可分（本场景 ready_routes 正确为 0、depth>0）",
        sum_ready == 0 && gauge_ready == 0 && sum_last > 0,
        "Σ scan_ready_routes_total=" + std::to_string(sum_ready) + "（正确：route 靠 requeue 续留，"
        "非本轮新入队）、Σ deferred_depth_after_last=" + std::to_string(sum_last) +
            " ⇒ 两量不同源、不得互相替代");

    /* 清理顺序（与 test_recv_worker 的 RouteName 同规）：先让 route 对象析构（disconnect），
     * 再清段。反过来会撞上"route 析构时再 disconnect 已被清掉的段"。 */
    for (auto& w : workers) w->stop();
    workers.clear();
    routes.clear();
    for (int w = 0; w < K; ++w)
        for (int r = 0; r < ROUTES_PER_WORKER; ++r)
        {
            const std::string nm = "o3g_" + std::to_string(w) + "_" + std::to_string(r);
            ipc::route::clear_storage(nm.c_str());
        }
    return g_failures;
}

/* ---------------------------------------------------------------------------
 * 用例 B：四档规模（N=1/100/500/1000）× diag on/off —— 走真实产品路径
 * ------------------------------------------------------------------------- */
struct PoolSnap
{
    std::uint64_t scan_rounds{0}, scanned_routes_total{0}, scan_ready_rounds{0};
    std::uint64_t deferred_depth_last{0}, deferred_depth_max{0};
    std::uint64_t ready_routes_total{0}, after_last{0}, after_max{0}, after_total{0};
    std::uint64_t elapsed_ns_total{0};
    std::uint64_t wait_wakeups{0}, wait_timeouts{0};
    std::size_t route_count{0}, workers{0};
};

static PoolSnap snap_pool()
{
    const auto& pool = dzIPC::threepools::RecvWorkerPool::instance();
    const auto s = pool.stats();
    PoolSnap p;
    p.scan_rounds = s.scan_rounds;
    p.scanned_routes_total = s.scanned_routes_total;
    p.scan_ready_rounds = s.scan_ready_rounds;
    p.deferred_depth_last = s.deferred_depth_last;
    p.deferred_depth_max = s.deferred_depth_max;
    p.ready_routes_total = s.scan_ready_routes_total;
    p.after_last = s.deferred_depth_after_last;
    p.after_max = s.deferred_depth_after_max;
    p.after_total = s.deferred_depth_after_total;
    p.elapsed_ns_total = s.scan_elapsed_ns_total;
    p.wait_wakeups = s.wait_wakeups;
    p.wait_timeouts = s.wait_timeouts;
    p.route_count = s.route_count;
    p.workers = pool.worker_count();
    return p;
}

int run_scale(long dom, long n, bool diag, long window_ms, const std::string& out, const std::string& run_id)
{
    dzIPC::measure::CounterRegistry::instance().set_diagnostics_enabled(diag);
    /* 先标定尾部常数（诊断开启才有意义；关闭档 elapsed 恒 0，标定值只作参考）。
     * 取 5 轮**最小值**：尾部是"下界性质"的量（调度抖动只会把它抬高），最小值最接近
     * 真实固定开销；同时打印极差作为离散度，避免把噪声当常数。 */
    {
        double best = 1e18, worst = 0.0, lib_first = 0.0;
        for (int rep = 0; rep < 5; ++rep)
        {
            double lib_tail = 0.0;
            const double t = calibrate_tail_ns(200000, &lib_tail);
            if (rep == 0) lib_first = lib_tail;
            if (t < best) best = t;
            if (t > worst) worst = t;
        }
        g_tail_ns = best;
        std::printf("tail_calib_ns_per_round=%.2f (min of 5; range=[%.2f, %.2f]; lib_dtor_only(rep1)=%.2f)\n",
                    best, best, worst, lib_first);
    }
    auto& pool = dzIPC::threepools::RecvWorkerPool::instance();
    dzIPC::threepools::RecvBudget b{};
    b.wait_timeout = std::chrono::milliseconds{100};
    b.idle_keep_alive = std::chrono::milliseconds{60000};
    if (!pool.start(4, b)) std::printf("NOTE pool already started (workers=%zu)\n", pool.worker_count());

    std::vector<std::unique_ptr<dzIPC::shm::shm_pub_ipc>> pubs;
    std::vector<std::unique_ptr<dzIPC::shm::shm_sub_ipc>> subs;
    std::vector<std::string> names;
    for (long i = 0; i < n; ++i)
    {
        names.push_back("o3s_" + std::to_string(dom) + "_" + std::to_string(i));
        pubs.emplace_back(new dzIPC::shm::shm_pub_ipc(td(), names.back(), static_cast<std::size_t>(dom), false));
        pubs.back()->InitChannel("o3");
        subs.emplace_back(new dzIPC::shm::shm_sub_ipc(td(), names.back(), static_cast<std::size_t>(dom), 16, false));
        subs.back()->InitChannel("o3");
    }
    std::size_t attached = 0;
    {
        const auto dl = Clock::now() + std::chrono::seconds(120);
        while (Clock::now() < dl)
        {
            attached = 0;
            for (long i = 0; i < n; ++i)
            {
                dzIPC::control_plane_shm::TopicControlPlane cp;
                if (cp.open(shm_topic_control_name(names[static_cast<std::size_t>(i)], static_cast<std::size_t>(dom)))
                    && cp.peer_count() >= 1) ++attached;
            }
            if (attached == static_cast<std::size_t>(n)) break;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    /* 让每条 route 都进池（worker 臂）；容量够（4×127=508 ≥ n 当 n≤500）。 */
    {
        const auto dl = Clock::now() + std::chrono::seconds(30);
        while (Clock::now() < dl && pool.route_count() < static_cast<std::size_t>(std::min<long>(n, 508)))
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }

    /* 窗口内不断有真实就绪（后台轮转发布），否则 ready_routes 恒 0、判据没有牙。 */
    std::atomic<bool> stop{false};
    std::atomic<long> sent{0};
    std::thread feeder([&] {
        long i = 0;
        while (!stop.load(std::memory_order_relaxed))
        {
            auto m = std::make_shared<dzIPC::Msg::StdString>();
            m->set_msg_id(kMsgId);
            m->str = "F" + std::to_string(i);
            if (pubs[static_cast<std::size_t>(i % n)]->publish(m)) sent.fetch_add(1);
            auto sink = td();
            (void)subs[static_cast<std::size_t>(i % n)]->try_get_clone(sink);
            ++i;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    });

    const auto a = snap_pool();
    const std::uint64_t g_rounds0 = ctr(dzIPC::measure::CounterId::scan_rounds);
    const std::uint64_t g_readyobs0 = ctr(dzIPC::measure::CounterId::ready_observed);
    const std::uint64_t g_time0 = ctr(dzIPC::measure::CounterId::scan_time_ns_total);
    const std::uint64_t g_after0 = ctr(dzIPC::measure::CounterId::deferred_depth_after_total);
    const std::uint64_t g_aftermax0 = ctr(dzIPC::measure::CounterId::deferred_depth_after_max);
    const std::uint64_t g_ready0 = ctr(dzIPC::measure::CounterId::scan_ready_routes_total);
    const auto w0 = Clock::now();
    std::this_thread::sleep_for(std::chrono::milliseconds(window_ms));
    const double win_s = std::chrono::duration<double>(Clock::now() - w0).count();
    const std::uint64_t g_time1 = ctr(dzIPC::measure::CounterId::scan_time_ns_total);
    const std::uint64_t g_after1 = ctr(dzIPC::measure::CounterId::deferred_depth_after_total);
    const std::uint64_t g_aftermax1 = ctr(dzIPC::measure::CounterId::deferred_depth_after_max);
    const std::uint64_t g_ready1 = ctr(dzIPC::measure::CounterId::scan_ready_routes_total);
    const auto z = snap_pool();
    stop.store(true);
    feeder.join();

    const std::uint64_t d_rounds = z.scan_rounds - a.scan_rounds;
    const std::uint64_t d_ready_rounds = z.scan_ready_rounds - a.scan_ready_rounds;
    const std::uint64_t d_ready_routes = z.ready_routes_total - a.ready_routes_total;
    const std::uint64_t d_after_total = z.after_total - a.after_total;
    const std::uint64_t d_elapsed = z.elapsed_ns_total - a.elapsed_ns_total;
    const std::uint64_t d_g_time = g_time1 - g_time0;
    const std::uint64_t d_g_after = g_after1 - g_after0;
    const std::uint64_t d_g_ready = g_ready1 - g_ready0;
    (void)g_aftermax0;
    (void)g_aftermax1;

    std::printf("scale n=%ld workers=%zu diag=%s attached=%zu route_count=%zu window_s=%.3f sent=%ld\n", n,
                z.workers, diag ? "on" : "off", attached, z.route_count, win_s, sent.load());
    std::printf("  resident rounds=%llu ready_rounds=%llu ready_routes=%llu after_last=%llu after_max=%llu "
                "after_total=%llu elapsed_ns=%llu\n",
                (unsigned long long)d_rounds, (unsigned long long)d_ready_rounds,
                (unsigned long long)d_ready_routes, (unsigned long long)z.after_last,
                (unsigned long long)z.after_max, (unsigned long long)d_after_total,
                (unsigned long long)d_elapsed);
    /* ⛔ 全部用**窗口增量**（不是进程累计）—— 否则与常驻面不可比。 */
    std::printf("  gated    rounds=%llu ready_observed_delta=%llu ready_routes=%llu after_total=%llu time_ns=%llu\n",
                (unsigned long long)(ctr(dzIPC::measure::CounterId::scan_rounds) - g_rounds0),
                (unsigned long long)(ctr(dzIPC::measure::CounterId::ready_observed) - g_readyobs0),
                (unsigned long long)d_g_ready, (unsigned long long)d_g_after, (unsigned long long)d_g_time);
    const double conserve = d_g_time > 0 ? static_cast<double>(d_elapsed) / static_cast<double>(d_g_time) : 0.0;
    std::printf("  conservation Σelapsed/scan_time=%.4f\n", conserve);

    const bool uncollected = dzIPC::measure::counter_is_uncollected(
        dzIPC::measure::CounterId::scan_ready_routes_total, diag);
    const std::string tag = "n=" + std::to_string(n) + (diag ? " diag=on" : " diag=off");

    if (diag)
    {
        rec(("B1 四量 0→非 0（scan_ready_routes_total） " + tag).c_str(),
            d_ready_routes > 0 && d_g_ready > 0,
            "常驻 Δ=" + std::to_string(d_ready_routes) + "、门控 Δ=" + std::to_string(d_g_ready) +
                "（接线前两者恒 0）");
        rec(("B2 四量 0→非 0（deferred_depth_after_total） " + tag).c_str(),
            d_after_total > 0 && d_g_after > 0 && d_after_total == d_g_after,
            "常驻 Δ=" + std::to_string(d_after_total) + "、门控 Δ=" + std::to_string(d_g_after) +
                "（同源 ⇒ 必须逐值相等）");
        /* ⚠️ 池级两个量的**定义不同**，⛔ 不能互相比较大小：
         *   · `after_last`（池级）= **Σ 每 worker 的"最近一轮扫描后深度"** = 全池**当前**总深度；
         *   · `after_max`（池级）= **各 worker 峰值中的最大**（单个 worker 的历史最深）。
         * 多个 worker 各持 1 时 Σ=4 而 max=1，两者都对。可比的只有：
         *   `after_total`（逐轮 Σ 累计）≥ `after_last`（当前 Σ）与 ≥ `after_max`。 */
        rec(("B3 池级 after_last=Σ每worker（当前总深度）、after_max=单worker峰值（定义不同） " + tag).c_str(),
            z.after_last >= 1 && z.after_max >= 1 && d_after_total >= z.after_last,
            "after_last(Σ每worker)=" + std::to_string(z.after_last) + "、after_max(单worker峰值)=" +
                std::to_string(z.after_max) + "、Δafter_total=" + std::to_string(d_after_total) +
                "（⛔ 二者定义不同，不得互相比较大小）");
        rec(("B4 量纲机械核对：Σ route 数 ≥ 就绪轮数 " + tag).c_str(),
            d_ready_routes >= d_ready_rounds,
            "Δscan_ready_routes_total=" + std::to_string(d_ready_routes) + " ≥ Δscan_ready_rounds=" +
                std::to_string(d_ready_rounds) + "（route 数口径 vs 轮数口径，⛔ 不得相加）");
        /* ⚠️ 「守恒」的**准确形式**（本轮实测把 t44 的 "≈" 精确化，⛔ 不放宽容差蒙过）：
         *   · 两者**同起点**（`ScanRoundScope` 构造读的那一次钟）；
         *   · 终点不同：`elapsed_ns` 止于 `finish()` 内的读钟，`scan_time_ns_total` 止于**析构**读钟；
         *   ⇒ `scan_time - Σelapsed` = 「finish 之后 → 析构读钟」这段**每轮固定**的尾部
         *     （本层接线为 `scan_acc_.add()` + 返回/展开 + 析构前几条语句）。
         * 判据因此是两条**可机械核对**的：
         *   (i) 严格序 `Σelapsed ≤ scan_time`（同起点、终点更早 ⇒ 必为真；违反即接线位置错）；
         *   (ii) 残差是**每轮常数**：`(scan_time − Σelapsed) / rounds` 必须落在同 run 标定的
         *        尾部常数 `tail_ns` 的 [0.2×, 5×] 内（本 run 由 `--case conserve` 标定）。
         * ⛔ 报告里引用 `scan_time_ns_total` 时必须同时给这条残差，不得当作"纯扫描耗时"。 */
        const double resid_per_round = d_rounds > 0
            ? static_cast<double>(d_g_time - d_elapsed) / static_cast<double>(d_rounds) : 0.0;
        rec(("B5a 守恒(严格序)：Σ elapsed_ns ≤ scan_time_ns_total " + tag).c_str(),
            d_g_time > 0 && d_elapsed > 0 && d_elapsed <= d_g_time,
            "Σelapsed=" + std::to_string(d_elapsed) + " ≤ scan_time=" + std::to_string(d_g_time) +
                "，比值=" + std::to_string(conserve).substr(0, 6));
        rec(("B5b 守恒(残差=每轮固定尾部)：0 < 残差/轮 ∈ [0.25×,4×] 标定值 且 < 500 ns/轮 " + tag).c_str(),
            g_tail_ns > 0 && resid_per_round > 0 && resid_per_round >= 0.25 * g_tail_ns &&
                resid_per_round <= 4.0 * g_tail_ns && resid_per_round < 500.0,
            "残差/轮=" + std::to_string(resid_per_round).substr(0, 6) + " ns、标定尾部(5 轮取min)=" +
                std::to_string(g_tail_ns).substr(0, 6) + " ns（⛔ 该残差是【finish 之后→析构读钟】的固定尾部，不属于扫描）");
    }
    else
    {
        /* ⛔ 「关闭诊断 ⇒ 未采集」这一条判在**门控面**上（四个 `CounterId` 都
         * diagnostics_only=true，这才是"四量属 diagnostics_only"的字面所指）。
         * 常驻孪生（`RecvWorkerStats` 追加字段）是**另一条面**（与 t30 的常驻/门控分工
         * 同构），它不受门控、off 档仍可读 —— 单独一条断言，⛔ 不与门控四量混列。 */
        rec(("B1' 关闭诊断：**门控**四量为 0 且 counter_is_uncollected()=true（未采集） " + tag).c_str(),
            d_g_ready == 0 && d_g_after == 0 && d_g_time == 0 && uncollected &&
                d_rounds > 0 /* 同窗口确实在扫 ⇒ 0 不是"没扫" */,
            "门控 Δready_routes=0 Δafter_total=0 Δtime=0 且 counter_is_uncollected()=true ⇒ 标 **未采集**；"
            "同一窗口常驻扫描量仍非零（Δrounds=" + std::to_string(d_rounds) + "）⇒ 证明 0 不是【没扫】");
        rec(("B1'' 关闭诊断：常驻孪生（另一条面）仍可读且非零 " + tag).c_str(),
            d_ready_routes > 0 && d_after_total > 0,
            "常驻 Δready_routes=" + std::to_string(d_ready_routes) + " Δafter_total=" +
                std::to_string(d_after_total) + "（无时钟、不受门控；⛔ 与门控四量是两条面，不得混列）");
        rec(("B2' 关闭诊断：elapsed 亦为未采集（0），且不读时钟 " + tag).c_str(),
            d_elapsed == 0 && d_g_time == 0,
            "Σelapsed=" + std::to_string(d_elapsed) + " scan_time=" + std::to_string(d_g_time) +
                "（0 = 未采集；t44 已把 clock_gettime 完全置于门控内）");
    }

    if (!out.empty())
    {
        std::error_code ec;
        std::filesystem::create_directories(out, ec);
        /* ⛔ 每档是一个**独立进程** ⇒ `static bool header` 在每个进程里都是 false，
         * 会把表头重复写多次（t53 首轮实测：4 档 × 2 臂 = 8 份表头）。改为按**文件是否已存在**
         * 判定，跨进程正确。 */
        const std::string csv = out + "/scale_readings.csv";
        const bool need_header = !std::filesystem::exists(csv);
        std::ofstream f(csv, std::ios::app);
        if (need_header)
        {
            f << "run_id,n,diag,workers,route_count,window_s,attached,sent,resident_rounds,ready_rounds,"
                 "ready_routes,after_last,after_max,after_total,elapsed_ns,gated_ready_routes,"
                 "gated_after_total,gated_time_ns,conservation\n";
        }
        f << run_id << "," << n << "," << (diag ? "on" : "off") << "," << z.workers << "," << z.route_count << ","
          << win_s << "," << attached << "," << sent.load() << "," << d_rounds << "," << d_ready_rounds << ","
          << d_ready_routes << "," << z.after_last << "," << z.after_max << "," << d_after_total << ","
          << d_elapsed << "," << d_g_ready << "," << d_g_after << "," << d_g_time << "," << conserve << "\n";
    }

    subs.clear();
    pubs.clear();
    for (const auto& nm : names)
    {
        ipc::route::clear_storage(shm_topic_segment_name(nm, static_cast<std::size_t>(dom)).c_str());
        ipc::shm::handle::clear_storage(shm_topic_control_name(nm, static_cast<std::size_t>(dom)).c_str());
    }
    return g_failures;
}

/* ---------------------------------------------------------------------------
 * 用例 C：**尾部常数标定**（conservation 判据的输入）
 *
 * 为什么必须单独标定：`elapsed_ns`（finish 读钟）与 `scan_time_ns_total`（析构读钟）
 * 同起点、终点不同，差值 = 「finish 之后 → 析构读钟」的每轮固定尾部。守恒判据
 * 必须把这段**不属于扫描**的开销显式扣掉，而不是放宽容差把它算进"扫描耗时"。
 * 标定与接线**同形态**（构造 → set_ready → finish → add → 析构），100 万次取均值。
 * ------------------------------------------------------------------------- */
static int run_conserve(const std::string& out, const std::string& run_id)
{
    dzIPC::measure::CounterRegistry::instance().set_diagnostics_enabled(true);
    const int IT = 1000000;
    double lib_tail = 0.0;
    g_tail_ns = calibrate_tail_ns(IT, &lib_tail);
    /* 单独取一次 elapsed 均值（口径与判据一致）。 */
    std::uint64_t e_sum = 0, s_sum = 0;
    for (int i = 0; i < IT / 4; ++i)
    {
        const auto t0 = Clock::now();
        std::uint64_t e = 0;
        {
            dzIPC::measure::ScanRoundScope sc(31, 0);
            sc.set_ready(true);
            sc.finish(2, 1);
            e = sc.result().elapsed_ns;
        }
        e_sum += e;
        s_sum += static_cast<std::uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - t0).count());
    }
    const double elapsed_ns = static_cast<double>(e_sum) / (IT / 4);
    const double span_ns = static_cast<double>(s_sum) / (IT / 4);
    /* 口径说明（避免把不同环路的量混印）：`elapsed_ns` 与 `span_ns` 同环路 ⇒
     * `g_tail_ns = span − elapsed` 是该环路的尾部；`lib_tail` 由**去掉 acc.add** 的环路
     * 单独测出；两者之差就是接线侧 `acc.add()` 的贡献。 */
    std::printf("conserve: elapsed_ns/round=%.2f  tail_with_wiring=%.2f  tail_lib_only=%.2f  "
                "wiring_add=%.2f  (span_lib_loop=%.2f)\n",
                elapsed_ns, g_tail_ns, lib_tail, g_tail_ns - lib_tail, span_ns);
    rec("C1 尾部常数可标定且为正（finish→析构确有开销，必须从守恒里扣除）", g_tail_ns > 0.0,
        "tail=" + std::to_string(g_tail_ns).substr(0, 6) + " ns/轮（其中纯库析构=" +
            std::to_string(lib_tail).substr(0, 6) + "、接线侧 acc.add=" +
            std::to_string(g_tail_ns - lib_tail).substr(0, 6) + "）");
    rec("C2 elapsed 区间读数非零（诊断开启时才采集）", elapsed_ns > 0.0,
        "elapsed_ns/round=" + std::to_string(elapsed_ns).substr(0, 6));
    if (!out.empty())
    {
        std::ofstream f(out + "/conserve_tail.json");
        f << "{\n  \"run_id\": \"" << run_id << "\",\n  \"iterations\": " << IT
          << ",\n  \"elapsed_ns_per_round\": " << elapsed_ns << ",\n  \"span_ns_per_round\": " << span_ns
          << ",\n  \"tail_ns_per_round\": " << g_tail_ns << ",\n  \"lib_dtor_tail_ns_per_round\": "
          << lib_tail << ",\n  \"wiring_add_contribution_ns_per_round\": " << (g_tail_ns - lib_tail)
          << "\n}\n";
    }
    return g_failures;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string kase = arg_str(argc, argv, "--case", "gauge");
    const long dom = arg_long(argc, argv, "--domain", 9000);
    const long n = arg_long(argc, argv, "--n", 100);
    const long window_ms = arg_long(argc, argv, "--window-ms", 4000);
    const bool diag = std::strcmp(arg_str(argc, argv, "--diag", "on"), "on") == 0;
    const std::string out = arg_str(argc, argv, "--out", "");
    const std::string run_id = arg_str(argc, argv, "--run-id", "o3");
    int rc = 0;
    if (kase == "gauge")
    {
        rc = run_gauge(dom);
    }
    else if (kase == "conserve")
    {
        rc = run_conserve(out, run_id);
    }
    else
    {
        rc = run_scale(dom, n, diag, window_ms, out, run_id);
    }
    if (!out.empty())
    {
        std::ofstream f(out + "/cases.csv", std::ios::app);
        for (const auto& r : g_rows) f << "\"" << r << "\"\n";
    }
    std::printf("W10_O3_ENDVALUES_DONE case=%s verdict=%s failures=%d\n", kase.c_str(),
                g_failures ? "FAIL" : "PASS", g_failures);
    return rc ? 1 : 0;
}

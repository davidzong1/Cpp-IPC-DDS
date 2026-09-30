/* W10-R5 最小验证集（t38 / 方案 §6.2 八个用例）。
 *
 * 方案 §6.2 原文要求的最小验证包含：
 *   ① 无 route            ② 固定 route 无消息       ③ 一次已知就绪
 *   ④ 预算耗尽重入 deferred ⑤ 断开/注销             ⑥ 假就绪
 *   ⑦ 后端错误            ⑧ 诊断开关前后对照
 * 并要求「断言范围、单调性和池统计一致性；普通并发运行不要求调度相关计数精确等于某个常数」。
 *
 * ⛔ 本工装**不修改**任何产品代码；对无法在 SHM 侧构成的形态（⑥ 假就绪、⑦ 后端错误）
 *    如实登记「不适用 / 无法构成」并给出理由，⛔ 不用其它形态冒充。
 *
 * 用法：
 *   w10_r5_minimal [--case 1..8|all] [--domain 8800] [--out <dir>] [--run-id <id>]
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "dzIPC/common/name_operator.h"
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

struct Resident
{
    std::uint64_t scan_rounds{0}, scanned_routes_total{0}, scan_ready_rounds{0};
    std::uint64_t deferred_depth_last{0}, deferred_depth_max{0};
    std::uint64_t wait_timeouts{0}, wait_wakeups{0}, budget_yields{0}, deferred_drains{0};
    std::uint64_t recv_once_calls{0}, recv_errors{0}, idle_exits{0}, thread_restarts{0};
    std::uint64_t messages_received{0};
};
static Resident snap()
{
    Resident r;
    const auto st = dzIPC::threepools::RecvWorkerPool::instance().stats();
    r.scan_rounds = st.scan_rounds;
    r.scanned_routes_total = st.scanned_routes_total;
    r.scan_ready_rounds = st.scan_ready_rounds;
    r.deferred_depth_last = st.deferred_depth_last;
    r.deferred_depth_max = st.deferred_depth_max;
    r.wait_timeouts = st.wait_timeouts;
    r.wait_wakeups = st.wait_wakeups;
    r.budget_yields = st.budget_yields;
    r.deferred_drains = st.deferred_drains;
    r.recv_once_calls = st.recv_once_calls;
    r.recv_errors = st.recv_errors;
    r.idle_exits = st.idle_exits;
    r.thread_restarts = st.thread_restarts;
    r.messages_received = st.messages_received;
    return r;
}

struct Gated
{
    std::uint64_t scan_rounds{0}, scanned_routes_total{0}, scan_time_ns_total{0};
    std::uint64_t ready_observed{0}, wait_timeout_count{0};
};
static Gated snapg()
{
    Gated g;
    const auto s = dzIPC::measure::CounterRegistry::instance().snapshot();
    const auto v = [&](dzIPC::measure::CounterId id) { return static_cast<std::uint64_t>(s.get(id)); };
    g.scan_rounds = v(dzIPC::measure::CounterId::scan_rounds);
    g.scanned_routes_total = v(dzIPC::measure::CounterId::scanned_routes_total);
    g.scan_time_ns_total = v(dzIPC::measure::CounterId::scan_time_ns_total);
    g.ready_observed = v(dzIPC::measure::CounterId::ready_observed);
    g.wait_timeout_count = v(dzIPC::measure::CounterId::wait_timeout_count);
    return g;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string want_case = arg_str(argc, argv, "--case", "all");
    const long dom = arg_long(argc, argv, "--domain", 8800);
    const std::string out = arg_str(argc, argv, "--out", "");
    const std::string run_id = arg_str(argc, argv, "--run-id", "r5min");
    bool all = (want_case == "all");
    auto want = [&](int c) { return all || want_case == std::to_string(c); };

    auto td = [] { return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId); };
    std::vector<std::string> rows;   /* "case,result,detail" */
    int failures = 0;
    auto rec = [&](int c, const char* name, bool ok, const std::string& detail) {
        char b[1024];
        std::snprintf(b, sizeof b, "%d,\"%s\",%s,\"%s\"", c, name, ok ? "PASS" : "FAIL", detail.c_str());
        rows.push_back(b);
        std::printf("case%d %-28s %s  %s\n", c, name, ok ? "PASS" : "FAIL", detail.c_str());
        if (!ok) ++failures;
    };
    auto note = [&](int c, const char* name, const std::string& detail) {
        char b[1024];
        std::snprintf(b, sizeof b, "%d,\"%s\",N/A,\"%s\"", c, name, detail.c_str());
        rows.push_back(b);
        std::printf("case%d %-28s N/A   %s\n", c, name, detail.c_str());
    };

    auto& pool = dzIPC::threepools::RecvWorkerPool::instance();
    /* 本用例只需要少量 worker（不构成规模结论，只验计数语义）。 */
    if (!pool.start(4, dzIPC::threepools::RecvBudget{}))
        std::printf("NOTE pool already started workers=%zu\n", pool.worker_count());
    const std::size_t W = pool.worker_count();

    /* ---------------- 用例 1：无 route（池在跑但 route 表为空） ---------------- */
    if (want(1))
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        const auto a = snap();
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        const auto b = snap();
        const std::uint64_t d_rounds = b.scan_rounds - a.scan_rounds;
        const std::uint64_t d_scan = b.scanned_routes_total - a.scanned_routes_total;
        const bool ok = (pool.route_count() == 0) && (d_rounds > 0) && (d_scan == 0) && (b.scan_ready_rounds == 0);
        char d[256];
        std::snprintf(d, sizeof d, "route_count=0；Δscan_rounds=%llu（>0 说明空表也在按轮扫描）、Δscanned=%llu（应为 0）、"
                                   "scan_ready_rounds=%llu（应为 0）",
                      (unsigned long long)d_rounds, (unsigned long long)d_scan,
                      (unsigned long long)b.scan_ready_rounds);
        rec(1, "无 route（空表轮扫描）", ok, d);
    }

    /* ---------------- 用例 2：固定 route 无消息 ---------------- */
    std::vector<std::unique_ptr<dzIPC::shm::shm_sub_ipc>> subs;
    std::vector<std::unique_ptr<dzIPC::shm::shm_pub_ipc>> pubs;
    const long N2 = 10;
    std::vector<std::string> names2;
    for (long i = 0; i < N2; ++i) names2.push_back("r5min_" + std::to_string(dom + 1) + "_" + std::to_string(i));
    if (want(2))
    {
        for (long i = 0; i < N2; ++i)
        {
            pubs.emplace_back(new dzIPC::shm::shm_pub_ipc(td(), names2[static_cast<std::size_t>(i)],
                                                          static_cast<std::size_t>(dom + 1), false));
            pubs.back()->InitChannel("mn");
            subs.emplace_back(new dzIPC::shm::shm_sub_ipc(td(), names2[static_cast<std::size_t>(i)],
                                                          static_cast<std::size_t>(dom + 1), 64, false));
            subs.back()->InitChannel("mn");
        }
        /* 等注册落到池里（控制面 tick 10 ms 触发归属）。 */
        const auto dl = Clock::now() + std::chrono::seconds(20);
        while (Clock::now() < dl && pool.route_count() < static_cast<std::size_t>(N2))
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const auto a = snap();
        std::this_thread::sleep_for(std::chrono::milliseconds(1200));
        const auto b = snap();
        const std::uint64_t d_scan = b.scanned_routes_total - a.scanned_routes_total;
        const std::uint64_t d_ready = b.scan_ready_rounds - a.scan_ready_rounds;
        const bool ok = (pool.route_count() == static_cast<std::size_t>(N2)) && (d_scan > 0) && (d_ready == 0);
        char d[256];
        std::snprintf(d, sizeof d, "route_count=%zu（应=%ld）；Δscanned=%llu（>0）；Δscan_ready_rounds=%llu（应=0 —— 无消息）",
                      pool.route_count(), N2, (unsigned long long)d_scan, (unsigned long long)d_ready);
        rec(2, "固定 route 无消息", ok, d);
    }

    /* ---------------- 用例 3：一次已知就绪（ready_observed 单调 +1 轮） ---------------- */
    if (want(3))
    {
        /* 常驻/门控一致性**必须**在诊断开启时才有意义（关诊断时门控恒 0）。 */
        dzIPC::measure::CounterRegistry::instance().set_diagnostics_enabled(true);
        const auto a = snap();
        const auto ag = snapg();
        /* 只给**一个** topic 发一条（其余保持静默）⇒ 就绪轮数应增加且远小于总轮数。 */
        {
            auto m = std::make_shared<dzIPC::Msg::StdString>();
            m->set_msg_id(kMsgId);
            m->str = "one";
            (void)pubs[0]->publish(m);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(800));
        const auto b = snap();
        const auto bg = snapg();
        const std::uint64_t d_ready = b.scan_ready_rounds - a.scan_ready_rounds;
        const std::uint64_t d_rounds = b.scan_rounds - a.scan_rounds;
        const std::uint64_t d_ready_g = bg.ready_observed - ag.ready_observed;
        const std::uint64_t d_rounds_g = bg.scan_rounds - ag.scan_rounds;
        /* 池统计一致性：常驻与门控的"就绪轮数 / 轮数"必须同步增长。 */
        const bool ok = (d_ready >= 1) && (d_ready <= d_rounds) && (d_ready_g >= 1) && (d_ready_g <= d_rounds_g) &&
                        (d_ready == d_ready_g) && (d_rounds == d_rounds_g);
        char d[320];
        std::snprintf(d, sizeof d,
                      "一次发布 ⇒ Δscan_ready_rounds=%llu（≥1）、Δscan_rounds=%llu；门控 ready_observed Δ=%llu、"
                      "scan_rounds Δ=%llu（常驻/门控逐值一致=%d）",
                      (unsigned long long)d_ready, (unsigned long long)d_rounds, (unsigned long long)d_ready_g,
                      (unsigned long long)d_rounds_g, (d_ready == d_ready_g && d_rounds == d_rounds_g) ? 1 : 0);
        rec(3, "一次已知就绪", ok, d);
        dzIPC::measure::CounterRegistry::instance().set_diagnostics_enabled(false);
        /* 排空，避免影响后续用例。 */
        for (long i = 0; i < N2; ++i)
        {
            auto sink = td();
            while (subs[static_cast<std::size_t>(i)]->try_get_clone(sink)) sink = td();
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(300));
    }

    /* ---------------- 用例 4：预算耗尽重入 deferred ----------------
     * 造法：把 wait_timeout 之外的**消息预算**压到 1（RecvBudget.max_messages_per_route=1
     * 由首个成功 start() 决定，本进程已 start ⇒ 无法改）；改为用"大量积压 + 1 条 route"
     * 触发 deferral：先灌满 64 深度的用户队列，让 worker 一次预算跑不完 ⇒ budget_yields 增长。 */
    if (want(4))
    {
        const auto a = snap();
        /* 一次发 64 条（= 订阅端队列容量），worker 的 max_messages_per_route=32 ⇒ 必须分两轮。 */
        for (int k = 0; k < 64; ++k)
        {
            auto m = std::make_shared<dzIPC::Msg::StdString>();
            m->set_msg_id(kMsgId);
            m->str = "B" + std::to_string(k);
            (void)pubs[0]->publish(m);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        const auto b = snap();
        long got = 0;
        {
            auto sink = td();
            while (subs[0]->try_get_clone(sink)) { ++got; sink = td(); }
        }
        const std::uint64_t d_yield = b.budget_yields - a.budget_yields;
        const std::uint64_t d_drain = b.deferred_drains - a.deferred_drains;
        const bool ok = (d_yield > 0 || d_drain > 0) && (got > 0) && (b.recv_errors == 0);
        char d[320];
        std::snprintf(d, sizeof d,
                      "灌 64 条（>每轮消息预算 32）⇒ Δbudget_yields=%llu、Δdeferred_drains=%llu、实收=%ld、"
                      "recv_errors=%llu（预算耗尽**不是丢弃**）",
                      (unsigned long long)d_yield, (unsigned long long)d_drain, got,
                      (unsigned long long)b.recv_errors);
        rec(4, "预算耗尽重入 deferred", ok, d);
    }

    /* ---------------- 用例 5：断开/注销（route 归零 + 线程归还） ---------------- */
    if (want(5))
    {
        const auto a = snap();
        const std::size_t before = pool.route_count();
        subs.clear();
        pubs.clear();
        const auto dl = Clock::now() + std::chrono::seconds(20);
        while (Clock::now() < dl && pool.route_count() != 0) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        const auto b = snap();
        std::this_thread::sleep_for(std::chrono::milliseconds(1400));
        const auto c = snap();
        const bool ok = (before == static_cast<std::size_t>(N2)) && (pool.route_count() == 0) &&
                        ((c.idle_exits - a.idle_exits) > 0);
        char d[320];
        std::snprintf(d, sizeof d, "注销前 route_count=%zu ⇒ 注销后 %zu；Δidle_exits=%llu（>0 = 线程按需归还）、"
                                   "Δthread_restarts=%llu",
                      before, pool.route_count(), (unsigned long long)(c.idle_exits - a.idle_exits),
                      (unsigned long long)(c.thread_restarts - a.thread_restarts));
        rec(5, "断开/注销", ok, d);
    }

    /* ---------------- 用例 6：假就绪（SHM 侧不适用，如实登记） ---------------- */
    if (want(6))
        note(6, "假就绪（false-ready）",
             "SHM 侧**不适用**：SHM 的事实判据是共享内存 seq（`seq != last_seq`），不存在"
             "『内核报了就绪但读不到』的形态；假就绪是 **socket** 侧形态（`fruitless_backoffs`/"
             "`rearm_events`，W04-F3 已修）。⛔ 不用其它形态冒充，socket 侧证据另见 W07 交付。");

    /* ---------------- 用例 7：后端错误（无法在 SHM 侧构成，如实登记） ---------------- */
    if (want(7))
        note(7, "后端错误（wait_errors/recv_errors）",
             "**无法在不改产品代码的前提下构成**：SHM 后端的 wait 只在 futex_waitv 返回异常时计"
             "`wait_errors`，该路径需要注入失败（属产品侧射程，⛔ 本次不改）。当前读数："
             "recv_errors=" + std::to_string(pool.stats().recv_errors) +
             "、wait_errors=" + std::to_string(pool.stats().wait_errors) + "（均 0，⛔ 不得当作『已覆盖』）。");

    /* ---------------- 用例 8：诊断开关前后对照（同配置、独立进程由 sweep 负责；此处做同进程交替） ---------------- */
    if (want(8))
    {
        std::vector<std::string> names;
        const long N8 = 20;
        std::vector<std::unique_ptr<dzIPC::shm::shm_sub_ipc>> s8;
        std::vector<std::unique_ptr<dzIPC::shm::shm_pub_ipc>> p8;
        for (long i = 0; i < N8; ++i) names.push_back("r5min8_" + std::to_string(dom + 8) + "_" + std::to_string(i));
        for (long i = 0; i < N8; ++i)
        {
            p8.emplace_back(new dzIPC::shm::shm_pub_ipc(td(), names[static_cast<std::size_t>(i)],
                                                        static_cast<std::size_t>(dom + 8), false));
            p8.back()->InitChannel("m8");
            s8.emplace_back(new dzIPC::shm::shm_sub_ipc(td(), names[static_cast<std::size_t>(i)],
                                                        static_cast<std::size_t>(dom + 8), 64, false));
            s8.back()->InitChannel("m8");
        }
        {
            const auto dl = Clock::now() + std::chrono::seconds(20);
            while (Clock::now() < dl && pool.route_count() < static_cast<std::size_t>(N8))
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        auto measure = [&](bool diag, std::uint64_t* d_rounds, std::uint64_t* d_scan, std::uint64_t* d_time,
                           std::uint64_t* d_ready_g) {
            dzIPC::measure::CounterRegistry::instance().set_diagnostics_enabled(diag);
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            const auto a = snap();
            const auto ag = snapg();
            std::this_thread::sleep_for(std::chrono::milliseconds(1500));
            const auto b = snap();
            const auto bg = snapg();
            *d_rounds = b.scan_rounds - a.scan_rounds;
            *d_scan = b.scanned_routes_total - a.scanned_routes_total;
            *d_time = bg.scan_time_ns_total - ag.scan_time_ns_total;
            *d_ready_g = bg.ready_observed - ag.ready_observed;
        };
        std::uint64_t r_off = 0, s_off = 0, t_off = 0, g_off = 0;
        std::uint64_t r_on = 0, s_on = 0, t_on = 0, g_on = 0;
        measure(false, &r_off, &s_off, &t_off, &g_off);
        measure(true, &r_on, &s_on, &t_on, &g_on);
        /* ⛔ 两档是**两个独立窗口**（时长近似但非逐值相等），故只要求量级一致（±20%）；
         * 门控"扫描耗时/就绪"在 off 档**必须恒 0**（未采集），在 on 档必须 >0。 */
        const auto near = [](std::uint64_t a, std::uint64_t b) {
            if (a == 0 || b == 0) return false;
            const double hi = static_cast<double>(std::max(a, b)), lo = static_cast<double>(std::min(a, b));
            return (hi / lo) <= 1.20;
        };
        const bool ok = (r_off > 0) && (s_off > 0) && (r_on > 0) && (s_on > 0) && (t_off == 0) && (g_off == 0) &&
                        (t_on > 0) && near(r_off, r_on) && near(s_off, s_on);
        char d[400];
        std::snprintf(d, sizeof d,
                      "diag=off：Δrounds=%llu Δscanned=%llu gated_scan_time=%llu（**未采集**，⛔ 非零成本）；"
                      "diag=on：Δrounds=%llu Δscanned=%llu gated_scan_time=%llu（>0）；"
                      "常驻量两档量级一致(±20%%)=%d",
                      (unsigned long long)r_off, (unsigned long long)s_off, (unsigned long long)t_off,
                      (unsigned long long)r_on, (unsigned long long)s_on, (unsigned long long)t_on,
                      (near(r_off, r_on) && near(s_off, s_on)) ? 1 : 0);
        rec(8, "诊断开关前后对照", ok, d);
        dzIPC::measure::CounterRegistry::instance().set_diagnostics_enabled(false);
        s8.clear();
        p8.clear();
    }

    /* ---------------- 落盘 ---------------- */
    if (!out.empty())
    {
        std::error_code ec;
        if (std::filesystem::exists(out, ec) && !std::filesystem::is_empty(out, ec))
        {
            std::printf("ARTIFACT_DIR_NOT_EMPTY: %s\n", out.c_str());
            return 2;
        }
        std::filesystem::create_directories(out, ec);
        {
            std::ofstream f(out + "/minimal_cases.csv");
            f << "case,name,result,detail\n";
            for (auto& r : rows) f << r << "\n";
        }
        {
            std::ofstream f(out + "/manifest.json");
            f << "{\n  \"run_id\": \"" << run_id << "\",\n  \"work_package\": \"W10-R5-minimal\",\n";
            f << "  \"criteria_version\": \"W10-R5/§6.2-minimal/v1\",\n";
            f << "  \"workers\": " << W << ",\n  \"topic_count\": " << N2 << ",\n";
            f << "  \"note\": \"SHM 侧；假就绪与后端错误两项如实登记为不适用/无法构成\"\n}\n";
        }
        {
            std::ofstream f(out + "/verdict.md");
            f << "# W10-R5 最小验证集（方案 §6.2）\n\n- run_id: `" << run_id << "`｜workers=" << W << "\n";
            f << "- 判定: **" << (failures ? "不通过" : "通过") << "**（失败 " << failures << " 项）\n\n";
            f << "| 用例 | 名称 | 结果 | 说明 |\n|---|---|---|---|\n";
            for (auto& r : rows)
            {
                std::string t = r;
                for (auto& ch : t) if (ch == ',') ch = '\t';
                std::istringstream is(t);
                std::string c, nm, res, det;
                std::getline(is, c, '\t'); std::getline(is, nm, '\t'); std::getline(is, res, '\t'); std::getline(is, det);
                f << "| " << c << " | " << nm << " | " << res << " | " << det << " |\n";
            }
        }
        std::printf("artifacts_dir=%s\n", out.c_str());
    }

    std::printf("W10_R5_MIN_DONE verdict=%s failures=%d\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}

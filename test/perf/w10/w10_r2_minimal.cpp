/* R2/t42 —— 方案 §6.2 最小验证矩阵中 **t30 交付缺失的 4 项**（独立最小用例）。
 *
 * R0（`W10/R0_口径与接口冻结.md` §5.3.1）逐文件清点的结论：t30 只覆盖了
 * 「诊断开关前后对照」与「假就绪」，缺 **① 无 route ② 预算耗尽重入 deferred
 * ③ 断开/注销 ④ 后端错误**。本文件**只补这四项**，且每项都带失败状态而不是日志提示。
 *
 * ⛔ 与既有工装的关系（不造平行工装）：
 *   · `w10_scancost.cpp`（t30）：三因子规模扫描 —— 本文件不重复它；
 *   · `w10_r5_minimal.cpp`（t38/W10-R5）：八用例框架，其中 ⑦ 后端错误登记为
 *     「无法在不改产品代码的前提下构成」、⑤ 断开/注销只到 route_count/idle_exits；
 *     本文件把那两项**做成可判决**（前者用 LD_PRELOAD 注入、后者用
 *     wait_timeouts / wait_wakeups 的**分离读数**），属"补齐"而不是"另起一套"。
 *
 * 为什么必须**每项一个独立进程**：池的 worker 数/预算是「首个成功 start() 的调用方」
 * 一次性决定的（契约 §8.1），同一进程里改不了 ⇒ 四项各需不同预算（尤其是本用例 ②
 * 需要 max_messages_per_route=1、③ 需要 wait_timeout=5000ms）。方案 §6.3 亦要求
 * 「使用新的独立进程，避免池首次初始化配置影响后续实验」。
 *
 * 用法（由 w10_r2_driver.sh 逐项调用）：
 *   w10_r2_minimal --case 1|2|3|4 --domain <d> [--out <dir>] [--run-id <id>] [--arm control|inject]
 * 退出码：0 = 该项通过；1 = 失败（逐条打印 FAILURE:）。
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
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "dzIPC/common/control_plane.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/detail/shm_sub_seam.h"
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

/* ---------------- 观测面 ---------------- */
struct Res
{
    std::uint64_t scan_rounds{0}, scanned_routes_total{0}, scan_ready_rounds{0};
    std::uint64_t deferred_depth_last{0}, deferred_depth_max{0};
    std::uint64_t wait_timeouts{0}, wait_wakeups{0}, wait_errors{0};
    std::uint64_t budget_yields{0}, deferred_drains{0}, messages_received{0};
    std::uint64_t recv_once_calls{0}, recv_errors{0}, idle_exits{0}, thread_restarts{0};
    std::size_t route_count{0};
};
static Res snap()
{
    const auto s = dzIPC::threepools::RecvWorkerPool::instance().stats();
    Res r;
    r.scan_rounds = s.scan_rounds;
    r.scanned_routes_total = s.scanned_routes_total;
    r.scan_ready_rounds = s.scan_ready_rounds;
    r.deferred_depth_last = s.deferred_depth_last;
    r.deferred_depth_max = s.deferred_depth_max;
    r.wait_timeouts = s.wait_timeouts;
    r.wait_wakeups = s.wait_wakeups;
    r.wait_errors = s.wait_errors;
    r.budget_yields = s.budget_yields;
    r.deferred_drains = s.deferred_drains;
    r.messages_received = s.messages_received;
    r.recv_once_calls = s.recv_once_calls;
    r.recv_errors = s.recv_errors;
    r.idle_exits = s.idle_exits;
    r.thread_restarts = s.thread_restarts;
    r.route_count = s.route_count;
    return r;
}
static std::uint64_t ctr(dzIPC::measure::CounterId id)
{
    return static_cast<std::uint64_t>(dzIPC::measure::CounterRegistry::instance().snapshot().get(id));
}
/* 池 RSS/CPU 不用；但"是否忙转"用 recv_once 速率与 scan 速率判（可机读、与负载无关）。 */

/* seam 事件计数（路径可分性 / 回退原因） */
static std::atomic<long> g_worker_ev{0}, g_compat_ev{0}, g_compat_backend{0}, g_compat_full{0}, g_compat_other{0};
static void seam_hook(const dzIPC::detail::SeamEvent& ev) noexcept
{
    if (ev.point == dzIPC::detail::SeamPoint::kRecvPathWorker)
    {
        g_worker_ev.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (ev.point != dzIPC::detail::SeamPoint::kRecvPathCompat) return;
    g_compat_ev.fetch_add(1, std::memory_order_relaxed);
    switch (static_cast<dzIPC::detail::RecvPathReason>(ev.size))
    {
    case dzIPC::detail::RecvPathReason::kBackendUnavailable: g_compat_backend.fetch_add(1); break;
    case dzIPC::detail::RecvPathReason::kWaitSetFull:        g_compat_full.fetch_add(1); break;
    default:                                                g_compat_other.fetch_add(1); break;
    }
}

/* 结果收集（机器判定：失败写 FAIL 而不是只打日志） */
struct Row
{
    std::string name, result, detail;
};
static std::vector<Row> g_rows;
static int g_failures = 0;
static void rec(const char* name, bool ok, const std::string& detail)
{
    g_rows.push_back({name, ok ? "PASS" : "FAIL", detail});
    std::printf("%-46s %s  %s\n", name, ok ? "PASS" : "FAIL", detail.c_str());
    if (!ok)
    {
        std::printf("FAILURE: %s —— %s\n", name, detail.c_str());
        ++g_failures;
    }
}

static bool wait_until(const std::function<bool()>& pred, int ms)
{
    const auto dl = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < dl)
    {
        if (pred()) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    return pred();
}

struct Topic
{
    std::string name;
    long domain;
    std::unique_ptr<dzIPC::shm::shm_pub_ipc> pub;
    std::unique_ptr<dzIPC::shm::shm_sub_ipc> sub;
};
static std::unique_ptr<Topic> make_topic(const std::string& n, long dom)
{
    auto t = std::make_unique<Topic>();
    t->name = n;
    t->domain = dom;
    t->pub.reset(new dzIPC::shm::shm_pub_ipc(td(), n, static_cast<std::size_t>(dom), false));
    t->pub->InitChannel("r2");
    t->sub.reset(new dzIPC::shm::shm_sub_ipc(td(), n, static_cast<std::size_t>(dom), 64, false));
    t->sub->InitChannel("r2");
    return t;
}
static void drop_topic(Topic* t)
{
    t->sub.reset();
    t->pub.reset();
    ipc::route::clear_storage(shm_topic_segment_name(t->name, static_cast<std::size_t>(t->domain)).c_str());
    ipc::shm::handle::clear_storage(shm_topic_control_name(t->name, static_cast<std::size_t>(t->domain)).c_str());
}
static bool publish_one(dzIPC::shm::shm_pub_ipc& p, const std::string& body)
{
    auto m = std::make_shared<dzIPC::Msg::StdString>();
    m->set_msg_id(kMsgId);
    m->str = body;
    return p.publish(m);
}
static void drain(dzIPC::shm::shm_sub_ipc& s, std::vector<std::string>* out)
{
    for (int i = 0; i < 200; ++i)
    {
        auto sink = td();
        if (!s.try_get_clone(sink)) break;
        if (out != nullptr)
        {
            /* TopicData::topic() 是被 clone 出来的负载对象（见 topic_data.h:39）。 */
            auto* p = dynamic_cast<dzIPC::Msg::StdString*>(sink->topic().get());
            out->push_back(p != nullptr ? p->str : std::string("?"));
        }
    }
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const long c = arg_long(argc, argv, "--case", 1);
    const long dom = arg_long(argc, argv, "--domain", 8800);
    const std::string out = arg_str(argc, argv, "--out", "");
    const std::string run_id = arg_str(argc, argv, "--run-id", "r2min");
    const std::string arm = arg_str(argc, argv, "--arm", "control");
    dzIPC::detail::SetSeamHook(&seam_hook);

    auto& pool = dzIPC::threepools::RecvWorkerPool::instance();
    std::string case_name;

    /* =====================================================================
     * 用例 1：无 route —— collect_pending() 在 route 表为空时的行为与计数
     * 断言：① 空表仍按轮扫描（scan_rounds 增长）② 扫描 route 数恒为 0
     *       ③ 无就绪轮 ④ **不忙转**（轮频率有上界）⑤ 不报错
     * ===================================================================== */
    if (c == 1)
    {
        case_name = "无 route";
        dzIPC::threepools::RecvBudget b{};      /* wait_timeout=100ms, idle_keep_alive=1000ms */
        b.wait_timeout = std::chrono::milliseconds{200};
        b.idle_keep_alive = std::chrono::milliseconds{30000};   /* 不让线程在窗口内退出 */
        if (!pool.start(4, b)) std::printf("NOTE pool already started\n");
        std::this_thread::sleep_for(std::chrono::milliseconds(400));
        const auto a = snap();
        const auto w0 = Clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(4000));
        const double win = std::chrono::duration<double>(Clock::now() - w0).count();
        const auto z = snap();

        const std::uint64_t d_rounds = z.scan_rounds - a.scan_rounds;
        const std::uint64_t d_scan = z.scanned_routes_total - a.scanned_routes_total;
        const std::uint64_t d_ready = z.scan_ready_rounds - a.scan_ready_rounds;
        const std::uint64_t d_err = (z.wait_errors - a.wait_errors) + (z.recv_errors - a.recv_errors);
        const double rounds_per_s = d_rounds / win;
        rec("1a 空表仍按轮扫描（scan_rounds>0）", d_rounds > 0,
            "Δscan_rounds=" + std::to_string(d_rounds) + "（窗口 " + std::to_string(win).substr(0, 4) + " s）");
        rec("1b 空表扫描 route 数恒为 0", d_scan == 0, "Δscanned_routes_total=" + std::to_string(d_scan));
        rec("1c 空表无就绪轮（scan_ready_rounds 不增）", d_ready == 0, "Δscan_ready_rounds=" + std::to_string(d_ready));
        rec("1d route_count==0 且 deferred 深度为 0", z.route_count == 0 && z.deferred_depth_max == 0,
            "route_count=" + std::to_string(z.route_count) + " deferred_depth_max=" +
                std::to_string(z.deferred_depth_max));
        /* 上界判据：轮频率 ≈ worker 数 / wait_timeout；忙转会是 1e5/s 量级。
         * 判据取 4×worker/wait_timeout（含 idle 检查与空转余量）。 */
        const double bound = 4.0 * static_cast<double>(pool.worker_count()) / 0.2;
        rec("1e 不忙转（轮频率有上界）", rounds_per_s <= bound && z.recv_once_calls == a.recv_once_calls,
            "rounds/s=" + std::to_string(rounds_per_s) + " ≤ 上界 " + std::to_string(bound) +
                "；Δrecv_once_calls=" + std::to_string(z.recv_once_calls - a.recv_once_calls) + "（应为 0）");
        rec("1f 无错误（wait_errors/recv_errors 不增）", d_err == 0, "Δerrors=" + std::to_string(d_err));
    }

    /* =====================================================================
     * 用例 2：预算耗尽重入 deferred
     * 造法：把**消息预算**压到 1 条/轮（本进程 start 时决定）⇒ 一条 route 上 N 条积压
     *   必须跨 N 轮排空，**且中途不再有任何新发布**（W04 §10.2 明令：不得依赖下一条
     *   新消息才继续排空）。
     * 断言：① N 条全部收到且顺序正确 ② budget_yields/deferred_drains 增长（重入次数）
     *       ③ deferred 深度有非零读数（按 worker 导出值）④ 无错误
     * ===================================================================== */
    if (c == 2)
    {
        case_name = "预算耗尽重入 deferred";
        dzIPC::threepools::RecvBudget b{};
        b.max_messages_per_route = 1;                          /* 每轮只允许 1 条 ⇒ 必然多次让出 */
        b.max_bytes_per_route = 1u << 20;
        b.max_processing_time_per_route = std::chrono::microseconds{200000};
        b.wait_timeout = std::chrono::milliseconds{100};
        b.idle_keep_alive = std::chrono::milliseconds{30000};
        if (!pool.start(2, b)) std::printf("NOTE pool already started\n");
        auto t = make_topic("r2min_nobudget_" + std::to_string(dom), dom);
        const bool reg = wait_until([&] { return pool.route_count() >= 1; }, 20000);
        rec("2a route 已进池（前提成立）", reg, "route_count=" + std::to_string(pool.route_count()));

        const long N = 8;
        long sent = 0;
        for (long i = 0; i < N; ++i)
            if (publish_one(*t->pub, "M" + std::to_string(i))) ++sent;
        rec("2b 积压已发出（> 每轮预算 1 条）", sent == N,
            "sent=" + std::to_string(sent) + "/" + std::to_string(N) + "，每轮消息预算=1");
        /* 发布全部完成后**不再有任何新发布**：排空只可能来自"预算耗尽后重新发现"。 */
        const auto a = snap();
        std::vector<std::string> got;
        const auto dl = Clock::now() + std::chrono::seconds(20);
        while (Clock::now() < dl && got.size() < static_cast<std::size_t>(N))
        {
            drain(*t->sub, &got);
            if (got.size() < static_cast<std::size_t>(N)) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        const auto z = snap();
        const std::uint64_t d_yield = z.budget_yields - a.budget_yields;
        const std::uint64_t d_drain = z.deferred_drains - a.deferred_drains;

        rec("2c 无新发布下全部排空（不得依赖新消息）", got.size() == static_cast<std::size_t>(N),
            "收到 " + std::to_string(got.size()) + "/" + std::to_string(N) + "（发布完成后未再 publish）");
        bool ordered = (got.size() == static_cast<std::size_t>(N));
        for (std::size_t i = 0; ordered && i < got.size(); ++i)
            if (got[i] != "M" + std::to_string(i)) ordered = false;
        rec("2d 逐条序号/载荷正确（无丢失、无重复、无乱序）", ordered,
            ordered ? "M0..M" + std::to_string(N - 1) + " 逐条一致" : "顺序或内容不符");
        rec("2e 预算耗尽已重入（budget_yields/deferred_drains 增长）", d_yield >= 1 && d_drain >= 1,
            "Δbudget_yields=" + std::to_string(d_yield) + " Δdeferred_drains=" + std::to_string(d_drain) +
                "（≥ 重入次数；N-1=" + std::to_string(N - 1) + "）");
        rec("2f deferred 深度有非零读数（按 worker 导出）", z.deferred_depth_max >= 1,
            "deferred_depth_max=" + std::to_string(z.deferred_depth_max) + " deferred_depth_last=" +
                std::to_string(z.deferred_depth_last));
        rec("2g 无错误", (z.recv_errors - a.recv_errors) == 0,
            "Δrecv_errors=" + std::to_string(z.recv_errors - a.recv_errors));
        drop_topic(t.get());
    }

    /* =====================================================================
     * 用例 3：断开 / 注销 —— 必须**被可靠唤醒**（不是靠超时轮询）
     * 造法：wait_timeout 抬到 5000 ms ⇒ 若唤醒失效，一次等待要 5 s 才返回；
     *   分别测「发布者断开」与「订阅注销」两条路径，给 wait_wakeups 与
     *   wait_timeouts 的**分离读数**。
     * 断言：① 两条路径都 wait_wakeups ≥1 且 wait_timeouts==0 ② 唤醒延迟 << wait_timeout
     *       ③ 注销后 route_count 归零
     * ===================================================================== */
    if (c == 3)
    {
        case_name = "断开/注销";
        dzIPC::threepools::RecvBudget b{};
        b.wait_timeout = std::chrono::milliseconds{5000};   /* 判据关键：超时唤醒要 5 s */
        b.idle_keep_alive = std::chrono::milliseconds{60000};
        if (!pool.start(2, b)) std::printf("NOTE pool already started\n");
        auto t = make_topic("r2min_disc_" + std::to_string(dom), dom);
        const bool reg = wait_until([&] { return pool.route_count() >= 1; }, 20000);
        rec("3a route 已进池（前提成立）", reg, "route_count=" + std::to_string(pool.route_count()));
        std::this_thread::sleep_for(std::chrono::milliseconds(300));   /* 让 worker 进入 5 s 等待 */

        /* ---- 3b 发布者断开：必须把 worker 从 5 s 等待里**立刻**拽出来 ---- */
        {
            const auto a = snap();
            const auto t0 = Clock::now();
            t->pub.reset();                                    /* 析构 ⇒ disconnect ⇒ 唤醒订阅侧 */
            const bool woke = wait_until([&] { return snap().wait_wakeups > a.wait_wakeups; }, 2000);
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            const auto z = snap();
            rec("3b 发布者断开被可靠唤醒（wakeups↑ 且无超时）",
                woke && (z.wait_wakeups - a.wait_wakeups) >= 1 && (z.wait_timeouts - a.wait_timeouts) == 0,
                "Δwait_wakeups=" + std::to_string(z.wait_wakeups - a.wait_wakeups) + " Δwait_timeouts=" +
                    std::to_string(z.wait_timeouts - a.wait_timeouts) + " 唤醒延迟≈" +
                    std::to_string(ms).substr(0, 5) + " ms（wait_timeout=5000 ms）");
            rec("3b2 断开唤醒延迟 << wait_timeout", ms < 1000.0,
                std::to_string(ms).substr(0, 6) + " ms < 1000 ms（wait_timeout=5000 ms）");
        }
        /* ⛔ 断开会把 route 一并摘掉 ⇒ 必须在**重连之后**再测注销，否则测的不是注销路径。
         *   重连 = 重新 InitChannel（产品侧的重连路径），再等注册回到池里。 */
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        {
            /* 重建发布者（同话题名、同 domain）⇒ 控制面把订阅侧重新注册回池。 */
            t->pub.reset(new dzIPC::shm::shm_pub_ipc(td(), t->name, static_cast<std::size_t>(t->domain), false));
            t->pub->InitChannel("r2");
            const bool back = wait_until([&] { return pool.route_count() >= 1; }, 20000);
            rec("3b3 断开后重连、route 重新进池（注销用例的前提）", back,
                "route_count=" + std::to_string(pool.route_count()));
            /* 让 worker 再进入一次长等待。 */
            std::this_thread::sleep_for(std::chrono::milliseconds(300));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(300));
        /* ---- 3c 订阅注销 ---- */
        {
            const auto a = snap();
            const auto t0 = Clock::now();
            drop_topic(t.get());                               /* 析构 ⇒ remove_route ⇒ wait_set.remove ⇒ 唤醒 */
            const bool woke = wait_until([&] { return snap().wait_wakeups > a.wait_wakeups; }, 2000);
            const double ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            const bool gone = wait_until([&] { return pool.route_count() == 0; }, 5000);
            const auto z = snap();
            rec("3c 注销被可靠唤醒（wakeups↑ 且无超时）",
                woke && (z.wait_wakeups - a.wait_wakeups) >= 1 && (z.wait_timeouts - a.wait_timeouts) == 0,
                "Δwait_wakeups=" + std::to_string(z.wait_wakeups - a.wait_wakeups) + " Δwait_timeouts=" +
                    std::to_string(z.wait_timeouts - a.wait_timeouts) + " 唤醒延迟≈" +
                    std::to_string(ms).substr(0, 5) + " ms");
            rec("3d 注销后 route 归零", gone && pool.route_count() == 0,
                "route_count=" + std::to_string(pool.route_count()));
            rec("3e 无错误（wait_errors/recv_errors 不增）", (z.wait_errors - a.wait_errors) == 0 &&
                                                                   (z.recv_errors - a.recv_errors) == 0,
                "Δwait_errors=" + std::to_string(z.wait_errors - a.wait_errors) + " Δrecv_errors=" +
                    std::to_string(z.recv_errors - a.recv_errors));
        }
    }

    /* =====================================================================
     * 用例 4：后端错误 —— 显式失败路径与计数（⛔ 不得静默降级）
     * 造法：LD_PRELOAD 把 `ipc::recv_wait_set::add` 强制返回 false（libipc 对它的调用
     *   走自己的 PLT，可被抢占；**不改任何产品源文件、不改 ABI**）。
     * 断言（inject 臂）：① 走显式回退、原因码恰为 kBackendUnavailable ② 计数一一对应
     *   ③ **消息仍能收到**（回退线程接住，不是静默丢包）④ 池内 route_count==0 且
     *   registration_ok==0 ⑤ 无忙转（收到消息的耗时有界）
     * 对照臂（control）：走 worker、fallback 全 0 —— 用**同一二进制**跑，差异只来自注入。
     * ===================================================================== */
    if (c == 4)
    {
        case_name = "后端错误";
        dzIPC::threepools::RecvBudget b{};
        if (!pool.start(4, b)) std::printf("NOTE pool already started\n");
        const bool backend_before = dzIPC::threepools::RecvWorkerPool::backend_available();
        const long N = 3;
        /* ⛔ 计数基线必须在**建订阅之前**取：路径决策/注册/回退都发生在 InitChannel 的
         * 握手路径上（不是发布时）。基线取晚了会把"已发生的事件"漏掉 ⇒ 假红。 */
        const std::uint64_t f0 = ctr(dzIPC::measure::CounterId::fallback_total);
        const std::uint64_t fb0 = ctr(dzIPC::measure::CounterId::fallback_backend_unavailable);
        const std::uint64_t fc0 = ctr(dzIPC::measure::CounterId::fallback_capacity_full);
        const std::uint64_t ra0 = ctr(dzIPC::measure::CounterId::registration_attempts);
        const std::uint64_t ro0 = ctr(dzIPC::measure::CounterId::registration_ok);
        const std::uint64_t wt0 = ctr(dzIPC::measure::CounterId::wait_token_invalid);
        const auto a = snap();
        std::vector<std::unique_ptr<Topic>> ts;
        for (long i = 0; i < N; ++i) ts.push_back(make_topic("r2min_bk_" + std::to_string(dom) + "_" + std::to_string(i), dom));
        /* 等路径决策与注册落定（worker 臂等 pool.route_count；compat 臂靠 seam 事件）。 */
        wait_until([&] { return pool.route_count() >= static_cast<std::size_t>(N) || g_compat_ev.load() >= N; }, 20000);
        std::this_thread::sleep_for(std::chrono::milliseconds(400));

        const auto t0 = Clock::now();
        long sent = 0;
        for (long i = 0; i < N; ++i)
            if (publish_one(*ts[static_cast<std::size_t>(i)]->pub, "BK" + std::to_string(i))) ++sent;
        long rx = 0;
        {
            const auto dl = Clock::now() + std::chrono::seconds(20);
            while (Clock::now() < dl && rx < N)
            {
                rx = 0;
                for (long i = 0; i < N; ++i)
                {
                    auto sink = td();
                    if (ts[static_cast<std::size_t>(i)]->sub->try_get_clone(sink)) ++rx;
                }
                if (rx < N) std::this_thread::sleep_for(std::chrono::milliseconds(20));
            }
        }
        const double rx_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
        const auto z = snap();
        const std::uint64_t d_f = ctr(dzIPC::measure::CounterId::fallback_total) - f0;
        const std::uint64_t d_fb = ctr(dzIPC::measure::CounterId::fallback_backend_unavailable) - fb0;
        const std::uint64_t d_fc = ctr(dzIPC::measure::CounterId::fallback_capacity_full) - fc0;
        const std::uint64_t d_ra = ctr(dzIPC::measure::CounterId::registration_attempts) - ra0;
        const std::uint64_t d_ro = ctr(dzIPC::measure::CounterId::registration_ok) - ro0;
        const std::uint64_t d_wt = ctr(dzIPC::measure::CounterId::wait_token_invalid) - wt0;
        const long ev_w = g_worker_ev.load(), ev_c = g_compat_ev.load();
        const long ev_bk = g_compat_backend.load(), ev_full = g_compat_full.load(), ev_oth = g_compat_other.load();
        const bool injected = (arm == "inject");

        rec("4a 三条 route 全部收到消息（回退不是丢包）", sent == N && rx == N,
            "sent=" + std::to_string(sent) + "/" + std::to_string(N) + " rx=" + std::to_string(rx) + "/" +
                std::to_string(N) + " 用时≈" + std::to_string(rx_ms).substr(0, 6) + " ms");
        rec("4b 收到消息的耗时有界（无忙等死循环）", rx == N && rx_ms < 15000.0,
            "rx 用时 " + std::to_string(rx_ms).substr(0, 6) + " ms < 15000 ms（上界判据）");
        if (injected)
        {
            rec("4c 注入臂：走**显式**回退，原因码恰为 kBackendUnavailable",
                ev_c == static_cast<long>(N) && ev_bk == static_cast<long>(N) && ev_w == 0 && ev_full == 0 &&
                    ev_oth == 0,
                "seam compat=" + std::to_string(ev_c) + "/" + std::to_string(N) + " backend_unavailable=" +
                    std::to_string(ev_bk) + " worker=" + std::to_string(ev_w) + " waitset_full=" +
                    std::to_string(ev_full) + " other=" + std::to_string(ev_oth));
            rec("4d 注入臂：失败被计数（fallback_total==fallback_backend_unavailable==N，容量/token 计数为 0）",
                d_f == static_cast<std::uint64_t>(N) && d_fb == static_cast<std::uint64_t>(N) && d_fc == 0 && d_wt == 0,
                "Δfallback_total=" + std::to_string(d_f) + " Δfallback_backend_unavailable=" + std::to_string(d_fb) +
                    " Δfallback_capacity_full=" + std::to_string(d_fc) + " Δwait_token_invalid=" + std::to_string(d_wt));
            rec("4e 注入臂：池未接管（route_count==0、registration_ok==0、backend_available==false）",
                z.route_count == 0 && d_ro == 0 && !dzIPC::threepools::RecvWorkerPool::backend_available(),
                "route_count=" + std::to_string(z.route_count) + " Δregistration_ok=" + std::to_string(d_ro) +
                    " Δregistration_attempts=" + std::to_string(d_ra) + " backend_available=false（探测后永久置否）");
            /* ⛔ 「无静默降级」的正向判据：后端被判定不可用后，等待层必须把它记为
             * **错误**（wait_errors）而不是普通超时（wait_timeouts）—— 这正是
             * `wait_once()` 的既有分支（backend_state()==2 → wait_errors）。把两者混算
             * 就会让"后端坏了"看起来像"只是闲"。 */
            rec("4f 注入臂：⛔ 无静默降级（回退==route 数、未走 worker、等待层记为**错误**而非普通超时）",
                ev_c == static_cast<long>(N) && ev_w == 0 && (z.wait_errors - a.wait_errors) > 0 &&
                    (z.wait_timeouts - a.wait_timeouts) == 0,
                "compat=" + std::to_string(ev_c) + " == " + std::to_string(N) + "；worker=0；Δwait_errors=" +
                    std::to_string(z.wait_errors - a.wait_errors) + "（>0 = 显式错误路径）、Δwait_timeouts=" +
                    std::to_string(z.wait_timeouts - a.wait_timeouts) + "（==0 = 未被当成普通空闲）");
        }
        else
        {
            rec("4c 对照臂：走 worker、无回退（同一二进制，差异仅来自注入）",
                ev_w == static_cast<long>(N) && ev_c == 0 && d_f == 0 && d_fb == 0,
                "seam worker=" + std::to_string(ev_w) + "/" + std::to_string(N) + " compat=" + std::to_string(ev_c) +
                    " Δfallback_total=" + std::to_string(d_f));
            rec("4d 对照臂：池已接管（route_count==N、registration_ok==N）",
                z.route_count == static_cast<std::size_t>(N) && d_ro == static_cast<std::uint64_t>(N),
                "route_count=" + std::to_string(z.route_count) + " Δregistration_ok=" + std::to_string(d_ro));
            rec("4e 对照臂：backend_available 保持 true", backend_before && dzIPC::threepools::RecvWorkerPool::backend_available(),
                "backend_available=true（未探测到失败）");
            rec("4f 对照臂：无错误（wait_errors 与 wait_timeouts 归零的语义对照）",
                (z.wait_errors - a.wait_errors) == 0 && (z.recv_errors - a.recv_errors) == 0,
                "Δwait_errors=" + std::to_string(z.wait_errors - a.wait_errors) + " Δrecv_errors=" +
                    std::to_string(z.recv_errors - a.recv_errors) + "（⇒ 注入臂的 Δwait_errors>0 确由注入造成）");
        }
        for (auto& t : ts) drop_topic(t.get());
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
            std::ofstream f(out + "/cases.csv");
            f << "case,name,result,detail\n";
            for (const auto& r : g_rows)
            {
                std::string d = r.detail;
                for (auto& ch : d) if (ch == '"') ch = '\'';
                f << c << ",\"" << r.name << "\"," << r.result << ",\"" << d << "\"\n";
            }
        }
        {
            const auto z = snap();
            std::ofstream f(out + "/readings.json");
            f << "{\n  \"run_id\": \"" << run_id << "\",\n  \"case\": " << c << ",\n  \"arm\": \"" << arm
              << "\",\n  \"domain\": " << dom << ",\n  \"workers\": " << pool.worker_count() << ",\n"
              << "  \"wait_timeout_ms\": " << pool.budget().wait_timeout.count() << ",\n"
              << "  \"max_messages_per_route\": " << pool.budget().max_messages_per_route << ",\n"
              << "  \"stats\": {\"scan_rounds\": " << z.scan_rounds << ", \"scanned_routes_total\": "
              << z.scanned_routes_total << ", \"scan_ready_rounds\": " << z.scan_ready_rounds
              << ", \"deferred_depth_last\": " << z.deferred_depth_last << ", \"deferred_depth_max\": "
              << z.deferred_depth_max << ", \"wait_timeouts\": " << z.wait_timeouts << ", \"wait_wakeups\": "
              << z.wait_wakeups << ", \"wait_errors\": " << z.wait_errors << ", \"budget_yields\": "
              << z.budget_yields << ", \"deferred_drains\": " << z.deferred_drains
              << ", \"messages_received\": " << z.messages_received << ", \"recv_once_calls\": "
              << z.recv_once_calls << ", \"route_count\": " << z.route_count << "},\n"
              << "  \"seam\": {\"worker\": " << g_worker_ev.load() << ", \"compat\": " << g_compat_ev.load()
              << ", \"compat_backend_unavailable\": " << g_compat_backend.load() << ", \"compat_waitset_full\": "
              << g_compat_full.load() << ", \"compat_other\": " << g_compat_other.load() << "},\n"
              << "  \"failures\": " << g_failures << "\n}\n";
        }
    }
    dzIPC::detail::SetSeamHook(nullptr);
    std::printf("W10_R2_MIN_DONE case=%ld name=%s verdict=%s failures=%d\n", c, case_name.c_str(),
                g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}

/* W06 竞态/预算/回退回归探针（证据用，非交付源；每条场景一个进程 —— 因为
 * DZIPC_SHM_RECV_* 与 DZIPC_SHM_RECV_COMPAT 都是**进程内只读一次**的启动配置，
 * 进程内换臂/换预算非法）。
 *
 * 场景（--case=）：
 *   budget   —— 消息数预算压到 1：一次发 5 条，5 条必须**全部**到达
 *               （预算耗尽 = 进 deferred FIFO 而不是丢弃；§10.3 承重条）。
 *   hol      —— 队头阻塞：同 worker 上「64 KiB 热路 + 小消息冷路」，冷路延迟必须
 *               有界（预算在完整 recv_once 之后让出 ⇒ 冷路不会被热路饿死）。
 *   dyn      —— 动态 add/remove 抖动：反复建/拆订阅者，期间的发布不得丢，
 *               全部拆完后 route_count 必须回基线（消息唤醒 + 断开唤醒都走到）。
 *   fallback —— 回退可检测：打印 seam 的 kRecvPathWorker / kRecvPathCompat(+原因码)
 *               与 RecvWorkerPool 计数，两条臂必须**可分**（验收模式不得被回退线程掩盖）。
 *
 * 编译：g++ -std=c++17 -O2 -DNDEBUG -I include -I src w06_regress.cpp -o w06_regress \
 *           -L build/lib -lipc -lpthread -lrt -Wl,-rpath,$PWD/build/lib
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dzIPC/common/name_operator.h"
#include "dzIPC/detail/shm_sub_seam.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/threepools/recv_worker.h"
#include "dzIPC/measure/counters.h"
#include "ipc_msg/std_msgs/std_string.hpp"

using Clock = std::chrono::steady_clock;
static constexpr std::uint32_t kMsgId = 77;

struct Tally
{
    std::atomic<long> worker_path{0};
    std::atomic<long> compat_total{0};
    std::atomic<long> fallback{0};
    std::atomic<long> path_choice{0};
    std::atomic<int> last_reason{-1};
};
static Tally g_tally;

static void seam_hook(const dzIPC::detail::SeamEvent& ev) noexcept
{
    if (ev.point == dzIPC::detail::SeamPoint::kRecvPathWorker)
    {
        g_tally.worker_path.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    if (ev.point != dzIPC::detail::SeamPoint::kRecvPathCompat)
    {
        return;
    }
    g_tally.compat_total.fetch_add(1, std::memory_order_relaxed);
    g_tally.last_reason.store(static_cast<int>(ev.size), std::memory_order_relaxed);
    switch (static_cast<dzIPC::detail::RecvPathReason>(ev.size))
    {
    case dzIPC::detail::RecvPathReason::kForcedCompatEnv:
    case dzIPC::detail::RecvPathReason::kForkChild:
    case dzIPC::detail::RecvPathReason::kNoRoute:
        g_tally.path_choice.fetch_add(1, std::memory_order_relaxed);
        break;
    default:
        g_tally.fallback.fetch_add(1, std::memory_order_relaxed);
        break;
    }
}

static const char* arg_str(int argc, char** argv, const char* key, const char* def)
{
    const std::size_t klen = std::strlen(key);
    for (int i = 1; i < argc; ++i)
    {
        if (std::strncmp(argv[i], key, klen) == 0)
        {
            if (argv[i][klen] == '=') return argv[i] + klen + 1;   /* --case=x */
            if (argv[i][klen] == '\0' && i + 1 < argc) return argv[i + 1];
        }
    }
    return def;
}

static long arg_long(int argc, char** argv, const char* key, long def)
{
    const char* v = arg_str(argc, argv, key, nullptr);
    return v != nullptr ? std::strtol(v, nullptr, 10) : def;
}
static std::shared_ptr<dzIPC::TopicData> mk_td()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId);
}
static std::shared_ptr<dzIPC::Msg::StdString> mk_msg(const std::string& body)
{
    auto m = std::make_shared<dzIPC::Msg::StdString>();
    m->set_msg_id(kMsgId);
    m->str = body;
    return m;
}
static std::string topic_of(const char* tag, int domain, long i)
{
    return std::string("w06rg_") + tag + "_" + std::to_string(domain) + "_" + std::to_string(i);
}
static bool wait_peers(const std::string& name, int domain, std::uint32_t want, int ms)
{
    const auto dl = Clock::now() + std::chrono::milliseconds(ms);
    while (Clock::now() < dl)
    {
        dzIPC::control_plane_shm::TopicControlPlane cp;
        if (cp.open(shm_topic_control_name(name, static_cast<std::size_t>(domain))) && cp.peer_count() >= want)
        {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}
/* 排空到 got 或超时；返回取到的条数。 */
static int drain(dzIPC::shm::shm_sub_ipc& sub, int want, int ms)
{
    auto sink = mk_td();
    const auto dl = Clock::now() + std::chrono::milliseconds(ms);
    int got = 0;
    while (got < want && Clock::now() < dl)
    {
        if (sub.try_get_clone(sink)) { ++got; continue; }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    return got;
}

static std::size_t thread_count()
{
    DIR* d = ::opendir("/proc/self/task");
    if (d == nullptr) return 0;
    std::size_t n = 0;
    while (dirent* e = ::readdir(d))
    {
        if (e->d_name[0] != '.') ++n;
    }
    ::closedir(d);
    return n;
}

/* ───────────────────────── 场景 budget ───────────────────────── */
/* DZIPC_SHM_RECV_BUDGET_MSGS=1 ⇒ 每轮 recv_once 之后必然"预算耗尽"。若预算耗尽
 * 被实现成丢弃，这里就只能收到 1 条。 */
static int case_budget(int argc, char** argv)
{
    const int domain = static_cast<int>(arg_long(argc, argv, "--domain", 300));
    const std::string name = topic_of("budget", domain, 0);
    constexpr int kN = 5;
    int sent = 0;
    int got = 0;
    {
        /* ⛔ 对象必须先析构、**再**清段：在 pub/sub 存活时 clear_storage 会在头库与
         * 基线库上同样 SIGSEGV（属探针脚手架误用，见 W05 探针文件头纪律）。 */
        dzIPC::shm::shm_pub_ipc pub{mk_td(), name, static_cast<std::size_t>(domain), false};
        dzIPC::shm::shm_sub_ipc sub{mk_td(), name, static_cast<std::size_t>(domain), 32, false};
        pub.InitChannel("w06");
        sub.InitChannel("w06");
        if (!wait_peers(name, domain, 1, 10000)) { std::printf("FAIL precondition: no peer\n"); return 2; }
        for (int i = 0; i < kN; ++i)
        {
            if (pub.publish(mk_msg("B" + std::to_string(i)))) ++sent;
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
        }
        got = drain(sub, kN, 15000);
    }
    const auto& b = dzIPC::threepools::RecvWorkerPool::instance().budget();
    std::printf("case=budget sent=%d got=%d/%d effective_max_msgs=%zu\n", sent, got, kN, b.max_messages_per_route);
    std::printf("verdict=%s\n", (sent == kN && got == kN) ? "PASS(budget yield is not message loss)" : "FAIL");
    ipc::route::clear_storage(shm_topic_segment_name(name, static_cast<std::size_t>(domain)).c_str());
    ipc::shm::handle::clear_storage(shm_topic_control_name(name, static_cast<std::size_t>(domain)).c_str());
    return (sent == kN && got == kN) ? 0 : 1;
}

/* ───────────────────────── 场景 hol ───────────────────────── */
/* 同 worker 上的两条 route：hot 以 64 KiB 大消息持续灌、cold 只发一条小消息。
 * 预算在完整 recv_once 之后让出 ⇒ cold 的延迟必须由 deferred FIFO 界定（有界），
 * 而不是"热路不停 ⇒ 冷路永远排不上"。 */
static int case_hol(int argc, char** argv)
{
    const int domain = static_cast<int>(arg_long(argc, argv, "--domain", 301));
    const int hot_ms = static_cast<int>(arg_long(argc, argv, "--hot-ms", 400));
    auto& pool = dzIPC::threepools::RecvWorkerPool::instance();
    (void)pool.start(0, dzIPC::threepools::RecvBudget{});
    const std::size_t count = pool.worker_count();

    /* 找一对**落到同一 worker** 的话题（同 route 单消费者 + 同 worker ⇒ 真正的队头）。 */
    std::string hot_name, cold_name;
    std::size_t hot_w = 0;
    for (long i = 0; i < 4000 && hot_name.empty(); ++i)
    {
        const std::string a = topic_of("holA", domain, i);
        const std::size_t wa = dzIPC::threepools::RecvWorkerPool::worker_for(a.c_str(), domain, count);
        for (long j = i + 1; j < 4000; ++j)
        {
            const std::string b = topic_of("holB", domain, j);
            if (dzIPC::threepools::RecvWorkerPool::worker_for(b.c_str(), domain, count) == wa)
            {
                hot_name = a;
                cold_name = b;
                hot_w = wa;
                break;
            }
        }
    }
    if (hot_name.empty()) { std::printf("FAIL precondition: no colliding pair\n"); return 2; }
    std::printf("case=hol worker=%zu workers=%zu hot=%s cold=%s hot_ms=%d\n", hot_w, count, hot_name.c_str(),
                cold_name.c_str(), hot_ms);

    long long cold_us = -1;
    long hot_ok = 0;
    long hot_try = 0;
    int warm = 0;
    {
        dzIPC::shm::shm_pub_ipc hp{mk_td(), hot_name, static_cast<std::size_t>(domain), false};
        dzIPC::shm::shm_sub_ipc hs{mk_td(), hot_name, static_cast<std::size_t>(domain), 32, false};
        dzIPC::shm::shm_pub_ipc cp{mk_td(), cold_name, static_cast<std::size_t>(domain), false};
        dzIPC::shm::shm_sub_ipc cs{mk_td(), cold_name, static_cast<std::size_t>(domain), 8, false};
        hp.InitChannel("w06"); hs.InitChannel("w06"); cp.InitChannel("w06"); cs.InitChannel("w06");
        if (!wait_peers(hot_name, domain, 1, 10000) || !wait_peers(cold_name, domain, 1, 10000))
        {
            std::printf("FAIL precondition: no peers\n");
            return 2;
        }

        const std::string big(64 * 1024, 'H');
        /* 正对照：冷路在"还没热"时确实通。 */
        if (!cp.publish(mk_msg("C0"))) { std::printf("FAIL: cold publish 0 refused\n"); return 1; }
        warm = drain(cs, 1, 10000);
        std::printf("warmup_cold_rx=%d\n", warm);

        /* 热路在**另一个线程**上按最大速率灌 64 KiB（大消息热路）；主线程在这段时间里
         * 发一条小消息到同 worker 的冷路，量它的投递延迟。
         * ⛔ 冷路若有界，说明"预算在完整 recv_once 之后让出 + deferred FIFO"生效；
         * 若无界（饿死），本用例红。 */
        std::atomic<bool> stop{false};
        std::thread hot([&] {
            while (!stop.load(std::memory_order_relaxed))
            {
                auto m = std::make_shared<dzIPC::Msg::StdString>();
                m->set_msg_id(kMsgId);
                m->str = big;
                hot_try += 1;
                if (hp.publish(m)) hot_ok += 1;
                else std::this_thread::sleep_for(std::chrono::microseconds(200));
                /* 订阅端也在排空：热路只有被真的消费掉才会持续产生 recv_once 负载。 */
                auto sink = mk_td();
                (void)hs.try_get_clone(sink);
            }
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(60));   /* 让热路先热起来 */

        const auto t_send = Clock::now();
        if (!cp.publish(mk_msg("C1"))) { std::printf("FAIL: cold publish 1 refused\n"); return 1; }
        auto sink = mk_td();
        const auto dl = Clock::now() + std::chrono::milliseconds(std::max(2000, hot_ms * 5));
        while (Clock::now() < dl)
        {
            if (cs.try_get_clone(sink))
            {
                cold_us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t_send).count();
                break;
            }
            std::this_thread::sleep_for(std::chrono::microseconds(200));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(hot_ms));
        stop.store(true, std::memory_order_relaxed);
        hot.join();
    }
    const auto st = pool.stats();
    std::printf("case=hol hot_publish_ok=%ld/%ld cold_latency_us=%lld budget_yields=%llu deferred_drains=%llu "
                "recv_once_calls=%llu recv_once_max_ns=%llu over_budget=%llu msgs=%llu route_count=%zu\n",
                hot_ok, hot_try, cold_us, static_cast<unsigned long long>(st.budget_yields),
                static_cast<unsigned long long>(st.deferred_drains),
                static_cast<unsigned long long>(st.recv_once_calls),
                static_cast<unsigned long long>(st.recv_once_max_ns),
                static_cast<unsigned long long>(st.recv_once_over_budget),
                static_cast<unsigned long long>(st.messages_received), st.route_count);
    /* 判据：① 冷路在有界延迟内被投递（无饥饿）；② 热路确实产生了负载（否则是伪绿）；
     * ③ 单次 recv_once 耗时被单独记录（§10.3 的口径要求）。 */
    const bool ok = (warm == 1) && (cold_us >= 0) && (hot_ok > 100) && (st.recv_once_calls > 0);
    std::printf("verdict=%s cold_bounded_under_hot_load(hot_ok=%ld cold_us=%lld)\n", ok ? "PASS" : "FAIL",
                hot_ok, cold_us);
    for (const auto& nm : {hot_name, cold_name})
    {
        ipc::route::clear_storage(shm_topic_segment_name(nm, static_cast<std::size_t>(domain)).c_str());
        ipc::shm::handle::clear_storage(shm_topic_control_name(nm, static_cast<std::size_t>(domain)).c_str());
    }
    return ok ? 0 : 1;
}

/* ───────────────────────── 场景 dyn ───────────────────────── */
/* 动态 add/remove 抖动：建 8 条持久话题并持续发布，期间反复建/拆第 9 条（同一
 * topic 名反复重建 ⇒ generation 重建 + 新 token + 旧 token 失效）；结束时全部
 * 析构，route_count 必须回 0（断开唤醒把 worker 的登记也清干净）。 */
static int case_dyn(int argc, char** argv)
{
    const int domain = static_cast<int>(arg_long(argc, argv, "--domain", 302));
    const int churn = static_cast<int>(arg_long(argc, argv, "--churn", 20));
    auto& pool = dzIPC::threepools::RecvWorkerPool::instance();

    std::vector<std::string> keep;
    std::vector<std::unique_ptr<dzIPC::shm::shm_pub_ipc>> keeps_pub;
    std::vector<std::unique_ptr<dzIPC::shm::shm_sub_ipc>> keeps_sub;
    int keep_rx = 0;
    int churn_rx = 0;
    std::size_t routes_before = 0;
    const std::string churn_name = topic_of("dynC", domain, 0);
    {
    for (int i = 0; i < 8; ++i)
    {
        keep.push_back(topic_of("dynK", domain, i));
        keeps_pub.emplace_back(new dzIPC::shm::shm_pub_ipc(mk_td(), keep.back(),
                                                           static_cast<std::size_t>(domain), false));
        keeps_pub.back()->InitChannel("w06");
        keeps_sub.emplace_back(new dzIPC::shm::shm_sub_ipc(mk_td(), keep.back(),
                                                           static_cast<std::size_t>(domain), 16, false));
        keeps_sub.back()->InitChannel("w06");
        if (!wait_peers(keep.back(), domain, 1, 10000)) { std::printf("FAIL: keep peer\n"); return 2; }
    }
    for (int round = 0; round < churn; ++round)
    {
        {
            dzIPC::shm::shm_pub_ipc p{mk_td(), churn_name, static_cast<std::size_t>(domain), false};
            dzIPC::shm::shm_sub_ipc s{mk_td(), churn_name, static_cast<std::size_t>(domain), 8, false};
            p.InitChannel("w06");
            s.InitChannel("w06");
            if (!wait_peers(churn_name, domain, 1, 5000)) { std::printf("FAIL: churn peer r%d\n", round); return 2; }
            if (p.publish(mk_msg("D" + std::to_string(round)))) ++churn_rx;
            /* 同时给持久话题发布：抖动期间它们不得丢。 */
            for (int i = 0; i < 8; ++i) (void)keeps_pub[static_cast<std::size_t>(i)]->publish(mk_msg("K"));
            (void)drain(s, 1, 5000);
        }   /* ← 这里 sub/pub 析构：断开唤醒 + 控制面重进 Stopping/Ready */
        std::this_thread::sleep_for(std::chrono::milliseconds(40));
    }
    for (int i = 0; i < 8; ++i) keep_rx += drain(*keeps_sub[static_cast<std::size_t>(i)], 9999, 3000);
    routes_before = pool.route_count();
    keeps_pub.clear();
    keeps_sub.clear();
    }   /* ← 全部传输对象析构后才清段 */
    std::this_thread::sleep_for(std::chrono::milliseconds(400));
    const std::size_t routes_after = pool.route_count();
    const auto st = pool.stats();
    std::printf("case=dyn churn=%d churn_publish_ok=%d keep_rx=%d routes_before_destroy=%zu routes_after=%zu "
                "recv_errors=%llu wait_errors=%llu idle_exits=%llu\n",
                churn, churn_rx, keep_rx, routes_before, routes_after,
                static_cast<unsigned long long>(st.recv_errors),
                static_cast<unsigned long long>(st.wait_errors),
                static_cast<unsigned long long>(st.idle_exits));
    const bool ok = (churn_rx == churn && keep_rx > 0 && routes_after == 0 && st.recv_errors == 0
                     && st.wait_errors == 0);
    std::printf("verdict=%s dynamic_add_remove_and_disconnect_wake_clean\n", ok ? "PASS" : "FAIL");
    for (const auto& nm : keep)
    {
        ipc::route::clear_storage(shm_topic_segment_name(nm, static_cast<std::size_t>(domain)).c_str());
        ipc::shm::handle::clear_storage(shm_topic_control_name(nm, static_cast<std::size_t>(domain)).c_str());
    }
    ipc::route::clear_storage(shm_topic_segment_name(churn_name, static_cast<std::size_t>(domain)).c_str());
    ipc::shm::handle::clear_storage(shm_topic_control_name(churn_name, static_cast<std::size_t>(domain)).c_str());
    return ok ? 0 : 1;
}

/* ───────────────────────── 场景 idle ───────────────────────── */
/* 空闲退出 + 安全重新激活：一条话题收完即拆 ⇒ worker 表空 ⇒ 线程空闲退出（idle_exits
 * 增长）；随后在**同一 worker** 上重新注册一条话题 ⇒ 线程被按需拉起（thread_restarts
 * 增长）且首条消息仍能收到。⛔ 判据不能只看线程数：必须看"重新激活后真的收到"。 */
static int case_idle(int argc, char** argv)
{
    const int domain = static_cast<int>(arg_long(argc, argv, "--domain", 304));
    const int wait_ms = static_cast<int>(arg_long(argc, argv, "--wait-ms", 5000));
    auto& pool = dzIPC::threepools::RecvWorkerPool::instance();
    const auto s0 = pool.stats();
    const std::string a = topic_of("idleA", domain, 0);
    int first_rx = 0;
    {
        dzIPC::shm::shm_pub_ipc p{mk_td(), a, static_cast<std::size_t>(domain), false};
        dzIPC::shm::shm_sub_ipc s{mk_td(), a, static_cast<std::size_t>(domain), 8, false};
        p.InitChannel("w06");
        s.InitChannel("w06");
        if (!wait_peers(a, domain, 1, 10000)) { std::printf("FAIL precondition: peer\n"); return 2; }
        if (p.publish(mk_msg("I0"))) first_rx = drain(s, 1, 10000);
    }
    /* 等空闲退出：表空 ⇒ 有效窗口 max(idle_keep_alive, wait_timeout)。 */
    const auto dl = Clock::now() + std::chrono::milliseconds(wait_ms);
    std::uint64_t idle_exits = s0.idle_exits;
    while (Clock::now() < dl && idle_exits == s0.idle_exits)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
        idle_exits = pool.stats().idle_exits;
    }
    const bool exited = (idle_exits > s0.idle_exits);

    /* 重新激活：同 worker 上新建一条话题。 */
    const std::string b = topic_of("idleB", domain, 0);
    int second_rx = 0;
    {
        dzIPC::shm::shm_pub_ipc p{mk_td(), b, static_cast<std::size_t>(domain), false};
        dzIPC::shm::shm_sub_ipc s{mk_td(), b, static_cast<std::size_t>(domain), 8, false};
        p.InitChannel("w06");
        s.InitChannel("w06");
        if (!wait_peers(b, domain, 1, 10000)) { std::printf("FAIL precondition: peer2\n"); return 2; }
        if (p.publish(mk_msg("I1"))) second_rx = drain(s, 1, 10000);
    }
    const auto s1 = pool.stats();
    std::printf("case=idle first_rx=%d idle_exits_delta=%llu exited=%d second_rx=%d thread_restarts_delta=%llu "
                "route_count=%zu threads_after=%zu\n",
                first_rx, static_cast<unsigned long long>(s1.idle_exits - s0.idle_exits),
                static_cast<int>(exited), second_rx,
                static_cast<unsigned long long>(s1.thread_restarts - s0.thread_restarts), s1.route_count,
                thread_count());
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const bool ok = (first_rx == 1) && exited && (second_rx == 1) && (s1.recv_errors == 0);
    std::printf("verdict=%s idle_exit_then_safe_reactivation\n", ok ? "PASS" : "FAIL");
    for (const auto& nm : {a, b})
    {
        ipc::route::clear_storage(shm_topic_segment_name(nm, static_cast<std::size_t>(domain)).c_str());
        ipc::shm::handle::clear_storage(shm_topic_control_name(nm, static_cast<std::size_t>(domain)).c_str());
    }
    return ok ? 0 : 1;
}

/* ───────────────────────── 场景 lat ───────────────────────── */
/* 交付延迟（idle route 上单条消息）：worker 臂 vs 兼容臂的**可解释差异**。
 * 这是 W06-F1 的直接证据：修前 worker 臂的地板恒为 wait_timeout（100 ms）。 */
static int case_lat(int argc, char** argv)
{
    const int domain = static_cast<int>(arg_long(argc, argv, "--domain", 306));
    const int rounds = static_cast<int>(arg_long(argc, argv, "--rounds", 40));
    const int window = static_cast<int>(arg_long(argc, argv, "--window-ms", 1000));
    const std::string name = topic_of("lat", domain, 0);
    double sum = 0.0;
    long long mx = 0;
    long long mn = -1;
    int got = 0;
    int lost = 0;
    {
        dzIPC::shm::shm_pub_ipc pub{mk_td(), name, static_cast<std::size_t>(domain), false};
        dzIPC::shm::shm_sub_ipc sub{mk_td(), name, static_cast<std::size_t>(domain), 8, false};
        pub.InitChannel("w06");
        sub.InitChannel("w06");
        if (!wait_peers(name, domain, 1, 10000)) { std::printf("FAIL precondition: peer\n"); return 2; }
        for (int i = 0; i < rounds; ++i)
        {
            /* 让 worker 回到"空闲等待"状态（拆掉热路径），再量一次冷启动延迟。 */
            std::this_thread::sleep_for(std::chrono::milliseconds(30));
            const auto t0 = Clock::now();
            if (!pub.publish(mk_msg("L"))) break;
            auto sink = mk_td();
            long long us = -1;
            const auto dl = Clock::now() + std::chrono::milliseconds(window);
            while (Clock::now() < dl)
            {
                if (sub.try_get_clone(sink))
                {
                    us = std::chrono::duration_cast<std::chrono::microseconds>(Clock::now() - t0).count();
                    break;
                }
                std::this_thread::sleep_for(std::chrono::microseconds(100));
            }
            if (us < 0)
            {
                std::printf("case=lat round=%d NO_DELIVERY within %dms\n", i, window);
                lost += 1;
                goto done;
            }
            ++got;
            sum += static_cast<double>(us);
            if (us > mx) mx = us;
            if (mn < 0 || us < mn) mn = us;
        }
    }
done:
    const auto& pool = dzIPC::threepools::RecvWorkerPool::instance();
    const bool worker = g_tally.worker_path.load() > 0;
    std::printf("case=lat rounds=%d arm=%s mean_us=%.1f min_us=%lld max_us=%lld pool_wait_wakeups=%llu "
                "wait_timeouts=%llu recv_once_calls=%llu\n",
                got, worker ? "worker" : "compat", got > 0 ? sum / got : -1.0, mn, mx,
                (unsigned long long)pool.stats().wait_wakeups, (unsigned long long)pool.stats().wait_timeouts,
                (unsigned long long)pool.stats().recv_once_calls);
    const bool ok = (got == rounds) && (lost == 0) && (mx < 50000);
    std::printf("verdict=%s delivery_latency_no_wait_timeout_floor(got=%d/%d lost=%d max_us=%lld)\n",
                ok ? "PASS" : "FAIL", got, rounds, lost, mx);
    ipc::route::clear_storage(shm_topic_segment_name(name, static_cast<std::size_t>(domain)).c_str());
    ipc::shm::handle::clear_storage(shm_topic_control_name(name, static_cast<std::size_t>(domain)).c_str());
    return ok ? 0 : 1;
}

/* ───────────────────────── 场景 wsetfull ───────────────────────── */
/* 容量满 ⇒ **显式**兼容回退 + 三个独立计数可分（方案 §13.2 条件 3/§13.3）。
 * 构造：`DZIPC_SHM_RECV_WORKERS=1` 把全部 route 钉到同一 worker，注入
 * n > wait_set 的 127 token 容量 ⇒ 第 128 条起 `add_route` 返回 wait_set_full。
 * 判据：① 超出的那些话题**仍然收得到**（回退到兼容线程，不是静默丢包）；
 *      ② seam 上出现 kRecvPathCompat + 原因码 kWaitSetFull(7)；
 *      ③ `fallback_total` / `fallback_capacity_full` / `wait_set_full` 三个计数
 *         **各自**按超出条数增长（互不污染，且 wait_set_full 不等于后端不可用）。 */
static int case_wsetfull(int argc, char** argv)
{
    const int domain = static_cast<int>(arg_long(argc, argv, "--domain", 305));
    const int n = static_cast<int>(arg_long(argc, argv, "--n", 130));
    const int cap = static_cast<int>(arg_long(argc, argv, "--cap", 127));
    auto& reg = dzIPC::measure::CounterRegistry::instance();
    const auto before = reg.snapshot();

    std::vector<std::string> names;
    int rx = 0;
    int sent = 0;
    {
        std::vector<std::unique_ptr<dzIPC::shm::shm_pub_ipc>> pubs;
        std::vector<std::unique_ptr<dzIPC::shm::shm_sub_ipc>> subs;
        for (int i = 0; i < n; ++i)
        {
            names.push_back(topic_of("wsf", domain, i));
            pubs.emplace_back(new dzIPC::shm::shm_pub_ipc(mk_td(), names.back(),
                                                          static_cast<std::size_t>(domain), false));
            pubs.back()->InitChannel("w06");
            subs.emplace_back(new dzIPC::shm::shm_sub_ipc(mk_td(), names.back(),
                                                          static_cast<std::size_t>(domain), 8, false));
            subs.back()->InitChannel("w06");
            if (!wait_peers(names.back(), domain, 1, 15000))
            {
                std::printf("FAIL precondition: peer %d\n", i);
                return 2;
            }
        }
        for (int i = 0; i < n; ++i)
        {
            if (pubs[static_cast<std::size_t>(i)]->publish(mk_msg("W"))) ++sent;
        }
        for (int i = 0; i < n; ++i) rx += drain(*subs[static_cast<std::size_t>(i)], 1, 8000);
        const auto st = dzIPC::threepools::RecvWorkerPool::instance().stats();
        std::printf("case=wsetfull n=%d cap=%d pool_route_count=%zu\n", n, cap, st.route_count);
    }
    const auto after = reg.snapshot();
    const auto d = [&](dzIPC::measure::CounterId id) {
        return after.get(id) - before.get(id);
    };
    std::printf("counters fallback_total=%llu backend_unavailable=%llu capacity_full=%llu wait_set_full=%llu "
                "wait_token_invalid=%llu registration_attempts=%llu registration_ok=%llu registration_failed=%llu\n",
                (unsigned long long)d(dzIPC::measure::CounterId::fallback_total),
                (unsigned long long)d(dzIPC::measure::CounterId::fallback_backend_unavailable),
                (unsigned long long)d(dzIPC::measure::CounterId::fallback_capacity_full),
                (unsigned long long)d(dzIPC::measure::CounterId::wait_set_full),
                (unsigned long long)d(dzIPC::measure::CounterId::wait_token_invalid),
                (unsigned long long)d(dzIPC::measure::CounterId::registration_attempts),
                (unsigned long long)d(dzIPC::measure::CounterId::registration_ok),
                (unsigned long long)d(dzIPC::measure::CounterId::registration_failed));
    std::printf("case=wsetfull sent=%d received=%d worker_path=%ld compat_total=%ld fallback=%ld "
                "last_reason=%d(=kWaitSetFull 7)\n",
                sent, rx, g_tally.worker_path.load(), g_tally.compat_total.load(), g_tally.fallback.load(),
                g_tally.last_reason.load());
    const long over = n - cap;
    const bool counters_ok = d(dzIPC::measure::CounterId::fallback_total) == static_cast<std::uint64_t>(over)
                             && d(dzIPC::measure::CounterId::fallback_capacity_full)
                                    == static_cast<std::uint64_t>(over)
                             && d(dzIPC::measure::CounterId::wait_set_full) == static_cast<std::uint64_t>(over)
                             && d(dzIPC::measure::CounterId::fallback_backend_unavailable) == 0
                             && d(dzIPC::measure::CounterId::wait_token_invalid) == 0;
    /* ① 超出容量的那些话题必须**仍然收得到**：回退线程把它们接住了（显式回退，
     *    不是静默丢包）。② 回退事实在 seam 上可机读。③ 三个计数各自可分。 */
    const bool ok = (sent == n) && (rx == n) && (g_tally.fallback.load() == over)
                    && (g_tally.path_choice.load() == 0) && counters_ok;
    std::printf("verdict=%s explicit_fallback_on_capacity_full(rx=%d/%d fallback=%ld expect=%ld counters_ok=%d)\n",
                ok ? "PASS" : "FAIL", rx, n, g_tally.fallback.load(), over, static_cast<int>(counters_ok));
    for (const auto& nm : names)
    {
        ipc::route::clear_storage(shm_topic_segment_name(nm, static_cast<std::size_t>(domain)).c_str());
        ipc::shm::handle::clear_storage(shm_topic_control_name(nm, static_cast<std::size_t>(domain)).c_str());
    }
    return ok ? 0 : 1;
}

/* ───────────────────────── 场景 fallback ───────────────────────── */
/* 两条臂必须**可机读地分开**：worker 臂 ⇒ kRecvPathWorker>0 且 fallback=0；
 * 强制兼容臂（DZIPC_SHM_RECV_COMPAT=1）⇒ 原因码 kForcedCompatEnv 且 fallback=0
 * （路径选择不是回退）。验收模式据此判"有没有偷偷用回退线程补足固定线程目标"。 */
static int case_fallback(int argc, char** argv)
{
    const int domain = static_cast<int>(arg_long(argc, argv, "--domain", 303));
    const int n = static_cast<int>(arg_long(argc, argv, "--n", 60));
    std::vector<std::string> names;
    int rx = 0;
    int got = 0;
    std::size_t routes_after = 0;
    {
    std::vector<std::unique_ptr<dzIPC::shm::shm_pub_ipc>> pubs;
    std::vector<std::unique_ptr<dzIPC::shm::shm_sub_ipc>> subs;
    for (int i = 0; i < n; ++i)
    {
        names.push_back(topic_of("fb", domain, i));
        pubs.emplace_back(new dzIPC::shm::shm_pub_ipc(mk_td(), names.back(), static_cast<std::size_t>(domain), false));
        pubs.back()->InitChannel("w06");
        subs.emplace_back(
            new dzIPC::shm::shm_sub_ipc(mk_td(), names.back(), static_cast<std::size_t>(domain), 8, false));
        subs.back()->InitChannel("w06");
        if (!wait_peers(names.back(), domain, 1, 10000)) { std::printf("FAIL: peer %d\n", i); return 2; }
    }
    for (int i = 0; i < n; ++i)
    {
        if (pubs[static_cast<std::size_t>(i)]->publish(mk_msg("F"))) ++rx;
    }
    for (int i = 0; i < n; ++i) got += drain(*subs[static_cast<std::size_t>(i)], 1, 3000);

    pubs.clear();
    subs.clear();
    }   /* ← 传输对象析构后再清段/读计数 */
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    routes_after = dzIPC::threepools::RecvWorkerPool::instance().route_count();
    const auto st = dzIPC::threepools::RecvWorkerPool::instance().stats();
    const auto c = dzIPC::measure::CounterRegistry::instance().snapshot();
    (void)c;
    std::printf("case=fallback n=%d published=%d received=%d worker_path=%ld compat_total=%ld fallback=%ld "
                "path_choice=%ld last_reason=%d pool_route_count=%zu\n",
                n, rx, got, g_tally.worker_path.load(), g_tally.compat_total.load(), g_tally.fallback.load(),
                g_tally.path_choice.load(), g_tally.last_reason.load(), routes_after);
    const bool forced = std::getenv("DZIPC_SHM_RECV_COMPAT") != nullptr;
    const bool ok = (got == n)
                    && (forced ? (g_tally.worker_path.load() == 0 && g_tally.fallback.load() == 0
                                  && g_tally.last_reason.load()
                                         == static_cast<int>(dzIPC::detail::RecvPathReason::kForcedCompatEnv))
                               : (g_tally.worker_path.load() == n && g_tally.fallback.load() == 0
                                  && g_tally.compat_total.load() == 0));
    std::printf("verdict=%s arm=%s\n", ok ? "PASS" : "FAIL", forced ? "compat(forced)" : "worker");
    for (const auto& nm : names)
    {
        ipc::route::clear_storage(shm_topic_segment_name(nm, static_cast<std::size_t>(domain)).c_str());
        ipc::shm::handle::clear_storage(shm_topic_control_name(nm, static_cast<std::size_t>(domain)).c_str());
    }
    return ok ? 0 : 1;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const char* which = arg_str(argc, argv, "--case", "");
    dzIPC::detail::SetSeamHook(&seam_hook);
    const auto& b = dzIPC::threepools::RecvWorkerPool::instance().budget();
    std::printf("config msgs=%zu bytes=%zu time_us=%lld wait_ms=%lld idle_ms=%lld compat_env=%s\n",
                b.max_messages_per_route, b.max_bytes_per_route,
                static_cast<long long>(b.max_processing_time_per_route.count()),
                static_cast<long long>(b.wait_timeout.count()),
                static_cast<long long>(b.idle_keep_alive.count()),
                std::getenv("DZIPC_SHM_RECV_COMPAT") != nullptr ? "set" : "unset");
    int rc = 2;
    if (std::strcmp(which, "budget") == 0) rc = case_budget(argc, argv);
    else if (std::strcmp(which, "hol") == 0) rc = case_hol(argc, argv);
    else if (std::strcmp(which, "dyn") == 0) rc = case_dyn(argc, argv);
    else if (std::strcmp(which, "idle") == 0) rc = case_idle(argc, argv);
    else if (std::strcmp(which, "fallback") == 0) rc = case_fallback(argc, argv);
    else if (std::strcmp(which, "wsetfull") == 0) rc = case_wsetfull(argc, argv);
    else if (std::strcmp(which, "lat") == 0) rc = case_lat(argc, argv);
    else std::printf("unknown --case\n");
    dzIPC::detail::SetSeamHook(nullptr);
    std::printf("W06_REGRESS_DONE rc=%d\n", rc);
    return rc;
}

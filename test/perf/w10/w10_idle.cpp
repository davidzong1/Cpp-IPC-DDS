/* W10 空闲成本与"忙转"判别探针（§10.1 三态 / §10.2 扫描成本 / W04-F3 假就绪）。
 *
 * 判据：
 *   状态 3（已连接、有效订阅、停止发布）下 worker 必须**阻塞**在等待层，
 *   ⛔ 不得忙转。区分方式不是只看 CPU，而是同时看 worker 自己的计数：
 *     · 忙转 ⇒ recv_once_calls / deferred_drains 随窗口线性暴涨，且 wait_timeouts≈0
 *     · 阻塞 ⇒ wait_timeouts ≈ N_worker × 窗口/切片，recv_once_calls 基本不增长
 *   ctx 逐 TID 聚合（⛔ 不用 /proc/self/status）；CPU 用 /proc/self/stat（whole=1，覆盖线程组）。
 *
 * 用法：w10_idle --n 1000 --domain 930 [--state 3] [--windows 3] [--win-s 5] [--out <dir>]
 *   --state 1 : 无注册 route（析构全部订阅后）
 *   --state 2 : 已注册未连接（构造订阅但发布端从未建链——用另一个 domain 的发布端做不到，
 *               故 state 2 由"只建 sub、不建 pub"近似，且逐 route 记录 peer_count）
 *   --state 3 : 已连接有效订阅 + 停止发布（默认）
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include "dzIPC/common/control_plane.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/threepools/recv_worker.h"
#include "dzIPC/threepools/shm_control_scheduler.h"
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

static double proc_cpu_ticks()
{
    std::ifstream f("/proc/self/stat");
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
        if (i == 13) u = std::strtoull(t.c_str(), nullptr, 10);
        else if (i == 14) s = std::strtoull(t.c_str(), nullptr, 10);
        ++i;
    }
    return u + s;
}

struct Ctx
{
    unsigned long long vol{0}, nonvol{0};
    int unreadable{0};
};
static Ctx ctxsum()
{
    Ctx o;
    DIR* d = ::opendir("/proc/self/task");
    if (!d) return o;
    while (dirent* e = ::readdir(d))
    {
        if (e->d_name[0] == '.') continue;
        char p[300];
        std::snprintf(p, sizeof p, "/proc/self/task/%s/status", e->d_name);
        std::ifstream f(p);
        if (!f) { ++o.unreadable; continue; }
        std::string l;
        while (std::getline(f, l))
        {
            if (l.rfind("voluntary_ctxt_switches:", 0) == 0) o.vol += std::strtoull(l.c_str() + 24, nullptr, 10);
            else if (l.rfind("nonvoluntary_ctxt_switches:", 0) == 0) o.nonvol += std::strtoull(l.c_str() + 27, nullptr, 10);
        }
    }
    ::closedir(d);
    return o;
}
static std::size_t thread_count()
{
    DIR* d = ::opendir("/proc/self/task");
    if (!d) return 0;
    std::size_t n = 0;
    while (dirent* e = ::readdir(d)) if (e->d_name[0] != '.') ++n;
    ::closedir(d);
    return n;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const long n = arg_long(argc, argv, "--n", 1000);
    const long domain = arg_long(argc, argv, "--domain", 930);
    const int state = static_cast<int>(arg_long(argc, argv, "--state", 3));
    const int windows = static_cast<int>(arg_long(argc, argv, "--windows", 3));
    const double win_s = arg_long(argc, argv, "--win-s", 5);
    const std::string out = arg_str(argc, argv, "--out", "");
    /* t45 仲裁用：topic 名前缀可配（默认 idle_shm_）。**只改名字模板**，不动窗口与采样口径。 */
    const std::string tprefix = arg_str(argc, argv, "--topic-prefix", "idle_shm_");
    /* ⛔ 逐 TID ctx 采样本身要读 34 个 /proc 文件/次，会把 CPU 读数顶高。
     * --cpu-only 只测 CPU（用于把"产品空闲成本"与"观测器成本"分开）。 */
    bool cpu_only = false;
    for (int i = 1; i < argc; ++i) if (std::strcmp(argv[i], "--cpu-only") == 0) cpu_only = true;

    auto td = [] { return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId); };
    std::vector<std::string> names;
    std::vector<std::unique_ptr<dzIPC::shm::shm_sub_ipc>> subs;
    std::vector<std::unique_ptr<dzIPC::shm::shm_pub_ipc>> pubs;
    for (long i = 0; i < n; ++i)
    {
        names.push_back(tprefix + std::to_string(domain) + "_" + std::to_string(i));
        if (state == 3)
        {
            pubs.emplace_back(new dzIPC::shm::shm_pub_ipc(td(), names.back(), static_cast<std::size_t>(domain), false));
            pubs.back()->InitChannel("idle");
        }
        subs.emplace_back(new dzIPC::shm::shm_sub_ipc(td(), names.back(), static_cast<std::size_t>(domain), 64, false));
        subs.back()->InitChannel("idle");
    }
    auto& pool = dzIPC::threepools::RecvWorkerPool::instance();
    /* state 3 需要控制面真的握手完成；state 2 只建 sub（无 peer）。 */
    if (state == 3)
    {
        const auto dl = Clock::now() + std::chrono::seconds(120);
        long ok = 0;
        while (Clock::now() < dl && ok < n)
        {
            ok = 0;
            for (long i = 0; i < n; ++i)
            {
                dzIPC::control_plane_shm::TopicControlPlane cp;
                if (cp.open(shm_topic_control_name(names[static_cast<std::size_t>(i)], static_cast<std::size_t>(domain)))
                    && cp.peer_count() >= 1) ++ok;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::printf("handshake_ok=%ld/%ld\n", ok, n);
    }
    std::printf("state=%d n=%ld domain=%ld pool running=%d workers=%zu route_count=%zu threads=%zu\n", state, n, domain,
                static_cast<int>(pool.running()), pool.worker_count(), pool.route_count(), thread_count());
    std::printf("window,state,wall_s,threads,cpu_cores,ctx_per_s,ctx_per_route_per_s,unreadable_tids,"
                "wait_wakeups,wait_timeouts,budget_yields,deferred_drains,recv_once_calls,recv_errors\n");
    std::this_thread::sleep_for(std::chrono::seconds(2));

    if (state == 1)
    {
        /* 状态 1（无注册 route）：先析构全部订阅与发布端，再进测量窗口。 */
        subs.clear();
        pubs.clear();
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
    }

    for (int w = 0; w < windows; ++w)
    {
        const auto s0 = pool.stats();
        const auto sch0 = dzIPC::shm_control::ShmControlScheduler::instance().stats();
        const double c0 = proc_cpu_ticks();
        const Ctx x0 = cpu_only ? Ctx{} : ctxsum();
        const auto t0 = Clock::now();
        std::this_thread::sleep_for(std::chrono::milliseconds(static_cast<long>(win_s * 1000)));
        const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
        const auto s1 = pool.stats();
        const auto sch1 = dzIPC::shm_control::ShmControlScheduler::instance().stats();
        const double c1 = proc_cpu_ticks();
        const Ctx x1 = cpu_only ? Ctx{} : ctxsum();
        const double ctx = static_cast<double>((x1.vol - x0.vol) + (x1.nonvol - x0.nonvol)) / wall;
        std::printf("%d,%d,%.3f,%zu,%.5f,%.1f,%.4f,%d,%llu,%llu,%llu,%llu,%llu,%llu\n", w, state, wall, thread_count(),
                    (c1 - c0) / 100.0 / wall, ctx, ctx / static_cast<double>(n), x0.unreadable + x1.unreadable,
                    static_cast<unsigned long long>(s1.wait_wakeups - s0.wait_wakeups),
                    static_cast<unsigned long long>(s1.wait_timeouts - s0.wait_timeouts),
                    static_cast<unsigned long long>(s1.budget_yields - s0.budget_yields),
                    static_cast<unsigned long long>(s1.deferred_drains - s0.deferred_drains),
                    static_cast<unsigned long long>(s1.recv_once_calls - s0.recv_once_calls),
                    static_cast<unsigned long long>(s1.recv_errors - s0.recv_errors));
        std::printf("   scheduler entries=%zu tick_delta=%llu tick_wakes_per_s=%.1f tick_max_us=%.2f overruns=%llu\n",
                    sch1.entry_count,
                    static_cast<unsigned long long>(sch1.tick_count - sch0.tick_count),
                    static_cast<double>(sch1.tick_count - sch0.tick_count) / wall,
                    static_cast<double>(sch1.tick_duration_max_ns) / 1000.0,
                    static_cast<unsigned long long>(sch1.tick_overrun_count));
    }

    if (state == 1)
    {
        const auto st = pool.stats();
        std::printf("state1_after_destroy route_count=%zu threads=%zu idle_exits=%llu restarts=%llu\n", pool.route_count(),
                    thread_count(), static_cast<unsigned long long>(st.idle_exits),
                    static_cast<unsigned long long>(st.thread_restarts));
    }
    if (!out.empty())
    {
        std::ofstream f(out);
        f << "state=" << state << " n=" << n << " windows=" << windows << " win_s=" << win_s << "\n";
    }
    std::printf("W10_IDLE_DONE\n");
    return 0;
}

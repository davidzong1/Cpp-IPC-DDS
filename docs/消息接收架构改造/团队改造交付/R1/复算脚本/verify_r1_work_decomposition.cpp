/* t32 独立复算（**决定性分解**）：把门槛 3 稳态 CPU 的两个候选成本源**在同一进程内**
 * 用各自的原生计数直接量化，避免「关池」反事实的混淆（关池会换成 1000 条兼容收包线程）：
 *   · 控制面：tick 次数 × 在册项数（每次 tick 至少一次 wait_for_due 全扫 + 一次选取全扫 + sort）
 *   · 接收池：RecvWorkerStats::scanned_routes_total（每次唤醒对**本 worker 在册 route** 的一次全扫）
 * 两者都是「项访问次数/s」，量级可比 ⇒ 直接判定谁是主成本。
 * 用法: decomp <n_topics> <win_s> */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <fstream>
#include <sstream>
#include "dzIPC/common/control_plane.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/threepools/recv_worker.h"
#include "dzIPC/threepools/shm_control_scheduler.h"
#include "ipc_msg/std_msgs/std_string.hpp"
static double cpu_jiffies(){ std::ifstream f("/proc/self/stat"); std::string b; std::getline(f,b);
  auto q=b.rfind(')'); if(q==std::string::npos) return -1; std::istringstream is(b.substr(q+2)); std::string t;
  int i=2; double u=0,s=0; while(is>>t){ if(i==13)u=strtoull(t.c_str(),0,10); else if(i==14)s=strtoull(t.c_str(),0,10); ++i;} return u+s; }
int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const int n = argc > 1 ? std::atoi(argv[1]) : 1000;
    const double win = argc > 2 ? std::atof(argv[2]) : 60.0;
    const int dom = 52000 + (int)(::getpid() % 400);
    auto td = [] { return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 93); };
    std::vector<std::string> names;
    std::vector<std::unique_ptr<dzIPC::shm::shm_pub_ipc>> pubs;
    std::vector<std::unique_ptr<dzIPC::shm::shm_sub_ipc>> subs;
    for (int i = 0; i < n; ++i)
    {
        names.push_back("decomp_" + std::to_string(dom) + "_" + std::to_string(i));
        pubs.emplace_back(new dzIPC::shm::shm_pub_ipc(td(), names.back(), dom, false));
        pubs.back()->InitChannel("dc");
        subs.emplace_back(new dzIPC::shm::shm_sub_ipc(td(), names.back(), dom, 64, false));
        subs.back()->InitChannel("dc");
    }
    long ok = 0; const auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(150);
    while (std::chrono::steady_clock::now() < dl && ok < n)
    {
        ok = 0;
        for (int i = 0; i < n; ++i)
        {
            dzIPC::control_plane_shm::TopicControlPlane cp;
            if (cp.open(shm_topic_control_name(names[i], (std::size_t)dom)) && cp.peer_count() >= 1) ++ok;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
    auto& pool = dzIPC::threepools::RecvWorkerPool::instance();
    auto& sched = dzIPC::shm_control::ShmControlScheduler::instance();
    const auto p0 = pool.stats(); const auto s0 = sched.stats(); const double c0 = cpu_jiffies();
    std::this_thread::sleep_for(std::chrono::duration<double>(win));
    const auto p1 = pool.stats(); const auto s1 = sched.stats(); const double c1 = cpu_jiffies();
    const double ticks  = (double)(s1.tick_count - s0.tick_count) / win;
    const std::size_t entries = sched.entry_count();
    const double ctl_visits = ticks * (double)entries * 2.0;   /* wait_for_due 全扫 + tick 选取全扫 */
    const double pool_visits = (double)(p1.scanned_routes_total - p0.scanned_routes_total) / win;
    const double core = (c1 - c0) / 100.0 / win;
    std::printf("CONFIG valid=%ld/%d entries=%zu pool_routes=%zu workers=%zu threads=%zu\n",
                ok, n, entries, pool.route_count(), pool.worker_count(), (size_t)0);
    std::printf("WIN %.0fs cpu=%.4f core\n", win, core);
    std::printf("ORG 控制面: ticks=%.1f/s × entries=%zu × 2 = %.3e 项访问/s\n", ticks, entries, ctl_visits);
    std::printf("ORG 接收池: scan_rounds=%.1f/s scanned_routes_total=%.1f/s = %.3e 项访问/s\n",
                (double)(p1.scan_rounds - p0.scan_rounds) / win, pool_visits, pool_visits);
    std::printf("ORG 比值 控制面/接收池 = %.1f×\n", pool_visits > 0 ? ctl_visits / pool_visits : 0.0);
    std::printf("ORG 若全部成本归一为「项访问」: %.3e core·s/项 (用控制面口径)\n", ctl_visits > 0 ? core / ctl_visits : 0.0);
    std::printf("ORG 接收池按其占比应分摊 = %.5f core（实际总 %.4f）\n",
                core * (pool_visits / (ctl_visits + pool_visits)), core);
    std::printf("POOLSTATS wait_wakeups=%.1f/s wait_timeouts=%.1f/s recv_once_calls=%.1f/s idle_exits_delta=%llu\n",
                (double)(p1.wait_wakeups - p0.wait_wakeups)/win, (double)(p1.wait_timeouts - p0.wait_timeouts)/win,
                (double)(p1.recv_once_calls - p0.recv_once_calls)/win, (unsigned long long)(p1.idle_exits - p0.idle_exits));
    std::fflush(stdout);
    std::_Exit(0);
}

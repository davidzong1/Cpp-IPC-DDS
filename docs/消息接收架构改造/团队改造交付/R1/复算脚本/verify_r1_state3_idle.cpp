/* t32 独立复算：门槛 3 冻结配置（1000 有效订阅 + 无发布）下，
 * RecvWorkerStats 的**非门控**量在 60 s 窗口内的增量 —— 特别是 idle_exits / thread_restarts。
 * 为什么需要：r25 的 w10_idle 只在 state==1 打印 idle_exits（w10_idle.cpp:200-205），
 * state 3 的 idle_exits **无原始读数**，而 R-2 要把它写进门槛判据。
 * 用法: state3idle <n> <win_s> */
#include <chrono>
#include <unistd.h>
#include <cstdio>
#include <cstdlib>
#include <memory>
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
int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const int n = argc > 1 ? std::atoi(argv[1]) : 1000;
    const double win = argc > 2 ? std::atof(argv[2]) : 60.0;
    const int dom = 45000 + (int)(::getpid() % 500);
    auto td = [] { return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 93); };
    std::vector<std::string> names;
    std::vector<std::unique_ptr<dzIPC::shm::shm_pub_ipc>> pubs;
    std::vector<std::unique_ptr<dzIPC::shm::shm_sub_ipc>> subs;
    for (int i = 0; i < n; ++i)
    {
        names.push_back("s3idle_" + std::to_string(dom) + "_" + std::to_string(i));
        pubs.emplace_back(new dzIPC::shm::shm_pub_ipc(td(), names.back(), dom, false));
        pubs.back()->InitChannel("s3");
        subs.emplace_back(new dzIPC::shm::shm_sub_ipc(td(), names.back(), dom, 64, false));
        subs.back()->InitChannel("s3");
    }
    /* 等控制面握手完成（= 门槛 3 的「有效订阅」）。 */
    long ok = 0; const auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(120);
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
    const auto a = pool.stats(); const auto sa = sched.stats();
    std::this_thread::sleep_for(std::chrono::duration<double>(win));
    const auto b = pool.stats(); const auto sb = sched.stats();
    std::printf("CONFIG valid_rx=%ld/%d pool_routes=%zu entries=%zu workers=%zu\n", ok, n, pool.route_count(), sched.entry_count(), pool.worker_count());
    std::printf("WIN %.0fs | idle_exits_delta=%llu thread_restarts_delta=%llu | wait_wakeups=%llu wait_timeouts=%llu recv_once_calls=%llu recv_errors=%llu\n",
        win, (unsigned long long)(b.idle_exits - a.idle_exits), (unsigned long long)(b.thread_restarts - a.thread_restarts),
        (unsigned long long)(b.wait_wakeups - a.wait_wakeups), (unsigned long long)(b.wait_timeouts - a.wait_timeouts),
        (unsigned long long)(b.recv_once_calls - a.recv_once_calls), (unsigned long long)(b.recv_errors - a.recv_errors));
    std::printf("WIN %.0fs | sched tick_delta=%llu wakes/s=%.1f tick_max_us=%.1f overruns=%llu\n", win,
        (unsigned long long)(sb.tick_count - sa.tick_count), (double)(sb.tick_count - sa.tick_count)/win,
        (double)sb.tick_duration_max_ns/1000.0, (unsigned long long)sb.tick_overrun_count);
    std::printf("VERDICT idle_exits==0 ? %s ; thread_restarts==0 ? %s\n",
        ((b.idle_exits - a.idle_exits) == 0) ? "YES" : "NO",
        ((b.thread_restarts - a.thread_restarts) == 0) ? "YES" : "NO");
    std::fflush(stdout);
    std::_Exit(0);
}

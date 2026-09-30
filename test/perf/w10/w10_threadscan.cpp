/* W10 空闲成本归因探针：在**进程内**按 TID 统计 CPU，把空闲 core 拆到
 * 「控制面 worker」/「RecvWorker 池」/「主线程」/「测试编排线程」四类上。
 *
 * 用途：门槛 3（空闲 CPU/切换）与既有缺陷 5（W04-F3 假就绪忙转）都必须能
 * 归因到具体线程，否则"空闲 CPU 超标"是不可定位的。
 *
 * 本探针自身在主线程里每 50 ms 读一次 每个 TID 的 /proc stat（34 个文件），
 * 因此会把一部分 CPU 记到 selfscan 线程上 —— 判读时必须先扣掉这一项（报告里单列）。
 *
 * 用法：w10_threadscan <n_topics> <domain> <window_s>
 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <sys/syscall.h>

#include "dzIPC/common/control_plane.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/threepools/recv_worker.h"
#include "ipc_msg/std_msgs/std_string.hpp"

static std::map<int, unsigned long long> tid_cpu()
{
    std::map<int, unsigned long long> m;
    DIR* d = ::opendir("/proc/self/task");
    if (!d) return m;
    while (dirent* e = ::readdir(d))
    {
        if (e->d_name[0] == '.') continue;
        char p[320];
        std::snprintf(p, sizeof p, "/proc/self/task/%s/stat", e->d_name);
        std::ifstream f(p);
        if (!f) continue;
        std::string b;
        std::getline(f, b);
        const auto q = b.rfind(')');
        if (q == std::string::npos) continue;
        std::istringstream is(b.substr(q + 2));
        std::string t;
        int i = 2;
        unsigned long long u = 0, s = 0;
        while (is >> t)
        {
            if (i == 13) u = std::strtoull(t.c_str(), nullptr, 10);
            else if (i == 14) s = std::strtoull(t.c_str(), nullptr, 10);
            ++i;
        }
        m[std::atoi(e->d_name)] = u + s;
    }
    ::closedir(d);
    return m;
}

static std::string comm(int tid)
{
    char p[320];
    std::snprintf(p, sizeof p, "/proc/self/task/%d/comm", tid);
    std::ifstream f(p);
    std::string s;
    std::getline(f, s);
    return s;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const int n = argc > 1 ? std::atoi(argv[1]) : 1000;
    const int dom = argc > 2 ? std::atoi(argv[2]) : 7200;
    const double win = argc > 3 ? std::atof(argv[3]) : 10.0;

    auto td = [] { return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 93); };
    std::vector<std::string> names;
    std::vector<std::unique_ptr<dzIPC::shm::shm_sub_ipc>> subs;
    std::vector<std::unique_ptr<dzIPC::shm::shm_pub_ipc>> pubs;
    for (int i = 0; i < n; ++i)
    {
        names.push_back("ts_" + std::to_string(dom) + "_" + std::to_string(i));
        pubs.emplace_back(new dzIPC::shm::shm_pub_ipc(td(), names.back(), static_cast<std::size_t>(dom), false));
        pubs.back()->InitChannel("ts");
        subs.emplace_back(new dzIPC::shm::shm_sub_ipc(td(), names.back(), static_cast<std::size_t>(dom), 64, false));
        subs.back()->InitChannel("ts");
    }
    {
        const auto dl = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        long ok = 0;
        while (std::chrono::steady_clock::now() < dl && ok < n)
        {
            ok = 0;
            for (int i = 0; i < n; ++i)
            {
                dzIPC::control_plane_shm::TopicControlPlane cp;
                if (cp.open(shm_topic_control_name(names[static_cast<std::size_t>(i)], static_cast<std::size_t>(dom)))
                    && cp.peer_count() >= 1) ++ok;
            }
            if (ok < n) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    std::this_thread::sleep_for(std::chrono::seconds(2));
    const auto a = tid_cpu();
    std::this_thread::sleep_for(std::chrono::duration<double>(win));
    const auto b = tid_cpu();
    /* ⛔ 只统计**窗口开始就存在**的 TID：窗口内新建的线程若按"全生命周期 CPU"计入，
     * 会把创建前的成本算进窗口（实测过这个假读数）。窗口内新建/退出分开报。 */
    std::map<int, unsigned long long> per_tid;
    unsigned long long total = 0;
    int new_tids = 0, gone_tids = 0;
    for (const auto& kv : b)
    {
        const auto it = a.find(kv.first);
        if (it == a.end()) { ++new_tids; continue; }
        const unsigned long long d = kv.second - it->second;
        per_tid[kv.first] = d;
        total += d;
    }
    for (const auto& kv : a) if (b.find(kv.first) == b.end()) ++gone_tids;
    const int self_tid = static_cast<int>(::syscall(SYS_gettid));
    std::printf("n=%d win=%.0fs total_cpu=%.5f core (%llu ticks)  threads=%zu  pid=%d main_tid=%d  "
                "tids_new_in_window=%d tids_gone_in_window=%d\n",
                n, win, total / 100.0 / win, total, per_tid.size(), (int)::getpid(), self_tid, new_tids, gone_tids);
    for (const auto& kv : per_tid)
        if (kv.second > 0)
            std::printf("   tid=%-7d %-24s %.5f core%s\n", kv.first, comm(kv.first).c_str(),
                        kv.second / 100.0 / win, kv.first == self_tid ? "   <- 主线程(本探针自身读数开销)" : "");
    std::fflush(stdout);
    std::_Exit(0);
}

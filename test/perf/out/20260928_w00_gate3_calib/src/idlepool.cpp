/* 真实产品池的空闲成本标定（门槛 3 可达性）：N 个独立话题 socket 订阅（修正同 topic 缺陷），
 * 静默 W 秒，用 /proc/self/stat(utime+stime，整进程线程组) 记 CPU，
 * 用逐 TID /proc/self/task/<tid>/status 汇总记 ctx。 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include "dzIPC/common/topic_data.h"
#include "dzIPC/socket_pub_sub_ipc.h"
#include "dzIPC/threepools/socket_recv_worker.h"
#include "dzIPC/ipc_info_pool.h"
#include "ipc_msg/std_msgs/std_image.hpp"

static double stat_cpu(const char* path) {
    FILE* f = fopen(path, "r"); if (!f) return -1; char b[8192];
    if (!fgets(b, sizeof b, f)) { fclose(f); return -1; } fclose(f);
    char* q = strrchr(b, ')'); if (!q) return -1; q++;
    int i = 2; char* t = strtok(q, " "); double u = 0, st = 0;
    while (t) { if (i == 13) u = strtoull(t, 0, 10); else if (i == 14) st = strtoull(t, 0, 10); t = strtok(NULL, " "); i++; }
    return u + st;
}
static unsigned long long ctxsum() {
    DIR* d = opendir("/proc/self/task"); unsigned long long s = 0; if (!d) return 0;
    struct dirent* e;
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        char p[256]; snprintf(p, sizeof p, "/proc/self/task/%s/status", e->d_name);
        FILE* f = fopen(p, "r"); if (!f) continue; char l[512];
        while (fgets(l, sizeof l, f))
            if (!strncmp(l, "voluntary_ctxt_switches:", 24) || !strncmp(l, "nonvoluntary_ctxt_switches:", 27))
                s += strtoull(strchr(l, ':') + 1, 0, 10);
        fclose(f);
    }
    closedir(d); return s;
}
static size_t fd_count(size_t* mx){ DIR* d=opendir("/proc/self/fd"); if(!d) return 0; size_t n=0, m=0; struct dirent* e;
  while((e=readdir(d))){ if(e->d_name[0]=='.') continue; ++n; long v=strtol(e->d_name,0,10); if(v>0 && (size_t)v>m) m=(size_t)v; } closedir(d); if(mx) *mx=m; return n; }
static size_t thread_count() {
    DIR* d = opendir("/proc/self/task"); if (!d) return 0; size_t n = 0;
    while (dirent* e = readdir(d)) if (e->d_name[0] != '.') ++n;
    closedir(d); return n;
}
int main(int argc, char** argv) {
    const int n = argc > 1 ? atoi(argv[1]) : 200;
    const int dom = argc > 2 ? atoi(argv[2]) : 666;
    const double win = argc > 3 ? atof(argv[3]) : 10.0;
    std::printf("mode=socket_pub_sub_independent_topics n=%d domain=%d\n", n, dom);
    std::vector<std::unique_ptr<dzIPC::socket::socket_sub_ipc>> subs;
    subs.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        auto td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 37);
        subs.push_back(std::make_unique<dzIPC::socket::socket_sub_ipc>(
            td, "idle_" + std::to_string(i), static_cast<size_t>(dom), 1024, false));
        subs.back()->InitChannel("idle");
    }
    auto& pool = dzIPC::threepools::SocketRecvWorkerPool::instance();
    { auto snap = dzIPC::info_pool::IpcInfoPool::instance().snapshot(false);
      size_t inuse=0; for (auto& e : snap) if (e.in_use) ++inuse;
      std::printf("info_pool_snapshot_size=%zu in_use=%zu (expect n+1=%d if registry unbounded)\n", snap.size(), inuse, n+1); }
    size_t mxfd=0; size_t fds=fd_count(&mxfd);
    std::printf("pool_running=%d workers=%zu route_count=%zu threads_now=%zu fds_open=%zu max_fd=%zu\n",
                (int)pool.running(), pool.worker_count(), pool.route_count(), thread_count(), fds, mxfd);
    long hz = sysconf(_SC_CLK_TCK);
    double a = stat_cpu("/proc/self/stat"); unsigned long long c0 = ctxsum();
    size_t tmin = thread_count(), tmax = tmin;
    auto w0 = std::chrono::steady_clock::now();
    for (double e = 0; e < win; e += 0.25) {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        size_t t = thread_count(); if (t < tmin) tmin = t; if (t > tmax) tmax = t;
    }
    auto w1 = std::chrono::steady_clock::now();
    double b = stat_cpu("/proc/self/stat"); unsigned long long c1 = ctxsum();
    double wall = std::chrono::duration<double>(w1 - w0).count();
    auto st = pool.stats();
    std::printf("window_s=%.3f cpu_cores=%.5f ctx_per_s=%.1f ctx_per_route_per_s=%.3f\n",
                wall, (b - a) / hz / wall, (c1 - c0) / wall, ((c1 - c0) / wall) / (n ? n : 1));
    std::printf("threads_min=%zu threads_max=%zu route_count=%zu idle_exits=%llu wait_timeouts=%llu\n",
                tmin, tmax, pool.route_count(), (unsigned long long)st.idle_exits,
                (unsigned long long)st.wait_timeouts);
    std::printf("DONE\n");
    std::fflush(stdout);
    _exit(0);
}

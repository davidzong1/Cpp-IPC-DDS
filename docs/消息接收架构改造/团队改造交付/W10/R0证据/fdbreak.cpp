/* t34/R0 证据：fd 三类分账实测（pub / sub / 固定池开销）。
 * 用 /proc/self/fd 计数（与 r25 工装 fd_count 同口径：opendir + 数字名条目）。
 * 用法: fdbreak <mode:pub|sub|both> <n> <domain> */
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <memory>
#include <string>
#include <unistd.h>
#include <vector>
#include "dzIPC/common/topic_data.h"
#include "dzIPC/socket_pub_sub_ipc.h"
#include "dzIPC/threepools/socket_recv_worker.h"
#include "ipc_msg/std_msgs/std_image.hpp"

static size_t fd_count(size_t* mx){ DIR* d=opendir("/proc/self/fd"); if(!d) return 0; size_t n=0,m=0; struct dirent* e;
  while((e=readdir(d))){ if(e->d_name[0]=='.') continue; ++n; long v=strtol(e->d_name,0,10); if(v>0&&(size_t)v>m) m=(size_t)v; } closedir(d); if(mx)*mx=m; return n; }
static size_t thread_count(){ DIR* d=opendir("/proc/self/task"); if(!d) return 0; size_t n=0; while(dirent* e=readdir(d)) if(e->d_name[0]!='.') ++n; closedir(d); return n; }

int main(int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "both";
    const int n = argc > 2 ? atoi(argv[2]) : 100;
    const int dom = argc > 3 ? atoi(argv[3]) : 700;
    auto td = [] { return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 37); };
    size_t m0 = 0, f0 = fd_count(&m0);
    const size_t t0 = thread_count();
    std::printf("mode=%s n=%d dom=%d | 建前 fds=%zu max_fd=%zu threads=%zu\n", mode.c_str(), n, dom, f0, m0, t0);

    std::vector<std::unique_ptr<dzIPC::socket::socket_pub_ipc>> pubs;
    std::vector<std::unique_ptr<dzIPC::socket::socket_sub_ipc>> subs;
    pubs.reserve(n); subs.reserve(n);
    for (int i = 0; i < n; ++i)
    {
        const std::string topic = "fd_" + std::to_string(dom) + "_" + std::to_string(i);
        if (mode == "pub" || mode == "both") pubs.push_back(std::make_unique<dzIPC::socket::socket_pub_ipc>(td(), topic, (size_t)dom, false));
        if (mode == "sub" || mode == "both") subs.push_back(std::make_unique<dzIPC::socket::socket_sub_ipc>(td(), topic, (size_t)dom, 1024, false));
    }
    size_t m1 = 0, f1 = fd_count(&m1);
    std::printf("mode=%s n=%d | InitChannel 前 fds=%zu (Δ=%+zd)\n", mode.c_str(), n, f1, (ssize_t)f1 - (ssize_t)f0);
    for (auto& p : pubs) p->InitChannel("fd");
    for (auto& s : subs) s->InitChannel("fd");
    sleep(1);
    size_t m2 = 0, f2 = fd_count(&m2);
    const size_t t2 = thread_count();
    auto& pool = dzIPC::threepools::SocketRecvWorkerPool::instance();
    const double nfd = (double)f2 - (double)f0;
    std::printf("mode=%-4s n=%-5d | 建后 fds=%zu max_fd=%zu threads=%zu | Δfd=%+.0f  Δfd/端点=%.4f  pool_running=%d workers=%zu routes=%zu\n",
                mode.c_str(), n, f2, m2, t2, nfd, nfd / (double)((mode == "both" ? 2 * n : n)),
                (int)pool.running(), pool.worker_count(), pool.route_count());
    std::fflush(stdout);
    _Exit(0);
}

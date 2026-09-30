/* 判定 socket 独立话题订阅是否真的留在固定 worker 池里（门槛1/门槛3 的关键前提）。
 * 每路一个唯一 topic（修正 t6scale 的"同 topic"缺陷）。 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include "dzIPC/common/topic_data.h"
#include "dzIPC/socket_pub_sub_ipc.h"
#include "dzIPC/threepools/socket_recv_worker.h"
#include "ipc_msg/std_msgs/std_image.hpp"

int main(int argc, char** argv) {
    const int n = argc > 1 ? atoi(argv[1]) : 200;
    const int dom = argc > 2 ? atoi(argv[2]) : 555;
    using Clock = std::chrono::steady_clock;
    auto t0 = Clock::now();
    std::vector<std::unique_ptr<dzIPC::socket::socket_sub_ipc>> subs;
    subs.reserve(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) {
        auto td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 37);
        subs.push_back(std::make_unique<dzIPC::socket::socket_sub_ipc>(
            td, "rp_" + std::to_string(i), static_cast<size_t>(dom), 1024, false));
        subs.back()->InitChannel("rp");
    }
    double create_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
    auto& pool = dzIPC::threepools::SocketRecvWorkerPool::instance();
    auto s0 = pool.stats();
    std::printf("n=%d domain=%d create_ms=%.1f pool_running=%d workers=%zu route_count=%zu idle_exits=%llu\n",
                n, dom, create_ms, (int)pool.running(), pool.worker_count(), pool.route_count(),
                (unsigned long long)s0.idle_exits);
    std::this_thread::sleep_for(std::chrono::seconds(2));
    auto s1 = pool.stats();
    std::printf("after2s route_count=%zu idle_exits=%llu thread_restarts=%llu wait_timeouts=%llu\n",
                pool.route_count(), (unsigned long long)s1.idle_exits,
                (unsigned long long)s1.thread_restarts, (unsigned long long)s1.wait_timeouts);
    std::fflush(stdout);
    _exit(0);
}

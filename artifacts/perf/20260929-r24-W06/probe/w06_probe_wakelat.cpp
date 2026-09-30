/* W06 诊断探针 H（判定性）：wait-set 的 futex 唤醒是不是**立即**的？
 *   · 无任何 wait_timeout 干扰：wait(5000ms)，publish 之后测返回延迟。
 *   · 期望：立即（<1ms）。若接近 0 说明 futex_wake 生效；若只有超时才返回，
 *     说明 worker 臂的收包延迟由 wait_timeout 决定（=丢消息窗口）。 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

#include "libipc/ipc.h"
#include "libipc/recv_wait_set.h"

using namespace std::chrono_literals;

static std::chrono::steady_clock::time_point g_t0;

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_t0 = std::chrono::steady_clock::now();
    const std::string name = "w06probeH_" + std::to_string(::getpid());
    ipc::route pub{name.c_str(), ipc::sender, false};
    ipc::route sub{name.c_str(), ipc::receiver, false};

    ipc::recv_wait_set set;
    if (!set.add(sub.read_wait_token())) { std::printf("[H] backend unavailable\n"); return 0; }

    std::atomic<bool> armed{false};
    std::atomic<long long> latency_us{-1};
    std::atomic<long long> ret_t{-1};
    std::thread waiter([&] {
        armed.store(true);
        const auto begin = std::chrono::steady_clock::now();
        const bool woke = set.wait(5000ms);
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - begin).count();
        if (woke) latency_us.store(us);
        ret_t.store(std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - g_t0).count());
        std::printf("[H] wait returned woke=%d after %lldus\n", static_cast<int>(woke), (long long)us);
    });

    while (!armed.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(100ms);   /* 让 waiter 真正睡进 futex_waitv */
    const auto send_t = std::chrono::steady_clock::now();
    std::printf("[H] try_send=%d at t=%lldus\n", static_cast<int>(pub.try_send("hello", 100)),
                (long long)std::chrono::duration_cast<std::chrono::microseconds>(send_t - g_t0).count());
    std::this_thread::sleep_for(500ms);
    std::printf("[H] wait returned at t=%lldus; latency from arming=%lldus\n", ret_t.load(),
                latency_us.load());
    waiter.join();
    sub.recv(100);

    /* 第二轮：先 send 再 wait（EAGAIN 路径）作对照。 */
    ipc::recv_wait_set set2;
    ipc::route pub2{name.c_str(), ipc::sender, false};
    ipc::route sub2{name.c_str(), ipc::receiver, false};
    if (set2.add(sub2.read_wait_token()))
    {
        pub2.try_send("again", 100);
        const auto begin = std::chrono::steady_clock::now();
        const bool woke = set2.wait(5000ms);
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - begin).count();
        std::printf("[H] prescan wait woke=%d after %lldus\n", static_cast<int>(woke), (long long)us);
    }
    pub2.clear();
    sub2.clear();
    pub.clear();
    sub.clear();
    ipc::route::clear_storage(name.c_str());
    return 0;
}

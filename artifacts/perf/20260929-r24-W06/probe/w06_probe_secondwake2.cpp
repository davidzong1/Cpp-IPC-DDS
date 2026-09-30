/* 判定性收尾 B：在**同一个 wait-set 实例**上，第一轮用 wait(50ms)（模拟 worker 的
 * 100ms 超时轮），看第二轮 send 的唤醒是否还能立即生效；并打印每轮的 arm 值。 */
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>

#include "libipc/ipc.h"
#include "libipc/recv_wait_set.h"

using namespace std::chrono_literals;

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string name = "w06probeR3_" + std::to_string(::getpid());
    ipc::route sub{name.c_str(), ipc::receiver, false};
    ipc::route pub{name.c_str(), ipc::sender, false};
    std::this_thread::sleep_for(300ms);

    ipc::recv_wait_set set;
    set.add(sub.read_wait_token());

    /* 轮 1：完全复刻 worker —— 第一次调用 wait 之前没有任何"清 seq"的动作。 */
    {
        const auto t0 = std::chrono::steady_clock::now();
        const bool woke = set.wait(200ms);
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        std::printf("[R3] round1 (no prior drain) woke=%d latency=%lldus ready=%zu\n", static_cast<int>(woke),
                    (long long)us, set.consume_ready().size());
    }

    bool sent = pub.try_send("HELLO", 100);
    std::printf("[R3] try_send(after round1 wait)=%d\n", static_cast<int>(sent));
    {
        const auto t0 = std::chrono::steady_clock::now();
        const bool woke = set.wait(200ms);
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        std::printf("[R3] round2 (send happened BEFORE this wait) woke=%d latency=%lldus ready=%zu "
                    "recv=%zu\n",
                    static_cast<int>(woke), (long long)us, set.consume_ready().size(), sub.recv(0).size());
    }

    pub.clear();
    sub.clear();
    ipc::route::clear_storage(name.c_str());
    return 0;
}

/* 最终判定：纯 libipc，无 worker。arm 之后连续 10 轮"send→wait"，每轮打印 wait 延迟。
 *   · 若每轮都 ≈ wait_timeout ⇒ 唤醒通道完全无效（只有超时兜底）；
 *   · 若只有第一轮慢 ⇒ arming 竞态（首轮错过通知）。 */
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
    const std::string name = "w06probeR2_" + std::to_string(::getpid());
    ipc::route sub{name.c_str(), ipc::receiver, false};
    ipc::route pub{name.c_str(), ipc::sender, false};
    std::this_thread::sleep_for(300ms);
    while (!sub.recv(0).empty()) {}

    ipc::recv_wait_set set;
    if (!set.add(sub.read_wait_token())) { std::printf("backend unavailable\n"); return 0; }

    for (int round = 1; round <= 10; ++round)
    {
        /* 先同步 consume 掉可能已累计的就绪，再 arm。 */
        set.wait(0ms);
        (void)set.consume_ready();
        while (!sub.recv(0).empty()) {}

        const auto t0 = std::chrono::steady_clock::now();
        const bool sent = pub.try_send("HELLO", 100);
        const bool woke = set.wait(200ms);
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        const std::size_t got = sub.recv(0).size();
        std::printf("[R2] round=%2d sent=%d woke=%d latency=%6lldus recv=%zu%s\n", round, static_cast<int>(sent),
                    static_cast<int>(woke), (long long)us, got, (us > 50000 ? "   <== 只靠超时" : ""));
    }
    pub.clear();
    sub.clear();
    ipc::route::clear_storage(name.c_str());
    return 0;
}

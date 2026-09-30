/* 探针 W（判定性，无 worker、无 wait-set）：sub+pub 连接完成后 try_send，随后以 200us 间隔
 * 轮询 recv(0)，测"数据从 push 到对接收方可见"的延迟。
 *   · 若 ≈0  ⇒ 数据立即可见，worker 臂的 100ms 延迟在共享层的 wait/collect 环节；
 *   · 若 ≈100ms ⇒ libipc 的可见性/通知有延迟（更底层）。 */
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>

#include "libipc/ipc.h"

using namespace std::chrono_literals;

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string name = "w06probeW_" + std::to_string(::getpid());
    ipc::route sub{name.c_str(), ipc::receiver, false};
    ipc::route pub{name.c_str(), ipc::sender, false};
    std::this_thread::sleep_for(300ms);

    /* 先把 attach 期间可能已入队的东西取空（与 worker 的"读空"状态对齐）。 */
    std::size_t drained = 0;
    while (!sub.recv(0).empty()) ++drained;
    const auto tok = sub.read_wait_token();
    std::printf("[W] attach done; drained=%zu seq=%u connected_sub=%u\n", drained,
                tok.sequence() ? tok.sequence()->load() : 0u, sub.connected_id());

    const auto t0 = std::chrono::steady_clock::now();
    const bool sent = pub.try_send("HELLO", 100);
    const auto us = [&] {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0).count();
    };
    std::printf("[W] try_send=%d at t=%lldus\n", static_cast<int>(sent), (long long)us());
    long long first = -1;
    for (int i = 0; i < 2000; ++i)
    {
        const auto d = sub.recv(0);
        if (!d.empty())
        {
            first = us();
            std::printf("[W] data visible at t=%lldus size=%zu seq=%u\n", first, d.size(),
                        tok.sequence() ? tok.sequence()->load() : 0u);
            break;
        }
        std::this_thread::sleep_for(200us);
    }
    std::printf("[W] first_visible_us=%lld verdict=%s\n", first,
                (first >= 0 && first < 5000) ? "立即可见" : "延迟约 100ms");

    pub.clear();
    sub.clear();
    ipc::route::clear_storage(name.c_str());
    return 0;
}

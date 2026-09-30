/* 探针 U（判定性）：不涉及 RecvWorker。复刻"worker 的等待姿势"：
 *   sub + pub 都建好 → wait-set.add(sub token) → 先消费掉 attach 那一次叫醒（像 worker 那样
 *   wait 一次并 collect）→ 再 try_send → 测第二次 wait 是否被**立即**唤醒。
 *   若第二次只能靠超时 ⇒ 缺陷在 libipc 的"第二次通知"（W06 无权改，须上报/走共享层）。
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <unistd.h>

#include "libipc/ipc.h"
#include "libipc/recv_wait_set.h"

using namespace std::chrono_literals;

static std::uint32_t seqv(const ipc::route& r)
{
    const auto t = r.read_wait_token();
    return (t.sequence() != nullptr) ? t.sequence()->load() : 0u;
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string name = "w06probeU_" + std::to_string(::getpid());

    ipc::route sub{name.c_str(), ipc::receiver, false};
    ipc::route pub{name.c_str(), ipc::sender, false};
    std::this_thread::sleep_for(300ms);
    std::printf("[U] attach done: sub seq=%u pub seq=%u connected_pub=%u\n", seqv(sub), seqv(pub),
                pub.connected_id());

    ipc::recv_wait_set set;
    if (!set.add(sub.read_wait_token())) { std::printf("[U] backend unavailable\n"); return 0; }

    /* 第一次 wait：把 attach 期间累计的 seq 变化消费掉（worker 在 registered 后就会这么做）。 */
    const bool first = set.wait(50ms);
    const auto first_ready = set.consume_ready();
    std::printf("[U] first wait=%d ready=%zu seq now sub=%u pub=%u\n", static_cast<int>(first), first_ready.size(),
                seqv(sub), seqv(pub));
    /* 顺手像 worker 那样把数据取空（保持与 worker 相同的"读空"状态）。 */
    std::size_t drained = 0;
    while (!sub.recv(0).empty()) ++drained;
    std::printf("[U] drained=%zu seq now sub=%u pub=%u\n", drained, seqv(sub), seqv(pub));

    std::atomic<bool> armed{false};
    std::atomic<long long> latency{-1};
    std::thread waiter([&] {
        armed.store(true);
        const auto t0 = std::chrono::steady_clock::now();
        const bool woke = set.wait(2000ms);
        latency.store(std::chrono::duration_cast<std::chrono::microseconds>(
                          std::chrono::steady_clock::now() - t0).count());
        std::printf("[U] second wait woke=%d after %lldus\n", static_cast<int>(woke),
                    (long long)latency.load());
    });
    while (!armed.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(100ms);

    const auto t_send = std::chrono::steady_clock::now();
    const bool sent = pub.try_send("HELLO", 100);
    waiter.join();
    const auto dt = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - t_send).count();
    std::printf("[U] sent=%d; send->join=%lldus; wait latency=%lldus; seq now sub=%u pub=%u\n",
                static_cast<int>(sent), (long long)dt, latency.load(), seqv(sub), seqv(pub));
    std::printf("[U] verdict: %s\n",
                latency.load() < 1000 ? "第二次通知生效" : "第二次通知丢失（只能等超时）");

    pub.clear();
    sub.clear();
    ipc::route::clear_storage(name.c_str());
    return 0;
}

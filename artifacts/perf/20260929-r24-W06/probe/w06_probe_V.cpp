/* 探针 V（判定性，纯 libipc，无 RecvWorker）：
 *   假设："token 在 attach 通知已经发生之后才 add 进 wait-set" ⇒ 之后的那次 send 唤醒丢失。
 *   A: add 在 attach 之后（seq 已经是 1），然后 send（→2），测 wait 是否立即返回。
 *   B: add 在 attach 之前（seq=0），然后 send（→1），测 wait。
 *   两者只差"add 时 seq 是否已非 0"。 */
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

static void run(const char* tag, bool add_after_attach)
{
    const std::string name = std::string("w06probeV_") + tag + "_" + std::to_string(::getpid());
    ipc::route sub{name.c_str(), ipc::receiver, false};
    ipc::recv_wait_set set;
    bool added = false;
    if (!add_after_attach)
    {
        added = set.add(sub.read_wait_token());
        std::printf("[V/%s] add before attach: added=%d seq=%u\n", tag, static_cast<int>(added), seqv(sub));
    }
    ipc::route pub{name.c_str(), ipc::sender, false};
    std::this_thread::sleep_for(300ms);   /* attach 通知走完 */
    if (add_after_attach)
    {
        added = set.add(sub.read_wait_token());
        std::printf("[V/%s] add after attach: added=%d seq=%u pub_seq=%u\n", tag, static_cast<int>(added),
                    seqv(sub), seqv(pub));
    }
    else
    {
        std::printf("[V/%s] seq after attach (already added): %u pub_seq=%u\n", tag, seqv(sub), seqv(pub));
    }

    std::atomic<bool> armed{false};
    std::atomic<long long> lat{-1};
    std::thread waiter([&] {
        armed.store(true);
        const auto t0 = std::chrono::steady_clock::now();
        const bool woke = set.wait(3000ms);
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        std::printf("[V/%s] wait woke=%d after %lldus\n", tag, static_cast<int>(woke), (long long)us);
        lat.store(us);
    });
    while (!armed.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(200ms);
    const auto t_send = std::chrono::steady_clock::now();
    const bool sent = pub.try_send("HELLO", 100);
    const auto us_send = [&] {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t_send)
            .count();
    };
    waiter.join();
    std::printf("[V/%s] sent=%d send->join=%lldus  latency=%lldus  seq sub=%u pub=%u ready=%zu recv=%zu\n", tag,
                static_cast<int>(sent), (long long)us_send(), lat.load(), seqv(sub), seqv(pub),
                set.consume_ready().size(), sub.recv(100).size());

    pub.clear();
    sub.clear();
    ipc::route::clear_storage(name.c_str());
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    run("A_add_after_attach", true);
    run("B_add_before_attach", false);
    return 0;
}

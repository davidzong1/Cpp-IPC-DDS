/* 探针 T（判定性）：发布端与前端的 rd_waiter seq 是不是**同一个 futex key**？
 *   步骤：建 sub → 建 pub → 等 200ms（attach 通知走完）→ 起线程 wait(2000ms) → 再等 200ms（确保已 arm）
 *        → 发 3 条 → 分别打印 sub/pub 的 seq 值 与 wait 返回延迟。
 *   同一 key ⇒ wait 立即返回、且两侧计数值同步增长。
 *   不同 key ⇒ wait 等满 2000ms，且两侧计数各自独立。
 */
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

static std::uint32_t seqv(const ipc::route& r)
{
    const auto t = r.read_wait_token();
    return (t.sequence() != nullptr) ? t.sequence()->load() : 0u;
}
static const void* seqp(const ipc::route& r)
{
    return static_cast<const void*>(r.read_wait_token().sequence());
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string name = "w06probeT_" + std::to_string(::getpid());

    ipc::route sub{name.c_str(), ipc::receiver, false};
    ipc::route pub{name.c_str(), ipc::sender, false};
    std::this_thread::sleep_for(200ms);

    std::printf("[T] after attach: sub seq(addr=%p)=%u | pub seq(addr=%p)=%u\n", seqp(sub), seqv(sub), seqp(pub),
                seqv(pub));

    ipc::recv_wait_set set;
    const bool added = set.add(sub.read_wait_token());
    std::atomic<bool> armed{false};
    std::atomic<long long> latency_us{-1};
    std::atomic<bool> woke_flag{false};
    std::thread waiter([&] {
        armed.store(true);
        const auto t0 = std::chrono::steady_clock::now();
        const bool woke = set.wait(2000ms);
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        woke_flag.store(woke);
        latency_us.store(us);
    });
    while (!armed.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(200ms);   /* 确保 wait 真的进了 futex_waitv */

    const auto t_send = std::chrono::steady_clock::now();
    const bool s1 = pub.try_send("m1", 100);
    const bool s2 = pub.try_send("m2", 100);
    const bool s3 = pub.try_send("m3", 100);
    std::printf("[T] send=%d/%d/%d\n", static_cast<int>(s1), static_cast<int>(s2), static_cast<int>(s3));

    waiter.join();
    const auto send_to_wake = std::chrono::duration_cast<std::chrono::microseconds>(
                                  std::chrono::steady_clock::now() - t_send).count();
    std::printf("[T] wait woke=%d latency=%lldus (send->join=%lldus) added=%d\n", static_cast<int>(woke_flag.load()),
                latency_us.load(), (long long)send_to_wake, static_cast<int>(added));
    std::printf("[T] after 3 sends: sub seq=%u | pub seq=%u\n", seqv(sub), seqv(pub));
    std::printf("[T] verdict: %s\n",
                (latency_us.load() < 2000) ? "同一 futex key（唤醒生效）" : "不同 key（只能靠超时发现数据）");

    pub.clear();
    sub.clear();
    ipc::route::clear_storage(name.c_str());
    return 0;
}

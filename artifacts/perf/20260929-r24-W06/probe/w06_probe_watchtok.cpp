/* 探针 S（判定性）：在 RecvWorker 之外，用同一个 token 另建一个 recv_wait_set 一起
 * 监听，比较"worker 自己的 wait-set"与"我另建的 wait-set"谁被唤醒。
 *
 * 三种可能的结论：
 *   1) 两者都立即醒  ⇒ 共享层无问题，dz 侧接入有别的错；
 *   2) 只有我另建的醒 ⇒ worker 的 wait-set 实例处于坏状态（stale arming）；
 *   3) 两者都不醒    ⇒ token 指向的 seq 字与发布端 notify 的字不是同一个（跨段）。
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

#include "dzIPC/threepools/recv_worker.h"
#include "libipc/ipc.h"
#include "libipc/recv_wait_set.h"

using namespace std::chrono_literals;

static std::chrono::steady_clock::time_point g_t0;
static long long now_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_t0).count();
}

struct RawRoute final : dzIPC::threepools::RecvRouteSource
{
    std::string name;
    std::unique_ptr<ipc::route> rx;
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> bytes{0};
    std::atomic<const ipc::route*> pub_route{nullptr};

    RawRoute(const std::string& n) : name(n) { rx = std::make_unique<ipc::route>(n.c_str(), ipc::receiver, false); }
    const char* route_name() const noexcept override { return name.c_str(); }
    std::uint32_t domain_id() const noexcept override { return 0; }
    ipc::recv_wait_token read_wait_token() const noexcept override { return rx->read_wait_token(); }
    std::size_t recv_once() override
    {
        ++calls;
        const auto d = rx->recv(0);
        if (!d.empty()) bytes.fetch_add(d.size());
        return d.size();
    }
    bool has_pending() const noexcept override { return false; }
    dzIPC::threepools::RecvOwner recv_owner() const noexcept override
    {
        return owner.load(std::memory_order_acquire);
    }
    bool try_claim_recv(dzIPC::threepools::RecvOwner w) noexcept override
    {
        auto e = dzIPC::threepools::RecvOwner::none;
        if (w == dzIPC::threepools::RecvOwner::none) return false;
        return owner.compare_exchange_strong(e, w, std::memory_order_acq_rel);
    }
    void release_recv() noexcept override { owner.store(dzIPC::threepools::RecvOwner::none); }
    void stop_and_wake() noexcept override { rx->disconnect(); }
    void wait_quiescent() noexcept override {}
    std::atomic<dzIPC::threepools::RecvOwner> owner{dzIPC::threepools::RecvOwner::none};
};

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_t0 = std::chrono::steady_clock::now();
    const std::string name = "w06probeS_" + std::to_string(::getpid());

    auto* worker = new dzIPC::threepools::RecvWorker(0, dzIPC::threepools::RecvBudget{});
    worker->start();
    auto route = std::make_shared<RawRoute>(name);
    auto sender = std::make_unique<ipc::route>(name.c_str(), ipc::sender, false);
    std::this_thread::sleep_for(300ms);

    const auto add = worker->add_route(route);
    std::this_thread::sleep_for(200ms);

    const auto tok = route->read_wait_token();
    const auto pub_tok = sender->read_wait_token();
    std::printf("[S] add=%d worker-token seq ptr=%p val=%u | sender-token seq ptr=%p val=%u\n",
                static_cast<int>(add), static_cast<const void*>(tok.sequence()),
                tok.sequence() ? tok.sequence()->load() : 0u, static_cast<const void*>(pub_tok.sequence()),
                pub_tok.sequence() ? pub_tok.sequence()->load() : 0u);

    /* 另建一个 wait-set，盯**同一个 token**。 */
    ipc::recv_wait_set manual;
    const bool manual_added = manual.add(tok);
    std::atomic<bool> manual_woke{false};
    std::atomic<long long> manual_us{-1};
    std::atomic<bool> armed{false};
    std::thread watcher([&] {
        armed.store(true);
        const bool woke = manual.wait(3000ms);
        if (woke)
        {
            manual_woke.store(true);
            manual_us.store(now_us());
        }
    });
    while (!armed.load()) std::this_thread::sleep_for(1ms);
    std::this_thread::sleep_for(50ms);

    const auto stats_before = worker->stats();
    const auto t_send = now_us();
    const bool sent = sender->try_send("HELLO", 100);
    std::printf("[S] t=%lldus try_send=%d manual_add=%d\n", t_send, static_cast<int>(sent),
                static_cast<int>(manual_added));

    for (int i = 0; i < 15; ++i)
    {
        std::this_thread::sleep_for(20ms);
        const auto st = worker->stats();
        std::printf("[S] t=%6lldus tok val=%u | worker: calls=%llu msgs=%llu wakeups=%llu timeouts=%llu | manual_woke=%d@%lldus\n",
                    now_us(), tok.sequence() ? tok.sequence()->load() : 0u,
                    (unsigned long long)(st.recv_once_calls - stats_before.recv_once_calls),
                    (unsigned long long)(st.messages_received - stats_before.messages_received),
                    (unsigned long long)(st.wait_wakeups - stats_before.wait_wakeups),
                    (unsigned long long)(st.wait_timeouts - stats_before.wait_timeouts),
                    static_cast<int>(manual_woke.load()), manual_us.load());
    }

    watcher.join();
    worker->remove_route(route.get());
    route.reset();
    sender.reset();
    ipc::route::clear_storage(name.c_str());
    return 0;
}

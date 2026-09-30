/* 探针 K（判定性，隔离共享层）：直接用 RecvWorker 包一条 raw ipc::route，测
 * "publish → recv_once 被调用"的延迟。若 ≈0 ⇒ 共享层唤醒正常，问题在 dz 接入；
 * 若 ≈wait_timeout(100ms) ⇒ 共享层/socket 侧的 wait 路径有问题（W06 无权改，需上报）。 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

#include "dzIPC/threepools/recv_worker.h"
#include "libipc/ipc.h"

using namespace std::chrono_literals;

struct RawRoute final : dzIPC::threepools::RecvRouteSource
{
    std::string name;
    std::unique_ptr<ipc::route> rx;
    std::atomic<std::uint64_t> calls{0};
    std::atomic<std::uint64_t> bytes{0};
    std::chrono::steady_clock::time_point first_call{};

    RawRoute(const std::string& n, bool as_sub) : name(n)
    {
        rx = std::make_unique<ipc::route>(n.c_str(), as_sub ? ipc::receiver : ipc::sender, false);
    }
    const char* route_name() const noexcept override { return name.c_str(); }
    std::uint32_t domain_id() const noexcept override { return 0; }
    ipc::recv_wait_token read_wait_token() const noexcept override { return rx->read_wait_token(); }
    std::chrono::steady_clock::time_point t_send{};
    std::atomic<long long> first_data_us{-1};
    std::size_t recv_once() override
    {
        const auto d = rx->recv(0);
        std::fprintf(stderr, "[DBG-ADP] recv(0) -> %zu (seq=%u conn=%u)\n", d.size(),
                     rx->read_wait_token().sequence() ? rx->read_wait_token().sequence()->load() : 0u,
                     rx->connected_id());
        if (calls.fetch_add(1) == 0) first_call = std::chrono::steady_clock::now();
        if (!d.empty())
        {
            bytes.fetch_add(d.size());
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - t_send).count();
            long long expect = -1;
            first_data_us.compare_exchange_strong(expect, us);
        }
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
    const std::string name = "w06probeK_" + std::to_string(::getpid());

    auto* worker = new dzIPC::threepools::RecvWorker(0, dzIPC::threepools::RecvBudget{});
    worker->start();
    auto route = std::make_shared<RawRoute>(name, /*as_sub=*/true);

    auto sender = std::make_unique<ipc::route>(name.c_str(), ipc::sender, false);
    std::this_thread::sleep_for(300ms);   /* 让 sender 完成连接并唤醒一次 */

    const auto t0b = route->rx->read_wait_token();
    std::printf("[K] BEFORE add_route: route token addr=%p val=%u connected=%u\n",
                (const void*)t0b.sequence(), t0b.sequence() ? t0b.sequence()->load() : 0u,
                route->rx->connected_id());
    const auto add = worker->add_route(route);
    {
        const auto t1 = route->rx->read_wait_token();
        std::printf("[K] AFTER  add_route: route token addr=%p val=%u connected=%u\n",
                    (const void*)t1.sequence(), t1.sequence() ? t1.sequence()->load() : 0u,
                    route->rx->connected_id());
    }
    std::printf("[K] add_route=%d connected=%u\n", static_cast<int>(add), route->rx->connected_id());
    std::this_thread::sleep_for(200ms);

    const auto calls_before = route->calls.load();
    const auto t0 = std::chrono::steady_clock::now();
    route->t_send = t0;
    std::atomic<bool> mon_stop{false};
    std::thread monitor([&] {
        auto prev = worker->stats();
        while (!mon_stop.load())
        {
            std::this_thread::sleep_for(1ms);
            const auto cur = worker->stats();
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - t0).count();
            if (cur.wait_wakeups != prev.wait_wakeups || cur.wait_timeouts != prev.wait_timeouts
                || cur.recv_once_calls != prev.recv_once_calls || cur.messages_received != prev.messages_received)
            {
                std::printf("[Kmon] t=%6lldus wakeups=%llu timeouts=%llu calls=%llu msgs=%llu bytes=%llu\n",
                            (long long)us, (unsigned long long)cur.wait_wakeups,
                            (unsigned long long)cur.wait_timeouts, (unsigned long long)cur.recv_once_calls,
                            (unsigned long long)cur.messages_received, (unsigned long long)cur.bytes_received);
                prev = cur;
            }
        }
    });
    const bool sent = sender->try_send("HELLO", 100);
    std::this_thread::sleep_for(250ms);
    mon_stop.store(true);
    monitor.join();
    for (int i = 0; i < 20; ++i)
    {
        std::this_thread::sleep_for(10ms);
        if (route->calls.load() > calls_before && route->bytes.load() > 0)
        {
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - t0).count();
            std::printf("[K] sent=%d -> data recv_once observed after %lldus (calls %llu->%llu bytes=%llu)\n",
                        static_cast<int>(sent), (long long)us, (unsigned long long)calls_before,
                        (unsigned long long)route->calls.load(), (unsigned long long)route->bytes.load());
            break;
        }
        if (i == 19)
        {
            std::printf("[K] sent=%d -> NO data observed within 200ms (calls=%llu bytes=%llu)\n",
                        static_cast<int>(sent), (unsigned long long)route->calls.load(),
                        (unsigned long long)route->bytes.load());
        }
    }
    std::printf("[K] first_data_us after send = %lld\n", route->first_data_us.load());
    const auto st = worker->stats();
    std::printf("[K] worker: calls=%llu msgs=%llu wakeups=%llu timeouts=%llu\n",
                (unsigned long long)st.recv_once_calls, (unsigned long long)st.messages_received,
                (unsigned long long)st.wait_wakeups, (unsigned long long)st.wait_timeouts);
    worker->remove_route(route.get());
    route.reset();
    sender.reset();
    ipc::route::clear_storage(name.c_str());
    return 0;
}

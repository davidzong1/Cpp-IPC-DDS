/* 判定性收尾 D（针对"worker 的等待 token 变陈旧"假说）：
 *   raw route + RecvWorker 稳定后，在 send 之前做一次 remove_route + add_route
 *   （⇒ wait-set 用**当前** token 重新 arm），然后测 send→data 延迟。
 *     重新 arm 后延迟 ≈0   ⇒ 陈旧 token 就是根因（模块可在重建/重连后重注册来规避）
 *     仍然 ≈wait_timeout    ⇒ 与 token 陈旧无关
 *   对照：同一次运行里再发一条（不再重 arm），看延迟是否也 ≈0。 */
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

struct R final : dzIPC::threepools::RecvRouteSource
{
    std::string name;
    std::unique_ptr<ipc::route> rx;
    std::atomic<long long> sent_at_us{-1};
    std::atomic<long long> first_data_us{-1};
    std::atomic<std::uint64_t> bytes{0};
    std::chrono::steady_clock::time_point origin{};
    R(const std::string& n) : name(n) { rx = std::make_unique<ipc::route>(n.c_str(), ipc::receiver, false); }
    long long now_us() const
    {
        return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - origin).count();
    }
    const char* route_name() const noexcept override { return name.c_str(); }
    std::uint32_t domain_id() const noexcept override { return 0; }
    ipc::recv_wait_token read_wait_token() const noexcept override { return rx->read_wait_token(); }
    std::size_t recv_once() override
    {
        const auto d = rx->recv(0);
        if (!d.empty())
        {
            bytes.fetch_add(d.size());
            long long expect = -1;
            first_data_us.compare_exchange_strong(expect, now_us());
        }
        return d.size();
    }
    bool has_pending() const noexcept override { return false; }
    dzIPC::threepools::RecvOwner recv_owner() const noexcept override { return owner.load(); }
    bool try_claim_recv(dzIPC::threepools::RecvOwner w) noexcept override
    {
        auto e = dzIPC::threepools::RecvOwner::none;
        if (w == dzIPC::threepools::RecvOwner::none) return false;
        return owner.compare_exchange_strong(e, w);
    }
    void release_recv() noexcept override { owner.store(dzIPC::threepools::RecvOwner::none); }
    void stop_and_wake() noexcept override { rx->disconnect(); }
    void wait_quiescent() noexcept override {}
    std::atomic<dzIPC::threepools::RecvOwner> owner{dzIPC::threepools::RecvOwner::none};
};

static long long measure(const char* tag, R& route, ipc::route& sender, bool rearm,
                         dzIPC::threepools::RecvWorker* worker)
{
    route.first_data_us.store(-1);
    const auto t_send = route.now_us();
    route.sent_at_us.store(t_send);
    const bool ok = sender.try_send("HELLO", 100);
    /* 等到数据出现（最多 400ms）。 */
    long long first = -1;
    for (int i = 0; i < 400; ++i)
    {
        std::this_thread::sleep_for(1ms);
        if (route.first_data_us.load() >= 0)
        {
            first = route.first_data_us.load() - t_send;
            break;
        }
    }
    std::printf("[RA] %-22s rearm=%d sent=%d first_data_after_send=%lldus\n", tag, static_cast<int>(rearm),
                static_cast<int>(ok), first);
    (void)worker;
    return first;
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string name = "w06probeRA_" + std::to_string(::getpid());
    auto* worker = new dzIPC::threepools::RecvWorker(0, dzIPC::threepools::RecvBudget{});
    worker->start();
    auto route = std::make_shared<R>(name);
    route->origin = std::chrono::steady_clock::now();
    auto sender = std::make_unique<ipc::route>(name.c_str(), ipc::sender, false);
    std::this_thread::sleep_for(400ms);

    worker->add_route(route);
    std::this_thread::sleep_for(400ms);   /* 消化 attach 通知 */

    measure("before-rearm", *route, *sender, false, worker);
    std::this_thread::sleep_for(200ms);

    /* 重新 arm（remove + add）：wait-set 换成当前 token。 */
    worker->remove_route(route.get());
    const auto t_rearm = route->now_us();
    worker->add_route(route);
    std::printf("[RA] re-armed at +%lldus\n", route->now_us() - t_rearm);
    std::this_thread::sleep_for(50ms);
    measure("after-rearm", *route, *sender, true, worker);
    measure("after-rearm-2nd", *route, *sender, true, worker);

    worker->remove_route(route.get());
    route.reset();
    sender.reset();
    ipc::route::clear_storage(name.c_str());
    return 0;
}

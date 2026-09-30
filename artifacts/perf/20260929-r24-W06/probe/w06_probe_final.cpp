/* 最后一次判定：worker 的 waitv arm 地址/值与 publish 端 futex_wake 的地址是否同一物理页。 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <cstdio>
#include <unistd.h>

#include "dzIPC/threepools/recv_worker.h"
#include "libipc/ipc.h"

using namespace std::chrono_literals;

struct R final : dzIPC::threepools::RecvRouteSource
{
    std::string name;
    std::unique_ptr<ipc::route> rx;
    R(const std::string& n) : name(n) { rx = std::make_unique<ipc::route>(n.c_str(), ipc::receiver, false); }
    const char* route_name() const noexcept override { return name.c_str(); }
    std::uint32_t domain_id() const noexcept override { return 0; }
    ipc::recv_wait_token read_wait_token() const noexcept override { return rx->read_wait_token(); }
    std::size_t recv_once() override { return rx->recv(0).size(); }
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

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string name = "w06probeZ_" + std::to_string(::getpid());
    auto* worker = new dzIPC::threepools::RecvWorker(0, dzIPC::threepools::RecvBudget{});
    worker->start();
    auto route = std::make_shared<R>(name);
    auto sender = std::make_unique<ipc::route>(name.c_str(), ipc::sender, false);
    std::this_thread::sleep_for(400ms);
    worker->add_route(route);           /* arm 之后 worker 会先消费一次 attach 通知 */
    std::this_thread::sleep_for(300ms);  /* 让 worker 回到 wait */

    const auto tok = route->rx->read_wait_token();
    {
        /* worker 侧 rd_waiter 段文件的 inode/名字，与 sender 侧对照。 */
        auto show = [](const char* tag, const void* p) {
            const auto* a = static_cast<const char*>(p);
            const std::uintptr_t page =
                reinterpret_cast<std::uintptr_t>(a) - (reinterpret_cast<std::uintptr_t>(a) % 4096);
            std::FILE* f = std::fopen("/proc/self/maps", "r");
            char buf[1024];
            while (f != nullptr && std::fgets(buf, sizeof(buf), f) != nullptr)
            {
                unsigned long lo = 0, hi = 0;
                if (std::sscanf(buf, "%lx-%lx", &lo, &hi) == 2 && page >= lo && page < hi)
                {
                    std::printf("[Z] %s seq@%p :: %s", tag, p, buf);
                    std::fflush(stdout);
                    break;
                }
            }
            if (f) std::fclose(f);
        };
        show("worker-route", tok.sequence());
        show("sender      ", sender->read_wait_token().sequence());
    }
    std::printf("[Z] at send time: worker-route token @%p val=%u | sender token @%p val=%u | connected_sub=%u\n",
                (const void*)tok.sequence(), tok.sequence()->load(),
                (const void*)sender->read_wait_token().sequence(), sender->read_wait_token().sequence()->load(),
                route->rx->connected_id());
    std::fflush(stdout);
    const auto t0 = std::chrono::steady_clock::now();
    std::printf("[Z] try_send=%d\n", static_cast<int>(sender->try_send("HELLO", 100)));
    std::fflush(stdout);
    for (int i = 0; i < 25; ++i)
    {
        std::this_thread::sleep_for(5ms);
        const auto st = worker->stats();
        if (st.messages_received > 0)
        {
            std::printf("[Z] data at +%lldus (msgs=%llu calls=%llu wakeups=%llu timeouts=%llu)\n",
                        (long long)std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0).count(),
                        (unsigned long long)st.messages_received, (unsigned long long)st.recv_once_calls,
                        (unsigned long long)st.wait_wakeups, (unsigned long long)st.wait_timeouts);
            std::fflush(stdout);
            break;
        }
    }
    worker->remove_route(route.get());
    route.reset();
    sender.reset();
    ipc::route::clear_storage(name.c_str());
    return 0;
}

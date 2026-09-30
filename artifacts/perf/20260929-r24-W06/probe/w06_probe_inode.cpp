/* 判定性收尾：同一进程内 worker 侧 route 的 RD_CONN 段与 sender 侧 RD_CONN 段
 *   · dev/inode 是否相同（相同 ⇒ 同一 futex key）；
 *   · 直接对 worker 侧 token 调 futex_wake，看返回的 woken 数。 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <climits>
#include <linux/futex.h>
#include <memory>
#include <string>
#include <sys/syscall.h>
#include <thread>
#include <unistd.h>

#include "dzIPC/threepools/recv_worker.h"
#include "libipc/ipc.h"

using namespace std::chrono_literals;

static std::string inode_of(const void* p)
{
    const auto* a = static_cast<const char*>(p);
    const std::uintptr_t page = reinterpret_cast<std::uintptr_t>(a) - (reinterpret_cast<std::uintptr_t>(a) % 4096);
    std::FILE* f = std::fopen("/proc/self/maps", "r");
    if (f == nullptr) return "<no maps>";
    char buf[1024];
    std::string out = "<not found>";
    while (std::fgets(buf, sizeof(buf), f) != nullptr)
    {
        unsigned long lo = 0, hi = 0;
        if (std::sscanf(buf, "%lx-%lx", &lo, &hi) == 2 && page >= lo && page < hi)
        {
            out = buf;
            if (!out.empty() && out.back() == '\n') out.pop_back();
            break;
        }
    }
    std::fclose(f);
    return out;
}

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
    const std::string name = "w06probeI_" + std::to_string(::getpid());
    auto* worker = new dzIPC::threepools::RecvWorker(0, dzIPC::threepools::RecvBudget{});
    worker->start();
    auto route = std::make_shared<R>(name);
    auto sender = std::make_unique<ipc::route>(name.c_str(), ipc::sender, false);
    std::this_thread::sleep_for(400ms);
    worker->add_route(route);
    std::this_thread::sleep_for(300ms);

    const auto wt = route->rx->read_wait_token();
    const auto st = sender->read_wait_token();
    std::printf("[I] worker-token seq @%p val=%u\n  %s\n", (const void*)wt.sequence(), wt.sequence()->load(),
                inode_of(wt.sequence()).c_str());
    std::printf("[I] sender-token seq @%p val=%u\n  %s\n", (const void*)st.sequence(), st.sequence()->load(),
                inode_of(st.sequence()).c_str());

    /* 直接对 worker 侧的字 futex_wake：当前 worker 应正阻塞在该字上。 */
    const int woken = static_cast<int>(::syscall(SYS_futex, wt.sequence(), FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0));
    std::printf("[I] direct futex_wake(worker-token) -> woken=%d\n", woken);

    /* 再发一条，测延迟（这次先手动唤醒过，worker 可能已在 collect）。 */
    worker->add_route(route);   /* duplicate/ok 无妨 */
    const auto t0 = std::chrono::steady_clock::now();
    std::printf("[I] try_send=%d\n", static_cast<int>(sender->try_send("HELLO", 100)));
    long long first = -1;
    for (int i = 0; i < 40; ++i)
    {
        std::this_thread::sleep_for(5ms);
        if (worker->stats().messages_received > 0)
        {
            first = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - t0)
                        .count();
            break;
        }
    }
    std::printf("[I] first_data_us=%lld\n", first);
    worker->remove_route(route.get());
    route.reset();
    sender.reset();
    ipc::route::clear_storage(name.c_str());
    return 0;
}

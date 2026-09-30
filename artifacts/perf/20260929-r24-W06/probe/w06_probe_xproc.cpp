/* 判定性收尾 E（跨进程对照，纯共享层）：
 *   子进程 = RecvWorker + raw ipc::route(receiver)；父进程 = ipc::route(sender)。
 *   · 若跨进程send后数据立刻到 ⇒ 共享层在跨进程下正常，100ms 地板是同进程双映射特有；
 *   · 若同样 ≈wait_timeout ⇒ 共享层唤醒通道整体失效（W06 无权改，须上报共享层负责人）。
 * 先 fork 再开 worker，避免"父进程先建池、子进程继承"的 pid 闸混淆。 */
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "dzIPC/threepools/recv_worker.h"
#include "libipc/ipc.h"

using namespace std::chrono_literals;

struct R final : dzIPC::threepools::RecvRouteSource
{
    std::string name;
    std::unique_ptr<ipc::route> rx;
    std::chrono::steady_clock::time_point origin;
    std::atomic<long long> first_us{-1};
    std::atomic<std::uint64_t> bytes{0};
    R(const std::string& n) : name(n)
    {
        origin = std::chrono::steady_clock::now();
        rx = std::make_unique<ipc::route>(n.c_str(), ipc::receiver, false);
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
            first_us.compare_exchange_strong(expect,
                                             std::chrono::duration_cast<std::chrono::microseconds>(
                                                 std::chrono::steady_clock::now() - origin).count());
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

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const std::string name = "w06probeXP_" + std::to_string(::getpid());
    int pfd[2];
    if (::pipe(pfd) != 0) return 1;
    const pid_t child = ::fork();
    if (child == 0)
    {
        ::close(pfd[0]);
        auto* worker = new dzIPC::threepools::RecvWorker(0, dzIPC::threepools::RecvBudget{});
        worker->start();
        auto route = std::make_shared<R>(name);
        std::this_thread::sleep_for(300ms);
        worker->add_route(route);
        std::this_thread::sleep_for(400ms);   /* 消化 attach 通知 */
        ::write(pfd[1], "R", 1);
        /* 父进程在收到 R 之后 300ms 发消息；子进程持续观察。 */
        for (int i = 0; i < 600; ++i)
        {
            std::this_thread::sleep_for(1ms);
            if (route->first_us.load() >= 0) break;
        }
        char buf[64];
        const int n = std::snprintf(buf, sizeof(buf), "%lld", route->first_us.load());
        ::write(pfd[1], buf, static_cast<std::size_t>(n));
        ::_exit(0);
    }
    ::close(pfd[1]);
    char c = 0;
    if (::read(pfd[0], &c, 1) != 1 || c != 'R')
    {
        std::printf("[XP] child not ready\n");
    }
    else
    {
        std::this_thread::sleep_for(300ms);
        ipc::route tx{name.c_str(), ipc::sender, false};
        std::this_thread::sleep_for(300ms);
        std::printf("[XP] parent try_send=%d\n", static_cast<int>(tx.try_send("HELLO", 100)));
        char buf[64] = {0};
        const ssize_t n = ::read(pfd[0], buf, sizeof(buf) - 1);
        std::printf("[XP] child first_data_us_since_start=%s (send happened at ~300ms after R)\n",
                    n > 0 ? buf : "?");
        std::printf("[XP] verdict: %s\n",
                    (n > 0 && std::atoll(buf) > 0) ? "见上：与 300ms 的差 = 唤醒延迟" : "no data");
        tx.clear();
    }
    ::close(pfd[0]);
    int status = 0;
    ::waitpid(child, &status, 0);
    ipc::route::clear_storage(name.c_str());
    return 0;
}

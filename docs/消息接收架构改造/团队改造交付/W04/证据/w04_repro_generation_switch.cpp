/* W04 复现件：**已注册 route 上直接做 generation 重建 ⇒ worker 在 recv_wait_set::wait 里 SIGSEGV**。
 *
 * 用法：
 *   g++ -std=c++17 -O1 -I include -I src -I 3rdparty <本文件> -o /tmp/repro \
 *       -L build/lib -lipc -lpthread -Wl,-rpath,$PWD/build/lib
 *   /tmp/repro 1   # 不安全顺序（先 reset 旧 sender / 直接 rebuild）⇒ 期望 SIGSEGV(139)
 *   /tmp/repro 2   # 不安全顺序的另一种排列            ⇒ 期望 SIGSEGV(139)
 *   /tmp/repro 9   # 安全顺序（remove_route → rebuild → add_route）⇒ 期望 rc=0
 *
 * 结论（本机 Linux 6.8 / 32 线程 / Release）：
 *   mode 1/2 → 8/8 次 SIGSEGV，gdb 栈顶为
 *     ipc::recv_wait_set::wait ← RecvWorker::Impl::wait_once ← Impl::loop（worker 线程）；
 *   mode 9   → rc=0（安全顺序）。
 * 因此"generation 重建必须先把 route 从 wait-set 同步摘除"是**硬约束**，不是风格建议。
 * 详见 ../接口与生命周期.md §7 与 §9 的 R-07。
 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

#include "dzIPC/shm_route_session.h"
#include "dzIPC/threepools/recv_worker.h"
#include "libipc/ipc.h"

using namespace dzIPC::threepools;

struct Src : RecvRouteSource {
    explicit Src(std::string n)
        : name_(std::move(n))
        , create_([this] { return std::make_shared<ipc::route>(name_.c_str(), ipc::receiver, false); })
    {
        session_.begin_rebuild(1, create_);
    }

    const char* route_name() const noexcept override { return name_.c_str(); }
    std::uint32_t domain_id() const noexcept override { return 0; }
    ipc::recv_wait_token read_wait_token() const noexcept override
    {
        auto r = session_.current_route();
        return r ? r->read_wait_token() : ipc::recv_wait_token{};
    }
    std::size_t recv_once() override
    {
        auto lease = session_.acquire_receive();
        if (!lease.has_value()) return 0;
        ipc::buff_t buf = lease->route->recv(0);
        session_.release_receive();
        if (buf.empty()) return 0;
        std::lock_guard<std::mutex> lk(m_);
        ++n_;
        return buf.size();
    }
    RecvOwner recv_owner() const noexcept override { return owner_.load(); }
    bool try_claim_recv(RecvOwner w) noexcept override
    {
        RecvOwner e = RecvOwner::none;
        return owner_.compare_exchange_strong(e, w);
    }
    void release_recv() noexcept override { owner_.store(RecvOwner::none); }
    void stop_and_wake() noexcept override { session_.stop_and_wake(); }
    void wait_quiescent() noexcept override { session_.wait_quiescent(); }
    void rebuild() { session_.begin_rebuild(++gen_, create_); }

    std::size_t received() const
    {
        std::lock_guard<std::mutex> lk(m_);
        return n_;
    }

    std::string name_;
    std::uint32_t gen_{1};
    mutable std::mutex m_;
    std::size_t n_{0};
    std::function<std::shared_ptr<ipc::route>()> create_;
    dzIPC::shm::RouteSession session_;
    std::atomic<RecvOwner> owner_{RecvOwner::none};
};

static bool try_send_loop(const std::shared_ptr<ipc::route>& pub, const char* s)
{
    for (int i = 0; i < 60; ++i)
    {
        if (pub->try_send(s, 3, 50)) return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    return false;
}

int main(int argc, char** argv)
{
    const int mode = (argc > 1) ? std::atoi(argv[1]) : 9;
    /* 段名带 pid：每次运行都是干净段，避免上一次运行的残留让预热假失败。 */
    const std::string n = std::string("w04repro_") + std::to_string(mode) + "_" + std::to_string(::getpid());
    ipc::route::clear_storage(n.c_str());
    auto src = std::make_shared<Src>(n);
    auto pub = std::make_shared<ipc::route>(n.c_str(), ipc::sender, false);
    RecvWorker w(0);
    w.start();
    const auto st = w.add_route(src);
    if (st == RecvRegisterStatus::backend_unavailable)
    {
        std::printf("REPRO: backend unavailable\n");
        return 2;
    }
    /* 预热到确实收到一条（否则"没收到"会被误读成停收）。 */
    bool warmed = false;
    for (int round = 0; round < 20 && !warmed; ++round)
    {
        (void)try_send_loop(pub, "warm");
        for (int i = 0; i < 100 && src->received() == 0; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
        warmed = (src->received() > 0);
    }
    std::printf("REPRO[mode=%d]: warmup received=%zu\n", mode, src->received());
    std::fflush(stdout);
    if (!warmed)
    {
        std::printf("REPRO: 预热未成功（环境/组播问题），本轮不判\n");
        std::fflush(nullptr);
        _exit(0);
    }

    if (mode == 1)
    {
        /* 不安全：底层 route 对象的对端被换掉，且**未**先把 token 从 wait-set 摘除。 */
        pub.reset();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        src->rebuild();          // RouteSession::begin_rebuild 内部会 release() 旧 route
    }
    else if (mode == 2)
    {
        /* 不安全：先 rebuild（释放旧 route），再换对端。 */
        src->rebuild();
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        pub.reset();
    }
    else
    {
        /* 安全：同步摘除 wait 项 → 再替换底层对象 → 再注册。 */
        w.remove_route(src.get());
        src->rebuild();
        pub.reset();
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));

    auto pub2 = std::make_shared<ipc::route>(n.c_str(), ipc::sender, false);
    if (mode == 9)
    {
        const auto st2 = w.add_route(src);
        std::printf("REPRO: re-add status=%d\n", static_cast<int>(st2));
    }
    (void)try_send_loop(pub2, "after");
    for (int i = 0; i < 200; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));

    std::printf("REPRO[mode=%d]: survived; received=%zu\n", mode, src->received());
    if (mode == 9)
    {
        w.remove_route(src.get());
        w.stop();
    }
    std::fflush(nullptr);
    /* 段是本进程独有（名字含 pid），收尾清掉，避免污染后续运行。 */
    ipc::route::clear_storage(n.c_str());
    _exit(0);   // 不安全顺序下 worker 线程会崩在 wait 里，用 _exit 避免把死锁/崩溃掩盖成"能退出"
}

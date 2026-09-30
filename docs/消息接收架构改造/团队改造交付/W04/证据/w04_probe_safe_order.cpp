/* 探针 9（W04 关键结论的正向验证）：generation 切换的**安全顺序**
 *   remove_route（同步注销）→ begin_rebuild（释放旧 route）→ add_route（新 token 进 wait-set）
 * 反例顺序（probe8 mode 1/2）已实测 SIGSEGV 于 recv_wait_set::wait：
 * 已注册 token 指向的 route 被 release ⇒ worker 在 wait 里解引用已解映射的共享内存。
 * 有界；不挂死。 */
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
    explicit Src(std::string n) : name_(std::move(n)),
        create_([this] { return std::make_shared<ipc::route>(name_.c_str(), ipc::receiver, false); })
    { session_.begin_rebuild(1, create_); }
    const char* route_name() const noexcept override { return name_.c_str(); }
    std::uint32_t domain_id() const noexcept override { return 0; }
    ipc::recv_wait_token read_wait_token() const noexcept override {
        auto r = session_.current_route(); return r ? r->read_wait_token() : ipc::recv_wait_token{};
    }
    std::size_t recv_once() override {
        auto lease = session_.acquire_receive();
        if (!lease.has_value()) return 0;
        ipc::buff_t buf = lease->route->recv(0);
        session_.release_receive();
        if (buf.empty()) return 0;
        std::lock_guard<std::mutex> lk(m_); n_ += 1;
        return buf.size();
    }
    RecvOwner recv_owner() const noexcept override { return owner_.load(); }
    bool try_claim_recv(RecvOwner w) noexcept override { RecvOwner e = RecvOwner::none; return owner_.compare_exchange_strong(e, w); }
    void release_recv() noexcept override { owner_.store(RecvOwner::none); }
    void stop_and_wake() noexcept override { session_.stop_and_wake(); }
    void wait_quiescent() noexcept override { session_.wait_quiescent(); }
    void rebuild() { session_.begin_rebuild(++gen_, create_); }
    std::size_t n() { std::lock_guard<std::mutex> lk(m_); return n_; }
    std::string name_; std::uint32_t gen_{1}; std::size_t n_{0};
    std::function<std::shared_ptr<ipc::route>()> create_;
    dzIPC::shm::RouteSession session_;
    std::atomic<RecvOwner> owner_{RecvOwner::none};
    std::mutex m_;
};

int main() {
    const std::string n = "w04probe9";
    auto src = std::make_shared<Src>(n);
    auto pub = std::make_shared<ipc::route>(n.c_str(), ipc::sender, false);
    RecvWorker w(0); w.start();
    auto st = w.add_route(src);
    if (st == RecvRegisterStatus::backend_unavailable) { std::printf("P9: backend unavailable\n"); return 2; }
    for (int i = 0; i < 40; ++i) { if (pub->try_send("m1", 3, 50)) break; std::this_thread::sleep_for(std::chrono::milliseconds(5)); }
    for (int i = 0; i < 200 && src->n() < 1; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::printf("P9: m1 received=%zu\n", src->n()); std::fflush(stdout);

    /* 安全顺序：先摘 wait 项，再释放旧 route，再重新注册。 */
    w.remove_route(src.get());
    std::printf("P9: remove_route returned, owner=%d\n", (int)src->recv_owner()); std::fflush(stdout);
    src->rebuild();
    pub.reset();
    auto pub2 = std::make_shared<ipc::route>(n.c_str(), ipc::sender, false);
    auto st2 = w.add_route(src);
    std::printf("P9: re-add_route status=%d\n", (int)st2); std::fflush(stdout);
    for (int i = 0; i < 40; ++i) { if (pub2->try_send("m2", 3, 50)) break; std::this_thread::sleep_for(std::chrono::milliseconds(10)); }
    for (int i = 0; i < 200 && src->n() < 2; ++i) std::this_thread::sleep_for(std::chrono::milliseconds(10));
    std::printf("P9: final received=%zu -> %s\n", src->n(), src->n() >= 2 ? "RESUMED (安全顺序)" : "STOPPED");
    w.remove_route(src.get());
    w.stop();
    std::fflush(nullptr);
    _exit(0);
}

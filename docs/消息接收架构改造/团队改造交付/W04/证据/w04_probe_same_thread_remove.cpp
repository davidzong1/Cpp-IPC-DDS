/* 有界探针：从 worker 线程内部调用 remove_route（头文件声称有兜底）。
 * 续写机制说明：stdout 在调用前 fflush，若调用不返回，后面的 printf 就永不出现。 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

#include "dzIPC/shm_route_session.h"
#include "dzIPC/threepools/recv_worker.h"
#include "libipc/ipc.h"

using namespace dzIPC::threepools;

struct Src : RecvRouteSource {
    explicit Src(const std::string& n) : name_(n),
        create_([this] { return std::make_shared<ipc::route>(name_.c_str(), ipc::receiver, false); })
    { session_.begin_rebuild(1, create_); }

    const char* route_name() const noexcept override { return name_.c_str(); }
    std::uint32_t domain_id() const noexcept override { return 0; }
    ipc::recv_wait_token read_wait_token() const noexcept override {
        auto r = session_.current_route(); return r ? r->read_wait_token() : ipc::recv_wait_token{};
    }
    std::size_t recv_once() override {
        if (!removed_.exchange(true)) {
            std::printf("PROBE: inside recv_once, calling remove_route(self) from worker thread\n");
            std::fflush(stdout);
            worker_->remove_route(this);
            std::printf("PROBE: remove_route RETURNED (same-thread fallback exists)\n");
            std::fflush(stdout);
        }
        return 0;
    }
    RecvOwner recv_owner() const noexcept override { return owner_.load(); }
    bool try_claim_recv(RecvOwner who) noexcept override {
        RecvOwner e = RecvOwner::none; return owner_.compare_exchange_strong(e, who);
    }
    void release_recv() noexcept override { owner_.store(RecvOwner::none); }
    void stop_and_wake() noexcept override { session_.stop_and_wake(); }
    void wait_quiescent() noexcept override { session_.wait_quiescent(); }

    std::string name_;
    std::atomic<bool> removed_{false};
    std::function<std::shared_ptr<ipc::route>()> create_;
    dzIPC::shm::RouteSession session_;
    std::atomic<RecvOwner> owner_{RecvOwner::none};
    RecvWorker* worker_{nullptr};
};

int main() {
    const std::string n = "w04probe_route";
    RecvWorker w2(0);
    w2.start();
    auto s2 = std::make_shared<Src>(n);
    s2->worker_ = &w2;
    const auto st = w2.add_route(s2);
    if (st == RecvRegisterStatus::backend_unavailable) { std::printf("PROBE: backend unavailable\n"); return 2; }
    std::printf("PROBE: add_route status=%d; the worker will now call recv_once -> remove_route(self)\n",
                static_cast<int>(st));
    std::fflush(stdout);
    for (int i = 0; i < 30; ++i) {          // 有界等待：最多 3s，不永久挂住
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    std::printf("PROBE: main survived 3s of waiting (no deadlock observed in this window)\n");
    std::fflush(stdout);
    /* 故意**不**再调 remove_route/w2.stop()：若同线程兜底不存在，worker 线程正卡在自己的
     * 注销等待里，任何 stop()/join 都会一起挂住 —— 那正是本条要证伪的形态。
     * 用 _exit 直接结束，避免把死锁掩盖成"能退出"。 */
    std::fflush(nullptr);
    _exit(0);
}

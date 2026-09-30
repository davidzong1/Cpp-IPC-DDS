/* 探针 10：socket 侧「假就绪兜底」的实际效果（契约 §5 表格最后一行）。
 * 构造：一个**真**可读但从不被消费的 fd（pipe 读端，LT），宿主 recv_once 恒返回 0。
 * 判据（只报事实，不判 bug）：
 *   · recv_once 速率：是否被 kMaxFruitlessReadiness 限制住，还是以 loop 速度自旋；
 *   · 同 worker 的真邻居是否仍被服务（不饿死）。
 * 有界 2s；不挂死。 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>

#include "dzIPC/threepools/socket_recv_worker.h"
#include "libipc/ipc.h"

using namespace dzIPC::threepools;

struct PipeRoute : SocketRecvRouteSource {
    explicit PipeRoute(std::string n, int fd) : name_(std::move(n)), fd_(fd) {}
    const char* route_name() const noexcept override { return name_.c_str(); }
    std::uint32_t domain_id() const noexcept override { return 0; }
    SocketWaitToken wait_token() const noexcept override { return SocketWaitToken{this, static_cast<std::uintptr_t>(fd_)}; }
    std::size_t recv_once() override { calls_.fetch_add(1, std::memory_order_relaxed); return 0; }
    RecvOwner recv_owner() const noexcept override { return owner_.load(); }
    bool try_claim_recv(RecvOwner w) noexcept override { RecvOwner e = RecvOwner::none; return owner_.compare_exchange_strong(e, w); }
    void release_recv() noexcept override { owner_.store(RecvOwner::none); }
    void stop_and_wake() noexcept override {}
    void wait_quiescent() noexcept override {}
    std::string name_; int fd_;
    std::atomic<RecvOwner> owner_{RecvOwner::none};
    std::atomic<std::size_t> calls_{0};
};

/* 真邻居：真 UDPNode 通道（用 libipc 的 socket 路径太重，这里用第二个 pipe 做“真数据”
 * 通道，recv_once 真的把 1 字节读出来 ⇒ 它必须仍被服务）。 */
struct DataPipeRoute : SocketRecvRouteSource {
    explicit DataPipeRoute(std::string n, int fd) : name_(std::move(n)), fd_(fd) {}
    const char* route_name() const noexcept override { return name_.c_str(); }
    std::uint32_t domain_id() const noexcept override { return 0; }
    SocketWaitToken wait_token() const noexcept override { return SocketWaitToken{this, static_cast<std::uintptr_t>(fd_)}; }
    std::size_t recv_once() override {   // 契约要求：非阻塞或短超时
        char c = 0;
        const ssize_t n = ::read(fd_, &c, 1);
        if (n <= 0) return 0;
        got_.fetch_add(1, std::memory_order_relaxed);
        return 1;
    }
    RecvOwner recv_owner() const noexcept override { return owner_.load(); }
    bool try_claim_recv(RecvOwner w) noexcept override { RecvOwner e = RecvOwner::none; return owner_.compare_exchange_strong(e, w); }
    void release_recv() noexcept override { owner_.store(RecvOwner::none); }
    void stop_and_wake() noexcept override {}
    void wait_quiescent() noexcept override {}
    std::string name_; int fd_;
    std::atomic<RecvOwner> owner_{RecvOwner::none};
    std::atomic<std::size_t> got_{0};
};

int main() {
    int p[2];
    if (::pipe(p) != 0) { std::printf("P10: pipe failed\n"); return 2; }
    char buf[64]; std::memset(buf, 'x', sizeof(buf));
    (void)::write(p[1], buf, sizeof(buf));   // 读端持续可读（LT），且永不消费

    auto route = std::make_shared<PipeRoute>("w04_probe10", p[0]);
    SocketRecvWorker w(0);
    w.start();
    const auto st = w.add_route(route);
    if (st == RecvRegisterStatus::backend_unavailable) { std::printf("P10: backend unavailable\n"); return 2; }
    if (st != RecvRegisterStatus::ok) { std::printf("P10: add_route=%d\n", (int)st); return 2; }
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    const auto a = route->calls_.load();
    std::this_thread::sleep_for(std::chrono::seconds(1));
    const auto b = route->calls_.load();
    std::printf("P10: 恒返回 0 的 LT 可读通道：1s 内 recv_once=%zu 次（%.0f/s）\n", b - a, double(b - a));
    /* 同 worker 的真邻居：在忙转期间必须仍被服务（不饿死）。 */
    int q[2];
    if (::pipe(q) == 0) {
        (void)::write(q[1], "ab", 2);
        (void)::fcntl(q[0], F_SETFL, O_NONBLOCK);   // 邻居 recv_once 必须非阻塞（契约义务）
        auto neighbour = std::make_shared<DataPipeRoute>("w04_probe10_neighbour", q[0]);
        w.add_route(neighbour);
        const auto t0 = std::chrono::steady_clock::now();
        while (neighbour->got_.load() < 1 && std::chrono::steady_clock::now() - t0 < std::chrono::seconds(2)) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        std::printf("P10: 忙转期间同 worker 真邻居 got=%zu（不饿死判据 >=1）\n", neighbour->got_.load());
        w.remove_route(neighbour.get());
        ::close(q[0]); ::close(q[1]);
    }
    w.remove_route(route.get());
    std::printf("P10: remove_route returned; owner=%d calls_total=%zu\n", (int)route->recv_owner(), route->calls_.load());
    w.stop();
    ::close(p[0]); ::close(p[1]);
    std::fflush(nullptr);
    _exit(0);
}

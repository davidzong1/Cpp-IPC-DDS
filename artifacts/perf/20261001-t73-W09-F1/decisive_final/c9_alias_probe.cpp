/* 自包含反例探针：专属前缀 + 每轮种子进程 + 8 话题并发首借；报告同 id / 同 data 指针对数。 */
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <vector>
#include "libipc/ipc.h"

namespace {
constexpr char const *kPrefix = "w09c9alias";   /* ⛔ 不用空前缀：全机共享会被邻居污染 */
constexpr int kTopics = 8;
constexpr std::size_t kLoanSize = 8000;         /* ⇒ 档 9216 */

int one_round(int seed)
{
    std::vector<std::unique_ptr<ipc::route>> rx, tx;
    for (int i = 0; i < kTopics; ++i) {
        const std::string t = "c9_" + std::to_string(seed) + "_t" + std::to_string(i);
        rx.emplace_back(new ipc::route{ipc::prefix{kPrefix}, t.c_str(), ipc::receiver});
        tx.emplace_back(new ipc::route{ipc::prefix{kPrefix}, t.c_str(), ipc::sender});
    }
    for (int i = 0; i < kTopics; ++i)
        if (!tx[(std::size_t)i]->wait_for_recv(1, 3000)) return 3;
    std::atomic<int> go{0};
    std::mutex m;
    struct Owned { int t; ipc::loan_t lo; };
    std::vector<Owned> held;
    std::vector<std::thread> th;
    for (int i = 0; i < kTopics; ++i)
        th.emplace_back([&, i] {
            while (go.load(std::memory_order_acquire) == 0) {}
            auto lo = tx[(std::size_t)i]->loan(kLoanSize);
            if (lo.valid()) {
                std::memset(lo.data, 'A' + i, 64);
                std::lock_guard<std::mutex> g(m);
                held.push_back(Owned{i, lo});
            }
        });
    go.store(1, std::memory_order_release);
    for (auto &x : th) x.join();

    int ids = 0, ptrs = 0;
    for (std::size_t i = 0; i < held.size(); ++i)
        for (std::size_t j = i + 1; j < held.size(); ++j) {
            if (held[i].t == held[j].t) continue;
            if (held[i].lo.id == held[j].lo.id) ++ids;
            if (held[i].lo.data == held[j].lo.data) ++ptrs;
        }
    for (auto &h : held) tx[(std::size_t)h.t]->discard_loan(h.lo);
    std::fflush(nullptr);
    return (ids || ptrs) ? 1 : 0;
}
}  // namespace

int main(int argc, char **argv)
{
    const int rounds = argc > 1 ? std::atoi(argv[1]) : 1200;
    int bad = 0, skip = 0, ids_total = 0, ptrs_total = 0;
    /* ⛔ 只清一次（不是每轮清）：每轮的"非素净"由种子进程现造；清太勤会退化成"永远全新段"。 */
    ipc::route::clear_storage(ipc::prefix{kPrefix}, "c9_seed");
    for (int r = 0; r < rounds && bad == 0; ++r) {
        /* ① 种子进程：借 3 块后 _exit（不归还）⇒ 段留存且空闲链**非素净** ⇒ 复位才会进入。 */
        const ::pid_t sp = ::fork();
        if (sp == 0) {
            ipc::route stx{ipc::prefix{kPrefix}, "c9_seed", ipc::sender};
            ipc::route srx{ipc::prefix{kPrefix}, "c9_seed", ipc::receiver};
            if (!stx.wait_for_recv(1, 3000)) ::_exit(0);
            std::vector<ipc::loan_t> keep;
            for (int i = 0; i < 3; ++i) {
                auto lo = stx.loan(kLoanSize);
                if (!lo.valid()) break;
                keep.push_back(lo);
            }
            std::fflush(nullptr);
            ::_exit(0);
        }
        int sst = 0; (void)::waitpid(sp, &sst, 0);
        /* ② 测试进程：8 话题并发首借。 */
        const ::pid_t pid = ::fork();
        if (pid == 0) ::_exit(one_round(r));
        int st = 0; (void)::waitpid(pid, &st, 0);
        if (!WIFEXITED(st)) { std::printf("C9_ABNORMAL r=%d sig=%d\n", r, WIFSIGNALED(st) ? WTERMSIG(st) : -1); std::fflush(nullptr); return 1; }
        const int c = WEXITSTATUS(st);
        if (c == 3) ++skip; else if (c == 1) ++bad;
    }
    std::printf("C9_ALIAS rounds=%d topics=%d bad_rounds=%d skipped=%d\n", rounds, kTopics, bad, skip);
    std::fflush(nullptr);
    (void)ids_total; (void)ptrs_total;
    return bad ? 1 : 0;
}

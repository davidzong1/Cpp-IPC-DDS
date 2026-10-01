/* t65：检验 `reclaim_orphan_segment` 的**重锁后 memcmp 复核**是否结构上必要。
 *
 * 窗口（读源码得出）：`chunk_storages().get_info()` 在 `lock_` 内只做
 * "建 handle + 记 newly_attached"，**释放锁之后**才调 `reclaim_orphan_segment`
 * （其中含 ms 级的 /proc 扫描）。同期另一个线程再调 get_info 时
 * `h->valid()` 已为真 ⇒ `newly_attached=false` ⇒ **直接拿到 info 并借块**，
 * 不必等第一个线程的探活结束。若无 memcmp 复核，第一个线程随后 reset 会把
 * 第二个线程正持有的 id 再次发出去（双重分配）。
 *
 * 本探针：同一 (prefix, size) 上 N 线程同时首次 loan()，收集**成功**的 id，
 * 检测是否存在"两个不同线程同时持有同一 id"。
 */
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include "libipc/ipc.h"

int main(int argc, char** argv) {
    const int nthreads = argc > 1 ? atoi(argv[1]) : 8;
    const int rounds   = argc > 2 ? atoi(argv[2]) : 40;
    const std::size_t kReq = 8000;      /* ⇒ chunk 档 9216 */
    int dup_rounds = 0, total_dup = 0, held_total = 0;
    for (int r = 0; r < rounds; ++r) {
        const std::string prefix = "t65race_" + std::to_string(r);
        const std::string topic  = "t65race_topic_" + std::to_string(r);
        ipc::route rx{ipc::prefix{prefix.c_str()}, topic.c_str(), ipc::receiver};
        ipc::route tx{ipc::prefix{prefix.c_str()}, topic.c_str(), ipc::sender};
        if (!tx.wait_for_recv(1, 3000)) { std::printf("NO_RECV r=%d\n", r); return 3; }
        std::atomic<int> go{0};
        std::vector<std::vector<int>> ids(nthreads);
        std::vector<ipc::loan_t> held_loans;
        std::mutex m;
        std::vector<std::thread> th;
        for (int t = 0; t < nthreads; ++t) {
            th.emplace_back([&, t] {
                while (go.load() == 0) {}
                auto lo = tx.loan(kReq);
                std::lock_guard<std::mutex> g(m);
                if (lo.valid()) { ids[t].push_back(static_cast<int>(lo.id)); held_loans.push_back(lo); }
            });
        }
        go.store(1);
        for (auto& x : th) x.join();
        for (auto& lo : held_loans) tx.discard_loan(lo);   /* ⛔ 先归还再清段 */
        held_loans.clear();
        /* 跨线程重复检测 */
        int seen[64] = {0};
        int dup = 0, held = 0;
        for (int t = 0; t < nthreads; ++t)
            for (int id : ids[t]) {
                ++held;
                if (id >= 0 && id < 64) { if (seen[id]++) ++dup; }
            }
        held_total += held;
        if (dup) {
            ++dup_rounds; total_dup += dup;
            std::printf("DUP_ROUND r=%d prefix=%s dup=%d ids=", r, prefix.c_str(), dup);
            for (int t = 0; t < nthreads; ++t)
                for (int id : ids[t]) std::printf("%d,", id);
            std::printf("\n");
            std::fflush(stdout);
        }
    }
    std::printf("RACE_PROBE threads=%d rounds=%d held_total=%d dup_rounds=%d total_dup=%d\n",
                nthreads, rounds, held_total, dup_rounds, total_dup);
    return dup_rounds > 0 ? 1 : 0;
}

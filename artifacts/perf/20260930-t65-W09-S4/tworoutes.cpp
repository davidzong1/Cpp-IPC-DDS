/* t65（决定性）：**两个不同话题**（各自单生产者，完全合法的用法）在同一进程中并发首次借样。
 * 池归属键 = (prefix='', chunk_size) ⇒ 两个话题**共用同一个池段**。
 * 若 `reclaim_orphan_segment` 的锁外窗口把 A 线程已借出的 id 复位重发 ⇒ 两个话题拿到同一 id
 * ⇒ 两块"不同消息"指向同一内存 ⇒ **堆/段内数据互相踩踏**（数据面正确性缺陷）。 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include "libipc/ipc.h"

int main(int argc, char** argv) {
    const int rounds = argc > 1 ? atoi(argv[1]) : 40;
    const int nt = argc > 2 ? atoi(argv[2]) : 2;      /* 话题对数（每对 1 生产者线程） */
    int bad_rounds = 0, same_id = 0, same_ptr = 0;
    for (int r = 0; r < rounds; ++r) {
        std::vector<ipc::route> rx, tx;
        std::vector<std::unique_ptr<ipc::route>> keep_rx, keep_tx;
        for (int i = 0; i < nt; ++i) {
            const std::string t = "t65two_r" + std::to_string(r) + "_" + std::to_string(i);
            keep_rx.emplace_back(new ipc::route{ipc::prefix{nullptr}, t.c_str(), ipc::receiver});
            keep_tx.emplace_back(new ipc::route{ipc::prefix{nullptr}, t.c_str(), ipc::sender});
        }
        for (int i = 0; i < nt; ++i)
            if (!keep_tx[(size_t)i]->wait_for_recv(1, 3000)) { std::printf("NO_RECV\n"); return 3; }
        std::atomic<int> go{0};
        std::vector<ipc::loan_t> held; std::mutex m;
        std::vector<std::thread> th;
        for (int i = 0; i < nt; ++i) {
            th.emplace_back([&, i] {
                while (go.load() == 0) {}
                auto lo = keep_tx[(size_t)i]->loan(8000);
                if (lo.valid()) {
                    std::memset(lo.data, 'A' + i, 64);
                    std::lock_guard<std::mutex> g(m);
                    held.push_back(lo);
                }
            });
        }
        go.store(1);
        for (auto& x : th) x.join();
        int si = 0, sp = 0;
        for (size_t i = 0; i < held.size(); ++i)
            for (size_t j = i + 1; j < held.size(); ++j) {
                if (held[i].id == held[j].id) ++si;
                if (held[i].data == held[j].data) ++sp;
            }
        if (si || sp) {
            ++bad_rounds; same_id += si; same_ptr += sp;
            std::printf("TWO_ROUTES_ALIAS r=%d same_id=%d same_data_ptr=%d bytes=[", r, si, sp);
            for (auto& lo : held) std::printf("%c", ((const char*)lo.data)[0]);
            std::printf("]\n"); std::fflush(stdout);
        }
        for (auto& lo : held) keep_tx[0]->discard_loan(lo);   /* 归还，保持池健康 */
    }
    std::printf("TWO_ROUTES_TOTAL rounds=%d topics=%d bad_rounds=%d same_id_pairs=%d same_ptr_pairs=%d\n",
                rounds, nt, bad_rounds, same_id, same_ptr);
    return bad_rounds > 0 ? 1 : 0;
}

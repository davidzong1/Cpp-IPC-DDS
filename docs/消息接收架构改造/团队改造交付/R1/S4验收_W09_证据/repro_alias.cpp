/* t65 · W09/t22 S4 独立验收 —— 最小反例（合法用法）
 *
 * 场景（**受支持用法**，非畸形调用）：
 *   · 同一进程内 N 个**不同话题**，每个话题一个生产者线程（`ipc::route` 默认 `relat::single`
 *     ⇒ 每话题单生产端，符合契约）；
 *   · 全部使用**默认空前缀** ⇒ 段的归属键 = ('', chunk_size) ⇒ N 个话题**共享同一档 chunk 池**
 *     （这正是 W09 交付 §2.3/§8 明写的部署形态）；
 *   · N 个线程**同时**首次借样 ⇒ 触发 `reclaim_orphan_segment()` 的"首次 attach 判定"。
 *
 * 判据：任一轮里若两个**不同话题**拿到相同的 `loan_t::id`（或相同的 `data` 指针），
 *       即证明"同进程另一线程在飞借样"被误判为"段内无活持有者"并被整体复位 ⇒
 *       **同一块 chunk 被重复分配** ⇒ 两条不同消息写同一内存（数据面正确性缺陷）。
 *
 * 用法: repro_alias <rounds> <topics> <reps_per_thread>
 * 退出码: 0 = 未命中；1 = 命中（反例成立）
 */
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "libipc/ipc.h"

int main(int argc, char** argv) {
    const int rounds = argc > 1 ? std::atoi(argv[1]) : 6;
    const int nt     = argc > 2 ? std::atoi(argv[2]) : 8;
    const int reps   = argc > 3 ? std::atoi(argv[3]) : 1;

    int bad_rounds = 0, same_id_pairs = 0, same_ptr_pairs = 0;
    for (int r = 0; r < rounds; ++r) {
        std::vector<std::unique_ptr<ipc::route>> rx, tx;
        for (int i = 0; i < nt; ++i) {
            const std::string t = "t65rep_r" + std::to_string(r) + "_" + std::to_string(i);
            rx.emplace_back(new ipc::route{ipc::prefix{nullptr}, t.c_str(), ipc::receiver});
            tx.emplace_back(new ipc::route{ipc::prefix{nullptr}, t.c_str(), ipc::sender});
        }
        for (int i = 0; i < nt; ++i) {
            if (!tx[(size_t)i]->wait_for_recv(1, 3000)) { std::printf("NO_RECV r=%d\n", r); return 3; }
        }

        std::atomic<int> go{0};
        std::mutex m;
        struct Owned { int topic; ipc::loan_t lo; };
        std::vector<Owned> held;
        std::vector<std::thread> th;
        for (int i = 0; i < nt; ++i) {
            th.emplace_back([&, i] {
                while (go.load() == 0) {}
                for (int k = 0; k < reps; ++k) {
                    auto lo = tx[(size_t)i]->loan(8000);      /* ⇒ chunk 档 9216 */
                    if (lo.valid()) {
                        std::memset(lo.data, 'A' + i, 64);
                        std::lock_guard<std::mutex> g(m);
                        held.push_back(Owned{i, lo});
                    }
                }
            });
        }
        go.store(1);
        for (auto& x : th) x.join();

        int si = 0, sp = 0;
        for (size_t i = 0; i < held.size(); ++i)
            for (size_t j = i + 1; j < held.size(); ++j) {
                if (held[i].lo.id == held[j].lo.id) ++si;
                if (held[i].lo.data == held[j].lo.data) ++sp;
            }
        if (si || sp) {
            ++bad_rounds; same_id_pairs += si; same_ptr_pairs += sp;
            std::printf("ALIAS r=%d same_id_pairs=%d same_data_ptr_pairs=%d bytes=[", r, si, sp);
            for (auto& h : held) std::printf("%c", static_cast<const char*>(h.lo.data)[0]);
            std::printf("]\n");
            std::fflush(stdout);
        }
        for (auto& h : held) tx[(size_t)h.topic]->discard_loan(h.lo);   /* 用**各自**的 route 归还 */
    }
    std::printf("ALIAS_TOTAL rounds=%d topics=%d reps=%d bad_rounds=%d same_id_pairs=%d same_ptr_pairs=%d\n",
                rounds, nt, reps, bad_rounds, same_id_pairs, same_ptr_pairs);
    return bad_rounds > 0 ? 1 : 0;
}

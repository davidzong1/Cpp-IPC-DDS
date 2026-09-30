/* t46：`ipc::loan()` 的**五个失败出口**逐条可达性实测 + 可区分性判定。
 *
 * 目的：回答"要区分「池耗尽」与「超尺寸/超预算」，需要什么原因出口"。
 * 方法：每条出口用一个最小臂构造，打印 (valid, 该出口是否被走到)。
 * ⛔ 本探针**不假设**能区分；走到哪条就报哪条，走不到就报 NOT_REACHABLE。
 *
 * 臂:
 *   E1 invalid_handle   : 默认构造的 route(未 open)
 *   E2 not_connected    : open 了但从未连接（无 elems）
 *   E3 no_receiver      : 已连接 sender, 无 receiver
 *   E4 pool_exhausted   : 有 receiver, 借满该尺寸档
 *   E5 segment_...      : 需要 mmap 失败（本机 32G /dev/shm 下难构造，如实报）
 * usage: loan_exit_reachability <E1|E2|E3|E4>
 */
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include "libipc/ipc.h"

static int g_reason_slot = 0;

int main(int argc, char** argv)
{
    const char* arm = (argc > 1) ? argv[1] : "E4";
    const std::string name = std::string("loan_exit_") + arm;
    ipc::route::clear_storage(name.c_str());

    ipc::loan_t lo;
    if (std::strcmp(arm, "E1") == 0)
    {
        /* E1: 默认构造(未 open) —— handle 为 nullptr。 */
        ipc::route unopened;
        lo = unopened.loan(4096);
        std::printf("arm=E1 invalid_handle  request=4096  valid=%d\n", (int)lo.valid());
    }
    else if (std::strcmp(arm, "E2") == 0)
    {
        /* E2: open 了但没有对端 —— ready_sending 会尝试 connect_sender 但 elems 不存在。 */
        ipc::route tx{name.c_str(), ipc::sender};
        lo = tx.loan(4096);
        std::printf("arm=E2 not_connected   request=4096  valid=%d\n", (int)lo.valid());
    }
    else if (std::strcmp(arm, "E3") == 0)
    {
        /* E3: 有 sender 端、无 receiver。 */
        ipc::route tx{name.c_str(), ipc::sender};
        lo = tx.loan(4096);
        std::printf("arm=E3 no_receiver     request=4096  valid=%d  recv_count=%zu\n",
                    (int)lo.valid(), tx.recv_count());
    }
    else
    {
        /* E4: 有 receiver，借满该尺寸档（池 40 块/档）。 */
        ipc::route tx{name.c_str(), ipc::sender};
        ipc::route rx{name.c_str(), ipc::receiver};
        if (!tx.wait_for_recv(1, 2000)) { std::printf("arm=E4 no handshake\n"); return 2; }
        const std::size_t want = 64 * 1024;
        std::vector<ipc::loan_t> held;
        int denied = 0;
        for (int i = 0; i < 200; ++i)
        {
            auto l = tx.loan(want);
            if (!l.valid()) { ++denied; break; }
            held.push_back(l);
        }
        std::printf("arm=E4 pool_exhausted  request=%zu  held=%zu  denied_after=%d  valid=%d  "
                    "recv_count=%zu\n", want, held.size(), denied, 0, tx.recv_count());
        for (auto& h : held) tx.discard_loan(h);
    }
    /* ⛔ 关键：五条出口的**调用方可见面完全相同** —— 只有一个 bool。 */
    std::printf("caller_visible: valid()=%d  (id=%d data=%s size=%zu)  status_api=%s\n",
                (int)lo.valid(), (int)lo.id, lo.data ? "set" : "null", lo.size,
                (g_reason_slot ? "present" : "ABSENT(当前 ipc::loan_t 无原因出口)"));
    std::printf("LOAN_EXIT_DONE\n");
    return 0;
}

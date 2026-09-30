/* 父进程：用**不同 route 名**（不同话题），但**同一 (prefix 空 ⇒ 同一尺寸档)** 的 chunk 池。
 * ⛔ 这样才不会踩「单生产端 sender flag 被死进程永久占用」那条（不同 route ⇒ 不同 queue ⇒
 *    不同 sender flag），从而把「未发布借样的 chunk 占用」单独测出来。
 * 判据（队长预期）：剩余 = 40 − CHILD_HELD（即 39，而不是 0）。 */
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>
#include "libipc/ipc.h"
int main(int argc, char** argv)
{
    const char* topic = (argc > 1) ? argv[1] : "w09leak_probe2";
    const std::size_t req = 8192;
    ipc::route tx{topic, ipc::sender};
    ipc::route rx{topic, ipc::receiver};
    if (!tx.wait_for_recv(1, 3000)) { std::printf("PARENT_SETUP_FAIL\n"); return 2; }
    std::vector<ipc::loan_t> held;
    for (int i = 0; i < 60; ++i) { auto lo = tx.loan(req); if (!lo.valid()) break; held.push_back(lo); }
    std::printf("PARENT_CAN_LOAN=%zu (pool=%zu)\n", held.size(), (std::size_t)ipc::large_msg_cache);
    for (auto& lo : held) tx.discard_loan(lo);
    return 0;
}

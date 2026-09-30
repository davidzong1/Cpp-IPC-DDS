/* 父进程：在已有一个"借样后暴死"的子进程之后，看同一档还能借到几块。
 * 判据：若 published=0 的死悬挂**不被**清扫 ⇒ 剩余可用 = 40 - CHILD_HELD。 */
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <vector>
#include "libipc/ipc.h"
int main(){
    const std::size_t req = 8192;
    ipc::route tx{ipc::prefix{"w09leak"}, "w09leak_topic", ipc::sender};
    ipc::route rx{ipc::prefix{"w09leak"}, "w09leak_topic", ipc::receiver};
    if (!tx.wait_for_recv(1, 3000)) { std::printf("PARENT_SETUP_FAIL\n"); return 2; }
    std::vector<ipc::loan_t> held;
    for (int i = 0; i < 60; ++i) { auto lo = tx.loan(req); if (!lo.valid()) break; held.push_back(lo); }
    std::printf("PARENT_CAN_LOAN=%zu (pool=%zu)\n", held.size(), (std::size_t)ipc::large_msg_cache);
    for (auto& lo : held) tx.discard_loan(lo);
    return 0;
}

/* 子进程：在 prefix=w09leak 上借 N 块 loan，**不发布**（published 保持 0），
 * 然后 _exit（不跑析构 ⇒ 不 discard）。模拟"应用借样后暴死"。 */
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <unistd.h>
#include <vector>
#include "libipc/ipc.h"
int main(int argc, char** argv){
    const int n = (argc>1)? std::atoi(argv[1]) : 40;
    const std::size_t req = 8192;
    ipc::route tx{ipc::prefix{"w09leak"}, "w09leak_topic", ipc::sender};
    ipc::route rx{ipc::prefix{"w09leak"}, "w09leak_topic", ipc::receiver};
    if (!tx.wait_for_recv(1, 3000)) { std::printf("CHILD_SETUP_FAIL\n"); return 2; }
    int got = 0;
    for (int i = 0; i < n; ++i) { auto lo = tx.loan(req); if (!lo.valid()) break; (void)lo; ++got; }
    std::printf("CHILD_HELD=%d (published 全为 0)\n", got);
    std::fflush(stdout);
    ::_exit(0);
}

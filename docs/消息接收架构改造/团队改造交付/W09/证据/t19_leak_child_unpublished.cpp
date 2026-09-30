/* t19 更正实验（队长 D-16 第 3 条）：
 * 子进程在 prefix=w09leak、话题名 w09leak_topic 上借 N 块 loan，**不发布**，_exit。
 * 目的：制造「死进程的未发布借样」悬挂，供父进程用**不同 route 名**测同档剩余块数。 */
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include "libipc/ipc.h"
int main(int argc, char** argv)
{
    const int n = (argc > 1) ? std::atoi(argv[1]) : 40;
    const std::size_t req = 8192;
    const char* tn = (argc > 2) ? argv[2] : "w09leak_topic";
    ipc::route tx{tn, ipc::sender};
    ipc::route rx{tn, ipc::receiver};
    if (!tx.wait_for_recv(1, 3000)) { std::printf("CHILD_SETUP_FAIL\n"); return 2; }
    int got = 0;
    for (int i = 0; i < n; ++i) { auto lo = tx.loan(req); if (!lo.valid()) break; (void)lo; ++got; }
    std::printf("CHILD_HELD=%d (published 全为 0)\n", got);
    std::fflush(stdout);
    ::_exit(0);
}

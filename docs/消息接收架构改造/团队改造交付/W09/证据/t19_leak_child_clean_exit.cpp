#include <cstdio>
#include <cstdlib>
#include "libipc/ipc.h"
int main(int argc, char** argv) {
    const int n = (argc > 1) ? std::atoi(argv[1]) : 1;
    const char* tn = (argc > 2) ? argv[2] : "w09samenormal";
    { ipc::route tx{tn, ipc::sender}; ipc::route rx{tn, ipc::receiver};
      if (!tx.wait_for_recv(1, 3000)) { std::printf("CHILD_SETUP_FAIL\n"); return 2; }
      int got = 0;
      for (int i = 0; i < n; ++i) { auto lo = tx.loan(8192); if (!lo.valid()) break; (void)lo; ++got; }
      std::printf("CHILD_HELD=%d\n", got); std::fflush(stdout);
      return 0;   /* 正常返回 ⇒ 跑析构 ⇒ disconnect_sender() */
    }
}

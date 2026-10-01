/* t65：造一个**指定 prefix 的残留段**：子进程建路 + 借满 40 块，然后 _exit(0)（不跑析构 ⇒ 段留下）。
 * 用法: leave_seg <prefix>   —— 父进程打印子进程 pid 后立即退出（段由子进程留下）。 */
#include <cstdlib>
#include <cstdio>
#include <string>
#include <unistd.h>
#include <sys/wait.h>
#include <vector>
#include "libipc/ipc.h"
int main(int argc,char**argv){
  const std::string p = argc>1?argv[1]:"t65leave";
  const std::string t = p + "_topic";
  const pid_t c = ::fork();
  if (c == 0) {
    ipc::route rx{ipc::prefix{p.c_str()}, t.c_str(), ipc::receiver};
    ipc::route tx{ipc::prefix{p.c_str()}, t.c_str(), ipc::sender};
    if (!tx.wait_for_recv(1, 3000)) ::_exit(3);
    std::vector<ipc::loan_t> held;
    for (int i=0;i<40;++i){ auto lo=tx.loan(8000); if(!lo.valid()) break; held.push_back(lo); }
    std::printf("child held=%zu\n", held.size());
    std::fflush(stdout);
    ::_exit(0);                 /* ⛔ 不跑析构 ⇒ 段不被 unlink，借样不归还 */
  }
  int st=0; ::waitpid(c,&st,0);
  std::printf("child exited status=%d\n", st);
  return 0;
}

/* t65：为什么第 1 轮就会走到"非素净"分支？逐步打印 */
#include <cstdio>
#include <string>
#include "libipc/ipc.h"
int main(){
  const std::string p="t65why", t="t65why_topic";
  ipc::route::clear_storage(ipc::prefix{p.c_str()}, t.c_str());
  ipc::route rx{ipc::prefix{p.c_str()}, t.c_str(), ipc::receiver};
  ipc::route tx{ipc::prefix{p.c_str()}, t.c_str(), ipc::sender};
  std::printf("wait=%d\n", (int)tx.wait_for_recv(1,3000));
  std::system("ls /dev/shm | grep t65why | sed 's/^/  seg /'");
  auto a = tx.loan(8000);
  std::printf("loan1 valid=%d id=%d\n", (int)a.valid(), a.valid()?(int)a.id:-1);
  auto b = tx.loan(8000);
  std::printf("loan2 valid=%d id=%d\n", (int)b.valid(), b.valid()?(int)b.id:-1);
  auto c = tx.loan(8000);
  std::printf("loan3 valid=%d id=%d\n", (int)c.valid(), c.valid()?(int)c.id:-1);
  tx.discard_loan(a); tx.discard_loan(b); tx.discard_loan(c);
  return 0;
}

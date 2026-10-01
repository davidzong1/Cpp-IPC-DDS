/* t65：把"重复 id"升级为"**两块借样指向同一内存**"的直接证据。 */
#include <atomic>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include "libipc/ipc.h"
int main(int argc,char**argv){
  const int nt = argc>1?atoi(argv[1]):2;
  const int rounds = argc>2?atoi(argv[2]):60;
  const int reps = argc>3?atoi(argv[3]):1;   /* 每轮内部再重复借样次数 */
  int bad_rounds = 0, bad_alias = 0;
  for (int r = 0; r < rounds; ++r) {
  const std::string p = "t65alias_" + std::to_string(r), t = p + "_topic";
  ipc::route tx{ipc::prefix{p.c_str()}, t.c_str(), ipc::sender};
  ipc::route rx{ipc::prefix{p.c_str()}, t.c_str(), ipc::receiver};
  if(!tx.wait_for_recv(1,3000)){ std::printf("NO_RECV r=%d\n", r); return 3; }
  std::atomic<int> go{0};
  std::vector<ipc::loan_t> held; std::mutex m;
  std::vector<std::thread> th;
  for(int i=0;i<nt;++i) th.emplace_back([&,i]{
    while(go.load()==0){}
    for (int k=0;k<reps;++k){
      auto lo = tx.loan(8000);
      if(lo.valid()){ std::memset(lo.data, 'A'+i, 64); std::lock_guard<std::mutex> g(m); held.push_back(lo); }
    }
  });
  go.store(1);
  for(auto&x:th)x.join();
  int same_ptr=0, same_id=0;
  for(size_t i=0;i<held.size();++i) for(size_t j=i+1;j<held.size();++j)
    if(held[i].data == held[j].data) ++same_ptr;
  for(size_t i=0;i<held.size();++i) for(size_t j=i+1;j<held.size();++j)
    if(held[i].id == held[j].id) ++same_id;
  if (same_id || same_ptr) {
    std::printf("ALIAS_ROUND r=%d prefix=%s same_id=%d same_data_ptr=%d bytes=[", r, p.c_str(), same_id, same_ptr);
    for(auto&lo:held) std::printf("%c", ((const char*)lo.data)[0]);
    std::printf("]\n"); std::fflush(stdout);
    ++bad_rounds; bad_alias += same_ptr;
  }
  for(auto&lo:held) tx.discard_loan(lo);
  }
  std::printf("ALIAS_TOTAL rounds=%d bad_rounds=%d ptr_alias=%d\n", rounds, bad_rounds, bad_alias);
  return bad_rounds>0?1:0;
}

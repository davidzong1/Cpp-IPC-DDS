/* 拆分：只建订阅者（无发布端）vs 只建发布端，判定 300 条额外线程归谁 */
#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "dzIPC/common/topic_data.h"
#include "dzIPC/socket_pub_sub_ipc.h"
#include "dzIPC/threepools/socket_recv_worker.h"
#include "ipc_msg/std_msgs/std_image.hpp"
static constexpr std::uint32_t kMsgId=37;
static int tc(){ DIR*d=opendir("/proc/self/task"); if(!d) return 0; int n=0;
  while(dirent*e=readdir(d)) if(e->d_name[0]!='.') ++n; closedir(d); return n; }
int main(int argc,char**argv){
  const int n=argc>1?atoi(argv[1]):300; const int mode=argc>2?atoi(argv[2]):0; const int dom=argc>3?atoi(argv[3]):33300;
  auto td=[](){ return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(),kMsgId); };
  std::vector<std::unique_ptr<dzIPC::socket::socket_sub_ipc>> subs;
  std::vector<std::unique_ptr<dzIPC::socket::socket_pub_ipc>> pubs;
  for(int i=0;i<n;i++){ const std::string t="t33t2_"+std::to_string(dom)+"_"+std::to_string(i);
    if(mode!=2){ subs.emplace_back(new dzIPC::socket::socket_sub_ipc(td(),t,(size_t)dom,1024,false)); subs.back()->InitChannel("x"); }
    if(mode!=1){ pubs.emplace_back(new dzIPC::socket::socket_pub_ipc(td(),t,(size_t)dom,false)); pubs.back()->InitChannel("x"); } }
  std::this_thread::sleep_for(std::chrono::seconds(2));
  auto& pool=dzIPC::threepools::SocketRecvWorkerPool::instance();
  std::printf("mode=%d n=%d pool_workers=%zu pool_routes=%zu TOTAL_threads=%d\n", mode,n,pool.worker_count(),pool.route_count(),tc());
  return 0; }

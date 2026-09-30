/* 方案 §10.1 三态空闲的实测：state2(已注册未连接)/state3(已连接有效订阅但停止发布)/state1(无 route)。
 * 用真实产品池 SocketRecvWorkerPool。 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include "dzIPC/common/topic_data.h"
#include "dzIPC/socket_pub_sub_ipc.h"
#include "dzIPC/threepools/socket_recv_worker.h"
#include "ipc_msg/std_msgs/std_image.hpp"
static size_t tc(){ DIR*d=opendir("/proc/self/task"); if(!d) return 0; size_t n=0;
  while(dirent*e=readdir(d)) if(e->d_name[0]!='.') ++n; closedir(d); return n; }
int main(int argc,char**argv){
  const int n=argc>1?atoi(argv[1]):1000; const int dom=argc>2?atoi(argv[2]):777;
  using SC=std::chrono::steady_clock;
  std::vector<std::unique_ptr<dzIPC::socket::socket_sub_ipc>> subs; subs.reserve((size_t)n);
  for(int i=0;i<n;i++){ auto td=std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(),37);
    subs.push_back(std::make_unique<dzIPC::socket::socket_sub_ipc>(td,"st_"+std::to_string(i),(size_t)dom,1024,false));
    subs.back()->InitChannel("st"); }
  auto& pool=dzIPC::threepools::SocketRecvWorkerPool::instance();
  std::printf("[state2/3 建立] n=%d threads=%zu route_count=%zu idle_exits=%llu\n", n, tc(), pool.route_count(),
              (unsigned long long)pool.stats().idle_exits);
  std::this_thread::sleep_for(std::chrono::seconds(3));
  std::printf("[state3 已连接有效订阅+静默 3s] threads=%zu route_count=%zu idle_exits=%llu  <- 应保持接收能力(不回落到<=8)\n",
              tc(), pool.route_count(), (unsigned long long)pool.stats().idle_exits);
  subs.clear();
  std::this_thread::sleep_for(std::chrono::seconds(3));
  std::printf("[state1 无 route 静默 3s] threads=%zu route_count=%zu idle_exits=%llu  <- 空闲退出应生效\n",
              tc(), pool.route_count(), (unsigned long long)pool.stats().idle_exits);
  std::printf("DONE\n"); std::fflush(stdout); _exit(0);
}

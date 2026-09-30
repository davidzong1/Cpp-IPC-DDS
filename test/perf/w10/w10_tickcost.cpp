/* 控制面 tick 的 CPU 成本归因：CPU_share ≈ wake_rate × mean_tick_time。
 * 同时给出 tick 自身耗时（callback 为空），从而把「调度器扫描成本」与「回调成本」分开。 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <sstream>
#include <string>
#include <thread>
#include <vector>
#include "dzIPC/threepools/shm_control_scheduler.h"
struct IdleSub : dzIPC::shm_control::SubControlState {
  void on_sub_heartbeat(dzIPC::shm_control::ControlClock::time_point) override {}
  const char* debug_name() const noexcept override { return "i"; } };
struct IdlePub : dzIPC::shm_control::PubControlState {
  void on_pub_heartbeat(dzIPC::shm_control::ControlClock::time_point) override {}
  bool has_peers() const override { return false; }
  void on_pub_stale_scan(dzIPC::shm_control::ControlClock::time_point, std::chrono::nanoseconds) override {}
  const char* debug_name() const noexcept override { return "i"; } };
static double cpu(){ std::ifstream f("/proc/self/stat"); std::string b; std::getline(f,b);
  auto q=b.rfind(')'); if(q==std::string::npos) return -1; std::istringstream is(b.substr(q+2)); std::string t;
  int i=2; double u=0,s=0; while(is>>t){ if(i==13)u=strtoull(t.c_str(),0,10); else if(i==14)s=strtoull(t.c_str(),0,10); ++i;} return u+s; }
int main(int argc,char**argv){
  const int n=argc>1?atoi(argv[1]):1000; const double win=argc>2?atof(argv[2]):20;
  auto& sch=dzIPC::shm_control::ShmControlScheduler::instance();
  std::vector<std::shared_ptr<IdleSub>> subs; std::vector<std::shared_ptr<IdlePub>> pubs;
  for(int i=0;i<n;i++){ auto s=std::make_shared<IdleSub>(); subs.push_back(s); sch.register_subscriber(s);
    auto p=std::make_shared<IdlePub>(); pubs.push_back(p); sch.register_publisher(p); }
  std::this_thread::sleep_for(std::chrono::seconds(1));
  double c0=cpu(); auto a=sch.stats();
  std::this_thread::sleep_for(std::chrono::duration<double>(win));
  double c1=cpu(); auto b=sch.stats();
  const double wakes=(double)(b.tick_count-a.tick_count)/win;
  const double cores=(c1-c0)/100.0/win;
  const double mean_tick_us = wakes>0 ? cores/wakes*1e6 : 0;
  std::printf("entries=%-5zu wakes=%.1f/s cpu=%.5f core  ⇒ mean_tick=%.1f us  tick_max=%.1f us  overruns=%llu\n",
    sch.entry_count(),wakes,cores,mean_tick_us,(double)b.tick_duration_max_ns/1000.0,
    (unsigned long long)b.tick_overrun_count);
  std::fflush(stdout); std::_Exit(0); }

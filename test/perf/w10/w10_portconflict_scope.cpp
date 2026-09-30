/* 判定：socket 千路下"话题端口被自己进程的**临时源端口**占用"这一机制。
 *
 * 机制链：SendOnly 的 ACK 通道**不 bind**（udp.h:169 注释）⇒ 内核分配临时源端口；
 * 而 `ip_local_port_range = 32768..60999` 与 dzIPC 的端口窗口 [11451,65531] 重叠
 * ⇒ 一个话题的**接收端口**可能恰好等于另一个话题**发送套接字的临时源端口**。
 * 那种情况下 `bind(组地址:PORT)` 会 EADDRINUSE（实测见 bind_context），
 * InitChannel 于是进入无上界重试。
 *
 * 本探针：打印 1000 路 socket 话题端口与 ip_local_port_range 的重叠统计，
 * 并给出"临时端口区间内有多少话题端口"这一可判定量。 */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "dzIPC/common/hash.h"
static long arg_long(int argc,char**argv,const char* k,long d){ for(int i=1;i+1<argc;++i) if(!std::strcmp(argv[i],k)) return strtol(argv[i+1],0,10); return d; }
int main(int argc,char**argv){
  const long dom=arg_long(argc,argv,"--domain",3104); const long n=arg_long(argc,argv,"--n",1000);
  // 读 ip_local_port_range
  long lo=32768,hi=60999;
  { FILE*f=fopen("/proc/sys/net/ipv4/ip_local_port_range","r"); if(f){ if(fscanf(f,"%ld %ld",&lo,&hi)!=2){lo=32768;hi=60999;} fclose(f);} }
  // 读已占端口
  long occupied=0; { FILE*f=fopen("/proc/net/udp","r"); char line[512]; if(f){ fgets(line,sizeof line,f);
    while(fgets(line,sizeof line,f)) ++occupied; fclose(f);} }
  long in_range=0; long total=0;
  for(long i=0;i<n;i++){
    char name[128]; snprintf(name,sizeof name,"w10_socket_independent_%ld_%ld",dom,i);
    const unsigned p=dzIPC::common::udp_discovery_port_calculate(name,(int)dom);
    ++total;
    if((long)p>=lo && (long)p<=hi) ++in_range;
  }
  printf("domain=%ld n=%ld ip_local_port_range=[%ld,%ld] occupied_udp_sockets=%ld\n",dom,n,lo,hi,occupied);
  printf("话题端口落在临时端口区间内的数量=%ld/%ld (%.1f%%)\n",in_range,total,100.0*in_range/total);
  printf("结论：%.1f%% 的话题接收端口与内核临时端口池同区间 ⇒ 与任何进程（含本进程 SendOnly 套接字）\n",100.0*in_range/total);
  printf("      的临时源端口存在碰撞面；碰撞后 InitChannel 无上界重试。\n");
  return 0; }

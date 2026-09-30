/* epoll_wait(100ms) 空闲循环标定 — 与 RecvWorker/SocketRecvWorker 的空闲等待同形
 * (epoll_create1 + epoll_wait(timeout) 周期超时, 无事件源)。 */
#define _GNU_SOURCE
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>
static int N=32; static long SLICE_MS=100; static volatile int stop=0;
static void *worker(void *a){ (void)a; int ep=epoll_create1(0); struct epoll_event ev[8];
  while(!stop) epoll_wait(ep, ev, 8, (int)SLICE_MS); close(ep); return NULL; }
static unsigned long long ctxsum(void){ DIR*d=opendir("/proc/self/task"); unsigned long long s=0; if(!d) return 0;
  struct dirent*e; while((e=readdir(d))){ if(e->d_name[0]=='.') continue; char p[256];
  snprintf(p,sizeof p,"/proc/self/task/%s/status",e->d_name); FILE*f=fopen(p,"r"); if(!f) continue; char l[512];
  while(fgets(l,sizeof l,f)){ if(!strncmp(l,"voluntary_ctxt_switches:",24)||!strncmp(l,"nonvoluntary_ctxt_switches:",27))
  s+=strtoull(strchr(l,':')+1,0,10);} fclose(f);} closedir(d); return s; }
int main(int argc,char**argv){ N=argc>1?atoi(argv[1]):32; SLICE_MS=argc>2?atol(argv[2]):100; double w=argc>3?atof(argv[3]):10.0;
  pthread_t th[2048]; for(int i=0;i<N;i++) pthread_create(&th[i],NULL,worker,NULL); sleep(1);
  unsigned long long a=ctxsum(); struct timespec t0,t1,req={(time_t)w,(long)((w-(long)w)*1e9)};
  clock_gettime(CLOCK_MONOTONIC,&t0); nanosleep(&req,NULL); clock_gettime(CLOCK_MONOTONIC,&t1); unsigned long long b=ctxsum();
  double wall=(t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)/1e9;
  printf("EPOLL threads=%d slice=%ldms wall=%.2fs ctx_total=%.1f/s per_thread=%.2f/s\n",N,SLICE_MS,wall,(b-a)/wall,((b-a)/wall)/N);
  stop=1; for(int i=0;i<N;i++) pthread_join(th[i],NULL); return 0; }

/* 门槛 3 可达性标定：N 个 epoll_wait 空闲线程（与 RecvWorker 同形的阻塞超时等待）+
 * 1 个 10ms 周期 tick 线程（模拟 ShmControlScheduler）。
 * 用 /proc/self/stat(utime+stime, 整进程线程组) 记 CPU；逐 TID 逐 TID status 记录 ctx。 */
#define _GNU_SOURCE
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <time.h>
#include <unistd.h>
static int SLICE_MS=100; static volatile int stop=0;
static void *idle_epoll(void *a){ (void)a; int ep=epoll_create1(0); struct epoll_event ev[8];
  while(!stop) epoll_wait(ep, ev, 8, SLICE_MS); close(ep); return NULL; }
static void *tick(void *a){ long ms=(long)a; while(!stop){ struct timespec ts={ms/1000,(ms%1000)*1000000L}; nanosleep(&ts,NULL);} return NULL; }
static double proc_cpu_ticks(void){ FILE*f=fopen("/proc/self/stat","r"); if(!f) return -1; char b[8192];
 if(!fgets(b,sizeof b,f)){fclose(f);return -1;} fclose(f); char*q=strrchr(b,')'); if(!q) return -1; q++;
 int i=2; char*t=strtok(q," "); double u=0,st=0;
 while(t){ if(i==13) u=strtoull(t,0,10); else if(i==14) st=strtoull(t,0,10); t=strtok(NULL," "); i++; } return u+st; }
static unsigned long long ctxsum(void){ DIR*d=opendir("/proc/self/task"); unsigned long long s=0; if(!d) return 0;
 struct dirent*e; while((e=readdir(d))){ if(e->d_name[0]=='.') continue; char p[256];
 snprintf(p,sizeof p,"/proc/self/task/%s/status",e->d_name); FILE*f=fopen(p,"r"); if(!f) continue; char l[512];
 while(fgets(l,sizeof l,f)) if(!strncmp(l,"voluntary_ctxt_switches:",24)||!strncmp(l,"nonvoluntary_ctxt_switches:",27))
   s+=strtoull(strchr(l,':')+1,0,10); fclose(f);} closedir(d); return s; }
int main(int argc,char**argv){ int N=argc>1?atoi(argv[1]):32; SLICE_MS=argc>2?atoi(argv[2]):100;
 int nticks=argc>3?atoi(argv[3]):1; long tickms=argc>4?atol(argv[4]):10; double w=argc>5?atof(argv[5]):10.0;
 long hz=sysconf(_SC_CLK_TCK); pthread_t th[4096]; pthread_t tk[64];
 for(int i=0;i<N;i++) pthread_create(&th[i],NULL,idle_epoll,NULL);
 for(int i=0;i<nticks;i++) pthread_create(&tk[i],NULL,tick,(void*)tickms);
 sleep(1); double a=proc_cpu_ticks(); unsigned long long c0=ctxsum();
 struct timespec t0,t1,req={(time_t)w,(long)((w-(long)w)*1e9)};
 clock_gettime(CLOCK_MONOTONIC,&t0); nanosleep(&req,NULL); clock_gettime(CLOCK_MONOTONIC,&t1);
 double b=proc_cpu_ticks(); unsigned long long c1=ctxsum();
 double wall=(t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)/1e9;
 printf("N_worker=%-3d slice=%-5dms ticks=%d@%ldms : cpu=%.5f core  ctx=%.1f/s  (per worker %.2f/s)\n",
   N,SLICE_MS,nticks,tickms,(b-a)/hz/wall,(c1-c0)/wall,((c1-c0)/wall)/(N?N:1));
 stop=1; for(int i=0;i<N;i++) pthread_join(th[i],NULL); for(int i=0;i<nticks;i++) pthread_join(tk[i],NULL); return 0; }

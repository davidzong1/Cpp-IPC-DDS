/* 控制面 tick 的 ctx 下限：与 ShmControlScheduler::Impl::loop 同形（mutex + cv.wait_until）。 */
#define _GNU_SOURCE
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static volatile int stop=0; static long PERIOD_MS=10;
static pthread_mutex_t m=PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t cv;
static void initcv(void){ pthread_condattr_t a; pthread_condattr_init(&a); pthread_condattr_setclock(&a,CLOCK_MONOTONIC); pthread_cond_init(&cv,&a);}
static void *tick(void *a){ (void)a; struct timespec now; while(!stop){
    clock_gettime(CLOCK_MONOTONIC,&now); now.tv_nsec += PERIOD_MS*1000000L;
    if(now.tv_nsec>=1000000000L){ now.tv_sec++; now.tv_nsec-=1000000000L; }
    pthread_mutex_lock(&m); pthread_cond_timedwait(&cv,&m,&now); pthread_mutex_unlock(&m); } return NULL; }
static unsigned long long ctxsum(void){ DIR*d=opendir("/proc/self/task"); unsigned long long s=0; if(!d) return 0;
 struct dirent*e; while((e=readdir(d))){ if(e->d_name[0]=='.') continue; char p[256];
 snprintf(p,sizeof p,"/proc/self/task/%s/status",e->d_name); FILE*f=fopen(p,"r"); if(!f) continue; char l[512];
 while(fgets(l,sizeof l,f)) if(!strncmp(l,"voluntary_ctxt_switches:",24)||!strncmp(l,"nonvoluntary_ctxt_switches:",27))
   s+=strtoull(strchr(l,':')+1,0,10); fclose(f);} closedir(d); return s; }
int main(int argc,char**argv){ int n=argc>1?atoi(argv[1]):1; PERIOD_MS=argc>2?atol(argv[2]):10;
 double w=argc>3?atof(argv[3]):10.0; pthread_t tk[64]; initcv();
 for(int i=0;i<n;i++) pthread_create(&tk[i],NULL,tick,NULL); sleep(1);
 unsigned long long c0=ctxsum(); struct timespec t0,t1,req={(time_t)w,(long)((w-(long)w)*1e9)};
 clock_gettime(CLOCK_MONOTONIC,&t0); nanosleep(&req,NULL); clock_gettime(CLOCK_MONOTONIC,&t1); unsigned long long c1=ctxsum();
 double wall=(t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)/1e9;
 printf("cv_tick threads=%d period=%ldms : ctx=%.1f/s  per_thread=%.2f/s\n",n,PERIOD_MS,(c1-c0)/wall,((c1-c0)/wall)/n);
 stop=1; for(int i=0;i<n;i++){ pthread_mutex_lock(&m); pthread_cond_broadcast(&cv); pthread_mutex_unlock(&m);} for(int i=0;i<n;i++) pthread_join(tk[i],NULL); return 0; }

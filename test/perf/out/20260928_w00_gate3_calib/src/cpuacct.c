/* 判断 /proc/self/stat 的 utime+stime 是否覆盖全部线程: N 个忙线程跑 3s。 */
#define _GNU_SOURCE
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
static volatile int stop=0; static volatile unsigned long long sink=0;
static void *busy(void *a){ (void)a; while(!stop) sink++; return NULL; }
static double stat_cpu(const char* path){ FILE*f=fopen(path,"r"); if(!f) return -1; char b[8192];
 if(!fgets(b,sizeof b,f)){fclose(f);return -1;} fclose(f); char*q=strrchr(b,')'); if(!q) return -1; q++;
 int i=2; char*t=strtok(q," "); double u=0,st=0;
 while(t){ if(i==13) u=strtoull(t,0,10); else if(i==14) st=strtoull(t,0,10); t=strtok(NULL," "); i++; } return u+st; }
static double sum_task_cpu(void){ DIR*d=opendir("/proc/self/task"); if(!d) return -1; double s=0; struct dirent*e;
 while((e=readdir(d))){ if(e->d_name[0]=='.') continue; char p[256]; snprintf(p,sizeof p,"/proc/self/task/%s/stat",e->d_name);
   double v=stat_cpu(p); if(v>0) s+=v; } closedir(d); return s; }
int main(int argc,char**argv){ int N=argc>1?atoi(argv[1]):32; long hz=sysconf(_SC_CLK_TCK); pthread_t th[4096];
 printf("hz=%ld\n",hz);
 for(int i=0;i<N;i++) pthread_create(&th[i],NULL,busy,NULL); sleep(1);
 double a1=stat_cpu("/proc/self/stat"), a2=sum_task_cpu();
 struct timespec t0,t1,req={3,0}; clock_gettime(CLOCK_MONOTONIC,&t0); nanosleep(&req,NULL); clock_gettime(CLOCK_MONOTONIC,&t1);
 double b1=stat_cpu("/proc/self/stat"), b2=sum_task_cpu();
 double wall=(t1.tv_sec-t0.tv_sec)+(t1.tv_nsec-t0.tv_nsec)/1e9;
 printf("N=%d wall=%.2fs  /proc/self/stat=%.5f cores    sum(task/*/stat)=%.5f cores   ratio=%.2f\n",
   N, wall, (b1-a1)/hz/wall, (b2-a2)/hz/wall, (b2-a2)/((b1-a1)>0?(b1-a1):1));
 stop=1; for(int i=0;i<N;i++) pthread_join(th[i],NULL); return 0; }

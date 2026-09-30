/*
 * csw_cpu_scope_probe.c —— 实测三件事的口径差异 (W01 纠偏要点 4)
 *
 * 背景: test/perf/out/20260927_t6_probes/t6scale.cpp 的 proc_stat() 同时读
 *   /proc/self/status 的 voluntary_ctxt_switches
 *   /proc/self/stat   的 utime+stime
 * 本文档原稿把前者当作"进程空闲上下文切换", 把后者当作"空闲 CPU(核)"。
 * 本探针同时给出"调用线程自身值"与"全线程组累计值", 用实测判定两者的可比范围。
 *
 * 负载: 8 条 usleep(1ms) 工作线程(制造大量自愿切换) + 1 条忙转线程(制造 CPU),
 *       全部线程在整个 2s 采样窗口内保活, 避免线程退出导致累计口径失真。
 *
 * 构建/运行:
 *   gcc -O2 -o /tmp/csw_cpu_scope_probe csw_cpu_scope_probe.c -lpthread && /tmp/csw_cpu_scope_probe
 *
 * 结论(见 .output.txt):
 *   A) self/status voluntary  <<  B) 全线程组 voluntary   ⇒ A 不是全线程组累计值
 *   C) self/stat utime+stime  ≈  D) 全线程组 utime+stime  ⇒ C 已是线程组累计值
 * 即: 该探针的 ctx_vol/ctx_nonvol 只是主线程自身; CPU 一列则是进程级累计。
 */
#include <dirent.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static volatile int g_stop = 0;

static void* sleeper(void* a) { (void)a; while (!g_stop) usleep(1000); return 0; }
static void* spinner(void* a) { (void)a; volatile double x = 0; while (!g_stop) { for (int j = 0; j < 20000; ++j) x += j * 0.5; } return 0; }

static unsigned long long read_field(const char* path, const char* key) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char line[512];
    unsigned long long v = 0;
    const size_t klen = strlen(key);
    while (fgets(line, sizeof line, f)) {
        if (!strncmp(line, key, klen)) { v = strtoull(line + klen, 0, 10); break; }
    }
    fclose(f);
    return v;
}

static unsigned long long stat_cpu_of(const char* path) {
    FILE* f = fopen(path, "r");
    if (!f) return 0;
    char buf[4096];
    if (!fgets(buf, sizeof buf, f)) { fclose(f); return 0; }
    fclose(f);
    char* c = strrchr(buf, ')');
    if (!c) return 0;
    char* save = NULL;
    int idx = 3;
    unsigned long long u = 0, s = 0;
    for (char* t = strtok_r(c + 2, " ", &save); t; t = strtok_r(NULL, " ", &save), ++idx) {
        if (idx == 14) u = strtoull(t, NULL, 10);
        else if (idx == 15) s = strtoull(t, NULL, 10);
    }
    return u + s;
}

/* 全线程组累计: 遍历本进程每个 TID 的 status/stat */
static void sum_group(unsigned long long* vol, unsigned long long* cpu) {
    DIR* d = opendir("/proc/self/task");
    *vol = 0; *cpu = 0;
    if (!d) return;
    struct dirent* e;
    char p[320];
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        snprintf(p, sizeof p, "/proc/self/task/%s/status", e->d_name);
        *vol += read_field(p, "voluntary_ctxt_switches:");
        snprintf(p, sizeof p, "/proc/self/task/%s/stat", e->d_name);
        *cpu += stat_cpu_of(p);
    }
    closedir(d);
}

int main(void) {
    pthread_t th[9];
    for (int i = 0; i < 8; ++i) pthread_create(&th[i], 0, sleeper, 0);
    pthread_create(&th[8], 0, spinner, 0);
    sleep(1);   /* 让线程稳定 */

    unsigned long long a0 = read_field("/proc/self/status", "voluntary_ctxt_switches:");
    unsigned long long c0 = stat_cpu_of("/proc/self/stat");
    unsigned long long b0, d0; sum_group(&b0, &d0);

    sleep(2);   /* 采样窗口 2s, 线程全部保活 */

    unsigned long long a1 = read_field("/proc/self/status", "voluntary_ctxt_switches:");
    unsigned long long c1 = stat_cpu_of("/proc/self/stat");
    unsigned long long b1, d1; sum_group(&b1, &d1);

    g_stop = 1;
    for (int i = 0; i < 9; ++i) pthread_join(th[i], 0);

    const long hz = sysconf(_SC_CLK_TCK);
    printf("window=2.0s  payload: 8x usleep(1ms) keep-alive threads + 1x busy spin thread\n");
    printf("CLK_TCK=%ld\n", hz);
    printf("A  /proc/self/status voluntary delta       = %llu\n", a1 - a0);
    printf("B  task-group       voluntary delta       = %llu\n", b1 - b0);
    printf("C  /proc/self/stat   utime+stime delta     = %llu ticks (%.4f core over 2s)\n",
           c1 - c0, hz > 0 ? (double)(c1 - c0) / (double)hz / 2.0 : 0.0);
    printf("D  task-group        utime+stime delta     = %llu ticks (%.4f core over 2s)\n",
           d1 - d0, hz > 0 ? (double)(d1 - d0) / (double)hz / 2.0 : 0.0);
    printf("judgement: A%sB  => self/status ctxt switch %s thread-group aggregate\n",
           (a1 - a0) * 5 < (b1 - b0) ? " << " : " ~= ", (a1 - a0) * 5 < (b1 - b0) ? "is NOT" : "is");
    printf("judgement: C~=D (C/D = %.3f) => self/stat CPU %s thread-group aggregate\n",
           (d1 - d0) ? (double)(c1 - c0) / (double)(d1 - d0) : -1.0,
           (d1 - d0) && (double)(c1 - c0) / (double)(d1 - d0) > 0.8 ? "IS" : "is NOT");
    return 0;
}

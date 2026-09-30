/* 门槛 3（空闲切换）可达性的关键实测：真实 ShmControlScheduler 注册 N 个订阅项
 * （ControlTiming 默认 sub_heartbeat=10ms），测 60s 静默窗口内的全线程组 ctx 与 CPU。
 * 目的：判定「≤50 次·s⁻¹·进程⁻¹」在冻结 ControlTiming 下是否可达。 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include "dzIPC/threepools/shm_control_scheduler.h"

struct IdleSub : dzIPC::shm_control::SubControlState {
    int id{0};
    void on_sub_heartbeat(dzIPC::shm_control::ControlClock::time_point) override { ++ticks; }
    const char* debug_name() const noexcept override { return "idle-sub"; }
    unsigned ticks{0};
};

static double stat_cpu(const char* path) {
    FILE* f = fopen(path, "r"); if (!f) return -1; char b[8192];
    if (!fgets(b, sizeof b, f)) { fclose(f); return -1; } fclose(f);
    char* q = strrchr(b, ')'); if (!q) return -1; q++;
    int i = 2; char* t = strtok(q, " "); double u = 0, st = 0;
    while (t) { if (i == 13) u = strtoull(t, 0, 10); else if (i == 14) st = strtoull(t, 0, 10); t = strtok(NULL, " "); i++; }
    return u + st;
}
static unsigned long long ctxsum() {
    DIR* d = opendir("/proc/self/task"); unsigned long long s = 0; if (!d) return 0; struct dirent* e;
    while ((e = readdir(d))) { if (e->d_name[0] == '.') continue; char p[256];
        snprintf(p, sizeof p, "/proc/self/task/%s/status", e->d_name); FILE* f = fopen(p, "r"); if (!f) continue; char l[512];
        while (fgets(l, sizeof l, f)) if (!strncmp(l, "voluntary_ctxt_switches:", 24) || !strncmp(l, "nonvoluntary_ctxt_switches:", 27))
            s += strtoull(strchr(l, ':') + 1, 0, 10);
        fclose(f); }
    closedir(d); return s;
}
static size_t tc() { DIR* d = opendir("/proc/self/task"); if (!d) return 0; size_t n = 0;
    while (dirent* e = readdir(d)) if (e->d_name[0] != '.') ++n; closedir(d); return n; }

int main(int argc, char** argv) {
    const int n = argc > 1 ? atoi(argv[1]) : 1000;
    const double win = argc > 2 ? atof(argv[2]) : 60.0;
    const long hb = argc > 3 ? atol(argv[3]) : 10;   /* sub_heartbeat ms */
    auto& sched = dzIPC::shm_control::ShmControlScheduler::instance();
    dzIPC::shm_control::ControlTiming tm;
    tm.sub_heartbeat = std::chrono::milliseconds{hb};
    std::vector<std::shared_ptr<IdleSub>> states;
    states.reserve((size_t)n);
    std::vector<dzIPC::shm_control::ShmControlScheduler::EntryId> ids;
    for (int i = 0; i < n; ++i) {
        auto s = std::make_shared<IdleSub>(); s->id = i; states.push_back(s);
        ids.push_back(sched.register_subscriber(s, tm));
    }
    size_t bad = 0; for (auto id : ids) if (id == dzIPC::shm_control::ShmControlScheduler::kInvalidEntry) ++bad;
    std::printf("n=%d sub_heartbeat=%ldms entry_count=%zu invalid=%zu threads=%zu\n",
                n, hb, sched.entry_count(), bad, tc());
    std::this_thread::sleep_for(std::chrono::seconds(2));
    long hz = sysconf(_SC_CLK_TCK);
    double a = stat_cpu("/proc/self/stat"); unsigned long long c0 = ctxsum();
    auto w0 = std::chrono::steady_clock::now();
    std::this_thread::sleep_for(std::chrono::duration<double>(win));
    auto w1 = std::chrono::steady_clock::now();
    double b = stat_cpu("/proc/self/stat"); unsigned long long c1 = ctxsum();
    double wall = std::chrono::duration<double>(w1 - w0).count();
    auto st = sched.stats();
    std::printf("window_s=%.3f cpu_cores=%.5f ctx_total_per_s=%.1f (budget 50)\n",
                wall, (b - a) / hz / wall, (c1 - c0) / wall);
    std::printf("tick_count=%llu tick_overrun=%llu entry_count=%zu ticks_sample=%u tick_wakes_per_s=%.1f tick_last_us=%.2f tick_max_us=%.2f\n",
                (unsigned long long)st.tick_count, (unsigned long long)st.tick_overrun_count,
                sched.entry_count(), states.empty() ? 0u : states[0]->ticks,
                (double)st.tick_count / wall, st.tick_duration_last_ns / 1000.0,
                st.tick_duration_max_ns / 1000.0);
    std::printf("DONE\n"); std::fflush(stdout);
    std::_Exit(0);   /* 不跑静态析构：避免故意泄漏单例的退出顺序噪声 */
}

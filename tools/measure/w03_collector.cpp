// W03 采集器 —— 测量口径与路径观测的证据入口。
//
// 交付依据: docs/消息接收架构改造/团队改造方案_性能证据闭环与SHM规模化.md
//   §4 W03（时钟/CPU/上下文切换/空闲/计数开销）、§10.1（三种空闲）、
//   §10.2（每轮扫描诊断）、§12（证据目录与 manifest.json 字段）、
//   §6.2（相位与窗口）、§14.2（运行后清单）。
//
// 用法（见 --help）：
//   w03_collector schema        [--out FILE]
//   w03_collector overhead      [--iterations N] [--out FILE]
//   w03_collector clock-check   [--out FILE]
//   w03_collector evidence      --out-dir DIR --run-id ID ... [--job=label,role,cmd,arg...]
//   w03_collector selftest
//
// 上下文切换口径：**rusage 优先**。本采集器 fork+exec 拉起被测进程，结束时
// wait4() 取 ru_nvcsw/ru_nivcsw —— 覆盖线程组整个生命周期，是"完整"口径；
// 同时按 /proc 逐 TID 采样给出相位序列，并在输出里明确标注两种口径。
// 被观测进程不是本采集器子进程时只有逐 TID 采样，完整性由
// ThreadGroupCtxSwitch::complete + *_undercount_bound 表达。
// ⛔ 任何输出都不把 /proc/<pid>/status（主线程）当作全进程合计。
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>
#include <thread>
#include <unistd.h>
#include <vector>

#include "dzIPC/measure/measurement.h"

using namespace dzIPC::measure;

namespace {

constexpr const char* kUsage =
"W03 采集器 (测量口径与路径观测)\n"
"用法:\n"
"  w03_collector schema       [--out FILE]\n"
"  w03_collector overhead     [--iterations N] [--out FILE]\n"
"  w03_collector clock-check  [--out FILE]\n"
"  w03_collector evidence     --out-dir DIR [options]\n"
"                             [--job=label,role,cmd,arg...] [--watch=pid,role,label]\n"
"                             [--window-s S] [--sample-ms MS] [--required-window-s S]\n"
"                             [--diagnostics on|off] [--timeout-s S]\n"
"                             [--startup-s S --steady-s S --idle-s S]\n"
"                             [--periodic-ticks N --periodic-source STR]\n"
"                             [--counters-json=path,label] [--path-spec=FILE]\n"
"                             [--recovery-spec=FILE]\n"
"                             [--run-id ID] [--transport T] [--worker-count N]\n"
"                             [--expect-threads N] [--source-revision REV] [--build-dir DIR]\n"
"  w03_collector selftest\n"
"  w03_collector scan-end-values   # R0-9/R0-10 结束值接口机器可读自测\n"
"说明:\n"
"  · evidence 模式: 拉起 --job 子进程并全程采样；结束时用 wait4 取整个线程组\n"
"    生命周期的 rusage（CPU + 上下文切换），同时输出逐相位序列。\n"
"  · 相位来自子进程 stdout 的 `W03_PHASE <name> <monotonic_ns>` 标记，或\n"
"    --startup-s/--steady-s/--idle-s 时间切分；两者都没有则标 未确认。\n";

struct Args {
    std::string mode;
    std::string out;
    std::string out_dir;
    std::string run_id;
    std::string transport{"unknown"};
    std::string source_revision;
    std::string build_dir;
    std::vector<std::string> jobs;
    std::vector<std::string> watches;
    std::vector<std::string> counters_json;
    std::string path_spec;
    std::string recovery_spec;
    std::uint64_t iterations{200000};
    double window_s{60.0};
    double required_window_s{60.0};
    double sample_ms{200.0};
    double timeout_s{0.0};
    double startup_s{-1.0};
    double steady_s{-1.0};
    double idle_s{-1.0};
    bool phase_from_stdout{false};
    bool diagnostics{false};
    std::uint64_t periodic_ticks{0};
    std::string periodic_source;
    int worker_count{0};
    long expect_threads{-1};
};

bool starts_with(const std::string& s, const std::string& p)
{
    return s.size() >= p.size() && s.compare(0, p.size(), p) == 0;
}

std::vector<std::string> split(const std::string& s, char sep)
{
    std::vector<std::string> v;
    std::string cur;
    for (char c : s) {
        if (c == sep) { v.push_back(cur); cur.clear(); }
        else cur += c;
    }
    v.push_back(cur);
    return v;
}

Args parse_args(int argc, char** argv)
{
    Args a;
    if (argc >= 2 && argv[1][0] != '-') a.mode = argv[1];
    for (int i = (a.mode.empty() ? 1 : 2); i < argc; ++i) {
        const std::string s = argv[i];
        std::string v;
        auto val = [&](const char* key) -> bool {
            if (!starts_with(s, std::string(key) + "=")) return false;
            v = s.substr(std::strlen(key) + 1);
            return true;
        };
        auto next = [&]() -> std::string { return (i + 1 < argc) ? std::string(argv[++i]) : std::string(); };
        if (s == "--out") a.out = next();
        else if (val("--out")) a.out = v;
        else if (val("--out-dir")) a.out_dir = v;
        else if (val("--run-id")) a.run_id = v;
        else if (val("--transport")) a.transport = v;
        else if (val("--source-revision")) a.source_revision = v;
        else if (val("--build-dir")) a.build_dir = v;
        else if (val("--job")) a.jobs.push_back(v);
        else if (val("--watch")) a.watches.push_back(v);
        else if (val("--counters-json")) a.counters_json.push_back(v);
        else if (val("--path-spec")) a.path_spec = v;
        else if (val("--recovery-spec")) a.recovery_spec = v;
        else if (val("--iterations")) a.iterations = std::strtoull(v.c_str(), nullptr, 10);
        else if (val("--window-s")) a.window_s = std::atof(v.c_str());
        else if (val("--required-window-s")) a.required_window_s = std::atof(v.c_str());
        else if (val("--sample-ms")) a.sample_ms = std::atof(v.c_str());
        else if (val("--timeout-s")) a.timeout_s = std::atof(v.c_str());
        else if (val("--startup-s")) a.startup_s = std::atof(v.c_str());
        else if (val("--steady-s")) a.steady_s = std::atof(v.c_str());
        else if (val("--idle-s")) a.idle_s = std::atof(v.c_str());
        else if (val("--periodic-ticks")) a.periodic_ticks = std::strtoull(v.c_str(), nullptr, 10);
        else if (val("--periodic-source")) a.periodic_source = v;
        else if (val("--worker-count")) a.worker_count = std::atoi(v.c_str());
        else if (val("--expect-threads")) a.expect_threads = std::strtol(v.c_str(), nullptr, 10);
        else if (s == "--phase-from-stdout") a.phase_from_stdout = true;
        else if (val("--diagnostics")) a.diagnostics = (v == "on" || v == "true" || v == "1");
        else if (s == "--help" || s == "-h") { std::fputs(kUsage, stdout); std::exit(0); }
    }
    return a;
}

std::string json_escape_str(const std::string& s)
{
    std::string o;
    for (char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': o += "\\r"; break;
        case '\t': o += "\\t"; break;
        default:
            if (static_cast<unsigned char>(c) < 0x20) {
                char tmp[8];
                std::snprintf(tmp, sizeof(tmp), "\\u%04x", static_cast<unsigned>(c));
                o += tmp;
            } else o += c;
        }
    }
    return o;
}

std::string json_array_of_lines(const std::string& text)
{
    std::string o = "[";
    std::istringstream is(text);
    std::string line;
    bool first = true;
    while (std::getline(is, line)) {
        if (line.empty()) continue;
        if (!first) o += ", ";
        first = false;
        o += "\"" + json_escape_str(line) + "\"";
    }
    o += "]";
    return o;
}

std::string now_utc_string()
{
    char buf[64] = {0};
    const std::time_t t = std::time(nullptr);
    std::tm tmv {};
    ::gmtime_r(&t, &tmv);
    std::strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%SZ", &tmv);
    return buf;
}

bool write_file(const std::string& path, const std::string& content)
{
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f << content;
    return static_cast<bool>(f);
}

void make_dir(const std::string& path)
{
    if (path.empty()) return;
    std::string cur;
    for (std::size_t i = 0; i < path.size(); ++i) {
        cur += path[i];
        if (path[i] == '/') ::mkdir(cur.c_str(), 0755);
    }
    if (path.back() != '/') ::mkdir(path.c_str(), 0755);
}

std::string run_command_capture(const std::string& cmd)
{
    std::string out;
    FILE* p = ::popen(cmd.c_str(), "r");
    if (!p) return out;
    char buf[512];
    while (std::fgets(buf, sizeof(buf), p)) out += buf;
    ::pclose(p);
    return out;
}

std::string sha256_of_file(const std::string& path)
{
    if (path.empty()) return std::string();
    const std::string out = run_command_capture("sha256sum '" + path + "' 2>/dev/null");
    if (out.size() < 64) return std::string();
    return out.substr(0, 64);
}

std::string file_stat_line(const std::string& path)
{
    struct stat st {};
    if (::stat(path.c_str(), &st) != 0) return "missing: " + path;
    std::ostringstream o;
    o << path << " size=" << static_cast<long long>(st.st_size)
      << " mtime=" << static_cast<long long>(st.st_mtime);
    return o.str();
}

ProcessRole parse_role(const std::string& s)
{
    if (s == "publisher") return ProcessRole::publisher;
    if (s == "subscriber") return ProcessRole::subscriber;
    if (s == "daemon") return ProcessRole::daemon;
    if (s == "orchestrator") return ProcessRole::orchestrator;
    if (s == "other") return ProcessRole::other;
    return ProcessRole::unknown;
}

/* ------------------------------------------------------------------ 采样 */

struct SamplePoint
{
    std::uint64_t ns{0};
    double        offset_s{0.0};
    int           pid{-1};
    ProcessRole   role{ProcessRole::unknown};
    std::string   label;
    std::size_t   threads{0};
    double        cpu_cores{0.0};
    double        ctx_switch_per_s{0.0};
    std::uint64_t rss_kb{0};
    long          max_fd{-1};
    char          state{'?'};
    /* CPU 权威口径：/proc/<pid>/stat 的 utime+stime（内核 whole=1 = 线程组累计）。
     * 逐 TID 之和只作交叉检查，不单独作为 CPU 结论（见 CpuScopeSample 注释）。 */
    double        proc_stat_cpu_s{0.0};
    double        task_group_sum_cpu_s{0.0};
    std::string   phase;
};

struct Child
{
    std::string              label;
    ProcessRole              role{ProcessRole::unknown};
    std::vector<std::string> argv;
    std::string              stdout_path;
    std::string              stderr_path;
    std::string              cmdline;
    ChildProcessRunner       runner;
    ProcessSampler*          sampler{nullptr};
    int                      pid{-1};
    bool                     exited{false};
    bool                     watched_only{false};
    std::uint64_t            last_cpu_ticks{0};
    std::uint64_t            last_ctx_vol{0};
    std::uint64_t            last_ctx_nonvol{0};
    std::uint64_t            last_ns{0};
    std::uint64_t            cpu_ticks_at_exit{0};
    bool                     cpu_ticks_at_exit_valid{false};
    /* CPU 口径核验（§4 W03 第 2 条 / W01 UF-06）:
     * /proc/<pid>/stat(线程组累计候选) vs 逐 TID utime+stime 之和。
     * 只在"无线程退出"的采样点有效（见 proc_sampler.h CpuScopeSample 注释）。 */
    CpuScopeSample           cpu_scope{};
    ThreadGroupCtxSwitch     ctx{};
    ChildRusage              rusage{};
    std::size_t              threads_max{0};
    long                     max_fd_seen{-1};
};

struct PhaseMarker
{
    std::uint64_t ns{0};
    std::string   name;
};

std::vector<PhaseMarker> parse_phase_markers(const std::string& path)
{
    std::vector<PhaseMarker> v;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        const std::size_t pos = line.find("W03_PHASE");
        if (pos == std::string::npos) continue;
        std::istringstream is(line.substr(pos + 9));
        std::string name;
        unsigned long long ns = 0;
        if (is >> name >> ns) v.push_back(PhaseMarker{static_cast<std::uint64_t>(ns), name});
    }
    std::sort(v.begin(), v.end(), [](const PhaseMarker& x, const PhaseMarker& y) { return x.ns < y.ns; });
    return v;
}

const char* phase_at(const std::vector<PhaseMarker>& markers, std::uint64_t ns)
{
    const char* cur = nullptr;
    for (const PhaseMarker& m : markers) {
        if (m.ns <= ns) cur = m.name.c_str();
        else break;
    }
    return cur;
}

/* ------------------------------------------------------------------ evidence */

int run_evidence(const Args& a)
{
    const std::string dir = a.out_dir;
    make_dir(dir);
    make_dir(dir + "/stdout");
    make_dir(dir + "/stderr");

    PlatformInfo platform = collect_platform_info();
    const ClockCost clock_cost = measure_clock_cost(std::max<std::uint64_t>(1000, a.iterations / 4));
    const CounterOverhead overhead = measure_counter_overhead(std::max<std::uint64_t>(1000, a.iterations));
    const XprocClockCheck xproc = verify_cross_process_clock();

    std::string git_rev = a.source_revision;
    if (git_rev.empty()) {
        git_rev = run_command_capture("git -C . rev-parse HEAD 2>/dev/null");
        while (!git_rev.empty() && (git_rev.back() == '\n' || git_rev.back() == '\r')) git_rev.pop_back();
    }
    const std::string git_status = run_command_capture("git -C . status --short 2>/dev/null");

    std::ostringstream cmd;
    cmd << "w03_collector evidence";
    if (!a.run_id.empty()) cmd << " --run-id=" << a.run_id;
    cmd << " --out-dir=" << dir << " --window-s=" << a.window_s
        << " --required-window-s=" << a.required_window_s
        << " --sample-ms=" << a.sample_ms
        << " --diagnostics=" << (a.diagnostics ? "on" : "off");
    for (const std::string& j : a.jobs) cmd << " --job=" << j;
    for (const std::string& w : a.watches) cmd << " --watch=" << w;
    for (const std::string& c : a.counters_json) cmd << " --counters-json=" << c;
    if (!a.path_spec.empty()) cmd << " --path-spec=" << a.path_spec;
    if (!a.recovery_spec.empty()) cmd << " --recovery-spec=" << a.recovery_spec;
    if (a.worker_count) cmd << " --worker-count=" << a.worker_count;
    if (a.expect_threads >= 0) cmd << " --expect-threads=" << a.expect_threads;
    cmd << "\n";
    write_file(dir + "/command.txt", cmd.str());
    const std::string config_hash = sha256_of_file(dir + "/command.txt");

    CounterRegistry::instance().set_diagnostics_enabled(a.diagnostics);

    std::vector<std::string> unconfirmed;

    /* ---- 组装 job/watch ---- */
    std::vector<Child> children;
    for (const std::string& job : a.jobs) {
        const std::vector<std::string> parts = split(job, ',');
        if (parts.size() < 3) {
            std::fprintf(stderr, "[w03] --job 需要 label,role,cmd[,args...]: %s\n", job.c_str());
            return 2;
        }
        Child c;
        c.label = parts[0];
        c.role = parse_role(parts[1]);
        c.stdout_path = dir + "/stdout/" + c.label + ".log";
        c.stderr_path = dir + "/stderr/" + c.label + ".log";
        for (std::size_t k = 2; k < parts.size(); ++k) c.argv.push_back(parts[k]);
        children.push_back(std::move(c));
    }
    for (const std::string& w : a.watches) {
        const std::vector<std::string> parts = split(w, ',');
        if (parts.size() < 3) {
            std::fprintf(stderr, "[w03] --watch 需要 pid,role,label: %s\n", w.c_str());
            return 2;
        }
        Child c;
        c.watched_only = true;
        c.label = parts[2];
        c.role = parse_role(parts[1]);
        c.pid = std::atoi(parts[0].c_str());
        children.push_back(std::move(c));
    }
    if (children.empty()) {
        std::fprintf(stderr, "[w03] evidence 至少需要一个 --job 或 --watch\n");
        return 2;
    }

    for (Child& c : children) {
        if (!c.watched_only) {
            if (!c.runner.spawn(c.argv, c.label, c.stdout_path, c.stderr_path)) {
                std::fprintf(stderr, "[w03] 无法拉起 %s\n", c.label.c_str());
                return 3;
            }
            c.pid = c.runner.pid();
            /* cmdline 必须在进程还在时抓取（退出后 /proc/<pid>/cmdline 为空） */
            c.cmdline = process_cmdline(c.pid);
        }
        c.sampler = new ProcessSampler(c.pid);
        if (!c.sampler->attach()) {
            std::fprintf(stderr, "[w03] 进程 %d (%s) 不存在, 采样跳过\n", c.pid, c.label.c_str());
            c.exited = true;
            unconfirmed.push_back("进程 " + c.label + " 在采样开始前不存在");
            continue;
        }
        const ProcessStat& st = c.sampler->stat();
        /* 与 wait4 rusage 同口径: 只算本进程 utime+stime (不含已回收子进程)。
         * 采样口径是下界, 完整值以 rusage 为准。 */
        c.last_cpu_ticks = st.utime_ticks + st.stime_ticks;
        c.last_ns = monotonic_now_ns();
    }

    const std::uint64_t t_begin = monotonic_now_ns();
    const std::uint64_t window_ns = static_cast<std::uint64_t>(a.window_s * 1e9);
    const std::uint64_t sample_ns = static_cast<std::uint64_t>(std::max(1.0, a.sample_ms) * 1e6);
    const double timeout_s = a.timeout_s > 0.0 ? a.timeout_s : std::max(a.window_s * 2.0, 120.0);
    const std::uint64_t timeout_ns = static_cast<std::uint64_t>(timeout_s * 1e9);

    std::vector<SamplePoint> samples;
    bool timed_out = false;

    for (;;) {
        const std::uint64_t now = monotonic_now_ns();
        const std::uint64_t elapsed = now - t_begin;

        for (Child& c : children) {
            if (c.exited || c.sampler == nullptr) continue;
            if (!c.sampler->sample()) { c.exited = true; continue; }
            const ProcessStat st = c.sampler->stat();
            if (st.state == 'Z') { c.exited = true; continue; }
            const std::uint64_t ticks = st.utime_ticks + st.stime_ticks;
            const std::uint64_t dt_ns = now - c.last_ns;
            const double dt_s = dt_ns > 0 ? static_cast<double>(dt_ns) / 1e9 : 0.0;
            const double hz = static_cast<double>(clock_ticks_per_sec());
            SamplePoint sp;
            sp.ns = now;
            sp.offset_s = static_cast<double>(elapsed) / 1e9;
            sp.pid = c.pid;
            sp.role = c.role;
            sp.label = c.label;
            sp.threads = c.sampler->threads().size();
            sp.rss_kb = st.rss_pages * (static_cast<std::uint64_t>(::sysconf(_SC_PAGESIZE)) / 1024u);
            sp.max_fd = c.sampler->fd().max_fd;
            sp.state = st.state;
            {
                const double hz_ = static_cast<double>(clock_ticks_per_sec());
                sp.proc_stat_cpu_s = static_cast<double>(st.utime_ticks + st.stime_ticks) / hz_;
                sp.task_group_sum_cpu_s =
                    static_cast<double>(c.sampler->task_group_cpu_ticks()) / hz_;
            }
            sp.cpu_cores = (dt_s > 0.0 && ticks >= c.last_cpu_ticks)
                ? static_cast<double>(ticks - c.last_cpu_ticks) / hz / dt_s : 0.0;
            const ThreadGroupCtxSwitch& cs = c.sampler->ctx();
            const std::uint64_t dv = cs.voluntary >= c.last_ctx_vol ? cs.voluntary - c.last_ctx_vol : 0;
            const std::uint64_t dn = cs.nonvoluntary >= c.last_ctx_nonvol ? cs.nonvoluntary - c.last_ctx_nonvol : 0;
            sp.ctx_switch_per_s = dt_s > 0.0 ? static_cast<double>(dv + dn) / dt_s : 0.0;
            if (sp.threads > c.threads_max) c.threads_max = sp.threads;
            if (sp.max_fd > c.max_fd_seen) c.max_fd_seen = sp.max_fd;
            c.last_cpu_ticks = ticks;
            c.last_ctx_vol = cs.voluntary;
            c.last_ctx_nonvol = cs.nonvoluntary;
            c.last_ns = now;
            samples.push_back(sp);
        }
        /* 会话内 rusage 采样也需要"当前线程数"，即使本次采样被判 exited 也保留上次值 */

        bool all_exited = true;
        for (const Child& c : children) if (!c.exited) { all_exited = false; break; }

        if (elapsed >= timeout_ns && !all_exited) {
            timed_out = true;
            for (Child& c : children) if (!c.exited && !c.watched_only) c.runner.kill();
            for (Child& c : children) if (!c.exited) c.exited = true;
            break;
        }
        /* 所有被测进程都退出 ⇒ 观测自然结束（相位由被测进程自己的时间线决定）。
         * 窗口是否足够由 actual_window_s vs required_window_s 判定，而不是靠空等。 */
        if (all_exited && elapsed > 1000000000ull) break;
        /* 兜底：窗口已到且没有待观测对象时也退出，避免无意义空转。 */
        if (elapsed >= window_ns && all_exited) break;
        std::this_thread::sleep_for(std::chrono::nanoseconds(sample_ns));
    }

    /* ---- 收尾：finish() + rusage ---- */
    for (Child& c : children) {
        if (c.sampler != nullptr) {
            if (!c.exited) c.sampler->sample();
            c.ctx = c.sampler->finish();
            const ProcessStat st = c.sampler->stat();
            c.cpu_ticks_at_exit = st.utime_ticks + st.stime_ticks;
            c.cpu_ticks_at_exit_valid = st.valid;
            c.cpu_scope = c.sampler->cpu_scope();
        }
        if (!c.watched_only) {
            c.rusage = c.runner.wait();
            if (!c.rusage.valid) unconfirmed.push_back("进程 " + c.label + " 的 rusage 不可用");
            if (c.rusage.killed_by_collector)
                unconfirmed.push_back("进程 " + c.label + " 被超时 SIGKILL: 其生命周期不完整");
        } else {
            unconfirmed.push_back("watched 进程 " + c.label + " 无 rusage: 上下文切换只有逐 TID 采样口径");
        }
    }
    const std::uint64_t t_end = monotonic_now_ns();
    const double actual_window_s = static_cast<double>(t_end - t_begin) / 1e9;
    if (timed_out) unconfirmed.push_back("观测被 timeout 强制结束, 进程未自然退出");

    /* ---- 相位回填 ---- */
    std::vector<PhaseMarker> markers;
    for (const Child& c : children) {
        if (c.stdout_path.empty()) continue;
        for (const PhaseMarker& m : parse_phase_markers(c.stdout_path)) markers.push_back(m);
    }
    std::sort(markers.begin(), markers.end(),
              [](const PhaseMarker& x, const PhaseMarker& y) { return x.ns < y.ns; });
    const bool time_based = (a.startup_s >= 0.0 || a.steady_s >= 0.0 || a.idle_s >= 0.0);
    for (SamplePoint& sp : samples) {
        if (!markers.empty()) {
            const char* ph = phase_at(markers, sp.ns);
            sp.phase = ph != nullptr ? ph : std::string("unassigned");
        } else if (time_based) {
            double acc = a.startup_s > 0 ? a.startup_s : 0.0;
            if (sp.offset_s < acc) sp.phase = "startup_peak";
            else {
                acc += a.steady_s > 0 ? a.steady_s : 0.0;
                sp.phase = sp.offset_s < acc ? "steady_active" : "idle_fallback";
            }
        } else {
            sp.phase = "steady_active";
        }
    }
    if (markers.empty() && !time_based)
        unconfirmed.push_back("未提供相位来源(stdout marker 或 --startup-s/--steady-s/--idle-s): 启动峰值/空闲回落相位缺失");

    /* ---- 相位聚合 → PhaseObserver ---- */
    PhaseObserver observer(std::max(a.required_window_s, 1.0));
    observer.begin(t_begin);
    {
        /* 键必须是 (ns, phase) 的**数值**序，不能是 "ns|phase" 字符串序：
         * ns 的十进制位数在运行中可能从 13 位跨到 14 位，字典序与数值序不一致，
         * 会让按相位聚合的采样顺序错乱（曾导致 duration_s 无符号下溢 1.8e10）。 */
        std::map<std::pair<std::uint64_t, std::string>, std::vector<SamplePoint*>> by_instant;
        for (SamplePoint& sp : samples)
            by_instant[std::make_pair(sp.ns, sp.phase)].push_back(&sp);
        for (auto& kv : by_instant) {
            std::vector<SamplePoint*>& v = kv.second;
            PhaseSample ps;
            ps.monotonic_ns = v.front()->ns;
            double cpu = 0.0, ctx = 0.0;
            std::uint64_t rss = 0;
            for (SamplePoint* sp : v) {
                ps.threads += sp->threads;
                cpu += sp->cpu_cores;
                ctx += sp->ctx_switch_per_s;
                rss += sp->rss_kb;
            }
            ps.cpu_cores = cpu;
            ps.ctx_switch_per_s = ctx;
            ps.rss_kb = rss;
            ps.idle_state = IdleState::unconfirmed;
            Phase p = Phase::steady_active;
            if (v.front()->phase == "startup_peak") p = Phase::startup_peak;
            else if (v.front()->phase == "idle_fallback") p = Phase::idle_fallback;
            observer.add(p, ps);
        }
    }
    PeriodicTaskCoverage periodic;
    periodic.configured = (a.periodic_ticks > 0 || !a.periodic_source.empty());
    periodic.ticks_observed = a.periodic_ticks;
    periodic.ticks_per_s = actual_window_s > 0 ? static_cast<double>(a.periodic_ticks) / actual_window_s : 0.0;
    periodic.source = a.periodic_source;
    periodic.note = periodic.configured ? "由采集参数声明" : "未声明周期任务计数来源 => 空闲未覆盖周期任务";
    /* 恢复观测（§10.1：连接 → 确认 → 静默 > keep-alive → 恢复发送）。
     * 由 --recovery-spec 提供机器可读的一轮或多轮结果；缺省则 count=0 并在 verdict 标未确认。 */
    RecoveryLatencyStats recovery;
    if (!a.recovery_spec.empty()) {
        std::ifstream rf(a.recovery_spec);
        std::string line;
        RecoveryObservation cur;
        bool have = false;
        auto flush = [&]() {
            if (have) recovery.add(cur);
            cur = RecoveryObservation{};
            have = false;
        };
        while (std::getline(rf, line)) {
            if (line.empty() || line[0] == '#') continue;
            const std::size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = line.substr(0, eq);
            const std::string v = line.substr(eq + 1);
            if (k == "round") { flush(); have = true; }
            else if (k == "silence_ms") { cur.silence_ns = static_cast<std::uint64_t>(std::atof(v.c_str()) * 1e6); have = true; }
            else if (k == "first_packet_us") { cur.first_packet_latency_ns = static_cast<std::uint64_t>(std::atof(v.c_str()) * 1e3); have = true; }
            else if (k == "routes_before") cur.routed_routes_before = std::strtoull(v.c_str(), nullptr, 10);
            else if (k == "routes_after") cur.routed_routes_after = std::strtoull(v.c_str(), nullptr, 10);
            else if (k == "lost") cur.lost_packets = (v == "1" || v == "true");
            else if (k == "rereg") cur.reregistration_required = (v == "1" || v == "true");
            else if (k == "reconnected") cur.reconnected = (v == "1" || v == "true");
        }
        flush();
        observer.set_recovery(recovery);
        if (recovery.count() == 0) unconfirmed.push_back("recovery-spec 解析后为空");
    } else {
        unconfirmed.push_back("未执行/未提供连接-静默-恢复强制用例 => 恢复首包延迟未确认");
    }

    observer.set_periodic_coverage(periodic);
    observer.set_diagnostics_enabled(a.diagnostics);
    const IdleSummary idle = observer.summary();

    /* ---- 落盘 ---- */
    const std::string run_id = a.run_id.empty() ? ("w03-" + now_utc_string()) : a.run_id;

    write_file(dir + "/source-status.txt",
               "git revision: " + git_rev + "\n\ngit status --short:\n"
               + (git_status.empty() ? std::string("(clean)\n") : git_status));
    {
        std::ostringstream o;
        o << "build_dir=" << a.build_dir << "\n";
        o << run_command_capture("grep '^CMAKE_BUILD_TYPE:' " + a.build_dir + "/CMakeCache.txt 2>/dev/null");
        o << run_command_capture("grep '^CMAKE_CXX_COMPILER:' " + a.build_dir + "/CMakeCache.txt 2>/dev/null");
        o << run_command_capture("grep '^CMAKE_GENERATOR:' " + a.build_dir + "/CMakeCache.txt 2>/dev/null");
        o << "uname=" << run_command_capture("uname -a");
        o << "clock_source=" << clock_source_name() << "\n";
        write_file(dir + "/build-config.txt", o.str());
    }
    {
        std::ostringstream o;
        for (const Child& c : children) {
            if (c.watched_only) {
                o << "watched pid=" << c.pid << " label=" << c.label
                  << " cmdline=" << process_cmdline(c.pid) << "\n";
            } else {
                const std::string bin = c.argv.empty() ? std::string() : c.argv[0];
                o << "job=" << c.label << " role=" << process_role_name(c.role) << " bin=" << bin
                  << " sha256=" << sha256_of_file(bin) << "\n    stat=" << file_stat_line(bin) << "\n";
            }
        }
        o << "collector_sha256=" << sha256_of_file("/proc/self/exe") << "\n";
        o << "libipc_sha256=" << sha256_of_file(a.build_dir + "/lib/libipc.so") << "\n";
        write_file(dir + "/binary-fingerprint.txt", o.str());
    }
    {
        std::ostringstream o;
        o << "{\n  \"run_id\": \"" << json_escape_str(run_id) << "\",\n";
        o << "  \"work_package\": \"W03\",\n";
        o << "  \"source_revision\": \"" << json_escape_str(git_rev) << "\",\n";
        o << "  \"working_tree_clean\": " << (git_status.empty() ? "true" : "false") << ",\n";
        o << "  \"working_tree_diff\": " << json_array_of_lines(git_status) << ",\n";
        o << "  \"build_dir\": \"" << json_escape_str(a.build_dir) << "\",\n";
        o << "  \"transport\": \"" << json_escape_str(a.transport) << "\",\n";
        o << "  \"process_model\": \"cross-process\",\n";
        o << "  \"measurement_mode\": \"rusage+proc_task_sample\",\n";
        o << "  \"diagnostics_enabled\": " << (a.diagnostics ? "true" : "false") << ",\n";
        o << "  \"clock_source\": \"" << clock_source_name() << "\",\n";
        o << "  \"clock_cost_ns_per_call\": " << clock_cost.vdso_ns_per_call << ",\n";
        o << "  \"clock_cost_syscall_ns_per_call\": " << clock_cost.syscall_ns_per_call << ",\n";
        o << "  \"clock_vdso_in_use\": " << (clock_cost.vdso_in_use ? "true" : "false") << ",\n";
        o << "  \"clock_cross_process_consistent\": " << (xproc.consistent ? "true" : "false") << ",\n";
        o << "  \"clock_cross_process_bracket_ns\": " << xproc.bracket_ns << ",\n";
        o << "  \"worker_count\": " << a.worker_count << ",\n";
        o << "  \"window_s\": " << actual_window_s << ",\n";
        o << "  \"required_window_s\": " << a.required_window_s << ",\n";
        o << "  \"sample_interval_ms\": " << a.sample_ms << ",\n";
        o << "  \"sample_rows\": " << samples.size() << ",\n";
        o << "  \"config_hash\": \"" << config_hash << "\",\n";
        o << "  \"result_files\": [\"samples.csv\", \"cpu.json\", \"idle.json\", \"counters.json\", "
             "\"observation_overhead.json\", \"path_evidence.json\", \"schema.json\", \"schema.csv\", "
             "\"environment.json\", \"topology.json\", \"verdict.md\"],\n";
        o << "  \"created_utc\": \"" << now_utc_string() << "\"\n}\n";
        write_file(dir + "/manifest.json", o.str());
    }
    write_file(dir + "/environment.json",
               std::string("{\n  \"captured_utc\": \"") + now_utc_string()
               + "\",\n  \"clock_source\": \"" + clock_source_name() + "\",\n  \"platform\": "
               + platform_info_json(platform) + "}\n");
    {
        std::ostringstream o;
        o << "{\n  \"online_cpus\": " << platform.online_cpus << ",\n";
        o << "  \"configured_cpus\": " << platform.configured_cpus << ",\n";
        o << "  \"smt_on\": " << (platform.smt_on ? "true" : "false") << ",\n";
        o << "  \"numa_nodes\": " << platform.numa_nodes << ",\n";
        o << "  \"governors_unique\": [";
        for (std::size_t i = 0; i < platform.cpu_governors_unique.size(); ++i) {
            if (i) o << ", ";
            o << "\"" << json_escape_str(platform.cpu_governors_unique[i]) << "\"";
        }
        o << "],\n  \"jobs\": [\n";
        for (std::size_t i = 0; i < children.size(); ++i) {
            const Child& c = children[i];
            o << "    {\"label\": \"" << json_escape_str(c.label) << "\", \"role\": \""
              << process_role_name(c.role) << "\", \"pid\": " << c.pid
              << ", \"watched_only\": " << (c.watched_only ? "true" : "false") << "}";
            if (i + 1 < children.size()) o << ",";
            o << "\n";
        }
        o << "  ]\n}\n";
        write_file(dir + "/topology.json", o.str());
    }
    {
        std::ostringstream o;
        o << "run_id,monotonic_ns,offset_s,phase,pid,role,label,threads,proc_stat_cpu_s,"
             "task_group_sum_cpu_s,cpu_cores_interval,ctx_switch_per_s_interval,rss_kb,max_fd,state\n";
        for (const SamplePoint& sp : samples) {
            o << run_id << ',' << sp.ns << ',' << sp.offset_s << ',' << sp.phase << ','
              << sp.pid << ',' << process_role_name(sp.role) << ',' << sp.label << ','
              << sp.threads << ',' << sp.proc_stat_cpu_s << ',' << sp.task_group_sum_cpu_s << ','
              << sp.cpu_cores << ',' << sp.ctx_switch_per_s << ','
              << sp.rss_kb << ',' << sp.max_fd << ',' << sp.state << "\n";
        }
        write_file(dir + "/samples.csv", o.str());
    }
    {
        std::ostringstream o;
        o << "{\n  \"kind\": \"cpu_and_ctx_switch\",\n";
        o << "  \"window_s\": " << actual_window_s << ",\n";
        o << "  \"orchestrator\": {\"pid\": " << ::getpid()
          << ", \"note\": \"采集器自身; 观测开销见 observation_overhead.json\"},\n";
        o << "  \"processes\": [\n";
        double total_cpu = 0.0;
        std::uint64_t total_vol = 0, total_nonvol = 0;
        std::uint64_t total_vol_sampled = 0, total_nonvol_sampled = 0;
        bool any_maxrss = false;
        for (std::size_t i = 0; i < children.size(); ++i) {
            const Child& c = children[i];
            const double hz = static_cast<double>(clock_ticks_per_sec());
            const double sampled_total_s = c.cpu_ticks_at_exit_valid
                ? static_cast<double>(c.cpu_ticks_at_exit) / hz : 0.0;
            total_cpu += c.rusage.total_cpu_s;
            total_vol += c.rusage.voluntary;
            total_nonvol += c.rusage.nonvoluntary;
            total_vol_sampled += c.ctx.voluntary;
            total_nonvol_sampled += c.ctx.nonvoluntary;
            if (c.rusage.maxrss_kb > 0) any_maxrss = true;
            o << "    {\n";
            o << "      \"label\": \"" << json_escape_str(c.label) << "\",\n";
            o << "      \"role\": \"" << process_role_name(c.role) << "\",\n";
            o << "      \"pid\": " << c.pid << ",\n";
            o << "      \"cmdline\": \"" << json_escape_str(c.cmdline) << "\",\n";
            o << "      \"rusage_valid\": " << (c.rusage.valid ? "true" : "false") << ",\n";
            o << "      \"rusage_user_s\": " << c.rusage.user_s << ",\n";
            o << "      \"rusage_sys_s\": " << c.rusage.sys_s << ",\n";
            o << "      \"rusage_total_cpu_s\": " << c.rusage.total_cpu_s << ",\n";
            o << "      \"proc_stat_cpu_s_sampled_lower_bound\": " << sampled_total_s << ",\n";
            o << "      \"ctx_switch_rusage_lifetime\": {\"voluntary\": " << c.rusage.voluntary
              << ", \"nonvoluntary\": " << c.rusage.nonvoluntary
              << ", \"complete\": " << (c.rusage.valid ? "true" : "false") << "},\n";
            o << "      \"ctx_switch_proc_task_sample\": {\"voluntary\": " << c.ctx.voluntary
              << ", \"nonvoluntary\": " << c.ctx.nonvoluntary
              << ", \"complete\": " << (c.ctx.complete ? "true" : "false")
              << ", \"voluntary_undercount_bound\": " << c.ctx.voluntary_undercount_bound
              << ", \"nonvoluntary_undercount_bound\": " << c.ctx.nonvoluntary_undercount_bound
              << ", \"tids_seen\": " << c.ctx.tids_seen
              << ", \"tids_present_first_sample\": " << c.ctx.tids_present_first_sample
              << ", \"tids_born_mid_window\": " << c.ctx.tids_born_mid_window
              << ", \"tids_starttime_boundary\": " << c.ctx.tids_starttime_boundary
              << ", \"tids_starttime_unknown\": " << c.ctx.tids_starttime_unknown
              << ", \"tids_exited\": " << c.ctx.tids_exited
              << ", \"samples\": " << c.ctx.samples
              << ", \"method\": \"" << c.ctx.method << "\""
              << ", \"main_thread_only_voluntary_reference\": " << c.ctx.main_thread_voluntary_reference
              << ", \"note\": \"" << json_escape_str(c.ctx.note) << "\"},\n";
            {
                /* CPU 完整性: 最后一次成功采样点的 /proc/<pid>/stat 与 wait4 rusage 对照。
                 * 两者都在毫秒级 CPU 上应接近; 差值主要来自最后一次采样到退出之间的区间。 */
                const double last_proc_s = c.cpu_ticks_at_exit_valid
                    ? static_cast<double>(c.cpu_ticks_at_exit) / hz : 0.0;
                const double agree = c.rusage.total_cpu_s > 0.0
                    ? last_proc_s / c.rusage.total_cpu_s : 0.0;
                o << "      \"cpu_integrity\": {\"proc_pid_stat_last_sample_s\": " << last_proc_s
                  << ", \"rusage_total_cpu_s\": " << c.rusage.total_cpu_s
                  << ", \"ratio\": " << agree
                  << ", \"judgement\": \""
                  << (c.rusage.valid
                      ? (agree > 0.5 && agree <= 1.05
                         ? "一致(差值 = 最后一次采样到进程退出之间的区间)"
                         : "不一致, 需检查采样是否过早结束")
                      : "无 rusage(外部进程), 不可判")
                  << "\"},\n";
            }
            o << "      \"threads_max\": " << c.threads_max << ",\n";
            {
                const double proc_stat_s = static_cast<double>(c.cpu_scope.proc_stat_ticks) / hz;
                const double tg_sum_s = static_cast<double>(c.cpu_scope.task_group_sum_ticks) / hz;
                o << "      \"cpu_scope_check\": {\"valid\": "
                  << (c.cpu_scope.valid ? "true" : "false")
                  << ", \"proc_pid_stat_cpu_s\": " << proc_stat_s
                  << ", \"task_group_sum_cpu_s\": " << tg_sum_s
                  << ", \"ratio\": " << c.cpu_scope.ratio
                  << ", \"tids\": " << c.cpu_scope.tids
                  << ", \"judgement\": \"" << c.cpu_scope.judgement() << "\"},\n";
            }
            o << "      \"max_fd_seen\": " << c.max_fd_seen << "\n";
            o << "    }";
            if (i + 1 < children.size()) o << ",";
            o << "\n";
        }
        o << "  ],\n";
        o << "  \"totals\": {\"rusage_total_cpu_s\": " << total_cpu
          << ", \"rusage_ctx_voluntary\": " << total_vol
          << ", \"rusage_ctx_nonvoluntary\": " << total_nonvol
          << ", \"sampled_ctx_voluntary\": " << total_vol_sampled
          << ", \"sampled_ctx_nonvoluntary\": " << total_nonvol_sampled << "},\n";
        o << "  \"cpu_cores_of_total\": " << (actual_window_s > 0 ? total_cpu / actual_window_s : 0.0) << ",\n";
        o << "  \"notes\": [\n";
        o << "    \"上下文切换完整口径 = rusage ru_nvcsw/ru_nivcsw, 覆盖整个线程组生命周期\",\n";
        o << "    \"proc_task_sample 为逐 TID 采样口径; complete=false 表示窗口内有读不到的区间\",\n";
        o << "    \"/proc/<pid>/status 的主线程值只作参考(main_thread_only_*_reference), 不是全进程合计\",\n";
        o << "    \"daemon 角色的服务进程成本必须单独列行; 未声明则为 0 且 verdict 标注未确认\"\n";
        o << "  ]\n}\n";
        write_file(dir + "/cpu.json", o.str());
        if (!any_maxrss) unconfirmed.push_back("无任何进程提供 rusage maxrss");
    }
    write_file(dir + "/idle.json", observer.to_json());
    {
        std::ostringstream o;
        o << "{\n  \"kind\": \"counter_merge\",\n  \"diagnostics_enabled\": "
          << (a.diagnostics ? "true" : "false") << ",\n";
        o << "  \"sources\": [\n";
        for (std::size_t i = 0; i < a.counters_json.size(); ++i) {
            const std::vector<std::string> parts = split(a.counters_json[i], ',');
            o << "    {\"path\": \"" << json_escape_str(parts[0]) << "\", \"label\": \""
              << json_escape_str(parts.size() > 1 ? parts[1] : std::string()) << "\", \"sha256\": \""
              << sha256_of_file(parts[0]) << "\", \"exists\": "
              << (read_text_file(parts[0]).empty() ? "false" : "true") << "}";
            if (i + 1 < a.counters_json.size()) o << ",";
            o << "\n";
        }
        o << "  ],\n";
        o << "  \"collector_process_snapshot\": " << CounterRegistry::instance().to_json() << ",\n";
        o << "  \"note\": \""
          << (a.counters_json.empty()
              ? "未提供被测进程导出的计数器文件(--counters-json) => 该运行的路径计数未确认"
              : "被测进程计数按文件引用; 求和由汇总脚本完成") << "\"\n}\n";
        write_file(dir + "/counters.json", o.str());
        if (a.counters_json.empty())
            unconfirmed.push_back("未提供被测进程计数器导出 => 路径计数未确认");
    }
    {
        std::ostringstream o;
        o << "{\n  \"kind\": \"observation_overhead\",\n  \"iterations\": " << overhead.iterations << ",\n";
        o << "  \"counter_inc_ns\": " << overhead.counter_inc_ns << ",\n";
        o << "  \"counter_inc_cached_ns\": " << overhead.counter_inc_cached_ns << ",\n";
        o << "  \"diagnostics_inc_enabled_ns\": " << overhead.diag_inc_enabled_ns << ",\n";
        o << "  \"diagnostics_inc_disabled_ns\": " << overhead.diag_inc_disabled_ns << ",\n";
        o << "  \"scan_scope_enabled_ns\": " << overhead.scan_scope_enabled_ns
          << ",  /* 当前接线形态: 构造->finish->set_ready->析构 */\n";
        o << "  \"scan_scope_disabled_ns\": " << overhead.scan_scope_disabled_ns << ",\n";
        o << "  \"scan_scope_enabled_no_finish_ns\": " << overhead.scan_scope_enabled_no_finish_ns
          << ",  /* 历史口径: 构造->析构, 与旧读数可比 */\n";
        o << "  \"scan_scope_disabled_no_finish_ns\": " << overhead.scan_scope_disabled_no_finish_ns << ",\n";
        o << "  \"scan_scope_finish_delta_enabled_ns\": "
          << (overhead.scan_scope_enabled_ns - overhead.scan_scope_enabled_no_finish_ns)
          << ",  /* 只调 finish 的增量 (诊断开启时含 1 次 clock_gettime) */\n";
        o << "  \"clock_now_ns\": " << overhead.clock_now_ns << ",\n";
        o << "  \"clock_vdso_ns_per_call\": " << clock_cost.vdso_ns_per_call << ",\n";
        o << "  \"clock_syscall_ns_per_call\": " << clock_cost.syscall_ns_per_call << ",\n";
        o << "  \"proc_self_stat_read_ns\": " << overhead.proc_self_stat_read_ns << ",\n";
        o << "  \"proc_task_status_all_ns\": " << overhead.proc_task_status_all_ns << ",\n";
        o << "  \"collector_sampling_cost_ns_per_interval\": "
          << overhead.proc_self_stat_read_ns + overhead.proc_task_status_all_ns << ",\n";
        o << "  \"notes\": [\"逐 TID 采样每区间成本 = proc_self_stat_read_ns + proc_task_status_all_ns\",\n";
        o << "             \"ScanRoundScope 关闭诊断时每个扫描轮只多一次 relaxed 布尔读\"]\n}\n";
        write_file(dir + "/observation_overhead.json", o.str());
    }
    write_file(dir + "/schema.json", schema_document_json(run_id));
    write_file(dir + "/schema.csv", schema_document_csv());

    /* 路径证据 */
    EvidenceRegister reg;
    if (!a.path_spec.empty()) {
        std::ifstream f(a.path_spec);
        std::string line;
        PathEvidence cur;
        bool have = false;
        auto flush = [&]() { if (have) reg.add(cur); cur = PathEvidence{}; have = false; };
        while (std::getline(f, line)) {
            if (line.empty() || line[0] == '#') continue;
            const std::size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            const std::string k = line.substr(0, eq);
            const std::string v = line.substr(eq + 1);
            if (k == "path") {
                flush();
                have = true;
                if (v == "tlv") cur.path = PathKind::tlv;
                else if (v == "dzflat-a") cur.path = PathKind::dzflat_a;
                else if (v == "dzflat-b") cur.path = PathKind::dzflat_b;
                else if (v == "cyclonedds-iox") cur.path = PathKind::cyclonedds_iox;
                else if (v == "compat-fallback") cur.path = PathKind::compat_fallback;
                else cur.path = PathKind::unknown;
            } else if (k == "level") {
                if (v == "S1") cur.level = EvidenceLevel::s1_source_exists;
                else if (v == "S2") cur.level = EvidenceLevel::s2_module_wired;
                else if (v == "S3") cur.level = EvidenceLevel::s3_single_case_verified;
                else if (v == "S4") cur.level = EvidenceLevel::s4_independent_verified;
                else if (v == "S5") cur.level = EvidenceLevel::s5_default_enabled;
                else cur.level = EvidenceLevel::s0_unchecked;
            } else if (k == "experiment") cur.experiment_id = v;
            else if (k == "call_site") cur.call_sites.push_back(v);
            else if (k == "runtime") cur.runtime_evidence.push_back(v);
            else if (k == "fingerprint") cur.binary_fingerprints.push_back(v);
            else if (k == "counter") cur.counters.push_back(v);
            else if (k == "group") cur.experiment_groups.push_back(v);
            else if (k == "samples") cur.samples = std::strtoull(v.c_str(), nullptr, 10);
            else if (k == "fallback") cur.fallback_count = std::strtoull(v.c_str(), nullptr, 10);
        }
        flush();
        if (reg.size() == 0) unconfirmed.push_back("path-spec 解析后为空");
    } else {
        const PathKind kinds[] = {PathKind::tlv, PathKind::dzflat_a, PathKind::dzflat_b,
                                  PathKind::cyclonedds_iox, PathKind::compat_fallback};
        for (PathKind k : kinds) {
            PathEvidence e;
            e.path = k;
            e.level = EvidenceLevel::s0_unchecked;
            e.experiment_id = run_id;
            reg.add(e);
        }
        unconfirmed.push_back("未提供路径证据规格: 所有数据路径按 未确认 登记");
    }
    write_file(dir + "/path_evidence.json", reg.to_json());

    /* verdict.md */
    {
        std::ostringstream o;
        o << "# W03 运行判定 — " << run_id << "\n\n";
        o << "## 口径\n\n| 项 | 值 | 证据文件 |\n|---|---|---|\n";
        o << "| 时基 | " << clock_source_name() << " | manifest.json / environment.json |\n";
        o << "| 计时成本 | " << clock_cost.vdso_ns_per_call << " ns/call (vDSO="
          << (clock_cost.vdso_in_use ? "是" : "否") << ", 强制 syscall="
          << clock_cost.syscall_ns_per_call << " ns) | observation_overhead.json |\n";
        o << "| 跨进程时钟一致性 | " << (xproc.consistent ? "通过" : "未通过")
          << " (fork+pipe 区间 " << xproc.bracket_ns << " ns) | manifest.json |\n";
        o << "| 上下文切换完整口径 | rusage ru_nvcsw/ru_nivcsw（整个线程组生命周期） | cpu.json |\n";
        o << "| 上下文切换采样口径 | 逐 TID 增量 + complete/undercount_bound 标注 | cpu.json |\n";
        o << "| 观测窗口 | " << actual_window_s << " s（要求 >= " << a.required_window_s << " s） | idle.json |\n";
        o << "| 诊断计数 | " << (a.diagnostics ? "开启" : "关闭") << " | counters.json |\n";
        o << "| 采样行数 | " << samples.size() << " (间隔 " << a.sample_ms << " ms) | samples.csv |\n";
        o << "| 恢复首包延迟 | count=" << idle.recovery.count << ", mean="
          << idle.recovery.mean_us << " us, p99=" << idle.recovery.p99_us << " us, lost="
          << idle.recovery.lost_count << ", rereg=" << idle.recovery.rereg_count
          << " | idle.json |\n\n";
        o << "## 相位\n\n| 相位 | covered | threads mean/min/max | cpu_cores mean | ctx/s mean |\n|---|---|---|---|---|\n";
        auto row = [&](const char* n, const PhaseStats& s) {
            o << "| " << n << " | " << (s.covered ? "yes" : "NO") << " | " << s.threads_mean << "/"
              << s.threads_min << "/" << s.threads_max << " | " << s.cpu_cores_mean << " | "
              << s.ctx_switch_per_s_mean << " |\n";
        };
        row("startup_peak", idle.startup_peak);
        row("steady_active", idle.steady_active);
        row("idle_fallback", idle.idle_fallback);
        o << "\n周期任务覆盖: " << (idle.periodic.configured ? "已声明" : "**未声明**")
          << ", ticks=" << idle.periodic.ticks_observed << ", source="
          << (idle.periodic.source.empty() ? std::string("-") : idle.periodic.source) << "\n";
        o << "空闲回落: " << (idle.idle_fallback_observed ? "已观测" : "**未观测**")
          << " (线程回落比例 " << idle.thread_fallback_ratio << ")\n";
        if (a.expect_threads >= 0) {
            long observed = 0;
            for (const Child& c : children) observed = std::max(observed, static_cast<long>(c.threads_max));
            o << "已知线程校准: 期望 " << a.expect_threads << ", 观测峰值 " << observed << " => "
              << (observed >= a.expect_threads ? "PASS" : "FAIL") << "\n";
            if (observed < a.expect_threads)
                unconfirmed.push_back("已知线程数未达标: 期望 " + std::to_string(a.expect_threads)
                                      + " 观测 " + std::to_string(observed));
        }
        o << "\n## 路径证据\n\n已确认 " << reg.confirmed_paths().size() << " 条, 未确认 "
          << reg.unconfirmed_paths().size() << " 条\n\n";
        for (const std::string& p : reg.unconfirmed_paths()) o << "- **未确认**: " << p << "\n";
        o << "\n## 未确认项（必须随结论一起报告）\n\n";
        bool any = false;
        for (const std::string& n : unconfirmed) { o << "- " << n << "\n"; any = true; }
        if (!idle.window_sufficient) { o << "- 观测窗口不足 " << a.required_window_s << " s\n"; any = true; }
        if (!idle.startup_peak.covered) { o << "- 缺启动峰值相位\n"; any = true; }
        if (!idle.steady_active.covered) { o << "- 缺稳定活跃相位\n"; any = true; }
        if (!idle.idle_fallback.covered) { o << "- 缺空闲回落相位\n"; any = true; }
        if (!any) o << "- （无）\n";
        o << "\n## 逐项引用\n\n";
        o << "- 相位数值: idle.json phases.*\n- 进程 CPU/上下文切换: cpu.json processes[]\n";
        o << "- 采样序列: samples.csv\n- 计数开销: observation_overhead.json\n";
        o << "- 字段定义: schema.json / schema.csv\n- 路径证据: path_evidence.json\n";
        o << "- 原始 stdout/stderr: stdout/*.log, stderr/*.log\n";
        write_file(dir + "/verdict.md", o.str());
    }

    std::printf("evidence_dir=%s\n", dir.c_str());
    std::printf("window_s=%.3f samples=%zu jobs=%zu\n", actual_window_s, samples.size(), children.size());
    std::printf("clock_consistent=%d clock_ns_per_call=%.2f\n", (int)xproc.consistent, clock_cost.vdso_ns_per_call);
    for (const Child& c : children) {
        std::printf("job=%s pid=%d rusage_cpu=%.3fs rusage_ctx_vol=%llu sampled_ctx_vol=%llu "
                    "sample_complete=%d tids_born=%llu tids_exited=%llu threads_max=%zu\n",
                    c.label.c_str(), c.pid, c.rusage.total_cpu_s,
                    (unsigned long long)c.rusage.voluntary, (unsigned long long)c.ctx.voluntary,
                    (int)c.ctx.complete, (unsigned long long)c.ctx.tids_born_mid_window,
                    (unsigned long long)c.ctx.tids_exited, c.threads_max);
    }
    std::printf("window_sufficient=%d idle_fallback_observed=%d unconfirmed_paths=%zu\n",
                (int)idle.window_sufficient, (int)idle.idle_fallback_observed, reg.unconfirmed_paths().size());
    std::printf("W03_EVIDENCE_DONE\n");

    for (Child& c : children) delete c.sampler;
    return 0;
}

/* 自检：已知线程活动是否被正确计入（TID 生命周期 + 线程组合计 vs 主线程参考）。 */

/* W03/t44：结束值接口（R0-9/R0-10）的机器可读自测。
 * 用途：W06 接线后可用它对照"接口语义是否被正确使用"（每 worker 出参、gauge 不得当
 * 全池总量、两个 gauge 语义不同、量纲不同不得相加、诊断关闭=未采集）。 */
int run_scan_end_values()
{
    using namespace dzIPC::measure;
    CounterRegistry& r = CounterRegistry::instance();
    std::string out = "{\n  \"kind\": \"scan_end_values_selftest\",\n";
    bool ok = true;

    /* A. 关诊断 ⇒ 未采集（⛔ 非实测 0） */
    r.set_diagnostics_enabled(false);
    const std::uint64_t off_ready = r.get(CounterId::scan_ready_routes_total);
    const bool off_uncollected = counter_is_uncollected(CounterId::scan_ready_routes_total, false);
    bool off_elapsed_uncollected = false;
    {
        ScanRoundScope s(8, 1);
        s.finish(2, 3);
        off_elapsed_uncollected = !s.result().elapsed_ns_collected;
        ok = ok && s.finished();
    }
    const bool off_no_write = (r.get(CounterId::scan_ready_routes_total) == off_ready);
    ok = ok && off_uncollected && off_elapsed_uncollected && off_no_write;

    /* B. 开诊断 ⇒ 5 轮结束值逐项可核对 */
    r.set_diagnostics_enabled(true);
    ScanRoundAccumulator w0, w1;
    const std::uint64_t ready_before = r.get(CounterId::scan_ready_routes_total);
    const std::uint64_t after_total_before = r.get(CounterId::deferred_depth_after_total);
    for (int i = 0; i < 3; ++i) { ScanRoundScope s(16, 5); s.finish(1, 5); w0.add(s); }
    for (int i = 0; i < 2; ++i) { ScanRoundScope s(16, 7); s.finish(0, 7); w1.add(s); }
    const std::uint64_t ready_delta = r.get(CounterId::scan_ready_routes_total) - ready_before;
    const std::uint64_t after_total_delta = r.get(CounterId::deferred_depth_after_total) - after_total_before;
    const std::uint64_t pool_depth = w0.deferred_depth_after_last() + w1.deferred_depth_after_last();
    const std::uint64_t b_gauge_after_last = r.get(CounterId::deferred_depth_after_last);
    const std::uint64_t b_before_last = r.get(CounterId::deferred_depth_last);
    const bool sums_ok = (ready_delta == w0.ready_routes_total() + w1.ready_routes_total())
                      && (after_total_delta == w0.deferred_depth_after_total() + w1.deferred_depth_after_total());
    const bool pool_depth_ok = (pool_depth == 12u);
    const bool gauge_not_pool = (b_gauge_after_last != pool_depth);
    ok = ok && sums_ok && pool_depth_ok && gauge_not_pool;

    /* C. 扫描前/扫描后两个 gauge 语义不同 */
    {
        ScanRoundScope s(4, 9);
        s.finish(0, 1);
    }
    const bool gauges_distinct = (r.get(CounterId::deferred_depth_last) == 9u)
                              && (r.get(CounterId::deferred_depth_after_last) == 1u);
    ok = ok && gauges_distinct;

    /* D. 量纲不同（轮数 vs route 数）不得相加 */
    {
        ScanRoundScope s(4, 0);
        s.finish(2, 2);
        s.set_ready(true);
    }
    const bool dims_distinct = (r.get(CounterId::scan_ready_routes_total) != 0u);
    (void)dims_distinct;
    r.set_diagnostics_enabled(false);

    out += "  \"A_diag_off_uncollected\": " + std::string(off_uncollected ? "true" : "false") + ",\n";
    out += "  \"A_diag_off_elapsed_uncollected\": " + std::string(off_elapsed_uncollected ? "true" : "false") + ",\n";
    out += "  \"A_diag_off_no_counter_write\": " + std::string(off_no_write ? "true" : "false") + ",\n";
    out += "  \"B_sums_match_global\": " + std::string(sums_ok ? "true" : "false") + ",\n";
    out += "  \"B_pool_depth_is_sum_of_workers\": " + std::string(pool_depth_ok ? "true" : "false") + ",\n";
    out += "  \"B_gauge_is_not_pool_total\": " + std::string(gauge_not_pool ? "true" : "false") + ",\n";
    out += "  \"B_ready_routes_delta\": " + std::to_string(ready_delta) + ",\n";
    out += "  \"B_deferred_after_total_delta\": " + std::to_string(after_total_delta) + ",\n";
    out += "  \"B_pool_current_depth\": " + std::to_string(pool_depth) + ",\n";
    out += "  \"B_gauge_after_last_at_that_moment\": " + std::to_string(b_gauge_after_last) + ",\n";
    out += "  \"B_gauge_before_last_at_that_moment\": " + std::to_string(b_before_last) + ",\n";
    out += "  \"C_before_after_gauges_distinct\": " + std::string(gauges_distinct ? "true" : "false") + ",\n";
    out += "  \"verdict\": \"" + std::string(ok ? "PASS" : "FAIL") + "\"\n}\n";
    std::fputs(out.c_str(), stdout);
    return ok ? 0 : 1;
}

int run_selftest()
{
    std::printf("== W03 selftest: 已知线程活动可被计入 ==\n");
    const int self = static_cast<int>(::getpid());

    ProcessSampler base(self);
    if (!base.attach()) { std::printf("attach failed\n"); return 1; }
    base.sample();
    const std::size_t threads_before = base.threads().size();
    const ThreadStatus lead_before = read_thread_status(self, self);

    constexpr int kThreads = 4;
    constexpr int kSleeps = 150;
    std::atomic<int> ready{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&ready]() {
            ready.fetch_add(1);
            for (int i = 0; i < kSleeps; ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        });
    }
    while (ready.load() < kThreads) std::this_thread::sleep_for(std::chrono::microseconds(200));

    const std::uint64_t t0 = monotonic_now_ns();
    std::size_t threads_peak = 0;
    while (monotonic_now_ns() - t0 < 600ull * 1000 * 1000) {
        base.sample();
        threads_peak = std::max(threads_peak, base.threads().size());
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    for (auto& th : threads) th.join();
    base.sample();
    const ThreadGroupCtxSwitch cs = base.finish();
    const std::size_t threads_after = list_tids(self).size();
    const ThreadStatus lead_after = read_thread_status(self, self);

    const std::uint64_t lead_delta = lead_after.voluntary - lead_before.voluntary;
    std::printf("threads_before=%zu threads_peak=%zu threads_after=%zu\n",
                threads_before, threads_peak, threads_after);
    std::printf("tids_seen=%llu born_mid_window=%llu exited=%llu samples=%zu complete=%d\n",
                (unsigned long long)cs.tids_seen, (unsigned long long)cs.tids_born_mid_window,
                (unsigned long long)cs.tids_exited, cs.samples, (int)cs.complete);
    std::printf("ctx_voluntary_threadgroup=%llu ctx_nonvoluntary_threadgroup=%llu\n",
                (unsigned long long)cs.voluntary, (unsigned long long)cs.nonvoluntary);
    std::printf("main_thread_only_voluntary_reference=%llu (raw delta %llu)\n",
                (unsigned long long)cs.main_thread_voluntary_reference, (unsigned long long)lead_delta);
    std::printf("undercount_bound_vol=%llu method=%s\n",
                (unsigned long long)cs.voluntary_undercount_bound, cs.method.c_str());

    bool ok = true;
    if (cs.tids_born_mid_window < static_cast<std::uint64_t>(kThreads)) {
        std::printf("FAIL: 新生线程未被识别 (born=%llu < %d)\n",
                    (unsigned long long)cs.tids_born_mid_window, kThreads); ok = false;
    }
    if (cs.tids_exited < static_cast<std::uint64_t>(kThreads)) {
        std::printf("FAIL: 退出线程未被识别 (exited=%llu < %d)\n",
                    (unsigned long long)cs.tids_exited, kThreads); ok = false;
    }
    if (cs.voluntary <= cs.main_thread_voluntary_reference) {
        std::printf("FAIL: 线程组合计未超过主线程参考值 => 全进程合计口径可疑\n"); ok = false;
    }
    if (cs.complete) {
        std::printf("FAIL: 窗口内存在退出线程却声明 complete=true\n"); ok = false;
    }
    if (threads_peak < threads_before + static_cast<std::size_t>(kThreads)) {
        std::printf("FAIL: 线程峰值未包含已知线程 (peak=%zu)\n", threads_peak); ok = false;
    }
    std::printf("selftest=%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}

}   // namespace

int main(int argc, char** argv)
{
    const Args a = parse_args(argc, argv);
    if (a.mode.empty()) { std::fputs(kUsage, stdout); return 1; }
    if (a.mode == "schema") {
        const std::string s = schema_document_json("w03-schema");
        if (a.out.empty()) std::fputs(s.c_str(), stdout);
        else { write_file(a.out, s); write_file(a.out + ".csv", schema_document_csv()); }
        return 0;
    }
    if (a.mode == "overhead") {
        const CounterOverhead o = measure_counter_overhead(a.iterations);
        const ClockCost c = measure_clock_cost(a.iterations);
        std::ostringstream s;
        s << "{\n  \"counter_inc_ns\": " << o.counter_inc_ns << ",\n"
          << "  \"counter_inc_cached_ns\": " << o.counter_inc_cached_ns << ",\n"
          << "  \"diagnostics_inc_enabled_ns\": " << o.diag_inc_enabled_ns << ",\n"
          << "  \"diagnostics_inc_disabled_ns\": " << o.diag_inc_disabled_ns << ",\n"
          << "  \"scan_scope_enabled_ns\": " << o.scan_scope_enabled_ns
          << ",  /* 当前接线形态: 构造->finish->set_ready->析构 */\n"
          << "  \"scan_scope_disabled_ns\": " << o.scan_scope_disabled_ns << ",\n"
          << "  \"scan_scope_enabled_no_finish_ns\": " << o.scan_scope_enabled_no_finish_ns
          << ",  /* 历史口径: 构造->析构, 与旧读数可比 */\n"
          << "  \"scan_scope_disabled_no_finish_ns\": " << o.scan_scope_disabled_no_finish_ns << ",\n"
          << "  \"scan_scope_finish_delta_enabled_ns\": "
          << (o.scan_scope_enabled_ns - o.scan_scope_enabled_no_finish_ns) << ",\n"
          << "  \"clock_now_ns\": " << o.clock_now_ns << ",\n"
          << "  \"clock_vdso_ns_per_call\": " << c.vdso_ns_per_call << ",\n"
          << "  \"clock_syscall_ns_per_call\": " << c.syscall_ns_per_call << ",\n"
          << "  \"proc_self_stat_read_ns\": " << o.proc_self_stat_read_ns << ",\n"
          << "  \"proc_task_status_all_ns\": " << o.proc_task_status_all_ns << "\n}\n";
        const std::string js = s.str();
        if (a.out.empty()) std::fputs(js.c_str(), stdout); else write_file(a.out, js);
        return 0;
    }
    if (a.mode == "clock-check") {
        const XprocClockCheck x = verify_cross_process_clock();
        const ClockCost c = measure_clock_cost(a.iterations);
        std::ostringstream s;
        s << "{\"clock_source\": \"" << clock_source_name() << "\", \"consistent\": "
          << (x.consistent ? "true" : "false") << ", \"performed\": "
          << (x.performed ? "true" : "false") << ", \"parent_before_ns\": " << x.parent_before_ns
          << ", \"child_ns\": " << x.child_ns << ", \"parent_after_ns\": " << x.parent_after_ns
          << ", \"bracket_ns\": " << x.bracket_ns
          << ", \"vdso_ns_per_call\": " << c.vdso_ns_per_call
          << ", \"syscall_ns_per_call\": " << c.syscall_ns_per_call
          << ", \"note\": \"" << json_escape_str(x.note) << "\"}\n";
        const std::string js = s.str();
        if (a.out.empty()) std::fputs(js.c_str(), stdout); else write_file(a.out, js);
        return x.consistent ? 0 : 1;
    }
    if (a.mode == "selftest") return run_selftest();
    if (a.mode == "scan-end-values") return run_scan_end_values();
    if (a.mode == "evidence") {
        if (a.out_dir.empty()) { std::fprintf(stderr, "--out-dir 必填\n"); return 2; }
        return run_evidence(a);
    }
    std::fputs(kUsage, stdout);
    return 1;
}

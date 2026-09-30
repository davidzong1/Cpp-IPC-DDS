#pragma once
/* W03 测量口径 · 进程/线程组 CPU 与上下文切换采集
 * ============================================================================
 * 交付依据：团队改造方案 §4 W03 第 2、3、6 条：
 *   · 「CPU 统计注明发布进程、订阅进程、守护进程及总量；CycloneDDS/RouDi 的服务
 *     成本不能无说明地遗漏。」
 *   · 「上下文切换优先使用能覆盖线程组整个生命周期的工具；若只能逐 TID 采样，
 *     必须处理新生/退出线程或将结果标为不完整，不能把 /proc/self/status 当全进程
 *     合计。」
 *   · 「空闲观测覆盖周期任务，稳定窗口建议至少 60 秒；线程启动峰值、稳定活动
 *     线程、空闲回落分别统计。」
 *
 * 两种模式（报告必须写明用的是哪一种）：
 *   A. **rusage 全生命周期模式（首选）**：本采集器用 `fork+execvp` 拉起被测进程，
 *      结束时 `wait4()` 取 `struct rusage`。`ru_nvcsw/ru_nivcsw` 覆盖**整个线程组
 *      从创建到退出**的上下文切换，`ru_utime/ru_stime` 覆盖全部线程 CPU，无采样
 *      损失。这是"覆盖线程组整个生命周期"的最强证据。
 *   B. /proc 逐 TID 采样模式（外部进程）：被测进程不是本采集器的子进程时，
 *      每个采样周期遍历 `/proc/<pid>/task/<tid>/status` 求增量。必须处理：
 *        · **新生线程**：TID 在窗口开始后才创建 ⇒ 从 0 开始累计（该线程的全部
 *          生命周期切换都在窗口内），按 `/proc/<pid>/task/<tid>/stat` 的
 *          `starttime` 与窗口起点（`/proc/uptime` 同基）比较判定，不靠"第一次看见"；
 *        · **退出线程**：最后一次观察到退出之间存在一个采样区间无法读取 ⇒ 计入
 *          `*_undercount_bound`（以观测到的单 TID 单区间最大增量作为界）并把
 *          `complete=false`，**不允许**把不完整值当成全量；
 *        · **PID 复用**：以 `(pid, starttime)` 为身份，身份变了就作废本次采集。
 *      ⛔ 明令禁止把 `/proc/<pid>/status`（或 `/proc/self/status`）的
 *        `voluntary_ctxt_switches` 当作全进程合计 —— 它是**线程组主线程**的值。
 *        本文件把它作为 `main_thread_reference_*` 单独输出，字段名自带
 *        "reference" 字样，schema 描述写明"不是全进程合计"。
 *
 * CPU 口径：
 *   · \(\text{utime}+\text{stime}\) 来自 `/proc/<pid>/stat`，本身覆盖线程组全部线程，
 *     因此进程 CPU 不需要逐 TID 求和（与上下文切换不同）。
 *   · 角色分离：publisher / subscriber / daemon(服务进程，如 RouDi、CycloneDDS
 *     服务) / orchestrator(编排) / other，各自单独一行 + `total` 一行。
 *   · 若本次实验声明了守护进程但实际没采到（进程不存在/未启动），
 *     `daemon_cpu_seconds` 必须是 null 且 verdict 标注"服务成本未计入 ⇒ 未确认"，
 *     ⛔ 不允许静默按 0 计入。
 */
#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fcntl.h>
#include <fstream>
#include <map>
#include <signal.h>
#include <sstream>
#include <string>
#include <sys/resource.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#include <vector>

#include "dzIPC/measure/counters.h"

namespace dzIPC {
namespace measure {

/* 时钟节拍（USER_HZ）。同一进程内缓存。 */
inline long clock_ticks_per_sec() noexcept
{
    static const long v = ::sysconf(_SC_CLK_TCK);
    return v > 0 ? v : 100;
}

inline std::string read_text_file(const std::string& path, bool* ok = nullptr)
{
    std::ifstream f(path, std::ios::in | std::ios::binary);
    if (!f) { if (ok) *ok = false; return std::string(); }
    std::ostringstream ss;
    ss << f.rdbuf();
    if (ok) *ok = true;
    return ss.str();
}

inline bool read_uint_file(const std::string& path, std::uint64_t* out) noexcept
{
    std::ifstream f(path);
    if (!f) return false;
    long long v = -1;
    f >> v;
    if (v < 0) return false;
    if (out) *out = static_cast<std::uint64_t>(v);
    return true;
}

/* /proc/uptime 秒数：与 /proc/<pid>/task/<tid>/stat 的 starttime 同一时基
 * (CLOCK_BOOTTIME 的 tick 表示)。用于判定线程是否"窗口开始后才出生"。 */
inline double proc_uptime_seconds() noexcept
{
    std::ifstream f("/proc/uptime");
    double up = -1.0;
    if (f) f >> up;
    return up;
}

inline std::uint64_t parse_u64(const char* s) noexcept
{
    return static_cast<std::uint64_t>(std::strtoull(s, nullptr, 10));
}

/* ---------------- /proc/<pid>/stat ---------------- */

struct ProcessStat
{
    bool          valid{false};
    int           pid{-1};
    std::uint64_t starttime_ticks{0};   ///< 字段 22，防 PID 复用
    std::uint64_t utime_ticks{0};       ///< 字段 14（线程组合计）
    std::uint64_t stime_ticks{0};       ///< 字段 15（线程组合计）
    std::uint64_t cutime_ticks{0};      ///< 字段 16（已回收子进程）
    std::uint64_t cstime_ticks{0};      ///< 字段 17
    std::uint64_t num_threads{0};       ///< 字段 20
    std::uint64_t rss_pages{0};         ///< 字段 24
    std::uint64_t vsize_bytes{0};       ///< 字段 23
    long          priority{0};
    char          state{'?'};
};

/* 解析 /proc/<pid>/stat。comm 可能含空格与右括号，因此从最后一个 ')' 之后切。 */
inline ProcessStat read_process_stat(int pid)
{
    ProcessStat st;
    const std::string raw = read_text_file("/proc/" + std::to_string(pid) + "/stat");
    if (raw.empty()) return st;
    const std::size_t close = raw.rfind(')');
    if (close == std::string::npos) return st;
    const std::size_t open = raw.find('(');
    if (open != std::string::npos && open + 1 < close) {
        /* 不做长名截断：只保留用于诊断的短名。 */
    }
    st.pid = pid;
    std::istringstream is(raw.substr(close + 1));
    std::string tok;
    /* token[k] 对应 man proc 的字段 k+3。 */
    for (int k = 0; is >> tok; ++k) {
        switch (k) {
        case 0: st.state = tok.empty() ? '?' : tok[0]; break;
        case 11: st.utime_ticks = parse_u64(tok.c_str()); break;
        case 12: st.stime_ticks = parse_u64(tok.c_str()); break;
        case 13: st.cutime_ticks = parse_u64(tok.c_str()); break;
        case 14: st.cstime_ticks = parse_u64(tok.c_str()); break;
        case 15: st.priority = std::strtol(tok.c_str(), nullptr, 10); break;
        case 17: st.num_threads = parse_u64(tok.c_str()); break;
        case 19: st.starttime_ticks = parse_u64(tok.c_str()); break;
        case 20: st.vsize_bytes = parse_u64(tok.c_str()); break;
        case 21: st.rss_pages = parse_u64(tok.c_str()); break;
        default: break;
        }
    }
    st.valid = true;
    return st;
}

/* ---------------- /proc/<pid>/task/<tid>/status ---------------- */

struct ThreadStatus
{
    bool          valid{false};
    std::uint64_t voluntary{0};
    std::uint64_t nonvoluntary{0};
};

/* 单 TID 的切换计数。⚠️ 这是==单线程==口径，不是进程合计。
 * 注意标签长度："voluntary_ctxt_switches:" = 24 字符, "nonvoluntary_ctxt_switches:" = 27。
 * 用 rfind(prefix,0)==0 做前缀判定（不要用 compare(0,N,literal) —— 子串长度与字面量
 * 长度不等时 compare 会因长度差异判为不相等，这是本项目踩过的实际 bug）。 */
inline constexpr std::size_t kVoluntaryLabelLen = 24;
inline constexpr std::size_t kNonvoluntaryLabelLen = 27;

inline ThreadStatus read_thread_status(int pid, int tid)
{
    ThreadStatus s;
    std::ifstream f("/proc/" + std::to_string(pid) + "/task/" + std::to_string(tid) + "/status");
    if (!f) return s;
    std::string line;
    while (std::getline(f, line)) {
        if (line.rfind("voluntary_ctxt_switches:", 0) == 0)
            s.voluntary = parse_u64(line.c_str() + kVoluntaryLabelLen);
        else if (line.rfind("nonvoluntary_ctxt_switches:", 0) == 0)
            s.nonvoluntary = parse_u64(line.c_str() + kNonvoluntaryLabelLen);
    }
    s.valid = true;
    return s;
}

/* 单 TID 的 CPU tick 合计（utime+stime，字段 14+15）。
 * 用于 §4 W03 第 2 条的 CPU 口径核验：/proc/<pid>/stat 的 utime+stime 是否等于
 * 逐 TID 之和（W01 UF-06 的 C≈D 检查，本文件把它做成每次运行都出的机器判据）。 */
inline std::uint64_t read_thread_cpu_ticks(int pid, int tid)
{
    const std::string raw = read_text_file("/proc/" + std::to_string(pid) + "/task/"
                                          + std::to_string(tid) + "/stat");
    if (raw.empty()) return 0;
    const std::size_t close = raw.rfind(')');
    if (close == std::string::npos) return 0;
    std::istringstream is(raw.substr(close + 1));
    std::string tok;
    std::uint64_t utime = 0, stime = 0;
    for (int k = 0; is >> tok; ++k) {
        if (k == 11) utime = parse_u64(tok.c_str());
        else if (k == 12) stime = parse_u64(tok.c_str());
    }
    return utime + stime;
}

/* 单 TID 生命周期起点的 tick 表示（/proc/<pid>/task/<tid>/stat 字段 22）。 */
inline std::uint64_t read_thread_starttime_ticks(int pid, int tid)
{
    const std::string raw = read_text_file("/proc/" + std::to_string(pid) + "/task/"
                                          + std::to_string(tid) + "/stat");
    if (raw.empty()) return 0;
    const std::size_t close = raw.rfind(')');
    if (close == std::string::npos) return 0;
    std::istringstream is(raw.substr(close + 1));
    std::string tok;
    for (int k = 0; is >> tok; ++k) {
        if (k == 19) return parse_u64(tok.c_str());
    }
    return 0;
}

inline std::vector<int> list_tids(int pid)
{
    std::vector<int> tids;
    DIR* d = ::opendir(("/proc/" + std::to_string(pid) + "/task").c_str());
    if (!d) return tids;
    while (dirent* e = ::readdir(d)) {
        if (e->d_name[0] == '.') continue;
        tids.push_back(static_cast<int>(std::strtol(e->d_name, nullptr, 10)));
    }
    ::closedir(d);
    std::sort(tids.begin(), tids.end());
    return tids;
}

/* 进程打开 fd 数与最大 fd（§4 W07 的 fd 容量观测）。 */
struct FdInfo
{
    bool valid{false};
    std::size_t count{0};
    long max_fd{-1};
};

inline FdInfo read_fd_info(int pid)
{
    FdInfo info;
    DIR* d = ::opendir(("/proc/" + std::to_string(pid) + "/fd").c_str());
    if (!d) return info;
    while (dirent* e = ::readdir(d)) {
        if (e->d_name[0] == '.') continue;
        ++info.count;
        const long v = std::strtol(e->d_name, nullptr, 10);
        if (v > info.max_fd) info.max_fd = v;
    }
    ::closedir(d);
    info.valid = true;
    return info;
}

/* ---------------- 角色 ---------------- */

enum class ProcessRole
{
    unknown = 0,
    publisher,
    subscriber,
    daemon,        ///< 服务进程（RouDi / CycloneDDS 服务等）
    orchestrator,  ///< 基准编排进程
    other
};

inline const char* process_role_name(ProcessRole r) noexcept
{
    switch (r) {
    case ProcessRole::publisher:    return "publisher";
    case ProcessRole::subscriber:   return "subscriber";
    case ProcessRole::daemon:       return "daemon";
    case ProcessRole::orchestrator: return "orchestrator";
    case ProcessRole::other:        return "other";
    case ProcessRole::unknown:      break;
    }
    return "unknown";
}

inline std::vector<int> find_pids_by_comm(const std::string& comm)
{
    std::vector<int> pids;
    DIR* d = ::opendir("/proc");
    if (!d) return pids;
    while (dirent* e = ::readdir(d)) {
        if (e->d_name[0] < '0' || e->d_name[0] > '9') continue;
        const int pid = static_cast<int>(std::strtol(e->d_name, nullptr, 10));
        bool ok = false;
        std::string name = read_text_file("/proc/" + std::to_string(pid) + "/comm", &ok);
        if (!ok) continue;
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r')) name.pop_back();
        if (name == comm) pids.push_back(pid);
    }
    ::closedir(d);
    std::sort(pids.begin(), pids.end());
    return pids;
}

inline std::string process_cmdline(int pid, std::size_t limit = 256)
{
    bool ok = false;
    std::string s = read_text_file("/proc/" + std::to_string(pid) + "/cmdline", &ok);
    if (!ok) return std::string();
    for (char& c : s) if (c == '\0') c = ' ';
    while (!s.empty() && s.back() == ' ') s.pop_back();
    if (s.size() > limit) s.resize(limit);
    return s;
}

/* ---------------- 线程组上下文切换：全生命周期语义 ---------------- */

struct ThreadGroupCtxSwitch
{
    /* 覆盖窗口的上下文切换合计（逐 TID 增量求和；rusage 模式为全生命周期精确值）。 */
    std::uint64_t voluntary{0};
    std::uint64_t nonvoluntary{0};

    /* 采样模式下的完整性证据 */
    std::uint64_t tids_seen{0};                 ///< 窗口内出现过的 TID 数
    std::uint64_t tids_present_first_sample{0}; ///< 窗口开始时已在册的 TID 数
    std::uint64_t tids_born_mid_window{0};      ///< 窗口开始后才创建的 TID 数
    std::uint64_t tids_exited{0};               ///< 窗口内退出的 TID 数
    std::uint64_t tids_missed_at_first_sample{0}; ///< 存在但首采样未捕获（竞态）
    std::uint64_t tids_starttime_boundary{0};   ///< starttime 恰等于窗口起点 tick（±1 tick 有界）
    std::uint64_t tids_starttime_unknown{0};    ///< starttime 不可读，按新生处理
    std::uint64_t voluntary_undercount_bound{0};   ///< 退出线程最后区间丢失上界
    std::uint64_t nonvoluntary_undercount_bound{0};
    std::uint64_t timer_tick_anomalies{0};      ///< 计数回退（异常）次数
    std::size_t   samples{0};
    bool          complete{false};              ///< 是否可声称覆盖整个生命周期
    bool          pid_reused{false};            ///< (pid, starttime) 身份变化
    bool          main_thread_only_reference_valid{false};
    std::uint64_t main_thread_voluntary_reference{0};    ///< ⚠️ 仅主线程，不是合计
    std::uint64_t main_thread_nonvoluntary_reference{0}; ///< ⚠️ 仅主线程，不是合计
    double        window_s{0.0};
    std::string   method{"none"};               ///< "rusage" | "proc_task_sample"
    std::string   note;
};

/* CPU 聚合语义核验的一次合格采样。
 * ⚠️ 为什么必须"无线程退出"时才取：`/proc/<pid>/stat` 走 `whole=1`
 * (`thread_group_cputime_adjusted`)，它把**已退出线程**的 CPU 也累计进线程组总量；
 * 而逐 TID 求和只看得到**当前存活**的 TID。窗口内一旦有线程退出，两者就不再是同一
 * 口径（求和偏小），据此算比值会得到假结论。因此只在 `tids_exited == 0` 的采样点取值，
 * 并保留最后一次合格采样。内核依据见
 * docs/消息接收架构改造/团队改造交付/W03/外部来源存证/proc_array_v6.8_do_task_stat.md。 */
struct CpuScopeSample
{
    bool          valid{false};
    std::uint64_t proc_stat_ticks{0};      ///< /proc/<pid>/stat 字段 14+15 (whole=1)
    std::uint64_t task_group_sum_ticks{0}; ///< 逐 TID /proc/<pid>/task/<tid>/stat 14+15 之和
    std::size_t   tids{0};
    std::size_t   sample_index{0};
    double        ratio{0.0};

    const char* judgement() const noexcept
    {
        if (!valid) return "无合格采样点(窗口内已有线程退出或采样不足) => 未确认";
        if (ratio > 0.9 && ratio < 1.1)
            return "一致 => /proc/<pid>/stat 为线程组累计(与 kernel do_task_stat whole=1 及 W01 C~=D 一致)";
        if (ratio < 0.9)
            return "逐 TID 之和偏小: 线程若以短于一个 tick(1/CLK_TCK=10ms) 的片段运行, "
                   "tick 记账不会把该片段计入其 utime/stime, 而线程组口径的 cputime_adjust 会用 "
                   "sum_exec_runtime 补回。=> CPU 结论一律以 /proc/<pid>/stat 或 rusage 为准, "
                   "逐 TID 之和只作长片段线程的交叉检查; 不是解析错误";
        return "逐 TID 之和偏大 => 需检查是否重复计数(同一 TID 被计两次)";
    }
};

/* 逐 TID 采样器（模式 B）。 */
class ProcessSampler
{
public:
    struct TidState
    {
        std::uint64_t last_vol{0};
        std::uint64_t last_nonvol{0};
        bool          born_mid_window{false};
        bool          seeded{false};
    };

    explicit ProcessSampler(int pid) : pid_(pid) {}

    /* 记录身份 + 取窗口起点。返回 false = 进程不存在。 */
    bool attach()
    {
        ps_ = read_process_stat(pid_);
        if (!ps_.valid) return false;
        const double up = proc_uptime_seconds();
        hz_ = clock_ticks_per_sec();
        window_start_ticks_ = up > 0.0
            ? static_cast<std::uint64_t>(up * static_cast<double>(hz_)) : 0;
        window_start_ns_ = monotonic_now_ns();
        attached_ = true;
        return true;
    }

    /* 采集一次；返回 false = 进程已消失（此后调用方应走 finish()）。 */
    bool sample()
    {
        if (!attached_) return false;
        ProcessStat now = read_process_stat(pid_);
        if (!now.valid) { alive_ = false; return false; }
        if (now.starttime_ticks != ps_.starttime_ticks) {
            ctx_.pid_reused = true;
            alive_ = false;
            return false;
        }
        ps_ = now;
        last_fd_ = read_fd_info(pid_);
        last_threads_ = list_tids(pid_);

        ThreadStatus lead = read_thread_status(pid_, pid_);
        if (lead.valid && pid_ > 0) {
            /* 主线程(= 线程组主线程)参考值：仅用于证伪 /proc/<pid>/status 口径。 */
            if (!main_ref_seeded_) {
                main_ref_vol_stop_ = lead.voluntary;
                main_ref_nonvol_stop_ = lead.nonvoluntary;
                main_ref_seeded_ = true;
            }
            main_ref_vol_cur_ = lead.voluntary;
            main_ref_nonvol_cur_ = lead.nonvoluntary;
        }

        for (int tid : last_threads_) {
            ThreadStatus ts = read_thread_status(pid_, tid);
            if (!ts.valid) continue;
            auto it = tids_.find(tid);
            if (it == tids_.end()) {
                TidState st;
                const std::uint64_t start_ticks = read_thread_starttime_ticks(pid_, tid);
                /* 窗口开始后才创建的线程：它全部生命周期切换都在窗口内 ⇒ 从 0 累计。
                 * 判据用 starttime >= 窗口起点 tick（不是 >）：CLOCK_BOOTTIME 的 tick 粒度
                 * 是 1/HZ（本机 10 ms），窗口起点与线程创建落在同一 tick 时用 > 会漏判，
                 * 把"窗口内新生"错当成"窗口前已在册"而丢掉该线程的全部切换。代价是最坏
                 * 把窗口开始前不到一个 tick 的切换算进来 —— 有界（<10 ms）且远小于采样间隔，
                 * 但必须登记：tids_starttime_boundary 记录落在同一 tick 的 TID 数。
                 * starttime 读不到（0）时同样按"新生"处理并登记 tids_starttime_unknown，
                 * 因为按"已在册"处理会静默少算，按"新生"处理最坏只是有界多算。 */
                const bool starttime_known = (start_ticks != 0 && window_start_ticks_ != 0);
                bool born = true;
                if (starttime_known) {
                    born = (start_ticks >= window_start_ticks_);
                    if (start_ticks == window_start_ticks_) ++ctx_.tids_starttime_boundary;
                } else {
                    ++ctx_.tids_starttime_unknown;
                }
                st.born_mid_window = born;
                if (born) {
                    st.last_vol = 0;
                    st.last_nonvol = 0;
                    ++ctx_.tids_born_mid_window;
                } else {
                    st.last_vol = ts.voluntary;
                    st.last_nonvol = ts.nonvoluntary;
                    if (samples_ > 0) ++ctx_.tids_missed_at_first_sample;
                }
                st.seeded = true;
                it = tids_.emplace(tid, st).first;
                ++ctx_.tids_seen;
            }
            TidState& st = it->second;
            if (ts.voluntary < st.last_vol || ts.nonvoluntary < st.last_nonvol) {
                ++ctx_.timer_tick_anomalies;
            } else {
                const std::uint64_t dv = ts.voluntary - st.last_vol;
                const std::uint64_t dn = ts.nonvoluntary - st.last_nonvol;
                ctx_.voluntary += dv;
                ctx_.nonvoluntary += dn;
                per_interval_vol_max_ = std::max(per_interval_vol_max_, dv);
                per_interval_nonvol_max_ = std::max(per_interval_nonvol_max_, dn);
            }
            st.last_vol = ts.voluntary;
            st.last_nonvol = ts.nonvoluntary;
        }

        /* 上一轮在册、本轮消失的 TID：完整窗口内退出。最后一个区间读不到 ⇒ 计入上界。 */
        for (auto it = tids_.begin(); it != tids_.end();) {
            const int tid = it->first;
            if (std::find(last_threads_.begin(), last_threads_.end(), tid) == last_threads_.end()) {
                ++ctx_.tids_exited;
                ctx_.voluntary_undercount_bound += per_interval_vol_max_;
                ctx_.nonvoluntary_undercount_bound += per_interval_nonvol_max_;
                it = tids_.erase(it);
            } else {
                ++it;
            }
        }

        /* CPU 口径核验：逐 TID 求和，与 /proc/<pid>/stat 的 utime+stime 对照。
         * 两者接近 ⇒ /proc/<pid>/stat 是线程组累计（与 W01 的 C≈D 一致）。
         * ⚠️ 内核实现层来源未归档（W01 UF-06）⇒ 该结论标注为"实测支持, 机制未确认"。 */
        task_group_cpu_ticks_ = 0;
        for (int tid : last_threads_) task_group_cpu_ticks_ += read_thread_cpu_ticks(pid_, tid);
        if (ctx_.tids_exited == 0 && !last_threads_.empty()) {
            cpu_scope_.valid = true;
            cpu_scope_.proc_stat_ticks = ps_.utime_ticks + ps_.stime_ticks;
            cpu_scope_.task_group_sum_ticks = task_group_cpu_ticks_;
            cpu_scope_.tids = last_threads_.size();
            cpu_scope_.sample_index = samples_;
            cpu_scope_.ratio = cpu_scope_.proc_stat_ticks > 0
                ? static_cast<double>(cpu_scope_.task_group_sum_ticks)
                  / static_cast<double>(cpu_scope_.proc_stat_ticks)
                : 0.0;
        }

        ++samples_;
        ++ctx_.samples;
        last_utime_ = ps_.utime_ticks;
        last_stime_ = ps_.stime_ticks;
        return true;
    }

    /* 结束采样：结算完整性、窗口时长与 CPU 增量。 */
    ThreadGroupCtxSwitch finish()
    {
        ctx_.method = "proc_task_sample";
        ctx_.samples = samples_;
        ctx_.window_s = static_cast<double>(monotonic_now_ns() - window_start_ns_) / 1e9;
        ctx_.tids_present_first_sample =
            ctx_.tids_seen - ctx_.tids_born_mid_window - ctx_.tids_missed_at_first_sample;
        ctx_.main_thread_only_reference_valid = main_ref_seeded_;
        ctx_.main_thread_voluntary_reference =
            main_ref_vol_cur_ >= main_ref_vol_stop_ ? main_ref_vol_cur_ - main_ref_vol_stop_ : 0;
        ctx_.main_thread_nonvoluntary_reference =
            main_ref_nonvol_cur_ >= main_ref_nonvol_stop_ ? main_ref_nonvol_cur_ - main_ref_nonvol_stop_ : 0;
        /* 完整性判据：无退出线程（无最后区间损失）、有 >=2 个采样点、无计数异常、
         * 身份未复用。任何一条不满足都只能报"不完整"并给出下界/上界。 */
        ctx_.complete = !ctx_.pid_reused && samples_ >= 2
                     && ctx_.tids_exited == 0 && ctx_.timer_tick_anomalies == 0;
        if (!ctx_.complete) {
            ctx_.note = ctx_.pid_reused ? "pid identity changed (pid reuse)"
                      : (samples_ < 2 ? "fewer than 2 samples"
                      : (ctx_.tids_exited > 0
                         ? "threads exited inside the window: last partial interval is unobservable; value is a lower bound"
                         : "timer tick counter went backwards"));
        } else {
            ctx_.note = "per-TID accumulation complete over the window";
        }
        return ctx_;
    }

    /* 首采样时在册的 TID 数（= tids_seen - born_mid_window - missed）。 */
    void note_first_sample_tids()
    {
        ctx_.tids_present_first_sample =
            ctx_.tids_seen - ctx_.tids_born_mid_window - ctx_.tids_missed_at_first_sample;
    }

    int pid() const noexcept { return pid_; }
    bool alive() const noexcept { return alive_; }
    const ProcessStat& stat() const noexcept { return ps_; }
    const ThreadGroupCtxSwitch& ctx() const noexcept { return ctx_; }
    const std::vector<int>& threads() const noexcept { return last_threads_; }
    const FdInfo& fd() const noexcept { return last_fd_; }
    /* CPU 聚合语义核验（只在无线程退出的采样点有效）。 */
    const CpuScopeSample& cpu_scope() const noexcept { return cpu_scope_; }
    /* 逐 TID CPU 之和（最近一次采样）。 */
    std::uint64_t task_group_cpu_ticks() const noexcept { return task_group_cpu_ticks_; }
    /* /proc/<pid>/stat 的 utime+stime（线程组累计口径的候选值）。 */
    std::uint64_t process_stat_cpu_ticks() const noexcept
    { return ps_.utime_ticks + ps_.stime_ticks; }
    std::uint64_t utime_ticks() const noexcept { return last_utime_; }
    std::uint64_t stime_ticks() const noexcept { return last_stime_; }
    bool pid_reused() const noexcept { return ctx_.pid_reused; }

private:
    int           pid_{-1};
    bool          attached_{false};
    bool          alive_{true};
    long          hz_{100};
    std::uint64_t window_start_ticks_{0};
    std::uint64_t window_start_ns_{0};
    ProcessStat   ps_{};
    std::map<int, TidState> tids_{};
    std::vector<int> last_threads_{};
    FdInfo        last_fd_{};
    std::size_t   samples_{0};
    std::uint64_t per_interval_vol_max_{0};
    std::uint64_t per_interval_nonvol_max_{0};
    bool          main_ref_seeded_{false};
    std::uint64_t main_ref_vol_stop_{0};
    std::uint64_t main_ref_vol_cur_{0};
    std::uint64_t main_ref_nonvol_stop_{0};
    std::uint64_t main_ref_nonvol_cur_{0};
    std::uint64_t last_utime_{0};
    std::uint64_t last_stime_{0};
    std::uint64_t task_group_cpu_ticks_{0};
    CpuScopeSample cpu_scope_{};
    ThreadGroupCtxSwitch ctx_{};
};

/* ---------------- rusage 全生命周期模式（模式 A） ---------------- */

struct ChildRusage
{
    bool          valid{false};
    int           pid{-1};
    std::string   label;
    std::string   command;
    int           exit_code{0};
    bool          signaled{false};
    int           signal{0};
    double        user_s{0.0};
    double        sys_s{0.0};
    double        total_cpu_s{0.0};
    std::uint64_t voluntary{0};        ///< ru_nvcsw  —— 整个线程组生命周期
    std::uint64_t nonvoluntary{0};     ///< ru_nivcsw —— 整个线程组生命周期
    long          maxrss_kb{0};
    long          minflt{0};
    long          majflt{0};
    bool          killed_by_collector{false};  ///< 采集器因超时 SIGKILL
    bool          complete{true};      ///< rusage 口径恒为完整（覆盖整个线程组生命周期）
    std::string   method{"rusage"};
    std::string   note;
};

/* 拉起一个子进程并在退出时取 rusage。argv[0] 用 PATH 解析（execvp）。
 * stdout_path / stderr_path 非空时把子进程输出重定向到文件（相位标记需要子进程
 * stdout；同时避免被测进程输出污染采集器输出）。 */
class ChildProcessRunner
{
public:
    bool spawn(const std::vector<std::string>& argv, const std::string& label,
               const std::string& stdout_path = std::string(),
               const std::string& stderr_path = std::string())
    {
        if (argv.empty()) return false;
        label_ = label;
        command_ = join(argv);
        pid_ = ::fork();
        if (pid_ < 0) { pid_ = -1; return false; }
        if (pid_ == 0) {
            if (!stdout_path.empty()) {
                ::fflush(stdout);
                const int fd = ::open(stdout_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd >= 0) { ::dup2(fd, STDOUT_FILENO); ::close(fd); }
            }
            if (!stderr_path.empty()) {
                ::fflush(stderr);
                const int fd = ::open(stderr_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
                if (fd >= 0) { ::dup2(fd, STDERR_FILENO); ::close(fd); }
            }
            std::vector<char*> cargv;
            cargv.reserve(argv.size() + 1);
            for (const std::string& a : argv) cargv.push_back(const_cast<char*>(a.c_str()));
            cargv.push_back(nullptr);
            ::execvp(cargv[0], cargv.data());
            ::_exit(127);
        }
        return true;
    }

    /* 杀掉子进程（SIGKILL）。用于超时收尾，避免 wait() 永久阻塞。 */
    void kill() noexcept
    {
        if (pid_ > 0) ::kill(pid_, SIGKILL);
    }

    /* 阻塞等待并取 rusage（RU_OK）。必须在 spawn() 成功之后调用一次。 */
    ChildRusage wait()
    {
        ChildRusage r;
        r.label = label_;
        r.command = command_;
        if (pid_ <= 0) { r.note = "no child"; return r; }
        int status = 0;
        struct rusage ru {};
        const pid_t got = ::wait4(pid_, &status, 0, &ru);
        if (got != pid_) { r.note = "wait4 failed"; pid_ = -1; return r; }
        r.pid = static_cast<int>(got);
        r.valid = true;
        if (WIFEXITED(status)) { r.exit_code = WEXITSTATUS(status); }
        else if (WIFSIGNALED(status)) {
            r.signaled = true;
            r.signal = WTERMSIG(status);
            r.exit_code = 128 + r.signal;
            if (r.signal == SIGKILL) r.killed_by_collector = true;
        }
        r.user_s = static_cast<double>(ru.ru_utime.tv_sec) + static_cast<double>(ru.ru_utime.tv_usec) / 1e6;
        r.sys_s  = static_cast<double>(ru.ru_stime.tv_sec) + static_cast<double>(ru.ru_stime.tv_usec) / 1e6;
        r.total_cpu_s = r.user_s + r.sys_s;
        r.voluntary = static_cast<std::uint64_t>(ru.ru_nvcsw);
        r.nonvoluntary = static_cast<std::uint64_t>(ru.ru_nivcsw);
        r.maxrss_kb = ru.ru_maxrss;
        r.minflt = ru.ru_minflt;
        r.majflt = ru.ru_majflt;
        r.note = "whole-lifetime rusage: ritme/nvcsw/nivcsw cover the entire thread group";
        pid_ = -1;
        return r;
    }

    int pid() const noexcept { return pid_; }

private:
    static std::string join(const std::vector<std::string>& v)
    {
        std::string s;
        for (std::size_t i = 0; i < v.size(); ++i) { if (i) s += ' '; s += v[i]; }
        return s;
    }
    int         pid_{-1};
    std::string label_;
    std::string command_;
};

/* ---------------- 角色化 CPU 汇总 ---------------- */

struct ProcessCpuRow
{
    ProcessRole   role{ProcessRole::unknown};
    int           pid{-1};
    std::string   label;
    std::string   cmdline;
    double        user_s{0.0};
    double        sys_s{0.0};
    double        total_cpu_s{0.0};
    double        cpu_cores{0.0};       ///< total_cpu_s / window_s
    std::uint64_t voluntary{0};
    std::uint64_t nonvoluntary{0};
    std::uint64_t thread_count{0};
    std::uint64_t thread_count_max{0};
    long          max_fd{-1};
    bool          ctx_complete{false};
    std::string   ctx_method{"none"};
    std::string   note;
};

/* 一次采样的原始读数，供采集器按相位聚合。 */
struct ProcessSampleRow
{
    std::uint64_t monotonic_ns{0};
    double        offset_s{0.0};
    int           pid{-1};
    ProcessRole   role{ProcessRole::unknown};
    std::string   label;
    std::uint64_t threads{0};
    double        cpu_cores{0.0};
    std::uint64_t rss_kb{0};
    long          max_fd{-1};
    char          state{'?'};
};

/* ---------------- 计数开销对照 ---------------- */

inline CounterOverhead measure_counter_overhead(std::uint64_t iterations = 200000)
{
    CounterOverhead o;
    o.iterations = iterations;
    if (iterations == 0) return o;
    volatile std::uint64_t sink = 0;

    CounterRegistry& reg = CounterRegistry::instance();
    reg.set_diagnostics_enabled(false);

    /* 单例访问形态 */
    std::uint64_t a = monotonic_now_ns();
    for (std::uint64_t i = 0; i < iterations; ++i) reg.inc(CounterId::scanned_routes_total, 1u);
    std::uint64_t b = monotonic_now_ns();
    o.counter_inc_ns = static_cast<double>(b - a) / static_cast<double>(iterations);

    /* 已持引用形态（模块热路径实际写法：一次 fetch_add） */
    std::atomic<std::uint64_t> local{0};
    a = monotonic_now_ns();
    for (std::uint64_t i = 0; i < iterations; ++i)
        local.fetch_add(1u, std::memory_order_relaxed);
    b = monotonic_now_ns();
    o.counter_inc_cached_ns = static_cast<double>(b - a) / static_cast<double>(iterations);

    /* 诊断宏：关闭 / 开启 */
    reg.set_diagnostics_enabled(false);
    a = monotonic_now_ns();
    for (std::uint64_t i = 0; i < iterations; ++i) {
        if (reg.diagnostics_enabled()) reg.inc(CounterId::scan_rounds, 1u);
    }
    b = monotonic_now_ns();
    o.diag_inc_disabled_ns = static_cast<double>(b - a) / static_cast<double>(iterations);

    reg.set_diagnostics_enabled(true);
    a = monotonic_now_ns();
    for (std::uint64_t i = 0; i < iterations; ++i) reg.inc(CounterId::scan_rounds, 1u);
    b = monotonic_now_ns();
    o.diag_inc_enabled_ns = static_cast<double>(b - a) / static_cast<double>(iterations);

    /* ScanRoundScope 三档（各 1/4 迭代量；**夫妻对拍**以抵消频率漂移：
     * 先测 A 再测 B，随后**重复一轮**并取两轮的最小值作为各档读数）。
     *   A) 构造 → 析构（**不调 finish**）—— 历史口径，可与旧读数（0.176 / 29.3 ns）比较
     *   B) 构造 → finish() → set_ready() → 析构 —— **当前接线形态**（W06/t42 的写法）
     *   C) = B − A ⇒ 只调 finish 的增量（诊断开启时含 1 次 clock_gettime）
     * ⚠️ 口径变更必须登记（W03/t44）：A 与 B 不可混用同一张对照表。 */
    const std::uint64_t half = std::max<std::uint64_t>(1u, iterations / 4u);

    /* 两轮交错测量，各档取两轮最小值（最小值对频率下漂更稳健）。 */
    for (int rep = 0; rep < 2; ++rep)
    {
        reg.set_diagnostics_enabled(true);
        a = monotonic_now_ns();
        for (std::uint64_t i = 0; i < half; ++i) { ScanRoundScope s(100u, 3u); s.set_ready(false); }
        b = monotonic_now_ns();
        o.scan_scope_enabled_no_finish_ns = std::min(
            o.scan_scope_enabled_no_finish_ns == 0.0 ? 1e18 : o.scan_scope_enabled_no_finish_ns,
            static_cast<double>(b - a) / static_cast<double>(half));

        a = monotonic_now_ns();
        for (std::uint64_t i = 0; i < half; ++i)
        {
            ScanRoundScope s(100u, 3u);
            s.finish(1u, 3u);
            s.set_ready(false);
        }
        b = monotonic_now_ns();
        o.scan_scope_enabled_ns = std::min(
            o.scan_scope_enabled_ns == 0.0 ? 1e18 : o.scan_scope_enabled_ns,
            static_cast<double>(b - a) / static_cast<double>(half));

        reg.set_diagnostics_enabled(false);
        a = monotonic_now_ns();
        for (std::uint64_t i = 0; i < half; ++i) { ScanRoundScope s(100u, 3u); s.set_ready(false); }
        b = monotonic_now_ns();
        o.scan_scope_disabled_no_finish_ns = std::min(
            o.scan_scope_disabled_no_finish_ns == 0.0 ? 1e18 : o.scan_scope_disabled_no_finish_ns,
            static_cast<double>(b - a) / static_cast<double>(half));

        a = monotonic_now_ns();
        for (std::uint64_t i = 0; i < half; ++i)
        {
            ScanRoundScope s(100u, 3u);
            s.finish(1u, 3u);
            s.set_ready(false);
        }
        b = monotonic_now_ns();
        o.scan_scope_disabled_ns = std::min(
            o.scan_scope_disabled_ns == 0.0 ? 1e18 : o.scan_scope_disabled_ns,
            static_cast<double>(b - a) / static_cast<double>(half));
    }
    reg.set_diagnostics_enabled(false);

    /* 时钟成本（已在 clock.h 里测，这里给热路径单次参考） */
    a = monotonic_now_ns();
    for (std::uint64_t i = 0; i < iterations; ++i) sink = sink ^ monotonic_now_ns();
    b = monotonic_now_ns();
    o.clock_now_ns = static_cast<double>(b - a) / static_cast<double>(iterations);

    /* /proc 读取成本（采样模式的基本代价） */
    const std::uint64_t few = std::max<std::uint64_t>(1u, iterations / 50u);
    const int self = static_cast<int>(::getpid());
    a = monotonic_now_ns();
    for (std::uint64_t i = 0; i < few; ++i) {
        const ProcessStat st = read_process_stat(self);
        sink = sink ^ st.utime_ticks;
    }
    b = monotonic_now_ns();
    o.proc_self_stat_read_ns = static_cast<double>(b - a) / static_cast<double>(few);

    a = monotonic_now_ns();
    for (std::uint64_t i = 0; i < few; ++i) {
        for (int tid : list_tids(self)) {
            const ThreadStatus ts = read_thread_status(self, tid);
            sink = sink ^ ts.voluntary;
        }
    }
    b = monotonic_now_ns();
    o.proc_task_status_all_ns = static_cast<double>(b - a) / static_cast<double>(few);
    (void)sink;
    return o;
}

}   // namespace measure
}   // namespace dzIPC

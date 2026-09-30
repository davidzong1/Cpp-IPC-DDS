#pragma once
/* W03 测量口径 · 三种「空闲」状态观测与相位统计
 * ============================================================================
 * 交付依据：团队改造方案 §10.1 与 §4 W03 第 5、6 条。
 *
 * §10.1 明确要求分别测试三种"空闲"，并且**不能**把它们混成一个 CPU 数字：
 *   | 状态 | 预期行为 | 必须记录 |
 *   | 没有注册 route | keep-alive 到期后可退出，下次注册可重新拉起 | 在册 route 数、线程退出/拉起计数 |
 *   | 已注册但无发布者或尚未连接 | 按模块生命周期契约决定是否已加入等待层 | 应用对象数、控制项数、worker route 数 |
 *   | 已连接、有效订阅但停止发布 | 保留接收能力；后续恢复发布不依赖重新注册 | 静默前后 route 数、恢复后首包延迟、是否丢包 |
 * 还要有强制用例："连接成功 → 收到确认消息 → 静默超过 keep-alive → 原发布者恢复发送"。
 * ⛔ 「不能把未接入等待层时的少线程/低 CPU 用作千个有效订阅的空闲成绩」——
 *    因此 `wait_layer_attached == false` 且 `registered_routes > 0` 的组合被判为
 *    `unconfirmed`（不计入空闲成绩），而不是"很省"。
 *
 * §4 W03 第 6 条要求：空闲观测覆盖**周期任务**（控制面 tick / heartbeat 这类按固定
 * 周期运行的线程），稳定窗口 >= 60 s，并分别统计启动峰值 / 稳定活跃 / 空闲回落。
 * 本文件把"周期任务"显式建模为 `PeriodicTaskCoverage`：若一次空闲观测没有记录到
 * 周期任务的 tick，则该窗口标 `periodic_tasks_covered = false`，不能声称空闲观测
 * 已覆盖周期任务。
 */
#include <algorithm>
#include <cstdint>
#include <string>
#include <vector>

#include "dzIPC/measure/monotonic_clock.h"

namespace dzIPC {
namespace measure {

/* ------------------------------------------------------------------ 空闲状态 */

enum class IdleState
{
    unconfirmed = 0,          ///< 路径/接入证据不足，不得计入空闲成绩
    no_registered_route,      ///< §10.1 状态 1：没有注册 route
    registered_no_publisher,  ///< §10.1 状态 2：已注册但无发布者/尚未连接
    connected_silent,         ///< §10.1 状态 3：已连接、有效订阅但停止发布
    active_publishing         ///< 非空闲（发送中），仅用于状态机完备性
};

inline const char* idle_state_name(IdleState s) noexcept
{
    switch (s) {
    case IdleState::unconfirmed:             return "unconfirmed";
    case IdleState::no_registered_route:     return "no_registered_route";
    case IdleState::registered_no_publisher: return "registered_no_publisher";
    case IdleState::connected_silent:        return "connected_silent";
    case IdleState::active_publishing:       return "active_publishing";
    }
    return "unknown";
}

inline const char* idle_state_expected_behavior(IdleState s) noexcept
{
    switch (s) {
    case IdleState::unconfirmed:
        return "证据不足: 不得作为空闲成绩";
    case IdleState::no_registered_route:
        return "keep-alive 到期后线程可退出; 下次注册按需拉起";
    case IdleState::registered_no_publisher:
        return "按模块生命周期契约决定是否已加入等待层; 记录对象/控制项/worker route 数";
    case IdleState::connected_silent:
        return "保留接收能力; 恢复发布不依赖重新注册; 记录恢复首包延迟与丢包";
    case IdleState::active_publishing:
        return "非空闲状态, 不计入空闲回落";
    }
    return "";
}

/* 一次空闲判定的输入。数值全部来自可核验的运行时状态，不靠推断。 */
struct IdleInputs
{
    std::size_t   registered_routes{0};      ///< 在册 route 数（应用视角）
    std::size_t   worker_routes{0};          ///< worker 表内 route 数（RecvWorkerPool::route_count）
    std::size_t   application_objects{0};    ///< 应用对象数（句柄/订阅对象）
    std::size_t   control_items{0};          ///< 控制项数（控制面在册条目）
    std::uint64_t thread_exits{0};           ///< 空闲退出累计（RecvWorkerStats::idle_exits）
    std::uint64_t thread_restarts{0};        ///< 按需拉起累计（thread_restarts）
    bool          publisher_connected{false};
    bool          publishing{false};
    bool          wait_layer_attached{false};///< 是否真的加入了等待层
    bool          periodic_tasks_running{false};
};

inline IdleState classify_idle(const IdleInputs& in) noexcept
{
    if (in.publishing) return IdleState::active_publishing;

    /* 注册数 > 0 但 worker 在册数为 0：要么还没接入等待层，要么统计口径不一致，
     * 两种都不能当作"有效订阅的空闲成绩"。 */
    if (in.registered_routes > 0 && !in.wait_layer_attached)
        return IdleState::unconfirmed;
    if (in.registered_routes > 0 && in.worker_routes == 0)
        return IdleState::unconfirmed;

    if (in.registered_routes == 0) {
        /* 没有任何应用对象/控制项时才允许声称"无注册 route"；否则是对象在册但
         * 没进等待层，属未确认。 */
        if (in.application_objects == 0 && in.control_items == 0 && in.worker_routes == 0)
            return IdleState::no_registered_route;
        return IdleState::unconfirmed;
    }
    if (!in.publisher_connected) return IdleState::registered_no_publisher;
    return IdleState::connected_silent;
}

/* 一次"静默 → 恢复"的观测（§10.1 强制用例的机器记录）。 */
struct RecoveryObservation
{
    std::uint64_t silence_ns{0};               ///< 静默持续时长
    std::uint64_t first_packet_latency_ns{0};  ///< 发布者恢复发送 → 订阅端首包到达
    std::uint64_t routed_routes_before{0};     ///< 静默前在册 route 数
    std::uint64_t routed_routes_after{0};      ///< 恢复后在册 route 数
    bool          lost_packets{false};
    bool          reregistration_required{false};
    bool          reconnected{false};
};

/* 恢复首包延迟统计（多轮）。单位 us，输出含分位数。 */
class RecoveryLatencyStats
{
public:
    void add(const RecoveryObservation& r)
    {
        obs_.push_back(r);
        if (r.reregistration_required) ++rereg_count_;
        if (r.lost_packets) ++lost_count_;
        if (!r.reconnected) ++failed_recovery_;
    }

    std::size_t count() const noexcept { return obs_.size(); }
    std::uint64_t rereg_count() const noexcept { return rereg_count_; }
    std::uint64_t lost_count() const noexcept { return lost_count_; }
    std::uint64_t failed_recovery() const noexcept { return failed_recovery_; }

    struct Summary
    {
        std::size_t   count{0};
        double        min_us{0.0};
        double        mean_us{0.0};
        double        p50_us{0.0};
        double        p99_us{0.0};
        double        max_us{0.0};
        std::uint64_t rereg_count{0};
        std::uint64_t lost_count{0};
        std::uint64_t failed_recovery{0};
    };

    Summary summary() const
    {
        Summary s;
        s.count = obs_.size();
        if (obs_.empty()) return s;
        std::vector<double> v;
        v.reserve(obs_.size());
        double sum = 0.0;
        for (const auto& o : obs_) {
            const double us = static_cast<double>(o.first_packet_latency_ns) / 1000.0;
            v.push_back(us);
            sum += us;
        }
        std::sort(v.begin(), v.end());
        s.min_us = v.front();
        s.max_us = v.back();
        s.mean_us = sum / static_cast<double>(v.size());
        s.p50_us = percentile(v, 50.0);
        s.p99_us = percentile(v, 99.0);
        s.rereg_count = rereg_count_;
        s.lost_count = lost_count_;
        s.failed_recovery = failed_recovery_;
        return s;
    }

    static double percentile(const std::vector<double>& sorted, double p)
    {
        if (sorted.empty()) return 0.0;
        if (sorted.size() == 1) return sorted.front();
        const double rank = (p / 100.0) * static_cast<double>(sorted.size() - 1);
        const std::size_t lo = static_cast<std::size_t>(rank);
        const std::size_t hi = std::min(lo + 1, sorted.size() - 1);
        const double frac = rank - static_cast<double>(lo);
        return sorted[lo] * (1.0 - frac) + sorted[hi] * frac;
    }

private:
    std::vector<RecoveryObservation> obs_{};
    std::uint64_t rereg_count_{0};
    std::uint64_t lost_count_{0};
    std::uint64_t failed_recovery_{0};
};

/* 周期任务覆盖证据（控制面 tick / heartbeat 等）。 */
struct PeriodicTaskCoverage
{
    bool          configured{false};   ///< 本次实验是否声明了周期任务
    std::uint64_t ticks_observed{0};   ///< 空闲窗口内观测到的 tick 数
    double        ticks_per_s{0.0};
    std::string   source;              ///< 计数来源（模块名 / 计数器名）
    std::string   note;
};

/* ------------------------------------------------------------------ 相位统计 */

enum class Phase
{
    startup_peak = 0,   ///< 启动峰值：注册/建连阶段线程与 CPU 的峰值
    steady_active,      ///< 稳定活跃：定速/满速发送期
    idle_fallback       ///< 空闲回落：发布停止后 keep-alive 生效期
};

inline const char* phase_name(Phase p) noexcept
{
    switch (p) {
    case Phase::startup_peak:   return "startup_peak";
    case Phase::steady_active:  return "steady_active";
    case Phase::idle_fallback:  return "idle_fallback";
    }
    return "unknown";
}

/* 一个相位里的一次采样。 */
struct PhaseSample
{
    std::uint64_t monotonic_ns{0};
    std::size_t   threads{0};
    double        cpu_cores{0.0};        ///< 该相位内该进程的 CPU 占用（核）
    double        ctx_switch_per_s{0.0}; ///< 该采样区间的上下文切换速率
    std::uint64_t rss_kb{0};
    std::uint64_t registered_routes{0};
    IdleState     idle_state{IdleState::unconfirmed};
};

struct PhaseStats
{
    Phase         phase{Phase::startup_peak};
    std::uint64_t samples{0};
    double        duration_s{0.0};
    double        threads_mean{0.0};
    double        threads_min{0.0};
    double        threads_max{0.0};
    double        threads_p95{0.0};
    double        cpu_cores_mean{0.0};
    double        cpu_cores_max{0.0};
    double        ctx_switch_per_s_mean{0.0};
    double        rss_kb_mean{0.0};
    bool          covered{false};        ///< 该相位是否真的采到样本
};

/* 一次相位序列的汇总（启动峰值 / 稳定活跃 / 空闲回落 三态）。 */
struct IdleSummary
{
    double        total_window_s{0.0};
    double        required_window_s{60.0};
    bool          window_sufficient{false};
    bool          diagnostics_enabled{false};
    PhaseStats    startup_peak{};
    PhaseStats    steady_active{};
    PhaseStats    idle_fallback{};
    PeriodicTaskCoverage periodic{};
    RecoveryLatencyStats::Summary recovery{};
    IdleInputs    last_inputs{};
    /* 回落判据：空闲相位线程均值相对稳定活跃相位的回落比例（>=0.5 视为已回落）。 */
    double        thread_fallback_ratio{0.0};
    bool          idle_fallback_observed{false};
    std::string   verdict;
};

class PhaseObserver
{
public:
    explicit PhaseObserver(double required_window_s = 60.0)
        : required_window_s_(required_window_s) {}

    void begin(std::uint64_t now_ns)
    {
        begin_ns_ = now_ns;
        last_ns_ = now_ns;
        begun_ = true;
    }

    bool begun() const noexcept { return begun_; }

    void set_periodic_coverage(const PeriodicTaskCoverage& p) { periodic_ = p; }

    void add(Phase phase, const PhaseSample& s)
    {
        if (!begun_) begin(s.monotonic_ns);
        samples(phase).push_back(s);
        if (last_ns_ == 0 || s.monotonic_ns > last_ns_) last_ns_ = s.monotonic_ns;
    }

    void set_recovery(const RecoveryLatencyStats& r) { recovery_ = r; }
    void set_last_inputs(const IdleInputs& in) { last_inputs_ = in; }
    void set_diagnostics_enabled(bool on) { diagnostics_enabled_ = on; }

    IdleSummary summary() const
    {
        IdleSummary out;
        out.total_window_s = begun_ && last_ns_ >= begin_ns_
                           ? static_cast<double>(last_ns_ - begin_ns_) / 1e9 : 0.0;
        out.required_window_s = required_window_s_;
        out.window_sufficient = out.total_window_s >= required_window_s_;
        out.diagnostics_enabled = diagnostics_enabled_;
        out.startup_peak = compute(Phase::startup_peak, out.total_window_s);
        out.steady_active = compute(Phase::steady_active, out.total_window_s);
        out.idle_fallback = compute(Phase::idle_fallback, out.total_window_s);
        out.periodic = periodic_;
        out.recovery = recovery_.summary();
        out.last_inputs = last_inputs_;

        if (out.steady_active.threads_mean > 0.0) {
            out.thread_fallback_ratio =
                out.idle_fallback.threads_mean / out.steady_active.threads_mean;
        }
        out.idle_fallback_observed = out.idle_fallback.covered
                                  && out.steady_active.covered
                                  && out.thread_fallback_ratio <= 0.5;

        /* verdict：只描述可核验事实，缺项直接点名。 */
        std::string v;
        if (!out.window_sufficient) {
            v += "窗口不足(需 >= " + std::to_string(static_cast<long>(required_window_s_))
               + " s, 实测 " + std::to_string(out.total_window_s) + " s) => 未确认; ";
        }
        if (!out.startup_peak.covered) v += "缺启动峰值相位 => 未确认; ";
        if (!out.steady_active.covered) v += "缺稳定活跃相位 => 未确认; ";
        if (!out.idle_fallback.covered) v += "缺空闲回落相位 => 未确认; ";
        if (!out.periodic.configured) v += "未声明周期任务 => 空闲未覆盖周期任务, 未确认; ";
        else if (out.periodic.ticks_observed == 0) v += "周期任务 tick 为 0 => 未覆盖, 未确认; ";
        if (out.idle_fallback_observed)
            v += "空闲回落已观测(线程均值回落至稳定活跃的 "
               + std::to_string(out.thread_fallback_ratio) + "); ";
        else if (out.idle_fallback.covered && out.steady_active.covered)
            v += "空闲回落未达阈值(比例 " + std::to_string(out.thread_fallback_ratio) + "); ";
        if (v.empty()) v = "三态相位与窗口均满足";
        out.verdict = v;
        return out;
    }

    std::string to_json() const
    {
        const IdleSummary s = summary();
        std::string o;
        o += "{\n";
        o += "  \"required_window_s\": " + std::to_string(s.required_window_s) + ",\n";
        o += "  \"total_window_s\": " + std::to_string(s.total_window_s) + ",\n";
        o += "  \"window_sufficient\": ";
        o += s.window_sufficient ? "true" : "false";
        o += ",\n  \"diagnostics_enabled\": ";
        o += s.diagnostics_enabled ? "true" : "false";
        o += ",\n";
        o += "  \"phases\": {\n";
        o += phase_json("startup_peak", s.startup_peak, true);
        o += phase_json("steady_active", s.steady_active, true);
        o += phase_json("idle_fallback", s.idle_fallback, false);
        o += "  },\n";
        o += "  \"periodic_task_coverage\": {\"configured\": ";
        o += s.periodic.configured ? "true" : "false";
        o += ", \"ticks_observed\": " + std::to_string(s.periodic.ticks_observed);
        o += ", \"ticks_per_s\": " + std::to_string(s.periodic.ticks_per_s);
        o += ", \"source\": \"" + s.periodic.source + "\"},\n";
        o += "  \"recovery_first_packet_us\": {\"count\": " + std::to_string(s.recovery.count);
        o += ", \"min\": " + std::to_string(s.recovery.min_us);
        o += ", \"mean\": " + std::to_string(s.recovery.mean_us);
        o += ", \"p50\": " + std::to_string(s.recovery.p50_us);
        o += ", \"p99\": " + std::to_string(s.recovery.p99_us);
        o += ", \"max\": " + std::to_string(s.recovery.max_us);
        o += ", \"reregistration_required\": " + std::to_string(s.recovery.rereg_count);
        o += ", \"lost\": " + std::to_string(s.recovery.lost_count);
        o += ", \"failed\": " + std::to_string(s.recovery.failed_recovery) + "},\n";
        o += "  \"thread_fallback_ratio\": " + std::to_string(s.thread_fallback_ratio) + ",\n";
        o += "  \"idle_fallback_observed\": ";
        o += s.idle_fallback_observed ? "true" : "false";
        o += ",\n  \"verdict\": \"" + json_escape_simple(s.verdict) + "\"\n";
        o += "}\n";
        return o;
    }

    static std::string json_escape_simple(const std::string& s)
    {
        std::string o;
        for (char c : s) {
            if (c == '"' || c == '\\') { o += '\\'; o += c; }
            else if (c == '\n') o += "\\n";
            else o += c;
        }
        return o;
    }

private:
    std::vector<PhaseSample>& samples(Phase p)
    {
        switch (p) {
        case Phase::startup_peak:  return startup_;
        case Phase::steady_active: return steady_;
        case Phase::idle_fallback: return idle_;
        }
        return idle_;
    }
    const std::vector<PhaseSample>& samples_of(Phase p) const
    {
        switch (p) {
        case Phase::startup_peak:  return startup_;
        case Phase::steady_active: return steady_;
        case Phase::idle_fallback: return idle_;
        }
        return idle_;
    }

    PhaseStats compute(Phase p, double total_window_s) const
    {
        const std::vector<PhaseSample>& v = samples_of(p);
        PhaseStats st;
        st.phase = p;
        st.samples = v.size();
        if (v.empty()) return st;
        st.covered = true;
        std::vector<double> threads;
        threads.reserve(v.size());
        double sum_t = 0.0, sum_cpu = 0.0, max_cpu = 0.0, sum_ctx = 0.0, sum_rss = 0.0;
        for (const auto& s : v) {
            threads.push_back(static_cast<double>(s.threads));
            sum_t += static_cast<double>(s.threads);
            sum_cpu += s.cpu_cores;
            max_cpu = std::max(max_cpu, s.cpu_cores);
            sum_ctx += s.ctx_switch_per_s;
            sum_rss += static_cast<double>(s.rss_kb);
        }
        std::sort(threads.begin(), threads.end());
        st.threads_min = threads.front();
        st.threads_max = threads.back();
        st.threads_mean = sum_t / static_cast<double>(v.size());
        st.threads_p95 = RecoveryLatencyStats::percentile(threads, 95.0);
        st.cpu_cores_mean = sum_cpu / static_cast<double>(v.size());
        st.cpu_cores_max = max_cpu;
        st.ctx_switch_per_s_mean = sum_ctx / static_cast<double>(v.size());
        st.rss_kb_mean = sum_rss / static_cast<double>(v.size());
        /* 相位时长 = 该相位采样的 (max_ns - min_ns)。
         * ⚠️ 必须用 min/max 而不是 front()/back()：调用方（采集器）按"每采样时刻"分组后
         * 逐组喂入，其遍历顺序不一定单调（例如用字符串键排序时，13 位与 14 位 ns 的字典序
         * 与数值序不一致）。用 front/back 会得到无符号下溢值（(2^64-1)/1e9 ≈ 1.8446744e10），
         * 这是一个曾经真实发生过的缺陷。本函数因此与插入顺序无关。 */
        if (v.size() >= 2) {
            std::uint64_t lo = v.front().monotonic_ns;
            std::uint64_t hi = v.front().monotonic_ns;
            for (const PhaseSample& ps : v) {
                if (ps.monotonic_ns < lo) lo = ps.monotonic_ns;
                if (ps.monotonic_ns > hi) hi = ps.monotonic_ns;
            }
            st.duration_s = static_cast<double>(hi - lo) / 1e9;   // hi >= lo 恒成立
        } else {
            st.duration_s = 0.0;
        }
        (void)total_window_s;
        return st;
    }

    static std::string phase_json(const char* name, const PhaseStats& st, bool comma)
    {
        std::string o = "    \"";
        o += name;
        o += "\": {\"covered\": ";
        o += st.covered ? "true" : "false";
        o += ", \"samples\": " + std::to_string(st.samples);
        o += ", \"duration_s\": " + std::to_string(st.duration_s);
        o += ", \"threads_mean\": " + std::to_string(st.threads_mean);
        o += ", \"threads_min\": " + std::to_string(st.threads_min);
        o += ", \"threads_max\": " + std::to_string(st.threads_max);
        o += ", \"threads_p95\": " + std::to_string(st.threads_p95);
        o += ", \"cpu_cores_mean\": " + std::to_string(st.cpu_cores_mean);
        o += ", \"cpu_cores_max\": " + std::to_string(st.cpu_cores_max);
        o += ", \"ctx_switch_per_s_mean\": " + std::to_string(st.ctx_switch_per_s_mean);
        o += ", \"rss_kb_mean\": " + std::to_string(st.rss_kb_mean);
        o += "}";
        if (comma) o += ",";
        o += "\n";
        return o;
    }

    double        required_window_s_{60.0};
    bool          begun_{false};
    std::uint64_t begin_ns_{0};
    std::uint64_t last_ns_{0};
    std::vector<PhaseSample> startup_{};
    std::vector<PhaseSample> steady_{};
    std::vector<PhaseSample> idle_{};
    PeriodicTaskCoverage periodic_{};
    RecoveryLatencyStats recovery_{};
    IdleInputs    last_inputs_{};
    bool          diagnostics_enabled_{false};
};

}   // namespace measure
}   // namespace dzIPC

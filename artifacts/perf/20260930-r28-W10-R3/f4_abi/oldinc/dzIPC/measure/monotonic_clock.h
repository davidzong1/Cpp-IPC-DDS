#pragma once
/* W03 测量口径 · 单机跨进程一致单调时钟
 * ============================================================================
 * 交付依据：docs/消息接收架构改造/团队改造方案_性能证据闭环与SHM规模化.md
 *   §4 W03 第 1 条「单机跨进程采用一致的单调时钟；明确时间戳写入位置和计时本身
 *   的成本」；§12「建议把每条消息的样本字段固定为：运行编号、生产序号、发送
 *   时间戳、传输完成时间戳、应用获得时间戳、完整消费时间戳、route 标识、
 *   generation、路径（TLV/A/B）、载荷校验、重试/淘汰标记」。
 *
 * 口径约定（冻结，不得在报告里混用）：
 *   1. 单机跨进程**只允许** `CLOCK_MONOTONIC`。发布进程与订阅进程在同一台机器
 *      上用同一个时基，因此两边的原始 ns 可以直接相减；`CLOCK_REALTIME`
 *      （受 settimeofday/NTP 跳变影响）与 `std::chrono::system_clock` 禁止
 *      参与延迟计算。
 *   2. `std::chrono::steady_clock` 在本项目所支持的 glibc 上就是
 *      `CLOCK_MONOTONIC`，但它是实现细节；正式采集一律走本文件的
 *      `monotonic_now_ns()`，避免"某进程用了 system_clock"这种静默口径错配。
 *   3. 本机采集不跨机：`CLOCK_MONOTONIC` 是 per-boot 的，两台机器之间不可比。
 *      需要跨机时另立工作包，不在 W03 射程。
 *   4. 计时本身有成本：`clock_gettime(CLOCK_MONOTONIC)` 通常走 vDSO（约十几到
 *      几十 ns），但如果内核/环境使 vDSO 失效（例如 seccomp 拦截或静态链接），
 *      会退化成 syscall（数百 ns 到 1 µs）。因此本文件同时提供
 *      `monotonic_now_ns_syscall()`（强制 syscall）和 `measure_clock_cost()`，
 *      采集器必须在结果里报告实际成本，不能默认"计时免费"。
 *
 * 时间戳写入位置（每条消息）与字段名（对应 §12 的样本字段）：
 *   produced_ns         生产端：为本条消息生成新数据/载荷之前（序号已分配）
 *   publish_enter_ns    生产端：publish()/publish_loan() 入口
 *   transport_done_ns   生产端：publish()/publish_loan() 返回（数据已对订阅端可见）
 *   app_obtained_ns     订阅端：应用拿到可用视图/对象（get()/try_get() 返回）
 *   fully_consumed_ns   订阅端：完整载荷遍历 + 校验结束
 * 另有诊断口径（不参与逐样本延迟，用于 §10.2 扫描/预算）：
 *   recv_once_enter_ns / recv_once_exit_ns
 *   scan_enter_ns / scan_exit_ns
 *   recovery_first_packet_ns   空闲恢复后首包到达时刻（§10.1）
 *
 * ⛔ 采集到的字段里 null（未写）与 0 必须区分：未测项写 null 并在 verdict 标注
 *    "未确认"，不允许用 0 冒充"没有延迟"。
 */
#include <cstdint>
#include <ctime>
#include <string>

#if defined(__linux__)
#  include <sys/syscall.h>
#  include <sys/wait.h>
#  include <unistd.h>
#endif

namespace dzIPC {
namespace measure {

/* 时间基标识：写进 manifest/environment，便于事后核对口径。 */
inline const char* clock_source_name() noexcept { return "CLOCK_MONOTONIC"; }

/* 正式口径：单机跨进程一致的单调时钟，返回纳秒。 */
inline std::uint64_t monotonic_now_ns() noexcept
{
#if defined(__linux__)
    timespec ts{};
    ::clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull
         + static_cast<std::uint64_t>(ts.tv_nsec);
#else
    timespec ts{};
    ::timespec_get(&ts, TIME_UTC);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull
         + static_cast<std::uint64_t>(ts.tv_nsec);
#endif
}

/* 强制系统调用版本：只用于"计时成本"对照，不用于正式采样点。
 * 它绕过 vDSO，代表最坏情况下的时钟写入成本。 */
inline std::uint64_t monotonic_now_ns_syscall() noexcept
{
#if defined(__linux__)
    timespec ts{};
    ::syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &ts);
    return static_cast<std::uint64_t>(ts.tv_sec) * 1000000000ull
         + static_cast<std::uint64_t>(ts.tv_nsec);
#else
    return monotonic_now_ns();
#endif
}

/* 时间戳写入点。枚举值参与 JSON 输出，**只允许追加**（保留既有数值语义）。 */
enum class TimestampPoint : std::uint32_t
{
    produced = 0,               ///< 生产端：本条消息数据生成开始
    publish_enter = 1,          ///< 生产端：发布 API 入口
    transport_done = 2,         ///< 生产端：发布 API 返回（传输完成）
    app_obtained = 3,           ///< 订阅端：应用获得视图/对象
    fully_consumed = 4,         ///< 订阅端：完整载荷消费结束
    recv_once_enter = 5,        ///< 诊断：worker 一次 recv_once 入口
    recv_once_exit = 6,         ///< 诊断：worker 一次 recv_once 出口
    scan_enter = 7,             ///< 诊断：一轮 collect_pending 扫描入口
    scan_exit = 8,              ///< 诊断：一轮扫描出口
    recovery_first_packet = 9,  ///< §10.1：静默恢复后首包到达
    count = 10
};

inline constexpr std::size_t kTimestampPointCount =
    static_cast<std::size_t>(TimestampPoint::count);

inline const char* timestamp_point_name(TimestampPoint p) noexcept
{
    switch (p) {
    case TimestampPoint::produced:              return "produced";
    case TimestampPoint::publish_enter:         return "publish_enter";
    case TimestampPoint::transport_done:        return "transport_done";
    case TimestampPoint::app_obtained:          return "app_obtained";
    case TimestampPoint::fully_consumed:        return "fully_consumed";
    case TimestampPoint::recv_once_enter:       return "recv_once_enter";
    case TimestampPoint::recv_once_exit:        return "recv_once_exit";
    case TimestampPoint::scan_enter:            return "scan_enter";
    case TimestampPoint::scan_exit:             return "scan_exit";
    case TimestampPoint::recovery_first_packet: return "recovery_first_packet";
    case TimestampPoint::count:                 break;
    }
    return "unknown";
}

inline const char* timestamp_point_description(TimestampPoint p) noexcept
{
    switch (p) {
    case TimestampPoint::produced:
        return "生产端: 为本条消息生成新数据/载荷之前 (序号已分配)";
    case TimestampPoint::publish_enter:
        return "生产端: publish()/publish_loan() 入口";
    case TimestampPoint::transport_done:
        return "生产端: 发布 API 返回, 数据已对订阅端可见";
    case TimestampPoint::app_obtained:
        return "订阅端: 应用拿到可用视图/对象 (get()/try_get() 返回)";
    case TimestampPoint::fully_consumed:
        return "订阅端: 完整载荷遍历与校验结束";
    case TimestampPoint::recv_once_enter:
        return "诊断: worker 单次 recv_once 入口 (§10.2/§10.3)";
    case TimestampPoint::recv_once_exit:
        return "诊断: worker 单次 recv_once 出口";
    case TimestampPoint::scan_enter:
        return "诊断: collect_pending 一轮扫描入口 (§10.2)";
    case TimestampPoint::scan_exit:
        return "诊断: 一轮扫描出口";
    case TimestampPoint::recovery_first_packet:
        return "§10.1: 静默恢复后的首包到达时刻";
    case TimestampPoint::count:
        break;
    }
    return "";
}

/* 计时成本测量结果。单位 ns/call。 */
struct ClockCost
{
    std::string  source{"CLOCK_MONOTONIC"};
    std::uint64_t iterations{0};
    double       vdso_ns_per_call{0.0};      ///< 正式口径的实际成本 (通常走 vDSO)
    double       syscall_ns_per_call{0.0};   ///< 强制 syscall 的上界成本
    bool         vdso_in_use{true};          ///< syscall/普通 比值 < 8 视为 vDSO 生效
};

/* 计时成本：微基准。默认 200k 次，约几十 ms。 */
inline ClockCost measure_clock_cost(std::uint64_t iterations = 200000)
{
    ClockCost c;
    c.iterations = iterations;
    if (iterations == 0) return c;

    volatile std::uint64_t sink = 0;
    std::uint64_t a = monotonic_now_ns();
    for (std::uint64_t i = 0; i < iterations; ++i) sink = sink ^ monotonic_now_ns();
    std::uint64_t b = monotonic_now_ns();
    c.vdso_ns_per_call = static_cast<double>(b - a) / static_cast<double>(iterations);

    std::uint64_t d = monotonic_now_ns();
    for (std::uint64_t i = 0; i < iterations; ++i) sink = sink ^ monotonic_now_ns_syscall();
    std::uint64_t e = monotonic_now_ns();
    c.syscall_ns_per_call = static_cast<double>(e - d) / static_cast<double>(iterations);
    (void)sink;

    /* vDSO 判据: 直接调用远快于强制 syscall，且绝对成本在 vDSO 量级(<100 ns)。
     * 只用比值会在"两侧都被环境拖慢"时误判，因此同时要求绝对量级。 */
    if (c.vdso_ns_per_call <= 0.0) {
        c.vdso_in_use = false;
    } else {
        const double ratio = c.syscall_ns_per_call / c.vdso_ns_per_call;
        c.vdso_in_use = (c.vdso_ns_per_call < 100.0) && (ratio < 20.0);
    }
    return c;
}

/* 跨进程一致性的实际核验（fork + 管道）。
 * 判据：子进程写下的 CLOCK_MONOTONIC 值必须落在父进程"fork 前/收完"两次读数
 * 之间。因为同机 CLOCK_MONOTONIC 同源，若子进程用了别的时基（system_clock /
 * TSC 私有换算），这个区间判定必然失败。这是"跨进程身份可核验"的机器判据，
 * 不是口头声明。 */
struct XprocClockCheck
{
    bool          consistent{false};
    bool          performed{false};
    std::uint64_t parent_before_ns{0};
    std::uint64_t child_ns{0};
    std::uint64_t parent_after_ns{0};
    std::uint64_t bracket_ns{0};        ///< parent_after - parent_before，一致性上界
    std::string   method{"fork+pipe"};
    std::string   note;
};

#if defined(__linux__)
inline XprocClockCheck verify_cross_process_clock()
{
    XprocClockCheck r;
    int fds[2] = {-1, -1};
    if (::pipe(fds) != 0) { r.note = "pipe() failed"; return r; }

    r.parent_before_ns = monotonic_now_ns();
    const pid_t pid = ::fork();
    if (pid == 0) {
        ::close(fds[0]);
        const std::uint64_t t = monotonic_now_ns();
        const ssize_t n = ::write(fds[1], &t, sizeof(t));
        (void)n;
        ::close(fds[1]);
        ::_exit(0);
    }
    if (pid < 0) {
        ::close(fds[0]);
        ::close(fds[1]);
        r.note = "fork() failed";
        return r;
    }
    ::close(fds[1]);
    std::uint64_t child = 0;
    ssize_t got = 0;
    while (got < static_cast<ssize_t>(sizeof(child))) {
        const ssize_t n = ::read(fds[0], reinterpret_cast<char*>(&child) + got,
                                 sizeof(child) - static_cast<std::size_t>(got));
        if (n <= 0) break;
        got += n;
    }
    ::close(fds[0]);
    int status = 0;
    ::waitpid(pid, &status, 0);
    r.parent_after_ns = monotonic_now_ns();
    r.performed = true;
    if (got != static_cast<ssize_t>(sizeof(child))) {
        r.note = "child did not report a timestamp";
        return r;
    }
    r.child_ns = child;
    r.bracket_ns = r.parent_after_ns - r.parent_before_ns;
    r.consistent = (child >= r.parent_before_ns) && (child <= r.parent_after_ns);
    r.note = r.consistent
        ? "child CLOCK_MONOTONIC value lies inside the parent bracket"
        : "child timestamp outside parent bracket => clock mismatch";
    return r;
}
#else
inline XprocClockCheck verify_cross_process_clock()
{
    XprocClockCheck r;
    r.note = "fork+pipe clock check is implemented for Linux only";
    return r;
}
#endif

}   // namespace measure
}   // namespace dzIPC

/// UF-004 opt-out(`dzIPC::DisableShutdownMonitor()`)的验收用例。
///
/// 被验的现状(改动前): 库在**首个 IPC 对象构造**时隐式接管进程退出 ——
/// `src/dzIPC/dzipc.cc` 的四个 `*IPCPtrMake` → `EnsureShutdownMonitorStarted()` →
/// `std::signal(SIGINT/SIGTERM)` + 100ms 轮询线程 → `std::exit(0)`。于是应用自己装的
/// 处理器被覆盖, 且退出发生在**分离线程**里(不展开栈 ⇒ 段不 unlink ⇒ 残留)。
///
/// 新开关只解除"库接管", 且**只在首个 IPC 构造之前**有效:
///   默认              : 不调 ⇒ 逐位不变(库照旧接管);
///   提前 opt-out      : 返回 true ⇒ 不装处理器、不起线程 ⇒ 信号走应用自己的处置(SIG_DFL 即硬杀);
///   晚调用            : 返回 false ⇒ 太晚, **行为不变**(仍由库接管);
///   应用自有处理器    : opt-out 后处理器存活且被调用(这才是"应用自负退出"的可用形态);
///   显式 StartShutdownMonitor(): opt-out 后**仍然照装**(开关只关隐式这一条路);
///   显式 Start **先**、opt-out **后**: 返回 true 但**不撤销**已装处理器 ⇒ 返回值**不代表**
///                                     "库当前没接管", 行为保持"已启动"(A3b: 注释与本臂同口径)。
///
/// ⚠️ 每个臂都必须在**子进程**里跑, 两个原因缺一不可:
///   ① 监控是**进程级一次性**的(atomic + once_flag) ⇒ 在测试进程里跑一次就污染所有后续臂;
///   ② 走默认路径时监控线程会 `std::exit(0)`, 直接把 gtest 进程带走。
/// 父进程只负责 fork / 读回报 / waitpid, 自身不碰任何 IPC。
///
/// 判据取"进程退出状态"(退出码 / 被杀信号)+ 子进程直接读出的 `sigaction` 处置,
/// 二者都不可被日志措辞掩盖。

#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include "dzIPC/dzipc.h"
#include "ipc_msg/test_msg2/test_msg.hpp"

#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;

int64_t remaining_ms(const std::chrono::steady_clock::time_point& deadline)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
        .count();
}

/* ---- 子进程回报: 只要 write() 成功就到父进程, 没有 stdio 缓冲 ---- */
void report(int fd, const char* text)
{
    if (fd < 0)
        return;
    const ssize_t n = ::write(fd, text, std::strlen(text));
    (void)n;
}

struct ChildResult
{
    bool signaled{false};
    int  signo{0};
    bool exited{false};
    int  code{0};
    bool timed_out{false};   /* 父进程侧的硬超时: 子进程没在窗口内结束 ⇒ 用例转红, 不挂住套件 */
    std::string log;
};

std::string describe(const ChildResult& r)
{
    std::string s = "log=[" + r.log + "] ";
    if (r.signaled)
        s += "被信号 " + std::to_string(r.signo) + " 终止";
    else if (r.exited)
        s += "正常退出 code=" + std::to_string(r.code);
    else
        s += "状态未知";
    if (r.timed_out)
        s += " (父进程硬超时, 已 SIGKILL)";
    return s;
}

/* 在子进程里跑 body(fd); body 自己 _exit()。父进程拿退出状态 + 子进程回报的文本。 */
template <typename Body>
ChildResult run_in_child(Body body)
{
    ChildResult r;
    int pfd[2] = {-1, -1};
    if (::pipe(pfd) != 0)
    {
        r.log = "pipe() failed";
        return r;
    }
    const ::pid_t pid = ::fork();
    if (pid < 0)
    {
        ::close(pfd[0]);
        ::close(pfd[1]);
        r.log = "fork() failed";
        return r;
    }
    if (pid == 0)
    {
        ::close(pfd[0]);
        body(pfd[1]);
        ::_exit(90);   /* body 一律自己 _exit, 走到这里说明写漏了 */
    }
    ::close(pfd[1]);

    /* 先读回报再收尸: 文本很短, 不会写满管道。EOF 出现在子进程关闭/退出时,
       所以"读到 EOF"本身就说明子进程已经结束 —— 中途卡住时由 poll 超时兜底。 */
    const auto deadline = std::chrono::steady_clock::now() + 8000ms;
    for (;;)
    {
        const int64_t remain = remaining_ms(deadline);
        if (remain <= 0)
        {
            r.timed_out = true;
            break;
        }
        struct pollfd p{pfd[0], POLLIN, 0};
        const int pr = ::poll(&p, 1, static_cast<int>(remain));
        if (pr < 0)
        {
            if (errno == EINTR)
                continue;
            break;
        }
        if (pr == 0)
        {
            r.timed_out = true;
            break;
        }
        char buf[256];
        const ssize_t n = ::read(pfd[0], buf, sizeof(buf));
        if (n > 0)
        {
            r.log.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0)
            break;   /* EOF: 子进程已结束(或已关闭写端) */
        if (errno == EINTR)
            continue;
        break;
    }
    ::close(pfd[0]);

    int st = 0;
    for (;;)
    {
        const ::pid_t w = ::waitpid(pid, &st, WNOHANG);
        if (w == pid)
            break;
        if (w < 0 && errno != EINTR)
            break;
        if (remaining_ms(deadline) <= 0)
        {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, &st, 0);
            r.timed_out = true;
            break;
        }
        ::usleep(5 * 1000);
    }
    if (WIFSIGNALED(st))
    {
        r.signaled = true;
        r.signo = WTERMSIG(st);
    }
    else if (WIFEXITED(st))
    {
        r.exited = true;
        r.code = WEXITSTATUS(st);
    }
    return r;
}

/* 子进程侧: 读当前 SIGINT 处置是不是 SIG_DFL。比"看退出码"更直接 ——
 * 它测的就是"库有没有装处理器"这件事本身。 */
bool sigint_is_dfl()
{
    struct sigaction sa;
    if (::sigaction(SIGINT, nullptr, &sa) != 0)
        return false;
    return sa.sa_handler == SIG_DFL;
}

/* 触发**隐式**安装的唯一入口: 四个公共工厂之一(dzipc.cc 里 EnsureShutdownMonitorStarted
 * 只在那里被调)。用 SHM + 每次运行唯一的 topic 名, 避免与本机其它测试串台。 */
bool construct_ipc(const std::string& topic)
{
    try
    {
        auto td = dzIPC::TopicDataPtrMake<dzIPC::Msg::TestMsg>(0);
        auto pub = dzIPC::PublisherIPCPtrMake(td, topic, 0, dzIPC::IPC_SHM, false);
        return pub != nullptr;
    }
    catch (...)
    {
        return false;
    }
}

/* 本轮运行专用 topic 前缀: 带上**父进程** pid ⇒ 清理只会碰到自己的残留。
 * ⛔ 必须在**父进程**里取一次(lambda 按值捕获), 因为子进程里再取会拿到子进程 pid,
 * 那样"父进程按 tag 清理"就永远匹配不上子进程建的段。 */
const std::string& run_tag()
{
    static const std::string tag = "/ut_uf004_" + std::to_string(static_cast<long>(::getpid())) + "_";
    return tag;
}

/* 清掉本用例自己在 /dev/shm 留下的段。
 * 为什么需要: 走默认路径的臂由监控线程 std::exit(0) 结束, **不展开栈** ⇒ 局部
 * shared_ptr 不析构 ⇒ 段不 unlink。这正是 UF-004 的残留现象本身, 用例不为它翻案,
 * 只是别把垃圾堆在共享机器上(名字带本轮 pid, 不会碰到别人的段)。 */
void cleanup_own_shm_residue(const std::string& tag)
{
    DIR* d = ::opendir("/dev/shm");
    if (d == nullptr)
        return;
    int removed = 0;
    while (struct dirent* e = ::readdir(d))
    {
        if (std::strstr(e->d_name, tag.c_str()) == nullptr)
            continue;
        const std::string p = std::string("/dev/shm/") + e->d_name;
        if (::unlink(p.c_str()) == 0)
            ++removed;
    }
    ::closedir(d);
    if (removed != 0)
        std::printf("[UF-004] 清理本轮自建 /dev/shm 段: %d\n", removed);
}

/* 默认路径下监控线程 100ms 轮询后才 exit(0) ⇒ 给足窗口再判"没退出"。 */
constexpr int kMonitorExitWindowMs = 1500;

void wait_monitor_or_report(int fd)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kMonitorExitWindowMs);
    while (remaining_ms(deadline) > 0)
        ::usleep(10 * 1000);
    report(fd, "monitor_did_not_exit=1\n");
    ::_exit(9);
}

/* ===================================================================== *
 * 臂 1: 基线 —— 应用没装处理器、库也没接管 ⇒ SIG_DFL 硬杀。
 * 它是另外三个臂的**对照**: 没有它, "opt-out 后进程被杀"这个观测就无法与
 * "进程本来就会被杀"区分开。
 * ===================================================================== */
TEST(DzIpcShutdownMonitorOptOut, SigDflBaselineDiesBySignal)
{
    const std::string tag = run_tag();
    for (const int sig : {SIGINT, SIGTERM})
    {
        const ChildResult r = run_in_child(
            [sig](int fd)
            {
                std::signal(SIGINT, SIG_DFL);
                std::signal(SIGTERM, SIG_DFL);
                report(fd, "ipc=0 optout=0\n");
                std::raise(sig);
                report(fd, "survived=1\n");   /* SIG_DFL 下到不了这里 */
                ::_exit(7);
            });
        EXPECT_TRUE(r.signaled) << "signal " << sig << ": 无处置时必须是 SIG_DFL 硬杀; 实测 " << describe(r);
        EXPECT_EQ(r.signo, sig) << describe(r);
        EXPECT_EQ(r.log.find("survived=1"), std::string::npos) << describe(r);
    }
    cleanup_own_shm_residue(tag);
}

/* ===================================================================== *
 * 臂 2: 默认(不调 opt-out) —— 库接管: 装处理器 + 监控线程 exit(0)。
 * 本臂就是"默认行为逐位不变"的可执行定义, 也是臂 3/4 的参照。
 * ===================================================================== */
TEST(DzIpcShutdownMonitorOptOut, DefaultPathLibraryTakesOverExitZero)
{
    const std::string tag = run_tag();
    const ChildResult r = run_in_child(
        [tag](int fd)
        {
            std::signal(SIGINT, SIG_DFL);
            const bool built = construct_ipc(tag + "default");
            report(fd, built ? "ipc=1 optout=0\n" : "ipc=0 optout=0\n");
            if (!built)
                ::_exit(8);
            report(fd, sigint_is_dfl() ? "sigint=dfl\n" : "sigint=not_dfl\n");
            std::raise(SIGINT);
            wait_monitor_or_report(fd);
        });
    ASSERT_NE(r.log.find("ipc=1"), std::string::npos)
        << "前提不成立: 工厂没能建出 IPC 对象, 本臂无意义; " << describe(r);
    EXPECT_NE(r.log.find("sigint=not_dfl"), std::string::npos)
        << "默认路径必须由库接管(SIGINT 处置被库覆盖); " << describe(r);
    EXPECT_TRUE(r.exited) << "默认路径必须由监控线程 std::exit(0) 结束; " << describe(r);
    EXPECT_EQ(r.code, 0) << describe(r);
    cleanup_own_shm_residue(tag);
}

/* ===================================================================== *
 * 臂 3(承重): 提前 opt-out ⇒ 库不接管, 信号走 SIG_DFL。
 * 变异: 把 DisableShutdownMonitor() 改成恒 false / 空实现 ⇒ 本臂转红(库照旧接管)。
 * ===================================================================== */
TEST(DzIpcShutdownMonitorOptOut, EarlyOptOutKeepsSigDfl)
{
    const std::string tag = run_tag();
    for (const int sig : {SIGINT, SIGTERM})
    {
        const ChildResult r = run_in_child(
            [sig, tag](int fd)
            {
                std::signal(SIGINT, SIG_DFL);
                std::signal(SIGTERM, SIG_DFL);
                const bool ok = dzIPC::DisableShutdownMonitor();   /* 首次构造之前 */
                report(fd, ok ? "optout=1\n" : "optout=0\n");
                const bool built = construct_ipc(tag + "early");
                report(fd, built ? "ipc=1\n" : "ipc=0\n");
                if (!built)
                    ::_exit(8);
                report(fd, sigint_is_dfl() ? "sigint=dfl\n" : "sigint=not_dfl\n");
                std::raise(sig);
                report(fd, "survived=1\n");
                ::_exit(7);
            });
        EXPECT_NE(r.log.find("optout=1"), std::string::npos)
            << "signal " << sig << ": 首次构造之前调用必须返回 true; " << describe(r);
        ASSERT_NE(r.log.find("ipc=1"), std::string::npos) << describe(r);
        EXPECT_NE(r.log.find("sigint=dfl"), std::string::npos)
            << "signal " << sig << ": opt-out 后库不得装处理器; " << describe(r);
        EXPECT_TRUE(r.signaled) << "signal " << sig << ": opt-out 后信号必须走 SIG_DFL(硬杀); " << describe(r);
        EXPECT_EQ(r.signo, sig) << describe(r);
        EXPECT_EQ(r.log.find("survived=1"), std::string::npos) << describe(r);
    }
    cleanup_own_shm_residue(tag);
}

/* ===================================================================== *
 * 臂 4(承重): 晚调用 ⇒ 返回 false(不可撤销) **且行为不变**(仍由库接管)。
 * 变异: 把返回值改成恒 true ⇒ 本臂的 "optout=0" 断言转红。
 * ===================================================================== */
TEST(DzIpcShutdownMonitorOptOut, LateCallReturnsFalseAndBehaviorUnchanged)
{
    const std::string tag = run_tag();
    const ChildResult r = run_in_child(
        [tag](int fd)
        {
            std::signal(SIGINT, SIG_DFL);
            const bool built = construct_ipc(tag + "late");
            report(fd, built ? "ipc=1\n" : "ipc=0\n");
            if (!built)
                ::_exit(8);
            const bool ok = dzIPC::DisableShutdownMonitor();   /* 监控已在跑 ⇒ 太晚 */
            report(fd, ok ? "optout=1\n" : "optout=0\n");
            std::raise(SIGINT);
            wait_monitor_or_report(fd);
        });
    ASSERT_NE(r.log.find("ipc=1"), std::string::npos) << describe(r);
    EXPECT_NE(r.log.find("optout=0"), std::string::npos)
        << "监控已启动后调用必须返回 false(调用方要能区分\"生效\"与\"太晚\"); " << describe(r);
    EXPECT_TRUE(r.exited) << "晚调用不得改变行为: 仍须由库接管退出; " << describe(r);
    EXPECT_EQ(r.code, 0) << describe(r);
    cleanup_own_shm_residue(tag);
}

/* ===================================================================== *
 * 臂 5(承重): opt-out + 应用自有处理器 ⇒ 处理器**存活并被调用**。
 * 这是"应用自负退出"的可用形态: 既不是库 exit(0), 也不是 SIG_DFL 硬杀。
 * 变异: opt-out 改成空实现 ⇒ 库的 std::signal 覆盖应用处理器 ⇒ handler_kept=0 转红。
 * ===================================================================== */
volatile std::sig_atomic_t g_app_sigint_called = 0;
void app_sigint_handler(int) { g_app_sigint_called = 1; }   /* 异步信号安全: 只置位 */

TEST(DzIpcShutdownMonitorOptOut, EarlyOptOutPreservesApplicationHandler)
{
    const std::string tag = run_tag();
    const ChildResult r = run_in_child(
        [tag](int fd)
        {
            std::signal(SIGINT, SIG_DFL);
            g_app_sigint_called = 0;
            std::signal(SIGINT, app_sigint_handler);   /* 应用自己的处理器(在构造之前) */
            const bool ok = dzIPC::DisableShutdownMonitor();
            report(fd, ok ? "optout=1\n" : "optout=0\n");
            const bool built = construct_ipc(tag + "apph");
            report(fd, built ? "ipc=1\n" : "ipc=0\n");
            struct sigaction sa;
            const bool kept =
                (::sigaction(SIGINT, nullptr, &sa) == 0) && (sa.sa_handler == app_sigint_handler);
            report(fd, kept ? "handler_kept=1\n" : "handler_kept=0\n");
            if (!built)
                ::_exit(8);
            std::raise(SIGINT);
            /* 处理器只置位 ⇒ 只有"库没接管"才可能走到下面 */
            for (int i = 0; i < 100 && !g_app_sigint_called; ++i)
                ::usleep(5 * 1000);
            report(fd, g_app_sigint_called ? "handler_called=1\n" : "handler_called=0\n");
            ::_exit(g_app_sigint_called ? 42 : 7);
        });
    ASSERT_NE(r.log.find("ipc=1"), std::string::npos) << describe(r);
    EXPECT_NE(r.log.find("optout=1"), std::string::npos) << describe(r);
    EXPECT_NE(r.log.find("handler_kept=1"), std::string::npos)
        << "opt-out 后构造第一个 IPC 对象不得覆盖应用已装的处理器; " << describe(r);
    EXPECT_NE(r.log.find("handler_called=1"), std::string::npos)
        << "应用处理器必须真的被调用(而不是被库接管或硬杀); " << describe(r);
    EXPECT_TRUE(r.exited) << "应用处理器只置位, 进程应由应用自己结束; " << describe(r);
    EXPECT_EQ(r.code, 42) << describe(r);
    cleanup_own_shm_residue(tag);
}

/* ===================================================================== *
 * 臂 6(承重): opt-out 只关**隐式**安装 —— 显式 StartShutdownMonitor() 仍照装。
 * 变异: 把判定搬进 StartShutdownMonitor() 的 call_once lambda ⇒ 提前 return 会把
 *       once_flag 记成"已执行完" ⇒ 公开 API(含 Python 绑定)永久静默失效 ⇒ 本臂转红。
 * ===================================================================== */
TEST(DzIpcShutdownMonitorOptOut, ExplicitStartStillInstallsAfterOptOut)
{
    const std::string tag = run_tag();
    const ChildResult r = run_in_child(
        [tag](int fd)
        {
            std::signal(SIGINT, SIG_DFL);
            const bool ok = dzIPC::DisableShutdownMonitor();
            report(fd, ok ? "optout=1\n" : "optout=0\n");
            /* 刻意**不**构造任何 IPC: 本臂只验显式调用这条路 */
            dzIPC::StartShutdownMonitor();
            report(fd, sigint_is_dfl() ? "sigint=dfl\n" : "sigint=not_dfl\n");
            std::raise(SIGINT);
            wait_monitor_or_report(fd);
        });
    EXPECT_NE(r.log.find("optout=1"), std::string::npos) << describe(r);
    EXPECT_NE(r.log.find("sigint=not_dfl"), std::string::npos)
        << "opt-out 只能关隐式路径: 显式 StartShutdownMonitor() 必须仍然安装处理器; " << describe(r);
    EXPECT_TRUE(r.exited) << describe(r);
    EXPECT_EQ(r.code, 0) << "显式 StartShutdownMonitor() 后 SIGINT 仍须由库接管退出; " << describe(r);
    cleanup_own_shm_residue(tag);
}

/* ===================================================================== *
 * 臂 7(承重, A3b 返工点): 顺序**反转** —— 先显式 StartShutdownMonitor(), 再 opt-out。
 * 本臂钉死的是**返回值语义**(不是某个产品行为):
 *   · 显式 Start 走 StartShutdownMonitor() → std::call_once, 它**不**置内部"已启动"标志
 *     (started 只由 EnsureShutdownMonitorStarted() 维护) ⇒ 随后调用 DisableShutdownMonitor()
 *     时 started 仍为 false ⇒ **返回 true**;
 *   · 但处理器已由 call_once 装上、监控线程已经在跑, 本调用**不可能**撤销它们 ⇒ 行为保持
 *     "已启动"(SIGINT 仍由库接管并 exit(0))。
 * 所以 true 只承诺"此后不再**隐式**安装", **不**承诺"库当前没接管"。头文件注释与本臂必须
 * 说的是同一件事 —— 这正是 reviewer seq157 指出的 A3b 缺口。
 * 变异: 让 StartShutdownMonitor() 也置 started(把"显式启动"也算已启动) ⇒ 本臂 "optout=1"
 *       转红(变成返回 false); `DisableShutdownMonitor` 恒返 false 的变异同理会打红它。
 * ===================================================================== */
TEST(DzIpcShutdownMonitorOptOut, ExplicitStartThenDisableReturnsTrueButHandlerStays)
{
    const std::string tag = run_tag();
    const ChildResult r = run_in_child(
        [tag](int fd)
        {
            std::signal(SIGINT, SIG_DFL);
            dzIPC::StartShutdownMonitor();   /* 显式启动; 刻意**不**构造任何 IPC 对象 ⇒ started 仍为 false */
            report(fd, sigint_is_dfl() ? "sigint_after_start=dfl\n" : "sigint_after_start=not_dfl\n");
            const bool ok = dzIPC::DisableShutdownMonitor();   /* 顺序反转: 库已启动之后才 opt-out */
            report(fd, ok ? "optout=1\n" : "optout=0\n");
            report(fd, sigint_is_dfl() ? "sigint_after_optout=dfl\n" : "sigint_after_optout=not_dfl\n");
            std::raise(SIGINT);
            wait_monitor_or_report(fd);
        });
    ASSERT_NE(r.log.find("sigint_after_start=not_dfl"), std::string::npos)
        << "前提不成立: 显式 StartShutdownMonitor() 必须先装上处理器, 否则本臂没验到东西; " << describe(r);
    EXPECT_NE(r.log.find("optout=1"), std::string::npos)
        << "顺序反转时返回 true(内部\"已启动\"标志只由隐式路径维护) —— 头文件注释必须写明 true "
           "不代表\"库没接管/已撤销\", 要判当前状态得自查 sigaction; " << describe(r);
    EXPECT_NE(r.log.find("sigint_after_optout=not_dfl"), std::string::npos)
        << "本调用不得(也不可能)撤销已装的处理器: 行为保持\"已启动\"; " << describe(r);
    EXPECT_TRUE(r.exited) << "已在跑的监控必须仍然接管退出(不是 SIG_DFL 硬杀); " << describe(r);
    EXPECT_EQ(r.code, 0) << describe(r);
    cleanup_own_shm_residue(tag);
}

}   // namespace

/// UF-009 链式接管(2026-09-18 落码)的验收用例。
///
/// 被验行为(src/dzIPC/dzipc.cc 的 ShutdownMonitorThreadBody):
///   ① 应用既有处理器被**回放调用**(不再被库覆盖后吞掉);
///   ② 回放后**复权**: 应用处置装回、库处理器退出;
///   ③ **宽限窗口(500ms)内库不夺走进程**: 应用 main 正常 return / 析构实例 ⇒
///      栈展开 ⇒ 话题控制面段被 unlink —— factory 路径的"残留 17 段"从优雅路径上消除;
///   ④ SIG_DFL(应用从未装处理器)**不回放** ⇒ 默认路径仍由库收尾
///      (exit(128+SIGINT)=130, UF-004 子项收口 2026-09-20; "不得信号硬杀"由
///      test_uf004 套件与本臂共同把守);
///   ⑤ RequestShutdown() 的库内部退出路径: 不回放不宽限, 行为与改前一致。
///
/// ⚠️ 每臂照 UF-004 的纪律在**子进程**里跑: 监控是进程级一次性的, 且默认路径
///    会 std::exit(0) 带走进程; 父进程只 fork/读回报/waitpid, 自身不碰 IPC。
/// ⚠️ 段的 unlink 发生在**实例析构**时(main return 的栈展开做的事), 因此臂 1
///    在子进程内依次观测: 处理器回放 → 显式 srv.reset()(等价于 main return 的
///    析构步)→ 逐名 stat 验证段已消失。判据取"进程退出状态 + 段计数",
///    不靠日志措辞。

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <dirent.h>
#include <poll.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "dzIPC/dzipc.h"
#include "ipc_msg/std_msgs/std_image.hpp"

#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;

int64_t remaining_ms(const std::chrono::steady_clock::time_point& deadline)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now())
        .count();
}

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
    bool timed_out{false};
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

/* 与 test_uf004 同骨架: 子进程跑 body(fd), body 自己 _exit(); 父进程读回报 + 收尸。 */
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
        ::_exit(90);
    }
    ::close(pfd[1]);

    const auto deadline = std::chrono::steady_clock::now() + 15000ms;
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
            break;
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

/* ---- 工厂路径(pub/sub 对 + IPC_SHM)+ 真实 publish: 探针实测建出 13 段
 * (队列/等待器/控制面), 与矩阵 factory 腿的残留构成同源; 段随实例析构回收。 */

/* 本进程映射的、名字含 tag 的 /dev/shm 段(读 /proc/self/maps —— 矩阵统计残留的同款口径)。 */
std::vector<std::string> mapped_tagged(const std::string& tag)
{
    std::vector<std::string> out;
    std::ifstream f("/proc/self/maps");
    std::string line;
    while (std::getline(f, line))
    {
        const std::size_t p = line.find("/dev/shm/");
        if (p == std::string::npos)
            continue;
        std::string name = line.substr(p + 9);
        while (!name.empty() && (name.back() == '\n' || name.back() == '\r'))
            name.pop_back();
        if (name.find(tag) != std::string::npos)
            out.push_back(name);
    }
    std::sort(out.begin(), out.end());
    out.erase(std::unique(out.begin(), out.end()), out.end());
    return out;
}

const std::string& run_tag()
{
    /* ⚠️ 不带前导斜杠: 段名里话题的前导 "/" 会被剥掉(dz_ipc_d0__ut_uf009_...),
     * 带斜杠的 tag 在 /proc/self/maps 与 /dev/shm 目录名里都永远匹配不上。 */
    static const std::string tag = "ut_uf009_" + std::to_string(static_cast<long>(::getpid())) + "_";
    return tag;
}

/* /dev/shm 下名字含 tag 的段数(父进程收尸后断言零残留用)。 */
int count_tagged_segments(const std::string& tag)
{
    DIR* d = ::opendir("/dev/shm");
    if (d == nullptr)
        return -1;
    int n = 0;
    while (struct dirent* e = ::readdir(d))
    {
        if (std::strstr(e->d_name, tag.c_str()) == nullptr)
            continue;
        ++n;
    }
    ::closedir(d);
    return n;
}

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
        std::printf("[UF-009] 清理本轮自建 /dev/shm 段: %d\n", removed);
}

constexpr int kMonitorExitWindowMs = 3000;   /* 默认路径 = 100ms 轮询 + 500ms 宽限, 留 6 倍裕量 */

void wait_monitor_or_report(int fd)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kMonitorExitWindowMs);
    while (remaining_ms(deadline) > 0)
        ::usleep(10 * 1000);
    report(fd, "monitor_did_not_exit=1\n");
    ::_exit(9);
}

/* ===================================================================== *
 * 臂 1(承重): 应用自有处理器 + 客户端握手建段 ⇒ 处理器被回放、段随实例
 * 析构被回收。场景与矩阵 factory 腿一致(server + client 握手建出话题控制面段)。
 *
 * 有牙判据(全部机器判, 不靠措辞):
 *   - handler_called=1 : 应用处理器被库回放(UF-009 核心);
 *   - total>0          : raise 前本进程确实映射着带 tag 的段(前提成立);
 *   - unlinked==total  : srv.reset()(= main return 的析构步)后逐名 stat,
 *                         段全部从 /dev/shm 消失 —— 残留 17 的源头被消除;
 *   - 进程 exit 0      : 库在宽限窗口内没有夺走进程(main 正常走完)。
 * ===================================================================== */
std::atomic<int> g_handled{0};

void uf009_on_sig(int) { g_handled.store(1, std::memory_order_relaxed); }

TEST(DzIpcUf009GracefulExit, GracefulPathReplaysHandlerAndUnlinksSegments)
{
    const std::string tag = run_tag();
    const ChildResult r = run_in_child(
        [tag](int fd)
        {
            std::signal(SIGINT, uf009_on_sig);
            std::signal(SIGTERM, SIG_IGN);   /* 本臂只触发 SIGINT; SIGTERM 保持 IGN 免串扰 */

            /* 工厂构造(触发监控安装) + pub/sub 对 + 真实 publish ⇒ 建出话题段族。
             * (ser-cli 握手不建段, 矩阵的 17 段随流量/订阅建立 —— 探针实测同源场景 13 段。) */
            const std::string topic = tag + "graceful";
            auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 9);
            auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 9);
            auto pub = dzIPC::PublisherIPCPtrMake(pub_td, topic, 0, dzIPC::IPC_SHM, false);
            auto sub = dzIPC::SubscriberIPCPtrMake(sub_td, topic, 0, 8, dzIPC::IPC_SHM, false);
            if (pub == nullptr || sub == nullptr)
            {
                report(fd, "ipc=0\n");
                ::_exit(8);
            }
            pub->InitChannel();
            sub->InitChannel();
            report(fd, "ipc=1\n");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            for (int i = 0; i < 3; ++i)
            {
                auto m = std::make_shared<dzIPC::Msg::StdImage>();
                m->set_msg_id(9);
                pub->publish(m);
                std::this_thread::sleep_for(std::chrono::milliseconds(30));
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            /* raise 前清点本进程映射的带 tag 段 —— 前提: total>0。 */
            char line[160];
            const std::vector<std::string> names = mapped_tagged(tag);
            const int total = static_cast<int>(names.size());
            std::snprintf(line, sizeof(line), "total=%d\n", total);
            report(fd, line);
            if (total <= 0)
                ::_exit(8);

            g_handled.store(0, std::memory_order_relaxed);
            std::raise(SIGINT);

            /* 等处理器被回放(库在宽限窗口内不会杀进程, 这个轮询没有竞态对手)。 */
            const auto deadline = std::chrono::steady_clock::now() + 3000ms;
            while (g_handled.load(std::memory_order_relaxed) == 0 && remaining_ms(deadline) > 0)
                ::usleep(5 * 1000);

            /* 析构步(main return 在栈展开时做的同一件事), 然后逐名 stat 验证。 */
            sub.reset();
            pub.reset();
            int unlinked = 0;
            for (const auto& n : names)
            {
                struct stat stb;
                if (::stat(("/dev/shm/" + n).c_str(), &stb) != 0)
                    ++unlinked;
            }
            std::snprintf(line, sizeof(line), "handler_called=%d unlinked=%d total=%d\n",
                          g_handled.load(std::memory_order_relaxed), unlinked, total);
            report(fd, line);
            ::_exit(0);
        });
    ASSERT_NE(r.log.find("ipc=1"), std::string::npos)
        << "前提不成立: 工厂没能建出 IPC 对象, 本臂无意义; " << describe(r);
    EXPECT_NE(r.log.find("handler_called=1"), std::string::npos)
        << "应用处理器必须在库宽限前被回放调用(UF-009 核心判据); " << describe(r);
    EXPECT_NE(r.log.find("unlinked="), std::string::npos) << describe(r);
    {
        /* unlinked == total 的机器判读(log: "handler_called=%d unlinked=%d total=%d") */
        const std::size_t u = r.log.find("unlinked=");
        const std::size_t t = r.log.find(" total=");
        if (u != std::string::npos && t != std::string::npos)
        {
            const int unlinked = std::atoi(r.log.c_str() + u + 9);
            const int total = std::atoi(r.log.c_str() + t + 7);
            EXPECT_GT(total, 0) << "前提: raise 前本进程必须映射着带 tag 的段; " << describe(r);
            EXPECT_EQ(unlinked, total) << "实例析构后段必须全部消失(残留 17 的源头); " << describe(r);
        }
    }
    EXPECT_TRUE(r.exited) << "宽限窗口内库不得夺走进程, main 必须正常走完; " << describe(r);
    EXPECT_EQ(r.code, 0) << describe(r);
    EXPECT_EQ(count_tagged_segments(tag), 0) << "进程结束后仍应零残留; " << describe(r);
    cleanup_own_shm_residue(tag);
}

/* ===================================================================== *
 * 臂 2: 应用从未装处理器(SIG_DFL)⇒ 不得回放 SIG_DFL, 仍由库收尾退出。
 * 把守 UF-004 的"默认路径不得信号硬杀": 若实现错把 SIG_DFL 也回放/复权, 信号会
 * 硬杀进程(被信号终止)而非库收尾; 库收尾退出码 128+SIGINT=130(UF-004 子项, 2026-09-20)。
 * ===================================================================== */
TEST(DzIpcUf009GracefulExit, SigDflPathStillLibraryManagedExit)
{
    const std::string tag = run_tag();
    const ChildResult r = run_in_child(
        [tag](int fd)
        {
            std::signal(SIGINT, SIG_DFL);
            auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 9);
            auto pub = dzIPC::PublisherIPCPtrMake(pub_td, tag + "sigdfl", 0, dzIPC::IPC_SHM, false);
            if (pub == nullptr)
            {
                report(fd, "ipc=0\n");
                ::_exit(8);
            }
            report(fd, "ipc=1\n");
            std::raise(SIGINT);
            wait_monitor_or_report(fd);
        });
    ASSERT_NE(r.log.find("ipc=1"), std::string::npos) << describe(r);
    EXPECT_TRUE(r.exited) << "SIG_DFL 场景不得回放: 必须仍由库收尾, 而非信号硬杀; " << describe(r);
    EXPECT_EQ(r.code, 130) << "退出码 128+SIGINT=130(UF-004 子项收口: 超时收尾如实上报信号); " << describe(r);
    cleanup_own_shm_residue(tag);
}

/* ===================================================================== *
 * 臂 3: RequestShutdown() 的库内部退出路径 —— 不回放不宽限, 行为与改前一致。
 * ===================================================================== */
TEST(DzIpcUf009GracefulExit, RequestShutdownPathUnchanged)
{
    const std::string tag = run_tag();
    const ChildResult r = run_in_child(
        [tag](int fd)
        {
            auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 9);
            auto pub = dzIPC::PublisherIPCPtrMake(pub_td, tag + "reqshutdown", 0, dzIPC::IPC_SHM, false);
            if (pub == nullptr)
            {
                report(fd, "ipc=0\n");
                ::_exit(8);
            }
            report(fd, "ipc=1\n");
            dzIPC::RequestShutdown();
            wait_monitor_or_report(fd);
        });
    ASSERT_NE(r.log.find("ipc=1"), std::string::npos) << describe(r);
    EXPECT_TRUE(r.exited) << "库内部退出路径必须保持原行为(立即收尾 exit(0)); " << describe(r);
    EXPECT_EQ(r.code, 0) << describe(r);
    cleanup_own_shm_residue(tag);
}

}   // namespace

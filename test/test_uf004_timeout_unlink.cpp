/// UF-004 保留面收口(2026-09-20)验收用例: ipc::shm::unlink_created_segments。
///
/// 收口行为(src/libipc/platform/posix/shm_posix.cpp + src/dzIPC/dzipc.cc):
///   ① acquire(create 模式)登记创建者; unlink_created_segments 按名字扫除本进程段;
///   ② attach-only(非创建者)段豁免: 只有创建者登记的名字会被扫;
///   ③ 原 acquire 语义逐位保留: 纯 create 模式对已存在段仍 EEXIST 失败,
///      default(create|open) 模式不存在时仍创建;
///   ④ dzipc 超时收尾路径(500ms 宽限耗尽, 栈不展开)扫除本进程建出的段 ——
///      factory 路径的"残留 17 段"不再出现在超时路径上;
///   ⑥ UF-004 子项收口(2026-09-20 第二批): 超时路径退出码 128+SIGINT=130(不再恒 0);
///      RegisterShutdownCallBack 回调在超时路径(signo=2)/RequestShutdown 路径(signo=0)
///      均于收尾最早时机被调, 异常不打断收尾;
///   ⑤ fork 继承的登记条目(pid 不符)不误扫: 父进程 fork 前建的段
///      不被子进程的超时收尾误删。
///
/// ⚠️ 臂 2 按 UF-004/UF-009 纪律在子进程里跑: 监控是进程级一次性的, 且超时路径
///    std::exit(0) 带走进程; 父进程只 fork/读回报/waitpid + 断言段计数。

#include <unistd.h>

#include <algorithm>
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
#include <fcntl.h>
#include <poll.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

#include "dzIPC/dzipc.h"
#include "ipc_msg/std_msgs/std_image.hpp"
#include "libipc/shm.h"

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

/* 与 test_uf009 同骨架: 子进程跑 body(fd), body 自己 _exit(); 父进程读回报 + 收尸。 */
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

/* 本进程映射的、名字含 tag 的 /dev/shm 段(读 /proc/self/maps)。 */
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

/* /dev/shm 下名字含 tag 的段(父进程断言零残留用; 名单一并返回便于失败定位)。 */
std::vector<std::string> tagged_names(const std::string& tag)
{
    std::vector<std::string> out;
    DIR* d = ::opendir("/dev/shm");
    if (d == nullptr)
        return out;
    while (struct dirent* e = ::readdir(d))
    {
        if (std::strstr(e->d_name, tag.c_str()) == nullptr)
            continue;
        out.push_back(e->d_name);
    }
    ::closedir(d);
    std::sort(out.begin(), out.end());
    return out;
}

int count_tagged_segments(const std::string& tag)
{
    return static_cast<int>(tagged_names(tag).size());
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
        std::printf("[UF-004-sweep] 清理本轮自建 /dev/shm 段: %d\n", removed);
}

/* ⚠️ 不带前导斜杠: 段名里前导 "/" 会被剥掉, 带斜杠 tag 匹配不上目录名。 */
const std::string& run_tag()
{
    static const std::string tag = "ut_uf004sw_" + std::to_string(static_cast<long>(::getpid())) + "_";
    return tag;
}

constexpr int kMonitorExitWindowMs = 3000;   /* 默认路径 = 100ms 轮询 + 500ms 宽限 + 扫除, 留 3 倍裕量 */

void wait_monitor_or_report(int fd)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(kMonitorExitWindowMs);
    while (remaining_ms(deadline) > 0)
        ::usleep(10 * 1000);
    report(fd, "monitor_did_not_exit=1\n");
    ::_exit(9);
}

/* ===================================================================== *
 * 臂 1(进程内, 直测 API): 扫创建者段、attach-only 豁免、幂等、
 * 原 acquire 语义保留。
 * ===================================================================== */
TEST(Uf004TimeoutUnlink, SweepUnlinksCreatedKeepsAttachedAndPreservesSemantics)
{
    const std::string tag = run_tag();
    const std::string created_name = tag + "created_seg";
    const std::string attached_name = tag + "attached_seg";
    ::shm_unlink(("/" + created_name).c_str());
    ::shm_unlink(("/" + attached_name).c_str());

    /* ① default(create|open) 建段 → 创建者, 登记。 */
    ipc::shm::id_t created = ipc::shm::acquire(created_name.c_str(), 64);
    ASSERT_NE(created, nullptr) << "前提: default 模式建段必须成功";

    /* ② 非 libipc 原生建段 + default 模式 attach-only → 不登记。 */
    {
        const int raw = ::shm_open(("/" + attached_name).c_str(), O_RDWR | O_CREAT, S_IRUSR | S_IWUSR);
        ASSERT_GE(raw, 0);
        ::close(raw);
    }
    ipc::shm::id_t attached = ipc::shm::acquire(attached_name.c_str(), 64);
    ASSERT_NE(attached, nullptr) << "前提: attach-only 必须成功";

    /* ③ 原语义保留: 纯 create 模式对已存在段仍须失败。 */
    errno = 0;
    EXPECT_EQ(ipc::shm::acquire(attached_name.c_str(), 64, ipc::shm::create), nullptr)
        << "纯 create 模式必须保持\"存在即失败\"语义";

    /* ④ 扫除: 创建者段名消失、attach-only 段存活、二次扫除幂等。 */
    const std::size_t swept = ipc::shm::unlink_created_segments();
    EXPECT_EQ(swept, static_cast<std::size_t>(1)) << "本进程名单应恰有一条登记";
    struct stat stb;
    EXPECT_NE(::stat(("/dev/shm/" + created_name).c_str(), &stb), 0)
        << "创建者段名必须被扫除";
    EXPECT_EQ(::stat(("/dev/shm/" + attached_name).c_str(), &stb), 0)
        << "attach-only(非创建者)段不得被误扫";
    EXPECT_EQ(ipc::shm::unlink_created_segments(), static_cast<std::size_t>(0))
        << "扫除必须幂等";

    /* 清理 */
    ipc::shm::release(created);
    ipc::shm::release(attached);
    ::shm_unlink(("/" + attached_name).c_str());
}

/* ===================================================================== *
 * 臂 2(承重, fork): dzipc 超时收尾路径(SIG_DFL 不回放 → 宽限耗尽 →
 * 扫除 + exit(0))。
 *   - 子进程建的段被扫: factory 路径在超时路径上零残留;
 *   - 父进程 fork 前建的段(pid 守卫)不被子进程扫除误删。
 * ===================================================================== */
TEST(Uf004TimeoutUnlink, TimeoutPathSweepsCreatedAndRespectsForkInherited)
{
    const std::string parent_tag = run_tag();
    const std::string parent_seg = parent_tag + "parent_owned";
    ::shm_unlink(("/" + parent_seg).c_str());
    ipc::shm::id_t pseg = ipc::shm::acquire(parent_seg.c_str(), 64);
    ASSERT_NE(pseg, nullptr) << "前提: 父进程建段(登记)必须成功";

    const ChildResult r = run_in_child(
        [](int fd)
        {
            /* ⚠️ 不能用 run_tag(): 其 static 在父进程 fork 前已初始化, 子进程继承后会与父 tag 同串,
             * 父段名就会落进子进程的零残留检查。子进程内现算自己的 pid。 */
            const std::string child_tag =
                "ut_uf004sw_" + std::to_string(static_cast<long>(::getpid())) + "_";
            std::signal(SIGINT, SIG_DFL);              /* 应用不装处理器 → 库超时路径 */

            auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 9);
            auto pub = dzIPC::PublisherIPCPtrMake(pub_td, child_tag + "tmo", 0, dzIPC::IPC_SHM, false);
            if (pub == nullptr)
            {
                report(fd, "ipc=0\n");
                ::_exit(8);
            }
            pub->InitChannel();
            report(fd, "ipc=1\n");
            std::this_thread::sleep_for(std::chrono::milliseconds(200));
            pub->publish(std::make_shared<dzIPC::Msg::StdImage>());   /* 物化段族 */
            std::this_thread::sleep_for(std::chrono::milliseconds(200));

            /* 前提: 本进程确实映射着带 tag 的段(否则零残留判据空转)。 */
            const int mapped = static_cast<int>(mapped_tagged(child_tag).size());
            char line[128];
            std::snprintf(line, sizeof(line), "mapped=%d\n", mapped);
            report(fd, line);
            if (mapped <= 0)
                ::_exit(8);
            std::snprintf(line, sizeof(line), "tag=%s\n", child_tag.c_str());
            report(fd, line);

            std::raise(SIGINT);
            wait_monitor_or_report(fd);   /* 超时路径: 回调+扫除 + exit(130); 监控未退 → _exit(9) */
        });

    ASSERT_NE(r.log.find("ipc=1"), std::string::npos)
        << "前提不成立: 工厂没能建出 IPC 对象, 本臂无意义; " << describe(r);

    /* 前提: raise 前子进程确实映射着带 tag 的段。 */
    {
        const std::size_t p = r.log.find("mapped=");
        ASSERT_NE(p, std::string::npos) << describe(r);
        const int mapped = std::atoi(r.log.c_str() + p + 7);
        ASSERT_GT(mapped, 0) << "前提: 子进程必须映射着带 tag 的段; " << describe(r);
    }

    EXPECT_TRUE(r.exited) << "库超时路径必须完成收尾退出; " << describe(r);
    EXPECT_EQ(r.code, 130) << "退出码 128+SIGINT=130(UF-004 子项收口); " << describe(r);

    /* 取子进程回报的 tag, 断言零残留。 */
    std::string child_tag;
    {
        const std::size_t p = r.log.find("tag=");
        ASSERT_NE(p, std::string::npos) << describe(r);
        const std::size_t e = r.log.find('\n', p);
        child_tag = r.log.substr(p + 4, (e == std::string::npos ? std::string::npos : e - p - 4));
    }
    {
        const std::vector<std::string> leftover = tagged_names(child_tag);
        std::string names;
        for (const auto& n : leftover)
            names += " [" + n + "]";
        EXPECT_TRUE(leftover.empty())
            << "超时收尾后本进程(子进程)create 建出的段必须被名字级扫除 —— UF-004 保留面; 残留:" << names
            << "; " << describe(r);
    }

    /* pid 守卫: 父进程 fork 前建的段不得被子进程的超时扫除误删。 */
    struct stat stb;
    EXPECT_EQ(::stat(("/dev/shm/" + parent_seg).c_str(), &stb), 0)
        << "父进程 fork 前建的段不得被子进程超时收尾误删(fork 继承条目不扫); " << describe(r);

    ipc::shm::release(pseg);
    ::shm_unlink(("/" + parent_seg).c_str());
    cleanup_own_shm_residue(parent_tag);
    cleanup_own_shm_residue(child_tag);
}

/* ===================================================================== *
 * 臂 3: UF-004 收尾回调钩子 —— 超时路径上回调被调(signo=实际信号)、
 * 退出码 128+SIGINT=130。
 * ===================================================================== */
TEST(Uf004TimeoutUnlink, ShutdownCallBackFiresOnTimeoutPathWithSignalNumber)
{
    const std::string tag = run_tag();
    const ChildResult r = run_in_child(
        [tag](int fd)
        {
            const std::string child_tag =
                "ut_uf004sw_" + std::to_string(static_cast<long>(::getpid())) + "_";
            std::signal(SIGINT, SIG_DFL);   /* 应用不装处理器 → 库超时路径 */

            dzIPC::RegisterShutdownCallBack([fd](int signo)
            {
                char line[64];
                std::snprintf(line, sizeof(line), "cb_timeout=1 signo=%d\n", signo);
                report(fd, line);
            });

            auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 9);
            auto pub = dzIPC::PublisherIPCPtrMake(pub_td, child_tag + "cbtno", 0, dzIPC::IPC_SHM, false);
            if (pub == nullptr)
            {
                report(fd, "ipc=0\n");
                ::_exit(8);
            }
            pub->InitChannel();
            report(fd, "ipc=1\n");
            std::raise(SIGINT);
            wait_monitor_or_report(fd);
        });
    ASSERT_NE(r.log.find("ipc=1"), std::string::npos) << describe(r);
    EXPECT_NE(r.log.find("cb_timeout=1 signo=2"), std::string::npos)
        << "超时路径回调必须被调且带实际信号号(SIGINT=2); " << describe(r);
    EXPECT_TRUE(r.exited) << "回调返回后收尾必须继续完成; " << describe(r);
    EXPECT_EQ(r.code, 130) << "退出码 128+SIGINT; " << describe(r);
    cleanup_own_shm_residue(tag);
}

/* ===================================================================== *
 * 臂 4: RequestShutdown 内部收尾路径 —— 回调被调且 signo=0、退出码保持 0。
 * ===================================================================== */
TEST(Uf004TimeoutUnlink, ShutdownCallBackFiresOnRequestShutdownPathWithSignoZero)
{
    const std::string tag = run_tag();
    const ChildResult r = run_in_child(
        [tag](int fd)
        {
            std::signal(SIGINT, SIG_DFL);
            dzIPC::RegisterShutdownCallBack([fd](int signo)
            {
                char line[64];
                std::snprintf(line, sizeof(line), "cb_req=1 signo=%d\n", signo);
                report(fd, line);
            });
            auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 9);
            auto pub = dzIPC::PublisherIPCPtrMake(pub_td, tag + "cbreq", 0, dzIPC::IPC_SHM, false);
            if (pub == nullptr)
            {
                report(fd, "ipc=0\n");
                ::_exit(8);
            }
            pub->InitChannel();
            report(fd, "ipc=1\n");
            dzIPC::RequestShutdown();
            wait_monitor_or_report(fd);
        });
    ASSERT_NE(r.log.find("ipc=1"), std::string::npos) << describe(r);
    EXPECT_NE(r.log.find("cb_req=1 signo=0"), std::string::npos)
        << "RequestShutdown 内部收尾回调必须被调且 signo=0(非信号死亡); " << describe(r);
    EXPECT_TRUE(r.exited) << describe(r);
    EXPECT_EQ(r.code, 0) << "库内部退出路径退出码保持 0; " << describe(r);
    cleanup_own_shm_residue(tag);
}

}   // namespace

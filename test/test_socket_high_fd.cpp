/* [W07] socket 容量与高 fd 边界回归（验收: 千路有效注册并实际收发 / 高 fd 不崩溃）。
 *
 * ── 为什么这两组判据是承重的（不是"跑绿即过"）────────────────────────────
 * ① **高 fd 不崩溃**。`UDPNode::receive(tm)` 原先用 `select` + `FD_SET` 等一个 fd;
 *    `FD_SET` 是定长位图, fd >= FD_SETSIZE(1024) 时走 glibc 的 `__fdelt_chk`
 *    ⇒ "*** buffer overflow detected ***" + SIGABRT。socket 侧每个订阅者约 2 个 fd,
 *    千路订阅即 ~2000 个 fd。gdb 栈已钉死在 `__fdelt_chk ← ipc::socket::UDPNode::receive
 *    ← recv_chunk_common_impl`（test/perf/out/20260927_t6_fd/head_511_gdb.log）。
 *    本文件先用 `/dev/null` **预占 fd** 把随后的 socket fd 顶到 1024 之上, 再走真实
 *    订阅/发布往返: 崩了父进程看到的是 WIFSIGNALED, 不会被"子进程没输出"糊过去。
 *
 * ② **1000 路有效注册**。注册表(`IpcInfoPool`)容量与接收线程数**无关**, 所以判据取
 *    「本进程 SocketSub 有效条目数 == 1000」, 而不是"接收线程变少了"——后者正是本工作包
 *    要避免的替代验证。基线 `kMaxEntries=512` 时第 513 路起注册失败
 *    (`[dzIPC][info_pool] register_entry 失败: 表满...`), 千路根本注册不满。
 *
 * ── 为什么每个场景 fork 一个子进程 ────────────────────────────────────
 * `DZIPC_SOCKET_COMPAT_THREAD` 与 recv 池的 owner pid 都是**进程内只读一次**的静态量,
 * 中途 setenv 无效; 且 fd 预占会污染同进程后续用例。故一个场景一个全新子进程,
 * 结果经管道回传, 判据在父进程里断言。
 *
 * 平台: 组播不可用时 GTEST_SKIP, 不假绿(同 test_socket_wait_set.cpp 的手法)。
 */
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dzIPC/common/hash.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/socket_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_image.hpp"
#include "libipc/udp.h"

#include <gtest/gtest.h>

namespace {

using namespace std::chrono_literals;

/* 与 test/perf/out/20260927_t6_probes/t6scale.cpp 同模板 id —— 探针与回归用同一条
 * 消息类型, 免得"探针能过、回归不过"是模板差异造成的。 */
constexpr std::uint32_t kMsgId = 37;

constexpr int kCompatDomain = 71;   ///< 与既有 socket 用例不同的 domain, 避免串扰
constexpr int kWorkerDomain = 72;
constexpr int kScaleDomain = 73;

std::size_t open_fd_count(std::size_t* max_fd = nullptr)
{
    DIR* d = ::opendir("/proc/self/fd");
    if (d == nullptr)
    {
        if (max_fd) *max_fd = 0;
        return 0;
    }
    std::size_t n = 0;
    std::size_t mx = 0;
    while (dirent* e = ::readdir(d))
    {
        if (e->d_name[0] == '.') continue;
        ++n;
        const long v = std::strtol(e->d_name, nullptr, 10);
        if (v > 0 && static_cast<std::size_t>(v) > mx) mx = static_cast<std::size_t>(v);
    }
    ::closedir(d);
    if (max_fd) *max_fd = mx;
    return n;
}

std::size_t max_open_fd()
{
    std::size_t mx = 0;
    open_fd_count(&mx);
    return mx;
}

/* 预占 fd: 用 /dev/null 只吃 fd 号(不占内存、不产生事件), 直到某个 fd 号 >= target,
 * 于是随后新建的 socket 必然拿到 target 之上的 fd 号。析构全部归还。 */
struct FdHog
{
    std::vector<int> fds;

    bool occupy_above(std::size_t target)
    {
        for (;;)
        {
            const int fd = ::open("/dev/null", O_RDONLY);
            if (fd < 0) return false;
            fds.push_back(fd);
            if (static_cast<std::size_t>(fd) >= target) return true;
        }
    }

    ~FdHog()
    {
        for (int fd : fds) ::close(fd);
    }
    FdHog() = default;
    FdHog(const FdHog&) = delete;
    FdHog& operator=(const FdHog&) = delete;
};

struct ChildRun
{
    bool exited{false};
    int code{-1};
    int signal{0};
    std::string text;
};

/* fork 子进程执行 body, 把它写到管道里的文本回传给父进程。 */
ChildRun run_child(const std::function<void(int)>& body)
{
    int fds[2];
    if (::pipe(fds) != 0) return {};
    const ::pid_t pid = ::fork();
    if (pid < 0)
    {
        ::close(fds[0]);
        ::close(fds[1]);
        return {};
    }
    if (pid == 0)
    {
        ::close(fds[0]);
        int rc = 1;
        try
        {
            body(fds[1]);
            rc = 0;
        }
        catch (...)
        {
            rc = 9;
        }
        /* ⛔ _exit 而非 return: 子进程是 gtest 进程的 fork, 走 static 析构会把
         * gtest/atexit 的处理器再跑一遍。 */
        ::_exit(rc);
    }
    ::close(fds[1]);

    ChildRun r;
    char buf[2048];
    ssize_t n = 0;
    while ((n = ::read(fds[0], buf, sizeof(buf))) > 0)
    {
        r.text.append(buf, static_cast<std::size_t>(n));
    }
    ::close(fds[0]);

    int status = 0;
    ::waitpid(pid, &status, 0);
    if (WIFEXITED(status))
    {
        r.exited = true;
        r.code = WEXITSTATUS(status);
    }
    else if (WIFSIGNALED(status))
    {
        r.signal = WTERMSIG(status);
    }
    return r;
}

void emit(int fd, const std::string& line)
{
    const std::string s = line + "\n";
    ssize_t off = 0;
    while (off < static_cast<ssize_t>(s.size()))
    {
        const ssize_t w = ::write(fd, s.data() + off, s.size() - static_cast<std::size_t>(off));
        if (w <= 0) return;
        off += w;
    }
}

/* 从 "k=v k=v" 里取一个值; 找不到返回空串。 */
std::string kv(const std::string& text, const std::string& key)
{
    const std::string pat = key + "=";
    const std::size_t p = text.find(pat);
    if (p == std::string::npos) return {};
    const std::size_t b = p + pat.size();
    const std::size_t e = text.find_first_of(" \n", b);
    return text.substr(b, e == std::string::npos ? std::string::npos : e - b);
}

/* 组播可用性探测: 不可用时用例 SKIP, 而不是让"没送出去"冒充"没崩"。 */
bool multicast_available(const std::string& topic, int domain)
{
    const std::string ip = dzIPC::common::udp_discovery_addr_calculate(topic);
    const std::uint16_t port =
        dzIPC::common::udp_discovery_port_calculate(topic, static_cast<std::size_t>(domain));
    ipc::socket::UDPNode node("w07_probe", ip.c_str(), port, ipc::socket::NodeRole::RecvOnly);
    return node.connect();
}

/* ---- 场景 A: 预占 fd 使 socket fd 落在 FD_SETSIZE 之上, 仍要能真实收发 ------ */
void high_fd_round_trip_child(int out, bool compat)
{
    if (compat)
    {
        ::setenv("DZIPC_SOCKET_COMPAT_THREAD", "1", 1);
    }
    else
    {
        ::unsetenv("DZIPC_SOCKET_COMPAT_THREAD");
    }

    const int domain = compat ? kCompatDomain : kWorkerDomain;
    const std::string topic = compat ? "w07_fd_compat" : "w07_fd_worker";

    if (!multicast_available("w07_fd_probe", domain))   /* 探测在预占之前: 少一个 fd 干扰 */
    {
        emit(out, "multicast=0");
        return;
    }

    FdHog hog;
    const bool hogged = hog.occupy_above(static_cast<std::size_t>(FD_SETSIZE) + 32);

    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::socket::socket_sub_ipc sub{sub_td, topic, static_cast<std::size_t>(domain), 64, false};
    sub.InitChannel("w07_highfd");

    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::socket::socket_pub_ipc pub{pub_td, topic, static_cast<std::size_t>(domain), false};
    pub.InitChannel("w07_highfd");

    std::this_thread::sleep_for(300ms);

    constexpr int kMsgs = 10;
    int sent = 0;
    for (int i = 0; i < kMsgs; ++i)
    {
        auto m = std::make_shared<dzIPC::Msg::StdImage>();
        m->set_msg_id(kMsgId);
        m->width = 8;
        m->height = 8;
        m->step = 24;
        m->encoding = "rgb8";
        m->data.assign(8 * 8 * 3, static_cast<std::uint8_t>(i));
        if (pub.publish(m)) ++sent;
        std::this_thread::sleep_for(5ms);
    }
    std::this_thread::sleep_for(600ms);

    int received = 0;
    dzIPC::Sample sample;
    while (sub.try_get(sample)) ++received;
    auto sink = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    while (sub.try_get_clone(sink)) ++received;

    emit(out, "multicast=1 hogged=" + std::to_string(hogged ? 1 : 0) + " maxfd=" +
                  std::to_string(max_open_fd()) + " sent=" + std::to_string(sent) +
                  " received=" + std::to_string(received));
}

/* ---- 场景 B: 1000 路独立订阅——有效注册数 + 实际收发 ---------------------- */
void thousand_lane_child(int out)
{
    ::unsetenv("DZIPC_SOCKET_COMPAT_THREAD");   /* 默认(固定 worker)路径 */

    constexpr int kScale = 1000;
    const std::string topic = "w07_scale";
    const std::size_t domain = static_cast<std::size_t>(kScaleDomain);

    if (!multicast_available("w07_scale_probe", kScaleDomain))
    {
        emit(out, "multicast=0");
        return;
    }

    std::vector<std::unique_ptr<dzIPC::socket::socket_sub_ipc>> subs;
    subs.reserve(kScale);
    for (int i = 0; i < kScale; ++i)
    {
        auto td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
        subs.push_back(std::make_unique<dzIPC::socket::socket_sub_ipc>(
            td, topic, domain, static_cast<std::size_t>(64), false));
        subs.back()->InitChannel("w07_scale");
    }

    /* 有效注册判据: 本进程**真实在册**的 SocketSub 条目数。 */
    std::size_t registered = 0;
    for (const auto& e : dzIPC::info_pool::IpcInfoPool::instance().snapshot(false))
    {
        if (e.pid == static_cast<int32_t>(::getpid()) &&
            e.kind == dzIPC::info_pool::EntryKind::SocketSub)
        {
            ++registered;
        }
    }

    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    dzIPC::socket::socket_pub_ipc pub{pub_td, topic, domain, false};
    pub.InitChannel("w07_scale");

    std::this_thread::sleep_for(300ms);

    constexpr int kMsgs = 20;
    int sent = 0;
    for (int i = 0; i < kMsgs; ++i)
    {
        auto m = std::make_shared<dzIPC::Msg::StdImage>();
        m->set_msg_id(kMsgId);
        m->width = 4;
        m->height = 4;
        m->step = 12;
        m->encoding = "rgb8";
        m->data.assign(4 * 4 * 3, static_cast<std::uint8_t>(i));
        if (pub.publish(m)) ++sent;
        std::this_thread::sleep_for(5ms);
    }
    std::this_thread::sleep_for(900ms);

    int received = 0;
    dzIPC::Sample sample;
    while (subs[0]->try_get(sample)) ++received;
    auto sink = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
    while (subs[0]->try_get_clone(sink)) ++received;

    emit(out, "multicast=1 scale=" + std::to_string(kScale) + " registered=" +
                  std::to_string(registered) + " maxfd=" + std::to_string(max_open_fd()) +
                  " openfds=" + std::to_string(open_fd_count()) + " sent=" + std::to_string(sent) +
                  " received=" + std::to_string(received));
}

std::string child_text_or_skip(const ChildRun& r)
{
    return r.text;
}

}   // namespace

/* ① 高 fd 不崩溃 + 预占 fd 后仍能真实收发（固定 worker 路径）。
 * 基线: FD_SET(fd>=1024) ⇒ SIGABRT(6); 本用例把信号显式报出来。 */
TEST(SocketHighFd, RoundTripWithFdAboveFdSetSizeWorkerPath)
{
    const ChildRun r = run_child([](int out) { high_fd_round_trip_child(out, false); });
    ASSERT_TRUE(r.exited) << "子进程未正常退出: signal=" << r.signal
                          << " (6=SIGABRT, 即 FD_SET/__fdelt_chk 崩) text=" << r.text;
    ASSERT_EQ(r.code, 0);
    if (kv(r.text, "multicast") == "0") GTEST_SKIP() << "组播不可用";
    std::printf("[W07 worker-path] %s", r.text.c_str());

    EXPECT_EQ(kv(r.text, "hogged"), "1") << "fd 预占失败, 本用例失去意义: " << r.text;
    const long maxfd = std::strtol(kv(r.text, "maxfd").c_str(), nullptr, 10);
    EXPECT_GT(maxfd, static_cast<long>(FD_SETSIZE))
        << "socket fd 没有超过 FD_SETSIZE, 未覆盖高 fd 边界: " << r.text;
    EXPECT_EQ(kv(r.text, "sent"), "10") << r.text;
    EXPECT_EQ(kv(r.text, "received"), "10") << r.text;
}

/* ①' 同上, 但走兼容 per-subscription 接收线程(socket wait-set 不可用时的回退路径)。 */
TEST(SocketHighFd, RoundTripWithFdAboveFdSetSizeCompatPath)
{
    const ChildRun r = run_child([](int out) { high_fd_round_trip_child(out, true); });
    ASSERT_TRUE(r.exited) << "子进程未正常退出: signal=" << r.signal
                          << " (6=SIGABRT, 即 FD_SET/__fdelt_chk 崩) text=" << r.text;
    ASSERT_EQ(r.code, 0);
    if (kv(r.text, "multicast") == "0") GTEST_SKIP() << "组播不可用";
    std::printf("[W07 compat-path] %s", r.text.c_str());

    EXPECT_EQ(kv(r.text, "hogged"), "1") << r.text;
    const long maxfd = std::strtol(kv(r.text, "maxfd").c_str(), nullptr, 10);
    EXPECT_GT(maxfd, static_cast<long>(FD_SETSIZE)) << r.text;
    EXPECT_EQ(kv(r.text, "sent"), "10") << r.text;
    EXPECT_EQ(kv(r.text, "received"), "10") << r.text;
}

/* ② 1000 路有效注册并实际收发。
 * 基线会同时踩两个坑: 第 513 路起注册失败(kMaxEntries=512), 且 ~2068 个 fd 触发
 * FD_SET 崩溃 —— 两个都在这里被钉住。 */
TEST(SocketHighFd, ThousandLanesRegisterAndRoundTrip)
{
    const ChildRun r = run_child([](int out) { thousand_lane_child(out); });
    ASSERT_TRUE(r.exited) << "子进程未正常退出: signal=" << r.signal << " text=" << r.text;
    ASSERT_EQ(r.code, 0);
    if (kv(r.text, "multicast") == "0") GTEST_SKIP() << "组播不可用";
    std::printf("[W07 1000-lane] %s", r.text.c_str());

    EXPECT_EQ(kv(r.text, "registered"), "1000")
        << "1000 路有效注册未达成(注册表容量或注册失败): " << r.text;
    const long maxfd = std::strtol(kv(r.text, "maxfd").c_str(), nullptr, 10);
    EXPECT_GT(maxfd, static_cast<long>(FD_SETSIZE))
        << "千路订阅应产生 >FD_SETSIZE 的 fd, 否则未覆盖高 fd: " << r.text;
    EXPECT_EQ(kv(r.text, "sent"), "20") << r.text;
    EXPECT_EQ(kv(r.text, "received"), "20") << r.text;
}

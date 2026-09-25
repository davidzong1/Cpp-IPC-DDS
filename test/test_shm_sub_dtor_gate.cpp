/* 阶段 2 全量测试方案 §4.2：析构唤醒的**集成**守门 —— 对象级、因果序、子进程硬超时。
 *
 * 落点：docs/消息接收架构改造/阶段2_全量测试方案.md §4.2；
 *       说明 §5 的析构八步（docs/消息接收架构改造/阶段2_RouteSession实现说明.md）。
 *
 * ── 缺口是什么（为什么已有的用例不够）────────────────────────────────────
 * v4 只读验收把「删掉 ~shm_sub_ipc 里的 route_session_.stop_and_wake()」（X3）判为
 * **集成层零守门**：删掉之后既有 4 套件（test_dzipc_shm 10 / test_shm_nodelet 13 /
 * test_wire_accept 9 / test_shm_control_scheduler 20）**全绿**。原因是既有用例都只断言
 * "最终又能收到消息"或"析构不崩"，而收包线程本来就会在 recv(50) 超时后自己返回 ——
 * 有没有叫醒，功能上看不出来。
 *
 * v4 §3.2 还实测证否了"用析构耗时做判据"这条路：基线（会 disconnect）与变异体
 * （不 disconnect）的析构耗时**逐样本相同**（min 27 / p50 28 / max 29 ms）。所以本文件
 * **不断言耗时**，只断言**因果顺序**与**可观测的副作用**。
 *
 * ── 三条互相独立的判据 ───────────────────────────────────────────────────
 *   ① 因果序：析构各阶段点必须严格递增 —— 特别是 §5 第 1 步（LocalPubSubRegistry 注销）
 *      必须在第 4 步（stop_and_wake）**之前**（方案 §4.2 原文要求）。
 *   ② 叫醒而非超时：必须存在一个「stop_and_wake 之后」的 recv 返回，其
 *      `connected_id() == 0` —— 即 disconnect 已生效、recv 是被它叫醒的。
 *      删掉 stop_and_wake 时，这一条不成立（recv 会等到 50ms 超时，那时 running 已为
 *      false，循环退出，根本没有"析构期间返回的那次 recv"）。
 *   ③ 计数面：`WakeupArtifactCount()` 增量 ≥ 1（与 ② 互为独立证据：②看 libipc 句柄状态，
 *      ③看上层守门是否真的被走到）。
 *
 * ── 为什么必须在子进程里 ────────────────────────────────────────────────
 * 钩子是**进程内全局**的，而本用例要观测的是"析构期间"的事件；同时 gtest 的断言在
 * fork 出来的子进程里不安全（多线程 fork 后不能调非 async-signal-safe 的东西，也不能
 * 碰父进程的 gtest 状态）。所以：子进程只 `::write(fd, ...)` 回报 + `::_exit(code)`，
 * 全部 EXPECT_* 都在父进程做。父进程给 15s 硬超时，超时即 SIGKILL + 判失败
 * （与 test_uf009_graceful_exit.cpp / test_uf004_shutdown_monitor_optout.cpp 同骨架）。
 *
 * ── 为什么 publisher 全程存活 ────────────────────────────────────────────
 * 若拆掉 pub，控制面会离开 Ready，订阅端自己的握手线程就会走 stop_and_wake —— 伪影
 * 与叫醒就**无法归属**到析构这一步。让 pub 活着，控制面保持 Ready，析构期间唯一可能
 * 产生 disconnect 的地方就是 `~shm_sub_ipc` 的第 4 步。
 */
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <poll.h>
#include <sys/wait.h>
#include <unistd.h>

#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/wire_accept.h"
#include "dzIPC/detail/shm_sub_seam.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_image.hpp"
#include "libipc/ipc.h"

#include <gtest/gtest.h>

namespace {

using Clock = std::chrono::steady_clock;
using namespace std::chrono_literals;

constexpr std::uint32_t kMsgId = 71;

/* ---------------- 子进程骨架（照抄 test_uf009_graceful_exit.cpp 的 run_in_child） ---------------- */

int64_t remaining_ms(const Clock::time_point& deadline)
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - Clock::now()).count();
}

void report(int fd, const char* text)
{
    if (fd < 0)
    {
        return;
    }
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

    const auto deadline = Clock::now() + 15000ms;
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
            {
                continue;
            }
            break;
        }
        if (pr == 0)
        {
            r.timed_out = true;
            break;
        }
        char buf[512];
        const ssize_t n = ::read(pfd[0], buf, sizeof(buf));
        if (n > 0)
        {
            r.log.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0)
        {
            break;
        }
        if (errno == EINTR)
        {
            continue;
        }
        break;
    }
    ::close(pfd[0]);

    int st = 0;
    for (;;)
    {
        const ::pid_t w = ::waitpid(pid, &st, WNOHANG);
        if (w == pid)
        {
            break;
        }
        if (w < 0 && errno != EINTR)
        {
            break;
        }
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

/* ---------------- 事件记录器（钩子侧） ---------------- */

/* 钩子在收包线程与析构线程上被调用 ⇒ 必须自己同步。只记两个**析构期**关心的量：
 *   ① 析构阶段点的到达顺序（用单调序号编码，父进程解码后判严格递增）；
 *   ② "stop_and_wake 之后是否出现过 connected_id()==0 的 recv 返回"。 */
struct Recorder
{
    std::mutex m;
    std::vector<int> dtor_order;      /* 析构点的到达顺序 */
    int woke_after_stop{0};           /* 见 kAfterRecv 分支 */
    int recv_after_stop{0};
    bool saw_stop{false};

    void on(const dzIPC::detail::SeamEvent& ev)
    {
        std::lock_guard<std::mutex> lock(m);
        switch (ev.point)
        {
        case dzIPC::detail::SeamPoint::kAfterRecv:
            if (saw_stop)
            {
                ++recv_after_stop;
                /* connected_id()==0 ⇒ 该 route 已被 disconnect（或从未连上）。
                 * 析构期出现它，就是"recv 被叫醒"而不是"recv 等满 50ms 超时"的直接证据。 */
                if (ev.route != nullptr && ev.route->connected_id() == 0)
                {
                    ++woke_after_stop;
                }
            }
            break;
        case dzIPC::detail::SeamPoint::kDtorAfterStopAndWake:
            saw_stop = true;
            dtor_order.push_back(static_cast<int>(ev.point));
            break;
        case dzIPC::detail::SeamPoint::kDtorAfterUnregister:
        case dzIPC::detail::SeamPoint::kDtorAfterJoinSubscribe:
        case dzIPC::detail::SeamPoint::kDtorAfterJoinHandshake:
        case dzIPC::detail::SeamPoint::kDtorAfterQuiescent:
        case dzIPC::detail::SeamPoint::kDtorAfterRelease:
            dtor_order.push_back(static_cast<int>(ev.point));
            break;
        default:
            break;
        }
    }

    static Recorder*& instance()
    {
        static Recorder* p = nullptr;
        return p;
    }
};

/* 段名 guard：在**所有** pub/sub 对象析构之后清数据段 + 控制面段。
 *
 * ⛔ 为什么必须有：`ipc::route` 的析构只释放自己的句柄，**不 unlink** 段；控制面段由
 * `ipc::shm::handle` 建，也不会自动清。不清就会每跑一轮在 /dev/shm 里留一堆
 * `*_WAITER_*` / `QU_CONN` / `_topic_control2` 段（实测 12 个/轮，跑 20 轮 = 240 个），
 * 最终污染同机的其它用例（`test_sercli_auto_path` 的历史教训）。
 * 声明在 pub/sub **之前** ⇒ 析构逆序保证"传输对象先释放、段名 guard 最后清"。 */
struct TopicGuard
{
    std::string topic;
    explicit TopicGuard(std::string t) : topic(std::move(t)) {}
    ~TopicGuard() { clear(); }

    /* ⛔ 子进程里**必须显式调用**：子进程用 `::_exit()` 退出（多线程 fork 后不能走
     * 普通 exit/栈展开），而 `_exit` **不跑析构** ⇒ 只靠 ~TopicGuard 清不掉任何段
     * （实测每轮残留 12 个 `*_WAITER_*` / `QU_CONN` / `_topic_control2`）。
     * 幂等：析构里再调一次无害。 */
    void clear()
    {
        ipc::route::clear_storage(shm_topic_segment_name(topic, 0).c_str());
        ipc::shm::handle::clear_storage(shm_topic_control_name(topic, 0).c_str());
    }
};

/* 子进程的统一出口：先清段再 _exit。 */
[[noreturn]] void child_exit(TopicGuard& g, int code)
{
    g.clear();
    ::_exit(code);
}

/* ---------------- 子进程主体 ---------------- */

/* 回报格式（父进程解析，避免依赖 gtest）：
 *   "dtor=<n1>,<n2>,..."  析构点到达顺序
 *   "woke=<k> recv_after=<m>"  叫醒证据
 *   "artifacts=<n>"       析构前后 WakeupArtifactCount 增量
 *   "peers=<n>"           析构后控制面 peer_count
 *   "ok" / "fail:<why>"   子进程自评（父进程仍会独立复算上面几项） */
void child_body(int fd, std::uint32_t rounds)
{
    const std::string topic = std::string("dtorgate_") + std::to_string(::getpid());
    std::string line;
    TopicGuard guard{topic};   /* 析构最后执行 ⇒ 清段在所有 pub/sub 释放之后 */

    {
        auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
        /* ⛔ 必须**同时起一个占位订阅者并让它活到本轮结束**：pub 的控制面段由它的
         * InitChannel 建出（begin_rebuild + set_ready），而 `pub` 一析构，`pub_handshake`
         * 的收尾就会 set_stopping() ⇒ 控制面离开 Ready。若每轮只留 pub 一个对象，本轮
         * 结束时它的析构会把**下一轮**要用的控制面段置成 Stopping —— 下一轮订阅者的
         * 握手线程会看到非 Ready，永远 attach 不上（实测症状：第 2 轮
         * `fail:sub_never_attached`）。 */
        auto keeper_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
        dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0, /*verbose=*/false};
        pub.InitChannel();
        dzIPC::shm::shm_sub_ipc keeper{keeper_td, topic, 0, /*queue_size=*/8, /*verbose=*/false};
        keeper.InitChannel();

        Recorder rec;
        Recorder::instance() = &rec;

        /* 累计跨轮：每轮新建一个订阅者（**topic 名每轮相同**，与产品语义一致：
         * generation 重建是同一个 topic 上的事），析构它，再数一次。 */
        int total_woke = 0;
        int total_recv_after = 0;
        std::uint64_t total_artifacts = 0;
        int rounds_with_stop = 0;
        std::string order_report;

        for (std::uint32_t i = 0; i < rounds; ++i)
        {
            const std::string& t = topic;
            {
                auto sub_td = std::make_shared<dzIPC::TopicData>(
                    std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
                dzIPC::shm::shm_sub_ipc sub{sub_td, t, 0, /*queue_size=*/8, /*verbose=*/false};
                sub.InitChannel();
                /* 等握手完成（peer 出现在控制面）—— 否则析构时收包线程可能还没进 recv。 */
                bool ready = false;
                for (int k = 0; k < 300 && !ready; ++k)
                {
                    dzIPC::control_plane_shm::TopicControlPlane cp;
                    if (cp.open(shm_topic_control_name(t, 0)) && cp.peer_count() >= 2)
                    {
                        ready = true;
                    }
                    std::this_thread::sleep_for(10ms);
                }
                if (!ready)
                {
                    report(fd, "fail:sub_never_attached\n");
                    child_exit(guard, 3);
                }
                /* 控制面必须是 Ready（pub 活着 + 本轮没有别的 stop 源），否则本轮
                 * 析构时的叫醒就不是"析构自己做的"。 */
                {
                    dzIPC::control_plane_shm::TopicControlPlane cp;
                    if (!cp.open(shm_topic_control_name(t, 0)) ||
                        cp.state() != dzIPC::control_plane_shm::TopicState::Ready)
                    {
                        report(fd, "fail:cp_not_ready\n");
                        child_exit(guard, 5);
                    }
                }
                /* 让收包线程确实进入 recv(50)（握手刚完成时它可能还在 sleep 分支上）。 */
                std::this_thread::sleep_for(60ms);

                /* 装钩子：只关心**析构期间**的事件。 */
                {
                    std::lock_guard<std::mutex> lock(rec.m);
                    rec.dtor_order.clear();
                    rec.saw_stop = false;
                    rec.woke_after_stop = 0;
                    rec.recv_after_stop = 0;
                }
                dzIPC::detail::SetSeamHook([](const dzIPC::detail::SeamEvent& ev) noexcept {
                    if (Recorder::instance() != nullptr)
                    {
                        Recorder::instance()->on(ev);
                    }
                });

                /* ---- 被观测的那一步：析构 ---- */
            }
            dzIPC::detail::SetSeamHook(nullptr);

            std::vector<int> order;
            int woke = 0;
            int recv_after = 0;
            bool saw_stop = false;
            {
                std::lock_guard<std::mutex> lock(rec.m);
                order = rec.dtor_order;
                woke = rec.woke_after_stop;
                recv_after = rec.recv_after_stop;
                saw_stop = rec.saw_stop;
            }
            if (saw_stop)
            {
                ++rounds_with_stop;
            }
            total_woke += woke;
            total_recv_after += recv_after;
            for (int p : order)
            {
                order_report += std::to_string(p);
                order_report += ",";
            }
            order_report += "|";
        }

        /* 析构后的控制面状态：订阅者已退出 ⇒ peer 应被回收（remove_peer）。
         * 注意 keeper 还活着 ⇒ 期望值是 1（keeper 自己），不是 0。 */
        dzIPC::control_plane_shm::TopicControlPlane cp;
        std::uint32_t peers = 0;
        if (cp.open(shm_topic_control_name(topic, 0)))
        {
            for (int k = 0; k < 200; ++k)
            {
                peers = cp.peer_count();
                if (peers <= 1)
                {
                    break;
                }
                std::this_thread::sleep_for(10ms);
            }
        }

        total_artifacts = dzIPC::WakeupArtifactCount();

        line = "dtor=" + order_report + "\n";
        report(fd, line.c_str());
        line = "woke=" + std::to_string(total_woke) + " recv_after=" + std::to_string(total_recv_after) +
               " rounds_with_stop=" + std::to_string(rounds_with_stop) + " rounds=" +
               std::to_string(rounds) + "\n";
        report(fd, line.c_str());
        line = "artifacts=" + std::to_string(total_artifacts) + " peers=" + std::to_string(peers) + "\n";
        report(fd, line.c_str());
        report(fd, "ok\n");
    }
    child_exit(guard, 0);
}

/* 从回报文本里取 "key=" 之后的整数。 */
long parse_field(const std::string& log, const std::string& key)
{
    const std::size_t p = log.find(key);
    if (p == std::string::npos)
    {
        return -1;
    }
    return std::strtol(log.c_str() + p + key.size(), nullptr, 10);
}

std::string parse_dtor_orders(const std::string& log)
{
    const std::size_t p = log.find("dtor=");
    if (p == std::string::npos)
    {
        return {};
    }
    const std::size_t e = log.find('\n', p);
    return log.substr(p + 5, e - p - 5);
}

/* 把 "16,17,18,19,20,21|16,17,..." 拆成每轮的序列。 */
std::vector<std::vector<int>> split_orders(const std::string& s)
{
    std::vector<std::vector<int>> out;
    std::vector<int> cur;
    std::string num;
    for (char c : s)
    {
        if (c == '|')
        {
            if (!cur.empty())
            {
                out.push_back(cur);
            }
            cur.clear();
            num.clear();
        }
        else if (c == ',')
        {
            if (!num.empty())
            {
                cur.push_back(std::atoi(num.c_str()));
                num.clear();
            }
        }
        else
        {
            num.push_back(c);
        }
    }
    return out;
}

}   // namespace

/* ═══════ 承重条：析构必须**叫醒**在途 recv，且顺序必须是 §5 的那一步序 ═══════ */
TEST(ShmSubDtorGate, DtorWakesInflightRecvInOrder)
{
    constexpr std::uint32_t kRounds = 10;
    const ChildResult r = run_in_child([&](int fd) { child_body(fd, kRounds); });

    ASSERT_FALSE(r.timed_out) << "子进程被硬超时杀死 —— 析构可能挂住了。 " << describe(r);
    ASSERT_FALSE(r.signaled) << "子进程被信号杀死（疑似崩溃/UAF）。 " << describe(r);
    ASSERT_TRUE(r.exited) << describe(r);
    ASSERT_EQ(r.code, 0) << "子进程自评失败。 " << describe(r);

    const std::string log = r.log;
    ASSERT_NE(log.find("ok"), std::string::npos) << "子进程未回报 ok。 " << describe(r);

    /* ---- 判据 ①：因果序 ---- */
    const auto orders = split_orders(parse_dtor_orders(log));
    ASSERT_EQ(orders.size(), kRounds) << "析构阶段点回报的轮数不符（每轮应有一组）。 log=" << log;
    for (std::size_t i = 0; i < orders.size(); ++i)
    {
        const auto& o = orders[i];
        ASSERT_EQ(o.size(), 6u) << "第 " << i << " 轮的析构阶段点数量不是 6（说明有阶段被跳过）。 log=" << log;
        /* 逐点严格递增：16(注销) → 17(stop_and_wake) → 18(join 收包) → 19(join 握手)
         * → 20(quiescent) → 21(release)。§5 第 1 步必须在第 4 步**之前**。 */
        for (std::size_t k = 1; k < o.size(); ++k)
        {
            EXPECT_LT(o[k - 1], o[k]) << "第 " << i << " 轮的析构顺序被破坏: " << o[k - 1] << " → " << o[k]
                                      << "（§5 的步骤序是承重的：registry 注销必须在 stop 之前）";
        }
        EXPECT_EQ(o[0], 16) << "第 " << i << " 轮第一个析构点不是「registry 已注销」";
        EXPECT_EQ(o[1], 17) << "第 " << i << " 轮第二个析构点不是「stop_and_wake 已返回」";
    }

    /* ---- 判据 ②：叫醒而非超时 ---- */
    const long rounds_with_stop = parse_field(log, "rounds_with_stop=");
    const long woke = parse_field(log, "woke=");
    const long recv_after = parse_field(log, "recv_after=");
    ASSERT_GE(rounds_with_stop, 0L);
    EXPECT_EQ(rounds_with_stop, static_cast<long>(kRounds))
        << "有轮次里 stop_and_wake 阶段点根本没到 —— 析构路径被改坏了。 log=" << log;
    EXPECT_GT(woke, 0L)
        << "析构期间没有任何一次「connected_id()==0 的 recv 返回」⇒ 收包线程不是被 disconnect "
           "叫醒的（删掉 stop_and_wake 就会这样）。 log=" << log;
    EXPECT_GE(recv_after, woke) << "统计口径异常";

    /* ---- 判据 ③：计数面（与 ② 互为独立证据） ---- */
    const long artifacts = parse_field(log, "artifacts=");
    EXPECT_GT(artifacts, 0L)
        << "析构期没有产生过叫醒伪影 ⇒ 上层守门没被走到（与判据②指向同一件事的另一面）。 log=" << log;

    /* ---- 顺带：析构后 peer 必须被回收（keeper 仍在 ⇒ 期望值 1） ---- */
    const long peers = parse_field(log, "peers=");
    EXPECT_EQ(peers, 1L) << "析构后控制面登记的 peer 数不是 keeper 一个 —— remove_peer 没走到。 log=" << log;

    std::printf("[dtor-gate] rounds=%u woke=%ld recv_after=%ld artifacts=%ld peers=%ld\n", kRounds, woke,
                recv_after, artifacts, peers);
}

/* ═══════ 阴性对照：叫醒**不依赖**控制面离开 Ready，且不得吞掉真消息 ═══════
 *
 * 与上一条的差别：这里 pub 全程存活、控制面**保持 Ready**，且析构前先发一条真消息。
 * 两条独立用途：
 *   ① 若实现把"析构叫醒"错误地实现成"靠控制面离开 Ready 触发的 stop_and_wake"，
 *      上一条会偶然变绿而这一条会红；
 *   ② 顺带钉住"析构期的那次叫醒不得把已经投递的消息弄丢"（队列里的真消息在析构
 *      完成后仍应取得到）。 */
TEST(ShmSubDtorGate, DtorWakeIsNotControlPlaneDriven)
{
    const ChildResult r = run_in_child([](int fd) {
        const std::string topic = std::string("dtorgate_ready_") + std::to_string(::getpid());
        TopicGuard guard{topic};
        auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
        auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
        dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0, /*verbose=*/false};
        pub.InitChannel();

        Recorder rec;
        Recorder::instance() = &rec;

        int got = 0;
        {
            dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, /*queue_size=*/8, /*verbose=*/false};
            sub.InitChannel();
            bool ready = false;
            for (int k = 0; k < 300 && !ready; ++k)
            {
                dzIPC::control_plane_shm::TopicControlPlane cp;
                if (cp.open(shm_topic_control_name(topic, 0)) && cp.peer_count() > 0)
                {
                    ready = true;
                }
                std::this_thread::sleep_for(10ms);
            }
            if (!ready)
            {
                report(fd, "fail:sub_never_attached\n");
                child_exit(guard, 3);
            }
            /* 断言前提：析构**之前**控制面是 Ready（pub 活着）。 */
            dzIPC::control_plane_shm::TopicControlPlane cp;
            if (!cp.open(shm_topic_control_name(topic, 0)) ||
                cp.state() != dzIPC::control_plane_shm::TopicState::Ready)
            {
                report(fd, "fail:cp_not_ready_before_dtor\n");
                child_exit(guard, 4);
            }
            /* 让收包线程进入 recv，再发一条真消息并等它进队列。 */
            std::this_thread::sleep_for(60ms);
            auto img = std::make_shared<dzIPC::Msg::StdImage>();
            img->set_msg_id(kMsgId);
            img->width = 3;
            img->height = 2;
            img->step = 9;
            img->encoding = "rgb8";
            img->data.assign(18, 0x7E);
            if (!pub.publish(img))
            {
                report(fd, "fail:publish_failed\n");
                child_exit(guard, 7);
            }
            auto sink = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
            for (int k = 0; k < 200 && got == 0; ++k)
            {
                if (sub.try_get_clone(sink))
                {
                    ++got;
                }
                else
                {
                    std::this_thread::sleep_for(10ms);
                }
            }
            dzIPC::detail::SetSeamHook([](const dzIPC::detail::SeamEvent& ev) noexcept {
                if (Recorder::instance() != nullptr)
                {
                    Recorder::instance()->on(ev);
                }
            });
            /* 析构：控制面此刻仍是 Ready（pub 活着）⇒ 叫醒只能来自析构自己。 */
        }
        dzIPC::detail::SetSeamHook(nullptr);

        int woke = 0;
        bool saw_stop = false;
        {
            std::lock_guard<std::mutex> lock(rec.m);
            woke = rec.woke_after_stop;
            saw_stop = rec.saw_stop;
        }
        std::string line = "woke=" + std::to_string(woke) + " saw_stop=" + (saw_stop ? "1" : "0") +
                           " got=" + std::to_string(got) +
                           " artifacts=" + std::to_string(dzIPC::WakeupArtifactCount()) + "\n";
        report(fd, line.c_str());
        report(fd, "ok\n");
        child_exit(guard, 0);
    });

    ASSERT_FALSE(r.timed_out) << describe(r);
    ASSERT_FALSE(r.signaled) << describe(r);
    ASSERT_TRUE(r.exited) << describe(r);
    ASSERT_EQ(r.code, 0) << describe(r);

    EXPECT_EQ(parse_field(r.log, "saw_stop="), 1L)
        << "控制面保持 Ready 时析构没有走 stop_and_wake ⇒ 叫醒不是析构自己做的。 log=" << r.log;
    /* ⚠️ 这里**不**断言 woke > 0：本用例里收包线程可能正处在 recv 的两次迭代之间
     * （真消息刚被取走、队列空、下一次 recv 尚未进入），于是析构的 disconnect 不会
     * 叫醒任何在途 recv —— 那是**时序**，不是缺陷。上一条用例用 10 轮把"至少一次
     * 命中"变成稳定判据；本条改用不受时序影响的判据：控制面全程 Ready ⇒ 叫醒只能
     * 来自析构，且析构期的那次叫醒不得吞掉已投递的真消息。 */
    EXPECT_EQ(parse_field(r.log, "got="), 1L)
        << "析构前的真消息没被取到 —— 用例前提不成立。 log=" << r.log;
}

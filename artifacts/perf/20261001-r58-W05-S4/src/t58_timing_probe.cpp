/* t58 独立复算：`kControlTiming{}` 的**运行期生效值**（⛔ 不读头文件常量）。
 *
 * 三条读数都取自**真实产品路径**（真实 shm_pub_ipc/shm_sub_ipc + 真实控制面段）：
 *   A) `pub_heartbeat = 50 ms`：读控制面段头的 `heartbeat_ns`（发布端 on_pub_heartbeat
 *      每周期写一次，见 control_plane.cc 的 heartbeat()）推进速率 ⇒ 1000/50 = 20/s；
 *   B) `sub_heartbeat = 10 ms`：读订阅端 peer 槽位的 `heartbeat_ns`（每周期 peer_heartbeat()
 *      写一次）推进速率 ⇒ 1000/10 = 100/s；
 *   C) `peer_dead_timeout = 2 s`：**行为判据** —— 用 SIGSTOP 冻结订阅进程（进程仍存活、
 *      只是不再心跳），测发布端 `peer_count` 从 1 → 0 的耗时。它必须≈2 s 而**不是**
 *      一个 50 ms 周期（排除"按 pid 存活判死"与"按单周期判死"两种替代解释）。
 *
 * 段头/槽位的读取用 `ipc::shm::handle::acquire(..., ipc::shm::open)`：**只开不建**、
 * 只读，不改任何产品状态。
 *
 * 用法: t58_timing_probe <mode> [n] [window_s] [domain]
 *   mode = inproc  : 同进程建 pub+sub，测 A/B 周期
 *   mode = fork    : 子进程持订阅（可被 SIGSTOP），父进程持发布，测 C 判死超时
 */
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>
#include <signal.h>
#include <sys/wait.h>

#include "dzIPC/common/control_plane.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "libipc/shm.h"
#include "ipc_msg/std_msgs/std_string.hpp"

using Clock = std::chrono::steady_clock;
using dzIPC::control_plane_shm::TopicControl;
static constexpr std::uint32_t kMsgId = 93;
static std::shared_ptr<dzIPC::TopicData> td()
{ return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId); }

/* 只读映射控制面段（不创建）。 */
struct RawSeg
{
    ipc::shm::handle h;
    TopicControl* c{nullptr};
    bool open(const std::string& name)
    {
        if (!h.acquire(name.c_str(), sizeof(TopicControl), ipc::shm::open)) return false;
        c = static_cast<TopicControl*>(h.get());
        return c != nullptr && c->magic.load() == dzIPC::control_plane_shm::kTopicControlMagic;
    }
    int64_t pub_hb() const { return c->heartbeat_ns.load(std::memory_order_acquire); }
    int64_t peer_hb(int i) const { return c->peers[i].heartbeat_ns.load(std::memory_order_acquire); }
    uint32_t peer_count() const { return c->peer_count.load(std::memory_order_acquire); }
};

/* 统计一个"单调递增的 ns 时间戳"在窗口内的**推进次数**（= 周期数）。 */
static double rate_probe(const std::function<int64_t()>& get, double win_s, long* changes)
{
    const auto t0 = Clock::now();
    int64_t last = get();
    long n = 0;
    while (std::chrono::duration<double>(Clock::now() - t0).count() < win_s)
    {
        const int64_t v = get();
        if (v != last) { ++n; last = v; }
        std::this_thread::sleep_for(std::chrono::microseconds(100));
    }
    const double wall = std::chrono::duration<double>(Clock::now() - t0).count();
    if (changes) *changes = n;
    return static_cast<double>(n) / wall;
}

#define SAY(...) do { std::printf(__VA_ARGS__); std::fflush(stdout); } while (0)

int main(int argc, char** argv)
{
    const std::string mode = argc > 1 ? argv[1] : "inproc";
    const long n = argc > 2 ? strtol(argv[2], nullptr, 10) : 1;
    const double win = argc > 3 ? atof(argv[3]) : 4.0;
    const long dom = argc > 4 ? strtol(argv[4], nullptr, 10) : 9800;
    const std::string topic = "t58t_" + std::to_string(dom) + "_0";
    const char* envv = ::getenv("DZIPC_SHM_CONTROL_SCHEDULER");
    const char* arm = (envv != nullptr && envv[0] != '\0' && !(envv[0] == '0' && envv[1] == '\0'))
                          ? "compat-per-topic-thread" : "process-scheduler";
    SAY("mode=%s arm=%s n=%ld win=%.1f topic=%s\n", mode.c_str(), arm, n, win, topic.c_str());

    if (mode == "inproc")
    {
        /* A/B：同进程 1 对 pub/sub（n 条同话题订阅会占 peer 槽，取 slot 最大的活槽） */
        std::vector<std::unique_ptr<dzIPC::shm::shm_pub_ipc>> pubs;
        std::vector<std::unique_ptr<dzIPC::shm::shm_sub_ipc>> subs;
        for (long i = 0; i < n; ++i)
        {
            const std::string t = "t58t_" + std::to_string(dom) + "_" + std::to_string(i);
            pubs.emplace_back(new dzIPC::shm::shm_pub_ipc(td(), t, (size_t)dom, false));
            pubs.back()->InitChannel("t58");
            subs.emplace_back(new dzIPC::shm::shm_sub_ipc(td(), t, (size_t)dom, 64, false));
            subs.back()->InitChannel("t58");
        }
        /* 等握手 */
        {
            const auto dl = Clock::now() + std::chrono::seconds(60);
            while (Clock::now() < dl)
            {
                RawSeg s;
                if (s.open(shm_topic_control_name(topic, (size_t)dom)) && s.peer_count() >= 1) break;
                std::this_thread::sleep_for(std::chrono::milliseconds(5));
            }
        }
        RawSeg s;
        if (!s.open(shm_topic_control_name(topic, (size_t)dom))) { SAY("open_fail\n"); return 2; }
        long cp = 0, cs = 0;
        const double pub_rate = rate_probe([&] { return s.pub_hb(); }, win, &cp);
        /* 找一个 in_use 的 peer 槽（订阅侧心跳） */
        int slot = -1;
        for (int i = 0; i < (int)dzIPC::control_plane_shm::kMaxPeerSlots; ++i)
            if (s.c->peers[i].in_use.load() != 0) { slot = i; break; }
        double sub_rate = -1.0;
        if (slot >= 0) sub_rate = rate_probe([&] { return s.peer_hb(slot); }, win, &cs);
        SAY("pub_heartbeat_changes=%ld rate_per_s=%.2f implied_period_ms=%.2f\n", cp, pub_rate,
            pub_rate > 0 ? 1000.0 / pub_rate : -1.0);
        SAY("sub_heartbeat_changes=%ld rate_per_s=%.2f implied_period_ms=%.2f slot=%d\n", cs, sub_rate,
            sub_rate > 0 ? 1000.0 / sub_rate : -1.0, slot);
        SAY("T58_TIMING_DONE\n");
        std::_Exit(0);
    }

    /* fork：子进程持订阅，父进程持发布；冻结子进程后测 peer 判死耗时。 */
    pid_t child = ::fork();
    if (child == 0)
    {
        /* 子：只建订阅，然后进入静默循环（心跳由它自己的控制面驱动） */
        auto sub = std::make_unique<dzIPC::shm::shm_sub_ipc>(td(), topic, (size_t)dom, 64, false);
        sub->InitChannel("t58");
        while (true) std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    auto pub = std::make_unique<dzIPC::shm::shm_pub_ipc>(td(), topic, (size_t)dom, false);
    pub->InitChannel("t58");
    RawSeg s;
    const auto dl0 = Clock::now() + std::chrono::seconds(60);
    while (Clock::now() < dl0)
    {
        if (s.open(shm_topic_control_name(topic, (size_t)dom)) && s.peer_count() >= 1) break;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    /* ⚠️ 判死判据**不是** peer_count：`collect_stale_peers()` 只把槽位清掉
     * （`in_use=0` + cc_id/pid/generation/heartbeat 清零），peer_count 由
     * add_peer/remove_peer 管理。故行为判据看**槽位 in_use 是否被清**。 */
    int slot0 = -1;
    for (int i = 0; i < (int)dzIPC::control_plane_shm::kMaxPeerSlots; ++i)
        if (s.c->peers[i].in_use.load() != 0) { slot0 = i; break; }
    SAY("peer_count_before_stop=%u slot=%d child_pid=%d\n", s.peer_count(), slot0, (int)child);
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    /* 冻结子进程：它仍**存活**（kill(pid,0) 成功）只是不再刷新心跳 ⇒ 只有心跳超时
     * 能把槽位判死。若判死按 pid 存活或按单个 50 ms 周期，耗时会显著偏离 2 s。 */
    const auto t0 = Clock::now();
    const bool use_kill = (::getenv("T58_CHILD_KILL") != nullptr);
    if (use_kill) ::kill(child, SIGKILL); else ::kill(child, SIGSTOP);
    double gone_ms = -1.0;
    while (std::chrono::duration<double>(Clock::now() - t0).count() < 20.0)
    {
        if (slot0 >= 0 && s.c->peers[slot0].in_use.load() == 0)
        {
            gone_ms = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();
            break;
        }
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    /* 反向对照：槽位被清**之后**才发生 disconnect_receivers（on_pub_stale_scan 内），
     * 用发布端 recv_count() 观察连接位图是否真的摘掉。 */
    SAY("mode_child=%s slot_reclaimed_ms=%.2f in_use_after=%u peer_count_after=%u child_alive=%d\n",
        use_kill ? "SIGKILL" : "SIGSTOP", gone_ms,
        slot0 >= 0 ? s.c->peers[slot0].in_use.load() : 99u, s.peer_count(),
        (::kill(child, 0) == 0) ? 1 : 0);
    ::kill(child, SIGKILL);
    ::waitpid(child, nullptr, 0);
    SAY("T58_TIMING_DONE\n");
    _exit(0);
}

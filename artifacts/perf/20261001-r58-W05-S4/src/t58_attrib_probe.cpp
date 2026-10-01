/* t58 独立复算：per-route 控制线程的**归属证明**（⛔ 只看总数不算证明）。
 *
 * 三条互相独立的归属证据：
 *  ① **算术恒等式**（池规模钉死）：`threads == 1(主) + W(池 worker) + 1(调度器 worker)`
 *     —— 池用 `DZIPC_SHM_RECV_WORKERS=<W>` 钉死，且 N ≤ 127×W 以避免容量回退（否则
 *     溢出 route 会走兼容收包线程，把"控制面线程"与"容量回退线程"混在一起）。
 *  ② **归属差分（决定性反例）**：同一二进制、同一 N，仅改 `DZIPC_SHM_CONTROL_SCHEDULER`
 *     ⇒ 线程数差分**恰为 2N**、且 `ShmControlScheduler::entry_count()` 由 2N 变 0。
 *     2N = 每话题 pub 控制 + sub 控制各一条 ⇒ 那 2N 条线程**就是**控制项自己的线程，
 *     不存在第二种解释（若残留的是别的东西，差分不会是精确的 2N）。
 *  ③ **接收侧 per-route = 0**：默认臂下 `RecvWorkerPool::route_count() == N`（全部 route
 *     都在共享池里），且线程数不随 N 增长 ⇒ 接收侧没有 per-route 线程。
 *  ④ **wchan 直方图**（第三来源）：统计各线程的内核等待点，跨 N 比较形状。
 *
 * 用法: t58_attrib_probe <n> <window_s> <domain>
 */
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <dirent.h>
#include <fstream>
#include <map>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "dzIPC/common/control_plane.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/threepools/recv_worker.h"
#include "dzIPC/threepools/shm_control_scheduler.h"
#include "ipc_msg/std_msgs/std_string.hpp"

using Clock = std::chrono::steady_clock;
static constexpr std::uint32_t kMsgId = 93;
static std::shared_ptr<dzIPC::TopicData> td()
{ return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId); }
static std::size_t thread_count()
{
    DIR* d = ::opendir("/proc/self/task"); if (!d) return 0;
    std::size_t n = 0; while (dirent* e = ::readdir(d)) if (e->d_name[0] != '.') ++n;
    ::closedir(d); return n;
}
/* 每个线程的内核等待点（wchan）+ 线程名（comm），用于"跨 N 的形状"比较。 */
static std::map<std::string,int> wchan_hist()
{
    std::map<std::string,int> h;
    DIR* d = ::opendir("/proc/self/task"); if (!d) return h;
    while (dirent* e = ::readdir(d))
    {
        if (e->d_name[0] == '.') continue;
        const std::string base = std::string("/proc/self/task/") + e->d_name + "/";
        std::ifstream f(base + "wchan");
        std::string w; std::getline(f, w);
        if (w.empty()) w = "?";
        std::ifstream g(base + "comm");
        std::string c; std::getline(g, c);
        ++h[w + "|" + c];
    }
    ::closedir(d);
    return h;
}
static std::string hist_str(const std::map<std::string,int>& h)
{
    std::string s;
    for (const auto& kv : h) s += kv.first + "=" + std::to_string(kv.second) + " ";
    return s;
}
#define SAY(...) do { std::printf(__VA_ARGS__); std::fflush(stdout); } while (0)

int main(int argc, char** argv)
{
    const long n = argc > 1 ? strtol(argv[1], nullptr, 10) : 100;
    const double win = argc > 2 ? atof(argv[2]) : 3.0;
    const long dom = argc > 3 ? strtol(argv[3], nullptr, 10) : 9000;
    const char* v = ::getenv("DZIPC_SHM_CONTROL_SCHEDULER");
    const bool fallback = (v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0'));
    const char* wv = ::getenv("DZIPC_SHM_RECV_WORKERS");
    SAY("n=%ld dom=%ld arm=%s RECV_WORKERS=%s\n", n, dom,
        fallback ? "compat-per-topic-thread" : "process-scheduler", wv ? wv : "<unset>");
    SAY("threads_before=%zu\n", thread_count());

    std::vector<std::string> names;
    std::vector<std::unique_ptr<dzIPC::shm::shm_pub_ipc>> pubs;
    std::vector<std::unique_ptr<dzIPC::shm::shm_sub_ipc>> subs;
    for (long i = 0; i < n; ++i)
    {
        names.push_back("t58a_" + std::to_string(dom) + "_" + std::to_string(i));
        pubs.emplace_back(new dzIPC::shm::shm_pub_ipc(td(), names.back(), (size_t)dom, false));
        pubs.back()->InitChannel("t58");
        subs.emplace_back(new dzIPC::shm::shm_sub_ipc(td(), names.back(), (size_t)dom, 64, false));
        subs.back()->InitChannel("t58");
    }
    {
        const auto dl = Clock::now() + std::chrono::seconds(90);
        long ok = 0;
        while (Clock::now() < dl && ok < n)
        {
            ok = 0;
            for (long i = 0; i < n; ++i)
            {
                dzIPC::control_plane_shm::TopicControlPlane cp;
                if (cp.open(shm_topic_control_name(names[(size_t)i], (size_t)dom)) && cp.peer_count() >= 1) ++ok;
            }
            if (ok < n) std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        SAY("handshake_ok=%ld/%ld\n", ok, n);
    }
    std::this_thread::sleep_for(std::chrono::milliseconds(800));
    auto& pool = dzIPC::threepools::RecvWorkerPool::instance();
    auto& sched = dzIPC::shm_control::ShmControlScheduler::instance();
    SAY("threads_settled=%zu pool_workers=%zu pool_route_count=%zu scheduler_entry_count=%zu "
        "scheduler_worker_active=%d\n",
        thread_count(), pool.worker_count(), pool.route_count(), sched.entry_count(),
        sched.worker_active() ? 1 : 0);
    const auto h = wchan_hist();
    SAY("wchan_hist=%s\n", hist_str(h).c_str());
    SAY("T58_ATTRIB_DONE\n");
    _exit(0);
}

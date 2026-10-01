/* t58 独立复算：`DZIPC_SHM_CONTROL_SCHEDULER` 的**极性**（五种取值）+ 回退路径**功能可用性**。
 *
 * 极性判据（运行期，非读头文件）：同一二进制、同一 N，只改环境变量 ⇒
 *   · 线程数：默认/新路径 = 常数（1 + N 收包 + 1 调度器，池 worker 按在册 route）；
 *     回退 = 2N 条兼容控制线程（每话题 pub+sub 各一条）。
 *   · `ShmControlScheduler::entry_count()`：新路径 = 2N（在册项）；回退 = 0（未注册）。
 *   · 调度器 `worker_active()`：两者都为 1（进程级常驻单例）——⛔ 不得用它判极性。
 *
 * 功能可用性判据：把控制面切到回退臂后，**真实收发**必须照常工作：
 *   建 N 对 pub/sub → 发布 → 收到（逐条校验载荷）→ 对端 peer 发现（peer_count>=1）。
 *
 * 用法: t58_polarity_probe <n> <window_s>
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include <dirent.h>
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
#define SAY(...) do { std::printf(__VA_ARGS__); std::fflush(stdout); } while (0)

int main(int argc, char** argv)
{
    const long n = argc > 1 ? strtol(argv[1], nullptr, 10) : 100;
    const double win = argc > 2 ? atof(argv[2]) : 3.0;
    const long dom = argc > 3 ? strtol(argv[3], nullptr, 10) : 9950;
    const char* v = ::getenv("DZIPC_SHM_CONTROL_SCHEDULER");
    const char* vshow = (v == nullptr) ? "<unset>" : (v[0] == '\0' ? "<empty>" : v);
    SAY("env_DZIPC_SHM_CONTROL_SCHEDULER=%s n=%ld dom=%ld\n", vshow, n, dom);
    SAY("threads_before=%zu\n", thread_count());

    std::vector<std::string> names;
    std::vector<std::unique_ptr<dzIPC::shm::shm_pub_ipc>> pubs;
    std::vector<std::unique_ptr<dzIPC::shm::shm_sub_ipc>> subs;
    for (long i = 0; i < n; ++i)
    {
        names.push_back("t58pol_" + std::to_string(dom) + "_" + std::to_string(i));
        pubs.emplace_back(new dzIPC::shm::shm_pub_ipc(td(), names.back(), (size_t)dom, false));
        pubs.back()->InitChannel("t58");
        subs.emplace_back(new dzIPC::shm::shm_sub_ipc(td(), names.back(), (size_t)dom, 64, false));
        subs.back()->InitChannel("t58");
    }
    auto& sched = dzIPC::shm_control::ShmControlScheduler::instance();
    SAY("threads_after_create=%zu scheduler_entry_count=%zu worker_active=%d\n",
        thread_count(), sched.entry_count(), sched.worker_active() ? 1 : 0);
    /* 等握手 + 等回退控制线程建立（旧路径线程在 InitChannel 后拉起） */
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
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    SAY("threads_settled=%zu scheduler_entry_count=%zu\n", thread_count(), sched.entry_count());

    /* ---- 功能可用性：真实收发（逐条校验载荷） ---- */
    long sent = 0, got = 0, mismatched = 0;
    for (long round = 0; round < 3; ++round)
    {
        for (long i = 0; i < n; ++i)
        {
            auto m = std::make_shared<dzIPC::Msg::StdString>();
            m->set_msg_id(kMsgId);
            m->str = "P" + std::to_string(round) + "_" + std::to_string(i);
            if (pubs[(size_t)i]->publish(m)) ++sent;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        for (long i = 0; i < n; ++i)
        {
            auto sink = td();
            if (subs[(size_t)i]->try_get_clone(sink))
            {
                auto* p = dynamic_cast<dzIPC::Msg::StdString*>(sink->topic().get());
                const std::string want = "P" + std::to_string(round) + "_" + std::to_string(i);
                if (p == nullptr || p->str != want) ++mismatched; else ++got;
            }
        }
    }
    SAY("rx_sent=%ld rx_ok=%ld rx_mismatch=%ld expected=%ld\n", sent, got, mismatched, n * 3);
    const auto st = dzIPC::threepools::RecvWorkerPool::instance().stats();
    SAY("pool_routes=%zu pool_workers=%zu recv_errors=%llu\n",
        dzIPC::threepools::RecvWorkerPool::instance().route_count(),
        dzIPC::threepools::RecvWorkerPool::instance().worker_count(),
        (unsigned long long)st.recv_errors);
    /* 极性判定（机器判据，写在输出里便于脚本核对） */
    const bool fallback = (v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0'));
    SAY("arm=%s threads_per_topic=%.3f\n", fallback ? "compat-per-topic-thread" : "process-scheduler",
        n > 0 ? (double)thread_count() / n : 0.0);
    (void)win;
    SAY("T58_POLARITY_DONE\n");
    _exit(0);
}

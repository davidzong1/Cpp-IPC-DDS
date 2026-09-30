/* W05 控制面接入探针（t6 取证用，非 gtest）：
 *   · 建 N 个**真实** shm_sub_ipc + N 个 shm_pub_ipc，测进程线程数随 N 的增长曲线；
 *   · 读 ShmControlScheduler::stats() 的 tick_count / tick_duration_max_ns /
 *     tick_overrun_count / entry_count；
 *   · 兼容臂对照（DZIPC_SHM_CONTROL_SCHEDULER=1 ⇒ 每话题控制线程）。
 * 用法: w05_control_probe <N> <window_s>
 * ⛔ 只读探针：不改任何产品代码；输出为 k=v 行，便于脚本判定。 */
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include <vector>

#include "dzIPC/common/topic_data.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/threepools/shm_control_scheduler.h"
#include "ipc_msg/std_msgs/std_image.hpp"

namespace {

int thread_count()
{
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
    {
        if (line.rfind("Threads:", 0) == 0) return std::atoi(line.c_str() + 8);
    }
    return -1;
}

std::string arg(int argc, char** argv, int idx, const char* dflt)
{
    return (argc > idx) ? std::string(argv[idx]) : std::string(dflt);
}

}  // namespace

int main(int argc, char** argv)
{
    const int n = std::atoi(arg(argc, argv, 1, "200").c_str());
    const int window_s = std::atoi(arg(argc, argv, 2, "4").c_str());
    const char* env = ::getenv("DZIPC_SHM_CONTROL_SCHEDULER");
    const char* mode = (env != nullptr && env[0] != '\0' && !(env[0] == '0' && env[1] == '\0'))
                           ? "compat-per-topic-thread" : "process-scheduler";

    std::printf("probe=W05 n=%d window_s=%d mode=%s pid=%d\n", n, window_s, mode, (int)::getpid());
    std::printf("threads_before=%d\n", thread_count());
    std::fflush(stdout);

    const int kMsgId = 55;
    std::vector<std::unique_ptr<dzIPC::shm::shm_sub_ipc>> subs;
    std::vector<std::unique_ptr<dzIPC::shm::shm_pub_ipc>> pubs;
    subs.reserve(static_cast<std::size_t>(n));
    pubs.reserve(static_cast<std::size_t>(n));

    /* 每路一个**独立**话题（§6.1 有效规模口径），topic 名不含 '-'。 */
    for (int i = 0; i < n; ++i)
    {
        const std::string topic = "/w05/t" + std::to_string(i);
        auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
        subs.push_back(std::make_unique<dzIPC::shm::shm_sub_ipc>(sub_td, topic, 0, 8));
        subs.back()->InitChannel("w05probe");
    }
    std::printf("threads_after_subs=%d\n", thread_count());
    std::fflush(stdout);

    for (int i = 0; i < n; ++i)
    {
        const std::string topic = "/w05/t" + std::to_string(i);
        auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
        pubs.push_back(std::make_unique<dzIPC::shm::shm_pub_ipc>(pub_td, topic, 0, false));
        pubs.back()->InitChannel("w05probe");
    }
    const int threads_after_all = thread_count();
    std::printf("threads_after_all=%d\n", threads_after_all);
    std::fflush(stdout);

    auto& sched = dzIPC::shm_control::ShmControlScheduler::instance();
    std::printf("entry_count_initial=%zu worker_active_initial=%d\n", sched.entry_count(),
                sched.worker_active() ? 1 : 0);

    /* 观测窗口：让控制面跑起来。⛔ 分两次采样：第一段（含"首次打开 N 个控制面段"
     * 的启动突发）与第二段（稳态），用来区分 tick 超时是启动突发还是稳态问题。 */
    const int half = (window_s > 1) ? (window_s / 2) : 1;
    std::this_thread::sleep_for(std::chrono::seconds(half));
    const auto sm = sched.stats();
    std::printf("phase=early tick_count=%llu tick_duration_max_ns=%lld tick_overrun_count=%llu entry_count=%zu\n",
                (unsigned long long)sm.tick_count, (long long)sm.tick_duration_max_ns,
                (unsigned long long)sm.tick_overrun_count, sched.entry_count());
    std::fflush(stdout);
    std::this_thread::sleep_for(std::chrono::seconds(window_s - half));
    /* 两段之间的增量 = 纯稳态（启动突发已被早期窗口吸收）。 */
    const auto ss = sched.stats();
    std::printf("phase=steady_delta tick_count=%llu tick_duration_max_ns=%lld tick_overrun_count=%llu\n",
                (unsigned long long)(ss.tick_count - sm.tick_count),
                (long long)ss.tick_duration_max_ns, (unsigned long long)(ss.tick_overrun_count - sm.tick_overrun_count));
    std::fflush(stdout);
    const auto s1 = sched.stats();
    const int threads_end = thread_count();
    std::printf("threads_end=%d entry_count=%zu\n", threads_end, sched.entry_count());
    std::printf("tick_count=%llu tick_duration_last_ns=%lld tick_duration_max_ns=%lld tick_overrun_count=%llu "
                "tick_deferred_count=%llu callback_exception_count=%llu worker_active=%d\n",
                (unsigned long long)s1.tick_count, (long long)s1.tick_duration_last_ns,
                (long long)s1.tick_duration_max_ns, (unsigned long long)s1.tick_overrun_count,
                (unsigned long long)s1.tick_deferred_count,
                (unsigned long long)s1.callback_exception_count, sched.worker_active() ? 1 : 0);
    std::printf("threads_per_route=%.3f\n",
                n > 0 ? static_cast<double>(threads_end) / n : 0.0);
    std::printf("W05_PROBE_DONE\n");
    std::fflush(stdout);
    ::fflush(nullptr);
    _exit(0);   /* ⛔ 跳过静态析构：本探针只为取数，且池段是全机共享 */
}

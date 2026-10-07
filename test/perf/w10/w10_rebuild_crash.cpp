/* W10 最小复现：generation 重建路径上的 SIGSEGV（候选**阻塞级**缺陷）。
 *
 * 现场（gdb，`w10_faults --only F3`）：
 *   #0 a0_mtx_lock
 *   #1 ipc::sync::mutex::lock
 *   #2 ipc::detail::waiter::wake(bool)
 *   #3 ipc::chan_impl<...>::disconnect(void*)
 *   #4 dzIPC::shm::RouteSession::stop_and_wake()
 *   #5 dzIPC::threepools::RecvWorker::remove_route()
 *   #6 RecvWorkerPool::remove_route
 *   #7 dzIPC::shm::shm_sub_ipc::before_generation_rebuild()
 *   #8 SubHandshakeState::on_sub_heartbeat()          ← 控制面 tick（W05 接入）
 *   #9 ShmControlScheduler::Impl::tick()
 *
 * 本文件的目的是把"我工装里的现象"降到**最小步骤**，并给出可控的反事实：
 *   mode=0 只建 sub+pub，析构 pub 触发一次重建（最小）
 *   mode=1 同上，但在重建前先析构 sub（对照：看是否与"sub 存活"有关）
 *   mode=2 只建 sub+pub，析构 pub 后**立刻**析构 sub（不等待 tick）
 *   mode=3 冒烟对照：只建 sub+pub，不做任何重建
 *
 * 用法：w10_rebuild_crash <mode> <domain> [rounds]
 * 退出码：0 = 全程未崩；139 = SIGSEGV（shell 约定）。
 */
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>
#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/threepools/recv_worker.h"
#include "ipc_msg/std_msgs/std_string.hpp"

using namespace std::chrono_literals;
static constexpr std::uint32_t kMsgId = 94;

static std::shared_ptr<dzIPC::TopicData> td()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId);
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    const int mode = argc > 1 ? std::atoi(argv[1]) : 0;
    const long domain = argc > 2 ? std::atol(argv[2]) : 5400;
    const int rounds = argc > 3 ? std::atoi(argv[3]) : 5;
    std::printf("mode=%d domain=%ld rounds=%d pid=%d\n", mode, domain, rounds, (int)::getpid());

    auto& pool = dzIPC::threepools::RecvWorkerPool::instance();

    for (int r = 0; r < rounds; ++r)
    {
        const std::string topic = "rc_" + std::to_string(domain) + "_" + std::to_string(r);
        auto sub = std::make_unique<dzIPC::shm::shm_sub_ipc>(td(), topic, static_cast<std::size_t>(domain), 64, false);
        sub->InitChannel("rc");
        auto pub = std::make_unique<dzIPC::shm::shm_pub_ipc>(td(), topic, static_cast<std::size_t>(domain), false);
        pub->InitChannel("rc");
        std::this_thread::sleep_for(1500ms);
        std::printf("round=%d ready routes=%zu threads=%zu\n", r, pool.route_count(), (std::size_t)0);
        if (mode == 3)
        {
            ipc::route::clear_storage(shm_topic_segment_name(topic, static_cast<std::size_t>(domain)).c_str());
            ipc::shm::handle::clear_storage(shm_topic_control_name(topic, static_cast<std::size_t>(domain)).c_str());
            continue;
        }
        if (mode == 1)
        {
            /* 对照：先拆订阅者，再拆发布者 ⇒ 控制面回调时 sub 已不在。 */
            sub.reset();
            std::this_thread::sleep_for(100ms);
        }
        pub.reset();   /* ← 触发订阅侧 generation 重建（控制面 tick 会调 before_generation_rebuild） */
        if (mode == 2)
        {
            sub.reset();   /* 不等 tick，立刻拆 */
        }
        else
        {
            std::this_thread::sleep_for(2000ms);   /* 留足 tick 时间：sub_heartbeat=10ms */
        }
        if (mode != 2) std::printf("round=%d after_pub_reset routes=%zu\n", r, pool.route_count());
        sub.reset();
        std::this_thread::sleep_for(300ms);
        ipc::route::clear_storage(shm_topic_segment_name(topic, static_cast<std::size_t>(domain)).c_str());
        ipc::shm::handle::clear_storage(shm_topic_control_name(topic, static_cast<std::size_t>(domain)).c_str());
    }
    std::printf("W10_REBUILD_CRASH_DONE mode=%d rounds=%d\n", mode, rounds);
    return 0;
}

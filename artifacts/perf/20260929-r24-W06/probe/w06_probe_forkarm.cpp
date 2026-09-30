/* 诊断：fork 子进程里本模块走哪条臂？池的 running()/route_count() 在子进程里是什么？
 * 背景：test_shm_sub_dtor_gate 的子进程报告 arm=compat 且 keeper_routes=0，
 * 但 worker_unregister_rounds 又显示在册数有变化 ⇒ 事实需要直接测，不靠推断。 */
#include <atomic>
#include <cstdlib>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>

#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/detail/shm_sub_seam.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/threepools/recv_worker.h"
#include "ipc_msg/std_msgs/std_string.hpp"

using namespace std::chrono_literals;
static constexpr std::uint32_t kMsgId = 71;
static std::atomic<long> g_worker{0};
static std::atomic<long> g_compat{0};
static std::atomic<int> g_reason{-1};
static void hook(const dzIPC::detail::SeamEvent& ev) noexcept
{
    if (ev.point == dzIPC::detail::SeamPoint::kRecvPathWorker) ++g_worker;
    else if (ev.point == dzIPC::detail::SeamPoint::kRecvPathCompat)
    {
        ++g_compat;
        g_reason.store(static_cast<int>(ev.size));
    }
}
static std::shared_ptr<dzIPC::TopicData> td()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId);
}

static void child_body(const char* tag)
{
    auto& pool = dzIPC::threepools::RecvWorkerPool::instance();
    std::printf("[FA] %s BEFORE: pool_running=%d route_count=%zu pid=%d\n", tag, (int)pool.running(),
                pool.route_count(), (int)::getpid());
    const std::string topic = std::string("w06fa_") + tag + "_" + std::to_string(::getpid());
    dzIPC::detail::SetSeamHook(&hook);
    {
        dzIPC::shm::shm_pub_ipc pub{td(), topic, 0, false};
        pub.InitChannel();
        dzIPC::shm::shm_sub_ipc sub{td(), topic, 0, 8, false};
        sub.InitChannel();
        std::this_thread::sleep_for(400ms);
        std::printf("[FA] %s AFTER: pool_running=%d route_count=%zu worker=%ld compat=%ld reason=%d\n", tag,
                    (int)pool.running(), pool.route_count(), g_worker.load(), g_compat.load(), g_reason.load());
        std::this_thread::sleep_for(200ms);
        std::printf("[FA] %s DTOR: route_count_before=%zu\n", tag, pool.route_count());
    }
    std::printf("[FA] %s DTOR: route_count_after=%zu\n", tag, pool.route_count());
    dzIPC::detail::SetSeamHook(nullptr);
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    dzIPC::detail::SetSeamHook(&hook);
    if (std::getenv("W06_NO_PARENT_POOL") == nullptr)
    {
        const std::string t = "w06fa_parent_" + std::to_string(::getpid());
        dzIPC::shm::shm_pub_ipc p{td(), t, 0, false};
        p.InitChannel();
        dzIPC::shm::shm_sub_ipc s{td(), t, 0, 8, false};
        s.InitChannel();
        std::this_thread::sleep_for(400ms);
        std::printf("[FA] PARENT: pool_running=%d route_count=%zu worker=%ld compat=%ld reason=%d\n",
                    (int)dzIPC::threepools::RecvWorkerPool::instance().running(),
                    dzIPC::threepools::RecvWorkerPool::instance().route_count(), g_worker.load(),
                    g_compat.load(), g_reason.load());
        ::fflush(stdout);
        const pid_t pid = ::fork();
        if (pid == 0)
        {
            g_worker.store(0);
            g_compat.store(0);
            g_reason.store(-1);
            child_body("forked");
            ::_exit(0);
        }
        int st = 0;
        ::waitpid(pid, &st, 0);
        std::printf("[FA] PARENT child status=%d\n", st);
        ipc::route::clear_storage(shm_topic_segment_name(t, 0).c_str());
        ipc::shm::handle::clear_storage(shm_topic_control_name(t, 0).c_str());
    }
    else
    {
        /* 父进程完全不碰池：子进程里的 owner pid 会记录成**子进程自己**。 */
        const std::size_t before = dzIPC::threepools::RecvWorkerPool::instance().route_count();
        const bool running = dzIPC::threepools::RecvWorkerPool::instance().running();
        std::printf("[FA] PARENT(no-pool): running=%d route_count=%zu\n", (int)running, before);
        ::fflush(stdout);
        const pid_t pid = ::fork();
        if (pid == 0)
        {
            g_worker.store(0);
            g_compat.store(0);
            g_reason.store(-1);
            child_body("freshchild");
            ::_exit(0);
        }
        int st = 0;
        ::waitpid(pid, &st, 0);
        std::printf("[FA] PARENT(no-pool) child status=%d\n", st);
    }
    dzIPC::detail::SetSeamHook(nullptr);
    return 0;
}

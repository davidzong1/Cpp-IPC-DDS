/* 为「既有回归在两条接收臂下的等价正对照」取证：同一段场景在两臂下各跑一遍，
 * 打印可机读的臂判据。场景 = test_wakeup_artifact 的 NoPhantomMessageOnGenerationRebuild
 * （sub 常驻、3 轮起停 pub），外加一次控制面 set_stopping/begin_rebuild 转换。 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>

#include <unistd.h>

#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/common/wire_accept.h"
#include "dzIPC/detail/shm_sub_seam.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/threepools/recv_worker.h"
#include "ipc_msg/std_msgs/std_string.hpp"

using namespace std::chrono_literals;
static constexpr std::uint32_t kMsgId = 0;

static std::atomic<long> g_after_recv{0};
static std::atomic<long> g_after_release{0};
static std::atomic<long> g_worker_path{0};
static std::atomic<long> g_compat_path{0};
static std::atomic<int> g_compat_reason{-1};

static void hook(const dzIPC::detail::SeamEvent& ev) noexcept
{
    switch (ev.point)
    {
    case dzIPC::detail::SeamPoint::kAfterRecv: ++g_after_recv; break;
    case dzIPC::detail::SeamPoint::kAfterRecvRelease: ++g_after_release; break;
    case dzIPC::detail::SeamPoint::kRecvPathWorker: ++g_worker_path; break;
    case dzIPC::detail::SeamPoint::kRecvPathCompat:
        ++g_compat_path;
        g_compat_reason.store(static_cast<int>(ev.size));
        break;
    default: break;
    }
}

static std::shared_ptr<dzIPC::TopicData> td()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), kMsgId);
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    dzIPC::detail::SetSeamHook(&hook);
    const std::string topic = "w06arms_" + std::to_string(::getpid());
    auto& pool = dzIPC::threepools::RecvWorkerPool::instance();

    auto sub_td = td();
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8, false};
    sub.InitChannel();
    std::this_thread::sleep_for(300ms);
    const auto s0 = pool.stats();
    std::printf("[AR] after sub init: pool_route_count=%zu threads=%zu after_recv=%ld after_release=%ld\n",
                pool.route_count(), pool.worker_count(), g_after_recv.load(), g_after_release.load());
    std::printf("[AR] seam worker_path=%ld compat_path=%ld reason=%d\n", g_worker_path.load(), g_compat_path.load(),
                g_compat_reason.load());

    for (int i = 0; i < 3; ++i)
    {
        {
            auto pub_td = td();
            dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0, false};
            pub.InitChannel();
            std::this_thread::sleep_for(300ms);
        }
        std::this_thread::sleep_for(300ms);
    }
    const auto s1 = pool.stats();
    std::printf("[AR] after 3 pub rounds: artifacts=%llu after_recv=%ld after_release=%ld\n",
                (unsigned long long)dzIPC::WakeupArtifactCount(), g_after_recv.load(), g_after_release.load());
    std::printf("[AR] pool: route_count=%zu recv_once_calls=%llu wait_wakeups=%llu wait_timeouts=%llu "
                "idle_exits=%llu restarts=%llu (deltas %llu/%llu)\n",
                pool.route_count(), (unsigned long long)s1.recv_once_calls,
                (unsigned long long)s1.wait_wakeups, (unsigned long long)s1.wait_timeouts,
                (unsigned long long)s1.idle_exits, (unsigned long long)s1.thread_restarts,
                (unsigned long long)(s1.recv_once_calls - s0.recv_once_calls),
                (unsigned long long)(s1.wait_wakeups - s0.wait_wakeups));
    std::printf("[AR] seam worker_path=%ld compat_path=%ld reason=%d\n", g_worker_path.load(), g_compat_path.load(),
                g_compat_reason.load());

    /* 控制面驱动**两轮**状态转换（与 test_shm_ready_transition 同一路径）。 */
    for (int round = 0; round < 2; ++round)
    {
        const auto t = g_after_release.load();
        const auto pr = pool.route_count();
        {
            dzIPC::control_plane_shm::TopicControlPlane cp;
            if (!cp.open(shm_topic_control_name(topic, 0)))
            {
                std::printf("[AR] round %d: cp open FAILED\n", round);
                break;
            }
            cp.set_stopping();
            const auto dl0 = std::chrono::steady_clock::now() + 3s;
            while (std::chrono::steady_clock::now() < dl0 && cp.peer_count() != 0)
                std::this_thread::sleep_for(10ms);
            const auto peers_after_stop = cp.peer_count();
            cp.begin_rebuild();
            cp.set_ready();
            const auto dl1 = std::chrono::steady_clock::now() + 3s;
            while (std::chrono::steady_clock::now() < dl1 && cp.peer_count() != 1)
                std::this_thread::sleep_for(10ms);
            std::printf("[AR] round %d: peers_after_stop=%u peers_after_ready=%u gen=%u route_count_before=%zu "
                        "route_count_after=%zu after_release=%ld\n",
                        round, peers_after_stop, cp.peer_count(), cp.generation(), pr, pool.route_count(),
                        g_after_release.load());
        }
        const auto dl = std::chrono::steady_clock::now() + 3s;
        while (std::chrono::steady_clock::now() < dl && g_after_release.load() == t)
        {
            std::this_thread::sleep_for(10ms);
        }
        std::printf("[AR] round %d: tick_advanced=%s worker_path=%ld compat_path=%ld reason=%d\n", round,
                    g_after_release.load() != t ? "yes" : "no", g_worker_path.load(), g_compat_path.load(),
                    g_compat_reason.load());
    }
    std::printf("[AR] post-transitions: artifacts=%llu\n", (unsigned long long)dzIPC::WakeupArtifactCount());

    /* 真消息仍能到达（阴性对照）。 */
    {
        auto pub_td = td();
        dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0, false};
        pub.InitChannel();
        std::this_thread::sleep_for(500ms);
        auto m = std::make_shared<dzIPC::Msg::StdString>();
        m->set_msg_id(kMsgId);
        m->str = "REAL";
        const bool ok = pub.publish(m);
        auto sink = td();
        long long us = -1;
        const auto t0 = std::chrono::steady_clock::now();
        for (int i = 0; i < 3000 && us < 0; ++i)
        {
            if (sub.try_get_clone(sink)) us = std::chrono::duration_cast<std::chrono::microseconds>(
                                                 std::chrono::steady_clock::now() - t0).count();
            else std::this_thread::sleep_for(1ms);
        }
        std::printf("[AR] real message publish=%d delivered_us=%lld\n", static_cast<int>(ok), us);
    }
    dzIPC::detail::SetSeamHook(nullptr);
    std::printf("W06_ARMS_DONE\n");
    return 0;
}

/* W06 诊断探针 B：保持 pub 存活 vs 立刻销毁 pub，区分"worker 收不到"与"route 被提前摘掉"。 */
#include <atomic>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <unistd.h>

#include "dzIPC/common/control_plane.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/detail/shm_sub_seam.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/threepools/recv_worker.h"
#include "ipc_msg/std_msgs/std_string.hpp"

using namespace std::chrono_literals;

int main(int argc, char** argv)
{
    const std::string mode = (argc > 1) ? std::string(argv[1]) : std::string("keep");
    const bool keep_pub = (mode == "keep");
    const int hold_ms = (mode == "hold") ? 100 : 0;
    static std::atomic<std::uint64_t> after_recv{0};
    static std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    dzIPC::detail::SetSeamHook([](const dzIPC::detail::SeamEvent& ev) noexcept {
        if (ev.point == dzIPC::detail::SeamPoint::kAfterRecv)
        {
            after_recv.fetch_add(1, std::memory_order_relaxed);
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - t0).count();
            std::printf("[seam] after_recv t=%lldus gen=%u size=%zu connected=%u\n", (long long)us, ev.generation,
                        ev.size, (ev.route != nullptr) ? ev.route->connected_id() : 0u);
            std::fflush(stdout);
        }
        else if (ev.point == dzIPC::detail::SeamPoint::kAfterRecvRelease)
        {
            const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                                std::chrono::steady_clock::now() - t0).count();
            std::printf("[seam] released      t=%lldus size=%zu\n", (long long)us, ev.size);
            std::fflush(stdout);
        }
    });

    const std::string topic = std::string("w06probeB_") + (keep_pub ? "keep_" : "drop_") + std::to_string(::getpid());
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 0);
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8, false};
    sub.InitChannel();

    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 0);
    auto pub = std::make_shared<dzIPC::shm::shm_pub_ipc>(pub_td, topic, 0, false);
    pub->InitChannel();
    std::this_thread::sleep_for(400ms);
    auto msg = std::make_shared<dzIPC::Msg::StdString>();
    msg->str = "HELLO";
    const auto st_before = dzIPC::threepools::RecvWorkerPool::instance().stats();
    const auto us_before = std::chrono::duration_cast<std::chrono::microseconds>(
                               std::chrono::steady_clock::now() - t0).count();
    const int ok = static_cast<int>(pub->publish(msg));
    const auto us_after = std::chrono::duration_cast<std::chrono::microseconds>(
                              std::chrono::steady_clock::now() - t0).count();
    std::printf("[probe] publish=%d mode=%s t=%lldus (enter %lldus)\n", ok, mode.c_str(), (long long)us_after,
                (long long)us_before);
    {
        const auto st = dzIPC::threepools::RecvWorkerPool::instance().stats();
        std::printf("[probe]   stats delta: wakeups=%llu timeouts=%llu calls=%llu msgs=%llu\n",
                    (unsigned long long)(st.wait_wakeups - st_before.wait_wakeups),
                    (unsigned long long)(st.wait_timeouts - st_before.wait_timeouts),
                    (unsigned long long)(st.recv_once_calls - st_before.recv_once_calls),
                    (unsigned long long)(st.messages_received - st_before.messages_received));
    }
    if (hold_ms > 0)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{hold_ms});
        std::printf("[probe] hold=%dms after_recv=%llu\n", hold_ms, (unsigned long long)after_recv.load());
        {
            const auto st = dzIPC::threepools::RecvWorkerPool::instance().stats();
            std::printf("[probe]   stats delta: wakeups=%llu timeouts=%llu calls=%llu msgs=%llu\n",
                        (unsigned long long)(st.wait_wakeups - st_before.wait_wakeups),
                        (unsigned long long)(st.wait_timeouts - st_before.wait_timeouts),
                        (unsigned long long)(st.recv_once_calls - st_before.recv_once_calls),
                        (unsigned long long)(st.messages_received - st_before.messages_received));
        }
    }
    if (!keep_pub)
    {
        pub.reset();
        std::printf("[probe] pub destroyed right after publish\n");
    }
    std::this_thread::sleep_for(400ms);
    std::printf("[probe] after_recv_events=%llu pool_routes=%zu\n",
                (unsigned long long)after_recv.load(),
                dzIPC::threepools::RecvWorkerPool::instance().route_count());

    std::string seen;
    auto sink = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 0);
    for (int i = 0; i < 40; ++i)
    {
        if (sub.try_get_clone(sink))
        {
            auto s = sink->topic()->msgcast<dzIPC::Msg::StdString>();
            if (s) seen += s->str + ",";
        }
        else
        {
            std::this_thread::sleep_for(20ms);
        }
    }
    std::printf("[probe] keep_pub=%d seen=[%s]\n", static_cast<int>(keep_pub), seen.c_str());
    if (pub) pub.reset();

    dzIPC::detail::SetSeamHook(nullptr);
    ipc::route::clear_storage(shm_topic_segment_name(topic, 0).c_str());
    ipc::shm::handle::clear_storage(shm_topic_control_name(topic, 0).c_str());
    return 0;
}

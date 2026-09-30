/* W06 诊断探针 F（判定性）：逐字复刻 test_wakeup_artifact 的 rebuild 场景，打印每一次
 * recv_once / 路径事件 / 池计数，定位"真消息在重建前后都丢"的机制。 */
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

static std::chrono::steady_clock::time_point g_t0;

static void stamp()
{
    const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                        std::chrono::steady_clock::now() - g_t0).count();
    std::printf("t=%7lldus ", (long long)us);
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_t0 = std::chrono::steady_clock::now();

    dzIPC::detail::SetSeamHook([](const dzIPC::detail::SeamEvent& ev) noexcept {
        switch (ev.point)
        {
        case dzIPC::detail::SeamPoint::kRecvPathWorker:
            stamp(); std::printf("[seam] PATH=WORKER\n");
            break;
        case dzIPC::detail::SeamPoint::kRecvPathCompat:
            stamp(); std::printf("[seam] PATH=COMPAT reason=%zu\n", ev.size);
            break;
        case dzIPC::detail::SeamPoint::kAfterRecv:
            stamp(); std::printf("[seam] recv gen=%u size=%zu connected=%u route=%p\n", ev.generation, ev.size,
                                 (ev.route != nullptr) ? ev.route->connected_id() : 0u,
                                 static_cast<const void*>(ev.route));
            break;
        case dzIPC::detail::SeamPoint::kAfterRecvRelease:
            stamp(); std::printf("[seam] released gen=%u size=%zu\n", ev.generation, ev.size);
            break;
        default:
            break;
        }
        std::fflush(stdout);
    });

    const std::string topic = "w06probeF_" + std::to_string(::getpid());
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 0);
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8, false};
    sub.InitChannel();

    auto publish_marker = [&](const std::string& marker) {
        stamp(); std::printf("[step] publish_marker(%s): creating pub\n", marker.c_str());
        auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 0);
        dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0, false};
        pub.InitChannel();
        stamp(); std::printf("[step] pub attached; sleeping 300ms\n");
        std::this_thread::sleep_for(300ms);
        auto msg = std::make_shared<dzIPC::Msg::StdString>();
        msg->str = marker;
        const bool ok = pub.publish(msg);
        stamp(); std::printf("[step] publish(%s)=%d; destroying pub now\n", marker.c_str(), static_cast<int>(ok));
        return ok;
    };

    const bool a = publish_marker("BEFORE_REBUILD");
    stamp(); std::printf("[step] pub1 gone (ret=%d)\n", static_cast<int>(a));
    std::this_thread::sleep_for(400ms);
    const bool b = publish_marker("AFTER_REBUILD");
    stamp(); std::printf("[step] pub2 gone (ret=%d)\n", static_cast<int>(b));
    std::this_thread::sleep_for(600ms);

    stamp(); std::printf("[step] draining\n");
    std::string seen;
    auto sink = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 0);
    for (int i = 0; i < 40; ++i)
    {
        if (sub.try_get_clone(sink))
        {
            auto s = sink->topic()->msgcast<dzIPC::Msg::StdString>();
            if (s && !s->str.empty()) seen += s->str + ",";
        }
        if (seen.find("BEFORE_REBUILD") != std::string::npos && seen.find("AFTER_REBUILD") != std::string::npos)
            break;
        std::this_thread::sleep_for(20ms);
    }
    const auto st = dzIPC::threepools::RecvWorkerPool::instance().stats();
    std::printf("[F] seen=[%s] pool: routes=%zu calls=%llu msgs=%llu bytes=%llu wakeups=%llu timeouts=%llu "
                "idle_exits=%llu restarts=%llu\n",
                seen.c_str(), st.route_count, (unsigned long long)st.recv_once_calls,
                (unsigned long long)st.messages_received, (unsigned long long)st.bytes_received,
                (unsigned long long)st.wait_wakeups, (unsigned long long)st.wait_timeouts,
                (unsigned long long)st.idle_exits, (unsigned long long)st.thread_restarts);

    dzIPC::detail::SetSeamHook(nullptr);
    ipc::route::clear_storage(shm_topic_segment_name(topic, 0).c_str());
    ipc::shm::handle::clear_storage(shm_topic_control_name(topic, 0).c_str());
    return 0;
}

/* W06 诊断探针（过程证据，不属于交付 API）：worker 臂下 generation 重建前后能否收包。 */
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

static bool wait_for(const std::function<bool()>& p, int ms)
{
    const auto dl = std::chrono::steady_clock::now() + std::chrono::milliseconds{ms};
    while (std::chrono::steady_clock::now() < dl)
    {
        if (p()) return true;
        std::this_thread::sleep_for(5ms);
    }
    return p();
}

static const char* step_name(dzIPC::detail::SeamPoint p)
{
    switch (p)
    {
    case dzIPC::detail::SeamPoint::kRecvPathWorker: return "recv_path_worker";
    case dzIPC::detail::SeamPoint::kRecvPathCompat: return "recv_path_compat";
    default: return "other";
    }
}

int main()
{
    static std::atomic<std::uint64_t> recv_events{0};
    static std::atomic<std::uint64_t> recv_bytes{0};
    dzIPC::detail::SetSeamHook([](const dzIPC::detail::SeamEvent& ev) noexcept {
        if (ev.point == dzIPC::detail::SeamPoint::kRecvPathWorker
            || ev.point == dzIPC::detail::SeamPoint::kRecvPathCompat)
        {
            /* 路径事件是**跨实例**的：用 topic 名标识本实例（第一个 compat 事件带原因码）。 */
            std::printf("[seam] %s event\n", step_name(ev.point));
        }
        else if (ev.point == dzIPC::detail::SeamPoint::kAfterRecv)
        {
            recv_events.fetch_add(1, std::memory_order_relaxed);
            recv_bytes.fetch_add(ev.size, std::memory_order_relaxed);
            std::printf("[seam] after_recv gen=%u size=%zu connected=%u\n", ev.generation, ev.size,
                        (ev.route != nullptr) ? ev.route->connected_id() : 0u);
        }
    });

    const std::string topic = "w06probe_" + std::to_string(::getpid());
    std::printf("[probe] topic=%s pid=%d\n", topic.c_str(), static_cast<int>(::getpid()));
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 0);
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8, false};
    sub.InitChannel();

    auto publish = [&](const std::string& marker) {
        auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 0);
        auto pub = std::make_shared<dzIPC::shm::shm_pub_ipc>(pub_td, topic, 0, false);
        pub->InitChannel();
        std::this_thread::sleep_for(300ms);
        auto msg = std::make_shared<dzIPC::Msg::StdString>();
        msg->str = marker;
        const bool ok = pub->publish(msg);
        std::printf("[probe] publish(%s)=%d pool_routes=%zu\n", marker.c_str(), static_cast<int>(ok),
                    dzIPC::threepools::RecvWorkerPool::instance().route_count());
        return ok;
    };

    publish("BEFORE");
    std::this_thread::sleep_for(400ms);
    std::printf("[probe] recv_events=%llu recv_bytes=%llu\n",
                (unsigned long long)recv_events.load(), (unsigned long long)recv_bytes.load());

    int n = 0;
    std::string seen;
    auto sink = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 0);
    for (int i = 0; i < 60 && n < 8; ++i)
    {
        if (sub.try_get_clone(sink))
        {
            auto s = sink->topic()->msgcast<dzIPC::Msg::StdString>();
            if (s) { seen += s->str + ","; ++n; }
        }
        else
        {
            std::this_thread::sleep_for(20ms);
        }
    }
    std::printf("[probe] after BEFORE: got=%d seen=[%s] pool_routes=%zu\n", n, seen.c_str(),
                dzIPC::threepools::RecvWorkerPool::instance().route_count());

    publish("AFTER");
    std::this_thread::sleep_for(600ms);
    int m = 0;
    seen.clear();
    for (int i = 0; i < 60 && m < 8; ++i)
    {
        if (sub.try_get_clone(sink))
        {
            auto s = sink->topic()->msgcast<dzIPC::Msg::StdString>();
            if (s) { seen += s->str + ","; ++m; }
        }
        else
        {
            std::this_thread::sleep_for(20ms);
        }
    }
    std::printf("[probe] after AFTER: got=%d seen=[%s] pool_routes=%zu\n", m, seen.c_str(),
                dzIPC::threepools::RecvWorkerPool::instance().route_count());

    auto cp = dzIPC::control_plane_shm::TopicControlPlane{};
    if (cp.open(shm_topic_control_name(topic, 0)))
    {
        std::printf("[probe] cp state=%d gen=%u peers=%u\n", static_cast<int>(cp.state()), cp.generation(),
                    cp.peer_count());
    }
    const auto st = dzIPC::threepools::RecvWorkerPool::instance().stats();
    std::printf("[probe] pool stats: routes=%zu calls=%llu msgs=%llu bytes=%llu yields=%llu deferred=%llu "
                "idle_exits=%llu restarts=%llu\n",
                st.route_count, (unsigned long long)st.recv_once_calls,
                (unsigned long long)st.messages_received, (unsigned long long)st.bytes_received,
                (unsigned long long)st.budget_yields, (unsigned long long)st.deferred_drains,
                (unsigned long long)st.idle_exits, (unsigned long long)st.thread_restarts);

    dzIPC::detail::SetSeamHook(nullptr);
    ipc::route::clear_storage(shm_topic_segment_name(topic, 0).c_str());
    ipc::shm::handle::clear_storage(shm_topic_control_name(topic, 0).c_str());
    return 0;
}

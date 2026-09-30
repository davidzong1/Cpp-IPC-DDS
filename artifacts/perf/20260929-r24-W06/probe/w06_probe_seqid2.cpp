/* 探针 G（判定性）：拿到 worker 真正在等的那条 route 对象后，
 *   · 读它的 read_wait_token().sequence() 字地址与值；
 *   · 在同名段上另建一条 receiver，读它的 token 字地址与值；
 *   · publish 后逐一复读 —— 谁变了、谁没变，一望即知。 */
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
#include "libipc/ipc.h"

using namespace std::chrono_literals;

static std::atomic<const ipc::route*> g_route{nullptr};

static std::uint32_t seq_of(const ipc::route* r)
{
    if (r == nullptr) return 0xDEADBEEF;
    const auto t = r->read_wait_token();
    return (t.sequence() != nullptr) ? t.sequence()->load() : 0xBAD0BAD0;
}
static const void* ptr_of(const ipc::route* r)
{
    if (r == nullptr) return nullptr;
    return static_cast<const void*>(r->read_wait_token().sequence());
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    dzIPC::detail::SetSeamHook([](const dzIPC::detail::SeamEvent& ev) noexcept {
        if ((ev.point == dzIPC::detail::SeamPoint::kAfterRecv
             || ev.point == dzIPC::detail::SeamPoint::kAfterRecvRelease)
            && ev.route != nullptr)
        {
            g_route.store(ev.route);
        }
    });

    const std::string topic = "w06probeG_" + std::to_string(::getpid());
    const std::string seg = shm_topic_segment_name(topic, 0);
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 0);
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8, false};
    sub.InitChannel();
    std::this_thread::sleep_for(300ms);

    const ipc::route* r = g_route.load();
    std::printf("[G] segment=%s\n", seg.c_str());
    std::printf("[G] worker-route ptr=%p token-seq ptr=%p val=%u connected=%u\n", static_cast<const void*>(r),
                ptr_of(r), seq_of(r), (r != nullptr) ? r->connected_id() : 0u);

    /* 另建一条 receiver（同名同前缀）作为对照组。 */
    ipc::route probe_rx{seg.c_str(), ipc::receiver, false};
    std::printf("[G] local-probe-rx      token-seq ptr=%p val=%u connected=%u\n", ptr_of(&probe_rx),
                seq_of(&probe_rx), probe_rx.connected_id());

    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 0);
    auto pub = std::make_shared<dzIPC::shm::shm_pub_ipc>(pub_td, topic, 0, false);
    pub->InitChannel();
    std::this_thread::sleep_for(300ms);
    std::printf("[G] pre-publish: worker-route val=%u local-probe val=%u pool_wakeups=%llu calls=%llu\n",
                seq_of(r), seq_of(&probe_rx),
                (unsigned long long)dzIPC::threepools::RecvWorkerPool::instance().stats().wait_wakeups,
                (unsigned long long)dzIPC::threepools::RecvWorkerPool::instance().stats().recv_once_calls);

    auto msg = std::make_shared<dzIPC::Msg::StdString>();
    msg->str = "HELLO";
    std::printf("[G] publish=%d\n", static_cast<int>(pub->publish(msg)));
    for (int i = 0; i < 12; ++i)
    {
        std::this_thread::sleep_for(25ms);
        const auto st = dzIPC::threepools::RecvWorkerPool::instance().stats();
        std::printf("[G] +%3dms worker-route val=%u local-probe val=%u | wakeups=%llu calls=%llu msgs=%llu\n",
                    (i + 1) * 25, seq_of(r), seq_of(&probe_rx), (unsigned long long)st.wait_wakeups,
                    (unsigned long long)st.recv_once_calls, (unsigned long long)st.messages_received);
    }

    /* 对照组自己 recv 一次，确认消息确实在段里。 */
    const auto d = probe_rx.recv(100);
    std::printf("[G] local-probe recv size=%zu\n", d.size());

    dzIPC::detail::SetSeamHook(nullptr);
    pub.reset();
    ipc::route::clear_storage(seg.c_str());
    ipc::shm::handle::clear_storage(shm_topic_control_name(topic, 0).c_str());
    return 0;
}

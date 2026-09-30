/* 探针 P（判定性）：比较"dz 自己的 pub"与"我手工建的 raw ipc::route sender"在同一
 * 订阅上的唤醒效果；并对照两者的 rd_waiter seq 字（值相等 ⇒ 同一物理页）。
 * 若 raw sender 立即送达而 dz pub 不送达 ⇒ 差异在 dz 的 pub 路径（含 clear_storage）。 */
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
static std::atomic<long long> g_recv_us{-1};
static std::chrono::steady_clock::time_point g_t0;
static std::atomic<std::uint64_t> g_events{0};

static std::uint32_t seqv(const ipc::route* r)
{
    if (r == nullptr) return 0xDEADBEEF;
    const auto t = r->read_wait_token();
    return (t.sequence() != nullptr) ? t.sequence()->load() : 0u;
}
static const void* seqp(const ipc::route* r)
{
    if (r == nullptr) return nullptr;
    return static_cast<const void*>(r->read_wait_token().sequence());
}
static long long now_us()
{
    return std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_t0).count();
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    g_t0 = std::chrono::steady_clock::now();

    dzIPC::detail::SetSeamHook([](const dzIPC::detail::SeamEvent& ev) noexcept {
        if (ev.point == dzIPC::detail::SeamPoint::kAfterRecv)
        {
            g_events.fetch_add(1);
            if (ev.route != nullptr) g_route.store(ev.route);
            if (ev.size > 0)
            {
                long long expect = -1;
                g_recv_us.compare_exchange_strong(expect, now_us());
                std::printf("  t=%7lldus [seam] DATA size=%zu\n", now_us(), ev.size);
            }
        }
    });

    const std::string topic = "w06probeP_" + std::to_string(::getpid());
    const std::string seg = shm_topic_segment_name(topic, 0);
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 0);
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8, false};
    sub.InitChannel();

    /* A 段：dz 自己的 pub（含 clear_storage + 控制面）。 */
    {
        auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 0);
        dzIPC::shm::shm_pub_ipc pub{pub_td, topic, 0, false};
        pub.InitChannel();
        std::this_thread::sleep_for(700ms);   /* 让 worker 把 attach 通知与重建都消化掉 */
        std::printf("t=%7lldus [A] dz pub attached & settled\n", now_us());
        for (int round = 1; round <= 4; ++round)
        {
            auto msg = std::make_shared<dzIPC::Msg::StdString>();
            msg->str = "FROM_DZ_PUB_" + std::to_string(round);
            g_recv_us.store(-1);
            const auto t_send = now_us();
            const bool ok = pub.publish(msg);
            std::this_thread::sleep_for(250ms);
            std::printf("t=%7lldus [A] round %d publish=%d first_data_after_send=%lldus\n", now_us(), round,
                        static_cast<int>(ok), g_recv_us.load() < 0 ? -1 : g_recv_us.load() - t_send);
        }
    }
    std::this_thread::sleep_for(500ms);
    std::printf("t=%7lldus [A] dz pub destroyed (rebuild expected)\n", now_us());

    /* B 段：raw sender，不碰 clear_storage。 */
    {
        ipc::route raw_sender{seg.c_str(), ipc::sender, false};
        std::this_thread::sleep_for(400ms);
        const ipc::route* r = g_route.load();
        std::printf("t=%7lldus [B] raw sender attached. worker-route=%p token-seq ptr=%p val=%u | raw-sender "
                    "token-seq ptr=%p val=%u\n",
                    now_us(), static_cast<const void*>(r), seqp(r), seqv(r), seqp(&raw_sender), seqv(&raw_sender));
        g_recv_us.store(-1);
        const auto t_send = now_us();
        const bool ok = raw_sender.try_send("FROM_RAW_SENDER", 100);
        std::printf("t=%7lldus [B] raw try_send=%d\n", now_us(), static_cast<int>(ok));
        std::this_thread::sleep_for(300ms);
        const ipc::route* r2 = g_route.load();
        std::printf("t=%7lldus [B] raw result: first_data_at=%lldus (after send %lldus) worker-seq val=%u "
                    "raw-sender-seq val=%u events=%llu\n",
                    now_us(), g_recv_us.load(), g_recv_us.load() < 0 ? -1 : g_recv_us.load() - t_send, seqv(r2),
                    seqv(&raw_sender), (unsigned long long)g_events.load());
        raw_sender.clear();
    }
    ipc::route::clear_storage(seg.c_str());
    dzIPC::detail::SetSeamHook(nullptr);
    return 0;
}

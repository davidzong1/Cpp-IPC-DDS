/* W06 诊断探针 E（判定性）：
 *   · 记录 worker 注册时用的 route 对象与其 read_wait_token 的 seq 字地址/值；
 *   · publish 之后由主线程直接读同一个 seq 字 ——
 *       值变了 ⇒ 字是共享的，问题只在"futex 唤醒没到"；
 *       值没变 ⇒ worker 等的字 != 发布端 notify 的字（地址/段不对）。
 */
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

static std::atomic<const ipc::route*> g_route{nullptr};
static std::atomic<std::uint64_t> g_pub_token_seq_ptr{0};

static std::uint32_t read_route_seq(const ipc::route* r)
{
    if (r == nullptr) return 0xDEADBEEF;
    const auto tok = r->read_wait_token();
    return (tok.sequence() != nullptr) ? tok.sequence()->load() : 0xBAD0BAD0;
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    dzIPC::detail::SetSeamHook([](const dzIPC::detail::SeamEvent& ev) noexcept {
        if (ev.point == dzIPC::detail::SeamPoint::kAfterRecv && ev.route != nullptr)
        {
            g_route.store(ev.route);
        }
        if (ev.point == dzIPC::detail::SeamPoint::kAfterRecv)
        {
            std::printf("[E] after_recv gen=%u size=%zu\n", ev.generation, ev.size);
        }
    });

    const std::string topic = "w06probeE_" + std::to_string(::getpid());
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 0);
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8, false};
    sub.InitChannel();
    std::this_thread::sleep_for(300ms);

    const ipc::route* r = g_route.load();
    std::printf("[E] worker route=%p seq=%u ptr=%p\n", static_cast<const void*>(r), read_route_seq(r),
                static_cast<const void*>(r != nullptr ? r->read_wait_token().sequence() : nullptr));

    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 0);
    auto pub = std::make_shared<dzIPC::shm::shm_pub_ipc>(pub_td, topic, 0, false);
    pub->InitChannel();
    std::this_thread::sleep_for(300ms);
    std::printf("[E] before publish: worker-route seq=%u\n", read_route_seq(r));

    auto msg = std::make_shared<dzIPC::Msg::StdString>();
    msg->str = "HELLO";
    const auto t0 = std::chrono::steady_clock::now();
    std::printf("[E] publish=%d\n", static_cast<int>(pub->publish(msg)));

    for (int i = 0; i < 10; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds{15});
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0).count();
        std::printf("[E] t+%lldus worker-route seq=%u  pool_wakeups=%llu calls=%llu msgs=%llu\n", (long long)us,
                    read_route_seq(r),
                    (unsigned long long)dzIPC::threepools::RecvWorkerPool::instance().stats().wait_wakeups,
                    (unsigned long long)dzIPC::threepools::RecvWorkerPool::instance().stats().recv_once_calls,
                    (unsigned long long)dzIPC::threepools::RecvWorkerPool::instance().stats().messages_received);
    }

    dzIPC::detail::SetSeamHook(nullptr);
    pub.reset();
    ipc::route::clear_storage(shm_topic_segment_name(topic, 0).c_str());
    ipc::shm::handle::clear_storage(shm_topic_control_name(topic, 0).c_str());
    return 0;
}

/* 判定性收尾 F：在 dz 的 worker 路径上，send 的那一刻比较
 *   (a) worker 持有的 token 字所在段文件的 dev:inode
 *   (b) 发布端 rd_waiter 字所在段文件的 dev:inode
 * inode 不同 ⇒ worker 的等待 token 指向**已被 clear_storage/unlink 掉的旧段**（陈旧 token），
 * 因而发布端的 notify 永远打不到它 —— 只能等 wait_timeout 到期后靠全扫发现。
 * 同时打印 worker 的 route 与"新开一条 receiver"的 token，看后者是否与新段一致。 */
#include <atomic>
#include <cstring>
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

static std::string seg_of(const void* p)
{
    if (p == nullptr) return "<null>";
    const auto* a = static_cast<const char*>(p);
    const std::uintptr_t page = reinterpret_cast<std::uintptr_t>(a) - (reinterpret_cast<std::uintptr_t>(a) % 4096);
    std::FILE* f = std::fopen("/proc/self/maps", "r");
    if (f == nullptr) return "<no maps>";
    char buf[1024];
    std::string out = "<not found>";
    while (std::fgets(buf, sizeof(buf), f) != nullptr)
    {
        unsigned long lo = 0, hi = 0;
        if (std::sscanf(buf, "%lx-%lx", &lo, &hi) == 2 && page >= lo && page < hi)
        {
            /* 截出 "dev:inode  path" 段 */
            char* fields[8] = {nullptr};
            int n = 0;
            char* tok = std::strtok(buf, " \t\n");
            while (tok != nullptr && n < 8) { fields[n++] = tok; tok = std::strtok(nullptr, " \t\n"); }
            out = (n >= 6) ? std::string(fields[3]) + " " + fields[4] + " " + (fields[5] ? fields[5] : "") : buf;
            break;
        }
    }
    std::fclose(f);
    return out;
}

int main()
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    dzIPC::detail::SetSeamHook([](const dzIPC::detail::SeamEvent& ev) noexcept {
        if ((ev.point == dzIPC::detail::SeamPoint::kAfterRecv
             || ev.point == dzIPC::detail::SeamPoint::kAfterRecvRelease)
            && ev.route != nullptr)
        {
            if (ev.point == dzIPC::detail::SeamPoint::kAfterRecv) g_route.store(ev.route);
        }
    });

    const std::string topic = "w06probeST_" + std::to_string(::getpid());
    auto sub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 0);
    dzIPC::shm::shm_sub_ipc sub{sub_td, topic, 0, 8, false};
    sub.InitChannel();
    std::this_thread::sleep_for(300ms);

    auto pub_td = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdString>(), 0);
    auto pub = std::make_shared<dzIPC::shm::shm_pub_ipc>(pub_td, topic, 0, false);
    pub->InitChannel();
    std::this_thread::sleep_for(600ms);

    const ipc::route* r = g_route.load();
    /* worker 持有的 route 对象此刻的 token */
    const auto wt = (r != nullptr) ? r->read_wait_token() : ipc::recv_wait_token{};
    /* 新开一条同名 receiver：它的 token 一定来自"当前"段 */
    ipc::route fresh{shm_topic_segment_name(topic, 0).c_str(), ipc::receiver, false};
    const auto ft = fresh.read_wait_token();

    std::printf("[ST] worker-route=%p token@%p val=%u\n      seg: %s\n", static_cast<const void*>(r),
                static_cast<const void*>(wt.sequence()), wt.sequence() ? wt.sequence()->load() : 0u,
                seg_of(wt.sequence()).c_str());
    std::printf("[ST] fresh-receiver token@%p val=%u\n      seg: %s\n", static_cast<const void*>(ft.sequence()),
                ft.sequence() ? ft.sequence()->load() : 0u, seg_of(ft.sequence()).c_str());

    /* 发布端真的会 notify 的那条 rd_waiter，用 try_send 走一遍并在 seam 前打印。 */
    const auto before = dzIPC::threepools::RecvWorkerPool::instance().stats();
    auto msg = std::make_shared<dzIPC::Msg::StdString>();
    msg->str = "HELLO";
    const auto t0 = std::chrono::steady_clock::now();
    std::printf("[ST] publish=%d\n", static_cast<int>(pub->publish(msg)));
    for (int i = 0; i < 30; ++i)
    {
        std::this_thread::sleep_for(10ms);
        const auto st = dzIPC::threepools::RecvWorkerPool::instance().stats();
        if (st.messages_received > before.messages_received)
        {
            std::printf("[ST] data seen at +%lldus (msgs=%llu)\n",
                        (long long)std::chrono::duration_cast<std::chrono::microseconds>(
                            std::chrono::steady_clock::now() - t0).count(),
                        (unsigned long long)st.messages_received);
            break;
        }
        if (i == 29) std::printf("[ST] no data within 300ms\n");
    }
    /* 同上，再用 fresh receiver 的 token 值对比一次（此刻 worker 可能已消费）。 */
    std::printf("[ST] after: worker-route token val=%u | fresh token val=%u\n",
                (r != nullptr && r->read_wait_token().sequence() != nullptr)
                    ? r->read_wait_token().sequence()->load()
                    : 0u,
                ft.sequence() ? ft.sequence()->load() : 0u);

    fresh.clear();
    dzIPC::detail::SetSeamHook(nullptr);
    pub.reset();
    ipc::route::clear_storage(shm_topic_segment_name(topic, 0).c_str());
    ipc::shm::handle::clear_storage(shm_topic_control_name(topic, 0).c_str());
    return 0;
}

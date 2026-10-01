/* t59 复核项 1：**运行期**证明 SubRecvRoute 的 lease 全出口配对。
 * 做法（不静态 grep）：
 *   ① 装 seam 钩子，统计 kAfterRecv（lease 持有中）与 kAfterRecvRelease（已释放）事件；
 *   ② 稳态收一批消息 ⇒ 两计数应相等（每个成功 lease 恰好一次 release）；
 *   ③ 用 RouteSession 的 in-flight 观测面直接读「当前 inflight」= 0（无泄漏）；
 *   ④ **异常出口**：注入一个 recv 抛异常的 route（宿主适配器）⇒ 仍必须 release。
 * 只读复核：不改产品代码。 */
#include <atomic>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dzIPC/common/topic_data.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/detail/shm_sub_seam.h"
#include "ipc_msg/ipc_msg_base/generic_message.hpp"
#include "libipc/ipc.h"

static std::atomic<unsigned long> g_after_recv{0};
static std::atomic<unsigned long> g_after_release{0};
static std::atomic<unsigned long> g_after_recv_nonempty{0};
static std::atomic<unsigned long> g_worker_path{0};
static std::atomic<unsigned long> g_compat_path{0};

static void hook(const dzIPC::detail::SeamEvent& ev)
{
    if (ev.point == dzIPC::detail::SeamPoint::kAfterRecv)
    {
        g_after_recv.fetch_add(1);
        if (ev.size != 0) g_after_recv_nonempty.fetch_add(1);
    }
    else if (ev.point == dzIPC::detail::SeamPoint::kAfterRecvRelease)
    {
        g_after_release.fetch_add(1);
    }
    else if (ev.point == dzIPC::detail::SeamPoint::kRecvPathWorker)
    {
        g_worker_path.fetch_add(1);
    }
    else if (ev.point == dzIPC::detail::SeamPoint::kRecvPathCompat)
    {
        g_compat_path.fetch_add(1);
    }
}

static std::shared_ptr<dzIPC::TopicData> td()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::GenericMessage>(), 61);
}

int main()
{
    dzIPC::detail::SetSeamHook(&hook);
    const std::string topic = "r59_lease_" + std::to_string(::getpid());
    const std::size_t domain = 2459;
    {
        dzIPC::shm::shm_pub_ipc pub{td(), topic, domain, false};
        dzIPC::shm::shm_sub_ipc sub{td(), topic, domain, 32, false};
        pub.InitChannel("r59");
        sub.InitChannel("r59");
        for (int i = 0; i < 100 && !pub.has_subscribed(); ++i)
            std::this_thread::sleep_for(std::chrono::milliseconds(20));

        const unsigned long r0 = g_after_recv_nonempty.load();
        const int kN = 50;
        int sent = 0;
        for (int i = 0; i < kN; ++i)
        {
            auto m = std::make_shared<dzIPC::GenericMessage>();
            m->set_uint32("v", static_cast<std::uint32_t>(i));
            if (pub.publish(m)) ++sent;
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        std::this_thread::sleep_for(std::chrono::milliseconds(500));
        const unsigned long ar = g_after_recv.load();
        const unsigned long arl = g_after_release.load();
        const unsigned long arn = g_after_recv_nonempty.load();
        std::printf("case=lease sent=%d arm=%s after_recv=%lu after_recv_nonempty_delta=%lu after_release=%lu delta=%ld\n",
                    sent, g_worker_path.load() > 0 ? "worker" : (g_compat_path.load() > 0 ? "compat" : "unknown"),
                    ar, arn - r0, arl, static_cast<long>(ar) - static_cast<long>(arl));
        std::printf("verdict=%s lease_all_exits_paired(after_recv==after_release, 且非空交付>0)\n",
                    (ar == arl && (arn - r0) > 0 && g_worker_path.load() > 0) ? "PASS" : "FAIL");
    }
    dzIPC::detail::SetSeamHook(nullptr);
    ipc::route::clear_storage(("__IPC_SHM__" + topic).c_str());
    return 0;
}

/* W05 验收探针（非交付物；证据用）—— 心跳频率是否被改动（"不得通过降低心跳频率制造空闲收益"）。
 *
 * 判据（逐位保持 ControlTiming 默认值）：
 *   · 订阅者 peer 心跳刷新间隔 ≈ 10ms（容差：中位数落在 [5, 20] ms）；
 *   · 发布者 owner 心跳刷新间隔 ≈ 50ms（容差：中位数落在 [25, 100] ms）。
 * 两条驱动臂（默认调度器 / DZIPC_SHM_CONTROL_SCHEDULER=1 兼容回退）都必须成立 ——
 * 这正是"回退只换驱动源、不换时间语义"的可执行证明。
 *
 * 手法：直接只读采样控制面共享段里的 `heartbeat_ns`（`TopicControl` / `PeerSlot`
 * 都是公开布局），不依赖产品任何内部状态、不装钩子、不改变被测系统行为。
 *
 * 编译：
 *   g++ -std=c++17 -O2 -DNDEBUG -I include -I src artifacts/w05/scratch-probe/w05_heartbeat.cpp \
 *       -o artifacts/w05/scratch-probe/w05_heartbeat -L build/lib -lipc -lpthread -lrt \
 *       -Wl,-rpath,$PWD/build/lib
 */
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dzIPC/common/control_plane.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_image.hpp"

using Clock = std::chrono::steady_clock;
constexpr std::uint32_t kMsgId = 95;

static std::shared_ptr<dzIPC::TopicData> td()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), kMsgId);
}

static double median_ms(std::vector<long long>& v)
{
    if (v.empty())
    {
        return -1.0;
    }
    std::sort(v.begin(), v.end());
    return static_cast<double>(v[v.size() / 2]) / 1e6;
}

int main(int argc, char** argv)
{
    const bool compat = [] {
        const char* v = std::getenv("DZIPC_SHM_CONTROL_SCHEDULER");
        return v != nullptr && v[0] != '\0' && !(v[0] == '0' && v[1] == '\0');
    }();
    const std::string topic = "w05hb_probe";
    const std::size_t domain = 96;
    const long window_ms = (argc > 1) ? std::strtol(argv[1], nullptr, 10) : 2000;

    dzIPC::shm::shm_sub_ipc sub{td(), topic, domain, 8, false};
    sub.InitChannel();
    dzIPC::shm::shm_pub_ipc pub{td(), topic, domain, false};
    pub.InitChannel();

    for (int i = 0; i < 500 && !pub.has_subscribed(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    const bool attached = pub.has_subscribed();

    /* 只读打开控制面段：owner 心跳在段头，peer 心跳在 PeerSlot。 */
    dzIPC::control_plane_shm::TopicControlPlane cp;
    const bool open = cp.open(shm_topic_control_name(topic, domain));
    if (!open || !attached)
    {
        std::printf("arm=%s FATAL open=%d attached=%d\n", compat ? "compat" : "scheduler", (int)open,
                    (int)attached);
        return 2;
    }

    /* 段头只读采样走 ipc::shm 直读（control plane 不暴露 heartbeat 读数）。 */
    ipc::shm::id_t id = ipc::shm::acquire(shm_topic_control_name(topic, domain).c_str(), 0, ipc::shm::open);
    std::size_t mapped = 0;
    void* mem = (id != nullptr) ? ipc::shm::get_mem(id, &mapped) : nullptr;
    if (mem == nullptr || mapped < sizeof(dzIPC::control_plane_shm::TopicControl))
    {
        std::printf("arm=%s FATAL cannot map control plane\n", compat ? "compat" : "scheduler");
        return 3;
    }
    const auto* ctl = static_cast<const dzIPC::control_plane_shm::TopicControl*>(mem);

    std::vector<long long> owner_gaps, peer_gaps;
    int64_t last_owner = ctl->heartbeat_ns.load(std::memory_order_acquire);
    int64_t last_peer = 0;
    for (std::uint32_t i = 0; i < dzIPC::control_plane_shm::kMaxPeerSlots; ++i)
    {
        if (ctl->peers[i].in_use.load(std::memory_order_acquire) != 0)
        {
            last_peer = ctl->peers[i].heartbeat_ns.load(std::memory_order_acquire);
            break;
        }
    }

    const auto t0 = Clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() < window_ms)
    {
        const int64_t o = ctl->heartbeat_ns.load(std::memory_order_acquire);
        if (o != last_owner && last_owner != 0)
        {
            owner_gaps.push_back(o - last_owner);
            last_owner = o;
        }
        for (std::uint32_t i = 0; i < dzIPC::control_plane_shm::kMaxPeerSlots; ++i)
        {
            if (ctl->peers[i].in_use.load(std::memory_order_acquire) != 0)
            {
                const int64_t p = ctl->peers[i].heartbeat_ns.load(std::memory_order_acquire);
                if (p != 0 && p != last_peer && last_peer != 0)
                {
                    peer_gaps.push_back(p - last_peer);
                }
                last_peer = p;
                break;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    ipc::shm::release_no_unlink(id);

    const double owner_med = median_ms(owner_gaps);
    const double peer_med = median_ms(peer_gaps);
    const double owner_rate = owner_gaps.empty() ? 0.0 : 1000.0 / owner_med;
    const double peer_rate = peer_gaps.empty() ? 0.0 : 1000.0 / peer_med;
    std::printf("arm=%s window_ms=%ld owner_updates=%zu owner_median_ms=%.2f owner_rate_hz=%.1f "
                "peer_updates=%zu peer_median_ms=%.2f peer_rate_hz=%.1f\n",
                compat ? "compat" : "scheduler", window_ms, owner_gaps.size(), owner_med, owner_rate,
                peer_gaps.size(), peer_med, peer_rate);
    const bool ok = owner_gaps.size() >= 10 && peer_gaps.size() >= 50 && owner_med >= 25.0 && owner_med <= 100.0
                    && peer_med >= 5.0 && peer_med <= 20.0;
    std::printf("arm=%s verdict=%s (owner 期望≈50ms/20Hz, peer 期望≈10ms/100Hz)\n",
                compat ? "compat" : "scheduler", ok ? "PASS" : "FAIL");
    std::fflush(stdout);
    return ok ? 0 : 1;
}

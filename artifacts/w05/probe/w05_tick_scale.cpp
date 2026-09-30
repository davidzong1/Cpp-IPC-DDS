/* W05 验收探针（非交付物；证据用）—— 千路状态下控制面**按期服务能力**与 tick 延迟/扫描耗时。
 *
 * 需求：任务书第 (4) 条「单个慢 tick 不得无界阻塞所有 route，记录 tick 延迟与扫描耗时，
 * 评估千路状态下按期服务能力」；W04 §4.5 第 6 条「记录千路下 tick_duration_max_ns /
 * tick_overrun_count」。
 *
 * 手法（全部读数来自公开面，不装钩子、不改被测行为）：
 *   · `ShmControlScheduler::stats()` —— tick 轮数 / 最大轮耗时 / 超期计数 / 在册项数；
 *   · 控制面共享段 `heartbeat_ns` —— 一个订阅者的**实际**心跳间隔（期望 10ms）与
 *     一个发布者的 owner 心跳间隔（期望 50ms）⇒ 直接证明"千路在册时心跳仍按期刷新"；
 *   · 稳态窗口内的 tick 轮数 ⇒ 每轮扫描的项数与平均扫描耗时。
 *
 * 编译：
 *   g++ -std=c++17 -O2 -DNDEBUG -I include -I src artifacts/w05/scratch-probe/w05_tick_scale.cpp \
 *       -o artifacts/w05/scratch-probe/w05_tick_scale -L build/lib -lipc -lpthread -lrt \
 *       -Wl,-rpath,$PWD/build/lib
 */
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "dzIPC/common/control_plane.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/threepools/shm_control_scheduler.h"
#include "ipc_msg/std_msgs/std_image.hpp"

using Clock = std::chrono::steady_clock;
constexpr std::uint32_t kMsgId = 101;

static long arg_long(int argc, char** argv, const char* key, long def)
{
    for (int i = 1; i + 1 < argc; ++i)
    {
        if (!std::strcmp(argv[i], key))
        {
            return std::strtol(argv[i + 1], nullptr, 10);
        }
    }
    return def;
}

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
static double pct_ms(std::vector<long long>& v, double p)
{
    if (v.empty())
    {
        return -1.0;
    }
    std::sort(v.begin(), v.end());
    const std::size_t i = std::min(v.size() - 1, static_cast<std::size_t>(v.size() * p));
    return static_cast<double>(v[i]) / 1e6;
}

int main(int argc, char** argv)
{
    const long n = arg_long(argc, argv, "--n", 1000);
    const long window_ms = arg_long(argc, argv, "--window-ms", 2000);
    const int domain = static_cast<int>(arg_long(argc, argv, "--domain", 1100));

    std::vector<std::string> names;
    names.reserve(static_cast<std::size_t>(n));
    using PubPtr = std::unique_ptr<dzIPC::shm::shm_pub_ipc>;
    using SubPtr = std::unique_ptr<dzIPC::shm::shm_sub_ipc>;
    std::vector<PubPtr> pubs;
    std::vector<SubPtr> subs;
    for (long i = 0; i < n; ++i)
    {
        names.push_back("w05tick_" + std::to_string(domain) + "_" + std::to_string(i));
        pubs.emplace_back(new dzIPC::shm::shm_pub_ipc(td(), names.back(), static_cast<std::size_t>(domain), false));
        pubs.back()->InitChannel("w05");
        subs.emplace_back(new dzIPC::shm::shm_sub_ipc(td(), names.back(), static_cast<std::size_t>(domain), 8, false));
        subs.back()->InitChannel("w05");
    }
    long attached = 0;
    const auto dl = Clock::now() + std::chrono::seconds(60);
    while (Clock::now() < dl && attached < n)
    {
        attached = 0;
        for (long i = 0; i < n; ++i)
        {
            dzIPC::control_plane_shm::TopicControlPlane cp;
            if (cp.open(shm_topic_control_name(names[static_cast<std::size_t>(i)], static_cast<std::size_t>(domain)))
                && cp.peer_count() >= 1)
            {
                ++attached;
            }
        }
        if (attached < n)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
    }
    std::printf("probe=w05_tick_scale n=%ld domain=%d attached=%ld/%ld\n", n, domain, attached, n);

    /* 采样 topic 0 的 PeerSlot 心跳 + 段头 owner 心跳（只读共享段）。 */
    const std::string ctl_name = shm_topic_control_name(names[0], static_cast<std::size_t>(domain));
    ipc::shm::id_t id = ipc::shm::acquire(ctl_name.c_str(), 0, ipc::shm::open);
    std::size_t mapped = 0;
    void* mem = (id != nullptr) ? ipc::shm::get_mem(id, &mapped) : nullptr;
    if (mem == nullptr || mapped < sizeof(dzIPC::control_plane_shm::TopicControl))
    {
        std::printf("FATAL cannot map control plane\n");
        return 2;
    }
    const auto* ctl = static_cast<const dzIPC::control_plane_shm::TopicControl*>(mem);
    std::uint32_t slot = dzIPC::control_plane_shm::kMaxPeerSlots;
    for (std::uint32_t i = 0; i < dzIPC::control_plane_shm::kMaxPeerSlots; ++i)
    {
        if (ctl->peers[i].in_use.load(std::memory_order_acquire) != 0)
        {
            slot = i;
            break;
        }
    }

    /* 稳态窗口。先取一次基线读数（把建连期的抖动排除在窗口之外）。 */
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    auto s0 = dzIPC::shm_control::ShmControlScheduler::instance().stats();
    int64_t last_peer = (slot < dzIPC::control_plane_shm::kMaxPeerSlots)
                            ? ctl->peers[slot].heartbeat_ns.load(std::memory_order_acquire)
                            : 0;
    int64_t last_owner = ctl->heartbeat_ns.load(std::memory_order_acquire);
    std::vector<long long> peer_gaps, owner_gaps;
    const auto t0 = Clock::now();
    while (std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - t0).count() < window_ms)
    {
        if (slot < dzIPC::control_plane_shm::kMaxPeerSlots)
        {
            const int64_t p = ctl->peers[slot].heartbeat_ns.load(std::memory_order_acquire);
            if (p != last_peer && last_peer != 0)
            {
                peer_gaps.push_back(p - last_peer);
            }
            last_peer = p;
        }
        const int64_t o = ctl->heartbeat_ns.load(std::memory_order_acquire);
        if (o != last_owner && last_owner != 0)
        {
            owner_gaps.push_back(o - last_owner);
        }
        last_owner = o;
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    const auto t1 = Clock::now();
    auto s1 = dzIPC::shm_control::ShmControlScheduler::instance().stats();
    const double wall_s = std::chrono::duration<double>(t1 - t0).count();

    const double peer_med = median_ms(peer_gaps);
    const double peer_p99 = pct_ms(peer_gaps, 0.99);
    const double owner_med = median_ms(owner_gaps);
    const double owner_p99 = pct_ms(owner_gaps, 0.99);
    const double ticks = static_cast<double>(s1.tick_count - s0.tick_count);
    const std::size_t entries = s1.entry_count;
    const double scans = ticks * static_cast<double>(entries);
    std::printf("entries=%zu wall_s=%.3f ticks=%llu ticks_per_s=%.1f scans=%.0f scans_per_s=%.0f\n", entries,
                wall_s, static_cast<unsigned long long>(s1.tick_count - s0.tick_count), ticks / wall_s, scans,
                scans / wall_s);
    std::printf("tick_last_ns=%lld tick_max_ns=%lld overruns_total=%llu deferred_total=%llu "
                "exceptions_total=%llu\n",
                static_cast<long long>(s1.tick_duration_last_ns), static_cast<long long>(s1.tick_duration_max_ns),
                static_cast<unsigned long long>(s1.tick_overrun_count),
                static_cast<unsigned long long>(s1.tick_deferred_count),
                static_cast<unsigned long long>(s1.callback_exception_count));
    std::printf("peer_heartbeat: n=%zu median_ms=%.2f p99_ms=%.2f\n", peer_gaps.size(), peer_med, peer_p99);
    std::printf("owner_heartbeat: n=%zu median_ms=%.2f p99_ms=%.2f\n", owner_gaps.size(), owner_med, owner_p99);
    /* 按期服务判据：订阅心跳中位数落在 [5, 20]ms、p99 ≤ 40ms（最后一个采样窗口的截断）；
     * 发布 owner 心跳中位数落在 [25, 100]ms；tick 无异常隔离（exceptions==0）。 */
    const bool ok = attached == n && peer_gaps.size() >= 20 && peer_med >= 5.0 && peer_med <= 20.0
                    && peer_p99 <= 40.0 && owner_med >= 25.0 && owner_med <= 100.0
                    && s1.callback_exception_count == 0;
    std::printf("verdict=%s\n", ok ? "PASS" : "FAIL");

    ipc::shm::release_no_unlink(id);
    pubs.clear();
    subs.clear();
    for (const auto& nm : names)
    {
        ipc::route::clear_storage(shm_topic_segment_name(nm, static_cast<std::size_t>(domain)).c_str());
        ipc::shm::handle::clear_storage(shm_topic_control_name(nm, static_cast<std::size_t>(domain)).c_str());
    }
    std::fflush(stdout);
    return ok ? 0 : 1;
}

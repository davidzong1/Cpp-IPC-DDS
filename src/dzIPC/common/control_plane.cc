#include "dzIPC/common/control_plane.h"
#include <chrono>
#include <thread>
#include "libipc/shm.h"

#if defined(_WIN32)
#include <windows.h>
#else
#include <unistd.h>
#endif

namespace dzIPC {
namespace control_plane_shm {
namespace {

constexpr uint32_t kMagic = 0x445A4351U;   // v2: TopicControl 增加 PeerSlot 表, 与 v1 布局不兼容
constexpr auto kRebuildPeerDrainTimeout = std::chrono::milliseconds(200);
constexpr auto kRebuildPeerDrainPoll = std::chrono::milliseconds(2);

int32_t current_pid()
{
#if defined(_WIN32)
    return static_cast<int32_t>(::GetCurrentProcessId());
#else
    return static_cast<int32_t>(::getpid());
#endif
}

int64_t now_ns()
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

}   // namespace

struct TopicControlPlane::Impl
{
    ipc::shm::handle handle;
};

TopicControlPlane::TopicControlPlane() = default;

TopicControlPlane::~TopicControlPlane() = default;

bool TopicControlPlane::open(const std::string& name)
{
    control_ = nullptr;
    if (!impl_)
    {
        impl_ = std::make_unique<Impl>();
    }
    if (!impl_->handle.acquire(name.c_str(), sizeof(TopicControl), ipc::shm::create | ipc::shm::open))
    {
        return false;
    }
    control_ = static_cast<TopicControl*>(impl_->handle.get());
    initialize_if_needed();
    return control_ != nullptr;
}

uint32_t TopicControlPlane::begin_rebuild()
{
    if (!control_)
    {
        return 0;
    }
    control_->state.store(static_cast<uint32_t>(TopicState::Clearing), std::memory_order_release);
    control_->owner_pid.store(current_pid(), std::memory_order_release);
    control_->heartbeat_ns.store(now_ns(), std::memory_order_release);
    const auto deadline = std::chrono::steady_clock::now() + kRebuildPeerDrainTimeout;
    while (control_->peer_count.load(std::memory_order_acquire) > 0
           && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(kRebuildPeerDrainPoll);
    }
    control_->peer_count.store(0, std::memory_order_release);
    /* 新一代 generation 会让所有旧订阅者重新 attach, 它们的 cc_id 随之作废,
     * 槽位一并清空, 否则旧 cc_id 会在下一轮被当成"死连接"去断新的连接。 */
    clear_all_peer_slots();
    control_->heartbeat_ns.store(now_ns(), std::memory_order_release);
    return control_->generation.fetch_add(1, std::memory_order_acq_rel) + 1;
}

void TopicControlPlane::set_ready()
{
    if (!control_)
    {
        return;
    }
    control_->heartbeat_ns.store(now_ns(), std::memory_order_release);
    control_->state.store(static_cast<uint32_t>(TopicState::Ready), std::memory_order_release);
}

void TopicControlPlane::set_stopping()
{
    if (!control_)
    {
        return;
    }
    control_->heartbeat_ns.store(now_ns(), std::memory_order_release);
    control_->state.store(static_cast<uint32_t>(TopicState::Stopping), std::memory_order_release);
}

void TopicControlPlane::heartbeat()
{
    if (control_)
    {
        control_->heartbeat_ns.store(now_ns(), std::memory_order_release);
    }
}

uint32_t TopicControlPlane::generation() const
{
    return control_ ? control_->generation.load(std::memory_order_acquire) : 0;
}

TopicState TopicControlPlane::state() const
{
    return control_ ? static_cast<TopicState>(control_->state.load(std::memory_order_acquire)) : TopicState::Empty;
}

uint32_t TopicControlPlane::peer_count() const
{
    return control_ ? control_->peer_count.load(std::memory_order_acquire) : 0;
}

bool TopicControlPlane::add_peer(uint32_t generation)
{
    if (!control_ || generation == 0 || generation != this->generation() || state() != TopicState::Ready)
    {
        return false;
    }
    control_->peer_count.fetch_add(1, std::memory_order_acq_rel);
    return true;
}

void TopicControlPlane::remove_peer(uint32_t generation)
{
    if (!control_ || generation == 0 || generation != this->generation())
    {
        return;
    }
    uint32_t current = control_->peer_count.load(std::memory_order_acquire);
    while (current > 0
           && !control_->peer_count.compare_exchange_weak(current, current - 1, std::memory_order_acq_rel,
                                                          std::memory_order_acquire))
    {}
}

void TopicControlPlane::clear_all_peer_slots()
{
    if (!control_)
    {
        return;
    }
    for (uint32_t i = 0; i < kMaxPeerSlots; ++i)
    {
        PeerSlot& p = control_->peers[i];
        p.cc_id.store(0, std::memory_order_relaxed);
        p.heartbeat_ns.store(0, std::memory_order_relaxed);
        p.pid.store(0, std::memory_order_relaxed);
        p.generation.store(0, std::memory_order_relaxed);
        p.in_use.store(0, std::memory_order_release);
    }
}

int TopicControlPlane::acquire_peer_slot(uint32_t generation, uint32_t cc_id)
{
    if (!control_ || cc_id == 0)
    {
        return -1;
    }
    for (uint32_t i = 0; i < kMaxPeerSlots; ++i)
    {
        PeerSlot& p = control_->peers[i];
        uint32_t expected = 0;
        if (!p.in_use.compare_exchange_strong(expected, 1, std::memory_order_acq_rel,
                                              std::memory_order_acquire))
        {
            continue;
        }
        /* 槽位已归本进程所有。cc_id 与 heartbeat_ns 在 release_peer_slot() /
         * clear_all_peer_slots() 里都被清成 0, 而 collect_stale_peers() 会跳过
         * 这两者为 0 的槽位, 所以从 CAS 成功到下面填完之间的窗口不会被误判死。*/
        p.cc_id.store(cc_id, std::memory_order_relaxed);
        p.pid.store(current_pid(), std::memory_order_relaxed);
        p.generation.store(generation, std::memory_order_relaxed);
        p.heartbeat_ns.store(now_ns(), std::memory_order_release);
        return static_cast<int>(i);
    }
    return -1;
}

void TopicControlPlane::peer_heartbeat(int slot)
{
    if (!control_ || slot < 0 || static_cast<uint32_t>(slot) >= kMaxPeerSlots)
    {
        return;
    }
    control_->peers[slot].heartbeat_ns.store(now_ns(), std::memory_order_release);
}

void TopicControlPlane::release_peer_slot(int slot)
{
    if (!control_ || slot < 0 || static_cast<uint32_t>(slot) >= kMaxPeerSlots)
    {
        return;
    }
    PeerSlot& p = control_->peers[slot];
    /* 先清内容再放开 in_use: 反过来的话, 槽位可能被别的订阅者抢占并填好,
     * 随后被本次的清零覆盖掉。 */
    p.cc_id.store(0, std::memory_order_relaxed);
    p.heartbeat_ns.store(0, std::memory_order_relaxed);
    p.pid.store(0, std::memory_order_relaxed);
    p.generation.store(0, std::memory_order_relaxed);
    p.in_use.store(0, std::memory_order_release);
}

uint32_t TopicControlPlane::collect_stale_peers(int64_t timeout_ns)
{
    if (!control_ || timeout_ns <= 0)
    {
        return 0;
    }
    const int64_t now = now_ns();
    uint32_t stale = 0;
    for (uint32_t i = 0; i < kMaxPeerSlots; ++i)
    {
        PeerSlot& p = control_->peers[i];
        if (p.in_use.load(std::memory_order_acquire) == 0)
        {
            continue;
        }
        const uint32_t cc_id = p.cc_id.load(std::memory_order_acquire);
        const int64_t hb = p.heartbeat_ns.load(std::memory_order_acquire);
        if (cc_id == 0 || hb == 0)
        {
            continue;   // 刚占用还没填完, 下一轮再看
        }
        if (now - hb < timeout_ns)
        {
            continue;   // 心跳还在, 哪怕它读得慢也不动它
        }
        /* 判定为死连接。先摘槽位再累加 cc_id, 避免下一轮重复回收。 */
        stale |= cc_id;
        p.cc_id.store(0, std::memory_order_relaxed);
        p.heartbeat_ns.store(0, std::memory_order_relaxed);
        p.pid.store(0, std::memory_order_relaxed);
        p.generation.store(0, std::memory_order_relaxed);
        p.in_use.store(0, std::memory_order_release);
    }
    return stale;
}

void TopicControlPlane::initialize_if_needed()
{
    if (!control_)
    {
        return;
    }
    if (control_->magic.load(std::memory_order_acquire) == kMagic)
    {
        return;
    }
    control_->generation.store(0, std::memory_order_relaxed);
    control_->state.store(static_cast<uint32_t>(TopicState::Empty), std::memory_order_relaxed);
    control_->owner_pid.store(0, std::memory_order_relaxed);
    control_->heartbeat_ns.store(now_ns(), std::memory_order_relaxed);
    control_->peer_count.store(0, std::memory_order_relaxed);
    clear_all_peer_slots();
    control_->magic.store(kMagic, std::memory_order_release);
}

}   // namespace control_plane_shm
}   // namespace dzIPC

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

constexpr uint32_t kMagic = 0x445A4350U;
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
    control_->magic.store(kMagic, std::memory_order_release);
}

}   // namespace control_plane_shm
}   // namespace dzIPC

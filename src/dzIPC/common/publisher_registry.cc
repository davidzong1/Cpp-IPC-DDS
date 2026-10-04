#include "dzIPC/common/publisher_registry.h"

#include <chrono>
#include <cerrno>
#include <fstream>
#include <functional>
#include <sstream>
#include <thread>

#if defined(_WIN32)
#    include <process.h>
#else
#    include <signal.h>
#    include <unistd.h>
#endif

#include "libipc/shm.h"

namespace dzIPC {
namespace control_plane_shm {

namespace {

/* Registry operations are short, but the final-publisher cleanup runs
 * outside the registry and is intentionally represented by `closing`.  A
 * crashed process must not strand either state forever. */
constexpr std::int64_t kMutationLockTimeoutNs = 2'000'000'000LL;
constexpr std::int64_t kJoinWaitNs = 3'000'000'000LL;
/* mutation_owner is a small cross-process state machine.  The top two bits
 * are reserved for the two transitional states; normal owner tokens use the
 * lower 62 bits.  A timestamp in the transitional state closes the crash
 * window between claiming/releasing the gate and publishing its companion
 * fields. */
constexpr std::uint64_t kMutationStateMask = 0xC000000000000000ULL;
constexpr std::uint64_t kMutationClaiming = 0x4000000000000000ULL;
constexpr std::uint64_t kMutationReleasing = 0x8000000000000000ULL;
constexpr std::uint64_t kMutationTimestampMask = ~kMutationStateMask;

std::uint64_t process_id() noexcept
{
#if defined(_WIN32)
    return static_cast<std::uint64_t>(::_getpid());
#else
    return static_cast<std::uint64_t>(::getpid());
#endif
}

std::uint64_t process_start_token() noexcept
{
    const auto pid = process_id();
    static std::atomic<std::uint64_t> cached_pid{0};
    static std::atomic<std::uint64_t> cached_token{0};
    if (cached_pid.load(std::memory_order_acquire) == pid)
    {
        const auto token = cached_token.load(std::memory_order_acquire);
        if (token != 0)
        {
            return token;
        }
    }

    std::uint64_t token = 0;
#if defined(__linux__)
    try
    {
        std::ifstream input("/proc/self/stat");
        std::string line;
        if (std::getline(input, line))
        {
            const auto close = line.rfind(')');
            if (close != std::string::npos && close + 2 < line.size())
            {
                std::istringstream fields(line.substr(close + 2));
                std::string state;
                fields >> state;
                for (int field = 4; field <= 22 && (fields >> token); ++field)
                {
                }
            }
        }
    }
    catch (...)
    {
        token = 0;
    }
#endif
    if (token == 0)
    {
        token = static_cast<std::uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        token ^= pid * 0x9e3779b97f4a7c15ULL;
        if (token == 0)
        {
            token = 1;
        }
    }
    cached_token.store(token, std::memory_order_release);
    cached_pid.store(pid, std::memory_order_release);
    return token;
}

bool process_alive(std::int32_t pid, std::uint64_t start_token) noexcept
{
    if (pid <= 0)
    {
        return false;
    }
#if defined(_WIN32)
    (void)start_token;
    return true;
#else
    if (::kill(static_cast<pid_t>(pid), 0) != 0 && errno != EPERM)
    {
        return false;
    }
    if (start_token == 0)
    {
        return true;
    }
    try
    {
        std::ifstream input(std::string("/proc/") + std::to_string(pid) + "/stat");
        std::string line;
        if (!std::getline(input, line))
        {
            return true;
        }
        const auto close = line.rfind(')');
        if (close == std::string::npos || close + 2 >= line.size())
        {
            return true;
        }
        std::istringstream fields(line.substr(close + 2));
        std::string state;
        fields >> state;
        std::uint64_t value = 0;
        for (int field = 4; field <= 22; ++field)
        {
            if (!(fields >> value))
            {
                return true;
            }
        }
        return value == start_token;
    }
    catch (...)
    {
        return true;
    }
#endif
}

std::uint64_t mutation_marker(std::uint64_t state, std::int64_t now) noexcept
{
    return state | (static_cast<std::uint64_t>(now) & kMutationTimestampMask);
}

bool mutation_marker_expired(std::uint64_t value, std::int64_t now) noexcept
{
    const auto stamp = value & kMutationTimestampMask;
    if (stamp == 0)
    {
        return false;
    }
    const auto current = static_cast<std::uint64_t>(now) & kMutationTimestampMask;
    return current >= stamp &&
           current - stamp >= static_cast<std::uint64_t>(kMutationLockTimeoutNs);
}

} // namespace

struct PublisherRegistry::Impl {
    ipc::shm::handle handle;
};

PublisherRegistry::PublisherRegistry() = default;

PublisherRegistry::~PublisherRegistry() = default;

std::int64_t PublisherRegistry::now_ns() noexcept
{
    const auto now = std::chrono::steady_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::nanoseconds>(now).count();
}

std::uint64_t PublisherRegistry::mutation_token() const noexcept
{
    const auto pid = process_id();
    const auto cached_pid = mutation_process_id_.load(std::memory_order_acquire);
    if (cached_pid != pid)
    {
        /* A PublisherRegistry can be copied by fork().  Its local cache must
         * never make the child impersonate the parent's lock owner. */
        mutation_identity_.store(0, std::memory_order_release);
        mutation_process_id_.store(pid, std::memory_order_release);
    }
    auto token = mutation_identity_.load(std::memory_order_acquire);
    if (token != 0)
    {
        return token;
    }
    const auto thread_hash = static_cast<std::uint64_t>(
        std::hash<std::thread::id>{}(std::this_thread::get_id()));
    token = ((pid << 32) ^ thread_hash ^
             static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(this)) ^
             0x9e3779b97f4a7c15ULL) & kMutationTimestampMask;
    if (token == 0)
    {
        token = 1;
    }
    std::uint64_t expected = 0;
    mutation_identity_.compare_exchange_strong(expected, token,
                                               std::memory_order_acq_rel,
                                               std::memory_order_acquire);
    return mutation_identity_.load(std::memory_order_acquire);
}

bool PublisherRegistry::lock_mutation() noexcept
{
    if (control_ == nullptr)
    {
        return false;
    }
    const auto token = mutation_token();
    const auto owner_pid = static_cast<std::int32_t>(process_id());
    const auto owner_start_token = process_start_token();
    const auto deadline = now_ns() + kMutationLockTimeoutNs;
    for (;;)
    {
        const auto now = now_ns();
        std::uint64_t expected = 0;
        const auto claiming = mutation_marker(kMutationClaiming, now);
        if (control_->mutation_owner.compare_exchange_weak(
                expected, claiming, std::memory_order_acq_rel,
                std::memory_order_acquire))
        {
            control_->mutation_since_ns.store(now, std::memory_order_release);
            control_->mutation_pid.store(owner_pid, std::memory_order_release);
            control_->mutation_start_token.store(owner_start_token,
                                                 std::memory_order_release);
            control_->mutation_owner.store(token, std::memory_order_release);
            return true;
        }

        const auto owner = control_->mutation_owner.load(std::memory_order_acquire);
        const auto state = owner & kMutationStateMask;
        const auto held_pid = control_->mutation_pid.load(std::memory_order_acquire);
        const auto held_start_token =
            control_->mutation_start_token.load(std::memory_order_acquire);
        if ((state == kMutationClaiming || state == kMutationReleasing) &&
            mutation_marker_expired(owner, now) &&
            !process_alive(held_pid, held_start_token))
        {
            const auto claiming = mutation_marker(kMutationClaiming, now);
            auto stale_marker = owner;
            if (control_->mutation_owner.compare_exchange_weak(
                    stale_marker, claiming, std::memory_order_acq_rel,
                    std::memory_order_acquire))
            {
                control_->mutation_since_ns.store(now, std::memory_order_release);
                control_->mutation_pid.store(owner_pid, std::memory_order_release);
                control_->mutation_start_token.store(owner_start_token,
                                                     std::memory_order_release);
                control_->mutation_owner.store(token, std::memory_order_release);
                return true;
            }
        }
        else if (state == 0)
        {
            const auto since = control_->mutation_since_ns.load(std::memory_order_acquire);
            if (since != 0 && now - since >= kMutationLockTimeoutNs &&
                !process_alive(held_pid, held_start_token))
            {
                const auto claiming = mutation_marker(kMutationClaiming, now);
                auto stale_owner = owner;
                if (stale_owner != 0 &&
                    control_->mutation_owner.compare_exchange_weak(
                        stale_owner, claiming, std::memory_order_acq_rel,
                        std::memory_order_acquire))
                {
                    control_->mutation_since_ns.store(now, std::memory_order_release);
                    control_->mutation_pid.store(owner_pid, std::memory_order_release);
                    control_->mutation_start_token.store(owner_start_token,
                                                         std::memory_order_release);
                    control_->mutation_owner.store(token, std::memory_order_release);
                    return true;
                }
            }
        }
        if (now >= deadline)
        {
            return false;
        }
        std::this_thread::yield();
    }
}

void PublisherRegistry::unlock_mutation() noexcept
{
    if (control_ == nullptr)
    {
        return;
    }
    const auto token = mutation_token();
    const auto releasing = mutation_marker(kMutationReleasing, now_ns());
    auto expected = token;
    if (control_->mutation_owner.compare_exchange_strong(
            expected, releasing, std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        control_->mutation_since_ns.store(0, std::memory_order_release);
        control_->mutation_pid.store(0, std::memory_order_release);
        control_->mutation_start_token.store(0, std::memory_order_release);
        control_->mutation_owner.store(0, std::memory_order_release);
    }
}

bool PublisherRegistry::recover_stale_close(std::int64_t now) noexcept
{
    if (control_ == nullptr || control_->closing.load(std::memory_order_acquire) == 0)
    {
        return false;
    }
    const auto since = control_->closing_since_ns.load(std::memory_order_acquire);
    const auto closer_pid = control_->closing_pid.load(std::memory_order_acquire);
    const auto closer_start_token =
        control_->closing_start_token.load(std::memory_order_acquire);
    if (since == 0 || now - since < kMutationLockTimeoutNs ||
        process_alive(closer_pid, closer_start_token))
    {
        return false;
    }
    std::uint32_t expected = 1;
    if (!control_->closing.compare_exchange_strong(
            expected, 0, std::memory_order_acq_rel,
            std::memory_order_acquire))
    {
        return false;
    }
    control_->closing_since_ns.store(0, std::memory_order_release);
    control_->closing_pid.store(0, std::memory_order_release);
    control_->closing_start_token.store(0, std::memory_order_release);
    return true;
}

void PublisherRegistry::clear_slot(PublisherSlot& slot) noexcept
{
    slot.publisher_id.store(0, std::memory_order_relaxed);
    slot.pid.store(0, std::memory_order_relaxed);
    slot.start_token.store(0, std::memory_order_relaxed);
    slot.generation.store(0, std::memory_order_relaxed);
    slot.heartbeat_ns.store(0, std::memory_order_relaxed);
    slot.in_use.store(0, std::memory_order_release);
}

bool PublisherRegistry::initialize_if_needed()
{
    if (control_ == nullptr)
    {
        return false;
    }
    const auto magic = control_->magic.load(std::memory_order_acquire);
    const auto layout = control_->layout.load(std::memory_order_acquire);
    if (magic == kPublisherRegistryMagic && layout == kPublisherRegistryLayout)
    {
        return true;
    }
    /* A zeroed new SHM object is initialized by the first opener.  Any other
     * magic/layout is an incompatible live segment and must not be reset. */
    if (magic == kPublisherRegistryInitializing)
    {
        for (unsigned i = 0; i < 1000; ++i)
        {
            std::this_thread::yield();
            if (control_->magic.load(std::memory_order_acquire) !=
                kPublisherRegistryInitializing)
            {
                return control_->magic.load(std::memory_order_acquire) ==
                       kPublisherRegistryMagic &&
                       control_->layout.load(std::memory_order_acquire) ==
                       kPublisherRegistryLayout;
            }
        }
        return false;
    }
    if (magic != 0 || layout != 0)
    {
        return false;
    }
    std::uint32_t expected_magic = 0;
    if (!control_->magic.compare_exchange_strong(
            expected_magic, kPublisherRegistryInitializing,
            std::memory_order_acq_rel, std::memory_order_acquire))
    {
        return initialize_if_needed();
    }
    control_->generation.store(0, std::memory_order_relaxed);
    control_->publisher_count.store(0, std::memory_order_relaxed);
    control_->coordinator_slot.store(-1, std::memory_order_relaxed);
    control_->coordinator_lease_ns.store(0, std::memory_order_relaxed);
    control_->mutation_owner.store(0, std::memory_order_relaxed);
    control_->mutation_since_ns.store(0, std::memory_order_relaxed);
    control_->mutation_pid.store(0, std::memory_order_relaxed);
    control_->mutation_start_token.store(0, std::memory_order_relaxed);
    control_->closing.store(0, std::memory_order_relaxed);
    control_->closing_since_ns.store(0, std::memory_order_relaxed);
    control_->closing_pid.store(0, std::memory_order_relaxed);
    control_->closing_start_token.store(0, std::memory_order_relaxed);
    for (auto& slot : control_->publishers)
    {
        clear_slot(slot);
    }
    control_->layout.store(kPublisherRegistryLayout, std::memory_order_relaxed);
    control_->magic.store(kPublisherRegistryMagic, std::memory_order_release);
    return true;
}

bool PublisherRegistry::open(const std::string& name)
{
    control_ = nullptr;
    if (name.empty())
    {
        return false;
    }
    if (!impl_)
    {
        impl_ = std::make_unique<Impl>();
    }
    /* Probe existing segments with open-only first.  The POSIX create|open
     * path carries the requested size into get_mem() and may ftruncate an
     * existing object, which would erase the evidence needed to reject an
     * incompatible layout. */
    if (!impl_->handle.acquire(name.c_str(), sizeof(PublisherControl),
                               ipc::shm::open) &&
        !impl_->handle.acquire(name.c_str(), sizeof(PublisherControl),
                               ipc::shm::create | ipc::shm::open))
    {
        return false;
    }
    if (impl_->handle.size() < sizeof(PublisherControl))
    {
        impl_->handle.release_no_unlink();
        return false;
    }
    control_ = static_cast<PublisherControl*>(impl_->handle.get());
    if (initialize_if_needed())
    {
        return true;
    }
    impl_->handle.release_no_unlink();
    control_ = nullptr;
    return false;
}

std::uint32_t PublisherRegistry::generation() const noexcept
{
    return control_ ? control_->generation.load(std::memory_order_acquire) : 0;
}

std::uint32_t PublisherRegistry::publisher_count() const noexcept
{
    return control_ ? control_->publisher_count.load(std::memory_order_acquire) : 0;
}

std::int32_t PublisherRegistry::coordinator_slot() const noexcept
{
    return control_ ? control_->coordinator_slot.load(std::memory_order_acquire) : -1;
}

std::uint64_t PublisherRegistry::coordinator_publisher_id() const noexcept
{
    if (control_ == nullptr)
        return 0;
    const auto slot = control_->coordinator_slot.load(std::memory_order_acquire);
    if (slot < 0 || static_cast<std::uint32_t>(slot) >= kMaxPublisherSlots)
        return 0;
    const auto& entry = control_->publishers[slot];
    if (entry.in_use.load(std::memory_order_acquire) != 1)
        return 0;
    return entry.publisher_id.load(std::memory_order_acquire);
}

bool PublisherRegistry::compare_exchange_generation(std::uint32_t expected,
                                                    std::uint32_t desired) noexcept
{
    if (control_ == nullptr || desired == 0 || !lock_mutation())
    {
        return false;
    }
    const bool ok = control_->generation.compare_exchange_strong(
        expected, desired, std::memory_order_acq_rel, std::memory_order_acquire);
    unlock_mutation();
    return ok;
}

bool PublisherRegistry::advance_generation(std::uint32_t expected,
                                           std::uint32_t desired) noexcept
{
    if (control_ == nullptr || desired == 0 || !lock_mutation())
    {
        return false;
    }
    if (!control_->generation.compare_exchange_strong(
            expected, desired, std::memory_order_acq_rel, std::memory_order_acquire))
    {
        unlock_mutation();
        return false;
    }
    /* Rebuild is serialized by the coordinator lease.  Publish the new
     * generation to all slots so an already joined publisher remains a valid
     * member while its data handle is being recreated. */
    for (auto& slot : control_->publishers)
    {
        if (slot.in_use.load(std::memory_order_acquire) == 1)
        {
            slot.generation.store(desired, std::memory_order_release);
        }
    }
    unlock_mutation();
    return true;
}

int PublisherRegistry::join(std::uint32_t generation_value,
                            std::uint64_t publisher_id,
                            std::int32_t pid,
                            std::uint64_t start_token)
{
    if (control_ == nullptr || generation_value == 0 || publisher_id == 0 ||
        start_token == 0)
    {
        return -1;
    }
    const auto close_deadline = now_ns() + kJoinWaitNs;
    for (;;)
    {
        if (!lock_mutation())
        {
            return -1;
        }
        const auto now = now_ns();
        if (control_->closing.load(std::memory_order_acquire) != 0 &&
            !recover_stale_close(now))
        {
            unlock_mutation();
            if (now >= close_deadline)
            {
                return -1;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
            continue;
        }
        auto current = control_->generation.load(std::memory_order_acquire);
        if (current == 0)
        {
            if (!control_->generation.compare_exchange_strong(
                    current, generation_value, std::memory_order_acq_rel,
                    std::memory_order_acquire) && current != generation_value)
            {
                unlock_mutation();
                return -1;
            }
        }
        else if (current != generation_value)
        {
            unlock_mutation();
            return -1;
        }
        break;
    }

    for (std::uint32_t i = 0; i < kMaxPublisherSlots; ++i)
    {
        auto& slot = control_->publishers[i];
        if (slot.in_use.load(std::memory_order_acquire) == 1 &&
            slot.publisher_id.load(std::memory_order_acquire) == publisher_id)
        {
            if (slot.start_token.load(std::memory_order_acquire) != start_token ||
                slot.pid.load(std::memory_order_acquire) != pid)
            {
                unlock_mutation();
                return -1;
            }
            slot.heartbeat_ns.store(now_ns(), std::memory_order_release);
            const int result = static_cast<int>(i);
            unlock_mutation();
            return result;
        }
    }

    for (std::uint32_t i = 0; i < kMaxPublisherSlots; ++i)
    {
        auto& slot = control_->publishers[i];
        std::uint32_t expected = 0;
        if (!slot.in_use.compare_exchange_strong(expected, 2,
                                                 std::memory_order_acq_rel,
                                                 std::memory_order_acquire))
        {
            continue;
        }
        slot.publisher_id.store(publisher_id, std::memory_order_relaxed);
        slot.pid.store(pid, std::memory_order_relaxed);
        slot.start_token.store(start_token, std::memory_order_relaxed);
        slot.generation.store(generation_value, std::memory_order_relaxed);
        slot.heartbeat_ns.store(now_ns(), std::memory_order_release);
        control_->publisher_count.fetch_add(1, std::memory_order_acq_rel);
        slot.in_use.store(1, std::memory_order_release);
        const int result = static_cast<int>(i);
        unlock_mutation();
        return result;
    }
    unlock_mutation();
    return -1;
}

bool PublisherRegistry::identity_matches(int slot,
                                         std::uint64_t publisher_id,
                                         std::uint64_t start_token) const noexcept
{
    if (control_ == nullptr || slot < 0 ||
        static_cast<std::uint32_t>(slot) >= kMaxPublisherSlots)
    {
        return false;
    }
    const auto& entry = control_->publishers[slot];
    return entry.in_use.load(std::memory_order_acquire) == 1 &&
           entry.publisher_id.load(std::memory_order_acquire) == publisher_id &&
           entry.start_token.load(std::memory_order_acquire) == start_token;
}

bool PublisherRegistry::heartbeat(int slot, std::uint64_t publisher_id,
                                  std::uint64_t start_token)
{
    if (!lock_mutation())
    {
        return false;
    }
    if (!identity_matches(slot, publisher_id, start_token))
    {
        unlock_mutation();
        return false;
    }
    control_->publishers[slot].heartbeat_ns.store(now_ns(),
                                                   std::memory_order_release);
    unlock_mutation();
    return true;
}

bool PublisherRegistry::leave(int slot, std::uint64_t publisher_id,
                              std::uint64_t start_token, bool* became_empty)
{
    if (became_empty != nullptr)
    {
        *became_empty = false;
    }
    if (!lock_mutation())
    {
        return false;
    }
    if (!identity_matches(slot, publisher_id, start_token))
    {
        unlock_mutation();
        return false;
    }
    auto& entry = control_->publishers[slot];
    std::uint32_t expected_in_use = 1;
    if (!entry.in_use.compare_exchange_strong(expected_in_use, 2,
                                              std::memory_order_acq_rel,
                                              std::memory_order_acquire))
    {
        unlock_mutation();
        return false;
    }
    const auto coordinator = control_->coordinator_slot.load(std::memory_order_acquire);
    if (coordinator == slot)
    {
        auto expected = slot;
        control_->coordinator_slot.compare_exchange_strong(
            expected, -1, std::memory_order_acq_rel,
            std::memory_order_acquire);
        control_->coordinator_lease_ns.store(0, std::memory_order_release);
    }
    clear_slot(entry);
    const auto count = control_->publisher_count.fetch_sub(1, std::memory_order_acq_rel);
    if (became_empty != nullptr)
    {
        *became_empty = (count == 1);
    }
    if (count == 1)
    {
        control_->closing_since_ns.store(now_ns(), std::memory_order_release);
        control_->closing_pid.store(static_cast<std::int32_t>(process_id()),
                                    std::memory_order_release);
        control_->closing_start_token.store(process_start_token(),
                                             std::memory_order_release);
        control_->closing.store(1, std::memory_order_release);
        closing_owner_ = true;
        closing_owner_process_id_ = process_id();
    }
    unlock_mutation();
    return true;
}

bool PublisherRegistry::finish_close() noexcept
{
    if (control_ == nullptr || !closing_owner_ ||
        closing_owner_process_id_ != process_id() || !lock_mutation())
    {
        return false;
    }
    std::uint32_t expected = 1;
    const bool ok = control_->closing.compare_exchange_strong(
        expected, 0, std::memory_order_acq_rel, std::memory_order_acquire);
    if (ok)
    {
        control_->closing_since_ns.store(0, std::memory_order_release);
        control_->closing_pid.store(0, std::memory_order_release);
        control_->closing_start_token.store(0, std::memory_order_release);
    }
    closing_owner_ = false;
    closing_owner_process_id_ = 0;
    unlock_mutation();
    return ok;
}

std::uint32_t PublisherRegistry::reap_stale(std::int64_t timeout_ns)
{
    if (control_ == nullptr || timeout_ns <= 0 || !lock_mutation())
    {
        return 0;
    }
    const auto now = now_ns();
    if (control_->closing.load(std::memory_order_acquire) != 0 &&
        !recover_stale_close(now))
    {
        unlock_mutation();
        return 0;
    }
    std::uint32_t removed = 0;
    for (std::uint32_t i = 0; i < kMaxPublisherSlots; ++i)
    {
        auto& entry = control_->publishers[i];
        if (entry.in_use.load(std::memory_order_acquire) != 1)
        {
            continue;
        }
        const auto heartbeat = entry.heartbeat_ns.load(std::memory_order_acquire);
        if (heartbeat == 0 || now - heartbeat < timeout_ns)
        {
            continue;
        }
        std::uint32_t expected = 1;
        if (!entry.in_use.compare_exchange_strong(expected, 2,
                                                  std::memory_order_acq_rel,
                                                  std::memory_order_acquire))
        {
            continue;
        }
        /* A heartbeat may have passed its identity check immediately before
         * the CAS.  Re-read after taking the reclaim state so that such an
         * in-flight refresh keeps the publisher alive. */
        const auto refreshed = entry.heartbeat_ns.load(std::memory_order_acquire);
        if (refreshed != 0 && now - refreshed < timeout_ns)
        {
            entry.in_use.store(1, std::memory_order_release);
            continue;
        }
        const auto coordinator = control_->coordinator_slot.load(std::memory_order_acquire);
        if (coordinator == static_cast<std::int32_t>(i))
        {
            auto coordinator_expected = static_cast<std::int32_t>(i);
            control_->coordinator_slot.compare_exchange_strong(
                coordinator_expected, -1, std::memory_order_acq_rel,
                std::memory_order_acquire);
            control_->coordinator_lease_ns.store(0, std::memory_order_release);
        }
        entry.publisher_id.store(0, std::memory_order_relaxed);
        entry.pid.store(0, std::memory_order_relaxed);
        entry.start_token.store(0, std::memory_order_relaxed);
        entry.generation.store(0, std::memory_order_relaxed);
        entry.heartbeat_ns.store(0, std::memory_order_relaxed);
        entry.in_use.store(0, std::memory_order_release);
        auto count = control_->publisher_count.load(std::memory_order_acquire);
        while (count != 0 && !control_->publisher_count.compare_exchange_weak(
                   count, count - 1, std::memory_order_acq_rel,
                   std::memory_order_acquire))
        {
        }
        ++removed;
    }
    unlock_mutation();
    return removed;
}

bool PublisherRegistry::acquire_coordinator(int slot,
                                            std::uint64_t publisher_id,
                                            std::uint64_t start_token,
                                            std::int64_t lease_ns)
{
    if (lease_ns <= 0 || !lock_mutation())
    {
        return false;
    }
    if (!identity_matches(slot, publisher_id, start_token))
    {
        unlock_mutation();
        return false;
    }
    const auto now = now_ns();
    auto owner = control_->coordinator_slot.load(std::memory_order_acquire);
    if (owner == slot)
    {
        control_->coordinator_lease_ns.store(now + lease_ns,
                                             std::memory_order_release);
        unlock_mutation();
        return true;
    }
    if (owner >= 0 && control_->coordinator_lease_ns.load(std::memory_order_acquire) > now)
    {
        unlock_mutation();
        return false;
    }
    if (!control_->coordinator_slot.compare_exchange_strong(
            owner, slot, std::memory_order_acq_rel, std::memory_order_acquire))
    {
        unlock_mutation();
        return false;
    }
    control_->coordinator_lease_ns.store(now + lease_ns,
                                         std::memory_order_release);
    unlock_mutation();
    return true;
}

bool PublisherRegistry::is_coordinator(int slot, std::uint64_t publisher_id,
                                       std::uint64_t start_token) const
{
    return identity_matches(slot, publisher_id, start_token) &&
           control_->coordinator_slot.load(std::memory_order_acquire) == slot &&
           control_->coordinator_lease_ns.load(std::memory_order_acquire) > now_ns();
}

} // namespace control_plane_shm
} // namespace dzIPC

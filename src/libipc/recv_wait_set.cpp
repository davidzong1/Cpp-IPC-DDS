#include "libipc/recv_wait_set.h"

#include <algorithm>
#include <atomic>
#include <array>
#include <cerrno>
#include <climits>
#include <mutex>
#include <vector>

#include "libipc/utility/log.h"

#if defined(__linux__)
#include <linux/futex.h>
#include <sys/syscall.h>
#include <time.h>
#include <unistd.h>
#elif defined(_WIN32)
#include <Windows.h>
#endif

namespace ipc {
namespace {
std::atomic<detail::recv_wait_trace_hook> g_recv_wait_trace_hook{nullptr};

template<class Factory>
void emit_recv_wait_trace(detail::recv_wait_trace_hook hook, Factory&& make_event) noexcept
{
    if (hook != nullptr)
    {
        const auto event = make_event();
        hook(event);
    }
}

constexpr std::size_t kMaxRoutes =
#if defined(_WIN32)
    63;
#elif defined(__linux__)
    127; // futex_waitv has 128 slots; reserve one for interruption.
#else
    0;
#endif

#if defined(__linux__)
#if defined(SYS_futex_waitv)
constexpr long kFutexWaitvSyscall = SYS_futex_waitv;
#elif defined(__NR_futex_waitv)
constexpr long kFutexWaitvSyscall = __NR_futex_waitv;
#elif defined(__x86_64__)
// Linux 5.16 ABI fallback for old libc/kernel headers that omit __NR_futex_waitv.
// Kept in this Linux platform implementation, never exposed to callers.
constexpr long kFutexWaitvSyscall = 449;
#else
constexpr long kFutexWaitvSyscall = -1;
#endif
constexpr std::uint32_t kFutex32 = 2;
struct futex_waitv_abi { std::uint64_t val, uaddr; std::uint32_t flags, reserved; };

int futex_wake(const std::uint32_t* p, bool local = false) noexcept
{
    if (p == nullptr) return 0;
    return static_cast<int>(::syscall(SYS_futex, p, local ? FUTEX_WAKE_PRIVATE : FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0));
}

bool backend_supported() noexcept
{
    static const bool supported = [] {
        if (kFutexWaitvSyscall < 0) return false;
        alignas(4) std::uint32_t word = 1;
        futex_waitv_abi item{0, reinterpret_cast<std::uint64_t>(&word), kFutex32, 0};
        timespec ts{0, 0};
        const long rc = ::syscall(kFutexWaitvSyscall, &item, 1, 0, &ts, CLOCK_MONOTONIC);
        return rc >= 0 || errno == EAGAIN || errno == ETIMEDOUT || errno == EINTR;
    }();
    return supported;
}
#elif defined(_WIN32)
bool backend_supported() noexcept { return true; }
#else
bool backend_supported() noexcept { return false; }
#endif

void report_unavailable_once() noexcept
{
    static std::atomic<bool> reported{false};
    if (!reported.exchange(true, std::memory_order_relaxed))
        ipc::log("recv_wait_set backend unavailable; keep one receive thread per route\n");
}
}  // namespace

namespace detail {
void set_recv_wait_trace_hook(recv_wait_trace_hook hook) noexcept
{
    g_recv_wait_trace_hook.store(hook, std::memory_order_release);
}

recv_wait_trace_hook get_recv_wait_trace_hook() noexcept
{
    return g_recv_wait_trace_hook.load(std::memory_order_relaxed);
}
} // namespace detail

struct recv_wait_set::impl
{
    struct entry { recv_wait_token token; std::uint32_t last{0}; bool enabled{true}; };
    std::mutex mutex;
    std::vector<entry> entries;
    std::vector<recv_wait_token> ready;
    bool stopped{false};
#if defined(__linux__)
    alignas(4) std::atomic<std::uint32_t> interrupt{0};
    std::atomic<unsigned> waiters{0};
    std::uint32_t observed_interrupt{0}; // mutex保护，保留“全扫后、入睡前”的控制通知
#elif defined(_WIN32)
    HANDLE stop_event{::CreateEventA(nullptr, FALSE, FALSE, nullptr)};
    ~impl() { if (stop_event != nullptr) ::CloseHandle(stop_event); }
#endif
};

void recv_wait_set_wake(const std::atomic<std::uint32_t>* seq, void* wake_handle) noexcept
{
    const auto hook = detail::get_recv_wait_trace_hook();
    int wake_result = 0;
    int error = 0;
#if defined(__linux__)
    (void)wake_handle;
    wake_result = futex_wake(reinterpret_cast<const std::uint32_t*>(seq));
    if (hook != nullptr && wake_result < 0) error = errno;
#elif defined(_WIN32)
    (void)seq;
    if (wake_handle != nullptr && !::SetEvent(static_cast<HANDLE>(wake_handle))) {
        error = static_cast<int>(::GetLastError());
        ipc::log("recv_wait_set SetEvent failed: %lu\n", static_cast<unsigned long>(::GetLastError()));
    }
#else
    (void)seq; (void)wake_handle;
#endif
    emit_recv_wait_trace(hook, [&]() noexcept {
        detail::recv_wait_trace_event event{};
        event.point = detail::recv_wait_trace_point::route_notify;
        event.result = error ? detail::recv_wait_trace_result::error : detail::recv_wait_trace_result::changed;
        event.flags = error ? (1u << 4) : 0;
        event.object = seq;
        event.token = seq;
        event.error_code = error;
        if (seq != nullptr) event.sequence_after = seq->load(std::memory_order_acquire);
        event.wake_result = wake_result;
        return event;
    });
}

bool recv_wait_change_supported() noexcept {
#if defined(__linux__)
    return backend_supported();
#else
    return false;
#endif
}

static recv_wait_result wait_change(const recv_wait_token& token, std::uint32_t expected,
    const std::atomic<std::uint32_t>& signal, std::uint32_t signal_expected,
    std::uint64_t timeout_ns, bool local) noexcept {
#if defined(__linux__)
    if (!backend_supported()) return recv_wait_result::unavailable;
    futex_waitv_abi slots[2]{}; unsigned count = 0;
    if (token.valid()) slots[count++] = {expected, reinterpret_cast<std::uint64_t>(token.sequence()), kFutex32, 0};
    slots[count++] = {signal_expected, reinterpret_cast<std::uint64_t>(&signal), kFutex32 | (local ? FUTEX_PRIVATE_FLAG : 0u), 0};
    timespec end{}, *deadline = nullptr;
    if (timeout_ns != UINT64_MAX) {
        if (::clock_gettime(CLOCK_MONOTONIC, &end)) return recv_wait_result::unavailable;
        end.tv_sec += timeout_ns / 1000000000ull;
        end.tv_nsec += timeout_ns % 1000000000ull;
        if (end.tv_nsec >= 1000000000L) { ++end.tv_sec; end.tv_nsec -= 1000000000L; }
        deadline = &end;
    }
    const auto result = ::syscall(kFutexWaitvSyscall, slots, count, 0, deadline, CLOCK_MONOTONIC);
    if (result >= 0 || errno == EAGAIN || errno == EINTR) return recv_wait_result::changed;
    return errno == ETIMEDOUT ? recv_wait_result::timeout : recv_wait_result::unavailable;
#else
    (void)token; (void)expected; (void)signal; (void)signal_expected; (void)timeout_ns; (void)local;
    return recv_wait_result::unavailable;
#endif
}

recv_wait_result recv_wait_change(const recv_wait_token& token, std::uint32_t expected,
    const std::atomic<std::uint32_t>& signal, std::uint32_t signal_expected,
    std::uint64_t timeout_ns) noexcept {
    return wait_change(token, expected, signal, signal_expected, timeout_ns, false);
}
void recv_local_signal::notify() noexcept {
    // 与wait的登记/复查构成顺序一致的StoreLoad握手，不能同时看不见对方。
    const auto hook = detail::get_recv_wait_trace_hook();
    const auto sequence_before = sequence_.fetch_add(1, std::memory_order_seq_cst);
    const auto waiters = waiters_.load(std::memory_order_seq_cst);
    int wake_result = 0;
    int error = 0;
#if defined(__linux__)
    if (waiters)
    {
        wake_result = futex_wake(reinterpret_cast<const std::uint32_t*>(&sequence_), true);
        if (hook != nullptr && wake_result < 0) error = errno;
    }
#endif
    emit_recv_wait_trace(hook, [&]() noexcept {
        detail::recv_wait_trace_event event{};
        event.point = detail::recv_wait_trace_point::local_notify;
        event.result = error ? detail::recv_wait_trace_result::error : detail::recv_wait_trace_result::changed;
        event.flags = waiters ? 1u : 0u;
        if (error) event.flags |= 1u << 4;
        event.object = this;
        event.error_code = error;
        event.sequence_before = sequence_before;
        event.sequence_after = sequence_before + 1;
        event.waiters = waiters;
        event.wake_result = wake_result;
        return event;
    });
}
recv_wait_result recv_local_signal::wait(const recv_wait_token& token, std::uint32_t expected,
    std::uint32_t local_expected, std::uint64_t timeout_ns) noexcept {
    const auto hook = detail::get_recv_wait_trace_hook();
    waiters_.fetch_add(1, std::memory_order_seq_cst);
    const auto observed_before = snapshot();
    emit_recv_wait_trace(hook, [&]() noexcept {
        detail::recv_wait_trace_event event{};
        event.point = detail::recv_wait_trace_point::local_wait_begin;
        event.object = this;
        event.token = token.sequence();
        event.sequence_before = local_expected;
        event.sequence_after = observed_before;
        event.expected = expected;
        event.observed = token.valid() ? token.sequence()->load(std::memory_order_acquire) : 0;
        event.waiters = waiters_.load(std::memory_order_seq_cst);
        return event;
    });
    const auto result = observed_before != local_expected ? recv_wait_result::changed
        : wait_change(token, expected, sequence_, local_expected, timeout_ns, true);
    std::uint32_t observed_after = 0;
    if (hook != nullptr) observed_after = snapshot();
    const auto waiters_after = waiters_.fetch_sub(1, std::memory_order_seq_cst) - 1;
    emit_recv_wait_trace(hook, [&]() noexcept {
        detail::recv_wait_trace_event event{};
        event.point = detail::recv_wait_trace_point::local_wait_end;
        event.result = result == recv_wait_result::changed ? detail::recv_wait_trace_result::changed
            : result == recv_wait_result::timeout ? detail::recv_wait_trace_result::timeout
            : detail::recv_wait_trace_result::unavailable;
        event.flags = result == recv_wait_result::unavailable ? (1u << 4) : 0;
        event.object = this;
        event.token = token.sequence();
        event.sequence_before = local_expected;
        event.sequence_after = observed_after;
        event.expected = expected;
        event.observed = token.valid() ? token.sequence()->load(std::memory_order_acquire) : 0;
        event.waiters = waiters_after;
        return event;
    });
    return result;
}

recv_wait_set::recv_wait_set() : impl_(new impl) {}
recv_wait_set::~recv_wait_set() { stop(); delete impl_; impl_ = nullptr; }

bool recv_wait_set::add(const recv_wait_token& token)
{
    const auto hook = detail::get_recv_wait_trace_hook();
    if (!impl_ || !token.valid() || !backend_supported()) {
        report_unavailable_once();
        emit_recv_wait_trace(hook, [&]() noexcept {
            detail::recv_wait_trace_event event{};
            event.point = detail::recv_wait_trace_point::set_add;
            event.result = detail::recv_wait_trace_result::unavailable;
            event.flags = 1u << 4;
            event.object = impl_;
            event.token = token.sequence();
            return event;
        });
        return false;
    }
#if defined(_WIN32)
    if (token.wake_handle_ == nullptr || impl_->stop_event == nullptr) {
        emit_recv_wait_trace(hook, [&]() noexcept {
            detail::recv_wait_trace_event event{};
            event.point = detail::recv_wait_trace_point::set_add;
            event.result = detail::recv_wait_trace_result::error;
            event.flags = 1u << 4;
            event.object = impl_;
            event.token = token.sequence();
            return event;
        });
        return false;
    }
#endif
    std::uint32_t entries = 0;
    std::uint32_t enabled = 0;
    bool added = false;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto it = std::find_if(impl_->entries.begin(), impl_->entries.end(),
                                     [&](const auto& e) { return e.token == token; });
        if (it != impl_->entries.end()) added = true;
        else if (impl_->entries.size() < kMaxRoutes) {
            impl_->entries.push_back({token, token.sequence()->load(std::memory_order_acquire)});
            added = true;
        }
        if (hook != nullptr)
        {
            entries = static_cast<std::uint32_t>(impl_->entries.size());
            enabled = static_cast<std::uint32_t>(std::count_if(impl_->entries.begin(), impl_->entries.end(),
                                                              [](const auto& e) { return e.enabled; }));
        }
    }
    emit_recv_wait_trace(hook, [&]() noexcept {
        detail::recv_wait_trace_event event{};
        event.point = detail::recv_wait_trace_point::set_add;
        event.result = added ? detail::recv_wait_trace_result::changed : detail::recv_wait_trace_result::error;
        if (!added) event.flags |= 1u << 4;
        event.object = impl_;
        event.token = token.sequence();
        event.sequence_after = token.sequence()->load(std::memory_order_acquire);
        event.entries = entries;
        event.enabled = enabled;
        return event;
    });
    return added;
}

bool recv_wait_set::remove(const recv_wait_token& token)
{
    const auto hook = detail::get_recv_wait_trace_hook();
    if (!impl_) return true;
    std::uint32_t entries = 0;
    std::uint32_t enabled = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->entries.erase(std::remove_if(impl_->entries.begin(), impl_->entries.end(),
                                            [&](const auto& e) { return e.token == token; }),
                             impl_->entries.end());
        interrupt();
        if (hook != nullptr)
        {
            entries = static_cast<std::uint32_t>(impl_->entries.size());
            enabled = static_cast<std::uint32_t>(std::count_if(impl_->entries.begin(), impl_->entries.end(),
                                                              [](const auto& e) { return e.enabled; }));
        }
    }
    emit_recv_wait_trace(hook, [&]() noexcept {
        detail::recv_wait_trace_event event{};
        event.point = detail::recv_wait_trace_point::set_remove;
        event.result = detail::recv_wait_trace_result::changed;
        event.object = impl_;
        event.token = token.sequence();
        event.sequence_after = token.valid() ? token.sequence()->load(std::memory_order_acquire) : 0;
        event.entries = entries;
        event.enabled = enabled;
        return event;
    });
    return true;
}

bool recv_wait_set::wait(std::chrono::milliseconds timeout)
{
    const auto hook = detail::get_recv_wait_trace_hook();
    if (!impl_ || !backend_supported()) {
        report_unavailable_once();
        emit_recv_wait_trace(hook, [&]() noexcept {
            detail::recv_wait_trace_event event{};
            event.point = detail::recv_wait_trace_point::set_wait_end;
            event.result = detail::recv_wait_trace_result::unavailable;
            event.flags = 1u << 4;
            event.object = impl_;
            return event;
        });
        return false;
    }
    std::unique_lock<std::mutex> lock(impl_->mutex);
    std::uint32_t enabled_count = 0;
    auto scan = [&] {
        impl_->ready.clear();
        if (hook != nullptr) enabled_count = 0;
        for (auto& entry : impl_->entries) {
            if (!entry.enabled) continue;
            if (hook != nullptr) ++enabled_count;
            const auto now = entry.token.sequence()->load(std::memory_order_acquire);
            if (now != entry.last) { entry.last = now; impl_->ready.push_back(entry.token); }
        }
        return !impl_->ready.empty();
    };
    const bool ready = scan();
#if defined(__linux__)
    const auto pending_interrupt = impl_->interrupt.load(std::memory_order_seq_cst);
    const auto interrupt_pending = pending_interrupt != impl_->observed_interrupt;
    if (ready || impl_->stopped || interrupt_pending) {
        impl_->observed_interrupt = pending_interrupt;
        const auto entries = hook != nullptr ? static_cast<std::uint32_t>(impl_->entries.size()) : 0;
        const auto ready_count = hook != nullptr ? static_cast<std::uint32_t>(impl_->ready.size()) : 0;
        const auto stopped = hook != nullptr && impl_->stopped;
        lock.unlock();
        emit_recv_wait_trace(hook, [&]() noexcept {
            detail::recv_wait_trace_event event{};
            event.point = detail::recv_wait_trace_point::set_wait_scan;
            event.result = ready_count ? detail::recv_wait_trace_result::changed : detail::recv_wait_trace_result::none;
            event.flags = (ready_count ? (1u << 1) : 0) | (stopped ? (1u << 3) : 0) |
                          (interrupt_pending ? (1u << 2) : 0);
            event.object = impl_;
            event.interrupt_before = pending_interrupt;
            event.interrupt_after = pending_interrupt;
            event.entries = entries;
            event.enabled = enabled_count;
            event.ready = ready_count;
            return event;
        });
        emit_recv_wait_trace(hook, [&]() noexcept {
            detail::recv_wait_trace_event event{};
            event.point = detail::recv_wait_trace_point::set_wait_end;
            event.result = detail::recv_wait_trace_result::changed;
            event.flags = (ready_count ? (1u << 1) : 0) | (stopped ? (1u << 3) : 0) |
                          (interrupt_pending ? (1u << 2) : 0);
            event.object = impl_;
            event.interrupt_before = pending_interrupt;
            event.interrupt_after = pending_interrupt;
            event.entries = entries;
            event.enabled = enabled_count;
            event.ready = ready_count;
            return event;
        });
        return true;
    }
    std::array<futex_waitv_abi, kMaxRoutes + 1> waiters;
    std::size_t count = 0;
    for (const auto& entry : impl_->entries)
        if (entry.enabled) waiters[count++] = {entry.last, reinterpret_cast<std::uint64_t>(entry.token.sequence()), kFutex32, 0};
    impl_->waiters.fetch_add(1, std::memory_order_seq_cst);
    const auto interrupt_value = impl_->observed_interrupt;
    waiters[count++] = {interrupt_value, reinterpret_cast<std::uint64_t>(&impl_->interrupt), kFutex32 | FUTEX_PRIVATE_FLAG, 0};
    timespec ts{}; timespec* tsp = nullptr;
    if (timeout.count() >= 0) {
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec += timeout.count() / 1000;
        ts.tv_nsec += (timeout.count() % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ++ts.tv_sec; ts.tv_nsec -= 1000000000L; }
        tsp = &ts;
    }
    const auto entries_before_wait = hook != nullptr ? static_cast<std::uint32_t>(impl_->entries.size()) : 0;
    const auto ready_before_wait = hook != nullptr ? static_cast<std::uint32_t>(impl_->ready.size()) : 0;
    const auto enabled_before_wait = hook != nullptr ? enabled_count : 0;
    const auto stopped_before_wait = hook != nullptr && impl_->stopped;
    lock.unlock();
    emit_recv_wait_trace(hook, [&]() noexcept {
        detail::recv_wait_trace_event event{};
        event.point = detail::recv_wait_trace_point::set_wait_scan;
        event.flags = (ready_before_wait ? (1u << 1) : 0) | (stopped_before_wait ? (1u << 3) : 0);
        event.object = impl_;
        event.interrupt_before = interrupt_value;
        event.interrupt_after = interrupt_value;
        event.entries = entries_before_wait;
        event.enabled = enabled_before_wait;
        event.ready = ready_before_wait;
        return event;
    });
    emit_recv_wait_trace(hook, [&]() noexcept {
        detail::recv_wait_trace_event event{};
        event.point = detail::recv_wait_trace_point::set_wait_begin;
        event.object = impl_;
        event.interrupt_before = interrupt_value;
        event.entries = entries_before_wait;
        event.enabled = enabled_before_wait;
        event.ready = ready_before_wait;
        return event;
    });
    const long rc = ::syscall(kFutexWaitvSyscall, waiters.data(), count, 0, tsp, CLOCK_MONOTONIC);
    const int error = errno;
    const auto interrupt_after_wait = hook != nullptr
        ? impl_->interrupt.load(std::memory_order_seq_cst) : interrupt_value;
    impl_->waiters.fetch_sub(1, std::memory_order_seq_cst);
    lock.lock();
    impl_->observed_interrupt = impl_->interrupt.load(std::memory_order_seq_cst);
    const auto stopped_after_wait = hook != nullptr && impl_->stopped;
    std::uint32_t ready_after_wait = 0;
    if (!(rc < 0 && (error == ETIMEDOUT || (error != EAGAIN && error != EINTR)))) {
        scan();
        if (hook != nullptr) ready_after_wait = static_cast<std::uint32_t>(impl_->ready.size());
    }
    const auto entries_after_wait = hook != nullptr ? static_cast<std::uint32_t>(impl_->entries.size()) : 0;
    const auto enabled_after_wait = hook != nullptr ? enabled_count : 0;
    const auto waiters_after_wait = hook != nullptr
        ? impl_->waiters.load(std::memory_order_seq_cst) : 0;
    lock.unlock();
    emit_recv_wait_trace(hook, [&]() noexcept {
        detail::recv_wait_trace_event event{};
        event.point = detail::recv_wait_trace_point::set_wait_scan;
        event.flags = (ready_after_wait ? (1u << 1) : 0) | (stopped_after_wait ? (1u << 3) : 0) |
                      (interrupt_after_wait != interrupt_value ? (1u << 2) : 0);
        event.object = impl_;
        event.interrupt_before = interrupt_value;
        event.interrupt_after = interrupt_after_wait;
        event.entries = entries_after_wait;
        event.enabled = enabled_after_wait;
        event.ready = ready_after_wait;
        return event;
    });
    emit_recv_wait_trace(hook, [&]() noexcept {
        detail::recv_wait_trace_event event{};
        event.point = detail::recv_wait_trace_point::set_wait_end;
        event.result = rc < 0 && error == ETIMEDOUT ? detail::recv_wait_trace_result::timeout
            : rc < 0 && error != EAGAIN && error != EINTR ? detail::recv_wait_trace_result::error
            : detail::recv_wait_trace_result::changed;
        event.flags = (ready_after_wait ? (1u << 1) : 0) | (stopped_after_wait ? (1u << 3) : 0) |
                      (interrupt_after_wait != interrupt_value ? (1u << 2) : 0);
        if (rc < 0 && error != ETIMEDOUT && error != EAGAIN && error != EINTR) event.flags |= 1u << 4;
        event.object = impl_;
        event.error_code = rc < 0 ? error : 0;
        event.observed = static_cast<std::uint32_t>(rc < 0 ? 0 : rc);
        event.interrupt_before = interrupt_value;
        event.interrupt_after = interrupt_after_wait;
        event.waiters = waiters_after_wait;
        event.entries = entries_after_wait;
        event.enabled = enabled_after_wait;
        event.ready = ready_after_wait;
        return event;
    });
    if (rc < 0 && error == ETIMEDOUT) return false;
    if (rc < 0 && error != EAGAIN && error != EINTR) {
        ipc::error("recv_wait_set futex_waitv failed: %d\n", error);
        return false;
    }
    return true;
#elif defined(_WIN32)
    if (ready || impl_->stopped) return true;
    if (impl_->stop_event == nullptr) return false;
    std::vector<HANDLE> handles;
    handles.reserve(impl_->entries.size() + 1);
    handles.push_back(impl_->stop_event);
    for (const auto& entry : impl_->entries)
        if (entry.enabled) handles.push_back(static_cast<HANDLE>(entry.token.wake_handle_));
    const DWORD ms = timeout.count() < 0 ? INFINITE :
        static_cast<DWORD>(std::min<std::int64_t>(timeout.count(), MAXDWORD - 1));
    lock.unlock();
    const DWORD rc = ::WaitForMultipleObjects(static_cast<DWORD>(handles.size()), handles.data(), FALSE, ms);
    const DWORD error = rc == WAIT_FAILED ? ::GetLastError() : 0;
    lock.lock();
    if (rc == WAIT_TIMEOUT) return false;
    if (rc == WAIT_FAILED || rc >= WAIT_ABANDONED_0 && rc < WAIT_ABANDONED_0 + handles.size()) {
        ipc::error("recv_wait_set WaitForMultipleObjects failed: %lu\n", static_cast<unsigned long>(error));
        return false;
    }
    scan();
    // Reset only after sequence accounting, then recheck to close the SetEvent/reset race.
    for (const auto& entry : impl_->entries) {
        if (!entry.enabled) continue;
        auto event = static_cast<HANDLE>(entry.token.wake_handle_);
        if (::ResetEvent(event) && entry.token.sequence()->load(std::memory_order_acquire) != entry.last)
            ::SetEvent(event);
    }
    return true;
#else
    (void)timeout;
    return false;
#endif
}

bool recv_wait_set::set_enabled(const recv_wait_token& token, bool enabled)
{
    if (!set_enabled_deferred(token, enabled)) return false;
    interrupt();
    return true;
}

bool recv_wait_set::set_enabled_deferred(const recv_wait_token& token, bool enabled)
{
    const auto hook = detail::get_recv_wait_trace_hook();
    if (!impl_) return false;
    bool found = false;
    std::uint32_t entries = 0;
    std::uint32_t enabled_count = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        const auto entry = std::find_if(impl_->entries.begin(), impl_->entries.end(),
                                       [&](const auto& e) { return e.token == token; });
        if (entry != impl_->entries.end()) {
            found = true;
            entry->enabled = enabled;
        }
        if (hook != nullptr)
        {
            entries = static_cast<std::uint32_t>(impl_->entries.size());
            enabled_count = static_cast<std::uint32_t>(std::count_if(impl_->entries.begin(), impl_->entries.end(),
                                                                    [](const auto& e) { return e.enabled; }));
        }
    }
    emit_recv_wait_trace(hook, [&]() noexcept {
        detail::recv_wait_trace_event event{};
        event.point = detail::recv_wait_trace_point::set_enable;
        event.result = found ? detail::recv_wait_trace_result::changed : detail::recv_wait_trace_result::error;
        if (!found) event.flags |= 1u << 4;
        event.object = impl_;
        event.token = token.sequence();
        event.sequence_after = token.valid() ? token.sequence()->load(std::memory_order_acquire) : 0;
        event.expected = enabled ? 1u : 0u;
        event.entries = entries;
        event.enabled = enabled_count;
        return event;
    });
    return found;
}

std::vector<recv_wait_token> recv_wait_set::consume_ready()
{
    if (!impl_) return {};
    const auto hook = detail::get_recv_wait_trace_hook();
    std::vector<recv_wait_token> result;
    std::uint32_t ready_count = 0;
    std::uint32_t entries = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (hook != nullptr)
        {
            ready_count = static_cast<std::uint32_t>(impl_->ready.size());
            entries = static_cast<std::uint32_t>(impl_->entries.size());
        }
        result = std::move(impl_->ready);
        impl_->ready.clear();
    }
    emit_recv_wait_trace(hook, [&]() noexcept {
        detail::recv_wait_trace_event event{};
        event.point = detail::recv_wait_trace_point::set_consume_ready;
        event.result = detail::recv_wait_trace_result::changed;
        event.object = impl_;
        event.entries = entries;
        event.ready = ready_count;
        return event;
    });
    return result;
}

void recv_wait_set::interrupt() noexcept
{
    if (!impl_) return;
    const auto hook = detail::get_recv_wait_trace_hook();
    std::uint32_t sequence_before = 0;
    std::uint32_t sequence_after = 0;
    std::uint32_t waiters = 0;
    int wake_result = 0;
    int error = 0;
#if defined(__linux__)
    sequence_before = impl_->interrupt.fetch_add(1, std::memory_order_seq_cst);
    sequence_after = sequence_before + 1;
    waiters = impl_->waiters.load(std::memory_order_seq_cst);
    if (waiters) {
        wake_result = futex_wake(reinterpret_cast<const std::uint32_t*>(&impl_->interrupt), true);
        if (hook != nullptr && wake_result < 0) error = errno;
    }
#elif defined(_WIN32)
    if (impl_->stop_event != nullptr && !::SetEvent(impl_->stop_event)) error = static_cast<int>(::GetLastError());
#endif
    emit_recv_wait_trace(hook, [&]() noexcept {
        detail::recv_wait_trace_event event{};
        event.point = detail::recv_wait_trace_point::set_interrupt;
        event.result = error ? detail::recv_wait_trace_result::error : detail::recv_wait_trace_result::changed;
        event.flags = waiters ? 1u : 0u;
        if (error) event.flags |= 1u << 4;
        event.object = impl_;
        event.error_code = error;
        event.wake_result = wake_result;
        event.sequence_before = sequence_before;
        event.sequence_after = sequence_after;
        event.waiters = waiters;
        return event;
    });
}

void recv_wait_set::stop() noexcept
{
    if (!impl_) return;
    const auto hook = detail::get_recv_wait_trace_hook();
    std::uint32_t entries = 0;
    std::uint32_t enabled = 0;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->stopped = true;
        interrupt();
        if (hook != nullptr)
        {
            entries = static_cast<std::uint32_t>(impl_->entries.size());
            enabled = static_cast<std::uint32_t>(std::count_if(impl_->entries.begin(), impl_->entries.end(),
                                                              [](const auto& e) { return e.enabled; }));
        }
    }
    emit_recv_wait_trace(hook, [&]() noexcept {
        detail::recv_wait_trace_event event{};
        event.point = detail::recv_wait_trace_point::set_stop;
        event.result = detail::recv_wait_trace_result::changed;
        event.flags = 1u << 3;
        event.object = impl_;
        event.entries = entries;
        event.enabled = enabled;
        return event;
    });
}
}  // namespace ipc

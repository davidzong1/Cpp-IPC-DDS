#include "libipc/recv_wait_set.h"

#include <algorithm>
#include <atomic>
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

int futex_wake(const std::uint32_t* p) noexcept
{
    if (p == nullptr) return 0;
    return static_cast<int>(::syscall(SYS_futex, p, FUTEX_WAKE, INT_MAX, nullptr, nullptr, 0));
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

struct recv_wait_set::impl
{
    struct entry { recv_wait_token token; std::uint32_t last{0}; };
    std::mutex mutex;
    std::vector<entry> entries;
    std::vector<recv_wait_token> ready;
    bool stopped{false};
#if defined(__linux__)
    alignas(4) std::atomic<std::uint32_t> interrupt{0};
#elif defined(_WIN32)
    HANDLE stop_event{::CreateEventA(nullptr, FALSE, FALSE, nullptr)};
    ~impl() { if (stop_event != nullptr) ::CloseHandle(stop_event); }
#endif
};

void recv_wait_set_wake(const std::atomic<std::uint32_t>* seq, void* wake_handle) noexcept
{
#if defined(__linux__)
    (void)wake_handle;
    futex_wake(reinterpret_cast<const std::uint32_t*>(seq));
#elif defined(_WIN32)
    (void)seq;
    if (wake_handle != nullptr && !::SetEvent(static_cast<HANDLE>(wake_handle)))
        ipc::log("recv_wait_set SetEvent failed: %lu\n", static_cast<unsigned long>(::GetLastError()));
#else
    (void)seq; (void)wake_handle;
#endif
}

recv_wait_set::recv_wait_set() : impl_(new impl) {}
recv_wait_set::~recv_wait_set() { stop(); delete impl_; impl_ = nullptr; }

bool recv_wait_set::add(const recv_wait_token& token)
{
    if (!impl_ || !token.valid() || !backend_supported()) {
        report_unavailable_once();
        return false;
    }
#if defined(_WIN32)
    if (token.wake_handle_ == nullptr || impl_->stop_event == nullptr) return false;
#endif
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto it = std::find_if(impl_->entries.begin(), impl_->entries.end(),
                                 [&](const auto& e) { return e.token == token; });
    if (it != impl_->entries.end()) return true;
    if (impl_->entries.size() >= kMaxRoutes) return false;
    impl_->entries.push_back({token, token.sequence()->load(std::memory_order_acquire)});
    return true;
}

bool recv_wait_set::remove(const recv_wait_token& token)
{
    if (!impl_) return true;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->entries.erase(std::remove_if(impl_->entries.begin(), impl_->entries.end(),
                                        [&](const auto& e) { return e.token == token; }),
                         impl_->entries.end());
#if defined(__linux__)
    impl_->interrupt.fetch_add(1, std::memory_order_release);
    futex_wake(reinterpret_cast<const std::uint32_t*>(&impl_->interrupt));
#elif defined(_WIN32)
    if (impl_->stop_event != nullptr) ::SetEvent(impl_->stop_event);
#endif
    return true;
}

bool recv_wait_set::wait(std::chrono::milliseconds timeout)
{
    if (!impl_ || !backend_supported()) { report_unavailable_once(); return false; }
    std::unique_lock<std::mutex> lock(impl_->mutex);
    auto scan = [&] {
        impl_->ready.clear();
        for (auto& entry : impl_->entries) {
            const auto now = entry.token.sequence()->load(std::memory_order_acquire);
            if (now != entry.last) { entry.last = now; impl_->ready.push_back(entry.token); }
        }
        return !impl_->ready.empty();
    };
    if (scan() || impl_->stopped) return true;
#if defined(__linux__)
    std::vector<futex_waitv_abi> waiters;
    waiters.reserve(impl_->entries.size() + 1);
    for (const auto& entry : impl_->entries)
        waiters.push_back({entry.last, reinterpret_cast<std::uint64_t>(entry.token.sequence()), kFutex32, 0});
    const auto interrupt_value = impl_->interrupt.load(std::memory_order_acquire);
    waiters.push_back({interrupt_value, reinterpret_cast<std::uint64_t>(&impl_->interrupt), kFutex32, 0});
    timespec ts{}; timespec* tsp = nullptr;
    if (timeout.count() >= 0) {
        ::clock_gettime(CLOCK_MONOTONIC, &ts);
        ts.tv_sec += timeout.count() / 1000;
        ts.tv_nsec += (timeout.count() % 1000) * 1000000L;
        if (ts.tv_nsec >= 1000000000L) { ++ts.tv_sec; ts.tv_nsec -= 1000000000L; }
        tsp = &ts;
    }
    lock.unlock();
    const long rc = ::syscall(kFutexWaitvSyscall, waiters.data(), waiters.size(), 0, tsp, CLOCK_MONOTONIC);
    const int error = errno;
    lock.lock();
    if (rc < 0 && error == ETIMEDOUT) return false;
    if (rc < 0 && error != EAGAIN && error != EINTR) {
        ipc::error("recv_wait_set futex_waitv failed: %d\n", error);
        return false;
    }
    scan();
    return true;
#elif defined(_WIN32)
    if (impl_->stop_event == nullptr) return false;
    std::vector<HANDLE> handles;
    handles.reserve(impl_->entries.size() + 1);
    handles.push_back(impl_->stop_event);
    for (const auto& entry : impl_->entries)
        handles.push_back(static_cast<HANDLE>(entry.token.wake_handle_));
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

std::vector<recv_wait_token> recv_wait_set::consume_ready()
{
    if (!impl_) return {};
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto result = std::move(impl_->ready);
    impl_->ready.clear();
    return result;
}

void recv_wait_set::stop() noexcept
{
    if (!impl_) return;
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->stopped = true;
#if defined(__linux__)
    impl_->interrupt.fetch_add(1, std::memory_order_release);
    futex_wake(reinterpret_cast<const std::uint32_t*>(&impl_->interrupt));
#elif defined(_WIN32)
    if (impl_->stop_event != nullptr) ::SetEvent(impl_->stop_event);
#endif
}
}  // namespace ipc

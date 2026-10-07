#pragma once

#include <chrono>
#include <cstdint>
#include <atomic>
#include <vector>

#include "libipc/export.h"

namespace ipc {
namespace detail { class waiter; }

/*
 * Internal wait-path diagnostic seam.  It is deliberately separate from the
 * public wait API: when no hook is installed the implementation only performs
 * one relaxed atomic load and does not change the wait protocol.  A hook must
 * be noexcept and should not block or call back into recv_wait_set.
 *
 * The event is a value type so a collector can copy it into a bounded buffer
 * from any wait/notify thread.  `flags` is a point-specific bit set:
 *   bit 0: a futex/event wake was issued;
 *   bit 1: the initial scan found a ready entry;
 *   bit 2: the return observed an interrupt;
 *   bit 3: the wait set was stopped;
 *   bit 4: the backend returned an error/unavailable result.
 */
namespace detail {
enum class recv_wait_trace_point : std::uint8_t {
    local_notify = 0,
    local_wait_begin = 1,
    local_wait_end = 2,
    set_add = 3,
    set_remove = 4,
    set_enable = 5,
    set_interrupt = 6,
    set_wait_begin = 7,
    set_wait_scan = 8,
    set_wait_end = 9,
    set_consume_ready = 10,
    set_stop = 11,
    route_notify = 12,
};

enum class recv_wait_trace_result : std::int8_t {
    none = 0,
    changed = 1,
    timeout = 2,
    unavailable = 3,
    error = 4,
};

struct recv_wait_trace_event {
    recv_wait_trace_point point{recv_wait_trace_point::local_notify};
    recv_wait_trace_result result{recv_wait_trace_result::none};
    std::uint16_t flags{0};
    const void* object{nullptr};
    const void* token{nullptr};
    std::int32_t error_code{0};
    std::int32_t wake_result{0};
    std::uint32_t sequence_before{0};
    std::uint32_t sequence_after{0};
    std::uint32_t expected{0};
    std::uint32_t observed{0};
    std::uint32_t interrupt_before{0};
    std::uint32_t interrupt_after{0};
    std::uint32_t waiters{0};
    std::uint32_t entries{0};
    std::uint32_t enabled{0};
    std::uint32_t ready{0};
};

using recv_wait_trace_hook = void (*)(const recv_wait_trace_event&) noexcept;

IPC_EXPORT void set_recv_wait_trace_hook(recv_wait_trace_hook hook) noexcept;
IPC_EXPORT recv_wait_trace_hook get_recv_wait_trace_hook() noexcept;
} // namespace detail

class IPC_EXPORT recv_wait_token
{
public:
    recv_wait_token() noexcept = default;
    bool valid() const noexcept { return seq_ != nullptr; }
    const std::atomic<std::uint32_t>* sequence() const noexcept { return seq_; }
    friend bool operator==(const recv_wait_token& a, const recv_wait_token& b) noexcept
    { return a.seq_ == b.seq_; }
    friend bool operator!=(const recv_wait_token& a, const recv_wait_token& b) noexcept
    { return !(a == b); }
private:
    friend class detail::waiter;
    friend class recv_wait_set;
    explicit recv_wait_token(const std::atomic<std::uint32_t>* seq,
                             void* wake_handle = nullptr) noexcept
        : seq_(seq), wake_handle_(wake_handle) {}
    const std::atomic<std::uint32_t>* seq_{nullptr};
    void* wake_handle_{nullptr};
};

/* Internal wake hook used by waiter::wake(). It is intentionally a no-op on
 * platforms without a wait-set backend. */
IPC_EXPORT void recv_wait_set_wake(const std::atomic<std::uint32_t>* seq,
                                   void* wake_handle = nullptr) noexcept;

// 固定两个槽位的协作等待。调用者必须保活token映射和本地signal直到返回。
// 无效token表示只等signal；UINT64_MAX表示无限等待，其他值为剩余纳秒。
enum class recv_wait_result { changed, timeout, unavailable };
IPC_EXPORT bool recv_wait_change_supported() noexcept;
IPC_EXPORT recv_wait_result recv_wait_change(const recv_wait_token& token, std::uint32_t expected,
    const std::atomic<std::uint32_t>& signal, std::uint32_t signal_expected,
    std::uint64_t timeout_ns) noexcept;

// 进程内的队列/取消通知；不改变跨进程SHM序号或原双槽接口。
class IPC_EXPORT recv_local_signal {
public:
    std::uint32_t snapshot() const noexcept { return sequence_.load(std::memory_order_seq_cst); }
    void notify() noexcept;
    recv_wait_result wait(const recv_wait_token& token, std::uint32_t expected,
                          std::uint32_t local_expected, std::uint64_t timeout_ns) noexcept;
private:
    alignas(4) std::atomic<std::uint32_t> sequence_{0};
    std::atomic<unsigned> waiters_{0};
};

class IPC_EXPORT recv_wait_set
{
public:
    struct impl;
    recv_wait_set();
    ~recv_wait_set();
    recv_wait_set(const recv_wait_set&) = delete;
    recv_wait_set& operator=(const recv_wait_set&) = delete;
    bool add(const recv_wait_token& token);
    bool remove(const recv_wait_token& token);
    // 保留注册槽位和观察序号；中断旧快照后再按启用状态构造等待集合。
    bool set_enabled(const recv_wait_token& token, bool enabled);
    // 调用方释放上层锁后必须interrupt；用于将交接唤醒移出worker表临界区。
    bool set_enabled_deferred(const recv_wait_token& token, bool enabled);
    void interrupt() noexcept;
    bool wait(std::chrono::milliseconds timeout);
    std::vector<recv_wait_token> consume_ready();
    void stop() noexcept;
private:
    impl* impl_{nullptr};
};
}  // namespace ipc

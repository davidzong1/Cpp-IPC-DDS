#pragma once

#include <chrono>
#include <cstdint>
#include <atomic>
#include <vector>

#include "libipc/export.h"

namespace ipc {
namespace detail { class waiter; }

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

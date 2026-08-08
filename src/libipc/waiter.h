#pragma once

#include <utility>
#include <string>
#include <mutex>
#include <atomic>
#include <new>

#include "libipc/def.h"
#include "libipc/mutex.h"
#include "libipc/condition.h"
#include "libipc/shm.h"
#include "libipc/platform/detail.h"

namespace ipc {
namespace detail {

class waiter {
    struct state_t {
        std::atomic<std::uint32_t> waiters {0};
        std::atomic<std::uint32_t> seq     {0};
    };

    ipc::sync::condition cond_;
    ipc::sync::mutex     lock_;
    ipc::shm::handle     state_h_;
    std::atomic<bool>    quit_ {false};

    state_t* state() const noexcept {
        return static_cast<state_t*>(state_h_.get());
    }

    bool wake(bool all) noexcept {
        auto st = state();
        if (st != nullptr) {
            st->seq.fetch_add(1, std::memory_order_release);
        }
        {
            IPC_UNUSED_ std::lock_guard<ipc::sync::mutex> barrier{lock_};
            st = state();
            if ((st != nullptr) &&
                (st->waiters.load(std::memory_order_acquire) == 0)) {
                return true;
            }
        }
        return all ? cond_.broadcast(lock_) : cond_.notify(lock_);
    }

public:
    static void init();

    waiter() = default;
    waiter(char const *name) {
        open(name);
    }

    ~waiter() {
        close();
    }

    bool valid() const noexcept {
        return cond_.valid() && lock_.valid() && state_h_.valid();
    }

    bool open(char const *name) noexcept {
        quit_.store(false, std::memory_order_relaxed);
        if (!cond_.open((std::string{name} + "_WAITER_COND_").c_str())) {
            return false;
        }
        if (!lock_.open((std::string{name} + "_WAITER_LOCK_").c_str())) {
            cond_.close();
            return false;
        }
        if (!state_h_.acquire((std::string{name} + "_WAITER_STATE_").c_str(),
                              sizeof(state_t))) {
            lock_.close();
            cond_.close();
            return false;
        }
        if (state_h_.ref() <= 1) {
            new (state_h_.get()) state_t();
        }
        return valid();
    }

    void close() noexcept {
        state_h_.release();
        cond_.close();
        lock_.close();
    }

    void clear() noexcept {
        state_h_.clear();
        cond_.clear();
        lock_.clear();
    }

    static void clear_storage(char const *name) noexcept {
        ipc::sync::condition::clear_storage((std::string{name} + "_WAITER_COND_").c_str());
        ipc::sync::mutex::clear_storage((std::string{name} + "_WAITER_LOCK_").c_str());
        ipc::shm::handle::clear_storage((std::string{name} + "_WAITER_STATE_").c_str());
    }

    template <typename F>
    bool wait_if(F &&pred, std::uint64_t tm = ipc::invalid_value) noexcept {
        IPC_UNUSED_ std::lock_guard<ipc::sync::mutex> guard {lock_};
        while ([this, &pred] {
                    return !quit_.load(std::memory_order_relaxed)
                        && std::forward<F>(pred)();
                }()) {
            auto st = state();
            if (st != nullptr) {
                st->waiters.fetch_add(1, std::memory_order_acq_rel);
                bool ok = cond_.wait(lock_, tm);
                st->waiters.fetch_sub(1, std::memory_order_acq_rel);
                if (!ok) return false;
            }
            else if (!cond_.wait(lock_, tm)) return false;
        }
        return true;
    }

    std::uint32_t waiter_count() const noexcept {
        auto st = state();
        return (st == nullptr) ? 0 : st->waiters.load(std::memory_order_acquire);
    }

    bool notify() noexcept {
        return wake(false);
    }

    bool broadcast() noexcept {
        return wake(true);
    }

    bool quit_waiting() {
        quit_.store(true, std::memory_order_release);
        return broadcast();
    }
};

} // namespace detail
} // namespace ipc

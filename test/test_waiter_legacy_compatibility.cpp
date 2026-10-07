#include <atomic>
#include <chrono>
#include <future>
#include <mutex>
#include <string>

#include <gtest/gtest.h>
#include "libipc/waiter.h"

namespace {
using namespace std::chrono_literals;

struct LegacyState {
    std::atomic<std::uint32_t> waiters{0};
    std::atomic<std::uint32_t> sequence{0};
};

struct LegacyPair {
    const std::string name = "waiter_legacy_" + std::to_string(
        std::chrono::steady_clock::now().time_since_epoch().count());
    ipc::detail::waiter sender{name.c_str()};
    ipc::sync::condition condition{(name + "_WAITER_COND_").c_str()};
    ipc::sync::mutex mutex{(name + "_WAITER_LOCK_").c_str()};
    ipc::shm::handle state;

    LegacyPair() {
        state.acquire((name + "_WAITER_STATE_").c_str(), sizeof(LegacyState));
    }
    ~LegacyPair() {
        state.release();
        mutex.close();
        condition.close();
        sender.close();
        ipc::detail::waiter::clear_storage(name.c_str());
    }
};
}

TEST(WaiterLegacyCompatibility, PredicateWindowNotificationMustReachLegacyConditionWait) {
    LegacyPair pair;
    ASSERT_TRUE(pair.sender.valid());
    ASSERT_TRUE(pair.state.valid());
    ASSERT_TRUE(pair.condition.valid());
    ASSERT_TRUE(pair.mutex.valid());
    std::atomic<bool> published{false};
    std::promise<void> entered, released;
    auto release = released.get_future();
    auto receiver = std::async(std::launch::async, [&] {
        std::lock_guard<ipc::sync::mutex> guard{pair.mutex};
        const bool pending = !published.load();
        entered.set_value();
        release.wait();
        if (!pending) return true;
        // 旧二进制在谓词检查后登记；段名、布局和等待顺序保持父版本协议。
        auto* state = static_cast<LegacyState*>(pair.state.get());
        state->waiters.fetch_add(1, std::memory_order_acq_rel);
        const bool ready = pair.condition.wait(pair.mutex, 100);
        state->waiters.fetch_sub(1, std::memory_order_acq_rel);
        return ready && published.load();
    });
    EXPECT_EQ(entered.get_future().wait_for(500ms), std::future_status::ready);
    auto notifier = std::async(std::launch::async, [&] {
        published.store(true);
        return pair.sender.broadcast();
    });
    const auto notified = notifier.wait_for(10ms);
    released.set_value();
    EXPECT_EQ(notified, std::future_status::timeout);
    EXPECT_TRUE(notifier.get());
    EXPECT_TRUE(receiver.get());
    EXPECT_EQ(pair.sender.waiter_count(), 0u);
}

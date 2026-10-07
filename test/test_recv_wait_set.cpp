#include <atomic>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdint>
#include <future>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#if defined(__linux__)
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "libipc/ipc.h"
#include "libipc/recv_wait_set.h"

#include <gtest/gtest.h>

namespace {
using namespace std::chrono_literals;

constexpr std::size_t trace_event_capacity = 512;
std::array<ipc::detail::recv_wait_trace_event, trace_event_capacity> trace_events{};
std::atomic<std::size_t> trace_event_count{0};

void collect_wait_trace(const ipc::detail::recv_wait_trace_event& event) noexcept
{
    const auto index = trace_event_count.fetch_add(1, std::memory_order_relaxed);
    if (index < trace_event_capacity) trace_events[index] = event;
}

struct WaitTraceHookGuard
{
    WaitTraceHookGuard() { ipc::detail::set_recv_wait_trace_hook(&collect_wait_trace); }
    ~WaitTraceHookGuard() { ipc::detail::set_recv_wait_trace_hook(nullptr); }
};

std::size_t count_trace(ipc::detail::recv_wait_trace_point point,
                        ipc::detail::recv_wait_trace_result result = ipc::detail::recv_wait_trace_result::none)
{
    const auto count = std::min(trace_event_count.load(std::memory_order_acquire), trace_event_capacity);
    std::size_t found = 0;
    for (std::size_t i = 0; i < count; ++i)
        if (trace_events[i].point == point && (result == ipc::detail::recv_wait_trace_result::none || trace_events[i].result == result))
            ++found;
    return found;
}

struct RoutePair
{
    std::string name;
    std::unique_ptr<ipc::route> pub;
    std::unique_ptr<ipc::route> sub;

    explicit RoutePair(const char* tag)
    {
        static std::atomic<unsigned> serial{0};
        name = std::string("recv_wait_") + tag + "_" + std::to_string(serial.fetch_add(1));
        pub = std::make_unique<ipc::route>(name.c_str(), ipc::sender, false);
        sub = std::make_unique<ipc::route>(name.c_str(), ipc::receiver, false);
    }

    ~RoutePair()
    {
        pub.reset();
        sub.reset();
        ipc::route::clear_storage(name.c_str());
    }
};

bool require_backend(ipc::recv_wait_set& set, const ipc::recv_wait_token& token)
{
    return set.add(token);
}
}  // namespace

TEST(RecvWaitTrace, ReportsSignalSequenceWaiterAndWaitSetOutcomes)
{
    if (!ipc::recv_wait_change_supported()) GTEST_SKIP();
    trace_event_count.store(0, std::memory_order_relaxed);
    WaitTraceHookGuard hook_guard;

    ipc::recv_local_signal signal;
    const auto before = signal.snapshot();
    signal.notify();
    EXPECT_EQ(signal.wait({}, 0, before, 1000000), ipc::recv_wait_result::changed);

    RoutePair pair{"trace"}; ipc::recv_wait_set set;
    const auto token = pair.sub->read_wait_token();
    ASSERT_TRUE(set.add(token));
    ASSERT_TRUE(set.set_enabled(token, false));
    ASSERT_TRUE(set.set_enabled(token, true));
    EXPECT_TRUE(set.wait(0ms)); // 消费 set_enabled 的 interrupt。
    EXPECT_FALSE(set.wait(2ms));
    ASSERT_TRUE(pair.pub->try_send("trace", sizeof("trace"), 100));
    EXPECT_TRUE(set.wait(100ms));
    EXPECT_EQ(set.consume_ready().size(), 1u);
    EXPECT_FALSE(pair.sub->recv(0).empty());
    EXPECT_TRUE(set.remove(token));

    EXPECT_EQ(count_trace(ipc::detail::recv_wait_trace_point::local_notify,
                          ipc::detail::recv_wait_trace_result::changed), 1u);
    EXPECT_EQ(count_trace(ipc::detail::recv_wait_trace_point::local_wait_begin), 1u);
    EXPECT_EQ(count_trace(ipc::detail::recv_wait_trace_point::local_wait_end,
                          ipc::detail::recv_wait_trace_result::changed), 1u);
    EXPECT_GE(count_trace(ipc::detail::recv_wait_trace_point::set_enable), 2u);
    EXPECT_GE(count_trace(ipc::detail::recv_wait_trace_point::set_wait_scan), 3u);
    EXPECT_EQ(count_trace(ipc::detail::recv_wait_trace_point::set_wait_begin), 1u);
    EXPECT_EQ(count_trace(ipc::detail::recv_wait_trace_point::set_wait_end,
                          ipc::detail::recv_wait_trace_result::timeout), 1u);
    EXPECT_GE(count_trace(ipc::detail::recv_wait_trace_point::route_notify), 1u);
    EXPECT_GE(count_trace(ipc::detail::recv_wait_trace_point::set_consume_ready), 1u);

    bool saw_sequence = false;
    const auto count = std::min(trace_event_count.load(std::memory_order_acquire), trace_event_capacity);
    for (std::size_t i = 0; i < count; ++i) {
        const auto& event = trace_events[i];
        if (event.point == ipc::detail::recv_wait_trace_point::local_notify) {
            EXPECT_EQ(event.sequence_before, before);
            EXPECT_EQ(event.sequence_after, before + 1);
            EXPECT_EQ(event.waiters, 0u);
            saw_sequence = true;
        }
    }
    EXPECT_TRUE(saw_sequence);
}

TEST(RecvWaitChange, SnapshotClosesNotificationBeforeSleepAndPreservesDeadline)
{
    if (!ipc::recv_wait_change_supported()) GTEST_SKIP();
    std::atomic<std::uint32_t> signal{1};
    EXPECT_EQ(ipc::recv_wait_change({}, 0, signal, 0, 1000000000), ipc::recv_wait_result::changed);
    const auto start = std::chrono::steady_clock::now();
    EXPECT_EQ(ipc::recv_wait_change({}, 0, signal, 1, 20000000), ipc::recv_wait_result::timeout);
    EXPECT_GE(std::chrono::steady_clock::now() - start, 15ms);
}

TEST(RecvWaitChange, BothSharedDataAndLocalCancellationWakeBlockedCaller)
{
    if (!ipc::recv_wait_change_supported()) GTEST_SKIP();
    RoutePair pair{"dual_wait"}; std::atomic<std::uint32_t> signal{0};
    const auto token = pair.sub->read_wait_token(); ASSERT_TRUE(token.valid());
    for (bool cancel : {false, true}) {
        const auto seq = token.sequence()->load(); const auto local = signal.load();
        auto blocked = std::async(std::launch::async, [&] {
            return ipc::recv_wait_change(token, seq, signal, local, 1000000000);
        });
        EXPECT_EQ(blocked.wait_for(10ms), std::future_status::timeout);
        if (cancel) { ++signal; ipc::recv_wait_set_wake(&signal); }
        else ASSERT_TRUE(pair.pub->try_send("wake", sizeof("wake"), 100));
        EXPECT_EQ(blocked.wait_for(500ms), std::future_status::ready);
        EXPECT_EQ(blocked.get(), ipc::recv_wait_result::changed);
        if (!cancel) EXPECT_FALSE(pair.sub->recv(0).empty());
    }
}

TEST(RecvWaitSet, DisabledEntryRetainsChangesUntilReenabled)
{
    RoutePair pair{"paused"}; ipc::recv_wait_set set;
    const auto token = pair.sub->read_wait_token();
    if (!require_backend(set, token)) GTEST_SKIP();
    ASSERT_TRUE(set.set_enabled(token, false));
    EXPECT_TRUE(set.wait(0ms)); // 消费暂停的控制通知，随后才度量真实空闲等待。
    ASSERT_TRUE(pair.pub->try_send("paused", sizeof("paused"), 100));
    EXPECT_FALSE(set.wait(20ms));
    EXPECT_TRUE(set.consume_ready().empty());
    ASSERT_TRUE(set.set_enabled(token, true));
    ASSERT_TRUE(set.wait(1000ms));
    EXPECT_EQ(set.consume_ready().size(), 1u);
    EXPECT_FALSE(pair.sub->recv(0).empty());
    EXPECT_TRUE(set.remove(token));
    EXPECT_FALSE(set.set_enabled(token, true));
}

TEST(RecvLocalSignal, NotificationBeforeRegistrationAndBothWakeSources)
{
    if (!ipc::recv_wait_change_supported()) GTEST_SKIP();
    ipc::recv_local_signal signal;
    const auto old = signal.snapshot();
    signal.notify(); // 没有等待者：跳过系统调用后，旧快照依然必须立即返回。
    EXPECT_EQ(signal.wait({}, 0, old, 1000000000), ipc::recv_wait_result::changed);
    EXPECT_EQ(signal.wait({}, 0, signal.snapshot(), 1000000), ipc::recv_wait_result::timeout);
    RoutePair pair{"private_signal"}; const auto token = pair.sub->read_wait_token();
    for (bool local : {false, true}) {
        const auto seq = token.sequence()->load(); const auto snapshot = signal.snapshot();
        std::vector<std::future<ipc::recv_wait_result>> waiters;
        for (unsigned i = 0; i < 4; ++i) waiters.push_back(std::async(std::launch::async, [&] {
            return signal.wait(token, seq, snapshot, 1000000000);
        }));
        for (auto& waiter : waiters) EXPECT_EQ(waiter.wait_for(5ms), std::future_status::timeout);
        if (local) signal.notify();
        else ASSERT_TRUE(pair.pub->try_send("all", sizeof("all"), 100));
        for (auto& waiter : waiters) {
            EXPECT_EQ(waiter.wait_for(500ms), std::future_status::ready);
            EXPECT_EQ(waiter.get(), ipc::recv_wait_result::changed);
        }
        if (!local) EXPECT_FALSE(pair.sub->recv(0).empty());
    }
}

TEST(RecvLocalSignal, ConcurrentRegistrationDoesNotLoseNotification)
{
    if (!ipc::recv_wait_change_supported()) GTEST_SKIP();
    ipc::recv_local_signal signal;
    std::atomic<unsigned> request{0}, acknowledged{0};
    auto notifier = std::async(std::launch::async, [&] {
        for (unsigned i = 1; i <= 1000; ++i) {
            while (request.load() != i) std::this_thread::yield();
            signal.notify(); acknowledged.store(i);
        }
    });
    for (unsigned i = 1; i <= 1000; ++i) {
        const auto snapshot = signal.snapshot(); request.store(i);
        EXPECT_EQ(signal.wait({}, 0, snapshot, 100000000), ipc::recv_wait_result::changed);
        while (acknowledged.load() != i) std::this_thread::yield();
    }
    notifier.get();
}

TEST(RecvWaitSet, DeferredResumeInterruptsExistingSnapshot)
{
    RoutePair pair{"deferred_resume"}; ipc::recv_wait_set set;
    const auto token = pair.sub->read_wait_token();
    if (!require_backend(set, token)) GTEST_SKIP();
    ASSERT_TRUE(set.set_enabled(token, false));
    EXPECT_TRUE(set.wait(0ms));
    auto blocked = std::async(std::launch::async, [&] { return set.wait(1000ms); });
    EXPECT_EQ(blocked.wait_for(10ms), std::future_status::timeout);
    ASSERT_TRUE(pair.pub->try_send("ready", sizeof("ready"), 100));
    ASSERT_TRUE(set.set_enabled_deferred(token, true));
    set.interrupt();
    EXPECT_EQ(blocked.wait_for(500ms), std::future_status::ready);
    EXPECT_TRUE(blocked.get());
    EXPECT_EQ(set.consume_ready().size(), 1u);
}

TEST(RecvWaitSet, ControlNotificationBeforeWaitIsNotLost)
{
    RoutePair pair{"early_interrupt"}; ipc::recv_wait_set set;
    if (!require_backend(set, pair.sub->read_wait_token())) GTEST_SKIP();
    set.interrupt(); // 对应worker全扫后、尚未进入wait时的最后一个getter释放。
    const auto start = std::chrono::steady_clock::now();
    EXPECT_TRUE(set.wait(1000ms));
    EXPECT_LT(std::chrono::steady_clock::now() - start, 200ms);
    EXPECT_TRUE(set.consume_ready().empty());
    EXPECT_FALSE(set.wait(1ms));
}

TEST(RecvWaitSet, OneRouteMessageProducesOneReadyToken)
{
    RoutePair pair{"one"};
    ipc::recv_wait_set set;
    if (!require_backend(set, pair.sub->read_wait_token())) { GTEST_SKIP(); return; }
    ASSERT_TRUE(pair.pub->try_send("hello", sizeof("hello"), 100));
    ASSERT_TRUE(set.wait(1000ms));
    const auto ready = set.consume_ready();
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready[0], pair.sub->read_wait_token());
    EXPECT_FALSE(pair.sub->recv(100).empty());
}

TEST(RecvWaitSet, AllChangedRoutesAreLevelTriggered)
{
    RoutePair first{"first"};
    RoutePair second{"second"};
    ipc::recv_wait_set set;
    if (!require_backend(set, first.sub->read_wait_token())) { GTEST_SKIP(); return; }
    ASSERT_TRUE(set.add(second.sub->read_wait_token()));
    ASSERT_TRUE(first.pub->try_send("a", sizeof("a"), 100));
    ASSERT_TRUE(second.pub->try_send("b", sizeof("b"), 100));
    ASSERT_TRUE(set.wait(1000ms));
    const auto ready = set.consume_ready();
    ASSERT_EQ(ready.size(), 2u);
    EXPECT_FALSE(first.sub->recv(100).empty());
    EXPECT_FALSE(second.sub->recv(100).empty());
}

TEST(RecvWaitSet, SequenceAlreadyChangedDoesNotSleep)
{
    RoutePair pair{"prescan"};
    ipc::recv_wait_set set;
    if (!require_backend(set, pair.sub->read_wait_token())) { GTEST_SKIP(); return; }
    ASSERT_TRUE(pair.pub->try_send("ready", sizeof("ready"), 100));
    const auto begin = std::chrono::steady_clock::now();
    ASSERT_TRUE(set.wait(1000ms));
    EXPECT_LT(std::chrono::steady_clock::now() - begin, 200ms);
    ASSERT_EQ(set.consume_ready().size(), 1u);
}

TEST(RecvWaitSet, RemoveWakesBlockedWaitAndRemovesToken)
{
    RoutePair pair{"remove"};
    ipc::recv_wait_set set;
    if (!require_backend(set, pair.sub->read_wait_token())) { GTEST_SKIP(); return; }
    std::atomic<bool> returned{false};
    std::thread waiter([&] { set.wait(5000ms); returned.store(true); });
    std::this_thread::sleep_for(20ms);
    ASSERT_TRUE(set.remove(pair.sub->read_wait_token()));
    waiter.join();
    EXPECT_TRUE(returned.load());
    EXPECT_TRUE(set.consume_ready().empty());
}

TEST(RecvWaitSet, StopWakesBlockedWait)
{
    RoutePair pair{"stop"};
    ipc::recv_wait_set set;
    if (!require_backend(set, pair.sub->read_wait_token())) { GTEST_SKIP(); return; }
    std::atomic<bool> returned{false};
    std::thread waiter([&] { set.wait(5000ms); returned.store(true); });
    std::this_thread::sleep_for(20ms);
    set.stop();
    waiter.join();
    EXPECT_TRUE(returned.load());
}

TEST(RecvWaitSet, DisconnectWakesWait)
{
    RoutePair pair{"disconnect"};
    ipc::recv_wait_set set;
    if (!require_backend(set, pair.sub->read_wait_token())) { GTEST_SKIP(); return; }
    std::atomic<bool> returned{false};
    std::thread waiter([&] { set.wait(5000ms); returned.store(true); });
    std::this_thread::sleep_for(20ms);
    pair.sub->disconnect();
    waiter.join();
    EXPECT_TRUE(returned.load());
}

TEST(RecvWaitSet, CapacityIsBounded)
{
    ipc::recv_wait_set set;
    constexpr unsigned capacity =
#if defined(_WIN32)
        63;
#else
        127;
#endif
    std::vector<std::unique_ptr<RoutePair>> routes;
    routes.reserve(capacity + 1);
    for (unsigned i = 0; i < capacity + 1; ++i)
    {
        routes.emplace_back(std::make_unique<RoutePair>("capacity"));
        const bool added = set.add(routes.back()->sub->read_wait_token());
        if (i == 0 && !added)
            GTEST_SKIP() << "recv_wait_set backend unavailable on this platform/kernel";
        EXPECT_EQ(added, i < capacity);
        if (i == 0) ASSERT_TRUE(set.set_enabled(routes.front()->sub->read_wait_token(), false));
    }
    ASSERT_TRUE(routes.back() != nullptr);
    ASSERT_TRUE(set.set_enabled(routes.front()->sub->read_wait_token(), true));
    ASSERT_TRUE(routes.front()->pub->try_send("at-capacity", sizeof("at-capacity"), 100));
    ASSERT_TRUE(set.wait(1000ms));
    const auto ready = set.consume_ready();
    ASSERT_EQ(ready.size(), 1u);
    EXPECT_EQ(ready.front(), routes.front()->sub->read_wait_token());
}

TEST(RecvWaitSet, TimeoutReturnsFalse)
{
    RoutePair pair{"timeout"};
    ipc::recv_wait_set set;
    if (!require_backend(set, pair.sub->read_wait_token())) { GTEST_SKIP(); return; }
    EXPECT_FALSE(set.wait(20ms));
    EXPECT_TRUE(set.consume_ready().empty());
}

#if defined(__linux__)
TEST(RecvWaitSet, PublisherInAnotherProcessWakesWaiter)
{
    RoutePair pair{"cross_process"};
    int ready_pipe[2];
    ASSERT_EQ(::pipe(ready_pipe), 0);
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0)
    {
        ::close(ready_pipe[0]);
        ipc::route receiver(pair.name.c_str(), ipc::receiver, false);
        ipc::recv_wait_set set;
        if (!set.add(receiver.read_wait_token())) _exit(10);
        const char ready = 'R';
        if (::write(ready_pipe[1], &ready, 1) != 1) _exit(11);
        const bool woke = set.wait(3000ms);
        const auto tokens = set.consume_ready();
        const bool received = !receiver.recv(100).empty();
        _exit(woke && tokens.size() == 1 && received ? 0 : 12);
    }
    ::close(ready_pipe[1]);
    char ready = 0;
    ASSERT_EQ(::read(ready_pipe[0], &ready, 1), 1);
    ASSERT_EQ(ready, 'R');
    ASSERT_TRUE(pair.pub->try_send("cross-process", sizeof("cross-process"), 500));
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    ::close(ready_pipe[0]);
}
#endif

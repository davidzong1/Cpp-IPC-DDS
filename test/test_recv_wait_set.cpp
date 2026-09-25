#include <atomic>
#include <chrono>
#include <cstdint>
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

TEST(RecvWaitSet, OneRouteMessageProducesOneReadyToken)
{
    RoutePair pair{"one"};
    ipc::recv_wait_set set;
    if (!require_backend(set, pair.sub->read_wait_token())) { GTEST_SKIP(); return; }
    ASSERT_TRUE(pair.pub->try_send("hello", 100));
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
    ASSERT_TRUE(first.pub->try_send("a", 100));
    ASSERT_TRUE(second.pub->try_send("b", 100));
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
    ASSERT_TRUE(pair.pub->try_send("ready", 100));
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
    }
    ASSERT_TRUE(routes.back() != nullptr);
    ASSERT_TRUE(routes.front()->pub->try_send("at-capacity", 100));
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
    ASSERT_TRUE(pair.pub->try_send("cross-process", 500));
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    EXPECT_TRUE(WIFEXITED(status));
    EXPECT_EQ(WEXITSTATUS(status), 0);
    ::close(ready_pipe[0]);
}
#endif

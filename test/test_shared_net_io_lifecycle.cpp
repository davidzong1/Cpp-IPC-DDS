#include "dzIPC/net/datagram_endpoint.h"
#include "gtest/gtest.h"
#include <fcntl.h>
#include <future>
#include <thread>
using namespace dzIPC::net;
using namespace std::chrono_literals;
namespace
{
std::unique_ptr<DatagramEndpoint> endpoint()
{
    return std::make_unique<DatagramEndpoint>(Ipv4Address::parse("127.0.0.1", 0));
}
void send_many(DatagramEndpoint &tx, Ipv4Address to, unsigned n)
{
    const Bytes bytes{7};
    for (unsigned i = 0; i < n; ++i)
        ASSERT_EQ(tx.send(ByteView(bytes), to).count, 1u);
}
} // namespace
TEST(SharedNetIo, StopWakesBlockedWaitAndIsIdempotent)
{
    DatagramLoop loop;
    loop.add(endpoint());
    std::promise<void> started;
    auto waiter = std::async(std::launch::async, [&] {
        started.set_value();
        loop.run_once(5s, [](auto, const auto &) {});
    });
    started.get_future().wait();
    loop.request_stop();
    loop.request_stop();
    EXPECT_EQ(waiter.wait_for(500ms), std::future_status::ready);
    waiter.get();
    EXPECT_TRUE(loop.stopped());
}
TEST(SharedNetIo, PacketBudgetDefersWithoutNewNotification)
{
    IoBudget budget;
    budget.packets = 2;
    budget.time = 1s;
    DatagramLoop loop(budget);
    auto rx = endpoint();
    const auto address = rx->local_address();
    auto tx = endpoint();
    loop.add(std::move(rx));
    send_many(*tx, address, 7);
    unsigned seen = 0;
    const auto handler = [&](auto, const auto &p) {
        EXPECT_EQ(p.size, 1u);
        ++seen;
    };
    auto round = loop.run_once(500ms, handler);
    EXPECT_EQ(round.packets, 2u);
    EXPECT_TRUE(round.deferred);
    for (unsigned i = 0; i < 4 && seen < 7; ++i)
        loop.run_once(0ms, handler);
    EXPECT_EQ(seen, 7u);
    EXPECT_FALSE(loop.run_once(0ms, handler).deferred);
}
TEST(SharedNetIo, TimeBudgetRetainsAlreadyReceivedBatchSuffix)
{
    IoBudget budget;
    budget.time = 200us;
    budget.batch_max = 32;
    DatagramLoop loop(budget);
    auto rx = endpoint();
    const auto address = rx->local_address();
    auto tx = endpoint();
    loop.add(std::move(rx));
    send_many(*tx, address, 5);
    unsigned seen = 0;
    auto slow_handler = [&](auto, const auto &) {
        ++seen;
        std::this_thread::sleep_for(1ms);
    };
    for (unsigned i = 0; i < 5; ++i)
    {
        const auto round = loop.run_once(500ms, slow_handler);
        EXPECT_EQ(round.packets, 1u);
        EXPECT_TRUE(round.deferred);
    }
    EXPECT_EQ(seen, 5u);
    EXPECT_EQ(loop.run_once(0ms, slow_handler).packets, 0u);
}
TEST(SharedNetIo, RemoveBeforeCloseAndFdReuseDiscardOldDeferredWork)
{
    IoBudget budget;
    budget.packets = 1;
    budget.time = 1s;
    DatagramLoop loop(budget);
    auto tx = endpoint();
    auto rx = endpoint();
    const int fd = rx->native_handle();
    const auto address = rx->local_address();
    const auto old_token = loop.add(std::move(rx));
    send_many(*tx, address, 2);
    EXPECT_EQ(loop.run_once(500ms, [](auto, const auto &) {}).packets, 1u);
    ASSERT_TRUE(loop.remove(old_token));
    EXPECT_EQ(fcntl(fd, F_GETFD), -1);
    auto next = endpoint();
    EXPECT_EQ(next->native_handle(), fd);
    const auto next_address = next->local_address();
    const auto token = loop.add(std::move(next));
    EXPECT_NE(token, old_token);
    EXPECT_TRUE(loop.remove(old_token));
    EXPECT_EQ(
        loop.run_once(0ms, [](auto, const auto &) { ADD_FAILURE() << "旧事件错误投递"; }).packets,
        0u);
    send_many(*tx, next_address, 1);
    EXPECT_EQ(
        loop.run_once(500ms, [&](auto actual, const auto &) { EXPECT_EQ(actual, token); }).packets,
        1u);
}
TEST(SharedNetIo, ByteBudgetAndHotEndpointDoNotStarveOtherEndpoint)
{
    IoBudget budget;
    budget.bytes = 1;
    budget.time = 1s;
    DatagramLoop loop(budget);
    auto a = endpoint();
    auto b = endpoint();
    auto tx = endpoint();
    const auto aa = a->local_address(), ba = b->local_address();
    loop.add(std::move(a));
    loop.add(std::move(b));
    send_many(*tx, aa, 20);
    send_many(*tx, ba, 1);
    std::vector<DatagramLoop::Token> seen;
    const auto round =
        loop.run_once(500ms, [&](auto token, const auto &) { seen.push_back(token); });
    EXPECT_EQ(round.packets, 2u);
    ASSERT_EQ(seen.size(), 2u);
    EXPECT_NE(seen[0], seen[1]);
}
TEST(SharedNetIo, HandlerMayRemoveAllReadyEndpoints)
{
    DatagramLoop loop;
    auto a = endpoint();
    auto b = endpoint();
    auto tx = endpoint();
    const auto aa = a->local_address(), ba = b->local_address();
    const auto first = loop.add(std::move(a)), second = loop.add(std::move(b));
    send_many(*tx, aa, 1);
    send_many(*tx, ba, 1);
    unsigned calls = 0;
    loop.run_once(500ms, [&](auto, const auto &) {
        ++calls;
        EXPECT_TRUE(loop.remove(first));
        EXPECT_TRUE(loop.remove(second));
    });
    EXPECT_EQ(calls, 1u);
    EXPECT_EQ(loop.size(), 0u);
}
TEST(SharedNetIo, HandlerFailureKeepsBatchSuffixScheduled)
{
    DatagramLoop loop;
    auto rx = endpoint();
    const auto address = rx->local_address();
    auto tx = endpoint();
    loop.add(std::move(rx));
    send_many(*tx, address, 3);
    EXPECT_THROW(
        loop.run_once(500ms, [](auto, const auto &) { throw std::runtime_error("故障注入"); }),
        std::runtime_error);
    unsigned seen = 0;
    loop.run_once(0ms, [&](auto, const auto &) { ++seen; });
    EXPECT_EQ(seen, 2u);
}

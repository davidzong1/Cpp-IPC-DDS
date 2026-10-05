#include "shared_net/runtime_fixture.h"
#include "gtest/gtest.h"
#include <future>
#include <sys/wait.h>
using namespace shared_net_test;
namespace
{
struct FakeGateway
{
    local::Listener listener;
    std::future<void> worker;
    FakeGateway(const std::string &path,
                std::function<void(int, std::uint64_t, std::uint64_t)> action)
        : listener(path), worker(std::async(std::launch::async, [this, action] {
              if (!local::ready(listener.fd(), POLLIN, local::monotonic_ns() + 2000000000ull))
                  throw std::runtime_error("假网关等待超时");
              local::Fd fd(accept4(listener.fd(), nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK));
              if (!fd)
                  throw std::runtime_error("假网关接纳失败");
              receive(fd.get());
              WelcomeBody welcome;
              welcome.locality = local::locality();
              welcome.tx_name = outbox_name(welcome.locality, 123, 1);
              Bytes body;
              if (!encode_welcome(welcome, body))
                  throw std::runtime_error("假网关编码失败");
              transmit(fd.get(), encode(LocalKind::Welcome, 1, 1, 123, body));
              action(fd.get(), 1, 123);
          }))
    {
    }
    ~FakeGateway()
    {
        if (worker.valid()) worker.wait();
    }
};
} // namespace
TEST(SharedNetClient, OldCumulativeNotificationsAndRouteVersionAreIdempotent)
{
    Directory dir;
    Identity pub{};
    pub[0] = 3;
    std::promise<void> release;
    auto released = release.get_future();
    FakeGateway gateway(dir.control(), [&](int fd, auto session, auto epoch) {
        for (const auto value : {CreditCounters{200, 2}, CreditCounters{100, 2},
                                 CreditCounters{200, 1}, CreditCounters{200, 2}})
        {
            transmit(fd,
                     encode(LocalKind::CreditGrant, 0, session, epoch, encode_credit_grant(value)));
            transmit(fd,
                     encode(LocalKind::TxProgress, 0, session, epoch, encode_tx_progress(value)));
        }
        for (const auto version : {2u, 1u})
        {
            RouteStateBody state;
            state.publisher_id = pub;
            state.state_version = version;
            state.remote_ready_count = version;
            state.synchronized = true;
            Bytes b;
            if (!encode_route_state(state, b))
                throw std::runtime_error("路由编码失败");
            transmit(fd, encode(LocalKind::RouteState, 0, session, epoch, b));
        }
        released.wait_for(2s);
    });
    auto client = ClientRuntime::acquire(dir.control());
    EXPECT_TRUE(until([&] { return client->route_state(pub).state_version == 2; }));
    EXPECT_EQ(client->route_state(pub).remote_ready_count, 2u);
    EXPECT_EQ(client->granted().bytes, 200u);
    EXPECT_EQ(client->released().records, 2u);
    EXPECT_TRUE(client->healthy());
    release.set_value();
    gateway.worker.get();
}
TEST(SharedNetClient, UnexpectedReplyFailsPendingAndReliableWaiters)
{
    Directory dir;
    FakeGateway gateway(dir.control(), [](int fd, auto session, auto epoch) {
        auto packet = receive(fd);
        LocalHeader header;
        ByteView body;
        if (!decode_local(ByteView(packet.bytes), header, body))
            throw std::runtime_error("假网关解码失败");
        transmit(fd, encode(LocalKind::Pong, header.request_id, session, epoch, Bytes(8)));
    });
    auto client = ClientRuntime::acquire(dir.control());
    Identity pub{};
    pub[0] = 1;
    const auto ticket = client->prepare_send(pub, 1);
    EXPECT_THROW(client->status(), std::runtime_error);
    EXPECT_FALSE(client->healthy());
    EXPECT_EQ(client->wait_send(ticket, local::monotonic_ns()).result, SendResultCode::GatewayLost);
    gateway.worker.get();
}
TEST(SharedNetClient, PendingRequestDeadlineFailsSession)
{
    Directory dir;
    std::promise<void> release;
    auto released = release.get_future();
    FakeGateway gateway(dir.control(), [&](int fd, auto, auto) {
        receive(fd);
        released.wait_for(2s);
    });
    auto client = ClientRuntime::acquire(dir.control());
    EXPECT_THROW(client->request(LocalKind::QueryState, Bytes{0}, 20ms), std::runtime_error);
    EXPECT_FALSE(client->healthy());
    release.set_value();
    gateway.worker.get();
}
TEST(SharedNetClient, ThousandHandlesShareOneConnectionAndConcurrentRequests)
{
    Directory dir;
    GatewayRuntime gateway(configuration(dir));
    auto client = ClientRuntime::acquire(dir.control());
    const auto before = fd_count();
    std::vector<std::shared_ptr<ClientRuntime>> handles;
    for (unsigned i = 0; i < 1000; ++i)
        handles.push_back(ClientRuntime::acquire(dir.control()));
    EXPECT_EQ(fd_count(), before);
    for (const auto &h : handles)
        EXPECT_EQ(h.get(), client.get());
    EXPECT_NE(client->status().find("\"client_sessions\":1"), std::string::npos);
    EXPECT_THROW(ClientRuntime::acquire(dir.path + "/other.sock"), std::runtime_error);
    std::vector<std::future<std::string>> requests;
    for (unsigned i = 0; i < 32; ++i)
        requests.push_back(std::async(std::launch::async, [&] { return client->status(); }));
    for (auto &request : requests)
        EXPECT_NE(request.get().find("ControlReady"), std::string::npos);
    EXPECT_THROW(client->request(LocalKind::CreditRequest, Bytes{}), std::runtime_error);
    EXPECT_TRUE(client->healthy());
}
TEST(SharedNetClient, CreditsReplayQuotaAndZeroInitialGrant)
{
    Directory dir;
    auto c = configuration(dir);
    c.limits.initial_send_bytes = c.limits.initial_send_records = 0;
    GatewayRuntime gateway(c);
    RawClient client(dir.control());
    EXPECT_EQ(client.welcome.granted_bytes, 0u);
    EXPECT_EQ(client.welcome.granted_records, 0u);
    Bytes body;
    codec::append(body, 8192, 4);
    codec::append(body, 1, 4);
    const auto packet = client.request(LocalKind::CreditRequest, 2, body);
    transmit(client.fd.get(), packet);
    const auto first = receive(client.fd.get()).bytes;
    transmit(client.fd.get(), packet);
    EXPECT_EQ(receive(client.fd.get()).bytes, first);
    LocalHeader h;
    ByteView b;
    ASSERT_TRUE(decode_local(ByteView(first), h, b));
    CreditCounters credit;
    ASSERT_TRUE(decode_credit_grant(b, credit));
    EXPECT_EQ(credit.bytes, 8192u);
    EXPECT_EQ(credit.records, 1u);
    body[0] ^= 1;
    transmit(client.fd.get(), client.request(LocalKind::CreditRequest, 2, body));
    auto conflict = receive(client.fd.get());
    ASSERT_TRUE(decode_local(ByteView(conflict.bytes), h, b));
    EXPECT_EQ(h.kind, LocalKind::Error);
    body.clear();
    codec::append(body, c.limits.session_send_bytes, 4);
    codec::append(body, 1, 4);
    transmit(client.fd.get(), client.request(LocalKind::CreditRequest, 3, body));
    auto busy = receive(client.fd.get());
    ASSERT_TRUE(decode_local(ByteView(busy.bytes), h, b));
    EXPECT_EQ(h.kind, LocalKind::Error);
    client.fd.reset();
    ASSERT_TRUE(until([&] {
        return gateway.status_json().find("\"allocated_send_bytes\":\"0\"") != std::string::npos;
    }));
}
TEST(SharedNetClient, WrongLocalityAndClockAreRejectedAndAncillaryFdClosed)
{
    Directory dir;
    GatewayRuntime gateway(configuration(dir));
    for (bool wrong_clock : {false, true})
    {
        auto fd = local::connect_control(dir.control(), local::monotonic_ns() + 2000000000ull);
        auto id = local::locality();
        if (!wrong_clock)
            id[0] ^= 1;
        transmit(fd.get(), hello(id, local::clock_domain(getpid()) + (wrong_clock ? 1 : 0)));
        EXPECT_THROW(receive(fd.get()), std::runtime_error);
    }
    const auto before = fd_count();
    {
        RawClient client(dir.control());
        auto event = local::event();
        auto packet = client.request(LocalKind::Ping, 2, Bytes(8));
        ASSERT_TRUE(local::send(client.fd.get(), ByteView(packet), event.get()));
        EXPECT_THROW(receive(client.fd.get()), std::runtime_error);
    }
    ASSERT_TRUE(until([&] { return fd_count() == before; }));
}
TEST(SharedNetClient, EarlyResultWrongIdentityTimeoutDisconnectAndLegAggregation)
{
    SendWaitTable table(2);
    Identity pub{};
    pub[0] = 7;
    auto ticket = table.insert(1, pub, 1);
    SendResultBody result;
    result.publisher_id = pub;
    result.sequence = 2;
    EXPECT_FALSE(table.complete(1, result));
    result.sequence = 1;
    result.target_count = result.acked_count = 1;
    EXPECT_TRUE(table.complete(1, result));
    EXPECT_FALSE(table.complete(1, result));
    EXPECT_EQ(table.wait(ticket, local::monotonic_ns()).result, SendResultCode::Completed);
    EXPECT_TRUE(reliable_result(SubmitState::NotRequired, result));
    EXPECT_FALSE(reliable_result(SubmitState::NotSubmitted, result));
    result.result = SendResultCode::NoSubscribers;
    EXPECT_TRUE(reliable_result(SubmitState::Committed, result));
    EXPECT_FALSE(reliable_result(SubmitState::NotRequired, result));
    auto timeout = table.insert(2, pub, 2);
    EXPECT_EQ(table.wait(timeout, local::monotonic_ns()).result, SendResultCode::TimedOut);
    auto lost = table.insert(3, pub, 3);
    table.disconnect();
    result = table.wait(lost, local::monotonic_ns());
    EXPECT_EQ(result.result, SendResultCode::GatewayLost);
    EXPECT_FALSE(reliable_result(SubmitState::Committed, result));
    for (auto a : {SubmitState::NotRequired, SubmitState::NotSubmitted, SubmitState::Committed,
                   SubmitState::Indeterminate})
        for (auto b : {SubmitState::NotRequired, SubmitState::NotSubmitted, SubmitState::Committed,
                       SubmitState::Indeterminate})
            EXPECT_EQ(best_effort_result(a, b),
                      a == SubmitState::Committed || b == SubmitState::Committed ||
                          a == SubmitState::Indeterminate || b == SubmitState::Indeterminate);
}
TEST(SharedNetClient, GatewayLossWakesAllReliableWaitersAndNewSessionUsesNewEpoch)
{
    Directory dir;
    auto c = configuration(dir);
    GatewayRuntime gateway(c);
    auto old = ClientRuntime::acquire(dir.control());
    Identity pub{};
    pub[0] = 1;
    std::vector<SendTicket> tickets;
    for (unsigned i = 1; i <= 128; ++i)
        tickets.push_back(old->prepare_send(pub, i));
    gateway.stop();
    ASSERT_TRUE(until([&] { return !old->healthy(); }));
    for (const auto &ticket : tickets)
        EXPECT_EQ(old->wait_send(ticket, local::monotonic_ns()).result,
                  SendResultCode::GatewayLost);
    GatewayRuntime next(c);
    auto current = ClientRuntime::acquire(dir.control());
    EXPECT_NE(current.get(), old.get());
    EXPECT_NE(current->gateway_epoch(), old->gateway_epoch());
    EXPECT_FALSE(old->healthy());
    std::thread a([&] { old->stop(); }), b([&] { old->stop(); });
    a.join();
    b.join();
}
TEST(SharedNetClient, ForkRejectsBeforeInheritedLocks)
{
    Directory dir;
    GatewayRuntime gateway(configuration(dir));
    auto client = ClientRuntime::acquire(dir.control());
    const auto pid = fork();
    ASSERT_GE(pid, 0);
    if (!pid)
    {
        bool acquire_failed = false, request_failed = false;
        try
        {
            ClientRuntime::acquire(dir.control());
        }
        catch (...)
        {
            acquire_failed = true;
        }
        try
        {
            client->status();
        }
        catch (...)
        {
            request_failed = true;
        }
        client.reset();
        _exit(acquire_failed && request_failed ? 0 : 1);
    }
    int state = 0;
    ASSERT_EQ(waitpid(pid, &state, 0), pid);
    EXPECT_TRUE(WIFEXITED(state));
    EXPECT_EQ(WEXITSTATUS(state), 0);
    EXPECT_TRUE(client->healthy());
}
TEST(SharedNetClient, SlowReaderDoesNotBlockOtherSessionsOrStop)
{
    Directory dir;
    GatewayRuntime gateway(configuration(dir));
    RawClient slow(dir.control());
    int size = 1024;
    setsockopt(slow.fd.get(), SOL_SOCKET, SO_RCVBUF, &size, sizeof(size));
    for (std::uint64_t id = 2; id < 10000; ++id)
    {
        try
        {
            const auto p = slow.request(LocalKind::QueryState, id, Bytes{0});
            if (!local::send(slow.fd.get(), ByteView(p)))
                break;
        }
        catch (...)
        {
            break;
        }
    }
    auto fast = ClientRuntime::acquire(dir.control());
    EXPECT_NE(fast->status().find("ControlReady"), std::string::npos);
    auto stopped = std::async(std::launch::async, [&] { gateway.stop(); });
    EXPECT_EQ(stopped.wait_for(1s), std::future_status::ready);
    stopped.get();
}

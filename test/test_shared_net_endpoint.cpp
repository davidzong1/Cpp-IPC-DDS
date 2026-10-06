#include "dzIPC/common/channel_scope.h"
#include "dzIPC/net/datagram_endpoint.h"
#include "gtest/gtest.h"
#include <arpa/inet.h>
#include <fcntl.h>
#include <filesystem>
#include <sys/socket.h>
#include <system_error>
#include <unistd.h>
using namespace dzIPC::net;
namespace
{
Ipv4Address loopback()
{
    return Ipv4Address::parse("127.0.0.1", 0);
}
std::size_t sockets()
{
    std::size_t n = 0;
    for (const auto &entry : std::filesystem::directory_iterator("/proc/self/fd"))
    {
        std::error_code error;
        const auto link = std::filesystem::read_symlink(entry.path(), error).string();
        if (!error && link.compare(0, 8, "socket:[") == 0)
            ++n;
    }
    return n;
}
} // namespace
TEST(SharedNetEndpoint, NonblockingEmptyDatagramSourceAndExclusiveBind)
{
    DatagramEndpoint tx(loopback()), rx(loopback());
    EXPECT_TRUE(fcntl(rx.native_handle(), F_GETFL) & O_NONBLOCK);
    EXPECT_TRUE(fcntl(rx.native_handle(), F_GETFD) & FD_CLOEXEC);
    EXPECT_GT(rx.receive_buffer_bytes(), 0);
    EXPECT_GT(tx.send_buffer_bytes(), 0);
    EXPECT_THROW(DatagramEndpoint second(rx.local_address()), std::system_error);
    ReceivedDatagram data;
    EXPECT_EQ(rx.receive(data).status, IoStatus::WouldBlock);
    ASSERT_EQ(tx.send({}, rx.local_address()).status, IoStatus::Data);
    ASSERT_EQ(rx.receive(data).status, IoStatus::Data);
    EXPECT_EQ(data.size, 0u);
    EXPECT_EQ(data.source, tx.local_address());
    const Bytes payload{1, 2, 3};
    ASSERT_EQ(tx.send(ByteView(payload), rx.local_address()).count, 1u);
    ASSERT_EQ(rx.receive(data).status, IoStatus::Data);
    EXPECT_EQ(data.size, payload.size());
    EXPECT_EQ(Bytes(data.bytes.begin(), data.bytes.begin() + data.size), payload);
}
TEST(SharedNetEndpoint, TruncationIsNotAnEmptyOrValidPacket)
{
    DatagramEndpoint tx(loopback()), rx(loopback());
    Bytes oversized(2048, 0x5a);
    sockaddr_in destination{};
    destination.sin_family = AF_INET;
    destination.sin_addr.s_addr = htonl(rx.local_address().host);
    destination.sin_port = htons(rx.local_address().port);
    ASSERT_EQ(sendto(tx.native_handle(), oversized.data(), oversized.size(), 0,
                     reinterpret_cast<sockaddr *>(&destination), sizeof(destination)),
              2048);
    ReceivedDatagram data;
    const auto result = rx.receive(data);
    EXPECT_EQ(result.status, IoStatus::Truncated);
    EXPECT_EQ(result.count, 1u);
    EXPECT_EQ(data.size, 0u);
    EXPECT_EQ(rx.receive(data).status, IoStatus::WouldBlock);
}
TEST(SharedNetEndpoint, BatchesAndPartialSendExposeOnlyAcceptedPrefix)
{
    DatagramEndpoint tx(loopback()), rx(loopback());
    Bytes payload{42};
    std::array<OutgoingDatagram, 32> outgoing;
    for (auto &item : outgoing)
        item = {rx.local_address(), ByteView(payload)};
    EXPECT_EQ(tx.send_batch(outgoing.data(), outgoing.size()).count, 32u);
    std::array<ReceivedDatagram, 64> incoming;
    EXPECT_EQ(rx.receive_batch(incoming.data(), incoming.size()).count, 32u);
    EXPECT_EQ(rx.receive_batch(incoming.data(), 1).status, IoStatus::WouldBlock);
    outgoing[1].destination = {0xffffffffu,
                               rx.local_address().port}; // 未开启 SO_BROADCAST，第二包确定失败
    auto partial = tx.send_batch(outgoing.data(), 2);
    ASSERT_EQ(partial.status, IoStatus::Data);
    ASSERT_EQ(partial.count, 1u);
    EXPECT_EQ(tx.send_batch(outgoing.data() + partial.count, 2 - partial.count).status,
              IoStatus::Fatal);
    EXPECT_EQ(rx.receive_batch(incoming.data(), 64).count, 1u);
    EXPECT_EQ(rx.receive_batch(incoming.data(), 64).status, IoStatus::WouldBlock);
}
TEST(SharedNetEndpoint, HundredDestinationsReuseOneSocket)
{
    const auto before = sockets();
    {
        DatagramEndpoint tx(loopback());
        const int fd = tx.native_handle();
        const Bytes payload{1};
        for (unsigned i = 2; i <= 101; ++i)
        {
            EXPECT_EQ(tx.send(ByteView(payload),
                              Ipv4Address::parse("127.0.0." + std::to_string(i), 29000))
                          .count,
                      1u);
            EXPECT_EQ(tx.native_handle(), fd);
        }
        EXPECT_EQ(sockets(), before + 1);
    }
    EXPECT_EQ(sockets(), before);
}
TEST(SharedNetEndpoint, FixedKPlusTwoAndCrossShardConfigurations)
{
    const auto before = sockets();
    for (std::uint16_t k : {1, 4, 16})
    {
        GatewayConfig c;
        c.listen_ip = "127.0.0.1";
        c.interface = "lo";
        c.control_path = "/tmp/unused-shared-net-check.sock";
        c.data_shards = k;
        std::vector<std::unique_ptr<DatagramEndpoint>> endpoints;
        for (unsigned attempt = 0; attempt < 64 && endpoints.empty(); ++attempt)
        {
            c.data_base_port = 20000 + ((getpid() + attempt) % 1000) * 32;
            c.control_port = c.data_base_port + k;
            c.discovery_port = c.control_port + 1;
            try
            {
                endpoints = open_gateway_endpoints(c);
            }
            catch (const std::system_error &e)
            {
                if (e.code().value() != EADDRINUSE)
                    throw;
            }
        }
        ASSERT_EQ(endpoints.size(), k + 2u);
        EXPECT_EQ(sockets(), before + k + 2u);
        EXPECT_THROW(open_gateway_endpoints(c), std::system_error);
        EXPECT_EQ(sockets(), before + k + 2u);
        for (unsigned topic = 0; topic < 1000; ++topic)
        {
            RouteKey route{dzIPC::common::channel_scope_token("/topic/" + std::to_string(topic), 7,
                                                              dzIPC::common::ScopeKind::PubSub),
                           topic};
            for (std::uint16_t peer_k : {1, 4, 16})
            {
                const auto port = data_port(route, 24000, peer_k);
                EXPECT_EQ(port, 24000 + route_hash(route) % peer_k);
                EXPECT_TRUE(matches_data_shard(route, {0x7f000001, port}, 24000, peer_k));
                EXPECT_FALSE(matches_data_shard(
                    route, {0x7f000001, static_cast<std::uint16_t>(port + 1)}, 24000, peer_k));
            }
        }
        EXPECT_EQ(sockets(), before + k + 2u);
    }
    EXPECT_EQ(sockets(), before);
}
#if defined(__linux__)
TEST(SharedNetEndpoint, ResourceBudgetRejectsBeforeOpeningSockets)
{
    GatewayConfig c;
    c.listen_ip = "127.0.0.1";
    c.interface = "lo";
    c.control_path = "/tmp/unused-shared-net-budget.sock";
    c.data_socket_cap = 1;
    c.data_shards = 4;
    EXPECT_EQ(validate_config(c).code, ConfigCode::SocketCap);
    c = GatewayConfig{};
    c.listen_ip = "127.0.0.1";
    c.interface = "lo";
    c.control_path = "/tmp/unused-shared-net-budget.sock";
    c.data_port_range = "24000:24003";
    c.data_base_port = 24000;
    c.control_port = 24004;
    c.discovery_port = 24005;
    c.socket_buffer_budget_bytes = 1;
    const auto before = sockets();
    EXPECT_THROW(open_gateway_endpoints(c), ConfigError);
    EXPECT_EQ(sockets(), before);
}
#endif

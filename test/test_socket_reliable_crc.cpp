#include <cstdint>
#include <memory>
#include <string>
#include "dzIPC/common/crc32c.h"
#include "dzIPC/common/data_rev.h"
#include "dzIPC/common/hash.h"
#include "ipc_msg/test_msg2/test_msg.hpp"

#include <gtest/gtest.h>

namespace {

std::shared_ptr<ipc::socket::UDPNode> make_node(const std::string& name)
{
    const std::string ip = dzIPC::common::udp_discovery_addr_calculate(name);
    const uint16_t port = dzIPC::common::udp_discovery_port_calculate(name, 1);
    auto node = std::make_shared<ipc::socket::UDPNode>(name.c_str(), ip.c_str(), port);
    if (!node->connect())
    {
        return {};
    }
    return node;
}

ipc::buffer make_large_message()
{
    dzIPC::Msg::TestMsg msg;
    msg.data1.assign(256, 1.25);
    msg.data2.assign(256, 42);
    msg.data3.assign(64, std::string(64, 'x'));
    msg.data4 = true;
    return msg.serialize();
}

}   // namespace

TEST(Crc32c, KnownVector)
{
    const char input[] = "123456789";
    EXPECT_EQ(dzIPC::common::crc32c(input, 9), 0xE3069283u);
}

TEST(SocketReliable, NoAckFails)
{
    auto node = make_node("socket_reliable_no_ack");
    if (!node)
    {
        GTEST_SKIP() << "UDP multicast socket is not available in this environment";
    }

    ipc::buffer data = make_large_message();
    ASSERT_GT(data.size(), 1472u);

    dzIPC::socket::SocketSendOptions options;
    options.delivery = dzIPC::socket::SocketDeliveryMode::Reliable;
    options.integrity = dzIPC::socket::SocketIntegrityMode::CRC32C;
    options.ack_timeout_ms = 30;

    const dzIPC::socket::SocketSendReport report = dzIPC::socket::chunk_send_ex(node, data, options);
    EXPECT_EQ(report.status, dzIPC::socket::SocketSendStatus::FailedTimeout);
    EXPECT_NE(report.crc32c, 0u);
}

TEST(SocketBestEffort, NoAckIsUnconfirmedSuccess)
{
    auto node = make_node("socket_best_effort_no_ack");
    if (!node)
    {
        GTEST_SKIP() << "UDP multicast socket is not available in this environment";
    }

    ipc::buffer data = make_large_message();
    ASSERT_GT(data.size(), 1472u);

    dzIPC::socket::SocketSendOptions options;
    options.delivery = dzIPC::socket::SocketDeliveryMode::BestEffort;
    options.integrity = dzIPC::socket::SocketIntegrityMode::None;

    const dzIPC::socket::SocketSendReport report = dzIPC::socket::chunk_send_ex(node, data, options);
    EXPECT_EQ(report.status, dzIPC::socket::SocketSendStatus::SentUnconfirmed);
}

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

/* 发送端上报的 CRC 必须等于**线上真实字节**的 CRC。
 *
 * 这条不变量看起来是废话, 但它曾经被违反过, 且后果是 Reliable + CRC32C 的多页
 * 消息 100% 报 FailedIntegrity:
 *
 *   chunk_send_ex 把 publish_data 切成若干 chunk —— chunk 是 publish_data 的
 *   **非拥有视图**, 随后 write_now_page 就地改写每片 tail 里的 now_page 字段
 *   (IpcMsgBase::adapt_memcpy_tos 是先 ++page 再 add_tail_msg, 写出来的页号
 *   差一, write_now_page 的职责正是纠正它)。若 CRC 在纠正之前算, 得到的就是
 *   一份"从未上过线"的字节流的校验和。
 *
 *   接收端 place_page 把整片(含 tail)原样拼进 assembled 再 CRC, 拿到的是线上
 *   真实字节。两者在每片 2 个字节上不同, N 片差 N-1 处。
 *
 * 这个错位长期不可见: 端点分离之前 ACK 根本到不了发送端, 代码在比较 CRC 之前
 * 就 FailedTimeout 返回了; 单页消息也不受影响(page 从未自增, 无需纠正)。
 *
 * 这里利用"chunk_send_ex 就地改写入参"这一点: 调用返回后 data 里就是线上字节,
 * 直接与 report.crc32c 比对即可。不依赖页号是否差一 —— 哪天 adapt_memcpy_tos
 * 被修正了, 本用例依然成立。 */
TEST(SocketReliable, ReportedCrcMatchesWireBytes)
{
    auto node = make_node("socket_crc_wire_match");
    if (!node)
    {
        GTEST_SKIP() << "UDP multicast socket is not available in this environment";
    }

    ipc::buffer data = make_large_message();
    ASSERT_GT(data.size(), 1472u) << "必须多页, 单页不会触发页号纠正";

    dzIPC::socket::SocketSendOptions options;
    options.delivery = dzIPC::socket::SocketDeliveryMode::Reliable;
    options.integrity = dzIPC::socket::SocketIntegrityMode::CRC32C;
    options.ack_timeout_ms = 30;   // 无订阅者, 必然超时 —— 但 CRC 与改写都已发生

    const dzIPC::socket::SocketSendReport report = dzIPC::socket::chunk_send_ex(node, data, options);

    const uint32_t wire_crc = dzIPC::common::crc32c(data.data(), data.size());
    EXPECT_EQ(report.crc32c, wire_crc)
        << "发送端 CRC 与线上字节不符 —— 多半是 CRC 算在了 write_now_page 之前";
}

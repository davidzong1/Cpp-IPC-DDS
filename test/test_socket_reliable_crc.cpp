#include <cstdint>
#include <memory>
#include <string>
#include <algorithm>
#include <cstring>
#include "ipc_msg/std_msgs/std_image.hpp"
#include "ipc_msg/ipc_msg_base/udp_rtps_ack_msg.hpp"
#include "dzIPC/common/topic_data.h"
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


/* 有意漏掉 A 的后续页，在 A 的 final HB 后排入同尺寸 B。
 * A 的缺页不能由 B 填满；允许丢弃 A，但不得把混合字节交给应用。 */
TEST(SocketBestEffort, FinalHeartbeatCannotMixFollowingMessagePages)
{
    const std::string name="socket_best_effort_frame_boundary";
    const auto ip=dzIPC::common::udp_discovery_addr_calculate(name);
    const auto port=dzIPC::common::udp_discovery_port_calculate(name, 185);
    auto rx=std::make_shared<ipc::socket::UDPNode>("boundary_rx",ip.c_str(),port,ipc::socket::NodeRole::RecvOnly);
    auto tx=std::make_shared<ipc::socket::UDPNode>("boundary_tx",ip.c_str(),port,ipc::socket::NodeRole::SendOnly);
    ASSERT_TRUE(rx->connect());
    ASSERT_TRUE(tx->connect());
    dzIPC::Msg::StdImage a,b;
    a.set_msg_id(77);b.set_msg_id(77);
    a.width=1;b.width=2;
    a.data.assign(8192,0x11);b.data.assign(8192,0x22);
    auto wa=a.serialize(), wb=b.serialize();
    constexpr std::size_t mtu=1472;
    auto send_page=[&](ipc::buffer& wire,std::size_t offset) {
        const auto n=std::min(mtu,wire.size()-offset);
        auto* ptr=static_cast<std::uint8_t*>(wire.data())+offset;
        const std::uint16_t page=offset/mtu+1;
        ptr[n-10]=page>>8;ptr[n-9]=page&255;
        ipc::buffer fragment(ptr,n);
        return tx->send(fragment);
    };
    ASSERT_TRUE(send_page(wa,0));
    IpcRtpsHeartbeatMsg hb;
    hb.page_cnt=(wa.size()+mtu-1)/mtu;hb.total_size=wa.size();hb.data_msg_id=77;
    hb.sequence=1;hb.flags=IpcRtpsHeartbeatMsg::kFlagFinal;
    auto control=hb.serialize();
    ASSERT_TRUE(tx->send(control));
    for(std::size_t offset=0;offset<wb.size();offset+=mtu) ASSERT_TRUE(send_page(wb,offset));
    auto result=std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(),77);
    const bool first=dzIPC::socket::chunk_rev_topic(rx,result,100,tx);
    EXPECT_FALSE(first) << "已终止的不完整 A 不得借用 B 的分片交付";
    ASSERT_TRUE(dzIPC::socket::chunk_rev_topic(rx,result,100,tx));
    auto received=std::static_pointer_cast<dzIPC::Msg::StdImage>(result->topic());
    EXPECT_EQ(received->width,2u);
    EXPECT_EQ(received->data,b.data);
}

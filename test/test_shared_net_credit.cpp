#include "shared_net/peer_stub.h"
#include "shared_net/shm_wire_fixture.h"
#include "dzIPC/net/publisher_endpoint.h"
#include "ipc_msg/std_msgs/std_image.hpp"
#include "gtest/gtest.h"
using namespace shared_net_test;
TEST(SharedNetCredit, ProgressDoesNotRegrantPayloadWhileAckOutstanding) {
    BusinessTopic topic; Directory dir; auto config = configuration(dir); GatewayRuntime gateway(config);
    auto client = ClientRuntime::acquire(dir.control()); PublisherEndpoint publisher(client, topic.descriptor);
    PeerStub peer; peer.announce(config, topic.descriptor, client, publisher.publisher_id());
    const auto granted = client->granted(), released = client->released(); const auto bytes = flat_blob(1024);
    auto future = std::async(std::launch::async, [&] { return publisher.prebuilt(ByteView(bytes), Delivery::Reliable, 1000); });
    ReceivedDatagram packet; ASSERT_TRUE(until([&] { return peer.data.receive(packet).status == IoStatus::Data; }));
    ASSERT_TRUE(until([&] { return client->released().records > released.records; }));
    EXPECT_EQ(client->granted().bytes, granted.bytes); EXPECT_EQ(client->granted().records, granted.records);
    EXPECT_EQ(future.wait_for(0ms), std::future_status::timeout);
    WireHeader header; ByteView payload; ASSERT_TRUE(decode_packet(packet.view(), header, payload));
    peer.reply(config, header, PacketKind::Ack);
    ASSERT_EQ(future.wait_for(500ms), std::future_status::ready); auto result = future.get();
    EXPECT_TRUE(result.success); EXPECT_EQ(result.remote.target_count, 1u); EXPECT_EQ(result.remote.acked_count, 1u);
    ASSERT_TRUE(until([&] { return client->granted().records == granted.records + 1; }));
    EXPECT_EQ(client->granted().bytes, granted.bytes + 1024); EXPECT_EQ(client->granted().records, granted.records + 1);
    EXPECT_NE(client->status().find("\"credit_requests\":\"0\""), std::string::npos);
}
TEST(SharedNetCredit, NoSubscribersAndGatewayLossHaveDifferentLocalMerge) {
    BusinessTopic topic; Directory dir; auto config = configuration(dir); GatewayRuntime gateway(config);
    auto client = ClientRuntime::acquire(dir.control()); PublisherEndpoint publisher(client, topic.descriptor);
    const auto bytes = flat_blob(64);
    auto none = publisher.prebuilt(ByteView(bytes), Delivery::Reliable, 1000);
    EXPECT_FALSE(none.success); EXPECT_EQ(none.remote.result, SendResultCode::NoSubscribers);
    ipc::mpmc_channel receiver(topic.segment().c_str(), ipc::receiver, false);
    auto local = publisher.prebuilt(ByteView(bytes), Delivery::Reliable, 1000);
    EXPECT_TRUE(local.success); EXPECT_EQ(local.remote.result, SendResultCode::NoSubscribers);
    auto sample = receiver.try_recv(); ASSERT_EQ(sample.size(), bytes.size()); EXPECT_EQ(std::memcmp(sample.data(), bytes.data(), bytes.size()), 0);
    gateway.stop(); ASSERT_TRUE(until([&] { return !client->healthy(); }));
    auto failed = publisher.prebuilt(ByteView(bytes), Delivery::Reliable, 1000);
    EXPECT_FALSE(failed.success); EXPECT_EQ(failed.local, SubmitState::Committed); EXPECT_EQ(failed.remote.result, SendResultCode::GatewayLost);
    sample = receiver.try_recv(); ASSERT_EQ(sample.size(), bytes.size()); EXPECT_EQ(std::memcmp(sample.data(), bytes.data(), bytes.size()), 0);
}
TEST(SharedNetCredit, TimedOutWithoutAckReleasesCreditAndReportsPossibleDelivery) {
    BusinessTopic topic; Directory dir; auto config = configuration(dir); GatewayRuntime gateway(config);
    auto client = ClientRuntime::acquire(dir.control()); PublisherEndpoint publisher(client, topic.descriptor);
    PeerStub peer; peer.announce(config, topic.descriptor, client, publisher.publisher_id());
    const auto granted = client->granted(); const auto bytes = flat_blob(64);
    auto result = publisher.prebuilt(ByteView(bytes), Delivery::Reliable, 30);
    EXPECT_FALSE(result.success); EXPECT_EQ(result.remote.result, SendResultCode::TimedOut); EXPECT_TRUE(result.remote.possible_remote_delivery); EXPECT_EQ(result.remote.acked_count, 0u);
    EXPECT_TRUE(until([&] { return client->granted().records == granted.records + 1; }));
    EXPECT_EQ(client->granted().bytes, granted.bytes + 64);
}
TEST(SharedNetCredit, SlowEncoderCannotResetDeadlineOrSubmitAfterExpiration) {
    struct SlowImage : dzIPC::Msg::StdImage {
        bool dzflat_write(void* data, std::uint32_t size) const override {
            std::this_thread::sleep_for(20ms); return StdImage::dzflat_write(data, size);
        }
    } message;
    BusinessTopic topic; Directory dir; auto config = configuration(dir); GatewayRuntime gateway(config);
    auto client = ClientRuntime::acquire(dir.control()); PublisherEndpoint publisher(client, topic.descriptor);
    ipc::mpmc_channel receiver(topic.segment().c_str(), ipc::receiver, false);
    message.set_msg_id(71); message.data.assign(32, 0x77);
    const auto result = publisher.publish(message, Delivery::Reliable, 1);
    EXPECT_FALSE(result.success); EXPECT_EQ(result.local, SubmitState::NotSubmitted); EXPECT_EQ(result.network, SubmitState::NotSubmitted); EXPECT_EQ(result.remote.result, SendResultCode::TimedOut);
    EXPECT_TRUE(receiver.try_recv().empty()); EXPECT_NE(client->status().find("\"outbox_records\":\"0\""), std::string::npos);
}
TEST(SharedNetCredit, ReplayedOutboxSequenceDoesNotStartAnotherTransmission) {
    BusinessTopic topic; Directory dir; auto config = configuration(dir); GatewayRuntime gateway(config);
    auto client = ClientRuntime::acquire(dir.control()); PublisherEndpoint publisher(client, topic.descriptor);
    PeerStub peer; peer.announce(config, topic.descriptor, client, publisher.publisher_id());
    const auto bytes = flat_blob(64); const auto sent = publisher.prebuilt(ByteView(bytes)); ASSERT_EQ(sent.network, SubmitState::Committed);
    ReceivedDatagram first; ASSERT_TRUE(until([&] { return peer.data.receive(first).status == IoStatus::Data; }));
    OutboxHeader h; h.session_id = client->session_id(); h.gateway_epoch = client->gateway_epoch(); h.publisher_id = publisher.publisher_id();
    h.sequence = sent.sequence; h.route = topic.descriptor.key; h.schema_hash = 0xabcdef01; h.encoding = Encoding::DzFlat;
    ASSERT_EQ(client->submit_outbox(h, ByteView(bytes)).state, SubmitState::Committed);
    ASSERT_TRUE(until([&] { return client->status().find("\"outbox_records\":\"2\"") != std::string::npos; }));
    ReceivedDatagram duplicate; EXPECT_EQ(peer.data.receive(duplicate).status, IoStatus::WouldBlock);
}

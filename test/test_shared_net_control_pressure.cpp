#include "shared_net/peer_stub.h"
#include "shared_net/shm_wire_fixture.h"
#include "dzIPC/net/publisher_endpoint.h"
#include "gtest/gtest.h"
using namespace shared_net_test;
TEST(SharedNetControlPressure, InvalidAndDuplicateAcksDoNotBlockStateOrFinishWrongTransaction) {
    BusinessTopic topic; Directory dir; auto config = configuration(dir); GatewayRuntime gateway(config);
    auto client = ClientRuntime::acquire(dir.control()); PublisherEndpoint publisher(client, topic.descriptor);
    PeerStub peer; peer.announce(config, topic.descriptor, client, publisher.publisher_id());
    const auto bytes = flat_blob(1024); auto future = std::async(std::launch::async, [&] { return publisher.prebuilt(ByteView(bytes), Delivery::Reliable, 1000); });
    ReceivedDatagram packet; ASSERT_TRUE(until([&] { return peer.data.receive(packet).status == IoStatus::Data; }));
    WireHeader header; ByteView payload; ASSERT_TRUE(decode_packet(packet.view(), header, payload));
    auto wrong = header; ++wrong.message_crc;
    for (unsigned i = 0; i < 128; ++i) peer.reply(config, wrong, PacketKind::Ack);
    EXPECT_NE(client->status().find("\"state\":\"Ready\""), std::string::npos); EXPECT_EQ(future.wait_for(0ms), std::future_status::timeout);
    ASSERT_TRUE(until([&] { peer.reply(config, header, PacketKind::Ack); return future.wait_for(0ms) == std::future_status::ready; }, 500ms)); EXPECT_TRUE(future.get().success);
    ASSERT_TRUE(until([&] { return client->granted().records > config.limits.initial_send_records; }));
    const auto granted = client->granted(); for (unsigned i = 0; i < 64; ++i) peer.reply(config, header, PacketKind::Ack);
    std::this_thread::sleep_for(10ms); EXPECT_EQ(client->granted().records, granted.records); EXPECT_TRUE(client->healthy());
}

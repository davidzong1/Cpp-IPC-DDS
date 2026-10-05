#include "shared_net/shm_wire_fixture.h"
#include "shared_net/directory_fixture.h"
#include "dzIPC/net/publisher_endpoint.h"
#include "gtest/gtest.h"
using namespace shared_net_test;
namespace {
struct RemoteDemand {
    DatagramEndpoint control{Ipv4Address::parse("127.0.0.1", 0)}, data{Ipv4Address::parse("127.0.0.1", 0)};
    void announce(const GatewayConfig& config, const RouteDescriptor& descriptor, const std::shared_ptr<ClientRuntime>& client, Identity publisher) {
        DiscoveryHello h; h.gateway_id = identity(782); h.gateway_epoch = 88; h.snapshot_version = 1;
        h.control_port = control.local_address().port; h.data_base_port = data.local_address().port; h.data_shards = 1;
        Bytes hello; ASSERT_TRUE(encode_hello(h, hello));
        ASSERT_EQ(control.send(ByteView(hello), Ipv4Address::parse(config.listen_ip, config.discovery_port)).status, IoStatus::Data);
        ReceivedDatagram request; ASSERT_TRUE(until([&] { return control.receive(request).status == IoStatus::Data; }));
        CatalogHeader header; ByteView bytes; ASSERT_TRUE(decode_catalog(request.view(), header, bytes));
        std::swap(header.source_id, header.target_id); std::swap(header.source_epoch, header.target_epoch);
        auto route = descriptor; route.role_flags = 2; route.receiver_route_epoch = 77;
        DirectorySnapshot snapshot; snapshot.version = 1; ASSERT_TRUE(encode_directory({route}, snapshot.body)); snapshot.body_crc = crc32c(ByteView(snapshot.body));
        auto page = catalog_page(snapshot, header, 0); ASSERT_EQ(control.send(ByteView(page), request.source).status, IoStatus::Data);
        ASSERT_TRUE(until([&] { return client->route_state(publisher).remote_ready_count == 1; }));
    }
};
}
TEST(SharedNetPartialSubmit, EveryTwoLegCombinationPreservesFallbackBoundary) {
    for (auto a : {SubmitState::NotRequired, SubmitState::NotSubmitted, SubmitState::Committed, SubmitState::Indeterminate})
        for (auto b : {SubmitState::NotRequired, SubmitState::NotSubmitted, SubmitState::Committed, SubmitState::Indeterminate}) {
            const bool visible = a == SubmitState::Committed || b == SubmitState::Committed || a == SubmitState::Indeterminate || b == SubmitState::Indeterminate;
            EXPECT_EQ(best_effort_result(a, b), visible);
        }
}
TEST(SharedNetPartialSubmit, HealthyZeroDemandSkipsOutboxAndGatewayLossKeepsLocalWriter) {
    BusinessTopic topic; Directory dir; auto config = configuration(dir); GatewayRuntime gateway(config);
    auto client = ClientRuntime::acquire(dir.control()); PublisherEndpoint writer(client, topic.descriptor);
    ipc::mpmc_channel receiver(topic.segment().c_str(), ipc::receiver, false);
    ASSERT_TRUE(until([&] { return client->route_state(writer.publisher_id()).synchronized; }));
    const auto bytes = flat_blob(64); auto sent = writer.prebuilt(ByteView(bytes));
    EXPECT_TRUE(sent.success); EXPECT_EQ(sent.local, SubmitState::Committed); EXPECT_EQ(sent.network, SubmitState::NotRequired);
    EXPECT_NE(client->status().find("\"outbox_records\":\"0\""), std::string::npos);
    auto sample = receiver.try_recv(); ASSERT_EQ(sample.size(), bytes.size()); EXPECT_EQ(std::memcmp(sample.data(), bytes.data(), bytes.size()), 0);
    gateway.stop(); ASSERT_TRUE(until([&] { return !client->healthy(); }));
    sent = writer.prebuilt(ByteView(bytes)); EXPECT_TRUE(sent.success); EXPECT_EQ(sent.local, SubmitState::Committed); EXPECT_EQ(sent.network, SubmitState::NotSubmitted);
    sample = receiver.try_recv(); ASSERT_EQ(sample.size(), bytes.size()); EXPECT_EQ(std::memcmp(sample.data(), bytes.data(), bytes.size()), 0);
    EXPECT_TRUE(receiver.try_recv().empty());
    sent = writer.prebuilt(ByteView(bytes), Delivery::Reliable, 1000);
    EXPECT_FALSE(sent.success); EXPECT_EQ(sent.local, SubmitState::Committed); EXPECT_EQ(sent.remote.result, SendResultCode::GatewayLost);
    sample = receiver.try_recv(); ASSERT_EQ(sample.size(), bytes.size()); EXPECT_TRUE(receiver.try_recv().empty());
}
TEST(SharedNetPartialSubmit, LocalPoolFailureStillHandsOffNetworkAndNeverRetriesLocalLeg) {
    BusinessTopic topic; Directory dir; auto config = configuration(dir); GatewayRuntime gateway(config);
    auto client = ClientRuntime::acquire(dir.control()); PublisherEndpoint writer(client, topic.descriptor);
    ipc::mpmc_channel receiver(topic.segment().c_str(), ipc::receiver, false);
    ShmWireWriter filler(topic.descriptor); const auto bytes = flat_blob(1024);
    for (unsigned i = 0; i < 10; ++i) ASSERT_EQ(filler.try_commit_prebuilt(ByteView(bytes)), SubmitState::Committed);
    RemoteDemand remote; remote.announce(config, topic.descriptor, client, writer.publisher_id());
    const auto sent = writer.prebuilt(ByteView(bytes)); EXPECT_TRUE(sent.success); EXPECT_EQ(sent.local, SubmitState::NotSubmitted); EXPECT_EQ(sent.network, SubmitState::Committed);
    ReceivedDatagram packet; ASSERT_TRUE(until([&] { return remote.data.receive(packet).status == IoStatus::Data; }));
    WireHeader header; ByteView payload; ASSERT_TRUE(decode_packet(packet.view(), header, payload));
    EXPECT_EQ(header.sequence, sent.sequence); ASSERT_EQ(payload.size, bytes.size()); EXPECT_EQ(std::memcmp(payload.data, bytes.data(), bytes.size()), 0);
    for (unsigned i = 0; i < 10; ++i) { auto sample = receiver.try_recv(); ASSERT_EQ(sample.size(), bytes.size()); EXPECT_EQ(std::memcmp(sample.data(), bytes.data(), bytes.size()), 0); }
    EXPECT_TRUE(receiver.try_recv().empty());
}
TEST(SharedNetPartialSubmit, ZeroTimeoutAndInvalidPrebuiltNeverBecomeVisible) {
    BusinessTopic topic; Directory dir; auto config = configuration(dir); GatewayRuntime gateway(config);
    auto client = ClientRuntime::acquire(dir.control()); PublisherEndpoint writer(client, topic.descriptor);
    ipc::mpmc_channel receiver(topic.segment().c_str(), ipc::receiver, false);
    auto bytes = flat_blob(64); const auto zero = writer.prebuilt(ByteView(bytes), Delivery::Reliable, 0);
    EXPECT_FALSE(zero.success); EXPECT_EQ(zero.local, SubmitState::NotSubmitted); EXPECT_TRUE(receiver.try_recv().empty());
    bytes[0] ^= 1; EXPECT_FALSE(writer.prebuilt(ByteView(bytes)).success); EXPECT_TRUE(receiver.try_recv().empty());
    bytes[0] ^= 1; ASSERT_TRUE(until([&] { return client->route_state(writer.publisher_id()).synchronized; }));
    EXPECT_TRUE(writer.prebuilt(ByteView(bytes)).success); auto sample = receiver.try_recv(); ASSERT_EQ(sample.size(), bytes.size()); EXPECT_EQ(std::memcmp(sample.data(), bytes.data(), bytes.size()), 0); EXPECT_TRUE(receiver.try_recv().empty());
}

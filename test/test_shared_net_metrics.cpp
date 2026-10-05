#include "shared_net/reassembly_fixture.h"
#include "shared_net/directory_fixture.h"
#include "shared_net/peer_stub.h"
#include "shared_net/shm_wire_fixture.h"
#include "dzIPC/net/publisher_endpoint.h"
#include "gtest/gtest.h"
using namespace shared_net_test;
namespace {
std::uint64_t number(const std::string& json, const std::string& key) {
    const auto begin = json.find('"' + key + "\":");
    if (begin == std::string::npos) throw std::runtime_error("缺少指标 " + key);
    return std::stoull(json.substr(begin + key.size() + 3));
}
std::string stage(const std::string& json, const std::string& key) {
    const auto begin = json.find('"' + key + "\":{");
    if (begin == std::string::npos) throw std::runtime_error("缺少阶段 " + key);
    return json.substr(begin, json.find('}', begin) - begin + 1);
}
}
TEST(SharedNetMetrics, ConcurrentHistogramHasExactCountsAndBoundedQuantiles) {
    LatencyHistogram histogram; std::vector<std::thread> threads;
    for (unsigned n = 0; n < 8; ++n) threads.emplace_back([&] { for (unsigned i = 0; i < 1000; ++i) histogram.observe(1000); });
    for (auto& thread : threads) thread.join();
    const auto json = histogram.json(); EXPECT_EQ(number(json, "count"), 8000u);
    EXPECT_EQ(number(json, "sum_ns"), 8000000u); EXPECT_EQ(number(json, "max_ns"), 1000u);
    EXPECT_EQ(number(json, "p99_upper_ns"), 1023u);
    histogram.observe(UINT64_MAX); EXPECT_EQ(number(histogram.json(), "max_ns"), UINT64_MAX);
}
TEST(SharedNetMetrics, MalformedDatagramsAreClassifiedBeforeAllocation) {
    ReassemblyFixture f; auto original = f.packets(flat_blob(2048)).front();
    auto packet = original; packet.bytes[0] ^= 1; f.shard->ingest(packet, f.admission, 1);
    packet = original; packet.bytes[4] = 255; f.shard->ingest(packet, f.admission, 1);
    packet = original; packet.bytes[200] ^= 1; f.shard->ingest(packet, f.admission, 1);
    packet = original; packet.size = 12; f.shard->ingest(packet, f.admission, 1);
    packet = original; ++packet.source.port; f.shard->ingest(packet, f.admission, 1);
    packet = changed_packet(original, [](auto& h, auto&) { ++h.target_epoch; }); f.shard->ingest(packet, f.admission, 1);
    const auto m = f.budget->metrics();
    EXPECT_EQ(m->get(NetMetric::bad_magic), 1u); EXPECT_EQ(m->get(NetMetric::bad_version), 1u);
    EXPECT_EQ(m->get(NetMetric::bad_header), 1u); EXPECT_EQ(m->get(NetMetric::packet_crc_fail), 1u);
    EXPECT_EQ(m->get(NetMetric::source_route_unverified), 1u); EXPECT_EQ(m->get(NetMetric::foreign_route), 1u);
    EXPECT_EQ(f.budget->usage().assemblies, 0u); EXPECT_EQ(f.budget->usage().streams, 0u);
}
TEST(SharedNetMetrics, DuplicateFragmentsAndCommittedMessagesAreSeparate) {
    ReassemblyFixture f; const auto bytes = flat_blob(2048); const auto packets = f.packets(bytes);
    f.shard->ingest(packets[0], f.admission, 1); f.shard->ingest(packets[0], f.admission, 2);
    f.shard->ingest(packets[1], f.admission, 3);
    f.shard->tick(4, [](auto&, auto&) { return SubmitState::Committed; });
    f.shard->ingest(packets[0], f.admission, 5);
    EXPECT_EQ(f.budget->metrics()->get(NetMetric::duplicate_fragment), 1u);
    EXPECT_EQ(f.budget->metrics()->get(NetMetric::duplicate_message_suppressed), 1u);
    EXPECT_EQ(f.budget->metrics()->get(NetMetric::reassembly_copy_bytes), bytes.size());
    EXPECT_EQ(f.admission.subscriber->metrics.commits.load(), 1u);
    EXPECT_EQ(number(stage(f.budget->metrics()->json(2), "remote_commit"), "count"), 1u);
    f.shard.reset(); EXPECT_EQ(f.budget->usage().bytes, 0u); EXPECT_EQ(f.budget->usage().receipts, 0u);
}
TEST(SharedNetMetrics, EveryReceiveQuotaRejectsWithoutLeakingReservation) {
    struct Case { const char* name; std::uint64_t Limits::*field; std::uint64_t limit; };
    for (const auto& c : std::vector<Case>{{"message_bytes", &Limits::message_bytes, 1024}, {"reassembly_bytes", &Limits::reassembly_bytes, 1024},
        {"peer_reassembly_bytes", &Limits::peer_reassembly_bytes, 1024}, {"route_reassembly_bytes", &Limits::route_reassembly_bytes, 1024},
        {"assemblies", &Limits::assemblies, 0}, {"streams", &Limits::streams, 0}, {"receipts", &Limits::receipts, 0},
        {"peer_receipts", &Limits::peer_receipts, 0}, {"commit_pending_bytes", &Limits::commit_pending_bytes, 1024}}) {
        SCOPED_TRACE(c.name); Limits limits; limits.*(c.field) = c.limit; ReassemblyFixture f(limits);
        if (std::string(c.name) == "commit_pending_bytes") f.feed(flat_blob(2048));
        else f.shard->ingest(f.packets(flat_blob(2048)).front(), f.admission, 1);
        EXPECT_EQ(number(stage(f.budget->metrics()->json(1), c.name), "rejected"), 1u);
        f.shard.reset(); const auto usage = f.budget->usage();
        EXPECT_EQ(usage.bytes, 0u); EXPECT_EQ(usage.streams, 0u); EXPECT_EQ(usage.receipts, 0u); EXPECT_EQ(usage.pending_bytes, 0u);
    }
}
TEST(SharedNetMetrics, SendBudgetPeaksRejectionsAndReleaseAreExact) {
    auto m = std::make_shared<NetMetrics>(); SendBudget budget({128,2}, {64,1}, m);
    auto a = budget.open({64,1}), b = budget.open({64,1});
    EXPECT_FALSE(a->grant({1,1}));
    for (const auto* name : {"send_bytes", "send_records", "session_send_bytes", "session_send_records"})
        EXPECT_EQ(number(stage(m->json(1), name), "rejected"), 1u);
    EXPECT_EQ(number(stage(m->json(1), "send_bytes"), "peak"), 128u);
    ASSERT_TRUE(a->take({64,1})); a->close(); EXPECT_EQ(budget.occupied().bytes, 128u);
    a->finish({64,1}); b->close(); EXPECT_EQ(budget.occupied().bytes, 0u); EXPECT_EQ(budget.inflight().bytes, 0u);
    EXPECT_EQ(m->get(NetMetric::credit_grant_rejected), 1u);
}
TEST(SharedNetMetrics, DirectoryChargesRouteMetricsAndExpiresOnlyOnce) {
    DirectoryFixture f; ASSERT_EQ(f.install({catalog_route("metric/topic")}), DirectoryCode::Ok);
    EXPECT_EQ(f.budget->metrics()->get(NetMetric::snapshot_complete), 1u);
    EXPECT_GE(f.budget->usage().installed, sizeof(RouteMetrics));
    f.peers.tick(3000000001ull); f.peers.tick(3000000002ull);
    EXPECT_EQ(f.budget->metrics()->get(NetMetric::peer_expired), 1u);
    Limits limits; limits.directory_bytes = 1; DirectoryBudget tiny(limits);
    EXPECT_FALSE(tiny.replace({catalog_route("metric/topic")}, 1));
    EXPECT_EQ(number(stage(tiny.metrics()->json(1), "directory_bytes"), "rejected"), 1u); EXPECT_EQ(tiny.usage().installed, 0u);
}
TEST(SharedNetMetrics, RealAckPopulatesStagesRouteAndShardAndReleasesCredit) {
    BusinessTopic topic; Directory dir; auto config = configuration(dir); GatewayRuntime gateway(config);
    auto client = ClientRuntime::acquire(dir.control()); PublisherEndpoint publisher(client, topic.descriptor);
    PeerStub peer; peer.announce(config, topic.descriptor, client, publisher.publisher_id()); const auto bytes = flat_blob(1024);
    const auto granted = client->granted();
    auto call = std::async(std::launch::async, [&] { return publisher.prebuilt(ByteView(bytes), Delivery::Reliable, 1000); });
    ReceivedDatagram packet; ASSERT_TRUE(until([&] { return peer.data.receive(packet).status == IoStatus::Data; }));
    WireHeader h; ByteView payload; ASSERT_TRUE(decode_packet(packet.view(), h, payload)); peer.reply(config, h, PacketKind::Ack);
    ASSERT_EQ(call.wait_for(500ms), std::future_status::ready); ASSERT_TRUE(call.get().success);
    const auto counters = client->gateway_metrics(0); EXPECT_EQ(number(counters, "acks_rx"), 1u);
    EXPECT_EQ(number(counters, "reliable_started"), 1u); EXPECT_EQ(number(counters, "reliable_completed"), 1u);
    const auto latency = client->gateway_metrics(2);
    for (const auto* name : {"gateway_queue_wait", "network_first_send", "ack_wait"}) EXPECT_EQ(number(stage(latency, name), "count"), 1u);
    EXPECT_EQ(number(stage(latency, "remote_commit"), "count"), 0u);
    EXPECT_EQ(number(stage(client->metrics().json(2), "outbox_submit"), "count"), 1u);
    EXPECT_EQ(number(stage(client->metrics().json(2), "api_return"), "count"), 1u);
    EXPECT_GT(number(client->route_status(topic.descriptor.key), "tx_bytes"), 0u);
    EXPECT_NE(client->gateway_metrics(3).find("\"shards\":["), std::string::npos);
    for (unsigned category = 0; category < 4; ++category) EXPECT_LT(client->gateway_metrics(category).size(), 8000u);
    EXPECT_THROW(client->gateway_metrics(4), std::invalid_argument);
    ASSERT_TRUE(until([&] { return client->granted().records == granted.records + 1; }));
    publisher.close(); EXPECT_NE(client->route_status(topic.descriptor.key).find("\"ready\":false"), std::string::npos);
    client->stop(); EXPECT_EQ(client->metrics().get(NetMetric::gateway_lost), 0u);
}
TEST(SharedNetMetrics, CreditWaitSamplesOnlyActualResourceWaits) {
    BusinessTopic topic; Directory dir; auto config = configuration(dir);
    config.limits.publisher_reliable = config.limits.send_records = config.limits.session_send_records = config.limits.initial_send_records = 1;
    GatewayRuntime gateway(config); auto client = ClientRuntime::acquire(dir.control()); PublisherEndpoint publisher(client, topic.descriptor);
    PeerStub peer; peer.announce(config, topic.descriptor, client, publisher.publisher_id()); const auto bytes = flat_blob(64);
    auto first = std::async(std::launch::async, [&] { return publisher.prebuilt(ByteView(bytes), Delivery::Reliable, 1000); });
    ReceivedDatagram packet; ASSERT_TRUE(until([&] { return peer.data.receive(packet).status == IoStatus::Data; }));
    WireHeader original; ByteView body; ASSERT_TRUE(decode_packet(packet.view(), original, body));
    auto second = std::async(std::launch::async, [&] { return publisher.prebuilt(ByteView(bytes), Delivery::Reliable, 1000); });
    ASSERT_TRUE(until([&] { return number(client->diagnostics_json(), "credit_wait_count") > 0; }));
    EXPECT_EQ(second.wait_for(0ms), std::future_status::timeout);
    peer.reply(config, original, PacketKind::Ack);
    ASSERT_EQ(first.wait_for(500ms), std::future_status::ready); ASSERT_TRUE(first.get().success);
    WireHeader next;
    ASSERT_TRUE(until([&] { return peer.data.receive(packet).status == IoStatus::Data && decode_packet(packet.view(), next, body) && next.sequence != original.sequence; }));
    peer.reply(config, next, PacketKind::Ack);
    ASSERT_EQ(second.wait_for(500ms), std::future_status::ready); ASSERT_TRUE(second.get().success);
    const auto wait = stage(client->metrics().json(2), "credit_wait");
    EXPECT_GT(number(wait, "count"), 0u); EXPECT_GT(number(wait, "max_ns"), 0u);
    EXPECT_GT(number(client->gateway_metrics(0), "credit_grant_rejected"), 0u);
}

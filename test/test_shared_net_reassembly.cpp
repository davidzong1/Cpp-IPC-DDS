#include "shared_net/reassembly_fixture.h"
#include "shared_net/shm_wire_fixture.h"
#include "gtest/gtest.h"
#include <algorithm>
#include <set>
using namespace shared_net_test;
TEST(SharedNetReassembly, BoundarySizesReverseOrderAndDuplicatesPreserveEveryByte) {
    for (const auto size : {1023u, 1024u, 1025u, 1024u * 1024, kMaxMessageBytes}) {
        ReassemblyFixture f; const auto bytes = flat_blob(size); auto packets = f.packets(bytes); std::reverse(packets.begin(), packets.end());
        for (const auto& packet : packets) { f.shard->ingest(packet, f.admission, 1); EXPECT_EQ(f.shard->ingest(packet, f.admission, 2).disposition, ReceiveDisposition::Duplicate); }
        unsigned commits = 0;
        const auto replies = f.shard->tick(3, [&](const auto&, const auto& blob) { ++commits; EXPECT_EQ(blob.size(), bytes.size()); EXPECT_EQ(std::memcmp(blob.view().data, bytes.data(), bytes.size()), 0); return SubmitState::Committed; });
        EXPECT_EQ(commits, 1u); ASSERT_EQ(replies.size(), 1u); EXPECT_EQ(reply_kind(replies.front()), PacketKind::Ack);
        EXPECT_EQ(f.budget->usage().bytes, 0u); EXPECT_EQ(f.budget->usage().pending_bytes, 0u);
    }
    Limits limits; limits.message_bytes = 1024; ReassemblyFixture f(limits);
    EXPECT_EQ(f.shard->ingest(f.packets(flat_blob(1025)).front(), f.admission, 1).disposition, ReceiveDisposition::Dropped);
    EXPECT_EQ(f.budget->usage().assemblies, 0u);
    Bytes too_large(kMaxMessageBytes + 1); EXPECT_THROW(f.packets(too_large), std::runtime_error);
}
TEST(SharedNetReassembly, V2RejectsStaleSourcePortAndEndpointEpochBeforeAllocation) {
    ReassemblyFixture f;
    f.admission.network_version = NetworkVersion::V2;
    f.admission.peer->network_version = NetworkVersion::V2;
    f.admission.peer->hello_v2.gateway_id = f.admission.peer->hello.gateway_id;
    f.admission.peer->hello_v2.gateway_epoch = f.admission.peer->hello.gateway_epoch;
    f.admission.publisher->descriptor.data_port = 31001;
    f.admission.publisher->descriptor.endpoint_epoch = 101;
    f.admission.publisher->descriptor.endpoint_flags = 1;
    f.admission.subscriber->descriptor.data_port = 31002;
    f.admission.subscriber->descriptor.endpoint_epoch = 202;
    f.admission.subscriber->descriptor.endpoint_flags = 1;
    f.admission.local_data_port = 31002;
    f.shard = std::make_unique<ReassemblyShard>(f.local, 3, 0, 1, f.budget, 2000000, 2000000, NetworkVersion::V2);
    const auto bytes = flat_blob(64);
    auto packet = f.packets(bytes).front();

    auto stale = changed_packet(packet, [](auto& h, auto&) { ++h.data_source_endpoint_epoch; });
    EXPECT_EQ(f.shard->ingest(stale, f.admission, 1).disposition, ReceiveDisposition::Dropped);
    stale = packet; ++stale.source.port;
    EXPECT_EQ(f.shard->ingest(stale, f.admission, 1).disposition, ReceiveDisposition::Dropped);
    stale = changed_packet(packet, [](auto& h, auto&) { ++h.data_target_endpoint_epoch; });
    EXPECT_EQ(f.shard->ingest(stale, f.admission, 1).disposition, ReceiveDisposition::Dropped);
    EXPECT_EQ(f.budget->usage().assemblies, 0u);

    EXPECT_EQ(f.shard->ingest(packet, f.admission, 2).disposition, ReceiveDisposition::CommitPending);
    const auto feedback = f.shard->tick(3, [](const auto&, const auto&) { return SubmitState::Committed; });
    ASSERT_EQ(feedback.size(), 1u);
    WireHeader ack; ByteView payload;
    ASSERT_TRUE(decode_packet_v2(ByteView(feedback.front().control_packet), ack, payload));
    EXPECT_EQ(ack.data_source_endpoint_epoch, 101u);
    EXPECT_EQ(ack.data_target_endpoint_epoch, 202u);
}
TEST(SharedNetReassembly, InterleavedPublishersAndTopicsNeverMix) {
    ReassemblyFixture f, other; other.route("reassembly/b");
    std::vector<Bytes> originals; std::vector<std::vector<ReceivedDatagram>> batches;
    for (unsigned i = 0; i < 4; ++i) { originals.push_back(flat_blob(6145, i + 1)); batches.push_back((i < 2 ? f : other).packets(originals.back(), 19, i % 2 + 1)); }
    for (int fragment = 6; fragment >= 0; --fragment) for (unsigned i = 0; i < 4; ++i) f.shard->ingest(batches[i][fragment], i < 2 ? f.admission : other.admission, 1);
    unsigned commits = 0; f.shard->tick(2, [&](const WireHeader& h, const WireBlob& blob) {
        const auto index = (h.route == f.admission.publisher->descriptor.key ? 0u : 2u) + h.publisher_id[0] - 1;
        EXPECT_EQ(std::memcmp(blob.view().data, originals[index].data(), blob.size()), 0); ++commits; return SubmitState::Committed;
    });
    EXPECT_EQ(commits, 4u); EXPECT_EQ(f.shard->stats().completed, 4u);
}
TEST(SharedNetReassembly, CollectingConflictRejectsButPendingConflictCannotReplaceTicket) {
    ReassemblyFixture f; const auto bytes = flat_blob(2048); auto packets = f.packets(bytes);
    f.shard->ingest(packets[0], f.admission, 1);
    auto conflict = changed_packet(packets[0], [](auto&, auto& body) { body[40] ^= 1; });
    EXPECT_EQ(f.shard->ingest(conflict, f.admission, 2).disposition, ReceiveDisposition::Rejected);
    f.shard->ingest(packets[1], f.admission, 3); unsigned commits = 0;
    f.shard->tick(4, [&](auto&, auto&) { ++commits; return SubmitState::Committed; }); EXPECT_EQ(commits, 0u);
    auto next = f.packets(bytes, 2); for (const auto& packet : next) f.shard->ingest(packet, f.admission, 5);
    conflict = changed_packet(next[0], [](auto&, auto& body) { body[40] ^= 1; });
    EXPECT_EQ(f.shard->ingest(conflict, f.admission, 6).disposition, ReceiveDisposition::Rejected);
    conflict = changed_packet(next[0], [](auto& h, auto&) { h.message_crc ^= 1; });
    EXPECT_EQ(f.shard->ingest(conflict, f.admission, 7).disposition, ReceiveDisposition::Rejected);
    EXPECT_EQ(f.budget->usage().assemblies, 1u);
    f.shard->tick(8, [&](const auto&, const auto& blob) { ++commits; EXPECT_EQ(std::memcmp(blob.view().data, bytes.data(), bytes.size()), 0); return SubmitState::Committed; }); EXPECT_EQ(commits, 1u);
}
TEST(SharedNetReassembly, PendingRetryAndIndeterminateFinishAtMostOnce) {
    ReassemblyFixture f; f.feed(flat_blob(40)); unsigned attempts = 0;
    auto commit = [&](const auto&, const auto&) { return ++attempts < 3 ? SubmitState::NotSubmitted : SubmitState::Committed; };
    EXPECT_TRUE(f.shard->tick(2, commit).empty()); EXPECT_TRUE(f.shard->tick(1000000, commit).empty()); EXPECT_EQ(attempts, 1u);
    EXPECT_TRUE(f.shard->tick(2000002, commit).empty()); EXPECT_EQ(attempts, 2u);
    auto reply = f.shard->tick(4000002, commit); ASSERT_EQ(reply.size(), 1u); EXPECT_EQ(reply_kind(reply.front()), PacketKind::Ack);
    f.shard->tick(9000000, commit); EXPECT_EQ(attempts, 3u);
    f.feed(flat_blob(40), 2, Delivery::Reliable, 10000000);
    reply = f.shard->tick(10000001, [&](const auto&, const auto&) { ++attempts; return SubmitState::Indeterminate; }); ASSERT_EQ(reply.size(), 1u);
    EXPECT_EQ(reply_kind(reply.front()), PacketKind::Reject); f.shard->tick(20000000, commit); EXPECT_EQ(attempts, 4u);
}
TEST(SharedNetReassembly, NackGroupsAreBoundedAndDuplicatesDoNotPostponeTimer) {
    ReassemblyFixture f; auto packets = f.packets(flat_blob(300 * 1024));
    f.shard->ingest(packets.back(), f.admission, 1); f.shard->ingest(packets.back(), f.admission, 1900000);
    auto never = [](const auto&, const auto&) { ADD_FAILURE(); return SubmitState::NotSubmitted; };
    EXPECT_TRUE(f.shard->tick(1999999, never).empty()); auto replies = f.shard->tick(2000001, never); ASSERT_EQ(replies.size(), 1u);
    WireHeader header; ByteView body; ASSERT_TRUE(decode_packet(ByteView(replies.front().control_packet), header, body));
    EXPECT_EQ(header.kind, PacketKind::Nack); EXPECT_EQ(body.size, 256u * 4); EXPECT_EQ(codec::get(body.data, 4), 0u); EXPECT_EQ(codec::get(body.data + body.size - 4, 4), 255u);
    replies = f.shard->tick(4000001, never); ASSERT_EQ(replies.size(), 1u); ASSERT_TRUE(decode_packet(ByteView(replies.front().control_packet), header, body));
    EXPECT_EQ(body.size, 43u * 4); EXPECT_EQ(codec::get(body.data, 4), 256u);
    replies = f.shard->tick(5000000001ull, never); ASSERT_EQ(replies.size(), 1u); EXPECT_EQ(reply_kind(replies.front()), PacketKind::Reject); EXPECT_EQ(f.budget->usage().bytes, 0u);
}
TEST(SharedNetReassembly, IdentitySourcePortAndWrongShardAllocateNothing) {
    ReassemblyFixture f; auto packet = f.packets(flat_blob(1025)).front();
    auto bad = packet; bad.status = IoStatus::Truncated; f.shard->ingest(bad, f.admission, 1);
    bad = packet; ++bad.source.port; f.shard->ingest(bad, f.admission, 1);
    bad = packet; ++bad.source.host; f.shard->ingest(bad, f.admission, 1);
    bad = packet; bad.bytes[200] ^= 1; f.shard->ingest(bad, f.admission, 1);
    bad = changed_packet(packet, [](auto& h, auto&) { ++h.target_epoch; }); f.shard->ingest(bad, f.admission, 1);
    f.admission.publisher->active.store(false); f.shard->ingest(packet, f.admission, 1); f.admission.publisher->active.store(true);
    const unsigned right = route_hash(f.admission.publisher->descriptor.key) % 2;
    ReassemblyShard wrong(f.local, 3, 1 - right, 2, f.budget); wrong.ingest(packet, f.admission, 1);
    EXPECT_EQ(wrong.stats().wrong_shard, 1u); EXPECT_EQ(f.budget->usage().assemblies, 0u); EXPECT_EQ(f.budget->usage().streams, 0u);
}
TEST(SharedNetReassembly, AckOnlyFollowsRealShmCommit) {
    BusinessTopic topic; ReassemblyFixture f; f.route(topic.descriptor.topic);
    ShmWireBridge bridge(topic.descriptor); ipc::mpmc_channel rx(topic.segment().c_str(), ipc::receiver, false);
    const auto data = flat_blob(4096); EXPECT_EQ(f.feed(data).disposition, ReceiveDisposition::CommitPending); EXPECT_TRUE(rx.try_recv().empty());
    auto replies = f.shard->tick(2, [&](const auto&, const auto& blob) { return bridge.try_commit(blob); });
    ASSERT_EQ(replies.size(), 1u); EXPECT_EQ(reply_kind(replies.front()), PacketKind::Ack); auto sample = rx.try_recv(); ASSERT_EQ(sample.size(), data.size()); EXPECT_EQ(std::memcmp(sample.data(), data.data(), data.size()), 0);
    EXPECT_TRUE(rx.try_recv().empty());
}
TEST(SharedNetReassembly, FullDomainsZeroIdAndLongNamesStayDistinctOnOneShard) {
    std::vector<std::unique_ptr<ReassemblyFixture>> routes;
    std::vector<Bytes> originals;
    std::vector<std::vector<ReceivedDatagram>> packets;
    const std::array<std::uint64_t, 4> domains{0, 1ull << 32, (1ull << 40) + 3, UINT64_MAX};
    const std::string prefix(127, 'x');
    for (auto domain : domains) for (const auto* suffix : {"/alpha", "/beta"}) {
        auto f = std::make_unique<ReassemblyFixture>(); f->route(prefix + suffix);
        auto& p = f->admission.publisher->descriptor; auto& s = f->admission.subscriber->descriptor;
        p.key.scope = dzIPC::common::channel_scope_token(p.topic, domain, dzIPC::common::ScopeKind::PubSub);
        p.key.msg_id = 0; s.key = p.key;
        f->admission.peer->hello.data_shards = 1; // 全部共用相同来源端口和接收 shard。
        Bytes encoded; ASSERT_TRUE(encode_descriptor(p, false, encoded)); RouteDescriptor decoded;
        ASSERT_TRUE(decode_descriptor(ByteView(encoded), false, decoded)); EXPECT_EQ(decoded.topic, p.topic); EXPECT_EQ(decoded.key, p.key);
        auto bytes = flat_blob(4096, routes.size() + 1); dzflat::SegHeader h{};
        std::memcpy(&h, bytes.data(), sizeof(h)); h.msg_id = 0; std::memcpy(bytes.data(), &h, sizeof(h));
        packets.push_back(f->packets(bytes, 7, 1)); originals.push_back(std::move(bytes)); routes.push_back(std::move(f));
    }
    for (int fragment = 3; fragment >= 0; --fragment) for (unsigned i = 0; i < routes.size(); ++i)
        routes.front()->shard->ingest(packets[i][fragment], routes[i]->admission, 1);
    std::set<RouteKey> committed;
    routes.front()->shard->tick(2, [&](const WireHeader& h, const WireBlob& blob) {
        const auto i = std::find_if(routes.begin(), routes.end(), [&](const auto& f) { return f->admission.publisher->descriptor.key == h.route; });
        EXPECT_NE(i, routes.end());
        if (i != routes.end()) EXPECT_EQ(std::memcmp(blob.view().data, originals[i-routes.begin()].data(), blob.size()), 0);
        EXPECT_TRUE(committed.insert(h.route).second); return SubmitState::Committed;
    });
    EXPECT_EQ(committed.size(), 8u);
}
TEST(SharedNetReassembly, DifferentShardCountsAndDuplicateEndpointsCommitOnlyAtOwner) {
    BusinessTopic topic; ReassemblyFixture f; f.route(topic.descriptor.topic);
    ShmWireBridge bridge(topic.descriptor); ipc::mpmc_channel rx(topic.segment().c_str(), ipc::receiver, false);
    const auto owner = route_hash(topic.descriptor.key) % 2;
    ReassemblyShard first(f.local, 3, 0, 2, f.budget), second(f.local, 3, 1, 2, f.budget);
    const auto bytes = flat_blob(4096);
    for (const auto& packet : f.packets(bytes)) {
        first.ingest(packet, f.admission, 1); second.ingest(packet, f.admission, 1);
    }
    unsigned commits = 0;
    auto commit = [&](const auto&, const auto& blob) { ++commits; return bridge.try_commit(blob); };
    const auto a = first.tick(2, commit), b = second.tick(2, commit);
    EXPECT_EQ(commits, 1u); EXPECT_EQ(a.size(), owner == 0 ? 1u : 0u); EXPECT_EQ(b.size(), owner == 1 ? 1u : 0u);
    auto sample = rx.try_recv(); ASSERT_EQ(sample.size(), bytes.size()); EXPECT_EQ(std::memcmp(sample.data(), bytes.data(), bytes.size()), 0);
    EXPECT_TRUE(rx.try_recv().empty());
}

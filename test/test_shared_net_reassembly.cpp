#include "shared_net/reassembly_fixture.h"
#include "shared_net/shm_wire_fixture.h"
#include "gtest/gtest.h"
#include <algorithm>
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

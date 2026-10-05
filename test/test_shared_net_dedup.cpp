#include "shared_net/reassembly_fixture.h"
#include "gtest/gtest.h"
using namespace shared_net_test;
TEST(SharedNetDedup, LargeFirstSequenceWrapAndIdleNeverForgetCommittedData) {
    Limits limits; limits.stream_window = 4; ReassemblyFixture f(limits); unsigned commits = 0;
    auto commit = [&](const auto&, const auto&) { ++commits; return SubmitState::Committed; }; const auto data = flat_blob(40);
    constexpr std::uint64_t first = 1000000000000ull;
    f.feed(data, first, Delivery::BestEffort); f.shard->tick(2, commit);
    f.feed(data, first + 4, Delivery::BestEffort, 3); f.shard->tick(4, commit); EXPECT_EQ(commits, 2u);
    f.shard->tick(6000000000ull, commit); f.feed(data, first, Delivery::BestEffort, 6000000001ull); f.feed(data, first + 4, Delivery::BestEffort, 6000000001ull);
    f.shard->tick(6000000002ull, commit); EXPECT_EQ(commits, 2u); EXPECT_EQ(f.budget->usage().streams, 1u);
}
TEST(SharedNetDedup, ReliableReceiptSurvivesWindowSlideThenExpiresWithoutReplay) {
    Limits limits; limits.stream_window = 4; ReassemblyFixture f(limits); unsigned commits = 0;
    auto commit = [&](const auto&, const auto&) { ++commits; return SubmitState::Committed; }; auto data = flat_blob(40);
    f.feed(data); f.shard->tick(2, commit); f.feed(data, 10000, Delivery::BestEffort, 3); f.shard->tick(4, commit);
    auto replay = f.feed(data, 1, Delivery::Reliable, 2000002); EXPECT_EQ(reply_kind(replay), PacketKind::Ack);
    auto wrong = changed_packet(f.packets(data).front(), [](auto& h, auto&) { h.message_crc ^= 1; });
    EXPECT_EQ(reply_kind(f.shard->ingest(wrong, f.admission, 4000002)), PacketKind::Reject);
    f.shard->tick(5000000001ull, commit); EXPECT_EQ(f.budget->usage().receipts, 0u);
    replay = f.feed(data, 1, Delivery::Reliable, 6000000000ull); EXPECT_EQ(reply_kind(replay), PacketKind::Reject); f.shard->tick(6000000001ull, commit); EXPECT_EQ(commits, 2u);
}
TEST(SharedNetDedup, InProgressReliableIsNotEvictedByNewerBestEffort) {
    Limits limits; limits.stream_window = 4; ReassemblyFixture f(limits); auto data = flat_blob(2048); auto low = f.packets(data); unsigned commits = 0;
    auto commit = [&](const auto&, const auto&) { ++commits; return SubmitState::Committed; };
    f.shard->ingest(low.front(), f.admission, 1); f.feed(data, 10000, Delivery::BestEffort, 2); f.shard->tick(3, commit);
    EXPECT_EQ(f.shard->ingest(low.back(), f.admission, 4).disposition, ReceiveDisposition::CommitPending); f.shard->tick(5, commit); EXPECT_EQ(commits, 2u);
}
TEST(SharedNetDedup, CapacityDoesNotLruEvictOldStreamAndRetirementIsExplicit) {
    Limits limits; limits.streams = 1; ReassemblyFixture f(limits); auto data = flat_blob(40); unsigned commits = 0;
    auto commit = [&](const auto&, const auto&) { ++commits; return SubmitState::Committed; };
    f.feed(data, 1, Delivery::BestEffort); f.shard->tick(2, commit); f.shard->tick(10000000000ull, commit);
    auto another = f.packets(data, 1, 2); EXPECT_EQ(f.shard->ingest(another.front(), f.admission, 10000000001ull).disposition, ReceiveDisposition::Rejected);
    f.feed(data, 1, Delivery::BestEffort, 10000000002ull); f.shard->tick(10000000003ull, commit); EXPECT_EQ(commits, 1u);
    f.shard->retire_route(f.admission.subscriber); EXPECT_EQ(f.budget->usage().streams, 0u);
    f.shard->ingest(another.front(), f.admission, 10000000004ull); EXPECT_EQ(f.budget->usage().streams, 0u);
}
TEST(SharedNetDedup, TemporarilyExpiredPeerKeepsHistoryAcrossReturn) {
    ReassemblyFixture f; auto data = flat_blob(40); unsigned commits = 0; auto commit = [&](const auto&, const auto&) { ++commits; return SubmitState::Committed; };
    f.feed(data, 1, Delivery::BestEffort); f.shard->tick(2, commit); f.admission.peer->active.store(false); f.shard->tick(6000000000ull, commit);
    EXPECT_EQ(f.budget->usage().streams, 1u); f.admission.peer->active.store(true); f.feed(data, 1, Delivery::BestEffort, 6000000001ull); f.shard->tick(6000000002ull, commit); EXPECT_EQ(commits, 1u);
    f.shard->retire_peer_epoch(f.admission.peer); EXPECT_EQ(f.budget->usage().streams, 0u);
}

#include "shared_net/reassembly_fixture.h"
#include "gtest/gtest.h"
using namespace shared_net_test;
TEST(SharedNetQuota, GlobalPeerRouteLimitsAndRejectedReservationRollback) {
    Limits limits; limits.reassembly_bytes = 4096; limits.peer_reassembly_bytes = 3072; limits.route_reassembly_bytes = 2048;
    ReassemblyFixture a(limits), b, c; b.route("reassembly/b"); c.route("reassembly/c"); c.admission.peer->hello.gateway_id[0] = 77;
    const auto data = flat_blob(2048); auto first = a.packets(data).front(); a.shard->ingest(first, a.admission, 1);
    EXPECT_EQ(a.shard->ingest(a.packets(data, 2).front(), a.admission, 2).disposition, ReceiveDisposition::Rejected);
    EXPECT_EQ(a.shard->ingest(b.packets(data).front(), b.admission, 2).disposition, ReceiveDisposition::Rejected);
    EXPECT_EQ(a.shard->ingest(c.packets(data).front(), c.admission, 2).disposition, ReceiveDisposition::Accepted);
    EXPECT_EQ(a.budget->usage().bytes, 4096u); EXPECT_EQ(a.budget->usage().streams, 2u);
    a.shard->tick(5000000002ull, [](const auto&, const auto&) { ADD_FAILURE(); return SubmitState::Committed; });
    EXPECT_EQ(a.budget->usage().bytes, 0u); EXPECT_EQ(a.budget->usage().bitmap_bytes, 0u); EXPECT_EQ(a.budget->usage().assemblies, 0u); EXPECT_EQ(a.budget->usage().receipts, 0u);
}
TEST(SharedNetQuota, ReliableReceiptIsReservedBeforeAnyCommit) {
    Limits limits; limits.receipts = limits.peer_receipts = 1; ReassemblyFixture f(limits); auto data = flat_blob(40);
    EXPECT_EQ(f.feed(data).disposition, ReceiveDisposition::CommitPending);
    auto another = f.packets(data, 2, 2); EXPECT_EQ(f.shard->ingest(another.front(), f.admission, 1).disposition, ReceiveDisposition::Rejected);
    EXPECT_EQ(f.budget->usage().streams, 1u); EXPECT_EQ(f.budget->usage().receipts, 1u);
    unsigned commits = 0; f.shard->tick(2, [&](const auto&, const auto&) { ++commits; return SubmitState::Committed; }); EXPECT_EQ(commits, 1u);
}
TEST(SharedNetQuota, PendingLimitAndShutdownReleaseCapacityButRetainNeededHistory) {
    Limits limits; limits.commit_pending_bytes = 32; ReassemblyFixture f(limits);
    EXPECT_EQ(f.feed(flat_blob(40)).disposition, ReceiveDisposition::Rejected);
    EXPECT_EQ(f.budget->usage().bytes, 0u); EXPECT_EQ(f.budget->usage().pending_bytes, 0u); EXPECT_EQ(f.budget->usage().streams, 1u); EXPECT_EQ(f.budget->usage().receipts, 1u);
    f.shard.reset(); auto u = f.budget->usage(); EXPECT_EQ(u.streams, 0u); EXPECT_EQ(u.stream_bytes, 0u); EXPECT_EQ(u.receipts, 0u); EXPECT_EQ(u.assemblies, 0u);
}
TEST(SharedNetQuota, ShardsUseOneGlobalBudget) {
    Limits limits; limits.reassembly_bytes = 2048; auto budget = std::make_shared<ReassemblyBudget>(limits);
    ReassemblyFixture a, b;
    for (unsigned i = 0; route_hash(a.admission.publisher->descriptor.key) % 2 != 0; ++i) a.route("shard0/" + std::to_string(i));
    for (unsigned i = 0; route_hash(b.admission.publisher->descriptor.key) % 2 != 1; ++i) b.route("shard1/" + std::to_string(i));
    ReassemblyShard left(a.local, 3, 0, 2, budget), right(b.local, 3, 1, 2, budget);
    EXPECT_EQ(left.ingest(a.packets(flat_blob(2048)).front(), a.admission, 1).disposition, ReceiveDisposition::Accepted);
    EXPECT_EQ(right.ingest(b.packets(flat_blob(2048)).front(), b.admission, 1).disposition, ReceiveDisposition::Rejected);
    EXPECT_EQ(budget->usage().bytes, 2048u); EXPECT_EQ(budget->usage().streams, 1u);
}
TEST(SharedNetQuota, ActiveAssemblyAndReceiptDestructionReturnsEveryCharge) {
    ReassemblyFixture f; f.shard->ingest(f.packets(flat_blob(2048)).front(), f.admission, 1); f.feed(flat_blob(40), 2);
    EXPECT_EQ(f.budget->usage().assemblies, 2u); EXPECT_EQ(f.budget->usage().receipts, 2u); f.shard.reset();
    auto u = f.budget->usage(); EXPECT_EQ(u.bytes, 0u); EXPECT_EQ(u.bitmap_bytes, 0u); EXPECT_EQ(u.pending_bytes, 0u); EXPECT_EQ(u.assemblies, 0u); EXPECT_EQ(u.streams, 0u); EXPECT_EQ(u.receipts, 0u);
}

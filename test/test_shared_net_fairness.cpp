#include "shared_net/reliable_fixture.h"
#include "gtest/gtest.h"
using namespace shared_net_test;
TEST(SharedNetFairness, FirstFlightVisitsEveryFrozenTargetBeforeSecondFragment) {
    ReliableFixture f(1024 * 256); std::vector<ReliableTarget> targets(8, f.target);
    for (unsigned i = 0; i < targets.size(); ++i) {
        auto peer = std::make_shared<PeerAdmission>(); peer->hello = f.target.peer.admission->hello;
        peer->hello.gateway_id[0] = i + 20; peer->ipv4 = f.target.peer.admission->ipv4; targets[i].peer.admission = std::move(peer);
    }
    ReliableSession tx(f.header, targets, 5000000000ull); const auto batch = tx.batch(1, 32);
    ASSERT_EQ(batch.size(), 32u);
    for (unsigned i = 0; i < batch.size(); ++i) { EXPECT_EQ(batch[i].target, i % 8); EXPECT_EQ(batch[i].fragment, i / 8); }
    tx.accepted(17, 2); const auto suffix = tx.batch(3, 32); EXPECT_EQ(suffix.size(), 15u); EXPECT_EQ(suffix.front().target, 1u); EXPECT_EQ(suffix.front().fragment, 2u);
}
TEST(SharedNetFairness, NackStormIsCoalescedAndProbeBackoffIsBounded) {
    ReliableFixture f(1024 * 10); ReliableSession tx(f.header, {f.target}, 5000000000ull);
    auto first = tx.batch(1, 32); tx.accepted(first.size(), 1);
    auto nack = changed_packet(f.ack(), [](auto& h, auto& body) { h.kind = PacketKind::Nack; body = {0, 0, 0, 3}; });
    for (unsigned i = 0; i < 1000; ++i) tx.control(nack, 1000 + i);
    EXPECT_EQ(tx.stats().nacks, 1u); const auto retry = tx.batch(3000, 32); ASSERT_EQ(retry.size(), 1u); EXPECT_EQ(retry[0].fragment, 3u); EXPECT_TRUE(retry[0].retry);
    tx.accepted(1, 3000); EXPECT_TRUE(tx.batch(5000, 32).empty());
    const auto probe = tx.batch(2001000, 32); ASSERT_EQ(probe.size(), 1u); EXPECT_EQ(probe[0].fragment, 0u);
    tx.accepted(1, 2001000); EXPECT_TRUE(tx.batch(3000000, 32).empty());
}

#include "shared_net/reliable_fixture.h"
#include "gtest/gtest.h"
using namespace shared_net_test;
TEST(SharedNetReliable, FirstFlightLossFragmentLossAndAckLossRecoverWithoutSecondCommit) {
    for (unsigned scenario = 0; scenario < 3; ++scenario) {
        ReliableFixture f(1024 * 64 + 13); ReliableSession tx(f.header, {f.target}, 5000000000ull);
        unsigned commits = 0, ack_drops = 0; std::uint64_t random = 20261005;
        for (std::uint64_t now = 1; now < 5000000000ull && !tx.result(); now += 1000000) {
            const auto batch = tx.batch(now, 32); std::vector<ReceivedDatagram> packets;
            for (const auto& fragment : batch) packets.push_back(f.data(tx.header(fragment)));
            tx.accepted(batch.size(), now);
            auto reply = [&](ReceiveFeedback feedback) {
                if (feedback.control_packet.empty()) return;
                const auto kind = reply_kind(feedback);
                if (kind == PacketKind::Ack && scenario == 2 && !ack_drops++) return;
                tx.control(f.control(feedback.control_packet), now);
            };
            for (unsigned i = 0; i < packets.size(); ++i) {
                random = random * 6364136223846793005ull + 1;
                if ((scenario == 0 && !batch[i].retry) || (scenario == 1 && random % 100 < 5)) continue;
                reply(f.receiver.shard->ingest(packets[i], f.receiver.admission, now));
            }
            for (auto feedback : f.receiver.shard->tick(now, [&](const auto&, const WireBlob& blob) {
                ++commits; EXPECT_EQ(blob.size(), f.blob.size()); EXPECT_EQ(std::memcmp(blob.view().data, f.blob.data(), f.blob.size()), 0); return SubmitState::Committed;
            })) reply(std::move(feedback));
            tx.tick(now);
        }
        ASSERT_TRUE(tx.result()) << scenario; EXPECT_EQ(tx.result()->result, SendResultCode::Completed) << scenario;
        EXPECT_EQ(tx.result()->acked_count, 1u); EXPECT_EQ(commits, 1u); EXPECT_GT(tx.stats().retry_packets, 0u);
    }
}
TEST(SharedNetReliable, PartialBatchAndWouldBlockPreserveUnsentSuffixIdentity) {
    ReliableFixture f(1024 * 10); ReliableSession tx(f.header, {f.target}, 5000000000ull);
    const auto first = tx.batch(1, 8); ASSERT_EQ(first.size(), 8u); tx.accepted(0, 2);
    const auto same = tx.batch(3, 8); for (unsigned i = 0; i < first.size(); ++i) EXPECT_EQ(same[i].fragment, first[i].fragment);
    tx.accepted(3, 4); const auto suffix = tx.batch(5, 8); ASSERT_EQ(suffix.size(), 5u);
    for (unsigned i = 0; i < suffix.size(); ++i) EXPECT_EQ(suffix[i].fragment, i + 3);
    tx.accepted(5, 6); const auto last = tx.batch(7, 8); ASSERT_EQ(last.size(), 2u); EXPECT_EQ(last[0].fragment, 8u);
    tx.accepted(2, 8); EXPECT_TRUE(tx.batch(100, 8).empty()); EXPECT_FALSE(tx.batch(2000008, 8).empty());
    EXPECT_EQ(tx.stats().original_packets, 10u);
}
TEST(SharedNetReliable, WrongAckIdentityMetadataAddressAndPortCannotFinish) {
    ReliableFixture f; ReliableSession tx(f.header, {f.target}, 5000000000ull);
    auto batch = tx.batch(1, 32); tx.accepted(batch.size(), 1);
    const std::vector<std::function<void(WireHeader&, Bytes&)>> changes = {
        [](auto& h, auto&) { ++h.source_epoch; }, [](auto& h, auto&) { ++h.target_epoch; },
        [](auto& h, auto&) { ++h.sequence; }, [](auto& h, auto&) { ++h.receiver_route_epoch; },
        [](auto& h, auto&) { ++h.schema_hash; }, [](auto& h, auto&) { ++h.message_crc; },
        [](auto& h, auto&) { ++h.publisher_id[0]; }, [](auto& h, auto&) { ++h.route.msg_id; }
    };
    for (const auto& change : changes) { tx.control(changed_packet(f.ack(), change), 2); EXPECT_FALSE(tx.result()); }
    auto wrong = f.ack(); ++wrong.source.port; tx.control(wrong, 2); EXPECT_FALSE(tx.result());
    wrong = f.ack(); ++wrong.source.host; tx.control(wrong, 2); EXPECT_FALSE(tx.result());
    EXPECT_EQ(tx.stats().ignored_controls, changes.size() + 2);
    tx.control(f.ack(), 3); ASSERT_TRUE(tx.result()); EXPECT_EQ(tx.result()->result, SendResultCode::Completed);
    tx.tick(6000000000ull); EXPECT_EQ(tx.result()->result, SendResultCode::Completed);
}
TEST(SharedNetReliable, TimeoutWithNoAckStillReportsPossibleDeliveryAndTerminalIsSingle) {
    ReliableFixture f; ReliableSession tx(f.header, {f.target}, 100);
    auto batch = tx.batch(1, 32); tx.accepted(batch.size(), 1); tx.control(f.ack(), 100);
    ASSERT_TRUE(tx.result()); EXPECT_EQ(tx.result()->result, SendResultCode::TimedOut);
    EXPECT_EQ(tx.result()->acked_count, 0u); EXPECT_TRUE(tx.result()->possible_remote_delivery);
    tx.control(f.ack(), 101); EXPECT_EQ(tx.result()->result, SendResultCode::TimedOut);
    ReliableSession empty(f.header, {}, 100); empty.tick(1); ASSERT_TRUE(empty.result()); EXPECT_EQ(empty.result()->result, SendResultCode::NoSubscribers);
    ReliableSession expired(f.header, {f.target}, 1); EXPECT_TRUE(expired.batch(1, 32).empty()); EXPECT_FALSE(expired.result()->possible_remote_delivery);
}
TEST(SharedNetReliable, FrozenTargetsDoNotShrinkOnPeerLossAndRestart) {
    ReliableFixture f; auto other = f.target; other.peer.admission = std::make_shared<PeerAdmission>();
    other.peer.admission->hello = f.target.peer.admission->hello; other.peer.admission->hello.gateway_id[0] = 17; other.peer.admission->ipv4 = f.target.peer.admission->ipv4;
    ReliableSession tx(f.header, {f.target, other}, 5000000000ull);
    auto batch = tx.batch(1, 32); tx.accepted(batch.size(), 1); tx.control(f.ack(), 2); EXPECT_FALSE(tx.result());
    other.peer.admission->retired.store(true); other.peer.admission->active.store(false); tx.tick(3);
    ASSERT_TRUE(tx.result()); EXPECT_EQ(tx.result()->target_count, 2u); EXPECT_EQ(tx.result()->acked_count, 1u);
    EXPECT_EQ(tx.result()->result, SendResultCode::PeerRestarted); EXPECT_TRUE(tx.result()->possible_remote_delivery);
}
TEST(SharedNetReliable, PublisherRoleChangeDoesNotCancelUnchangedSubscriberTarget) {
    ReliableFixture f; auto budget = std::make_shared<DirectoryBudget>();
    auto sub = f.target.route->descriptor; auto first = budget->replace({sub}, 1); ASSERT_TRUE(first);
    f.target.peer.snapshot = first; f.target.route = first->routes.at(sub.key);
    ReliableSession tx(f.header, {f.target}, 5000000000ull); const auto batch = tx.batch(1, 32); tx.accepted(batch.size(), 1);
    sub.role_flags = 3; auto second = budget->replace({sub}, 2, first); ASSERT_TRUE(second);
    tx.tick(2); EXPECT_FALSE(tx.result()); EXPECT_TRUE(f.target.route->subscriber_active.load());
    tx.control(f.ack(), 3); ASSERT_TRUE(tx.result()); EXPECT_EQ(tx.result()->result, SendResultCode::Completed);
}

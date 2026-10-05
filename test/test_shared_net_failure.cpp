#include "shared_net/public_fixture.h"
#include "shared_net/reassembly_fixture.h"
#include "gtest/gtest.h"
using namespace shared_net_test;

TEST(SharedNetFailure, KilledGatewayPreservesLocalDeliveryAndBorrowedSample) {
    PublicFixture f; f.gateway->stop(); GatewayProcess gateway(f.config);
    ASSERT_TRUE(until([&] { return std::filesystem::exists(public_directory().control()); }));
    dzIPC::shared_net::Subscriber sub(f.model(), f.topic.descriptor.topic, 0, 8); sub.InitChannel();
    dzIPC::shared_net::Publisher pub(f.model(), f.topic.descriptor.topic, 0); pub.InitChannel();
    ASSERT_TRUE(pub.publish(f.message())); dzIPC::Sample held; ASSERT_TRUE(sub.get(held, 1000));
    Bytes original(static_cast<const std::uint8_t*>(held.data()), static_cast<const std::uint8_t*>(held.data()) + held.size());
    gateway.stop_now();
    ASSERT_TRUE(pub.publish(f.message())); dzIPC::Sample next; ASSERT_TRUE(sub.get(next, 1000));
    EXPECT_EQ(std::memcmp(held.data(), original.data(), original.size()), 0);
    EXPECT_FALSE(pub.publish_blocking(f.message(), 1000)); ASSERT_TRUE(sub.get(next, 1000));
    EXPECT_FALSE(sub.try_get(next));
}
TEST(SharedNetFailure, LifecycleEdgesNeverAckBeforeCommitOrCommitTwiceAfterAckLoss) {
    ReassemblyFixture f; const auto packets = f.packets(flat_blob(4096)); unsigned commits = 0;
    for (std::size_t i = 0; i + 1 < packets.size(); ++i) f.shard->ingest(packets[i], f.admission, 1);
    auto commit = [&](const auto&, const auto&) { ++commits; return SubmitState::Committed; };
    EXPECT_TRUE(f.shard->tick(2, commit).empty()); EXPECT_EQ(commits, 0u);
    ASSERT_EQ(f.shard->ingest(packets.back(), f.admission, 3).disposition, ReceiveDisposition::CommitPending);
    EXPECT_EQ(commits, 0u);
    auto replies = f.shard->tick(4, commit); ASSERT_EQ(replies.size(), 1u); EXPECT_EQ(reply_kind(replies[0]), PacketKind::Ack);
    // 丢弃已生成的 ACK，再重放全部 DATA。
    for (const auto& packet : packets) f.shard->ingest(packet, f.admission, 5);
    f.shard->tick(6, commit); EXPECT_EQ(commits, 1u);
    f.admission.subscriber->active.store(false); f.shard->retire_route(f.admission.subscriber);
    for (const auto& packet : packets) EXPECT_EQ(f.shard->ingest(packet, f.admission, 7).disposition, ReceiveDisposition::Rejected);
    EXPECT_EQ(commits, 1u);
}
TEST(SharedNetFailure, ClosingSubscriberWakesAndJoinsEnteredGetter) {
    PublicFixture f;
    auto sub = std::make_unique<dzIPC::shared_net::Subscriber>(f.model(), f.topic.descriptor.topic, 0, 8); sub->InitChannel();
    auto* raw = sub.get(); std::promise<void> entered;
    auto getter = std::async(std::launch::async, [&] { entered.set_value(); dzIPC::Sample sample; return raw->get(sample, 5000); });
    entered.get_future().wait(); std::this_thread::sleep_for(10ms);
    sub.reset(); ASSERT_EQ(getter.wait_for(500ms), std::future_status::ready); EXPECT_FALSE(getter.get());
}
TEST(SharedNetFailure, CancellableQueueNeverMissesPublicationAndWakesAllOnCancel) {
    CircularQueue<int> queue(8); queue.enable_cancellable_wait();
    std::atomic<unsigned> consumed{0};
    auto reader = std::async(std::launch::async, [&] {
        for (unsigned i = 0; i < 1000; ++i) {
            std::shared_ptr<int> item;
            if (!queue.pop_cancellable(item, 1000) || !item || *item != static_cast<int>(i)) return false;
            ++consumed;
        }
        return true;
    });
    for (unsigned i = 0; i < 1000; ++i) {
        queue.push(std::make_shared<int>(i));
        ASSERT_TRUE(until([&] { return consumed.load() == i + 1; }));
    }
    ASSERT_TRUE(reader.get());
    std::atomic<unsigned> entered{0}; std::vector<std::future<bool>> waiters;
    for (unsigned i = 0; i < 8; ++i) waiters.push_back(std::async(std::launch::async, [&] {
        ++entered; std::shared_ptr<int> item; return queue.pop_cancellable(item, UINT64_MAX);
    }));
    ASSERT_TRUE(until([&] { return entered.load() == 8; })); queue.cancel_waits();
    for (auto& waiter : waiters) { ASSERT_EQ(waiter.wait_for(500ms), std::future_status::ready); EXPECT_FALSE(waiter.get()); }
    queue.push(std::make_shared<int>(7)); std::shared_ptr<int> item;
    EXPECT_FALSE(queue.pop_cancellable(item, 0));
}

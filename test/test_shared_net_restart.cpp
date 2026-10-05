#include "shared_net/public_fixture.h"
#include "gtest/gtest.h"
using namespace shared_net_test;
TEST(SharedNetRestart, NewGatewayRequiresNewIdentityOldHandlesRemainLocalOnly) {
    PublicFixture f;
    dzIPC::shared_net::Subscriber old_sub(f.model(), f.topic.descriptor.topic, 0, 8); old_sub.InitChannel();
    dzIPC::shared_net::Publisher old_pub(f.model(), f.topic.descriptor.topic, 0); old_pub.InitChannel();
    auto old = ClientRuntime::acquire(f.config.control_path); const auto epoch = old->gateway_epoch();
    f.gateway->stop(); f.gateway = std::make_unique<GatewayRuntime>(f.config);
    auto next = ClientRuntime::acquire(f.config.control_path); EXPECT_NE(next->gateway_epoch(), epoch);
    EXPECT_FALSE(old->healthy()); EXPECT_NE(next->session_id(), 0u);
    ASSERT_TRUE(old_pub.publish(f.message())); dzIPC::Sample sample; ASSERT_TRUE(old_sub.get(sample, 1000));
    EXPECT_FALSE(old_pub.publish_blocking(f.message(), 100)); ASSERT_TRUE(old_sub.get(sample, 1000));
    EXPECT_NE(next->status().find("\"outbox_records\":\"0\""), std::string::npos);
    dzIPC::shared_net::Publisher pub(f.model(), f.topic.descriptor.topic, 0); pub.InitChannel();
    ASSERT_TRUE(pub.publish_blocking(f.message(), 1000)); ASSERT_TRUE(old_sub.get(sample, 1000));
    EXPECT_FALSE(old_sub.try_get(sample));
}
TEST(SharedNetRestart, ConcurrentResetAndPublishLeaveNoMixedDescriptor) {
    PublicFixture f; dzIPC::shared_net::Subscriber sub(f.model(), f.topic.descriptor.topic, 0, 256); sub.InitChannel();
    dzIPC::shared_net::Publisher pub(f.model(), f.topic.descriptor.topic, 0); pub.InitChannel();
    std::atomic<bool> done{false}; std::atomic<unsigned> calls{0};
    auto writer = std::async(std::launch::async, [&] { while (!done.load()) { pub.publish(f.message()); ++calls; std::this_thread::yield(); } });
    for (unsigned i = 0; i < 20; ++i) { pub.reset_message(f.model()); sub.reset_message(f.model()); }
    done.store(true); writer.get(); EXPECT_GT(calls.load(), 0u);
    f.gateway->stop(); EXPECT_THROW(pub.reset_message(f.model(72)), std::exception);
    EXPECT_FALSE(pub.publish(f.message())); EXPECT_FALSE(pub.publish(f.message(72)));
}

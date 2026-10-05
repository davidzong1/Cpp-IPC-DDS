#include "shared_net/public_fixture.h"
#include "gtest/gtest.h"
using namespace shared_net_test;
TEST(SharedNetPublicApi, FactoryCoversPublishVariantsSampleCloneAndWait) {
    PublicFixture f; auto model = f.model();
    dzIPC::pimpl::subscriber_ipc_impl sub(model, f.topic.descriptor.topic, 0, 8, dzIPC::IPCType::Socket); sub.InitChannel();
    dzIPC::pimpl::publisher_ipc_impl pub(model, f.topic.descriptor.topic, 0, dzIPC::IPCType::Socket); pub.InitChannel();
    ASSERT_TRUE(until([&] { return pub.has_subscribed(); }));
    auto message = f.message();
    for (unsigned variant = 0; variant < 4; ++variant) {
        const bool sent = variant == 0 ? pub.publish(message) : variant == 1 ? pub.publish_best_effort(message) : variant == 2 ? pub.publish_for_sniffer(message) : pub.publish_blocking(message, 1000);
        ASSERT_TRUE(sent); dzIPC::Sample sample; ASSERT_TRUE(sub.get(sample, 1000));
        const auto view = sample.view<dzIPC::Msg::StdImageFlat>(); ASSERT_TRUE(view.valid()); EXPECT_EQ(view.data().size(), message->data.size());
        EXPECT_EQ(std::memcmp(view.data().data(), message->data.data(), message->data.size()), 0); EXPECT_FALSE(sub.try_get(sample));
    }
    dzIPC::EnableDzFlat(false); ASSERT_TRUE(pub.publish(message)); auto clone = f.model();
    ASSERT_TRUE(until([&] { return sub.try_get_clone(clone); })); EXPECT_EQ(clone->topic()->msgcast<dzIPC::Msg::StdImage>()->data, message->data);
    dzIPC::Sample empty; EXPECT_FALSE(sub.get(empty, 1));
}
TEST(SharedNetPublicApi, ResetCancelsOldGetterAndCanRebuildSameDescriptor) {
    PublicFixture f; auto model = f.model();
    dzIPC::shared_net::Subscriber sub(model, f.topic.descriptor.topic, 0, 8); sub.InitChannel();
    dzIPC::shared_net::Publisher pub(model, f.topic.descriptor.topic, 0); pub.InitChannel();
    auto waiting = std::async(std::launch::async, [&] { dzIPC::Sample sample; return sub.get(sample, 5000); });
    std::this_thread::sleep_for(5ms); sub.reset_message(model); ASSERT_EQ(waiting.wait_for(500ms), std::future_status::ready); EXPECT_FALSE(waiting.get());
    pub.reset_message(model); ASSERT_TRUE(pub.publish(f.message())); dzIPC::Sample sample; ASSERT_TRUE(sub.get(sample, 1000)); EXPECT_TRUE(sample.valid());
    pub.InitChannel(); ASSERT_TRUE(pub.publish(f.message())); ASSERT_TRUE(sub.get(sample, 1000));
}
TEST(SharedNetPublicApi, FailedTypeResetLeavesEntirePublisherUnavailable) {
    PublicFixture f; auto model = f.model(); dzIPC::shared_net::Subscriber sub(model, f.topic.descriptor.topic, 0, 8); sub.InitChannel();
    dzIPC::shared_net::Publisher pub(model, f.topic.descriptor.topic, 0); pub.InitChannel();
    EXPECT_THROW(pub.reset_message(f.model(72)), std::runtime_error); EXPECT_TRUE(pub.exit_flag.load());
    EXPECT_FALSE(pub.publish(f.message(72))); EXPECT_FALSE(pub.has_subscribed());
    dzIPC::Sample sample; EXPECT_FALSE(sub.get(sample, 10));
    pub.reset_message(model); EXPECT_TRUE(pub.publish(f.message())); EXPECT_TRUE(sub.get(sample, 1000));
}
TEST(SharedNetPublicApi, OfflineResetClosesBothLegsAndExistingSubscriberRemainsLocal) {
    PublicFixture f; auto model = f.model(); dzIPC::shared_net::Subscriber sub(model, f.topic.descriptor.topic, 0, 8); sub.InitChannel();
    dzIPC::shared_net::Publisher pub(model, f.topic.descriptor.topic, 0); pub.InitChannel();
    f.gateway->stop(); EXPECT_TRUE(pub.publish(f.message())); dzIPC::Sample sample; EXPECT_TRUE(sub.get(sample, 1000));
    EXPECT_THROW(pub.reset_message(f.model(72)), std::exception); EXPECT_TRUE(pub.exit_flag.load()); EXPECT_FALSE(pub.publish(f.message(72)));
    EXPECT_FALSE(sub.get(sample, 10));
}

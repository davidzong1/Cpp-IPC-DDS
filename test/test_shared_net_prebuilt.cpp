#include "shared_net/public_fixture.h"
#include "gtest/gtest.h"
using namespace shared_net_test;
TEST(SharedNetPrebuilt, LargeCopyIsVisibleImmediatelyAfterSharedCommit) {
    PublicFixture f; dzIPC::shared_net::Subscriber sub(f.model(), f.topic.descriptor.topic, 0, 8); sub.InitChannel();
    dzIPC::shared_net::Publisher pub(f.topic.generic(), f.topic.descriptor.topic, 0); pub.InitChannel();
    auto message = f.message(); message->data.resize(1024 * 1024 + 37);
    for (unsigned turn = 0; turn < 8; ++turn) {
        for (std::size_t i = 0; i < message->data.size(); ++i) message->data[i] = (i * 7 + turn) & 255;
        WireBlob blob; ASSERT_TRUE(WireEncoder::encode(*message, true, blob));
        auto received = std::async(std::launch::async, [&] {
            dzIPC::Sample sample;
            return sub.get(sample, 1000) && sample.size() == blob.size() && std::memcmp(sample.data(), blob.view().data, blob.size()) == 0;
        });
        ASSERT_TRUE(pub.publish_prebuilt_segment(blob.view().data, blob.size()));
        EXPECT_TRUE(received.get());
    }
}
TEST(SharedNetPrebuilt, GenericPublisherUsesCallerSchemaWithoutTlvRoundTrip) {
    PublicFixture f; dzIPC::shared_net::Subscriber sub(f.model(), f.topic.descriptor.topic, 0, 8); sub.InitChannel();
    dzIPC::shared_net::Publisher pub(f.topic.generic(), f.topic.descriptor.topic, 0); pub.InitChannel();
    auto message = f.message(); WireBlob blob; ASSERT_TRUE(WireEncoder::encode(*message, true, blob));
    ASSERT_TRUE(pub.publish_prebuilt_segment(blob.view().data, blob.size())); dzIPC::Sample sample; ASSERT_TRUE(sub.get(sample, 1000));
    ASSERT_EQ(sample.size(), blob.size()); EXPECT_EQ(std::memcmp(sample.data(), blob.view().data, blob.size()), 0);
    EXPECT_FALSE(sub.try_get(sample));
}
TEST(SharedNetPrebuilt, InvalidSegmentAllowsExactlyOneFallbackAndOfflineCommitDoesNot) {
    PublicFixture f; dzIPC::shared_net::Subscriber sub(f.model(), f.topic.descriptor.topic, 0, 8); sub.InitChannel();
    dzIPC::shared_net::Publisher pub(f.topic.generic(), f.topic.descriptor.topic, 0); pub.InitChannel();
    Bytes invalid(48); EXPECT_FALSE(pub.publish_prebuilt_segment(invalid.data(), invalid.size()));
    ASSERT_TRUE(pub.publish(f.message())); dzIPC::Sample sample; ASSERT_TRUE(sub.get(sample, 1000)); EXPECT_FALSE(sub.try_get(sample));
    WireBlob blob; auto message = f.message(); ASSERT_TRUE(WireEncoder::encode(*message, true, blob));
    f.gateway->stop(); EXPECT_TRUE(pub.publish_prebuilt_segment(blob.view().data, blob.size()));
    ASSERT_TRUE(sub.get(sample, 1000)); ASSERT_EQ(sample.size(), blob.size()); EXPECT_EQ(std::memcmp(sample.data(), blob.view().data, blob.size()), 0); EXPECT_FALSE(sub.try_get(sample));
}

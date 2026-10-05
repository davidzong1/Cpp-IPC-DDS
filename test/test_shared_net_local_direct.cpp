#include "ipc_msg/std_msgs/std_image.hpp"
#include "shared_net/shm_wire_fixture.h"
#include "gtest/gtest.h"
#include <future>
using namespace shared_net_test;
namespace
{
class CountingImage : public dzIPC::Msg::StdImage
{
  public:
    unsigned serializations = 0, encodes = 0;
    bool fail_encode = false, throw_encode = false;
    std::promise<void> *entered = nullptr;
    std::shared_future<void> release;
    ipc::buffer serialize() override
    {
        ++serializations;
        return StdImage::serialize();
    }
    bool dzflat_write(void *pointer, std::uint32_t capacity) const override
    {
        auto *self = const_cast<CountingImage *>(this);
        ++self->encodes;
        if (entered)
        {
            entered->set_value();
            release.wait();
        }
        if (throw_encode)
            throw std::runtime_error("编码故障注入");
        return !fail_encode && StdImage::dzflat_write(pointer, capacity);
    }
};
} // namespace
TEST(SharedNetLocalDirect, DirectDzFlatEncodingAndTlvReachNormalSubscriber)
{
    BusinessTopic topic;
    ShmWireWriter writer(topic.descriptor);
    auto model = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 71);
    dzIPC::shm::shm_sub_ipc sub(model, topic.descriptor.topic, 0, 8);
    sub.InitChannel();
    ASSERT_TRUE(until([&] { return writer.has_subscribers(); }));
    CountingImage msg;
    msg.set_msg_id(71);
    msg.width = 4;
    msg.height = 4;
    msg.data.assign(48, 0x77);
    EXPECT_EQ(writer.try_commit_local(msg), SubmitState::Committed);
    EXPECT_EQ(msg.encodes, 1u);
    EXPECT_EQ(msg.serializations, 0u);
    dzIPC::Sample sample;
    ASSERT_TRUE(until([&] { return sub.try_get(sample); }));
    ASSERT_TRUE(sample.valid());
    const auto expected = msg.data;
    auto view = sample.view<dzIPC::Msg::StdImageFlat>();
    ASSERT_TRUE(view.valid());
    EXPECT_EQ(view.data().size(), expected.size());
    EXPECT_EQ(std::memcmp(view.data().data(), expected.data(), expected.size()), 0);
    // 同一类型强制 TLV 路径，普通订阅者按原解码规则接收。
    EXPECT_EQ(writer.try_commit_local(msg, false), SubmitState::Committed);
    EXPECT_EQ(msg.serializations, 1u);
    auto result = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 71);
    ASSERT_TRUE(until([&] { return sub.try_get_clone(result); }));
    EXPECT_EQ(result->topic()->msgcast<dzIPC::Msg::StdImage>()->data, expected);
    const auto copy = Bytes(static_cast<const std::uint8_t *>(sample.data()),
                            static_cast<const std::uint8_t *>(sample.data()) + sample.size());
    writer.close_after_quiescent();
    EXPECT_EQ(std::memcmp(sample.data(), copy.data(), copy.size()), 0);
}
TEST(SharedNetLocalDirect, LocalSuccessNetworkFailureDoesNotAuthorizeFallback)
{
    BusinessTopic topic;
    ShmWireWriter writer(topic.descriptor);
    ipc::mpmc_channel rx(topic.segment().c_str(), ipc::receiver, false);
    const auto bytes = flat_blob(40);
    const auto local = writer.try_commit_prebuilt(ByteView(bytes));
    EXPECT_EQ(local, SubmitState::Committed);
    EXPECT_TRUE(best_effort_result(local, SubmitState::NotSubmitted));
    auto sample = rx.try_recv();
    ASSERT_EQ(sample.size(), bytes.size());
    EXPECT_EQ(std::memcmp(sample.data(), bytes.data(), bytes.size()), 0);
    EXPECT_TRUE(rx.try_recv().empty());
    EXPECT_EQ(writer.try_commit_prebuilt(ByteView(bytes), local::monotonic_ns()),
              SubmitState::NotSubmitted);
    EXPECT_TRUE(rx.try_recv().empty());
}
TEST(SharedNetLocalDirect, EncodeFailureNeverFallsBackAndCloseWaitsForActiveCall)
{
    BusinessTopic topic;
    ShmWireWriter writer(topic.descriptor);
    ipc::mpmc_channel rx(topic.segment().c_str(), ipc::receiver, false);
    CountingImage msg;
    msg.set_msg_id(71);
    msg.data.assign(48, 0x55);
    msg.fail_encode = true;
    EXPECT_EQ(writer.try_commit_local(msg), SubmitState::NotSubmitted);
    EXPECT_EQ(msg.serializations, 0u);
    EXPECT_TRUE(rx.try_recv().empty());
    msg.fail_encode = false;
    msg.throw_encode = true;
    EXPECT_EQ(writer.try_commit_local(msg), SubmitState::NotSubmitted);
    EXPECT_TRUE(rx.try_recv().empty());
    msg.throw_encode = false;
    std::promise<void> entered, release;
    msg.entered = &entered;
    msg.release = release.get_future().share();
    auto publishing = std::async(std::launch::async, [&] { return writer.try_commit_local(msg); });
    entered.get_future().wait();
    auto closing = std::async(std::launch::async, [&] { writer.close_after_quiescent(); });
    EXPECT_EQ(closing.wait_for(10ms), std::future_status::timeout);
    release.set_value();
    EXPECT_EQ(publishing.get(), SubmitState::Committed);
    closing.get();
    EXPECT_FALSE(writer.has_subscribers());
}

TEST(SharedNetLocalDirect, PoolExhaustionAndForkAreDefinitelyNotSubmitted)
{
    BusinessTopic topic;
    ShmWireWriter writer(topic.descriptor);
    ipc::mpmc_channel rx(topic.segment().c_str(), ipc::receiver, false);
    const auto bytes = flat_blob(40);
    for (unsigned i = 0; i < 10; ++i)
        ASSERT_EQ(writer.try_commit_prebuilt(ByteView(bytes)), SubmitState::Committed);
    EXPECT_EQ(writer.try_commit_prebuilt(ByteView(bytes)), SubmitState::NotSubmitted);
    for (unsigned i = 0; i < 10; ++i)
    {
        auto sample = rx.try_recv();
        ASSERT_EQ(sample.size(), bytes.size());
        EXPECT_EQ(std::memcmp(sample.data(), bytes.data(), bytes.size()), 0);
    }
    EXPECT_TRUE(rx.try_recv().empty());
    const auto pid = fork();
    ASSERT_GE(pid, 0);
    if (!pid)
        _exit(writer.try_commit_prebuilt(ByteView(bytes)) == SubmitState::NotSubmitted ? 0 : 1);
    ChildGuard child(pid);
    int state;
    ASSERT_EQ(waitpid(pid, &state, 0), pid);
    child.pid = -1;
    ASSERT_TRUE(WIFEXITED(state));
    EXPECT_EQ(WEXITSTATUS(state), 0);
    EXPECT_TRUE(rx.try_recv().empty());
}

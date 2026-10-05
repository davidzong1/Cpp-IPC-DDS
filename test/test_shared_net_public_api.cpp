#include "shared_net/public_fixture.h"
#include "dzIPC/detail/shm_sub_seam.h"
#include "gtest/gtest.h"
using namespace shared_net_test;
namespace {
struct AssistGate {
    static AssistGate* current;
    static thread_local int slot;
    std::atomic<bool> armed{false}, blocked{false}, release{false};
    std::atomic<bool> pause_dispatch{false}, at_dispatch{false}, continue_dispatch{false};
    std::atomic<unsigned> assisted{0}, enqueued{0};
    std::array<std::atomic<bool>, 4> waiting{};
    AssistGate() {
        current = this;
        dzIPC::detail::SetSeamHook([](const dzIPC::detail::SeamEvent& event) {
            auto* gate = current;
            if (event.point == dzIPC::detail::SeamPoint::kCallerAssistedReceive) ++gate->assisted;
            if (event.point == dzIPC::detail::SeamPoint::kBeforeViewEnqueue) ++gate->enqueued;
            if (event.point == dzIPC::detail::SeamPoint::kBeforeCallerWait && slot >= 0) gate->waiting[slot].store(true);
            if (event.point == dzIPC::detail::SeamPoint::kAfterRecvRelease && slot >= 0 &&
                event.size != 0 && gate->pause_dispatch.load()) {
                gate->at_dispatch.store(true);
                const auto end = std::chrono::steady_clock::now() + 2s;
                while (!gate->continue_dispatch.load() && std::chrono::steady_clock::now() < end)
                    std::this_thread::sleep_for(1ms);
            }
            if (event.point == dzIPC::detail::SeamPoint::kBeforeWorkerReceive && gate->armed.load()) {
                gate->blocked.store(true);
                const auto end = std::chrono::steady_clock::now() + 2s;
                while (!gate->release.load() && std::chrono::steady_clock::now() < end) std::this_thread::sleep_for(1ms);
            }
        });
    }
    ~AssistGate() { release.store(true); continue_dispatch.store(true); dzIPC::detail::SetSeamHook(nullptr); }
};
AssistGate* AssistGate::current = nullptr;
thread_local int AssistGate::slot = -1;
}

TEST(SharedNetPublicApi, CallerReceivesWhileWorkerIsPausedBeforeConsumeLock) {
    PublicFixture f; AssistGate gate;
    dzIPC::shared_net::Subscriber sub(f.model(), f.topic.descriptor.topic, 0, 8); sub.InitChannel();
    dzIPC::shared_net::Publisher pub(f.model(), f.topic.descriptor.topic, 0); pub.InitChannel();
    gate.armed.store(true);
    ASSERT_TRUE(pub.publish(f.message()));
    ASSERT_TRUE(until([&] { return gate.blocked.load(); }));
    dzIPC::Sample sample;
    EXPECT_TRUE(sub.get(sample, 100));
    EXPECT_GT(gate.assisted.load(), 0u);
    gate.release.store(true);
    ASSERT_TRUE(sample.valid());
    EXPECT_EQ(sample.view<dzIPC::Msg::StdImageFlat>().data().size(), 64u);
    EXPECT_FALSE(sub.try_get(sample));
}

TEST(SharedNetPublicApi, BackgroundBufferAndEvictionRemainActiveWithoutGetters) {
    PublicFixture f; AssistGate gate;
    dzIPC::shared_net::Subscriber sub(f.model(), f.topic.descriptor.topic, 0, 2); sub.InitChannel();
    dzIPC::shared_net::Publisher pub(f.model(), f.topic.descriptor.topic, 0); pub.InitChannel();
    for (unsigned i = 0; i < 6; ++i) {
        auto message = f.message(); message->data[0] = i;
        ASSERT_TRUE(pub.publish(message));
        ASSERT_TRUE(until([&] { return gate.enqueued.load() >= i + 1; }));
    }
    // 等最后一次push完成，入队前打点本身不能当成队列提交屏障。
    std::this_thread::sleep_for(5ms);
    EXPECT_EQ(gate.assisted.load(), 0u);
    for (unsigned expected : {4u, 5u}) {
        dzIPC::Sample sample; ASSERT_TRUE(sub.get(sample, 1000));
        EXPECT_EQ(sample.view<dzIPC::Msg::StdImageFlat>().data()[0], expected);
    }
    dzIPC::Sample sample; EXPECT_FALSE(sub.try_get(sample));
}

TEST(SharedNetPublicApi, AssistedViewGetterDrainsTlvBeforeQueuedViewWithoutNewPublish) {
    PublicFixture f; AssistGate gate;
    dzIPC::shared_net::Subscriber sub(f.model(), f.topic.descriptor.topic, 0, 8); sub.InitChannel();
    dzIPC::shared_net::Publisher pub(f.model(), f.topic.descriptor.topic, 0); pub.InitChannel();
    gate.armed.store(true);
    dzIPC::EnableDzFlat(false);
    const bool tlv_sent = pub.publish(f.message());
    dzIPC::EnableDzFlat(true);
    ASSERT_TRUE(tlv_sent);
    ASSERT_TRUE(until([&] { return gate.blocked.load(); }));
    ASSERT_TRUE(pub.publish(f.message()));
    dzIPC::Sample sample; EXPECT_TRUE(sub.get(sample, 100));
    auto clone = f.model(); EXPECT_TRUE(sub.try_get_clone(clone));
    EXPECT_EQ(clone->topic()->msgcast<dzIPC::Msg::StdImage>()->data.size(), 64u);
    EXPECT_GE(gate.assisted.load(), 2u);
    gate.release.store(true);
}

TEST(SharedNetPublicApi, ConcurrentGettersDeliverEachSequenceOnceAndCancelInfiniteWait) {
    PublicFixture f; AssistGate gate;
    dzIPC::shared_net::Subscriber sub(f.model(), f.topic.descriptor.topic, 0, 8); sub.InitChannel();
    dzIPC::shared_net::Publisher pub(f.model(), f.topic.descriptor.topic, 0); pub.InitChannel();
    constexpr unsigned count = 200;
    std::array<std::atomic<unsigned>, count> seen{};
    std::atomic<unsigned> received{0}, entered{0}, bad{0}; std::atomic<bool> done{false}; std::vector<std::future<void>> readers;
    for (unsigned i = 0; i < 4; ++i) readers.push_back(std::async(std::launch::async, [&] {
        ++entered;
        while (!done.load()) {
            dzIPC::Sample sample;
            if (!sub.get(sample, 50)) continue;
            auto view = sample.view<dzIPC::Msg::StdImageFlat>();
            if (!view.valid() || !view.data().size() || view.data()[0] >= count) { ++bad; continue; }
            ++seen[view.data()[0]]; ++received;
        }
    }));
    EXPECT_TRUE(until([&] { return entered.load() == 4; }));
    bool sent = true;
    for (unsigned i = 0; i < count; ++i) {
        auto message = f.message(); message->data[0] = i;
        if (!pub.publish(message) || !until([&] { return received.load() >= i + 1; })) { sent = false; break; }
    }
    done.store(true);
    for (auto& reader : readers) { EXPECT_EQ(reader.wait_for(500ms), std::future_status::ready); reader.get(); }
    EXPECT_TRUE(sent); EXPECT_EQ(bad.load(), 0u); EXPECT_EQ(received.load(), count);
    for (auto& value : seen) EXPECT_EQ(value.load(), 1u);
    std::vector<std::future<bool>> infinite;
    for (unsigned i = 0; i < 4; ++i) infinite.push_back(std::async(std::launch::async, [&, i] {
        AssistGate::slot = i; dzIPC::Sample sample; return sub.get(sample, UINT64_MAX);
    }));
    EXPECT_TRUE(until([&] { return std::all_of(gate.waiting.begin(), gate.waiting.end(), [](const auto& flag) { return flag.load(); }); }));
    sub.reset_message(f.model());
    for (auto& reader : infinite) { EXPECT_EQ(reader.wait_for(500ms), std::future_status::ready); EXPECT_FALSE(reader.get()); }
    ASSERT_TRUE(pub.publish(f.message())); dzIPC::Sample sample; EXPECT_TRUE(sub.get(sample, 1000));
}

TEST(SharedNetPublicApi, WaitingLeaseReleasesAcrossUnderlyingShmGenerationRebuild) {
    PublicFixture f; AssistGate gate;
    dzIPC::shm::shm_sub_ipc sub(f.model(), f.topic.descriptor.topic, 0, 8);
    sub.enable_cancellable_wait(); ASSERT_TRUE(sub.enable_receive_assist()); sub.InitChannel();
    dzIPC::shm::shm_pub_ipc pub(f.model(), f.topic.descriptor.topic, 0); pub.InitChannel();
    ASSERT_TRUE(until([&] { return sub.connected_generation() != 0; }));
    const auto old_generation = sub.connected_generation();
    auto reading = std::async(std::launch::async, [&] {
        AssistGate::slot = 0; dzIPC::Sample sample; return sub.get_cancellable(sample, 2000) && sample.valid();
    });
    EXPECT_TRUE(until([&] { return gate.waiting[0].load(); }));
    dzIPC::control_plane_shm::TopicControlPlane control;
    const bool opened = control.open(shm_topic_mpmc_control_name(f.topic.descriptor.topic, 0));
    EXPECT_TRUE(opened);
    if (opened) {
        const auto next_generation = control.begin_rebuild(); control.set_ready();
        EXPECT_GT(next_generation, old_generation);
        EXPECT_TRUE(until([&] { return sub.connected_generation() == next_generation; }, 1000ms));
    }
    EXPECT_TRUE(pub.publish(f.message()));
    EXPECT_EQ(reading.wait_for(1000ms), std::future_status::ready);
    sub.cancel_waits();
    EXPECT_TRUE(reading.get());
}

TEST(SharedNetPublicApi, CallerReleasesReceiveLeaseBeforeDispatchAcrossRebuild) {
    PublicFixture f; AssistGate gate;
    dzIPC::shm::shm_sub_ipc sub(f.model(), f.topic.descriptor.topic, 0, 8);
    sub.enable_cancellable_wait(); ASSERT_TRUE(sub.enable_receive_assist()); sub.InitChannel();
    dzIPC::shm::shm_pub_ipc pub(f.model(), f.topic.descriptor.topic, 0); pub.InitChannel();
    ASSERT_TRUE(until([&] { return sub.connected_generation() != 0; }));
    dzIPC::control_plane_shm::TopicControlPlane control;
    ASSERT_TRUE(control.open(shm_topic_mpmc_control_name(f.topic.descriptor.topic, 0)));
    gate.pause_dispatch.store(true);
    auto reading = std::async(std::launch::async, [&] {
        AssistGate::slot = 0;
        dzIPC::Sample sample;
        sub.get_cancellable(sample, 3000);
        return sample;
    });
    EXPECT_TRUE(until([&] { return gate.waiting[0].load(); }));
    EXPECT_TRUE(pub.publish(f.message()));
    const bool paused = until([&] { return gate.at_dispatch.load(); });
    EXPECT_TRUE(paused);
    if (paused) {
        const auto next_generation = control.begin_rebuild(); control.set_ready();
        // getter仍停在入队前；旧映射的接收计数必须已经归零，否则重建不能前进。
        EXPECT_TRUE(until([&] { return sub.connected_generation() == next_generation; }, 500ms));
        EXPECT_EQ(reading.wait_for(0ms), std::future_status::timeout);
    }
    gate.continue_dispatch.store(true);
    EXPECT_EQ(reading.wait_for(1000ms), std::future_status::ready);
    sub.cancel_waits();
    auto sample = reading.get();
    ASSERT_TRUE(sample.valid());
    const auto view = sample.view<dzIPC::Msg::StdImageFlat>();
    ASSERT_TRUE(view.valid());
    ASSERT_EQ(view.data().size(), 64u);
    for (auto byte : view.data()) EXPECT_EQ(byte, 0x7b);
}
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

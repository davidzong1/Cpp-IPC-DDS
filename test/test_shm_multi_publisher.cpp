#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <memory>
#include <signal.h>
#include <string>
#include <thread>
#include <unistd.h>
#include <sys/wait.h>
#include <vector>

#include <gtest/gtest.h>

#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/publisher_registry.h"
#include "dzIPC/ipc_info_pool.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "ipc_msg/std_msgs/std_image.hpp"
#include "libipc/ipc.h"
#include "libipc/shm.h"

namespace {

using namespace std::chrono_literals;

struct RuntimeOptions {
    bool nodelet{dzIPC::IsNodeletEnabled()};
    bool dzflat{dzIPC::IsDzFlatEnabled()};

    RuntimeOptions()
    {
        dzIPC::EnableNodelet(false);
        dzIPC::EnableDzFlat(false);
    }

    ~RuntimeOptions()
    {
        dzIPC::EnableNodelet(nodelet);
        dzIPC::EnableDzFlat(dzflat);
    }
};

std::shared_ptr<dzIPC::TopicData> topic_data()
{
    return std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::StdImage>(), 731);
}

std::shared_ptr<dzIPC::Msg::StdImage> image(std::uint8_t marker)
{
    auto value = std::make_shared<dzIPC::Msg::StdImage>();
    value->set_msg_id(731);
    value->data.assign(256, marker);
    return value;
}

bool wait_for(const std::function<bool()>& predicate, std::chrono::milliseconds timeout)
{
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (predicate())
        {
            return true;
        }
        std::this_thread::sleep_for(2ms);
    }
    return predicate();
}

void clear_topic(const std::string& topic)
{
    ipc::mpmc_channel::clear_storage(shm_topic_mpmc_segment_name(topic, 0).c_str());
    ipc::shm::handle::clear_storage(shm_topic_mpmc_control_name(topic, 0).c_str());
    ipc::shm::handle::clear_storage(shm_topic_publisher_registry_name(topic, 0).c_str());
}

TEST(ShmMultiPublisher, JoinAndSurvivorContinuePublishing)
{
    ASSERT_EQ(::setenv("DZIPC_SHM_MPMC", "1", 1), 0);
    RuntimeOptions options;
    const auto topic = std::string("shm_mpmc_e2e_") + std::to_string(::getpid());
    clear_topic(topic);

    auto pub1 = std::make_unique<dzIPC::shm::shm_pub_ipc>(topic_data(), topic, 0, false);
    auto pub2 = std::make_unique<dzIPC::shm::shm_pub_ipc>(topic_data(), topic, 0, false);
    auto sub = std::make_unique<dzIPC::shm::shm_sub_ipc>(topic_data(), topic, 0, 32, false);
    pub1->InitChannel();
    pub2->InitChannel();
    sub->InitChannel();

    ASSERT_TRUE(pub1->channel_ready());
    ASSERT_TRUE(pub2->channel_ready());
    dzIPC::control_plane_shm::PublisherRegistry registry;
    ASSERT_TRUE(registry.open(shm_topic_publisher_registry_name(topic, 0)));
    ASSERT_TRUE(wait_for([&] { return registry.publisher_count() == 2; }, 3s));
    ASSERT_EQ(registry.generation(), 1u);

    ASSERT_TRUE(wait_for(
        [&]
        {
            std::size_t observed = 0;
            for (const auto& entry : dzIPC::info_pool::IpcInfoPool::instance().snapshot(false))
            {
                if (entry.kind != dzIPC::info_pool::EntryKind::ShmPub ||
                    entry.topic_name != topic)
                    continue;
                ++observed;
                EXPECT_LE(entry.extra.size(), dzIPC::info_pool::kMaxExtra - 1);
                EXPECT_NE(entry.extra.find("transport=shm_mpmc"), std::string::npos);
                EXPECT_NE(entry.extra.find("layout=V2"), std::string::npos);
                EXPECT_NE(entry.extra.find("gen=1"), std::string::npos);
                EXPECT_NE(entry.extra.find("pub="), std::string::npos);
                EXPECT_NE(entry.extra.find("coord="), std::string::npos);
            }
            return observed >= 2;
        },
        3s));

    ASSERT_TRUE(wait_for([&] { return pub1->has_subscribed() && pub2->has_subscribed(); }, 3s));
    for (std::uint8_t marker = 1; marker <= 4; ++marker)
    {
        ASSERT_TRUE(pub1->publish_best_effort(image(marker)));
        ASSERT_TRUE(pub2->publish_best_effort(image(static_cast<std::uint8_t>(100 + marker))));
    }

    std::size_t first_batch = 0;
    ASSERT_TRUE(wait_for(
        [&] {
            auto out = topic_data();
            while (sub->try_get_clone(out))
            {
                ++first_batch;
            }
            return first_batch >= 8;
        },
        5s));

    pub2.reset();
    ASSERT_TRUE(wait_for([&] { return registry.publisher_count() == 1; }, 3s));
    ASSERT_TRUE(pub1->publish_best_effort(image(201)));
    auto survivor = topic_data();
    ASSERT_TRUE(wait_for([&] { return sub->try_get_clone(survivor); }, 3s));
    ASSERT_EQ(survivor->topic()->msgcast<dzIPC::Msg::StdImage>()->data.front(), 201);

    sub.reset();
    pub1.reset();
    clear_topic(topic);
}

TEST(ShmMultiPublisher, CrossProcessJoinAndSurvivor)
{
    ASSERT_EQ(::setenv("DZIPC_SHM_MPMC", "1", 1), 0);
    RuntimeOptions options;
    const auto topic = std::string("shm_mpmc_xproc_") + std::to_string(::getpid());
    clear_topic(topic);

    int ready_pipe[2]{};
    int go_pipe[2]{};
    ASSERT_EQ(::pipe(ready_pipe), 0);
    ASSERT_EQ(::pipe(go_pipe), 0);
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0)
    {
        ::close(ready_pipe[0]);
        ::close(go_pipe[1]);
        {
            dzIPC::shm::shm_pub_ipc child_pub(topic_data(), topic, 0, false);
            child_pub.InitChannel();
            if (!child_pub.channel_ready())
            {
                _exit(10);
            }
            const char ready = 1;
            if (::write(ready_pipe[1], &ready, 1) != 1)
            {
                _exit(11);
            }
            char go = 0;
            if (::read(go_pipe[0], &go, 1) != 1)
            {
                _exit(12);
            }
            if (!child_pub.publish_best_effort(image(77)))
            {
                _exit(13);
            }
        }
        _exit(0);
    }

    ::close(ready_pipe[1]);
    ::close(go_pipe[0]);
    char ready = 0;
    ASSERT_EQ(::read(ready_pipe[0], &ready, 1), 1);

    auto parent_pub = std::make_unique<dzIPC::shm::shm_pub_ipc>(topic_data(), topic, 0, false);
    auto sub = std::make_unique<dzIPC::shm::shm_sub_ipc>(topic_data(), topic, 0, 32, false);
    parent_pub->InitChannel();
    sub->InitChannel();
    ASSERT_TRUE(parent_pub->channel_ready());
    ASSERT_TRUE(wait_for([&] { return parent_pub->has_subscribed(); }, 3s));

    dzIPC::control_plane_shm::PublisherRegistry registry;
    ASSERT_TRUE(registry.open(shm_topic_publisher_registry_name(topic, 0)));
    ASSERT_TRUE(wait_for([&] { return registry.publisher_count() == 2; }, 3s));
    const char go = 1;
    ASSERT_EQ(::write(go_pipe[1], &go, 1), 1);

    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFEXITED(status));
    ASSERT_EQ(WEXITSTATUS(status), 0);
    ASSERT_TRUE(wait_for([&] { return registry.publisher_count() == 1; }, 3s));

    auto received = topic_data();
    ASSERT_TRUE(wait_for([&] { return sub->try_get_clone(received); }, 3s));
    ASSERT_EQ(received->topic()->msgcast<dzIPC::Msg::StdImage>()->data.front(), 77);

    ASSERT_TRUE(parent_pub->publish_best_effort(image(88)));
    received = topic_data();
    ASSERT_TRUE(wait_for([&] { return sub->try_get_clone(received); }, 3s));
    ASSERT_EQ(received->topic()->msgcast<dzIPC::Msg::StdImage>()->data.front(), 88);

    sub.reset();
    parent_pub.reset();
    ::close(ready_pipe[0]);
    ::close(go_pipe[1]);
    clear_topic(topic);
}

TEST(ShmMultiPublisher, CoordinatorKillIsRecoveredBySurvivor)
{
    ASSERT_EQ(::setenv("DZIPC_SHM_MPMC", "1", 1), 0);
    RuntimeOptions options;
    const auto topic = std::string("shm_mpmc_coord_kill_") + std::to_string(::getpid());
    clear_topic(topic);

    int ready_pipe[2]{};
    ASSERT_EQ(::pipe(ready_pipe), 0);
    const pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0)
    {
        ::close(ready_pipe[0]);
        dzIPC::shm::shm_pub_ipc coordinator(topic_data(), topic, 0, false);
        coordinator.InitChannel();
        if (!coordinator.channel_ready())
            _exit(20);
        const char ready = 1;
        if (::write(ready_pipe[1], &ready, 1) != 1)
            _exit(21);
        /* Keep the coordinator alive until the parent verifies its lease. */
        for (;;)
            ::pause();
    }

    ::close(ready_pipe[1]);
    char ready = 0;
    ASSERT_EQ(::read(ready_pipe[0], &ready, 1), 1);
    ::close(ready_pipe[0]);

    auto survivor = std::make_unique<dzIPC::shm::shm_pub_ipc>(topic_data(), topic, 0, false);
    auto sub = std::make_unique<dzIPC::shm::shm_sub_ipc>(topic_data(), topic, 0, 16, false);
    survivor->InitChannel();
    sub->InitChannel();
    ASSERT_TRUE(survivor->channel_ready());

    dzIPC::control_plane_shm::PublisherRegistry registry;
    ASSERT_TRUE(registry.open(shm_topic_publisher_registry_name(topic, 0)));
    ASSERT_TRUE(wait_for([&] { return registry.publisher_count() == 2; }, 3s));

    ipc::shm::handle registry_view;
    ASSERT_TRUE(registry_view.acquire(
        shm_topic_publisher_registry_name(topic, 0).c_str(),
        sizeof(dzIPC::control_plane_shm::PublisherControl), ipc::shm::open));
    auto* control = static_cast<dzIPC::control_plane_shm::PublisherControl*>(registry_view.get());
    ASSERT_NE(control, nullptr);
    ASSERT_TRUE(wait_for(
        [&]
        {
            const auto slot = control->coordinator_slot.load(std::memory_order_acquire);
            return slot >= 0 &&
                   control->publishers[slot].pid.load(std::memory_order_acquire) ==
                       static_cast<std::int32_t>(child);
        },
        3s));

    ASSERT_EQ(::kill(child, SIGKILL), 0);
    int status = 0;
    ASSERT_EQ(::waitpid(child, &status, 0), child);
    ASSERT_TRUE(WIFSIGNALED(status));
    ASSERT_EQ(WTERMSIG(status), SIGKILL);

    ASSERT_TRUE(wait_for([&] { return registry.publisher_count() == 1; }, 5s));
    ASSERT_TRUE(wait_for(
        [&]
        {
            const auto slot = control->coordinator_slot.load(std::memory_order_acquire);
            return slot >= 0 &&
                   control->publishers[slot].pid.load(std::memory_order_acquire) ==
                       static_cast<std::int32_t>(::getpid());
        },
        5s));
    ASSERT_TRUE(wait_for([&] { return survivor->has_subscribed(); }, 3s));

    ASSERT_TRUE(survivor->publish_best_effort(image(233)));
    auto received = topic_data();
    ASSERT_TRUE(wait_for([&] { return sub->try_get_clone(received); }, 3s));
    ASSERT_EQ(received->topic()->msgcast<dzIPC::Msg::StdImage>()->data.front(), 233);

    registry_view.release_no_unlink();
    sub.reset();
    survivor.reset();
    clear_topic(topic);
}

TEST(ShmMultiPublisher, MpmcHighLevelDzFlatSegmentRoundTrip)
{
    ASSERT_EQ(::setenv("DZIPC_SHM_MPMC", "1", 1), 0);
    RuntimeOptions options;
    const bool previous_dzflat = dzIPC::IsDzFlatEnabled();
    dzIPC::EnableDzFlat(true);
    const auto topic = std::string("shm_mpmc_dzflat_") + std::to_string(::getpid());
    clear_topic(topic);

    dzIPC::shm::shm_pub_ipc pub{topic_data(), topic, 0, false};
    dzIPC::shm::shm_sub_ipc sub{topic_data(), topic, 0, 16, false};
    pub.InitChannel();
    sub.InitChannel();
    ASSERT_TRUE(pub.channel_ready());
    ASSERT_TRUE(wait_for([&] { return pub.has_subscribed(); }, 3s));

    auto source = image(0xA5);
    source->header.frame_id = "mpmc-dzflat";
    source->width = 16;
    source->height = 8;
    source->step = 48;
    source->encoding = "rgb8";
    std::vector<std::uint8_t> segment(source->dzflat_size());
    ASSERT_TRUE(source->dzflat_write(segment.data(), static_cast<std::uint32_t>(segment.size())));
    ASSERT_TRUE(pub.publish_prebuilt_segment(segment.data(), segment.size()));

    dzIPC::Sample sample;
    ASSERT_TRUE(wait_for([&] { return sub.try_get(sample); }, 3s));
    ASSERT_TRUE(sample.valid());
    ASSERT_EQ(sample.msg_id(), 731u);
    ASSERT_EQ(sample.size() >= segment.size(), true);
    ASSERT_EQ(std::memcmp(sample.data(), segment.data(), segment.size()), 0);

    dzIPC::EnableDzFlat(previous_dzflat);
    clear_topic(topic);
}

} // namespace

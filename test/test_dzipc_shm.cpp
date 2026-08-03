#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/shm_ser_cli_ipc.h"
#include "dzIPC/common/thread_dispatch.h"
#include "ipc_msg/test_msg2/test_msg.hpp"
#include "ipc_srv/request_response_test/request_response_test.hpp"

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace {

std::atomic<bool> response_complete{false};
std::atomic<bool> response_ok{true};
std::mutex response_mutex;
std::string response_error;

std::atomic<bool> subscriber_done{false};
std::atomic<int> subscriber_count{0};
using namespace dzIPC;

#if defined(__linux__)
int first_allowed_cpu()
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    if (sched_getaffinity(0, sizeof(cpu_set_t), &cpuset) != 0)
    {
        return -1;
    }
    for (int cpu = 0; cpu < CPU_SETSIZE; ++cpu)
    {
        if (CPU_ISSET(cpu, &cpuset))
        {
            return cpu;
        }
    }
    return -1;
}
#endif

void record_response_error(const std::string& message)
{
    response_ok.store(false);
    std::lock_guard<std::mutex> lock(response_mutex);
    if (response_error.empty())
    {
        response_error = message;
    }
}

void ser_thread_function()
{
    std::shared_ptr<ServiceData> message_
        = std::make_shared<dzIPC::ServiceData>(std::make_shared<dzIPC::Srv::RequestResponseTestRequest>(),
                                               std::make_shared<dzIPC::Srv::RequestResponseTestResponse>());
    dzIPC::shm::shm_ser_ipc service_ipc(
        "request_response_test", message_,
        [](std::shared_ptr<ServiceData>& msg)
        {
            auto req = std::static_pointer_cast<dzIPC::Srv::RequestResponseTestRequest>(msg->request());
            auto res = std::static_pointer_cast<dzIPC::Srv::RequestResponseTestResponse>(msg->response());
            res->response.resize(req->request.size());
            for (int i = 0; i < static_cast<int>(req->request.size()); i++)
            {
                res->response[i] = req->request[i] + 1.0;
            }
        },
        1, true);
    service_ipc.InitChannel();
    while (!response_complete.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void cli_thread_function()
{
    std::shared_ptr<ServiceData> message_
        = std::make_shared<dzIPC::ServiceData>(std::make_shared<dzIPC::Srv::RequestResponseTestRequest>(),
                                               std::make_shared<dzIPC::Srv::RequestResponseTestResponse>());
    dzIPC::shm::shm_cli_ipc client_ipc("request_response_test", message_, 1, true);
    std::vector<double> test_data;
    client_ipc.InitChannel();
    int cnt = 0;
    while (cnt < 10)
    {
        std::chrono::duration<double, std::micro> elapsed_us[10];
        test_data.clear();
        for (int i = 0; i < 10; i++)
        {
            test_data.push_back(i);
            message_->request()->msgcast<dzIPC::Srv::RequestResponseTestRequest>()->request = test_data;
            auto start = std::chrono::high_resolution_clock::now();
            while (!client_ipc.send_request(message_))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            auto end = std::chrono::high_resolution_clock::now();
            elapsed_us[i] = end - start;
            auto response_ = message_->response()->msgcast<dzIPC::Srv::RequestResponseTestResponse>();
            if (response_->response.size() != test_data.size())
            {
                record_response_error("response size mismatch");
                response_complete.store(true);
                return;
            }
            for (int k = 0; k < static_cast<int>(test_data.size()); k++)
            {
                if (response_->response[k] != test_data[k] + 1.0)
                {
                    record_response_error("response value mismatch");
                    response_complete.store(true);
                    return;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        std::cout << "Round " << cnt + 1 << " latency (us): ";
        for (int i = 0; i < 10; i++)
        {
            std::cout << static_cast<long long>(elapsed_us[i].count()) << " ";
        }
        std::cout << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        cnt++;
    }

    response_complete.store(true);
}

void publish_thread_function()
{
    std::shared_ptr<TopicData> topic_msg_ = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::TestMsg>());
    dzIPC::shm::shm_pub_ipc publisher(topic_msg_, "TestMsg2", 1, true);
    publisher.InitChannel();
    int count = 0;
    bool exit_flag = false;
    while (!exit_flag)
    {
        auto msg = std::make_shared<dzIPC::Msg::TestMsg>();
        msg->data1 = {1.1, 2.2, 3.3};
        msg->data2 = {1, 2, 3, 4};
        msg->data3 = {"hello", "world", "from", "dzIPC"};
        if (count >= 10)
        {
            msg->data3.push_back("exit");
            exit_flag = true;
        }
        msg->data4 = (count % 2 == 0);
        count++;
        if (!publisher.publish(msg))
        {
            std::cerr << "\033[31mError publishing message on topic: "
                      << "\033[0m" << std::endl;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

void subscribe_thread_function()
{
    std::shared_ptr<TopicData> topic_msg_ = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::TestMsg>());
    dzIPC::shm::shm_sub_ipc subscriber(topic_msg_, "TestMsg2", 10, 1, true);
    subscriber.InitChannel();
    bool exit_flag = false;
    while (!exit_flag)
    {
        if (subscriber.try_get(topic_msg_))
        {
            auto received_msg = topic_msg_->topic()->msgcast<dzIPC::Msg::TestMsg>();
            subscriber_count.fetch_add(1);
            for (const auto& str : received_msg->data3)
            {
                if (str == "exit")
                {
                    exit_flag = true;
                    subscriber_done.store(true);
                    break;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

}   // namespace

TEST(DzIpcShm, RequestResponse)
{
    response_complete.store(false);
    response_ok.store(true);
    {
        std::lock_guard<std::mutex> lock(response_mutex);
        response_error.clear();
    }

    std::thread ser_thread(ser_thread_function);
    std::thread cli_thread(cli_thread_function);
    cli_thread.join();
    ser_thread.join();

    std::string error_snapshot;
    {
        std::lock_guard<std::mutex> lock(response_mutex);
        error_snapshot = response_error;
    }
    ASSERT_TRUE(response_ok.load()) << error_snapshot;
}

TEST(DzIpcShm, PubSub)
{
    subscriber_done.store(false);
    subscriber_count.store(0);

    std::thread pub_thread(publish_thread_function);
    std::thread sub_thread(subscribe_thread_function);
    pub_thread.join();
    sub_thread.join();

    ASSERT_TRUE(subscriber_done.load());
    ASSERT_GT(subscriber_count.load(), 0);
}

TEST(DzIpcShm, SubscriberRecoversAfterPublisherRestart)
{
    auto topic_msg = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::TestMsg>());
    dzIPC::shm::shm_sub_ipc subscriber(topic_msg, "/restart/recovery", 0, 4, false);
    subscriber.InitChannel();

    auto publish_once = [](const std::string& marker)
    {
        auto pub_topic = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::TestMsg>());
        dzIPC::shm::shm_pub_ipc publisher(pub_topic, "/restart/recovery", 0, false);
        publisher.InitChannel();
        std::this_thread::sleep_for(std::chrono::milliseconds(80));

        auto msg = std::make_shared<dzIPC::Msg::TestMsg>();
        msg->data1 = {1.0};
        msg->data2 = {1};
        msg->data3 = {marker};
        msg->data4 = true;
        ASSERT_TRUE(publisher.publish(msg));
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
    };

    auto wait_for_marker = [&](const std::string& marker)
    {
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (subscriber.try_get(topic_msg))
            {
                auto received = topic_msg->topic()->msgcast<dzIPC::Msg::TestMsg>();
                for (const auto& item : received->data3)
                {
                    if (item == marker)
                    {
                        return true;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
        return false;
    };

    publish_once("before_restart");
    ASSERT_TRUE(wait_for_marker("before_restart"));

    publish_once("after_restart");
    ASSERT_TRUE(wait_for_marker("after_restart"));
}

TEST(DzIpcShm, ThreadDispatchCpuAffinityExample)
{
#if defined(__linux__)
    const int cpu_id = first_allowed_cpu();
    ASSERT_GE(cpu_id, 0);

    std::atomic<bool> start{false};
    std::atomic<int> observed_cpu{-1};
    std::thread worker(
        [&]()
        {
            while (!start.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            observed_cpu.store(sched_getcpu(), std::memory_order_release);
        });

    ASSERT_TRUE(dzIPC::ThreadDispatch::set_thread_affinity(&worker, cpu_id, true, "CpuAffinityExample"));
    start.store(true, std::memory_order_release);
    worker.join();

    EXPECT_EQ(observed_cpu.load(std::memory_order_acquire), cpu_id);
#else
    GTEST_SKIP() << "CPU affinity verification is only implemented for Linux in this test.";
#endif
}

TEST(DzIpcShm, ThreadDispatchFifoPriorityExample)
{
#if defined(__linux__)
    std::atomic<bool> start{false};
    std::atomic<int> observed_policy{-1};
    std::atomic<int> observed_priority{-1};

    std::thread worker(
        [&]()
        {
            while (!start.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
            int policy = 0;
            sched_param param{};
            if (pthread_getschedparam(pthread_self(), &policy, &param) == 0)
            {
                observed_policy.store(policy, std::memory_order_release);
                observed_priority.store(param.sched_priority, std::memory_order_release);
            }
        });

    const bool fifo_ok = dzIPC::ThreadDispatch::set_thread_priority(&worker, 20, true, "FifoPriorityExample", true);
    start.store(true, std::memory_order_release);
    worker.join();

    if (!fifo_ok)
    {
        GTEST_SKIP() << "SCHED_FIFO requires root or CAP_SYS_NICE; priority path was exercised but not permitted.";
    }
    EXPECT_EQ(observed_policy.load(std::memory_order_acquire), SCHED_FIFO);
    EXPECT_GE(observed_priority.load(std::memory_order_acquire), sched_get_priority_min(SCHED_FIFO));
#else
    GTEST_SKIP() << "SCHED_FIFO verification is only implemented for Linux in this test.";
#endif
}

TEST(DzIpcShm, PublishForSnifferWithoutSubscriber)
{
    std::shared_ptr<TopicData> topic_msg = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::TestMsg>());
    dzIPC::shm::shm_pub_ipc publisher(topic_msg, "SnifferOnly", 1, true);
    publisher.InitChannel();

    ipc::sniffer sniffer;
    ASSERT_TRUE(sniffer.open("dz_ipc_SnifferOnly_topic", ipc::sniffer::topology::route));
    EXPECT_EQ(sniffer.receiver_connections(), 0u);
    EXPECT_TRUE(sniffer.try_recv().empty());

    auto msg = std::make_shared<dzIPC::Msg::TestMsg>();
    msg->data1 = {1.1, 2.2};
    msg->data2 = {1, 2};
    msg->data3 = {"sniffer", "only"};
    msg->data4 = true;

    const auto start = std::chrono::steady_clock::now();
    ASSERT_TRUE(publisher.publish_for_sniffer(msg));
    ipc::sniffer::meta meta;
    ipc::buffer raw = sniffer.recv(500, &meta);
    const auto elapsed = std::chrono::steady_clock::now() - start;

    ASSERT_FALSE(raw.empty());
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 500);
    EXPECT_EQ(sniffer.receiver_connections(), 0u);
}

TEST(DzIpcShm, PublishTopicWithSlashForSnifferWithoutSubscriber)
{
    std::shared_ptr<TopicData> topic_msg = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::TestMsg>());
    dzIPC::shm::shm_pub_ipc publisher(topic_msg, "/demo/depth_image", 0, false);
    publisher.InitChannel();

    auto msg = std::make_shared<dzIPC::Msg::TestMsg>();
    msg->data1 = {1.1, 2.2};
    msg->data2 = {1, 2};
    msg->data3 = {"slash", "topic"};
    msg->data4 = true;

    ASSERT_TRUE(publisher.publish_for_sniffer(msg));
}

TEST(DzIpcShm, PublishLargeForSnifferWithoutSubscriber)
{
    std::shared_ptr<TopicData> topic_msg = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::TestMsg>());
    dzIPC::shm::shm_pub_ipc publisher(topic_msg, "SnifferLarge", 1, false);
    publisher.InitChannel();

    ipc::sniffer sniffer;
    ASSERT_TRUE(sniffer.open("dz_ipc_SnifferLarge_topic", ipc::sniffer::topology::route));
    EXPECT_TRUE(sniffer.try_recv().empty());

    std::string large_text(ipc::data_length * 8, 'x');
    auto msg = std::make_shared<dzIPC::Msg::TestMsg>();
    msg->data1 = {1.1, 2.2};
    msg->data2 = {1, 2};
    msg->data3 = {large_text};
    msg->data4 = true;

    ASSERT_TRUE(publisher.publish_for_sniffer(msg));

    ipc::buffer raw = sniffer.recv(500);
    ASSERT_FALSE(raw.empty());
    EXPECT_GT(raw.size(), ipc::data_length);

    auto received = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::TestMsg>());
    ASSERT_TRUE(received->check_msg_id(raw));
    received->topic()->deserialize(raw);
    auto received_msg = received->topic()->msgcast<dzIPC::Msg::TestMsg>();
    ASSERT_EQ(received_msg->data3.size(), 1u);
    EXPECT_EQ(received_msg->data3[0], large_text);
    EXPECT_EQ(sniffer.receiver_connections(), 0u);
}

TEST(DzIpcShm, RejectOversizedPublishForSnifferWithoutSubscriber)
{
    std::shared_ptr<TopicData> topic_msg = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::TestMsg>());
    dzIPC::shm::shm_pub_ipc publisher(topic_msg, "SnifferOversized", 1, false);
    publisher.InitChannel();

    auto msg = std::make_shared<dzIPC::Msg::TestMsg>();
    msg->data1 = {1.1};
    msg->data2 = {1};
    msg->data3 = {std::string(ipc::sniffer_payload_limit + ipc::data_length, 'z')};
    msg->data4 = true;

    const auto start = std::chrono::steady_clock::now();
    EXPECT_FALSE(publisher.publish_for_sniffer(msg));
    const auto elapsed = std::chrono::steady_clock::now() - start;
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(), 50);
}

TEST(DzIpcShm, SnifferReopenAfterPublisherRestart)
{
    // Regression: old sniffer close() must NOT shm_unlink the SHM segments
    // of a new publisher that reused the same topic name after restart.
    //
    // Correct lifecycle: pub1 exits → pub2 recreates same SHM names →
    // old_sniffer closes → new_sniffer opens pub2's SHM → receives data.
    //
    // If old_sniffer's close() calls shm_unlink on the name (which now
    // points to pub2's inode), pub2's SHM is deleted, and new_sniffer
    // fails to open or receives nothing.

    constexpr const char* topic = "SnifferReopenV2";

    auto msg1 = std::make_shared<dzIPC::Msg::TestMsg>();
    msg1->data1 = {1.0, 2.0};
    msg1->data2 = {1, 2};

    // --- Phase 1: open first publisher + first sniffer, verify sniffing works ---
    std::shared_ptr<TopicData> td1 =
        std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::TestMsg>());
    auto pub1 = std::make_unique<dzIPC::shm::shm_pub_ipc>(td1, topic, 1, true);
    pub1->InitChannel();

    auto sniffer1 = std::make_unique<ipc::sniffer>();
    ASSERT_TRUE(sniffer1->open("dz_ipc_SnifferReopenV2_topic", ipc::sniffer::topology::route));
    EXPECT_TRUE(sniffer1->try_recv().empty());

    ASSERT_TRUE(pub1->publish_for_sniffer(msg1));
    {
        ipc::buffer raw = sniffer1->recv(500);
        ASSERT_FALSE(raw.empty());
    }

    // --- Phase 2: destroy pub1 (publisher exits, cleans up its own SHM) ---
    pub1.reset();

    // --- Phase 3: create pub2 on same topic (new SHM generation) ---
    // This is the critical window: pub2's SHM segments now own the names
    // that sniffer1 still holds fds to (old inodes).
    std::shared_ptr<TopicData> td2 =
        std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::TestMsg>());
    auto pub2 = std::make_unique<dzIPC::shm::shm_pub_ipc>(td2, topic, 1, true);
    pub2->InitChannel();

    // --- Phase 4: NOW close old sniffer1 ---
    // Its close()/destructor must NOT shm_unlink pub2's SHM segments.
    sniffer1.reset();

    // --- Phase 5: open new sniffer2 on pub2's SHM, verify it receives data ---
    auto sniffer2 = std::make_unique<ipc::sniffer>();
    ASSERT_TRUE(sniffer2->open("dz_ipc_SnifferReopenV2_topic", ipc::sniffer::topology::route))
        << "sniffer2 failed to open — old sniffer may have unlinked pub2's SHM";
    EXPECT_TRUE(sniffer2->try_recv().empty());

    auto msg2 = std::make_shared<dzIPC::Msg::TestMsg>();
    msg2->data1 = {3.0, 4.0};
    msg2->data2 = {3, 4};

    ASSERT_TRUE(pub2->publish_for_sniffer(msg2));
    {
        ipc::buffer raw = sniffer2->recv(500);
        ASSERT_FALSE(raw.empty()) << "sniffer2 received nothing — pub2's SHM may have been unlinked";

        auto received = std::make_shared<dzIPC::TopicData>(std::make_shared<dzIPC::Msg::TestMsg>());
        ASSERT_TRUE(received->check_msg_id(raw));
        received->topic()->deserialize(raw);
        auto out_msg = received->topic()->msgcast<dzIPC::Msg::TestMsg>();
        ASSERT_EQ(out_msg->data1.size(), 2u);
        EXPECT_DOUBLE_EQ(out_msg->data1[0], 3.0);
        EXPECT_DOUBLE_EQ(out_msg->data1[1], 4.0);
    }
}

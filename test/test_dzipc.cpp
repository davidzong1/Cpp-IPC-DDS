#include <atomic>
#include <chrono>
#include <csignal>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "dzIPC/common/thread_dispatch.h"
#include "dzIPC/dzipc.h"
#include "ipc_msg/test_msg2/test_msg.hpp"
#include "ipc_srv/request_response_test/request_response_test.hpp"

#include <gtest/gtest.h>

#if defined(__linux__)
#include <pthread.h>
#include <sched.h>
#endif

namespace {
using namespace dzIPC;
std::atomic<bool> socket_response_complete{false};
std::atomic<bool> socket_response_ok{true};
std::mutex socket_socket_response_error;
std::string response_error;

std::atomic<bool> socket_subscriber_done{false};
std::atomic<int> socket_subscriber_count{0};
std::atomic<bool> ctrlc_captured{false};

#if defined(__linux__)
bool cpu_allowed(int cpu_id)
{
    cpu_set_t cpuset;
    CPU_ZERO(&cpuset);
    if (sched_getaffinity(0, sizeof(cpu_set_t), &cpuset) != 0)
    {
        return false;
    }
    return cpu_id >= 0 && cpu_id < CPU_SETSIZE && CPU_ISSET(cpu_id, &cpuset);
}

bool can_apply_ultra_high_priority_cpu7()
{
    constexpr int kCpuId = CPU_CORE::CORE7;
    constexpr int kPriority = DispatchPriority::ultraHighPriority;

    if (!cpu_allowed(kCpuId))
    {
        return false;
    }

    std::atomic<bool> start{false};
    std::thread worker(
        [&]()
        {
            while (!start.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }
        });

    const auto options = dzIPC::ThreadDispatch::make_realtime_options(Qos::UseQos, kCpuId, kPriority);
    const bool qos_applied =
        dzIPC::ThreadDispatch::apply_thread_options(&worker, options, false, "UltraHighPriorityCpu7Probe");

    start.store(true, std::memory_order_release);
    worker.join();
    return qos_applied;
}
#endif

void socket_record_response_error(const std::string& message)
{
    socket_response_ok.store(false);
    std::lock_guard<std::mutex> lock(socket_socket_response_error);
    if (response_error.empty())
    {
        response_error = message;
    }
}

void socket_ser_thread_function()
{
    ServerDataPtr message_
        = ServerDataPtrMake<dzIPC::Srv::RequestResponseTestRequest, dzIPC::Srv::RequestResponseTestResponse>(
            114'514);   // msg id is 114514,default is 0
    ServerIPCPtr service_ipc = ServerIPCPtrMake(
        "request_response_test", message_,
        [](ServerDataPtr& msg)
        {
            auto req = msg->request()->msgcast<dzIPC::Srv::RequestResponseTestRequest>();
            auto res = msg->response()->msgcast<dzIPC::Srv::RequestResponseTestResponse>();
            res->response.resize(req->request.size());
            for (int i = 0; i < static_cast<int>(req->request.size()); i++)
            {
                res->response[i] = req->request[i] + 1.0;
            }
        },
        1, IPC_SOCKET, true);
    service_ipc->InitChannel();
    while (!socket_response_complete.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void socket_cli_thread_function()
{
    ServerDataPtr message_
        = ServerDataPtrMake<dzIPC::Srv::RequestResponseTestRequest, dzIPC::Srv::RequestResponseTestResponse>(
            114'514);   // msg id is 114514,default is 0
    ClientIPCPtr client_ipc = ClientIPCPtrMake("request_response_test", message_, 1, IPC_SOCKET, true);
    std::vector<double> test_data;
    client_ipc->InitChannel();
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
            while (!client_ipc->send_request(message_))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            auto end = std::chrono::high_resolution_clock::now();
            elapsed_us[i] = end - start;
            auto response_ = message_->response()->msgcast<dzIPC::Srv::RequestResponseTestResponse>();
            if (response_->response.size() != test_data.size())
            {
                std::cerr << "Expected response size: " << test_data.size()
                          << ", but got: " << response_->response.size() << std::endl;
                socket_record_response_error("response size mismatch");
                socket_response_complete.store(true);
                return;
            }
            for (int k = 0; k < static_cast<int>(test_data.size()); k++)
            {
                if (response_->response[k] != test_data[k] + 1.0)
                {
                    std::cerr << "Expected response value: " << test_data[k] + 1.0
                              << ", but got: " << response_->response[k] << std::endl;
                    socket_record_response_error("response value mismatch");
                    socket_response_complete.store(true);
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
    socket_response_complete.store(true);
}

void pulish_thread_function()
{
    TopicDataPtr topic_msg_ = TopicDataPtrMake<dzIPC::Msg::TestMsg>();
    PublisherIPCPtr publisher = PublisherIPCPtrMake(topic_msg_, "TestMsg2", 1, IPC_SOCKET, true);
    publisher->InitChannel();
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
        publisher->publish(msg);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

void subscribe_thread_function()
{
    TopicDataPtr topic_msg_ = TopicDataPtrMake<dzIPC::Msg::TestMsg>();
    SubscriberIPCPtr subscriber = SubscriberIPCPtrMake(topic_msg_, "TestMsg2", 1, 10, IPC_SOCKET, true);
    subscriber->InitChannel();
    bool exit_flag = false;
    while (!exit_flag)
    {
        if (subscriber->try_get_clone(topic_msg_))
        {
            auto rev_msg = topic_msg_->topic()->msgcast<dzIPC::Msg::TestMsg>();
            socket_subscriber_count.fetch_add(1);
            for (const auto& str : rev_msg->data3)
            {
                if (str == "exit")
                {
                    exit_flag = true;
                    socket_subscriber_done.store(true);
                    break;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

// =====================================================================================
// SHM variants — same logic as the SOCKET tests but using IPC_SHM
// =====================================================================================
std::atomic<bool> shm_response_complete{false};
std::atomic<bool> shm_response_ok{true};
std::mutex shm_response_error;
std::string shm_response_error_str;
std::atomic<bool> shm_timing_response_complete{false};
std::atomic<bool> shm_timing_response_ok{true};
std::mutex shm_timing_response_error;
std::string shm_timing_response_error_str;

void shm_record_response_error(const std::string& message)
{
    shm_response_ok.store(false);
    std::lock_guard<std::mutex> lock(shm_response_error);
    if (shm_response_error_str.empty())
    {
        shm_response_error_str = message;
    }
}

void shm_timing_record_response_error(const std::string& message)
{
    shm_timing_response_ok.store(false);
    std::lock_guard<std::mutex> lock(shm_timing_response_error);
    if (shm_timing_response_error_str.empty())
    {
        shm_timing_response_error_str = message;
    }
}

void shm_ser_thread_function()
{
    ServerDataPtr message_
        = ServerDataPtrMake<dzIPC::Srv::RequestResponseTestRequest, dzIPC::Srv::RequestResponseTestResponse>(
            114'515);   // msg id is 114515
    ServerIPCPtr service_ipc = ServerIPCPtrMake(
        "request_response_test_shm", message_,
        [](ServerDataPtr& msg)
        {
            auto req = msg->request()->msgcast<dzIPC::Srv::RequestResponseTestRequest>();
            auto res = msg->response()->msgcast<dzIPC::Srv::RequestResponseTestResponse>();
            res->response.resize(req->request.size());
            for (int i = 0; i < static_cast<int>(req->request.size()); i++)
            {
                res->response[i] = req->request[i] + 1.0;
            }
        },
        1, IPC_SHM, true);
    service_ipc->InitChannel();
    while (!shm_response_complete.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void shm_cli_thread_function()
{
    ServerDataPtr message_
        = ServerDataPtrMake<dzIPC::Srv::RequestResponseTestRequest, dzIPC::Srv::RequestResponseTestResponse>(
            114'515);   // msg id is 114515
    ClientIPCPtr client_ipc = ClientIPCPtrMake("request_response_test_shm", message_, 1, IPC_SHM, true);
    std::vector<double> test_data;
    client_ipc->InitChannel();
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
            while (!client_ipc->send_request(message_))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            auto end = std::chrono::high_resolution_clock::now();
            elapsed_us[i] = end - start;
            auto response_ = message_->response()->msgcast<dzIPC::Srv::RequestResponseTestResponse>();
            if (response_->response.size() != test_data.size())
            {
                std::cerr << "Expected response size: " << test_data.size()
                          << ", but got: " << response_->response.size() << std::endl;
                shm_record_response_error("response size mismatch");
                shm_response_complete.store(true);
                return;
            }
            for (int k = 0; k < static_cast<int>(test_data.size()); k++)
            {
                if (response_->response[k] != test_data[k] + 1.0)
                {
                    std::cerr << "Expected response value: " << test_data[k] + 1.0
                              << ", but got: " << response_->response[k] << std::endl;
                    shm_record_response_error("response value mismatch");
                    shm_response_complete.store(true);
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
    shm_response_complete.store(true);
}

void shm_timing_ser_thread_function()
{
    ServerDataPtr message_
        = ServerDataPtrMake<dzIPC::Srv::RequestResponseTestRequest, dzIPC::Srv::RequestResponseTestResponse>(
            114'516);   // msg id is 114516
    ServerIPCPtr service_ipc = ServerIPCPtrMake(
        "request_response_test_shm_timing", message_,
        [](ServerDataPtr& msg)
        {
            auto req = msg->request()->msgcast<dzIPC::Srv::RequestResponseTestRequest>();
            auto res = msg->response()->msgcast<dzIPC::Srv::RequestResponseTestResponse>();
            res->response.resize(req->request.size());
            for (int i = 0; i < static_cast<int>(req->request.size()); i++)
            {
                res->response[i] = req->request[i] + 1.0;
            }
        },
        1, IPC_SHM, true, Qos::UseQos, CPU_CORE::CORE7, DispatchPriority::ultraHighPriority);
    service_ipc->InitChannel();
    while (!shm_timing_response_complete.load())
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

void shm_timing_cli_thread_function()
{
    ServerDataPtr message_
        = ServerDataPtrMake<dzIPC::Srv::RequestResponseTestRequest, dzIPC::Srv::RequestResponseTestResponse>(
            114'516);   // msg id is 114516
    ClientIPCPtr client_ipc = ClientIPCPtrMake("request_response_test_shm_timing", message_, 1, IPC_SHM, true,
                                               Qos::UseQos, CPU_CORE::CORE7,
                                               DispatchPriority::ultraHighPriority);
    std::vector<double> test_data;
    client_ipc->InitChannel();
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
            while (!client_ipc->send_request(message_))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            }
            auto end = std::chrono::high_resolution_clock::now();
            elapsed_us[i] = end - start;
            auto response_ = message_->response()->msgcast<dzIPC::Srv::RequestResponseTestResponse>();
            if (response_->response.size() != test_data.size())
            {
                std::cerr << "Expected response size: " << test_data.size()
                          << ", but got: " << response_->response.size() << std::endl;
                shm_timing_record_response_error("response size mismatch");
                shm_timing_response_complete.store(true);
                return;
            }
            for (int k = 0; k < static_cast<int>(test_data.size()); k++)
            {
                if (response_->response[k] != test_data[k] + 1.0)
                {
                    std::cerr << "Expected response value: " << test_data[k] + 1.0
                              << ", but got: " << response_->response[k] << std::endl;
                    shm_timing_record_response_error("response value mismatch");
                    shm_timing_response_complete.store(true);
                    return;
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        std::cout << "QoS shm round " << cnt + 1 << " latency (us): ";
        for (int i = 0; i < 10; i++)
        {
            std::cout << static_cast<long long>(elapsed_us[i].count()) << " ";
        }
        std::cout << std::endl;
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
        cnt++;
    }
    shm_timing_response_complete.store(true);
}

std::atomic<bool> shm_subscriber_done{false};
std::atomic<int> shm_subscriber_count{0};

void shm_publish_thread_function()
{
    TopicDataPtr topic_msg_ = TopicDataPtrMake<dzIPC::Msg::TestMsg>();
    PublisherIPCPtr publisher = PublisherIPCPtrMake(topic_msg_, "TestMsg2Shm", 1, IPC_SHM, true);
    publisher->InitChannel();
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
        publisher->publish(msg);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

void shm_subscribe_thread_function()
{
    TopicDataPtr topic_msg_ = TopicDataPtrMake<dzIPC::Msg::TestMsg>();
    SubscriberIPCPtr subscriber = SubscriberIPCPtrMake(topic_msg_, "TestMsg2Shm", 1, 10, IPC_SHM, true);
    subscriber->InitChannel();
    bool exit_flag = false;
    while (!exit_flag)
    {
        if (subscriber->try_get_clone(topic_msg_))
        {
            auto rev_msg = topic_msg_->topic()->msgcast<dzIPC::Msg::TestMsg>();
            shm_subscriber_count.fetch_add(1);
            for (const auto& str : rev_msg->data3)
            {
                if (str == "exit")
                {
                    exit_flag = true;
                    shm_subscriber_done.store(true);
                    break;
                }
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
}

}   // namespace

TEST(DzIpcSocket, RequestResponse)
{
    socket_response_complete.store(false);
    socket_response_ok.store(true);
    {
        std::lock_guard<std::mutex> lock(socket_socket_response_error);
        response_error.clear();
    }

    std::thread ser_thread(socket_ser_thread_function);
    std::thread cli_thread(socket_cli_thread_function);
    cli_thread.join();
    ser_thread.join();

    std::string error_snapshot;
    {
        std::lock_guard<std::mutex> lock(socket_socket_response_error);
        error_snapshot = response_error;
    }
    ASSERT_TRUE(socket_response_ok.load()) << error_snapshot;
}

TEST(DzIpcSocket, PubSub)
{
    socket_subscriber_done.store(false);
    socket_subscriber_count.store(0);

    std::thread pub_thread(pulish_thread_function);
    std::thread sub_thread(subscribe_thread_function);
    pub_thread.join();
    sub_thread.join();

    EXPECT_TRUE(socket_subscriber_done.load());
    EXPECT_GE(socket_subscriber_count.load(), 1);
}

TEST(DzIpcShm, RequestResponse)
{
    shm_response_complete.store(false);
    shm_response_ok.store(true);
    {
        std::lock_guard<std::mutex> lock(shm_response_error);
        shm_response_error_str.clear();
    }

    std::thread ser_thread(shm_ser_thread_function);
    std::thread cli_thread(shm_cli_thread_function);
    cli_thread.join();
    ser_thread.join();

    std::string error_snapshot;
    {
        std::lock_guard<std::mutex> lock(shm_response_error);
        error_snapshot = shm_response_error_str;
    }
    ASSERT_TRUE(shm_response_ok.load()) << error_snapshot;
}

TEST(DzIpcShm, RequestResponseUltraHighPriorityCpu7Timing)
{
#if defined(__linux__)
    if (!cpu_allowed(CPU_CORE::CORE7))
    {
        GTEST_SKIP() << "CPU7 is not available in the current process affinity mask.";
    }
    if (!can_apply_ultra_high_priority_cpu7())
    {
        GTEST_SKIP() << "Applying SCHED_FIFO ultraHighPriority requires root or CAP_SYS_NICE.";
    }
#else
    GTEST_SKIP() << "Priority and CPU affinity verification is only implemented for Linux.";
#endif

    shm_timing_response_complete.store(false);
    shm_timing_response_ok.store(true);
    {
        std::lock_guard<std::mutex> lock(shm_timing_response_error);
        shm_timing_response_error_str.clear();
    }

    std::thread ser_thread(shm_timing_ser_thread_function);
    std::thread cli_thread(shm_timing_cli_thread_function);
    cli_thread.join();
    ser_thread.join();

    std::string error_snapshot;
    {
        std::lock_guard<std::mutex> lock(shm_timing_response_error);
        error_snapshot = shm_timing_response_error_str;
    }
    ASSERT_TRUE(shm_timing_response_ok.load()) << error_snapshot;
}

TEST(DzIpcShm, PubSub)
{
    shm_subscriber_done.store(false);
    shm_subscriber_count.store(0);

    std::thread pub_thread(shm_publish_thread_function);
    std::thread sub_thread(shm_subscribe_thread_function);
    pub_thread.join();
    sub_thread.join();

    EXPECT_TRUE(shm_subscriber_done.load());
    EXPECT_GE(shm_subscriber_count.load(), 1);
}

TEST(DzIpcThreadDispatch, UltraHighPriorityCpu7)
{
#if defined(__linux__)
    constexpr int kCpuId = CPU_CORE::CORE7;
    constexpr int kPriority = DispatchPriority::ultraHighPriority;

    if (!cpu_allowed(kCpuId))
    {
        GTEST_SKIP() << "CPU7 is not available in the current process affinity mask.";
    }

    std::atomic<bool> start{false};
    std::atomic<int> observed_cpu{-1};
    std::atomic<int> observed_policy{-1};
    std::atomic<int> observed_priority{-1};

    std::thread worker(
        [&]()
        {
            while (!start.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }

            observed_cpu.store(sched_getcpu(), std::memory_order_release);
            int policy = 0;
            sched_param param{};
            if (pthread_getschedparam(pthread_self(), &policy, &param) == 0)
            {
                observed_policy.store(policy, std::memory_order_release);
                observed_priority.store(param.sched_priority, std::memory_order_release);
            }
        });

    const auto options = dzIPC::ThreadDispatch::make_realtime_options(Qos::UseQos, kCpuId, kPriority);
    const bool qos_applied =
        dzIPC::ThreadDispatch::apply_thread_options(&worker, options, true, "UltraHighPriorityCpu7");

    start.store(true, std::memory_order_release);
    worker.join();

    if (!qos_applied)
    {
        GTEST_SKIP() << "Applying SCHED_FIFO ultraHighPriority requires root or CAP_SYS_NICE.";
    }

    EXPECT_EQ(observed_cpu.load(std::memory_order_acquire), kCpuId);
    EXPECT_EQ(observed_policy.load(std::memory_order_acquire), SCHED_FIFO);
    EXPECT_EQ(observed_priority.load(std::memory_order_acquire),
              dzIPC::ThreadDispatch::clamp_linux_fifo_priority(kPriority));
#else
    GTEST_SKIP() << "Priority and CPU affinity verification is only implemented for Linux.";
#endif
}

TEST(DzIpcSocket, CtrlCSignalCapture)
{
    ctrlc_captured.store(false);
    auto previous_sigint = std::signal(SIGINT, [](int) { ctrlc_captured.store(true); });

    std::raise(SIGINT);
    for (int i = 0; i < 50 && !ctrlc_captured.load(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    std::signal(SIGINT, previous_sigint);

    EXPECT_TRUE(ctrlc_captured.load());
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

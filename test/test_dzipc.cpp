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

/* ------------------------------------------------------------------------- *
 * T0 决策 1: 自动选路(pub/sub 不支持)必须**显式拒绝**。
 *
 * 注: IPCType::Auto 已从枚举中删除, 值 2 成为**空洞**。这里用 static_cast<IPCType>(2)
 * 构造那个值 —— 它可能来自旧二进制、旧配置或手写的字面量, 而 ser-cli 侧的自动选路
 * 依赖握手通道做两阶段裁定(pub/sub 没有), 所以这条拒绝逻辑与它的用例都必须留着。
 *
 * 它的实现依赖 ser-cli 的握手通道做两阶段路径裁定, 而 pub/sub 没有这条通道 ⇒
 * 这里不可能有正确实现, 只能拒绝。
 *
 * 本用例钉三件事, 缺一都会退化成"看起来能用":
 *   ① 必须**抛异常**, 而不是安静地按 socket 建链(那样用户会以为拿到了自动选路,
 *      实际永远走 UDP 且无任何告警 —— T0 决策清单 J-4 的公共 API 脚枪);
 *   ② 报错信息必须**可操作**: 要认出是 Auto 且要指路到 IPC_SHM/IPC_SOCKET。
 *      否则调用方看到通用报错会去改成 IPC_SOCKET —— pub/sub 的同机最优解通常是
 *      SHM, 那是比现在更糟的解法;
 *   ③ 现有 IPC_SHM/IPC_SOCKET 路径**不受影响**(拒绝不能顺手改变既有语义)。
 * ------------------------------------------------------------------------- */
TEST(DzIpcAutoType, PubSubRejectsIpcAutoExplicitly)
{
    auto td = TopicDataPtrMake<dzIPC::Msg::TestMsg>(0);

    /* ① 发布者: 构造即拒绝, 且绝不返回一个"能用但不自动"的对象。 */
    bool pub_threw = false;
    std::string pub_what;
    try
    {
        PublisherIPCPtr pub = PublisherIPCPtrMake(td, "AutoTypePubSub", 1, static_cast<IPCType>(2), false);
        /* 走到这里就已经违规: 没有抛异常。下面用 InitChannel 兼容"延后拒绝"的
         * 实现(若实现改成构造期不抛、建链期才抛, 本用例同样能判出来)。 */
        pub->InitChannel();
        ADD_FAILURE() << "自动选路(值 2)传给发布者没有报错 —— 公共 API 静默降级脚枪仍在";
    }
    catch (const std::invalid_argument& e)
    {
        pub_threw = true;
        pub_what = e.what();
    }
    catch (const std::exception& e)
    {
        pub_what = e.what();
    }
    EXPECT_TRUE(pub_threw) << "期望 std::invalid_argument, 实得: " << pub_what;

    /* ② 报错必须可操作: 认得 Auto, 且指路到显式传输。 */
    EXPECT_NE(pub_what.find("value 2"), std::string::npos)
        << "报错未点名那个值, 调用方无法判断是自己的错还是传了非法枚举值: " << pub_what;
    EXPECT_NE(pub_what.find("IPC_SHM"), std::string::npos)
        << "报错未指路到 IPC_SHM, 调用方最可能的反应是改成 IPC_SOCKET —— 那是更糟的解法: " << pub_what;

    /* 订阅者与发布者是对称的两处(两个 pimpl 各有一段 dispatch), 必须都拒绝。 */
    auto td2 = TopicDataPtrMake<dzIPC::Msg::TestMsg>(0);
    bool sub_threw = false;
    std::string sub_what;
    try
    {
        SubscriberIPCPtr sub = SubscriberIPCPtrMake(td2, "AutoTypePubSub", 1, 10, static_cast<IPCType>(2), false);
        sub->InitChannel();
        ADD_FAILURE() << "自动选路(值 2)传给订阅者没有报错 —— 只堵了一半";
    }
    catch (const std::invalid_argument& e)
    {
        sub_threw = true;
        sub_what = e.what();
    }
    catch (const std::exception& e)
    {
        sub_what = e.what();
    }
    EXPECT_TRUE(sub_threw) << "期望 std::invalid_argument, 实得: " << sub_what;
    EXPECT_NE(sub_what.find("value 2"), std::string::npos) << sub_what;

    /* ③ 既有两个传输不受影响: 构造与建链都正常(本用例不做收发, 收发由
     *    DzIpcSocket/DzIpcShm 两组既有用例覆盖 —— 这里只确认拒绝没有波及它们)。 */
    auto td3 = TopicDataPtrMake<dzIPC::Msg::TestMsg>(0);
    EXPECT_NO_THROW({
        PublisherIPCPtr pub = PublisherIPCPtrMake(td3, "AutoTypeExplicitShm", 1, IPC_SHM, false);
        pub->InitChannel();
    }) << "IPC_SHM 路径被 Auto 的拒绝逻辑波及";
    auto td4 = TopicDataPtrMake<dzIPC::Msg::TestMsg>(0);
    EXPECT_NO_THROW({
        SubscriberIPCPtr sub = SubscriberIPCPtrMake(td4, "AutoTypeExplicitSocket", 1, 10, IPC_SOCKET, false);
        sub->InitChannel();
    }) << "IPC_SOCKET 路径被 Auto 的拒绝逻辑波及";

    /* 真·非法枚举值仍走原有的通用报错(Auto 的拒绝是**特例**而不是替换掉原分支)。
     * 取 4 而不是更大的数: 无固定底层类型的枚举, 超出取值范围的值是 UB。
     * ⛔ 不能再用 3 —— SocketOnly 加入后 3 已合法, 那样这条用例会静默失去意义。 */
    auto td5 = TopicDataPtrMake<dzIPC::Msg::TestMsg>(0);
    bool bogus_threw = false;
    try
    {
        PublisherIPCPtr pub = PublisherIPCPtrMake(td5, "AutoTypeBogus", 1, static_cast<IPCType>(4), false);
        pub->InitChannel();
    }
    catch (const std::invalid_argument&)
    {
        bogus_threw = true;
    }
    EXPECT_TRUE(bogus_threw) << "未知枚举值不再被拒绝 —— 通用分支被 Auto 的改动吃掉了";
}

int main(int argc, char** argv)
{
    ::testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

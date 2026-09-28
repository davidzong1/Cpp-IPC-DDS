/* [t8] P0/P1 并发与生命周期回归：`reset_message` / `reset_callback` / stop-restart churn。
 *
 * 为什么必须单列一个套件（阻塞解阻方案 §3.3 / §9）：
 *   t7 的 C-01/C-02 是「`receive_state_` 这个 shared_ptr 成员被多线程无锁读写」的数据竞争，
 *   而**既有 test/ 对 reset_message / reset_callback / stop_data_plane / restart_data_plane
 *   零覆盖**（grep 命中数为 0）=> 功能用例全绿对它是不可证伪的。本套件把这三条入口真正
 *   并发跑起来，判据是「不崩、不挂、请求仍能往返」。
 *
 * ⛔ 本套件**不**替代结构性审查（访问点必须全部收口到访问口）；TSAN 是补充证据。
 */
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "gtest/gtest.h"

#include "dzIPC/socket_ser_cli_ipc.h"
#include "ipc_srv/request_response_test/request_response_test.hpp"

namespace {
using namespace std::chrono_literals;

std::atomic<int> g_cb_calls{0};

void echo_plus_one(std::shared_ptr<dzIPC::ServiceData>& msg)
{
    g_cb_calls.fetch_add(1, std::memory_order_relaxed);
    auto req = std::static_pointer_cast<dzIPC::Srv::RequestResponseTestRequest>(msg->request());
    auto res = std::static_pointer_cast<dzIPC::Srv::RequestResponseTestResponse>(msg->response());
    res->response = req->request;
    for (auto& v : res->response) v += 1.0;
}

std::shared_ptr<dzIPC::ServiceData> make_sd()
{
    return std::make_shared<dzIPC::ServiceData>(
        std::make_shared<dzIPC::Srv::RequestResponseTestRequest>(),
        std::make_shared<dzIPC::Srv::RequestResponseTestResponse>());
}

/* 一次往返：成功且返回值正确才算 true。 */
bool one_rpc(dzIPC::socket::socket_cli_ipc& cli, uint64_t tmo_ms = 2000)
{
    auto sd = make_sd();
    sd->request()->msgcast<dzIPC::Srv::RequestResponseTestRequest>()->request = {1.0, 2.0, 3.0};
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(tmo_ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (cli.send_request(sd, 1500))
        {
            const auto r = sd->response()->msgcast<dzIPC::Srv::RequestResponseTestResponse>()->response;
            return r.size() == 3 && r[0] == 2.0 && r[1] == 3.0 && r[2] == 4.0;
        }
        std::this_thread::sleep_for(2ms);
    }
    return false;
}

std::string topic_for(const char* tag)
{
    static std::atomic<int> n{0};
    return std::string("t8conc_") + tag + "_" + std::to_string(n.fetch_add(1));
}

}   // namespace

/* ---- 用例 1：并发 reset_message / reset_callback + 正常往返 ---- */
TEST(SocketSerConcurrency, ResetMessageAndCallbackWhileReceiving)
{
    const std::string topic = topic_for("reset");
    dzIPC::socket::socket_ser_ipc server(topic, make_sd(), echo_plus_one, 0, false);
    server.InitChannel();
    dzIPC::socket::socket_cli_ipc client(topic, make_sd(), 0, false);
    client.InitChannel();

    ASSERT_TRUE(one_rpc(client)) << "建立连接后的第一次往返失败（用例前提不成立）";

    std::atomic<bool> stop{false};
    std::atomic<int> ok{0};
    std::atomic<int> churn{0};

    /* 并发改写：这两条正是 t7 C-01/C-02 的触发路径（此前 test/ 零覆盖）。 */
    std::thread mutator([&] {
        while (!stop.load(std::memory_order_acquire))
        {
            server.reset_message(make_sd());
            server.reset_callback(echo_plus_one);
            churn.fetch_add(1, std::memory_order_relaxed);
            /* 不睡：让改写与收包在窗口里尽可能重叠（这才是竞争的必要条件）。 */
        }
    });

    for (int i = 0; i < 60; ++i)
    {
        if (one_rpc(client)) ok.fetch_add(1, std::memory_order_relaxed);
    }
    stop.store(true, std::memory_order_release);
    mutator.join();

    EXPECT_GT(churn.load(), 0) << "改写线程没有跑起来（用例无效）";
    EXPECT_GT(ok.load(), 0) << "并发改写期间一次往返都没有成功";
    EXPECT_GE(g_cb_calls.load(), ok.load()) << "callback 次数少于成功返回次数（丢投递）";
}

/* ---- 用例 2：stop -> restart churn 期间不挂、之后仍能往返 ---- */
TEST(SocketSerConcurrency, StopRestartChurnKeepsRoundTripAlive)
{
    const std::string topic = topic_for("churn");
    dzIPC::socket::socket_ser_ipc server(topic, make_sd(), echo_plus_one, 0, false);
    server.InitChannel();
    dzIPC::socket::socket_cli_ipc client(topic, make_sd(), 0, false);
    client.InitChannel();

    ASSERT_TRUE(one_rpc(client)) << "建立连接后的第一次往返失败（用例前提不成立）";

    /* 停/起循环：teardown_receive_path 与 start_receive_path 会被反复执行，
     * 覆盖「停止后重启能再次成功 claim owner」这条契约（§4.3）。 */
    for (int i = 0; i < 3; ++i)
    {
        server.stop_data_plane();
        std::this_thread::sleep_for(50ms);
        server.restart_data_plane();
        std::this_thread::sleep_for(50ms);
    }

    bool any = false;
    for (int i = 0; i < 10 && !any; ++i)
    {
        any = one_rpc(client);
    }
    EXPECT_TRUE(any) << "stop/restart churn 之后再也收不到请求（owner 或接收路径未恢复）";
}

/* ---- 用例 3：reset_* 与 stop/restart **同时**发生（阻塞解阻方案 §3.3 的组合要求）---- */
TEST(SocketSerConcurrency, ResetChurnAndStopRestartTogether)
{
    const std::string topic = topic_for("mix");
    dzIPC::socket::socket_ser_ipc server(topic, make_sd(), echo_plus_one, 0, false);
    server.InitChannel();
    dzIPC::socket::socket_cli_ipc client(topic, make_sd(), 0, false);
    client.InitChannel();

    ASSERT_TRUE(one_rpc(client)) << "建立连接后的第一次往返失败（用例前提不成立）";

    std::atomic<bool> stop{false};
    std::atomic<int> churn{0};

    std::thread mutator([&] {
        while (!stop.load(std::memory_order_acquire))
        {
            server.reset_message(make_sd());
            server.reset_callback(echo_plus_one);
            churn.fetch_add(1, std::memory_order_relaxed);
        }
    });

    /* 与改写并发地反复拆/建数据面：拆路径里的 teardown_receive_path 会 clear 成员指针，
     * 与 reset_message/reset_callback 的读构成 t7 C-01 描述的那条竞争。 */
    for (int i = 0; i < 4; ++i)
    {
        server.stop_data_plane();
        server.restart_data_plane();
    }

    stop.store(true, std::memory_order_release);
    mutator.join();

    int ok = 0;
    for (int i = 0; i < 20 && ok == 0; ++i)
    {
        if (one_rpc(client)) ++ok;
    }
    EXPECT_GT(churn.load(), 0) << "改写线程没有跑起来（用例无效）";
    EXPECT_GT(ok, 0) << "reset 与 stop/restart 并发之后再也收不到请求";
}

/* DZFlat 在服务/客户(ser-cli)路径上的回归 —— docs/dzflat_known_issues.md 第 6 条
 *
 * pub/sub 之外, shm_ser_cli 同样走 SHM 且共享同一个 chunk 池, 收益机制完全相同。
 * 此前它被排除在射程外(ServiceGenerator 不发射 DZFlat 段), 现已接入。
 *
 * 与 pub/sub 的差别: ser/cli 用的是 ipc::server(single-single-unicast), 不是 route。
 * 其 recv 侧 recycle 无条件归还(sub_rc 恒 true) —— 对借样没有影响, 因为一条消息只有
 * 一个接收方, 它的 buff_t 析构即归还。这个差异正是本文件要证明无害的东西。
 */
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/shm_ser_cli_ipc.h"
#include "ipc_srv/request_response_test/request_response_test.hpp"

namespace {
using namespace std::chrono_literals;

class DzFlatSwitch
{
public:
    explicit DzFlatSwitch(bool on) : prev_(dzIPC::IsDzFlatEnabled())
    {
        dzIPC::EnableDzFlat(on);
        dzIPC::ResetDzFlatCounters();
    }
    ~DzFlatSwitch() { dzIPC::EnableDzFlat(prev_); }

private:
    bool prev_;
};

std::shared_ptr<dzIPC::ServiceData> make_sd()
{
    return std::make_shared<dzIPC::ServiceData>(
        std::make_shared<dzIPC::Srv::RequestResponseTestRequest>(),
        std::make_shared<dzIPC::Srv::RequestResponseTestResponse>());
}

/* 服务端把请求每项 +1 回传。request 可能是 DZFlat 借样只读视图(request_view), 也可能
 * 是 TLV 物化 owning(request()) —— 两种都处理: 视图路径零拷贝是默认, owning 是 TLV。 */
void echo_plus_one(std::shared_ptr<dzIPC::ServiceData>& msg)
{
    auto res = std::static_pointer_cast<dzIPC::Srv::RequestResponseTestResponse>(msg->response());
    auto rv = msg->request_view<dzIPC::Srv::RequestResponseTestRequestFlat>();
    if (rv.valid())
    {
        auto span = rv.request();   /* span<const double> */
        res->response.resize(span.size());
        for (std::size_t i = 0; i < span.size(); ++i) res->response[i] = span[i] + 1.0;
    }
    else
    {
        auto req = std::static_pointer_cast<dzIPC::Srv::RequestResponseTestRequest>(msg->request());
        res->response.resize(req->request.size());
        for (std::size_t i = 0; i < req->request.size(); ++i)
            res->response[i] = req->request[i] + 1.0;
    }
}

void run_case(bool dzflat_on, const char* topic, std::size_t n)
{
    DzFlatSwitch sw{dzflat_on};

    auto srv_sd = make_sd();
    dzIPC::shm::shm_ser_ipc server{topic, srv_sd, echo_plus_one, false};
    server.InitChannel();

    auto cli_sd = make_sd();
    dzIPC::shm::shm_cli_ipc client{topic, cli_sd, false};
    client.InitChannel();
    const auto deadline = std::chrono::steady_clock::now() + 4000ms;
    while (!client.handshake_completed() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_TRUE(client.handshake_completed()) << "握手超时";

    std::vector<double> data(n);
    for (std::size_t i = 0; i < n; ++i) data[i] = double(i) * 1.5;

    bool ok = false;
    for (int attempt = 0; attempt < 40 && !ok; ++attempt)
    {
        cli_sd->request()->msgcast<dzIPC::Srv::RequestResponseTestRequest>()->request = data;
        ok = client.send_request(cli_sd);
        if (!ok) std::this_thread::sleep_for(20ms);
    }
    ASSERT_TRUE(ok) << "请求未得到响应";

    if (dzflat_on)
    {
        /* 严格分流: 开 DZFlat 时请求与响应都该以只读视图交付, 而非物化 owning。 */
        EXPECT_TRUE(cli_sd->response_is_view()) << "DZFlat 响应未走视图路径";
        auto rv = cli_sd->response_view<dzIPC::Srv::RequestResponseTestResponseFlat>();
        ASSERT_TRUE(rv.valid());
        auto span = rv.response();
        ASSERT_EQ(span.size(), n);
        for (std::size_t i = 0; i < n; ++i)
        {
            ASSERT_DOUBLE_EQ(span[i], data[i] + 1.0) << "i=" << i;
        }
    }
    else
    {
        EXPECT_FALSE(cli_sd->response_is_view()) << "TLV 响应不应走视图路径";
        auto res = cli_sd->response()->msgcast<dzIPC::Srv::RequestResponseTestResponse>();
        ASSERT_EQ(res->response.size(), n);
        for (std::size_t i = 0; i < n; ++i)
        {
            ASSERT_DOUBLE_EQ(res->response[i], data[i] + 1.0) << "i=" << i;
        }
    }
}

}   // namespace

/* 严格分流: 开 DZFlat 时服务端回调收到的 request 是只读视图(request_is_view),
 * 不开时是物化 owning。response 侧同理(见 run_case 的 response_is_view 断言)。 */
TEST(DzFlatSerCli, ServerReceivesRequestAsViewWhenDzflat)
{
    for (int mode = 0; mode < 2; ++mode)
    {
        const bool on = (mode == 1);
        DzFlatSwitch sw{on};
        const std::string topic = std::string("dzflat_sc_reqview") + (on ? "_f" : "_t");

        auto srv_sd = make_sd();
        bool saw_view = false;
        dzIPC::shm::shm_ser_ipc server{
            topic, srv_sd,
            [&saw_view](std::shared_ptr<dzIPC::ServiceData>& msg) {
                saw_view = msg->request_is_view();
                auto res =
                    std::static_pointer_cast<dzIPC::Srv::RequestResponseTestResponse>(msg->response());
                auto rv = msg->request_view<dzIPC::Srv::RequestResponseTestRequestFlat>();
                if (rv.valid())
                {
                    auto span = rv.request();
                    res->response.resize(span.size());
                    for (std::size_t i = 0; i < span.size(); ++i)
                        res->response[i] = span[i] + 1.0;
                }
                else
                {
                    auto req =
                        std::static_pointer_cast<dzIPC::Srv::RequestResponseTestRequest>(msg->request());
                    res->response.resize(req->request.size());
                    for (std::size_t i = 0; i < req->request.size(); ++i)
                        res->response[i] = req->request[i] + 1.0;
                }
            },
            false};
        server.InitChannel();

        auto cli_sd = make_sd();
        dzIPC::shm::shm_cli_ipc client{topic, cli_sd, false};
        client.InitChannel();
        const auto deadline = std::chrono::steady_clock::now() + 4000ms;
        while (!client.handshake_completed() && std::chrono::steady_clock::now() < deadline)
        {
            std::this_thread::sleep_for(10ms);
        }
        ASSERT_TRUE(client.handshake_completed());

        std::vector<double> data(200, 1.0);
        cli_sd->request()->msgcast<dzIPC::Srv::RequestResponseTestRequest>()->request = data;
        bool ok = false;
        for (int attempt = 0; attempt < 40 && !ok; ++attempt)
        {
            ok = client.send_request(cli_sd);
            if (!ok) std::this_thread::sleep_for(20ms);
        }
        ASSERT_TRUE(ok);
        EXPECT_EQ(saw_view, on) << (on ? "开 DZFlat 时回调未收到视图"
                                       : "关 DZFlat 时回调却收到视图");
    }
}

/* 开关关闭(默认态): 既有 TLV 路径行为不得改变。 */
TEST(DzFlatSerCli, TlvRoundTripUnchanged)
{
    run_case(false, "dzflat_sc_tlv", 2000);
    EXPECT_EQ(dzIPC::DzFlatPublishCount(), 0u) << "开关关闭却走了 DZFlat";
}

/* 开关打开: 请求与响应都应走借样, 且值一致。 */
TEST(DzFlatSerCli, DzFlatRoundTrip)
{
    run_case(true, "dzflat_sc_flat", 2000);
    /* 回退是静默的, 所以必须查计数器 —— 否则本用例在实现完全失效时也会全绿。
     * 一次往返两条消息(请求 + 响应), 都该走 DZFlat。 */
    EXPECT_GE(dzIPC::DzFlatPublishCount(), 2u)
        << "一条也没走 DZFlat(dzflat=" << dzIPC::DzFlatPublishCount()
        << " fallback=" << dzIPC::DzFlatFallbackCount() << ") —— 全部静默回退了";
}

/* 反复往返不得耗尽 chunk 池: unicast 的 recycle 语义与 route 不同, 这里钉住它。 */
TEST(DzFlatSerCli, RepeatedRoundTripsDoNotExhaustPool)
{
    DzFlatSwitch sw{true};

    auto srv_sd = make_sd();
    dzIPC::shm::shm_ser_ipc server{"dzflat_sc_loop", srv_sd, echo_plus_one, false};
    server.InitChannel();

    auto cli_sd = make_sd();
    dzIPC::shm::shm_cli_ipc client{"dzflat_sc_loop", cli_sd, false};
    client.InitChannel();
    const auto deadline = std::chrono::steady_clock::now() + 4000ms;
    while (!client.handshake_completed() && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_TRUE(client.handshake_completed());

    std::vector<double> data(1500, 3.25);
    int done = 0;
    /* 远超 32 块的池容量: 只有每次往返都把 chunk 还回去才能全部成功。 */
    for (int i = 0; i < 100; ++i)
    {
        cli_sd->request()->msgcast<dzIPC::Srv::RequestResponseTestRequest>()->request = data;
        if (client.send_request(cli_sd)) ++done;
    }
    EXPECT_GE(done, 90) << "100 次往返只成功 " << done << " 次 —— chunk 未被回收";
    EXPECT_GT(dzIPC::DzFlatPublishCount(), 0u);
}

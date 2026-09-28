/* [t8-C05] SHM ser-cli 有界 FIFO 的**单条 >8 MiB** 行为对照（阻塞解阻方案 §6.3 规则 2）。
 *
 * 为什么单列这个用例：t8 给 SerState 的请求 FIFO 落地了
 *   「pending_bytes + request_bytes <= kMaxPendingBytes(8 MiB)」的入库前判定，
 *   并对「队列为空 + 单条超限」补了一条显式特例（放行该条并计 oversize_admissions，
 *   因为它已经被 try_recv 收下、无法退回通道，拒绝就等于静默丢弃）。
 *
 * 判据（可判定）：单条 > 8 MiB 的请求必须**被正常处理并应答**（值逐元素校验），
 *   且服务端不崩溃、不永久阻塞。若容量判定把这条误判成「超限 ⇒ 丢弃」，用例红。
 *
 * ⛔ 本用例**不**证明「8 MiB 是内存上界」——恰恰相反：它证实单条可超过该值，
 *   因此严格上界是 8 MiB + max(单条实测尺寸)（见 t8 报告 §6）。
 */
#include <atomic>
#include <chrono>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

#include "dzIPC/shm_ser_cli_ipc.h"
#include "ipc_srv/request_response_test/request_response_test.hpp"

namespace {
using namespace std::chrono_literals;

std::atomic<int> g_oversize_cb_calls{0};
std::atomic<int> g_slow_cb_calls{0};

/* 慢 callback：把处理侧拖慢，用来观察「慢处理期间是否丢请求」。 */
void slow_echo(std::shared_ptr<dzIPC::ServiceData>& msg)
{
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    g_slow_cb_calls.fetch_add(1, std::memory_order_relaxed);
    auto req = std::static_pointer_cast<dzIPC::Srv::RequestResponseTestRequest>(msg->request());
    auto res = std::static_pointer_cast<dzIPC::Srv::RequestResponseTestResponse>(msg->response());
    res->response = req->request;
    for (auto& v : res->response) v += 1.0;
}

void echo_plus_one(std::shared_ptr<dzIPC::ServiceData>& msg)
{
    g_oversize_cb_calls.fetch_add(1, std::memory_order_relaxed);
    auto req = std::static_pointer_cast<dzIPC::Srv::RequestResponseTestRequest>(msg->request());
    auto res = std::static_pointer_cast<dzIPC::Srv::RequestResponseTestResponse>(msg->response());
    res->response.resize(req->request.size());
    for (std::size_t i = 0; i < req->request.size(); ++i) res->response[i] = req->request[i] + 1.0;
}

std::shared_ptr<dzIPC::ServiceData> make_sd()
{
    return std::make_shared<dzIPC::ServiceData>(
        std::make_shared<dzIPC::Srv::RequestResponseTestRequest>(),
        std::make_shared<dzIPC::Srv::RequestResponseTestResponse>());
}

std::string topic_for(const char* tag)
{
    static std::atomic<int> n{0};
    return std::string("t8cap_") + tag + "_" + std::to_string(n.fetch_add(1));
}

}   // namespace

/* 单条请求 > kMaxPendingBytes(8 MiB)：必须走「空队列超限放行」并被正常应答。 */
TEST(ShmSerFifoCapacity, SingleRequestLargerThanByteCapIsStillAnswered)
{
    /* 9 MiB payload = 1,179,648 个 double > 8 MiB 上限。 */
    constexpr std::size_t kDoubles = (9u << 20) / sizeof(double);
    const std::string topic = topic_for("oversize");
    g_oversize_cb_calls.store(0);

    dzIPC::shm::shm_ser_ipc server(topic, make_sd(), echo_plus_one, 0, false);
    server.InitChannel();

    dzIPC::shm::shm_cli_ipc cli(topic, make_sd(), 0, false);
    cli.InitChannel();
    const auto hs_deadline = std::chrono::steady_clock::now() + 10s;
    while (!cli.handshake_completed() && std::chrono::steady_clock::now() < hs_deadline)
    {
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_TRUE(cli.handshake_completed()) << "握手未完成（用例前提不成立）";

    auto sd = make_sd();
    auto& req = sd->request()->msgcast<dzIPC::Srv::RequestResponseTestRequest>()->request;
    req.resize(kDoubles);
    for (std::size_t i = 0; i < kDoubles; ++i) req[i] = static_cast<double>(i % 1000);

    bool ok = false;
    const auto deadline = std::chrono::steady_clock::now() + 60s;
    while (!ok && std::chrono::steady_clock::now() < deadline)
    {
        if (cli.send_request(sd, 30000))
        {
            const auto& resp = sd->response()->msgcast<dzIPC::Srv::RequestResponseTestResponse>()->response;
            if (resp.size() != kDoubles)
            {
                std::cerr << "[t8-C05] response size mismatch: got " << resp.size() << " want " << kDoubles << std::endl;
            }
            else
            {
                ok = true;
                for (std::size_t i = 0; i < kDoubles; ++i)
                {
                    if (resp[i] != req[i] + 1.0) { ok = false; std::cerr << "[t8-C05] value mismatch at " << i << std::endl; break; }
                }
            }
        }
        if (!ok) std::this_thread::sleep_for(5ms);
    }
    std::cerr << "[t8-C05] oversize request: bytes=" << (kDoubles * sizeof(double))
              << " answered_ok=" << ok << " callback_calls=" << g_oversize_cb_calls.load() << std::endl;
    EXPECT_TRUE(ok) << "单条 >8 MiB 请求未被正常应答（疑似被容量判定丢弃）";
    EXPECT_GE(g_oversize_cb_calls.load(), 1) << "callback 未被调用";
}

/* 慢 callback + 单客户端：SHM ser-cli 是**同步** 1:1 单播请求/应答（libipc single-single unicast），
 * 客户端在收到应答前不会再发下一条 ⇒ FIFO 深度上界 = 1，永远压不满。这条用例把这件事
 * 钉在数值上：慢处理期间零丢弃（每条发出且被应答），且 callback 覆盖全部请求。
 * 它同时是 R-1 的可判定依据（FIFO 背压判定点在本协议下不可达；唯一可达的容量路径是
 * 「单条 >8 MiB 特例」，已由上面的 SingleRequestLargerThanByteCapIsStillAnswered 直接覆盖）。
 * ⛔ 「FIFO 压不满」是**协议语义**的结论，不是「没测到就认为没问题」。 */
TEST(ShmSerFifoCapacity, SlowCallbackSingleClientLosesNothing)
{
    constexpr int kRequests = 20;
    const std::string topic = topic_for("slow");
    g_slow_cb_calls.store(0);

    dzIPC::shm::shm_ser_ipc server(topic, make_sd(), slow_echo, 0, false);
    server.InitChannel();
    dzIPC::shm::shm_cli_ipc cli(topic, make_sd(), 0, false);
    cli.InitChannel();
    const auto hs_deadline = std::chrono::steady_clock::now() + 10s;
    while (!cli.handshake_completed() && std::chrono::steady_clock::now() < hs_deadline)
    {
        std::this_thread::sleep_for(10ms);
    }
    ASSERT_TRUE(cli.handshake_completed()) << "握手未完成（用例前提不成立）";

    int answered = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < kRequests; ++i)
    {
        auto sd = make_sd();
        sd->request()->msgcast<dzIPC::Srv::RequestResponseTestRequest>()->request = {1.0, 2.0, 3.0};
        const auto deadline = std::chrono::steady_clock::now() + 20s;
        while (std::chrono::steady_clock::now() < deadline)
        {
            if (cli.send_request(sd, 10000))
            {
                const auto r = sd->response()->msgcast<dzIPC::Srv::RequestResponseTestResponse>()->response;
                if (r.size() == 3 && r[0] == 2.0 && r[1] == 3.0 && r[2] == 4.0) { ++answered; break; }
            }
            std::this_thread::sleep_for(2ms);
        }
    }
    const auto elapsed_ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                                std::chrono::steady_clock::now() - t0).count();
    std::cerr << "[t8-C05] slow-callback single client: sent=" << kRequests << " answered=" << answered
              << " callback_calls=" << g_slow_cb_calls.load() << " elapsed_ms=" << elapsed_ms << std::endl;
    EXPECT_EQ(answered, kRequests) << "慢处理期间有请求未被应答（疑似丢弃）";
    EXPECT_GE(g_slow_cb_calls.load(), kRequests) << "callback 次数少于请求数（投递丢失）";
}

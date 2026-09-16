/* IPC_SOCKET_ONLY: ser-cli 的"强制纯 socket"逃生口
 *
 * 背景: ser-cli 的 IPC_SOCKET 已被归一化为自动选路(server_ipc.cc 的
 * normalize_sercli_type), 于是"我要纯 socket"这个意图一度**没有公共表达方式**。
 * IPC_SOCKET_ONLY 补上它, 用途是**对照实验/基线**与排查("到底是 SHM 腿还是 socket
 * 腿出问题")。
 *
 * 本文件钉两件互为一体的事:
 *   ① IPC_SOCKET_ONLY → 传输**始终**是 socket, 同机也不切 SHM(逃生口是真的);
 *   ② IPC_SOCKET      → 同机**确实**切到 SHM(归一化没被写坏)。
 * 两条缺一不可: 只有①, 一个"永远不切"的实现也能全绿; 只有②, 逃生口不存在也测不出来。
 */
#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>

#include "gtest/gtest.h"

#include "dzIPC/dzipc.h"
#include "ipc_srv/request_response_test/request_response_test.hpp"

namespace {

using namespace std::chrono_literals;

std::string uniq(const char* tag)
{
    static std::atomic<int> n{0};
    return std::string("/socket_only/") + tag + "_" + std::to_string(n.fetch_add(1));
}

std::shared_ptr<dzIPC::ServiceData> make_sd()
{
    return std::make_shared<dzIPC::ServiceData>(
        std::make_shared<dzIPC::Srv::RequestResponseTestRequest>(),
        std::make_shared<dzIPC::Srv::RequestResponseTestResponse>());
}

struct Observed
{
    bool rpc_ok = false;
    bool handshook = false;
    dzIPC::path::Kind client_kind = dzIPC::path::Kind::None;
    dzIPC::path::Kind server_kind = dzIPC::path::Kind::None;
};

/* 起一对 ser-cli, 等状态稳定, 发一次请求; 报告两侧**当下实际**的传输。 */
Observed run_once(dzIPC::IPCType type)
{
    Observed o;
    const std::string topic =
        uniq(type == dzIPC::IPC_SOCKET_ONLY ? "only" : (type == dzIPC::IPC_SOCKET ? "auto" : "shm"));

    auto srv_sd = make_sd();
    dzIPC::shm::shm_ser_ipc* unused_probe = nullptr;
    (void)unused_probe;
    auto server = dzIPC::ServerIPCPtrMake(
        topic, srv_sd,
        [](dzIPC::ServerDataPtr& sd) {
            sd->response() = std::make_shared<dzIPC::Srv::RequestResponseTestResponse>();
        },
        0, type, false);
    server->InitChannel();

    auto cli_sd = make_sd();
    auto client = dzIPC::ClientIPCPtrMake(topic, cli_sd, 0, type, false);
    client->InitChannel("");

    /* 给自动选路留足判定 + 会合的时间。IPC_SOCKET_ONLY 不会切, 所以它只等握手 ——
     * 否则这条用例每次都要白等满整个预算。 */
    const auto dl = std::chrono::steady_clock::now() + 8s;
    while (std::chrono::steady_clock::now() < dl)
    {
        const bool both_shm = client->transport_current() == dzIPC::path::Kind::Shm
                              && server->transport_current() == dzIPC::path::Kind::Shm;
        if (both_shm)
        {
            break;
        }
        if (type == dzIPC::IPC_SOCKET_ONLY && client->handshake_completed())
        {
            /* 再给一点余量: 若实现有 bug 真去切了 SHM, 也要能被抓到, 而不是抢跑。 */
            std::this_thread::sleep_for(600ms);
            break;
        }
        std::this_thread::sleep_for(20ms);
    }

    o.handshook = client->handshake_completed();
    auto srv_req = make_sd();
    srv_req->request() = std::make_shared<dzIPC::Srv::RequestResponseTestRequest>();
    o.rpc_ok = client->send_request(srv_req, 2000);
    o.client_kind = client->transport_current();
    o.server_kind = server->transport_current();
    return o;
}

}   // namespace

/* ① IPC_SOCKET_ONLY: 同机也不得切 SHM。 */
TEST(SocketOnlyTransport, ForcesPureSocketEvenOnSameHost)
{
    const Observed o = run_once(dzIPC::IPC_SOCKET_ONLY);
    EXPECT_TRUE(o.handshook) << "IPC_SOCKET_ONLY 没握手 —— 逃生口连基本功能都不成立";
    ASSERT_TRUE(o.rpc_ok) << "IPC_SOCKET_ONLY 的 RPC 没通";
    EXPECT_EQ(o.client_kind, dzIPC::path::Kind::Socket)
        << "IPC_SOCKET_ONLY 的客户端切到了 SHM —— 逃生口是假的(对照实验做不了)";
    EXPECT_EQ(o.server_kind, dzIPC::path::Kind::Socket)
        << "IPC_SOCKET_ONLY 的服务端切到了 SHM —— 逃生口是假的";
}

/* ② IPC_SOCKET: 同机必须切 SHM(否则归一化被写坏了)。 */
TEST(SocketOnlyTransport, IpcSocketStillSwitchesToShmOnSameHost)
{
    const Observed o = run_once(dzIPC::IPC_SOCKET);
    ASSERT_TRUE(o.rpc_ok) << "IPC_SOCKET 的 RPC 没通";
    EXPECT_EQ(o.client_kind, dzIPC::path::Kind::Shm)
        << "IPC_SOCKET 没有归一到自动选路(同机应切 SHM)—— 归一化被写坏了";
    EXPECT_EQ(o.server_kind, dzIPC::path::Kind::Shm);
}

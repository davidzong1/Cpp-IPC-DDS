/* ser-cli 握手通道(base+2)只读观测探针的测试。
 *
 * 覆盖三类:
 *  ① 解码: 复用产品自己的序列化器造帧, 断言解出的 path_state 正确; 并**回放**
 *     旧版本(3 字节载荷)的帧, 断言走 Unknown 而不是读到页尾的随机字节。
 *  ② 默认兼容: 不打开观测时, 快照与 sniffer_info 的默认值必须是"什么都没发生"。
 *  ③ **UDP 多监听的业务影响**(本任务的核心安全问题): 组播下多挂一个监听者,
 *     业务对端收到的帧数必须与没有观察者时**逐个数字相同**; 且观察者的存在与
 *     排空动作绝不向通道注入任何报文。
 */
#include <atomic>
#include <chrono>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include "dzIPC/common/hash.h"
#include "dzIPC/dzipc.h"
#include "handshake_probe.h"
#include "ipc_msg/ipc_msg_base/udp_id_init_msg.hpp"
#include "ipc_srv/request_response_test/request_response_test.hpp"
#include "libipc/udp.h"
#include "info.h"

#include <gtest/gtest.h>

namespace {

using namespace dzIPC;

/* 用产品自己的序列化器造一帧握手报文 —— 判据来源单一, 测试不另写一套布局。 */
ipc::buffer make_handshake_frame(bool host, bool cli, bool run_status, IpcPubSubIdInitMsg::PathState ps)
{
    IpcPubSubIdInitMsg m;
    m.host_flag = host;
    m.cli_flag = cli;
    m.run_status = run_status;
    m.path_state = ps;
    return m.serialize();
}

/* 把新帧(4 字节载荷)改造成旧版本的 3 字节载荷帧: 去掉第 4 个载荷字节, 并把页尾里的
 * total_size 从 16 改成 15(大端, 位于 [n-8, n-5])。这正是 T2 §3 D2 里"旧对端"发出来的
 * 东西, 也是"不能用 size()>=4 判"那条教训的实体。 */
std::vector<uint8_t> to_legacy_frame(const ipc::buffer& raw)
{
    const auto* p = static_cast<const uint8_t*>(raw.data());
    std::vector<uint8_t> v(p, p + raw.size());
    v.erase(v.begin() + 3);
    const std::size_t n = v.size();
    v[n - 8] = 0;
    v[n - 7] = 0;
    v[n - 6] = 0;
    v[n - 5] = static_cast<uint8_t>(n);   // total_size = 整个缓冲长度(含 12 字节页尾)
    return v;
}

int drain_node(ipc::socket::UDPNode& n)
{
    int c = 0;
    while (true)
    {
        ipc::buffer b = n.receive_nowait();
        if (b.size() == 0)
        {
            return c;
        }
        ++c;
    }
}

}   // namespace

TEST(HandshakeWatch, DecodesPathStateFromProductFrame)
{
    const ipc::buffer raw = make_handshake_frame(true, false, true, IpcPubSubIdInitMsg::PathState::ProposeShm);
    const handshake_frame f = decode_handshake_frame(raw.data(), raw.size());

    EXPECT_TRUE(f.valid);
    EXPECT_TRUE(f.host_flag);
    EXPECT_FALSE(f.cli_flag);
    EXPECT_TRUE(f.run_status);
    EXPECT_TRUE(f.has_path_state);
    EXPECT_EQ(static_cast<uint8_t>(IpcPubSubIdInitMsg::PathState::ProposeShm), f.path_state);
    EXPECT_EQ(4u, f.payload_size);
    EXPECT_STREQ("ProposeShm", path_state_name(f.path_state));
}

TEST(HandshakeWatch, LegacyFrameFallsBackToUnknownNotEmpty)
{
    const ipc::buffer raw = make_handshake_frame(false, true, true, IpcPubSubIdInitMsg::PathState::ConfirmShm);
    const std::vector<uint8_t> legacy = to_legacy_frame(raw);
    ASSERT_EQ(15u, legacy.size());

    const handshake_frame f = decode_handshake_frame(legacy.data(), legacy.size());
    EXPECT_TRUE(f.valid);      // 旧帧仍然是本通道的一帧
    EXPECT_FALSE(f.host_flag);
    EXPECT_TRUE(f.cli_flag);
    EXPECT_FALSE(f.has_path_state);
    EXPECT_EQ(static_cast<uint8_t>(IpcPubSubIdInitMsg::PathState::Unknown), f.path_state);
    EXPECT_EQ(3u, f.payload_size);
}

TEST(HandshakeWatch, RejectsShapesThatAreNotThisChannelsFrame)
{
    const ipc::buffer raw = make_handshake_frame(true, false, true, IpcPubSubIdInitMsg::PathState::ProposeShm);

    EXPECT_FALSE(decode_handshake_frame(nullptr, 0).valid);
    EXPECT_FALSE(decode_handshake_frame(raw.data(), 14).valid);   // 比最短合法帧还短

    /* host/cli 同真或同假都不是对端帧(产品侧服务端认 check_cli, 客户端认 check_host,
     * 所以"恰好一个"才是本通道的身份)。 */
    const ipc::buffer both = make_handshake_frame(true, true, true, IpcPubSubIdInitMsg::PathState::ProposeShm);
    EXPECT_FALSE(decode_handshake_frame(both.data(), both.size()).valid);
    const ipc::buffer neither = make_handshake_frame(false, false, true, IpcPubSubIdInitMsg::PathState::ProposeShm);
    EXPECT_FALSE(decode_handshake_frame(neither.data(), neither.size()).valid);
}

TEST(HandshakeWatch, DefaultOffIsSilentAndInert)
{
    /* 默认行为兼容的落点: 没开观测 ⇒ 快照 opened==false, 且 sniffer_info 的默认值
     * 就是"什么都没发生"。渲染层按 opened 早退, 于是一个字符都不多。 */
    const handshake_probe probe;
    const handshake_snapshot snap = probe.snapshot();
    EXPECT_FALSE(snap.opened);
    EXPECT_FALSE(snap.server.seen);
    EXPECT_FALSE(snap.client.seen);
    EXPECT_EQ(0u, snap.frames);

    const sniffer_info empty{};
    EXPECT_FALSE(empty.hs.opened);
    EXPECT_TRUE(empty.request.empty());
    EXPECT_TRUE(empty.response.empty());

    /* 没打开时 poll() 必须是纯 no-op(不崩、不阻塞、不改变任何计数)。 */
    handshake_probe p2;
    p2.poll();
    EXPECT_EQ(0u, p2.snapshot().frames);
}

/* ---- ③ UDP 多监听的业务影响: 本任务的核心安全问题 ---- */

TEST(HandshakeWatch, ExtraListenerNeitherStealsFramesNorInjectsThem)
{
    const std::string topic = "hs_watch_multi_1";
    const int domain = 1;
    const std::string group = common::udp_discovery_addr_calculate(topic);
    const uint16_t data_port = common::udp_discovery_port_calculate(topic, domain);
    const uint16_t hs_port = static_cast<uint16_t>(data_port + common::kUdpPortOffsetHandshake);

    constexpr int kFrames = 20;

    /* 对照组: 只有业务双方, 没有观察者。 */
    int control_b_got = 0;
    {
        ipc::socket::UDPNode a(topic.c_str(), group.c_str(), hs_port, ipc::socket::NodeRole::SendRecv);
        ipc::socket::UDPNode b(topic.c_str(), group.c_str(), hs_port, ipc::socket::NodeRole::SendRecv);
        ASSERT_TRUE(a.connect());
        ASSERT_TRUE(b.connect());
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        drain_node(a);
        drain_node(b);
        for (int i = 0; i < kFrames; ++i)
        {
            ipc::buffer f(const_cast<char*>("PAD"), 3);
            a.send(f);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        control_b_got = drain_node(b);
        EXPECT_EQ(kFrames, control_b_got) << "对照组的业务对端自己就没收全, 后面无从比较";
    }

    /* 实验组: 完全相同, 只多一个**只读**观察者(用探针本体的 RecvOnly 节点)。 */
    int with_watcher_b_got = 0;
    {
        ipc::socket::UDPNode a(topic.c_str(), group.c_str(), hs_port, ipc::socket::NodeRole::SendRecv);
        ipc::socket::UDPNode b(topic.c_str(), group.c_str(), hs_port, ipc::socket::NodeRole::SendRecv);
        handshake_probe watcher;
        ASSERT_TRUE(watcher.open(topic, domain));
        ASSERT_TRUE(a.connect());
        ASSERT_TRUE(b.connect());
        std::this_thread::sleep_for(std::chrono::milliseconds(30));
        drain_node(a);
        drain_node(b);
        for (int i = 0; i < kFrames; ++i)
        {
            ipc::buffer f(const_cast<char*>("PAD"), 3);
            a.send(f);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(80));
        /* 观察者主动排空自己的副本(等同 sniffer 线程里的 poll), 再看业务侧少没少。 */
        watcher.poll();
        with_watcher_b_got = drain_node(b);

        EXPECT_EQ(kFrames, with_watcher_b_got) << "观察者偷走了业务对端的报文";
        EXPECT_EQ(control_b_got, with_watcher_b_got) << "有无观察者时业务收到的帧数不一致";

        /* 反向再验一次注入: 观察者排空之后, 业务侧不应多出任何"来源不明"的报文。
         * 这里的判据是 A 的排空计数 —— 它只包含 A 自己的回绕副本(IP_MULTICAST_LOOP=1),
         * 不允许出现观察者注入的帧。 */
        const int a_echo = drain_node(a);
        EXPECT_EQ(kFrames, a_echo) << "出现了额外的报文(疑似观察者写入通道)";

        watcher.close();
    }

    /* 对照组与实验组必须逐数字相同 —— 这就是"多监听对业务无影响"的可复现证据。 */
    EXPECT_EQ(control_b_got, with_watcher_b_got);
}

TEST(HandshakeWatch, RealSerCliPairIsObservableAndUnaffected)
{
    const std::string topic = "hs_watch_e2e";
    const int domain = 1;
    const uint32_t msg_id = 114'514;
    std::atomic<bool> stop_server{false};
    std::atomic<int> served{0};

    std::thread server(
        [&]()
        {
            ServerDataPtr msg
                = ServerDataPtrMake<dzIPC::Srv::RequestResponseTestRequest, dzIPC::Srv::RequestResponseTestResponse>(
                    msg_id);
            ServerIPCPtr ipc = ServerIPCPtrMake(
                topic, msg,
                [&served](ServerDataPtr& m)
                {
                    auto req = m->request()->msgcast<dzIPC::Srv::RequestResponseTestRequest>();
                    auto res = m->response()->msgcast<dzIPC::Srv::RequestResponseTestResponse>();
                    res->response.assign(req->request.size(), 1.0);
                    ++served;
                },
                domain, IPC_SOCKET_ONLY, false);
            ipc->InitChannel();
            while (!stop_server.load(std::memory_order_acquire))
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            }
        });

    /* ⛔ 故意**后**于服务端打开: 握手帧是周期性的(约 10 帧/秒), 所以观察者不需要
     * 与服务端同时启动也能看到状态 —— 这消除了一个启动顺序依赖。 */
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    handshake_probe probe;
    ASSERT_TRUE(probe.open(topic, domain));

    ServerDataPtr cli_msg
        = ServerDataPtrMake<dzIPC::Srv::RequestResponseTestRequest, dzIPC::Srv::RequestResponseTestResponse>(msg_id);
    ClientIPCPtr client = ClientIPCPtrMake(topic, cli_msg, domain, IPC_SOCKET_ONLY, false);
    client->InitChannel();

    std::vector<double> payload(64, 3.0);
    cli_msg->request()->msgcast<dzIPC::Srv::RequestResponseTestRequest>()->request = payload;
    int ok = 0;
    for (int i = 0; i < 10 && ok < 3; ++i)
    {
        if (client->send_request(cli_msg, 500))
        {
            ++ok;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    EXPECT_GE(ok, 3) << "挂了只读观察者之后业务 RPC 反而不通了";

    handshake_snapshot snap = probe.snapshot();
    for (int i = 0; i < 200 && !(snap.server.seen && snap.client.seen); ++i)
    {
        probe.poll();
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
        snap = probe.snapshot();
    }

    EXPECT_TRUE(snap.server.seen) << "没看到服务端(host_flag)的握手帧";
    EXPECT_TRUE(snap.client.seen) << "没看到客户端(cli_flag)的握手帧";
    EXPECT_EQ(0u, snap.undecodable) << "收到了本通道之外的东西(解码判据过松?)";
    /* ⛔ 这一对必须用 IPC_SOCKET_ONLY, **不能**用 IPC_SOCKET。
     *
     * 本用例的判据是"双方都不提议 SHM ⇒ path_state 停在 Unknown"。而 ser-cli 的
     * ser-cli 的 IPC_SOCKET **就是**自动选路(见 server_ipc.cc 的分派): 同主机时
     * 服务端会 PeerInPool → 提议 SHM, 客户端跟进 ⇒ path_state 变成
     * ProposeShm/ConfirmShm, 这条断言的前提就不成立了。
     *
     * 实测它一度仍然通过 —— 但那是**采样时序侥幸**: 快照取在协商完成之前, 此刻
     * path_state 还是 Unknown。机器慢一点、或轮询晚一点, 它就会翻。IPC_SOCKET_ONLY
     * 才是这个意图的正确表达: 强制纯 socket, 永不提议 SHM, 判据变成确定性的。
     *
     * 注意旧帧与"没提议"在这里取值相同, 用 has_path_state 区分二者。 */
    EXPECT_TRUE(snap.server.has_path_state);
    EXPECT_TRUE(snap.client.has_path_state);
    EXPECT_EQ(static_cast<uint8_t>(IpcPubSubIdInitMsg::PathState::Unknown), snap.server.path_state);
    EXPECT_EQ(static_cast<uint8_t>(IpcPubSubIdInitMsg::PathState::Unknown), snap.client.path_state);
    EXPECT_GE(served.load(), 3);

    /* RPC 全程跑通 + 观察者一直在排空, 业务侧的握手状态仍然必须是"活着"。 */
    EXPECT_TRUE(snap.server.run_status);

    stop_server.store(true, std::memory_order_release);
    server.join();
    probe.close();
}

/* T3: ser-cli 同主机路径切换 / 跨主机保持 socket / 断连清理 / 重连重判
 *
 * 覆盖任务列表里 T3 的验收项: 同主机、跨主机、握手中断、已建立连接断开、重连循环。
 * 判据一律取**可断言的量**(消息收发结果 + 统计字段 + 资源计数), 不以"日志看起来
 * 切换成功"为准 —— 这是任务列表统一约束的原话。
 *
 * 判据不是"有没有切换成功"就够, 更本质的是三条一致性断言(T2 §8 第 3 条):
 *   ① 服务端 callback 调用次数 == 客户端成功返回次数 ⇒ 证明**无重复副作用**;
 *   ② `dup_delivery_detected` 恒 0;
 *   ③ 切换前后无 SHM 通道残留(下一连接能正常建立)。
 */
#include <atomic>
#include <chrono>
#include <cstring>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "gtest/gtest.h"

#include "dzIPC/auto_ser_cli_ipc.h"
#include "dzIPC/common/control_plane.h"
#include "dzIPC/ipc_info_pool.h"
#include "dzIPC/shm_ser_cli_ipc.h"
#include "dzIPC/socket_ser_cli_ipc.h"
#include "ipc_msg/ipc_msg_base/udp_id_init_msg.hpp"
#include "ipc_srv/request_response_test/request_response_test.hpp"

#if !defined(_WIN32)
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
using namespace std::chrono_literals;

std::shared_ptr<dzIPC::ServiceData> make_sd()
{
    return std::make_shared<dzIPC::ServiceData>(std::make_shared<dzIPC::Srv::RequestResponseTestRequest>(),
                                                std::make_shared<dzIPC::Srv::RequestResponseTestResponse>());
}

int g_cb_calls = 0;

/* 服务端把请求每项 +1 回传。加个计数: "callback 次数 == 客户端成功返回次数"
 * 是"没有重复执行"的直接判据。 */
void echo_plus_one(std::shared_ptr<dzIPC::ServiceData>& msg)
{
    g_cb_calls++;
    auto req = std::static_pointer_cast<dzIPC::Srv::RequestResponseTestRequest>(msg->request());
    auto res = std::static_pointer_cast<dzIPC::Srv::RequestResponseTestResponse>(msg->response());
    res->response = req->request;
    for (auto& v : res->response) v += 1.0;
}

/* 一次往返: 成功且返回值正确才计 true。 */
bool one_rpc(dzIPC::cli_ipc_base& cli, uint64_t tmo_ms = 2000)
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

bool wait_for(const std::function<bool()>& pred, uint64_t tmo_ms)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(tmo_ms);
    while (std::chrono::steady_clock::now() < deadline)
    {
        if (pred()) return true;
        std::this_thread::sleep_for(2ms);
    }
    return pred();
}

int thread_count()
{
#if !defined(_WIN32)
    std::ifstream f("/proc/self/status");
    std::string line;
    while (std::getline(f, line))
    {
        if (line.rfind("Threads:", 0) == 0)
        {
            return std::atoi(line.c_str() + 8);
        }
    }
#endif
    return -1;
}

/* 每个用例用独立 topic: 这些用例在同一个进程里跑, 复用 topic 会让上一轮
 * 残留的池条目/控制面段变成下一轮的证据或"占用", 使断言不可复现。 */
std::string topic_for(const char* tag)
{
    static std::atomic<int> n{0};
    return std::string("t3_") + tag + "_" + std::to_string(n.fetch_add(1));
}

struct Rig
{
    std::string topic;
    std::atomic<bool> stop{false};
    std::thread ser_th;
    std::unique_ptr<dzIPC::autopath::auto_cli_ipc> cli;

    explicit Rig(const char* tag, dzIPC::autopath::Options srv_opts = {}, dzIPC::autopath::Options cli_opts = {})
        : topic(topic_for(tag))
    {
        ser_th = std::thread([this, srv_opts] {
            dzIPC::autopath::auto_ser_ipc s(topic, make_sd(), echo_plus_one, 3, srv_opts, false);
            s.InitChannel();
            while (!stop.load()) std::this_thread::sleep_for(5ms);
        });
        std::this_thread::sleep_for(150ms);
        cli = std::make_unique<dzIPC::autopath::auto_cli_ipc>(topic, make_sd(), 3, cli_opts, false);
    }
    ~Rig()
    {
        cli.reset();
        stop.store(true);
        if (ser_th.joinable()) ser_th.join();
    }

    void start_client() { cli->InitChannel(); }
    bool await_handshake(uint64_t tmo = 5000)
    {
        return wait_for([this] { return cli->handshake_completed(); }, tmo);
    }
    void reset_client(std::unique_ptr<dzIPC::autopath::auto_cli_ipc> c) { cli = std::move(c); }
};

}   // namespace

/* ------------------------------------------------------------------------- *
 * 1. 同主机: 握手先走 UDP, 之后自动切 SHM; 且切换前后同一套 RPC 语义不变。
 * ------------------------------------------------------------------------- */
TEST(SerCliAutoPath, SameHostSwitchesToShm)
{
    const int threads_before = thread_count();
    g_cb_calls = 0;
    {
        Rig rig("same");
        rig.start_client();
        ASSERT_TRUE(rig.await_handshake()) << "UDP 引导握手未完成";

        ASSERT_TRUE(wait_for([&] { return rig.cli->transport_current() == dzIPC::path::Kind::Shm; }, 4000))
            << "同主机未切到 SHM; decision=" << dzIPC::path::to_string(rig.cli->status().decision())
            << " fallback=" << dzIPC::path::to_string(rig.cli->status().fallback());

        int ok = 0;
        for (int i = 0; i < 8; ++i)
        {
            if (one_rpc(*rig.cli)) ++ok;
        }
        EXPECT_EQ(ok, 8) << "切换后 SHM 路径 RPC 未全部成功";

        /* ① 无重复副作用: 服务端 callback 次数必须**等于**客户端成功返回次数。
         * 双跑(两条路都活)会让这个数偏大, 那正是 T2 §0 的硬约束要防的。 */
        EXPECT_EQ(g_cb_calls, ok) << "callback 次数与成功返回次数不一致 => 疑似重复投递";
        EXPECT_EQ(rig.cli->status().dup_delivery_detected.load(), 0u);
        EXPECT_EQ(rig.cli->status().switch_successes.load(), 1u);
        EXPECT_EQ(rig.cli->status().switch_fallbacks.load(), 0u);
        EXPECT_EQ(rig.cli->status().decision(), dzIPC::path::DecisionReason::PeerInPool);
        EXPECT_EQ(rig.cli->status().fallback(), dzIPC::path::FallbackReason::None);
    }
    /* 资源释放: 析构后线程数应回到进入本用例前的水平(切换层/两臂线程全部 join)。 */
    ASSERT_TRUE(wait_for([&] { return thread_count() <= threads_before; }, 4000))
        << "析构后线程未回落: before=" << threads_before << " after=" << thread_count();
}

/* ------------------------------------------------------------------------- *
 * 2. 跨主机(无证据): 握手完成后**留在 socket**, 不建 SHM 腿, 且不算失败。
 * ------------------------------------------------------------------------- */
TEST(SerCliAutoPath, NoEvidenceStaysOnSocket)
{
    dzIPC::autopath::Options none;
    none.force_no_evidence = true;
    Rig rig("cross", none, none);
    rig.start_client();
    ASSERT_TRUE(rig.await_handshake());

    /* 给判定留足时间: 若实现有"等 T_est 再回退"的路径, 这里会暴露出来。 */
    std::this_thread::sleep_for(std::chrono::milliseconds(600));

    EXPECT_EQ(rig.cli->transport_current(), dzIPC::path::Kind::Socket) << "无证据却切了 SHM";
    EXPECT_EQ(rig.cli->status().decision(), dzIPC::path::DecisionReason::NoEvidence);
    EXPECT_EQ(rig.cli->status().switch_attempts.load(), 0u) << "无证据时不应尝试建立 SHM 腿";
    EXPECT_EQ(rig.cli->status().selected(), dzIPC::path::Kind::Socket);

    int ok = 0;
    for (int i = 0; i < 4; ++i)
    {
        if (one_rpc(*rig.cli)) ++ok;
    }
    EXPECT_EQ(ok, 4) << "跨主机路径(纯 socket)必须照常工作";
}

/* ------------------------------------------------------------------------- *
 * 3. 握手中断: 握手未完成就析构。必须**有界**退出, 不能卡在重试循环里。
 * ------------------------------------------------------------------------- */
TEST(SerCliAutoPath, HandshakeInterruptIsBounded)
{
    /* 没有任何服务端: 客户端的建链/握手都会失败。旧实现里那四处重试循环
       `while(!connect()) sleep(1s)` 不看 running, 析构会永久阻塞 —— 这里用
       一个硬上界把那种回归钉住。 */
    const auto t0 = std::chrono::steady_clock::now();
    {
        dzIPC::autopath::auto_cli_ipc c("t3_no_server_at_all", make_sd(), 3, dzIPC::autopath::Options{}, false);
        c.InitChannel();
        std::this_thread::sleep_for(120ms);   // 让建链/握手线程真的跑到失败分支
    }
    const auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0).count();
    EXPECT_LT(dt, 6000) << "析构被重试循环卡住(dt=" << dt << " ms)";
}

/* ------------------------------------------------------------------------- *
 * 4. 已建立连接断开 + 重连重判: 服务端消失后状态清理干净; 服务端回来时
 *    **重新判定**(而不是复用上一轮的结论)。
 * ------------------------------------------------------------------------- */
TEST(SerCliAutoPath, DisconnectThenReconnectRejudges)
{
    Rig rig("reconnect");
    rig.start_client();
    ASSERT_TRUE(rig.await_handshake());
    ASSERT_TRUE(wait_for([&] { return rig.cli->transport_current() == dzIPC::path::Kind::Shm; }, 4000));
    const uint64_t successes_before = rig.cli->status().switch_successes.load();
    EXPECT_EQ(successes_before, 1u);
    EXPECT_TRUE(one_rpc(*rig.cli));

    /* 断开: 服务端整体析构(含 shutdown). */
    rig.stop.store(true);
    rig.ser_th.join();
    ASSERT_TRUE(wait_for([&] { return !rig.cli->handshake_completed(); }, 4000)) << "对端断开后握手状态未回落";

    /* 清理有**两条**独立通道, 谁先命中取决于哪条先被判死, 这是设计使然:
     *   - SHM 腿先没了 => RemoteIoFailure (回退方向唯一: SHM -> socket, T2 §3 D7 铁律 1);
     *   - UDP 握手通道静默超时 => WithdrawnByPeer (唯一存活权威, T2 §3 D6)。
     * 两者都必须以"回到 socket + 判定缓存清空"收尾 —— 这才是本用例要钉的东西。
     * ⛔ 不能只等 transport==Socket 就断言: RemoteIoFailure 会**先**把它翻成 socket,
     * 而那时 UDP 侧的静默判死(1.5 s)还没到。本用例的第一版就踩了这个竞态(5 次里偶发 1 次)。 */
    ASSERT_TRUE(wait_for(
        [&] {
            return rig.cli->transport_current() == dzIPC::path::Kind::Socket
                   && rig.cli->status().evidence.kind.load() == 0
                   && rig.cli->status().fallback() == dzIPC::path::FallbackReason::WithdrawnByPeer;
        },
        5000))
        << "断连清理未收敛到 (socket, 证据清空, WithdrawnByPeer); 实际 transport="
        << dzIPC::path::to_string(rig.cli->transport_current())
        << " evidence=" << rig.cli->status().evidence.kind.load()
        << " fallback=" << dzIPC::path::to_string(rig.cli->status().fallback());
    EXPECT_EQ(rig.cli->status().selected(), dzIPC::path::Kind::Socket);

    /* 重连: 服务端重新起来 -> 客户端必须**重新走**一遍判定并再次切到 SHM。 */
    rig.stop.store(false);
    rig.ser_th = std::thread([&] {
        dzIPC::autopath::auto_ser_ipc s(rig.topic, make_sd(), echo_plus_one, 3, dzIPC::autopath::Options{}, false);
        s.InitChannel();
        while (!rig.stop.load()) std::this_thread::sleep_for(5ms);
    });
    ASSERT_TRUE(wait_for([&] { return rig.cli->handshake_completed(); }, 6000)) << "重连后握手未恢复";
    ASSERT_TRUE(wait_for([&] { return rig.cli->transport_current() == dzIPC::path::Kind::Shm; }, 6000))
        << "重连后未重新判定到 SHM (decision=" << dzIPC::path::to_string(rig.cli->status().decision()) << ")";
    EXPECT_EQ(rig.cli->status().switch_successes.load(), 2u) << "重连后应重新建立一次 SHM 会话";
    EXPECT_TRUE(one_rpc(*rig.cli));
}

/* ------------------------------------------------------------------------- *
 * 5. 占用守卫: 目标 SHM 通道被**别的进程**占着时不得切换
 *    (shm_ser_ipc::InitChannel 无条件 clear_storage, 会摧毁既有连接 —— T2 §7 R3)。
 * ------------------------------------------------------------------------- */
TEST(SerCliAutoPath, OccupiedShmChannelIsNotClobbered)
{
#if !defined(_WIN32)
    const std::string topic = topic_for("occupied");
    const size_t domain = 3;

    pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0)
    {
        /* 子进程: 先占住这条 SHM 服务通道并一直活着。 */
        dzIPC::shm::shm_ser_ipc occupant(topic, make_sd(), echo_plus_one, domain, false);
        occupant.InitChannel();
        while (true) std::this_thread::sleep_for(100ms);
        ::_exit(0);
    }
    /* 等子进程登记进池(占用判据读的是池条目)。 */
    std::this_thread::sleep_for(400ms);

    dzIPC::autopath::Options opts;
    bool switched = false, fell_back = false;
    {
        std::atomic<bool> stop{false};
        std::thread ser([&] {
            dzIPC::autopath::auto_ser_ipc s(topic, make_sd(), echo_plus_one, domain, opts, false);
            s.InitChannel();
            while (!stop.load()) std::this_thread::sleep_for(5ms);
        });
        std::this_thread::sleep_for(200ms);

        dzIPC::autopath::auto_cli_ipc c(topic, make_sd(), domain, opts, false);
        c.InitChannel();
        ASSERT_TRUE(wait_for([&] { return c.handshake_completed(); }, 5000));
        std::this_thread::sleep_for(700ms);   // 留出判定窗口

        switched = c.transport_current() == dzIPC::path::Kind::Shm;
        fell_back = c.status().fallback() == dzIPC::path::FallbackReason::ShmChannelOccupied;
        EXPECT_FALSE(switched) << "占用守卫失效: 切进了别人正占着的 SHM 通道";
        EXPECT_TRUE(fell_back) << "回退原因不是 ShmChannelOccupied, 而是 "
                               << dzIPC::path::to_string(c.status().fallback());
        EXPECT_EQ(c.status().decision(), dzIPC::path::DecisionReason::ChannelOccupied);
        /* 拒绝切换之后必须仍然可用(回退到 socket, 而不是半死状态)。 */
        EXPECT_TRUE(one_rpc(c));
        stop.store(true);
        if (ser.joinable()) ser.join();
    }
    ::kill(child, SIGKILL);
    ::waitpid(child, nullptr, 0);
#endif
}

/* ------------------------------------------------------------------------- *
 * 6. 线格式: 加第 4 个字节必须**双向兼容** —— 这是 D2 选"扩展握手帧"的前提
 *    (T2 §6 V1: 旧解码器解新帧不失败、新解码器解旧帧不失败)。
 * ------------------------------------------------------------------------- */
TEST(SerCliAutoPath, HandshakeFrameBackwardCompatible)
{
    /* 新帧 -> 旧读取路径。旧代码只读 data_ptr[0..2], 用 check_* 直接验。 */
    IpcPubSubIdInitMsg new_msg;
    new_msg.host_flag = true;
    new_msg.cli_flag = false;
    new_msg.run_status = true;
    new_msg.path_state = IpcPubSubIdInitMsg::PathState::ProposeShm;
    ipc::buffer new_buf = std::move(new_msg.serialize());

    EXPECT_TRUE(new_msg.check_host(new_buf)) << "新增字段后旧的 host_flag 读取被破坏";
    EXPECT_FALSE(new_msg.check_cli(new_buf));
    EXPECT_TRUE(new_msg.check_run_status(new_buf));
    EXPECT_EQ(IpcPubSubIdInitMsg::peek_path_state(new_buf), IpcPubSubIdInitMsg::PathState::ProposeShm);
    /* 尾部 12 字节仍在末尾: get_tail_msg 读的是 data.size()-12。 */
    EXPECT_EQ(new_msg.get_tail_msg(new_buf).total_size, new_buf.size());

    /* 旧帧 -> 新读取路径: 3 字节载荷(老版本序列化的样子) 必须被识别成 Unknown,
       也就是"按 socket 走"的安全默认, 而不是越界读或解析失败。 */
    ipc::buffer old_buf(new uint8_t[15], 15, [](void* p, std::size_t) { delete[] static_cast<uint8_t*>(p); });
    std::memset(old_buf.data(), 0, old_buf.size());
    static_cast<uint8_t*>(old_buf.data())[0] = 1;   // host_flag
    static_cast<uint8_t*>(old_buf.data())[1] = 1;   // cli_flag
    static_cast<uint8_t*>(old_buf.data())[2] = 1;   // run_status
    EXPECT_FALSE(IpcPubSubIdInitMsg::has_path_state(old_buf));
    EXPECT_EQ(IpcPubSubIdInitMsg::peek_path_state(old_buf), IpcPubSubIdInitMsg::PathState::Unknown);
    EXPECT_TRUE(new_msg.check_host(old_buf));
    EXPECT_TRUE(new_msg.check_cli(old_buf));
    EXPECT_TRUE(new_msg.check_run_status(old_buf));

    /* deserialize 也要在旧帧上安全(不置越界、保持 Unknown)。 */
    IpcPubSubIdInitMsg decoded;
    decoded.deserialize(old_buf);
    EXPECT_TRUE(decoded.deserialize_ok());
    EXPECT_EQ(decoded.path_state, IpcPubSubIdInitMsg::PathState::Unknown);
    EXPECT_TRUE(decoded.host_flag);
    EXPECT_TRUE(decoded.run_status);
}

/* ------------------------------------------------------------------------- *
 * 7. 占用守卫的第二证据源(T3 补充): 池里**查不到**、但控制面段被别的活进程占着
 *    时, 守卫必须仍然判成占用。
 *
 * 为什么单独立这一条: 池是**登记**式的证据(T2 §3 D1), 只有主动 rebind 过的进程
 * 才看得见。没登记(或登记被回收)的既有服务端在池里查不到 ⇒ 漏判 ⇒ 后续的
 * clear_storage 会摧毁别人的活动连接。控制面段是**建出来**的, 所以它能在池沉默
 * 时补上这一条。
 *
 * 同时钉住两个方向:
 *   - 判据本身(单元级): 只有另一进程的控制面段时 shm_channel_occupied 必须是 true;
 *   - 端到端: 同主机 Auto 不得切进被占通道, 且必须仍然可用(留在 socket 上)。
 *   - 反向: owner 死后同一段必须**不再**算占用, 否则崩一次就让该 topic 永久
 *     退不回 SHM(保守方向不能保守到自锁)。
 * ------------------------------------------------------------------------- */
TEST(SerCliAutoPath, OccupancyDetectedWithoutPoolEntry)
{
#if !defined(_WIN32)
    const std::string topic = topic_for("ctrlonly");
    const size_t domain = 3;
    const std::string ctrl_name = dzIPC::shm::ser_service_control_name(topic, domain);

    pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0)
    {
        /* 子进程: 只把控制面段建起来并置 Ready(owner = 自己), **不登记信息池**。
         * 这正是池启发式看不见、而 clear_storage 会摧毁其连接的情形。 */
        dzIPC::control_plane_shm::TopicControlPlane cp;
        if (!cp.open(ctrl_name))
        {
            ::_exit(2);
        }
        cp.begin_rebuild();
        cp.set_ready();
        while (true)
        {
            cp.heartbeat();
            std::this_thread::sleep_for(10ms);
        }
        ::_exit(0);
    }
    std::this_thread::sleep_for(400ms);

    /* 前提: 池里确实**没有**这条 topic+domain 的 SHM 条目 —— 否则本用例退化成
     * 用例 5(池证据), 检验不到新证据源。前提不成立就该显式失败而不是假装通过。 */
    bool pool_has_entry = false;
    for (const auto& e : dzIPC::info_pool::IpcInfoPool::instance().snapshot(false))
    {
        if (e.in_use && e.alive
            && (e.kind == dzIPC::info_pool::EntryKind::ShmServer || e.kind == dzIPC::info_pool::EntryKind::ShmClient)
            && e.topic_name == topic && e.domain_id == static_cast<int32_t>(domain))
        {
            pool_has_entry = true;
        }
    }
    ASSERT_FALSE(pool_has_entry) << "前提不成立: 池里已有该 topic 的 SHM 条目, 本用例检验不到控制面证据";

    /* 判据(单元级): 只凭控制面证据就要判出占用。 */
    EXPECT_TRUE(dzIPC::autopath::shm_channel_occupied(topic, domain, static_cast<int32_t>(::getpid())))
        << "池沉默 + 控制面被别的活进程占着, 守卫却判成未占用";

    /* 探测必须是**只读**的: 再探一次仍要看到占用 —— 若探测自己把段 unlink 了
     * (误用 release 而非 release_no_unlink), 第二次就会变成"未占用"。 */
    EXPECT_TRUE(dzIPC::autopath::shm_channel_occupied(topic, domain, static_cast<int32_t>(::getpid())))
        << "第二次探测结果变了 ⇒ 探测本身动了段(不得 unlink 别人正在用的段)";

    /* 端到端: 同主机 Auto 不得切进这条被占通道, 且必须仍然可用。 */
    {
        std::atomic<bool> stop{false};
        std::thread ser([&] {
            dzIPC::autopath::auto_ser_ipc s(topic, make_sd(), echo_plus_one, domain, dzIPC::autopath::Options{}, false);
            s.InitChannel();
            while (!stop.load()) std::this_thread::sleep_for(5ms);
        });
        std::this_thread::sleep_for(200ms);

        dzIPC::autopath::auto_cli_ipc c(topic, make_sd(), domain, dzIPC::autopath::Options{}, false);
        c.InitChannel();
        ASSERT_TRUE(wait_for([&] { return c.handshake_completed(); }, 5000));
        std::this_thread::sleep_for(900ms);   // 留出判定 + 建立窗口

        EXPECT_EQ(c.transport_current(), dzIPC::path::Kind::Socket)
            << "占用守卫漏判: 切进了别人正占着的 SHM 通道";
        EXPECT_EQ(c.status().fallback(), dzIPC::path::FallbackReason::ShmChannelOccupied);
        EXPECT_TRUE(one_rpc(c)) << "拒绝切换之后必须仍然可用(socket), 而不是半死状态";
        stop.store(true);
        if (ser.joinable()) ser.join();
    }

    /* 反向: owner 死后同一段不得再算占用 —— 否则崩溃一次就让该 topic 永久退不回
     * SHM。这里用 SIGKILL 让子进程不走析构(段与 owner_pid 都留下), 正是"残留段"
     * 的真实形态。 */
    ::kill(child, SIGKILL);
    ::waitpid(child, nullptr, 0);
    std::this_thread::sleep_for(150ms);
    EXPECT_FALSE(dzIPC::autopath::shm_channel_occupied(topic, domain, static_cast<int32_t>(::getpid())))
        << "owner 已死的残留段仍被判成占用 ⇒ 崩溃后该 topic 永久退不回 SHM";
#endif
}

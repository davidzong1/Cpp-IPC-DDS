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
#include "dzIPC/common/name_operator.h"
#include "dzIPC/ipc_info_pool.h"
#include "dzIPC/shm_ser_cli_ipc.h"
#include "dzIPC/socket_ser_cli_ipc.h"
#include "handshake_probe.h"
#include "ipc_msg/ipc_msg_base/udp_id_init_msg.hpp"
#include "ipc_srv/request_response_test/request_response_test.hpp"
#include "libipc/shm.h"

#if !defined(_WIN32)
#include <signal.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace {
using namespace std::chrono_literals;

using PS = IpcPubSubIdInitMsg::PathState;
using DR = dzIPC::path::DecisionReason;
using FR = dzIPC::path::FallbackReason;

uint8_t sig_of(PS ps)
{
    return static_cast<uint8_t>(ps);
}
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

/* ------------------------------------------------------------------------- *
 * A11/M3 的判据脚手架
 *
 * 整块平台门控: 它们只服务 POSIX 上的 A11 用例(该用例需要 fork 一个真占用者), 而
 * 本仓在 Windows 上按 -Wall 编译 —— 匿名 namespace 里"只被 #if 掉的代码用到"的函数
 * 会吃 -Wunused-function。整块门掉, 免得给 Windows 构建留一条只为告警存在的分支。
 * ------------------------------------------------------------------------- */
#if !defined(_WIN32)

/* 控制面段头的一次只读采样。
 *
 * ⛔ 读取手法必须与 control_plane_shm::occupied_by_other() 逐条一致:
 *   - acquire(name, 0, open) —— **只 open, 绝不 create**(size=0 时 libipc 既不建段
 *     也不 ftruncate): 用 create|open 的话, 段不存在时会凭空建一个空壳,
 *     "段不存在"与"段是空的"就再也分不开了;
 *   - 配平用 release_no_unlink() —— **绝不 release()**: 后者在引用计数归零时会
 *     shm_unlink, 那会把本用例正要证明"没被摧毁"的段自己删掉(用例会以最讽刺的方式红)。
 *
 * 依赖的是**公开**声明(TopicControl 结构体、kTopicControlMagic), 不是内部细节;
 * 若结构体将来增删字段, 这里会跟着编译期变化, 而 magic 校验保证"读不懂就当没有"。 */
struct ctrl_head
{
    bool exists{false};
    uint32_t magic{0};
    uint32_t generation{0};
    uint32_t state{0};
    int32_t owner_pid{0};
    uint32_t peer_count{0};   // 挂在这条通道上的对端数 = "引用计数"
    int32_t shm_ref{0};       // libipc 层的段引用计数(含本次只读采样自己那一份)
};

ctrl_head read_ctrl_head(const std::string& name)
{
    ctrl_head h;
    ipc::shm::id_t id = ipc::shm::acquire(name.c_str(), 0, ipc::shm::open);
    if (id == nullptr)
    {
        return h;   // ENOENT: 段不存在(唯一"不存在"的判据)
    }
    std::size_t mapped = 0;
    void* mem = ipc::shm::get_mem(id, &mapped);
    if (mem != nullptr && mapped >= sizeof(dzIPC::control_plane_shm::TopicControl))
    {
        const auto* c = static_cast<const dzIPC::control_plane_shm::TopicControl*>(mem);
        h.exists = true;
        h.magic = c->magic.load(std::memory_order_acquire);
        h.generation = c->generation.load(std::memory_order_acquire);
        h.state = c->state.load(std::memory_order_acquire);
        h.owner_pid = c->owner_pid.load(std::memory_order_acquire);
        h.peer_count = c->peer_count.load(std::memory_order_acquire);
        h.shm_ref = ipc::shm::get_ref(id);
    }
    ipc::shm::release_no_unlink(id);
    return h;
}

/* 变异条款的**可执行**形式: 把 shm_channel_occupied() 里"池"那一半单独复算一遍。
 *
 * 池按**原始 topic 名**匹配, 所以对别名碰撞是**盲**的 —— 它必然返回 false。于是
 * "把第二条证据源(派生段探测)去掉"这一变异会让函数退化成这个返回值, 用例里紧随其后的
 * ASSERT_TRUE(shm_channel_occupied(...)) 立刻红。这就是"禁用派生段探测时用例必须失败"。
 *
 * ⛔ 这是**变异论证**, 不是产品判据的第二份副本: 它只复算池匹配那几条 if, 刻意不复制
 * 控制面探测。池的匹配规则一旦变化, 这个前提会先红(而不是让用例静默失去检验力)。 */
bool pool_only_says_occupied(const std::string& topic, size_t domain, int32_t self_pid)
{
    for (const auto& e : dzIPC::info_pool::IpcInfoPool::instance().snapshot(false))
    {
        if (!e.in_use || !e.alive)
        {
            continue;
        }
        if (e.kind != dzIPC::info_pool::EntryKind::ShmServer && e.kind != dzIPC::info_pool::EntryKind::ShmClient)
        {
            continue;
        }
        if (e.topic_name != topic || e.domain_id != static_cast<int32_t>(domain))
        {
            continue;
        }
        if (e.pid == self_pid)
        {
            continue;
        }
        return true;
    }
    return false;
}

/* fork 出来的占用者必须**无条件**收尸: gtest 的 ASSERT_* 会提前 return, 任何一条断言
 * 失败都不能留下一个永远活着的进程(它会污染同进程后续用例的池/段, 并在 /dev/shm 里
 * 留残留段)。 */
struct child_reaper
{
    pid_t pid{-1};
    ~child_reaper()
    {
        if (pid > 0)
        {
            ::kill(pid, SIGKILL);
            ::waitpid(pid, nullptr, 0);
        }
    }
};

/* 服务端线程同理: ~std::thread 对 joinable 线程会直接 std::terminate, 所以"停 + join"
 * 必须绑在作用域上, 不能只写在用例末尾。 */
struct thread_joiner
{
    std::atomic<bool> stop{false};
    std::thread th;
    ~thread_joiner()
    {
        stop.store(true);
        if (th.joinable())
        {
            th.join();
        }
    }
};

/* 收尸之后顺手回收池里的死条目。
 *
 * 为什么需要它: 池是**宿主级**共享段, 而被 SIGKILL 的子进程不会跑析构 —— 它登记的条目
 * 会一直占着槽位(产品侧没有任何 `gc_dead()` 调用点, 见 auto_ser_cli_ipc.cc:45 的说明)。
 * 任何"fork 一个进程去登记点什么"的用例都会因此**每跑一轮永久占掉一格**, 512 格用完之后
 * 新进程再也登记不进池 ⇒ 同机判定永久退化成 NoEvidence。
 *
 * ⛔ 用例里这个守卫的**变量**必须声明在 child_reaper **之前**: 局部对象按声明逆序析构,
 * 于是"先收尸(pid 变死)、再回收(才判得出死)"。这里只回收 pid 已死的条目, 不会碰到
 * 宿主上别人的活条目。 */
struct pool_dead_reaper
{
    ~pool_dead_reaper() { (void)dzIPC::info_pool::IpcInfoPool::instance().gc_dead(); }
};

#endif   // !_WIN32

struct Rig
{
    std::string topic;
    std::atomic<bool> stop{false};
    std::thread ser_th;
    /* 服务端对象的裸指针, 供用例断言**服务端一侧**的记账 —— 此前 Rig 只暴露客户端,
     * 于是"两侧是否都按判定期记了数"只能靠 F2Wire 那条用例(那里服务端是手写的线程)。
     * ⛔ 只在服务端对象存活期间有效: 在 InitChannel **之前**发布(此时构造已完成,
     * 读 status_ 的原子字段是安全的), 线程退出时清空 —— 免得留下悬垂指针。 */
    std::atomic<dzIPC::autopath::auto_ser_ipc*> srv{nullptr};
    std::unique_ptr<dzIPC::autopath::auto_cli_ipc> cli;

    explicit Rig(const char* tag, dzIPC::autopath::Options srv_opts = {}, dzIPC::autopath::Options cli_opts = {})
        : topic(topic_for(tag))
    {
        ser_th = std::thread([this, srv_opts] {
            dzIPC::autopath::auto_ser_ipc s(topic, make_sd(), echo_plus_one, 3, srv_opts, false);
            srv.store(&s, std::memory_order_release);
            s.InitChannel();
            while (!stop.load()) std::this_thread::sleep_for(5ms);
            srv.store(nullptr, std::memory_order_release);
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
        rig.srv.store(&s, std::memory_order_release);
        s.InitChannel();
        while (!rig.stop.load()) std::this_thread::sleep_for(5ms);
        rig.srv.store(nullptr, std::memory_order_release);
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
        /* F2 失败注入: 这一支(fallback=ShmChannelOccupied)此前**只置标签不加计数**,
         * 于是"标签说有回退、计数说没有"—— 按标签做监控会漏报真实回退。此处把
         * 标签与计数钉在一起, 并断言目标不变量 attempts == successes + fallbacks。 */
        EXPECT_EQ(c.status().decision(), dzIPC::path::DecisionReason::ChannelOccupied)
            << "回退原因标签不准: 应记成通道被占(ChannelOccupied)而不是超时/对端未就绪";
        EXPECT_EQ(c.status().switch_attempts.load(), 1u)
            << "被拒绝的切换未计入 switch_attempts ⇒ 监控漏报";
        EXPECT_EQ(c.status().switch_fallbacks.load(), 1u)
            << "被拒绝的切换未计入 switch_fallbacks ⇒ 监控漏报";
        EXPECT_EQ(c.status().switch_attempts.load(),
                  c.status().switch_successes.load() + c.status().switch_fallbacks.load())
            << "不变量被破坏: switch_attempts != switch_successes + switch_fallbacks";
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

/* ------------------------------------------------------------------------- *
 * F1-a: ser-cli 段名的 '/' 最小清洗 —— 恒等性与边界（纯函数, 不起任何 IPC）。
 *
 * 判据是**可断言的字符串**: 段名规则是纯函数, 不需要活体就能钉死。
 *   - 含 '/' 的 topic: 段名内**不得**再有 '/'（否则 POSIX shm_open 恒 EINVAL(22)）;
 *   - 不含 '/' 的 topic: 段名必须与本改动**逐字节相同**（零改名 ⇒ 跨版本互通）;
 *   - legacy（F1 前）规则仍可算出, 作为回滚基准与旧名比对。
 * ------------------------------------------------------------------------- */
TEST(SerCliAutoPath, F1SegmentNameSanitizesOnlySlash)
{
    const size_t domain = 3;

    /* ① 修复目标: 前导 '/' 必须被消除。 */
    const std::string t1 = shm_service_prefix("/demo", domain);
    EXPECT_EQ(t1.find('/'), std::string::npos)
        << "段名内仍有内层 '/' ⇒ shm_open 必返 EINVAL(22); 实际: " << t1;
    EXPECT_EQ(t1, "dz_ipc_d3__demo");

    /* ② 内层 '/' 同样要清, 且**每个**都要清(不能只清第一个)。 */
    const std::string t2 = shm_service_prefix("/demo/depth", domain);
    EXPECT_EQ(t2.find('/'), std::string::npos) << "实际: " << t2;
    EXPECT_EQ(t2, "dz_ipc_d3__demo_depth");
    EXPECT_EQ(shm_service_prefix("a/b/c", domain).find('/'), std::string::npos);

    /* ③ 零改名 —— 这是"新旧版本互通"的**全部**依据: 不含 '/' 的 topic 逐字节不变。
     *    特别钉住 ':' —— 全量 sanitize_topic_name 会把 ':' 换成 '_' 从而**改名**,
     *    那会让新旧版本各建一段、静默不互通（见 F1 设计说明的取舍表）。 */
    for (const char* topic : {"plain", "with_underscore", "with-dash", "with.dot", "rt:chatter"})
    {
        EXPECT_EQ(shm_service_prefix(topic, domain), shm_service_legacy_prefix(topic, domain))
            << "不含 '/' 的 topic 段名被改了（违反零改名）: " << topic;
    }
    /* 反向钉死 ':' 不被清洗: 若有人把本函数改成复用 sanitize_topic_name, 在此失败。 */
    EXPECT_EQ(shm_service_prefix("rt:chatter", domain), "dz_ipc_d3_rt:chatter")
        << "':' 被清洗了 —— 这会给现行可用 topic 改名, 破坏跨版本互通";

    /* ④ legacy 规则仍可算出, 且对含 '/' 的 topic 与新名**不同**（回滚基准可比对）。 */
    EXPECT_NE(shm_service_prefix("/demo", domain), shm_service_legacy_prefix("/demo", domain))
        << "legacy 基准与 F1 新规则算出同一名字 ⇒ 回滚比对失去意义";
    EXPECT_EQ(shm_service_legacy_prefix("/demo", domain), "dz_ipc_d3_/demo")
        << "legacy 规则本身被改动了 —— 回滚基准不再忠实反映 F1 之前的行为";

#if defined(_WIN32)
    /* 平台谓词: Windows 段名没有 '/' 约束, 保持原样 ⇒ 新名与 legacy 完全相同。 */
    EXPECT_EQ(shm_service_prefix("/demo", domain), shm_service_legacy_prefix("/demo", domain))
        << "Windows 不应清洗（平台谓词失效）";
#endif
}

/* ------------------------------------------------------------------------- *
 * F1-b: 端到端 —— topic 含 '/' 时也必须真的切到 SHM。
 *
 * 与用例 1 唯一的不同是 **topic 名里带 '/'**, 而那个 '/' 正是 F1 修复的输入。
 * 修复前: 段名含内层 '/' ⇒ shm_open EINVAL(22) ⇒ 异常被 catch 吞掉 ⇒ **静默**留在
 * socket, `transport_current()` 永远到不了 Shm。活体证据(修复前):
 * build/live_runs/20260915_232219_2580718 —— `fail shm_open[22]` +
 * `FINAL kind=Socket decision=ChannelOccupied`, 而业务仍 8/8 送达(所以只有逐行对账
 * 才看得见)。本用例把"带斜杠 topic 也能切 SHM"变成可断言的事实。
 * ------------------------------------------------------------------------- */
TEST(SerCliAutoPath, F1SlashTopicActuallySwitchesToShm)
{
    g_cb_calls = 0;
    Rig rig("/f1slash");   // topic_for 会拼成 "t3_/f1slash_<n>" —— 含 '/' 的 topic
    ASSERT_NE(rig.topic.find('/'), std::string::npos)
        << "前提不成立: 本用例的 topic 必须含 '/', 实际: " << rig.topic;

    rig.start_client();
    ASSERT_TRUE(rig.await_handshake()) << "带 '/' 的 topic 连 UDP 引导握手都没完成";

    ASSERT_TRUE(wait_for([&] { return rig.cli->transport_current() == dzIPC::path::Kind::Shm; }, 4000))
        << "带 '/' 的 topic 未切到 SHM（F1 未生效）; topic=" << rig.topic
        << " decision=" << dzIPC::path::to_string(rig.cli->status().decision())
        << " fallback=" << dzIPC::path::to_string(rig.cli->status().fallback());

    int ok = 0;
    for (int i = 0; i < 8; ++i)
    {
        if (one_rpc(*rig.cli)) ++ok;
    }
    EXPECT_EQ(ok, 8) << "切换后 SHM 路径 RPC 未全部成功";
    EXPECT_EQ(g_cb_calls, ok) << "callback 次数与成功返回次数不一致 => 疑似重复投递";
    EXPECT_EQ(rig.cli->status().switch_successes.load(), 1u);
    EXPECT_EQ(rig.cli->status().switch_fallbacks.load(), 0u) << "带 '/' 的 topic 回退了";
}

/* ------------------------------------------------------------------------- *
 * F2-a: 对端撤销信号的解码表(纯函数, 不起任何 IPC)。
 *
 * 判据是**可断言的映射**: 协议改造的全部价值就在这张表上 —— 发出去的原因和对方记下的
 * 原因必须是同一张表, 否则"服务端说建腿失败、客户端记成通道被占用"这类错位会一直在。
 * ------------------------------------------------------------------------- */
TEST(SerCliAutoPath, F2PeerWithdrawDecodeIsFaithful)
{
    DR d = DR::PeerInPool;
    FR f = FR::None;

    /* ① 老端的泛化撤销(4): 老端对它**所有**的撤销原因都发 4, 所以原因不可考。
     *    ⛔ 必须记成"原因未区分", 猜成占用就是把老端的"建腿失败"重新说成"通道被占用"
     *    —— 那正是 F2 要修的错位。 */
    ASSERT_TRUE(dzIPC::autopath::decode_peer_withdraw(sig_of(PS::WithdrawToSocket), d, f));
    EXPECT_EQ(f, FR::WithdrawnByPeer) << "老端的 4 被记成了一个具体原因(应记成'原因未区分')";
    EXPECT_NE(d, DR::ChannelOccupied) << "老端的 4 被误判成'通道被占用'";
    EXPECT_EQ(d, DR::ShmNotReady);

    /* ② 三个新原因映射到**互不相同**的 fallback 标签 —— 这就是"至少区分三类"的判据。 */
    ASSERT_TRUE(dzIPC::autopath::decode_peer_withdraw(sig_of(PS::WithdrawChannelOccupied), d, f));
    EXPECT_EQ(f, FR::ShmChannelOccupied);
    const DR occupied_d = d;
    EXPECT_EQ(d, DR::ChannelOccupied);
    ASSERT_TRUE(dzIPC::autopath::decode_peer_withdraw(sig_of(PS::WithdrawEstablishFailed), d, f));
    EXPECT_EQ(f, FR::ShmEstablishFailed) << "建腿失败没有自己的标签";
    ASSERT_TRUE(dzIPC::autopath::decode_peer_withdraw(sig_of(PS::WithdrawRendezvousTimeout), d, f));
    EXPECT_EQ(f, FR::ShmRendezvousTimeout);
    EXPECT_EQ(d, DR::Timeout);
    ASSERT_TRUE(dzIPC::autopath::decode_peer_withdraw(sig_of(PS::WithdrawRuntimeDisconnect), d, f));
    EXPECT_EQ(f, FR::RemoteIoFailure) << "运行期断链没有自己的标签";

    /* 三个类别的 fallback 必须两两不同(否则"区分"只停在枚举上)。 */
    EXPECT_NE(FR::ShmEstablishFailed, FR::ShmRendezvousTimeout);
    EXPECT_NE(FR::ShmEstablishFailed, FR::RemoteIoFailure);
    EXPECT_NE(FR::ShmRendezvousTimeout, FR::RemoteIoFailure);
    EXPECT_EQ(occupied_d, DR::ChannelOccupied);

    /* ③ 不是撤销的信号一律返回 false(调用方继续等), 且**不得改写出参**。
     *    0=尚未提议, 1/2=协商中, 3=保留值, ≥9=**本端不认识的值**。
     *    ⛔ 未知值不能当撤销: 它可能是将来新增的积极信号, 读成拒绝会凭空造出一条
     *    错误的回退原因。返回 false 的后果是等满 T_est 后按超时安全回退。 */
    for (uint8_t sig : {0u, 1u, 2u, 3u, 9u, 99u, 200u, 255u})
    {
        DR d_sentinel = DR::NoEvidence;
        FR f_sentinel = FR::WithdrawnByPeer;
        EXPECT_FALSE(dzIPC::autopath::decode_peer_withdraw(sig, d_sentinel, f_sentinel))
            << "sig=" << static_cast<int>(sig) << " 被当成了撤销信号";
        EXPECT_EQ(d_sentinel, DR::NoEvidence) << "非撤销信号改写了 decision, sig=" << static_cast<int>(sig);
        EXPECT_EQ(f_sentinel, FR::WithdrawnByPeer) << "非撤销信号改写了 fallback, sig=" << static_cast<int>(sig);
    }
}

/* ------------------------------------------------------------------------- *
 * F2-b: 发送端映射 —— 每个回退原因都要能**往返**回同一个标签, 且不得退化回统一的 4。
 * ------------------------------------------------------------------------- */
TEST(SerCliAutoPath, F2FallbackReasonRoundTripsOnTheWire)
{
    const FR reasons[] = {FR::ShmEstablishFailed, FR::ShmRendezvousTimeout, FR::ShmChannelOccupied,
                          FR::RemoteIoFailure, FR::WithdrawnByPeer};
    for (FR r : reasons)
    {
        const uint8_t sig = dzIPC::autopath::wire_signal_for_fallback(r);
        DR d = DR::Pending;
        FR back = FR::None;
        ASSERT_TRUE(dzIPC::autopath::decode_peer_withdraw(sig, d, back))
            << "为 " << dzIPC::path::to_string(r) << " 选出的信号 sig=" << static_cast<int>(sig)
            << " 根本不是撤销信号";
        EXPECT_EQ(back, r) << "往返后原因变了: " << dzIPC::path::to_string(r)
                           << " -> sig=" << static_cast<int>(sig) << " -> " << dzIPC::path::to_string(back);
        /* ⛔ 除 WithdrawnByPeer(它本来就等于"原因未区分")外, 一律不得发 legacy 4 ——
         *    那正是 F2 之前的"所有原因都发 4", 谁改回去这条立刻红。 */
        if (r != FR::WithdrawnByPeer)
        {
            EXPECT_NE(sig, sig_of(PS::WithdrawToSocket))
                << dzIPC::path::to_string(r) << " 退化成了统一的 legacy 4";
        }
    }
    /* None("没有回退")必须发一个**已裁定**的值: 发 0 会让对端以为本端还没裁定(Unknown),
     * 于是它继续等 —— 而本端其实已经决定不动了。 */
    EXPECT_EQ(dzIPC::autopath::wire_signal_for_fallback(FR::None), sig_of(PS::WithdrawToSocket));
    EXPECT_NE(dzIPC::autopath::wire_signal_for_fallback(FR::None), 0u);
}

/* ------------------------------------------------------------------------- *
 * F2-c: 新信号不改变帧布局(新旧帧的字节判据必须原样成立)。
 * ------------------------------------------------------------------------- */
TEST(SerCliAutoPath, F2NewSignalsKeepFrameLayout)
{
    for (PS ps : {PS::WithdrawChannelOccupied, PS::WithdrawEstablishFailed, PS::WithdrawRendezvousTimeout,
                  PS::WithdrawRuntimeDisconnect})
    {
        IpcPubSubIdInitMsg m;
        m.host_flag = true;
        m.run_status = true;
        m.path_state = ps;
        const ipc::buffer raw = m.serialize();
        /* 载荷仍是 4 字节 ⇒ 老端 has_path_state 的判据、12 字节页尾的位置、前 3 字节的
         * 语义全部不变 —— "新旧帧双向兼容"这条契约不因为多出几个枚举值而改变。 */
        EXPECT_EQ(IpcPubSubIdInitMsg::pure_payload_size(raw), 4u);
        EXPECT_TRUE(IpcPubSubIdInitMsg::has_path_state(raw));
        EXPECT_EQ(IpcPubSubIdInitMsg::peek_path_state(raw), ps);

        IpcPubSubIdInitMsg decoded;
        decoded.deserialize(raw);
        EXPECT_TRUE(decoded.deserialize_ok());
        EXPECT_EQ(decoded.path_state, ps);
        EXPECT_TRUE(decoded.host_flag);
        EXPECT_TRUE(decoded.run_status);

        /* 监视侧(topic_cat 的只读探针)必须认得新值: 落空会显示成 path_state(?) ——
         * 那时运维在屏幕上看到的只是一个数字, 监控标签就白加了。 */
        EXPECT_STRNE("path_state(?)", dzIPC::path_state_name(static_cast<uint8_t>(ps)))
            << "topic_cat 的 path_state_name 没跟上新枚举值, 显示会退化成数字";
    }
    EXPECT_STREQ("Withdraw(ChannelOccupied)", dzIPC::path_state_name(sig_of(PS::WithdrawChannelOccupied)));
    EXPECT_STREQ("Withdraw(RendezvousTimeout)", dzIPC::path_state_name(sig_of(PS::WithdrawRendezvousTimeout)));
}

/* ------------------------------------------------------------------------- *
 * F2-d: 失败注入(占用) —— 服务端必须在**握手通道上**公告"通道被占用", 客户端据此记账。
 *
 * 为什么判据必须落在 wire 上: 只看 status() 分不清标签是"从对端解码来的"还是"本端猜的"
 * —— F2 之前客户端同样报 ChannelOccupied, 只是它是猜的(对端发的其实是 4)。所以承重的是
 * "服务端到底发了哪个值": 发 4 ⇒ 这条用例红。
 * ------------------------------------------------------------------------- */
TEST(SerCliAutoPath, F2WireAnnouncesOccupancyRefusal)
{
#if !defined(_WIN32)
    const std::string topic = topic_for("f2occupied");
    const size_t domain = 3;
    const std::string ctrl_name = dzIPC::shm::ser_service_control_name(topic, domain);

    /* ⛔ 断言任何东西之前先回收池里的**死条目**。
     *
     * 池是**宿主级**共享段(主机本地 SHM), 而死进程留下的条目没有任何产品代码会回收:
     * 全仓 `gc_dead()` 的调用点只有测试自己, `register_entry()` 在表满时**静默返回 -1**
     * (不抛、不日志), 而判定路径用的是 `snapshot(false)`(从不回收)、`register_entry()`
     * 也只找空槽、不判死。于是一台跑过很多轮的机器上, 池会被 SIGKILL 掉的子进程条目
     * 慢慢占满, 之后**新进程再也登记不进池** ⇒ 同机判定永久退化成 NoEvidence。
     *
     * 那种失效的**表现**与本用例要抓的东西长得一样: 服务端 attempts=0、wire 上没有任何
     * 公告 ⇒ 看上去像"占用注入没触发"。本用例断言的是**代码**, 不是宿主之前攒下的垃圾,
     * 所以先把死条目收掉, 并在下面把"池能不能用"写成显式前提。 */
    const std::size_t reaped = dzIPC::info_pool::IpcInfoPool::instance().gc_dead();

    pid_t child = ::fork();
    ASSERT_GE(child, 0);
    if (child == 0)
    {
        /* 子进程做两件事, 两件都必须是**别的进程**提供的:
         *   ① 把控制面段建起来并置 Ready: 这就是"通道被占用"的注入本体 —— 池启发式
         *      看不见它(没登记 SHM 条目), 守卫必须靠第二证据源(派生段名探测)判出占用;
         *   ② 登记一条 **SocketClient** 条目: 服务端的同机判定读的是池里的**对端条目**,
         *      没有它服务端连"要不要切"都不会开始判(直接落到 NoEvidence)。
         *      让**子进程**提供这条证据, 是为了让本用例的占用注入**不依赖真实客户端的
         *      登记时序** —— 否则"客户端登记失败/晚到"会伪装成"服务端没判出占用",
         *      而这两种情形的修法完全不同(前者是池/时序, 后者才是守卫)。
         *      注意子进程的 pid ≠ 服务端 pid, 所以不会被占用判据的 self_pid 过滤掉。 */
        dzIPC::control_plane_shm::TopicControlPlane cp;
        if (!cp.open(ctrl_name))
        {
            ::_exit(2);
        }
        cp.begin_rebuild();
        cp.set_ready();
        (void)dzIPC::info_pool::IpcInfoPool::instance().register_entry(
            {dzIPC::info_pool::EntryKind::SocketClient, topic, "test", "socket", static_cast<int32_t>(domain),
             "f2-occupancy-evidence"});
        while (true)
        {
            cp.heartbeat();
            std::this_thread::sleep_for(10ms);
        }
        ::_exit(0);
    }
    /* ⛔ 占用者必须**无条件**收尸: 下面的前提断言会提前 ASSERT 返回, 而手写在用例末尾的
     * kill/waitpid 是不会被执行的 —— 留下一个永远活着的占用者会污染同进程后续用例。
     * pool_guard 声明在**前面** ⇒ 它在收尸**之后**才析构, 顺手把子进程那条夹具条目
     * 从池里收掉(否则本用例每跑一轮就永久占掉一格 —— 正是第 2 节那个缺口的形态)。 */
    pool_dead_reaper pool_guard;
    child_reaper child_guard;
    child_guard.pid = child;
    std::this_thread::sleep_for(400ms);

    auto peer_visible_for = [&](int32_t pid) {
        for (const auto& e : dzIPC::info_pool::IpcInfoPool::instance().snapshot(false))
        {
            if (e.in_use && e.alive && e.kind == dzIPC::info_pool::EntryKind::SocketClient && e.pid == pid
                && e.topic_name == topic && e.domain_id == static_cast<int32_t>(domain))
            {
                return true;
            }
        }
        return false;
    };

    /* 前提 ①: 子进程那条**确定性**同机证据必须真的在池里。它不在, 服务端的判定必然
     * 落到 NoEvidence, 下面所有断言都失去意义(且那不是代码缺陷)。 */
    ASSERT_TRUE(wait_for([&] { return peer_visible_for(static_cast<int32_t>(child)); }, 2000))
        << "子进程没能登记 SocketClient 证据条目(已先回收 " << reaped
        << " 条死条目) ⇒ 服务端的同机判定必落 NoEvidence, 占用注入根本走不到。"
           "先查池是否已满: `./build/app/dzipc_list`";

    std::atomic<dzIPC::autopath::auto_ser_ipc*> srv{nullptr};
    /* 服务端线程同样绑在作用域上: 上面任何一条 ASSERT_* 提前 return 时, ~std::thread
     * 对 joinable 线程会直接 std::terminate(整个二进制当场死掉, 后续用例全部不跑)。 */
    thread_joiner ser;
    ser.th = std::thread(
        [&] {
            dzIPC::autopath::auto_ser_ipc s(topic, make_sd(), echo_plus_one, domain, dzIPC::autopath::Options{}, false);
            /* 发布指针后才 InitChannel: 构造已完成 ⇒ 主线程读 status_ 是安全的(全 atomic)。 */
            srv.store(&s, std::memory_order_release);
            s.InitChannel();
            while (!ser.stop.load()) std::this_thread::sleep_for(5ms);
            srv.store(nullptr, std::memory_order_release);
        });
    std::this_thread::sleep_for(200ms);

    /* 只读观测 base+2 握手通道上真正发出去的字节。 */
    dzIPC::handshake_probe probe;
    ASSERT_TRUE(probe.open(topic, domain)) << "打不开握手通道的只读观测";

    dzIPC::autopath::auto_cli_ipc c(topic, make_sd(), domain, dzIPC::autopath::Options{}, false);
    c.InitChannel();
    ASSERT_TRUE(wait_for([&] { return c.handshake_completed(); }, 5000));

    /* 前提 ②: 观测本身在工作。"探针一帧没收到"与"服务端没公告"是两件事, 必须先分开。 */
    ASSERT_TRUE(wait_for(
        [&] {
            probe.poll();
            return probe.snapshot().frames > 0;
        },
        3000))
        << "只读探针在 base+2 上一帧都没收到 ⇒ 观测失效(端口/入组/绑定), 后面的 wire 断言无从谈起";

    /* 前提 ③: 真实客户端自己的登记也要成功 —— 端到端那一段(客户端要能拿到服务端的
     * SocketServer 条目、服务端要能拿到客户端的条目)靠的是它, 不是子进程那条夹具。 */
    ASSERT_TRUE(wait_for([&] { return peer_visible_for(static_cast<int32_t>(::getpid())); }, 2000))
        << "真实客户端的池登记失败(池满? 锁失效?) ⇒ 本用例的端到端部分无从判定; 已先回收 " << reaped
        << " 条死条目";

    const uint8_t kOccupied = sig_of(PS::WithdrawChannelOccupied);
    const bool on_wire
        = wait_for([&] { probe.poll(); return probe.snapshot().server.path_state == kOccupied; }, 3000);
    const dzIPC::handshake_snapshot snap = probe.snapshot();
    auto* s = srv.load(std::memory_order_acquire);
    ASSERT_NE(s, nullptr);
    EXPECT_TRUE(snap.server.seen) << "没观测到服务端的握手帧";
    EXPECT_TRUE(on_wire) << "服务端在 wire 上公告的不是'通道被占用'(5), 而是 "
                         << static_cast<int>(snap.server.path_state) << " "
                         << dzIPC::path_state_name(snap.server.path_state)
                         << " —— 发 4 就等于把原因丢了, 对端只能猜"
                         << "; 服务端侧: decision=" << dzIPC::path::to_string(s->status().decision())
                         << " fallback=" << dzIPC::path::to_string(s->status().fallback())
                         << " evidence_kind=" << s->status().evidence.kind.load()
                         << " (0 = 池里没看到对端 ⇒ 判定停在 NoEvidence)"
                         << "; 客户端侧: decision=" << static_cast<int>(c.status().decision()) << " ("
                         << dzIPC::path::to_string(c.status().decision()) << ") attempts="
                         << c.status().switch_attempts.load()
                         << " (decision=1 且计数 0 = 客户端还在等对端裁定, 即服务端根本没公告)";
    EXPECT_NE(snap.server.path_state, sig_of(PS::WithdrawToSocket))
        << "退化回了统一的 legacy 4(即 F2 之前的行为)";

    /* ⛔ 从这里往下的四条断言**必须**等客户端的记账, 不能即时读 —— 这是本用例
     * 之前那一版红的**唯一原因**(tester 13:59 的 D1 定位, 指纹与实测逐字吻合)。
     *
     * `on_wire` 证明的是"**探针这个 socket** 收到了 5"; 而下面读的是**客户端自己的
     * 记账**, 那是另一条 socket、另一个线程, 中间隔着两步:
     *   ① 5 先到客户端**握手线程**, 它把 peer_path_signal_ 置成 5;
     *   ② 客户端 **supervise** 线程在下一轮轮询(kPollMs=20 ms)里读到它, 才
     *      `peer_rejected=true` ⇒ 落 decision/fallback + 两个计数(:802-806)。
     * ⇒ 探针看到 5 与客户端落账之间有**最大 20 ms** 的窗口; 在窗口内读到的正好是
     *   "什么都还没记"的样子: decision=1(PeerInPool, :698 已置) / fallback=0(None)
     *   / attempts=0 / fallbacks=0 —— 那不是判定错, 是**读早了**。旧用例
     *   (OccupiedShmChannelIsNotClobbered / OccupancyDetectedWithoutPoolEntry)用
     *   `sleep_for(700/900ms) "留出判定窗口"` 绕过它; 本用例改成**事件驱动**:
     *   断言"客户端记账里出现了什么", 而不是"等了多久"。
     *
     * 判别(留给下一个人): 若这条 wait **超时**, 说明客户端**始终没**把 5 记成占用
     * —— 那才是真问题(消息里把两侧的实际值全打出来); 若它瞬间为真而下面的等值
     * 断言仍红, 才是标签/计数本身的实现问题。 */
    ASSERT_TRUE(wait_for([&] { return c.status().decision() == DR::ChannelOccupied; }, 2000))
        << "客户端始终没把对端公告的占用记成占用: decision="
        << static_cast<int>(c.status().decision()) << " (" << dzIPC::path::to_string(c.status().decision())
        << ") fallback=" << static_cast<int>(c.status().fallback()) << " ("
        << dzIPC::path::to_string(c.status().fallback())
        << ") attempts=" << c.status().switch_attempts.load()
        << " fallbacks=" << c.status().switch_fallbacks.load()
        << " —— decision=1(PeerInPool) 且两个计数皆 0 = 判定停在'等对端裁定'"
           "(客户端那条路的唯一出口就是等满 T_est, 它不落任何账)";
    /* 服务端那 4 条同样要等它自己的记账: 它是在**判定支内**先加计数再置信号的,
     * 但"信号已上 wire"与"记账可见"仍是两个线程的两条路径 —— 用同一个判据等。 */
    ASSERT_TRUE(wait_for(
        [&] {
            auto* p = srv.load(std::memory_order_acquire);
            return p != nullptr && p->status().switch_fallbacks.load() >= 1u;
        },
        2000))
        << "服务端自己没把这次占用拒绝计入回退(它的记账应发生在公告之前)";

    /* 两侧记账: 一次尝试、一次回退、零成功, 标签与 wire 上的原因一致。 */
    EXPECT_EQ(c.transport_current(), dzIPC::path::Kind::Socket);
    EXPECT_EQ(c.status().decision(), DR::ChannelOccupied);
    EXPECT_EQ(c.status().fallback(), FR::ShmChannelOccupied);

    EXPECT_EQ(s->status().decision(), DR::ChannelOccupied) << "服务端自己的 decision 标签不准";
    EXPECT_EQ(s->status().fallback(), FR::ShmChannelOccupied);
    for (const dzIPC::path::Status* st : {&c.status(), &s->status()})
    {
        EXPECT_EQ(st->switch_attempts.load(), 1u)
            << "这一次占用拒绝没被记成**一次**尝试(实际 " << st->switch_attempts.load()
            << "); 0 = 判定期没发生; >1 = 同一条连接里判了两次 —— 服务端的占用支不置 "
               "attempted_this_connection, socket 腿一抖就会重判(tester 13:59 的 D2)";
        EXPECT_EQ(st->switch_fallbacks.load(), 1u);
        EXPECT_EQ(st->switch_successes.load(), 0u);
        EXPECT_LE(st->switch_successes.load(), st->switch_attempts.load())
            << "successes 超过了 attempts(监控口径不可能成立)";
        EXPECT_EQ(st->switch_attempts.load(), st->switch_successes.load() + st->switch_fallbacks.load())
            << "判定期不变量被破坏: switch_attempts != switch_successes + switch_fallbacks";
    }
    EXPECT_TRUE(one_rpc(c)) << "拒绝切换之后必须仍然可用(socket), 而不是半死状态";

    /* 收尾全部交给作用域守卫(child_guard / ser), 此处不再手写 kill/join —— 见上面的说明。 */
    probe.close();
#endif
}

/* ------------------------------------------------------------------------- *
 * F2-e: 失败注入(会合失败) —— 这一支此前被对端记成"通道被占用"。
 *
 * 注入手法: 把**服务端**的 T_est 压到 0 ⇒ 它建完腿就立刻判定"对端没接上"并撤销。
 * ⛔ 为什么是 0 而不是 1 ms: 服务端那条有界等待是
 *     `while (now < deadline) { …; sleep(kPollMs); }`, 而 deadline 是在**建腿之后**
 *     才算的 ⇒ est=1 实际给出的是"建腿耗时 + 一个 20 ms 睡眠"的窗口(≈21~23 ms),
 *     比客户端的采样周期(kPollMs=20 ms)**还宽** ⇒ 客户端几乎必然读到那个瞬态的
 *     ProposeShm(1)、跟着把腿建起来并 attach 成功(服务端那 1 ms 内根本没等到它),
 *     随后服务端撤腿 ⇒ 客户端记成**运行期断链**(RemoteIoFailure, successes=1 且
 *     fallbacks=1)。est=0 让那条循环体一次都不执行(条件先判且 now>=deadline),
 *     1 在**信号变量**上的存活窗口缩到"建腿耗时"(≈1~3 ms)。
 *
 *     ⛔ 订正(原先这里写的"客户端第一个采样点读到 0、第二个读到 7"是错的): 客户端的
 *     采样周期并不是判据 —— 握手帧只在**信号变化时**才重序列化, 而发送循环的周期约
 *     100 ms(socket_ser_cli_ipc.cc 的 refresh_out_frame + 每轮 receive(100))。所以
 *     "1 会不会被发出去"取决于那一轮刷新是否正好落在 Δ 窗口里(概率 ≈ Δ/100 ms),
 *     一旦发出去, 客户端 20 ms 的轮询**几乎必然**读到它(该值会一直留在客户端的
 *     peer_path_signal_ 里直到下一帧 ≈100 ms 后才变成 7)。⇒ est=1 时 P(路②) ≈
 *     21~23 %(Δ≈21~23 ms, 与 tester 13:59 的 E1 一致), est=0 把它压到 ≈1~3 %,
 *     **但压不到 0** —— 这正是下面必须同时保留路①与路②两支断言的原因。
 *
 * 判据三层:
 *   - wire 层(**承重, 确定性**): 服务端必须公告"会合失败"(7); 旧代码发 4, 这条立刻红。
 *     est=0 下服务端**必然**走到"没等到对端"这一支(它连一次检查都不做就撤销)。
 *   - 服务端记账层(**确定性**): attempts=1 / fallbacks=1 / successes=0 /
 *     fallback=ShmRendezvousTimeout。此前服务端这一支不区分原因, 记的是"未区分"。
 *   - 客户端记账层: 必须**不是**"通道被占用"(F2 的落点)。这条路有两种可能, 见下。
 * ------------------------------------------------------------------------- */
TEST(SerCliAutoPath, F2RendezvousFailureIsNotReportedAsOccupied)
{
    dzIPC::autopath::Options srv_opts;
    srv_opts.est_timeout_ms = 0;
    /* 同 F2Wire: 池是宿主级共享段, 死条目没有任何产品代码会回收 ⇒ 先收一遍。
     * 不收的话"客户端登记失败"会让服务端直接落到 NoEvidence, 本用例连一次 SHM 判定
     * 都不会发生(那时 wire 上什么都不会出现, 与"注入没生效"长得一模一样)。 */
    const std::size_t reaped = dzIPC::info_pool::IpcInfoPool::instance().gc_dead();
    Rig rig("f2rendez", srv_opts, dzIPC::autopath::Options{});

    /* 服务端已在跑, 但它要等客户端出现才有池证据, 所以现在挂观测不会漏掉裁定帧。 */
    dzIPC::handshake_probe probe;
    ASSERT_TRUE(probe.open(rig.topic, 3));

    rig.start_client();
    ASSERT_TRUE(rig.await_handshake());

    const uint8_t kRendezvous = sig_of(PS::WithdrawRendezvousTimeout);
    const bool on_wire
        = wait_for([&] { probe.poll(); return probe.snapshot().server.path_state == kRendezvous; }, 3000);
    const dzIPC::handshake_snapshot snap = probe.snapshot();
    auto* s = rig.srv.load(std::memory_order_acquire);
    ASSERT_NE(s, nullptr) << "服务端对象指针未发布(它应在本用例全程存活)";
    EXPECT_TRUE(on_wire) << "服务端没在 wire 上公告'会合失败', 实际 "
                         << static_cast<int>(snap.server.path_state) << " "
                         << dzIPC::path_state_name(snap.server.path_state)
                         << "; 服务端侧 evidence_kind=" << s->status().evidence.kind.load()
                         << " (0 = 池里没看到对端 ⇒ 判定停在 NoEvidence, 本次判定根本没发生; 已先回收 " << reaped
                         << " 条死条目)";
    EXPECT_NE(snap.server.path_state, sig_of(PS::WithdrawToSocket))
        << "退化回了统一的 legacy 4(即 F2 之前的行为)";

    /* ---- 服务端记账(确定性: est=0 下它必然走到"没等到对端") ---- */
    EXPECT_NE(s->status().evidence.kind.load(), 0) << "服务端没拿到同机对端证据 ⇒ 判定停在 NoEvidence";
    EXPECT_EQ(s->status().fallback(), FR::ShmRendezvousTimeout)
        << "服务端自己记的回退原因不是'会合超时', 而是 " << dzIPC::path::to_string(s->status().fallback())
        << " (若为 ShmEstablishFailed, 说明建腿本身抛了异常 —— 那是另一条路径, 也需要单独定性)";
    EXPECT_EQ(s->status().switch_attempts.load(), 1u);
    EXPECT_EQ(s->status().switch_fallbacks.load(), 1u);
    EXPECT_EQ(s->status().switch_successes.load(), 0u);
    EXPECT_EQ(s->status().switch_attempts.load(),
              s->status().switch_successes.load() + s->status().switch_fallbacks.load())
        << "服务端判定期不变量被破坏: switch_attempts != switch_successes + switch_fallbacks";

    /* ---- 客户端记账 ----
     * 客户端有两种**都不是"通道被占用"**的收尾, 断言对两者都成立:
     *   ① 读到 7(est=0 下的主路): peer_rejected ⇒ 判定期内记一次"会合失败";
     *   ② 万一采样点正好落在那个 1~3 ms 的窗口里: 它跟着建腿, 且若抢在服务端 set_stopping
     *      之前 attach 成功, 那次"成功"是**真的**建立过 ⇒ 随后腿被撤走是**运行期断链**,
     *      记成 RemoteIoFailure。①与②的分界只是采样相位, 不是代码分支对错。
     * ⛔ 判定期不变量(attempts == successes + fallbacks)只在**一次 SHM 判定窗口**内成立,
     *    运行期断链会给 fallbacks 再加一次(leader 已裁定) ⇒ ②下只要求 successes<=attempts。
     *    "7 该被解成 Timeout/ShmRendezvousTimeout"这条映射不靠本用例兜底 ——
     *    它由 F2PeerWithdrawDecodeIsFaithful(纯解码)钉死。 */
    ASSERT_TRUE(wait_for(
        [&] {
            return rig.cli->status().switch_fallbacks.load() >= 1u
                   || rig.cli->status().switch_successes.load() >= 1u;
        },
        4000))
        << "客户端既没切上 SHM 也没记回退, 用例无从判定";
    const uint64_t att = rig.cli->status().switch_attempts.load();
    const uint64_t succ = rig.cli->status().switch_successes.load();
    const uint64_t fbs = rig.cli->status().switch_fallbacks.load();

    EXPECT_NE(rig.cli->status().fallback(), FR::ShmChannelOccupied)
        << "会合失败被记成了'通道被占用' —— 这正是 F2 要修的错位";
    EXPECT_NE(rig.cli->status().decision(), DR::ChannelOccupied) << "decision 也被记成了 ChannelOccupied";
    EXPECT_EQ(rig.cli->transport_current(), dzIPC::path::Kind::Socket);
    EXPECT_LE(succ, att) << "successes 超过 attempts(监控口径不可能成立)";
    EXPECT_GE(fbs, 1u);

    if (succ == 0u)
    {
        /* ① 判定期失败: 强断言。 */
        const FR fb = rig.cli->status().fallback();
        EXPECT_EQ(att, 1u);
        EXPECT_EQ(fbs, 1u);
        EXPECT_TRUE(fb == FR::ShmRendezvousTimeout || fb == FR::ShmEstablishFailed)
            << "回退原因既不是会合失败也不是建腿失败: " << dzIPC::path::to_string(fb);
        const DR dr = rig.cli->status().decision();
        EXPECT_TRUE(dr == DR::Timeout || dr == DR::ShmNotReady)
            << "判定原因不是'超时/对端未就绪': " << dzIPC::path::to_string(dr);
        EXPECT_EQ(att, succ + fbs) << "判定期不变量被破坏: switch_attempts != switch_successes + switch_fallbacks";
    }
    else
    {
        /* ② 竞态分支: 客户端抢在服务端撤腿前 attach 成功, 随后运行期断链。 */
        EXPECT_EQ(succ, 1u) << "一次连接内不该出现多次 SHM 建立(铁律 1)";
        EXPECT_EQ(rig.cli->status().fallback(), FR::RemoteIoFailure)
            << "attach 成功后腿又没了 ⇒ 该记运行期断链, 实际 " << dzIPC::path::to_string(rig.cli->status().fallback());
        std::cerr << "\033[33m[f2rendez] 命中竞态分支②: 客户端采到瞬态 ProposeShm 并 attach 成功, "
                     "随后服务端撤腿 ⇒ 记成运行期断链(successes=1/fallbacks="
                  << fbs << ")。判定期不变量在此不适用。\033[0m" << std::endl;
    }
    EXPECT_TRUE(one_rpc(*rig.cli)) << "回退之后必须仍然可用(socket)";
    probe.close();
}

/* ------------------------------------------------------------------------- *
 * A11/M3: 别名碰撞 —— '_foo' 与 '/foo' **派生到同一个 SHM 段**。
 *
 * 这不是假想的边界: F1 把段名里的内层 '/' 清成 '_'(POSIX 只接受 /somename, 内层 '/'
 * 让 shm_open 恒 EINVAL(22)), 于是"下划线名"与"斜杠名"从**两个不同的 topic** 变成了
 * **同一个段**的两个别名。诚实标注: 碰撞是 F1 **引入**的, 但它引入的是"从不可用 ->
 * 与别人撞名"(斜杠那条以前根本建不出段), 不是把两条本来能用的通道并成一条。
 *
 * 要钉死的两件事:
 *   ① 后起的 '/foo' **不得摧毁**先起的 '_foo' 的既有连接。摧毁的直接签名是控制面段被
 *      clear_storage/begin_rebuild 重建: owner_pid 被改写、generation 被推进、对端计数
 *      归零; 行为面则是客户端被顶下 SHM、RPC 开始失败。
 *   ② 后果必须**可判定**: 服务端记 ChannelOccupied, 客户端照实记账(而不是超时),
 *      并且两条连接各自继续可用(SHM / socket 各走各的)。
 *
 * ---- 变异条款(可执行) ----
 * 池证据对别名是**盲**的(它按原始 topic 名匹配, 而两个名字不同), 所以挡住这条碰撞的
 * **唯一**机制就是第二条证据源: 按**派生段名**做的控制面探测。把那一半去掉,
 * shm_channel_occupied() 就退化成 pool_only_says_occupied() 的返回值 ⇒ 下面那条
 * ASSERT_TRUE 立刻红。更进一步, '/foo' 会真的切进去并 clear_storage 掉 '_foo' 的段,
 * 于是 c1 掉下 SHM、generation 被推进 —— 本用例有多重承重点, 不是一条断言撑着的。
 * ------------------------------------------------------------------------- */
TEST(SerCliAutoPath, A11AliasCollisionDoesNotDestroyExistingConnection)
{
#if !defined(_WIN32)
    const size_t domain = 3;
    static std::atomic<int> m3_n{0};
    const std::string base = "t3_m3alias_" + std::to_string(m3_n.fetch_add(1));
    const std::string topic_underscore = base + "_x";   // 先起: 下划线名
    const std::string topic_slash = base + "/x";        // 后起: 斜杠名(只差这一个字符)

    /* ---- 前提 1: 两个 topic 名不同, 派生段名却完全相同 ---- */
    ASSERT_NE(topic_underscore, topic_slash);
    ASSERT_EQ(shm_service_prefix(topic_underscore, domain), shm_service_prefix(topic_slash, domain))
        << "前提不成立: 两个 topic 没有派生到同一个段, 本用例检验不到别名碰撞";
    ASSERT_EQ(dzIPC::shm::ser_service_control_name(topic_underscore, domain),
              dzIPC::shm::ser_service_control_name(topic_slash, domain))
        << "控制面段名(占用判据用的就是它)没有碰撞";
    /* 老规则下两者名字不同 —— 碰撞确实是 F1 带来的(而 F1 前那条恒 EINVAL, 从未可用)。 */
    EXPECT_NE(shm_service_legacy_prefix(topic_underscore, domain), shm_service_legacy_prefix(topic_slash, domain))
        << "legacy 规则下两者居然同名 ⇒ 碰撞不是 F1 引入的, 本用例的定性错了";

    /* ---- 前提 2: 握手通道不撞端口(撞了会互相串台, 断言就不可复现了) ---- */
    ASSERT_NE(dzIPC::common::udp_discovery_port_calculate(topic_underscore, 3),
              dzIPC::common::udp_discovery_port_calculate(topic_slash, 3))
        << "这两个 topic 名哈希到同一个 UDP 端口, 会互相串台; 换一个 base 名再跑";

    const std::string ctrl_name = dzIPC::shm::ser_service_control_name(topic_underscore, domain);

    /* ---- 第一步: 子进程起 '_foo' 的服务端; 客户端留在**父进程** ---- */
    child_reaper child;
    child.pid = ::fork();
    ASSERT_GE(child.pid, 0);
    if (child.pid == 0)
    {
        dzIPC::autopath::auto_ser_ipc s(topic_underscore, make_sd(), echo_plus_one, domain, dzIPC::autopath::Options{},
                                        false);
        s.InitChannel();
        while (true)
        {
            std::this_thread::sleep_for(50ms);
        }
        ::_exit(0);
    }

    /* 客户端由父进程持有 —— 只有拿得住它, 才能直接断言"既有连接还在 SHM 上"。 */
    dzIPC::autopath::auto_cli_ipc c1(topic_underscore, make_sd(), domain, dzIPC::autopath::Options{}, false);
    c1.InitChannel();
    ASSERT_TRUE(wait_for([&] { return c1.handshake_completed(); }, 5000)) << "'_foo' 的 UDP 引导握手未完成";
    ASSERT_TRUE(wait_for([&] { return c1.transport_current() == dzIPC::path::Kind::Shm; }, 6000))
        << "'_foo' 未切到 SHM ⇒ 没有真实连接可被摧毁, 前提不成立; decision="
        << dzIPC::path::to_string(c1.status().decision());
    ASSERT_TRUE(one_rpc(c1)) << "'_foo' 的既有连接一开始就不能用";

    const ctrl_head before = read_ctrl_head(ctrl_name);
    ASSERT_TRUE(before.exists) << "既有连接已建立, 控制面段却不存在: " << ctrl_name;
    ASSERT_EQ(before.magic, dzIPC::control_plane_shm::kTopicControlMagic);
    const int32_t child_pid = static_cast<int32_t>(child.pid);
    ASSERT_EQ(before.owner_pid, child_pid) << "控制面 owner 不是子进程, 前提不成立";
    ASSERT_GT(before.generation, 0u) << "generation 还是 0 ⇒ 段没被真实建起来";
    ASSERT_GE(before.peer_count, 1u) << "既有连接没有登记到控制面, 前提不成立";

    /* ---- 第二步: 起 '/foo' —— 它的派生段与 '_foo' 是**同一个** ---- */
    std::atomic<dzIPC::autopath::auto_ser_ipc*> srv2{nullptr};
    thread_joiner ser2;
    ser2.th = std::thread([&] {
        dzIPC::autopath::auto_ser_ipc s(topic_slash, make_sd(), echo_plus_one, domain, dzIPC::autopath::Options{},
                                        false);
        srv2.store(&s, std::memory_order_release);
        s.InitChannel();
        while (!ser2.stop.load())
        {
            std::this_thread::sleep_for(5ms);
        }
    });
    std::this_thread::sleep_for(250ms);

    /* 变异条款: 池对别名是盲的 —— 这一条同时证明了**下一条 ASSERT 是承重的**:
     * 没有派生段探测, 函数返回的正是这里这个 false。 */
    EXPECT_FALSE(pool_only_says_occupied(topic_slash, domain, static_cast<int32_t>(::getpid())))
        << "池判据居然看见了这条别名占用 ⇒ 本用例不再能区分'池判据'与'派生段判据'";
    ASSERT_TRUE(dzIPC::autopath::shm_channel_occupied(topic_slash, domain, static_cast<int32_t>(::getpid())))
        << "别名碰撞未被拦下: '/foo' 会 clear_storage 掉 '_foo' 正在用的段";

    dzIPC::autopath::auto_cli_ipc c2(topic_slash, make_sd(), domain, dzIPC::autopath::Options{}, false);
    c2.InitChannel();
    ASSERT_TRUE(wait_for([&] { return c2.handshake_completed(); }, 5000));
    ASSERT_TRUE(wait_for([&] { return c2.status().switch_fallbacks.load() >= 1u; }, 6000))
        << "'/foo' 既没切上 SHM 也没记回退, 用例无从判定";

    /* ---- 判据 ②: 拒绝是**可判定**的, 且两侧标签与计数一致 ---- */
    EXPECT_EQ(c2.transport_current(), dzIPC::path::Kind::Socket);
    EXPECT_EQ(c2.status().fallback(), FR::ShmChannelOccupied);
    EXPECT_EQ(c2.status().decision(), DR::ChannelOccupied);
    EXPECT_EQ(c2.status().switch_successes.load(), 0u);
    EXPECT_EQ(c2.status().switch_attempts.load(), 1u);
    EXPECT_EQ(c2.status().switch_fallbacks.load(), 1u);
    EXPECT_EQ(c2.status().switch_attempts.load(),
              c2.status().switch_successes.load() + c2.status().switch_fallbacks.load())
        << "判定期不变量被破坏: switch_attempts != switch_successes + switch_fallbacks";

    auto* s2p = srv2.load(std::memory_order_acquire);
    ASSERT_NE(s2p, nullptr);
    EXPECT_EQ(s2p->status().decision(), DR::ChannelOccupied);
    EXPECT_EQ(s2p->status().fallback(), FR::ShmChannelOccupied);
    EXPECT_EQ(s2p->status().switch_attempts.load(), 1u);
    EXPECT_EQ(s2p->status().switch_fallbacks.load(), 1u);

    /* ---- 判据 ①(核心): 原连接**没有被摧毁** ---- */
    EXPECT_EQ(c1.transport_current(), dzIPC::path::Kind::Shm)
        << "别名碰撞把 '_foo' 的既有 SHM 连接顶下去了 —— 这正是本用例要防的";
    EXPECT_EQ(c1.status().switch_successes.load(), 1u) << "原连接被重新判定过(不该发生)";
    EXPECT_EQ(c1.status().switch_fallbacks.load(), 0u) << "原连接记到了回退 ⇒ 它的 SHM 腿被拆了";
    EXPECT_TRUE(one_rpc(c1)) << "'_foo' 的既有连接在 '/foo' 起来之后第一次往返失败";
    EXPECT_TRUE(one_rpc(c1)) << "'_foo' 的既有连接第二次往返失败";
    EXPECT_TRUE(one_rpc(c2)) << "'/foo' 退回 socket 之后必须仍然可用, 而不是半死状态";

    /* 控制面: 段还是**同一个段**, owner/代次/引用计数都没被动过。
     * 其中 generation 是最干脆的签名 —— begin_rebuild() 唯一会推进它的地方, 而它正是
     * clear_storage 之后重建通道的第一步。 */
    const ctrl_head after = read_ctrl_head(ctrl_name);
    ASSERT_TRUE(after.exists) << "控制面段消失了(被 unlink) —— 原连接的通道被摧毁";
    EXPECT_EQ(after.magic, dzIPC::control_plane_shm::kTopicControlMagic);
    EXPECT_EQ(after.owner_pid, child_pid) << "控制面 owner 被改写成 " << after.owner_pid
                                         << " ⇒ 段被 begin_rebuild 重建过";
    EXPECT_EQ(after.generation, before.generation)
        << "generation 从 " << before.generation << " 推进到 " << after.generation
        << " ⇒ 段被重建过(摧毁既有连接的直接签名)";
    EXPECT_EQ(after.state, static_cast<uint32_t>(dzIPC::control_plane_shm::TopicState::Ready))
        << "控制面状态不再是 Ready(实际 " << after.state << ")";
    EXPECT_EQ(after.peer_count, before.peer_count)
        << "对端计数(引用计数)从 " << before.peer_count << " 变成 " << after.peer_count
        << " ⇒ 既有连接被摧毁或掉线";
    EXPECT_GE(after.shm_ref, 1) << "libipc 层段引用计数归零 ⇒ 段被摧毁重建过";
#endif
}

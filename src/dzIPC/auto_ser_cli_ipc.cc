#include "dzIPC/auto_ser_cli_ipc.h"
#include <unistd.h>
#include <algorithm>
#include <utility>
#include "dzIPC/common/control_plane.h"
#include "dzIPC/ipc_info_pool.h"
#include "dzIPC/shm_ser_cli_ipc.h"
#include "dzIPC/socket_ser_cli_ipc.h"

namespace dzIPC {
namespace autopath {
namespace {

/* 判定轮询间隔。60 ms 是"比一次握手(实测 ~1.1 ms)慢两个数量级、又远小于 T_nego"
 * 的取法: 池是主机本地 SHM, 轮询开销可忽略; 间隔取太小只会无谓地抬高判定期的
 * CPU, 对缩短 T_nego 没有帮助。 */
constexpr uint64_t kPollMs = 20;
constexpr uint64_t kIdlePollMs = 50;   // Active 期的存活巡检(比判定期更省)

int64_t now_ns()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

/* 在池里找对端条目。取**最近注册**的那条: 同一 topic/domain 可能同时存在多个
 * 对端(例如两个客户端), 判定只需要"至少有一个同机对端", 但留痕要稳定 ——
 * 取最新的比取第一条哈希序更可复现。 */
bool find_peer(const std::string& topic_name, size_t domain_id, info_pool::EntryKind peer_kind,
               path::Evidence& ev)
{
    std::vector<info_pool::EntrySnapshot> all;
    try
    {
        /* gc_dead=false: 判定路径不应该顺带改动池(回收是池自己的职责), 否则
         * "读一次证据"会变成有副作用的操作, 无法在并发场景下推理。 */
        all = info_pool::IpcInfoPool::instance().snapshot(false);
    }
    catch (...)
    {
        return false;
    }
    const info_pool::EntrySnapshot* best = nullptr;
    for (const auto& e : all)
    {
        if (!e.in_use || !e.alive || e.kind != peer_kind)
        {
            continue;
        }
        if (e.topic_name != topic_name || e.domain_id != static_cast<int32_t>(domain_id))
        {
            continue;
        }
        if (best == nullptr || e.register_ts_ns > best->register_ts_ns)
        {
            best = &e;
        }
    }
    if (best == nullptr)
    {
        return false;
    }
    ev.kind.store(static_cast<int32_t>(best->kind), std::memory_order_release);
    ev.peer_pid.store(best->pid, std::memory_order_release);
    ev.ts_ns.store(best->register_ts_ns, std::memory_order_release);
    return true;
}

}   // namespace

bool look_for_peer(const std::string& topic_name, size_t domain_id, info_pool::EntryKind peer_kind,
                   path::Evidence& out)
{
    return find_peer(topic_name, domain_id, peer_kind, out);
}

bool shm_channel_occupied(const std::string& topic_name, size_t domain_id, int32_t self_pid)
{
    std::vector<info_pool::EntrySnapshot> all;
    try
    {
        all = info_pool::IpcInfoPool::instance().snapshot(false);
    }
    catch (...)
    {
        /* 查不出来就按"已占用"处理 —— 这一步是**守卫**(T2 §7 R3: 误判为未占用
         * 会让 clear_storage 摧毁既有连接), 所以不确定时选保守方向。 */
        return true;
    }
    for (const auto& e : all)
    {
        if (!e.in_use || !e.alive)
        {
            continue;
        }
        if (e.kind != info_pool::EntryKind::ShmServer && e.kind != info_pool::EntryKind::ShmClient)
        {
            continue;
        }
        if (e.topic_name != topic_name || e.domain_id != static_cast<int32_t>(domain_id))
        {
            continue;
        }
        if (e.pid == self_pid)
        {
            continue;   // 本进程自己的(上一次没清干净)不算占用
        }
        return true;
    }

    /* ---- 池没给出证据时, 补一条**不依赖登记**的第二证据源(T3 补充) ----
     *
     * 池是登记式的: 进程必须主动 rebind 才出现在池里, 没登记(或登记被 GC 掉)的
     * 既有服务端池里看不见 —— 那就是漏判, 代价是随后的 clear_storage 摧毁别人的
     * 活动连接。控制面段补上这一点: 段是**建出来**的, 谁真把这条 SHM 服务通道建
     * 起来段就在, 上面的 owned_by_other 会把"段在 + 有别的活 owner"判成占用。
     * 方向仍是保守: 段在但读不懂 ⇒ 占用; 段不存在 ⇒ 才放行(见该函数注释)。
     *
     * 仍未覆盖的边界(诚实标注, 非本层可解): 完全绕开 dzIPC 控制面、直接用 libipc
     * 在同样的段名上建通道的外部进程, 没有任何信号可供判定。 */
    try
    {
        return control_plane_shm::occupied_by_other(shm::ser_service_control_name(topic_name, domain_id), self_pid);
    }
    catch (...)
    {
        return true;   // 探测本身失败同样走保守方向
    }
}

/******************************************************************************************************/
/* 服务端                                                                                              */
/******************************************************************************************************/

auto_ser_ipc::auto_ser_ipc(const std::string& topic_name, const std::shared_ptr<ServiceData>& msg,
                           std::function<void(std::shared_ptr<ServiceData>&)> callback, size_t domain_id,
                           const Options& opts, bool verbose, bool enable_thread_qos, int cpu_id, int thread_priority)
    : ser_ipc_base(topic_name, msg, callback, domain_id, verbose)
    , topic_name_(topic_name)
    , domain_id_(domain_id)
    , verbose_(verbose)
    , opts_(opts)
    , thread_options_(dzIPC::ThreadDispatch::make_realtime_options(enable_thread_qos, cpu_id, thread_priority))
    , callback_(std::move(callback))
{
    message_.reset(msg->clone());
    socket_leg_ = std::make_unique<socket::socket_ser_ipc>(topic_name_, message_, callback_, domain_id_, verbose_,
                                                           enable_thread_qos, cpu_id, thread_priority);
}

auto_ser_ipc::~auto_ser_ipc()
{
    running_.store(false, std::memory_order_release);
    if (switch_thread_ != nullptr)
    {
        if (switch_thread_->joinable())
        {
            switch_thread_->join();
        }
        delete switch_thread_;
        switch_thread_ = nullptr;
    }
    teardown_all();
    exit_flag.store(true, std::memory_order_release);
}

void auto_ser_ipc::InitChannel(std::string extra_info)
{
    (void)extra_info;
    status_.set_state(path::State::Probe);
    status_.set_selected(path::Kind::Socket);
    socket_leg_->InitChannel(extra_info);
    switch_thread_ = new std::thread(&auto_ser_ipc::supervise, this);
    dzIPC::ThreadDispatch::apply_thread_options(switch_thread_, thread_options_, verbose_,
                                                topic_name_ + "_AutoSerSwitchThread");
}

void auto_ser_ipc::reset_message(const std::shared_ptr<ServiceData>& msg)
{
    if (!msg)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(leg_mtx_);
    message_.reset(msg->clone());
    if (socket_leg_)
    {
        socket_leg_->reset_message(message_);
    }
    if (shm_leg_)
    {
        shm_leg_->reset_message(message_);
    }
}

void auto_ser_ipc::reset_callback(std::function<void(std::shared_ptr<ServiceData>&)> callback)
{
    std::lock_guard<std::mutex> lock(leg_mtx_);
    callback_ = std::move(callback);
    if (socket_leg_)
    {
        socket_leg_->reset_callback(callback_);
    }
    if (shm_leg_)
    {
        shm_leg_->reset_callback(callback_);
    }
}

bool auto_ser_ipc::handshake_completed() const
{
    /* 服务端把"路径可用"定义为**当前 active 腿可用**。因为新腿确认之后才停旧腿
     * (见头注释), 这个值在切换全程保持 true —— 切换对调用方不可见。 */
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(leg_mtx_));
    if (use_shm_.load(std::memory_order_acquire) && shm_leg_)
    {
        /* SHM 期仍以 UDP 握手通道为存活权威(T2 §3 D6): 两边都必须活着。 */
        return socket_leg_ && socket_leg_->handshake_completed() && shm_leg_->handshake_completed();
    }
    return socket_leg_ && socket_leg_->handshake_completed();
}

path::Kind auto_ser_ipc::transport_current() const
{
    return use_shm_.load(std::memory_order_acquire) ? path::Kind::Shm : path::Kind::Socket;
}

void auto_ser_ipc::teardown_all()
{
    std::lock_guard<std::mutex> lock(leg_mtx_);
    if (shm_leg_)
    {
        shm_leg_.reset();   // 析构里 set_stopping() + 注销快速路径 + join
    }
    if (socket_leg_)
    {
        socket_leg_.reset();
    }
    use_shm_.store(false, std::memory_order_release);
    status_.set_state(path::State::Closed);
    status_.mark_cleanup_done(now_ns());
}

void auto_ser_ipc::withdraw_to_socket(path::FallbackReason reason)
{
    std::lock_guard<std::mutex> lock(leg_mtx_);
    if (shm_leg_)
    {
        /* 铁律 3(T2 §3 D7): 回退不得留下半死的 SHM 通道 —— 不释放控制面,
         * 下一个连接就会撞上"被占用"(R3 复发)。析构里已含 set_stopping()。 */
        shm_leg_.reset();
    }
    if (socket_leg_ && !socket_leg_->data_plane_running())
    {
        socket_leg_->restart_data_plane();
    }
    if (socket_leg_)
    {
        /* 告诉对端"退回 socket", 免得它单方面继续等 SHM 就绪。 */
        socket_leg_->set_path_signal(static_cast<uint8_t>(4 /* WithdrawToSocket */));
    }
    use_shm_.store(false, std::memory_order_release);
    status_.set_selected(path::Kind::Socket);
    status_.set_fallback(reason);
}

void auto_ser_ipc::supervise()
{
    bool attempted_this_connection = false;   // 铁律 1: 同一条连接内不二次尝试

    while (running_.load(std::memory_order_acquire))
    {
        /* ---- S1 Probe: 等 UDP 握手成立 ---- */
        status_.set_state(path::State::Probe);
        while (running_.load(std::memory_order_acquire) && !socket_leg_->handshake_completed())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
        }
        if (!running_.load(std::memory_order_acquire))
        {
            return;
        }
        status_.set_state(path::State::Negotiate);

        /* ---- S2 Negotiate → S3 Establish ---- */
        if (opts_.allow_shm && !attempted_this_connection)
        {
            path::Evidence ev;
            if (!opts_.force_no_evidence)
            {
                find_peer(topic_name_, domain_id_, kPeerKind, ev);
            }
            status_.evidence.kind.store(ev.kind.load(std::memory_order_acquire), std::memory_order_release);
            status_.evidence.peer_pid.store(ev.peer_pid.load(std::memory_order_acquire), std::memory_order_release);
            status_.evidence.ts_ns.store(ev.ts_ns.load(std::memory_order_acquire), std::memory_order_release);

            if (ev.kind.load(std::memory_order_acquire) == 0)
            {
                /* 无证据 ⇒ 按跨机处理, 保留 socket。**这不是错误, 是默认成功路径**
                 * (T2 §3 D7 第一行): 跨主机本来就该走 socket。 */
                status_.set_decision(path::DecisionReason::NoEvidence);
            }
            else if (shm_channel_occupied(topic_name_, domain_id_, static_cast<int32_t>(::getpid())))
            {
                status_.set_decision(path::DecisionReason::ChannelOccupied);
                status_.set_fallback(path::FallbackReason::ShmChannelOccupied);
                /* 公告(T2 §3 D7 铁律 2: 回退必须公告)。不公告的话对端只能靠等满
                 * T_est 才动, 而且它记下的原因是"对端没就绪"——与真实原因不符。 */
                socket_leg_->set_path_signal(4 /* WithdrawToSocket */);
            }
            else
            {
                attempted_this_connection = true;
                status_.set_decision(path::DecisionReason::PeerInPool);
                status_.switch_attempts.fetch_add(1, std::memory_order_acq_rel);
                status_.set_state(path::State::Establish);

                bool established = false;
                try
                {
                    socket_leg_->set_path_signal(1 /* ProposeShm */);
                    auto leg = std::make_unique<shm::shm_ser_ipc>(topic_name_, message_, callback_, domain_id_,
                                                                  verbose_);
                    leg->InitChannel();
                    {
                        std::lock_guard<std::mutex> lock(leg_mtx_);
                        shm_leg_ = std::move(leg);
                    }
                    /* 等**对端接上**(shm_ser_ipc::handshake_completed() == peer_count>0)。
                     * 这一步同时是"服务端可以停止 UDP 数据面"的判据: 客户端 attach 成功
                     * 意味着它已经在写 SHM 了, 此时停掉 UDP 不会丢请求。 */
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(opts_.est_timeout_ms);
                    while (running_.load(std::memory_order_acquire)
                           && std::chrono::steady_clock::now() < deadline)
                    {
                        if (shm_leg_ && shm_leg_->handshake_completed())
                        {
                            established = true;
                            break;
                        }
                        if (!socket_leg_->handshake_completed())
                        {
                            /* 握手中断: 对端在这一步之前/之中没了, 建立期就该收手
                             * (T3 验收项"握手中断"的失败面)。 */
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
                    }
                }
                catch (...)
                {
                    established = false;
                }

                if (established && running_.load(std::memory_order_acquire))
                {
                    std::lock_guard<std::mutex> lock(leg_mtx_);
                    /* 新腿已确认可用 -> **现在**才停旧腿 -> 翻 active。三者在一把锁里,
                     * 中间不存在"两条腿都可发"的时刻。 */
                    if (socket_leg_)
                    {
                        socket_leg_->set_path_signal(2 /* ConfirmShm */);
                        socket_leg_->stop_data_plane();
                    }
                    use_shm_.store(true, std::memory_order_release);
                    status_.set_selected(path::Kind::Shm);
                    status_.switch_successes.fetch_add(1, std::memory_order_acq_rel);
                }
                else
                {
                    const bool peer_gone = !socket_leg_->handshake_completed();
                    if (!peer_gone)
                    {
                        /* 建立失败也公告: 让对端立刻收手而不是干等 T_est。 */
                        socket_leg_->set_path_signal(4 /* WithdrawToSocket */);
                    }
                    withdraw_to_socket(peer_gone ? path::FallbackReason::WithdrawnByPeer
                                                 : path::FallbackReason::ShmRendezvousTimeout);
                    status_.switch_fallbacks.fetch_add(1, std::memory_order_acq_rel);
                }
            }
        }
        else if (!opts_.allow_shm)
        {
            status_.set_decision(path::DecisionReason::NoEvidence);
        }

        status_.set_state(path::State::Active);

        /* ---- S4 Active: 存活巡检。断连 -> 清理 -> 下次重连重判 ---- */
        while (running_.load(std::memory_order_acquire))
        {
            if (!socket_leg_->handshake_completed())
            {
                /* 对端断开了(UDP 握手通道是唯一存活权威)。清理**全部**相关状态,
                 * 并允许下一次连接重新判定 —— 判定结果不跨连接复用(T2 §3 D5 铁律 1)。 */
                status_.set_state(path::State::Withdraw);
                withdraw_to_socket(path::FallbackReason::WithdrawnByPeer);
                attempted_this_connection = false;
                status_.set_decision(path::DecisionReason::Pending);
                status_.evidence.kind.store(0, std::memory_order_release);
                status_.evidence.peer_pid.store(0, std::memory_order_release);
                status_.evidence.ts_ns.store(0, std::memory_order_release);
                break;
            }
            if (use_shm_.load(std::memory_order_acquire))
            {
                bool shm_ok;
                {
                    std::lock_guard<std::mutex> lock(leg_mtx_);
                    shm_ok = shm_leg_ && shm_leg_->handshake_completed();
                }
                if (!shm_ok)
                {
                    /* 运行期 SHM 侧断链: 回退到 socket。铁律 1 —— 同一条连接内
                     * 不再二次尝试 SHM, 要再试只能等下一次连接。 */
                    status_.set_state(path::State::Withdraw);
                    withdraw_to_socket(path::FallbackReason::RemoteIoFailure);
                    status_.switch_fallbacks.fetch_add(1, std::memory_order_acq_rel);
                    status_.set_state(path::State::Active);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kIdlePollMs));
        }
    }
}

/******************************************************************************************************/
/* 客户端                                                                                              */
/******************************************************************************************************/

auto_cli_ipc::auto_cli_ipc(const std::string& topic_name, const std::shared_ptr<ServiceData>& msg, size_t domain_id,
                           const Options& opts, bool verbose, bool enable_thread_qos, int cpu_id, int thread_priority)
    : cli_ipc_base(topic_name, msg, domain_id, verbose)
    , topic_name_(topic_name)
    , domain_id_(domain_id)
    , verbose_(verbose)
    , opts_(opts)
    , thread_options_(dzIPC::ThreadDispatch::make_realtime_options(enable_thread_qos, cpu_id, thread_priority))
{
    message_.reset(msg->clone());
    socket_leg_ = std::make_unique<socket::socket_cli_ipc>(topic_name_, message_, domain_id_, verbose_,
                                                           enable_thread_qos, cpu_id, thread_priority);
}

auto_cli_ipc::~auto_cli_ipc()
{
    running_.store(false, std::memory_order_release);
    if (switch_thread_ != nullptr)
    {
        if (switch_thread_->joinable())
        {
            switch_thread_->join();
        }
        delete switch_thread_;
        switch_thread_ = nullptr;
    }
    teardown_all();
    exit_flag.store(true, std::memory_order_release);
}

void auto_cli_ipc::InitChannel(std::string extra_info)
{
    (void)extra_info;
    status_.set_state(path::State::Probe);
    status_.set_selected(path::Kind::Socket);
    socket_leg_->InitChannel(extra_info);
    switch_thread_ = new std::thread(&auto_cli_ipc::supervise, this);
    dzIPC::ThreadDispatch::apply_thread_options(switch_thread_, thread_options_, verbose_,
                                                topic_name_ + "_AutoCliSwitchThread");
}

void auto_cli_ipc::reset_message(const std::shared_ptr<ServiceData>& msg)
{
    if (!msg)
    {
        return;
    }
    std::lock_guard<std::mutex> lock(route_mtx_);
    message_.reset(msg->clone());
    if (socket_leg_)
    {
        socket_leg_->reset_message(message_);
    }
    if (shm_leg_)
    {
        shm_leg_->reset_message(message_);
    }
}

bool auto_cli_ipc::send_request(std::shared_ptr<ServiceData>& request, uint64_t rev_tm)
{
    /* route_mtx_ 同时保护"选腿"与"停旧腿+翻 active": 请求要么完全走旧腿、
     * 要么完全走新腿, 不存在写到一半旧腿被关掉的状态。 */
    std::lock_guard<std::mutex> lock(route_mtx_);
    if (use_shm_.load(std::memory_order_acquire))
    {
        if (shm_leg_)
        {
            return shm_leg_->send_request(request, rev_tm);
        }
        return false;
    }
    if (socket_leg_)
    {
        return socket_leg_->send_request(request, rev_tm);
    }
    return false;
}

bool auto_cli_ipc::handshake_completed() const
{
    std::lock_guard<std::mutex> lock(const_cast<std::mutex&>(route_mtx_));
    if (use_shm_.load(std::memory_order_acquire) && shm_leg_)
    {
        return shm_leg_->handshake_completed();
    }
    return socket_leg_ && socket_leg_->handshake_completed();
}

path::Kind auto_cli_ipc::transport_current() const
{
    return use_shm_.load(std::memory_order_acquire) ? path::Kind::Shm : path::Kind::Socket;
}

void auto_cli_ipc::teardown_all()
{
    std::lock_guard<std::mutex> lock(route_mtx_);
    shm_leg_.reset();
    socket_leg_.reset();
    use_shm_.store(false, std::memory_order_release);
    status_.set_state(path::State::Closed);
    status_.mark_cleanup_done(now_ns());
}

void auto_cli_ipc::withdraw_to_socket(path::FallbackReason reason)
{
    std::lock_guard<std::mutex> lock(route_mtx_);
    shm_leg_.reset();
    if (socket_leg_ && !socket_leg_->data_plane_running())
    {
        socket_leg_->restart_data_plane();
    }
    use_shm_.store(false, std::memory_order_release);
    status_.set_selected(path::Kind::Socket);
    status_.set_fallback(reason);
}

void auto_cli_ipc::supervise()
{
    bool attempted_this_connection = false;

    while (running_.load(std::memory_order_acquire))
    {
        /* ---- S1 Probe ---- */
        status_.set_state(path::State::Probe);
        while (running_.load(std::memory_order_acquire) && !socket_leg_->handshake_completed())
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
        }
        if (!running_.load(std::memory_order_acquire))
        {
            return;
        }
        status_.set_state(path::State::Negotiate);

        if (opts_.allow_shm && !attempted_this_connection)
        {
            path::Evidence ev;
            if (!opts_.force_no_evidence)
            {
                find_peer(topic_name_, domain_id_, kPeerKind, ev);
            }
            status_.evidence.kind.store(ev.kind.load(std::memory_order_acquire), std::memory_order_release);
            status_.evidence.peer_pid.store(ev.peer_pid.load(std::memory_order_acquire), std::memory_order_release);
            status_.evidence.ts_ns.store(ev.ts_ns.load(std::memory_order_acquire), std::memory_order_release);

            /* 客户端还要等**服务端先建**。T2 §3 D3 第 4 步: SHM 通道只能服务端建
             * (shm_ser_ipc::InitChannel 无条件 clear_storage), 客户端先建会互相清存储。 */
            bool peer_ready = false;
            bool peer_rejected = false;
            if (ev.kind.load(std::memory_order_acquire) != 0)
            {
                status_.set_decision(path::DecisionReason::PeerInPool);
                attempted_this_connection = true;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(opts_.est_timeout_ms);
                while (running_.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
                {
                    /* 1(ProposeShm)/2(ConfirmShm) = 对端已把 SHM 腿建起来。这是**显式
                     * 裁定**: 不是客户端自己猜"我是不是同机", 而是对端告诉它。
                     * 4 = 对端的显式撤销(它自己判断不该切, 例如目标通道被占用) ——
                     * 立刻停手, 不要傻等满 T_est。 */
                    const uint8_t sig = socket_leg_->peer_path_signal();
                    if (sig == 1 || sig == 2)
                    {
                        peer_ready = true;
                        break;
                    }
                    if (sig == 4)
                    {
                        peer_rejected = true;
                        break;
                    }
                    if (!socket_leg_->handshake_completed())
                    {
                        break;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
                }
            }
            else
            {
                status_.set_decision(path::DecisionReason::NoEvidence);
            }

            if (peer_ready)
            {
                status_.switch_attempts.fetch_add(1, std::memory_order_acq_rel);
                status_.set_state(path::State::Establish);
                bool established = false;
                try
                {
                    auto leg = std::make_unique<shm::shm_cli_ipc>(topic_name_, message_, domain_id_, verbose_);
                    leg->InitChannel();
                    {
                        std::lock_guard<std::mutex> lock(route_mtx_);
                        shm_leg_ = std::move(leg);
                    }
                    /* ⛔ 有界等待。旧路径在这里是**无限期**等控制面 Ready
                     * (control_plane.cc:53 的 create|open 会让客户端自建一个永远不
                     * Ready 的段), 实测表现为"2 s 不完成、进程存活、无异常/日志/计数"
                     * 的静默挂起。切换必须把它变成一个有界、有终态、有计数的失败。 */
                    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(opts_.est_timeout_ms);
                    while (running_.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
                    {
                        if (shm_leg_ && shm_leg_->handshake_completed())
                        {
                            established = true;
                            break;
                        }
                        if (!socket_leg_->handshake_completed())
                        {
                            break;
                        }
                        std::this_thread::sleep_for(std::chrono::milliseconds(kPollMs));
                    }
                }
                catch (...)
                {
                    established = false;
                }

                if (established && running_.load(std::memory_order_acquire))
                {
                    std::lock_guard<std::mutex> lock(route_mtx_);
                    if (socket_leg_)
                    {
                        socket_leg_->set_path_signal(2 /* ConfirmShm */);
                        socket_leg_->stop_data_plane();
                    }
                    use_shm_.store(true, std::memory_order_release);
                    status_.set_selected(path::Kind::Shm);
                    status_.switch_successes.fetch_add(1, std::memory_order_acq_rel);
                }
                else
                {
                    const bool peer_gone = !socket_leg_->handshake_completed();
                    withdraw_to_socket(peer_gone ? path::FallbackReason::WithdrawnByPeer
                                                 : path::FallbackReason::ShmRendezvousTimeout);
                    status_.set_decision(peer_gone ? path::DecisionReason::Timeout : path::DecisionReason::ShmNotReady);
                    status_.switch_fallbacks.fetch_add(1, std::memory_order_acq_rel);
                }
            }
            else if (peer_rejected)
            {
                /* 对端明确说不切(例如它自己查到目标 SHM 通道被占用): 照它的原因记账,
                 * 而不是记成"超时"——后者会让一次有明确原因的拒绝看起来像一次故障。
                 * 注意这一支必须在 `if (peer_ready)` **外面**: 收到撤销时 peer_ready
                 * 是 false, 放在里面就永远不会被执行(本实现对的第一版就踩了这个)。 */
                status_.set_decision(path::DecisionReason::ChannelOccupied);
                status_.set_fallback(path::FallbackReason::ShmChannelOccupied);
                socket_leg_->set_path_signal(4 /* WithdrawToSocket */);   // 回执, 免得对端重试
            }
        }
        else if (!opts_.allow_shm)
        {
            status_.set_decision(path::DecisionReason::NoEvidence);
        }

        status_.set_state(path::State::Active);

        /* ---- S4 Active ---- */
        while (running_.load(std::memory_order_acquire))
        {
            if (!socket_leg_->handshake_completed())
            {
                status_.set_state(path::State::Withdraw);
                withdraw_to_socket(path::FallbackReason::WithdrawnByPeer);
                attempted_this_connection = false;
                status_.set_decision(path::DecisionReason::Pending);
                status_.evidence.kind.store(0, std::memory_order_release);
                status_.evidence.peer_pid.store(0, std::memory_order_release);
                status_.evidence.ts_ns.store(0, std::memory_order_release);
                break;
            }
            if (use_shm_.load(std::memory_order_acquire))
            {
                bool shm_ok;
                {
                    std::lock_guard<std::mutex> lock(route_mtx_);
                    shm_ok = shm_leg_ && shm_leg_->handshake_completed();
                }
                if (!shm_ok)
                {
                    status_.set_state(path::State::Withdraw);
                    withdraw_to_socket(path::FallbackReason::RemoteIoFailure);
                    status_.switch_fallbacks.fetch_add(1, std::memory_order_acq_rel);
                    status_.set_state(path::State::Active);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(kIdlePollMs));
        }
    }
}

}   // namespace autopath
}   // namespace dzIPC

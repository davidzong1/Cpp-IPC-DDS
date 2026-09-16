#include "dzIPC/auto_ser_cli_ipc.h"
#include <unistd.h>
#include <algorithm>
#include <iostream>
#include <utility>
#include "dzIPC/common/control_plane.h"
#include "dzIPC/ipc_info_pool.h"
#include "dzIPC/shm_ser_cli_ipc.h"
#include "dzIPC/socket_ser_cli_ipc.h"
#include "ipc_msg/ipc_msg_base/udp_id_init_msg.hpp"

namespace dzIPC {
namespace autopath {
namespace {

/* 判定轮询间隔。60 ms 是"比一次握手(实测 ~1.1 ms)慢两个数量级、又远小于 T_nego"
 * 的取法: 池是主机本地 SHM, 轮询开销可忽略; 间隔取太小只会无谓地抬高判定期的
 * CPU, 对缩短 T_nego 没有帮助。 */
constexpr uint64_t kPollMs = 20;
constexpr uint64_t kIdlePollMs = 50;   // Active 期的存活巡检(比判定期更省)

/* 握手帧里"协商正常推进"的那两个信号。取值直接取自 wire 枚举 —— 本文件里的每个
 * 信号值都必须来自**同一个**出处(IpcPubSubIdInitMsg::PathState), 散落的裸数字一旦
 * 与枚举脱钩就是静默错值(发 5 收到 4 这种错位不会报错, 只会记错原因)。
 * ⛔ 撤销类信号(4..8)**不**在这里 —— 它们一律走 decode_peer_withdraw() 的映射表。 */
constexpr uint8_t kSigProposeShm = static_cast<uint8_t>(IpcPubSubIdInitMsg::PathState::ProposeShm);
constexpr uint8_t kSigConfirmShm = static_cast<uint8_t>(IpcPubSubIdInitMsg::PathState::ConfirmShm);
constexpr uint8_t kSigWithdrawToSocket = static_cast<uint8_t>(IpcPubSubIdInitMsg::PathState::WithdrawToSocket);

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

uint8_t wire_signal_for_fallback(path::FallbackReason reason) noexcept
{
    using PS = IpcPubSubIdInitMsg::PathState;
    switch (reason)
    {
    case path::FallbackReason::ShmChannelOccupied:
        return static_cast<uint8_t>(PS::WithdrawChannelOccupied);
    case path::FallbackReason::ShmEstablishFailed:
        return static_cast<uint8_t>(PS::WithdrawEstablishFailed);
    case path::FallbackReason::ShmRendezvousTimeout:
        return static_cast<uint8_t>(PS::WithdrawRendezvousTimeout);
    case path::FallbackReason::RemoteIoFailure:
        return static_cast<uint8_t>(PS::WithdrawRuntimeDisconnect);
    case path::FallbackReason::WithdrawnByPeer:
    case path::FallbackReason::None:
        /* WithdrawnByPeer: 对端已经撤销了,**我们**这一端没有新原因可说 —— 回它一个
         * 泛化撤销即可(对端此时通常也已不在, 这条帧多半没有读者)。
         * None: "没有回退"却要发撤销 = 调用方用错了参数, 兜底走 legacy 值,
         * 绝不发 0 —— 0 是 Unknown("尚未提议"), 发出去会让对端以为本端还没裁定。 */
        return static_cast<uint8_t>(PS::WithdrawToSocket);
    }
    return static_cast<uint8_t>(PS::WithdrawToSocket);   // 枚举将来扩了也退回安全值
}

bool decode_peer_withdraw(uint8_t sig, path::DecisionReason& decision, path::FallbackReason& fallback) noexcept
{
    using PS = IpcPubSubIdInitMsg::PathState;
    switch (static_cast<PS>(sig))
    {
    case PS::WithdrawToSocket:
        /* 老端(F2 之前)只会发这个, 且它对**所有**撤销原因都发它 ⇒ 原因不可考。
         * 记成"对端撤销、原因未区分"是唯一诚实的写法: 猜成占用正是本次要修的错。 */
        decision = path::DecisionReason::ShmNotReady;
        fallback = path::FallbackReason::WithdrawnByPeer;
        return true;
    case PS::WithdrawChannelOccupied:
        decision = path::DecisionReason::ChannelOccupied;
        fallback = path::FallbackReason::ShmChannelOccupied;
        return true;
    case PS::WithdrawEstablishFailed:
        decision = path::DecisionReason::ShmNotReady;
        fallback = path::FallbackReason::ShmEstablishFailed;
        return true;
    case PS::WithdrawRendezvousTimeout:
        decision = path::DecisionReason::Timeout;
        fallback = path::FallbackReason::ShmRendezvousTimeout;
        return true;
    case PS::WithdrawRuntimeDisconnect:
        decision = path::DecisionReason::ShmNotReady;
        fallback = path::FallbackReason::RemoteIoFailure;
        return true;
    default:
        /* 0(尚未提议)/1/2(协商中)/3(保留值)/以及**不认识的任何值**。
         * 不认识的值不当撤销: 它可能是将来新增的积极信号, 读成拒绝会凭空造出一条
         * 错误原因; 交给调用方继续等, 最终按 T_est 超时安全回退(与老端逐字一致)。 */
        return false;
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
        /* 告诉对端"退回 socket", 免得它单方面继续等 SHM 就绪。
         * F2: **带上原因**(不再一律发 4)。对端据此记下的 fallback 才与事实相符 ——
         * 此前建腿失败/运行期断链都被对端记成"通道被占用"。老端不认 5..8 时的行为
         * 已在 udp_id_init_msg.hpp 里论证: 等满 T_est 后按超时安全回退, 不会误判。 */
        socket_leg_->set_path_signal(wire_signal_for_fallback(reason));
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
                /* 这次连接已经完成一次切换判定。占用拒绝也是一次终态，必须锁住；
                 * 否则 supervise 每轮都会重复递增 attempts/fallbacks 并重复公告。 */
                attempted_this_connection = true;
                /* F1 收口: 占用判定问的是**派生段名**, 不是 topic 名 —— 而派生规则会把
                 * '/' 清成 '_'(name_operator.cc 的 minimal_segment_sanitize), 于是
                 * "_foo" 与 "/foo" 是**同一个段**的两个别名。拒绝的真因在段名上, 光看
                 * topic 名字面看不出来: 同一份日志里两次运行、两个不同的 topic 名, 却指向
                 * 同一个段 —— 没有这条日志就只能去 /dev/shm 里按 topic 名找一个
                 * **根本不存在的段**(而真正占用的那一段名字完全不同)。
                 * ⛔ 门控在 verbose_ 上: 默认关(与仓内所有诊断输出一致), 打开时把
                 * **原始 topic** 与**派生段名**一起打出来, 让"别名"这件事在日志里可见。 */
                if (verbose_)
                {
                    std::cerr << "\033[33m[" << topic_name_ << "SerInfo] 拒绝切换: 目标 SHM 通道被占用; 原始 topic=\""
                              << topic_name_ << "\" domain=" << domain_id_ << " 派生段名=\""
                              << shm::ser_service_control_name(topic_name_, domain_id_)
                              << "\" (占用判据按派生段名匹配 —— 名字不同但派生段相同的两个 topic 互为别名)"
                              << "\033[0m" << std::endl;
                }
                status_.set_decision(path::DecisionReason::ChannelOccupied);
                status_.set_fallback(path::FallbackReason::ShmChannelOccupied);
                /* F2: 计数与标签必须同真。此前这一支只置标签不加计数, 于是
                 * `fallback=ShmChannelOccupied` 与 `switch_fallbacks=0` 同时成立 ——
                 * 按 `fallback=` 标签做监控会**漏报**一次真实回退(活体实测样本:
                 * build/live_runs/20260915_232219_2580718/client.log:22-23)。
                 * 目标不变量: switch_attempts == switch_successes + switch_fallbacks.
                 * `attempts` 由此包含"已取得证据、但因通道被占用而**拒绝**建立"的一次 ——
                 * 决策已作出, 故计入尝试(依据 F2 验收"失败路径各递增一次")。 */
                status_.switch_attempts.fetch_add(1, std::memory_order_acq_rel);
                status_.switch_fallbacks.fetch_add(1, std::memory_order_acq_rel);
                /* 公告(T2 §3 D7 铁律 2: 回退必须公告)。不公告的话对端只能靠等满
                 * T_est 才动, 而且它记下的原因是"对端没就绪"——与真实原因不符。
                 * F2: 公告**带上原因**(这一支发 5 = 通道被占用), 对端因此能记成
                 * ChannelOccupied 而不是"超时"。 */
                socket_leg_->set_path_signal(wire_signal_for_fallback(path::FallbackReason::ShmChannelOccupied));
            }
            else
            {
                attempted_this_connection = true;
                status_.set_decision(path::DecisionReason::PeerInPool);
                status_.switch_attempts.fetch_add(1, std::memory_order_acq_rel);
                status_.set_state(path::State::Establish);

                bool established = false;
                /* F2: 区分"为什么没建成"。建腿抛异常(clear_storage/create/set_ready
                 * 失败)与"腿建好了但对端没在 T_est 内接上"是两件不同的事, 此前都发
                 * 同一个 4, 对端只好一并记成"通道被占用"。默认取"会合超时"(下面的
                 * 等待循环是最可能走到的那条路), catch 里改判成"建腿失败"。 */
                path::FallbackReason fail_reason = path::FallbackReason::ShmRendezvousTimeout;
                try
                {
                    socket_leg_->set_path_signal(kSigProposeShm);
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
                    fail_reason = path::FallbackReason::ShmEstablishFailed;
                }

                if (established && running_.load(std::memory_order_acquire))
                {
                    std::lock_guard<std::mutex> lock(leg_mtx_);
                    /* 新腿已确认可用 -> **现在**才停旧腿 -> 翻 active。三者在一把锁里,
                     * 中间不存在"两条腿都可发"的时刻。 */
                    if (socket_leg_)
                    {
                        socket_leg_->set_path_signal(kSigConfirmShm);
                        socket_leg_->stop_data_plane();
                    }
                    use_shm_.store(true, std::memory_order_release);
                    status_.set_selected(path::Kind::Shm);
                    status_.switch_successes.fetch_add(1, std::memory_order_acq_rel);
                }
                else
                {
                    const bool peer_gone = !socket_leg_->handshake_completed();
                    /* 建立失败也要公告(让对端立刻收手而不是干等 T_est)。公告本身由
                     * withdraw_to_socket() 按原因选信号发出 —— 这里**不再**单独写一次
                     * 信号: 同一个字段有两个写入点, 先写的那个(永远是 legacy 4)会盖掉
                     * 真原因, 那正是 F2 要修的错位。 */
                    withdraw_to_socket(peer_gone ? path::FallbackReason::WithdrawnByPeer : fail_reason);
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
    /* ⛔ 这里**不**发撤销信号, 与 auto_ser_ipc 那边不对称 —— 这是事实而非疏漏:
     * SHM 腿是**服务端**建的, 客户端只是 attach, 它没有"通道被我占了/我建不出来"
     * 这类需要对端知道的原因; 而客户端真正会产生的两种撤销(对端没了 / 运行期腿断了)
     * 服务端都会**自己观察到**(它巡检同一对腿), 不需要客户端告诉它。
     * 补一句"反正也没人读": 服务端今天完全不读 peer_path_signal(见 auto_ser_ipc::
     * supervise), 所以在这里加一次 set_path_signal() 只会得到一个没有读者的写入,
     * 却会让"谁在什么时候公告什么"多出一处需要推理的地方。 */
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
            /* F2: 对端撤销时**它给的原因**。由 decode_peer_withdraw() 填, 不再由本端
             * 猜测 —— 见下面 peer_rejected 分支。 */
            path::DecisionReason peer_withdraw_decision = path::DecisionReason::Pending;
            path::FallbackReason peer_withdraw_fallback = path::FallbackReason::None;
            if (ev.kind.load(std::memory_order_acquire) != 0)
            {
                status_.set_decision(path::DecisionReason::PeerInPool);
                attempted_this_connection = true;
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(opts_.est_timeout_ms);
                while (running_.load(std::memory_order_acquire) && std::chrono::steady_clock::now() < deadline)
                {
                    /* 1(ProposeShm)/2(ConfirmShm) = 对端已把 SHM 腿建起来。这是**显式
                     * 裁定**: 不是客户端自己猜"我是不是同机", 而是对端告诉它。
                     * 4..8 = 对端的显式撤销(它自己判断不该切/没切成), 立刻停手, 不要
                     * 傻等满 T_est。**具体是哪一种撤销由对端给出**, 本端只解码不猜测;
                     * 老端只会发 4 ⇒ 记成"原因未区分"。 */
                    const uint8_t sig = socket_leg_->peer_path_signal();
                    if (sig == kSigProposeShm || sig == kSigConfirmShm)
                    {
                        peer_ready = true;
                        break;
                    }
                    if (decode_peer_withdraw(sig, peer_withdraw_decision, peer_withdraw_fallback))
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
                /* F2: 与服务端同因 —— 本端建腿抛异常(ShmEstablishFailed)与等满 T_est
                 * 仍没接上(ShmRendezvousTimeout)是两件事, 此前都记成"会合超时"。
                 * 客户端这一侧的标签是 F2 的主战场(它才是被监控那一端)。 */
                path::FallbackReason fail_reason = path::FallbackReason::ShmRendezvousTimeout;
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
                    fail_reason = path::FallbackReason::ShmEstablishFailed;
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
                    withdraw_to_socket(peer_gone ? path::FallbackReason::WithdrawnByPeer : fail_reason);
                    status_.set_decision(peer_gone ? path::DecisionReason::Timeout : path::DecisionReason::ShmNotReady);
                    status_.switch_fallbacks.fetch_add(1, std::memory_order_acq_rel);
                }
            }
            else if (peer_rejected)
            {
                /* 对端明确说不切: 照**它给出的原因**记账, 而不是本端猜一个。
                 * ⛔ 这一支是 F2 的落点。此前无论对端为什么撤销, 客户端都写死
                 * ChannelOccupied / ShmChannelOccupied —— 于是"建腿失败"和"运行期断链"
                 * 也被记成"通道被占用"(活体样本 build/live_runs/20260915_232219_2580718
                 * client.log:22-23), 监控按这个标签定性就指错了方向。
                 * 注意力放在**这一支的位置**: 它必须在 `if (peer_ready)` 外面 ——
                 * 收到撤销时 peer_ready 是 false, 放在里面就永远不会被执行。 */
                status_.set_decision(peer_withdraw_decision);
                status_.set_fallback(peer_withdraw_fallback);
                /* 计数与标签同真(否则按 fallback 标签监控会漏报一次真实回退)。 */
                status_.switch_attempts.fetch_add(1, std::memory_order_acq_rel);
                status_.switch_fallbacks.fetch_add(1, std::memory_order_acq_rel);
                /* 回执: 告诉对端"我收到了你的撤销, 免得它重试"。⛔ 回执只发 legacy 4:
                 * 回执**不是撤销**, 5..8 的语义是"我撤销的原因是 X", 让回执携带撤销原因
                 * 等于给对端一条假原因。今天这个值没有读者(服务端从不读 peer_path_signal,
                 * 见 auto_ser_ipc::supervise), 改成别的值既不产生新语义, 又会破坏与老端
                 * 行为的逐字一致性。 */
                socket_leg_->set_path_signal(kSigWithdrawToSocket);
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

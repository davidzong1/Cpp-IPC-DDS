#include "dzIPC/common/channel_scope.h"
#include "dzIPC/common/sample_message.h"
#include "dzIPC/socket_pub_sub_ipc.h"
#include <unistd.h>
#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <iostream>
#include <mutex>
#include <memory>
#include <typeinfo>
#include <vector>
#include "dzIPC/common/data_rev.h"
#include "dzIPC/threepools/socket_recv_worker.h"
#include "dzIPC/common/hash.h"
#include "dzIPC/common/local_pub_sub_registry.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/wire_accept.h"
#include "ipc_msg/ipc_msg_base/dzflat.h"
#include "ipc_msg/ipc_msg_base/udp_id_init_msg.hpp"
#include "libipc/platform/detail.h"
#include "libipc/utility/log.h"   // ipc::error：注销超时诊断（P1 §5）
#define ListenerWaitTime 1'000   // 1 second

namespace dzIPC {
namespace socket {

/* ===== 阶段 5：订阅收包接入共享层 socket worker（captain 裁定 A）=====
 * 通用 worker 由共享层提供；本模块只写 route 适配器 + 注册/回退/停机协议。
 * 收包路径只做：收取 + 多片重组 + 基础 wire 判别 + 投递进 msg_queue_/view_queue_。
 * 裁定 C：worker 路径的 recv_once 入口先 udp_node_readable()，无数据立即让路；
 *         兼容 subscribe_thread_ 路径不加该判据（否则退化成忙轮询）。
 * 裁定 B：正返回值 = 本次完整消息的真实字节数（out_bytes = meta.total_size），不得用 1 充字节。 */

/* 多片组包超时：与改造前逐字相同（不得缩短）。 */
constexpr std::uint64_t kSocketSubRecvTimeoutMs = 50;
/* 注销时等在途 recv_once 归零的上界（方案 §5 的 2000ms）。 */
constexpr int64_t kSubQuiesceTimeoutMs = 2000;

/* 订阅接收路径的 shared state（定义在 .cc；头文件只持 shared_ptr）。 */
struct socket_sub_receive_state
{
    std::string route_key;
    std::uint32_t domain_id{0};
    std::uint32_t generation{0};

    std::shared_ptr<ipc::socket::UDPNode> subscriber;
    std::shared_ptr<ipc::socket::UDPNode> ack_tx;

    mutable std::mutex mtx;
    std::shared_ptr<TopicData> msg_template;
    std::uint32_t exp_id{0};
    std::uint32_t exp_hash{0};

    std::shared_ptr<CircularQueue<IpcMsgBase>> msg_queue;
    std::shared_ptr<CircularQueue<Sample>> view_queue;

    std::atomic<threepools::RecvOwner> owner{threepools::RecvOwner::none};
    std::atomic<bool> stopping{false};
    std::atomic<std::size_t> recv_in_flight{0};
    std::mutex quiesce_mtx;
    std::condition_variable quiesce_cv;

    std::atomic<std::uint64_t> messages_received{0};
    std::atomic<std::uint64_t> bytes_received{0};
    std::atomic<std::uint64_t> recv_errors{0};
};

namespace {

/* ---- fork 防死锁闸（本模块自有的内部链接小工具，不引用其它模块符号）----
 * 池是进程级单例且 start() 一次性；fork 后子进程继承"已 start"却没有工作线程，子进程里
 * add_route 会走按需拉起（取池内锁 + 建线程），而被 fork 打断的父进程可能正持这些锁 ⇒ 死锁。
 * 因此 owner pid != 当前 pid 时完全不调用池，改走兼容收包线程。
 * 判断过程只取本文件的静态锁，不触碰池内部锁。 */
int32_t sub_pool_owner_pid()
{
    static std::mutex gate_mtx;
    static int32_t owner = 0;
    const int32_t pid = static_cast<int32_t>(::getpid());
    std::lock_guard<std::mutex> lock(gate_mtx);
    if (owner == 0)
    {
        owner = pid;
    }
    return owner;
}

bool sub_pool_allowed_in_this_process()
{
    return sub_pool_owner_pid() == static_cast<int32_t>(::getpid());
}

/* DZIPC_SOCKET_COMPAT_THREAD=1 ⇒ 强制兼容 subscribe_thread_（与 socket_ser_cli 同名同义）。
 * 进程内只读一次，避免"半程切换后端"。 */
bool sub_compat_forced()
{
    static const bool forced = [] {
        const char* v = std::getenv("DZIPC_SOCKET_COMPAT_THREAD");
        if (v == nullptr || v[0] == '\0')
        {
            return false;
        }
        return !(v[0] == '0' && v[1] == '\0');
    }();
    return forced;
}

/* ===== D-22：socket 通道「连接建立」的**有界**重试与可识别诊断 ==================
 *
 * 背景（队长裁决 D-22；完整取证见 artifacts/perf/20260929-r32-W10-portfix/）：
 * 原实现是 `while (!node->connect()) { sleep(1s); }` —— 三个缺陷叠在一起：
 *   ① **无上界**：一条话题永远连不上，就永远不返回，千路运行整片挂死（实测 rc=124，
 *      30 s 内 373~537 行重连日志，且**外部无法打断**）；
 *   ② **无诊断**：只打印"重连中"，既不说是哪个端口、也不说 errno 是什么 ——
 *      现场无法把"端口被占"与"地址写错/权限/路由"分开；
 *   ③ **不可退出**：析构/切换路径的 join 会永久阻塞（同类问题在 socket_ser_cli 的
 *      `connect_with_retry` 里已有先例：那里只为可中断而加 running 检查，其余语义逐字不变）。
 *
 * 本函数的三条纪律：
 *   · **有上界**：默认最多 kSocketConnectAttemptsDefault 次尝试（首次立即 + 其后 1 s 间隔，
 *     与旧实现的重试间隔**逐位相同**：分批 10×100 ms 只为让 sleep 可被打断）；
 *   · **有诊断**：每次失败立即取 errno 并分类（port_in_use / permission_denied /
 *     address_not_available / no_route / fd_exhausted / other_errno），首报 + 末报 + 每 10 次
 *     节流一行（与 note_pool_exhausted 同口径：真失败时逐条打印会淹没日志，而这里要的是
 *     "看得见"）；耗尽时再打一条**结构化终报**（含 topic/role/port/ip/errno/次数/耗时/处置建议）；
 *   · **显式失败**：返回 false ⇒ 调用方**必须**提前返回，不得把"没连上的通道"当成可用
 *     （不注册 info_pool 条目、不起发现线程/收包路径）。⛔ 不静默、不假装 ready。
 *
 * 兼容路径（旧语义，**显式**启用）：`DZIPC_SOCKET_CONNECT_MAX_ATTEMPTS=0` ⇒ 无界重试，
 * 并在进程内首报一行说明"已显式启用无界重试"。取 0 是"无界"，不是"不重试"。
 * 上限可配置的理由：不同部署的端口争用时长差别很大（脚本化 CI 里 30 s 足够；
 * 长期被外部服务占用的机器上，运维可能希望更早失败以便快速定位）。
 *
 * ⛔ 本函数**不修改端口公式、不做端口回退/漂移**（队长 D-22 第 2 条明确不要求）。
 *    它只把"永远等下去"换成"有界等 + 说清为什么失败"。 */
constexpr int kSocketConnectAttemptsDefault = 30;

/* 上限：进程内只读一次（与 DZIPC_SOCKET_COMPAT_THREAD 同一纪律，杜绝半程切换）。 */
int socket_connect_max_attempts()
{
    static const int n = [] {
        const char* v = std::getenv("DZIPC_SOCKET_CONNECT_MAX_ATTEMPTS");
        if (v == nullptr || v[0] == '\0')
        {
            return kSocketConnectAttemptsDefault;
        }
        const long parsed = std::strtol(v, nullptr, 10);
        if (parsed < 0)
        {
            return kSocketConnectAttemptsDefault;   /* 负数视为未设，不留"负次尝试"的怪态 */
        }
        return static_cast<int>(parsed);            /* 0 = 无界（显式兼容路径） */
    }();
    return n;
}

const char* connect_errno_class(int e) noexcept
{
    switch (e)
    {
    case 0:
        return "unknown";
    case EADDRINUSE:
        return "port_in_use";
    case EACCES:
    case EPERM:
        return "permission_denied";
    case EADDRNOTAVAIL:
        return "address_not_available";
    case ENODEV:
    case ENETUNREACH:
        return "no_route";
    case EMFILE:
    case ENFILE:
        return "fd_exhausted";
    default:
        return "other_errno";
    }
}

/* 处置建议按 errno 分类给 —— 给错方向的建议比不给更坏。 */
const char* connect_remediation(int e) noexcept
{
    switch (e)
    {
    case EADDRINUSE:
        return "端口已被占用；建议: ss -ulnp | grep :<port> 查占用者；若占用者在 "
               "net.ipv4.ip_local_port_range 内, 可由运维收窄该区间或用 "
               "net.ipv4.ip_local_reserved_ports 为 dzIPC 端口窗口让路(root); "
               "亦可换 domain 或话题名绕开该端口(端口是话题名的纯函数)";
    case EACCES:
    case EPERM:
        return "权限不足；建议: 检查容器 seccomp/cap_net_bind_service 与端口是否 <1024";
    case EADDRNOTAVAIL:
        return "本机没有该组播地址；建议: 检查网卡/组播路由与容器网络模式";
    case ENODEV:
    case ENETUNREACH:
        return "组播地址不可达；建议: 检查多播路由表与默认路由接口";
    case EMFILE:
    case ENFILE:
        return "fd 耗尽；建议: 提高 RLIMIT_NOFILE(千路 socket 约 2 fd/route)";
    default:
        return "未归类的 errno；请按 strerror 排查, 并保留本行日志";
    }
}

/* 返回 true = 已连上；false = 有界重试耗尽，或对象正在停止（running 变假）⇒ 调用方必须
 * **提前返回**并把本通道当"不可用"处理。 */
bool connect_with_bounded_retry(ipc::socket::UDPNode* node, const std::atomic<bool>& running,
                                const std::string& topic_name, const char* who, const char* noun,
                                const std::string& ipaddr, uint16_t port)
{
    if (node == nullptr)
    {
        return false;
    }
    const int max_attempts = socket_connect_max_attempts();
    if (max_attempts == 0)
    {
        static std::atomic<bool> legacy_notice_printed{false};
        bool expected = false;
        if (legacy_notice_printed.compare_exchange_strong(expected, true))
        {
            std::cerr << "\033[33m[dzIPC][socket_connect] DZIPC_SOCKET_CONNECT_MAX_ATTEMPTS=0 ⇒ 已**显式**启用"
                         "无界重试（W05/2025 之前的历史语义）: 连接失败将持续重试且不设上限, "
                         "外部只能通过销毁对象(停止标志)打断\033[0m" << std::endl;
        }
    }

    const auto t0 = std::chrono::steady_clock::now();
    int first_errno = 0;
    for (int attempt = 1;; ++attempt)
    {
        if (!running.load(std::memory_order_acquire))
        {
            std::cerr << "\033[33m[" << topic_name << who << "] connect aborted: object is stopping "
                      << "(attempt " << attempt << ", port " << port << ")\033[0m" << std::endl;
            return false;
        }
        /* ⛔ errno = 0 必须**紧贴**调用之前：connect() 也可能因为**非 errno 原因**返回
         * false（pimpl 失效态、inet_pton 解析失败路径），此时残留的旧 errno 会把它
         * 误分类成"端口被占"之类的具体原因 —— 那比不分类更坏。置 0 后这种情况统一
         * 落到 "unknown"，读日志的人不会得到一条自信的错误结论。 */
        errno = 0;
        if (node->connect())
        {
            return true;
        }
        /* ⛔ errno 必须在 connect() 返回后**立即**取：UDPNode::connect() 内部会 close(fd),
         * 任何后续 syscall 都可能覆写 errno。实测 (errno_preservation.txt): libipc 的
         * pimpl + IPC_EXCEPTION_ 包装不会吃掉 bind 的 EADDRINUSE，但仍以立即取为准。 */
        const int e = errno;
        if (first_errno == 0)
        {
            first_errno = e;
        }
        const bool legacy = (max_attempts == 0);
        const bool last = (!legacy && attempt >= max_attempts);
        const bool first = (attempt == 1);
        /* 首报 + 末报 + 每 10 次节流（避免 500+ 行洪水，又保证"看得见"）。 */
        if (first || last || (attempt % 10) == 0)
        {
            std::cerr << "\033[31m[" << topic_name << who << "] Failed to connect " << noun << " (attempt " << attempt
                      << (legacy ? "" : ("/" + std::to_string(max_attempts))) << "), reconnect after 1 second: "
                      << "port=" << port << " ip=" << ipaddr << " errno=" << e << "(" << connect_errno_class(e)
                      << ":" << std::strerror(e) << ")\033[0m" << std::endl;
        }
        if (last)
        {
            const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
                                .count();
            std::cerr << "\033[31m[dzIPC][socket_connect_failed] topic=" << topic_name << " role=" << who
                      << " noun=" << noun << " port=" << port << " ip=" << ipaddr
                      << " attempts=" << attempt << " elapsed_ms=" << ms << " first_errno=" << first_errno << "("
                      << connect_errno_class(first_errno) << ":" << std::strerror(first_errno) << ")"
                      << " last_errno=" << e << "(" << connect_errno_class(e) << ")"
                      << " ⇒ 本通道**不可用**, 已显式失败(不再重试); " << connect_remediation(first_errno)
                      << "; 如需旧的无界重试: DZIPC_SOCKET_CONNECT_MAX_ATTEMPTS=0\033[0m" << std::endl;
            return false;
        }
        /* 分批睡眠：一次睡满 1 s 会让"停止"的最坏延迟多 1 s（同 socket_ser_cli 先例）。 */
        for (int i = 0; i < 10; ++i)
        {
            if (!running.load(std::memory_order_acquire))
            {
                std::cerr << "\033[33m[" << topic_name << who << "] connect aborted: object is stopping "
                          << "(after attempt " << attempt << ", port " << port << ")\033[0m" << std::endl;
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
}

const char* sub_status_reason(threepools::RecvRegisterStatus s) noexcept
{
    switch (s)
    {
    case threepools::RecvRegisterStatus::backend_unavailable:
        return "backend_unavailable";
    case threepools::RecvRegisterStatus::duplicate:
        return "duplicate";
    case threepools::RecvRegisterStatus::busy:
        return "busy";
    case threepools::RecvRegisterStatus::stopped:
        return "stopped";
    case threepools::RecvRegisterStatus::invalid_token:
        return "invalid_token";
    case threepools::RecvRegisterStatus::invalid_route:
        return "invalid_route";
    case threepools::RecvRegisterStatus::wait_set_full:
        return "wait_set_full";
    case threepools::RecvRegisterStatus::ok:
        break;
    }
    return "ok";
}

/* 收包独占状态机（契约 §4.6）：worker 路径与兼容线程路径共用同一份实现。 */
threepools::RecvOwner sub_recv_owner(const socket_sub_receive_state& state) noexcept
{
    return state.owner.load(std::memory_order_acquire);
}

bool sub_try_claim_recv(socket_sub_receive_state& state, threepools::RecvOwner who) noexcept
{
    if (who == threepools::RecvOwner::none)
    {
        return false;
    }
    threepools::RecvOwner expected = threepools::RecvOwner::none;
    return state.owner.compare_exchange_strong(expected, who, std::memory_order_acq_rel);
}

void sub_release_recv(socket_sub_receive_state& state) noexcept
{
    auto expected = state.owner.load(std::memory_order_acquire);
    while (expected != threepools::RecvOwner::none)
    {
        if (state.owner.compare_exchange_weak(expected, threepools::RecvOwner::none, std::memory_order_acq_rel))
        {
            break;
        }
    }
}

/* 三支分流：与抽取前（subscribe lambda 内）逐行同义，只把 exp_id/exp_hash 提成参数、
 * msg_queue_/view_queue_ 换成 state 上的同一对象，并把 continue 换成 return。
 * 此处不做任何用户可见动作，只做 wire 判别与入队。 */
void process_received_wire(const std::shared_ptr<socket_sub_receive_state>& state,
                           std::shared_ptr<TopicData>& local_msg,
                           ipc::buffer& wire,
                           std::uint32_t exp_id,
                           std::uint32_t exp_hash)
{
    if (wire.empty())
    {
        /* ---- 物化路径: TLV（收不到段时 wire 恒空, 见 data_rev.h）---- */
        std::shared_ptr<IpcMsgBase> ptr_cache;
        local_msg->swap(ptr_cache);
        state->msg_queue->push(std::move(ptr_cache));
        return;
    }

    /* ---- 借样路径: 段首是 DZFlat（见 data_rev.cc 的分流闸）---- */
    const bool viewable = (exp_hash != 0);
    if (!viewable)
    {
        /* schema-less 话题（GenericMessage / 手写类型）走物化队列；段字节仍是借的（wire 是接收层
         * 去帧出来的独立连续块，自带所有权）。GenericMessage 覆写 dzflat_adopt 收下它；手写类型
         * 没有覆写 ⇒ 返回 false ⇒ 按类型不匹配丢弃，与 AcceptWire 一致。
         * 不设借样配额（UF-012 只管 SHM 腿）：这里的 wire 不占 chunk 池。 */
        std::uint32_t seg_id = 0;
        std::uint32_t seg_hash = 0;
        {
            dzflat::SegHeader h{};
            std::memcpy(&h, wire.data(), sizeof(h));
            seg_hash = h.schema_hash;
        }
        if (!IpcMsgBase::dzflat_peek_msg_id(wire.data(), wire.size(), seg_id) || seg_id != exp_id)
        {
            dzIPC::detail::NoteDzFlatRx(dzIPC::detail::DzFlatRxEvent::kDzFlatIdSkipped);
            return;
        }
        dzIPC::detail::NoteDzFlatRx(dzIPC::detail::DzFlatRxEvent::kDzFlatAccepted);
        if (!local_msg->topic()->dzflat_adopt(std::move(wire), seg_hash))
        {
            return;
        }
        std::shared_ptr<IpcMsgBase> ptr_cache;
        local_msg->swap(ptr_cache);
        state->msg_queue->push(std::move(ptr_cache));
        return;
    }

    std::uint32_t seg_id = 0;
    if (!IpcMsgBase::dzflat_peek_msg_id(wire.data(), wire.size(), seg_id) || seg_id != exp_id)
    {
        dzIPC::detail::NoteDzFlatRx(dzIPC::detail::DzFlatRxEvent::kDzFlatIdSkipped);
        return;
    }
    dzflat::SegHeader h{};
    std::memcpy(&h, wire.data(), sizeof(h));
    if (h.schema_hash != exp_hash)
    {
        dzIPC::detail::NoteDzFlatRx(dzIPC::detail::DzFlatRxEvent::kDzFlatSchemaDrop);
        return;
    }
    dzIPC::detail::NoteDzFlatRx(dzIPC::detail::DzFlatRxEvent::kDzFlatAccepted);
    /* 借样：wire 是一段独立、连续的 DZFlat 段（由接收层去帧保证），原样移进 Sample 即可。 */
    state->view_queue->push(std::make_shared<Sample>(std::move(wire), seg_id, exp_hash));
}

/* 一次"到完整消息边界"的收包 + 分流。worker 与兼容线程共用。
 *   gate_on_readiness == true  : worker 路径 —— 先做非阻塞可读判据（裁定 C），无数据立即返回 0；
 *   gate_on_readiness == false : 兼容路径 —— 保持"阻塞在 chunk_rev_topic(tm=50ms)"的既有语义，
 *                                不能加可读判据，否则循环退化成忙轮询（阶段 5 红线）。 */
std::size_t socket_sub_receive_once(const std::shared_ptr<socket_sub_receive_state>& state, bool gate_on_readiness)
{
    if (state->stopping.load(std::memory_order_acquire))
    {
        return 0;
    }
    if (gate_on_readiness && !udp_node_readable(state->subscriber))
    {
        return 0;
    }

    std::shared_ptr<TopicData> local_msg;
    std::uint32_t exp_id = 0;
    std::uint32_t exp_hash = 0;
    {
        std::lock_guard<std::mutex> lock(state->mtx);
        if (state->msg_template)
        {
            local_msg.reset(state->msg_template->clone());
        }
        exp_id = state->exp_id;
        exp_hash = state->exp_hash;
    }
    if (!local_msg)
    {
        return 0;
    }

    state->recv_in_flight.fetch_add(1, std::memory_order_acq_rel);
    ipc::buffer wire;
    std::size_t bytes = 0;
    bool received = false;
    try
    {
        /* rev timeout 50ms。out_payload 非空 ⇒ 收到 DZFlat 段时不做 TLV 反序列化，段（已去帧）
         * 从 wire 交回；收到 TLV 时 wire 保持为空 —— 判据是 wire 是否为空。
         * out_bytes（六参重载）= 本条完整消息的真实字节数（meta.total_size）。 */
        received = chunk_rev_topic(state->subscriber, local_msg, kSocketSubRecvTimeoutMs, state->ack_tx, &wire, &bytes);
    }
    catch (...)
    {
        /* 绝不把异常抛给共享 worker 线程（它会 std::terminate）。 */
        state->recv_errors.fetch_add(1, std::memory_order_relaxed);
    }
    state->recv_in_flight.fetch_sub(1, std::memory_order_acq_rel);
    state->quiesce_cv.notify_all();
    if (!received)
    {
        return 0;
    }

    process_received_wire(state, local_msg, wire, exp_id, exp_hash);
    state->messages_received.fetch_add(1, std::memory_order_relaxed);
    state->bytes_received.fetch_add(bytes, std::memory_order_relaxed);
    /* 裁定 B：确已收到就必须返回非 0（worker 用 0 判"本轮无数据"）。 */
    return bytes == 0 ? 1 : bytes;
}

/* SocketRecvRouteSource 适配器：本订阅的接收通道（只注册 subscriber_；
 * ack_tx_ 是发送端点、不入组，绝不进 worker）。 */
class socket_sub_receive_route final : public threepools::SocketRecvRouteSource
{
public:
    explicit socket_sub_receive_route(std::shared_ptr<socket_sub_receive_state> state)
        : state_(std::move(state))
    {}

    const char* route_name() const noexcept override { return state_->route_key.c_str(); }
    std::uint32_t domain_id() const noexcept override { return state_->domain_id; }

    /* owner = UDPNode*（宿主侧稳定身份），handle = udp_node_wait_handle（0 = 不可等待）。 */
    threepools::SocketWaitToken wait_token() const noexcept override
    {
        threepools::SocketWaitToken token;
        token.owner = state_->subscriber.get();
        token.handle = udp_node_wait_handle(state_->subscriber);
        return token;
    }

    std::size_t recv_once() override { return socket_sub_receive_once(state_, true); }

    /* socket 侧没有 sequence 字可做廉价重检；事实来源是共享 worker 的 level-triggered wait(0) 全扫。 */
    bool has_pending() const noexcept override { return false; }

    threepools::RecvOwner recv_owner() const noexcept override { return sub_recv_owner(*state_); }
    bool try_claim_recv(threepools::RecvOwner who) noexcept override { return sub_try_claim_recv(*state_, who); }
    void release_recv() noexcept override { sub_release_recv(*state_); }

    /* 注销协议第 3 步：置 stopping（拒绝新 recv_once）+ cancel_wait 打断在途组包。幂等、nullptr 安全。 */
    void stop_and_wake() noexcept override
    {
        state_->stopping.store(true, std::memory_order_release);
        udp_node_cancel_wait(state_->subscriber);
    }

    /* 注销协议第 5 步：等本模块 in-flight 归零（有界；超时只打诊断，不阻塞注销）。
     * P1（§5）：检查 wait_for 返回值；超时**先出锁取快照再打印**。 */
    void wait_quiescent() noexcept override
    {
        bool timed_out = false;
        std::size_t inflight = 0;
        {
            std::unique_lock<std::mutex> lock(state_->quiesce_mtx);
            state_->quiesce_cv.wait_for(lock, std::chrono::milliseconds{kSubQuiesceTimeoutMs}, [this] {
                return state_->recv_in_flight.load(std::memory_order_acquire) == 0;
            });
            inflight = state_->recv_in_flight.load(std::memory_order_acquire);
            timed_out = inflight != 0;
        }
        if (timed_out)
        {
            ipc::error("[socket_sub] wait_quiescent timeout after %lld ms: route='%s' generation=%u in_flight=%llu; "
                       "continuing bounded teardown\n",
                       static_cast<long long>(kSubQuiesceTimeoutMs),
                       state_->route_key.c_str(), state_->generation,
                       static_cast<unsigned long long>(inflight));
        }
    }

private:
    std::shared_ptr<socket_sub_receive_state> state_;
};

}   // namespace
/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
socket_pub_ipc::socket_pub_ipc(const std::shared_ptr<TopicData>& msg, const std::string& topic_name, size_t domain_id,
                               bool verbose, bool enable_thread_qos, int cpu_id, int thread_priority)
    : pub_ipc_base(msg, topic_name, domain_id, verbose)
    , topic_name_(topic_name)
    , domain_id_(domain_id)
    , verbose_(verbose)
    , thread_options_(dzIPC::ThreadDispatch::make_realtime_options(enable_thread_qos, cpu_id, thread_priority))
{
    this->topic_msg_.reset(msg->clone());
    this->port_hash_ = dzIPC::common::socket_scope_port(topic_name_, domain_id_, dzIPC::common::ScopeKind::PubSub);
    this->ipaddr_ = dzIPC::common::socket_scope_address(topic_name_, domain_id_, dzIPC::common::ScopeKind::PubSub);
    dzIPC::ThreadDispatch::apply_current_thread_options(thread_options_, verbose_, topic_name_ + "_SocketPubOwnerThread");
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
socket_pub_ipc::~socket_pub_ipc()
{
    {
        std::lock_guard<std::mutex> lock(sleep_mtx);
        running.store(false, std::memory_order_release);
    }
    sleep_cv.notify_all();   // 立即唤醒正在 wait_for 的线程
    if (discovery_thread_ != nullptr)
    {
        if (discovery_thread_->joinable())
        {
            discovery_thread_->join();
        }
        delete discovery_thread_;
        discovery_thread_ = nullptr;
    }
    if (publisher_)
    {
        publisher_->close();
    }
    if (ack_rx_)
    {
        ack_rx_->close();
    }
    exit_flag.store(true, std::memory_order_release);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void socket_pub_ipc::reset_message(const std::shared_ptr<TopicData>& msg)
{
    topic_msg_.reset(msg->clone());
    // Reset fast-path state: new message type requires re-confirmation.
    std::lock_guard<std::mutex> lock(fast_path_mtx_);
    fp_consecutive_ = 0;
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void socket_pub_ipc::InitChannel(std::string extra_info)
{
    try
    {
        /* 数据通道设 SendOnly —— 不加入组播组, 于是收不到自己发出去的分片回绕。
         *
         * 这是 Reliable 模式能工作的前提: 之前收发共用一条入了组的 socket,
         * 1 MB 消息的 713 个分片全部回绕进自己的接收队列, 订阅端的 ACK 排在
         * 它们后面, 等待窗口必然先超时(实测吞吐塌到 1 msg/s 而丢包率 0.00%)。
         *
         * 注意不能改用 IP_MULTICAST_LOOP=0 达到同样目的 —— 那是主机级开关,
         * 会让本机所有进程都收不到, 同机 IPC 直接失效。详见 libipc/udp.h。 */
        publisher_ = std::make_shared<ipc::socket::UDPNode>(this->topic_name_.c_str(), this->ipaddr_.c_str(),
                                                            this->port_hash_, ipc::socket::NodeRole::SendOnly);
        publisher_->set_scope(dzIPC::common::channel_scope_token(topic_name_, domain_id_, dzIPC::common::ScopeKind::PubSub));
        /* ACK 回传通道: 订阅端把 ACK/NACK 发到这个端口, 上面没有自己的数据。 */
        ack_rx_ = std::make_shared<ipc::socket::UDPNode>(
            this->topic_name_.c_str(), this->ipaddr_.c_str(),
            static_cast<uint16_t>(this->port_hash_ + dzIPC::common::kUdpPortOffsetAckData),
            ipc::socket::NodeRole::RecvOnly);
        ack_rx_->set_scope(dzIPC::common::channel_scope_token(topic_name_, domain_id_, dzIPC::common::ScopeKind::PubSub));
        if (verbose_)
            std::cerr << "\033[32m[" << topic_name_ << "PubInfo] Publisher initialized on IP: " << this->ipaddr_
                      << " Port: " << this->port_hash_ << " for topic: " << topic_name_ << "\033[0m" << std::endl;
        /* D-22：同样的有界重试（发布端是 SendOnly、不 bind，故端口冲突面不落在这里；
         * 但"无上界 + 无诊断"这条缺陷与角色无关，一并收口）。 */
        if (!connect_with_bounded_retry(publisher_.get(), running, topic_name_, "PubInfo", "publisher", ipaddr_,
                                        port_hash_))
        {
            return;
        }
        /* ACK 通道连不上不算致命: BestEffort 完全不需要它, 只有 publish_blocking
         * 会退化回"在数据通道上等 ACK"(即端点分离之前的行为)。 */
        if (!ack_rx_->connect())
        {
            if (verbose_)
            {
                std::cerr << "\033[33m[" << topic_name_
                          << "PubInfo] ACK channel unavailable; publish_blocking() will fall back to the "
                             "data socket and may time out on multi-fragment payloads\033[0m"
                          << std::endl;
            }
            ack_rx_.reset();
        }
        std::string topic_type_name = topic_msg_->topic()
                                          ? dzIPC::info_pool::demangle(typeid(*topic_msg_->topic()).name())
                                          : std::string{};
        topic_type_name = extract_last_segment(topic_type_name);
        pool_reg_.rebind({dzIPC::info_pool::EntryKind::SocketPub, topic_name_, topic_type_name, "socket",
                          static_cast<uint64_t>(domain_id_), extra_info});
        /* 本进程的 SocketPub 条目注册完成后再启动发现线程, 避免它先于
         * rebind 拿到不完整的池快照。重复 InitChannel 不再重复起线程。 */
        if (discovery_thread_ == nullptr)
        {
            discovery_thread_ = new std::thread(&socket_pub_ipc::discovery_loop, this);
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "\033[31m[" << topic_name_ << "PubInfo] Error initializing channel: " << e.what() << "\033[0m"
                  << std::endl;
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void socket_pub_ipc::discovery_loop()
{
    /* UDP 组播是单向的: 发布端把包投进组地址, 协议本身不会告诉它谁加入了组。
     * 因此这里退而求其次, 用进程外共享的 IpcInfoPool 统计本 topic/domain 下
     * 存活的 SocketSub 条目, 语义上对齐 SHM 的 shm_pub_ipc::pub_handshake()。
     *
     * 探测边界见头文件 has_subscribed() 注释: 池只覆盖走 dzIPC 注册的订阅者。*/
    bool had_subscriber = false;
    while (running.load(std::memory_order_acquire))
    {
        size_t total_subs = 0;
        bool pool_ok = true;
        try
        {
            auto pool_snap = info_pool::IpcInfoPool::instance().snapshot();
            for (auto& entry : pool_snap)
            {
                if (entry.kind == info_pool::EntryKind::SocketSub && entry.topic_name == topic_name_
                    && entry.domain_id == static_cast<uint64_t>(domain_id_) && entry.alive && entry.in_use)
                {
                    ++total_subs;
                }
            }
        }
        catch (const std::exception& e)
        {
            /* 池暂时不可用: 保持上一次的判定, 不要误报为"无订阅者" */
            pool_ok = false;
            if (verbose_)
            {
                std::cerr << "\033[33m[" << topic_name_ << "PubInfo] IpcInfoPool snapshot failed: " << e.what()
                          << "; keeping previous has_subscribed() state\033[0m" << std::endl;
            }
        }

        if (pool_ok)
        {
            const bool has_peer = total_subs > 0;
            subscribed_.store(has_peer, std::memory_order_release);
            if (has_peer != had_subscriber && verbose_)
            {
                std::cerr << "\033[32m[" << topic_name_ << "PubInfo] "
                          << (has_peer ? "Publisher detected a subscriber on topic: "
                                       : "Publisher lost all subscribers on topic: ")
                          << topic_name_ << "\033[0m" << std::endl;
            }
            had_subscriber = has_peer;
        }

        std::unique_lock<std::mutex> lock(sleep_mtx);
        sleep_cv.wait_for(lock, std::chrono::milliseconds(kDiscoveryPollMs),
                          [this] { return !running.load(std::memory_order_acquire); });
    }
    subscribed_.store(false, std::memory_order_release);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool socket_pub_ipc::publish(std::shared_ptr<IpcMsgBase> msg)
{
    return publish_best_effort(std::move(msg));
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool socket_pub_ipc::publish_best_effort(std::shared_ptr<IpcMsgBase> msg)
{
    // --- Intra-process fast path (socket nodelet) ---
    // Gated by the unified process-wide switch dzIPC::EnableNodelet(true).
    // When enabled, verify via IpcInfoPool that ALL known SocketSub entries
    // for this topic/domain reside in the current process.  Only then
    // clone-once + fanout, skipping UDP serialize+send.
    //
    // ---- Detection boundary (critical) ----
    // IpcInfoPool ONLY discovers subscribers registered through dzIPC's own
    // ScopedRegistration (socket_sub_ipc::InitChannel).  Native UDP listeners,
    // passive sniffers, raw-socket consumers, and any external tool reading
    // the UDP stream are INVISIBLE to this check.  When EnableNodelet is true
    // and the pool reports all-local, the fast path will bypass the UDP send
    // entirely — those invisible consumers receive NOTHING for that message.
    //
    // For debugging/monitoring: keep EnableNodelet false (default), or use
    // publish_for_sniffer() to force the UDP path for individual messages.
    //
    // ---- Stability gate ----
    // K=3 consecutive publishes with the same (key, local snapshot size,
    // IpcInfoPool SocketSub count) gates activation.  Any topology change
    // resets K to 0.
    //
    // TOCTOU risk: between the IpcInfoPool snapshot and the queue pushes, a
    // cross-process subscriber may join or leave.  K=3 dampens the window but
    // does not close it.  A late-joining remote sub will miss that message
    // (fast path skipped the UDP send).

    if (dzIPC::IsNodeletEnabled())
    {
        ChannelKey key{topic_name_, domain_id_, msg->msg_id(), ChannelKind::SocketPubSub};
        auto& reg = LocalPubSubRegistry::instance();
        auto snapshot = reg.subscriber_snapshot(key);

        // Query IpcInfoPool for total SocketSub count (all processes).
        size_t total_socket_subs = 0;
        {
            auto pool_snap = info_pool::IpcInfoPool::instance().snapshot();
            for (auto& entry : pool_snap)
            {
                if (entry.kind == info_pool::EntryKind::SocketSub
                    && entry.topic_name == topic_name_
                    && entry.domain_id == static_cast<uint64_t>(domain_id_)
                    && entry.alive && entry.in_use)
                {
                    ++total_socket_subs;
                }
            }
        }

        bool use_fast_path = false;
        {
            std::lock_guard<std::mutex> lock(fast_path_mtx_);

            // Reset on any state change: key, local snapshot size, or pool count.
            if (!(key == last_fp_key_) || snapshot.size() != last_fp_snapshot_size_
                || total_socket_subs != last_fp_total_subs_)
            {
                fp_consecutive_ = 0;
                last_fp_key_ = key;
                last_fp_snapshot_size_ = snapshot.size();
                last_fp_total_subs_ = total_socket_subs;
            }

            // All-local check: must have local subs, pool must be non-empty,
            // and pool count must exactly match local count.
            if (!snapshot.empty() && total_socket_subs > 0
                && total_socket_subs == snapshot.size())
            {
                ++fp_consecutive_;
                if (fp_consecutive_ >= kFastPathConfirm)
                {
                    use_fast_path = true;
                }
            }
            else
            {
                fp_consecutive_ = 0;

                // One-shot warnings (per instance, per reason).
                if (snapshot.empty() && !warned_no_local_sub_)
                {
                    warned_no_local_sub_ = true;
                    std::cerr << "\033[33m[" << topic_name_
                              << "PubInfo] socket nodelet requested but unavailable "
                              << "(no local subscribers); falling back to standard UDP path\033[0m"
                              << std::endl;
                }
                else if (!snapshot.empty() && total_socket_subs == 0 && !warned_pool_unavailable_)
                {
                    warned_pool_unavailable_ = true;
                    std::cerr << "\033[33m[" << topic_name_
                              << "PubInfo] socket nodelet requested but unavailable "
                              << "(IpcInfoPool returned 0 SocketSub entries — pool may be unavailable); "
                              << "falling back to standard UDP path\033[0m"
                              << std::endl;
                }
                else if (!snapshot.empty() && total_socket_subs > 0
                         && total_socket_subs != snapshot.size() && !warned_cross_process_)
                {
                    warned_cross_process_ = true;
                    std::cerr << "\033[33m[" << topic_name_
                              << "PubInfo] socket nodelet requested but unavailable "
                              << "(cross-process SocketSub detected: local=" << snapshot.size()
                              << " total=" << total_socket_subs << "); "
                              << "falling back to standard UDP path\033[0m"
                              << std::endl;
                }
            }
        }

        if (use_fast_path)
        {
            // Clone once, fanout to all local queues.
            std::shared_ptr<IpcMsgBase> cloned(msg->clone());
            for (auto& q : snapshot)
            {
                q->push(cloned);
            }
            return true;
        }
    }

    // Fallback: standard UDP path
    try
    {
        ipc::buffer response_data(std::move(msg->serialize()));
        SocketSendOptions options;
        /* pub-sub 的默认 publish 恒定 BestEffort, 不做"大包自动升级 Reliable"。
         *
         * 历史背景: 曾按 payload 尺寸自动切到 Reliable+CRC32C 以压低大包丢包率,
         * 因为两个原因撤销。端点分离之后其中一个已经解决, 另一个仍然成立:
         *
         * 1) [已解决] ACK 收不到。原先收发共用同一条入组的组播 socket, 发布端
         *    自己的分片全部回绕到自己的接收队列(1 MB = 713 片), 订阅端的 ACK
         *    排在它们之后, 首轮窗口根本轮不到 —— 实测吞吐塌到 1 msg/s 而丢包率
         *    仍是 0.00%。现在 publisher_ 是 SendOnly(不入组, 无回绕) 且 ACK 走
         *    独立的 ack_rx_ 端口, 确认能正常到达, 见 publish_blocking()。
         *
         * 2) [仍然成立] 组播下 ACK 语义不完整。N 个订阅者时, chunk_send_ex 收到
         *    任意一个匹配 ACK 即判定 DeliveredAcked, 无法表达"谁收到了、谁没
         *    收到"。这需要 RTPS 的 Reader/Writer 配对与逐 Reader 确认状态。
         *
         * 因此默认路径仍是 BestEffort: 需要确认的调用方显式用 publish_blocking(),
         * 并接受"至少一个订阅者确认"这一较弱的语义。
         *
         * 提升大包到达率的首选手段依然是扩大接收端 net.core.rmem_max —— 实测
         * 丢包是缓冲溢出型突发, 64 MB 缓冲下 1 MB payload 可跑满 110 MB/s 零丢包。 */
        options.delivery = SocketDeliveryMode::BestEffort;
        options.integrity = SocketIntegrityMode::None;
        const SocketSendReport report = chunk_send_ex(publisher_, response_data, options);
        if (!report.ok())
        {
            std::cerr << "\033[31m[" << topic_name_ << "PubInfo] Error publishing message: Failed to send"
                      << "\033[0m" << std::endl;
            return false;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "\033[31m[" << topic_name_ << "PubInfo] Error publishing message: " << e.what() << "\033[0m"
                  << std::endl;
        return false;
    }
    return true;
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool socket_pub_ipc::publish_blocking(std::shared_ptr<IpcMsgBase> msg, std::uint64_t tm)
{
    try
    {
        ipc::buffer response_data(std::move(msg->serialize()));
        SocketSendOptions options;
        options.delivery = SocketDeliveryMode::Reliable;
        options.integrity = SocketIntegrityMode::CRC32C;
        options.ack_timeout_ms = tm;
        /* 端点分离让这条路径从"实际不可用"变成可用: ACK 走 ack_rx_, 不再被自己
         * 的数据分片挤掉。ack_rx_ 为空(ACK 端口没连上)时退化为旧行为。
         *
         * 组播下的语义边界: N 个订阅者时, 收到任意一个匹配 ACK 即判定成功,
         * 无法表达"谁收到了、谁没收到"。要精确到每个订阅者, 需要 RTPS 的
         * Reader/Writer 配对与逐 Reader 的确认状态, 不在本次范围内。 */
        options.ack_node = ack_rx_;
        const SocketSendReport report = chunk_send_ex(publisher_, response_data, options);
        if (!report.ok())
        {
            std::cerr << "\033[31m[" << topic_name_ << "PubInfo] Reliable publish failed with status "
                      << static_cast<int>(report.status) << "\033[0m" << std::endl;
            return false;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "\033[31m[" << topic_name_ << "PubInfo] Error publishing message: " << e.what() << "\033[0m"
                  << std::endl;
        return false;
    }
    return true;
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool socket_pub_ipc::publish_for_sniffer(std::shared_ptr<IpcMsgBase> msg)
{
    // Always use the standard UDP path — sniffers depend on UDP data.
    try
    {
        ipc::buffer response_data(std::move(msg->serialize()));
        SocketSendOptions options;
        options.delivery = SocketDeliveryMode::BestEffort;
        options.integrity = SocketIntegrityMode::None;
        const SocketSendReport report = chunk_send_ex(publisher_, response_data, options);
        if (!report.ok())
        {
            std::cerr << "\033[31m[" << topic_name_ << "PubInfo] Error publishing message (sniffer): Failed to send"
                      << "\033[0m" << std::endl;
            return false;
        }
    }
    catch (const std::exception& e)
    {        std::cerr << "\033[31m[" << topic_name_ << "PubInfo] Error publishing message (sniffer): " << e.what()
                  << "\033[0m" << std::endl;
        return false;
    }
    return true;
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
/* 预构造段发布(见 pub_ipc_base.h): 段由调用方按自己的 schema 写好, 原样当作 UDP 载荷送出。
 *
 * 分帧完全复用既有腿 —— 与 publish_best_effort 的 UDP 分支逐行同构(同一套 1428+12 页尾、
 * 同一套 BestEffort/None), 所以 wire 格式没变, 变的只是"载荷是平坦段还是 TLV"。接收侧
 * 认得出它: 段首 magic 的分流闸在 data_rev.cc(socket 侧的 T1)。
 *
 * 三道门与 SHM 腿同源: 开关 / 段头自证 / 段头 msg_id == 本话题模板的 msg_id。 */
bool socket_pub_ipc::publish_prebuilt_segment(const void* seg, std::size_t len)
{
    if (!dzIPC::IsDzFlatEnabled() || seg == nullptr || !dzflat::looks_like_dzflat(seg, len))
    {
        return false;
    }
    dzflat::SegHeader h{};
    std::memcpy(&h, seg, sizeof(h));
    if (h.msg_id != template_msg_id())
    {
        return false;
    }
    /* nodelet 拓扑: 同进程订阅者是从**对象队列**取消息的(见 publish_best_effort 的头注释),
     * 段路径在这条拓扑下不可达 —— 没有对象可投。返回 false 让调用方回退 TLV, 由那条路去
     * 做本地 fanout(它已经处理好了 K=3 稳定性与同/跨进程判定, 这里不重复那套逻辑)。 */
    if (dzIPC::IsNodeletEnabled()
        && !LocalPubSubRegistry::instance()
                .subscriber_snapshot(ChannelKey{topic_name_, domain_id_, h.msg_id, ChannelKind::SocketPubSub})
                .empty())
    {
        return false;
    }

    /* UDP 的 wire 是"每 1428 字节数据后跟 12 字节页尾", 所以段在 wire 上**不连续** ——
     * 接收端正是按这个格式去帧还原成连续段的(data_rev.cc 的 de_frame_dzflat), 而
     * chunk_send_ex 要求载荷自带页尾(它只就地改写 now_page 字段)。所以这里必须先铺帧:
     * 这一跳拷贝是 UDP 平坦段的固有成本(TLV 路径的那一份由 serialize() 付)。 */
    constexpr std::size_t kDataPerPage = ipc::wire_packet_size - 12;   /* == IPC_MSG_MAX_SIZE */
    constexpr std::size_t kTailSize = 12;         /* == TAIL_SIZE */
    /* 接收端 valid_chunk_meta 的上界(data_rev.cc 的 MAX_RECV_TOTAL_SIZE), 超出必被丢。 */
    constexpr std::size_t kMaxWireBytes = 64 * 1'024 * 1'024;
    const std::size_t total = static_cast<std::size_t>(h.total_size);
    /* 页数算法必须与 IpcMsgBase::correct_total_size 同式(整数除 + 1), 与接收端的
     * 上下界也要对得上 —— 差一页会让整条消息进不来(place_page 逐片比对 page_cnt)。 */
    const std::size_t pages = total / kDataPerPage + 1;
    const std::size_t wire_size = total + pages * kTailSize;
    if (wire_size > kMaxWireBytes)
    {
        return false;
    }
    std::vector<std::uint8_t> wire(wire_size, 0);
    const auto* src = static_cast<const std::uint8_t*>(seg);
    std::size_t copied = 0;
    std::size_t at = 0;
    for (std::size_t page = 1; page <= pages; ++page)
    {
        const std::size_t n = std::min(kDataPerPage, total - copied);
        std::memcpy(wire.data() + at, src + copied, n);
        copied += n;
        at += n;
        /* 页尾: 大端, 与 IpcMsgBase::add_tail_msg 逐字节同序
         * (page_cnt / now_page / total_size / msg_id)。now_page 随后会被 chunk_send_ex
         * 就地纠正成 [1..N], 这里写对是为了让本次载荷自洽(valid_chunk_meta 先看前两项)。 */
        /* 页尾的 total_size 是**分帧流总长**(含全部页尾), 不是段长 —— 接收端按它分配
         * 组装缓冲, 再从中去帧还原段(见 correct_total_size 与 de_frame_dzflat 的配套)。
         * 写错这一项的症状是整条消息静默进不来。 */
        const auto wire_total = static_cast<std::uint32_t>(wire_size);
        auto* t = wire.data() + at;
        t[0] = static_cast<std::uint8_t>(pages >> 8);
        t[1] = static_cast<std::uint8_t>(pages & 0xFF);
        t[2] = static_cast<std::uint8_t>(page >> 8);
        t[3] = static_cast<std::uint8_t>(page & 0xFF);
        t[4] = static_cast<std::uint8_t>(wire_total >> 24);
        t[5] = static_cast<std::uint8_t>((wire_total >> 16) & 0xFF);
        t[6] = static_cast<std::uint8_t>((wire_total >> 8) & 0xFF);
        t[7] = static_cast<std::uint8_t>(wire_total & 0xFF);
        t[8] = static_cast<std::uint8_t>(h.msg_id >> 24);
        t[9] = static_cast<std::uint8_t>((h.msg_id >> 16) & 0xFF);
        t[10] = static_cast<std::uint8_t>((h.msg_id >> 8) & 0xFF);
        t[11] = static_cast<std::uint8_t>(h.msg_id & 0xFF);
        at += kTailSize;
    }
    if (copied != total || at != wire_size)
    {
        return false;   /* 铺帧算术自洽性 —— 不成立就宁可不发 */
    }
    try
    {
        /* 非拥有 view, 但 chunk_send_ex 会**就地改写**页尾的 now_page(见其注释),
         * 所以缓冲区必须可写、且至少活到本函数返回 —— wire 是本函数的栈上对象。 */
        ipc::buffer payload(wire.data(), wire.size());
        SocketSendOptions options;
        options.delivery = SocketDeliveryMode::BestEffort;
        options.integrity = SocketIntegrityMode::None;
        const SocketSendReport report = chunk_send_ex(publisher_, payload, options);
        if (!report.ok())
        {
            return false;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "\033[31m[" << topic_name_ << "PubInfo] Error publishing prebuilt segment: " << e.what()
                  << "\033[0m" << std::endl;
        return false;
    }
    /* 失败不在这里计数: 调用方随后那次 TLV publish() 会记一次回退(与 SHM 腿同一约定)。 */
    dzIPC::detail::NoteDzFlatPublish(true);
    return true;
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
socket_sub_ipc::socket_sub_ipc(const std::shared_ptr<TopicData>& msg, const std::string& topic_name, size_t domain_id,
                               const size_t queue_size, bool verbose, bool enable_thread_qos, int cpu_id,
                               int thread_priority)
    : sub_ipc_base(msg, topic_name, domain_id, queue_size, verbose)
    , topic_name_(topic_name)
    , domain_id_(domain_id)
    , verbose_(verbose)
    , thread_options_(dzIPC::ThreadDispatch::make_realtime_options(enable_thread_qos, cpu_id, thread_priority))
{
    this->topic_msg_.reset(msg->clone());
    this->port_hash_ = dzIPC::common::socket_scope_port(topic_name_, domain_id_, dzIPC::common::ScopeKind::PubSub);
    this->ipaddr_ = dzIPC::common::socket_scope_address(topic_name_, domain_id_, dzIPC::common::ScopeKind::PubSub);
    this->msg_queue_ = std::make_shared<CircularQueue<IpcMsgBase>>(queue_size);
    /* 视图队列与物化队列同容量: 两条队列各自承接一种 wire, 单条上的压力不会超过总入流。 */
    this->view_queue_ = std::make_shared<CircularQueue<Sample>>(queue_size);
    this->msg_id_ = topic_msg_->topic()->msg_id();
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
socket_sub_ipc::~socket_sub_ipc()
{
    // Deregister BEFORE stopping threads so fast-path publisher snapshots
    // can no longer include this queue while we shut down.
    {
        std::lock_guard<std::mutex> lock(topic_msg_mtx_);
        if (local_registered_)
        {
            ChannelKey key{topic_name_, domain_id_, msg_id_, ChannelKind::SocketPubSub};
            LocalPubSubRegistry::instance().unregister_subscriber(key, msg_queue_);
            local_registered_ = false;
        }
    }

    /* 析构 1-6 步（方案 §5）：
     *   1) 上面的 registry 注销（持 topic_msg_mtx_）
     *   2) active=false + 3) 注销 worker route（remove_route 内部：摘 wait 项 + 唤醒 /
     *      cancel_wait / 等 in_flight 归零 2000ms / 归还 owner）；兼容模式为 cancel_wait + join
     *   4/5) 关 subscriber_ 与 ack_tx_ —— fd/句柄只在 wait 项删除且 in-flight 清零**之后**才关
     *   6) 释放 state（teardown_receive_path 已 reset） */
    teardown_receive_path();
    if (subscriber_)
        subscriber_->close();
    if (ack_tx_)
        ack_tx_->close();
    exit_flag.store(true, std::memory_order_release);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void socket_sub_ipc::reset_message(const std::shared_ptr<TopicData>& msg)
{
    if (!msg)
    {
        return;
    }
    std::shared_ptr<TopicData> new_msg;
    new_msg.reset(msg->clone());
    const uint32_t new_msg_id = new_msg->topic()->msg_id();

    {
        std::lock_guard<std::mutex> lock(topic_msg_mtx_);
        topic_msg_ = std::move(new_msg);

        // If already registered (InitChannel completed) and msg_id changed,
        // re-register under the new key so publishers using the new msg_id
        // can find us via the fast path.
        if (local_registered_ && msg_id_ != new_msg_id)
        {
            auto& reg = LocalPubSubRegistry::instance();
            ChannelKey old_key{topic_name_, domain_id_, msg_id_, ChannelKind::SocketPubSub};
            ChannelKey new_key{topic_name_, domain_id_, new_msg_id, ChannelKind::SocketPubSub};
            reg.unregister_subscriber(old_key, msg_queue_);
            reg.register_subscriber(new_key, msg_queue_);
        }
        msg_id_ = new_msg_id;
    }
    /* worker 模式：worker 与兼容线程都从 state 快照取模板与分流期望值（worker 不持裸本对象）。
     * 锁序（P0 §3.2）：topic_msg_mtx_ →（取快照后释放 receive_state_mtx_）→ state->mtx；
     * 两把锁**绝不**同时持有。 */
    const std::shared_ptr<socket_sub_receive_state> state = current_receive_state();
    if (state)
    {
        std::shared_ptr<TopicData> snapshot;
        {
            std::lock_guard<std::mutex> lock(topic_msg_mtx_);
            snapshot = topic_msg_;
        }
        std::lock_guard<std::mutex> state_lock(state->mtx);
        state->msg_template.reset(snapshot ? snapshot->clone() : nullptr);
        state->exp_id = msg_id_;
        state->exp_hash = (snapshot && snapshot->topic()) ? snapshot->topic()->dzflat_schema_hash() : 0u;
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void socket_sub_ipc::InitChannel(std::string extra_info)
{
    /* D4 / 方案 §5：重复 InitChannel 先按同一顺序回收上一次的接收路径（幂等），再重新置
     * running=true —— teardown_receive_path() 会把它置假，漏了这一步新起的收包线程会立刻看到
     * running==false 直接退出（订阅端静默收不到任何消息）。 */
    teardown_receive_path();
    running.store(true, std::memory_order_release);
    try
    {
        subscriber_ = std::make_shared<ipc::socket::UDPNode>(this->topic_name_.c_str(), this->ipaddr_.c_str(),
                                                             this->port_hash_, ipc::socket::NodeRole::RecvOnly);
        subscriber_->set_scope(dzIPC::common::channel_scope_token(topic_name_, domain_id_, dzIPC::common::ScopeKind::PubSub));
        /* ACK 发送通道 (端点分离): 发到发布端 ack_rx_ 监听的端口。
         * SendOnly 不入组, 所以自己发的 ACK 不会回绕进 subscriber_。 */
        ack_tx_ = std::make_shared<ipc::socket::UDPNode>(
            this->topic_name_.c_str(), this->ipaddr_.c_str(),
            static_cast<uint16_t>(this->port_hash_ + dzIPC::common::kUdpPortOffsetAckData),
            ipc::socket::NodeRole::SendOnly);
        ack_tx_->set_scope(dzIPC::common::channel_scope_token(topic_name_, domain_id_, dzIPC::common::ScopeKind::PubSub));
        if (verbose_)
            std::cerr << "\033[32m[" << topic_name_ << "SubInfo] Subscriber initialized on IP: " << this->ipaddr_
                      << " Port: " << this->port_hash_ << " for topic: " << topic_name_ << "\033[0m" << std::endl;
        /* D-22：有界重试 + 可识别诊断（取代原先的 `while (!connect()) sleep(1s)` 无界循环）。
         * ⛔ 连不上就**显式失败并提前返回**：不注册 info_pool 条目、不建收包路径 ——
         * 让"某几路没建起来"在注册台账/逐 route 合法收包数上直接可见，而不是整片挂住。 */
        if (!connect_with_bounded_retry(subscriber_.get(), running, topic_name_, "SubInfo", "subscriber", ipaddr_,
                                        port_hash_))
        {
            return;
        }
        /* ACK 通道连不上不影响收数据, 只是无法回确认 —— BestEffort 下本就不需要。 */
        if (!ack_tx_->connect())
        {
            if (verbose_)
            {
                std::cerr << "\033[33m[" << topic_name_
                          << "SubInfo] ACK channel unavailable; acknowledgements will fall back to the "
                             "data socket\033[0m"
                          << std::endl;
            }
            ack_tx_.reset();
        }
        std::shared_ptr<TopicData> topic_template;
        {
            std::lock_guard<std::mutex> lock(topic_msg_mtx_);
            topic_template = topic_msg_;
        }
        std::string topic_type_name = (topic_template && topic_template->topic())
                                          ? dzIPC::info_pool::demangle(typeid(*topic_template->topic()).name())
                                          : std::string{};
        topic_type_name = extract_last_segment(topic_type_name);
        pool_reg_.rebind({dzIPC::info_pool::EntryKind::SocketSub, topic_name_, topic_type_name, "socket",
                          static_cast<uint64_t>(domain_id_), extra_info});
        /* ---- 接收路径：worker 优先；任何非 ok / 后端不可用 / 开关 / nodelet / fork 闸
         *      ⇒ start_receive_path() 已打显式原因，回退兼容 subscribe_thread_。---- */
        auto state = std::make_shared<socket_sub_receive_state>();
        state->route_key = topic_name_ + "#" + std::to_string(domain_id_);
        state->domain_id = static_cast<std::uint32_t>(domain_id_);
        state->generation = receive_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
        state->subscriber = subscriber_;
        state->ack_tx = ack_tx_;
        state->msg_queue = msg_queue_;
        state->view_queue = view_queue_;
        {
            std::lock_guard<std::mutex> lock(topic_msg_mtx_);
            if (topic_msg_)
            {
                state->msg_template.reset(topic_msg_->clone());
            }
            state->exp_id = msg_id_;
            state->exp_hash = (topic_msg_ && topic_msg_->topic()) ? topic_msg_->topic()->dzflat_schema_hash() : 0u;
        }
        set_receive_state(state);

        if (!start_receive_path())
        {
            /* 兼容回退：与 worker 路径**共用**同一个 state 与同一条收包函数，只是不带可读判据
             * （gate_on_readiness=false），因此仍是"阻塞在 50ms 组包超时"，不会忙轮询。 */
            subscribe_thread_ = new std::thread(
                [this, state]()
                {
                    if (!sub_try_claim_recv(*state, threepools::RecvOwner::compat_thread))
                    {
                        /* P1（§4.2）：claim 失败必须留痕（否则"无人消费"没有任何诊断面）。 */
                        ipc::error("[socket_sub] compat receive thread: claim failed for route '%s' (owner=%d); "
                                   "not double-receiving\n",
                                   state->route_key.c_str(), static_cast<int>(sub_recv_owner(*state)));
                        return;
                    }
                    while (running.load(std::memory_order_acquire))
                    {
                        (void)socket_sub_receive_once(state, false);
                    }
                    sub_release_recv(*state);
                });
            dzIPC::ThreadDispatch::apply_thread_options(subscribe_thread_, thread_options_, verbose_,
                                                        topic_name_ + "_SocketSubReceiveThread");
        }

        // Register for intra-process fast-path delivery (once only).
        // C7 裁决：worker 模式下**不**注册进程内快路径队列（nodelet 启用时本就走兼容收包线程）。
        if (!worker_mode_)
        {
            std::lock_guard<std::mutex> lock(topic_msg_mtx_);
            if (!local_registered_)
            {
                ChannelKey key{topic_name_, domain_id_, msg_id_, ChannelKind::SocketPubSub};
                LocalPubSubRegistry::instance().register_subscriber(key, msg_queue_);
                local_registered_ = true;
            }
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "\033[31m[" << topic_name_ << "SubInfo] Error initializing channel: " << e.what() << "\033[0m"
                  << std::endl;
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
/* 接收路径注册：成功返回 true（走共享层固定 socket worker）。返回 false 表示**必须**走兼容
 * subscribe_thread_，且已在 stderr 打了显式原因（绝不静默、绝不忙轮询降级）。
 * 前置条件：InitChannel 已建好 receive_state_（含模板/期望值/队列/节点强引用）。 */
bool socket_sub_ipc::start_receive_path()
{
    const auto fallback = [this](const char* why) {
        std::cerr << "\033[33m[" << topic_name_ << "SubInfo] socket wait-set unusable (" << why
                  << "); keeping per-subscription receive thread\033[0m" << std::endl;
        return false;
    };

    if (sub_compat_forced())
    {
        return fallback("DZIPC_SOCKET_COMPAT_THREAD=1");
    }
    /* C7 裁决：nodelet 与固定 worker 二选一 —— nodelet 启用时保留兼容接收线程。 */
    if (dzIPC::IsNodeletEnabled())
    {
        return fallback("nodelet enabled (C7: nodelet 与固定 worker 二选一)");
    }
    if (!sub_pool_allowed_in_this_process())
    {
        return fallback("forked child: recv pool owner pid mismatch");
    }
    if (!threepools::SocketRecvWorkerPool::backend_available())
    {
        return fallback("SocketWaitSet backend unavailable");
    }
    /* P0：取**本地快照**后再使用（成员指针可能被 teardown 替换）。 */
    const std::shared_ptr<socket_sub_receive_state> state = current_receive_state();
    if (!state)
    {
        return fallback("receive state missing");
    }

    auto route = std::make_shared<socket_sub_receive_route>(state);
    if (!route->wait_token().valid())
    {
        return fallback("subscriber channel is not waitable (invalid_token)");
    }
    auto& pool = threepools::SocketRecvWorkerPool::instance();
    if (!pool.running())
    {
        (void)pool.start();   // 一次性；已被别的 service/模块启动过时返回 false
    }
    if (!pool.running())
    {
        return fallback("SocketRecvWorkerPool::start() failed (thread creation?)");
    }
    const threepools::RecvRegisterStatus status = pool.add_route(route);
    if (status != threepools::RecvRegisterStatus::ok)
    {
        return fallback(sub_status_reason(status));
    }
    receive_route_ = route;
    worker_mode_ = true;
    if (verbose_)
    {
        std::cerr << "\033[32m[" << topic_name_ << "SubInfo] subscribe receive on shared socket worker "
                  << threepools::SocketRecvWorkerPool::worker_for(state->route_key.c_str(), state->domain_id,
                                                                  pool.worker_count())
                  << " (generation " << state->generation << ", workers " << pool.worker_count() << ")\033[0m"
                  << std::endl;
    }
    return true;
}

/* 接收路径注销（方案 §5 析构 1-6 步里的 2/3 步 + 6 步的状态释放）。
 * worker 模式：remove_route 同步完成契约 §4.4 的 1-6 步（摘表 → wait_set.remove 唤醒 → stop_and_wake
 *   = cancel_wait 打断在途组包 → 等 worker 侧 in-flight 归零 → wait_quiescent 等本模块 in-flight
 *   → release_recv 归还收包独占）；兼容模式：cancel_wait 打断在途 receive + join 收包线程。
 * fd/句柄在 wait 项删除且 in-flight 清零**之后**才由调用方关闭（本函数之后才 close 节点）。 */
void socket_sub_ipc::teardown_receive_path()
{
    /* P0：先取快照（成员指针可能被并发的 set/clear 替换），再操作 state 内部字段。 */
    const std::shared_ptr<socket_sub_receive_state> state = current_receive_state();
    if (state)
    {
        /* ② active=false（方案 §5 的 route stopping）：拒绝新的 recv_once。 */
        state->stopping.store(true, std::memory_order_release);
    }
    /* P1（§5 停机顺序）：**先禁止新工作**（running=false 与 stopping 同处）**再唤醒**当前
     * 等待，消除 cancel_wait 与 running=false 之间的紧循环窗口。 */
    running.store(false, std::memory_order_release);

    if (worker_mode_ && receive_route_)
    {
        threepools::SocketRecvWorkerPool::instance().remove_route(receive_route_.get());
    }
    else if (subscriber_ && udp_node_waitable(subscriber_))
    {
        /* 兼容模式：打断阻塞中的 receive(kSocketSubRecvTimeoutMs)，不必等满 50ms。
         * 只在仍可等待时调，避免对已关闭（fd 号可能被复用）的节点做 shutdown。 */
        udp_node_cancel_wait(subscriber_);
    }

    /* 兼容收包线程 join（等其当前一次收包/入队结束；running 已在上面置假）。 */
    if (subscribe_thread_ != nullptr)
    {
        if (subscribe_thread_->joinable())
        {
            subscribe_thread_->join();
        }
        delete subscribe_thread_;
        subscribe_thread_ = nullptr;
    }

    if (state)
    {
        sub_release_recv(*state);   // 幂等：worker 路径已由 remove_route 第 6 步归还
    }
    receive_route_.reset();
    clear_receive_state();
    worker_mode_ = false;
}

/* ---- P0：receive_state_ 的三个同步访问口（语义同 socket_ser_ipc）---- */
std::shared_ptr<socket_sub_receive_state> socket_sub_ipc::current_receive_state() const
{
    std::lock_guard<std::mutex> lock(receive_state_mtx_);
    return receive_state_;
}

void socket_sub_ipc::set_receive_state(const std::shared_ptr<socket_sub_receive_state>& state)
{
    std::lock_guard<std::mutex> lock(receive_state_mtx_);
    receive_state_ = state;
}

void socket_sub_ipc::clear_receive_state() noexcept
{
    std::lock_guard<std::mutex> lock(receive_state_mtx_);
    receive_state_.reset();
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
/* ---- 视图路径: 只服务借样的 DZFlat 段(见头注释与 subscribe 循环的分流) ----
 * 与 shm 侧逐行同构(shm_pub_sub_ipc.cc:806-846) —— 同一套语义, 不另立第二套。 */
void socket_sub_ipc::get(Sample& out)
{
    std::shared_ptr<Sample> s;
    view_queue_->pop(s);   /* 阻塞直到有 Sample; TLV-only 话题请用 get_clone, 见头注释 */
    if (s)
    {
        out = std::move(*s);
    }
}

bool socket_sub_ipc::try_get(Sample& out)
{
    std::shared_ptr<Sample> s;
    if (!view_queue_->try_pop(s))
    {
        return false;
    }
    if (!s)
    {
        return false;
    }
    out = std::move(*s);
    return true;
}

bool socket_sub_ipc::get(Sample& out, std::uint64_t tm_ms)
{
    /* CircularQueue::pop 本来就支持超时(见其 tm 参数) —— 这里只是把它接上。
     * 恒发 TLV 的话题上视图队列永远是空的, 不带超时的 get() 是注定挂死而非等待。 */
    std::shared_ptr<Sample> s;
    if (!view_queue_->pop(s, tm_ms))
    {
        return false;   // 超时
    }
    if (!s)
    {
        return false;
    }
    out = std::move(*s);
    return true;
}

void socket_sub_ipc::get_clone(std::shared_ptr<TopicData>& msg)
{
    std::shared_ptr<IpcMsgBase> ipc_msg;
    msg_queue_->pop(ipc_msg);
    msg->update(ipc_msg);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool socket_sub_ipc::try_get_clone(std::shared_ptr<TopicData>& msg)
{
    std::shared_ptr<IpcMsgBase> ipc_msg;
    if (msg_queue_->try_pop(ipc_msg))
    {
        msg->update(ipc_msg);
        return true;
    }
    else
    {
        return false;
    }
}

}   // namespace socket
}   // namespace dzIPC

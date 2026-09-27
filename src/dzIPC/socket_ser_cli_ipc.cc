#include "dzIPC/socket_ser_cli_ipc.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>   // ::getpid()：fork 防死锁闸（与 auto_ser_cli_ipc.cc 同一取法）
#include <atomic>
#include <condition_variable>
#include <deque>
#include <iostream>
#include <memory>
#include <thread>
#include <typeinfo>
#include <utility>
#include "dzIPC/common/data_rev.h"
#include "dzIPC/common/hash.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/threepools/socket_recv_worker.h"   // 阶段 5 共享层（captain 裁定 A：worker 归共享层）
#include "ipc_msg/ipc_msg_base/udp_id_init_msg.hpp"
#include "libipc/semaphore.h"
#define ListenerWaitTime 1'000   // 1 second
#define ServerRevTime 200        // 200 ms, allow large fragmented UDP payloads to complete

namespace dzIPC {
namespace socket {
enum class State {
    RunHS,
    StopHS
};
using namespace ipc;

/* ==================== 阶段 5：服务端请求接收接入共享层 socket worker ====================
 *
 * 分工（captain 裁定 A）：通用 socket 收包 worker（SocketRecvRouteSource / SocketRecvWorker /
 * SocketRecvWorkerPool）由共享层 t2 交付并冻结（include/dzIPC/threepools/socket_recv_worker.h）。
 * 本模块**只写 route 适配器 + 注册/回退/停机协议**：⛔ 不复制 worker、不新增等待原语、
 * 不出现平台宏、不 include 另一个 socket 模块。
 *
 * 线程模型（worker 模式）：
 *   共享层 worker 线程 ── recv_once() ──> chunk_rev_server（组包到**完整请求边界**，可跨分片）
 *                                     └─ 入本 service 的**有界**请求队列
 *   本 service 处理线程（response_thread_）── 出队 ──> 用户 callback
 *                                     └─ response 序列化 + chunk_send
 * ⛔ 用户 callback 与响应发送**不得**进收包 worker（需求 §1.2；recv_worker.h:214-220）。
 *
 * 有界队列的**满载策略**（明确，不留隐式无界堆积）：容量 kSerRequestQueueCap，满时丢**最旧**，
 * 丢一次计一次 queue_drops。为什么不阻塞入队：worker 是**共享**的，阻塞它等于让同 worker 的
 * 其它 route 一起等；客户端本来就是同步 rev_tm 等待，被丢的请求由它自己超时（与改造前
 * "收包线程停了之后的请求不会被应答"是同一种失败面）。
 *
 * 读路径（勘误 E1，硬约束）：wait_handle() 给的 Linux fd 是**阻塞**的；从 wait-set 拿到就绪后
 * 一律走 chunk_rev_server（内部 MSG_DONTWAIT 读第一片、按 ServerRevTime 组包），
 * ⛔ 绝不裸 recvfrom、⛔ 绝不自己置 O_NONBLOCK。
 */

/* 注销时"等在途 recv_once 归零"的上界（与方案 §5 的 kSocketQuiesceTimeout 同值）。 */
constexpr int64_t kSerQuiesceTimeoutMs = 2000;
/* 处理线程等待队列的切片：仅作停机兜底唤醒（正常由 queue_cv 唤醒）。 */
constexpr int64_t kSerQueueWaitMs = 100;

/* service shared state：worker 与处理路径的共同事实来源。
 * 收包 worker **只**通过下面的 socket_ser_request_route 适配器接触本结构，
 * 因此 worker 不持裸 socket_ser_ipc 指针（注销后不再回调已析构对象的前提）。 */
struct socket_ser_receive_state
{
    /* 稳定 service key：固定归属哈希与日志都用它，生成本对象后不再变化。 */
    std::string route_key;
    std::uint32_t domain_id{0};
    /* 数据面代际：restart 建新 generation（新 state + 新适配器），旧代先完全注销。 */
    std::uint32_t generation{0};

    /* 只注册请求数据通道 ipc_r_ptr_；ack_r_tx_ 是**发送端点**、握手 socket 与数据面分离，
     * 都不入 worker（方案 §3）。强引用 ⇒ worker 持 state 期间节点一直活着。 */
    std::shared_ptr<ipc::socket::UDPNode> request_node;
    std::shared_ptr<ipc::socket::UDPNode> request_ack_tx;
    /* 响应通道：只由处理路径使用（worker 不碰）。 */
    std::shared_ptr<ipc::socket::UDPNode> response_node;

    /* 模板/回调快照：worker 与处理路径都从这里取。 */
    mutable std::mutex mtx;
    std::shared_ptr<ServiceData> msg_template;
    std::function<void(std::shared_ptr<ServiceData>&)> callback;

    /* 单 route 单消费者（契约 §4.6）：宿主用 atomic<RecvOwner> 三行实现。 */
    std::atomic<threepools::RecvOwner> owner{threepools::RecvOwner::none};

    /* route 生命周期：stopping 拒绝新的 recv_once；in_flight 供 wait_quiescent 等。 */
    std::atomic<bool> stopping{false};
    std::atomic<std::size_t> recv_in_flight{0};
    std::mutex quiesce_mtx;
    std::condition_variable quiesce_cv;

    /* 有界请求队列：worker 入队 → 处理线程出队。 */
    static constexpr std::size_t kQueueCap = 64;
    std::mutex queue_mtx;
    std::condition_variable queue_cv;
    std::deque<std::shared_ptr<ServiceData>> queue;
    std::atomic<std::uint64_t> queue_drops{0};
    std::atomic<std::uint64_t> requests_received{0};
    std::atomic<std::uint64_t> recv_errors{0};
};

namespace {

/* fork 防死锁闸（captain 裁定 + socket_recv_worker.h 文件头）：池是进程级单例且 start()
 * 一次性；fork 之后子进程继承"已 start"却没有工作线程，子进程里 add_route 会走按需拉起
 * （要取池内部锁并创建线程），而被 fork 打断的父进程可能正持这些锁 ⇒ 子进程里死锁。
 * 因此 owner pid != 当前 pid 时**不调用池**，改用兼容收包线程。
 * ⛔ 判断过程只取本文件的静态锁，**不触碰池内部锁**。 */
int32_t pool_owner_pid()
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

bool pool_allowed_in_this_process()
{
    return pool_owner_pid() == static_cast<int32_t>(::getpid());
}

/* DZIPC_SOCKET_COMPAT_THREAD=1 ⇒ 强制走兼容收包线程（与 t4 同名同义；进程内只读一次，
 * 避免"半程切换后端"）。DZIPC_SOCKET_RECV_WORKERS 由共享池自己读一次，这里不碰。 */
bool socket_recv_compat_forced()
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

const char* register_status_reason(threepools::RecvRegisterStatus s) noexcept
{
    using threepools::RecvRegisterStatus;
    switch (s)
    {
    case RecvRegisterStatus::backend_unavailable:
        return "backend_unavailable (SocketWaitSet::backend_available()==false)";
    case RecvRegisterStatus::duplicate:
        return "duplicate (route already registered on this worker)";
    case RecvRegisterStatus::busy:
        return "busy (another owner is receiving this route)";
    case RecvRegisterStatus::stopped:
        return "stopped (pool not started / already stopped)";
    case RecvRegisterStatus::invalid_token:
        return "invalid_token (request channel is not waitable)";
    case RecvRegisterStatus::invalid_route:
        return "invalid_route";
    case RecvRegisterStatus::wait_set_full:
        return "wait_set_full (channel capacity exceeded)";
    case RecvRegisterStatus::ok:
        break;
    }
    return "ok";
}

/* 一次完整请求的字节量纲：DZFlat 视图路径直接取段长（srv_data.h 的 request_sample()->size()，
 * 零成本）；TLV owning 路径不重新序列化（那是热路径上的无谓开销），返回 1 表示"取到一次完整请求"。
 * 契约只要求 recv_once 返回 0 = 无数据/断开、非 0 = 取到东西；两项预算里 1 MiB 只是上界提示，
 * 真正的每轮让出由消息数(32)与处理时间(200us)决定。该口径已登记进"需回写方案文档条目"。 */
std::size_t received_request_bytes(const std::shared_ptr<ServiceData>& request)
{
    if (request && request->request_is_view() && request->request_sample())
    {
        return request->request_sample()->size();
    }
    return 1;
}

void enqueue_request(const std::shared_ptr<socket_ser_receive_state>& state, std::shared_ptr<ServiceData> request)
{
    bool dropped = false;
    {
        std::lock_guard<std::mutex> lock(state->queue_mtx);
        if (state->queue.size() >= socket_ser_receive_state::kQueueCap)
        {
            state->queue.pop_front();   // 满载：丢最旧，绝不阻塞共享 worker
            dropped = true;
        }
        state->queue.push_back(std::move(request));
    }
    if (dropped)
    {
        state->queue_drops.fetch_add(1, std::memory_order_relaxed);
    }
    state->queue_cv.notify_one();
}

/* SocketRecvRouteSource 适配器：socket_ser_ipc 服务端的**请求数据通道**。
 * recv_once() 只做"取一次到完整请求边界 + in-flight 记账 + 入队"，⛔ 不含用户 callback。 */
class socket_ser_request_route final : public threepools::SocketRecvRouteSource
{
public:
    explicit socket_ser_request_route(std::shared_ptr<socket_ser_receive_state> state)
        : state_(std::move(state))
    {}

    const char* route_name() const noexcept override { return state_->route_key.c_str(); }
    std::uint32_t domain_id() const noexcept override { return state_->domain_id; }

    /* owner = UDPNode*（宿主侧稳定身份），handle = udp_node_wait_handle（0 = 不可等待）。 */
    threepools::SocketWaitToken wait_token() const noexcept override
    {
        threepools::SocketWaitToken token;
        token.owner = state_->request_node.get();
        token.handle = udp_node_wait_handle(state_->request_node);   // nullptr 安全
        return token;
    }

    std::size_t recv_once() override
    {
        const std::shared_ptr<socket_ser_receive_state>& state = state_;
        if (state->stopping.load(std::memory_order_acquire))
        {
            return 0;
        }
        std::shared_ptr<ServiceData> request;
        {
            std::lock_guard<std::mutex> lock(state->mtx);
            if (state->msg_template)
            {
                request.reset(state->msg_template->clone());
            }
        }
        if (!request)
        {
            return 0;
        }

        state->recv_in_flight.fetch_add(1, std::memory_order_acq_rel);
        std::size_t bytes = 0;
        try
        {
            /* 一次调用 = 到**完整请求边界**（可跨多个分片；不得在组包中途切走）。
             * 预算只在它返回之后检查（共享 worker 侧负责）。 */
            if (chunk_rev_server(state->request_node, request, ServerRevTime, true, state->request_ack_tx))
            {
                bytes = received_request_bytes(request);
            }
        }
        catch (...)
        {
            /* 绝不把异常抛给共享 worker 线程（它会 std::terminate）。 */
            state->recv_errors.fetch_add(1, std::memory_order_relaxed);
        }
        state->recv_in_flight.fetch_sub(1, std::memory_order_acq_rel);
        state->quiesce_cv.notify_all();

        if (bytes == 0)
        {
            return 0;   // 超时/断开/组包失败
        }
        enqueue_request(state, std::move(request));
        state->requests_received.fetch_add(1, std::memory_order_relaxed);
        return bytes;
    }

    /* socket 侧没有 SHM 的 sequence 字可做廉价重检；事实来源是共享 worker 的
     * level-triggered wait(0) 全扫，因此保持默认语义（false）。 */
    bool has_pending() const noexcept override { return false; }

    threepools::RecvOwner recv_owner() const noexcept override
    {
        return state_->owner.load(std::memory_order_acquire);
    }

    bool try_claim_recv(threepools::RecvOwner who) noexcept override
    {
        if (who == threepools::RecvOwner::none)
        {
            return false;
        }
        threepools::RecvOwner expected = threepools::RecvOwner::none;
        return state_->owner.compare_exchange_strong(expected, who, std::memory_order_acq_rel);
    }

    /* 归还收包独占。worker 路径由 remove_route 第 6 步调用；兼容线程路径在退出前调用。
     * 允许 worker/compat_thread 任一 owner 归还到 none（契约 §4.6 两种路径都要归还）。 */
    void release_recv() noexcept override
    {
        auto expected = state_->owner.load(std::memory_order_acquire);
        while (expected != threepools::RecvOwner::none)
        {
            if (state_->owner.compare_exchange_weak(expected, threepools::RecvOwner::none, std::memory_order_acq_rel))
            {
                break;
            }
        }
    }

    /* 注销协议第 3 步：置 stopping（拒绝新 recv_once）+ cancel_wait 打断在途组包。
     * Linux 的 shutdown(SHUT_RD) 会让阻塞中的 receive/组包立刻返回空；幂等、nullptr 安全。 */
    void stop_and_wake() noexcept override
    {
        state_->stopping.store(true, std::memory_order_release);
        udp_node_cancel_wait(state_->request_node);
        state_->queue_cv.notify_all();
    }

    /* 注销协议第 5 步：等本模块 in-flight 记账归零（有界；超时只打诊断不阻塞注销）。 */
    void wait_quiescent() noexcept override
    {
        std::unique_lock<std::mutex> lock(state_->quiesce_mtx);
        state_->quiesce_cv.wait_for(lock, std::chrono::milliseconds{kSerQuiesceTimeoutMs}, [this] {
            return state_->recv_in_flight.load(std::memory_order_acquire) == 0;
        });
    }

private:
    std::shared_ptr<socket_ser_receive_state> state_;
};

}   // namespace

namespace {
/* 连接重试必须**可中断**。
 *
 * 旧写法是 `while (!node->connect()) { sleep(1s); }` —— 只看 connect() 的返回值,
 * 从不看 running。于是链路一直连不上时, 析构/切换路径的 join 会**永久阻塞**
 * (T2 §7 R4: 四处循环, 分别在本文件的数据面建链与两条握手线程里)。路径切换
 * 的清理步骤正是"关掉并等线程退出", 所以这一条不修就实现不了切换。
 *
 * 语义保持逐字不变: running 为真时行为与旧代码完全一致(失败即重试、每 1s 一条
 * stderr); 只有 running 变假时才提前返回 false。 */
bool connect_with_retry(ipc::socket::UDPNode* node, const std::atomic<bool>& running, const std::string& topic_name,
                        const char* who)
{
    if (node == nullptr)
    {
        return false;
    }
    while (!node->connect())
    {
        if (!running.load(std::memory_order_acquire))
        {
            return false;
        }
        std::cerr << "\033[31m[" << topic_name << who << "] Failed to connect,reconnect affter 1 second...\033[0m"
                  << std::endl;
        /* 分批睡眠: 一次睡满 1s 会让最坏 join 延迟多 1s, 与 T_stop_max 的预算不符。 */
        for (int i = 0; i < 10 && running.load(std::memory_order_acquire); ++i)
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }
    return true;
}
}   // namespace

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

socket_ser_ipc::socket_ser_ipc(const std::string& topic_name, const std::shared_ptr<ServiceData>& msg,
                               std::function<void(std::shared_ptr<ServiceData>&)> callback, size_t domain_id,
                               bool verbose, bool enable_thread_qos, int cpu_id, int thread_priority)
    : ser_ipc_base(topic_name, msg, callback, domain_id, verbose)
    , topic_name_(topic_name)
    , callback_(std::move(callback))
    , domain_id_(domain_id)
    , verbose_(verbose)
    , thread_options_(dzIPC::ThreadDispatch::make_realtime_options(enable_thread_qos, cpu_id, thread_priority))
{
    message_.reset(msg->clone());
    this->port_hash_ = dzIPC::common::udp_discovery_port_calculate(topic_name, domain_id_);
    this->ipaddr_ = dzIPC::common::udp_discovery_addr_calculate(topic_name);
    dzIPC::ThreadDispatch::apply_current_thread_options(thread_options_, verbose_, topic_name_ + "_SocketSerOwnerThread");
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void socket_ser_ipc::reset_message(const std::shared_ptr<ServiceData>& msg)
{
    if (!msg)
    {
        return;
    }
    std::shared_ptr<ServiceData> new_msg;
    new_msg.reset(msg->clone());
    std::lock_guard<std::mutex> lock(message_mtx_);
    message_ = std::move(new_msg);
    /* worker 模式：worker 从 state 的**独立克隆**取模板（它不能碰本对象的 mutex）。
     * 必须是独立对象 —— chunk_rev_server 把请求反序列化进传入的那个 ServiceData。 */
    if (receive_state_)
    {
        std::lock_guard<std::mutex> state_lock(receive_state_->mtx);
        receive_state_->msg_template.reset(message_->clone());
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void socket_ser_ipc::reset_callback(std::function<void(std::shared_ptr<ServiceData>&)> callback)
{
    std::lock_guard<std::mutex> lock(callback_mtx_);
    callback_ = std::move(callback);
    /* worker 模式：处理路径从 state 快照取 callback（worker 侧持 state ⇒ 不持裸本对象指针）。 */
    if (receive_state_)
    {
        std::lock_guard<std::mutex> state_lock(receive_state_->mtx);
        receive_state_->callback = callback_;
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

socket_ser_ipc::~socket_ser_ipc()
{
    running.store(false, std::memory_order_release);
    /* 顺序: 注销接收路径(摘 route + 取消 OS wait + join 收/处理线程) -> join 握手线程
     * -> 关通道。接收路径必须先于握手线程与关通道, 因为:
     *   · 兼容 response 线程读 ipc_r_ptr_, worker 线程读 state->request_node;
     *   · worker 侧可能仍持有 route 引用, 必须先由 remove_route 同步摘除;
     *   · 关通道前必须保证没有线程还会碰这些节点, 否则是 use-after-close。 */
    data_plane_running_.store(false, std::memory_order_release);
    teardown_receive_path();
    /* handshake 线程也必须 join，否则析构返回后该线程仍会读取本对象的成员
       (running / topic_name_ / verbose_ 等)，造成 use-after-free。Windows
       上的堆分配器更激进地复用内存，命中崩溃的概率远高于 Linux */
    if (handshake_thread_ != nullptr)
    {
        if (handshake_thread_->joinable())
        {
            handshake_thread_->join();
        }
        delete handshake_thread_;
        handshake_thread_ = nullptr;
    }
    close_data_plane();
    exit_flag.store(true, std::memory_order_release);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void socket_ser_ipc::InitChannel(std::string extra_info)
{
    try
    {
        if (!open_data_plane())
        {
            std::cerr << "\033[31m[" << topic_name_ << "SerInfo] Data plane failed to start; handshake only.\033[0m"
                      << std::endl;
            return;
        }
        std::shared_ptr<ServiceData> message_template;
        {
            std::lock_guard<std::mutex> lock(message_mtx_);
            message_template = message_;
        }
        std::string request_type_name =
            (message_template && message_template->request())
                ? dzIPC::info_pool::demangle(typeid(*message_template->request()).name())
                : std::string{};
        request_type_name = extract_last_segment(request_type_name);
        pool_reg_.rebind({dzIPC::info_pool::EntryKind::SocketServer, topic_name_, request_type_name, "socket",
                          static_cast<int32_t>(domain_id_), extra_info});
        /* 接收路径：优先接入共享层固定 socket worker；任何非 ok / 后端不可用 / 被开关或
         * fork 闸禁止 ⇒ start_receive_path() 已打显式原因，这里退回兼容 response 线程。 */
        const bool worker_mode = start_receive_path();
        /* ⛔ data_plane_running_ **最后**置位（裁定 ②）：此刻节点已建好、新 generation 已注册；
         * 在此之前处理路径与 send_request 都必须看到"数据面未就绪"。 */
        data_plane_running_.store(true, std::memory_order_release);
        response_thread_ = new std::thread(
            worker_mode ? &socket_ser_ipc::process_thread_func : &socket_ser_ipc::response_thread_func, this);
        dzIPC::ThreadDispatch::apply_thread_options(response_thread_, thread_options_, verbose_,
                                                    topic_name_ + "_SocketSerResponseThread");
        /* 握手线程只起一次。切换只换数据面, 握手通道是常驻的存活权威 (T2 §3 D6),
         * 重建它会让对端把一次切换误判成一次断连。 */
        if (handshake_thread_ == nullptr)
        {
            handshake_thread_ = new std::thread(&socket_ser_ipc::server_handshake, this);
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "\033[31m[" << topic_name_ << "SerInfo] Error initializing channel: " << e.what() << "\033[0m"
                  << std::endl;
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

bool socket_ser_ipc::open_data_plane()
{
    /* 服务端: 请求方向只收(RecvOnly), 响应方向只发(SendOnly)。
     * 这两条通道各自单向, 所以能干净地分开角色 —— 响应通道不入组, 自己发出
     * 的响应分片不会回绕进来堵住客户端的 ACK。 */
    ipc_r_ptr_ = std::make_shared<ipc::socket::UDPNode>(this->topic_name_.c_str(), this->ipaddr_.c_str(),
                                                        static_cast<uint16_t>(this->port_hash_),
                                                        ipc::socket::NodeRole::RecvOnly);
    ipc_w_ptr_ = std::make_shared<ipc::socket::UDPNode>(
        this->topic_name_.c_str(), this->ipaddr_.c_str(),
        static_cast<uint16_t>(this->port_hash_ + dzIPC::common::kUdpPortOffsetResponse),
        ipc::socket::NodeRole::SendOnly);
    /* 请求方向的 ACK: 服务端发出 -> 客户端收 */
    ack_r_tx_ = std::make_shared<ipc::socket::UDPNode>(
        this->topic_name_.c_str(), this->ipaddr_.c_str(),
        static_cast<uint16_t>(this->port_hash_ + dzIPC::common::kUdpPortOffsetAckData),
        ipc::socket::NodeRole::SendOnly);
    /* 响应方向的 ACK: 客户端发出 -> 服务端收 */
    ack_w_rx_ = std::make_shared<ipc::socket::UDPNode>(
        this->topic_name_.c_str(), this->ipaddr_.c_str(),
        static_cast<uint16_t>(this->port_hash_ + dzIPC::common::kUdpPortOffsetAckResponse),
        ipc::socket::NodeRole::RecvOnly);
    if (verbose_)
    {
        std::cerr << "\033[32m[" << topic_name_ << "SerInfo] Request initialized on IP: " << this->ipaddr_
                  << " Port: " << this->port_hash_ << " for topic: " << topic_name_ << "\033[0m" << std::endl;
        std::cerr << "\033[32m[" << topic_name_ << "SerInfo] Response initialized on IP: " << this->ipaddr_
                  << " Port: " << this->port_hash_ + 1 << " for topic: " << topic_name_ << "\033[0m" << std::endl;
    }
    if (!connect_with_retry(ipc_r_ptr_.get(), running, topic_name_, "SerInfo"))
    {
        return false;
    }
    if (!connect_with_retry(ipc_w_ptr_.get(), running, topic_name_, "SerInfo"))
    {
        return false;
    }
    /* ACK 通道连不上不致命: 请求/响应都走 BestEffort, 不依赖确认。
     * 置空后 chunk_rev_server 退回"在数据通道上回 ACK"的旧行为。 */
    if (!ack_r_tx_->connect())
    {
        ack_r_tx_.reset();
    }
    if (!ack_w_rx_->connect())
    {
        ack_w_rx_.reset();
    }
    /* ⛔ 不在这里置 data_plane_running_（裁定 ②/⑥）：调用方必须在"新 generation 注册完成"
     * 之后才置位，否则会出现"标志为真但接收路径还没接上"的窗口。 */
    return true;
}

void socket_ser_ipc::close_data_plane()
{
    if (ipc_r_ptr_)
    {
        ipc_r_ptr_->close();
    }
    if (ipc_w_ptr_)
    {
        ipc_w_ptr_->close();
    }
    if (ack_r_tx_)
    {
        ack_r_tx_->close();
    }
    if (ack_w_rx_)
    {
        ack_w_rx_->close();
    }
    /* 标志由调用方管理（stop/析构/restart 的步序需要它"最后置真、最先置假"）。 */
}

/* ---- 数据面停/起: 路径切换的"停-切-起"里属于传输层的那一半 ----
 *
 * 停机协议（裁定 ⑤ / 方案 §5）——顺序不可颠倒：
 *   ① 标记 data_plane_running_=false：处理线程在完成**当前**请求后退出；拒绝新处理；
 *   ② 同步注销 worker route：remove_route 内部完成契约 §4.4 的 1-6 步
 *      （摘表 → wait_set.remove 唤醒 wait → stop_and_wake=cancel_wait 打断在途组包 →
 *       等 worker 侧 in-flight 归零 → wait_quiescent 等本模块 in-flight → release_recv）；
 *   ③ join 收/处理线程：等 callback 与响应发送 in-flight 归零（⛔ 禁止对象销毁后继续发送）；
 *   ④ 关闭数据/ACK 节点。
 * ⛔ 先停线程再看 worker / 反过来的话，收包线程会在已关闭的 node 上继续 receive
 * （use-after-close）。兼容模式下没有 wait-set，② 退化为 udp_node_cancel_wait 唤醒在途 receive。
 *
 * ⛔ 不允许把"关掉握手通道让握手线程自己退"当作停数据面的手段: 那会同时打断
 * 存活检测, 让对端把一次切换读成一次断连 (T2 §3 D4)。
 *
 * 代价上界 = 当前那次 chunk_rev_server(ServerRevTime) 的剩余时间（cancel_wait 后更短）
 * + 当前 callback 与响应发送的剩余时间 + 关通道时间。它是**有界**的，这正是旧实现缺的
 * 那一条: 旧实现里建链失败会无限重试, 没有任何上界。 */
void socket_ser_ipc::stop_data_plane()
{
    data_plane_running_.store(false, std::memory_order_release);
    teardown_receive_path();
    close_data_plane();
    /* ⛔ 不清 handshake_completed_: 它的语义是"对端还活着", 而握手通道常驻,
     * 停数据面并不改变这个事实 (T2 §3 D6: 握手通道是唯一存活权威)。把两件事
     * 混在一个标志里会立刻产生一个死锁式的假象 —— 停过一次之后服务端永远
     * 看不到客户端"重新连上", 回退路径就再也起不来了。
     * "数据面能不能用"是 data_plane_running_ 的事, 调用方按需自己查。 */
}

void socket_ser_ipc::restart_data_plane()
{
    if (data_plane_running_.load(std::memory_order_acquire))
    {
        return;   // 幂等: 已经在跑
    }
    if (!running.load(std::memory_order_acquire))
    {
        return;   // 对象正在析构, 不再起新线程
    }
    /* ⛔ restart 必须用**新 generation**，且旧代先**完全**注销再关旧节点（裁定 ②/⑥）：
     *   完全注销旧 generation → 关旧节点 → 建并连接新节点 → 注册新 generation
     *   → **最后**置 data_plane_running_。
     * 反过来的话新节点可能拿到**旧 fd 号**，而旧 route 的 wait 项 / 在途 recv_once 还指着它
     * ⇒ fd 复用事件误关联。旧 UDPNode 一律不复用（open_data_plane 每次都新建）。 */
    teardown_receive_path();   // ① 完全注销旧 generation（含取消 OS wait 与等在途归零）
    close_data_plane();        // ② 关旧节点（此刻已无任何线程在碰它们）
    if (!open_data_plane())    // ③ 建并连接新节点（新 UDPNode ⇒ 新 fd）
    {
        return;
    }
    const bool worker_mode = start_receive_path();   // ④ 注册新 generation
    data_plane_running_.store(true, std::memory_order_release);   // ⑤ 最后置位
    response_thread_ = new std::thread(
        worker_mode ? &socket_ser_ipc::process_thread_func : &socket_ser_ipc::response_thread_func, this);
    dzIPC::ThreadDispatch::apply_thread_options(response_thread_, thread_options_, verbose_,
                                                topic_name_ + "_SocketSerResponseThread");
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

void socket_ser_ipc::server_handshake()
{
    uint64_t handshake_timeout_ms = 100;   // 100 ms
    ipc::socket::UDPNode ser_hs(topic_name_.c_str(), ipaddr_.c_str(), port_hash_ + dzIPC::common::kUdpPortOffsetHandshake);
    std::shared_ptr<IpcPubSubIdInitMsg> hs_msg = std::make_shared<IpcPubSubIdInitMsg>();
    /* R4: 连接失败必须可被 running 打断, 否则析构/切换的 join 永久阻塞。 */
    if (!connect_with_retry(&ser_hs, running, topic_name_, "SerInfo"))
    {
        return;
    }
    hs_msg->host_flag = true;
    ipc::buffer hs_buf;
    uint8_t last_sent_signal = 0xFF;   // 0xFF = 还没发过
    auto refresh_out_frame = [&]() {
        const uint8_t sig = hs_path_signal_.load(std::memory_order_acquire);
        if (sig == last_sent_signal)
        {
            return;
        }
        hs_msg->path_state = static_cast<IpcPubSubIdInitMsg::PathState>(sig);
        hs_buf = std::move(hs_msg->serialize());
        last_sent_signal = sig;
    };
    refresh_out_frame();
    State st = State::RunHS;
    /* 存活心跳(T2 §3 D6: 握手通道是**唯一**存活权威)。
     *
     * 为什么不能只靠那个 run_status=false 帧: 它是**一发定音**的 —— 对端析构时
     * 只发一条 UDP 报文就关通道, 丢一次就再也没有第二次, 双方会各自停在"连接还在"
     * 的假象里。实测该用例在全量套件里跑就会偶发失败(单跑 6/6, 套件里 1 次未回落)。
     *
     * 判据取"对端帧静默超时"而不是"收不到任何帧": 握手节点是 SendRecv(入组),
     * 自己发的帧会因 IP_MULTICAST_LOOP=1 回绕给自己, 所以 rev_buf 几乎不会为空 ——
     * 只有**满足对端身份**的帧(check_cli / check_host)才算心跳。
     *
     * ⛔ 只在"握手完成后还收到过对端帧"之后才启用(peer_post_hs_seen): 旧版本
     * 对端在 StopHS 里根本不发帧(T2 §7 R3 记录的旧行为), 对它们启用静默判死会
     * 制造假断连。新对端一定发, 于是这条通道**自配置**: 和旧进程互通时行为与今天
     * 逐字一致, 和新进程互通时才获得秒级断连检测。
     *
     * 阈值 1500 ms: 对端在 StopHS 的发帧频率被 receive(100ms) 天然限制在 ~10/s,
     * 15 个周期无声是"真的没了", 又远小于任何合理的业务超时。 */
    constexpr int64_t kPeerSilentTimeoutNs = 1'500'000'000LL;
    bool peer_post_hs_seen = false;
    auto last_peer_frame = std::chrono::steady_clock::now();
    while (running.load(std::memory_order_acquire))
    {
        refresh_out_frame();
        if (st == State::RunHS)
        {
            if (!ser_hs.send(hs_buf))
            {
                std::cerr << "\033[31m[" << topic_name_
                          << "SerInfo] Failed to send handshake message,retry affter 1 second...\033[0m" << std::endl;
                for (int i = 0; i < 10 && running.load(std::memory_order_acquire); ++i)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                continue;
            }
            ipc::buffer rev_buf = ser_hs.receive(handshake_timeout_ms);
            if (!rev_buf.empty())
            {
                peer_path_signal_.store(static_cast<uint8_t>(IpcPubSubIdInitMsg::peek_path_state(rev_buf)),
                                        std::memory_order_release);
            }
            if (rev_buf.empty() || !hs_msg->check_cli(rev_buf)
                || !hs_msg->check_run_status(rev_buf))   // 等待客户端回应，完成握手
            {
                continue;
            }
            else
            {
                if (verbose_)
                {
                    std::cerr << "\033[32m[" << topic_name_
                              << "SerInfo] Handshake with client completed for topic: " << topic_name_ << "\033[0m"
                              << std::endl;
                }
                handshake_completed_.store(true, std::memory_order_release);
                st = State::StopHS;
                continue;
            }
        }
        else
        {
            /* StopHS 也周期性发帧(频率由 receive 超时天然限制在 ~10 帧/秒, 与
             * RunHS 同量级): 路径裁定状态靠这条通道传给对端 (T2 §3 D2), 不发送
             * 就等于信号永远到不了。帧内 run_status 保持不变(true), 对端在 StopHS
             * 里读到它仍走 "continue" 分支, 既有的断连检测语义不受影响。 */
            refresh_out_frame();
            if (!ser_hs.send(hs_buf))
            {
                std::cerr << "\033[31m[" << topic_name_
                          << "SerInfo] Failed to send handshake message,retry affter 1 second...\033[0m" << std::endl;
                for (int i = 0; i < 10 && running.load(std::memory_order_acquire); ++i)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                continue;
            }
            ipc::buffer rev_buf = ser_hs.receive(handshake_timeout_ms);
            if (!rev_buf.empty() && hs_msg->check_cli(rev_buf))
            {
                peer_post_hs_seen = true;
                last_peer_frame = std::chrono::steady_clock::now();
                peer_path_signal_.store(static_cast<uint8_t>(IpcPubSubIdInitMsg::peek_path_state(rev_buf)),
                                        std::memory_order_release);
            }
            if (rev_buf.empty() || !hs_msg->check_cli(rev_buf)
                || hs_msg->check_run_status(rev_buf))   // 握手完成后继续监听客户端状态，直到客户端断开连接
            {
                /* 心跳判死: 只在见过新对端的心跳之后启用(见上面的说明)。 */
                const auto silent_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - last_peer_frame)
                                           .count();
                if (peer_post_hs_seen && silent_ns > kPeerSilentTimeoutNs
                    && running.load(std::memory_order_acquire))
                {
                    handshake_completed_.store(false, std::memory_order_release);
                    st = State::RunHS;
                    peer_post_hs_seen = false;
                }
                continue;
            }
            else
            {
                if (verbose_)
                {
                    std::cerr << "\033[32m[" << topic_name_
                              << "SerInfo] Client disconnected, restarting handshake for topic: " << topic_name_
                              << "\033[0m" << std::endl;
                }
                handshake_completed_.store(false, std::memory_order_release);
                st = State::RunHS;
                continue;
            }
        }
    }
    hs_msg->run_status = false;   // 客户端主动断开连接时通知服务端
    hs_buf = std::move(hs_msg->serialize());
    ser_hs.send(hs_buf);
    ser_hs.close();
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void socket_ser_ipc::response_thread_func()
{
    /* 两个条件缺一不可: running 是对象生命周期, data_plane_running_ 是数据面生命周期。
     * 只看 running 会让"停数据面"无法实现(切换的停-切-起需要它), 只看 data_plane_running_
     * 会让析构时的 join 依赖别的字段先被置位。 */
    while (running.load(std::memory_order_acquire) && data_plane_running_.load(std::memory_order_acquire))
    {
        /* 服务端等待请求,超时跳过 */
        std::shared_ptr<ServiceData> local_msg;
        {
            std::lock_guard<std::mutex> lock(message_mtx_);
            if (!message_)
            {
                continue;
            }
            local_msg.reset(message_->clone());
        }
        if (!chunk_rev_server(ipc_r_ptr_, local_msg, ServerRevTime, true, ack_r_tx_))
        {
            continue;
        }
        std::function<void(std::shared_ptr<ServiceData>&)> callback;
        {
            std::lock_guard<std::mutex> lock(callback_mtx_);
            callback = callback_;
        }
        if (callback)
        {
            callback(local_msg);
        }
        ipc::buffer response_data(std::move(local_msg->response()->serialize()));
        /* 响应与请求同理, 走 BestEffort。64 MB 缓冲下实测 110 MB/s 零丢包, 切
         * Reliable 要为每条消息多付一次 ACK 往返, 吞吐下降而收益不明显。
         *
         * 端点分离与 Heartbeat 在这条路径上的收益不是改变投递语义, 而是**丢片时
         * 更快放弃**: 客户端收到 Final 心跳就立即丢弃残缺消息, 不再空等两轮
         * NACK 超时(713 分片时最坏能省下近 1 秒)。 */
        if (!chunk_send(ipc_w_ptr_, response_data))
        {
            std::cerr << "\033[31m[" << topic_name_ << "SerInfo] Error sending response: Failed to send"
                      << "\033[0m" << std::endl;
        }
    }   // 客户段发送请求
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

/* worker 模式的处理路径：出队**完整请求** → 用户 callback → 响应序列化与发送。
 * ⛔ 这里（而不是收包 worker）才是允许跑用户回调的地方（需求 §1.2）。
 * 停机语义与兼容 response_thread_func 逐条一致：running / data_plane_running_ 变假后，
 * **当前这一条**处理完（含响应发送）即退出；队列里剩下的请求不再应答（已计 queue_drops
 * 提示，与改造前"收包线程停掉后到达的请求无人应答"是同一失败面）。 */
void socket_ser_ipc::process_thread_func()
{
    const std::shared_ptr<socket_ser_receive_state> state = receive_state_;
    if (!state)
    {
        return;
    }
    while (running.load(std::memory_order_acquire) && data_plane_running_.load(std::memory_order_acquire))
    {
        std::shared_ptr<ServiceData> request;
        {
            std::unique_lock<std::mutex> lock(state->queue_mtx);
            state->queue_cv.wait_for(lock, std::chrono::milliseconds{kSerQueueWaitMs}, [this, &state] {
                return !state->queue.empty() || !running.load(std::memory_order_acquire)
                       || !data_plane_running_.load(std::memory_order_acquire);
            });
            if (state->queue.empty())
            {
                continue;
            }
            request = std::move(state->queue.front());
            state->queue.pop_front();
        }
        if (!request)
        {
            continue;
        }
        std::function<void(std::shared_ptr<ServiceData>&)> callback;
        {
            std::lock_guard<std::mutex> lock(state->mtx);
            callback = state->callback;
        }
        if (callback)
        {
            callback(request);
        }
        ipc::buffer response_data(std::move(request->response()->serialize()));
        /* 与兼容路径同一句: 响应走 BestEffort, 发送失败只打日志（不抛、不重试）。 */
        if (!chunk_send(state->response_node, response_data))
        {
            std::cerr << "\033[31m[" << topic_name_ << "SerInfo] Error sending response: Failed to send"
                      << "\033[0m" << std::endl;
        }
    }
}

/* 接收路径注册：成功返回 true（本次走固定 socket worker）。返回 false 表示**必须**
 * 走兼容 response 线程，并且已经在 stderr 打了显式原因（绝不静默、绝不忙轮询降级）。 */
bool socket_ser_ipc::start_receive_path()
{
    const auto fallback = [this](const char* why) {
        std::cerr << "\033[33m[" << topic_name_ << "SerInfo] socket wait-set unusable (" << why
                  << "); keeping per-service receive thread\033[0m" << std::endl;
        return false;
    };

    if (socket_recv_compat_forced())
    {
        return fallback("DZIPC_SOCKET_COMPAT_THREAD=1");
    }
    if (!pool_allowed_in_this_process())
    {
        /* fork 后子进程不得碰池（池内锁可能被父进程持有）—— 见 pool_allowed_in_this_process。 */
        return fallback("forked child: recv pool owner pid mismatch");
    }
    if (!threepools::SocketRecvWorkerPool::backend_available())
    {
        return fallback("SocketWaitSet backend unavailable");
    }

    auto state = std::make_shared<socket_ser_receive_state>();
    state->route_key = topic_name_ + "#" + std::to_string(domain_id_);
    state->domain_id = static_cast<std::uint32_t>(domain_id_);
    /* 新 generation：restart 走的是"旧代完全注销 → 新代重新注册"，代际号只增不复用。 */
    state->generation = receive_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    /* 只注册请求数据通道；ack_r_tx_ 是发送端点、握手 socket 与数据面分离，都不入 worker。 */
    state->request_node = ipc_r_ptr_;
    state->request_ack_tx = ack_r_tx_;
    state->response_node = ipc_w_ptr_;
    {
        std::lock_guard<std::mutex> lock(message_mtx_);
        if (message_)
        {
            /* 独立克隆：chunk_rev_server 会反序列化进传入的那个 ServiceData，共用模板
             * 就等于让 worker 改模板。 */
            state->msg_template.reset(message_->clone());
        }
    }
    {
        std::lock_guard<std::mutex> lock(callback_mtx_);
        state->callback = callback_;
    }

    auto route = std::make_shared<socket_ser_request_route>(state);
    if (!route->wait_token().valid())
    {
        return fallback("request channel is not waitable (invalid_token)");
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
        return fallback(register_status_reason(status));
    }
    receive_state_ = state;
    receive_route_ = route;
    worker_mode_ = true;
    if (verbose_)
    {
        std::cerr << "\033[32m[" << topic_name_ << "SerInfo] request receive on shared socket worker "
                  << threepools::SocketRecvWorkerPool::worker_for(state->route_key.c_str(), state->domain_id,
                                                                  pool.worker_count())
                  << " (generation " << state->generation << ", workers " << pool.worker_count() << ")\033[0m"
                  << std::endl;
    }
    return true;
}

/* 接收路径注销：返回即"shared worker 不会再碰这条 route，本对象可以安全关节点/析构"。 */
void socket_ser_ipc::teardown_receive_path()
{
    if (worker_mode_ && receive_route_)
    {
        /* 契约 §4.4 的 1-6 步由 remove_route 同步完成（含 cancel_wait 与 release_recv）。 */
        threepools::SocketRecvWorkerPool::instance().remove_route(receive_route_.get());
    }
    else if (ipc_r_ptr_ && udp_node_waitable(ipc_r_ptr_))
    {
        /* 兼容模式：收包线程可能正阻塞在 receive(ServerRevTime)。cancel_wait 让它立刻
         * 返回（shutdown(SHUT_RD)），不必等满 200ms；节点随后由 close_data_plane 关闭。
         * 只在仍可等待时调，避免对已关闭（fd 号可能被复用）的节点做 shutdown。 */
        udp_node_cancel_wait(ipc_r_ptr_);
    }

    /* 等当前 callback 与响应发送结束（⛔ 禁止对象销毁后继续发送响应）。 */
    if (response_thread_ != nullptr)
    {
        if (response_thread_->joinable())
        {
            response_thread_->join();
        }
        delete response_thread_;
        response_thread_ = nullptr;
    }

    /* 队列里尚未处理的请求随本代作废（与改造前"收包线程停了之后无人应答"同一面），
     * 计数而不是静默丢弃。 */
    if (receive_state_)
    {
        std::lock_guard<std::mutex> lock(receive_state_->queue_mtx);
        if (!receive_state_->queue.empty())
        {
            receive_state_->queue_drops.fetch_add(receive_state_->queue.size(), std::memory_order_relaxed);
            receive_state_->queue.clear();
        }
    }
    receive_route_.reset();
    receive_state_.reset();
    worker_mode_ = false;
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

socket_cli_ipc::socket_cli_ipc(const std::string& topic_name, const std::shared_ptr<ServiceData>& msg, size_t domain_id,
                               bool verbose, bool enable_thread_qos, int cpu_id, int thread_priority)
    : cli_ipc_base(topic_name, msg, domain_id, verbose)
    , topic_name_(topic_name)
    , verbose_(verbose)
    /* domain_id_ 必须接到形参: IpcInfoPool 注册(rebind)读成员, T3 同机判定要求
     * topic+domain 双匹配 —— 漏接则客户端条目 domain 恒 0, 服务端永远看不到
     * 客户端, 切换永不触发。对齐 socket_ser_ipc 的写法。 */
    , domain_id_(domain_id)
    , thread_options_(dzIPC::ThreadDispatch::make_realtime_options(enable_thread_qos, cpu_id, thread_priority))
{
    message_.reset(msg->clone());
    this->port_hash_ = dzIPC::common::udp_discovery_port_calculate(topic_name_, domain_id);
    this->ipaddr_ = dzIPC::common::udp_discovery_addr_calculate(topic_name_);
    dzIPC::ThreadDispatch::apply_current_thread_options(thread_options_, verbose_, topic_name_ + "_SocketCliOwnerThread");
}

socket_cli_ipc::~socket_cli_ipc()
{
    running.store(false, std::memory_order_release);
    /* 必须先 join handshake 线程，再让本对象的成员变量被销毁；
       否则该线程后续 cli_hs.receive 超时返回时会读到已被释放的成员 */
    if (handshake_thread_ != nullptr)
    {
        if (handshake_thread_->joinable())
        {
            handshake_thread_->join();
        }
        delete handshake_thread_;
        handshake_thread_ = nullptr;
    }
    close_data_plane();
    exit_flag.store(true, std::memory_order_release);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

void socket_cli_ipc::InitChannel(std::string extra_info)
{
    try
    {
        if (!open_data_plane())
        {
            std::cerr << "\033[31m[" << topic_name_ << "CliInfo] Data plane failed to start; handshake only.\033[0m"
                      << std::endl;
            return;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "\033[31m[" << topic_name_ << "CliInfo] Error initializing channel: " << e.what() << "\033[0m"
                  << std::endl;
        return;
    }
    /* 池注册必须**先于**握手线程启动: 对端的同机判定读的是 IpcInfoPool
       (T2 §3 D1), 而客户端的握手应答一旦发出, 对端就可能立刻开始判定 ——
       现行顺序 (:429 起线程 vs :440 rebind) 是一个真实的竞态窗口, 在
       "切换必须双方一致"的设计下它会让判定永久发散。 */
    std::shared_ptr<ServiceData> message_template;
    {
        std::lock_guard<std::mutex> lock(message_mtx_);
        message_template = message_;
    }
    std::string response_type_name =
        (message_template && message_template->response())
            ? dzIPC::info_pool::demangle(typeid(*message_template->response()).name())
            : std::string{};
    response_type_name = extract_last_segment(response_type_name);
    pool_reg_.rebind({dzIPC::info_pool::EntryKind::SocketClient, topic_name_, response_type_name, "socket",
                      static_cast<int32_t>(domain_id_), extra_info});
    if (handshake_thread_ == nullptr)
    {
        handshake_thread_ = new std::thread(&socket_cli_ipc::client_handshake, this);
    }
    if (verbose_)
    {
        std::cerr << "\033[32m[" << topic_name_ << "_CliInfo] Client connected to server topic: " << topic_name_
                  << "\033[0m" << std::endl;
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

bool socket_cli_ipc::open_data_plane()
{
    /* 客户端与服务端镜像: 请求方向只发, 响应方向只收。 */
    ipc_r_ptr_ = std::make_shared<ipc::socket::UDPNode>(this->topic_name_.c_str(), this->ipaddr_.c_str(),
                                                        static_cast<uint16_t>(this->port_hash_),
                                                        ipc::socket::NodeRole::SendOnly);
    ipc_w_ptr_ = std::make_shared<ipc::socket::UDPNode>(
        this->topic_name_.c_str(), this->ipaddr_.c_str(),
        static_cast<uint16_t>(this->port_hash_ + dzIPC::common::kUdpPortOffsetResponse),
        ipc::socket::NodeRole::RecvOnly);
    /* 请求方向的 ACK: 服务端发出 -> 客户端收 */
    ack_r_rx_ = std::make_shared<ipc::socket::UDPNode>(
        this->topic_name_.c_str(), this->ipaddr_.c_str(),
        static_cast<uint16_t>(this->port_hash_ + dzIPC::common::kUdpPortOffsetAckData),
        ipc::socket::NodeRole::RecvOnly);
    /* 响应方向的 ACK: 客户端发出 -> 服务端收 */
    ack_w_tx_ = std::make_shared<ipc::socket::UDPNode>(
        this->topic_name_.c_str(), this->ipaddr_.c_str(),
        static_cast<uint16_t>(this->port_hash_ + dzIPC::common::kUdpPortOffsetAckResponse),
        ipc::socket::NodeRole::SendOnly);
    if (verbose_)
    {
        std::cerr << "\033[32m[" << topic_name_ << "CliInfo] Request initialized on IP: " << this->ipaddr_
                  << " Port: " << this->port_hash_ << " for topic: " << topic_name_ << "\033[0m" << std::endl;
        std::cerr << "\033[32m[" << topic_name_ << "CliInfo] Response initialized on IP: " << this->ipaddr_
                  << " Port: " << this->port_hash_ + 1 << " for topic: " << topic_name_ << "\033[0m" << std::endl;
    }
    if (!connect_with_retry(ipc_r_ptr_.get(), running, topic_name_, "CliInfo"))
    {
        return false;
    }
    if (!connect_with_retry(ipc_w_ptr_.get(), running, topic_name_, "CliInfo"))
    {
        return false;
    }
    /* ACK 通道非关键路径, 连不上就退回旧行为。 */
    if (!ack_r_rx_->connect())
    {
        ack_r_rx_.reset();
    }
    if (!ack_w_tx_->connect())
    {
        ack_w_tx_.reset();
    }
    data_plane_running_.store(true, std::memory_order_release);
    return true;
}

void socket_cli_ipc::close_data_plane()
{
    if (ipc_r_ptr_)
    {
        ipc_r_ptr_->close();
    }
    if (ipc_w_ptr_)
    {
        ipc_w_ptr_->close();
    }
    if (ack_r_rx_)
    {
        ack_r_rx_->close();
    }
    if (ack_w_tx_)
    {
        ack_w_tx_->close();
    }
    data_plane_running_.store(false, std::memory_order_release);
}

/* 停数据面: 客户端没有收包线程, 所以只是关 4 条通道。
 * ⛔ 不碰握手通道 (port+2) —— 它是存活权威, 重建会让服务端把一次切换读成一次断连。 */
void socket_cli_ipc::stop_data_plane()
{
    close_data_plane();
    /* ⛔ 同服务端: 不动 handshake_completed_。send_request() 另查 data_plane_running_。 */
}

void socket_cli_ipc::restart_data_plane()
{
    if (data_plane_running_.load(std::memory_order_acquire))
    {
        return;   // 幂等
    }
    if (!running.load(std::memory_order_acquire))
    {
        return;
    }
    open_data_plane();   // 失败时 data_plane_running_ 保持 false, send_request 继续拒绝
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

void socket_cli_ipc::client_handshake()
{
    uint64_t handshake_timeout_ms = 100;   // 100 ms
    ipc::socket::UDPNode cli_hs(topic_name_.c_str(), ipaddr_.c_str(), port_hash_ + dzIPC::common::kUdpPortOffsetHandshake);
    std::shared_ptr<IpcPubSubIdInitMsg> hs_msg = std::make_shared<IpcPubSubIdInitMsg>();
    /* R4: 连接失败必须可被 running 打断。 */
    if (!connect_with_retry(&cli_hs, running, topic_name_, "CliInfo"))
    {
        return;
    }
    hs_msg->cli_flag = true;
    ipc::buffer hs_buf;
    uint8_t last_sent_signal = 0xFF;
    auto refresh_out_frame = [&]() {
        const uint8_t sig = hs_path_signal_.load(std::memory_order_acquire);
        if (sig == last_sent_signal)
        {
            return;
        }
        hs_msg->path_state = static_cast<IpcPubSubIdInitMsg::PathState>(sig);
        hs_buf = std::move(hs_msg->serialize());
        last_sent_signal = sig;
    };
    refresh_out_frame();
    State st = State::RunHS;
    int send_cnt = 0;
    /* 存活心跳, 与服务端同构(详见 server_handshake 里的说明)。 */
    constexpr int64_t kPeerSilentTimeoutNs = 1'500'000'000LL;
    bool peer_post_hs_seen = false;
    auto last_peer_frame = std::chrono::steady_clock::now();
    while (running.load(std::memory_order_acquire))
    {
        refresh_out_frame();
        if (st == State::RunHS)
        {
            ipc::buffer rev_buf = cli_hs.receive(handshake_timeout_ms);
            if (!rev_buf.empty())
            {
                peer_path_signal_.store(static_cast<uint8_t>(IpcPubSubIdInitMsg::peek_path_state(rev_buf)),
                                        std::memory_order_release);
            }
            if (rev_buf.empty() || !hs_msg->check_host(rev_buf)
                || !hs_msg->check_run_status(rev_buf))   // 等待服务端发送握手消息
            {
                continue;
            }
            else
            {
                /* 只有**确实发出去**才置握手完成; 发送失败就留在 RunHS 等下一轮
                 * 服务端的 host_flag, 不宣称连接可用 —— 否则 send_request() 会往
                 * 无人接收的 SendOnly socket 上发请求并静默失败(本仓反复出现的
                 * "静默失败"族)。 */
                bool sent = false;
                while (!(sent = cli_hs.send(hs_buf)))
                {
                    send_cnt++;
                    if (send_cnt > 10)
                    {
                        send_cnt = 0;
                        break;
                    }
                    std::cerr << "\033[31m[" << topic_name_
                              << "CliInfo] Failed to send handshake message,retry affter 1 second...\033[0m"
                              << std::endl;
                    std::this_thread::sleep_for(std::chrono::seconds(1));
                }
                if (!sent)
                {
                    continue;   // 未送达: 不置 handshake_completed_, 留在 RunHS 重来
                }
                handshake_completed_.store(true, std::memory_order_release);
                if (verbose_)
                {
                    std::cerr << "\033[32m[" << topic_name_
                              << "CliInfo] Handshake with server completed for topic: " << topic_name_ << "\033[0m  "
                              << std::endl;
                }
                st = State::StopHS;
                continue;
            }
        }
        else
        {
            /* 同服务端: StopHS 也要周期性发帧, 否则"确认/撤销"这类裁定状态传不出去。 */
            refresh_out_frame();
            if (!cli_hs.send(hs_buf))
            {
                std::cerr << "\033[31m[" << topic_name_
                          << "CliInfo] Failed to send handshake message,retry affter 1 second...\033[0m" << std::endl;
                for (int i = 0; i < 10 && running.load(std::memory_order_acquire); ++i)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                continue;
            }
            ipc::buffer rev_buf = cli_hs.receive(handshake_timeout_ms);
            if (!rev_buf.empty() && hs_msg->check_host(rev_buf))
            {
                peer_post_hs_seen = true;
                last_peer_frame = std::chrono::steady_clock::now();
                peer_path_signal_.store(static_cast<uint8_t>(IpcPubSubIdInitMsg::peek_path_state(rev_buf)),
                                        std::memory_order_release);
            }
            if (rev_buf.empty() || !hs_msg->check_host(rev_buf)
                || hs_msg->check_run_status(rev_buf))   // 握手完成后继续监听服务端状态，直到服务端断开连接
            {
                const auto silent_ns = std::chrono::duration_cast<std::chrono::nanoseconds>(
                                           std::chrono::steady_clock::now() - last_peer_frame)
                                           .count();
                if (peer_post_hs_seen && silent_ns > kPeerSilentTimeoutNs
                    && running.load(std::memory_order_acquire))
                {
                    handshake_completed_.store(false, std::memory_order_release);
                    st = State::RunHS;
                    peer_post_hs_seen = false;
                }
                continue;
            }
            else
            {
                if (verbose_)
                {
                    std::cerr << "\033[31m[" << topic_name_
                              << "CliInfo] Server disconnected, restarting handshake for topic: " << topic_name_
                              << "\033[0m" << std::endl;
                }
                handshake_completed_.store(false, std::memory_order_release);
                st = State::RunHS;
                continue;
            }
        }
    }
    hs_msg->run_status = false;   // 客户端主动断开连接时通知服务端
    hs_buf = std::move(hs_msg->serialize());
    cli_hs.send(hs_buf);
    cli_hs.close();
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool socket_cli_ipc::send_request(std::shared_ptr<ServiceData>& request, uint64_t rev_tm)
{
    /* 数据面已停(切换的"停"阶段)必须**立即**返回 false, 既不能挂起也不能半发 ——
     * T2 §7 R9 的"切换引入的挂起"就是这一条的反例, 对照 shm_defect_fixes.md 第 4 条
     * (超时未接线 ⇒ 永久挂死) 的教训。 */
    if (!data_plane_running_.load(std::memory_order_acquire))
    {
        return false;
    }
    if (handshake_completed_.load(std::memory_order_acquire))
    {
        ipc::buffer request_data(std::move(request->request()->serialize()));
        /* ser-cli 两端都用 BestEffort (chunk_send), 不等 ACK。
         *
         * 曾经改成 chunk_send_reliable 试图修 1 MB 失败, 那是错误方向:
         *   1. 当时的 ipc_r_ptr_ 收发共用且入了组, 在它上面等 ACK 会撞上自己
         *      分片的 loopback 回绕 —— 与 pub-sub 单通道的失败模式相同。
         *   2. 1 MB 的原始失败出现在 "Error receiving response" 而不是
         *      "Failed to send" —— 发送端从没有问题, 是接收端 rmem_max=208KB
         *      的缓冲溢出导致片丢。
         *
         * 现在 ipc_r_ptr_ 已是 SendOnly(不入组, 无回绕), 技术上可以走 Reliable,
         * 但仍然维持 BestEffort: 64 MB 缓冲下已能跑 110 MB/s 零丢包, 多一次 ACK
         * 往返只会拉低吞吐。要可靠投递的调用方可以显式用带 ack_node 的
         * chunk_send_reliable(ipc_r_ptr_, data, ack_r_rx_)。 */
        if (!chunk_send(ipc_r_ptr_, request_data))
        {
            std::cerr << "\033[31m[" << topic_name_ << "CliInfo] Error sending request: Failed to send"
                      << "\033[0m" << std::endl;
            return false;
        }
        if (!chunk_rev_server(ipc_w_ptr_, request, rev_tm, false, ack_w_tx_))
        {
            std::cerr << "\033[31m[" << topic_name_
                      << "CliInfo] Error receiving response: Failed to receive or parse response\033[0m" << std::endl;
            return false;
        }
        return true;
    }
    else
    {
        return false;
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void socket_cli_ipc::reset_message(const std::shared_ptr<ServiceData>& msg)
{
    if (!msg)
    {
        return;
    }
    std::shared_ptr<ServiceData> new_msg;
    new_msg.reset(msg->clone());
    std::lock_guard<std::mutex> lock(message_mtx_);
    message_ = std::move(new_msg);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
}   // namespace socket
}   // namespace dzIPC

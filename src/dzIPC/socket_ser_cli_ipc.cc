#include "dzIPC/socket_ser_cli_ipc.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <atomic>
#include <iostream>
#include <memory>
#include <thread>
#include <typeinfo>
#include "dzIPC/common/data_rev.h"
#include "dzIPC/common/hash.h"
#include "dzIPC/common/name_operator.h"
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
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void socket_ser_ipc::reset_callback(std::function<void(std::shared_ptr<ServiceData>&)> callback)
{
    std::lock_guard<std::mutex> lock(callback_mtx_);
    callback_ = std::move(callback);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

socket_ser_ipc::~socket_ser_ipc()
{
    running.store(false, std::memory_order_release);
    /* 顺序: 停数据面(join response) -> join 握手线程 -> 关通道。
     * response 线程先于握手线程 join, 因为前者读 ipc_r_ptr_, 后者只读 ser_hs (局部)。 */
    data_plane_running_.store(false, std::memory_order_release);
    if (response_thread_ != nullptr)
    {
        if (response_thread_->joinable())
        {
            response_thread_->join();
        }
        delete response_thread_;
        response_thread_ = nullptr;
    }
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
        response_thread_ = new std::thread(&socket_ser_ipc::response_thread_func, this);
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
    data_plane_running_.store(true, std::memory_order_release);
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
    data_plane_running_.store(false, std::memory_order_release);
}

/* ---- 数据面停/起: 路径切换的"停-切-起"里属于传输层的那一半 ----
 *
 * ⛔ 顺序不可颠倒: 先置 running=false -> response 线程退出 -> 再关通道。
 * 反过来的话, 收包线程会继续在已关闭的 node 上 receive (旧代码里 response_thread_
 * 只检查 running, 不检查 node 是否已被 close), 这是 use-after-close。
 *
 * ⛔ 不允许把"关掉握手通道让握手线程自己退"当作停数据面的手段: 那会同时打断
 * 存活检测, 让对端把一次切换读成一次断连 (T2 §3 D4)。
 *
 * 代价上界 = 当前那次 chunk_rev_server(ServerRevTime) 的剩余时间 + 关通道时间。
 * 实测该值远小于 200 ms, 但它是**有界**的, 这正是旧实现缺的那一条: 旧实现里
 * 建链失败会无限重试, 没有任何上界。 */
void socket_ser_ipc::stop_data_plane()
{
    const bool was_running = data_plane_running_.load(std::memory_order_acquire);
    /* 先置假 -> response 线程在收包超时后退出 -> 再关通道。顺序不可颠倒:
     * 反过来的话收包线程会在已关闭的 node 上继续 receive, 是 use-after-close。 */
    data_plane_running_.store(false, std::memory_order_release);
    if (response_thread_ != nullptr)
    {
        if (response_thread_->joinable())
        {
            response_thread_->join();
        }
        delete response_thread_;
        response_thread_ = nullptr;
    }
    close_data_plane();
    (void)was_running;
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
    if (!open_data_plane())
    {
        return;
    }
    response_thread_ = new std::thread(&socket_ser_ipc::response_thread_func, this);
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
            /* ⛔ 旧实现在 StopHS **从不发送**(只在 RunHS 发 host_flag)。这在只有
             * 一个布尔"是否已握手"的世界里没问题, 但路径裁定状态要靠这条通道传给
             * 对端 (T2 §3 D2), 不发送就等于信号永远到不了 —— 实测表现是双方各自
             * 判定、客户端永远停在 socket、服务端白等一个 T_est 后回退。
             *
             * 因此 StopHS 也要周期性发帧。频率由下面的 receive 超时天然限制在
             * ~10 帧/秒, 与 RunHS 同量级; 帧内容里的 run_status 保持不变(true),
             * 所以对端在 StopHS 里读到它仍走"continue"分支, 既有的断连检测语义
             * (靠 run_status=false)完全不受影响。 */
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

socket_cli_ipc::socket_cli_ipc(const std::string& topic_name, const std::shared_ptr<ServiceData>& msg, size_t domain_id,
                               bool verbose, bool enable_thread_qos, int cpu_id, int thread_priority)
    : cli_ipc_base(topic_name, msg, domain_id, verbose)
    , topic_name_(topic_name)
    , verbose_(verbose)
    /* ⛔ domain_id_ 以前**从未被赋值**: 初始化列表里没有它, 构造函数体只把
     * 形参 domain_id 交给了 port_hash_, 于是成员一直是默认值 0。后果不是立刻
     * 可见的 —— 端口/组地址走的是形参那条路, 通信照常; 但 IpcInfoPool 的注册
     * 用的是成员(:440 的 rebind), 所以**客户端条目里的 domain 恒为 0**, 而服务端
     * 写的是真值。实测(t3_dump): 同一条连接上 server 条目 domain=7、client 条目
     * domain=0。
     *
     * T3 的同机判定读的就是池里的对端条目 (T2 §3 D1), 且要求 topic+domain 都匹配
     * ⇒ 服务端永远看不到客户端 —— 判定必然落到 NoEvidence, 切换永远不触发。
     * 修法就是把它接上; 对齐服务端 socket_ser_ipc 的写法(:34 domain_id_(domain_id))。 */
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
                /* ⛔ 旧实现在这里先把 send 重试 10 次, 每次失败后 `continue` 留在 RunHS,
                 * 但重试耗尽时是 `break` 出来 —— 然后**无条件**置 handshake_completed_=true。
                 * 结果是 10 次全失败也被记为"握手完成", send_request() 随即往一个
                 * 无人接收的 SendOnly socket 上发请求并静默失败(每条都超时返回 false,
                 * 而对端从未收到过任何请求)。这是本仓反复出现的"静默失败"族。
                 *
                 * 修法: 只有**确实发出去**才置完成; 发送失败就留在 RunHS 等下一轮
                 * 服务端的 host_flag, 不宣称连接可用。 */
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

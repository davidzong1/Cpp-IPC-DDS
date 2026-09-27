#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "dzIPC/common/srv_data.h"
#include "dzIPC/common/thread_dispatch.h"
#include "dzIPC/ipc_info_pool.h"
#include "dzIPC/ser_cli_base.h"
#include "libipc/udp.h"

namespace dzIPC {
namespace threepools {
/* 阶段 5 共享层：socket 侧 route 抽象（include/dzIPC/threepools/socket_recv_worker.h）。
 * 公开头只做前置声明，真正的 include 放在 .cc —— 只 include 本头的消费者不必为此
 * 拉进等待层。 */
class SocketRecvRouteSource;
}   // namespace threepools
namespace socket {
class socket_cli_ipc;
/* socket_ser_ipc 服务端接收路径的 service shared state（定义在 .cc）。
 * 收包 worker 只通过 SocketRecvRouteSource 适配器接触它，**不持**裸 socket_ser_ipc 指针。 */
struct socket_ser_receive_state;

class IPC_EXPORT socket_ser_ipc : public ser_ipc_base
{
public:
    explicit socket_ser_ipc(const std::string& topic_name, const std::shared_ptr<ServiceData>& msg,
                            std::function<void(std::shared_ptr<ServiceData>&)> callback, size_t domain_id,
                            bool verbose = false, bool enable_thread_qos = false, int cpu_id = -1,
                            int thread_priority = 0);
    ~socket_ser_ipc();
    void reset_message(const std::shared_ptr<ServiceData>& msg);
    void reset_callback(std::function<void(std::shared_ptr<ServiceData>&)> callback);
    void InitChannel(std::string extra_info = "");
    /* 禁用拷贝 */
    socket_ser_ipc(const socket_ser_ipc&) = delete;
    socket_ser_ipc& operator=(const socket_ser_ipc&) = delete;

    bool handshake_completed() const override { return handshake_completed_.load(std::memory_order_acquire); }

    /* ---- 数据面停/起(路径切换用) ----
     *
     * 握手通道与数据面是两件事: 握手通道(port+2)是**常驻的存活权威**, 切换只换数据面,
     * 所以它不在这两个函数的作用域内。停数据面 = 关 4 条 UDP 通道 + join response 线程;
     * 停的上界 = response 线程当前那次 chunk_rev_server(ServerRevTime=200ms) 的剩余时间
     * (T2 §3 D3 的 T_stop_max)。两者都幂等。 */
    void stop_data_plane();
    void restart_data_plane();
    bool data_plane_running() const { return data_plane_running_.load(std::memory_order_acquire); }

    /* ---- 握手帧里承载的路径裁定信号(T2 §3 D2) ----
     *
     * 取值是 IpcPubSubIdInitMsg::PathState 的数值(0=Unknown/1=ProposeShm/2=ConfirmShm/
     * 3=ConfirmSocket/4=WithdrawToSocket)。这里用 uint8_t 而不是把消息类型搬进头文件:
     * 让 socket 臂不必依赖 ipc_msg 的具体消息定义, 换消息布局时这里不受牵连。
     *
     * 为什么裁定要走握手帧而不是各自读 IpcInfoPool: T2 §2 的四条发散案例里,
     * "池满/注册顺序竞态"都会让两端独立判定出不同结果, 而发散的表现是**静默永久失败**
     * (客户端写 SHM、服务端读 UDP, 双方握手通道都活着所以都检测不到断连)。 */
    void set_path_signal(uint8_t s) { hs_path_signal_.store(s, std::memory_order_release); }
    uint8_t peer_path_signal() const { return peer_path_signal_.load(std::memory_order_acquire); }

    /* 本臂恒为 socket: 它不做切换(切换是上层 auto 层的事), 所以返回常量即可。 */
    path::Kind transport_current() const override { return path::Kind::Socket; }

protected:
    void response_thread_func();
    /* worker 模式的处理路径：只从有界队列取**完整请求** → 用户 callback → 响应序列化与发送。
     * 与兼容 response_thread_func 的分工见 .cc 文件头：⛔ 用户 callback 与响应发送
     * **不在**收包 worker 里（需求 §1.2）。 */
    void process_thread_func();
    void server_handshake();
    /* 数据面的建/拆。InitChannel 与 restart/stop 共用同一份, 避免两条路径漂移。
     * open 返回 false 表示建链被 running=false 打断(不是失败重试耗尽)。 */
    bool open_data_plane();
    void close_data_plane();
    /* 接收路径（共享层固定 socket worker 或兼容收包线程）的注册/注销。
     * start_receive_path 返回 false 表示本进程/本后端必须走兼容 response thread。 */
    bool start_receive_path();
    void teardown_receive_path();

private:
    size_t domain_id_{0};
    std::atomic<bool> running{true};
    std::atomic<bool> handshake_completed_{false};
    std::atomic<bool> data_plane_running_{false};
    std::atomic<uint8_t> hs_path_signal_{0};     // 本端要告诉对端的裁定信号
    std::atomic<uint8_t> peer_path_signal_{0};   // 从对端帧里读到的最新裁定信号
    uint64_t port_hash_;
    std::string ipaddr_;
    bool verbose_{true};
    std::function<void(std::shared_ptr<ServiceData>&)> callback_;
    std::mutex callback_mtx_;
    std::string topic_name_;
    std::shared_ptr<ServiceData> message_;
    std::mutex message_mtx_;
    std::thread* response_thread_{nullptr};
    std::thread* handshake_thread_{nullptr};
    std::shared_ptr<ipc::socket::UDPNode> ipc_r_ptr_;
    std::shared_ptr<ipc::socket::UDPNode> ipc_w_ptr_;
    /* 端点分离的 ACK 通道。服务端: 在请求方向回 ACK(ack_r_tx_), 在响应方向收
     * ACK(ack_w_rx_)。数据 socket 各自只承担一个方向, 因而可以不入组/入组分开设。 */
    std::shared_ptr<ipc::socket::UDPNode> ack_r_tx_;
    std::shared_ptr<ipc::socket::UDPNode> ack_w_rx_;
    std::vector<char> buf_;
    std::vector<char> response_buf_;
    dzIPC::info_pool::ScopedRegistration pool_reg_;
    dzIPC::ThreadDispatch::ThreadOptions thread_options_;
    /* ---- 阶段 5：固定 socket worker 接入面 ----
     * worker 本体由共享层提供（captain 裁定 A），本类只持 route 适配器与 service state。
     * worker 模式下只有 ipc_r_ptr_（请求数据通道）注册；ack_r_tx_ 是发送端点、握手
     * socket 与数据面分离，都不入 worker。 */
    std::shared_ptr<socket_ser_receive_state> receive_state_;
    std::shared_ptr<threepools::SocketRecvRouteSource> receive_route_;
    bool worker_mode_{false};
    /* 数据面代际：每次注册新接收路径 +1（restart 必须先完全注销旧 generation）。 */
    std::atomic<std::uint32_t> receive_generation_{0};
};

class IPC_EXPORT socket_cli_ipc : public cli_ipc_base
{
public:
    explicit socket_cli_ipc(const std::string& topic_name, const std::shared_ptr<ServiceData>& msg, size_t domain_id,
                            bool verbose = false, bool enable_thread_qos = false, int cpu_id = -1,
                            int thread_priority = 0);
    ~socket_cli_ipc();
    void InitChannel(std::string extra_info = "");
    void reset_message(const std::shared_ptr<ServiceData>& msg);
    bool send_request(std::shared_ptr<ServiceData>& request, uint64_t rev_tm = std::numeric_limits<uint32_t>::max());
    /* 禁用拷贝 */
    socket_cli_ipc(const socket_cli_ipc&) = delete;
    socket_cli_ipc& operator=(const socket_cli_ipc&) = delete;

    bool handshake_completed() const override { return handshake_completed_.load(std::memory_order_acquire); }

    /* ---- 数据面停/起(路径切换用) ---- 语义同 socket_ser_ipc: 握手通道常驻, 只动
     * 4 条数据/ACK UDP 通道。停之后 send_request() 立即返回 false(不阻塞、不半发)。 */
    void stop_data_plane();
    void restart_data_plane();
    bool data_plane_running() const { return data_plane_running_.load(std::memory_order_acquire); }

    /* 语义同 socket_ser_ipc。 */
    void set_path_signal(uint8_t s) { hs_path_signal_.store(s, std::memory_order_release); }
    uint8_t peer_path_signal() const { return peer_path_signal_.load(std::memory_order_acquire); }

    path::Kind transport_current() const override { return path::Kind::Socket; }

protected:
    void client_handshake();
    bool open_data_plane();
    void close_data_plane();

private:
    size_t domain_id_{0};
    std::atomic<bool> running{true};
    std::atomic<bool> handshake_completed_{false};
    std::atomic<bool> data_plane_running_{false};
    std::atomic<uint8_t> hs_path_signal_{0};
    std::atomic<uint8_t> peer_path_signal_{0};
    uint64_t port_hash_;
    std::string ipaddr_;
    bool verbose_{true};
    std::shared_ptr<ServiceData> message_;
    std::mutex message_mtx_;
    std::string topic_name_;
    std::shared_ptr<ipc::socket::UDPNode> ipc_r_ptr_;
    std::shared_ptr<ipc::socket::UDPNode> ipc_w_ptr_;
    /* 端点分离的 ACK 通道, 与服务端方向相反: 客户端在请求方向收 ACK(ack_r_rx_),
     * 在响应方向回 ACK(ack_w_tx_)。 */
    std::shared_ptr<ipc::socket::UDPNode> ack_r_rx_;
    std::shared_ptr<ipc::socket::UDPNode> ack_w_tx_;
    std::vector<char> buf_;
    std::vector<char> response_buf_;
    std::thread* handshake_thread_{nullptr};
    dzIPC::info_pool::ScopedRegistration pool_reg_;
    dzIPC::ThreadDispatch::ThreadOptions thread_options_;
};
}   // namespace socket
}   // namespace dzIPC

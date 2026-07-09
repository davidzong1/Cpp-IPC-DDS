#pragma once
#include <atomic>
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
namespace socket {
class socket_ser_ipc;
class socket_cli_ipc;

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

protected:
    void response_thread_func();
    void server_handshake();

private:
    size_t domain_id_{0};
    std::atomic<bool> running{true};
    std::atomic<bool> handshake_completed_{false};
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
    std::vector<char> buf_;
    std::vector<char> response_buf_;
    dzIPC::info_pool::ScopedRegistration pool_reg_;
    dzIPC::ThreadDispatch::ThreadOptions thread_options_;
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

protected:
    void client_handshake();

private:
    size_t domain_id_{0};
    std::atomic<bool> running{true};
    std::atomic<bool> handshake_completed_{false};
    uint64_t port_hash_;
    std::string ipaddr_;
    bool verbose_{true};
    std::shared_ptr<ServiceData> message_;
    std::mutex message_mtx_;
    std::string topic_name_;
    std::shared_ptr<ipc::socket::UDPNode> ipc_r_ptr_;
    std::shared_ptr<ipc::socket::UDPNode> ipc_w_ptr_;
    std::vector<char> buf_;
    std::vector<char> response_buf_;
    std::thread* handshake_thread_{nullptr};
    dzIPC::info_pool::ScopedRegistration pool_reg_;
    dzIPC::ThreadDispatch::ThreadOptions thread_options_;
};
}   // namespace socket
}   // namespace dzIPC

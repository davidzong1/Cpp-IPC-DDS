#pragma once
#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>
#include "dzIPC/common/srv_data.h"
#include "dzIPC/ipc_info_pool.h"
#include "dzIPC/ser_cli_base.h"
#include "libipc/ipc.h"

namespace dzIPC {
namespace shm {
class shm_ser_ipc;
class shm_cli_ipc;

class IPC_EXPORT shm_ser_ipc : public ser_ipc_base
{
public:
    explicit shm_ser_ipc(const std::string& topic_name, const std::shared_ptr<ServiceData>& msg,
                         std::function<void(std::shared_ptr<ServiceData>&)> callback, size_t domain_id,
                         bool verbose = false);
    ~shm_ser_ipc();
    void reset_message(const std::shared_ptr<ServiceData>& msg);
    void reset_callback(std::function<void(std::shared_ptr<ServiceData>&)> callback);
    void InitChannel(std::string extra_info = "");

    /* 禁用拷贝 */
    shm_ser_ipc(const shm_ser_ipc&) = delete;
    shm_ser_ipc& operator=(const shm_ser_ipc&) = delete;

protected:
    void response_thread_func();
    void ser_handshake();

private:
    size_t domain_id_{0};
    std::atomic<bool> running{true};
    std::atomic<bool> handshake_completed{false};
    bool verbose_{true};
    std::function<void(std::shared_ptr<ServiceData>&)> callback_;
    std::string topic_name_;
    std::shared_ptr<ServiceData> message_;
    std::thread* response_thread_{nullptr};
    std::thread* handshake_thread_{nullptr};
    std::shared_ptr<ipc::server> ipc_r_ptr_;
    std::shared_ptr<ipc::server> ipc_w_ptr_;
    std::vector<char> buf_;
    std::vector<char> response_buf_;
    dzIPC::info_pool::ScopedRegistration pool_reg_;
};

class IPC_EXPORT shm_cli_ipc : public cli_ipc_base
{
public:
    explicit shm_cli_ipc(const std::string& topic_name, const std::shared_ptr<ServiceData>& msg, size_t domain_id,
                         bool verbose = false);
    ~shm_cli_ipc();
    void InitChannel(std::string extra_info = "");
    void reset_message(const std::shared_ptr<ServiceData>& msg);
    bool send_request(std::shared_ptr<ServiceData>& request, uint64_t rev_tm = std::numeric_limits<uint32_t>::max());
    /* 禁用拷贝 */
    shm_cli_ipc(const shm_cli_ipc&) = delete;
    shm_cli_ipc& operator=(const shm_cli_ipc&) = delete;

protected:
    void cli_handshake();

private:
    size_t domain_id_{0};
    std::atomic<bool> running{true};
    std::atomic<bool> handshake_completed{false};
    bool verbose_{true};
    std::shared_ptr<ServiceData> message_;
    std::string topic_name_;
    std::shared_ptr<ipc::server> ipc_r_ptr_;
    std::shared_ptr<ipc::server> ipc_w_ptr_;
    std::vector<char> buf_;
    std::vector<char> response_buf_;
    std::thread* handshake_thread_{nullptr};
    dzIPC::info_pool::ScopedRegistration pool_reg_;
};
}   // namespace shm
}   // namespace dzIPC
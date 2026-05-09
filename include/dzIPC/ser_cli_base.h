#pragma once
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "dzIPC/common/srv_data.h"

namespace dzIPC {
class IPC_EXPORT ser_ipc_base
{
public:
    explicit ser_ipc_base(const std::string& topic_name, const std::shared_ptr<ServiceData>& msg,
                          std::function<void(std::shared_ptr<ServiceData>&)> callback, size_t domain_id, bool verbose)
    {}

    ~ser_ipc_base() = default;
    virtual void reset_message(const std::shared_ptr<ServiceData>& msg) = 0;
    virtual void reset_callback(std::function<void(std::shared_ptr<ServiceData>&)> callback) = 0;
    virtual void InitChannel(std::string extra_info) = 0;
    virtual bool handshake_completed() const = 0;
    std::atomic<bool> exit_flag{false};
};

class IPC_EXPORT cli_ipc_base
{
public:
    explicit cli_ipc_base(const std::string& topic_name, const std::shared_ptr<ServiceData>& msg, size_t domain_id,
                          bool verbose)
    {}

    ~cli_ipc_base() = default;
    virtual void InitChannel(std::string extra_info) = 0;
    virtual void reset_message(const std::shared_ptr<ServiceData>& msg) = 0;
    virtual bool send_request(std::shared_ptr<ServiceData>& request, uint64_t rev_tm) = 0;
    virtual bool handshake_completed() const = 0;
    std::atomic<bool> exit_flag{false};
};
}   // namespace dzIPC
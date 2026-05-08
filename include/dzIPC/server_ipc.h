#pragma once
#include "dzIPC/shm_ser_cli_ipc.h"
#include "dzIPC/socket_ser_cli_ipc.h"
#include "dzIPC/type.h"
#include "libipc/export.h"

namespace dzIPC {
namespace pimpl {
class IPC_EXPORT server_ipc_impl
{
public:
    explicit server_ipc_impl(const std::string& topic_name_, const std::shared_ptr<ServiceData>& msg,
                             std::function<void(std::shared_ptr<ServiceData>&)> callback, size_t domain_id,
                             IPCType ipc_type, bool verbose = false);
    ~server_ipc_impl();
    void InitChannel(std::string extra_info = "");
    void reset_message(const std::shared_ptr<ServiceData>& msg);
    void reset_callback(std::function<void(std::shared_ptr<ServiceData>&)> callback);
    bool exit_flag() const;

private:
    class server_ipc_impl_;
    server_ipc_impl_* p_;
};

class IPC_EXPORT client_ipc_impl
{
public:
    explicit client_ipc_impl(const std::string& topic_name_, const std::shared_ptr<ServiceData>& msg, size_t domain_id,
                             IPCType ipc_type, bool verbose = false);
    ~client_ipc_impl();
    void InitChannel(std::string extra_info = "");
    void reset_message(const std::shared_ptr<ServiceData>& msg);
    bool send_request(std::shared_ptr<ServiceData>& request, uint64_t rev_tm = std::numeric_limits<uint32_t>::max());
    bool exit_flag() const;

private:
    class client_ipc_impl_;
    client_ipc_impl_* p_;
};
}   // namespace pimpl
}   // namespace dzIPC
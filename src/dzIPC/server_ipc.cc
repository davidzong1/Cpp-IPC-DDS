#include "dzIPC/server_ipc.h"
#include <memory>
#include "libipc/utility/pimpl.h"

namespace dzIPC {
/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
/* 执行指针重定向实现继承多态 */
class pimpl::server_ipc_impl::server_ipc_impl_ : public ipc::pimpl<server_ipc_impl_>
{
public:
    std::unique_ptr<ser_ipc_base> ipc;
};

pimpl::server_ipc_impl::server_ipc_impl(const std::string& topic_name_, const std::shared_ptr<ServiceData>& msg,
                                        std::function<void(std::shared_ptr<ServiceData>&)> callback, size_t domain_id,
                                        IPCType ipc_type, bool verbose, bool enable_thread_qos, int cpu_id,
                                        int thread_priority)
    : p_(server_ipc_impl_::make())
{
    if (ipc_type == IPCType::Shm)
    {
        impl(p_)->ipc = std::make_unique<shm::shm_ser_ipc>(topic_name_, msg, callback, domain_id, verbose,
                                                           enable_thread_qos, cpu_id, thread_priority);
    }
    else if (ipc_type == IPCType::Socket)
    {
        impl(p_)->ipc = std::make_unique<socket::socket_ser_ipc>(topic_name_, msg, callback, domain_id, verbose,
                                                                 enable_thread_qos, cpu_id, thread_priority);
    }
    else
    {
        throw std::invalid_argument("Unsupported IPC type");
    }
}

pimpl::server_ipc_impl::~server_ipc_impl()
{
    ipc::clear_impl(p_);
}

void pimpl::server_ipc_impl::InitChannel(std::string extra_info)
{
    impl(p_)->ipc->InitChannel(extra_info);
}

void pimpl::server_ipc_impl::reset_message(const std::shared_ptr<ServiceData>& msg)
{
    impl(p_)->ipc->reset_message(msg);
}

void pimpl::server_ipc_impl::reset_callback(std::function<void(std::shared_ptr<ServiceData>&)> callback)
{
    impl(p_)->ipc->reset_callback(callback);
}

bool pimpl::server_ipc_impl::exit_flag() const
{
    return impl(p_)->ipc->exit_flag.load(std::memory_order_acquire);
}

bool pimpl::server_ipc_impl::handshake_completed() const
{
    return impl(p_)->ipc->handshake_completed();
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
/* 执行指针重定向实现继承多态 */
class pimpl::client_ipc_impl::client_ipc_impl_ : public ipc::pimpl<client_ipc_impl_>
{
public:
    std::unique_ptr<cli_ipc_base> ipc;
};

pimpl::client_ipc_impl::client_ipc_impl(const std::string& topic_name_, const std::shared_ptr<ServiceData>& msg,
                                        size_t domain_id, IPCType ipc_type, bool verbose, bool enable_thread_qos,
                                        int cpu_id, int thread_priority)
    : p_(client_ipc_impl_::make())
{
    if (ipc_type == IPCType::Shm)
    {
        impl(p_)->ipc = std::make_unique<shm::shm_cli_ipc>(topic_name_, msg, domain_id, verbose,
                                                           enable_thread_qos, cpu_id, thread_priority);
    }
    else if (ipc_type == IPCType::Socket)
    {
        impl(p_)->ipc = std::make_unique<socket::socket_cli_ipc>(topic_name_, msg, domain_id, verbose,
                                                                 enable_thread_qos, cpu_id, thread_priority);
    }
    else
    {
        throw std::invalid_argument("Unsupported IPC type");
    }
}

pimpl::client_ipc_impl::~client_ipc_impl()
{
    ipc::clear_impl(p_);
}

void pimpl::client_ipc_impl::InitChannel(std::string extra_info)
{
    impl(p_)->ipc->InitChannel(extra_info);
}

void pimpl::client_ipc_impl::reset_message(const std::shared_ptr<ServiceData>& msg)
{
    impl(p_)->ipc->reset_message(msg);
}

bool pimpl::client_ipc_impl::send_request(std::shared_ptr<ServiceData>& request, uint64_t rev_tm)
{
    return impl(p_)->ipc->send_request(request, rev_tm);
}

bool pimpl::client_ipc_impl::exit_flag() const
{
    return impl(p_)->ipc->exit_flag.load(std::memory_order_acquire);
}

bool pimpl::client_ipc_impl::handshake_completed() const
{
    return impl(p_)->ipc->handshake_completed();
}

}   // namespace dzIPC

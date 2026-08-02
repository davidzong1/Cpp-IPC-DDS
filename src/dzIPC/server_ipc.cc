#include "dzIPC/server_ipc.h"
#include <memory>
#include <new>
#include <typeinfo>
#include <utility>
#include "dzIPC/ipc_info_pool.h"
#include "dzIPC/logger/dzipc_log.h"
#include "libipc/utility/pimpl.h"

namespace dzIPC {
namespace {
std::function<void(std::shared_ptr<ServiceData>&)> wrap_server_callback(
    std::function<void(std::shared_ptr<ServiceData>&)> callback,
    std::string topic, size_t domain_id, IPCType ipc_type)
{
    return [callback = std::move(callback), topic = std::move(topic), domain_id, ipc_type]
        (std::shared_ptr<ServiceData>& data)
    {
        const auto transport = (ipc_type == IPCType::Shm) ? logger::TransportKind::kShm
                                                           : logger::TransportKind::kSocket;
        if (logger::IsDzipcLogRunning() && data && data->request())
        {
            try {
                auto& req = data->request();
                std::shared_ptr<IpcMsgBase> snapshot(req->clone());
                if (!snapshot) throw std::bad_alloc();
                ipc::buffer buf = snapshot->serialize();
                logger::LogEvent ev{};
                ev.timestamp_ns = logger::NowNs();
                ev.topic = topic;
                ev.type_name = info_pool::demangle(typeid(*req).name());
                ev.domain_id = static_cast<uint32_t>(domain_id);
                ev.msg_id = req->msg_id();
                ev.transport = transport;
                ev.role = logger::RoleKind::kServer;
                ev.event_kind = logger::EventKind::kRequest;
                if (buf.size() > 0) {
                    ev.payload = std::make_shared<std::vector<uint8_t>>(
                        static_cast<const uint8_t*>(buf.data()),
                        static_cast<const uint8_t*>(buf.data()) + buf.size());
                }
                logger::RecordEvent(std::move(ev));
            } catch (...) {
                // Logging must not prevent the service callback from running.
            }
        }

        if (callback) callback(data);

        if (logger::IsDzipcLogRunning() && data && data->response())
        {
            try {
                auto& resp = data->response();
                std::shared_ptr<IpcMsgBase> snapshot(resp->clone());
                if (!snapshot) throw std::bad_alloc();
                ipc::buffer buf = snapshot->serialize();
                logger::RecordResponse(topic, info_pool::demangle(typeid(*resp).name()),
                                       static_cast<uint32_t>(domain_id), resp->msg_id(), transport,
                                       static_cast<const uint8_t*>(buf.data()), buf.size());
            } catch (...) {
                // Logging must not alter the response path.
            }
        }
    };
}
}   // namespace
/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
/* 执行指针重定向实现继承多态 */
class pimpl::server_ipc_impl::server_ipc_impl_ : public ipc::pimpl<server_ipc_impl_>
{
public:
    std::unique_ptr<ser_ipc_base> ipc;
    std::string topic_name;
    size_t domain_id{0};
    IPCType ipc_type{IPCType::Shm};
};

pimpl::server_ipc_impl::server_ipc_impl(const std::string& topic_name_, const std::shared_ptr<ServiceData>& msg,
                                        std::function<void(std::shared_ptr<ServiceData>&)> callback, size_t domain_id,
                                        IPCType ipc_type, bool verbose, bool enable_thread_qos, int cpu_id,
                                        int thread_priority)
    : p_(server_ipc_impl_::make())
{
    impl(p_)->topic_name = topic_name_;
    impl(p_)->domain_id = domain_id;
    impl(p_)->ipc_type = ipc_type;
    callback = wrap_server_callback(std::move(callback), topic_name_, domain_id, ipc_type);
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
    impl(p_)->ipc->reset_callback(
        wrap_server_callback(std::move(callback), impl(p_)->topic_name,
                             impl(p_)->domain_id, impl(p_)->ipc_type));
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
    std::string topic_name;
    size_t domain_id{0};
    IPCType ipc_type{IPCType::Shm};
};

pimpl::client_ipc_impl::client_ipc_impl(const std::string& topic_name_, const std::shared_ptr<ServiceData>& msg,
                                        size_t domain_id, IPCType ipc_type, bool verbose, bool enable_thread_qos,
                                        int cpu_id, int thread_priority)
    : p_(client_ipc_impl_::make())
{
    impl(p_)->topic_name = topic_name_;
    impl(p_)->domain_id = domain_id;
    impl(p_)->ipc_type = ipc_type;
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
    // Logger hook: record service request
    if (dzIPC::logger::IsDzipcLogRunning()) {
        try {
            auto transport = (impl(p_)->ipc_type == IPCType::Shm) ? dzIPC::logger::TransportKind::kShm
                                                                  : dzIPC::logger::TransportKind::kSocket;
            auto& req_msg = request->request();
            std::shared_ptr<IpcMsgBase> snapshot(req_msg->clone());
            if (!snapshot) throw std::bad_alloc();
            ipc::buffer buf = snapshot->serialize();
            dzIPC::logger::RecordRequest(impl(p_)->topic_name,
                                         dzIPC::info_pool::demangle(typeid(*req_msg).name()),
                                         static_cast<uint32_t>(impl(p_)->domain_id),
                                         req_msg->msg_id(), transport,
                                         static_cast<const uint8_t*>(buf.data()), buf.size());
        } catch (...) {
            // Logging is diagnostic and must never change request behavior.
        }
    }
    const bool ok = impl(p_)->ipc->send_request(request, rev_tm);
    if (ok && dzIPC::logger::IsDzipcLogRunning() && request && request->response()) {
        try {
            auto transport = (impl(p_)->ipc_type == IPCType::Shm) ? dzIPC::logger::TransportKind::kShm
                                                                  : dzIPC::logger::TransportKind::kSocket;
            auto& resp = request->response();
            std::shared_ptr<IpcMsgBase> snapshot(resp->clone());
            if (!snapshot) throw std::bad_alloc();
            ipc::buffer buf = snapshot->serialize();
            dzIPC::logger::LogEvent ev{};
            ev.timestamp_ns = dzIPC::logger::NowNs();
            ev.topic = impl(p_)->topic_name;
            ev.type_name = dzIPC::info_pool::demangle(typeid(*resp).name());
            ev.domain_id = static_cast<uint32_t>(impl(p_)->domain_id);
            ev.msg_id = resp->msg_id();
            ev.transport = transport;
            ev.role = dzIPC::logger::RoleKind::kClient;
            ev.event_kind = dzIPC::logger::EventKind::kResponse;
            if (buf.size() > 0) {
                ev.payload = std::make_shared<std::vector<uint8_t>>(
                    static_cast<const uint8_t*>(buf.data()),
                    static_cast<const uint8_t*>(buf.data()) + buf.size());
            }
            dzIPC::logger::RecordEvent(std::move(ev));
        } catch (...) {
            // Logging is diagnostic and must never change response behavior.
        }
    }
    return ok;
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

#include "dzIPC/server_ipc.h"
#include <memory>
#include <new>
#include <typeinfo>
#include <utility>
#include "dzIPC/auto_ser_cli_ipc.h"
#include "dzIPC/ipc_info_pool.h"
#include "dzIPC/logger/dzipc_log.h"
#include "libipc/utility/pimpl.h"

namespace dzIPC {
namespace {

IPCType normalize_sercli_type(IPCType t) noexcept
{
    return t == IPCType::Socket ? IPCType::Auto : t;
}

template<typename Leg>
logger::TransportKind live_transport_kind(const Leg* leg, IPCType ipc_type) noexcept
{
    const path::Kind k = leg ? leg->transport_current() : path::Kind::None;
    if (k == path::Kind::Shm)
    {
        return logger::TransportKind::kShm;
    }
    if (k == path::Kind::Socket)
    {
        return logger::TransportKind::kSocket;
    }
    return ipc_type == IPCType::Shm ? logger::TransportKind::kShm : logger::TransportKind::kSocket;
}

/* 服务端回调的传输取值器。为什么不直接传 ipc_type: 回调是在**建腿之前**包好的
 * (腿要把回调收进去), 那一刻只拿得到构造期类型; 而腿的活值要等回调被调用时才有。
 * 所以传一个取值器, 在调用点求值。 */
using TransportGetter = std::function<logger::TransportKind()>;

std::function<void(std::shared_ptr<ServiceData>&)> wrap_server_callback(
    std::function<void(std::shared_ptr<ServiceData>&)> callback,
    std::string topic, size_t domain_id, TransportGetter transport_of)
{
    return [callback = std::move(callback), topic = std::move(topic), domain_id,
            transport_of = std::move(transport_of)]
        (std::shared_ptr<ServiceData>& data)
    {
        if (logger::IsDzipcLogRunning() && data)
        {
            const auto transport = transport_of();
            try {
                /* owned_copy: DZFlat 视图槽也能物化成 owning 供序列化记录。 */
                auto snapshot = data->request_owned_copy();
                if (!snapshot) throw std::bad_alloc();
                ipc::buffer buf = snapshot->serialize();
                logger::LogEvent ev{};
                ev.timestamp_ns = logger::NowNs();
                ev.topic = topic;
                ev.type_name = info_pool::demangle(typeid(*snapshot).name());
                ev.domain_id = static_cast<uint32_t>(domain_id);
                ev.msg_id = snapshot->msg_id();
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

        if (logger::IsDzipcLogRunning() && data)
        {
            const auto transport = transport_of();
            try {
                auto snapshot = data->response_owned_copy();
                if (!snapshot) throw std::bad_alloc();
                ipc::buffer buf = snapshot->serialize();
                logger::RecordResponse(topic, info_pool::demangle(typeid(*snapshot).name()),
                                       static_cast<uint32_t>(domain_id), snapshot->msg_id(),
                                       transport, static_cast<const uint8_t*>(buf.data()),
                                       buf.size());
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
    /* 记录用**归一化**值(IPC_SOCKET → Auto): 日志的回落映射读构造期类型, 不归一化会把
     * 自动选路记成强制 socket, 排查时把人引到错方向。
     * 分派用**用户原始意图**: Socket 与 Auto 都落到自动选路, SocketOnly 落到纯 socket。 */
    const IPCType normalized = normalize_sercli_type(ipc_type);
    impl(p_)->ipc_type = normalized;
    callback = wrap_server_callback(
        std::move(callback), topic_name_, domain_id,
        [this, normalized]() noexcept { return live_transport_kind(impl(p_) ? impl(p_)->ipc.get() : nullptr, normalized); });
    if (ipc_type == IPCType::Shm)
    {
        impl(p_)->ipc = std::make_unique<shm::shm_ser_ipc>(topic_name_, msg, callback, domain_id, verbose,
                                                           enable_thread_qos, cpu_id, thread_priority);
    }
    else if (ipc_type == IPCType::Socket || ipc_type == IPCType::Auto)
    {
        /* Socket 走这里而不是纯 socket 腿 —— 这就是"IPC_SOCKET 强制自动选路"的落点。 */
        impl(p_)->ipc = std::make_unique<autopath::auto_ser_ipc>(topic_name_, msg, callback, domain_id,
                                                                 autopath::Options{}, verbose, enable_thread_qos,
                                                                 cpu_id, thread_priority);
    }
    else if (ipc_type == IPCType::SocketOnly)
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
    impl(p_)->ipc->reset_callback(wrap_server_callback(
        std::move(callback), impl(p_)->topic_name, impl(p_)->domain_id,
        [this]() noexcept {
            return live_transport_kind(impl(p_) ? impl(p_)->ipc.get() : nullptr, impl(p_)->ipc_type);
        }));
}

bool pimpl::server_ipc_impl::exit_flag() const
{
    return impl(p_)->ipc->exit_flag.load(std::memory_order_acquire);
}

bool pimpl::server_ipc_impl::handshake_completed() const
{
    return impl(p_)->ipc->handshake_completed();
}

path::Kind pimpl::server_ipc_impl::transport_current() const
{
    return impl(p_)->ipc->transport_current();
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
    /* 同服务端: 记录归一化值, 分派按原始意图。 */
    const IPCType normalized = normalize_sercli_type(ipc_type);
    impl(p_)->ipc_type = normalized;
    if (ipc_type == IPCType::Shm)
    {
        impl(p_)->ipc = std::make_unique<shm::shm_cli_ipc>(topic_name_, msg, domain_id, verbose,
                                                           enable_thread_qos, cpu_id, thread_priority);
    }
    else if (ipc_type == IPCType::Socket || ipc_type == IPCType::Auto)
    {
        impl(p_)->ipc = std::make_unique<autopath::auto_cli_ipc>(topic_name_, msg, domain_id, autopath::Options{},
                                                                 verbose, enable_thread_qos, cpu_id, thread_priority);
    }
    else if (ipc_type == IPCType::SocketOnly)
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
            auto transport = live_transport_kind(impl(p_)->ipc.get(), impl(p_)->ipc_type);
            auto snapshot = request->request_owned_copy();
            if (!snapshot) throw std::bad_alloc();
            ipc::buffer buf = snapshot->serialize();
            dzIPC::logger::RecordRequest(impl(p_)->topic_name,
                                         dzIPC::info_pool::demangle(typeid(*snapshot).name()),
                                         static_cast<uint32_t>(impl(p_)->domain_id),
                                         snapshot->msg_id(), transport,
                                         static_cast<const uint8_t*>(buf.data()), buf.size());
        } catch (...) {
            // Logging is diagnostic and must never change request behavior.
        }
    }
    const bool ok = impl(p_)->ipc->send_request(request, rev_tm);
    if (ok && dzIPC::logger::IsDzipcLogRunning() && request) {
        try {
            auto transport = live_transport_kind(impl(p_)->ipc.get(), impl(p_)->ipc_type);
            auto snapshot = request->response_owned_copy();
            if (!snapshot) throw std::bad_alloc();
            ipc::buffer buf = snapshot->serialize();
            dzIPC::logger::LogEvent ev{};
            ev.timestamp_ns = dzIPC::logger::NowNs();
            ev.topic = impl(p_)->topic_name;
            ev.type_name = dzIPC::info_pool::demangle(typeid(*snapshot).name());
            ev.domain_id = static_cast<uint32_t>(impl(p_)->domain_id);
            ev.msg_id = snapshot->msg_id();
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

path::Kind pimpl::client_ipc_impl::transport_current() const
{
    return impl(p_)->ipc->transport_current();
}

}   // namespace dzIPC

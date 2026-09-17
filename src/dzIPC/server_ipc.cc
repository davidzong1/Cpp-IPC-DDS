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

/* ser-cli 的 IPC_SOCKET 走自动选路 —— 调用方无需(也无从)手动选。
 *
 * 理由: 调用方传 IPC_SOCKET 表达的意图是"**跨主机**通信", 而不是"禁止共享内存"。
 * socket 腿本身就能跨主机; 同主机时它只是绕远路(两个进程之间还要穿内核 UDP 栈两遍)。
 * 让自动选路去裁定"同机切 SHM、跨机保持 socket", 正是这个意图的正确实现。
 *
 * 与 pub/sub 的区别: 自动选路依赖 ser-cli 的握手通道做两阶段裁定, pub/sub 没有这条
 * 通道 —— 那边对"自动选路"**没有实现**(见 topic_ipc.cc)。
 *
 * 想让 ser-cli **只用** socket(对照实验/基线/排查), 用 IPC_SOCKET_ONLY —— 它是唯一
 * 表达这个意图的入口。不要靠"传 IPC_SOCKET 再指望它别切"来表达: 那正是被自动选路
 * 接管的那个。
 *
 * 注: 这里**不再**把 IPC_SOCKET 归一化成单独的枚举值。曾经这么做过(归一化成 Auto),
 * 但变异测试证明它没有任何可观测行为 —— 存进去的 ipc_type 只被 live_transport_kind
 * 当**回落**用, 而那里的判据只需区分"是不是 Shm", Socket 与 SocketOnly 落到同一个
 * 结果(改成恒等后 49/49 仍全绿)。既然不改变任何行为就删掉, 免得读代码的人以为
 * 那里有语义。 */

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
    /* 原样记录用户传的类型: 它只被日志的**回落**映射用, 而那里的判据只需区分
     * "是不是 Shm"(见 live_transport_kind)。 */
    impl(p_)->ipc_type = ipc_type;
    callback = wrap_server_callback(
        std::move(callback), topic_name_, domain_id,
        [this, ipc_type]() noexcept { return live_transport_kind(impl(p_) ? impl(p_)->ipc.get() : nullptr, ipc_type); });
    if (ipc_type == IPCType::Shm)
    {
        impl(p_)->ipc = std::make_unique<shm::shm_ser_ipc>(topic_name_, msg, callback, domain_id, verbose,
                                                           enable_thread_qos, cpu_id, thread_priority);
    }
    else if (ipc_type == IPCType::Socket)
    {
        /* Socket 走这里而不是纯 socket 腿 —— 这就是"IPC_SOCKET 走自动选路"的落点。 */
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
    /* 同服务端: 原样记录, 按用户意图分派。 */
    impl(p_)->ipc_type = ipc_type;
    if (ipc_type == IPCType::Shm)
    {
        impl(p_)->ipc = std::make_unique<shm::shm_cli_ipc>(topic_name_, msg, domain_id, verbose,
                                                           enable_thread_qos, cpu_id, thread_priority);
    }
    else if (ipc_type == IPCType::Socket)
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

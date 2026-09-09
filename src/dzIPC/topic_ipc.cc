#include "dzIPC/topic_ipc.h"
#include <memory>
#include <typeinfo>
#include <utility>
#include "dzIPC/ipc_info_pool.h"
#include "dzIPC/logger/dzipc_log.h"
#include "libipc/utility/pimpl.h"

namespace dzIPC {
/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
namespace {
// Logger hook helper for publish events
void log_publish_event(const std::string& topic, size_t domain_id, IPCType ipc_type,
                       std::shared_ptr<IpcMsgBase> msg) {
    if (!logger::IsDzipcLogRunning()) return;
    try {
        auto transport = (ipc_type == IPCType::Shm) ? logger::TransportKind::kShm
                                                     : logger::TransportKind::kSocket;
        std::shared_ptr<IpcMsgBase> snapshot(msg->clone());
        if (!snapshot) return;
        ipc::buffer buf = snapshot->serialize();
        logger::RecordPublish(topic, info_pool::demangle(typeid(*msg).name()),
                              static_cast<uint32_t>(domain_id),
                              msg->msg_id(), transport,
                              static_cast<const uint8_t*>(buf.data()), buf.size());
    } catch (...) {
        // Logging is diagnostic and must never change publish behavior.
    }
}

void log_subscribe_event(const std::string& topic, size_t domain_id, IPCType ipc_type,
                         const std::shared_ptr<IpcMsgBase>& msg) {
    if (!msg || !logger::IsDzipcLogRunning()) return;
    try {
        std::shared_ptr<IpcMsgBase> snapshot(msg->clone());
        if (!snapshot) return;
        ipc::buffer buf = snapshot->serialize();
        logger::LogEvent ev{};
        ev.timestamp_ns = logger::NowNs();
        ev.topic = topic;
        ev.type_name = info_pool::demangle(typeid(*msg).name());
        ev.domain_id = static_cast<uint32_t>(domain_id);
        ev.msg_id = msg->msg_id();
        ev.transport = (ipc_type == IPCType::Shm) ? logger::TransportKind::kShm
                                                   : logger::TransportKind::kSocket;
        ev.role = logger::RoleKind::kSubscriber;
        ev.event_kind = logger::EventKind::kPublish;
        if (buf.size() > 0) {
            ev.payload = std::make_shared<std::vector<uint8_t>>(
                static_cast<const uint8_t*>(buf.data()),
                static_cast<const uint8_t*>(buf.data()) + buf.size());
        }
        logger::RecordEvent(std::move(ev));
    } catch (...) {
        // Logging is diagnostic and must never change receive behavior.
    }
}
}  // namespace
/* 执行指针重定向实现继承多态 */
class pimpl::publisher_ipc_impl::publisher_ipc_impl_ : public ipc::pimpl<publisher_ipc_impl_>
{
public:
    std::unique_ptr<pub_ipc_base> ipc;
    std::string topic_name;
    size_t domain_id{0};
    IPCType ipc_type{IPCType::Shm};
};

pimpl::publisher_ipc_impl::publisher_ipc_impl(const std::shared_ptr<TopicData>& msg, const std::string& topic_name,
                                              size_t domain_id, IPCType ipc_type, bool verbose,
                                              bool enable_thread_qos, int cpu_id, int thread_priority)
    : p_(publisher_ipc_impl_::make())
{
    impl(p_)->topic_name = topic_name;
    impl(p_)->domain_id = domain_id;
    impl(p_)->ipc_type = ipc_type;
    if (ipc_type == IPCType::Shm)
    {
        impl(p_)->ipc = std::make_unique<shm::shm_pub_ipc>(msg, topic_name, domain_id, verbose,
                                                           enable_thread_qos, cpu_id, thread_priority);
    }
    else if (ipc_type == IPCType::Socket)
    {
        impl(p_)->ipc = std::make_unique<socket::socket_pub_ipc>(msg, topic_name, domain_id, verbose,
                                                                 enable_thread_qos, cpu_id, thread_priority);
    }
    else
    {
        throw std::invalid_argument("Unsupported IPC type");
    }
}

pimpl::publisher_ipc_impl::~publisher_ipc_impl()
{
    ipc::clear_impl(p_);
}

void pimpl::publisher_ipc_impl::InitChannel(std::string extra_info)
{
    impl(p_)->ipc->InitChannel(extra_info);
}

void pimpl::publisher_ipc_impl::reset_message(const std::shared_ptr<TopicData>& msg)
{
    impl(p_)->ipc->reset_message(msg);
}

bool pimpl::publisher_ipc_impl::publish(std::shared_ptr<IpcMsgBase> msg)
{
    log_publish_event(impl(p_)->topic_name, impl(p_)->domain_id, impl(p_)->ipc_type, msg);
    return impl(p_)->ipc->publish(msg);
}

bool pimpl::publisher_ipc_impl::publish_best_effort(std::shared_ptr<IpcMsgBase> msg)
{
    log_publish_event(impl(p_)->topic_name, impl(p_)->domain_id, impl(p_)->ipc_type, msg);
    return impl(p_)->ipc->publish_best_effort(msg);
}

bool pimpl::publisher_ipc_impl::publish_blocking(std::shared_ptr<IpcMsgBase> msg, std::uint64_t tm)
{
    log_publish_event(impl(p_)->topic_name, impl(p_)->domain_id, impl(p_)->ipc_type, msg);
    return impl(p_)->ipc->publish_blocking(msg, tm);
}

bool pimpl::publisher_ipc_impl::publish_for_sniffer(std::shared_ptr<IpcMsgBase> msg)
{
    log_publish_event(impl(p_)->topic_name, impl(p_)->domain_id, impl(p_)->ipc_type, msg);
    return impl(p_)->ipc->publish_for_sniffer(msg);
}

bool pimpl::publisher_ipc_impl::has_subscribed() const
{
    return impl(p_)->ipc->has_subscribed();
}

bool pimpl::publisher_ipc_impl::exit_flag() const
{
    return impl(p_)->ipc->exit_flag.load(std::memory_order_acquire);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
/* 执行指针重定向实现继承多态 */
class pimpl::subscriber_ipc_impl::subscriber_ipc_impl_ : public ipc::pimpl<subscriber_ipc_impl_>
{
public:
    std::unique_ptr<sub_ipc_base> ipc;
    std::string topic_name;
    size_t domain_id{0};
    IPCType ipc_type{IPCType::Shm};
};

pimpl::subscriber_ipc_impl::subscriber_ipc_impl(const std::shared_ptr<TopicData>& msg, const std::string& topic_name,
                                                size_t domain_id, const size_t queue_size, IPCType ipc_type,
                                                bool verbose, bool enable_thread_qos, int cpu_id, int thread_priority)
    : p_(subscriber_ipc_impl_::make())
{
    impl(p_)->topic_name = topic_name;
    impl(p_)->domain_id = domain_id;
    impl(p_)->ipc_type = ipc_type;
    if (ipc_type == IPCType::Shm)
    {
        impl(p_)->ipc = std::make_unique<shm::shm_sub_ipc>(msg, topic_name, domain_id, queue_size, verbose,
                                                           enable_thread_qos, cpu_id, thread_priority);
    }
    else if (ipc_type == IPCType::Socket)
    {
        impl(p_)->ipc = std::make_unique<socket::socket_sub_ipc>(msg, topic_name, domain_id, queue_size, verbose,
                                                                 enable_thread_qos, cpu_id, thread_priority);
    }
    else
    {
        throw std::invalid_argument("Unsupported IPC type");
    }
}

pimpl::subscriber_ipc_impl::~subscriber_ipc_impl()
{
    ipc::clear_impl(p_);
}

void pimpl::subscriber_ipc_impl::InitChannel(std::string extra_info)
{
    impl(p_)->ipc->InitChannel(extra_info);
}

void pimpl::subscriber_ipc_impl::reset_message(const std::shared_ptr<TopicData>& msg)
{
    impl(p_)->ipc->reset_message(msg);
}

/* 视图路径: 零拷贝借样, 无 owning 对象, 不记订阅事件日志。 */
void pimpl::subscriber_ipc_impl::get(Sample& out)
{
    impl(p_)->ipc->get(out);
}

bool pimpl::subscriber_ipc_impl::try_get(Sample& out)
{
    return impl(p_)->ipc->try_get(out);
}

void pimpl::subscriber_ipc_impl::get_clone(std::shared_ptr<TopicData>& msg)
{
    impl(p_)->ipc->get_clone(msg);
    if (msg && msg->topic()) {
        log_subscribe_event(impl(p_)->topic_name, impl(p_)->domain_id,
                            impl(p_)->ipc_type, msg->topic());
    }
}

bool pimpl::subscriber_ipc_impl::try_get_clone(std::shared_ptr<TopicData>& msg)
{
    const bool ok = impl(p_)->ipc->try_get_clone(msg);
    if (ok && msg && msg->topic()) {
        log_subscribe_event(impl(p_)->topic_name, impl(p_)->domain_id,
                            impl(p_)->ipc_type, msg->topic());
    }
    return ok;
}

bool pimpl::subscriber_ipc_impl::exit_flag() const
{
    return impl(p_)->ipc->exit_flag.load(std::memory_order_acquire);
}
}   // namespace dzIPC

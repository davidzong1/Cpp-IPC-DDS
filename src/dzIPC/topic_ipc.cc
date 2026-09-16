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
namespace {
/* ⛔ 发布/订阅**不支持** IPC_AUTO(T0 决策 1)。
 *
 * 为什么必须显式拒绝而不是让它落进通用分支:
 *   ① IPC_AUTO 现在是**导出的公共常量**(dzipc.h:19), 用户看得见就会试;
 *   ② 它的实现依赖 ser-cli 的握手通道做两阶段路径裁定(pub/sub 没有这条通道),
 *      所以这里**不可能**有正确实现 —— 只能拒绝;
 *   ③ 但拒绝必须是**可操作的**: 通用报错("Unsupported IPC type")会让调用方以为
 *      自己传了个非法枚举值, 而 Auto 是合法值、只是不适用于这条 API。真正的
 *      风险是调用方看到通用报错就去改成 IPC_SOCKET —— 那正是最糟的解法, 因为
 *      pub/sub 的同机最优解通常是 SHM。
 *
 * ⛔ 也绝不能静默按 socket 建链: 用户会以为拿到了"自动选路", 实际永远走 UDP,
 *    且没有任何地方告警(T0 决策清单 J-4 的公共 API 脚枪)。 */
[[noreturn]] void throw_auto_unsupported_for_pubsub(const char* api_name)
{
    throw std::invalid_argument(std::string(api_name) +
                                ": IPC_AUTO is not supported for publish/subscribe "
                                "(no handshake channel to negotiate the path); "
                                "pass IPC_SHM or IPC_SOCKET explicitly");
}
}   // namespace

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
    else if (ipc_type == IPCType::Auto)
    {
        throw_auto_unsupported_for_pubsub("publisher_ipc_impl");
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
    else if (ipc_type == IPCType::Auto)
    {
        throw_auto_unsupported_for_pubsub("subscriber_ipc_impl");
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

bool pimpl::subscriber_ipc_impl::get(Sample& out, std::uint64_t tm_ms)
{
    return impl(p_)->ipc->get(out, tm_ms);
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

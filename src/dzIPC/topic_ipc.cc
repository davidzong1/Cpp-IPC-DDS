#include "dzIPC/topic_ipc.h"
#include <memory>
#include "libipc/utility/pimpl.h"

namespace dzIPC {
/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
/* 执行指针重定向实现继承多态 */
class pimpl::publisher_ipc_impl::publisher_ipc_impl_ : public ipc::pimpl<publisher_ipc_impl_>
{
public:
    std::unique_ptr<pub_ipc_base> ipc;
};

pimpl::publisher_ipc_impl::publisher_ipc_impl(const std::shared_ptr<TopicData>& msg, const std::string& topic_name,
                                              size_t domain_id, IPCType ipc_type, bool verbose,
                                              bool enable_thread_qos, int cpu_id, int thread_priority)
    : p_(publisher_ipc_impl_::make())
{
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
    return impl(p_)->ipc->publish(msg);
}

bool pimpl::publisher_ipc_impl::publish_best_effort(std::shared_ptr<IpcMsgBase> msg)
{
    return impl(p_)->ipc->publish_best_effort(msg);
}

bool pimpl::publisher_ipc_impl::publish_blocking(std::shared_ptr<IpcMsgBase> msg, std::uint64_t tm)
{
    return impl(p_)->ipc->publish_blocking(msg, tm);
}

bool pimpl::publisher_ipc_impl::publish_for_sniffer(std::shared_ptr<IpcMsgBase> msg)
{
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
};

pimpl::subscriber_ipc_impl::subscriber_ipc_impl(const std::shared_ptr<TopicData>& msg, const std::string& topic_name,
                                                size_t domain_id, const size_t queue_size, IPCType ipc_type,
                                                bool verbose, bool enable_thread_qos, int cpu_id, int thread_priority)
    : p_(subscriber_ipc_impl_::make())
{
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

void pimpl::subscriber_ipc_impl::get(std::shared_ptr<TopicData>& msg)
{
    impl(p_)->ipc->get(msg);
}

bool pimpl::subscriber_ipc_impl::try_get(std::shared_ptr<TopicData>& msg)
{
    return impl(p_)->ipc->try_get(msg);
}

bool pimpl::subscriber_ipc_impl::exit_flag() const
{
    return impl(p_)->ipc->exit_flag.load(std::memory_order_acquire);
}
}   // namespace dzIPC

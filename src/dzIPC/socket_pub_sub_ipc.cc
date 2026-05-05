#include "dzIPC/socket_pub_sub_ipc.h"
#include <iostream>
#include <memory>
#include <typeinfo>
#include "dzIPC/common/data_rev.h"
#include "dzIPC/common/hash.h"
#include "dzIPC/common/name_operator.h"
#include "ipc_msg/ipc_msg_base/udp_id_init_msg.hpp"
#include "libipc/platform/detail.h"
#define ListenerWaitTime 1'000   // 1 second

namespace dzIPC {
namespace socket {
/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
socket_pub_ipc::socket_pub_ipc(const std::shared_ptr<TopicData>& msg, const std::string& topic_name, size_t domain_id,
                               bool verbose)
    : pub_ipc_base(msg, topic_name, domain_id, verbose)
    , topic_name_(topic_name)
    , domain_id_(domain_id)
    , verbose_(verbose)
{
    this->topic_msg_.reset(msg->clone());
    this->port_hash_ = dzIPC::common::udp_discovery_port_calculate(topic_name, domain_id);
    this->ipaddr_ = dzIPC::common::udp_discovery_addr_calculate(topic_name);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
socket_pub_ipc::~socket_pub_ipc()
{
    {
        std::lock_guard<std::mutex> lock(sleep_mtx);
        running.store(false, std::memory_order_release);
    }
    sleep_cv.notify_all();   // 立即唤醒正在 wait_for 的线程
    if (publisher_)
    {
        publisher_->close();
    }
    exit_flag.store(true, std::memory_order_release);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void socket_pub_ipc::reset_message(const std::shared_ptr<TopicData>& msg)
{
    topic_msg_.reset(msg->clone());
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void socket_pub_ipc::InitChannel(std::string extra_info)
{
    try
    {
        publisher_ = std::make_shared<ipc::socket::UDPNode>(this->topic_name_.c_str(), this->ipaddr_.c_str(),
                                                            this->port_hash_);
        if (verbose_)
            std::cerr << "\033[32m[" << topic_name_ << "PubInfo] Publisher initialized on IP: " << this->ipaddr_
                      << " Port: " << this->port_hash_ << " for topic: " << topic_name_ << "\033[0m" << std::endl;
        while (!publisher_->connect())
        {
            std::cerr << "\033[31m[" << topic_name_
                      << "PubInfo] Failed to connect publisher,reconnect affter 1 second...\033[0m" << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        std::string topic_type_name = topic_msg_->topic()
                                          ? dzIPC::info_pool::demangle(typeid(*topic_msg_->topic()).name())
                                          : std::string{};
        topic_type_name = extract_last_segment(topic_type_name);
        pool_reg_.rebind({dzIPC::info_pool::EntryKind::SocketPub, topic_name_, topic_type_name, "socket",
                          static_cast<int32_t>(domain_id_), extra_info});
    }
    catch (const std::exception& e)
    {
        std::cerr << "\033[31m[" << topic_name_ << "PubInfo] Error initializing channel: " << e.what() << "\033[0m"
                  << std::endl;
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool socket_pub_ipc::publish(std::shared_ptr<IpcMsgBase> msg)
{
    try
    {
        ipc::buffer response_data(std::move(msg->serialize()));
        if (!chunk_send(publisher_, response_data))
        {
            std::cerr << "\033[31m[" << topic_name_ << "PubInfo] Error publishing message: Failed to send"
                      << "\033[0m" << std::endl;
            return false;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "\033[31m[" << topic_name_ << "PubInfo] Error publishing message: " << e.what() << "\033[0m"
                  << std::endl;
        return false;
    }
    return true;
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
socket_sub_ipc::socket_sub_ipc(const std::shared_ptr<TopicData>& msg, const std::string& topic_name, size_t domain_id,
                               const size_t queue_size, bool verbose)
    : sub_ipc_base(msg, topic_name, domain_id, queue_size, verbose)
    , topic_name_(topic_name)
    , domain_id_(domain_id)
    , verbose_(verbose)
{
    this->topic_msg_.reset(msg->clone());
    this->port_hash_ = dzIPC::common::udp_discovery_port_calculate(topic_name, domain_id);
    this->ipaddr_ = dzIPC::common::udp_discovery_addr_calculate(topic_name);
    this->msg_queue_ = std::make_unique<CircularQueue<IpcMsgBase>>(queue_size);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
socket_sub_ipc::~socket_sub_ipc()
{
    running.store(false, std::memory_order_release);
    if (subscribe_thread_ != nullptr)
    {
        if (subscribe_thread_->joinable())
        {
            subscribe_thread_->join();
        }
        delete subscribe_thread_;
    }
    if (subscriber_)
        subscriber_->close();
    exit_flag.store(true, std::memory_order_release);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void socket_sub_ipc::reset_message(const std::shared_ptr<TopicData>& msg)
{
    if (!msg)
    {
        return;
    }
    topic_msg_.reset(msg->clone());
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void socket_sub_ipc::InitChannel(std::string extra_info)
{
    try
    {
        subscriber_ = std::make_shared<ipc::socket::UDPNode>(this->topic_name_.c_str(), this->ipaddr_.c_str(),
                                                             this->port_hash_);
        if (verbose_)
            std::cerr << "\033[32m[" << topic_name_ << "SubInfo] Subscriber initialized on IP: " << this->ipaddr_
                      << " Port: " << this->port_hash_ << " for topic: " << topic_name_ << "\033[0m" << std::endl;
        while (!subscriber_->connect())
        {
            std::cerr << "\033[31m[" << topic_name_
                      << "SubInfo] Failed to connect subscriber,reconnect affter 1 second...\033[0m" << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        std::string topic_type_name = topic_msg_->topic()
                                          ? dzIPC::info_pool::demangle(typeid(*topic_msg_->topic()).name())
                                          : std::string{};
        topic_type_name = extract_last_segment(topic_type_name);
        pool_reg_.rebind({dzIPC::info_pool::EntryKind::SocketSub, topic_name_, topic_type_name, "socket",
                          static_cast<int32_t>(domain_id_), extra_info});
        subscribe_thread_ = new std::thread(
            [this]()
            {
                while (running.load(std::memory_order_acquire))
                {
                    {
                        if (chunk_rev_topic(subscriber_, topic_msg_, 50))   // rev timeput 50ms
                        {
                            std::shared_ptr<IpcMsgBase> ptr_cache;
                            topic_msg_->swap(ptr_cache);
                            msg_queue_->push(std::move(ptr_cache));
                        }
                        else
                        {
                            continue;
                        }
                    }
                }
            });   // 占位线程，保持对象存活直到析构
    }
    catch (const std::exception& e)
    {
        std::cerr << "\033[31m[" << topic_name_ << "SubInfo] Error initializing channel: " << e.what() << "\033[0m"
                  << std::endl;
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void socket_sub_ipc::get(std::shared_ptr<TopicData>& msg)
{
    std::shared_ptr<IpcMsgBase> ipc_msg;
    msg_queue_->pop(ipc_msg);
    msg->update(ipc_msg);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool socket_sub_ipc::try_get(std::shared_ptr<TopicData>& msg)
{
    std::shared_ptr<IpcMsgBase> ipc_msg;
    if (msg_queue_->try_pop(ipc_msg))
    {
        msg->update(ipc_msg);
        return true;
    }
    else
    {
        return false;
    }
}

}   // namespace socket
}   // namespace dzIPC
#include <dzIPC/shm_pub_sub_ipc.h>
#include <fcntl.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <typeinfo>
#include "dzIPC/common/name_operator.h"

namespace dzIPC {
namespace shm {
enum class State {
    RunHS,
    StopHS
};
using namespace ipc;

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
shm_pub_ipc::shm_pub_ipc(const std::shared_ptr<TopicData>& msg, const std::string& topic_name, size_t domain_id,
                         bool verbose)
    : pub_ipc_base(msg, topic_name, domain_id, verbose)
    , topic_name_("dz_ipc_" + topic_name + "_topic")
    , raw_topic_name_(topic_name)
    , domain_id_(domain_id)
    , verbose_(verbose)
{
    topic_msg_.reset(msg->clone());
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
shm_pub_ipc::~shm_pub_ipc()
{
    running.store(false, std::memory_order_release);
    if (publish_thread_ != nullptr)
    {
        if (publish_thread_->joinable())
        {
            publish_thread_->join();
        }
        delete publish_thread_;
    }
    if (publisher_ && publisher_->valid())
    {
        publisher_->clear();
    }
    exit_flag.store(true, std::memory_order_release);
}

void shm_pub_ipc::reset_message(const std::shared_ptr<TopicData>& msg)
{
    topic_msg_.reset(msg->clone());
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void shm_pub_ipc::InitChannel(std::string extra_info)
{
    try
    {
        ipc::route::clear_storage(topic_name_.c_str());
        publisher_ = std::make_shared<ipc::route>(topic_name_.c_str(), ipc::sender, verbose_);
        publish_thread_ = new std::thread(&shm_pub_ipc::pub_handshake, this);
        std::string topic_type_name = topic_msg_->topic()
                                          ? dzIPC::info_pool::demangle(typeid(topic_msg_->topic()).name())
                                          : std::string{};
        topic_type_name = extract_last_segment(topic_type_name);
        pool_reg_.rebind({dzIPC::info_pool::EntryKind::ShmPub, raw_topic_name_, topic_type_name, "shm",
                          static_cast<int32_t>(domain_id_), extra_info});
        // publish_thread_ = std::thread(&shm_pub_ipc::sub_listener, this);
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
void shm_pub_ipc::pub_handshake()
{
    std::string pub_sem_name = "/" + topic_name_ + "_pub_node";
    std::string sub_sem_name = "/" + topic_name_ + "_sub_node";
    std::string stop_sem_name = "/" + topic_name_ + "_stop_node";
    ipc::sync::semaphore pub_node;
    ipc::sync::semaphore sub_node;
    ipc::sync::semaphore stop_node;
    int sub_cnt = 0;
    if (!pub_node.open(pub_sem_name.c_str(), 0))
    {
        std::cerr << "\033[31m[" << topic_name_
                  << "PubInfo] Error opening publisher semaphore for topic: " << topic_name_ << "\033[0m" << std::endl;
        throw std::runtime_error("Fatal error: sem_open failed");
    }
    if (!sub_node.open(sub_sem_name.c_str(), 0))
    {
        pub_node.close();
        std::cerr << "\033[31m[" << topic_name_
                  << "PubInfo] Error opening subscriber semaphore for topic: " << topic_name_ << "\033[0m" << std::endl;
        throw std::runtime_error("Fatal error: sem_open failed");
    }
    if (!stop_node.open(stop_sem_name.c_str(), 0))
    {
        pub_node.close();
        sub_node.close();
        std::cerr << "\033[31m[" << topic_name_ << "PubInfo] Error opening stop semaphore for topic: " << topic_name_
                  << "\033[0m" << std::endl;
        throw std::runtime_error("Fatal error: sem_open failed");
    }
    if (verbose_)
        std::cerr << "\033[32m[" << topic_name_ << "PubInfo] Publisher has created topic: " << topic_name_ << "\033[0m"
                  << std::endl;
    while (running.load(std::memory_order_acquire))
    {
        while (stop_node.try_wait())
            ;   // 清空停止信号量，防止之前的残留信号影响后续的订阅检测
        while (pub_node.try_wait())
            ;                     // 清空发布者信号量，防止之前的残留信号影响后续的订阅检测
        if (sub_node.wait(100))   // 每隔100ms检查一次订阅者是否存在
        {
            pub_node.post();
            sub_cnt++;
            subscribed_.store(true, std::memory_order_release);
            if (verbose_)
                std::cerr << "\033[32m[" << topic_name_
                          << "PubInfo] Publisher detected a "
                             "subscriber on topic: "
                          << topic_name_ << "\033[0m" << std::endl;
        }
    }
    for (int i = 0; i < sub_cnt; i++)
    {
        stop_node.post();   // 发送停止信号，通知所有订阅者退出等待
    }
    stop_node.close();
    pub_node.close();
    sub_node.close();
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool shm_pub_ipc::publish(std::shared_ptr<IpcMsgBase> msg)
{
    try
    {
        ipc::buffer response_data(std::move(msg->serialize()));
        int retry_count = 0;
        // no member pass
        while (!publisher_->no_member_try_send(response_data.data(), response_data.size()))
        {
            retry_count++;
            if (retry_count > 10)
            {
                std::cerr << "\033[31m[" << topic_name_
                          << "PubInfo] Warning: Failed to publish message after 10 attempts on topic: " << topic_name_
                          << "\033[0m" << std::endl;
                break;
            }
        };
        return true;
    }
    catch (const std::exception& e)
    {
        std::cerr << "\033[31m[" << topic_name_ << "PubInfo] Error publishing message: " << e.what() << "\033[0m"
                  << std::endl;
    }
    return false;
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
shm_sub_ipc::shm_sub_ipc(const std::shared_ptr<TopicData>& msg, const std::string& topic_name, size_t domain_id,
                         const size_t queue_size, bool verbose)
    : sub_ipc_base(msg, topic_name, domain_id, queue_size, verbose)
    , topic_name_("dz_ipc_" + topic_name + "_topic")
    , raw_topic_name_(topic_name)
    , domain_id_(domain_id)
    , verbose_(verbose)
{
    topic_msg_.reset(msg->clone());
    msg_queue_ = std::make_unique<CircularQueue<IpcMsgBase>>(queue_size);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
shm_sub_ipc::~shm_sub_ipc()
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
    if (sub_handshake_thread_ != nullptr)
    {
        if (sub_handshake_thread_->joinable())
        {
            sub_handshake_thread_->join();
        }
        delete sub_handshake_thread_;
    }
    if (subscriber_ && subscriber_->valid())
    {
        subscriber_->release();
    }
    exit_flag.store(true, std::memory_order_release);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void shm_sub_ipc::reset_message(const std::shared_ptr<TopicData>& msg)
{
    topic_msg_.reset(msg->clone());
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void shm_sub_ipc::sub_handshake()
{
    std::string pub_sem_name = "/" + topic_name_ + "_pub_node";
    std::string sub_sem_name = "/" + topic_name_ + "_sub_node";
    std::string stop_sem_name = "/" + topic_name_ + "_stop_node";
    ipc::sync::semaphore pub_node;
    ipc::sync::semaphore sub_node;
    ipc::sync::semaphore stop_node;
    if (!pub_node.open(pub_sem_name.c_str(), 0))
    {
        std::cerr << "\033[31m[" << topic_name_
                  << "SubInfo] Error opening publisher semaphore for topic: " << topic_name_ << "\033[0m" << std::endl;
        throw std::runtime_error("Fatal error: sem_open failed");
    }
    if (!sub_node.open(sub_sem_name.c_str(), 0))
    {
        pub_node.close();
        std::cerr << "\033[31m[" << topic_name_
                  << "SubInfo] Error opening subscriber semaphore for topic: " << topic_name_ << "\033[0m" << std::endl;
        throw std::runtime_error("Fatal error: sem_open failed");
    }
    if (!stop_node.open(stop_sem_name.c_str(), 0))
    {
        pub_node.close();
        sub_node.close();
        std::cerr << "\033[31m[" << topic_name_ << "SubInfo] Error opening stop semaphore for topic: " << topic_name_
                  << "\033[0m" << std::endl;
        throw std::runtime_error("Fatal error: sem_open failed");
    }
    auto st = State::RunHS;
    while (running.load(std::memory_order_acquire))
    {
        if (st == State::RunHS)
        {
            sub_node.try_wait();
            sub_node.post();   // 通知发布者自己已准备好
            if (pub_node.wait(100))
            {
                if (verbose_)
                    std::cerr << "\033[32m[" << topic_name_
                              << "SubInfo] Subscriber has subscribed to topic: " << topic_name_ << "\033[0m"
                              << std::endl;
                try
                {
                    subscriber_ = std::make_shared<ipc::route>(topic_name_.c_str(), ipc::receiver, verbose_);
                }
                catch (...)
                {
                    std::cerr << "\033[31m[" << topic_name_
                              << "SubInfo] Error initializing subscriber channel: " << topic_name_ << "\033[0m"
                              << std::endl;
                    throw std::runtime_error("[SubscriberInfo] Error initializing subscriber channel: " + topic_name_);
                }
                handshake_completed.store(true, std::memory_order_release);
                st = State::StopHS;
            }
        }
        else
        {
            if (stop_node.wait(100))
            {
                st = State::RunHS;   // 进入重新连接状态
                handshake_completed.store(false, std::memory_order_release);
            }
        }
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void shm_sub_ipc::InitChannel(std::string extra_info)
{
    std::string topic_type_name = topic_msg_->topic() ? dzIPC::info_pool::demangle(typeid(topic_msg_->topic()).name())
                                                      : std::string{};
    topic_type_name = extract_last_segment(topic_type_name);
    pool_reg_.rebind({dzIPC::info_pool::EntryKind::ShmSub, raw_topic_name_, topic_type_name, "shm",
                      static_cast<int32_t>(domain_id_), extra_info});
    sub_handshake_thread_ = new std::thread(&shm_sub_ipc::sub_handshake, this);
    subscribe_thread_ = new std::thread(
        [this]()
        {
            /* 进入订阅循环 */
            while (running.load(std::memory_order_acquire))
            {
                if (handshake_completed.load(std::memory_order_acquire))
                {
                    buff_t raw_data = subscriber_->recv(50);
                    if (raw_data.empty())
                    {
                        continue;
                    }
                    if (!topic_msg_->check_msg_id(raw_data))
                        continue;
                    topic_msg_->topic()->deserialize(raw_data);
                    std::shared_ptr<IpcMsgBase> ptr_cache;
                    topic_msg_->swap(ptr_cache);
                    msg_queue_->push(std::move(ptr_cache));
                }
                else
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));   // 每隔100ms检查一次发布者是否准备好
                }
            }
        });
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void shm_sub_ipc::get(std::shared_ptr<TopicData>& msg)
{
    std::shared_ptr<IpcMsgBase> ipc_msg;
    msg_queue_->pop(ipc_msg);
    msg->update(ipc_msg);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool shm_sub_ipc::try_get(std::shared_ptr<TopicData>& msg)
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
}   // namespace shm
}   // namespace dzIPC
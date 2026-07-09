#include <dzIPC/shm_pub_sub_ipc.h>
#include <fcntl.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <typeinfo>
#include "dzIPC/common/name_operator.h"

namespace dzIPC {
namespace shm {
using namespace ipc;
using dzIPC::control_plane_shm::TopicState;

namespace {

std::string control_name_for(const std::string& data_name)
{
    return data_name + "_control";
}

}   // namespace

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
shm_pub_ipc::shm_pub_ipc(const std::shared_ptr<TopicData>& msg, const std::string& topic_name, size_t domain_id,
                         bool verbose, bool enable_thread_qos, int cpu_id, int thread_priority)
    : pub_ipc_base(msg, topic_name, domain_id, verbose)
    , topic_name_("dz_ipc_" + topic_name + "_topic")
    , raw_topic_name_(topic_name)
    , domain_id_(domain_id)
    , verbose_(verbose)
    , thread_options_(dzIPC::ThreadDispatch::make_realtime_options(enable_thread_qos, cpu_id, thread_priority))
{
    topic_msg_.reset(msg->clone());
    dzIPC::ThreadDispatch::apply_current_thread_options(thread_options_, verbose_, topic_name_ + "_PubOwnerThread");
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
        if (!control_plane_.open(control_name_for(topic_name_)))
        {
            throw std::runtime_error("failed to open topic control plane");
        }
        control_plane_.begin_rebuild();
        ipc::route::clear_storage(topic_name_.c_str());
        publisher_ = std::make_shared<ipc::route>(topic_name_.c_str(), ipc::sender, verbose_);
        control_plane_.set_ready();
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
    if (verbose_)
        std::cerr << "\033[32m[" << topic_name_ << "PubInfo] Publisher has created topic: " << topic_name_ << "\033[0m"
                  << std::endl;
    bool had_subscriber = false;
    while (running.load(std::memory_order_acquire))
    {
        control_plane_.heartbeat();
        const bool has_peer = control_plane_.peer_count() > 0;
        subscribed_.store(has_peer, std::memory_order_release);
        if (has_peer && !had_subscriber && verbose_)
        {
            std::cerr << "\033[32m[" << topic_name_
                      << "PubInfo] Publisher detected a subscriber on topic: " << topic_name_ << "\033[0m"
                      << std::endl;
        }
        had_subscriber = has_peer;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    subscribed_.store(false, std::memory_order_release);
    control_plane_.set_stopping();
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool shm_pub_ipc::publish(std::shared_ptr<IpcMsgBase> msg)
{
    return publish_best_effort(std::move(msg));
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool shm_pub_ipc::publish_best_effort(std::shared_ptr<IpcMsgBase> msg)
{
    return publish_for_sniffer(std::move(msg));
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool shm_pub_ipc::publish_blocking(std::shared_ptr<IpcMsgBase> msg, std::uint64_t tm)
{
    try
    {
        ipc::buffer response_data(std::move(msg->serialize()));
        if (publisher_->recv_count() == 0)
        {
            return publisher_->no_member_try_send(response_data.data(), response_data.size(), 0);
        }
        return publisher_->try_send(response_data.data(), response_data.size(), tm);
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
bool shm_pub_ipc::publish_for_sniffer(std::shared_ptr<IpcMsgBase> msg)
{
    try
    {
        ipc::buffer response_data(std::move(msg->serialize()));
        return publisher_->no_member_try_send(response_data.data(), response_data.size(), 0);
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
                         const size_t queue_size, bool verbose, bool enable_thread_qos, int cpu_id,
                         int thread_priority)
    : sub_ipc_base(msg, topic_name, domain_id, queue_size, verbose)
    , topic_name_("dz_ipc_" + topic_name + "_topic")
    , raw_topic_name_(topic_name)
    , domain_id_(domain_id)
    , verbose_(verbose)
    , thread_options_(dzIPC::ThreadDispatch::make_realtime_options(enable_thread_qos, cpu_id, thread_priority))
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
    if (!msg)
    {
        return;
    }
    std::shared_ptr<TopicData> new_msg;
    new_msg.reset(msg->clone());
    std::lock_guard<std::mutex> lock(topic_msg_mtx_);
    topic_msg_ = std::move(new_msg);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void shm_sub_ipc::sub_handshake()
{
    if (!control_plane_.open(control_name_for(topic_name_)))
    {
        std::cerr << "\033[31m[" << topic_name_ << "SubInfo] Error opening control plane for topic: " << topic_name_
                  << "\033[0m" << std::endl;
        throw std::runtime_error("Fatal error: control plane open failed");
    }
    uint32_t attached_generation = 0;
    bool peer_registered = false;
    while (running.load(std::memory_order_acquire))
    {
        const uint32_t generation = control_plane_.generation();
        const TopicState state = control_plane_.state();
        if (state == TopicState::Ready && generation != 0)
        {
            if (!handshake_completed.load(std::memory_order_acquire) || attached_generation != generation)
            {
                if (peer_registered)
                {
                    control_plane_.remove_peer(attached_generation);
                    peer_registered = false;
                }
                {
                    std::lock_guard<std::mutex> lock(channel_mtx_);
                    if (subscriber_ && subscriber_->valid())
                    {
                        subscriber_->release();
                    }
                    subscriber_.reset();
                    subscriber_ = std::make_shared<ipc::route>(topic_name_.c_str(), ipc::receiver, verbose_);
                }
                attached_generation = generation;
                if (!control_plane_.add_peer(attached_generation))
                {
                    std::lock_guard<std::mutex> lock(channel_mtx_);
                    if (subscriber_ && subscriber_->valid())
                    {
                        subscriber_->release();
                    }
                    subscriber_.reset();
                    continue;
                }
                peer_registered = true;
                handshake_completed.store(true, std::memory_order_release);
                if (verbose_)
                {
                    std::cerr << "\033[32m[" << topic_name_
                              << "SubInfo] Subscriber has subscribed to topic: " << topic_name_ << "\033[0m"
                              << std::endl;
                }
            }
        }
        else
        {
            if (handshake_completed.exchange(false, std::memory_order_acq_rel))
            {
                if (peer_registered)
                {
                    control_plane_.remove_peer(attached_generation);
                    peer_registered = false;
                }
                std::lock_guard<std::mutex> lock(channel_mtx_);
                if (subscriber_ && subscriber_->valid())
                {
                    subscriber_->release();
                }
                subscriber_.reset();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (peer_registered)
    {
        control_plane_.remove_peer(attached_generation);
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void shm_sub_ipc::InitChannel(std::string extra_info)
{
    std::shared_ptr<TopicData> topic_template;
    {
        std::lock_guard<std::mutex> lock(topic_msg_mtx_);
        topic_template = topic_msg_;
    }
    std::string topic_type_name =
        (topic_template && topic_template->topic())
            ? dzIPC::info_pool::demangle(typeid(topic_template->topic()).name())
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
                    buff_t raw_data;
                    {
                        std::lock_guard<std::mutex> lock(channel_mtx_);
                        if (!subscriber_)
                        {
                            continue;
                        }
                        raw_data = subscriber_->recv(50);
                    }
                    if (raw_data.empty())
                    {
                        continue;
                    }
                    std::shared_ptr<TopicData> local_msg;
                    {
                        std::lock_guard<std::mutex> lock(topic_msg_mtx_);
                        if (!topic_msg_)
                        {
                            continue;
                        }
                        local_msg.reset(topic_msg_->clone());
                    }
                    if (!local_msg->check_msg_id(raw_data))
                        continue;
                    local_msg->topic()->deserialize(raw_data);
                    std::shared_ptr<IpcMsgBase> ptr_cache;
                    local_msg->swap(ptr_cache);
                    msg_queue_->push(std::move(ptr_cache));
                }
                else
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
            }
        });
    dzIPC::ThreadDispatch::apply_thread_options(subscribe_thread_, thread_options_, verbose_,
                                                topic_name_ + "_SubReceiveThread");
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

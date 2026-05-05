#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>
#include "dzIPC/common/circularqueue.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/ipc_info_pool.h"
#include "dzIPC/pub_sub_base.h"
#include "libipc/count_sem.h"
#include "libipc/ipc.h"
#include "libipc/semaphore.h"

namespace dzIPC {
namespace shm {
class shm_pub_ipc;
class shm_sub_ipc;

class shm_pub_ipc : public pub_ipc_base
{
public:
    explicit shm_pub_ipc(const std::shared_ptr<TopicData>& msg, const std::string& topic_name, size_t domain_id,
                         bool verbose = false);
    ~shm_pub_ipc();
    void reset_message(const std::shared_ptr<TopicData>& msg);
    void InitChannel(std::string extra_info = "");
    bool publish(std::shared_ptr<IpcMsgBase> msg);

    bool has_subscribed() const { return subscribed_; }

    /* 禁用拷贝 */
    shm_pub_ipc(const shm_pub_ipc&) = delete;
    shm_pub_ipc& operator=(const shm_pub_ipc&) = delete;

private:
    void pub_handshake();
    // void sub_listener();

private:
    size_t domain_id_{0};
    std::atomic<bool> subscribed_{false};
    bool verbose_{false};
    std::atomic<bool> running{true};
    std::string topic_name_;
    std::string raw_topic_name_;
    std::shared_ptr<ipc::route> publisher_;
    std::thread* publish_thread_;
    dzIPC::info_pool::ScopedRegistration pool_reg_;
    std::shared_ptr<TopicData> topic_msg_;
};

class shm_sub_ipc : public sub_ipc_base
{
public:
    explicit shm_sub_ipc(const std::shared_ptr<TopicData>& msg, const std::string& topic_name, size_t domain_id,
                         const size_t queue_size, bool verbose = false);
    ~shm_sub_ipc();
    void InitChannel(std::string extra_info = "");
    void reset_message(const std::shared_ptr<TopicData>& msg);
    void get(std::shared_ptr<TopicData>& msg);
    bool try_get(std::shared_ptr<TopicData>& msg);
    /* 禁用拷贝 */
    shm_sub_ipc(const shm_sub_ipc&) = delete;
    shm_sub_ipc& operator=(const shm_sub_ipc&) = delete;

private:
    void sub_handshake();

private:
    size_t domain_id_{0};
    std::atomic<bool> running{true};
    std::atomic<bool> handshake_completed{false};
    bool data_update_{false};
    bool verbose_{false};
    std::string topic_name_;
    std::string raw_topic_name_;
    std::shared_ptr<ipc::route> subscriber_;
    std::shared_ptr<TopicData> topic_msg_;
    std::unique_ptr<CircularQueue<IpcMsgBase>> msg_queue_;
    std::thread* subscribe_thread_;
    std::thread* sub_handshake_thread_;
    //
    ipc::sync::count_sem* empty_queue_;   // 用于通知订阅者消息队列中有新消息
    dzIPC::info_pool::ScopedRegistration pool_reg_;
};
}   // namespace shm
}   // namespace dzIPC
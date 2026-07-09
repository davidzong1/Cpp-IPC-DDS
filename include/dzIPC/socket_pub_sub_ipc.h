#pragma once
// #include <semaphore.h>
#include <atomic>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>
#include "dzIPC/common/circularqueue.h"
#include "dzIPC/common/thread_dispatch.h"
#include "dzIPC/common/topic_data.h"
#include "dzIPC/ipc_info_pool.h"
#include "dzIPC/pub_sub_base.h"
#include "libipc/udp.h"

namespace dzIPC {
namespace socket {
class socket_pub_ipc;
class socket_sub_ipc;

class IPC_EXPORT socket_pub_ipc : public pub_ipc_base
{
public:
    explicit socket_pub_ipc(const std::shared_ptr<TopicData>& msg, const std::string& topic_name, size_t domain_id,
                            bool verbose = false, bool enable_thread_qos = false, int cpu_id = -1,
                            int thread_priority = 20);
    ~socket_pub_ipc();
    void reset_message(const std::shared_ptr<TopicData>& msg);
    void InitChannel(std::string extra_info = "");
    bool publish(std::shared_ptr<IpcMsgBase> msg);

    bool has_subscribed() const { return subscribed_; }

    bool client_subscribed() const { return cli_cnt; }

    /* 禁用拷贝 */
    socket_pub_ipc(const socket_pub_ipc&) = delete;
    socket_pub_ipc& operator=(const socket_pub_ipc&) = delete;

private:
    size_t domain_id_{0};
    int cli_cnt{0};
    std::atomic<bool> subscribed_{false};
    bool verbose_{false};
    std::atomic<bool> running{true};
    std::string topic_name_;
    std::shared_ptr<ipc::socket::UDPNode> publisher_;
    uint16_t port_hash_;
    std::string ipaddr_;
    std::mutex sleep_mtx;
    std::condition_variable sleep_cv;
    dzIPC::info_pool::ScopedRegistration pool_reg_;
    std::shared_ptr<TopicData> topic_msg_;
    dzIPC::ThreadDispatch::ThreadOptions thread_options_;
};

class IPC_EXPORT socket_sub_ipc : public sub_ipc_base
{
public:
    explicit socket_sub_ipc(const std::shared_ptr<TopicData>& msg, const std::string& topic_name, size_t domain_id,
                            const size_t queue_size, bool verbose = false, bool enable_thread_qos = false,
                            int cpu_id = -1, int thread_priority = 20);
    ~socket_sub_ipc();
    void InitChannel(std::string extra_info = "");
    void reset_message(const std::shared_ptr<TopicData>& msg);
    void get(std::shared_ptr<TopicData>& msg);
    bool try_get(std::shared_ptr<TopicData>& msg);
    /* 禁用拷贝 */
    socket_sub_ipc(const socket_sub_ipc&) = delete;
    socket_sub_ipc& operator=(const socket_sub_ipc&) = delete;

private:
    size_t domain_id_{0};
    std::atomic<bool> subscribed_{false};
    std::atomic<bool> running{true};
    bool data_update_{false};
    bool verbose_{false};
    std::string topic_name_;
    uint16_t port_hash_;
    std::string ipaddr_;
    std::shared_ptr<ipc::socket::UDPNode> subscriber_;
    std::shared_ptr<TopicData> topic_msg_;
    std::mutex topic_msg_mtx_;
    std::unique_ptr<CircularQueue<IpcMsgBase>> msg_queue_;
    std::thread* subscribe_thread_{nullptr};
    dzIPC::info_pool::ScopedRegistration pool_reg_;
    dzIPC::ThreadDispatch::ThreadOptions thread_options_;
};
}   // namespace socket
}   // namespace dzIPC

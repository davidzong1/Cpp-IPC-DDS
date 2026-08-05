#include "dzIPC/socket_pub_sub_ipc.h"
#include <iostream>
#include <memory>
#include <typeinfo>
#include "dzIPC/common/data_rev.h"
#include "dzIPC/common/hash.h"
#include "dzIPC/common/local_pub_sub_registry.h"
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
                               bool verbose, bool enable_thread_qos, int cpu_id, int thread_priority)
    : pub_ipc_base(msg, topic_name, domain_id, verbose)
    , topic_name_(topic_name)
    , domain_id_(domain_id)
    , verbose_(verbose)
    , thread_options_(dzIPC::ThreadDispatch::make_realtime_options(enable_thread_qos, cpu_id, thread_priority))
{
    this->topic_msg_.reset(msg->clone());
    this->port_hash_ = dzIPC::common::udp_discovery_port_calculate(topic_name, domain_id);
    this->ipaddr_ = dzIPC::common::udp_discovery_addr_calculate(topic_name);
    dzIPC::ThreadDispatch::apply_current_thread_options(thread_options_, verbose_, topic_name_ + "_SocketPubOwnerThread");
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
    if (discovery_thread_ != nullptr)
    {
        if (discovery_thread_->joinable())
        {
            discovery_thread_->join();
        }
        delete discovery_thread_;
        discovery_thread_ = nullptr;
    }
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
    // Reset fast-path state: new message type requires re-confirmation.
    std::lock_guard<std::mutex> lock(fast_path_mtx_);
    fp_consecutive_ = 0;
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
        /* 本进程的 SocketPub 条目注册完成后再启动发现线程, 避免它先于
         * rebind 拿到不完整的池快照。重复 InitChannel 不再重复起线程。 */
        if (discovery_thread_ == nullptr)
        {
            discovery_thread_ = new std::thread(&socket_pub_ipc::discovery_loop, this);
        }
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
void socket_pub_ipc::discovery_loop()
{
    /* UDP 组播是单向的: 发布端把包投进组地址, 协议本身不会告诉它谁加入了组。
     * 因此这里退而求其次, 用进程外共享的 IpcInfoPool 统计本 topic/domain 下
     * 存活的 SocketSub 条目, 语义上对齐 SHM 的 shm_pub_ipc::pub_handshake()。
     *
     * 探测边界见头文件 has_subscribed() 注释: 池只覆盖走 dzIPC 注册的订阅者。*/
    bool had_subscriber = false;
    while (running.load(std::memory_order_acquire))
    {
        size_t total_subs = 0;
        bool pool_ok = true;
        try
        {
            auto pool_snap = info_pool::IpcInfoPool::instance().snapshot();
            for (auto& entry : pool_snap)
            {
                if (entry.kind == info_pool::EntryKind::SocketSub && entry.topic_name == topic_name_
                    && entry.domain_id == static_cast<int32_t>(domain_id_) && entry.alive && entry.in_use)
                {
                    ++total_subs;
                }
            }
        }
        catch (const std::exception& e)
        {
            /* 池暂时不可用: 保持上一次的判定, 不要误报为"无订阅者" */
            pool_ok = false;
            if (verbose_)
            {
                std::cerr << "\033[33m[" << topic_name_ << "PubInfo] IpcInfoPool snapshot failed: " << e.what()
                          << "; keeping previous has_subscribed() state\033[0m" << std::endl;
            }
        }

        if (pool_ok)
        {
            const bool has_peer = total_subs > 0;
            subscribed_.store(has_peer, std::memory_order_release);
            if (has_peer != had_subscriber && verbose_)
            {
                std::cerr << "\033[32m[" << topic_name_ << "PubInfo] "
                          << (has_peer ? "Publisher detected a subscriber on topic: "
                                       : "Publisher lost all subscribers on topic: ")
                          << topic_name_ << "\033[0m" << std::endl;
            }
            had_subscriber = has_peer;
        }

        std::unique_lock<std::mutex> lock(sleep_mtx);
        sleep_cv.wait_for(lock, std::chrono::milliseconds(kDiscoveryPollMs),
                          [this] { return !running.load(std::memory_order_acquire); });
    }
    subscribed_.store(false, std::memory_order_release);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool socket_pub_ipc::publish(std::shared_ptr<IpcMsgBase> msg)
{
    return publish_best_effort(std::move(msg));
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool socket_pub_ipc::publish_best_effort(std::shared_ptr<IpcMsgBase> msg)
{
    // --- Intra-process fast path (socket nodelet) ---
    // Gated by the unified process-wide switch dzIPC::EnableNodelet(true).
    // When enabled, verify via IpcInfoPool that ALL known SocketSub entries
    // for this topic/domain reside in the current process.  Only then
    // clone-once + fanout, skipping UDP serialize+send.
    //
    // ---- Detection boundary (critical) ----
    // IpcInfoPool ONLY discovers subscribers registered through dzIPC's own
    // ScopedRegistration (socket_sub_ipc::InitChannel).  Native UDP listeners,
    // passive sniffers, raw-socket consumers, and any external tool reading
    // the UDP stream are INVISIBLE to this check.  When EnableNodelet is true
    // and the pool reports all-local, the fast path will bypass the UDP send
    // entirely — those invisible consumers receive NOTHING for that message.
    //
    // For debugging/monitoring: keep EnableNodelet false (default), or use
    // publish_for_sniffer() to force the UDP path for individual messages.
    //
    // ---- Stability gate ----
    // K=3 consecutive publishes with the same (key, local snapshot size,
    // IpcInfoPool SocketSub count) gates activation.  Any topology change
    // resets K to 0.
    //
    // TOCTOU risk: between the IpcInfoPool snapshot and the queue pushes, a
    // cross-process subscriber may join or leave.  K=3 dampens the window but
    // does not close it.  A late-joining remote sub will miss that message
    // (fast path skipped the UDP send).

    if (dzIPC::IsNodeletEnabled())
    {
        ChannelKey key{topic_name_, domain_id_, msg->msg_id(), ChannelKind::SocketPubSub};
        auto& reg = LocalPubSubRegistry::instance();
        auto snapshot = reg.subscriber_snapshot(key);

        // Query IpcInfoPool for total SocketSub count (all processes).
        size_t total_socket_subs = 0;
        {
            auto pool_snap = info_pool::IpcInfoPool::instance().snapshot();
            for (auto& entry : pool_snap)
            {
                if (entry.kind == info_pool::EntryKind::SocketSub
                    && entry.topic_name == topic_name_
                    && entry.domain_id == static_cast<int32_t>(domain_id_)
                    && entry.alive && entry.in_use)
                {
                    ++total_socket_subs;
                }
            }
        }

        bool use_fast_path = false;
        {
            std::lock_guard<std::mutex> lock(fast_path_mtx_);

            // Reset on any state change: key, local snapshot size, or pool count.
            if (!(key == last_fp_key_) || snapshot.size() != last_fp_snapshot_size_
                || total_socket_subs != last_fp_total_subs_)
            {
                fp_consecutive_ = 0;
                last_fp_key_ = key;
                last_fp_snapshot_size_ = snapshot.size();
                last_fp_total_subs_ = total_socket_subs;
            }

            // All-local check: must have local subs, pool must be non-empty,
            // and pool count must exactly match local count.
            if (!snapshot.empty() && total_socket_subs > 0
                && total_socket_subs == snapshot.size())
            {
                ++fp_consecutive_;
                if (fp_consecutive_ >= kFastPathConfirm)
                {
                    use_fast_path = true;
                }
            }
            else
            {
                fp_consecutive_ = 0;

                // One-shot warnings (per instance, per reason).
                if (snapshot.empty() && !warned_no_local_sub_)
                {
                    warned_no_local_sub_ = true;
                    std::cerr << "\033[33m[" << topic_name_
                              << "PubInfo] socket nodelet requested but unavailable "
                              << "(no local subscribers); falling back to standard UDP path\033[0m"
                              << std::endl;
                }
                else if (!snapshot.empty() && total_socket_subs == 0 && !warned_pool_unavailable_)
                {
                    warned_pool_unavailable_ = true;
                    std::cerr << "\033[33m[" << topic_name_
                              << "PubInfo] socket nodelet requested but unavailable "
                              << "(IpcInfoPool returned 0 SocketSub entries — pool may be unavailable); "
                              << "falling back to standard UDP path\033[0m"
                              << std::endl;
                }
                else if (!snapshot.empty() && total_socket_subs > 0
                         && total_socket_subs != snapshot.size() && !warned_cross_process_)
                {
                    warned_cross_process_ = true;
                    std::cerr << "\033[33m[" << topic_name_
                              << "PubInfo] socket nodelet requested but unavailable "
                              << "(cross-process SocketSub detected: local=" << snapshot.size()
                              << " total=" << total_socket_subs << "); "
                              << "falling back to standard UDP path\033[0m"
                              << std::endl;
                }
            }
        }

        if (use_fast_path)
        {
            // Clone once, fanout to all local queues.
            std::shared_ptr<IpcMsgBase> cloned(msg->clone());
            for (auto& q : snapshot)
            {
                q->push(cloned);
            }
            return true;
        }
    }

    // Fallback: standard UDP path
    try
    {
        ipc::buffer response_data(std::move(msg->serialize()));
        SocketSendOptions options;
        /* pub-sub 恒定 BestEffort, 不做"大包自动升级 Reliable"。
         *
         * 曾经尝试按 payload 尺寸自动切到 Reliable+CRC32C 以压低大包丢包率
         * (1 MB 时实测 4.7-7.4%), 但在 pub-sub 上不成立, 已撤销。两个原因:
         *
         * 1) ACK 收不到。pub-sub 收发共用同一条组播 socket 且 IP_MULTICAST_LOOP=1,
         *    发布端发出的分片会全部回绕到自己的接收队列 (64 KB = 45 片,
         *    1 MB = 713 片)。订阅端回的 ACK 排在这些 loopback 分片之后, 首轮
         *    等待窗口 (RTT 自适应后约 1 ms) 根本轮不到它 -> 每条消息都等满
         *    超时预算才失败。实测吞吐塌到 1 msg/s, 而丢包率仍是 0.00% ——
         *    数据面是通的, 坏的是确认机制本身。
         *
         * 2) 组播下 ACK 语义不成立。N 个订阅者时, chunk_send_ex 收到任意一个
         *    匹配 ACK 即判定 DeliveredAcked, 无法表达"谁收到了、谁没收到"。
         *
         * 结论: pub-sub 的可靠性不能靠反向 ACK 实现。要提升大包到达率, 正确
         * 方向是扩大接收端 net.core.rmem_max (实测丢包是缓冲溢出型突发, 64 MB
         * 缓冲下 1 MB payload 可跑满 110 MB/s 且零丢包), 或发送端限速, 或由
         * 业务侧切成小块各自独立投递。
         *
         * 同一缺陷也存在于 publish_blocking() —— 它在 socket 上走 Reliable,
         * 在 pub-sub 的单组播通道上同样收不到 ACK。 */
        options.delivery = SocketDeliveryMode::BestEffort;
        options.integrity = SocketIntegrityMode::None;
        const SocketSendReport report = chunk_send_ex(publisher_, response_data, options);
        if (!report.ok())
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
bool socket_pub_ipc::publish_blocking(std::shared_ptr<IpcMsgBase> msg, std::uint64_t tm)
{
    try
    {
        ipc::buffer response_data(std::move(msg->serialize()));
        SocketSendOptions options;
        options.delivery = SocketDeliveryMode::Reliable;
        options.integrity = SocketIntegrityMode::CRC32C;
        options.ack_timeout_ms = tm;
        const SocketSendReport report = chunk_send_ex(publisher_, response_data, options);
        if (!report.ok())
        {
            std::cerr << "\033[31m[" << topic_name_ << "PubInfo] Reliable publish failed with status "
                      << static_cast<int>(report.status) << "\033[0m" << std::endl;
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
bool socket_pub_ipc::publish_for_sniffer(std::shared_ptr<IpcMsgBase> msg)
{
    // Always use the standard UDP path — sniffers depend on UDP data.
    try
    {
        ipc::buffer response_data(std::move(msg->serialize()));
        SocketSendOptions options;
        options.delivery = SocketDeliveryMode::BestEffort;
        options.integrity = SocketIntegrityMode::None;
        const SocketSendReport report = chunk_send_ex(publisher_, response_data, options);
        if (!report.ok())
        {
            std::cerr << "\033[31m[" << topic_name_ << "PubInfo] Error publishing message (sniffer): Failed to send"
                      << "\033[0m" << std::endl;
            return false;
        }
    }
    catch (const std::exception& e)
    {
        std::cerr << "\033[31m[" << topic_name_ << "PubInfo] Error publishing message (sniffer): " << e.what() << "\033[0m"
                  << std::endl;
        return false;
    }
    return true;
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
socket_sub_ipc::socket_sub_ipc(const std::shared_ptr<TopicData>& msg, const std::string& topic_name, size_t domain_id,
                               const size_t queue_size, bool verbose, bool enable_thread_qos, int cpu_id,
                               int thread_priority)
    : sub_ipc_base(msg, topic_name, domain_id, queue_size, verbose)
    , topic_name_(topic_name)
    , domain_id_(domain_id)
    , verbose_(verbose)
    , thread_options_(dzIPC::ThreadDispatch::make_realtime_options(enable_thread_qos, cpu_id, thread_priority))
{
    this->topic_msg_.reset(msg->clone());
    this->port_hash_ = dzIPC::common::udp_discovery_port_calculate(topic_name, domain_id);
    this->ipaddr_ = dzIPC::common::udp_discovery_addr_calculate(topic_name);
    this->msg_queue_ = std::make_shared<CircularQueue<IpcMsgBase>>(queue_size);
    this->msg_id_ = topic_msg_->topic()->msg_id();
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
socket_sub_ipc::~socket_sub_ipc()
{
    // Deregister BEFORE stopping threads so fast-path publisher snapshots
    // can no longer include this queue while we shut down.
    {
        std::lock_guard<std::mutex> lock(topic_msg_mtx_);
        if (local_registered_)
        {
            ChannelKey key{topic_name_, domain_id_, msg_id_, ChannelKind::SocketPubSub};
            LocalPubSubRegistry::instance().unregister_subscriber(key, msg_queue_);
            local_registered_ = false;
        }
    }

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
    std::shared_ptr<TopicData> new_msg;
    new_msg.reset(msg->clone());
    const uint32_t new_msg_id = new_msg->topic()->msg_id();

    {
        std::lock_guard<std::mutex> lock(topic_msg_mtx_);
        topic_msg_ = std::move(new_msg);

        // If already registered (InitChannel completed) and msg_id changed,
        // re-register under the new key so publishers using the new msg_id
        // can find us via the fast path.
        if (local_registered_ && msg_id_ != new_msg_id)
        {
            auto& reg = LocalPubSubRegistry::instance();
            ChannelKey old_key{topic_name_, domain_id_, msg_id_, ChannelKind::SocketPubSub};
            ChannelKey new_key{topic_name_, domain_id_, new_msg_id, ChannelKind::SocketPubSub};
            reg.unregister_subscriber(old_key, msg_queue_);
            reg.register_subscriber(new_key, msg_queue_);
        }
        msg_id_ = new_msg_id;
    }
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
        std::shared_ptr<TopicData> topic_template;
        {
            std::lock_guard<std::mutex> lock(topic_msg_mtx_);
            topic_template = topic_msg_;
        }
        std::string topic_type_name = (topic_template && topic_template->topic())
                                          ? dzIPC::info_pool::demangle(typeid(*topic_template->topic()).name())
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
                        std::shared_ptr<TopicData> local_msg;
                        {
                            std::lock_guard<std::mutex> lock(topic_msg_mtx_);
                            if (!topic_msg_)
                            {
                                continue;
                            }
                            local_msg.reset(topic_msg_->clone());
                        }
                        if (chunk_rev_topic(subscriber_, local_msg, 50))   // rev timeput 50ms
                        {
                            std::shared_ptr<IpcMsgBase> ptr_cache;
                            local_msg->swap(ptr_cache);
                            msg_queue_->push(std::move(ptr_cache));
                        }
                        else
                        {
                            continue;
                        }
                    }
                }
            });   // 占位线程，保持对象存活直到析构
        dzIPC::ThreadDispatch::apply_thread_options(subscribe_thread_, thread_options_, verbose_,
                                                    topic_name_ + "_SocketSubReceiveThread");

        // Register for intra-process fast-path delivery (once only).
        {
            std::lock_guard<std::mutex> lock(topic_msg_mtx_);
            if (!local_registered_)
            {
                ChannelKey key{topic_name_, domain_id_, msg_id_, ChannelKind::SocketPubSub};
                LocalPubSubRegistry::instance().register_subscriber(key, msg_queue_);
                local_registered_ = true;
            }
        }
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

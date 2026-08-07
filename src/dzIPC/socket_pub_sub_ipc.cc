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
    if (ack_rx_)
    {
        ack_rx_->close();
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
        /* 数据通道设 SendOnly —— 不加入组播组, 于是收不到自己发出去的分片回绕。
         *
         * 这是 Reliable 模式能工作的前提: 之前收发共用一条入了组的 socket,
         * 1 MB 消息的 713 个分片全部回绕进自己的接收队列, 订阅端的 ACK 排在
         * 它们后面, 等待窗口必然先超时(实测吞吐塌到 1 msg/s 而丢包率 0.00%)。
         *
         * 注意不能改用 IP_MULTICAST_LOOP=0 达到同样目的 —— 那是主机级开关,
         * 会让本机所有进程都收不到, 同机 IPC 直接失效。详见 libipc/udp.h。 */
        publisher_ = std::make_shared<ipc::socket::UDPNode>(this->topic_name_.c_str(), this->ipaddr_.c_str(),
                                                            this->port_hash_, ipc::socket::NodeRole::SendOnly);
        /* ACK 回传通道: 订阅端把 ACK/NACK 发到这个端口, 上面没有自己的数据。 */
        ack_rx_ = std::make_shared<ipc::socket::UDPNode>(
            this->topic_name_.c_str(), this->ipaddr_.c_str(),
            static_cast<uint16_t>(this->port_hash_ + dzIPC::common::kUdpPortOffsetAckData),
            ipc::socket::NodeRole::RecvOnly);
        if (verbose_)
            std::cerr << "\033[32m[" << topic_name_ << "PubInfo] Publisher initialized on IP: " << this->ipaddr_
                      << " Port: " << this->port_hash_ << " for topic: " << topic_name_ << "\033[0m" << std::endl;
        while (!publisher_->connect())
        {
            std::cerr << "\033[31m[" << topic_name_
                      << "PubInfo] Failed to connect publisher,reconnect affter 1 second...\033[0m" << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        /* ACK 通道连不上不算致命: BestEffort 完全不需要它, 只有 publish_blocking
         * 会退化回"在数据通道上等 ACK"(即端点分离之前的行为)。 */
        if (!ack_rx_->connect())
        {
            if (verbose_)
            {
                std::cerr << "\033[33m[" << topic_name_
                          << "PubInfo] ACK channel unavailable; publish_blocking() will fall back to the "
                             "data socket and may time out on multi-fragment payloads\033[0m"
                          << std::endl;
            }
            ack_rx_.reset();
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
        /* pub-sub 的默认 publish 恒定 BestEffort, 不做"大包自动升级 Reliable"。
         *
         * 历史背景: 曾按 payload 尺寸自动切到 Reliable+CRC32C 以压低大包丢包率,
         * 因为两个原因撤销。端点分离之后其中一个已经解决, 另一个仍然成立:
         *
         * 1) [已解决] ACK 收不到。原先收发共用同一条入组的组播 socket, 发布端
         *    自己的分片全部回绕到自己的接收队列(1 MB = 713 片), 订阅端的 ACK
         *    排在它们之后, 首轮窗口根本轮不到 —— 实测吞吐塌到 1 msg/s 而丢包率
         *    仍是 0.00%。现在 publisher_ 是 SendOnly(不入组, 无回绕) 且 ACK 走
         *    独立的 ack_rx_ 端口, 确认能正常到达, 见 publish_blocking()。
         *
         * 2) [仍然成立] 组播下 ACK 语义不完整。N 个订阅者时, chunk_send_ex 收到
         *    任意一个匹配 ACK 即判定 DeliveredAcked, 无法表达"谁收到了、谁没
         *    收到"。这需要 RTPS 的 Reader/Writer 配对与逐 Reader 确认状态。
         *
         * 因此默认路径仍是 BestEffort: 需要确认的调用方显式用 publish_blocking(),
         * 并接受"至少一个订阅者确认"这一较弱的语义。
         *
         * 提升大包到达率的首选手段依然是扩大接收端 net.core.rmem_max —— 实测
         * 丢包是缓冲溢出型突发, 64 MB 缓冲下 1 MB payload 可跑满 110 MB/s 零丢包。 */
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
        /* 端点分离让这条路径从"实际不可用"变成可用: ACK 走 ack_rx_, 不再被自己
         * 的数据分片挤掉。ack_rx_ 为空(ACK 端口没连上)时退化为旧行为。
         *
         * 组播下的语义边界: N 个订阅者时, 收到任意一个匹配 ACK 即判定成功,
         * 无法表达"谁收到了、谁没收到"。要精确到每个订阅者, 需要 RTPS 的
         * Reader/Writer 配对与逐 Reader 的确认状态, 不在本次范围内。 */
        options.ack_node = ack_rx_;
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
    if (ack_tx_)
        ack_tx_->close();
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
                                                             this->port_hash_, ipc::socket::NodeRole::RecvOnly);
        /* ACK 发送通道 (端点分离): 发到发布端 ack_rx_ 监听的端口。
         * SendOnly 不入组, 所以自己发的 ACK 不会回绕进 subscriber_。 */
        ack_tx_ = std::make_shared<ipc::socket::UDPNode>(
            this->topic_name_.c_str(), this->ipaddr_.c_str(),
            static_cast<uint16_t>(this->port_hash_ + dzIPC::common::kUdpPortOffsetAckData),
            ipc::socket::NodeRole::SendOnly);
        if (verbose_)
            std::cerr << "\033[32m[" << topic_name_ << "SubInfo] Subscriber initialized on IP: " << this->ipaddr_
                      << " Port: " << this->port_hash_ << " for topic: " << topic_name_ << "\033[0m" << std::endl;
        while (!subscriber_->connect())
        {
            std::cerr << "\033[31m[" << topic_name_
                      << "SubInfo] Failed to connect subscriber,reconnect affter 1 second...\033[0m" << std::endl;
            std::this_thread::sleep_for(std::chrono::seconds(1));
        }
        /* ACK 通道连不上不影响收数据, 只是无法回确认 —— BestEffort 下本就不需要。 */
        if (!ack_tx_->connect())
        {
            if (verbose_)
            {
                std::cerr << "\033[33m[" << topic_name_
                          << "SubInfo] ACK channel unavailable; acknowledgements will fall back to the "
                             "data socket\033[0m"
                          << std::endl;
            }
            ack_tx_.reset();
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
                        if (chunk_rev_topic(subscriber_, local_msg, 50, ack_tx_))   // rev timeput 50ms
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

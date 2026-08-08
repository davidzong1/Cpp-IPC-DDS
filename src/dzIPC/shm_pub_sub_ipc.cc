#include <dzIPC/shm_pub_sub_ipc.h>
#include <fcntl.h>
#include <chrono>
#include <cstring>
#include <iostream>
#include <typeinfo>
#include "dzIPC/common/local_pub_sub_registry.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/nodelet_config.h"

namespace dzIPC {
namespace shm {
using namespace ipc;
using dzIPC::control_plane_shm::TopicState;

namespace {

std::string control_name_for(const std::string& data_name)
{
    /* "_control2": TopicControl 增加 PeerSlot 表后结构体变大, 沿用旧名会让
     * ipc::shm::handle::acquire() 在已存在的小段上 mmap 出超出文件长度的区域,
     * 访问越界部分直接 SIGBUS。换名等于强制新建一段, 同时也隔离了新旧版本进程。*/
    return data_name + "_control2";
}

std::string shm_name_for_topic(const std::string& topic_name)
{
    return "dz_ipc_" + sanitize_topic_name(topic_name) + "_topic";
}

void wait_for_peer_drain(dzIPC::control_plane_shm::TopicControlPlane& control_plane)
{
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(200);
    while (control_plane.peer_count() > 0 && std::chrono::steady_clock::now() < deadline)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
}

}   // namespace

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
shm_pub_ipc::shm_pub_ipc(const std::shared_ptr<TopicData>& msg, const std::string& topic_name, size_t domain_id,
                         bool verbose, bool enable_thread_qos, int cpu_id, int thread_priority)
    : pub_ipc_base(msg, topic_name, domain_id, verbose)
    , topic_name_(shm_name_for_topic(topic_name))
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
        wait_for_peer_drain(control_plane_);
        publisher_->clear();
    }
    exit_flag.store(true, std::memory_order_release);
}

void shm_pub_ipc::reset_message(const std::shared_ptr<TopicData>& msg)
{
    topic_msg_.reset(msg->clone());
    // Reset fast-path state: new message type requires re-confirmation.
    std::lock_guard<std::mutex> lock(fast_path_mtx_);
    fp_consecutive_ = 0;
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
                                          ? dzIPC::info_pool::demangle(typeid(*topic_msg_->topic()).name())
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
        /* 死连接回收。
         *
         * libipc 的 force_push 以前在队列满时用 disconnect_receiver() 踢掉
         * "还没读完这一格"的订阅者 —— 那个判据区分不了慢和死, 会把活的慢
         * 订阅者永久摘下线。现在写路径只覆写不踢人, 回收改由这里驱动: 只有
         * 心跳停了 kPeerDeadTimeout 的订阅者才被判死。
         *
         * 超时取 2s = 200 个心跳周期(订阅端 10ms 一次), 留足余量, 宁可晚回收
         * 也不要误杀 —— 误杀正是这次要修掉的问题。 */
        constexpr int64_t kPeerDeadTimeoutNs = 2'000'000'000LL;
        const uint32_t stale = control_plane_.collect_stale_peers(kPeerDeadTimeoutNs);
        if (stale != 0 && publisher_ && publisher_->valid())
        {
            publisher_->disconnect_receivers(stale);
            if (verbose_)
            {
                std::cerr << "\033[33m[" << topic_name_
                          << "PubInfo] reaped dead subscriber connection(s), cc_ids = 0x" << std::hex << stale
                          << std::dec << "\033[0m" << std::endl;
            }
        }
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
    // --- Intra-process fast path ---
    // When all observed peers are local (same process), clone once and fanout
    // the same shared_ptr to every subscriber queue, skipping SHM serialize.
    // K=3 consecutive matching observations gates activation to reduce the
    // window where a remote peer may be completing handshake.
    //
    // Before any subscriber has completed handshake, recv_count() is 0 and
    // the snapshot is empty — the first few messages still go through SHM
    // so late joiners receive them correctly.
    //
    // Gated by the unified process-wide nodelet switch
    // (dzIPC::IsNodeletEnabled()).

    if (!dzIPC::IsNodeletEnabled())
    {
        return publish_for_sniffer(std::move(msg));
    }

    ChannelKey key{raw_topic_name_, domain_id_, msg->msg_id()};
    auto& reg = LocalPubSubRegistry::instance();
    auto snapshot = reg.subscriber_snapshot(key);
    const auto shm_recv = publisher_->recv_count();

    bool use_fast_path = false;
    {
        std::lock_guard<std::mutex> lock(fast_path_mtx_);
        // Reset on any state change: different key, or counts diverged.
        if (!(key == last_fp_key_) || snapshot.size() != last_fp_snapshot_size_
            || shm_recv != last_fp_recv_count_)
        {
            fp_consecutive_ = 0;
            last_fp_key_ = key;
            last_fp_snapshot_size_ = snapshot.size();
            last_fp_recv_count_ = shm_recv;
        }

        if (!snapshot.empty() && shm_recv == snapshot.size())
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
        }
    }

    if (use_fast_path)
    {
        // Clone once, fanout to all local queues.
        std::shared_ptr<IpcMsgBase> cloned(msg->clone());
        for (auto& q : snapshot)
        {
            q->push(cloned);   // const& overload: copies shared_ptr
        }
        return true;
    }

    // Nodelet requested but unavailable: one-shot warning per reason.
    // Atomic fetch_or prevents data races across concurrent publish calls
    // and guarantees each reason fires at most once per instance lifetime.
    enum : uint8_t
    {
        kWarnNoLocalSubs = 1 << 0,
        kWarnMixedPeers = 1 << 1,
    };
    if (snapshot.empty())
    {
        if (!(nodelet_warned_.fetch_or(kWarnNoLocalSubs, std::memory_order_relaxed) & kWarnNoLocalSubs))
        {
            std::cerr << "\033[33m[" << raw_topic_name_
                      << "] nodelet requested but unavailable; falling back to SHM path"
                      << " (no local subscribers)\033[0m" << std::endl;
        }
    }
    else if (shm_recv != snapshot.size())
    {
        if (!(nodelet_warned_.fetch_or(kWarnMixedPeers, std::memory_order_relaxed) & kWarnMixedPeers))
        {
            std::cerr << "\033[33m[" << raw_topic_name_
                      << "] nodelet requested but unavailable; falling back to SHM path"
                      << " (mixed local/remote peers: local=" << snapshot.size()
                      << " shm=" << shm_recv << ")\033[0m" << std::endl;
        }
    }

    // Fallback: old SHM path (serialize + shared-memory send)
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
    , topic_name_(shm_name_for_topic(topic_name))
    , raw_topic_name_(topic_name)
    , domain_id_(domain_id)
    , verbose_(verbose)
    , thread_options_(dzIPC::ThreadDispatch::make_realtime_options(enable_thread_qos, cpu_id, thread_priority))
{
    topic_msg_.reset(msg->clone());
    msg_id_ = topic_msg_->topic()->msg_id();
    msg_queue_ = std::make_shared<CircularQueue<IpcMsgBase>>(queue_size);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
shm_sub_ipc::~shm_sub_ipc()
{
    // Deregister BEFORE stopping threads so fast-path publisher snapshots
    // can no longer include this queue while we shut down.
    {
        std::lock_guard<std::mutex> lock(topic_msg_mtx_);
        if (local_registered_)
        {
            ChannelKey key{raw_topic_name_, domain_id_, msg_id_};
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
        subscriber_->disconnect();
    }
    subscriber_.reset();
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
            ChannelKey old_key{raw_topic_name_, domain_id_, msg_id_};
            ChannelKey new_key{raw_topic_name_, domain_id_, new_msg_id};
            reg.unregister_subscriber(old_key, msg_queue_);
            reg.register_subscriber(new_key, msg_queue_);
        }
        msg_id_ = new_msg_id;
    }
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
                    control_plane_.release_peer_slot(peer_slot_);
                    peer_slot_ = -1;
                    peer_registered = false;
                }
                {
                    std::lock_guard<std::mutex> lock(channel_mtx_);
                    if (subscriber_ && subscriber_->valid())
                    {
                        // The publisher has already advanced to a new
                        // generation when we reach this branch.  The old
                        // route storage may have been cleared, so do not
                        // operate on the old shared synchronization objects.
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
                        subscriber_->disconnect();
                    }
                    subscriber_.reset();
                    continue;
                }
                peer_registered = true;
                /* 向控制面登记本订阅者的 libipc 连接 bit, 并由下面的循环持续
                 * 刷新心跳。发布端据此判定死连接 —— 取代了 force_push 里那套
                 * "没读完就算无效读者"的误伤逻辑。 */
                {
                    std::lock_guard<std::mutex> lock(channel_mtx_);
                    const uint32_t cc_id = (subscriber_ && subscriber_->valid())
                                               ? subscriber_->connected_id()
                                               : 0u;
                    peer_slot_ = control_plane_.acquire_peer_slot(attached_generation, cc_id);
                }
                if (peer_slot_ < 0 && verbose_)
                {
                    std::cerr << "\033[33m[" << topic_name_
                              << "SubInfo] no free peer slot in control plane; this subscriber "
                                 "will not be reaped automatically if it dies\033[0m"
                              << std::endl;
                }
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
                {
                    std::lock_guard<std::mutex> lock(channel_mtx_);
                    if (subscriber_ && subscriber_->valid())
                    {
                        subscriber_->disconnect();
                    }
                    subscriber_.reset();
                }
                if (peer_registered)
                {
                    control_plane_.remove_peer(attached_generation);
                    control_plane_.release_peer_slot(peer_slot_);
                    peer_slot_ = -1;
                    peer_registered = false;
                }
            }
        }
        /* 心跳: 只要本订阅者进程还活着, 这里就会每 10ms 刷新一次。
         * 进程崩溃后心跳停止, 发布端超时即可安全回收其连接。 */
        control_plane_.peer_heartbeat(peer_slot_);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (peer_registered)
    {
        control_plane_.remove_peer(attached_generation);
    }
    control_plane_.release_peer_slot(peer_slot_);
    peer_slot_ = -1;
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
            ? dzIPC::info_pool::demangle(typeid(*topic_template->topic()).name())
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

    // Register for intra-process fast-path delivery (once only).
    {
        std::lock_guard<std::mutex> lock(topic_msg_mtx_);
        if (!local_registered_)
        {
            ChannelKey key{raw_topic_name_, domain_id_, msg_id_};
            LocalPubSubRegistry::instance().register_subscriber(key, msg_queue_);
            local_registered_ = true;
        }
    }
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

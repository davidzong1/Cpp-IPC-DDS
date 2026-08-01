#include "dzIPC/shm_ser_cli_ipc.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>
#include <typeinfo>
#include "dzIPC/common/local_pub_sub_registry.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/nodelet_config.h"

namespace dzIPC {
namespace shm {
using namespace ipc;
using dzIPC::control_plane_shm::TopicState;

namespace {

std::string service_control_name_for(const std::string& topic_name)
{
    return "dz_ipc_" + topic_name + "_ser_control";
}

// Internal envelope: carries a fast-path request together with the client's
// reply queue pointer.  This ensures the server routes the response to the
// exact client that issued the request — never broadcast to all registered
// queues under the same ChannelKey.
//
// Never serialized; only used within the intra-process fast path.
class FastPathRequestEnvelope : public IpcMsgBase
{
public:
    FastPathRequestEnvelope(std::shared_ptr<IpcMsgBase> request,
                            std::shared_ptr<CircularQueue<IpcMsgBase>> reply_queue)
        : request_(std::move(request)), reply_queue_(std::move(reply_queue))
    {}

    std::shared_ptr<IpcMsgBase>& request() { return request_; }
    std::shared_ptr<CircularQueue<IpcMsgBase>>& reply_queue() { return reply_queue_; }

    ipc::buffer serialize() override { return {}; }
    void deserialize(const ipc::buffer&) override {}
    FastPathRequestEnvelope* clone() const override
    {
        return new FastPathRequestEnvelope(
            std::shared_ptr<IpcMsgBase>(request_->clone()), reply_queue_);
    }

private:
    std::shared_ptr<IpcMsgBase> request_;
    std::shared_ptr<CircularQueue<IpcMsgBase>> reply_queue_;
};

}   // namespace

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

shm_ser_ipc::shm_ser_ipc(const std::string& topic_name, const std::shared_ptr<ServiceData>& msg,
                         std::function<void(std::shared_ptr<ServiceData>&)> callback, size_t domain_id, bool verbose,
                         bool enable_thread_qos, int cpu_id, int thread_priority)
    : ser_ipc_base(topic_name, msg, callback, domain_id, verbose)
    , topic_name_(topic_name)
    , callback_(std::move(callback))
    , domain_id_(domain_id)
    , verbose_(verbose)
    , thread_options_(dzIPC::ThreadDispatch::make_realtime_options(enable_thread_qos, cpu_id, thread_priority))
{
    message_.reset(msg->clone());
    fp_queue_ = std::make_shared<CircularQueue<IpcMsgBase>>(16);
    dzIPC::ThreadDispatch::apply_current_thread_options(thread_options_, verbose_, topic_name_ + "_SerOwnerThread");
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void shm_ser_ipc::reset_message(const std::shared_ptr<ServiceData>& msg)
{
    if (!msg)
    {
        return;
    }
    std::shared_ptr<ServiceData> new_msg;
    new_msg.reset(msg->clone());
    const uint32_t new_msg_id = new_msg->request()->msg_id();
    std::lock_guard<std::mutex> lock(message_mtx_);
    message_ = std::move(new_msg);

    // Re-register under new msg_id if fast-path is active and msg_id changed.
    if (fp_registered_ && fp_msg_id_ != new_msg_id)
    {
        auto& reg = LocalPubSubRegistry::instance();
        ChannelKey old_key{topic_name_, domain_id_, fp_msg_id_, ChannelKind::ShmService};
        ChannelKey new_key{topic_name_, domain_id_, new_msg_id, ChannelKind::ShmService};
        reg.unregister_subscriber(old_key, fp_queue_);
        reg.register_subscriber(new_key, fp_queue_);
        fp_msg_id_ = new_msg_id;
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void shm_ser_ipc::reset_callback(std::function<void(std::shared_ptr<ServiceData>&)> callback)
{
    std::lock_guard<std::mutex> lock(callback_mtx_);
    callback_ = std::move(callback);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

shm_ser_ipc::~shm_ser_ipc()
{
    // Deregister fast-path queue before stopping threads.
    if (fp_registered_)
    {
        ChannelKey key{topic_name_, domain_id_, fp_msg_id_, ChannelKind::ShmService};
        LocalPubSubRegistry::instance().unregister_subscriber(key, fp_queue_);
        fp_registered_ = false;
    }

    running = false;
    if (response_thread_ != nullptr)
    {
        if (response_thread_->joinable())
        {
            response_thread_->join();
        }
        delete response_thread_;
    }
    if (handshake_thread_ != nullptr)
    {
        if (handshake_thread_->joinable())
        {
            handshake_thread_->join();
        }
        delete handshake_thread_;
    }
    if (ipc_r_ptr_ && ipc_r_ptr_->valid())
    {
        ipc_r_ptr_->clear();
    }
    if (ipc_w_ptr_ && ipc_w_ptr_->valid())
    {
        ipc_w_ptr_->clear();
    }
    exit_flag.store(true, std::memory_order_release);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void shm_ser_ipc::InitChannel(std::string extra_info)
{
    std::string r_name, w_name;
    r_name = "dz_ipc_" + topic_name_ + "_ser_r";
    w_name = "dz_ipc_" + topic_name_ + "_ser_w";
    if (!control_plane_.open(service_control_name_for(topic_name_)))
    {
        throw std::runtime_error("failed to open service control plane");
    }
    control_plane_.begin_rebuild();
    ipc::server::clear_storage(r_name.c_str());
    ipc::server::clear_storage(w_name.c_str());
    ipc_r_ptr_ = std::make_shared<ipc::server>(r_name.c_str(), ipc::receiver, verbose_);
    ipc_w_ptr_ = std::make_shared<ipc::server>(w_name.c_str(), ipc::sender, verbose_);
    control_plane_.set_ready();
    handshake_thread_ = new std::thread(&shm_ser_ipc::ser_handshake, this);
    std::shared_ptr<ServiceData> message_template;
    {
        std::lock_guard<std::mutex> lock(message_mtx_);
        message_template = message_;
    }
    std::string request_type_name =
        (message_template && message_template->request())
            ? dzIPC::info_pool::demangle(typeid(*message_template->request()).name())
            : std::string{};
    request_type_name = extract_last_segment(request_type_name);
    pool_reg_.rebind({dzIPC::info_pool::EntryKind::ShmServer, topic_name_, request_type_name, "shm",
                      static_cast<int32_t>(domain_id_), extra_info});
    std::cerr << "\033[32m[" << topic_name_ << "_SerInfo] Server channel created for topic: " << topic_name_
              << "\033[0m" << std::endl;

    // Register for intra-process fast-path delivery BEFORE starting
    // response_thread so the thread can process fast-path requests
    // immediately (no race on fp_registered_).
    if (message_template)
    {
        fp_msg_id_ = message_template->request()->msg_id();
        ChannelKey key{topic_name_, domain_id_, fp_msg_id_, ChannelKind::ShmService};
        LocalPubSubRegistry::instance().register_subscriber(key, fp_queue_);
        fp_registered_ = true;
    }

    response_thread_ = new std::thread(&shm_ser_ipc::response_thread_func, this);
    dzIPC::ThreadDispatch::apply_thread_options(response_thread_, thread_options_, verbose_,
                                                topic_name_ + "_SerResponseThread");
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

void shm_ser_ipc::ser_handshake()
{
    bool had_client = false;
    while (running.load(std::memory_order_acquire))
    {
        control_plane_.heartbeat();
        const bool has_client = control_plane_.peer_count() > 0;
        handshake_completed_.store(has_client, std::memory_order_release);
        if (has_client && !had_client && verbose_)
        {
            std::cerr << "\033[32m[" << topic_name_ << "_SerInfo] Client connected to server: " << topic_name_
                      << "\033[0m" << std::endl;
        }
        if (!has_client && had_client && verbose_)
        {
            std::cerr << "\033[32m[" << topic_name_ << "_SerInfo] Client disconnected from server: " << topic_name_
                      << "\033[0m" << std::endl;
        }
        had_client = has_client;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    handshake_completed_.store(false, std::memory_order_release);
    control_plane_.set_stopping();
    if (verbose_)
    {
        std::cerr << "\033[32m[" << topic_name_
                  << "_SerInfo] Server exiting, marking control plane stopping: " << topic_name_ << "\033[0m"
                  << std::endl;
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void shm_ser_ipc::response_thread_func()
{
    while (running.load(std::memory_order_acquire))
    {
        // --- Intra-process fast path: check local request queue ---
        // Always try_pop unconditionally — fp_queue_ exists from construction.
        // Registry registration/unregistration is handled by InitChannel/dtor.
        std::shared_ptr<IpcMsgBase> fp_item;
        if (fp_queue_->try_pop(fp_item))
        {
            auto* envelope = dynamic_cast<FastPathRequestEnvelope*>(fp_item.get());
            if (envelope && envelope->reply_queue())
            {
                std::shared_ptr<IpcMsgBase> request_copy(envelope->request()->clone());
                std::function<void(std::shared_ptr<ServiceData>&)> callback;
                {
                    std::lock_guard<std::mutex> lock(callback_mtx_);
                    callback = callback_;
                }
                if (callback)
                {
                    std::shared_ptr<ServiceData> local_msg;
                    {
                        std::lock_guard<std::mutex> lock(message_mtx_);
                        if (!message_)
                        {
                            continue;
                        }
                        local_msg.reset(message_->clone());
                    }
                    // Swap in the fast-path request (bypass deserialization).
                    local_msg->request() = request_copy;
                    callback(local_msg);
                    // Push response directly to the requesting client's queue.
                    envelope->reply_queue()->push(local_msg->response());
                }
                continue;
            }
        }

        /* 服务端等待请求 — standard SHM path */
        ipc::buffer raw_data = ipc_r_ptr_->recv(50);
        if (raw_data.empty())
        {
            continue;
        }
        /* 身份判断 */
        std::shared_ptr<ServiceData> local_msg;
        {
            std::lock_guard<std::mutex> lock(message_mtx_);
            if (!message_)
            {
                continue;
            }
            local_msg.reset(message_->clone());
        }
        if (!local_msg->check_msg_id(raw_data))
        {
            if (verbose_)
            {
                std::cerr << "\033[33m[Warning] Received message with invalid ID on topic: " << topic_name_ << "\033[0m"
                          << std::endl;
            }
            continue;
        }
        /* 反序列转换为msg数据 */
        local_msg->request()->deserialize(raw_data);
        std::function<void(std::shared_ptr<ServiceData>&)> callback;
        {
            std::lock_guard<std::mutex> lock(callback_mtx_);
            callback = callback_;
        }
        if (callback)
        {
            callback(local_msg);
        }
        ipc::buffer response_data(std::move(local_msg->response()->serialize()));
        int retry_count = 0;
        while (!ipc_w_ptr_->try_send(response_data.data(), response_data.size())
               && running.load(std::memory_order_acquire))
        {
            retry_count++;
            if (retry_count > 10)
            {
                break;
            }
        }
    }   // 客户段发送请求
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

shm_cli_ipc::shm_cli_ipc(const std::string& topic_name, const std::shared_ptr<ServiceData>& msg, size_t domain_id,
                         bool verbose, bool enable_thread_qos, int cpu_id, int thread_priority)
    : cli_ipc_base(topic_name, msg, domain_id, verbose)
    , topic_name_(topic_name)
    , domain_id_(domain_id)
    , verbose_(verbose)
    , thread_options_(dzIPC::ThreadDispatch::make_realtime_options(enable_thread_qos, cpu_id, thread_priority))
{
    message_.reset(msg->clone());
    dzIPC::ThreadDispatch::apply_current_thread_options(thread_options_, verbose_, topic_name_ + "_CliOwnerThread");
}

shm_cli_ipc::~shm_cli_ipc()
{
    running.store(false, std::memory_order_release);
    if (handshake_thread_ != nullptr)
    {
        if (handshake_thread_->joinable())
        {
            handshake_thread_->join();
        }
        delete handshake_thread_;
    }
    {
        std::lock_guard<std::mutex> lock(channel_mtx_);
        if (ipc_r_ptr_ && ipc_r_ptr_->valid())
        {
            ipc_r_ptr_->release();
        }
        if (ipc_w_ptr_ && ipc_w_ptr_->valid())
        {
            ipc_w_ptr_->release();
        }
    }
    exit_flag.store(true, std::memory_order_release);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

void shm_cli_ipc::InitChannel(std::string extra_info)
{
    // 等待服务端创建通道
    handshake_thread_ = new std::thread(&shm_cli_ipc::cli_handshake, this);
    // 等待握手完成
    std::shared_ptr<ServiceData> message_template;
    {
        std::lock_guard<std::mutex> lock(message_mtx_);
        message_template = message_;
    }
    std::string response_type_name =
        (message_template && message_template->response())
            ? dzIPC::info_pool::demangle(typeid(*message_template->response()).name())
            : std::string{};
    response_type_name = extract_last_segment(response_type_name);
    pool_reg_.rebind({dzIPC::info_pool::EntryKind::ShmClient, topic_name_, response_type_name, "shm",
                      static_cast<int32_t>(domain_id_), extra_info});
    if (verbose_)
    {
        std::cerr << "\033[32m[" << topic_name_ << "_CLiInfo] Client connected to server topic: " << topic_name_
                  << "\033[0m" << std::endl;
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

void shm_cli_ipc::cli_handshake()
{
    if (!control_plane_.open(service_control_name_for(topic_name_)))
    {
        throw std::runtime_error("control plane open failed");
    }
    uint32_t attached_generation = 0;
    bool peer_registered = false;
    while (running.load(std::memory_order_acquire))
    {
        const uint32_t generation = control_plane_.generation();
        const TopicState state = control_plane_.state();
        if (state == TopicState::Ready && generation != 0)
        {
            if (!handshake_completed_.load(std::memory_order_acquire) || attached_generation != generation)
            {
                if (peer_registered)
                {
                    control_plane_.remove_peer(attached_generation);
                    peer_registered = false;
                }
                std::string r_name, w_name;
                r_name = "dz_ipc_" + topic_name_ + "_ser_w";
                w_name = "dz_ipc_" + topic_name_ + "_ser_r";
                {
                    std::lock_guard<std::mutex> lock(channel_mtx_);
                    if (ipc_r_ptr_ && ipc_r_ptr_->valid())
                    {
                        ipc_r_ptr_->release();
                    }
                    if (ipc_w_ptr_ && ipc_w_ptr_->valid())
                    {
                        ipc_w_ptr_->release();
                    }
                    ipc_r_ptr_ = std::make_shared<ipc::server>(r_name.c_str(), ipc::receiver, verbose_);
                    ipc_w_ptr_ = std::make_shared<ipc::server>(w_name.c_str(), ipc::sender, verbose_);
                }
                attached_generation = generation;
                if (!control_plane_.add_peer(attached_generation))
                {
                    std::lock_guard<std::mutex> lock(channel_mtx_);
                    if (ipc_r_ptr_ && ipc_r_ptr_->valid())
                    {
                        ipc_r_ptr_->release();
                    }
                    if (ipc_w_ptr_ && ipc_w_ptr_->valid())
                    {
                        ipc_w_ptr_->release();
                    }
                    ipc_r_ptr_.reset();
                    ipc_w_ptr_.reset();
                    continue;
                }
                peer_registered = true;
                handshake_completed_.store(true, std::memory_order_release);
            }
        }
        else
        {
            if (handshake_completed_.exchange(false, std::memory_order_acq_rel))
            {
                if (peer_registered)
                {
                    control_plane_.remove_peer(attached_generation);
                    peer_registered = false;
                }
                std::lock_guard<std::mutex> lock(channel_mtx_);
                if (ipc_r_ptr_ && ipc_r_ptr_->valid())
                {
                    ipc_r_ptr_->release();
                }
                if (ipc_w_ptr_ && ipc_w_ptr_->valid())
                {
                    ipc_w_ptr_->release();
                }
                ipc_r_ptr_.reset();
                ipc_w_ptr_.reset();
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (peer_registered)
    {
        control_plane_.remove_peer(attached_generation);
    }
    if (verbose_)
    {
        std::cerr << "\033[32m[" << topic_name_
                  << "_CLiInfo] Client exiting, detaching from server: " << topic_name_ << "\033[0m"
                  << std::endl;
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool shm_cli_ipc::send_request(std::shared_ptr<ServiceData>& request, uint64_t rev_tm)
{
    /* 主线程执行，因此无需考虑running */
    if (!handshake_completed_.load(std::memory_order_acquire))
    {
        if (verbose_)
        {
            std::cerr << "\033[32m[" << topic_name_
                      << "_CLiInfo] Handshake not completed, cannot send request on topic: " << topic_name_ << "\033[0m"
                      << std::endl;
        }
        // One-shot warning when nodelet is enabled but no server is reachable.
        if (dzIPC::IsNodeletEnabled() && !nodelet_no_server_warned_.exchange(true))
        {
            std::cerr << "\033[33m[" << topic_name_
                      << "] nodelet requested but unavailable; falling back\033[0m" << std::endl;
        }
        return false;
    }
    std::shared_ptr<ServiceData> message_template;
    {
        std::lock_guard<std::mutex> lock(message_mtx_);
        if (!message_)
        {
            return false;
        }
        message_template.reset(message_->clone());
    }

    // --- Intra-process fast path ---
    // Registry only holds server request queues.  The client creates a
    // per-request capacity-1 reply queue and embeds it in the envelope;
    // the server routes the response directly to that queue.  This
    // guarantees call-level correlation — no broadcast, no pollution.
    //
    // K=3 consecutive observations of a single local server queue gates
    // activation.  On any key or snapshot-size change the counter resets.
    //
    // IsNodeletEnabled() is checked at each send_request call, so
    // EnableNodelet(true) after InitChannel is supported.
    if (dzIPC::IsNodeletEnabled())
    {
        const uint32_t req_msg_id = request->request()->msg_id();
        ChannelKey key{topic_name_, domain_id_, req_msg_id, ChannelKind::ShmService};
        auto& reg = LocalPubSubRegistry::instance();
        auto snapshot = reg.subscriber_snapshot(key);

        // Server count: snapshot must contain exactly one entry (the server).
        const size_t server_count = snapshot.size();

        bool use_fp = false;
        {
            std::lock_guard<std::mutex> lock(fast_path_mtx_);
            if (!(key == last_fp_ser_key_) || server_count != last_fp_ser_snapshot_size_)
            {
                fp_consecutive_ = 0;
                last_fp_ser_key_ = key;
                last_fp_ser_snapshot_size_ = server_count;
            }

            if (server_count == 1)
            {
                ++fp_consecutive_;
                if (fp_consecutive_ >= kFastPathConfirm)
                {
                    use_fp = true;
                }
            }
            else
            {
                fp_consecutive_ = 0;
            }
        }

        if (use_fp)
        {
            // Per-request reply queue: capacity 1, lifetime scoped to this call.
            // Any late response arriving after we return is naturally discarded
            // when the queue goes out of scope.
            auto reply_queue = std::make_shared<CircularQueue<IpcMsgBase>>(1);
            auto envelope = std::make_shared<FastPathRequestEnvelope>(
                std::shared_ptr<IpcMsgBase>(request->request()->clone()), reply_queue);
            snapshot[0]->push(envelope);

            std::shared_ptr<IpcMsgBase> fp_response;
            if (reply_queue->pop(fp_response, rev_tm))
            {
                request->response() = fp_response;
                return true;
            }
            // Timeout: return false — do NOT fall through to SHM.
            // The server may still process the queued request asynchronously;
            // re-sending via SHM would double-execute the callback.
            return false;
        }

        // Issue one-shot warnings for why fast path is unavailable.
        // std::atomic exchange(true) atomically checks-and-sets in one op.
        if (server_count == 0 && !nodelet_no_server_warned_.exchange(true))
        {
            std::cerr << "\033[33m[" << topic_name_
                      << "] nodelet requested but unavailable; falling back\033[0m" << std::endl;
        }
        else if (server_count > 1 && !nodelet_anomaly_warned_.exchange(true))
        {
            std::cerr << "\033[33m[" << topic_name_
                      << "] nodelet registry anomaly (" << server_count
                      << " entries); falling back\033[0m" << std::endl;
        }
    }

    // --- Standard SHM path ---
    std::lock_guard<std::mutex> channel_lock(channel_mtx_);
    if (!handshake_completed_.load(std::memory_order_acquire) || !ipc_w_ptr_ || !ipc_r_ptr_)
    {
        return false;
    }
    ipc::buffer request_data(std::move(request->request()->serialize()));
    int retry_count = 0;
    while (!ipc_w_ptr_->try_send(request_data.data(), request_data.size()))
    {
        retry_count++;
        if (retry_count > 10)
        {
            return false;
        }
    };
    do
    {
        ipc::buffer raw_response = ipc_r_ptr_->recv(rev_tm);
        if (raw_response.empty())   // 超时未收到响应
        {
            return false;
        }
        if (message_template->check_msg_id(raw_response))   // 收到响应且ID正确,否则重新接收
        {
            request->response()->deserialize(raw_response);
            break;
        }
    } while (true);
    return true;
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void shm_cli_ipc::reset_message(const std::shared_ptr<ServiceData>& msg)
{
    if (!msg)
    {
        return;
    }
    std::shared_ptr<ServiceData> new_msg;
    new_msg.reset(msg->clone());
    std::lock_guard<std::mutex> lock(message_mtx_);
    message_ = std::move(new_msg);
}
}   // namespace shm
}   // namespace dzIPC

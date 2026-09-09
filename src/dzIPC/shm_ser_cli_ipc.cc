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
#include "dzIPC/common/wire_accept.h"

namespace dzIPC {
namespace shm {

namespace {

/* DZFlat 借样发送 (docs/dzflat_shm.md Step 2, srv 接入见 known_issues 第 6 条)。
 *
 * 返回 false 表示"本次不走 DZFlat", 调用方须回退既有整包序列化。回退是常态:
 * 开关未开 / 类型不支持 / 对端未连 / chunk 池耗尽都会走到那里。
 *
 * ser/cli 用的是 ipc::server(single-single-unicast), 与 pub/sub 的 route 不同:
 * 其 recv 侧 recycle 无条件归还(sub_rc 恒 true), chunk 的 conns 不承载持有信息 ——
 * 对借样没有影响, 因为一条消息只有一个接收方, 它的 buff_t 析构即归还。 */
bool try_send_dzflat(const std::shared_ptr<ipc::server>& ch, const std::shared_ptr<IpcMsgBase>& msg)
{
    /* 每次调用都记一笔: 回退是静默的, 没有计数就分不清"收益生效了"和"一直在回退"
     * (pub/sub 侧同理, 见 nodelet_config.h 的说明)。 */
    struct Note
    {
        bool used = false;
        ~Note() { dzIPC::detail::NoteDzFlatPublish(used); }
    } note;

    if (!dzIPC::IsDzFlatEnabled() || !ch || !msg || !msg->dzflat_supported())
    {
        return false;
    }
    if (ch->recv_count() == 0)
    {
        return false;   // 无接收方: chunk 借不到也无人回收
    }
    const std::uint32_t need = msg->dzflat_size();
    if (need == 0)
    {
        return false;
    }
    auto lo = ch->loan(need);
    if (!lo.valid())
    {
        return false;   // 池耗尽 —— 背压, 不是错误
    }
    if (!msg->dzflat_write(lo.data, static_cast<std::uint32_t>(lo.size)))
    {
        ch->discard_loan(lo);
        return false;
    }
    note.used = ch->publish_loan(lo, 0);
    return note.used;
}

/* 双 wire 接收: 判别 + 校验 + 拒收计数, 与 pub/sub 共用一份实现
 * (见 dzIPC/common/wire_accept.h)。返回 false 表示这条应当丢弃。
 *
 * ser/cli 的期待 msg_id 就取 target 自己的 —— 与 pub/sub 不同, 这里的目标对象不会被
 * swap() 移走, 它始终持有本请求/响应类型的 msg_id。 */
bool accept_wire(const ipc::buffer& raw, const std::shared_ptr<IpcMsgBase>& target)
{
    return dzIPC::AcceptWire(raw, target->msg_id(), *target);
}

/* 分流一个收到的段, 让 DZFlat request/response 走零拷贝借样(仿 pub/sub 视图路径)。
 *
 *   1  = 借样成功: typed DZFlat 且 id/schema 全对上 —— 段已 move 进 sample_out;
 *   0  = 应走物化 accept_wire: TLV / schema-less(GenericMessage)话题 / 手写类型;
 *  -1  = 识别为 typed DZFlat 但校验失败(id 或 schema 不符) —— 已计数, 调用方丢弃。
 *
 * 计数只在 1 / -1 路径记一次; 0 路径留给 accept_wire 自己记, 避免重复。 */
int classify_received(ipc::buffer& raw, const std::shared_ptr<IpcMsgBase>& tpl,
                      std::uint32_t tpl_msg_id, std::shared_ptr<dzIPC::Sample>& sample_out)
{
    using E = dzIPC::detail::DzFlatRxEvent;
    if (!dzflat::looks_like_dzflat(raw.data(), raw.size()))
    {
        return 0;
    }
    if (!tpl || !tpl->dzflat_supported())
    {
        return 0;   // schema-less / 手写类型: 物化(GenericMessage 拷段字节)
    }
    std::uint32_t seg_id = 0;
    if (!IpcMsgBase::dzflat_peek_msg_id(raw.data(), raw.size(), seg_id) || seg_id != tpl_msg_id)
    {
        dzIPC::detail::NoteDzFlatRx(E::kDzFlatIdSkipped);
        return -1;
    }
    dzflat::SegHeader h{};
    std::memcpy(&h, raw.data(), sizeof(h));
    if (h.schema_hash != tpl->dzflat_schema_hash())
    {
        dzIPC::detail::NoteDzFlatRx(E::kDzFlatSchemaDrop);
        return -1;
    }
    dzIPC::detail::NoteDzFlatRx(E::kDzFlatAccepted);
    sample_out =
        std::make_shared<dzIPC::Sample>(std::move(raw), seg_id, h.schema_hash);
    return 1;
}

}   // namespace
using namespace ipc;
using dzIPC::control_plane_shm::TopicState;

namespace {

std::string service_control_name_for(const std::string& topic_name)
{
    /* "_ser_control2": 与 pub-sub 侧同理, TopicControl 结构体已变大,
     * 必须换名以避免在旧的小共享内存段上越界映射。 */
    return "dz_ipc_" + topic_name + "_ser_control2";
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
        /* 死连接回收。
         *
         * ser-cli 的发送走 try_send(), 不会掉进 force_push, 所以它从来没有
         * "慢读者被误踢"的问题; 但它也从来没有回收死连接的机制 —— 崩溃的
         * 客户端在响应通道 (_ser_w) 的连接位图里留下的 bit 会让服务端的
         * try_send() 永远失败(重试 10 次后放弃), 这个方向就静默地发不出去了。
         *
         * 客户端在 cli_handshake() 里登记自己在响应通道上的 cc_id 并持续心跳,
         * 这里按心跳超时判死。超时同 pub-sub 取 2s = 200 个心跳周期。 */
        constexpr int64_t kPeerDeadTimeoutNs = 2'000'000'000LL;
        const uint32_t stale = control_plane_.collect_stale_peers(kPeerDeadTimeoutNs);
        if (stale != 0 && ipc_w_ptr_ && ipc_w_ptr_->valid())
        {
            ipc_w_ptr_->disconnect_receivers(stale);
            if (verbose_)
            {
                std::cerr << "\033[33m[" << topic_name_
                          << "_SerInfo] reaped dead client connection(s) on the response channel, cc_ids = 0x"
                          << std::hex << stale << std::dec << "\033[0m" << std::endl;
            }
        }
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
        /* 双 wire 分流: DZFlat + typed request → 借样成只读视图(回调用 request_view<T>()
         * 读, 零拷贝); TLV / schema-less → 物化进 owning request()。 */
        auto tpl = local_msg->request();
        std::shared_ptr<dzIPC::Sample> sample;
        const int wr = classify_received(raw_data, tpl, tpl ? tpl->msg_id() : 0, sample);
        if (wr == 1)
        {
            /* 借样成功: request() 保持模板克隆(不填收到的数据), 数据在借样段里。 */
            local_msg->request_sample() = std::move(sample);
        }
        else if (wr == 0)
        {
            if (!accept_wire(raw_data, local_msg->request()))
            {
                if (verbose_)
                {
                    std::cerr << "\033[33m[Warning] Received message with invalid ID on topic: " << topic_name_
                              << "\033[0m" << std::endl;
                }
                continue;
            }
        }
        else
        {
            continue;   // typed DZFlat 但 id/schema 不符 → 丢弃
        }
        std::function<void(std::shared_ptr<ServiceData>&)> callback;
        {
            std::lock_guard<std::mutex> lock(callback_mtx_);
            callback = callback_;
        }
        if (callback)
        {
            callback(local_msg);
        }
        if (try_send_dzflat(ipc_w_ptr_, local_msg->response()))
        {
            continue;   // 已借样送出
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
                    control_plane_.release_peer_slot(peer_slot_);
                    peer_slot_ = -1;
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
                /* 向控制面登记本客户端在响应通道 (ipc_r_ptr_) 上的连接 bit,
                 * 并由下面的循环持续刷新心跳。服务端据此判定死连接并回收。 */
                {
                    std::lock_guard<std::mutex> lock(channel_mtx_);
                    const uint32_t cc_id = (ipc_r_ptr_ && ipc_r_ptr_->valid())
                                               ? ipc_r_ptr_->connected_id()
                                               : 0u;
                    peer_slot_ = control_plane_.acquire_peer_slot(attached_generation, cc_id);
                }
                if (peer_slot_ < 0 && verbose_)
                {
                    std::cerr << "\033[33m[" << topic_name_
                              << "_CliInfo] no free peer slot in control plane; this client "
                                 "will not be reaped automatically if it dies\033[0m"
                              << std::endl;
                }
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
                    control_plane_.release_peer_slot(peer_slot_);
                    peer_slot_ = -1;
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
        /* 心跳: 只要本客户端进程还活着, 这里就会每 10ms 刷新一次。
         * 进程崩溃后心跳停止, 服务端超时即可安全回收其连接。 */
        control_plane_.peer_heartbeat(peer_slot_);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (peer_registered)
    {
        control_plane_.remove_peer(attached_generation);
    }
    control_plane_.release_peer_slot(peer_slot_);
    peer_slot_ = -1;
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
    if (!try_send_dzflat(ipc_w_ptr_, request->request()))
    {
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
    }
    request->response_sample().reset();   // 清掉上一次响应(若有), 本次重新收
    do
    {
        ipc::buffer raw_response = ipc_r_ptr_->recv(rev_tm);
        if (raw_response.empty())   // 超时未收到响应
        {
            return false;
        }
        auto resp_tpl = request->response();
        std::shared_ptr<dzIPC::Sample> sample;
        const int rr =
            classify_received(raw_response, resp_tpl, resp_tpl ? resp_tpl->msg_id() : 0, sample);
        if (rr == 1)
        {
            /* DZFlat 响应借样: response() 保持模板克隆, 借样段随 ServiceData 活到调用方读完。 */
            request->response_sample() = std::move(sample);
            break;
        }
        if (rr == 0 && accept_wire(raw_response, request->response()))
        {
            break;   // TLV / schema-less 响应已物化
        }
        /* rr == -1(typed DZFlat 不符)或 accept_wire 拒 → 重新接收 */
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

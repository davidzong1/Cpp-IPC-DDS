#include <dzIPC/shm_pub_sub_ipc.h>
#include <fcntl.h>
#include <algorithm>
#include <chrono>
#include <cstring>
#include <iostream>
#include <mutex>
#include <typeinfo>
#include <vector>
#include "dzIPC/common/local_pub_sub_registry.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/wire_accept.h"
#include "dzIPC/detail/shm_sub_seam.h"   /* 内部测试缝: 默认空指针 ⇒ 零行为变化 */
#include "ipc_msg/ipc_msg_base/generic_message.hpp"   /* fast-path 借样物化(UF-012) */

namespace dzIPC {
namespace shm {
using namespace ipc;
using dzIPC::control_plane_shm::TopicState;

namespace {

/* 单 topic 的接收方上限 = libipc 连接位图的位宽(circ::cc_t = uint32_t)。
 * 与 control_plane.h 的 kMaxPeerSlots(64) 不同 —— 那是控制面登记表的容量, 比这里大一倍;
 * 两者的差额正是 docs/shm_defect_fixes.md 第 2 条那个黑洞的容量。 */
constexpr std::size_t kMaxShmReceiversPerTopic = 32;

/* 段名规则收在 dzIPC/common/name_operator.h —— 传输层、sniffer、工具都从那一处取,
 * 免得规则一改要同时改五处且漏掉的那处是静默失效(见该头文件的说明)。
 * 段名含 domain_id: 不含就等于 SHM 上没有 domain 隔离(docs/shm_defect_fixes.md 第 1 条),
 * 代价是与旧版本进程不互通 —— 这是有意的, 旧进程段名不带 domain, 能互通就说明没生效。 */
std::string shm_name_for_topic(const std::string& topic_name, size_t domain_id)
{
    return shm_topic_segment_name(topic_name, domain_id);
}

/* pub/sub 控制面段名 —— 本文件只是**转调**, 规则("数据段名 + _control2")的唯一出处在
 * dzIPC/common/name_operator.h 的 shm_topic_control_name(), 与 ser 侧的
 * service_control_name_for() 同构。
 *
 * 传原始 topic 名 + domain 而不是已经拼好的 topic_name_: 两处调用点手上都有
 * (raw_topic_name_, domain_id_), 而收成一个组合完整的入参后, 这里再也不可能出现
 * "把某个别的东西当数据段名传进来" 的写法 —— 段名拼错不报错, 只静默多出一个空段。 */
std::string topic_control_name_for(const std::string& topic_name, size_t domain_id)
{
    return shm_topic_control_name(topic_name, domain_id);
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
    , topic_name_(shm_name_for_topic(topic_name, domain_id))
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
        if (!control_plane_.open(topic_control_name_for(raw_topic_name_, domain_id_)))
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
        /* ⛔ 队列里的借样只允许来自接收侧 adopt 配额(UF-012)。发布侧消息若自身持
         * 借样(订阅后转发的 GenericMessage), clone 拷的是 shared_ptr<buffer> ——
         * 借样随克隆进各订阅者队列, 绕开配额钉池。物化掉: clone 自持堆块。 */
        if (cloned->dzflat_is_borrowed())
        {
            auto* gm = dynamic_cast<GenericMessage*>(cloned.get());
            if (gm != nullptr)
            {
                gm->dzflat_read(gm->dzflat_data(), gm->dzflat_len());
            }
        }
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
        if (try_publish_dzflat(msg, tm))
        {
            dzIPC::detail::NoteDzFlatPublish(true);
            return true;
        }
        dzIPC::detail::NoteDzFlatPublish(false);
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
/* DZFlat 借样发布: 借一块共享 chunk, 把消息**直接按平坦布局写进共享内存**, 省掉
 * 「serialize() 整包 new + TLV 页尾分段拷贝 + send() 再 memcpy 进 chunk」这一整串。
 *
 * 返回 false 表示"本次不走 DZFlat", 调用方须回退既有整包路径。回退不是异常, 是常态:
 *   - 开关未开 / 消息类型不支持(手写类型、GenericMessage);
 *   - 该通道当前没有接收方(chunk 无人回收, loan 会拒绝);
 *   - chunk 池耗尽(每尺寸档位 32 块) —— 这是背压, 回退整包路径仍能送达。
 *
 * 生命周期: loan 成功后每条出口都必须以 publish_loan 或 discard_loan 结束。
 * publish_loan 失败时 chunk 已由其内部归还, 这里不得重复 discard。
 */
bool shm_pub_ipc::try_publish_dzflat(const std::shared_ptr<IpcMsgBase>& msg, std::uint64_t tm)
{
    if (!dzIPC::IsDzFlatEnabled() || !msg || !msg->dzflat_supported())
    {
        return false;
    }
    if (!publisher_ || publisher_->recv_count() == 0)
    {
        return false;
    }
    const std::uint32_t need = msg->dzflat_size();
    if (need == 0)
    {
        return false;
    }
    auto lo = publisher_->loan(need);
    if (!lo.valid())
    {
        return false;   // 池耗尽 / 无接收方 —— 回退整包
    }
    if (!msg->dzflat_write(lo.data, static_cast<std::uint32_t>(lo.size)))
    {
        publisher_->discard_loan(lo);
        return false;
    }
    return publisher_->publish_loan(lo, tm);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
/* 预构造段发布(见 pub_ipc_base.h): 段由调用方(今天的唯一使用者是 Python)按自己的 schema
 * 写好交来, 这里只负责借一块 chunk 把它**原样**送出去。
 *
 * 与 try_publish_dzflat 的关系: 门槛完全同源(开关 / 接收方 / chunk 池), 差别只在"段是
 * 谁写的" —— 那边是消息类型就地写进借来的 chunk(发布端也零拷贝), 这边是段已在调用方
 * 地址空间里, 唯一能做的是那一跳 memcpy。接收侧两条路完全一致(都是 DZFlat 段, 都借样)。
 *
 * 不记 publish 事件日志: log_publish_event 需要 owning 消息对象去 clone + serialize,
 * 段路径没有(段本身就是序列化结果)。这是取舍, 见函数末尾注释。 */
bool shm_pub_ipc::publish_prebuilt_segment(const void* seg, std::size_t len)
{
    if (!dzIPC::IsDzFlatEnabled() || seg == nullptr)
    {
        return false;
    }
    /* 段头先自证: magic / layout_ver / total_size <= len / root_off。坏段一律不发 ——
     * 送出去只会被对端按 kDzFlatHeaderBad 丢掉, 还白占一块 chunk。 */
    if (!dzflat::looks_like_dzflat(seg, len))
    {
        return false;
    }
    dzflat::SegHeader h{};
    std::memcpy(&h, seg, sizeof(h));
    /* 段头 msg_id 必须等于本话题模板的 msg_id。订阅端判"这条是不是我的话题"用的就是
     * 这个值(shm_pub_sub_ipc.cc 订阅循环的 exp_id = 话题注册键), 不符 ⇒ 对端**静默丢弃**。
     * 宁可让调用方回退 TLV 走慢路径, 也不要发一条注定被丢的段。 */
    if (h.msg_id != dzflat_msg_id())
    {
        return false;
    }
    if (!publisher_ || publisher_->recv_count() == 0)
    {
        return false;
    }
    auto lo = publisher_->loan(h.total_size);
    if (!lo.valid())
    {
        return false;   // 池耗尽 —— 背压, 回退整包
    }
    std::memcpy(lo.data, seg, h.total_size);
    const bool ok = publisher_->publish_loan(lo, 0);
    if (ok)
    {
        dzIPC::detail::NoteDzFlatPublish(true);
    }
    /* 失败不在这里计数: 调用方随后那次 TLV publish() 会记一次回退。两边各记一次会把
     * fallback/(dzflat+fallback) 这个比值算歪 —— 那个比值是判断"收益有没有生效"的唯一
     * 指标(nodelet_config.h), 不能因为记账方式失真。 */
    return ok;
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool shm_pub_ipc::publish_for_sniffer(std::shared_ptr<IpcMsgBase> msg)
{
    try
    {
        /* 有接收方时优先借样(tm=0: 与 no_member_try_send 的 best-effort 语义一致)。
         * 无接收方时必须走 no_member_try_send —— 那条路径会把负载送进 sniffer 环,
         * 而 sniffer 侧解析的是 TLV, 所以不能用 DZFlat。 */
        if (try_publish_dzflat(msg, 0))
        {
            dzIPC::detail::NoteDzFlatPublish(true);
            return true;
        }
        dzIPC::detail::NoteDzFlatPublish(false);
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

namespace {

/* 步骤③ 的钉生效时的**一次性**诊断。
 *
 * 为什么必须有: view 队列容量是对调用方**显式传入**的 queue_size 的覆盖(该参数在
 * SubscriberIPCPtrMake 里必填、无默认值)。覆盖而不说 = 又一次"静默改变行为",
 * 与本仓反复踩过的那一类(ArgParser 的 BOOL 吞 token、create|open 造空壳段)同族。
 *
 * 去重口径: 按"被请求的 queue_size"去重 —— 同一个值只报一次, 不同值各报一次,
 * 这样一个应用建了多档订阅者时不会漏掉后一档。输出口径照 ipc_info_pool.cc 的
 * 说明: src/dzIPC 侧统一用 std::cerr(黄色)。 */
void warn_view_queue_pinned(std::size_t requested, std::size_t applied)
{
    static std::mutex lock;
    static std::vector<std::size_t> seen;
    {
        std::lock_guard<std::mutex> guard{lock};
        if (std::find(seen.begin(), seen.end(), requested) != seen.end())
        {
            return;
        }
        seen.push_back(requested);
    }
    std::cerr << "\033[33m[dzIPC][view_queue] queue_size = " << requested << " 超过钉上限, 被钉到 " << applied
              << ": chunk 池每尺寸档只有 " << static_cast<std::size_t>(ipc::large_msg_cache)
              << " 块且全机共享(建 route 不带 prefix), 队列配得比池大就会把池吃干。"
                 "两个钉面同用此上限: view 队列钉到 " << applied
              << "; adopt 借样(schema-less 话题的 DZFlat 经 msg_queue_)配额同为 " << applied
              << ", 配额满即自动物化拷贝 —— 零拷贝只在配额内生效, 超额每消息多一次拷贝。"
                 "见 docs/shm_chunk_pool_occupancy_plan.md §3 步骤③与 UF-012; "
                 "EnableViewQueuePin(false) 可恢复原值。\033[0m"
              << std::endl;
}

}   // namespace

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
shm_sub_ipc::shm_sub_ipc(const std::shared_ptr<TopicData>& msg, const std::string& topic_name, size_t domain_id,
                         const size_t queue_size, bool verbose, bool enable_thread_qos, int cpu_id,
                         int thread_priority)
    : sub_ipc_base(msg, topic_name, domain_id, queue_size, verbose)
    , topic_name_(shm_name_for_topic(topic_name, domain_id))
    , raw_topic_name_(topic_name)
    , domain_id_(domain_id)
    , verbose_(verbose)
    , thread_options_(dzIPC::ThreadDispatch::make_realtime_options(enable_thread_qos, cpu_id, thread_priority))
{
    topic_msg_.reset(msg->clone());
    msg_id_ = topic_msg_->topic()->msg_id();
    msg_queue_ = std::make_shared<CircularQueue<IpcMsgBase>>(queue_size);
    /* 步骤③: view 队列钉 chunk(借样 Sample 持有 buff_t, 见本文件订阅循环 :773 处
     * 注释), adopt 借样(GenericMessage 收 schema-less DZFlat)经 msg_queue_ 也钉
     * chunk —— 两者的配额同源: ViewQueueCap() = large_msg_cache/4 = 10(对齐 ROS 2
     * 默认 QoS depth), 保持 "4 个订阅者满钉" 的池余量(10×4 = 40 = 池容量)。池每
     * 尺寸档 large_msg_cache 块且全机共享(建 route 不带 prefix) ⇒ 不设上限的队列
     * 配置就能把池吃干, 之后发布侧 loan 拿不到块而回退整包 TLV。设计与实测见
     * docs/shm_chunk_pool_occupancy_plan.md §3 步骤③。
     *
     * ⚠️ msg 队列的 TLV 物化消息不钉 chunk —— 钉的只是 adopt 借样, 由 adopt_cap_
     *    配额封顶(见订阅循环 adopt 分支与 UF-012); 缩 msg_queue_ 本体只是白减缓冲。
     * ⚠️ socket/UDP 侧不钉: 那边的 Sample/借样持有的是去帧独立堆块, 不占池。 */
    const std::size_t view_cap =
        dzIPC::IsViewQueuePinEnabled() ? dzIPC::ViewQueueCap() : queue_size;
    if (view_cap < queue_size)
    {
        warn_view_queue_pinned(queue_size, view_cap);
    }
    view_queue_ = std::make_shared<CircularQueue<Sample>>(
        (view_cap < queue_size) ? view_cap : queue_size);
    /* UF-012 adopt 借样配额: 与 view 队列同一上限、同一开关。借样进 msg_queue_
     * 的消息每条钉一块 chunk, 而队列深度是用户配置的 queue_size(可能远大于池),
     * 不设配额一个慢消费者就能把整档池钉干。计数三条路径: adopt 入队 +1,
     * pop(get_clone/try_get_clone) -1, 满队挤最老(evict 回调) -1。 */
    adopt_cap_ = view_cap;
    msg_queue_->set_evict_cb(
        [this](const std::shared_ptr<IpcMsgBase> &dropped)
        {
            if (dropped && dropped->dzflat_is_borrowed())
            {
                adopt_borrowed_.fetch_sub(1, std::memory_order_relaxed);
            }
        });
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
    /* 测试缝(阶段2_全量测试方案 §4.2): 记录"registry 注销发生在收包停止之前"这一条
     * 因果序。默认钩子为空 ⇒ 只有一次 relaxed load。 */
    detail::FireSeam({detail::SeamPoint::kDtorAfterUnregister, 0, nullptr, nullptr, 0});

    /* 阶段 2 说明 §5: running = false 只能让循环在 recv 返回后退出; 卡在
     * recv(50) 里时必须靠 stop_and_wake() 的 disconnect/quit_waiting 叫醒, 不能
     * 只靠 50ms 超时。stop_and_wake 同时拒绝新 lease, 于是 inflight 只减不增。 */
    running.store(false, std::memory_order_release);
    route_session_.stop_and_wake();
    /* §4.2 的承重点: 析构必须**叫醒**在途 recv, 不能只靠 recv(50) 超时。 */
    detail::FireSeam({detail::SeamPoint::kDtorAfterStopAndWake, 0, nullptr, nullptr, 0});

    /* 先 join 收包线程: 它退出前会做完最后一次 release_receive, 使 inflight 归零;
     * 握手线程若正卡在 begin_rebuild 的等待里, 也由此得以推进并看到 running==false。
     * 析构线程不得持有 RouteSession 锁时 join —— 这里没有持锁。 */
    if (subscribe_thread_ != nullptr)
    {
        if (subscribe_thread_->joinable())
        {
            subscribe_thread_->join();
        }
        delete subscribe_thread_;
        subscribe_thread_ = nullptr;
    }
    detail::FireSeam({detail::SeamPoint::kDtorAfterJoinSubscribe, 0, nullptr, nullptr, 0});
    if (sub_handshake_thread_ != nullptr)
    {
        if (sub_handshake_thread_->joinable())
        {
            sub_handshake_thread_->join();
        }
        delete sub_handshake_thread_;
        sub_handshake_thread_ = nullptr;
    }
    detail::FireSeam({detail::SeamPoint::kDtorAfterJoinHandshake, 0, nullptr, nullptr, 0});

    /* 第 6-7 步: 两个线程都已退出 ⇒ 无在途 recv, 此刻对 current_route() 的拷贝调
     * release() 与 recv 不并发(说明 §4 表格第 2 行)。disconnect 已由 stop_and_wake
     * 做过, 这里只释放句柄; 对象本身的 shared_ptr 由 route_session_ 析构时放掉。 */
    route_session_.wait_quiescent();
    detail::FireSeam({detail::SeamPoint::kDtorAfterQuiescent, 0, nullptr, nullptr, 0});
    {
        std::shared_ptr<ipc::route> cur = route_session_.current_route();
        if (cur && cur->valid())
        {
            cur->release();
        }
    }
    detail::FireSeam({detail::SeamPoint::kDtorAfterRelease, 0, nullptr, nullptr, 0});
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
    if (!control_plane_.open(topic_control_name_for(raw_topic_name_, domain_id_)))
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
                    /* 换成新 generation 的 route(阶段 2 说明 §4 表格第 1 行)。
                     * begin_rebuild 内部顺序: 锁内置 rebuilding_ → 锁外 disconnect
                     * 旧 route(叫醒卡住的 recv) → 等 inflight 归零 → 锁内 release 旧
                     * route → 锁外 create 新 route → 锁内发布。所以 release 旧 route
                     * 与 recv 不再可能并发 —— 这正是原先 channel_mtx_ 想挡的事。
                     * 发布端已推进到新 generation, 旧 route 的共享同步对象可能已被
                     * clear, 故旧 route 一律不再被操作。 */
                    route_session_.begin_rebuild(
                        generation,
                        [this]()
                        {
                            return std::make_shared<ipc::route>(topic_name_.c_str(), ipc::receiver, verbose_);
                        });
                }
                attached_generation = generation;
                if (!control_plane_.add_peer(attached_generation))
                {
                    /* add_peer 失败: 清空 route 且**不**建新对象(阶段 2 说明 §4
                     * 表格第 2 行)。stop_and_wake 拒绝新 lease 并叫醒在途 recv;
                     * wait_quiescent 之后对 current_route() 的拷贝调 release() 才安全。
                     * 下一轮循环会因 handshake_completed 仍为 false 而重新
                     * begin_rebuild, 即「不置 handshake_completed、稍后重试」。 */
                    route_session_.stop_and_wake();
                    route_session_.wait_quiescent();
                    std::shared_ptr<ipc::route> cur = route_session_.current_route();
                    if (cur && cur->valid())
                    {
                        cur->release();
                    }
                    continue;
                }
                peer_registered = true;
                /* 向控制面登记本订阅者的 libipc 连接 bit, 并由下面的循环持续
                 * 刷新心跳。发布端据此判定死连接 —— 取代了 force_push 里那套
                 * "没读完就算无效读者"的误伤逻辑。 */
                uint32_t cc_id = 0u;
                {
                    /* begin_rebuild 返回之后读 route: 此刻没有并发的 release
                     * (阶段 2 说明 §4 表格第 3 行)。current_route() 的拷贝让 route
                     * 在读取 connected_id() 期间保活。 */
                    std::shared_ptr<ipc::route> cur = route_session_.current_route();
                    cc_id = (cur && cur->valid()) ? cur->connected_id() : 0u;
                    peer_slot_ = control_plane_.acquire_peer_slot(attached_generation, cc_id);
                }

                /* 连接位耗尽 ⇒ **不能**宣布握手完成。
                 *
                 * libipc 的接收方连接位图是 cc_t = uint32_t, 只有 32 位; 位满时
                 * connect() 返回 0(circ/elem_def.h 的 "connection-slot is full")。旧实现
                 * 拿到 cc_id == 0 之后照样 handshake_completed.store(true), 于是第 33 个
                 * 订阅者进入一种**假成功态**: InitChannel 不报错、日志正常、
                 * handshake_completed 为真, 但一条消息都收不到 —— 而发布端只用
                 * recv_count() 判有无接收者, 两端都看不见这个截断。
                 *
                 * 控制面的 PeerSlot 表是 64 槽而连接位只有 32 个, 这个 2× 差额正是黑洞的
                 * 容量。acquire_peer_slot 本来就在 cc_id == 0 时返回 -1 —— 信号一直都在,
                 * 只是被丢掉了。见 docs/shm_defect_fixes.md 第 2 条。
                 *
                 * 处置: 不置 handshake_completed, 退掉已登记的 peer, 让下一轮重试 ——
                 * 有订阅者退出让出位时就能接上。告警**不受 verbose_ 约束**: 这是静默失败,
                 * 不该要求开了调试开关才看得见。 */
                if (cc_id == 0)
                {
                    static std::atomic<bool> warned_once{false};
                    if (!warned_once.exchange(true, std::memory_order_relaxed))
                    {
                        std::cerr << "\033[31m[" << topic_name_
                                  << "SubInfo] connection slots exhausted (max "
                                  << kMaxShmReceiversPerTopic
                                  << " receivers per topic); this subscriber is NOT connected and "
                                     "will receive nothing. Retrying until a slot frees up.\033[0m"
                                  << std::endl;
                    }
                    if (peer_registered)
                    {
                        control_plane_.remove_peer(attached_generation);
                        peer_registered = false;
                    }
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
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
                    /* 控制面离开 Ready: 与 add_peer 失败同处置(阶段 2 说明 §4
                     * 表格第 4 行)—— 清空 route 且不建新对象。 */
                    route_session_.stop_and_wake();
                    route_session_.wait_quiescent();
                    std::shared_ptr<ipc::route> cur = route_session_.current_route();
                    if (cur && cur->valid())
                    {
                        cur->release();
                    }
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
                    /* 阶段 2 说明 §3: 用 lease 取 route, 在**不持有 RouteSession 锁**
                     * 的情况下 recv, recv 返回后立刻 release_receive(无论 buffer 是否
                     * 为空)。lease 的 shared_ptr 在 recv 全程保活 route; release 旧 route
                     * 只可能发生在 inflight 归零之后, 所以 recv 与 release 不再并发。
                     * ⚠️ 不得因 lease.generation 落后于当前 generation 就丢掉已弹出的
                     * buffer —— 字节已从旧 route 弹出, 丢掉就是丢消息(说明 §3)。 */
                    auto lease = route_session_.acquire_receive();
                    if (!lease.has_value())
                    {
                        /* stopping / rebuilding / 尚无 route: 与未握手时同样睡 50ms
                         * (说明 §3), 不再忙等自旋。 */
                        std::this_thread::sleep_for(std::chrono::milliseconds(50));
                        continue;
                    }
                    buff_t raw_data;
                    try
                    {
                        raw_data = lease->route->recv(50);
                    }
                    catch (...)
                    {
                        /* 阶段 2 说明 §2/§3: 每个成功 lease 必须配对释放，包括 recv
                         * 抛异常的出口。单次 route 错误不应让工作线程因未配对 lease
                         * 卡死后续重建；释放后按未收到数据处理，继续循环。 */
                        route_session_.release_receive();
                        continue;
                    }
                    /* 测试缝(§4.2): 记录这次 recv 的返回形状。钩子读 route->connected_id()
                     * 即可判定"是 disconnect 叫醒(0)还是真的收到消息(非 0)" —— 这是
                     * 析构守门里"叫醒而非超时"那条判据的观测面。
                     * ⚠️ 此刻 inflight 仍为 1 ⇒ 钩子在这里**不得阻塞**: 阻塞会让任何
                     * 并发的 begin_rebuild 卡在第 4 步等归零。要暂停请用下面那个点。 */
                    detail::FireSeam({detail::SeamPoint::kAfterRecv, lease->generation,
                                      lease->route.get(), raw_data.data(), raw_data.size()});
                    route_session_.release_receive();
                    /* 测试缝(§4.1 的 I5 暂停点): recv 已返回、buffer 已在本线程手里, 而
                     * inflight 已归零(所以重建方能推进到 §4 第 5 步 release 旧 route)。
                     * 用例在这里把本线程停住, 去推进 generation, 再放行 —— 从而证明
                     * "已弹出的字节不会因为 lease.generation 落后于当前 generation 而
                     * 被丢弃"。**本点是唯一允许阻塞的收包点**(钩子内可阻塞, 见
                     * shm_sub_seam.h 的设计约束)。⚠️ 必须留在**分流之前**: 阶段 3 提取
                     * process_received_buffer 时本点随函数体一并迁移(见 shm_sub_seam.h)。 */
                    detail::FireSeam({detail::SeamPoint::kAfterRecvRelease, lease->generation,
                                      lease->route.get(), raw_data.data(), raw_data.size()});
                    if (raw_data.empty())
                    {
                        continue;
                    }
                    /* ⛔ 叫醒伪影门 —— 阶段 2 §5 第 4 步(stop_and_wake 用 disconnect
                     * 叫醒卡住的 recv)的必然副产物, 见 docs/消息接收架构改造/
                     * 阶段2_RouteSession实现说明.md §10。
                     *
                     * 被 disconnect()/quit_waiting() 叫醒的那次 recv 返回的**不是**空
                     * buffer, 而是 ipc::data_length 字节的**全零**缓冲(empty() 为 false);
                     * 机理与充要判据见 dzIPC/common/wire_accept.h 的 IsWakeupArtifact。
                     * 不放它进来, 话题 msg_id == 0 时 AcceptWire 的 check_id 恰好通过
                     * (尾 4 字节全零 == 0)且 deserialize_ok 为真 ⇒ 一条全零假消息被 push
                     * 进 msg_queue_, 用户侧 try_get_clone 无发送方也能取到; msg_id != 0
                     * 时虽被挡下, 但计数器被污染成 kTlvIdSkipped。
                     *
                     * ⚠️ 这道门必须与判空**同层、留在分流之前**: 阶段 3 提取
                     * process_received_buffer 时它属于「raw_data 非空之后」的函数体,
                     * 必须随之一并搬走, 不得只留在调用点 —— 否则阶段 5 的 worker 会各自
                     * 漏掉一处。
                     * ⚠️ 判据是**内容**(整段全零), 不是 generation: 后者会违反 RouteSession
                     * 的 I5(真消息可能已在 disconnect 前弹出), 把真 buffer 丢掉。 */
                    if (IsWakeupArtifact(raw_data))
                    {
                        /* 计数与丢弃分开: IsWakeupArtifact 是纯判别(单测会大量调它),
                         * 诊断计数只在这里涨 —— 见 wire_accept.h 的调用约定。 */
                        NoteWakeupArtifact();
                        continue;
                    }
                    /* 双 wire 分流 + 拒收计数 (docs/dzflat_shm.md §3.8 / wire_accept.h)
                     *
                     * 段首 4 字节: DZFlat 是 magic 'DZFL', TLV 是首字段名的长度(小整数),
                     * 结构上不可能碰撞, 故可无条件判别。分两条投递队列:
                     *
                     *   view 队列(借样 Sample)  ←  DZFlat 段 + 话题类型支持 DZFlat(typed/
                     *       由 get()/try_get() 服务, 零拷贝         generator 生成, schema_hash≠0)
                     *   clone 队列(物化对象)    ←  TLV 段, 以及发给 schema-less 话题
                     *       由 get_clone()/try_get_clone() 服务   (GenericMessage / 手写类型)
                     *       的 DZFlat 段 —— GenericMessage 无 C++ schema, 只能把段字节
                     *       拷进 dzflat_seg_ 留待 Python 解码, 见 generic_message.hpp。
                     *
                     * 严格分流: TLV 消息永远不会被 get()/try_get() 物化; 混合 wire(灰度期)
                     * 需要调用方两条都 drain。 */
                    std::uint32_t exp_id = 0, exp_hash = 0;
                    bool viewable = false;
                    {
                        std::lock_guard<std::mutex> lock(topic_msg_mtx_);
                        if (!topic_msg_)
                        {
                            continue;
                        }
                        exp_id = msg_id_;   /* 注册键 = 话题模板的 msg_id */
                        exp_hash = topic_msg_->topic()->dzflat_schema_hash();
                        viewable = (exp_hash != 0);   /* 仅 generator 生成的 typed 话题 */
                    }

                    const bool is_dzflat =
                        dzflat::looks_like_dzflat(raw_data.data(), raw_data.size());
                    if (is_dzflat)
                    {
                        if (viewable)
                        {
                            std::uint32_t seg_id = 0;
                            if (!IpcMsgBase::dzflat_peek_msg_id(raw_data.data(), raw_data.size(),
                                                                seg_id)
                                || seg_id != exp_id)
                            {
                                detail::NoteDzFlatRx(detail::DzFlatRxEvent::kDzFlatIdSkipped);
                                continue;
                            }
                            dzflat::SegHeader h{};
                            std::memcpy(&h, raw_data.data(), sizeof(h));
                            if (h.schema_hash != exp_hash)
                            {
                                detail::NoteDzFlatRx(
                                    detail::DzFlatRxEvent::kDzFlatSchemaDrop);
                                continue;
                            }
                            detail::NoteDzFlatRx(detail::DzFlatRxEvent::kDzFlatAccepted);
                            /* 借样: raw_data(buff_t) 让 chunk 的 conns 引用保持非零,
                             * 原样移进 Sample —— chunk 在用户读完字段前不会归还池。 */
                            view_queue_->push(std::make_shared<Sample>(
                                std::move(raw_data), seg_id, exp_hash));
                            continue;
                        }
                        /* schema-less 话题: 话题若是 GenericMessage, 把段**借**给它(不拷
                         * 字节), Python 按段头 schema_hash 查表解码 —— 这是 Python 侧零拷贝
                         * 的落点。msg_id 需对上; schema 无从在 C++ 校验(GenericMessage 无
                         * schema)。话题不是 GenericMessage(手写类型)收到 DZFlat = 类型不匹配,
                         * 丢弃。
                         *
                         * UF-012 借样配额: 借进 msg_queue_ 的消息每条钉一块 chunk, 而队列
                         * 深度是用户配置的 queue_size(可能远大于池容量)。配额内照旧借样
                         * (零拷贝); 配额满即物化(dzflat_read 拷进堆) —— raw_data 是循环体
                         * 作用域的 buff_t, 迭代末尾析构即还池。降级是每消息一次拷贝,
                         * 不降级成系统级池饿死。 */
                        std::uint32_t seg_id = 0, seg_hash = 0;
                        {
                            dzflat::SegHeader h{};
                            std::memcpy(&h, raw_data.data(), sizeof(h));
                            seg_hash = h.schema_hash;
                        }
                        if (!IpcMsgBase::dzflat_peek_msg_id(raw_data.data(), raw_data.size(),
                                                            seg_id)
                            || seg_id != exp_id)
                        {
                            detail::NoteDzFlatRx(detail::DzFlatRxEvent::kDzFlatIdSkipped);
                            continue;
                        }
                        detail::NoteDzFlatRx(detail::DzFlatRxEvent::kDzFlatAccepted);
                        std::shared_ptr<TopicData> local_msg;
                        {
                            std::lock_guard<std::mutex> lock(topic_msg_mtx_);
                            if (!topic_msg_)
                            {
                                continue;
                            }
                            local_msg.reset(topic_msg_->clone());
                        }
                        /* ⛔ 短路顺序即正确性: 配额满时**不调** adopt —— adopt 按值收
                         * buffer, 即便返回 false, raw_data 也已被 move 掏空, 之后的
                         * 物化分支就拿不到段字节了。配额满 ⇒ raw_data 完好 ⇒ 走物化。 */
                        const bool quota_ok =
                            adopt_borrowed_.load(std::memory_order_relaxed) <
                            static_cast<int>(adopt_cap_);
                        if (quota_ok &&
                            local_msg->topic()->dzflat_adopt(std::move(raw_data), seg_hash))
                        {
                            adopt_borrowed_.fetch_add(1, std::memory_order_relaxed);
                            std::shared_ptr<IpcMsgBase> ptr_cache;
                            local_msg->swap(ptr_cache);
                            msg_queue_->push(std::move(ptr_cache));
                            continue;
                        }
                        /* 配额满(GenericMessage)→ 物化拷贝 + 溢出计数; 非 GenericMessage
                         * (手写类型)→ adopt 已把 raw_data 掏空且 dzflat_read 基类返回
                         * false, 落到下面的类型不匹配丢弃 —— 与旧行为一致。 */
                        if (!raw_data.empty() &&
                            local_msg->topic()->dzflat_read(raw_data.data(), raw_data.size()))
                        {
                            detail::NoteDzFlatRx(
                                detail::DzFlatRxEvent::kDzFlatAdoptSpilled);
                            std::shared_ptr<IpcMsgBase> ptr_cache;
                            local_msg->swap(ptr_cache);
                            msg_queue_->push(std::move(ptr_cache));
                            continue;
                        }
                        continue;   /* 非 GenericMessage 的 schema-less 话题: 类型不匹配, 丢弃 */
                    }
                    else if (dzflat::has_dzflat_magic(raw_data.data(), raw_data.size()))
                    {
                        /* magic 在但 looks_like_dzflat 不过 ⇒ 段头自相矛盾 / layout_ver
                         * 不认识 ⇒ 损坏段, 不是"不是 DZFlat"。 */
                        detail::NoteDzFlatRx(detail::DzFlatRxEvent::kDzFlatHeaderBad);
                        continue;
                    }

                    /* TLV(或 schema-less 的 DZFlat)→ 物化, 与 ser/cli 共用 AcceptWire。 */
                    std::shared_ptr<TopicData> local_msg;
                    {
                        std::lock_guard<std::mutex> lock(topic_msg_mtx_);
                        if (!topic_msg_)
                        {
                            continue;
                        }
                        local_msg.reset(topic_msg_->clone());
                    }
                    if (!AcceptWire(raw_data, local_msg->msg_id(), *local_msg->topic()))
                    {
                        continue;
                    }
                    std::shared_ptr<IpcMsgBase> ptr_cache;                    local_msg->swap(ptr_cache);
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
/* ---- 视图路径: 只服务借样的 DZFlat 段(见 subscribe 循环的分流) ---- */
void shm_sub_ipc::get(Sample& out)
{
    std::shared_ptr<Sample> s;
    view_queue_->pop(s);   /* 阻塞直到有 Sample; TLV-only 话题请用 get_clone, 见基类注释 */
    if (s)
    {
        out = std::move(*s);
    }
}

bool shm_sub_ipc::get(Sample& out, std::uint64_t tm_ms)
{
    /* CircularQueue::pop 本来就支持超时(见其 tm 参数), 之前只是没接线 —— 于是只发 TLV 的
     * 话题上调 get(Sample&) 会永久挂死。见 docs/shm_defect_fixes.md 第 4 条。 */
    std::shared_ptr<Sample> s;
    if (!view_queue_->pop(s, tm_ms))
    {
        return false;   // 超时
    }
    if (!s)
    {
        return false;
    }
    out = std::move(*s);
    return true;
}

bool shm_sub_ipc::try_get(Sample& out)
{
    std::shared_ptr<Sample> s;
    if (!view_queue_->try_pop(s))
    {
        return false;
    }
    if (!s)
    {
        return false;
    }
    out = std::move(*s);
    return true;
}

void shm_sub_ipc::get_clone(std::shared_ptr<TopicData>& msg)
{
    std::shared_ptr<IpcMsgBase> ipc_msg;
    msg_queue_->pop(ipc_msg);
    /* 出队即离开配额账面(消息可能带着借样移交给用户 —— 与 view 路径同一契约:
     * 队列驻留有上限, 用户手持期是用户的约定)。 */
    if (ipc_msg && ipc_msg->dzflat_is_borrowed())
    {
        adopt_borrowed_.fetch_sub(1, std::memory_order_relaxed);
    }
    msg->update(ipc_msg);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool shm_sub_ipc::try_get_clone(std::shared_ptr<TopicData>& msg)
{
    std::shared_ptr<IpcMsgBase> ipc_msg;
    if (msg_queue_->try_pop(ipc_msg))
    {
        if (ipc_msg && ipc_msg->dzflat_is_borrowed())
        {
            adopt_borrowed_.fetch_sub(1, std::memory_order_relaxed);
        }
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

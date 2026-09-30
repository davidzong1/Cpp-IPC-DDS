#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "dzIPC/common/topic_data.h"

class Sample;

namespace dzIPC {
class IPC_EXPORT pub_ipc_base
{
public:
    explicit pub_ipc_base(const std::shared_ptr<TopicData>& msg, const std::string& topic_name, size_t domain_id,
                          bool verbose)
    {}

    virtual ~pub_ipc_base() = 0;
    virtual void reset_message(const std::shared_ptr<TopicData>& msg) = 0;
    virtual void InitChannel(std::string extra_info = "") = 0;
    virtual bool publish(std::shared_ptr<IpcMsgBase> msg) = 0;
    virtual bool publish_best_effort(std::shared_ptr<IpcMsgBase> msg) { return publish(std::move(msg)); }
    virtual bool publish_blocking(std::shared_ptr<IpcMsgBase> msg, std::uint64_t) { return publish(std::move(msg)); }
    virtual bool publish_for_sniffer(std::shared_ptr<IpcMsgBase> msg) { return publish_best_effort(std::move(msg)); }

    /* ------------------------------------------------------------------ 预构造段发布
     *
     * 供"schema 在调用方"的进程使用 —— 今天的唯一使用者是 Python: 生成的 schema 只以
     * **数据**形式发到 Python 侧(`gen_msgs/_dzflat_schema.py`), C++ 这边没有任何 TU 会
     * 编译生成头文件, 所以 GenericMessage 自己永远写不出平坦段(见 generic_message.hpp)。
     * 于是段由 Python 按 schema 写好, 原样交过来, 由传输层负责送出去。
     *
     * seg/len = **完整的** DZFlat 段(含 32B 段头), 不做任何解析或转换。
     *
     * 返回 false = "本次没走平坦段", 调用方**必须**回退普通 publish()(TLV)。false 是常态
     * 而不是错误: 开关未开 / 段头不合法(截断、layout_ver 不认识)/ 段头 msg_id 与本话题
     * 模板不符 / 无接收方 / chunk 池耗尽 / nodelet 拓扑 / 该传输没有平坦腿, 都会走到这里。
     *
     * 默认实现返回 false: 新传输不实现它就自动获得"回退 TLV"的行为, 不必逐个补门。
     */
    virtual bool publish_prebuilt_segment(const void* /*seg*/, std::size_t /*len*/) { return false; }

    virtual bool has_subscribed() const = 0;
    std::atomic<bool> exit_flag{false};
};

class sub_ipc_base
{
public:
    explicit IPC_EXPORT sub_ipc_base(const std::shared_ptr<TopicData>& msg, const std::string& topic_name,
                                     size_t domain_id, const size_t queue_size, bool verbose)
    {}

    virtual ~sub_ipc_base() = 0;
    virtual void InitChannel(std::string extra_info) = 0;
    virtual void reset_message(const std::shared_ptr<TopicData>& msg) = 0;
    /* ---- 视图路径(借样, 只服务 DZFlat 段) ----
     *
     * get() 阻塞直到拿到一个借样 Sample。注意: 若本话题恒发 TLV(DZFlat 未开/类型
     * 不支持), 视图队列永无 Sample, get() 会一直阻塞 —— 那类话题请用 get_clone()。
     * 混合 wire(灰度期)需要调用方 get()+get_clone() 双 drain, 见 Sample 头注释。
     *
     * 两种传输都有这条路径, 但**成本不同**: SHM 上是真零拷贝(借发布方写好的 chunk),
     * UDP 上恒有一次整段拷贝(接收缓冲是本进程复用的临时内存, 且分帧会把页尾插进段
     * 中间 —— 见 chunk_rev_topic 的 out_payload 契约)。语义与生命周期两边一致。 */
    virtual void get(Sample& out) = 0;
    virtual bool try_get(Sample& out) = 0;

    /* 带超时的视图 get: 超时返回 false 而不是永久阻塞。
     *
     * 为什么需要它: 视图队列只承载 DZFlat 段, 而"到达的段是不是 DZFlat"由**发布端**决定
     * (开关未开 / 类型不支持 / 无接收方 / chunk 池耗尽都会静默回退 TLV, 见
     * dzflat_shm.md §3.8)。所以在只发 TLV 的话题上, 视图队列**永远是空的** —— 不带超时的
     * get() 不是"等数据", 而是**注定挂死**。
     *
     * tm 单位毫秒。两种传输都由队列的超时能力实现(circularqueue.h 的 pop(MsgPtr&, tm))。 */
    virtual bool get(Sample& out, std::uint64_t tm_ms) = 0;

    /* ---- 物化路径(TLV + 快速路径克隆对象 + schema-less 话题) ---- */
    virtual void get_clone(std::shared_ptr<TopicData>& msg) = 0;
    virtual bool try_get_clone(std::shared_ptr<TopicData>& msg) = 0;
    std::atomic<bool> exit_flag{false};
};

inline pub_ipc_base::~pub_ipc_base() = default;
inline sub_ipc_base::~sub_ipc_base() = default;
}   // namespace dzIPC

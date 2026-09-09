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
    /* ---- 视图路径(零拷贝, 只服务 DZFlat 段; socket 恒返回 false) ----
     *
     * get() 阻塞直到拿到一个借样 Sample。注意: 若本话题恒发 TLV(DZFlat 未开/类型
     * 不支持), 视图队列永无 Sample, get() 会一直阻塞 —— 那类话题请用 get_clone()。
     * 混合 wire(灰度期)需要调用方 get()+get_clone() 双 drain, 见 Sample 头注释。 */
    virtual void get(Sample& out) = 0;
    virtual bool try_get(Sample& out) = 0;

    /* ---- 物化路径(TLV + 快速路径克隆对象 + schema-less 话题) ---- */
    virtual void get_clone(std::shared_ptr<TopicData>& msg) = 0;
    virtual bool try_get_clone(std::shared_ptr<TopicData>& msg) = 0;
    std::atomic<bool> exit_flag{false};
};

inline pub_ipc_base::~pub_ipc_base() = default;
inline sub_ipc_base::~sub_ipc_base() = default;
}   // namespace dzIPC

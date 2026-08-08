#pragma once
#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>
#include <vector>
#include "dzIPC/common/topic_data.h"

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
    virtual void get(std::shared_ptr<TopicData>& msg) = 0;
    virtual bool try_get(std::shared_ptr<TopicData>& msg) = 0;
    std::atomic<bool> exit_flag{false};
};

inline pub_ipc_base::~pub_ipc_base() = default;
inline sub_ipc_base::~sub_ipc_base() = default;
}   // namespace dzIPC

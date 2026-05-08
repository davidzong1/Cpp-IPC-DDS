#pragma once
#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <vector>
#include "dzIPC/common/topic_data.h"

namespace dzIPC {
class IPC_EXPORT pub_ipc_base
{
public:
    explicit pub_ipc_base(const std::shared_ptr<TopicData>& msg, const std::string& topic_name, size_t domain_id,
                          bool verbose)
    {}

    ~pub_ipc_base() = default;
    virtual void reset_message(const std::shared_ptr<TopicData>& msg) = 0;
    virtual void InitChannel(std::string extra_info = "") = 0;
    virtual bool publish(std::shared_ptr<IpcMsgBase> msg) = 0;
    virtual bool has_subscribed() const = 0;
    std::atomic<bool> exit_flag{false};
};

class sub_ipc_base
{
public:
    explicit IPC_EXPORT sub_ipc_base(const std::shared_ptr<TopicData>& msg, const std::string& topic_name,
                                     size_t domain_id, const size_t queue_size, bool verbose)
    {}

    ~sub_ipc_base() = default;
    virtual void InitChannel(std::string extra_info) = 0;
    virtual void reset_message(const std::shared_ptr<TopicData>& msg) = 0;
    virtual void get(std::shared_ptr<TopicData>& msg) = 0;
    virtual bool try_get(std::shared_ptr<TopicData>& msg) = 0;
    std::atomic<bool> exit_flag{false};
};
}   // namespace dzIPC
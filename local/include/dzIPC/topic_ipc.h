#pragma once
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/socket_pub_sub_ipc.h"
#include "dzIPC/type.h"
#include "libipc/export.h"

namespace dzIPC {
namespace pimpl {
class IPC_EXPORT publisher_ipc_impl
{
public:
    explicit publisher_ipc_impl(const std::shared_ptr<TopicData>& msg, const std::string& topic_name, size_t domain_id,
                                IPCType ipc_type, bool verbose = false, bool enable_thread_qos = Qos::NotUseQos,
                                int cpu_id = CPU_CORE::None, int thread_priority = DispatchPriority::LowPriority);
    ~publisher_ipc_impl();
    void InitChannel(std::string extra_info = "");
    void reset_message(const std::shared_ptr<TopicData>& msg);
    bool publish(std::shared_ptr<IpcMsgBase> msg);
    bool publish_best_effort(std::shared_ptr<IpcMsgBase> msg);
    bool publish_blocking(std::shared_ptr<IpcMsgBase> msg, std::uint64_t tm);
    bool publish_for_sniffer(std::shared_ptr<IpcMsgBase> msg);
    bool has_subscribed() const;
    bool exit_flag() const;

private:
    class publisher_ipc_impl_;
    publisher_ipc_impl_* p_;
};

class IPC_EXPORT subscriber_ipc_impl
{
public:
    explicit subscriber_ipc_impl(const std::shared_ptr<TopicData>& msg, const std::string& topic_name, size_t domain_id,
                                 const size_t queue_size, IPCType ipc_type, bool verbose = false,
                                 bool enable_thread_qos = Qos::NotUseQos, int cpu_id = CPU_CORE::None, int thread_priority = DispatchPriority::LowPriority);
    ~subscriber_ipc_impl();
    void InitChannel(std::string extra_info = "");
    void reset_message(const std::shared_ptr<TopicData>& msg);
    void get(std::shared_ptr<TopicData>& msg);
    bool try_get(std::shared_ptr<TopicData>& msg);
    bool exit_flag() const;

private:
    class subscriber_ipc_impl_;
    subscriber_ipc_impl_* p_;
};
}   // namespace pimpl
}   // namespace dzIPC

#pragma once
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/socket_pub_sub_ipc.h"
#include "dzIPC/type.h"
#include "libipc/export.h"

namespace dzIPC { class Sample; }

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
    /* 视图路径(零拷贝, DZFlat 段; 无 owning 对象可记日志)。 */
    void get(Sample& out);
    bool try_get(Sample& out);

    /* 物化路径(TLV + 快速路径对象; 记订阅事件日志)。 */
    void get_clone(std::shared_ptr<TopicData>& msg);
    bool try_get_clone(std::shared_ptr<TopicData>& msg);
    bool exit_flag() const;

private:
    class subscriber_ipc_impl_;
    subscriber_ipc_impl_* p_;
};
}   // namespace pimpl
}   // namespace dzIPC

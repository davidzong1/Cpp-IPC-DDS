#pragma once
#include "dzIPC/pub_sub_base.h"
#include "dzIPC/type.h"

namespace dzIPC::shared_net {
class IPC_EXPORT Publisher final : public pub_ipc_base {
public:
    Publisher(const std::shared_ptr<TopicData>&, const std::string&, std::size_t domain,
              bool verbose = false, bool qos = false, int cpu = -1, int priority = DispatchPriority::LowPriority);
    ~Publisher() override;
    void InitChannel(std::string extra = "") override;
    void reset_message(const std::shared_ptr<TopicData>&) override;
    bool publish(std::shared_ptr<IpcMsgBase>) override;
    bool publish_best_effort(std::shared_ptr<IpcMsgBase>) override;
    bool publish_blocking(std::shared_ptr<IpcMsgBase>, std::uint64_t timeout_ms) override;
    bool publish_for_sniffer(std::shared_ptr<IpcMsgBase>) override;
    bool publish_prebuilt_segment(const void*, std::size_t) override;
    bool has_subscribed() const override;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
class IPC_EXPORT Subscriber final : public sub_ipc_base {
public:
    Subscriber(const std::shared_ptr<TopicData>&, const std::string&, std::size_t domain,
               std::size_t queue_size, bool verbose = false, bool qos = false, int cpu = -1, int priority = DispatchPriority::LowPriority);
    ~Subscriber() override;
    void InitChannel(std::string extra = "") override;
    void reset_message(const std::shared_ptr<TopicData>&) override;
    void get(Sample&) override;
    bool get(Sample&, std::uint64_t timeout_ms) override;
    bool try_get(Sample&) override;
    void get_clone(std::shared_ptr<TopicData>&) override;
    bool try_get_clone(std::shared_ptr<TopicData>&) override;
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
} // namespace dzIPC::shared_net

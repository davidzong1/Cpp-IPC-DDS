#pragma once
#include "dzIPC/net/shm_wire.h"
#include "dzIPC/net/outbox.h"

namespace dzIPC::net {
struct PublishOutcome {
    SubmitState local = SubmitState::NotSubmitted, network = SubmitState::NotSubmitted;
    SendResultBody remote;
    std::uint64_t sequence = 0;
    bool success = false;
};
// 公共工厂接入前可由 probe 使用；本机先提交，网络随后独立接管。
class PublisherEndpoint {
public:
    PublisherEndpoint(std::shared_ptr<ClientRuntime>, RouteDescriptor);
    ~PublisherEndpoint();
    PublishOutcome publish(IpcMsgBase&, Delivery = Delivery::BestEffort, std::uint64_t timeout_ms = 0);
    PublishOutcome prebuilt(ByteView, Delivery = Delivery::BestEffort, std::uint64_t timeout_ms = 0);
    PublishOutcome publish_blob(const WireBlob&, Delivery = Delivery::BestEffort, std::uint64_t timeout_ms = 0);
    bool has_subscribers() const;
    Identity publisher_id() const;
    void close();
private:
    struct Impl; std::unique_ptr<Impl> impl_;
};
Bytes registration_body(Identity, const RouteDescriptor&);
} // namespace dzIPC::net

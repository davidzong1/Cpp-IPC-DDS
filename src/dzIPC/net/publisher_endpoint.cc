#include "dzIPC/net/publisher_endpoint.h"
#include "dzIPC/common/nodelet_config.h"
#include "local_control_linux.h"
#include <atomic>
#include <shared_mutex>
#include <mutex>
#include <unistd.h>

namespace dzIPC::net {
Bytes registration_body(Identity id, const RouteDescriptor& descriptor) {
    Bytes body(id.begin(), id.end()), encoded;
    if (!encode_descriptor(descriptor, true, encoded)) throw std::invalid_argument("登记描述无效");
    body.insert(body.end(), encoded.begin(), encoded.end()); return body;
}
struct PublisherEndpoint::Impl {
    const pid_t owner = getpid(); mutable std::shared_mutex gate;
    std::shared_ptr<ClientRuntime> runtime; RouteDescriptor descriptor;
    Identity id = local::random_identity(); std::atomic<std::uint64_t> sequence{0};
    std::unique_ptr<ShmWireWriter> writer;
    bool closed = false;
    std::uint64_t next() {
        auto old = sequence.load(); do { if (old == UINT64_MAX) throw std::runtime_error("发布序号耗尽"); } while (!sequence.compare_exchange_weak(old, old + 1)); return old + 1;
    }
    bool network_required(Delivery delivery) const {
        if (delivery == Delivery::Reliable || !runtime->healthy()) return true;
        const auto hint = runtime->route_state(id);
        return hint.publisher_id != id || !hint.state_version || !hint.synchronized || hint.remote_ready_count != 0;
    }
    PublishOutcome deliver(const WireBlob& blob, Delivery delivery, std::uint64_t deadline, bool local_required) {
        PublishOutcome out; out.sequence = next(); out.remote.publisher_id = id; out.remote.sequence = out.sequence;
        if (deadline && local::monotonic_ns() >= deadline) { out.remote.result = SendResultCode::TimedOut; return out; }
        out.local = local_required ? writer->try_commit_until(blob, deadline) : SubmitState::NotRequired;
        if (local_required && out.local == SubmitState::NotRequired) out.local = SubmitState::NotSubmitted;
        if (!network_required(delivery)) { out.network = SubmitState::NotRequired; out.remote.result = SendResultCode::NoSubscribers; }
        else if (!runtime->healthy()) out.remote.result = SendResultCode::GatewayLost;
        else {
            SendTicket ticket;
            try {
                OutboxHeader h; h.gateway_epoch = runtime->gateway_epoch(); h.session_id = runtime->session_id(); h.publisher_id = id; h.sequence = out.sequence;
                h.route = descriptor.key; h.schema_hash = blob.schema_hash(); h.encoding = blob.encoding(); h.delivery = delivery;
                if (delivery == Delivery::Reliable) { ticket = runtime->prepare_send(id, out.sequence); h.request_id = ticket.request_id; h.deadline_monotonic_ns = deadline; }
                const auto submitted = runtime->submit_outbox(h, blob.view()); out.network = submitted.state;
                if (delivery == Delivery::Reliable) {
                    // 未接管时立即结束等待者；不会留到原 deadline，也不会重发本机腿。
                    if (out.network == SubmitState::NotSubmitted) out.remote = runtime->cancel_send(ticket, SendResultCode::Busy);
                    else out.remote = runtime->wait_send(ticket, deadline);
                }
            } catch (...) {
                out.remote.result = SendResultCode::GatewayLost;
                if (ticket.request_id) runtime->cancel_send(ticket, SendResultCode::GatewayLost);
            }
        }
        out.success = delivery == Delivery::Reliable ? reliable_result(out.local, out.remote) : best_effort_result(out.local, out.network);
        return out;
    }
    static bool timeout(Delivery delivery, std::uint64_t ms, std::uint64_t& deadline, PublishOutcome& failure) {
        if (delivery == Delivery::BestEffort) return true;
        if (!ms || ms > 5000) { failure.remote.result = !ms ? SendResultCode::TimedOut : SendResultCode::UnsupportedTimeout; return false; }
        deadline = local::monotonic_ns() + ms * 1000000; return true;
    }
};
PublisherEndpoint::PublisherEndpoint(std::shared_ptr<ClientRuntime> runtime, RouteDescriptor descriptor) : impl_(new Impl) {
    impl_->runtime = std::move(runtime); impl_->descriptor = std::move(descriptor);
    if (!impl_->runtime || !impl_->runtime->healthy()) throw std::runtime_error("网关会话不可用");
    const auto reply = impl_->runtime->request(LocalKind::RegisterPub, registration_body(impl_->id, impl_->descriptor));
    if (reply.header.kind != LocalKind::PubRegistered) throw std::runtime_error("发布者登记失败");
    try { impl_->writer = std::make_unique<ShmWireWriter>(impl_->descriptor, false, impl_->runtime->gateway_epoch(), &impl_->runtime->metrics()); impl_->runtime->attach_outbox(); }
    catch (...) { try { Bytes b(impl_->id.begin(), impl_->id.end()); b.push_back(1); impl_->runtime->request(LocalKind::Unregister, b); } catch (...) {} throw; }
}
PublisherEndpoint::~PublisherEndpoint() { if (impl_->owner != getpid()) { impl_.release(); return; } close(); }
PublishOutcome PublisherEndpoint::publish(IpcMsgBase& message, Delivery delivery, std::uint64_t ms) {
    PublishOutcome failure; std::uint64_t deadline = 0;
    if (impl_->owner != getpid() || !Impl::timeout(delivery, ms, deadline, failure)) return failure;
    MetricTimer api(&impl_->runtime->metrics(), NetStage::ApiReturn);
    std::shared_lock<std::shared_mutex> lock(impl_->gate); if (impl_->closed) return failure;
    const bool local_required = impl_->writer->has_subscribers();
    if (!impl_->runtime->healthy() || !impl_->network_required(delivery)) {
        failure.sequence = impl_->next(); failure.network = impl_->runtime->healthy() ? SubmitState::NotRequired : SubmitState::NotSubmitted;
        failure.remote.result = failure.network == SubmitState::NotRequired ? SendResultCode::NoSubscribers : SendResultCode::GatewayLost;
        failure.local = local_required ? impl_->writer->try_commit_local(message, IsDzFlatEnabled(), deadline) : SubmitState::NotRequired;
        if (local_required && failure.local == SubmitState::NotRequired) failure.local = SubmitState::NotSubmitted;
        failure.success = delivery == Delivery::Reliable ? reliable_result(failure.local, failure.remote) : best_effort_result(failure.local, failure.network); return failure;
    }
    WireBlob blob;
    try { MetricTimer timer(&impl_->runtime->metrics(), NetStage::Encode); if (!WireEncoder::encode(message, IsDzFlatEnabled(), blob)) return failure; } catch (...) { return failure; }
    impl_->runtime->metrics().add(NetMetric::encode_copy_bytes, blob.size());
    return impl_->deliver(blob, delivery, deadline, local_required);
}
PublishOutcome PublisherEndpoint::prebuilt(ByteView bytes, Delivery delivery, std::uint64_t ms) {
    PublishOutcome failure; std::uint64_t deadline = 0;
    if (impl_->owner != getpid() || !Impl::timeout(delivery, ms, deadline, failure)) return failure;
    MetricTimer api(&impl_->runtime->metrics(), NetStage::ApiReturn);
    std::shared_lock<std::shared_mutex> lock(impl_->gate); if (impl_->closed) return failure;
    const bool local_required = impl_->writer->has_subscribers(); WireBlob blob;
    if (!impl_->runtime->healthy() || !impl_->network_required(delivery)) {
        failure.sequence = impl_->next(); failure.network = impl_->runtime->healthy() ? SubmitState::NotRequired : SubmitState::NotSubmitted;
        failure.remote.result = failure.network == SubmitState::NotRequired ? SendResultCode::NoSubscribers : SendResultCode::GatewayLost;
        failure.local = local_required ? impl_->writer->try_commit_prebuilt(bytes, deadline) : SubmitState::NotRequired;
        if (local_required && failure.local == SubmitState::NotRequired) failure.local = SubmitState::NotSubmitted;
        failure.success = delivery == Delivery::Reliable ? reliable_result(failure.local, failure.remote) : best_effort_result(failure.local, failure.network); return failure;
    }
    try { MetricTimer timer(&impl_->runtime->metrics(), NetStage::Encode); if (!WireEncoder::encode_prebuilt(bytes, impl_->descriptor.key.msg_id, impl_->descriptor.schema_hash, blob)) return failure; } catch (...) { return failure; }
    impl_->runtime->metrics().add(NetMetric::encode_copy_bytes, blob.size());
    return impl_->deliver(blob, delivery, deadline, local_required);
}
PublishOutcome PublisherEndpoint::publish_blob(const WireBlob& blob, Delivery delivery, std::uint64_t ms) {
    PublishOutcome failure; std::uint64_t deadline = 0;
    if (impl_->owner != getpid() || !Impl::timeout(delivery, ms, deadline, failure)) return failure;
    MetricTimer api(&impl_->runtime->metrics(), NetStage::ApiReturn);
    std::shared_lock<std::shared_mutex> lock(impl_->gate); if (impl_->closed || blob.msg_id() != impl_->descriptor.key.msg_id ||
        (blob.encoding() == Encoding::DzFlat && impl_->descriptor.schema_hash && blob.schema_hash() != impl_->descriptor.schema_hash)) return failure;
    return impl_->deliver(blob, delivery, deadline, impl_->writer->has_subscribers());
}
bool PublisherEndpoint::has_subscribers() const {
    if (impl_->owner != getpid()) return false;
    std::shared_lock<std::shared_mutex> lock(impl_->gate); if (impl_->closed) return false;
    return impl_->writer->has_subscribers() || (impl_->runtime->healthy() && impl_->runtime->route_state(impl_->id).remote_ready_count != 0);
}
Identity PublisherEndpoint::publisher_id() const { return impl_->id; }
void PublisherEndpoint::close() {
    if (impl_->owner != getpid()) return;
    std::unique_lock<std::shared_mutex> lock(impl_->gate); if (impl_->closed) return; impl_->closed = true;
    impl_->writer->close_after_quiescent();
    try { if (impl_->runtime->healthy()) { Bytes b(impl_->id.begin(), impl_->id.end()); b.push_back(1); impl_->runtime->request(LocalKind::Unregister, b); } } catch (...) {}
}
} // namespace dzIPC::net

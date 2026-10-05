#include "dzIPC/shared_pub_sub_ipc.h"
#include "dzIPC/net/publisher_endpoint.h"
#include "dzIPC/net/shared_config.h"
#include "dzIPC/shm_pub_sub_ipc.h"
#include "dzIPC/common/channel_scope.h"
#include "local_control_linux.h"
#include "byte_codec.h"
#include <condition_variable>
#include <limits>
#include <shared_mutex>
#include <thread>
#include <unistd.h>

namespace dzIPC::shared_net {
namespace {
net::RouteDescriptor descriptor(const std::shared_ptr<TopicData>& msg, const std::string& name, std::size_t domain) {
    if (!msg || !msg->topic()) throw std::invalid_argument("共享话题模板为空");
    net::RouteDescriptor d; d.topic = name; d.key.msg_id = msg->msg_id(); d.schema_hash = msg->topic()->dzflat_schema_hash();
    d.key.scope = common::channel_scope_token(name, domain, common::ScopeKind::PubSub); return d;
}
std::shared_ptr<net::ClientRuntime> runtime() { net::require_network_backend(false); return net::ClientRuntime::acquire(net::process_config().control_path); }
struct Wake {
    std::mutex mutex; std::condition_variable changed; std::uint64_t version = 0;
    void notify() { std::lock_guard<std::mutex> lock(mutex); ++version; changed.notify_all(); }
};
}
struct Publisher::Impl {
    const pid_t owner = getpid(); mutable std::shared_mutex gate;
    std::shared_ptr<TopicData> model; std::string topic; std::size_t domain;
    std::shared_ptr<net::ClientRuntime> client;
    std::unique_ptr<net::PublisherEndpoint> endpoint;
    bool initialized = false;
    void initialize() {
        endpoint.reset(); client = runtime();
        endpoint = std::make_unique<net::PublisherEndpoint>(client, descriptor(model, topic, domain)); initialized = true;
    }
};
Publisher::Publisher(const std::shared_ptr<TopicData>& msg, const std::string& topic, std::size_t domain, bool verbose, bool qos, int cpu, int priority)
    : pub_ipc_base(msg, topic, domain, verbose), impl_(new Impl) {
    if (qos || cpu != -1 || priority != DispatchPriority::LowPriority) throw std::invalid_argument("共享后端不支持每个对象独立 QoS/绑核参数");
    descriptor(msg, topic, domain); impl_->model = msg; impl_->topic = topic; impl_->domain = domain; impl_->client = runtime();
}
Publisher::~Publisher() { if (impl_->owner != getpid()) { impl_.release(); return; } exit_flag.store(true); std::unique_lock<std::shared_mutex> lock(impl_->gate); impl_->endpoint.reset(); }
void Publisher::InitChannel(std::string) {
    if (impl_->owner != getpid()) throw std::runtime_error("fork 后不能使用旧发布句柄");
    std::unique_lock<std::shared_mutex> lock(impl_->gate); exit_flag.store(true);
    impl_->initialize(); exit_flag.store(false);
}
void Publisher::reset_message(const std::shared_ptr<TopicData>& msg) {
    if (impl_->owner != getpid()) throw std::runtime_error("fork 后不能使用旧发布句柄");
    std::unique_lock<std::shared_mutex> lock(impl_->gate); exit_flag.store(true); impl_->endpoint.reset();
    descriptor(msg, impl_->topic, impl_->domain); impl_->model = msg;
    if (impl_->initialized) impl_->initialize(); exit_flag.store(false);
}
bool Publisher::publish(std::shared_ptr<IpcMsgBase> message) { return publish_best_effort(std::move(message)); }
bool Publisher::publish_best_effort(std::shared_ptr<IpcMsgBase> message) {
    if (impl_->owner != getpid() || !message) return false;
    std::shared_lock<std::shared_mutex> lock(impl_->gate, std::try_to_lock);
    if (!lock.owns_lock() || exit_flag.load()) return false;
    if (!impl_->endpoint) return false;
    const auto result = impl_->endpoint->publish(*message); impl_->client->record_publish(result.local, result.network); return result.success;
}
bool Publisher::publish_blocking(std::shared_ptr<IpcMsgBase> message, std::uint64_t timeout) {
    if (impl_->owner != getpid() || !message) return false;
    std::shared_lock<std::shared_mutex> lock(impl_->gate, std::try_to_lock);
    if (!lock.owns_lock() || exit_flag.load()) return false;
    if (!impl_->endpoint) return false;
    const auto result = impl_->endpoint->publish(*message, net::Delivery::Reliable, timeout); impl_->client->record_publish(result.local, result.network); return result.success;
}
bool Publisher::publish_for_sniffer(std::shared_ptr<IpcMsgBase> message) { return publish_best_effort(std::move(message)); }
bool Publisher::publish_prebuilt_segment(const void* bytes, std::size_t size) {
    if (impl_->owner != getpid() || !IsDzFlatEnabled()) return false;
    std::shared_lock<std::shared_mutex> lock(impl_->gate, std::try_to_lock);
    if (!lock.owns_lock() || exit_flag.load()) return false;
    if (!impl_->endpoint) return false;
    const auto result = impl_->endpoint->prebuilt({bytes, size}); impl_->client->record_publish(result.local, result.network); return result.success;
}
bool Publisher::has_subscribed() const {
    if (impl_->owner != getpid()) return false;
    std::shared_lock<std::shared_mutex> lock(impl_->gate); return impl_->endpoint && impl_->endpoint->has_subscribers();
}
struct Subscriber::Impl {
    struct Generation {
        std::shared_ptr<net::ClientRuntime> client; net::Identity id{};
        std::shared_ptr<Wake> wake = std::make_shared<Wake>();
        info_pool::ScopedRegistration registration;
        std::unique_ptr<shm::shm_sub_ipc> reader;
        std::atomic<bool> active{false};
        ~Generation() {
            active.store(false); wake->notify();
            try { if (client && client->healthy() && net::nonzero(id)) { net::Bytes body(id.begin(), id.end()); body.push_back(2); client->request(net::LocalKind::Unregister, body); } } catch (...) {}
        }
    };
    const pid_t owner = getpid(); std::mutex operation, state;
    std::condition_variable drained; std::size_t readers = 0; bool closing = false;
    std::shared_ptr<TopicData> model; std::string topic; std::size_t domain, queue_size;
    bool verbose, qos, initialized = false; int cpu, priority;
    std::shared_ptr<Generation> current;
    std::shared_ptr<net::ClientRuntime> initial_client;
    std::shared_ptr<Generation> snapshot() { std::lock_guard<std::mutex> lock(state); return current; }
    void retire() {
        std::shared_ptr<Generation> old;
        { std::lock_guard<std::mutex> lock(state); old = std::move(current); }
        if (!old) return;
        old->active.store(false); old->wake->notify();
        // 先撤销网络代次，再关闭旧 reader；借出的 Sample 自己持有池租约。
        if (old->client->healthy()) { net::Bytes body(old->id.begin(), old->id.end()); body.push_back(2); old->client->request(net::LocalKind::Unregister, body); }
        old->id = {};
    }
    void initialize(const std::string& extra) {
        retire(); auto next = std::make_shared<Generation>(); next->client = runtime(); initial_client.reset(); next->id = net::local::random_identity();
        next->registration.rebind({info_pool::EntryKind::SocketSub, topic, info_pool::demangle(typeid(*model->topic()).name()), "shared_v1", domain, "mode=shared_v1;epoch=" + std::to_string(next->client->gateway_epoch())});
        if (!next->registration.valid()) throw std::runtime_error("RegistryFull: 共享订阅诊断池已满");
        auto reply = next->client->request(net::LocalKind::RegisterSub, net::registration_body(next->id, descriptor(model, topic, domain)));
        if (reply.header.kind != net::LocalKind::SubRegistered) throw std::runtime_error("共享订阅登记失败");
        const auto generation = net::codec::get(reply.body.data() + 16, 4);
        next->reader = std::make_unique<shm::shm_sub_ipc>(model, topic, domain, queue_size, verbose, qos, cpu, priority);
        next->reader->set_internal(true);
        const std::weak_ptr<Wake> wake = next->wake;
        next->reader->set_receive_notifier([wake] { if (auto w = wake.lock()) w->notify(); }); next->reader->InitChannel(extra);
        const auto deadline = net::local::monotonic_ns() + 2000000000ull;
        while (next->reader->connected_generation() != generation) {
            if (!next->client->healthy() || net::local::monotonic_ns() >= deadline) throw std::runtime_error("共享订阅 SHM generation 未连接");
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        net::Bytes body(next->id.begin(), next->id.end()); net::codec::append(body, generation, 4);
        reply = next->client->request(net::LocalKind::SubReady, body);
        if (reply.header.kind != net::LocalKind::SubReadyAck) throw std::runtime_error("共享订阅 Ready 失败");
        next->active.store(true); { std::lock_guard<std::mutex> lock(state); current = std::move(next); } initialized = true;
    }
    template<class Try> bool read(std::uint64_t timeout, Try attempt) {
        struct Reading {
            Impl& impl;
            ~Reading() { std::lock_guard<std::mutex> lock(impl.state); if (!--impl.readers) impl.drained.notify_all(); }
        };
        std::shared_ptr<Generation> selected;
        { std::lock_guard<std::mutex> lock(state); if (closing || !current) return false; selected = current; ++readers; }
        Reading reading{*this};
        auto generation = std::move(selected);
        const auto now = std::chrono::steady_clock::now();
        const auto maximum = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::time_point::max() - now).count();
        const auto end = timeout >= static_cast<std::uint64_t>(maximum) ? std::chrono::steady_clock::time_point::max() : now + std::chrono::milliseconds(timeout);
        for (;;) {
            std::uint64_t observed; { std::lock_guard<std::mutex> lock(generation->wake->mutex); observed = generation->wake->version; }
            if (!generation->active.load()) return false;
            if (attempt(*generation->reader)) return generation->active.load();
            if (!timeout) return false;
            std::unique_lock<std::mutex> lock(generation->wake->mutex);
            if (!generation->wake->changed.wait_until(lock, end, [&] { return !generation->active.load() || generation->wake->version != observed; })) return false;
        }
    }
};
Subscriber::Subscriber(const std::shared_ptr<TopicData>& msg, const std::string& topic, std::size_t domain, std::size_t queue, bool verbose, bool qos, int cpu, int priority)
    : sub_ipc_base(msg, topic, domain, queue, verbose), impl_(new Impl) {
    if (qos || cpu != -1 || priority != DispatchPriority::LowPriority) throw std::invalid_argument("共享后端不支持每个对象独立 QoS/绑核参数");
    descriptor(msg, topic, domain); impl_->model = msg; impl_->topic = topic; impl_->domain = domain; impl_->queue_size = queue;
    impl_->verbose = verbose; impl_->qos = qos; impl_->cpu = cpu; impl_->priority = priority; impl_->initial_client = runtime();
}
Subscriber::~Subscriber() {
    if (impl_->owner != getpid()) { impl_.release(); return; }
    exit_flag.store(true); std::lock_guard<std::mutex> lock(impl_->operation);
    { std::lock_guard<std::mutex> state(impl_->state); impl_->closing = true; }
    try { impl_->retire(); } catch (...) {}
    std::unique_lock<std::mutex> state(impl_->state); impl_->drained.wait(state, [&] { return !impl_->readers; });
}
void Subscriber::InitChannel(std::string extra) {
    if (impl_->owner != getpid()) throw std::runtime_error("fork 后不能使用旧订阅句柄");
    std::lock_guard<std::mutex> lock(impl_->operation); exit_flag.store(true); impl_->initialize(extra); exit_flag.store(false);
}
void Subscriber::reset_message(const std::shared_ptr<TopicData>& msg) {
    if (impl_->owner != getpid()) throw std::runtime_error("fork 后不能使用旧订阅句柄");
    std::lock_guard<std::mutex> lock(impl_->operation); exit_flag.store(true); impl_->retire();
    descriptor(msg, impl_->topic, impl_->domain); impl_->model = msg;
    if (impl_->initialized) impl_->initialize(""); exit_flag.store(false);
}
void Subscriber::get(Sample& sample) { (void)get(sample, UINT64_MAX); }
bool Subscriber::get(Sample& sample, std::uint64_t timeout) {
    if (impl_->owner != getpid()) return false;
    Sample candidate;
    if (!impl_->read(timeout, [&](auto& reader) { return reader.try_get(candidate); })) return false;
    sample = std::move(candidate); return true;
}
bool Subscriber::try_get(Sample& sample) { return get(sample, 0); }
void Subscriber::get_clone(std::shared_ptr<TopicData>& out) {
    if (impl_->owner != getpid()) return;
    auto candidate = out;
    if (impl_->read(UINT64_MAX, [&](auto& reader) { return reader.try_get_clone(candidate); })) out = std::move(candidate);
}
bool Subscriber::try_get_clone(std::shared_ptr<TopicData>& out) {
    if (impl_->owner != getpid()) return false;
    auto candidate = out;
    if (!impl_->read(0, [&](auto& reader) { return reader.try_get_clone(candidate); })) return false;
    out = std::move(candidate); return true;
}
} // namespace dzIPC::shared_net

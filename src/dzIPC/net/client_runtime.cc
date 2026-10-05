#include "dzIPC/net/client_runtime.h"
#include "byte_codec.h"
#include "dzIPC/net/outbox.h"
#include "local_control_linux.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <fcntl.h>
#include <map>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace dzIPC::net
{
bool best_effort_result(SubmitState a, SubmitState b) noexcept
{
    return a == SubmitState::Committed || a == SubmitState::Indeterminate ||
           b == SubmitState::Committed || b == SubmitState::Indeterminate;
}
bool reliable_result(SubmitState local, const SendResultBody &remote) noexcept
{
    if (local != SubmitState::NotRequired && local != SubmitState::Committed)
        return false;
    if (remote.result == SendResultCode::NoSubscribers)
        return local == SubmitState::Committed;
    return remote.result == SendResultCode::Completed && remote.target_count &&
           remote.acked_count == remote.target_count;
}
struct SendWaitTable::Impl
{
    struct Entry
    {
        Identity publisher;
        std::uint64_t sequence;
        std::promise<SendResultBody> promise;
    };
    mutable std::mutex mutex;
    std::map<std::uint64_t, Entry> entries;
    std::size_t limit;
    bool disconnected = false;
};
SendWaitTable::SendWaitTable(std::size_t limit) : impl_(new Impl)
{
    impl_->limit = limit;
}
SendWaitTable::~SendWaitTable()
{
    disconnect();
}
SendTicket SendWaitTable::insert(std::uint64_t id, Identity publisher, std::uint64_t sequence)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!id || !sequence || !nonzero(publisher) || impl_->disconnected ||
        impl_->entries.size() >= impl_->limit || impl_->entries.count(id))
        throw std::runtime_error("可靠等待表不可接纳请求");
    auto &entry = impl_->entries.try_emplace(id).first->second;
    entry.publisher = publisher;
    entry.sequence = sequence;
    return {id, entry.promise.get_future().share()};
}
bool SendWaitTable::complete(std::uint64_t id, const SendResultBody &result)
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    auto found = impl_->entries.find(id);
    if (found == impl_->entries.end() || found->second.publisher != result.publisher_id ||
        found->second.sequence != result.sequence)
        return false;
    found->second.promise.set_value(result);
    impl_->entries.erase(found);
    return true;
}
SendResultBody SendWaitTable::wait(const SendTicket &ticket, std::uint64_t deadline)
{
    const auto now = local::monotonic_ns();
    if (ticket.result.wait_for(std::chrono::nanoseconds(deadline > now ? deadline - now : 0)) !=
        std::future_status::ready)
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        auto found = impl_->entries.find(ticket.request_id);
        if (found != impl_->entries.end())
        {
            SendResultBody result;
            result.publisher_id = found->second.publisher;
            result.sequence = found->second.sequence;
            result.result = SendResultCode::TimedOut;
            result.possible_remote_delivery = true;
            found->second.promise.set_value(result);
            impl_->entries.erase(found);
        }
    }
    return ticket.result.get();
}
void SendWaitTable::disconnect()
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    impl_->disconnected = true;
    for (auto &entry : impl_->entries)
    {
        SendResultBody result;
        result.publisher_id = entry.second.publisher;
        result.sequence = entry.second.sequence;
        result.result = SendResultCode::GatewayLost;
        result.possible_remote_delivery = true;
        entry.second.promise.set_value(result);
    }
    impl_->entries.clear();
}
SendResultBody SendWaitTable::cancel(const SendTicket& ticket, SendResultCode code)
{
    { std::lock_guard<std::mutex> lock(impl_->mutex);
      auto found = impl_->entries.find(ticket.request_id);
      if (found != impl_->entries.end()) {
          SendResultBody result; result.publisher_id = found->second.publisher; result.sequence = found->second.sequence; result.result = code;
          found->second.promise.set_value(result); impl_->entries.erase(found);
      }
    }
    return ticket.result.get();
}
std::size_t SendWaitTable::size() const
{
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->entries.size();
}

struct ClientRuntime::Impl
{
    struct Waiter
    {
        bool done = false;
        LocalKind expected;
        ControlReply reply;
    };
    struct Outgoing
    {
        Bytes bytes;
        local::Fd descriptor;
    };
    local::Fd socket, wake;
    std::thread thread;
    mutable std::mutex mutex;
    std::mutex stop_mutex;
    std::mutex attach_mutex;
    std::shared_ptr<OutboxSender> outbox;
    std::uint64_t credit_request = 0, credit_retry_after = 0;
    std::array<std::atomic<std::uint64_t>, 4> local_results{}, network_results{};
    std::atomic<std::uint64_t> partial_submit{0}, credit_wait_count{0}, credit_wait_ns{0};
    std::condition_variable changed;
    std::map<std::uint64_t, std::shared_ptr<Waiter>> pending;
    std::deque<Outgoing> outgoing;
    std::size_t outgoing_bytes = 0;
    std::map<Identity, RouteStateBody> routes;
    SendWaitTable sends;
    WelcomeBody welcome;
    CreditCounters granted, released;
    std::uint64_t session = 0, epoch = 0, next_request = 2;
    std::atomic<bool> healthy{true};
    std::string failure;
    std::uint64_t next_id()
    {
        if (next_request == UINT64_MAX)
            throw std::runtime_error("请求序号耗尽");
        return next_request++;
    }
    void enqueue(LocalKind kind, std::uint64_t id, const Bytes &body, int descriptor = -1)
    {
        Bytes packet;
        if (!encode_local({kind, id, session, epoch}, ByteView(body), packet))
            throw std::runtime_error("控制请求编码失败");
        if (outgoing.size() >= 256 || outgoing_bytes + packet.size() > 256 * 1024)
            throw std::runtime_error("控制输出队列已满");
        local::Fd owned;
        if (descriptor >= 0)
        {
            owned.reset(fcntl(descriptor, F_DUPFD_CLOEXEC, 3));
            if (!owned)
                throw std::runtime_error("复制附带 FD 失败");
        }
        outgoing.push_back({std::move(packet), std::move(owned)});
        outgoing_bytes += outgoing.back().bytes.size();
        local::notify(wake.get());
    }
    void fail_locked(const std::string &reason)
    {
        if (!healthy.exchange(false))
            return;
        failure = reason;
        if (outbox)
            outbox->stop();
        outgoing.clear();
        outgoing_bytes = 0;
        sends.disconnect();
        changed.notify_all();
        local::notify(wake.get());
    }
    void run() noexcept
    {
        auto last_received = local::monotonic_ns(), last_ping = last_received;
        try
        {
            while (healthy.load())
            {
                short events = POLLIN;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    if (!outgoing.empty())
                        events |= POLLOUT;
                }
                pollfd fds[] = {{wake.get(), POLLIN, 0}, {socket.get(), events, 0}};
                const auto n = ::poll(fds, 2, 100);
                if (n < 0)
                {
                    if (errno == EINTR)
                        continue;
                    throw std::runtime_error("控制轮询失败");
                }
                if (fds[0].revents)
                    local::drain_event(wake.get());
                std::lock_guard<std::mutex> lock(mutex);
                if (!healthy.load())
                    break;
                if (fds[1].revents & (POLLHUP | POLLERR | POLLNVAL))
                    throw std::runtime_error("网关连接关闭");
                for (unsigned sent = 0; sent < 64 && !outgoing.empty(); ++sent)
                {
                    auto &packet = outgoing.front();
                    if (!local::send(socket.get(), ByteView(packet.bytes), packet.descriptor.get()))
                        break;
                    outgoing_bytes -= packet.bytes.size();
                    outgoing.pop_front();
                }
                for (unsigned received = 0; received < 64; ++received)
                {
                    local::Packet packet;
                    const auto state = local::receive(socket.get(), packet);
                    if (state == local::Receive::WouldBlock)
                        break;
                    if (state != local::Receive::Packet || !packet.descriptors.empty())
                        throw std::runtime_error("无效网关控制包");
                    LocalHeader header;
                    ByteView body;
                    if (!decode_local(ByteView(packet.bytes), header, body) ||
                        header.session_id != session || header.gateway_epoch != epoch)
                        throw std::runtime_error("网关会话代次不匹配");
                    last_received = local::monotonic_ns();
                    if (header.kind == LocalKind::CreditGrant)
                    {
                        CreditCounters value;
                        decode_credit_grant(body, value);
                        if ((value.bytes < granted.bytes && value.records > granted.records) ||
                            (value.bytes > granted.bytes && value.records < granted.records))
                            throw std::runtime_error("累计授予不一致");
                        granted.bytes = std::max(granted.bytes, value.bytes);
                        granted.records = std::max(granted.records, value.records);
                        if (outbox)
                            outbox->grant(value);
                    }
                    else if (header.kind == LocalKind::TxProgress)
                    {
                        CreditCounters value;
                        decode_tx_progress(body, value);
                        if ((value.bytes < released.bytes && value.records > released.records) ||
                            (value.bytes > released.bytes && value.records < released.records))
                            throw std::runtime_error("累计出站进度不一致");
                        released.bytes = std::max(released.bytes, value.bytes);
                        released.records = std::max(released.records, value.records);
                        if (outbox)
                            outbox->progress(value);
                    }
                    else if (header.kind == LocalKind::RouteState)
                    {
                        RouteStateBody value;
                        decode_route_state(body, value);
                        if (routes.size() >= 8192 && !routes.count(value.publisher_id))
                            throw std::runtime_error("路由提示达到上限");
                        auto &old = routes[value.publisher_id];
                        if (value.state_version >= old.state_version)
                            old = value;
                    }
                    else if (header.kind == LocalKind::SendResult)
                    {
                        SendResultBody result;
                        decode_send_result(body, result);
                        sends.complete(header.request_id, result);
                    }
                    if (credit_request && header.request_id == credit_request)
                    {
                        if (header.kind != LocalKind::CreditGrant &&
                            header.kind != LocalKind::Error)
                            throw std::runtime_error("信用补充响应类型错误");
                        credit_request = 0;
                        credit_retry_after = local::monotonic_ns() + 1000000;
                    }
                    const auto found = pending.find(header.request_id);
                    if (found != pending.end())
                    {
                        if (header.kind != LocalKind::Error &&
                            header.kind != found->second->expected)
                            throw std::runtime_error("控制回复类型不匹配");
                        found->second->reply = {header, Bytes(body.data, body.data + body.size)};
                        found->second->done = true;
                        changed.notify_all();
                    }
                }
                const auto now = local::monotonic_ns();
                if (now - last_received > 3000000000ull)
                    throw std::runtime_error("网关健康检查超时");
                if (now - last_ping >= 500000000ull)
                {
                    Bytes body;
                    codec::append(body, now, 8);
                    enqueue(LocalKind::Ping, next_id(), body);
                    last_ping = now;
                }
            }
        }
        catch (const std::exception &e)
        {
            std::lock_guard<std::mutex> lock(mutex);
            fail_locked(e.what());
        }
        ::shutdown(socket.get(), SHUT_RDWR);
    }
};
ClientRuntime::ClientRuntime(const std::string &path) : owner_pid_(getpid()), impl_(new Impl)
{
    const auto deadline = local::monotonic_ns() + 2000000000ull;
    impl_->socket = local::connect_control(path, deadline);
    impl_->wake = local::event();
    const auto id = local::locality();
    Bytes body(id.begin(), id.end());
    codec::append(body, local::process_start(owner_pid_), 8);
    codec::append(body, local::clock_domain(owner_pid_), 8);
    codec::append(body, 1, 4);
    Bytes hello;
    if (!encode_local({LocalKind::Hello, 1, 0, 0}, ByteView(body), hello))
        throw std::runtime_error("HELLO 编码失败");
    while (!local::send(impl_->socket.get(), ByteView(hello)))
        if (!local::ready(impl_->socket.get(), POLLOUT, deadline))
            throw std::runtime_error("HELLO 超时");
    local::Packet packet;
    while (true)
    {
        const auto state = local::receive(impl_->socket.get(), packet);
        if (state == local::Receive::Packet)
            break;
        if (state != local::Receive::WouldBlock ||
            !local::ready(impl_->socket.get(), POLLIN, deadline))
            throw std::runtime_error("WELCOME 失败或超时");
    }
    LocalHeader header;
    ByteView payload;
    if (!packet.descriptors.empty() || !decode_local(ByteView(packet.bytes), header, payload) ||
        header.kind != LocalKind::Welcome || header.request_id != 1 ||
        !decode_welcome(payload, impl_->welcome) || impl_->welcome.locality != id)
        throw std::runtime_error("网关身份或协议不匹配");
    const auto peer = local::credentials(impl_->socket.get());
    if (local::clock_domain(peer.pid) != local::clock_domain(owner_pid_))
        throw std::runtime_error("网关时钟域不匹配");
    impl_->session = header.session_id;
    impl_->epoch = header.gateway_epoch;
    impl_->granted = {impl_->welcome.granted_bytes, impl_->welcome.granted_records};
    impl_->thread = std::thread([p = impl_.get()] { p->run(); });
}
std::shared_ptr<ClientRuntime> ClientRuntime::acquire(const std::string &path)
{
    struct Registry
    {
        const pid_t owner = getpid();
        std::mutex mutex;
        std::condition_variable changed;
        bool starting = false;
        std::string path;
        std::weak_ptr<ClientRuntime> value;
    };
    static Registry *registry = new Registry;
    if (registry->owner != getpid())
        throw std::runtime_error("ForkedProcess: fork 后须 exec 再使用共享网络");
    std::unique_lock<std::mutex> lock(registry->mutex);
    for (;;)
    {
        if (auto existing = registry->value.lock(); existing && existing->healthy())
        {
            if (registry->path != path)
                throw std::runtime_error("每进程只允许连接一个共享网关");
            return existing;
        }
        if (!registry->starting)
            break;
        registry->changed.wait(lock);
    }
    registry->starting = true;
    lock.unlock();
    try
    {
        auto result = std::shared_ptr<ClientRuntime>(new ClientRuntime(path));
        lock.lock();
        registry->starting = false;
        registry->path = path;
        registry->value = result;
        registry->changed.notify_all();
        return result;
    }
    catch (...)
    {
        if (!lock.owns_lock())
            lock.lock();
        registry->starting = false;
        registry->changed.notify_all();
        throw;
    }
}
ClientRuntime::~ClientRuntime()
{
    if (owner_pid_ != getpid())
    {
        impl_->socket.reset();
        impl_->wake.reset();
        impl_.release();
        return;
    }
    stop();
}
void ClientRuntime::check_process() const
{
    if (owner_pid_ != getpid())
        throw std::runtime_error("ForkedProcess: 旧运行时不可跨 fork 使用");
}
bool ClientRuntime::healthy() const noexcept
{
    return owner_pid_ == getpid() && impl_->healthy.load();
}
void ClientRuntime::stop()
{
    check_process();
    std::lock_guard<std::mutex> stopping(impl_->stop_mutex);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->fail_locked("运行时已关闭");
    }
    if (impl_->thread.joinable())
        impl_->thread.join();
}
ControlReply ClientRuntime::request(LocalKind kind, Bytes body, std::chrono::milliseconds timeout,
                                    int descriptor)
{
    LocalKind expected;
    switch (kind)
    {
    case LocalKind::AttachTx:
        expected = LocalKind::TxReady;
        break;
    case LocalKind::RegisterPub:
        expected = LocalKind::PubRegistered;
        break;
    case LocalKind::RegisterSub:
        expected = LocalKind::SubRegistered;
        break;
    case LocalKind::SubReady:
        expected = LocalKind::SubReadyAck;
        break;
    case LocalKind::Unregister:
        expected = LocalKind::Unregistered;
        break;
    case LocalKind::QueryState:
        expected = LocalKind::State;
        break;
    case LocalKind::Ping:
        expected = LocalKind::Pong;
        break;
    case LocalKind::CreditRequest:
        expected = LocalKind::CreditGrant;
        break;
    default:
        throw std::invalid_argument("不支持的控制请求类型");
    }
    check_process();
    std::unique_lock<std::mutex> lock(impl_->mutex);
    if (!healthy() || timeout.count() <= 0 || timeout > std::chrono::seconds(5) ||
        impl_->pending.size() >= 512)
        throw std::runtime_error("控制请求不可用");
    const auto id = impl_->next_id();
    auto waiter = std::make_shared<Impl::Waiter>();
    waiter->expected = expected;
    impl_->pending.emplace(id, waiter);
    try
    {
        impl_->enqueue(kind, id, body, descriptor);
    }
    catch (...)
    {
        impl_->pending.erase(id);
        throw;
    }
    if (!impl_->changed.wait_for(lock, timeout, [&] { return waiter->done || !healthy(); }))
        impl_->fail_locked("控制请求超时");
    impl_->pending.erase(id);
    if (!waiter->done)
        throw std::runtime_error(impl_->failure);
    if (waiter->reply.header.kind == LocalKind::Error)
    {
        const auto &b = waiter->reply.body;
        throw std::runtime_error("网关拒绝请求：" + std::string(b.begin() + 6, b.end()));
    }
    return std::move(waiter->reply);
}
WelcomeBody ClientRuntime::welcome() const
{
    check_process();
    return impl_->welcome;
}
std::uint64_t ClientRuntime::session_id() const
{
    check_process();
    return impl_->session;
}
std::uint64_t ClientRuntime::gateway_epoch() const
{
    check_process();
    return impl_->epoch;
}
CreditCounters ClientRuntime::granted() const
{
    check_process();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->granted;
}
CreditCounters ClientRuntime::released() const
{
    check_process();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    return impl_->released;
}
RouteStateBody ClientRuntime::route_state(const Identity &publisher) const
{
    check_process();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    const auto i = impl_->routes.find(publisher);
    return i == impl_->routes.end() ? RouteStateBody{} : i->second;
}
SendTicket ClientRuntime::prepare_send(Identity publisher, std::uint64_t sequence)
{
    check_process();
    std::lock_guard<std::mutex> lock(impl_->mutex);
    if (!healthy())
        throw std::runtime_error("网关已离线");
    return impl_->sends.insert(impl_->next_id(), publisher, sequence);
}
SendResultBody ClientRuntime::wait_send(const SendTicket &ticket, std::uint64_t deadline)
{
    check_process();
    return impl_->sends.wait(ticket, deadline);
}
SendResultBody ClientRuntime::cancel_send(const SendTicket& ticket, SendResultCode code)
{
    check_process(); return impl_->sends.cancel(ticket, code);
}
void ClientRuntime::record_publish(SubmitState local, SubmitState network) {
    check_process(); ++impl_->local_results[static_cast<unsigned>(local)]; ++impl_->network_results[static_cast<unsigned>(network)];
    if ((local == SubmitState::Committed && network == SubmitState::NotSubmitted) ||
        (network == SubmitState::Committed && local == SubmitState::NotSubmitted)) ++impl_->partial_submit;
}
std::string ClientRuntime::diagnostics_json() const {
    check_process(); std::shared_ptr<OutboxSender> sender;
    { std::lock_guard<std::mutex> lock(impl_->mutex); sender = impl_->outbox; }
    const auto usage = sender ? sender->usage() : OutboxUsage{};
    std::ostringstream s;
    s << "{\"gateway_epoch\":\"" << impl_->epoch << "\",\"session_id\":\"" << impl_->session
      << "\",\"network_healthy\":" << (healthy() ? "true" : "false")
      << ",\"local_committed\":" << impl_->local_results[2].load() << ",\"local_not_submitted\":" << impl_->local_results[1].load()
      << ",\"local_indeterminate\":" << impl_->local_results[3].load() << ",\"network_accepted\":" << impl_->network_results[2].load()
      << ",\"network_not_submitted\":" << impl_->network_results[1].load() << ",\"network_indeterminate\":" << impl_->network_results[3].load()
      << ",\"partial_submit\":" << impl_->partial_submit.load() << ",\"credit_wait_count\":" << impl_->credit_wait_count.load()
      << ",\"credit_wait_ns\":" << impl_->credit_wait_ns.load() << ",\"outbox_used_capacity\":" << usage.used.bytes
      << ",\"outbox_used_records\":" << usage.used.records << ",\"outbox_reserved_capacity\":" << usage.reserved.bytes
      << ",\"outbox_reserved_records\":" << usage.reserved.records << ",\"outbox_released_capacity\":" << usage.released.bytes
      << ",\"outbox_released_records\":" << usage.released.records << '}';
    return s.str();
}
std::string ClientRuntime::route_status(const RouteKey& key, Identity peer) {
    Bytes body{static_cast<std::uint8_t>(nonzero(peer) ? 2 : 1)};
    body.insert(body.end(), key.scope.begin(), key.scope.end()); codec::append(body, key.msg_id, 4);
    if (nonzero(peer)) body.insert(body.end(), peer.begin(), peer.end());
    const auto reply = request(LocalKind::QueryState, std::move(body));
    if (reply.header.kind != LocalKind::State) throw std::runtime_error("路由状态响应类型错误");
    return std::string(reply.body.begin() + 4, reply.body.end());
}
std::string ClientRuntime::status()
{
    const auto reply = request(LocalKind::QueryState, Bytes{0});
    if (reply.header.kind != LocalKind::State)
        throw std::runtime_error("状态响应类型错误");
    return std::string(reply.body.begin() + 4, reply.body.end());
}
} // namespace dzIPC::net

namespace dzIPC::net
{
void ClientRuntime::attach_outbox()
{
    check_process();
    std::lock_guard<std::mutex> starting(impl_->attach_mutex);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!healthy())
            throw std::runtime_error("网关已离线");
        if (impl_->outbox)
            return;
    }
    auto sender = std::make_shared<OutboxSender>(impl_->welcome, impl_->session, impl_->epoch);
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        sender->grant(impl_->granted);
        impl_->outbox = sender;
    }
    try
    {
        request(LocalKind::AttachTx, {}, std::chrono::seconds(2), sender->event_fd());
        sender->ready();
    }
    catch (...)
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        impl_->fail_locked("出站 SHM 初始化失败");
        throw;
    }
}
OutboxSubmit ClientRuntime::submit_outbox(const OutboxHeader &header, ByteView blob)
{
    check_process();
    std::shared_ptr<OutboxSender> sender;
    {
        std::lock_guard<std::mutex> lock(impl_->mutex);
        if (!healthy() || !impl_->outbox)
            return {};
        sender = impl_->outbox;
    }
    for (;;)
    {
        OutboxSubmit result;
        try { result = sender->submit(header, blob); }
        catch (...) { std::lock_guard<std::mutex> lock(impl_->mutex); impl_->fail_locked("出站提交前失败"); throw; }
        if (result.state == SubmitState::Indeterminate)
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            impl_->fail_locked("出站提交状态不确定");
            return result;
        }
        if (result.code != OutboxCode::CreditUnavailable &&
            result.code != OutboxCode::LoanUnavailable)
            return result;
        const auto need = sender->needed(blob.size);
        if (need.bytes || need.records)
        {
            std::lock_guard<std::mutex> lock(impl_->mutex);
            if (healthy() && !impl_->credit_request &&
                local::monotonic_ns() >= impl_->credit_retry_after)
            {
                Bytes body;
                codec::append(body, need.bytes, 4);
                codec::append(body, need.records, 4);
                const auto id = impl_->next_id();
                try
                {
                    impl_->enqueue(LocalKind::CreditRequest, id, body);
                    impl_->credit_request = id;
                }
                catch (...)
                {
                    impl_->fail_locked("信用补充请求无法排队");
                }
            }
        }
        if (header.delivery != Delivery::Reliable) return result;
        const auto wait_start = local::monotonic_ns(); ++impl_->credit_wait_count;
        const auto ready = sender->wait(header.deadline_monotonic_ns);
        impl_->credit_wait_ns += local::monotonic_ns() - wait_start;
        if (!ready) return result;
    }
}
} // namespace dzIPC::net

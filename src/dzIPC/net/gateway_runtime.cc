#include "dzIPC/net/gateway_runtime.h"
#include "byte_codec.h"
#include "dzIPC/net/datagram_endpoint.h"
#include "local_control_linux.h"
#include <atomic>
#include <deque>
#include <map>
#include <mutex>
#include <poll.h>
#include <sstream>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace dzIPC::net
{
namespace
{
std::string json_string(const std::string &value)
{
    std::string out = "\"";
    constexpr char hex[] = "0123456789abcdef";
    for (unsigned char c : value)
    {
        if (c == '"' || c == '\\')
        {
            out += '\\';
            out += c;
        }
        else if (c < 32)
        {
            out += "\\u00";
            out += hex[c >> 4];
            out += hex[c & 15];
        }
        else
            out += c;
    }
    return out + '"';
}
} // namespace
struct GatewayRuntime::Impl
{
    struct Cached
    {
        Bytes request, response;
    };
    struct Session
    {
        local::Fd fd;
        local::Credentials peer;
        std::uint64_t id = 0, last_request = 0, connected_at = 0;
        bool welcomed = false;
        CreditCounters granted;
        std::map<std::uint64_t, Cached> cache;
        std::size_t cache_bytes = 0, output_bytes = 0;
        std::deque<Bytes> output;
    };
    GatewayConfig config;
    Identity identity;
    std::uint64_t epoch = 0, clock = 0, next_session = 1;
    std::unique_ptr<local::Listener> listener;
    local::Fd wake;
    std::vector<std::unique_ptr<DatagramEndpoint>> endpoints;
    std::map<int, Session> sessions;
    std::thread thread;
    std::mutex stop_mutex;
    std::atomic<bool> running{true};
    std::atomic<std::uint64_t> session_count{0}, granted_bytes{0}, granted_records{0}, rejected{0};
    std::size_t control_memory = 0;
    std::string status() const
    {
        std::ostringstream s;
        s << "{\"protocol_version\":1,\"local_protocol_version\":2,\"gateway_id\":"
          << json_string(local::hex(identity)) << ",\"gateway_epoch\":\"" << epoch
          << "\",\"state\":\"" << (running.load() ? "ControlReady" : "Stopped")
          << "\",\"data_plane_ready\":false,\"listen_ip\":" << json_string(config.listen_ip)
          << ",\"interface\":" << json_string(config.interface)
          << ",\"udp_sockets\":" << (running.load() ? config.data_shards + 2 : 0)
          << ",\"client_sessions\":" << session_count.load() << ",\"unused_granted_bytes\":\""
          << granted_bytes.load() << "\",\"unused_granted_records\":\"" << granted_records.load()
          << "\",\"rejected\":\"" << rejected.load() << "\",\"data_ports\":[";
        for (std::uint64_t i = 0; i < config.data_shards; ++i)
        {
            if (i)
                s << ',';
            s << config.data_base_port + i;
        }
        s << "],\"control_port\":" << config.control_port
          << ",\"discovery_port\":" << config.discovery_port
          << ",\"discovery_group\":" << json_string(config.discovery_group) << '}';
        return s.str();
    }
    void queue(Session &session, Bytes packet)
    {
        if (session.output.size() >= 256 ||
            session.output_bytes + session.cache_bytes + packet.size() >
                config.limits.session_control_bytes ||
            control_memory + packet.size() > config.limits.local_control_bytes)
            throw std::runtime_error("控制输出达到配额");
        const auto size = packet.size();
        session.output.push_back(std::move(packet));
        session.output_bytes += size;
        control_memory += size;
    }
    Bytes response(Session &session, LocalKind kind, std::uint64_t request_id, const Bytes &body)
    {
        Bytes result;
        if (!encode_local({kind, request_id, session.id, epoch}, ByteView(body), result))
            throw std::runtime_error("控制响应编码失败");
        return result;
    }
    Bytes error(Session &session, std::uint64_t request_id, std::uint32_t code,
                const std::string &text)
    {
        Bytes body;
        codec::append(body, code, 4);
        codec::append(body, text.size(), 2);
        body.insert(body.end(), text.begin(), text.end());
        ++rejected;
        return response(session, LocalKind::Error, request_id, body);
    }
    void close_session(std::map<int, Session>::iterator i)
    {
        granted_bytes -= i->second.granted.bytes;
        granted_records -= i->second.granted.records;
        control_memory -= i->second.output_bytes + i->second.cache_bytes;
        if (i->second.welcomed)
            --session_count;
        sessions.erase(i);
    }
    void remember(Session &session, std::uint64_t id, const Bytes &request, const Bytes &reply)
    {
        const auto size = request.size() + reply.size();
        while (!session.cache.empty() &&
               (session.cache.size() >= 64 || session.cache_bytes + size > 64 * 1024 ||
                session.cache_bytes + session.output_bytes + size >
                    config.limits.session_control_bytes))
        {
            auto i = session.cache.begin();
            const auto bytes = i->second.request.size() + i->second.response.size();
            session.cache_bytes -= bytes;
            control_memory -= bytes;
            session.cache.erase(i);
        }
        if (size + control_memory > config.limits.local_control_bytes ||
            size + session.output_bytes > config.limits.session_control_bytes)
            throw std::runtime_error("请求历史达到配额");
        session.cache.emplace(id, Cached{request, reply});
        session.cache_bytes += size;
        control_memory += size;
    }
    void process(Session &session, local::Packet packet)
    {
        LocalHeader h;
        ByteView body;
        if (!decode_local(ByteView(packet.bytes), h, body) || !packet.descriptors.empty())
            throw std::runtime_error("无效控制包或未支持的附带 FD");
        if (session.welcomed && h.kind != LocalKind::Hello &&
            (h.session_id != session.id || h.gateway_epoch != epoch))
            throw std::runtime_error("会话代次不匹配");
        if (h.request_id <= session.last_request)
        {
            const auto cached = session.cache.find(h.request_id);
            if (cached == session.cache.end() || cached->second.request != packet.bytes)
                queue(session, error(session, h.request_id, 2, "RequestConflict"));
            else
                queue(session, cached->second.response);
            return;
        }
        Bytes reply;
        if (!session.welcomed)
        {
            if (h.kind != LocalKind::Hello || h.request_id != 1 ||
                !std::equal(identity.begin(), identity.end(), body.data) ||
                codec::get(body.data + 16, 8) != local::process_start(session.peer.pid) ||
                codec::get(body.data + 24, 8) != clock ||
                local::clock_domain(session.peer.pid) != clock)
                throw std::runtime_error("HELLO 身份或时钟域不匹配");
            session.welcomed = true;
            ++session_count;
            session.granted.bytes = std::min(config.limits.initial_send_bytes,
                                             config.limits.send_bytes - granted_bytes.load());
            session.granted.records = std::min(config.limits.initial_send_records,
                                               config.limits.send_records - granted_records.load());
            granted_bytes += session.granted.bytes;
            granted_records += session.granted.records;
            WelcomeBody welcome;
            welcome.locality = identity;
            welcome.max_message_bytes = config.limits.message_bytes;
            welcome.outbox_limit_bytes = config.limits.outbox_bytes;
            welcome.outbox_record_limit = config.limits.outbox_records;
            welcome.granted_bytes = session.granted.bytes;
            welcome.granted_records = session.granted.records;
            welcome.tx_name = outbox_name(identity, epoch, session.id);
            Bytes encoded;
            if (!encode_welcome(welcome, encoded))
                throw std::runtime_error("WELCOME 配额不合法");
            reply = response(session, LocalKind::Welcome, h.request_id, encoded);
        }
        else if (h.kind == LocalKind::Ping)
        {
            reply = response(session, LocalKind::Pong, h.request_id,
                             Bytes(body.data, body.data + body.size));
        }
        else if (h.kind == LocalKind::QueryState)
        {
            const auto text = status();
            Bytes encoded;
            codec::append(encoded, text.size(), 4);
            encoded.insert(encoded.end(), text.begin(), text.end());
            reply = response(session, LocalKind::State, h.request_id, encoded);
        }
        else if (h.kind == LocalKind::CreditRequest)
        {
            const auto bytes = codec::get(body.data, 4), records = codec::get(body.data + 4, 4);
            if (bytes > config.limits.session_send_bytes - session.granted.bytes ||
                records > config.limits.session_send_records - session.granted.records ||
                bytes > config.limits.send_bytes - granted_bytes.load() ||
                records > config.limits.send_records - granted_records.load())
                reply = error(session, h.request_id, 3, "Busy");
            else
            {
                session.granted.bytes += bytes;
                session.granted.records += records;
                granted_bytes += bytes;
                granted_records += records;
                reply = response(session, LocalKind::CreditGrant, h.request_id,
                                 encode_credit_grant(session.granted));
            }
        }
        else
            reply = error(session, h.request_id, 1, "NotImplemented");
        session.last_request = h.request_id;
        remember(session, h.request_id, packet.bytes, reply);
        queue(session, std::move(reply));
    }
    void run() noexcept
    {
        try
        {
            while (running.load())
            {
                std::vector<pollfd> fds{{wake.get(), POLLIN, 0}, {listener->fd(), POLLIN, 0}};
                std::vector<std::uint64_t> generations{0, 0};
                for (const auto &entry : sessions)
                {
                    fds.push_back(
                        {entry.first,
                         static_cast<short>(POLLIN | (entry.second.output.empty() ? 0 : POLLOUT)),
                         0});
                    generations.push_back(entry.second.id);
                }
                if (::poll(fds.data(), fds.size(), 100) < 0)
                {
                    if (errno == EINTR)
                        continue;
                    throw std::runtime_error("网关控制轮询失败");
                }
                if (fds[0].revents)
                    local::drain_event(wake.get());
                if (!running.load())
                    break;
                if (fds[1].revents & POLLIN)
                    for (unsigned accepted = 0; accepted < 16; ++accepted)
                    {
                        local::Fd fd(::accept4(listener->fd(), nullptr, nullptr,
                                               SOCK_NONBLOCK | SOCK_CLOEXEC));
                        if (!fd)
                        {
                            if (errno == EAGAIN || errno == EINTR)
                                break;
                            throw std::runtime_error("接纳控制连接失败");
                        }
                        auto peer = local::credentials(fd.get());
                        if (peer.uid != geteuid() || sessions.size() >= config.limits.sessions ||
                            next_session == UINT64_MAX)
                        {
                            ++rejected;
                            continue;
                        }
                        const int raw = fd.get();
                        Session session;
                        session.fd = std::move(fd);
                        session.peer = peer;
                        session.id = next_session++;
                        session.connected_at = local::monotonic_ns();
                        sessions.emplace(raw, std::move(session));
                    }
                for (std::size_t index = 2; index < fds.size(); ++index)
                {
                    auto found = sessions.find(fds[index].fd);
                    if (found == sessions.end() || found->second.id != generations[index])
                        continue;
                    auto &session = found->second;
                    try
                    {
                        if ((!session.welcomed &&
                             local::monotonic_ns() - session.connected_at > 2000000000ull) ||
                            (fds[index].revents & (POLLHUP | POLLERR | POLLNVAL)))
                            throw std::runtime_error("会话已关闭");
                        if (fds[index].revents & POLLIN)
                            for (unsigned received = 0; received < 16; ++received)
                            {
                                local::Packet packet;
                                const auto state = local::receive(session.fd.get(), packet);
                                if (state == local::Receive::WouldBlock)
                                    break;
                                if (state != local::Receive::Packet)
                                    throw std::runtime_error("控制接收异常");
                                process(session, std::move(packet));
                            }
                        for (unsigned sent = 0; sent < 16 && !session.output.empty(); ++sent)
                        {
                            if (!local::send(session.fd.get(), ByteView(session.output.front())))
                                break;
                            const auto bytes = session.output.front().size();
                            session.output_bytes -= bytes;
                            control_memory -= bytes;
                            session.output.pop_front();
                        }
                    }
                    catch (...)
                    {
                        ++rejected;
                        close_session(found);
                    }
                }
            }
        }
        catch (...)
        {
            ++rejected;
        }
        running.store(false);
        while (!sessions.empty())
            close_session(sessions.begin());
    }
};
GatewayRuntime::GatewayRuntime(GatewayConfig config) : impl_(new Impl)
{
    auto status = validate_host_interface(config);
    if (!status)
        throw ConfigError(status);
    impl_->config = std::move(config);
    impl_->identity = local::locality();
    impl_->epoch = local::random_epoch();
    impl_->clock = local::clock_domain(getpid());
    impl_->listener = std::make_unique<local::Listener>(impl_->config.control_path);
    impl_->endpoints = open_gateway_endpoints(impl_->config);
    impl_->wake = local::event();
    impl_->thread = std::thread([p = impl_.get()] { p->run(); });
}
GatewayRuntime::~GatewayRuntime()
{
    stop();
}
void GatewayRuntime::stop()
{
    std::lock_guard<std::mutex> lock(impl_->stop_mutex);
    impl_->running.store(false);
    local::notify(impl_->wake.get());
    if (impl_->thread.joinable())
        impl_->thread.join();
    impl_->endpoints.clear();
    impl_->listener.reset();
}
bool GatewayRuntime::running() const noexcept
{
    return impl_->running.load();
}
std::string GatewayRuntime::status_json() const
{
    return impl_->status();
}
} // namespace dzIPC::net

#pragma once
#include "../../src/dzIPC/net/byte_codec.h"
#include "../../src/dzIPC/net/local_control_linux.h"
#include "dzIPC/net/client_runtime.h"
#include "dzIPC/net/datagram_endpoint.h"
#include "dzIPC/net/gateway_runtime.h"
#include <filesystem>
#include <poll.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

namespace shared_net_test
{
using namespace dzIPC::net;
using namespace std::chrono_literals;
struct Directory
{
    std::string path;
    Directory()
    {
        char pattern[] = "/tmp/dzipc-net-test-XXXXXX";
        const auto result = mkdtemp(pattern);
        if (!result)
            throw std::runtime_error("测试目录创建失败");
        path = result;
    }
    ~Directory()
    {
        std::error_code ec;
        std::filesystem::remove_all(path, ec);
    }
    std::string control() const
    {
        return path + "/control.sock";
    }
};
inline GatewayConfig configuration(const Directory &dir)
{
    GatewayConfig c;
    c.listen_ip = "127.0.0.1";
    c.interface = "lo";
    c.control_path = dir.control();
    for (unsigned attempt = 0; attempt < 100; ++attempt)
    {
        c.data_base_port = 20000 + local::random_epoch() % 12000;
        c.control_port = c.data_base_port + 4;
        c.discovery_port = c.data_base_port + 5;
        try
        {
            auto reserved = open_gateway_endpoints(c);
            return c;
        }
        catch (const std::system_error &e)
        {
            if (e.code().value() != EADDRINUSE)
                throw;
        }
    }
    throw std::runtime_error("测试无法找到空闲 UDP 端口组");
}
template <class F> bool until(F predicate, std::chrono::milliseconds timeout = 2s)
{
    const auto end = std::chrono::steady_clock::now() + timeout;
    do
    {
        if (predicate())
            return true;
        std::this_thread::sleep_for(2ms);
    } while (std::chrono::steady_clock::now() < end);
    return predicate();
}
inline void transmit(int fd, const Bytes &packet)
{
    const auto end = local::monotonic_ns() + 2000000000ull;
    while (!local::send(fd, ByteView(packet)))
        if (!local::ready(fd, POLLOUT, end))
            throw std::runtime_error("测试发送超时");
}
inline local::Packet receive(int fd)
{
    const auto end = local::monotonic_ns() + 2000000000ull;
    for (;;)
    {
        local::Packet p;
        const auto state = local::receive(fd, p);
        if (state == local::Receive::Packet)
            return p;
        if (state != local::Receive::WouldBlock || !local::ready(fd, POLLIN, end))
            throw std::runtime_error("测试接收失败");
    }
}
inline Bytes encode(LocalKind kind, std::uint64_t id, std::uint64_t session, std::uint64_t epoch,
                    const Bytes &body)
{
    Bytes p;
    if (!encode_local({kind, id, session, epoch}, ByteView(body), p))
        throw std::runtime_error("测试编码失败");
    return p;
}
inline Bytes hello(Identity id = local::locality(),
                   std::uint64_t clock = local::clock_domain(getpid()))
{
    Bytes b(id.begin(), id.end());
    codec::append(b, local::process_start(getpid()), 8);
    codec::append(b, clock, 8);
    codec::append(b, 1, 4);
    return encode(LocalKind::Hello, 1, 0, 0, b);
}
struct RawClient
{
    local::Fd fd;
    LocalHeader header;
    WelcomeBody welcome;
    explicit RawClient(const std::string &path)
        : fd(local::connect_control(path, local::monotonic_ns() + 2000000000ull))
    {
        transmit(fd.get(), hello());
        auto p = receive(fd.get());
        ByteView body;
        if (!decode_local(ByteView(p.bytes), header, body) || !decode_welcome(body, welcome))
            throw std::runtime_error("测试握手失败");
    }
    Bytes request(LocalKind kind, std::uint64_t id, const Bytes &body) const
    {
        return encode(kind, id, header.session_id, header.gateway_epoch, body);
    }
};
inline std::size_t fd_count()
{
    return std::distance(std::filesystem::directory_iterator("/proc/self/fd"),
                         std::filesystem::directory_iterator{});
}
} // namespace shared_net_test

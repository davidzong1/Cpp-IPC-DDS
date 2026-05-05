
#include <winsock2.h>
#include <ws2tcpip.h>
#include <chrono>
#include <cstring>
#include "libipc/buffer.h"

namespace ipc {
namespace detail {
namespace socket {
class UDPNode
{
    char name[256]{};
    char ip[16]{};
    uint16_t port{};
    SOCKET server_fd{INVALID_SOCKET};
    std::vector<char> temp_buffer;

    static bool ensure_wsa()
    {
        static bool initialized = false;
        if (initialized)
        {
            return true;
        }
        WSADATA wsa{};
        const int err = ::WSAStartup(MAKEWORD(2, 2), &wsa);
        if (err == 0)
        {
            initialized = true;
        }
        return initialized;
    }

public:
    UDPNode() {}

    /* Instantiation */
    UDPNode(const char* name, const char* ip, uint16_t port) { create(name, ip, port); }

    void create(const char* name, const char* ip, uint16_t port)
    {
        if (server_fd != INVALID_SOCKET)
        {
            close();
        }

        if (name)
        {
            std::strncpy(this->name, name, sizeof(this->name) - 1);
            this->name[sizeof(this->name) - 1] = '\0';
        }
        else
        {
            this->name[0] = '\0';
        }

        if (ip)
        {
            std::strncpy(this->ip, ip, sizeof(this->ip) - 1);
            this->ip[sizeof(this->ip) - 1] = '\0';
        }
        else
        {
            this->ip[0] = '\0';
        }

        this->port = port;
        this->server_fd = INVALID_SOCKET;
        temp_buffer.resize(1'472);   // 预分配最大UDP报文长度
    }

    bool connect()
    {
        if (!ensure_wsa())
        {
            return false;
        }

        if (server_fd != INVALID_SOCKET)
        {
            close();
        }

        server_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (server_fd == INVALID_SOCKET)
        {
            return false;
        }

        BOOL reuse = TRUE;
        int nRecvBuf = 1'024 * 1'024;   // 1MB
        ::setsockopt(server_fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&nRecvBuf), sizeof(nRecvBuf));
        ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<char*>(&reuse), sizeof(reuse));
#ifdef SO_REUSEPORT
        ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, reinterpret_cast<char*>(&reuse), sizeof(reuse));
#endif

        sockaddr_in local_addr{};
        local_addr.sin_family = AF_INET;
        local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        local_addr.sin_port = htons(port);

        if (::bind(server_fd, reinterpret_cast<sockaddr*>(&local_addr), sizeof(local_addr)) == SOCKET_ERROR)
        {
            ::closesocket(server_fd);
            server_fd = INVALID_SOCKET;
            return false;
        }

        ip_mreq mreq{};
        if (::InetPtonA(AF_INET, ip, &mreq.imr_multiaddr) != 1)
        {
            ::closesocket(server_fd);
            server_fd = INVALID_SOCKET;
            return false;
        }
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);

        if (::setsockopt(server_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, reinterpret_cast<char*>(&mreq), sizeof(mreq))
            == SOCKET_ERROR)
        {
            ::closesocket(server_fd);
            server_fd = INVALID_SOCKET;
            return false;
        }

        unsigned char ttl = 1;
        ::setsockopt(server_fd, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<char*>(&ttl), sizeof(ttl));
        unsigned char loop = 1;
        ::setsockopt(server_fd, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<char*>(&loop), sizeof(loop));
        return true;
    }

    bool send(ipc::buffer& data)
    {
        if (server_fd == INVALID_SOCKET || !data)
        {
            return false;
        }

        void* payload = data.data();
        if (!payload)
        {
            return false;
        }

        size_t payload_size = data.size();

        sockaddr_in dst_addr{};
        dst_addr.sin_family = AF_INET;
        dst_addr.sin_port = htons(port);
        if (::InetPtonA(AF_INET, ip, &dst_addr.sin_addr) != 1)
        {
            return false;
        }

        int sent = ::sendto(server_fd, static_cast<const char*>(payload), static_cast<int>(payload_size), 0,
                            reinterpret_cast<sockaddr*>(&dst_addr), sizeof(dst_addr));
        return sent == static_cast<int>(payload_size);
    }

    ipc::buffer receive(uint64_t tm)
    {
        if (server_fd == INVALID_SOCKET)
            return ipc::buffer();

        // 1. 无限等待模式
        if (tm == ipc::invalid_value)
        {
            int err_cnt = 0;
            for (;;)
            {
                int received = ::recvfrom(server_fd, temp_buffer.data(), temp_buffer.size(), 0, nullptr, nullptr);
                if (received >= 0)
                {
                    ;
                    return ipc::buffer(temp_buffer.data(), received);
                }
            }

            // 2. 超时等待模式
            auto start_time = std::chrono::steady_clock::now();
            uint64_t remaining_ms = tm;
            int err_cnt = 0;

            while (remaining_ms > 0)
            {
                fd_set read_fds;
                FD_ZERO(&read_fds);
                FD_SET(server_fd, &read_fds);

                timeval timeout;
                timeout.tv_sec = static_cast<long>(remaining_ms / 1'000);
                timeout.tv_usec = static_cast<long>((remaining_ms % 1'000) * 1'000);

                int ret = ::select(0, &read_fds, nullptr, nullptr, &timeout);   // Windows下select第一个参数被忽略

                if (ret > 0)
                {
                    int received = ::recvfrom(server_fd, temp_buffer.data(), temp_buffer.size(), 0, nullptr, nullptr);
                    if (received >= 0)
                    {
                        return ipc::buffer(temp_buffer.data(), received);
                    }

                    int err = ::WSAGetLastError();
                    // info too large for buffer
                    if (err == WSAEMSGSIZE)
                        return ipc::buffer();
                    // 异常中断或资源暂时不可用
                    if ((err == WSAEINTR || err == WSAEWOULDBLOCK) && ++err_cnt < 10)
                    {
                        goto refresh_time;
                    }
                    return ipc::buffer();
                }
                else if (ret == 0)
                {   // Select timeout
                    return ipc::buffer();
                }
                else
                {   // Select error
                    if (::WSAGetLastError() == WSAEINTR && ++err_cnt < 10)
                    {
                        goto refresh_time;
                    }
                    return ipc::buffer();
                }

            refresh_time:
                auto now = std::chrono::steady_clock::now();
                auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - start_time).count();
                if (static_cast<uint64_t>(elapsed) >= tm)
                    return ipc::buffer();
                remaining_ms = tm - static_cast<uint64_t>(elapsed);
            }

            return ipc::buffer();
        }

        bool close()
        {
            if (server_fd == INVALID_SOCKET)
            {
                return true;
            }

            ip_mreq mreq{};
            if (::InetPtonA(AF_INET, ip, &mreq.imr_multiaddr) == 1)
            {
                mreq.imr_interface.s_addr = htonl(INADDR_ANY);
                ::setsockopt(server_fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, reinterpret_cast<char*>(&mreq), sizeof(mreq));
            }

            ::closesocket(server_fd);
            server_fd = INVALID_SOCKET;
            return true;
        }
    };

    void clear_cache()
    {
        // 循环读取直到缓冲区空
        char discard_buf[4'096];
        while (::recvfrom(server_fd, discard_buf.data(), discard_buf.size(), 0, nullptr, nullptr) > 0)
            ;
    }
}   // namespace socket
}   // namespace detail
}   // namespace ipc
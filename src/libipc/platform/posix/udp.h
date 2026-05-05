
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include "libipc/buffer.h"

namespace ipc {
namespace detail {
namespace socket {

class UDPNode
{
    char name[256]{};
    char ip[16]{};
    uint16_t port{};
    int server_fd{-1};
    ipc::buffer temp_buffer;
    char rev_fail_fail{};

public:
    UDPNode() {}

    /* Instantiation */
    UDPNode(const char* name, const char* ip, uint16_t port) { create(name, ip, port); }

    void create(const char* name, const char* ip, uint16_t port)
    {
        strncpy(this->name, name, sizeof(this->name) - 1);
        strncpy(this->ip, ip, sizeof(this->ip) - 1);
        this->port = port;
        uint8_t* ptr = new uint8_t[1'472];
        temp_buffer = ipc::buffer(ptr, 1'472, [](void* p, std::size_t s)
                                  { delete[] static_cast<uint8_t*>(p); });   // 预分配最大UDP报文长度
    }

    bool connect()
    {
        if (server_fd >= 0)
        {
            close();
        }

        server_fd = ::socket(AF_INET, SOCK_DGRAM, 0);
        if (server_fd < 0)
        {
            return false;
        }

        int reuse = 1;
        int nRecvBuf = 1'024 * 1'024;   // 1MB
        int nSendBuf = 1'024 * 1'024;   // 1MB
        ::setsockopt(server_fd, SOL_SOCKET, SO_RCVBUF, &nRecvBuf, sizeof(nRecvBuf));
        ::setsockopt(server_fd, SOL_SOCKET, SO_SNDBUF, &nSendBuf, sizeof(nSendBuf));
        ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#ifdef SO_REUSEPORT
        ::setsockopt(server_fd, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
#endif

        sockaddr_in local_addr{};
        local_addr.sin_family = AF_INET;
        local_addr.sin_addr.s_addr = htonl(INADDR_ANY);
        local_addr.sin_port = htons(port);

        if (::bind(server_fd, reinterpret_cast<sockaddr*>(&local_addr), sizeof(local_addr)) < 0)
        {
            ::close(server_fd);
            server_fd = -1;
            return false;
        }

        ip_mreq mreq{};
        if (::inet_pton(AF_INET, ip, &mreq.imr_multiaddr) != 1)
        {
            ::close(server_fd);
            server_fd = -1;
            return false;
        }
        mreq.imr_interface.s_addr = htonl(INADDR_ANY);

        if (::setsockopt(server_fd, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
        {
            ::close(server_fd);
            server_fd = -1;
            return false;
        }

        unsigned char ttl = 1;
        ::setsockopt(server_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        unsigned char loop = 1;
        ::setsockopt(server_fd, IPPROTO_IP, IP_MULTICAST_LOOP, &loop, sizeof(loop));
        return true;
    }

    bool send(ipc::buffer& data)
    {
        if (server_fd < 0 || data.size() == 0)
        {
            return false;
        }

        if (!data.data())
        {
            return false;
        }

        sockaddr_in dst_addr{};
        dst_addr.sin_family = AF_INET;
        dst_addr.sin_port = htons(port);
        if (::inet_pton(AF_INET, ip, &dst_addr.sin_addr) != 1)
        {
            return false;
        }
        if (data.size() > 1'472)   // UDP最大有效载荷限制
        {
            return false;
        }
        else
        {
            ssize_t sent = ::sendto(server_fd, data.data(), data.size(), 0, reinterpret_cast<sockaddr*>(&dst_addr),
                                    sizeof(dst_addr));
            if (sent < 0)
                return false;
        }
        return true;
    }

    ipc::buffer receive(uint64_t tm)
    {
        if (server_fd < 0)
            return ipc::buffer();

        int err_cnt = 0;
        if (tm == ipc::invalid_value)
        {
            for (;;)
            {
                ssize_t received = ::recvfrom(server_fd, temp_buffer.data(), temp_buffer.size(), 0, nullptr, nullptr);
                if (received >= 0)
                {
                    return ipc::buffer(temp_buffer.data(), received);
                }
                if (errno == EINTR)
                {
                    err_cnt++;
                    if (err_cnt >= 100)
                    {
                        return ipc::buffer();   // 避免无限重试
                    }
                    continue;
                }
                return ipc::buffer();
            }
        }

        // 2. 处理定时等待逻辑
        auto start_time = std::chrono::steady_clock::now();
        uint64_t remaining_ms = tm;

        while (remaining_ms > 0)
        {
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(server_fd, &read_fds);

            timeval timeout;
            timeout.tv_sec = static_cast<long>(remaining_ms / 1'000);
            timeout.tv_usec = static_cast<long>((remaining_ms % 1'000) * 1'000);

            int ret = ::select(server_fd + 1, &read_fds, nullptr, nullptr, &timeout);

            if (ret > 0)
            {
                ssize_t received = ::recvfrom(server_fd, temp_buffer.data(), temp_buffer.size(), MSG_DONTWAIT, nullptr,
                                              nullptr);
                if (received >= 0)
                {
                    return ipc::buffer(temp_buffer.data(), received, nullptr);
                }
                if ((errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK) && ++err_cnt < 10)
                {
                    goto refresh_time;
                }
                return ipc::buffer();
            }
            else if (ret == 0)
            {
                return ipc::buffer();   // 真正超时
            }
            else
            {
                if (errno == EINTR && ++err_cnt < 5)
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
        if (server_fd < 0)
        {
            return true;
        }

        ip_mreq mreq{};
        if (::inet_pton(AF_INET, ip, &mreq.imr_multiaddr) == 1)
        {
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);
            ::setsockopt(server_fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
        }

        ::close(server_fd);
        server_fd = -1;
        return true;
    }

    void clear_cache()
    {
        // 循环读取直到缓冲区空
        char discard_buf[4'096];
        while (::recvfrom(server_fd, discard_buf, sizeof(discard_buf), MSG_DONTWAIT, nullptr, nullptr) > 0)
            ;
    }
};
}   // namespace socket
}   // namespace detail
}   // namespace ipc
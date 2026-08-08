
#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>
#include <chrono>
#include <cstdio>
#include "libipc/buffer.h"
#include "libipc/udp.h"

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
    ipc::socket::NodeRole role{ipc::socket::NodeRole::SendRecv};

    /* 只有入了组的 socket 才会收到组播流量; 只发不收的节点跳过入组,
     * 从而天然屏蔽掉自己发出去又被内核回绕回来的分片。 */
    bool joins_group() const { return role != ipc::socket::NodeRole::SendOnly; }

public:
    UDPNode() {}

    /* Instantiation */
    UDPNode(const char* name, const char* ip, uint16_t port) { create(name, ip, port); }

    UDPNode(const char* name, const char* ip, uint16_t port, ipc::socket::NodeRole role)
    {
        create(name, ip, port, role);
    }

    void create(const char* name, const char* ip, uint16_t port)
    {
        create(name, ip, port, ipc::socket::NodeRole::SendRecv);
    }

    void create(const char* name, const char* ip, uint16_t port, ipc::socket::NodeRole role)
    {
        strncpy(this->name, name, sizeof(this->name) - 1);
        strncpy(this->ip, ip, sizeof(this->ip) - 1);
        this->port = port;
        this->role = role;
        uint8_t* ptr = new uint8_t[1'472];
        temp_buffer = ipc::buffer(ptr, 1'472, [](void* p, std::size_t s)
                                  { delete[] static_cast<uint8_t*>(p); });   // 预分配最大UDP报文长度
    }

    ipc::socket::NodeRole node_role() const noexcept { return role; }

    /* 检查内核是否把请求的 socket 缓冲截断了, 截断则打一次警告。
     *
     * setsockopt(SO_RCVBUF) 不会因为超过 net.core.rmem_max 而失败 —— 它静默地
     * 把值压到上限。Linux 默认 rmem_max = 212992 (208 KB), 于是这里请求的 1 MB
     * 实际只拿到 208 KB, 而调用方无从知道。
     *
     * 后果是大包收不全: 1 MB 消息 = 713 个 1472 B 分片, 发送端几百微秒就灌完,
     * 208 KB 缓冲只装得下约 141 片, 其余全被内核丢弃。表现为"消息级丢包率很高
     * 但链路明明没问题" —— 实测 1 MB 丢包 66%, 而同链路 64 KB 能跑 112 MB/s。
     *
     * 所有靠 UDP 传大包的中间件都要求调这个参数(Cyclone DDS 有
     * MinimumSocketReceiveBufferSize, ROS 2 / Autoware 的部署文档第一条就是
     * 把 rmem_max 调大), 区别只在于它们会告诉你, 而不是静默降级。
     *
     * 内核返回的值是请求值的两倍(用于记账开销), 所以按 actual/2 比较。 */
    static void warn_if_buffer_truncated(int fd, int want_recv, int want_send)
    {
        /* 每个进程只警告一次: connect() 会被每个 topic 的每条通道调用,
         * 逐次打印会淹没真正的日志。 */
        static bool warned = false;
        if (warned)
        {
            return;
        }

        auto effective = [fd](int optname) -> int {
            int actual = 0;
            socklen_t len = sizeof(actual);
            if (::getsockopt(fd, SOL_SOCKET, optname, &actual, &len) < 0)
            {
                return -1;   // 读不回来就不判断, 不要凭猜测报警
            }
            return actual / 2;
        };

        const int recv_got = effective(SO_RCVBUF);
        const int send_got = effective(SO_SNDBUF);
        const bool recv_short = recv_got >= 0 && recv_got < want_recv;
        const bool send_short = send_got >= 0 && send_got < want_send;
        if (!recv_short && !send_short)
        {
            return;
        }

        warned = true;
        std::fprintf(stderr,
                     "\033[33m[dzIPC][warn] UDP socket buffer truncated by the kernel:\n");
        if (recv_short)
        {
            std::fprintf(stderr, "  SO_RCVBUF: requested %d B, got %d B (net.core.rmem_max)\n", want_recv,
                         recv_got);
        }
        if (send_short)
        {
            std::fprintf(stderr, "  SO_SNDBUF: requested %d B, got %d B (net.core.wmem_max)\n", want_send,
                         send_got);
        }
        std::fprintf(stderr,
                     "  Large multi-fragment messages will lose fragments (a 1 MB payload is 713\n"
                     "  fragments of 1472 B; a 208 KB buffer holds only ~141 of them).\n"
                     "  Fix by raising the kernel limits, e.g.:\n"
                     "    sudo sysctl -w net.core.rmem_max=67108864\n"
                     "    sudo sysctl -w net.core.wmem_max=67108864\n"
                     "  Persist in /etc/sysctl.conf (or /etc/sysctl.d/) to survive reboot.\033[0m\n");
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
        /* 只发不收的节点不需要接收缓冲。每个 topic 现在有两条 socket, 无差别地各
         * 要 1 MB 收 + 1 MB 发会让百 topic 进程的内核内存翻倍。 */
        if (joins_group())
        {
            ::setsockopt(server_fd, SOL_SOCKET, SO_RCVBUF, &nRecvBuf, sizeof(nRecvBuf));
        }
        ::setsockopt(server_fd, SOL_SOCKET, SO_SNDBUF, &nSendBuf, sizeof(nSendBuf));
        if (joins_group())
        {
            warn_if_buffer_truncated(server_fd, nRecvBuf, nSendBuf);
        }

        /* SendOnly 不 bind: 它从不接收, 占着固定端口只会和同机同 topic 的其他
         * 进程抢端口。不 bind 时内核分配临时源端口, 目的地址仍是 group:port,
         * 对端完全无感。 */
        if (joins_group())
        {
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
        }

        /* 只发不收的节点不入组: 组播的接收资格来自 IP_ADD_MEMBERSHIP, 不入组就
         * 收不到任何东西 —— 包括自己刚发出、被内核回绕回来的分片。这是端点分离
         * 的实现手段。发送不需要组成员资格, 所以 send() 照常可用。 */
        if (joins_group())
        {
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
        }
        else
        {
            /* 不入组也要校验地址合法, 否则错误的组地址要等到第一次 send 才暴露。 */
            in_addr probe{};
            if (::inet_pton(AF_INET, ip, &probe) != 1)
            {
                ::close(server_fd);
                server_fd = -1;
                return false;
            }
        }

        unsigned char ttl = 1;
        ::setsockopt(server_fd, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
        /* 必须保持 1。置 0 会让本机所有进程都收不到我们发的组播(它是主机级开关,
         * 不是"只屏蔽自己"), 同机 IPC 会整体失效。屏蔽自己靠上面的不入组实现。 */
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

    ipc::buffer receive_nowait()
    {
        /* SendOnly 没有入组, 永远收不到东西。直接返回, 免得调用方以为"暂时没数据"
         * 而反复轮询。 */
        if (server_fd < 0 || !joins_group())
            return ipc::buffer();

        ssize_t received = ::recvfrom(server_fd, temp_buffer.data(), temp_buffer.size(), MSG_DONTWAIT, nullptr, nullptr);
        if (received >= 0)
        {
            return ipc::buffer(temp_buffer.data(), received, nullptr);
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
        {
            return ipc::buffer();   // 没有数据可读
        }
        return ipc::buffer();
    }

    ipc::buffer receive(uint64_t tm)
    {
        /* 同 receive_nowait: SendOnly 不入组, 等下去只会白白阻塞 tm 毫秒。
         * 这一条很关键 —— chunk_send_ex 的 ACK 等待循环若跑在 SendOnly 上,
         * 没有这个短路就会把 5 轮预算全耗在 select() 上。 */
        if (server_fd < 0 || !joins_group())
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

        if (joins_group())
        {
            ip_mreq mreq{};
            if (::inet_pton(AF_INET, ip, &mreq.imr_multiaddr) == 1)
            {
                mreq.imr_interface.s_addr = htonl(INADDR_ANY);
                ::setsockopt(server_fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
            }
        }

        ::close(server_fd);
        server_fd = -1;
        return true;
    }

    void clear_cache()
    {
        if (server_fd < 0)
        {
            return;
        }
        // 循环读取直到缓冲区空
        char discard_buf[4'096];
        while (::recvfrom(server_fd, discard_buf, sizeof(discard_buf), MSG_DONTWAIT, nullptr, nullptr) > 0)
            ;
    }
};
}   // namespace socket
}   // namespace detail
}   // namespace ipc

#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <poll.h>
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
    /* 阶段 5: cancel_wait() 之后置位 —— 接收面失效, waitable()/wait_handle()
     * 变中性值(false / 0)。create()/connect() 复位。 */
    bool cancelled_{false};

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
        /* 阶段 5: 重建节点复位 cancel 状态(冻结不变量: cancel 之后 waitable()==false /
         * wait_handle()==0, 只有 close()+connect() 能恢复)。 */
        cancelled_ = false;
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
        /* 阶段 5: close()+connect() 复位 cancel 状态(冻结不变量: cancel 之后
         * waitable()==false / wait_handle()==0, 只有重新连接才能恢复可等待)。 */
        cancelled_ = false;

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

            /* 绑**组播组地址**而不是 INADDR_ANY。
             *
             * 绑 INADDR_ANY 时, 同机所有 topic 只要落到同一端口就会互相收包 —— 实测
             * (docs/shm_defect_fixes.md 第 3 条): 默认 domain_id=0 下端口公式退化成常数
             * 11451, 于是两个不同 topic 的组播组虽不同, 却各自收到了对方的**全部**数据;
             * 若两者 msg_id 又都是默认 0, check_id 会放行, 直接按错误类型反序列化。
             *
             * 绑到组地址后由内核按组过滤, 同端口不同组不再串。寻址方案(端口/组公式)不变,
             * 所以不影响互操作。 */
            sockaddr_in local_addr{};
            local_addr.sin_family = AF_INET;
            local_addr.sin_port = htons(port);
            if (::inet_pton(AF_INET, ip, &local_addr.sin_addr) != 1)
            {
                ::close(server_fd);
                server_fd = -1;
                return false;
            }

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

        /* 阶段 5: cancel_wait() 之后接收面失效 —— shutdown(fd, SHUT_RD) 已排空接收,
         * 这里再显式短路, 保证"cancel 之后一律空 buffer"这条冻结语义不依赖内核
         * 对已 shutdown 的 UDP socket 的具体返回形态。 */
        if (cancelled_)
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

        /* 阶段 5: 同 receive_nowait —— cancel 之后一律返回空, 且**不得**在已
         * shutdown 的 fd 上跑无限等待分支(会立刻 EAGAIN 或紧循环)。 */
        if (cancelled_)
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

    /* ================= 阶段 5: 可等待句柄 (平台实现) =================
     * 被 ipc::socket::UDPNode 的同名方法(*纯转发*, src/libipc/socket/udp.cpp)调用,
     * 语义冻结于契约 §1 与勘误 E1(勘误优先于正文)。 */

    /* 该节点能否被多路等待: 已连接 + 入组 + 未被 cancel。
     * SendOnly 不入组 ⇒ 永远收不到东西, 不构成可等待通道。 */
    bool waitable() const noexcept
    {
        return server_fd >= 0 && joins_group() && !cancelled_;
    }

    /* 可等待句柄 = 接收 fd。
     *
     * ⛔ 勘误 E1: **不改变** fd 的阻塞模式 —— 这里没有 fcntl(O_NONBLOCK), 返回的
     * 就是那个**阻塞**的接收 fd。
     *
     * 依据(实现侧): epoll 是 level-triggered, 单消费者下 epoll_wait 报可读 ⇒ 数据
     * 一定还在, 读的时候用 MSG_DONTWAIT(receive_nowait)即可, 不需要把 fd 本身置成
     * 非阻塞; 而置了会**破坏既有语义** —— receive(invalid_value) 的无限等待分支用的
     * 是不带 MSG_DONTWAIT 的阻塞 recvfrom, 非阻塞 fd 上它会立刻返回 EAGAIN, 调用方
     * (如 data_rev.cc 的 ACK 等待)就从"阻塞等到数据"退化成紧循环忙轮询。
     *
     * 返回类型: fd 是 int, 转 std::uintptr_t 是无损的(非负小整数); 0 = 不可等待
     * (fd 0 是合法 stdin, 但 UDPNode 的接收 fd 一定是 socket(), 不会是 0 —— 即便
     * 理论上的极端情形, 也是"不可等待"这一侧的安全误判, 消费方回退兼容线程)。 */
    std::uintptr_t wait_handle() const noexcept
    {
        if (!waitable())
        {
            return std::uintptr_t{0};
        }
        return static_cast<std::uintptr_t>(server_fd);
    }

    /* 取消阻塞在 wait_handle 上的等待, 并让本节点的接收面失效。
     *
     * shutdown(fd, SHUT_RD) 是 Linux 上唯一能**同时**做到两件事的手段:
     *   · 让阻塞在 epoll_wait 上的等待方立刻返回(该 fd 变成 EPOLLIN|EPOLLRDHUP
     *     就绪 —— 这正是 test_socket_wait_set.cpp 的 CancelWaitWakesBlockedWait
     *     用例守的东西; 实测: 对 fd 只做了一个 epoll_ctl(DEL) 是**不会**唤醒
     *     epoll_wait 的, 那会一直睡到超时, 所以唤醒必须走 shutdown 或独立通道);
     *   · 让此后所有 recvfrom 立刻返回 0 ⇒ receive()/receive_nowait() 返回空。
     * 幂等: 重复调用对已 shutdown 的 fd 是 no-op(errno 而已, 不检查不报错);
     * cancelled_ 置位后 waitable()/wait_handle() 立即变中性值。
     *
     * ⛔ 不复用该 node 接收(契约 §1): 要恢复就 close() + connect()。 */
    void cancel_wait() noexcept
    {
        if (cancelled_)
        {
            return;   // 幂等: 已取消的直接返回, 不再打内核
        }
        cancelled_ = true;
        if (server_fd >= 0)
        {
            /* 返回值刻意不检查: SHUT_RD 对 UDP socket 已经"关了读"时可能返回
             * ENOTCONN, 而无论哪种情形, 读面都已经失效 —— 这正是我们要的结果。 */
            ::shutdown(server_fd, SHUT_RD);
        }
    }

    /* 清除 wait_handle 上的就绪提示。
     * Linux: **no-op** —— epoll 本身是 level-triggered, 直接重查 fd 的可读状态即可,
     * 没有 Windows 那种需要 WSAEnumNetworkEvents 回收的按事件计数。 */
    void clear_wait() noexcept {}

    /* ---------------- 阶段 5 追加(t12 裁定 C): 非阻塞可读判据 ----------------
     *
     * poll(fd, POLLIN, 0) > 0 —— 0 超时 ⇒ 不阻塞、无分配、O(1), 且**不做读操作**
     * (不消费数据、不动 temp_buffer), 因此可以任意次重复调用。
     *
     * 为什么必须是 poll 而不是"试着 recvfrom 一次": 后者会真的把数据取走(消费),
     * 而调用方(收包 worker 的 recv_once)在"有数据"时要走完整的 chunk_rev_* 路径
     * (含分片重组 + ACK), 不能被这里的探测吃掉一片。
     *
     * 为什么需要它: socket 侧唯一收包原语 chunk_rev_topic/chunk_rev_server 一律带 tm,
     * 空闲时会阻塞到 tm(50ms / 200ms)。worker 的预算循环是
     *   for(;;){ n = recv_once(); if (n == 0) break; ... }
     * 没有这个判据时, 收尾那一次 recv_once() 会让**共享** worker 线程空读阻塞最长 tm。
     *
     * 语义边界: server_fd 无效 / 未入组(SendOnly) / 已 cancel 一律 false —— 与
     * waitable() 同口径。cancelled_ 之后 fd 已被 shutdown(SHUT_RD) 而 poll 会永远报
     * POLLIN|POLLHUP(readable 会变成 true 而读不到东西), 那个短路因此是**语义必需**,
     * 不是优化。 */
    bool readable() const noexcept
    {
        if (server_fd < 0 || !joins_group() || cancelled_)
        {
            return false;
        }
        pollfd pfd{};
        pfd.fd = server_fd;
        pfd.events = POLLIN;
        pfd.revents = 0;
        const int rc = ::poll(&pfd, 1, 0);
        if (rc <= 0)
        {
            return false;   // rc == 0 超时(无数据); rc < 0 出错 ⇒ 当作不可读
        }
        /* 只认"真数据可读"。POLLERR/POLLHUP 单独出现(不带 POLLIN)不算可读 ——
         * 否则无数据的坏 fd 会让 worker 空转(阶段 5 的第一红线)。 */
        return (pfd.revents & POLLIN) != 0;
    }
};
}   // namespace socket
}   // namespace detail
}   // namespace ipc
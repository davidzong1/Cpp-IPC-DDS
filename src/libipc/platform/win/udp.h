
#include <winsock2.h>
#include <ws2tcpip.h>
#include <chrono>
#include <cstdio>
#include <cstring>
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
    SOCKET server_fd{INVALID_SOCKET};
    std::vector<char> temp_buffer;
    ipc::socket::NodeRole role{ipc::socket::NodeRole::SendRecv};
    /* 阶段 5: 惰性创建的可等待事件(WSAEventSelect)。WSA_INVALID_EVENT = 尚未创建。
     * mutable 是因为公开接口 wait_handle() 是 const noexcept(契约冻结签名),
     * 而首次调用需要建事件 —— 与 POSIX 侧(返回已有的 fd)在签名上对齐。 */
    mutable WSAEVENT wait_event_{WSA_INVALID_EVENT};
    /* cancel_wait() 之后置位: 接收面失效, waitable()/wait_handle() 变中性值。 */
    bool cancelled_{false};

    /* 见 posix/udp.h 同名函数: 只有入组的 socket 才收得到组播,
     * 只发不收的节点跳过入组以屏蔽自己的回绕。 */
    bool joins_group() const { return role != ipc::socket::NodeRole::SendOnly; }

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

    /* SOCKET 是原始内核句柄，禁止隐式拷贝以避免双重 closesocket 触发
       EXCEPTION_INVALID_HANDLE，导致进程异常退出 */
    UDPNode(const UDPNode&) = delete;
    UDPNode& operator=(const UDPNode&) = delete;

    /* 移动语义：转移句柄所有权，源对象置为空 */
    UDPNode(UDPNode&& rhs) noexcept
        : port(rhs.port)
        , server_fd(rhs.server_fd)
        , temp_buffer(std::move(rhs.temp_buffer))
        , role(rhs.role)
        , wait_event_(rhs.wait_event_)
        , cancelled_(rhs.cancelled_)
    {
        std::memcpy(name, rhs.name, sizeof(name));
        std::memcpy(ip, rhs.ip, sizeof(ip));
        rhs.server_fd = INVALID_SOCKET;
        rhs.name[0] = '\0';
        rhs.ip[0] = '\0';
        rhs.port = 0;
        rhs.role = ipc::socket::NodeRole::SendRecv;
        /* 阶段 5: 事件的所有权随句柄转移 —— 源对象析构时不得关它(否则新对象手里
         * 就是一个已关闭句柄, WaitForMultipleObjects 会 WAIT_FAILED)。 */
        rhs.wait_event_ = WSA_INVALID_EVENT;
        rhs.cancelled_ = false;
    }

    UDPNode& operator=(UDPNode&& rhs) noexcept
    {
        if (this != &rhs)
        {
            close();
            std::memcpy(name, rhs.name, sizeof(name));
            std::memcpy(ip, rhs.ip, sizeof(ip));
            port = rhs.port;
            server_fd = rhs.server_fd;
            temp_buffer = std::move(rhs.temp_buffer);
            role = rhs.role;
            /* 阶段 5: 事件所有权随句柄转移, 同移动构造。 */
            wait_event_ = rhs.wait_event_;
            cancelled_ = rhs.cancelled_;
            rhs.server_fd = INVALID_SOCKET;
            rhs.name[0] = '\0';
            rhs.ip[0] = '\0';
            rhs.port = 0;
            rhs.role = ipc::socket::NodeRole::SendRecv;
            rhs.wait_event_ = WSA_INVALID_EVENT;
            rhs.cancelled_ = false;
        }
        return *this;
    }

    /* 析构必须释放 socket，否则栈对象（例如握手用的 ser_hs/cli_hs）会泄露 fd；
       此外避免静态/全局对象在 Winsock DLL detach 之后才走 closesocket */
    ~UDPNode()
    {
        close();
        /* 阶段 5: 关掉惰性创建的可等待事件。close() 已先解除 WSAEventSelect 关联,
         * 这里才是事件的最终释放点(移动后的源对象 wait_event_ 已置空)。 */
        if (wait_event_ != WSA_INVALID_EVENT)
        {
            ::WSACloseEvent(wait_event_);
            wait_event_ = WSA_INVALID_EVENT;
        }
    }

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
        if (server_fd != INVALID_SOCKET)
        {
            close();
        }

        this->role = role;

        if (name)
        {
            strncpy_s(this->name, sizeof(this->name) - 1, name, _TRUNCATE);
            this->name[sizeof(this->name) - 1] = '\0';
        }
        else
        {
            this->name[0] = '\0';
        }

        if (ip)
        {
            strncpy_s(this->ip, sizeof(this->ip) - 1, ip, _TRUNCATE);
            this->ip[sizeof(this->ip) - 1] = '\0';
        }
        else
        {
            this->ip[0] = '\0';
        }

        this->port = port;
        this->server_fd = INVALID_SOCKET;
        /* 阶段 5: 重建节点复位 cancel 状态(冻结不变量: cancel 之后 waitable()==false /
         * wait_handle()==0, 只有 close()+connect() 能恢复)。惰性事件本身保留复用。 */
        cancelled_ = false;
        temp_buffer.resize(1'472);   // 预分配最大UDP报文长度
    }

    ipc::socket::NodeRole node_role() const noexcept { return role; }

    /* 检查内核是否把请求的 socket 缓冲截断了, 截断则打一次警告。
     * 与 posix/udp.h 的同名函数对应, 详见那边的注释。
     *
     * Windows 没有 net.core.rmem_max 这样的全局上限, 通常能拿到请求的值;
     * 但驱动或系统资源紧张时仍可能返回更小的值, 所以同样做检查。
     * Winsock 的 getsockopt 直接返回实际值, 不像 Linux 那样翻倍。 */
    static void warn_if_buffer_truncated(SOCKET fd, int want_recv)
    {
        static bool warned = false;
        if (warned)
        {
            return;
        }

        int actual = 0;
        int len = sizeof(actual);
        if (::getsockopt(fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&actual), &len) != 0)
        {
            return;   // 读不回来就不判断, 不要凭猜测报警
        }
        if (actual >= want_recv)
        {
            return;
        }

        warned = true;
        std::fprintf(stderr,
                     "[dzIPC][warn] UDP socket receive buffer truncated by the OS:\n"
                     "  SO_RCVBUF: requested %d B, got %d B\n"
                     "  Large multi-fragment messages may lose fragments (a 1 MB payload is 713\n"
                     "  fragments of 1472 B).\n",
                     want_recv, actual);
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
        /* 阶段 5: close() + connect() 复位 cancel 状态(见 create() 注释)。 */
        cancelled_ = false;

        BOOL reuse = TRUE;
        int nRecvBuf = 1'024 * 1'024;   // 1MB
        if (joins_group())
        {
            ::setsockopt(server_fd, SOL_SOCKET, SO_RCVBUF, reinterpret_cast<char*>(&nRecvBuf), sizeof(nRecvBuf));
            warn_if_buffer_truncated(server_fd, nRecvBuf);
        }

        /* SendOnly 既不 bind 也不入组 —— 详见 posix/udp.h connect() 的注释。 */
        if (joins_group())
        {
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
        }
        else
        {
            in_addr probe{};
            if (::InetPtonA(AF_INET, ip, &probe) != 1)
            {
                ::closesocket(server_fd);
                server_fd = INVALID_SOCKET;
                return false;
            }
        }

        unsigned char ttl = 1;
        ::setsockopt(server_fd, IPPROTO_IP, IP_MULTICAST_TTL, reinterpret_cast<char*>(&ttl), sizeof(ttl));
        /* 保持 1: 置 0 会让本机其他进程也收不到, 见 posix/udp.h 的说明。 */
        unsigned char loop = 1;
        ::setsockopt(server_fd, IPPROTO_IP, IP_MULTICAST_LOOP, reinterpret_cast<char*>(&loop), sizeof(loop));
        return true;
    }

    bool send(ipc::buffer& data)
    {
        if (server_fd == INVALID_SOCKET || data.empty())
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

    ipc::buffer receive_nowait()
    {
        /* SendOnly 没有入组, 永远收不到东西。 */
        if (server_fd == INVALID_SOCKET || !joins_group())
            return ipc::buffer();
        /* 阶段 5: cancel_wait() 之后接收面失效(冻结语义: 一律空 buffer), 不依赖
         * shutdown 后 Winsock 的具体错误码。 */
        if (cancelled_)
            return ipc::buffer();

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);

        timeval timeout{};
        int ready = ::select(0, &read_fds, nullptr, nullptr, &timeout);
        if (ready <= 0)
        {
            return ipc::buffer();
        }

        int received = ::recvfrom(server_fd, temp_buffer.data(), int(temp_buffer.size()), 0, nullptr, nullptr);
        if (received >= 0)
        {
            return ipc::buffer(temp_buffer.data(), received, nullptr);
        }
        else
        {
            int err = ::WSAGetLastError();
            // info too large for buffer
            if (err == WSAEMSGSIZE)
                return ipc::buffer();
            // 异常中断或资源暂时不可用
            if (err == WSAEINTR || err == WSAEWOULDBLOCK)
            {
                return ipc::buffer();
            }
            return ipc::buffer();
        }
    }

    ipc::buffer receive(uint64_t tm)
    {
        /* 同 receive_nowait: SendOnly 不入组, 等下去只是白白阻塞。 */
        if (server_fd == INVALID_SOCKET || !joins_group())
            return ipc::buffer();
        /* 阶段 5: cancel 之后一律返回空, 且**不得**跑无限等待分支。 */
        if (cancelled_)
            return ipc::buffer();

        int err_cnt = 0;

        // 1. 无限等待模式
        if (tm == ipc::invalid_value)
        {
            for (;;)
            {
                int received = ::recvfrom(server_fd, temp_buffer.data(), int(temp_buffer.size()), 0, nullptr, nullptr);
                if (received >= 0)
                {
                    return ipc::buffer(temp_buffer.data(), size_t(received));
                }
                int err = ::WSAGetLastError();
                if (err == WSAEINTR && ++err_cnt < 100)
                    continue;
                return ipc::buffer();
            }
        }

        // 2. 超时等待模式
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

            int ret = ::select(0, &read_fds, nullptr, nullptr, &timeout);   // Windows下select第一个参数被忽略

            if (ret > 0)
            {
                int received = ::recvfrom(server_fd, temp_buffer.data(), int(temp_buffer.size()), 0, nullptr,
                                          nullptr);
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
        if (joins_group() && ::InetPtonA(AF_INET, ip, &mreq.imr_multiaddr) == 1)
        {
            mreq.imr_interface.s_addr = htonl(INADDR_ANY);
            ::setsockopt(server_fd, IPPROTO_IP, IP_DROP_MEMBERSHIP, reinterpret_cast<char*>(&mreq), sizeof(mreq));
        }

        /* 阶段 5: 先解除 WSAEventSelect 关联再关 socket。
         * WSAEventSelect 的事件属于 node(不是 socket), close 之后它还必须能用
         * (cancel_wait 的 WSASetEvent 只有一个事件句柄可敲), 所以**不**在这里关它;
         * 真正释放留到 wait_handle() 创建新事件或进程退出。 */
        if (wait_event_ != WSA_INVALID_EVENT)
        {
            ::WSAEventSelect(server_fd, wait_event_, 0);
        }

        ::closesocket(server_fd);
        server_fd = INVALID_SOCKET;
        return true;
    }

    void clear_cache()
    {
        if (server_fd == INVALID_SOCKET)
            return;
        // 循环读取直到缓冲区空，用 select 0 超时判断可读，避免阻塞
        char discard_buf[1'472];   // UDP最大报文长度
        for (;;)
        {
            fd_set read_fds;
            FD_ZERO(&read_fds);
            FD_SET(server_fd, &read_fds);
            timeval tv{};
            int ready = ::select(0, &read_fds, nullptr, nullptr, &tv);
            if (ready <= 0)
                break;
            if (::recvfrom(server_fd, discard_buf, int(sizeof(discard_buf)), 0, nullptr, nullptr) <= 0)
                break;
        }
    }

    /* ================= 阶段 5: 可等待句柄 (平台实现) =================
     * 被 ipc::socket::UDPNode 的同名方法(纯转发, src/libipc/socket/udp.cpp)调用,
     * 语义冻结于契约 §1 与勘误 E1。 */

    /* 同 POSIX 侧: 已连接 + 入组 + 未 cancel。SendOnly 不入组 ⇒ 不可等待。 */
    bool waitable() const noexcept
    {
        return server_fd != INVALID_SOCKET && joins_group() && !cancelled_;
    }

    /* 可等待句柄 = 惰性创建的 WSAEVENT(不是 SOCKET)。
     *
     * 首次调用内部做 WSAEventSelect(fd, event, FD_READ | FD_CLOSE) —— WinSock 的
     * 固有副作用是把该 socket 置为非阻塞, 这里没有显式 FIONBIO(勘误 E1 明确不要加)。
     * 0 = 不可等待。
     *
     * 事件属于 node 而不是 socket: close() 只解除关联, 事件本身留到析构才关, 这样
     * cancel_wait() 的 WSASetEvent 始终有一个有效句柄可敲。 */
    std::uintptr_t wait_handle() const noexcept
    {
        if (!waitable())
        {
            return std::uintptr_t{0};
        }
        if (wait_event_ == WSA_INVALID_EVENT)
        {
            if (!ensure_wsa())
            {
                return std::uintptr_t{0};   // Winsock 未初始化 ⇒ 按不可等待处理
            }
            wait_event_ = ::WSACreateEvent();
            if (wait_event_ == WSA_INVALID_EVENT)
            {
                return std::uintptr_t{0};   // 事件创建失败 ⇒ 走兼容回退, 不假装可等待
            }
        }
        if (::WSAEventSelect(server_fd, wait_event_, FD_READ | FD_CLOSE) == SOCKET_ERROR)
        {
            /* 关联失败(例如 socket 刚被别的线程关掉) ⇒ 同样按不可等待处理。事件句柄保留:
             * 下一次 close()+connect() 会重新关联。 */
            return std::uintptr_t{0};
        }
        return reinterpret_cast<std::uintptr_t>(wait_event_);
    }

    /* 取消阻塞在 wait_handle 上的等待。
     * Windows 侧不能靠 shutdown 唤醒 WaitForMultipleObjects —— 事件是 node 的, 敲自己的
     * 事件是唯一手段。shutdown(SD_RECEIVE) 同时让读面失效(此后 recvfrom 立刻返回错误 ⇒
     * receive* 返回空 buffer)。幂等。 */
    void cancel_wait() noexcept
    {
        if (cancelled_)
        {
            return;
        }
        cancelled_ = true;
        if (wait_event_ != WSA_INVALID_EVENT)
        {
            ::WSASetEvent(wait_event_);
        }
        if (server_fd != INVALID_SOCKET)
        {
            ::shutdown(server_fd, SD_RECEIVE);   // 关闭读方向
        }
    }

    /* 清除 wait_handle 上的就绪提示: 回收 WSAEventSelect 记录的网络事件。
     * 没有这一步事件会保持置位(WSAEventSelect 是边沿记录 + 手动回收), 下一次
     * WaitForMultipleObjects 立刻返回同一通道 ⇒ level-triggered 重检退化成忙轮询。 */
    void clear_wait() noexcept
    {
        if (server_fd == INVALID_SOCKET || wait_event_ == WSA_INVALID_EVENT)
        {
            return;
        }
        WSANETWORKEVENTS events{};
        ::WSAEnumNetworkEvents(server_fd, wait_event_, &events);   // 同时重置事件
    }

    /* ---------------- 阶段 5 追加(t12 裁定 C): 非阻塞可读判据 ----------------
     *
     * 0 超时 select ⇒ 不阻塞、无分配、O(1), 且**不做读操作**(不消费数据)。
     *
     * ⛔ 刻意**不用** WSAEventSelect 的事件查询(如 WSAWaitForMultipleEvents(0)):
     * 那个事件是"边沿记录"语义(见 clear_wait 注释), 查询/回收会**改变**事件状态,
     * 而本函数的冻结语义是"不改变任何状态"。select 只读内核的接收队列状态, 幂等,
     * 且与 wait_handle 的事件机制互不干扰。
     *
     * 语义边界同 POSIX 侧: 无效 fd / 未入组 / 已 cancel 一律 false。注意
     * WaitForMultipleObjects 的就绪在 cancel 之后由 WSASetEvent 提供, 而这里返回
     * false —— 两者服务不同的问题("该不该醒来" vs "有没有真数据"), 消费方据此区分
     * "被唤醒" 与 "有数据可读"。 */
    bool readable() const noexcept
    {
        if (server_fd == INVALID_SOCKET || !joins_group() || cancelled_)
        {
            return false;
        }
        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(server_fd, &read_fds);
        timeval timeout{};   // 0 超时 = 纯查询
        const int rc = ::select(0, &read_fds, nullptr, nullptr, &timeout);
        return rc > 0 && FD_ISSET(server_fd, &read_fds);
    }
};
}   // namespace socket
}   // namespace detail
}   // namespace ipc
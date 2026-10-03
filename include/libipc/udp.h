#pragma once
#include <cstdint>
#include <array>
#include <cstring>
#include "libipc/buffer.h"
#include "libipc/debug.h"
#include "libipc/export.h"

namespace ipc {
namespace socket {

/* 节点的收发角色 —— 决定 connect() 是否加入组播组。
 *
 * 组播的接收资格由 IP_ADD_MEMBERSHIP 决定, 发送则不需要入组。于是"只发不收"
 * 的节点只要不入组, 就收不到任何东西, 包括自己发出去、被内核回绕回来的包。
 *
 * 这正是我们需要的自我屏蔽: 发送端在一条专用的 ACK socket 上等确认时, 不会被
 * 自己刚发出的几百个数据分片堵住。
 *
 * ---- 为什么不用 IP_MULTICAST_LOOP=0 ----
 * 那个选项看起来更直接, 但它是发送端选项且作用于**整台主机**: 一旦置 0, 本机
 * 所有 socket(包括其他进程的订阅者和抓包工具)都收不到这个包, 不只是发送方
 * 自己。dzIPC 的核心场景恰恰是同机跨进程, 关掉它等于让所有本地订阅者失聪。
 * 所以 IP_MULTICAST_LOOP 一律保持 1, 屏蔽自己靠"不入组"来做。 */
enum class NodeRole
{
    SendRecv,   // 收发共用一条 socket, 入组。缺省值, 行为与历史版本一致
    SendOnly,   // 只发不收: 不入组, 因而也收不到自己的回绕
    RecvOnly    // 只收不发: 入组。send() 仍可用, 但语义上不应调用
};

class IPC_EXPORT UDPNode
{
    UDPNode(const UDPNode&) = delete;
    UDPNode& operator=(const UDPNode&) = delete;

public:
    UDPNode();
    ~UDPNode();
    /* Instantiation */
    UDPNode(const char* name, const char* ip, uint16_t port);
    void create(const char* name, const char* ip, uint16_t port) IPC_EXCEPTION_;

    /* 带角色的重载。刻意做成重载而不是给上面两个加默认参数 —— 加默认参数会改变
     * 函数签名与符号名, 已经链接了旧 libipc 的二进制会找不到符号。重载则是纯新增。 */
    UDPNode(const char* name, const char* ip, uint16_t port, NodeRole role);
    void create(const char* name, const char* ip, uint16_t port, NodeRole role) IPC_EXCEPTION_;

    // 仅在连接/收发前设置。作用域固定头由传输层生成；底层裸 UDP 默认不封装。
    void set_scope(const std::array<std::uint8_t,32>& token);
    // 混合传输的可选来源头：共享内存身份 16B + 发布实例 8B + 序号低位 4B。
    // 仅发送前设置，由调用方串行化同一发布者；全零头恢复普通 UDP。
    void set_hybrid_source(const std::array<std::uint8_t,32>& source);
    // 在连接前配置；本机已经走 SHM 的帧在重组和 ACK 前丢弃。
    void suppress_hybrid_local(const std::array<std::uint8_t,16>& identity);
    std::uint64_t hybrid_suppressed() const noexcept;
    NodeRole role() const noexcept;

    bool connect() IPC_EXCEPTION_;
    bool send(ipc::buffer& data) IPC_EXCEPTION_;
    ipc::buffer receive_nowait() IPC_EXCEPTION_;
    ipc::buffer receive(uint64_t tm = ipc::invalid_value) IPC_EXCEPTION_;
    bool close() IPC_EXCEPTION_;
    void clear_cache() IPC_EXCEPTION_;

    /* ---------------- 阶段 5: 可等待句柄 (纯新增, 签名冻结) ----------------
     * 消费方: dzIPC 的 SocketWaitSet
     * (include/dzIPC/threepools/socket_wait_set.h), 以及经
     * dzIPC/common/data_rev.h 的 udp_node_* 四个转发接入的 socket 两个模块。
     *
     * 冻结语义见 docs/消息接收架构改造/
     * ipc-transport-phase5-shared-wait-layer-contract-99ff82a0f9af.md §1,
     * 并**以勘误** ...-errata-d8f45cfa45bb.md 的 E1 为准(勘误优先于正文):
     *
     *   wait_handle() **不改变** fd 的阻塞模式。Linux 返回的就是接收 fd 本身, 它
     *   保持**阻塞**; 从 wait-set 拿到就绪后一律用 receive_nowait() 读取, 不得对该
     *   fd 用裸 recvfrom() —— 那会让 worker 线程永久挂住。也**不得**自己给它置
     *   非阻塞: receive(invalid_value) 的无限等待分支依赖阻塞语义, 置了会退化成
     *   紧循环忙轮询(阶段 5 第一红线)。
     *
     * 不变量: wait_handle() != 0 ⇒ waitable() == true; cancel_wait() 之后
     * waitable() == false 且 wait_handle() == 0。close() + connect() 会复位
     * cancel 状态。 */

    /* 该节点能否被多路等待: 已 connect、入组(role != SendOnly)、未被 cancel。
     * SendOnly 不入组 ⇒ 永远收不到东西, 不构成可等待通道。 */
    bool waitable() const noexcept;

    /* 可等待句柄。Linux = 接收 fd(阻塞模式保持原样);
     * Windows = 惰性创建的 WSAEVENT(首次调用内部做 WSAEventSelect; WinSock 的固有
     * 副作用是把 socket 置为非阻塞)。0 表示不可等待。 */
    std::uintptr_t wait_handle() const noexcept;

    /* 取消阻塞在 wait_handle 上的等待, 并让本节点的接收面失效。
     * Linux: shutdown(fd, SHUT_RD) —— 阻塞中的 epoll_wait / recvfrom 立刻返回,
     *        此后 receive()/receive_nowait() 一律返回空 buffer。
     * Windows: WSASetEvent(WSAEventSelect 的事件属于 node, 必须由 node 唤醒)。
     * 幂等。⚠️ cancel 之后**不得**复用该 node 接收; close()+connect() 会复位。 */
    void cancel_wait() noexcept;

    /* 清除 wait_handle 上的就绪提示(level-triggered 重检前调用)。
     * Linux: no-op(epoll 本身就是 level-triggered);
     * Windows: WSAEnumNetworkEvents。 */
    void clear_wait() noexcept;

    /* ---------------- 阶段 5 追加(t12 裁定 C): 非阻塞可读判据 ----------------
     *
     * 该接收通道**当前**是否有可读数据。语义(冻结):
     *   · 不阻塞、不改动任何状态、**不做读操作**(不消费数据、不动临时缓冲);
     *   · 幂等、O(1)、无分配；
     *   · server_fd 无效 / 未入组(role == SendOnly) / 已 cancel_wait 一律 false。
     *
     * Linux: poll(fd, POLLIN, 0) > 0(等价 FIONREAD); Windows: 对 fd 做 0 超时 select。
     *
     * 消费方(socket 收包 worker 的 recv_once)必须**先问它再读**:
     * chunk_rev_topic/chunk_rev_server 一律带 tm 且空闲时会阻塞到 tm, 在**共享** worker
     * 线程上直接调用它们, 每次数据突发收尾都会让该线程空转阻塞最长 tm(注册轮同理)。
     * 有它就写成：if (!udp_node_readable(node)) return 0; 然后才 chunk_rev_* —— 无数据时
     * 立即返回 0, 不占用共享线程。 */
    bool readable() const noexcept;

private:
    class UDPNode_;
    UDPNode_* p_;
};
}   // namespace socket
}   // namespace ipc
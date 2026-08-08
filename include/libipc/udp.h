#pragma once
#include <cstdint>
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

    NodeRole role() const noexcept;

    bool connect() IPC_EXCEPTION_;
    bool send(ipc::buffer& data) IPC_EXCEPTION_;
    ipc::buffer receive_nowait() IPC_EXCEPTION_;
    ipc::buffer receive(uint64_t tm = ipc::invalid_value) IPC_EXCEPTION_;
    bool close() IPC_EXCEPTION_;
    void clear_cache() IPC_EXCEPTION_;

private:
    class UDPNode_;
    UDPNode_* p_;
};
}   // namespace socket
}   // namespace ipc
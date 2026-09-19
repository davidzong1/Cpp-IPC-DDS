#include "libipc/udp.h"
#include "libipc/debug.h"
#include "libipc/memory/resource.h"
#include "libipc/platform/detail.h"
#include "libipc/utility/log.h"
#include "libipc/utility/pimpl.h"
#if defined(IPC_OS_WINDOWS_)
#    include "libipc/platform/win/udp.h"
#elif defined(IPC_OS_LINUX_) || defined(IPC_OS_QNX_)
#    include "libipc/platform/posix/udp.h"
#endif

namespace ipc {
namespace socket {
/* 前向声明 */
class UDPNode::UDPNode_ : public ipc::pimpl<UDPNode_>
{
public:
    ipc::detail::socket::UDPNode node_;
};

/* 后续定义 */
UDPNode::UDPNode()
    : p_(p_->make())
{}

UDPNode::UDPNode(const char* name, const char* ip, uint16_t port)
    : UDPNode()
{
    create(name, ip, port);
}

UDPNode::UDPNode(const char* name, const char* ip, uint16_t port, NodeRole role)
    : UDPNode()
{
    create(name, ip, port, role);
}

UDPNode::~UDPNode()
{
    close();
    delete p_;
}

/* ⛔ UF-002: `p_ == nullptr` 是失效态(`pimpl<UDPNode_>` 走"不舒服"分支 ⇒ impl 在堆上,
 * `mem::alloc<UDPNode_>` 失败时返回 nullptr 而不抛, 构造函数不检查)。
 * 收口方式同 docs/unfixed_defects.md §2「修法选项 1」: 入口判空 + 失效态空转。
 * `~UDPNode()` 走的是 `delete p_`(安全), 但它上一句 `close()` 在旧实现里要解引用 `p_`。 */
void UDPNode::create(const char* name, const char* ip, uint16_t port) IPC_EXCEPTION_
{
    auto n = impl(p_);
    if (n == nullptr) {
        ipc::error("fail udp create: node is in invalid state (pimpl alloc failed)\n");
        return;
    }
    n->node_.create(name, ip, port);
}

void UDPNode::create(const char* name, const char* ip, uint16_t port, NodeRole role) IPC_EXCEPTION_
{
    auto n = impl(p_);
    if (n == nullptr) {
        ipc::error("fail udp create: node is in invalid state (pimpl alloc failed)\n");
        return;
    }
    n->node_.create(name, ip, port, role);
}

NodeRole UDPNode::role() const noexcept
{
    auto n = impl(p_);
    /* 失效态返回缺省角色(SendRecv, 见 udp.h:26 "缺省值, 行为与历史版本一致") ——
     * 刻意**不**新增枚举值: 加值会改枚举的取值范围(ABI/语义), 而失效态本身由
     * connect()/send() 的 false 暴露, 不需要靠 role() 说谎。 */
    return (n == nullptr) ? NodeRole::SendRecv : n->node_.node_role();
}

bool UDPNode::connect() IPC_EXCEPTION_
{
    auto n = impl(p_);
    return (n == nullptr) ? false : n->node_.connect();
}

bool UDPNode::send(ipc::buffer& data) IPC_EXCEPTION_
{
    auto n = impl(p_);
    return (n == nullptr) ? false : n->node_.send(data);
}

ipc::buffer UDPNode::receive_nowait() IPC_EXCEPTION_
{
    auto n = impl(p_);
    return (n == nullptr) ? ipc::buffer{} : n->node_.receive_nowait();
}

ipc::buffer UDPNode::receive(uint64_t tm) IPC_EXCEPTION_
{
    auto n = impl(p_);
    return (n == nullptr) ? ipc::buffer{} : n->node_.receive(tm);
}

bool UDPNode::close() IPC_EXCEPTION_
{
    auto n = impl(p_);
    return (n == nullptr) ? false : n->node_.close();
}

void UDPNode::clear_cache() IPC_EXCEPTION_
{
    auto n = impl(p_);
    if (n == nullptr) return;
    n->node_.clear_cache();
}
}   // namespace socket
}   // namespace ipc
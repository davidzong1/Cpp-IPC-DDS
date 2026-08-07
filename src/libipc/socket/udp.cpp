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

void UDPNode::create(const char* name, const char* ip, uint16_t port) IPC_EXCEPTION_
{
    impl(p_)->node_.create(name, ip, port);
}

void UDPNode::create(const char* name, const char* ip, uint16_t port, NodeRole role) IPC_EXCEPTION_
{
    impl(p_)->node_.create(name, ip, port, role);
}

NodeRole UDPNode::role() const noexcept
{
    return impl(p_)->node_.node_role();
}

bool UDPNode::connect() IPC_EXCEPTION_
{
    return impl(p_)->node_.connect();
}

bool UDPNode::send(ipc::buffer& data) IPC_EXCEPTION_
{
    return impl(p_)->node_.send(data);
}

ipc::buffer UDPNode::receive_nowait() IPC_EXCEPTION_
{
    return impl(p_)->node_.receive_nowait();
}

ipc::buffer UDPNode::receive(uint64_t tm) IPC_EXCEPTION_
{
    return impl(p_)->node_.receive(tm);
}

bool UDPNode::close() IPC_EXCEPTION_
{
    return impl(p_)->node_.close();
}

void UDPNode::clear_cache() IPC_EXCEPTION_
{
    impl(p_)->node_.clear_cache();
}
}   // namespace socket
}   // namespace ipc
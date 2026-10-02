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
    std::array<std::uint8_t,32> scope_{};
    bool scoped_=false;
    ipc::buffer accept(ipc::buffer data) {
        if(!scoped_)return data;
        if(data.size()<=scope_.size() || std::memcmp(data.data(),scope_.data(),scope_.size())!=0)return {};
        return ipc::buffer(static_cast<std::uint8_t*>(data.data())+scope_.size(),data.size()-scope_.size(),nullptr);
    }
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
    n->scoped_=false;
    n->node_.create(name, ip, port);
}

void UDPNode::create(const char* name, const char* ip, uint16_t port, NodeRole role) IPC_EXCEPTION_
{
    auto n = impl(p_);
    if (n == nullptr) {
        ipc::error("fail udp create: node is in invalid state (pimpl alloc failed)\n");
        return;
    }
    n->scoped_=false;
    n->node_.create(name, ip, port, role);
}

void UDPNode::set_scope(const std::array<std::uint8_t,32>& token) {
    auto n=impl(p_);if(n){n->scope_=token;n->scoped_=true;}
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
    if(!n)return false;
    if(!n->scoped_)return n->node_.send(data);
    if(data.empty() || data.size()>ipc::wire_packet_size)return false;
    // 载荷栈内封装，不为载荷申请堆内存；buffer 句柄仍用库分配器。总长不超过 1472 字节。
    std::array<std::uint8_t,1472> frame;
    std::memcpy(frame.data(),n->scope_.data(),n->scope_.size());
    std::memcpy(frame.data()+n->scope_.size(),data.data(),data.size());
    ipc::buffer wire(frame.data(),n->scope_.size()+data.size(),nullptr);
    return n->node_.send(wire);
}

ipc::buffer UDPNode::receive_nowait() IPC_EXCEPTION_
{
    auto n = impl(p_);
    return (n == nullptr) ? ipc::buffer{} : n->accept(n->node_.receive_nowait());
}

ipc::buffer UDPNode::receive(uint64_t tm) IPC_EXCEPTION_
{
    auto n = impl(p_);
    return (n == nullptr) ? ipc::buffer{} : n->accept(n->node_.receive(tm));
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

/* ---------------- 阶段 5: 可等待句柄的 pimpl 转发 ----------------
 * 勘误 E3 指定的**唯一落点**: include/libipc/udp.h 新增的 4 个声明的实现在这里,
 * 缺了就是未定义符号、链接失败。内容只有"4 个纯转发 + 失效态中性值", 不触碰任何
 * 既有方法。
 *
 * 失效态(p_ == nullptr, 见上面 UF-002)必须返回**中性值**, 而不是抛/崩:
 *   waitable    -> false (不可等待 ⇒ 调用方走兼容回退)
 *   wait_handle -> 0     (0 是"不可等待句柄"的冻结编码)
 *   cancel_wait / clear_wait -> no-op (幂等, 不得崩)
 * 与既有方法同一口径: connect() 失效态返回 false, receive* 失效态返回空 buffer。 */

bool UDPNode::waitable() const noexcept
{
    auto n = impl(p_);
    return (n == nullptr) ? false : n->node_.waitable();
}

std::uintptr_t UDPNode::wait_handle() const noexcept
{
    auto n = impl(p_);
    return (n == nullptr) ? std::uintptr_t{0} : n->node_.wait_handle();
}

void UDPNode::cancel_wait() noexcept
{
    auto n = impl(p_);
    if (n == nullptr) return;
    n->node_.cancel_wait();
}

void UDPNode::clear_wait() noexcept
{
    auto n = impl(p_);
    if (n == nullptr) return;
    n->node_.clear_wait();
}

/* 阶段 5 追加(t12 裁定 C): 非阻塞可读判据的纯转发。失效态(p_ == nullptr)同样返回
 * 中性值 false —— 与 waitable() 同口径("不可读" ⇒ 调用方走兼容回退, 而不是空转)。 */
bool UDPNode::readable() const noexcept
{
    auto n = impl(p_);
    return (n == nullptr) ? false : n->node_.readable();
}
}   // namespace socket
}   // namespace ipc
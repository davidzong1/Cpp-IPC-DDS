#pragma once
/* 阶段 5 · socket 侧可等待 backend（SocketWaitSet）。
 *
 * 落点与消费方见 docs/消息接收架构改造/socket_pub_sub线程池移植方案.md §3、
 * socket_ser_cli线程池移植方案.md §3、事件驱动线程池需求.md §5 与
 * `ipc-transport-phase5-shared-wait-layer-contract-99ff82a0f9af.md` §3。
 *
 * ---- 为什么不能复用 ipc::recv_wait_set ----
 * `ipc::recv_wait_set`（include/libipc/recv_wait_set.h）等的是**共享内存里的
 * sequence 字**，靠 futex_waitv 在多个进程的不同虚拟地址上匹配同一个物理页。
 * UDP socket 没有这样的共享字可等：可读与否只有内核知道，唯一的多路等待原语
 * 是 epoll（Linux）/ WaitForMultipleObjects（Windows）。两者语义可以对齐，机制
 * 不能复用，因此这里另起一套，但**对齐并加强** recv_wait_set 的对外语义（见下表）。
 *
 * ---- 平台边界（硬约束）----
 * 本头文件**不得**出现任何平台宏。句柄一律是 `std::uintptr_t`：
 *   Linux   = 接收 fd（由 ipc::socket::UDPNode::wait_handle() 给出；**不改动 fd
 *             的阻塞模式**，读取一律走 receive_nowait / MSG_DONTWAIT）
 *   Windows = 惰性创建的 WSAEVENT（同一函数给出，**不是** SOCKET）
 *   0       = 不可等待
 * 平台 #ifdef 只允许出现在 src/dzIPC/threepools/socket_wait_set.cc 里。
 * socket 两个模块作者只依赖本头文件，不得自己拼平台调用。
 *
 * ---- 语义（与 ipc::recv_wait_set 对齐，并加强）----
 *   add        幂等：同一 owner 重复 add 成功。同 owner 不同 handle、或不同
 *              owner 同 handle ⇒ 拒绝（false，⚠️ 本类加强项：recv_wait_set 只按 token 相等判重），不改变已有集合。超过
 *              max_channels() ⇒ false（由上层把通道分到别的 worker，不得循环
 *              多次等待假装支持无限路）。
 *   remove     幂等：未注册的 token 也是成功。**必须唤醒**阻塞中的 wait ——
 *              这是注销协议的第 2 步（先摘除、再唤醒、再等 in-flight 归零），
 *              漏掉唤醒就会让 worker 睡过注销。
 *   wait       true  = 至少一路就绪，**或**被 remove/stop 唤醒 ⇒ 调用方应
 *                      consume_ready()（可能为空集）。
 *              false = 超时，或 backend 系统调用失败（已记日志）。超时不是
 *                      错误；失败按「后端不可用」处理，**禁止忙等**。
 *   consume_ready 只返回**当前在册**的 handle，因此已 remove 的 fd 即便还有
 *              残留事件也不会误触发；remove 会 epoll_ctl(DEL) 清掉该 fd 在
 *              ready list 里的残留，fd 号复用（close 后新 socket 拿到同一 fd）
 *              也不会把旧事件算到新 token 上。
 *   level-triggered
 *              epoll 默认 LT：没读走的可读事件会让下一次 wait 立即返回，调用方
 *              **必须真读**（receive_nowait / chunk_rev_*），不能只消费事件。
 *   stop       noexcept，幂等；阻塞中的 wait 必须返回。之后 wait 立即返回 true，
 *              由 worker 循环据此退出。
 *
 * ---- 析构与并发约束（调用方必须遵守）----
 * 一个 SocketWaitSet 实例**同一时刻只允许一个线程在 wait() 里**（与 worker 循环
 * 一一对应）。`~SocketWaitSet()` 等价 stop()，但 stop 只保证阻塞中的 wait 会返回，
 * **不**保证它在 impl_ 释放前已经返回 —— 调用方必须先 stop() 并 join 掉 wait 线程
 * （或确保没有线程在 wait 里）再销毁本对象，否则是 use-after-free。
 *
 * ---- fd 复用为什么安全（实测依据）----
 * 本机实测（Linux 6.8）：
 *   · `epoll_ctl(EPOLL_CTL_DEL)` 会同步清除该 fd 在 ready list 中的残留；
 *   · `epoll_ctl(DEL)` **不会**唤醒阻塞中的 epoll_wait（实测返回 0 = 超时），
 *     所以 remove/stop 必须走**独立的唤醒通道**（内部 eventfd），不能指望 DEL；
 *   · `close(fd)` 之后新建 socket 会**复用同一 fd 号**（实测 A=3, B=3）。
 * 三条合起来 ⇒ 摘除必须显式 DEL + 显式唤醒，且 consume_ready 必须按当前在册
 * 集合过滤。缺任何一条都会出现「旧 fd 的事件算到新通道上」的误触发。
 */
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <vector>

#include "libipc/export.h"

namespace dzIPC {
namespace threepools {

/* 一路可等待通道的稳定身份。
 *
 * owner 是**宿主侧稳定身份**（`ipc::socket::UDPNode*` 或 shared state 指针），
 * 生命周期由宿主保证；handle 是平台句柄。两者一起构成身份：只用 handle 会在
 * fd 复用下撞车，只用 owner 无法发现宿主悄悄换了底层 socket。
 *
 * ⚠️ token **不拥有**通道。remove 之后宿主才能 close/destroy，且必须保证
 * remove 返回后再无 wait 会引用该 handle（本类的 remove 是同步摘除 + 唤醒，
 * 满足这一条）。 */
struct SocketWaitToken
{
    const void* owner{nullptr};
    std::uintptr_t handle{0};

    bool valid() const noexcept { return owner != nullptr && handle != 0; }

    friend bool operator==(const SocketWaitToken& a, const SocketWaitToken& b) noexcept
    {
        return a.owner == b.owner && a.handle == b.handle;
    }
    friend bool operator!=(const SocketWaitToken& a, const SocketWaitToken& b) noexcept
    {
        return !(a == b);
    }
};

/* 进程内可复用的多路等待集合。**一个 worker 一个实例**，不是进程单例 ——
 * 每个 worker 需要独立的 ready 集合与唤醒通道，共享一个集合会让唤醒互相干扰
 * （A worker 的 stop 会叫醒 B worker）。 */
class IPC_EXPORT SocketWaitSet
{
public:
    /* 能力探测：进程内缓存，只探测一次。
     * false ⇒ 调用方必须**显式回退**到兼容的每通道线程（socket 两模块方案
     * §3/U1/C1：不得用 receive_nowait 全量忙轮询降级），且不得在运行中来回
     * 切换后端。 */
    static bool backend_available() noexcept;

    /// 诊断名："epoll" / "WaitForMultipleObjects" / "none"。
    static const char* backend_name() noexcept;

    /* 本实现的通道数上限。Linux: 策略上限（内核侧 epoll 无固定路数限制）；
     * Windows: 63（WaitForMultipleObjects 上限 64，唤醒事件占 1）。 */
    static std::size_t max_channels() noexcept;

    SocketWaitSet();
    ~SocketWaitSet();   ///< 等价 stop()
    SocketWaitSet(const SocketWaitSet&) = delete;
    SocketWaitSet& operator=(const SocketWaitSet&) = delete;

    /* 见文件头语义表。返回 false 时集合不变。 */
    bool add(const SocketWaitToken& token);
    bool remove(const SocketWaitToken& token);

    bool wait(std::chrono::milliseconds timeout);

    /* 交出就绪集合并清空内部 ready 缓存。只含当前在册的 token。 */
    std::vector<SocketWaitToken> consume_ready();

    void stop() noexcept;

    /// 当前在册通道数。线程安全。
    std::size_t size() const noexcept;

    /* 与 ShmControlScheduler / RouteSession 同一理由：跨 .so 边界使用的类型，
     * Linux 上 IPC_EXPORT 当前展开为空，一旦引入 -fvisibility=hidden，未标注
     * 的类会导致符号不可见。 */
    struct Impl;

private:
    Impl* impl_{nullptr};
};

}   // namespace threepools
}   // namespace dzIPC

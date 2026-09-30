#pragma once
/* 阶段 2 `RouteSession` —— 单个订阅 route 的收包生命周期协议。
 *
 * 落点与范围见 docs/消息接收架构改造/阶段2_RouteSession实现说明.md §2-§6。
 * 本模块是分工表 A 项；当前阶段 2 主线已由 shm_pub_sub_ipc 的收包循环、握手路径
 * 和析构顺序接入（B/C/D 项），对应改造与验收见阶段 2 说明及进度缓存。
 *
 * ---- 为什么需要它（说明 §0）----
 * 现状 subscribe_thread_ 在 channel_mtx_ 里调用 subscriber_->recv(50)
 * （src/dzIPC/shm_pub_sub_ipc.cc:792-797），握手线程要 release/reset/disconnect
 * 时也拿这把锁。后果有两条：
 *   ① shared_ptr<ipc::route> 拷走也不够 —— 另一线程仍可对同一个对象调用
 *      release()，改掉内部句柄。锁是为了挡住这件事，不是为了挡住指针本身。
 *   ② 重建线程会堵在锁上，直到这次 recv(50) 返回。控制面若已迁到阶段 1 的
 *      单线程调度器，这 50ms 会推迟同进程其它话题的心跳。
 * 本类把「谁可以 recv」和「何时允许 release」写成协议，收包路径才能删掉
 * channel_mtx_。
 *
 * ---- 不变式（改实现前先读这五条；每一条都有单测守门）----
 *   I1 recv 期间对象存活：acquire_receive 返回的 shared_ptr 让 route 在 recv
 *      全程保活。release_receive 之前调用方不得自行 release/reset 该对象。
 *   I2 不并发 release：只有在 receive_inflight_ == 0 且已禁止新 lease 之后，
 *      才允许 release() 旧 route。shared_ptr 的引用计数只能保证 C++ 对象还在，
 *      **保证不了** release() 与 recv() 不并发（说明 §4 第 5 步）。
 *   I3 拒绝新 lease 在先：stopping_ / rebuilding_ 置位后 acquire_receive 一律
 *      返回空；重建期间不得有新的 recv 进入旧对象。
 *   I4 计数配对：每次成功 acquire_receive 恰好一次 release_receive（含 recv
 *      抛出的所有出口）。漏一次 ⇒ wait_quiescent 永不返回（静默挂死）；多一次
 *      ⇒ size_t 下溢 ⇒ 同样永不返回。两个方向都必须挡住。
 *   I5 不丢已弹出的字节：generation 只用于阻止**新的** recv 与决定何时 release，
 *      不得因为 lease.generation 落后于当前 generation 就丢掉 recv 已返回的
 *      buffer —— 字节已从旧 route 弹出，丢掉就是丢消息（说明 §3）。
 *
 * ---- 调用方义务（超出本类能力，必须在 B/C 里做对）----
 *   · create 由调用方提供（现有代码是 std::make_shared<ipc::route>(
 *     topic_name_.c_str(), ipc::receiver, verbose_)）。本类不拼段名。
 *   · disconnect / quit_waiting 由本类在**锁外**调用（说明 §4 第 3 步、§6），
 *     避免 recv 的唤醒路径回头再要 mtx_。
 *   · 析构顺序见说明 §5：先注销 LocalPubSubRegistry，再 stop_and_wake()，再
 *     join 收包线程，再 wait_quiescent()，最后 release/reset route。析构线程
 *     **不得**在持有本类锁时 join 收包线程（收包线程的 release_receive 要拿这
 *     把锁）。
 *   · 「清空 route 且不建新对象」（说明 §4 表格 add_peer 失败 / 控制面离开
 *     Ready）：用 stop_and_wake() + wait_quiescent()；此后对 current_route() 的
 *     拷贝调 release() 是安全的（此刻无 lease，且已拒绝新 lease）。
 *   · 析构本类之前必须先 wait_quiescent()：正在 recv 的线程持有 lease 里的
 *     shared_ptr，本类析构后它的 release_receive 会访问已析构对象（UB）。
 *
 * ---- 接口语义裁定（文档未明说，此处显式定下）----
 *   `stop_and_wake()` **不是终态**。一次**成功**的 begin_rebuild 会重新开启
 *   lease（复位 stopping_）：说明 §4 表格的「add_peer 失败」路径是
 *   stop_and_wake + wait_quiescent + release，之后调用方走「稍后重试」——
 *   重试会重新 Ready 并再次 begin_rebuild。若那里不复位 stopping_，
 *   acquire_receive 将永远为空，该话题收包**静默停摆**。
 *   析构路径不会再 begin_rebuild，所以说明 §5 的「先 stop_and_wake、join、
 *   再 wait_quiescent」语义不受影响（不会复活任何 lease）。
 */
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>

#include "libipc/export.h"
#include "libipc/ipc.h"

namespace dzIPC {
namespace shm {

/* 单个订阅 route 的生命周期协议。接口即 docs 阶段 2 说明 §2，签名一字不改。
 *
 * 线程模型：acquire_receive / release_receive / begin_rebuild / stop_and_wake /
 * wait_quiescent 可来自不同线程。阶段 2 的实际调用方只有 subscribe_thread_ 与
 * 握手线程（或阶段 1 调度线程上的握手回调）。
 *
 * 标 IPC_EXPORT 的理由同 SubControlState：它是跨 .so 边界被使用的类型（宿主在
 * libipc 内、单测在 libipc 外链接），Linux 上 IPC_EXPORT 当前展开为空，一旦引入
 * -fvisibility=hidden，未标注的类会导致符号不可见。 */
class IPC_EXPORT RouteSession
{
public:
    struct ReceiveLease
    {
        std::shared_ptr<ipc::route> route;
        uint32_t generation{0};
    };

    RouteSession() = default;

    /* 析构**不**替调用方 release：那需要与收包线程同步，而析构线程按说明 §5
     * 已经做过 stop_and_wake + join + wait_quiescent。这里只放掉自己那份
     * shared_ptr。调用方若跳过 wait_quiescent 直接析构，正在 recv 的线程仍持有
     * lease 里的 shared_ptr（对象不会析构），但它随后的 release_receive 会访问
     * 已析构的 RouteSession —— UB，责任在调用方。 */
    ~RouteSession() = default;

    RouteSession(const RouteSession&) = delete;
    RouteSession& operator=(const RouteSession&) = delete;

    /* stopping / rebuilding / 当前没有 route：返回空。成功则 receive_inflight_ + 1，
     * 返回的 shared_ptr 在 recv 期间保持对象存活。 */
    std::optional<ReceiveLease> acquire_receive();

    /* 每次成功 acquire_receive 必须配对一次，包括 recv 抛出前的所有出口。
     * noexcept：它常在 catch / 清理路径上被调用。 */
    void release_receive() noexcept;

    /* 按说明 §4 的顺序换成新 route。可在已有 route 为空时调用（首次建立）。
     * 返回后 acquire_receive 才能拿到新 generation。
     * create() 返回空或抛异常 ⇒ 视为失败：route_ 保持为空、rebuilding_ 复位、
     * generation_ 不变，不留下 rebuilding_ == true 的卡死态。 */
    void begin_rebuild(uint32_t new_generation,
                       const std::function<std::shared_ptr<ipc::route>()>& create);

    /* 拒绝新 lease，disconnect/wakeup 当前 route，唤醒卡在 wait_quiescent 的线程。
     * 幂等；可在已有 route 为空时调用。非终态（见文件头「接口语义裁定」）。 */
    void stop_and_wake() noexcept;

    /* 等到 receive_inflight_ == 0。stop_and_wake 之后调用。 */
    void wait_quiescent();

    std::shared_ptr<ipc::route> current_route() const;
    uint32_t generation() const;

private:
    mutable std::mutex mtx_;
    std::condition_variable cv_;
    std::shared_ptr<ipc::route> route_;
    uint32_t generation_{0};
    std::size_t receive_inflight_{0};
    bool stopping_{false};
    bool rebuilding_{false};
};

}   // namespace shm
}   // namespace dzIPC

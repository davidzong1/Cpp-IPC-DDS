/* 阶段 2 `RouteSession` 协议实现（说明 §2-§6）。协议不变式与调用方义务见头文件。
 *
 * 本文件落在 src/dzIPC/ 下 ⇒ 被现有 aux_source_directory(${...}/src/dzIPC) 收编,
 * 无需改 src/CMakeLists.txt（与说明 §1 一致）。
 */
#include "dzIPC/shm_route_session.h"

#include <cassert>
#include <utility>

namespace dzIPC {
namespace shm {

std::optional<RouteSession::ReceiveLease> RouteSession::acquire_receive()
{
    std::lock_guard<std::mutex> lock(mtx_);
    /* I3：stopping_ / rebuilding_ 一置位就拒绝新 lease。这两项都在同一把锁内
     * 置位，所以"拒绝"与"置位"之间不存在窗口：要么本函数先看到拒绝条件，要么
     * 置位方先看到本函数已把 receive_inflight_ 加一（于是它继续等归零）。 */
    if (stopping_ || rebuilding_ || !route_)
    {
        return std::nullopt;
    }
    ++receive_inflight_;
    return ReceiveLease{route_, generation_};
}

void RouteSession::release_receive() noexcept
{
    std::lock_guard<std::mutex> lock(mtx_);
    /* I4：漏配对 ⇒ wait_quiescent 永不返回；多一次 ⇒ size_t 下溢 ⇒ 计数变成
     * 天文数字 ⇒ 同样永不返回。两个方向都是**静默**挂死，所以下溢必须当场挡住
     * 而不是 wrap。 */
    if (receive_inflight_ == 0)
    {
        assert(false && "RouteSession::release_receive without a matching acquire_receive");
        return;
    }
    --receive_inflight_;
    /* 说明 §3：在锁内减计数并 notify_all —— 重建方在同一把锁里等归零，不能先
     * 要求它放锁。 */
    cv_.notify_all();
}

void RouteSession::begin_rebuild(uint32_t new_generation,
                                 const std::function<std::shared_ptr<ipc::route>()>& create)
{
    std::shared_ptr<ipc::route> old;
    {
        /* 1. 锁内：rebuilding_ = true，禁止新的 acquire_receive。
         * 2. 拷出旧 route 的 shared_ptr，放开锁。 */
        std::lock_guard<std::mutex> lock(mtx_);
        rebuilding_ = true;
        old = route_;
    }

    /* 3. 对旧 route disconnect()（内部会 quit_waiting，卡住的 recv(50) 返回）。
     *    ⚠️ 必须在锁外：recv 的唤醒路径不得回头再要 mtx_（说明 §6）。 */
    if (old)
    {
        old->disconnect();
    }

    {
        std::unique_lock<std::mutex> lock(mtx_);
        /* 4. 等待 receive_inflight_ == 0。release_receive 在同一把锁里减计数并
         *    notify，所以两者不会互相等待（说明 §6）。等待期间 rebuilding_ 已为
         *    true ⇒ 归零后不会被新的 acquire 重新占用。 */
        cv_.wait(lock, [this] { return receive_inflight_ == 0; });

        /* 5. 仍在锁内：旧 route release()，再 reset 指针。此刻无 lease、且已拒绝
         *    新 lease ⇒ release() 与 recv() 不并发（I2）。这是唯一允许 release 的
         *    位置。 */
        if (old)
        {
            old->release();
        }
        route_.reset();
    }

    /* 6. create 锁外：它可能阻塞（打开共享段），不得占着 mtx_ 挡住
     *    release_receive —— 若 create 阻塞而 mtx_ 被占，已持有 lease 的 recv
     *    线程会卡在 release_receive 上，begin_rebuild 自己的第 4 步也无法推进。
     *    失败（返回空或抛异常）按说明 §4 处理：保持 route_ 为空、rebuilding_
     *    复位，让调用方走「不置 handshake_completed、稍后重试」。 */
    std::shared_ptr<ipc::route> fresh;
    if (create)
    {
        try
        {
            fresh = create();
        }
        catch (...)
        {
            /* ⛔ 不向外抛：本类的调用方是收包线程/握手回调，抛出会带走调用栈上
             * 的生命周期管理（对照阶段 1 调度器对回调的约束）。失败等价于
             * create 返回空。 */
            fresh.reset();
        }
    }
    const bool created = (fresh != nullptr);

    {
        std::lock_guard<std::mutex> lock(mtx_);
        /* 7. generation_ = new_generation，route_ = 新对象，rebuilding_ = false，
         *    notify_all。 */
        if (created)
        {
            route_ = std::move(fresh);
            generation_ = new_generation;
            /* stopping_ 不是终态：一次**成功**的重建意味着"重新开始收包"。见头
             * 文件「接口语义裁定」—— 不复位它，说明 §4 表格的 add_peer 失败重试
             * 路径会让该话题收包静默停摆。 */
            stopping_ = false;
        }
        /* 失败时 route_ 保持第 5 步之后的空，generation_ 不假装新值生效。 */
        rebuilding_ = false;
        cv_.notify_all();
    }
}

void RouteSession::stop_and_wake() noexcept
{
    std::shared_ptr<ipc::route> cur;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        stopping_ = true;
        cur = route_;
        /* 唤醒卡在 wait_quiescent 的线程：谓词（receive_inflight_ == 0）本身没
         * 变，但要让它重新评估并继续（stopping_ 后不会有新 lease，所以归零是
         * 单调推进的）。 */
        cv_.notify_all();
    }
    /* disconnect 锁外（说明 §6）。内部 quit_waiting 会让阻塞中的 recv 立即返回
     * —— 这正是"不依赖 50ms 超时"的那一步（说明 §5 第 4 步）。
     * 幂等：重复调用只是再 disconnect 一次。 */
    if (cur)
    {
        cur->disconnect();
    }
}

void RouteSession::wait_quiescent()
{
    std::unique_lock<std::mutex> lock(mtx_);
    /* 只等计数归零，**不**等 !rebuilding_：
     *   · 重建方自己就在重建中途等归零，若这里也等 !rebuilding_，两个线程会
     *     各自等对方放锁（cv_.wait 会放锁，但谓词互斥）⇒ 活锁/超时；
     *   · stopping_ 同样不进谓词：stop_and_wake 只保证 recv 被唤醒，计数归零
     *     仍由 release_receive 推进。
     * 语义即说明 §2："等到 receive_inflight_ == 0"。 */
    cv_.wait(lock, [this] { return receive_inflight_ == 0; });
}

std::shared_ptr<ipc::route> RouteSession::current_route() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return route_;
}

uint32_t RouteSession::generation() const
{
    std::lock_guard<std::mutex> lock(mtx_);
    return generation_;
}

}   // namespace shm
}   // namespace dzIPC

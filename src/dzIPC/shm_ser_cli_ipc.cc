#include "dzIPC/shm_ser_cli_ipc.h"
#include <fcntl.h>
#include <sys/stat.h>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <iostream>
#include <memory>
#include <thread>
#include <typeinfo>
#include "dzIPC/common/local_pub_sub_registry.h"
#include "dzIPC/common/name_operator.h"
#include "dzIPC/common/nodelet_config.h"
#include "dzIPC/common/wire_accept.h"
/* 阶段 5 共享层：固定 route 收包 worker。**只消费，不复制**（本层 owner 是
 * ipc-transport）；模块侧不出现任何平台宏，句柄/平台差异全在它后面。 */
#include "dzIPC/threepools/recv_worker.h"
#include "libipc/utility/log.h"   // ipc::error：注销超时诊断（P1 §5）
#if defined(_WIN32)
#include <windows.h>   // ::GetCurrentProcessId —— fork 防死锁闸的 pid 来源
#else
#include <unistd.h>   // ::getpid —— 与 src/dzIPC/common/control_plane.cc 同一取法
#endif

/* ==================== 阶段 5：服务端请求接收接入固定 SHM RecvWorker ====================
 *
 * 分工（t3 / captain 裁决 D1、C7）：通用收包 worker（RecvRouteSource / RecvWorker /
 * RecvWorkerPool）由共享层交付并冻结（include/dzIPC/threepools/recv_worker.h）。
 * 本模块**只写 route 适配器 + 注册/回退/停机协议**：⛔ 不复制 worker、不新增等待
 * 原语、不出现平台宏、不 include 另一个待移植模块。
 *
 * 线程模型（worker 模式）：
 *   共享层 worker 线程 ── recv_once() ──> recv(0)（libipc 内部完成多片重组）
 *                                    └─ 唤醒伪影门（IsWakeupArtifact + 计数）
 *                                    └─ 入本 server 的**有界** FIFO（完整请求边界）
 *   本 server 处理线程（response_thread_）── 出队 ──> wire 分流（classify_received /
 *                                    AcceptWire）──> 用户 callback
 *                                    └─ response 序列化 + try_send / DZFlat 借样发送
 * ⛔ 用户 callback 与响应发送**不得**进收包 worker（需求 §1.2；recv_worker.h:214-220）。
 *
 * 为什么 FIFO 是"字节"而不是"ServiceData"：recv_once() 里做的事恰好是"取一次 +
 * 基础 wire 判断"，wire 分流（DZFlat 借样 vs TLV 物化）与 callback 在一起才有意义，
 * 它们都在处理路径上。于是 FIFO 里放的是 ipc::buffer（完整的请求字节）：
 *   · chunk 所有权：DZFlat 借样与 >64B 的 TLV 都由 libipc 的 chunk 池承担，buffer
 *     析构即回收 —— **chunk 始终在既有池配额之下**（需求 §1.2 第二句 / §7），
 *     本层没有、也不会引入第二套 chunk 记账；
 *   · ≤64B 的短 TLV 走 libipc 的 cache（堆副本），同样随 buffer 析构释放。
 * 转移点/归还点：recv(0) 返回即"字节所有权转移给 FIFO"，处理线程出队 → 分流后
 * 交给 Sample（借样）或物化进 ServiceData；错误出口（伪影/停机作废）一律让 buffer
 * 就地析构，归还发生在 buffer 析构处。
 *
 * 有界 FIFO 的**满载策略**（明确，不留隐式无界堆积；P1 §6.3 落地点）：容量
 * kMaxPendingRequests 条、kMaxPendingBytes 字节，两者任一达到即**不再收取**
 * （recv_once 返回 0，**不丢任何已到的请求**），并置 backpressured=true 让
 * has_pending() 返回 true ⇒ 共享 worker 的 level-triggered 重检会把该 route 放回
 * deferred FIFO，下一轮预算继续尝试。
 * 入库前按「pending_bytes + request_bytes <= kMaxPendingBytes」判定（§6.3 规则 1）；
 * 协议无单条尺寸上限（见 SerState 的说明），故补一条显式特例：队列为空时允许放入
 * **一条**超限请求（否则会静默丢弃已经 try_recv 下来的请求）⇒ 严格上界 =
 * kMaxPendingBytes + max(单条实测尺寸)；两种情况都有计数可观测。
 * 为什么不用 t5(socket) 的"满载丢最旧"：SHM 侧有 recv_wait_token::sequence() 这个
 * 廉价的重检字，返回 0 不会让请求搁死在通道里（has_pending() 保证同 route 下一轮
 * 仍被选中），因此能选**零丢失**的背压；socket 侧没有这个字，只能丢最旧。
 *
 * 线程数与收益（如实登记，交 t11）：callback 与响应发送必须在处理路径上，而本波
 * 共享层**不提供** ProcessingPool（t2 明确回复"不在 t2 交付面"）⇒ 采用每 server 一条
 * **处理**线程（消费本 server 的 FIFO）。因此：
 *   · per-server 线程数 = 2（处理线程 + ser_handshake 控制面线程），与改造前
 *     （response 线程 + ser_handshake）**相同**；
 *   · 变化的是"等待/组包"从 per-server 线程移到了进程级固定池（空闲时池线程归还
 *     OS），per-server 处理线程不再阻塞在 recv(50) 上。
 *   ⇒ **"收包线程数不随 server 数线性增长"这一收益本波未达成**（可观测的线程数
 *     不降)，需 shared 层 ProcessingPool 或测试断言侧决策（见 §6 已知偏差）。
 *
 * generation：服务端**不做** route 重建（ipc_r_ptr_ 只在 InitChannel 赋值一次，
 * 此后再不替换），因此 recv_once() 弹出的请求不会因任何 generation 变化被丢弃；
 * receive_generation_ 只用于"每次接入新数据面 +1"的诊断与固定归属日志。控制面段
 * 的 generation 由 TopicControlPlane 单调推进（与数据面无关）。
 */

namespace dzIPC {
namespace shm {

/* ==================== service shared state ====================
 *
 * worker 与处理路径的共同事实来源。收包 worker **只**通过 SerRequestRoute 适配器
 * 接触本结构，因此 worker 不持裸 shm_ser_ipc 指针（注销后不再回调已析构对象的前提）。 */
struct SerState
{
    /* 稳定 route key：<service_prefix>_ser_r，生命周期内不变（契约 §4.1 第 1 条）。
     * 变了就等于换了一条 route，必须 remove_route + add_route。 */
    std::string route_name;
    std::uint32_t domain_id{0};
    /* 诊断用代际（服务端无 route 重建；见文件头 generation 说明）。 */
    std::uint32_t generation{0};

    /* 请求数据通道（ipc::server, unicast）。仅在 InitChannel 里赋值一次，此后再不
     * 替换；注销路径在 remove_route + join 之后才 reset ⇒ read_wait_token() 可以
     * **无锁读**（契约 §4.1 要求它 noexcept 且可从任意线程调用）。ipc_w_ptr_（发送
     * 端点）与控制面段不入 worker。 */
    std::shared_ptr<ipc::server> req_ch;

    /* 单 route 单消费者（契约 §4.6）：宿主用 atomic<RecvOwner> 三行实现。 */
    std::atomic<threepools::RecvOwner> owner{threepools::RecvOwner::none};

    /* route 生命周期：stopping 拒绝新的 recv_once；receive_inflight 供 wait_quiescent。 */
    std::atomic<bool> stopping{false};
    std::atomic<std::size_t> receive_inflight{0};
    std::mutex quiesce_mtx;
    std::condition_variable quiesce_cv;

    /* ---- 有界请求 FIFO：worker 入队（完整请求字节）→ 处理线程出队 ----
     *
     * P1（阻塞解阻方案 §6.3 规则 1）：**入库前判定**，保证稳态满足
     *   pending_bytes + request_bytes <= kMaxPendingBytes
     * 超限且队列非空 ⇒ 不收取（背压，请求留在通道里，零丢失）。
     *
     * ⚠️ 严格上界（不是「8 MiB 是绝对内存上界」）：协议**没有**单条消息尺寸上限
     * （全仓 libipc 无 max message size 常量；recv_once 一旦 try_recv 成功就已经把整条
     * 请求收下，**无法退回通道**）。因此补一条显式特例：队列**为空**时允许放入
     * **一条**超限请求（否则会静默丢弃已收到的请求）。
     * ⇒ 内存持有严格上界 = kMaxPendingBytes + max(单条请求实测尺寸)，
     *   条数上界 = kMaxPendingRequests；两种情况都有计数可观测：
     *   fifo_peak_bytes / fifo_peak_requests / oversize_admissions / max_request_bytes。
     * 若 §6.2 对照实验证明该窗口造成 DZFlat 回退上升，则调小 cap 或改为协议层拒绝。 */
    static constexpr std::size_t kMaxPendingRequests = 64;
    static constexpr std::size_t kMaxPendingBytes = 8u << 20;   // 8 MiB
    std::mutex pending_mtx;
    std::condition_variable pending_cv;
    std::deque<ipc::buffer> pending;
    /* pending 的**无锁读口**（has_pending 是 noexcept，不允许取锁）。计数在
     * pending_mtx 内更新，读取方只做提示用判断。 */
    std::atomic<std::size_t> pending_count{0};
    std::atomic<std::size_t> pending_bytes{0};
    /* 上一次 recv_once 因满载而**未收取**：has_pending() 据此让 worker 重检。 */
    std::atomic<bool> backpressured{false};

    /* ---- 诊断计数（可观测性是本次交付物的一部分）---- */
    std::atomic<std::uint64_t> requests_received{0};
    std::atomic<std::uint64_t> requests_dropped{0};      // 停机时作废的队列残余
    std::atomic<std::uint64_t> recv_errors{0};           // recv_once 内被吞掉的异常
    std::atomic<std::uint64_t> wakeup_artifacts{0};      // 被伪影门挡下的叫醒伪影
    std::atomic<std::uint64_t> callback_count{0};
    std::atomic<std::uint64_t> callback_ns_total{0};     // callback 耗时（时长不可控 ⇒ 必须可观测）
    std::atomic<std::uint64_t> callback_ns_max{0};
    std::atomic<std::uint64_t> callback_exceptions{0};   // callback 抛出（隔离：不让处理线程死）
    std::atomic<std::uint64_t> responses_sent{0};
    std::atomic<std::uint64_t> response_send_failures{0};
    /* ---- P1（§6.2/§6.3）FIFO 容量可观测性：为容量对照实验提供原始读数 ---- */
    std::atomic<std::uint64_t> fifo_peak_requests{0};      ///< 队列条数峰值
    std::atomic<std::uint64_t> fifo_peak_bytes{0};          ///< 队列字节峰值
    std::atomic<std::uint64_t> oversize_admissions{0};      ///< 空队列放行的超限请求条数
    std::atomic<std::uint64_t> max_request_bytes{0};        ///< 见过的最大单条请求字节
    std::atomic<std::uint64_t> backpressure_hits{0};        ///< 因满载未收取的次数
};

/* ---- P1（阻塞解阻方案 §4.1）：owner CAS 放在 shared state 上 ----
 * worker 注册走的 route->try_claim_recv 与兼容线程走的本函数操作的是**同一个**
 * `SerState::owner` 原子量，因此两路不可能同时成功；回退路径即使没有成功注册 worker
 * route（req_route_ 为空）也照样能 claim/release。 */
bool ser_state_try_claim_recv(SerState& state, threepools::RecvOwner who) noexcept
{
    if (who == threepools::RecvOwner::none)
    {
        return false;   // 契约 §4.6：who == none 一律拒绝
    }
    threepools::RecvOwner expected = threepools::RecvOwner::none;
    return state.owner.compare_exchange_strong(expected, who, std::memory_order_acq_rel);
}

void ser_state_release_recv(SerState& state) noexcept
{
    auto expected = state.owner.load(std::memory_order_acquire);
    while (expected != threepools::RecvOwner::none)
    {
        if (state.owner.compare_exchange_weak(expected, threepools::RecvOwner::none,
                                              std::memory_order_acq_rel))
        {
            return;   // 重复 release 是安全的（幂等）
        }
    }
}

const char* ser_owner_name(threepools::RecvOwner who) noexcept
{
    switch (who)
    {
    case threepools::RecvOwner::none:
        return "none";
    case threepools::RecvOwner::compat_thread:
        return "compat_thread";
    case threepools::RecvOwner::worker:
        return "worker";
    }
    return "?";
}

/* RAII：兼容接收线程无论正常退出还是异常退出都归还 owner（§4.2「线程退出用 RAII guard」）。 */
struct SerCompatClaimGuard
{
    SerState* state{nullptr};
    ~SerCompatClaimGuard()
    {
        if (state != nullptr)
        {
            ser_state_release_recv(*state);
        }
    }
};

/* SerRequestRoute：shm_ser_ipc 服务端的**请求数据通道**在共享层里的适配器。
 * recv_once() 只做"取一次到完整请求边界 + 基础 wire 判断 + 入有界 FIFO"，
 * ⛔ 不含用户 callback、不含响应发送（裁决 D1）。 */
class SerRequestRoute final : public threepools::RecvRouteSource
{
public:
    explicit SerRequestRoute(std::shared_ptr<SerState> state)
        : state_(std::move(state))
    {}

    const char* route_name() const noexcept override { return state_->route_name.c_str(); }
    std::uint32_t domain_id() const noexcept override { return state_->domain_id; }
    ipc::recv_wait_token read_wait_token() const noexcept override;
    std::size_t recv_once() override;
    bool has_pending() const noexcept override;
    /* P1：三方法一律转调 state 级 CAS（只有一份 owner 状态，§4.1 末段）。 */
    threepools::RecvOwner recv_owner() const noexcept override
    {
        return state_->owner.load(std::memory_order_acquire);
    }
    bool try_claim_recv(threepools::RecvOwner who) noexcept override
    {
        return ser_state_try_claim_recv(*state_, who);
    }
    /* 归还收包独占（注销第 6 步，契约 §4.4）。worker 路径由 remove_route 内部调用，
     * 兼容线程路径在 response_thread_func 退出前显式调用；两种 owner 都必须能回到
     * none，漏一次就会让重新 add_route 永久返回 busy（静默丢包）。 */
    void release_recv() noexcept override
    {
        ser_state_release_recv(*state_);
    }
    /* 注销第 3 步：置 stopping（拒绝新的 recv_once）+ 唤醒阻塞中的 recv。
     * ipc::server::disconnect() 内部是 que_.disconnect() + quit_waiting()（唤醒
     * rd_waiter_），正是"拒绝新 lease + 唤醒"的语义；幂等。 */
    void stop_and_wake() noexcept override;
    /* 注销第 5 步：等 in-flight 记账归零（有界；超时只打诊断，不阻塞注销）。 */
    void wait_quiescent() noexcept override;

private:
    std::shared_ptr<SerState> state_;
};

/* 注销时"等在途 recv_once 归零"的上界（方案 §4 第 6 步 / §5）。 */
constexpr int64_t kSerQuiesceTimeoutMs = 2000;

/* ================= SerRequestRoute：5 个方法实现 =================
 * 签名以 include/dzIPC/threepools/recv_worker.h:200-237 为准（⛔ 不照方案文档清单）。 */

ipc::recv_wait_token SerRequestRoute::read_wait_token() const noexcept
{
    /* req_ch 只在 InitChannel 里赋值一次、注销路径在 remove_route + join 之后才 reset，
     * 因此这里可以**无锁读**（契约 §4.1 要求它 noexcept 且可从任意线程调用）。
     * 无效 token ⇒ worker 注册返回 invalid_token ⇒ 模块显式回退兼容接收线程。 */
    return state_->req_ch ? state_->req_ch->read_wait_token() : ipc::recv_wait_token{};
}

std::size_t SerRequestRoute::recv_once()
{
    /* ⛔ 裁决 D1：这里**只**做 收取（libipc 已完成多片重组，返回的就是完整请求）
     * + 基础 wire 判断（唤醒伪影门）+ 投递进本 server 的**有界** FIFO。
     * 不含用户 callback、不含响应序列化/发送 —— 它们在处理线程的 process_request() 上。 */
    if (state_->stopping.load(std::memory_order_acquire))
    {
        return 0;
    }
    /* 有界 FIFO 满载 ⇒ **不收取**（零丢失背压，而不是丢弃）：返回 0 并置 backpressured，
     * has_pending() 据此返回 true ⇒ 共享 worker 的 level-triggered 重检会把本 route 放回
     * deferred FIFO，下一轮预算继续尝试 —— 请求留在通道里，一个都不丢。 */
    const std::size_t pending_bytes_now = state_->pending_bytes.load(std::memory_order_acquire);
    const std::size_t pending_count_now = state_->pending_count.load(std::memory_order_acquire);
    if (pending_count_now >= SerState::kMaxPendingRequests
        || pending_bytes_now >= SerState::kMaxPendingBytes)
    {
        state_->backpressured.store(true, std::memory_order_release);
        state_->backpressure_hits.fetch_add(1, std::memory_order_relaxed);
        return 0;
    }

    state_->receive_inflight.fetch_add(1, std::memory_order_acq_rel);
    std::size_t bytes = 0;
    try
    {
        /* 真·非阻塞：try_recv()（契约 §4.1 的"非阻塞或短超时"）。
         * 多片重组与 chunk 池所有权全在 libipc 内部 —— 本层不引入第二套 chunk 记账
         * （需求 §1.2 第二句 / §7：不得让 SHM chunk 脱离现有 adopt 配额控制）。 */
        ipc::buffer raw = state_->req_ch->try_recv();
        if (!raw.empty())
        {
            /* 唤醒伪影门（基准对齐笔记 §1.5）：disconnect()/唤醒会让 recv 返回
             * size()==data_length 的**整段全零** buffer 且 empty()==false；不挡掉就会被
             * 当成真请求送进 callback（msg_id==0 的话题尤其危险）。判据必须整段全零。 */
            if (dzIPC::IsWakeupArtifact(raw))
            {
                dzIPC::NoteWakeupArtifact();
                state_->wakeup_artifacts.fetch_add(1, std::memory_order_relaxed);
            }
            else
            {
                bytes = raw.size();
                /* P1（§6.3 规则 1+2）：入库前判定容量。稳态保证
                 *   pending_bytes + bytes <= kMaxPendingBytes；
                 * 超限时只允许「队列为空 ⇒ 放行这一条」的显式特例（因为请求已经被
                 * try_recv 收下、无法退回通道），并计入 oversize_admissions。
                 * 队列非空且会超限 ⇒ 不该发生（入口已挡 count/bytes 上限），保守起见
                 * 仍按背压处理：不收取、不丢弃（buffer 就地析构，通道里的数据不受影响）。 */
                bool accepted = false;
                bool oversize = false;
                {
                    std::lock_guard<std::mutex> lock(state_->pending_mtx);
                    const std::size_t cur_bytes = state_->pending_bytes.load(std::memory_order_relaxed);
                    if (state_->pending.empty() || cur_bytes + bytes <= SerState::kMaxPendingBytes)
                    {
                        oversize = (cur_bytes + bytes > SerState::kMaxPendingBytes);
                        state_->pending.push_back(std::move(raw));   // 完整请求字节：所有权转移给 FIFO
                        state_->pending_count.fetch_add(1, std::memory_order_relaxed);
                        state_->pending_bytes.fetch_add(bytes, std::memory_order_relaxed);
                        state_->backpressured.store(false, std::memory_order_release);
                        accepted = true;
                        /* 峰值可观测（无锁 CAS 更新，供 §6.2 对照实验取原始读数）。 */
                        const std::size_t cnt = state_->pending_count.load(std::memory_order_relaxed);
                        const std::size_t byt = state_->pending_bytes.load(std::memory_order_relaxed);
                        std::uint64_t prev = state_->fifo_peak_requests.load(std::memory_order_relaxed);
                        while (cnt > prev
                               && !state_->fifo_peak_requests.compare_exchange_weak(prev, cnt,
                                                                                    std::memory_order_relaxed))
                        {
                        }
                        prev = state_->fifo_peak_bytes.load(std::memory_order_relaxed);
                        while (byt > prev
                               && !state_->fifo_peak_bytes.compare_exchange_weak(prev, byt,
                                                                                 std::memory_order_relaxed))
                        {
                        }
                    }
                }
                if (accepted)
                {
                    std::uint64_t prev = state_->max_request_bytes.load(std::memory_order_relaxed);
                    while (bytes > prev
                           && !state_->max_request_bytes.compare_exchange_weak(prev, bytes,
                                                                               std::memory_order_relaxed))
                    {
                    }
                    if (oversize) state_->oversize_admissions.fetch_add(1, std::memory_order_relaxed);
                    state_->pending_cv.notify_one();
                }
                else
                {
                    state_->backpressured.store(true, std::memory_order_release);
                    state_->backpressure_hits.fetch_add(1, std::memory_order_relaxed);
                }
            }
        }
    }
    catch (...)
    {
        /* 绝不把异常抛给共享 worker 线程（它会 std::terminate）。 */
        state_->recv_errors.fetch_add(1, std::memory_order_relaxed);
    }
    state_->receive_inflight.fetch_sub(1, std::memory_order_acq_rel);
    state_->quiesce_cv.notify_all();

    if (bytes == 0)
    {
        return 0;   // 无数据 / 伪影 / 异常
    }
    state_->requests_received.fetch_add(1, std::memory_order_relaxed);
    return bytes;   // 契约：非 0 = 取到一次完整请求（字节数）
}

bool SerRequestRoute::has_pending() const noexcept
{
    /* level-triggered 重检：满载未收取（零丢失背压）或还有待处理请求时保持 true，
     * 让共享 worker 下一轮预算继续选中本 route。 */
    return state_->backpressured.load(std::memory_order_acquire)
           || state_->pending_count.load(std::memory_order_acquire) > 0;
}

void SerRequestRoute::stop_and_wake() noexcept
{
    /* 注销第 3 步：置 stopping（拒绝新的 recv_once）+ 唤醒阻塞中的 recv。
     * ipc::server::disconnect() 内部是 que_.disconnect() + quit_waiting()（唤醒 rd_waiter_），
     * 正是"拒绝新 lease + 唤醒在途"的语义。 */
    state_->stopping.store(true, std::memory_order_release);
    if (state_->req_ch)
    {
        state_->req_ch->disconnect();
    }
    state_->pending_cv.notify_all();   // 让处理线程也立刻看到停机
}

void SerRequestRoute::wait_quiescent() noexcept
{
    /* 注销第 5 步：等本模块 in-flight 记账归零（有界；超时只打诊断、不阻塞注销）。
     * P1（§5）：检查 wait_for 返回值；超时**先出锁取快照再打印**（不在持锁时写日志）。 */
    bool timed_out = false;
    std::size_t inflight = 0;
    {
        std::unique_lock<std::mutex> lock(state_->quiesce_mtx);
        state_->quiesce_cv.wait_for(lock, std::chrono::milliseconds{kSerQuiesceTimeoutMs}, [this] {
            return state_->receive_inflight.load(std::memory_order_acquire) == 0;
        });
        inflight = state_->receive_inflight.load(std::memory_order_acquire);
        timed_out = inflight != 0;
    }
    if (timed_out)
    {
        ipc::error("[shm_ser] wait_quiescent timeout after %lld ms: route='%s' generation=%u in_flight=%llu; "
                   "continuing bounded teardown\n",
                   static_cast<long long>(kSerQuiesceTimeoutMs), state_->route_name.c_str(),
                   state_->generation, static_cast<unsigned long long>(inflight));
    }
}


namespace {

/* DZFlat 借样发送 (docs/dzflat_shm.md Step 2, srv 接入见 known_issues 第 6 条)。
 *
 * 返回 false 表示"本次不走 DZFlat", 调用方须回退既有整包序列化。回退是常态:
 * 开关未开 / 类型不支持 / 对端未连 / chunk 池耗尽都会走到那里。
 *
 * ser/cli 用的是 ipc::server(single-single-unicast), 与 pub/sub 的 route 不同:
 * 其 recv 侧 recycle 无条件归还(sub_rc 恒 true), chunk 的 conns 不承载持有信息 ——
 * 对借样没有影响, 因为一条消息只有一个接收方, 它的 buff_t 析构即归还。 */
bool try_send_dzflat(const std::shared_ptr<ipc::server>& ch, const std::shared_ptr<IpcMsgBase>& msg)
{
    /* 每次调用都记一笔: 回退是静默的, 没有计数就分不清"收益生效了"和"一直在回退"
     * (pub/sub 侧同理, 见 nodelet_config.h 的说明)。 */
    struct Note
    {
        bool used = false;
        ~Note() { dzIPC::detail::NoteDzFlatPublish(used); }
    } note;

    if (!dzIPC::IsDzFlatEnabled() || !ch || !msg || !msg->dzflat_supported())
    {
        return false;
    }
    if (ch->recv_count() == 0)
    {
        return false;   // 无接收方: chunk 借不到也无人回收
    }
    const std::uint32_t need = msg->dzflat_size();
    if (need == 0)
    {
        return false;
    }
    auto lo = ch->loan_topic(need);
    if (!lo.valid())
    {
        return false;   // 池耗尽 —— 背压, 不是错误
    }
    if (!msg->dzflat_write(lo.data, static_cast<std::uint32_t>(lo.size)))
    {
        ch->discard_loan(lo);
        return false;
    }
    note.used = ch->publish_loan(lo, 0);
    return note.used;
}

/* 双 wire 接收: 判别 + 校验 + 拒收计数, 与 pub/sub 共用一份实现
 * (见 dzIPC/common/wire_accept.h)。返回 false 表示这条应当丢弃。
 *
 * ser/cli 的期待 msg_id 就取 target 自己的 —— 与 pub/sub 不同, 这里的目标对象不会被
 * swap() 移走, 它始终持有本请求/响应类型的 msg_id。 */
bool accept_wire(const ipc::buffer& raw, const std::shared_ptr<IpcMsgBase>& target)
{
    return dzIPC::AcceptWire(raw, target->msg_id(), *target);
}

/* 分流一个收到的段, 让 DZFlat request/response 走零拷贝借样(仿 pub/sub 视图路径)。
 *
 *   1  = 借样成功: typed DZFlat 且 id/schema 全对上 —— 段已 move 进 sample_out;
 *   0  = 应走物化 accept_wire: TLV / schema-less(GenericMessage)话题 / 手写类型;
 *  -1  = 识别为 typed DZFlat 但校验失败(id 或 schema 不符) —— 已计数, 调用方丢弃。
 *
 * 计数只在 1 / -1 路径记一次; 0 路径留给 accept_wire 自己记, 避免重复。 */
int classify_received(ipc::buffer& raw, const std::shared_ptr<IpcMsgBase>& tpl,
                      std::uint32_t tpl_msg_id, std::shared_ptr<dzIPC::Sample>& sample_out)
{
    using E = dzIPC::detail::DzFlatRxEvent;
    if (!dzflat::looks_like_dzflat(raw.data(), raw.size()))
    {
        return 0;
    }
    if (!tpl || !tpl->dzflat_supported())
    {
        return 0;   // schema-less / 手写类型: 物化(GenericMessage 拷段字节)
    }
    std::uint32_t seg_id = 0;
    if (!IpcMsgBase::dzflat_peek_msg_id(raw.data(), raw.size(), seg_id) || seg_id != tpl_msg_id)
    {
        dzIPC::detail::NoteDzFlatRx(E::kDzFlatIdSkipped);
        return -1;
    }
    dzflat::SegHeader h{};
    std::memcpy(&h, raw.data(), sizeof(h));
    if (h.schema_hash != tpl->dzflat_schema_hash())
    {
        dzIPC::detail::NoteDzFlatRx(E::kDzFlatSchemaDrop);
        return -1;
    }
    dzIPC::detail::NoteDzFlatRx(E::kDzFlatAccepted);
    sample_out =
        std::make_shared<dzIPC::Sample>(std::move(raw), seg_id, h.schema_hash);
    return 1;
}

}   // namespace
using namespace ipc;
using dzIPC::control_plane_shm::TopicState;

namespace {

/* 段名规则的唯一出处在 dzIPC/common/name_operator.h, 这里只是转调。 */
std::string service_prefix_for(const std::string& topic_name, size_t domain_id)
{
    return shm_service_prefix(topic_name, domain_id);
}

std::string service_control_name_for(const std::string& topic_name, size_t domain_id)
{
    /* 段名的实现只有一份, 在 dzIPC::shm 里(占用判定也要用同一个名字)。 */
    return dzIPC::shm::ser_service_control_name(topic_name, domain_id);
}

// Internal envelope: carries a fast-path request together with the client's
// reply queue pointer.  This ensures the server routes the response to the
// exact client that issued the request — never broadcast to all registered
// queues under the same ChannelKey.
//
// Never serialized; only used within the intra-process fast path.
class FastPathRequestEnvelope : public IpcMsgBase
{
public:
    FastPathRequestEnvelope(std::shared_ptr<IpcMsgBase> request,
                            std::shared_ptr<CircularQueue<IpcMsgBase>> reply_queue)
        : request_(std::move(request)), reply_queue_(std::move(reply_queue))
    {}

    std::shared_ptr<IpcMsgBase>& request() { return request_; }
    std::shared_ptr<CircularQueue<IpcMsgBase>>& reply_queue() { return reply_queue_; }

    ipc::buffer serialize() override { return {}; }
    void deserialize(const ipc::buffer&) override {}
    FastPathRequestEnvelope* clone() const override
    {
        return new FastPathRequestEnvelope(
            std::shared_ptr<IpcMsgBase>(request_->clone()), reply_queue_);
    }

private:
    std::shared_ptr<IpcMsgBase> request_;
    std::shared_ptr<CircularQueue<IpcMsgBase>> reply_queue_;
};

}   // namespace

std::string ser_service_control_name(const std::string& topic_name, size_t domain_id)
{
    /* "_ser_control2": 与 pub-sub 侧同理, TopicControl 结构体已变大,
     * 必须换名以避免在旧的小共享内存段上越界映射。 */
    return shm_service_prefix(topic_name, domain_id) + "_ser_control2";
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

shm_ser_ipc::shm_ser_ipc(const std::string& topic_name, const std::shared_ptr<ServiceData>& msg,
                         std::function<void(std::shared_ptr<ServiceData>&)> callback, size_t domain_id, bool verbose,
                         bool enable_thread_qos, int cpu_id, int thread_priority)
    : ser_ipc_base(topic_name, msg, callback, domain_id, verbose)
    , topic_name_(topic_name)
    , callback_(std::move(callback))
    , domain_id_(domain_id)
    , verbose_(verbose)
    , thread_options_(dzIPC::ThreadDispatch::make_realtime_options(enable_thread_qos, cpu_id, thread_priority))
{
    message_.reset(msg->clone());
    fp_queue_ = std::make_shared<CircularQueue<IpcMsgBase>>(16);
    dzIPC::ThreadDispatch::apply_current_thread_options(thread_options_, verbose_, topic_name_ + "_SerOwnerThread");
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void shm_ser_ipc::reset_message(const std::shared_ptr<ServiceData>& msg)
{
    if (!msg)
    {
        return;
    }
    std::shared_ptr<ServiceData> new_msg;
    new_msg.reset(msg->clone());
    const uint32_t new_msg_id = new_msg->request()->msg_id();
    std::lock_guard<std::mutex> lock(message_mtx_);
    message_ = std::move(new_msg);

    // Re-register under new msg_id if fast-path is active and msg_id changed.
    if (fp_registered_ && fp_msg_id_ != new_msg_id)
    {
        auto& reg = LocalPubSubRegistry::instance();
        ChannelKey old_key{topic_name_, domain_id_, fp_msg_id_, ChannelKind::ShmService};
        ChannelKey new_key{topic_name_, domain_id_, new_msg_id, ChannelKind::ShmService};
        reg.unregister_subscriber(old_key, fp_queue_);
        reg.register_subscriber(new_key, fp_queue_);
        fp_msg_id_ = new_msg_id;
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void shm_ser_ipc::reset_callback(std::function<void(std::shared_ptr<ServiceData>&)> callback)
{
    std::lock_guard<std::mutex> lock(callback_mtx_);
    callback_ = std::move(callback);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

shm_ser_ipc::~shm_ser_ipc()
{
    // Deregister fast-path queue before stopping threads.
    if (fp_registered_)
    {
        ChannelKey key{topic_name_, domain_id_, fp_msg_id_, ChannelKind::ShmService};
        LocalPubSubRegistry::instance().unregister_subscriber(key, fp_queue_);
        fp_registered_ = false;
    }

    /* 方案 §4 的 8 步（先后不可颠倒）：
     *  1) 上面的 LocalPubSubRegistry 注销（fp_queue_）
     *  2/3) 数据面注销：remove_route（摘 wait 项 + 唤醒 + disconnect + 等 worker in-flight
     *      + wait_quiescent + release_recv）+ 处理线程 join；控制面 ser_handshake 随后 join
     *  4) 标记 stopping / 5) 唤醒在途 recv —— 都在 stop_data_plane() 内
     *  6) 等 worker 不再使用 route（remove_route 第 4/5 步）
     *  7) release/reset route（remove_route 第 6 步；兼容路径显式 release_recv）
     *  8) 释放队列与通道 —— fd/句柄在 wait 项删除且 in-flight 清零**之后**才关。
     * 注销返回后不得再回调已析构对象：worker 侧由 remove_route 同步保证，处理线程由 join 保证。 */
    running = false;
    stop_data_plane();
    if (handshake_thread_ != nullptr)
    {
        if (handshake_thread_->joinable())
        {
            handshake_thread_->join();
        }
        delete handshake_thread_;
    }
    if (ipc_r_ptr_ && ipc_r_ptr_->valid())
    {
        ipc_r_ptr_->clear();
    }
    if (ipc_w_ptr_ && ipc_w_ptr_->valid())
    {
        ipc_w_ptr_->clear();
    }
    exit_flag.store(true, std::memory_order_release);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void shm_ser_ipc::InitChannel(std::string extra_info)
{
    std::string r_name, w_name;
    r_name = service_prefix_for(topic_name_, domain_id_) + "_ser_r";
    w_name = service_prefix_for(topic_name_, domain_id_) + "_ser_w";
    if (!control_plane_.open(service_control_name_for(topic_name_, domain_id_)))
    {
        throw std::runtime_error("failed to open service control plane");
    }
    control_plane_.begin_rebuild();
    ipc::server::clear_storage(r_name.c_str());
    ipc::server::clear_storage(w_name.c_str());
    ipc_r_ptr_ = std::make_shared<ipc::server>(r_name.c_str(), ipc::receiver, verbose_);
    ipc_w_ptr_ = std::make_shared<ipc::server>(w_name.c_str(), ipc::sender, verbose_);
    control_plane_.set_ready();
    handshake_thread_ = new std::thread(&shm_ser_ipc::ser_handshake, this);
    std::shared_ptr<ServiceData> message_template;
    {
        std::lock_guard<std::mutex> lock(message_mtx_);
        message_template = message_;
    }
    std::string request_type_name =
        (message_template && message_template->request())
            ? dzIPC::info_pool::demangle(typeid(*message_template->request()).name())
            : std::string{};
    request_type_name = extract_last_segment(request_type_name);
    pool_reg_.rebind({dzIPC::info_pool::EntryKind::ShmServer, topic_name_, request_type_name, "shm",
                      static_cast<int32_t>(domain_id_), extra_info});
    std::cerr << "\033[32m[" << topic_name_ << "_SerInfo] Server channel created for topic: " << topic_name_
              << "\033[0m" << std::endl;

    // Register for intra-process fast-path delivery BEFORE starting
    // response_thread so the thread can process fast-path requests
    // immediately (no race on fp_registered_).
    /* 数据面接入：worker 优先；任何非 ok 状态 / nodelet(C7) / fork 闸 / 后端不可用
     * ⇒ start_data_plane() 已打显式原因，这里退回兼容接收线程（⛔ 不忙轮询降级）。 */
    const bool worker_mode = start_data_plane();

    /* C7：nodelet 启用时保留兼容接收线程；worker 模式下**不注册** fp_queue
     * （注册了会把进程内请求投进没有消费者的队列而静默挂起）。 */
    if (!worker_mode && message_template)
    {
        fp_msg_id_ = message_template->request()->msg_id();
        ChannelKey key{topic_name_, domain_id_, fp_msg_id_, ChannelKind::ShmService};
        LocalPubSubRegistry::instance().register_subscriber(key, fp_queue_);
        fp_registered_ = true;
    }

    /* 处理线程（形态 b）：worker 模式消费本 server 的 FIFO（收包在共享 worker 上）；
     * 兼容模式就是原来的 response_thread_func。两者共用 process_request/handle_fast_path。 */
    response_thread_ = new std::thread(worker_mode ? &shm_ser_ipc::process_thread_func
                                                   : &shm_ser_ipc::response_thread_func,
                                       this);
    dzIPC::ThreadDispatch::apply_thread_options(response_thread_, thread_options_, verbose_,
                                                topic_name_ + "_SerResponseThread");
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

void shm_ser_ipc::ser_handshake()
{
    bool had_client = false;
    while (running.load(std::memory_order_acquire))
    {
        control_plane_.heartbeat();
        /* 死连接回收。
         *
         * ser-cli 的发送走 try_send(), 不会掉进 force_push, 所以它从来没有
         * "慢读者被误踢"的问题; 但它也从来没有回收死连接的机制 —— 崩溃的
         * 客户端在响应通道 (_ser_w) 的连接位图里留下的 bit 会让服务端的
         * try_send() 永远失败(重试 10 次后放弃), 这个方向就静默地发不出去了。
         *
         * 客户端在 cli_handshake() 里登记自己在响应通道上的 cc_id 并持续心跳,
         * 这里按心跳超时判死。超时同 pub-sub 取 2s = 200 个心跳周期。 */
        constexpr int64_t kPeerDeadTimeoutNs = 2'000'000'000LL;
        const uint32_t stale = control_plane_.collect_stale_peers(kPeerDeadTimeoutNs);
        if (stale != 0 && ipc_w_ptr_ && ipc_w_ptr_->valid())
        {
            ipc_w_ptr_->disconnect_receivers(stale);
            if (verbose_)
            {
                std::cerr << "\033[33m[" << topic_name_
                          << "_SerInfo] reaped dead client connection(s) on the response channel, cc_ids = 0x"
                          << std::hex << stale << std::dec << "\033[0m" << std::endl;
            }
        }
        const bool has_client = control_plane_.peer_count() > 0;
        handshake_completed_.store(has_client, std::memory_order_release);
        if (has_client && !had_client && verbose_)
        {
            std::cerr << "\033[32m[" << topic_name_ << "_SerInfo] Client connected to server: " << topic_name_
                      << "\033[0m" << std::endl;
        }
        if (!has_client && had_client && verbose_)
        {
            std::cerr << "\033[32m[" << topic_name_ << "_SerInfo] Client disconnected from server: " << topic_name_
                      << "\033[0m" << std::endl;
        }
        had_client = has_client;
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    handshake_completed_.store(false, std::memory_order_release);
    control_plane_.set_stopping();
    if (verbose_)
    {
        std::cerr << "\033[32m[" << topic_name_
                  << "_SerInfo] Server exiting, marking control plane stopping: " << topic_name_ << "\033[0m"
                  << std::endl;
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void shm_ser_ipc::response_thread_func()
{
    /* 兼容后端（nodelet / fork 闸 / 后端不可用 / 注册失败）：单 route 单消费者 —— 启动前
     * claim(compat_thread)，退出前**显式** release_recv（契约 §4.6 / 注销末步；漏了会让重新
     * add_route 永久返回 busy、静默丢包）。 */
    const std::shared_ptr<SerState> state = current_ser_state();
    /* P1（§4.2）：不再以 req_route_ != nullptr 作为 claim 条件 —— 回退路径（nodelet /
     * fork 闸 / 后端不可用 / 注册失败）根本没有成功注册的 route，但 owner 状态在
     * SerState 上，兼容线程照样必须 claim。worker 注册与本函数操作同一个原子量。 */
    if (!state)
    {
        ipc::error("[shm_ser] compat receive thread: state missing for topic '%s'; exiting\n",
                   topic_name_.c_str());
        return;
    }
    if (!ser_state_try_claim_recv(*state, threepools::RecvOwner::compat_thread))
    {
        ipc::error("[shm_ser] compat receive thread: claim failed for route '%s' (owner=%s); not double-receiving\n",
                   state->route_name.c_str(), ser_owner_name(state->owner.load(std::memory_order_acquire)));
        return;
    }
    SerCompatClaimGuard claim_guard{state.get()};   // 正常/异常退出都归还 owner

    while (running.load(std::memory_order_acquire))
    {
        // --- Intra-process fast path: check local request queue ---
        // Always try_pop unconditionally — fp_queue_ exists from construction.
        // Registry registration/unregistration is handled by InitChannel/dtor.
        std::shared_ptr<IpcMsgBase> fp_item;
        if (fp_queue_ && fp_queue_->try_pop(fp_item) && fp_item)
        {
            if (handle_fast_path(state, *fp_item))
            {
                continue;
            }
        }

        /* 服务端等待请求 — standard SHM path（兼容路径保持"阻塞 50ms"的既有语义，
         * ⛔ 不加可读判据，否则循环退化成忙轮询）。 */
        ipc::buffer raw_data = ipc_r_ptr_->recv(50);
        if (raw_data.empty())
        {
            continue;
        }
        /* 处理路径与 worker 模式**共用**同一份（wire 分流 → callback → 响应发送）；
         * 差别只在"谁调用"（这里没有 FIFO，取到即处理）。 */
        process_request(state, std::move(raw_data));
    }   // 客户段发送请求
    /* 归还收包独占由 SerCompatClaimGuard 统一完成（正常与异常出口都覆盖）。 */
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

/* ==================== 阶段 5：数据面接入 / 注销 / 请求处理路径 ==================== */

namespace {

/* fork 防死锁闸（模块侧；共享层不做 pid 探测）：
 * 池是进程级单例且 start() 一次性；fork 之后子进程继承"已 start"却没有工作线程，子进程里
 * add_route 会走按需拉起（取池内部锁 + 建线程），而被 fork 打断的父进程可能正持这些锁 ⇒ 死锁。
 * 因此 owner pid != 当前 pid 时**不调用池**，改走兼容接收线程。
 * ⛔ 判断过程只取本文件的静态锁，绝不触碰池内部锁。 */
int32_t ser_current_process_id() noexcept
{
#if defined(_WIN32)
    return static_cast<int32_t>(::GetCurrentProcessId());
#else
    return static_cast<int32_t>(::getpid());
#endif
}

int32_t ser_recv_pool_owner_pid()
{
    static std::mutex gate_mtx;
    static int32_t owner = 0;
    const int32_t pid = ser_current_process_id();
    std::lock_guard<std::mutex> lock(gate_mtx);
    if (owner == 0)
    {
        owner = pid;
    }
    return owner;
}

bool ser_recv_pool_allowed_in_this_process()
{
    return ser_recv_pool_owner_pid() == ser_current_process_id();
}

const char* ser_register_status_reason(threepools::RecvRegisterStatus s) noexcept
{
    switch (s)
    {
    case threepools::RecvRegisterStatus::backend_unavailable:
        return "backend_unavailable (recv_wait_set backend unavailable)";
    case threepools::RecvRegisterStatus::duplicate:
        return "duplicate (route already registered on this worker)";
    case threepools::RecvRegisterStatus::busy:
        return "busy (another owner is receiving this route)";
    case threepools::RecvRegisterStatus::stopped:
        return "stopped (pool not started / already stopped)";
    case threepools::RecvRegisterStatus::invalid_token:
        return "invalid_token (request channel has no usable read wait token)";
    case threepools::RecvRegisterStatus::invalid_route:
        return "invalid_route";
    case threepools::RecvRegisterStatus::wait_set_full:
        return "wait_set_full (route capacity exceeded)";
    case threepools::RecvRegisterStatus::ok:
        break;
    }
    return "ok";
}

}   // namespace

std::shared_ptr<SerState> shm_ser_ipc::current_ser_state() const
{
    std::lock_guard<std::mutex> lock(state_mtx_);
    return ser_state_;
}

/* 数据面接入：建 state → 依次过 nodelet(C7) / fork 闸 / 后端能力闸 → add_route。
 * 返回 true = 本次在共享 worker 上收包；false = **必须**走兼容 response_thread_func，
 * 且已经在 stderr 打了显式原因（绝不静默回退、绝不忙轮询降级）。 */
bool shm_ser_ipc::start_data_plane()
{
    const auto fallback = [this](const char* why) {
        std::cerr << "\033[33m[" << topic_name_ << "_SerInfo] shm recv worker unusable (" << why
                  << "); keeping per-server receive thread\033[0m" << std::endl;
        return false;
    };

    /* 两条路径都要有 state（兼容路径也用它做 owner CAS / FIFO / 诊断）。 */
    auto state = std::make_shared<SerState>();
    state->route_name = service_prefix_for(topic_name_, domain_id_) + "_ser_r";
    state->domain_id = static_cast<std::uint32_t>(domain_id_);
    state->generation = receive_generation_.fetch_add(1, std::memory_order_acq_rel) + 1;
    state->req_ch = ipc_r_ptr_;
    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        ser_state_ = state;
    }

    /* C7：nodelet 与固定 worker 二选一 —— nodelet 启用时保留兼容接收线程。 */
    if (dzIPC::IsNodeletEnabled())
    {
        return fallback("nodelet enabled (C7: nodelet 与固定 worker 二选一)");
    }
    if (!ser_recv_pool_allowed_in_this_process())
    {
        return fallback("forked child: recv pool owner pid mismatch");
    }
    if (!threepools::RecvWorkerPool::backend_available())
    {
        return fallback("RecvWorkerPool backend unavailable");
    }

    auto route = std::make_shared<SerRequestRoute>(state);
    if (!route->read_wait_token().valid())
    {
        return fallback("invalid_token (request channel has no read wait token)");
    }

    auto& pool = threepools::RecvWorkerPool::instance();
    if (!pool.running())
    {
        (void)pool.start();   // 一次性；已被别的 server/模块启动过时返回 false
    }
    if (!pool.running())
    {
        return fallback("RecvWorkerPool::start() failed (thread creation?)");
    }
    const threepools::RecvRegisterStatus status = pool.add_route(route);
    if (status != threepools::RecvRegisterStatus::ok)
    {
        return fallback(ser_register_status_reason(status));
    }
    req_route_ = route;
    worker_mode_ = true;
    std::cerr << "\033[32m[" << topic_name_ << "_SerInfo] request receive on shared SHM worker "
              << threepools::RecvWorkerPool::worker_for(state->route_name.c_str(), state->domain_id, pool.worker_count())
              << " (generation " << state->generation << ", workers " << pool.worker_count() << ")\033[0m"
              << std::endl;
    return true;
}

/* 数据面注销（方案 §4 的第 2–6 步 + FIFO 残余记账）。幂等；由析构调用。 */
void shm_ser_ipc::stop_data_plane() noexcept
{
    const std::shared_ptr<SerState> state = current_ser_state();
    if (state)
    {
        state->stopping.store(true, std::memory_order_release);   /* ④ route stopping */
    }

    if (worker_mode_ && req_route_)
    {
        /* ② worker 路径：remove_route 同步完成契约 §4.4 的 1-6 步
         * （摘表 → wait_set.remove 唤醒 → stop_and_wake=disconnect 唤醒在途 → 等 worker 侧
         * in-flight 归零 → wait_quiescent 等本模块 in-flight → release_recv 归还收包独占）。 */
        threepools::RecvWorkerPool::instance().remove_route(req_route_.get());
    }
    else if (state && state->req_ch)
    {
        /* 兼容路径：唤醒在途 recv(50)（disconnect 亦使后续 recv 立刻返回空）。 */
        state->req_ch->disconnect();
    }

    /* 处理线程：等当前一条处理完（含 callback 与响应发送）后退出。 */
    if (state)
    {
        state->pending_cv.notify_all();
    }
    if (response_thread_ != nullptr)
    {
        if (response_thread_->joinable())
        {
            response_thread_->join();
        }
        delete response_thread_;
        response_thread_ = nullptr;
    }

    /* ⑤ 兼容路径在退出前**显式归还**收包独占（worker 路径已由 remove_route 第 6 步归还）。
     * 漏了这一步会让重新 add_route 永久返回 busy（静默丢包）。
     * P1：不再依赖 req_route_（回退路径它为空），直接在 state 的 owner 上归还；
     * 幂等，故对两条路径都安全。 */
    if (state && !worker_mode_)
    {
        ser_state_release_recv(*state);
    }

    /* ⑥ FIFO 残余按停机策略**作废**（明确策略：停机时不排空、直接丢弃并记账，
     * 与改造前"收包线程停掉后到达的请求无人应答"同一失败面，但有可观测计数）。 */
    if (state)
    {
        std::lock_guard<std::mutex> lock(state->pending_mtx);
        if (!state->pending.empty())
        {
            state->requests_dropped.fetch_add(state->pending.size(), std::memory_order_relaxed);
            state->pending_bytes.store(0, std::memory_order_relaxed);
            state->pending_count.store(0, std::memory_order_relaxed);
            state->pending.clear();
        }
    }

    {
        std::lock_guard<std::mutex> lock(state_mtx_);
        req_route_.reset();
        ser_state_.reset();
    }
    worker_mode_ = false;
}

void shm_ser_ipc::send_response(const std::shared_ptr<SerState>& state, const std::shared_ptr<ServiceData>& local_msg)
{
    if (!local_msg || !local_msg->response() || !ipc_w_ptr_ || !ipc_w_ptr_->valid())
    {
        if (state)
        {
            state->response_send_failures.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    /* 与基准实现同一份发送代码：DZFlat 借样优先，失败回退整包序列化 + 重试。 */
    if (try_send_dzflat(ipc_w_ptr_, local_msg->response()))
    {
        state->responses_sent.fetch_add(1, std::memory_order_relaxed);
        return;
    }
    ipc::buffer response_data(std::move(local_msg->response()->serialize()));
    int retry_count = 0;
    while (!ipc_w_ptr_->try_send(response_data.data(), response_data.size())
           && running.load(std::memory_order_acquire))
    {
        retry_count++;
        if (retry_count > 10)
        {
            break;
        }
    }
    if (retry_count > 10)
    {
        state->response_send_failures.fetch_add(1, std::memory_order_relaxed);
    }
    else
    {
        state->responses_sent.fetch_add(1, std::memory_order_relaxed);
    }
}

/* 一次完整请求的处理路径（worker 与兼容线程**共用**；⛔ 绝不在收包 worker 里跑）。
 * 与改造前的 response_thread_func 主体逐行同义，只是把"取一次"与"处理一次"分开。 */
void shm_ser_ipc::process_request(const std::shared_ptr<SerState>& state, ipc::buffer raw_data)
{
    if (raw_data.empty())
    {
        return;
    }
    /* 唤醒伪影门：与基准实现的 process_received_buffer 同层（门在**分流函数内部**），
     * 因此 worker 与兼容两条收包路径都必然过门（基准对齐笔记 §1.5 / B11）。 */
    if (dzIPC::IsWakeupArtifact(raw_data))
    {
        dzIPC::NoteWakeupArtifact();
        if (state)
        {
            state->wakeup_artifacts.fetch_add(1, std::memory_order_relaxed);
        }
        return;
    }
    std::shared_ptr<ServiceData> local_msg;
    {
        std::lock_guard<std::mutex> lock(message_mtx_);
        if (!message_)
        {
            return;
        }
        local_msg.reset(message_->clone());
    }
    /* 双 wire 分流: DZFlat + typed request → 借样成只读视图(回调用 request_view<T>() 读,
     * 零拷贝); TLV / schema-less → 物化进 owning request()。 */
    auto tpl = local_msg->request();
    std::shared_ptr<dzIPC::Sample> sample;
    const int wr = classify_received(raw_data, tpl, tpl ? tpl->msg_id() : 0, sample);
    if (wr == 1)
    {
        local_msg->request_sample() = std::move(sample);
    }
    else if (wr == 0)
    {
        if (!accept_wire(raw_data, local_msg->request()))
        {
            if (verbose_)
            {
                std::cerr << "\033[33m[Warning] Received message with invalid ID on topic: " << topic_name_
                          << "\033[0m" << std::endl;
            }
            return;
        }
    }
    else
    {
        return;   // typed DZFlat 但 id/schema 不符 → 丢弃
    }

    std::function<void(std::shared_ptr<ServiceData>&)> callback;
    {
        std::lock_guard<std::mutex> lock(callback_mtx_);
        callback = callback_;
    }
    if (callback)
    {
        /* callback 时长不可控 ⇒ 必须可观测（方案 §6/§8）；异常隔离，绝不让处理线程死。 */
        const auto t0 = std::chrono::steady_clock::now();
        bool threw = false;
        try
        {
            callback(local_msg);
        }
        catch (...)
        {
            threw = true;
        }
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - t0)
                            .count();
        if (state)
        {
            state->callback_count.fetch_add(1, std::memory_order_relaxed);
            state->callback_ns_total.fetch_add(static_cast<std::uint64_t>(ns), std::memory_order_relaxed);
            std::uint64_t prev = state->callback_ns_max.load(std::memory_order_relaxed);
            while (static_cast<std::uint64_t>(ns) > prev
                   && !state->callback_ns_max.compare_exchange_weak(prev, static_cast<std::uint64_t>(ns),
                                                                    std::memory_order_relaxed))
            {
            }
            if (threw)
            {
                state->callback_exceptions.fetch_add(1, std::memory_order_relaxed);
            }
        }
        if (threw)
        {
            return;   // callback 抛异常：不发陈旧响应（与"不改接口语义"一致，且不让线程死）
        }
    }
    send_response(state, local_msg);
}

/* nodelet 进程内快路径（**兼容模式专属**；worker 模式下 InitChannel 不注册 fp_queue）。
 * ⛔ 同样不在共享 worker 里跑 callback。返回 true = 已消化掉这个 item。 */
bool shm_ser_ipc::handle_fast_path(const std::shared_ptr<SerState>& state, IpcMsgBase& envelope_item)
{
    (void)state;
    auto* envelope = dynamic_cast<FastPathRequestEnvelope*>(&envelope_item);
    if (envelope == nullptr || !envelope->reply_queue())
    {
        return false;
    }
    std::shared_ptr<IpcMsgBase> request_copy(envelope->request()->clone());
    std::function<void(std::shared_ptr<ServiceData>&)> callback;
    {
        std::lock_guard<std::mutex> lock(callback_mtx_);
        callback = callback_;
    }
    if (!callback)
    {
        return true;   // 无回调也要按原样消化掉（与改造前同序）
    }
    std::shared_ptr<ServiceData> local_msg;
    {
        std::lock_guard<std::mutex> lock(message_mtx_);
        if (!message_)
        {
            return true;
        }
        local_msg.reset(message_->clone());
    }
    local_msg->request() = request_copy;   // 快路径：绕过反序列化
    callback(local_msg);
    envelope->reply_queue()->push(local_msg->response());   // 响应直接进请求方队列
    return true;
}

/* worker 模式的处理线程（形态 b）：只从本 server 的有界 FIFO 取**完整请求** → process_request。
 * 每 server 一条处理线程 ⇒ callback 串行与响应顺序由"单线程消费本 server FIFO"天然保证。 */
void shm_ser_ipc::process_thread_func()
{
    const std::shared_ptr<SerState> state = current_ser_state();
    if (!state)
    {
        return;
    }
    while (running.load(std::memory_order_acquire) && !state->stopping.load(std::memory_order_acquire))
    {
        ipc::buffer raw;
        {
            std::unique_lock<std::mutex> lock(state->pending_mtx);
            state->pending_cv.wait_for(lock, std::chrono::milliseconds{100}, [&] {
                return !state->pending.empty() || !running.load(std::memory_order_acquire)
                       || state->stopping.load(std::memory_order_acquire);
            });
            if (state->pending.empty())
            {
                continue;
            }
            raw = std::move(state->pending.front());
            state->pending.pop_front();
            state->pending_count.fetch_sub(1, std::memory_order_relaxed);
            state->pending_bytes.fetch_sub(raw.size(), std::memory_order_relaxed);
        }
        process_request(state, std::move(raw));
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

shm_cli_ipc::shm_cli_ipc(const std::string& topic_name, const std::shared_ptr<ServiceData>& msg, size_t domain_id,
                         bool verbose, bool enable_thread_qos, int cpu_id, int thread_priority)
    : cli_ipc_base(topic_name, msg, domain_id, verbose)
    , topic_name_(topic_name)
    , domain_id_(domain_id)
    , verbose_(verbose)
    , thread_options_(dzIPC::ThreadDispatch::make_realtime_options(enable_thread_qos, cpu_id, thread_priority))
{
    message_.reset(msg->clone());
    dzIPC::ThreadDispatch::apply_current_thread_options(thread_options_, verbose_, topic_name_ + "_CliOwnerThread");
}

shm_cli_ipc::~shm_cli_ipc()
{
    running.store(false, std::memory_order_release);
    if (handshake_thread_ != nullptr)
    {
        if (handshake_thread_->joinable())
        {
            handshake_thread_->join();
        }
        delete handshake_thread_;
    }
    {
        std::lock_guard<std::mutex> lock(channel_mtx_);
        if (ipc_r_ptr_ && ipc_r_ptr_->valid())
        {
            ipc_r_ptr_->release();
        }
        if (ipc_w_ptr_ && ipc_w_ptr_->valid())
        {
            ipc_w_ptr_->release();
        }
    }
    exit_flag.store(true, std::memory_order_release);
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

void shm_cli_ipc::InitChannel(std::string extra_info)
{
    // 等待服务端创建通道
    handshake_thread_ = new std::thread(&shm_cli_ipc::cli_handshake, this);
    // 等待握手完成
    std::shared_ptr<ServiceData> message_template;
    {
        std::lock_guard<std::mutex> lock(message_mtx_);
        message_template = message_;
    }
    std::string response_type_name =
        (message_template && message_template->response())
            ? dzIPC::info_pool::demangle(typeid(*message_template->response()).name())
            : std::string{};
    response_type_name = extract_last_segment(response_type_name);
    pool_reg_.rebind({dzIPC::info_pool::EntryKind::ShmClient, topic_name_, response_type_name, "shm",
                      static_cast<int32_t>(domain_id_), extra_info});
    if (verbose_)
    {
        std::cerr << "\033[32m[" << topic_name_ << "_CLiInfo] Client connected to server topic: " << topic_name_
                  << "\033[0m" << std::endl;
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/

void shm_cli_ipc::cli_handshake()
{
    /* ⛔ 这里**不能抛**: 本函数是 std::thread 的入口, 抛出去就是 std::terminate
     * ——一个"控制面段打不开"的局部失败会变成整个进程 abort。而这条路径现在是
     * 路径切换的常规分支(同机判定成立后客户端必然走这里), 失败必须是**可回退的**:
     * 直接返回 -> handshake_completed_ 保持 false -> 上层的有界等待超时 -> 回退
     * 到 socket。 */
    if (!control_plane_.open(service_control_name_for(topic_name_, domain_id_)))
    {
        std::cerr << "\033[31m[" << topic_name_
                  << "_CliInfo] control plane open failed; staying on the previous path\033[0m" << std::endl;
        return;
    }
    uint32_t attached_generation = 0;
    bool peer_registered = false;
    while (running.load(std::memory_order_acquire))
    {
        const uint32_t generation = control_plane_.generation();
        const TopicState state = control_plane_.state();
        if (state == TopicState::Ready && generation != 0)
        {
            if (!handshake_completed_.load(std::memory_order_acquire) || attached_generation != generation)
            {
                if (peer_registered)
                {
                    control_plane_.remove_peer(attached_generation);
                    control_plane_.release_peer_slot(peer_slot_);
                    peer_slot_ = -1;
                    peer_registered = false;
                }
                std::string r_name, w_name;
                r_name = service_prefix_for(topic_name_, domain_id_) + "_ser_w";
                w_name = service_prefix_for(topic_name_, domain_id_) + "_ser_r";
                {
                    std::lock_guard<std::mutex> lock(channel_mtx_);
                    if (ipc_r_ptr_ && ipc_r_ptr_->valid())
                    {
                        ipc_r_ptr_->release();
                    }
                    if (ipc_w_ptr_ && ipc_w_ptr_->valid())
                    {
                        ipc_w_ptr_->release();
                    }
                    ipc_r_ptr_ = std::make_shared<ipc::server>(r_name.c_str(), ipc::receiver, verbose_);
                    ipc_w_ptr_ = std::make_shared<ipc::server>(w_name.c_str(), ipc::sender, verbose_);
                }
                attached_generation = generation;
                if (!control_plane_.add_peer(attached_generation))
                {
                    std::lock_guard<std::mutex> lock(channel_mtx_);
                    if (ipc_r_ptr_ && ipc_r_ptr_->valid())
                    {
                        ipc_r_ptr_->release();
                    }
                    if (ipc_w_ptr_ && ipc_w_ptr_->valid())
                    {
                        ipc_w_ptr_->release();
                    }
                    ipc_r_ptr_.reset();
                    ipc_w_ptr_.reset();
                    continue;
                }
                peer_registered = true;
                /* 向控制面登记本客户端在响应通道 (ipc_r_ptr_) 上的连接 bit,
                 * 并由下面的循环持续刷新心跳。服务端据此判定死连接并回收。 */
                {
                    std::lock_guard<std::mutex> lock(channel_mtx_);
                    const uint32_t cc_id = (ipc_r_ptr_ && ipc_r_ptr_->valid())
                                               ? ipc_r_ptr_->connected_id()
                                               : 0u;
                    peer_slot_ = control_plane_.acquire_peer_slot(attached_generation, cc_id);
                }
                if (peer_slot_ < 0 && verbose_)
                {
                    std::cerr << "\033[33m[" << topic_name_
                              << "_CliInfo] no free peer slot in control plane; this client "
                                 "will not be reaped automatically if it dies\033[0m"
                              << std::endl;
                }
                handshake_completed_.store(true, std::memory_order_release);
            }
        }
        else
        {
            if (handshake_completed_.exchange(false, std::memory_order_acq_rel))
            {
                if (peer_registered)
                {
                    control_plane_.remove_peer(attached_generation);
                    control_plane_.release_peer_slot(peer_slot_);
                    peer_slot_ = -1;
                    peer_registered = false;
                }
                std::lock_guard<std::mutex> lock(channel_mtx_);
                if (ipc_r_ptr_ && ipc_r_ptr_->valid())
                {
                    ipc_r_ptr_->release();
                }
                if (ipc_w_ptr_ && ipc_w_ptr_->valid())
                {
                    ipc_w_ptr_->release();
                }
                ipc_r_ptr_.reset();
                ipc_w_ptr_.reset();
            }
        }
        /* 心跳: 只要本客户端进程还活着, 这里就会每 10ms 刷新一次。
         * 进程崩溃后心跳停止, 服务端超时即可安全回收其连接。 */
        control_plane_.peer_heartbeat(peer_slot_);
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    if (peer_registered)
    {
        control_plane_.remove_peer(attached_generation);
    }
    control_plane_.release_peer_slot(peer_slot_);
    peer_slot_ = -1;
    if (verbose_)
    {
        std::cerr << "\033[32m[" << topic_name_
                  << "_CLiInfo] Client exiting, detaching from server: " << topic_name_ << "\033[0m"
                  << std::endl;
    }
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
bool shm_cli_ipc::send_request(std::shared_ptr<ServiceData>& request, uint64_t rev_tm)
{
    /* 主线程执行，因此无需考虑running */
    if (!handshake_completed_.load(std::memory_order_acquire))
    {
        if (verbose_)
        {
            std::cerr << "\033[32m[" << topic_name_
                      << "_CLiInfo] Handshake not completed, cannot send request on topic: " << topic_name_ << "\033[0m"
                      << std::endl;
        }
        // One-shot warning when nodelet is enabled but no server is reachable.
        if (dzIPC::IsNodeletEnabled() && !nodelet_no_server_warned_.exchange(true))
        {
            std::cerr << "\033[33m[" << topic_name_
                      << "] nodelet requested but unavailable; falling back\033[0m" << std::endl;
        }
        return false;
    }
    std::shared_ptr<ServiceData> message_template;
    {
        std::lock_guard<std::mutex> lock(message_mtx_);
        if (!message_)
        {
            return false;
        }
        message_template.reset(message_->clone());
    }

    // --- Intra-process fast path ---
    // Registry only holds server request queues.  The client creates a
    // per-request capacity-1 reply queue and embeds it in the envelope;
    // the server routes the response directly to that queue.  This
    // guarantees call-level correlation — no broadcast, no pollution.
    //
    // K=3 consecutive observations of a single local server queue gates
    // activation.  On any key or snapshot-size change the counter resets.
    //
    // IsNodeletEnabled() is checked at each send_request call, so
    // EnableNodelet(true) after InitChannel is supported.
    if (dzIPC::IsNodeletEnabled())
    {
        const uint32_t req_msg_id = request->request()->msg_id();
        ChannelKey key{topic_name_, domain_id_, req_msg_id, ChannelKind::ShmService};
        auto& reg = LocalPubSubRegistry::instance();
        auto snapshot = reg.subscriber_snapshot(key);

        // Server count: snapshot must contain exactly one entry (the server).
        const size_t server_count = snapshot.size();

        bool use_fp = false;
        {
            std::lock_guard<std::mutex> lock(fast_path_mtx_);
            if (!(key == last_fp_ser_key_) || server_count != last_fp_ser_snapshot_size_)
            {
                fp_consecutive_ = 0;
                last_fp_ser_key_ = key;
                last_fp_ser_snapshot_size_ = server_count;
            }

            if (server_count == 1)
            {
                ++fp_consecutive_;
                if (fp_consecutive_ >= kFastPathConfirm)
                {
                    use_fp = true;
                }
            }
            else
            {
                fp_consecutive_ = 0;
            }
        }

        if (use_fp)
        {
            // Per-request reply queue: capacity 1, lifetime scoped to this call.
            // Any late response arriving after we return is naturally discarded
            // when the queue goes out of scope.
            auto reply_queue = std::make_shared<CircularQueue<IpcMsgBase>>(1);
            auto envelope = std::make_shared<FastPathRequestEnvelope>(
                std::shared_ptr<IpcMsgBase>(request->request()->clone()), reply_queue);
            snapshot[0]->push(envelope);

            std::shared_ptr<IpcMsgBase> fp_response;
            if (reply_queue->pop(fp_response, rev_tm))
            {
                request->response() = fp_response;
                return true;
            }
            // Timeout: return false — do NOT fall through to SHM.
            // The server may still process the queued request asynchronously;
            // re-sending via SHM would double-execute the callback.
            return false;
        }

        // Issue one-shot warnings for why fast path is unavailable.
        // std::atomic exchange(true) atomically checks-and-sets in one op.
        if (server_count == 0 && !nodelet_no_server_warned_.exchange(true))
        {
            std::cerr << "\033[33m[" << topic_name_
                      << "] nodelet requested but unavailable; falling back\033[0m" << std::endl;
        }
        else if (server_count > 1 && !nodelet_anomaly_warned_.exchange(true))
        {
            std::cerr << "\033[33m[" << topic_name_
                      << "] nodelet registry anomaly (" << server_count
                      << " entries); falling back\033[0m" << std::endl;
        }
    }

    // --- Standard SHM path ---
    std::lock_guard<std::mutex> channel_lock(channel_mtx_);
    if (!handshake_completed_.load(std::memory_order_acquire) || !ipc_w_ptr_ || !ipc_r_ptr_)
    {
        return false;
    }
    if (!try_send_dzflat(ipc_w_ptr_, request->request()))
    {
        ipc::buffer request_data(std::move(request->request()->serialize()));
        int retry_count = 0;
        while (!ipc_w_ptr_->try_send(request_data.data(), request_data.size()))
        {
            retry_count++;
            if (retry_count > 10)
            {
                return false;
            }
        };
    }
    request->response_sample().reset();   // 清掉上一次响应(若有), 本次重新收
    do
    {
        ipc::buffer raw_response = ipc_r_ptr_->recv(rev_tm);
        if (raw_response.empty())   // 超时未收到响应
        {
            return false;
        }
        auto resp_tpl = request->response();
        std::shared_ptr<dzIPC::Sample> sample;
        const int rr =
            classify_received(raw_response, resp_tpl, resp_tpl ? resp_tpl->msg_id() : 0, sample);
        if (rr == 1)
        {
            /* DZFlat 响应借样: response() 保持模板克隆, 借样段随 ServiceData 活到调用方读完。 */
            request->response_sample() = std::move(sample);
            break;
        }
        if (rr == 0 && accept_wire(raw_response, request->response()))
        {
            break;   // TLV / schema-less 响应已物化
        }
        /* rr == -1(typed DZFlat 不符)或 accept_wire 拒 → 重新接收 */
    } while (true);
    return true;
}

/******************************************************************************************************/
/******************************************************************************************************/
/******************************************************************************************************/
void shm_cli_ipc::reset_message(const std::shared_ptr<ServiceData>& msg)
{
    if (!msg)
    {
        return;
    }
    std::shared_ptr<ServiceData> new_msg;
    new_msg.reset(msg->clone());
    std::lock_guard<std::mutex> lock(message_mtx_);
    message_ = std::move(new_msg);
}
}   // namespace shm
}   // namespace dzIPC

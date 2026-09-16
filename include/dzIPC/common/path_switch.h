#pragma once
/* ser-cli 路径切换的可观测状态（T2 设计 §5）。
 *
 * 为什么单独一个头: 这套字段要同时被两条独立实现使用 —— socket 臂
 * (socket_ser_cli_ipc.h) 与 shm 臂 (shm_ser_cli_ipc.h), 以及切换层
 * (auto_path_ser_cli_ipc.h)。如果放在任一侧的头里, 另一侧就得反向依赖。
 *
 * 为什么全是 atomic: 写入者是握手/切换线程, 读取者是 (a) 业务线程、
 * (b) 测试断言线程。全部字段都允许多读者轮询, 不能要求加锁 —— 否则
 * 测试里"切换窗口内轮询 send_request 返回 false"这个断言会与切换线程死锁。
 *
 * 为什么是"状态"而不是"日志": 任务列表统一约束写明「不以日志"看起来切换成功"
 * 作为唯一证据」。这里的每个字段都要能被断言, 判据见各自注释。 */
#include <atomic>
#include <cstdint>
#include "libipc/export.h"

namespace dzIPC {
namespace path {

/* 路径状态机的状态集（T2 §4）。双端共用同一枚举。 */
enum class State : uint8_t {
    Idle = 0,        // S0 已构造, InitChannel 未调用
    Probe = 1,       // S1 UDP 握手通道已建, 交换握手帧
    Negotiate = 2,   // S2 握手成立 + 收集同机证据
    Establish = 3,   // S3 按裁定建数据通道
    Active = 4,      // S4 数据面可用, handshake_completed()==true
    Withdraw = 5,    // S5 建立失败或运行期断链 -> 撤销
    Teardown = 6,    // S6 teardown: 关通道 + 注销池/控制面/快速路径
    Closed = 7       // S7 终态
};

/* 当前实际承载数据的传输。
 *
 * ⛔ 与 IPCType 不是一回事: IPCType 在构造期一次定死（server_ipc.cc:90-103/
 * :159-172）, 切换之后**不再等于**实际传输。任何需要"当下真正走哪条路"的
 * 地方（日志 TransportKind、切换决策）都必须读这里, 不能读 IPCType。 */
enum class Kind : uint8_t {
    None = 0,
    Socket = 1,
    Shm = 2
};

/* 判定依据。取值必须能区分"有证据"与"证据不足", 因为后者是 fail-safe 方向
 * （保留 socket）, 而不是错误（T2 §3 D7）。 */
enum class DecisionReason : uint8_t {
    Pending = 0,        // 尚未判定
    PeerInPool = 1,     // 本机 IpcInfoPool 里看到对端条目 => 同机证据成立
    NoEvidence = 2,     // 无证据 => 按跨机处理, 保留 socket
    Timeout = 3,        // 协商超时 => 保留 socket
    ShmNotReady = 4,    // 有证据但对端 SHM 数据面在 T_est 内未就绪
    ChannelOccupied = 5 // 目标 SHM 通道已被占用, 不得 clear_storage
};

/* 回退原因（T2 §3 D7）。 */
enum class FallbackReason : uint8_t {
    None = 0,
    ShmEstablishFailed = 1,     // SHM 腿 InitChannel() 抛出异常
    ShmRendezvousTimeout = 2,   // 对端未在 T_est 内确认就绪
    ShmChannelOccupied = 3,     // 目标通道被占用
    RemoteIoFailure = 4,        // 运行期 SHM I/O 连续失败
    WithdrawnByPeer = 5         // 对端公告撤销
};

IPC_EXPORT const char* to_string(State s) noexcept;
IPC_EXPORT const char* to_string(Kind k) noexcept;
IPC_EXPORT const char* to_string(DecisionReason r) noexcept;
IPC_EXPORT const char* to_string(FallbackReason r) noexcept;

/* 同机证据留痕（可空）。kind==0 表示无证据。 */
struct Evidence
{
    std::atomic<int32_t> kind{0};       // info_pool::EntryKind 的数值
    std::atomic<int32_t> peer_pid{0};
    std::atomic<int64_t> ts_ns{0};
};

/* 可观测字段最小集。默认值 = "什么都没发生", 也就是 fail-safe 那一侧。 */
struct Status
{
    std::atomic<uint8_t> path_state{static_cast<uint8_t>(State::Idle)};
    std::atomic<uint8_t> path_selected{static_cast<uint8_t>(Kind::None)};
    std::atomic<uint8_t> decision_reason{static_cast<uint8_t>(DecisionReason::Pending)};
    Evidence evidence;
    std::atomic<uint8_t> fallback_reason{static_cast<uint8_t>(FallbackReason::None)};
    std::atomic<bool> cleanup_done{false};
    std::atomic<int64_t> cleanup_ts_ns{0};
    std::atomic<uint64_t> switch_attempts{0};
    std::atomic<uint64_t> switch_successes{0};
    std::atomic<uint64_t> switch_fallbacks{0};
    /* 切换窗口内被拒的请求数。判据: 这些请求**全部**立即返回 false,
     * 且没有任何一条挂起（T2 §4 W_switch 断言）。 */
    std::atomic<uint64_t> requests_in_switch_window{0};
    /* ⛔ 必须恒 0。非 0 即"同一 RPC 执行了两次", 是任务列表 R2 的红线。 */
    std::atomic<uint64_t> dup_delivery_detected{0};

    void set_state(State s) noexcept { path_state.store(static_cast<uint8_t>(s), std::memory_order_release); }
    State state() const noexcept { return static_cast<State>(path_state.load(std::memory_order_acquire)); }

    void set_selected(Kind k) noexcept { path_selected.store(static_cast<uint8_t>(k), std::memory_order_release); }
    Kind selected() const noexcept { return static_cast<Kind>(path_selected.load(std::memory_order_acquire)); }

    void set_decision(DecisionReason r) noexcept
    {
        decision_reason.store(static_cast<uint8_t>(r), std::memory_order_release);
    }
    DecisionReason decision() const noexcept
    {
        return static_cast<DecisionReason>(decision_reason.load(std::memory_order_acquire));
    }

    void set_fallback(FallbackReason r) noexcept
    {
        fallback_reason.store(static_cast<uint8_t>(r), std::memory_order_release);
    }
    FallbackReason fallback() const noexcept
    {
        return static_cast<FallbackReason>(fallback_reason.load(std::memory_order_acquire));
    }

    /* 清理完成证明。T4 的"无残留"断言读这两个字段 + 外部资源计数。 */
    void mark_cleanup_done(int64_t now_ns) noexcept
    {
        cleanup_ts_ns.store(now_ns, std::memory_order_release);
        cleanup_done.store(true, std::memory_order_release);
    }
};

}   // namespace path
}   // namespace dzIPC

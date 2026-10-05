#pragma once
/* 阶段 2 收包路径的**内部测试缝** —— 不是公共 API, 不属于任何对外契约。
 *
 * 落点: docs/消息接收架构改造/阶段2_全量测试方案.md §4.1 / §4.2。
 *
 * ── 为什么需要它 ────────────────────────────────────────────────────────
 * 方案 §4.1 要求给 I5(已弹出的 buffer 不得因 generation 落后被丢)做**固定事件顺序**
 * 的确定性守门, 事件序列是:
 *
 *   旧 route 的 recv 已返回一条带唯一序号的非空 buffer
 *     → 在收包线程处理该 buffer 前推进 generation / 启动 rebuild
 *     → 允许收包线程继续处理
 *     → 断言该序号恰好进入 view_queue_ 或 msg_queue_
 *
 * 中间那个窗口在真实代码里是**纳秒级**的(recv 返回到 release_receive 之间只有几
 * 条指令)。方案原文明确禁止"以随机压力碰撞窗口", 要求用条件变量或测试钩子在
 * 「recv 返回」与「分流入队」之间暂停。不暂停就**不可能**确定性命中 —— 这正是不加
 * 钩子时的现实: v4 只读验收把 X2(在收包循环加 generation 判断)判为
 * 「靠无人写该判断成立, 非测试守住」, 即根本没有判据。
 *
 * 方案 §4.2 同理: 对象级析构守门要求「记录 stop_and_wake、recv 返回、join 完成的
 * **因果顺序**」。v4 §3.2 已实测证否了另一条路 —— 在 recv(50) 下用析构**耗时**做判据
 * 不可行(基线 27/28/29ms 与删掉 disconnect 的变异体**逐样本相同**)。所以只能观测顺序,
 * 而析构顺序在外部不可见, 只能在内部打点。
 *
 * ── 设计约束 ────────────────────────────────────────────────────────────
 *   · **默认关闭**: 钩子为空指针时, 每个调用点只有一次 relaxed atomic load + 分支。
 *     接收尝试与视图入/出队分别打点；未安装钩子时不读取时钟。
 *   · **行为逐位不变**: 未设置钩子时产品行为与没有本头文件时完全一致。不新增公共
 *     生产 API、不改任何对外签名、不改控制面协议。
 *   · **钩子在不持锁时调用**: 所有调用点都在 RouteSession 锁外(收包循环本就不持锁;
 *     析构的点位按说明 §5 的顺序, 每次调用前后都不持 mtx_)。钩子内**允许阻塞** ——
 *     I5 用例正是靠它在 kAfterRecvRelease 处把收包线程停住。
 *   · **进程内全局**: 一个钩子覆盖所有 shm_sub_ipc 实例(与 IsWakeupArtifact 的计数
 *     器同风格)。测试在同一进程内只对单个实例打点, 且用 point 过滤。
 *   · **必须随收包路径迁移**: 阶段 3 提取 process_received_buffer 时,
 *     kAfterRecv / kAfterRecvRelease 两个点属于「raw_data 非空之后」的函数体,
 *     必须随之一并搬走, 不得只留在调用点 —— 否则阶段 5 的 worker 会各自漏掉一处
 *     (与 IsWakeupArtifact 的迁移守则同一条理由, 见 dzIPC/common/wire_accept.h)。
 */
#include <cstddef>
#include <cstdint>

#include "libipc/export.h"
/* `ipc::route` 是 `chan<...>` 的 using 别名, **无法前置声明**(前置声明会与 ipc.h 里
 * 的别名冲突: "has a previous declaration here")。本头文件是内部测试缝, 调用方
 * (shm_pub_sub_ipc.cc 与测试)本来就都包含 ipc.h, 直接引入。 */
#include "libipc/ipc.h"

namespace dzIPC {
namespace detail {

/* 打点位置。数值一旦定下就不要改 —— 测试按数值区间断言「析构各点的相对顺序」。 */
enum class SeamPoint : int
{
    /* ---- 收包路径 (shm_pub_sub_ipc 的收包一次) ----
     *
     * W06 起收包一次被抽成 shm_sub_ipc::recv_once_locked()，**worker 路径与兼容
     * 线程路径共用同一份实现** —— 因此这两个点的语义与位置逐句不变（原来是
     * subscribe_thread_ lambda 的函数体，现在随函数体一起搬）：
     *   · worker：由 RecvWorker 的宿主适配器 SubRecvRoute::recv_once() 调用；
     *   · compat：由 compat_recv_loop() 调用。
     * 两条路径的 recv 等待时长不同（worker = recv(0) 非阻塞；compat = recv(50)），
     * 但打点位置与 generation 语义完全相同。 */
    kAfterRecv        = 0,   /* recv() 已返回、尚未 release_receive。此时 inflight 仍为 1
                              * ⇒ 钩子**不得阻塞**(会拖住并发 begin_rebuild 的第 4 步)。 */
    kAfterRecvRelease = 1,   /* release_receive() 已执行、尚未分流。**I5 的暂停点**:
                              * 此刻 inflight == 0, 重建方可推进到第 5 步 release 旧 route。
                              * 这是唯一允许钩子阻塞的收包点。 */

    /* 接收分段诊断。默认关闭，开启的钩子不得阻塞或分配。
     * BeforeRecv在成功取得lease之后，未把lease取得时间冒称recv耗时。
     * 入队前与出队后包围实际队列操作，并非精确的队列驻留时间。 */
    kBeforeRecv            = 2,
    kBeforeViewEnqueue     = 3,
    kAfterViewDequeue      = 4,
    kCallerAssistedReceive = 5, // 调用线程完成一次非阻塞协作接收；不携带借用字节。
    kBeforeWorkerReceive   = 6, // 协作模式worker取消费锁之前；无lease，可供测试暂停。
    kBeforeCallerWait      = 7, // 调用线程即将阻塞；可能持有lease，钩子不得阻塞。
    kBeforeAssistRelease   = 8, // 恢复worker之前/之后；只允许不阻塞的诊断。
    kAfterAssistRelease    = 9,

    /* ---- 析构 (shm_sub_ipc::~shm_sub_ipc, 说明 §5 的八步) ---- */
    kDtorAfterUnregister    = 16,   /* §5 第 1 步: LocalPubSubRegistry 已注销 */
    kDtorAfterStopAndWake   = 17,   /* §5 第 4 步: route_session_.stop_and_wake() 已返回 */
    kDtorAfterJoinSubscribe = 18,   /* §5 第 5 步: 收包线程已 join */
    kDtorAfterJoinHandshake = 19,   /* §5 第 5' 步: 握手线程已 join */
    kDtorAfterQuiescent     = 20,   /* §5 第 6 步: wait_quiescent() 已返回 */
    kDtorAfterRelease       = 21,   /* §5 第 7 步: 旧 route 已 release */

    /* ---- W06 接收路径接入/注销（worker 路径与兼容路径的**可分性**观测点）----
     *
     * 用途：让"到底走了哪条路径"成为**运行期可观测**的事实，而不是从
     * "线程数少了"反推。方案 §13.2 条件 4 要求"不能通过额外 per-route 线程补齐"，
     * 验收模式必须**能检测回退**、不能用回退线程掩盖固定 worker 目标：
     *   · kRecvPathWorker    —— 数据面接入共享 RecvWorker（无 subscribe_thread_）；
     *   · kRecvPathCompat    —— 显式回退兼容收包线程（原因见同一次事件的 size 字段）。
     * event.size 携带的原因码 = CompatFallbackReason 的数值（0 = 非回退）。
     * 数值 32/33 与既有 0..21 不重叠。 */
    kRecvPathWorker         = 32,
    kRecvPathCompat         = 33,
};

/* W06：接收路径选择/回退的**原因码**（写在 kRecvPathCompat 事件的 `size` 字段；
 * kRecvPathWorker 恒为 kNone）。数值一旦定下就不要改 —— 测试与采集器按它分类，
 * 并且它让"回退了吗"变成可机械统计的事实，而不是从线程数反推。
 *
 * ⛔ kForcedCompatEnv 与 kNoRoute **不是故障回退**：
 *   · kForcedCompatEnv —— 配置显式要求兼容路径（回滚开关），属"路径选择"；
 *   · kNoRoute         —— 本次还没有 route（控制面尚未 Ready 或重建失败），
 *                        池根本未尝试，下一次重建会重试。
 * 两者的 fallback 计数都不 +1（与 W09 的"路径选择不是回退"同一纪律）。 */
enum class RecvPathReason : int
{
    kNone               = 0,   ///< 走共享 worker（kRecvPathWorker）
    kForcedCompatEnv    = 1,   ///< 回滚开关强制兼容（配置，不是回退）
    kForkChild          = 2,   ///< fork 子进程 pid 闸（防死锁，走兼容线程）
    kBackendUnavailable = 3,   ///< wait-set 后端不可用（永久回退信号）
    kPoolStartFailed    = 4,   ///< 池 start() 失败（线程创建失败）
    kNoRoute            = 5,   ///< 尚无 route（未尝试池；下次重建重试）
    kInvalidToken       = 6,   ///< read_wait_token() 无效
    kWaitSetFull        = 7,   ///< 该 worker 的 127 token 容量满
    kBusy               = 8,   ///< 别的 owner（如兼容线程）正在 recv
    kDuplicate          = 9,   ///< 同一 route 已在本 worker 注册
    kStopped            = 10,  ///< 池未 start / 已 stop
    kInvalidRoute       = 11,  ///< route == nullptr
};

/* 打点携带的信息。`route` 在recv前/返回/释放lease后为对应的legacy route，MPMC可为
 * nullptr；视图入/出队点为nullptr。data/size在recv返回与视图入/出队点提供借用字节，
 * 钩子不得在返回后继续引用这些字节。 */
struct SeamEvent
{
    SeamPoint point{SeamPoint::kAfterRecv};
    std::uint32_t generation{0};
    const ipc::route* route{nullptr};
    const void* data{nullptr};
    std::size_t size{0};
};

/* 钩子签名。**不得抛异常**(调用点在收包循环里, 抛出会带走生命周期管理)。 */
using SeamHook = void (*)(const SeamEvent&);

/* 安装/卸载钩子。传 nullptr 卸载。进程内全局, 默认 nullptr(= 关)。
 * 线程安全: 用 atomic 函数指针, 可在收包线程运行时安装/卸载。 */
IPC_EXPORT void SetSeamHook(SeamHook hook) noexcept;
IPC_EXPORT SeamHook GetSeamHook() noexcept;

/* 给调用点用的最小封装: 钩子为空时只做一次 relaxed load。 */
inline void FireSeam(const SeamEvent& ev) noexcept
{
    const SeamHook h = GetSeamHook();
    if (h != nullptr)
    {
        h(ev);
    }
}

}   // namespace detail
}   // namespace dzIPC

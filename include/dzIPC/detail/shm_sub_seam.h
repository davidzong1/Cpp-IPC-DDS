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
 *     收包循环每迭代(一次 recv(50))两次, 量级 ns, 相对一次 recv 可忽略。
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
    /* ---- 收包循环 (src/dzIPC/shm_pub_sub_ipc.cc 的 subscribe_thread_ lambda) ---- */
    kAfterRecv        = 0,   /* recv() 已返回、尚未 release_receive。此时 inflight 仍为 1
                              * ⇒ 钩子**不得阻塞**(会拖住并发 begin_rebuild 的第 4 步)。 */
    kAfterRecvRelease = 1,   /* release_receive() 已执行、尚未分流。**I5 的暂停点**:
                              * 此刻 inflight == 0, 重建方可推进到第 5 步 release 旧 route。
                              * 这是唯一允许钩子阻塞的收包点。 */

    /* ---- 析构 (shm_sub_ipc::~shm_sub_ipc, 说明 §5 的八步) ---- */
    kDtorAfterUnregister    = 16,   /* §5 第 1 步: LocalPubSubRegistry 已注销 */
    kDtorAfterStopAndWake   = 17,   /* §5 第 4 步: route_session_.stop_and_wake() 已返回 */
    kDtorAfterJoinSubscribe = 18,   /* §5 第 5 步: 收包线程已 join */
    kDtorAfterJoinHandshake = 19,   /* §5 第 5' 步: 握手线程已 join */
    kDtorAfterQuiescent     = 20,   /* §5 第 6 步: wait_quiescent() 已返回 */
    kDtorAfterRelease       = 21,   /* §5 第 7 步: 旧 route 已 release */
};

/* 打点携带的信息。`route` 在 kAfterRecv 上是刚返回的那条 route(可读 connected_id()
 * 判定 disconnect 是否已生效); 其余点位为 nullptr。data/size 只在收包点非空。 */
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

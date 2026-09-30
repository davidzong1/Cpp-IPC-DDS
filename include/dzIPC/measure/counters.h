#pragma once
/* W03 测量口径 · 低开销计数注册表
 * ============================================================================
 * 交付依据：团队改造方案 §4 W03 第 4 条「为 DZFlat A/B、TLV、回退原因、注册
 * 失败、wait-set 满、chunk 申请失败和队列淘汰定义低开销计数」；§10.2「测量必须
 * 增加：每轮扫描 route 数、扫描耗时、等待超时次数、有效就绪比例、deferred 深度，
 * 以及这些指标随 route 数量的变化。低开销常驻计数与详细诊断采样分开，性能报告
 * 注明诊断是否开启」；§13.3 的失败分类（registration_rejected / wait_token_invalid
 * / wait_set_full / fd_limit / chunk_exhausted / queue_backpressure /
 * generation_mismatch / fallback_activated / publish_blocked / rx_timeout）。
 *
 * 设计约束：
 *   · **常驻计数**一律 `std::atomic<uint64_t>` + `memory_order_relaxed`，热路径
 *     只有一次 `fetch_add`（x86 上是 `lock xadd`，几个 ns）。不做 map 查找、不
 *     分配、不加锁、不打日志。
 *   · **诊断计数**（每轮扫描 route 数 / 扫描耗时 / deferred 深度 / 就绪比例）会
 *     随 route 数放大，因此单独由 `diagnostics_enabled()` 门控：关闭时热路径只剩
 *     一次 relaxed 布尔读 + 分支；开启时每轮多两次 `clock_gettime`（§10.2 要求
 *     "性能报告注明诊断是否开启"）。开关是进程内状态，由采集器按运行配置设置。
 *   · 所有计数器是**进程内单例**（故意泄漏的静态对象），随进程存活；跨进程汇总由
 *     采集器把每个进程导出的 JSON 合并完成，不在共享内存里做加法。
 *   · 语义：计数 `xxx_bytes` 是"应用逻辑载荷字节"，`xxx_wire_bytes` 才是传输字节；
 *     两者不得混用（§4 W02 要求记录实际应用字节与传输字节）。
 *
 * 与既有实现的边界（不重复造轮子）：
 *   · `RecvWorkerStats`（include/dzIPC/threepools/recv_worker.h）已经是 worker 侧
 *     的唯一事实来源；本注册表是**采集/汇总口径**，W06 把 worker 的增量灌进这里，
 *     或由采集器直接读 `RecvWorkerPool::stats()` 后映射到同名计数器。两者数值应
 *     能互相核对（见 doc 的映射表）。
 *   · 本文件不修改任何既有头文件，属于纯新增的测量层。
 */
#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

#include "dzIPC/measure/monotonic_clock.h"

namespace dzIPC {
namespace measure {

/* 计数器登记表。**只允许追加**：枚举数值进 JSON 字段名与历史对比，不改语义。 */
enum class CounterId : std::size_t
{
    /* ---- 数据路径 (§10.7: 传输机制 / 完整读取 / 生产到消费 三条路径各计) ---- */
    tlv_messages = 0,          ///< TLV 物化路径交付的消息数
    dzflat_a_messages,         ///< DZFlat A（对象→共享段一次复制）交付的消息数
    dzflat_b_messages,         ///< DZFlat B（应用原地构造借样）交付的消息数
    tlv_bytes,                 ///< TLV 路径应用逻辑载荷字节
    dzflat_a_bytes,
    dzflat_b_bytes,
    tlv_wire_bytes,            ///< TLV 实际传输字节（含分片/头部）
    dzflat_wire_bytes,

    /* ---- 兼容回退 (§13.3 fallback_activated) ---- */
    fallback_total,            ///< 回退总次数
    fallback_backend_unavailable,  ///< wait-set 后端不可用（显式回退信号）
    fallback_capacity_full,        ///< 容量满导致的回退
    fallback_type_incompatible,    ///< schema/类型不兼容回退
    fallback_oversized,            ///< 超尺寸档/超预算回退
    fallback_pool_exhausted,       ///< chunk 池耗尽回退

    /* ---- 注册与容量 (§13.2 第 1/3 条) ---- */
    registration_attempts,
    registration_ok,
    registration_failed,
    registration_rejected,     ///< 首个失败资源: 注册被拒
    registration_duplicate,    ///< 同 worker 重复注册
    registration_busy,         ///< 兼容收包线程/其它 owner 正在 recv
    registration_stopped,      ///< worker 未 start 或已 stop
    registration_invalid_token,
    registration_invalid_route,
    wait_set_full,             ///< §10.5: 每 worker 127 token 边界
    wait_token_invalid,
    fd_limit,                  ///< §4 W07: fd 越界/EMFILE
    capacity_first_failed_resource_recorded,  ///< 是否已登记首个失败资源 (0/1 仪表)

    /* ---- 池 / 队列 / 背压 (§4 W09, §13.3) ---- */
    chunk_exhausted,           ///< chunk 池耗尽
    chunk_alloc_failed,        ///< chunk 申请失败（含 loan 失败）
    queue_evicted,             ///< 队列淘汰条数
    queue_backpressure,        ///< 队列背压触发次数
    generation_mismatch,       ///< 旧 generation token 被拒
    publish_blocked,           ///< publish 阻塞次数（定速发送计划必须记录）
    publish_failed,
    rx_timeout,                ///< 接收超时（不是错误，但必须可核算）

    /* ---- worker 每轮扫描 (§10.2，诊断门控) ---- */
    scan_rounds,               ///< 扫描轮数
    scanned_routes_total,      ///< 每轮扫描 route 数之和（除以 scan_rounds = 均值）
    scan_time_ns_total,        ///< 扫描耗时之和 (ns)
    wait_timeout_count,        ///< 等待超时次数
    ready_observed,            ///< 观察到"有就绪"的次数
    deferred_depth_last,       ///< 最近一轮 deferred 深度 (仪表)
    deferred_depth_max,        ///< deferred 深度最大值
    deferred_drains,
    budget_yields,             ///< 预算耗尽让出次数
    idle_exits,                ///< §10.1: worker 空闲退出次数
    thread_restarts,           ///< §10.1: worker 线程按需拉起次数
    recv_once_calls,
    recv_once_over_budget,     ///< §10.3: 单次 recv_once 超过时间预算的次数

    /* ---- 顺序/完整性核算 (§13.1) ---- */
    seq_monotonic_ok,
    seq_out_of_order,
    seq_duplicate,
    seq_lost,
    payload_checksum_ok,
    payload_checksum_bad,

    /* ---- W09/t19 追加（冻结规则：新增一律追加，不改既有枚举值与语义）----
     *
     * 三种**不得混算**的语义（方案 §4 W09 第 5 条 + 队长裁决 D-16）：
     *
     *   (a) **路径选择**（`path_selection_*`）：DZFlat **未启用**或类型本就不支持时的
     *       正常 TLV 路径。**不是回退**，⛔ 一个 `fallback_*` 计数都不得 +1。它的条数
     *       就在既有 `tlv_messages` 里（三路径条数的分母）；本组只补两个**可精确区分**
     *       的原因，供报告解释 tlv 档的来源构成。
     *   (b) **真正的回退**（`fallback_*`，含追加的 `fallback_reason_unknown`）：DZFlat
     *       **已启用且尝试过**，但因池耗尽/容量不满之外的失败而降级到 TLV ⇒
     *       `fallback_total` 与**对应原因**同时 +1（"首个失败资源"语义）。
     *   (c) **B 贷款失败**（`borrow_failed_*`）：应用 `loan()`/`publish_loaned()` 失败。
     *       ⛔ **绝不**计入 (b) 的 `fallback_*`：借样契约（`loaned_message.h` 三条）明写
     *       "失败是常态、由应用回退到普通 publish"，那一次回退已在 A/TLV 的调用点记过
     *       一次；这里再记会把 `fallback/(dzflat+fallback)` 比值算歪（W08 已登记同一纪律）。
     *
     * 分类实现见本文件末尾的 `DzFlatAttempt`/`classify_dzflat_attempt()`。 */
    path_selection_dzflat_disabled,     ///< (a) 开关关闭 ⇒ 走 TLV（正确值，不是回退）
    path_selection_type_unsupported,    ///< (a) 关闭**且**类型无平坦布局 ⇒ 走 TLV（开启也无收益）
    fallback_reason_unknown,            ///< (b) 确认是回退但原因无法精确判定（⛔不猜分）
    borrow_failed_no_receiver,          ///< (c) B 借样失败：无接收方
    borrow_failed_pool_exhausted,       ///< (c) B 借样失败：chunk 池耗尽
    borrow_failed_oversized,            ///< (c) B 借样失败：超变长预算（finalize 失败）
    borrow_failed_publish,              ///< (c) B 借样失败：publish_loan 失败
    borrow_failed_reason_unknown,       ///< (c) B 借样失败但原因无法精确判定

    /* ---- W03/t44 追加（R0-9 / R0-10 结束值接口；规格见 W06/deferred_结束值接口_规格提交W03.md）----
     *
     * ⛔ 一律 `diagnostics_only = true`（与 scan 族一致）；关闭诊断时为「**未采集**」，
     *    其数值 0 **不得**读作实测 0（见 `CounterRegistry::to_json()` 的
     *    `diagnostics_collection` 段与 `ScanRoundResult::elapsed_ns_collected`）。
     *
     * ⛔ 三条**不得混算**的铁律（R0-8/R0-9/R0-10 + 规格 S4）：
     *   1. `ready_observed`（**轮数**：本轮发现 ≥1 条就绪的轮数）与
     *      `scan_ready_routes_total`（**route 数**：逐轮新入队 route 数之和）**量纲不同，不得相加**；
     *      两者之比才是"平均每轮新入队 route 数"。
     *   2. `deferred_depth_last`（**入队前**快照）与 `deferred_depth_after_last`（**扫描后**快照）
     *      **语义不同、不得互相覆盖、不得混列一列**。
     *   3. 后两个 `*_last/_max` 是**全局 gauge**：多 worker 并发时只保留"最后写入者"的值
     *      ⇒ ⛔ **不得当作全池总量**（R0-10：现有 gauge 只保留兼容、不承载判据）。
     *      可安全求和的深度量**只有** `deferred_depth_after_total`（逐轮 Σ，可按 worker 相加）；
     *      全池**当前**总深度 = Σ(每 worker 的 `_after_last`)，由 `ScanRoundAccumulator` 提供。 */
    scan_ready_routes_total,        ///< Σ 每轮新入队 route 数（route 数，≠ ready_observed 的轮数）
    deferred_depth_after_last,      ///< 最近一轮**扫描后** deferred 深度（全局 gauge，⛔非全池总量）
    deferred_depth_after_max,       ///< 扫描后深度最大值（全局 gauge，⛔非全池总量）
    deferred_depth_after_total,     ///< Σ 每轮扫描后深度（可直接按 worker 相加求平均）

    count
};

inline constexpr std::size_t kCounterCount = static_cast<std::size_t>(CounterId::count);


/* 计数器元数据：名称/单位/类别/说明。schema 与文档由同一张表生成，避免文档漂移。 */
struct CounterMeta
{
    CounterId   id;
    const char* name;
    const char* unit;
    const char* category;
    const char* description;
    bool        diagnostics_only;
};

inline const std::array<CounterMeta, kCounterCount>& counter_table()
{
    static const std::array<CounterMeta, kCounterCount> kTable = {{
        {CounterId::tlv_messages, "tlv_messages", "count", "path", "TLV 物化路径交付消息数", false},
        {CounterId::dzflat_a_messages, "dzflat_a_messages", "count", "path", "DZFlat A (对象->共享段一次复制) 交付消息数", false},
        {CounterId::dzflat_b_messages, "dzflat_b_messages", "count", "path", "DZFlat B (应用原地构造借样) 交付消息数", false},
        {CounterId::tlv_bytes, "tlv_bytes", "byte", "path", "TLV 应用逻辑载荷字节", false},
        {CounterId::dzflat_a_bytes, "dzflat_a_bytes", "byte", "path", "DZFlat A 应用逻辑载荷字节", false},
        {CounterId::dzflat_b_bytes, "dzflat_b_bytes", "byte", "path", "DZFlat B 应用逻辑载荷字节", false},
        {CounterId::tlv_wire_bytes, "tlv_wire_bytes", "byte", "path", "TLV 实际传输字节(含分片/头部)", false},
        {CounterId::dzflat_wire_bytes, "dzflat_wire_bytes", "byte", "path", "DZFlat 实际传输字节", false},

        {CounterId::fallback_total, "fallback_total", "count", "fallback", "兼容回退总次数", false},
        {CounterId::fallback_backend_unavailable, "fallback_backend_unavailable", "count", "fallback", "wait-set 后端不可用导致的回退", false},
        {CounterId::fallback_capacity_full, "fallback_capacity_full", "count", "fallback", "容量满导致的回退", false},
        {CounterId::fallback_type_incompatible, "fallback_type_incompatible", "count", "fallback", "类型/schema 不兼容回退", false},
        {CounterId::fallback_oversized, "fallback_oversized", "count", "fallback", "超尺寸档/超预算回退", false},
        {CounterId::fallback_pool_exhausted, "fallback_pool_exhausted", "count", "fallback", "chunk 池耗尽回退", false},

        {CounterId::registration_attempts, "registration_attempts", "count", "registration", "注册尝试次数", false},
        {CounterId::registration_ok, "registration_ok", "count", "registration", "注册成功次数", false},
        {CounterId::registration_failed, "registration_failed", "count", "registration", "注册失败总数", false},
        {CounterId::registration_rejected, "registration_rejected", "count", "registration", "注册被拒(首个失败资源)", false},
        {CounterId::registration_duplicate, "registration_duplicate", "count", "registration", "同 worker 重复注册", false},
        {CounterId::registration_busy, "registration_busy", "count", "registration", "兼容线程/其它 owner 占用", false},
        {CounterId::registration_stopped, "registration_stopped", "count", "registration", "worker 未启动/已停止", false},
        {CounterId::registration_invalid_token, "registration_invalid_token", "count", "registration", "读等待 token 无效", false},
        {CounterId::registration_invalid_route, "registration_invalid_route", "count", "registration", "route == nullptr", false},
        {CounterId::wait_set_full, "wait_set_full", "count", "registration", "每 worker wait token 容量满", false},
        {CounterId::wait_token_invalid, "wait_token_invalid", "count", "registration", "wait token 失效", false},
        {CounterId::fd_limit, "fd_limit", "count", "registration", "fd 越界/EMFILE", false},
        {CounterId::capacity_first_failed_resource_recorded, "capacity_first_failed_resource_recorded", "bool", "registration", "是否已登记首个失败资源(仪表)", false},

        {CounterId::chunk_exhausted, "chunk_exhausted", "count", "capacity", "chunk 池耗尽次数", false},
        {CounterId::chunk_alloc_failed, "chunk_alloc_failed", "count", "capacity", "chunk 申请失败次数", false},
        {CounterId::queue_evicted, "queue_evicted", "count", "capacity", "队列淘汰条数", false},
        {CounterId::queue_backpressure, "queue_backpressure", "count", "capacity", "队列背压触发次数", false},
        {CounterId::generation_mismatch, "generation_mismatch", "count", "capacity", "旧 generation token 被拒次数", false},
        {CounterId::publish_blocked, "publish_blocked", "count", "capacity", "publish 阻塞次数", false},
        {CounterId::publish_failed, "publish_failed", "count", "capacity", "publish 失败次数", false},
        {CounterId::rx_timeout, "rx_timeout", "count", "capacity", "接收超时次数", false},

        {CounterId::scan_rounds, "scan_rounds", "count", "scan", "collect_pending 扫描轮数", true},
        {CounterId::scanned_routes_total, "scanned_routes_total", "count", "scan", "每轮扫描 route 数之和", true},
        {CounterId::scan_time_ns_total, "scan_time_ns_total", "ns", "scan", "扫描耗时之和", true},
        {CounterId::wait_timeout_count, "wait_timeout_count", "count", "scan", "等待超时次数", true},
        {CounterId::ready_observed, "ready_observed", "count", "scan", "观察到的就绪次数", true},
        {CounterId::deferred_depth_last, "deferred_depth_last", "count", "scan", "最近一轮 deferred 深度(仪表)", true},
        {CounterId::deferred_depth_max, "deferred_depth_max", "count", "scan", "deferred 深度最大值", true},
        {CounterId::deferred_drains, "deferred_drains", "count", "scan", "deferred FIFO 排空次数", true},
        {CounterId::budget_yields, "budget_yields", "count", "scan", "预算耗尽让出次数", true},
        {CounterId::idle_exits, "idle_exits", "count", "scan", "worker 空闲退出次数", true},
        {CounterId::thread_restarts, "thread_restarts", "count", "scan", "worker 线程按需拉起次数", true},
        {CounterId::recv_once_calls, "recv_once_calls", "count", "scan", "recv_once 调用次数", true},
        {CounterId::recv_once_over_budget, "recv_once_over_budget", "count", "scan", "单次 recv_once 超预算次数", true},

        {CounterId::seq_monotonic_ok, "seq_monotonic_ok", "count", "integrity", "序号连续的样本数", false},
        {CounterId::seq_out_of_order, "seq_out_of_order", "count", "integrity", "乱序样本数", false},
        {CounterId::seq_duplicate, "seq_duplicate", "count", "integrity", "重复样本数", false},
        {CounterId::seq_lost, "seq_lost", "count", "integrity", "丢失样本数", false},
        {CounterId::payload_checksum_ok, "payload_checksum_ok", "count", "integrity", "载荷校验通过数", false},
        {CounterId::payload_checksum_bad, "payload_checksum_bad", "count", "integrity", "载荷校验失败数", false},

        /* ---- W09/t19 追加（语义见枚举处注释）---- */
        {CounterId::path_selection_dzflat_disabled, "path_selection_dzflat_disabled", "count", "path", "(a) 路径选择: 开关关闭走 TLV(不是回退)", false},
        {CounterId::path_selection_type_unsupported, "path_selection_type_unsupported", "count", "path", "(a) 路径选择: 关闭且类型无平坦布局(开启亦无收益, 不是回退)", false},
        {CounterId::fallback_reason_unknown, "fallback_reason_unknown", "count", "fallback", "(b) 确认回退但原因无法精确判定(不猜分)", false},
        {CounterId::borrow_failed_no_receiver, "borrow_failed_no_receiver", "count", "borrow", "(c) B 借样失败: 无接收方", false},
        {CounterId::borrow_failed_pool_exhausted, "borrow_failed_pool_exhausted", "count", "borrow", "(c) B 借样失败: chunk 池耗尽", false},
        {CounterId::borrow_failed_oversized, "borrow_failed_oversized", "count", "borrow", "(c) B 借样失败: 超变长预算", false},
        {CounterId::borrow_failed_publish, "borrow_failed_publish", "count", "borrow", "(c) B 借样失败: publish_loan 失败", false},
        {CounterId::borrow_failed_reason_unknown, "borrow_failed_reason_unknown", "count", "borrow", "(c) B 借样失败: 原因无法精确判定", false},

        /* ---- W03/t44 追加（R0-9/R0-10 结束值接口）---- */
        {CounterId::scan_ready_routes_total, "scan_ready_routes_total", "count", "scan",
         "Σ 每轮新入队 route 数(route 数); /scan_rounds = 平均每轮新入队 route 数; "
         "量纲与 ready_observed(轮数)不同, 不得相加", true},
        {CounterId::deferred_depth_after_last, "deferred_depth_after_last", "count", "scan",
         "最近一轮扫描后 deferred 深度(全局 gauge=任一 worker 最后写入者的值); "
         "与 deferred_depth_last(入队前)语义不同、不得互相覆盖; 不得当全池总量", true},
        {CounterId::deferred_depth_after_max, "deferred_depth_after_max", "count", "scan",
         "扫描后 deferred 深度最大值(全局 gauge=任一 worker 的值); 不得当全池总量", true},
        {CounterId::deferred_depth_after_total, "deferred_depth_after_total", "count", "scan",
         "Σ 每轮扫描后深度(唯一可安全按 worker 相加的深度量); /scan_rounds = 平均扫描后深度", true},
    }};
    return kTable;
}

inline const char* counter_name(CounterId id) noexcept
{
    return counter_table()[static_cast<std::size_t>(id)].name;
}
inline const char* counter_unit(CounterId id) noexcept
{
    return counter_table()[static_cast<std::size_t>(id)].unit;
}
inline const char* counter_category(CounterId id) noexcept
{
    return counter_table()[static_cast<std::size_t>(id)].category;
}
inline const char* counter_description(CounterId id) noexcept
{
    return counter_table()[static_cast<std::size_t>(id)].description;
}
inline bool counter_is_diagnostics_only(CounterId id) noexcept
{
    return counter_table()[static_cast<std::size_t>(id)].diagnostics_only;
}
/* 诊断关闭时 count 族一律「**未采集**」而不是"实测 0"。
 * 判据：`counter_is_diagnostics_only(id) == true && !diagnostics_enabled()` ⇒ 未采集。
 * ⛔ 报告/脚本不得把此时的 0 当读数（见下方 `CounterRegistry::to_json()` 的
 * `diagnostics_collection` 段与 `ScanRoundResult::elapsed_ns_collected`）。 */
inline bool counter_is_uncollected(CounterId id, bool diagnostics_enabled) noexcept
{
    return counter_is_diagnostics_only(id) && !diagnostics_enabled;
}


/* ===========================================================================
 * W09/t19：DZFlat 发布尝试的**三种语义分类器**（唯一实现，调用点只负责填证据）
 * ---------------------------------------------------------------------------
 * 为什么要有它：`try_publish_dzflat()` 返回 `bool`，**无法**从返回值区分
 *   (a) 路径选择（开关关/类型不支持 ⇒ 本来就不该走 DZFlat）、
 *   (b) 真回退（启用了、尝试了、降级了）、
 *   (c) B 借样失败（借样契约里"失败是常态"，由应用回退）。
 * 现场只能看"有没有走成 DZFlat"，于是 W08 的 tlv 档出现了
 * `fallback_total=0` 与 `DzFlatFallbackCount=120` 并存的歧义（两者都不假、含义不同）。
 *
 * 纪律（队长 D-16 精确化）：
 *   · DZFlat **关闭**时一律**不计入任何** `fallback_*`；此时"类型不支持"也不计 ——
 *     因为"预期走 DZFlat"这个前提不成立。⇒ tlv 档（开关 OFF）期望 `fallback_total=0`，
 *     这是**正确值**而不是"未接线"。
 *   · DZFlat **开启**且类型不支持 ⇒ 计入 `fallback_type_incompatible`（一次真实降级）。
 *   · 无法精确判定 ⇒ 记 `fallback_reason_unknown` / `borrow_failed_reason_unknown`，
 *     ⛔ **不得猜分**。
 *
 * 可判定性论证（选 (i)"不改签名 + 旁用局部状态"，理由见交付文档 §5.2）：
 *   调用点**持有**比 `bool` 更多的信息 —— 开关值、`msg->dzflat_supported()`、
 *   `dzflat_size()`、`publisher_->recv_count()`、以及 **B 路径额外的
 *   `lo.valid()`/`finalize()`/`publish_loan()` 三个返回值**。因此除两处外都能精确判定。
 *   无法精确判定的两类（如实登记，不猜）：
 *     ① `loan()` 返回无效：其内部先判 `ready_sending()`（单生产端 flag）再判
 *        `connections()==0` 再判池，**这三者在 bool 返回值上不可区分** ⇒ 归
 *        `borrow_failed_reason_unknown`（若要精确区分须扩 `ipc::loan()` 的返回通道，
 *        属 libipc 接口变更，超出本任务授权）；
 *     ② `try_publish_dzflat` 的 `dzflat_write` 失败：仅意味着"段写不进去"，
 *        究竟是超预算还是类型布局自相矛盾不可区分 ⇒ 归 `fallback_reason_unknown`。
 * =========================================================================== */

/* 一次发布尝试观察到的事实。字段全部由调用点**零成本**取得（无新 API、无锁、无分配）。 */
struct DzFlatAttempt
{
    bool dzflat_enabled{false};       ///< dzIPC::IsDzFlatEnabled()
    bool type_supported{false};       ///< msg->dzflat_supported()
    bool had_receiver{true};          ///< publisher_->recv_count() > 0
    bool dzflat_delivered{false};     ///< 本次是否真的走了 DZFlat（成功）

    /* ---- B 路径专属（A/TLV 路径留默认即可）---- */
    bool borrow_requested{false};     ///< 本次确实调用了 loan()
    bool borrow_ok{false};            ///< loan() 返回有效（且已答 ok()）
    bool write_ok{false};             ///< finalize()/dzflat_write 成功
    bool publish_loan_ok{false};      ///< publish_loan() 成功

    /* 段大小证据（供 oversize 判定与报告；0 = 未知） */
    std::uint32_t need_bytes{0};
    std::uint32_t capacity_bytes{0};
};

/* 分类结果：恰好落到三类之一。`
 * kind` 的三种取值互斥，调用方据它决定记哪一组计数。 */
enum class DzFlatOutcome : std::uint8_t
{
    dzflat_delivered = 0,   ///< 走了 DZFlat（成功）—— 不记任何 fallback
    path_selection,         ///< (a) 正常 TLV 路径（开关关 / 类型不支持）—— **不记 fallback**
    fallback,               ///< (b) 真回退 —— 记 fallback_total + 原因
    borrow_failed,          ///< (c) B 借样失败 —— 记 borrow_failed_*，**⛔不记 fallback_***
};

struct DzFlatClassification
{
    DzFlatOutcome outcome{DzFlatOutcome::path_selection};
    CounterId     detail{CounterId::path_selection_dzflat_disabled};  ///< 原因 ID（零成本常量）
    bool          is_fallback{false};   ///< = (outcome == fallback)，供对账
};

/* 纯函数、无副作用（由调用方落计数）⇒ 可被单测穷举覆盖。 */
inline DzFlatClassification classify_dzflat_attempt(const DzFlatAttempt& a) noexcept
{
    if (a.dzflat_delivered)
    {
        return {DzFlatOutcome::dzflat_delivered, CounterId::dzflat_a_messages, false};
    }
    /* ---- (c) B 借样：先把"借样成功的三个条件全真"归一成 delivered（不依赖调用点
     * 是否记得回填 dzflat_delivered —— 分类器必须自洽，否则漏填会静默记成 fallback）---- */
    if (a.borrow_requested && a.borrow_ok && a.write_ok && a.publish_loan_ok)
    {
        return {DzFlatOutcome::dzflat_delivered, CounterId::borrow_failed_reason_unknown, false};
    }
    /* ---- (c) B 借样失败：只要**尝试过借样**且没走成，就是借样失败类别 ---- */
    if (a.borrow_requested)
    {
        if (!a.borrow_ok)
        {
            /* `loan()` 的 bool 无法区分「单生产端 flag 占用 / 无接收方 / 池耗尽」三态。
             * 若调用点另有证据（recv_count>0 且池空），调用方应把它填进 had_receiver 并
             * 用下面的分支；否则如实归 unknown。 */
            if (!a.had_receiver)
            {
                return {DzFlatOutcome::borrow_failed, CounterId::borrow_failed_no_receiver, false};
            }
            if (a.capacity_bytes != 0 && a.need_bytes > a.capacity_bytes)
            {
                return {DzFlatOutcome::borrow_failed, CounterId::borrow_failed_oversized, false};
            }
            return {DzFlatOutcome::borrow_failed, CounterId::borrow_failed_reason_unknown, false};
        }
        if (!a.write_ok)
        {
            return {DzFlatOutcome::borrow_failed, CounterId::borrow_failed_oversized, false};
        }
        return {DzFlatOutcome::borrow_failed, CounterId::borrow_failed_publish, false};
    }

    /* ---- (a) 路径选择：DZFlat **关闭** ⇒ ⛔ 不计入任何 fallback ----
     *
     * 关闭时再分两种（**二选一**，不是两个都记），信息量更大：
     *   · 类型本就无平坦布局 ⇒ `path_selection_type_unsupported`
     *     （对运维的直接含义：**即使开启 DZFlat，这些消息仍会走 TLV**，A 级推广范围要排除它们）
     *   · 类型支持 ⇒ `path_selection_dzflat_disabled`（开启即有收益，属"待启用"集合）
     * 两者合计 = 关闭态下的 TLV 条数，可与 `tlv_messages` 对账。 */
    if (!a.dzflat_enabled)
    {
        return {DzFlatOutcome::path_selection,
                a.type_supported ? CounterId::path_selection_dzflat_disabled
                                 : CounterId::path_selection_type_unsupported,
                false};
    }
    if (!a.type_supported)
    {
        /* DZFlat **已启用**且类型不支持 ⇒ 这是一次真实降级（队长 D-16 精确化：
         * 此时"预期走 DZFlat"成立，类型不兼容是一次真实回退）。 */
        return {DzFlatOutcome::fallback, CounterId::fallback_type_incompatible, true};
    }
    /* ---- (b) 真回退：启用了、类型支持、但没走成 ---- */
    if (!a.had_receiver)
    {
        return {DzFlatOutcome::fallback, CounterId::fallback_capacity_full, true};
    }
    if (a.capacity_bytes != 0 && a.need_bytes > a.capacity_bytes)
    {
        return {DzFlatOutcome::fallback, CounterId::fallback_oversized, true};
    }
    /* 「池耗尽」在 A 路径上不可从 bool 判定（`loan` 的 `ready_sending`/无接收方/池 三态
     * 混在一个 false 里）⇒ 如实记 unknown，⛔ 不猜成 pool_exhausted。 */
    return {DzFlatOutcome::fallback, CounterId::fallback_reason_unknown, true};
}

/* 计数快照：纯值数组 + 便捷取值。 */
struct CounterSnapshot
{
    std::array<std::uint64_t, kCounterCount> values{};
    std::uint64_t get(CounterId id) const noexcept
    {
        return values[static_cast<std::size_t>(id)];
    }
};

/* 进程内计数注册表。故意泄漏的单例：热路径被模块静态对象引用，必须活得比它们久。 */
class CounterRegistry
{
public:
    static CounterRegistry& instance() noexcept
    {
        static CounterRegistry* kInstance = new CounterRegistry();
        return *kInstance;
    }

    void inc(CounterId id, std::uint64_t n = 1) noexcept
    {
        counters_[static_cast<std::size_t>(id)].fetch_add(n, std::memory_order_relaxed);
    }
    void add(CounterId id, std::uint64_t n) noexcept { inc(id, n); }
    void set_gauge(CounterId id, std::uint64_t v) noexcept
    {
        counters_[static_cast<std::size_t>(id)].store(v, std::memory_order_relaxed);
    }
    void max_gauge(CounterId id, std::uint64_t v) noexcept
    {
        std::uint64_t cur = counters_[static_cast<std::size_t>(id)].load(std::memory_order_relaxed);
        while (v > cur && !counters_[static_cast<std::size_t>(id)].compare_exchange_weak(
                   cur, v, std::memory_order_relaxed)) {}
    }
    std::uint64_t get(CounterId id) const noexcept
    {
        return counters_[static_cast<std::size_t>(id)].load(std::memory_order_relaxed);
    }

    /* §10.2：低开销常驻计数与详细诊断采样分开。热路径只读这个 relaxed 布尔。 */
    void set_diagnostics_enabled(bool on) noexcept
    {
        diagnostics_enabled_.store(on, std::memory_order_relaxed);
    }
    bool diagnostics_enabled() const noexcept
    {
        return diagnostics_enabled_.load(std::memory_order_relaxed);
    }

    void reset() noexcept
    {
        for (auto& c : counters_) c.store(0, std::memory_order_relaxed);
    }

    CounterSnapshot snapshot() const noexcept
    {
        CounterSnapshot s;
        for (std::size_t i = 0; i < kCounterCount; ++i)
            s.values[i] = counters_[i].load(std::memory_order_relaxed);
        return s;
    }

    /* 上一份快照的差值：采集器按窗口差分，避免把进程启动前的计数算进来。 */
    static CounterSnapshot diff(const CounterSnapshot& before, const CounterSnapshot& after) noexcept
    {
        CounterSnapshot d;
        for (std::size_t i = 0; i < kCounterCount; ++i) {
            d.values[i] = after.values[i] >= before.values[i]
                        ? after.values[i] - before.values[i] : after.values[i];
        }
        return d;
    }

    std::string to_json() const
    {
        const CounterSnapshot s = snapshot();
        std::string out;
        out.reserve(kCounterCount * 48 + 64);
        out += "{\n";
        out += "  \"kind\": \"counter_snapshot\",\n";
        out += "  \"clock_source\": \""; out += clock_source_name(); out += "\",\n";
        out += "  \"diagnostics_enabled\": ";
        out += diagnostics_enabled() ? "true" : "false";
        out += ",\n";
        /* 采集状态声明（⛔ 诊断关闭时 diagnostics_only 的 0 不是实测值）：
         *   "collected"   —— 诊断开启，数值为实测
         *   "uncollected" —— 诊断关闭，diagnostics_only 的计数器**未采集**，其 0 不得当读数
         *   "mixed"       —— 本快照中部分计数器属诊断门控（逐 id 判定见上表 diagnostics_only） */
        out += "  \"diagnostics_collection\": \"";
        out += diagnostics_enabled() ? "collected" : "uncollected";
        out += "\",\n  \"diagnostics_only_counter_names\": ";
        out += diagnostics_only_names_json();
        out += ",\n  \"counters\": {\n";
        for (std::size_t i = 0; i < kCounterCount; ++i) {
            const CounterMeta& m = counter_table()[i];
            out += "    \"";
            out += m.name;
            out += "\": ";
            out += std::to_string(s.values[i]);
            if (i + 1 < kCounterCount) out += ",";
            out += "\n";
        }
        out += "  }\n}\n";
        return out;
    }

    /* 诊断门控计数器名清单（供脚本机械判定"该 0 是否可读"）。 */
    static std::string diagnostics_only_names_json()
    {
        std::string o = "[";
        bool first = true;
        for (std::size_t i = 0; i < kCounterCount; ++i) {
            if (!counter_table()[i].diagnostics_only) continue;
            if (!first) o += ", ";
            first = false;
            o += "\"";
            o += counter_table()[i].name;
            o += "\"";
        }
        o += "]";
        return o;
    }

    static std::string csv_header()
    {
        std::string h = "run_id,window_start_ns,window_end_ns,diagnostics_enabled";
        for (std::size_t i = 0; i < kCounterCount; ++i) {
            h += ",";
            h += counter_table()[i].name;
        }
        return h;
    }

    std::string csv_row(const std::string& run_id,
                        std::uint64_t window_start_ns,
                        std::uint64_t window_end_ns) const
    {
        const CounterSnapshot s = snapshot();
        std::string r = run_id;
        r += "," + std::to_string(window_start_ns);
        r += "," + std::to_string(window_end_ns);
        r += diagnostics_enabled() ? ",1" : ",0";
        for (std::size_t i = 0; i < kCounterCount; ++i) {
            r += "," + std::to_string(s.values[i]);
        }
        return r;
    }

    bool write_json_file(const std::string& path) const
    {
        std::FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        const std::string j = to_json();
        const std::size_t n = std::fwrite(j.data(), 1, j.size(), f);
        std::fclose(f);
        return n == j.size();
    }

private:
    CounterRegistry() noexcept
    {
        for (auto& c : counters_) c.store(0, std::memory_order_relaxed);
        diagnostics_enabled_.store(false, std::memory_order_relaxed);
    }
    std::array<std::atomic<std::uint64_t>, kCounterCount> counters_{};
    std::atomic<bool> diagnostics_enabled_{false};
};

/* ===========================================================================
 * 计数落点（调用点只调这一个函数，语义与"是否真的计数"完全一致）
 * ===========================================================================
 * ⛔ `path_selection_*` 也计数，但它**不是回退** —— 它只解释 `tlv_messages` 的来源构成。
 * ⛔ `fallback_total` 只在 outcome == fallback 时 +1，且**同时**给原因 +1
 *    （"首个失败资源"语义：原因维度与总量维度各记一份，不是二选一）。
 * ⛔ `borrow_failed_*` 绝不碰 `fallback_total`。 */
inline void note_dzflat_attempt(const DzFlatAttempt& a) noexcept
{
    const DzFlatClassification c = classify_dzflat_attempt(a);
    CounterRegistry& r = CounterRegistry::instance();
    switch (c.outcome)
    {
    case DzFlatOutcome::dzflat_delivered:
        return;   // 路径条数由 NoteDzFlatPathDelivered 记（W08），此处不重复
    case DzFlatOutcome::path_selection:
        r.inc(c.detail);
        return;
    case DzFlatOutcome::fallback:
        r.inc(CounterId::fallback_total);
        r.inc(c.detail);
        return;
    case DzFlatOutcome::borrow_failed:
        r.inc(c.detail);
        return;
    }
}

/* ---------------------------------------------------------------------------
 * W09/t19 (c) 的**专用落点**：B 借样失败（`LoanedMessage` 的三个出口）。
 *
 * 为什么单独一个函数而不是让调用点自己拼 `DzFlatAttempt`：B 路径在**公开头文件的
 * 模板里**（`shm_pub_sub_ipc.h::publish_loaned`），那里只有三个 bool 可用，且必须
 * 零分配、零日志。本函数把它们归一成 (c) 类别 —— 与 `classify_dzflat_attempt` 的
 * `borrow_failed` 分支**同一张判定表**，避免两处各写一套而漂移。
 *
 * 可判定性（如实登记）：
 *   · finalize_ok=false ⇒ 超变长预算（调用点语义确定）⇒ borrow_failed_oversized；
 *   · finalize_ok=true 且 publish_ok=false ⇒ publish_loan 失败 ⇒ borrow_failed_publish；
 *   · 两者皆 false 且 had_receiver=false ⇒ borrow_failed_no_receiver；
 *   · 其余 ⇒ borrow_failed_reason_unknown（`loan()` 的 bool 判不出
 *     「单生产端 flag 占用 / 无接收方 / 池耗尽」三态，须扩 `ipc::loan()` 返回通道才能精确
 *     区分 —— 属 libipc 接口变更，超出 t19 授权）。
 * ⛔ 本函数**绝不**碰 `fallback_total`。 */
inline void note_dzflat_borrow_failed(bool had_receiver, bool finalize_ok, bool publish_ok) noexcept
{
    CounterRegistry& r = CounterRegistry::instance();
    if (finalize_ok && !publish_ok)
    {
        r.inc(CounterId::borrow_failed_publish);
        return;
    }
    if (!finalize_ok && !publish_ok)
    {
        /* 两个出口在这里合流（lo 无效 / finalize 失败）—— 用 had_receiver 再分一次：
         * 无接收方时最可能的原因是无接收方；否则如实 unknown（可能是超预算，也可能是
         * loan 三态中的任一个，本层拿不到更细的证据）。 */
        r.inc(had_receiver ? CounterId::borrow_failed_reason_unknown
                           : CounterId::borrow_failed_no_receiver);
        return;
    }
    r.inc(CounterId::borrow_failed_reason_unknown);
}

/* ---------------------------------------------------------------------------
 * 记录入口。W06/W08/W09 等模块在自己的调用点引用这些宏即可；热路径无锁无分配。
 * 规范（写进 W03 字段定义文档）：
 *   · 路径计数：每成功交付一条消息，正好 +1 一次（重试只加 retry，不重复加 path）。
 *   · 回退计数：fallback_total 与具体原因同时 +1（原因可多个时以"首个失败资源"为准）。
 *   · 诊断计数：只在 diagnostics_enabled() 时累加；采集器在 counters.json 里写明
 *     诊断开关状态，性能表必须注明。
 * ------------------------------------------------------------------------- */
#define DZIPC_MEASURE_INC(id) \
    ::dzIPC::measure::CounterRegistry::instance().inc((id), 1u)
#define DZIPC_MEASURE_ADD(id, n) \
    ::dzIPC::measure::CounterRegistry::instance().inc((id), static_cast<std::uint64_t>(n))
#define DZIPC_MEASURE_SET_GAUGE(id, v) \
    ::dzIPC::measure::CounterRegistry::instance().set_gauge((id), static_cast<std::uint64_t>(v))
#define DZIPC_MEASURE_MAX_GAUGE(id, v) \
    ::dzIPC::measure::CounterRegistry::instance().max_gauge((id), static_cast<std::uint64_t>(v))
#define DZIPC_MEASURE_DIAG_INC(id)                                                 \
    do {                                                                           \
        ::dzIPC::measure::CounterRegistry& dzr =                                   \
            ::dzIPC::measure::CounterRegistry::instance();                         \
        if (dzr.diagnostics_enabled()) dzr.inc((id), 1u);                          \
    } while (0)
#define DZIPC_MEASURE_DIAG_ADD(id, n)                                              \
    do {                                                                           \
        ::dzIPC::measure::CounterRegistry& dzr =                                   \
            ::dzIPC::measure::CounterRegistry::instance();                         \
        if (dzr.diagnostics_enabled())                                             \
            dzr.inc((id), static_cast<std::uint64_t>(n));                          \
    } while (0)

/* ---------------------------------------------------------------------------
 * §10.2 每轮扫描的 RAII 观测（W03 提供实现；接线由接收池负责人完成）。
 *
 * 集成点（接收池侧，**接线由 W06/t42 owner 落地**）：
 *
 *   void RecvWorker::Impl::collect_pending() {
 *       std::lock_guard<std::mutex> lock(mtx);
 *       const std::size_t scanned = routes.size();     // 本轮遍历 route 数
 *       const std::size_t depth_in = deferred.size();  // 扫描前深度
 *       dzIPC::measure::ScanRoundScope scan(scanned, depth_in);
 *       std::size_t queued = 0;
 *       for (...) { ... ++queued; }
 *       // ------------------------------------------------------------------
 *       // 收尾值必须在遍历结束之后、且**在"扫描后深度"可观测的那一点**同一处取：
 *       const std::size_t depth_after = deferred.size();   // ← **扫描后**深度（R0-10）
 *       scan.finish(queued, depth_after);                  // 新增的结束值出参
 *       scan.set_ready(queued > 0);                        // 既有语义，不变
 *       // ------------------------------------------------------------------
 *   }
 *
 * 关闭诊断时构造 + 析构各只有一次 relaxed 布尔读（W03 实测 0.176 ns/轮），
 * 不取时钟、不写计数、不分配内存。
 *
 * ⛔ 与既有字段的兼容纪律（R0-8/R0-9 + 规格 S4）：`scan_rounds`、
 * `scanned_routes_total`、`scan_time_ns_total`、`ready_observed`、
 * `deferred_depth_last`、`deferred_depth_max` 的**语义与写入点一个字都不改**；
 * 本轮只是**追加**结束值通道。
 * ------------------------------------------------------------------------- */

/* 一次扫描的**结束值快照**（R0-9 的 `finish()` 出参形态 + R0-10 的按 worker 承载）。
 *
 * ⛔ 本结构体**不写全局计数**：它的用途是把「扫描后」的量交给每 worker 的
 * `RecvWorkerStats` 承接（那是常驻面，不受诊断门控），以及给调用点/探针做本地对账。
 * 全池总量 = Σ(每 worker 的对应字段)，⛔ 不得用任一全局 gauge 表达（R0-10）。 */
struct ScanRoundResult
{
    std::size_t   scanned_routes{0};          ///< 本轮遍历 route 数
    std::size_t   ready_routes{0};            ///< 本轮**新入队**的待处理 route 数
    std::size_t   deferred_depth_before{0};   ///< 扫描**前**深度（= 既有 deferred_depth_last 的口径）
    std::size_t   deferred_depth_after{0};    ///< 扫描**后**深度  ← R0-10 要的量
    /* 区间耗时（`ScanRoundScope` 构造 → `finish()`）。
     * ⛔ 与 `scan_time_ns_total` 不是同一区间，见文件末尾「scan_time 区间」冻结说明。
     * `elapsed_ns_collected == false` 表示**未采集**（诊断关闭），此时 `elapsed_ns == 0`
     * 是"未采集"而不是"零成本"，⛔ 不得当实测值引用。 */
    std::uint64_t elapsed_ns{0};
    bool          elapsed_ns_collected{false};
    bool          diagnostics_enabled{false};

    /* 本轮是否发现 ≥1 条新就绪 route（与既有 `set_ready` 同一事实的两种精度）。 */
    bool ready() const noexcept { return ready_routes > 0; }
};

/* 兼容构造 + 结束值通道的扫描观测。**只追加，不改既有签名与语义**。 */
class ScanRoundScope
{
public:
    ScanRoundScope(std::size_t scanned_routes, std::size_t deferred_depth) noexcept
        : on_(CounterRegistry::instance().diagnostics_enabled())
    {
        scanned_routes_ = scanned_routes;
        deferred_depth_ = deferred_depth;
        if (!on_) return;
        CounterRegistry::instance().max_gauge(CounterId::deferred_depth_max,
                                             static_cast<std::uint64_t>(deferred_depth));
        start_ns_ = monotonic_now_ns();
    }

    ~ScanRoundScope() noexcept
    {
        if (!on_) return;
        CounterRegistry& r = CounterRegistry::instance();
        const std::uint64_t end = monotonic_now_ns();
        r.inc(CounterId::scan_rounds, 1u);
        r.inc(CounterId::scanned_routes_total, static_cast<std::uint64_t>(scanned_routes_));
        r.inc(CounterId::scan_time_ns_total, end - start_ns_);
        r.set_gauge(CounterId::deferred_depth_last, static_cast<std::uint64_t>(deferred_depth_));
        if (ready_) r.inc(CounterId::ready_observed, 1u);

        /* ---- 追加（R0-9）：结束值只写"自己的"统计，⛔ 不使用 `deferred_depth_after_*` 的
         * gauge（那是多 worker 互相覆盖的量）。归 R0-10 的按 worker 承载见 ScanRoundAccumulator。
         *
         * ⚠️ 分母口径（必须写明）：`deferred_depth_after_total / scan_rounds` 只有在
         * **每一轮都调用 finish()** 时才是"平均扫描后深度"；未 finish 的轮其分子贡献 0
         * 而分母仍 +1。按 worker 的均值请用 `ScanRoundAccumulator`（它只在 add() 时计轮，
         * 即"已上报结束值的轮数"）。取证时用 `finished()` / `result().elapsed_ns_collected`
         * 逐轮断言。 */
        if (finished_) {
            r.inc(CounterId::scan_ready_routes_total, static_cast<std::uint64_t>(ready_routes_));
            r.inc(CounterId::deferred_depth_after_total,
                  static_cast<std::uint64_t>(deferred_depth_after_));
        }
        /* 未调用 finish() ⇒ 新 4 个门控计数中只写"扫描前"兼容 gauge 之外的部分一个都不写，
         * 且 `result_` 保持 deferred_depth_after == deferred_depth_before，避免假值。 */
    }

    ScanRoundScope(const ScanRoundScope&) = delete;
    ScanRoundScope& operator=(const ScanRoundScope&) = delete;

    /* 既有：本轮是否有就绪内容被处理（用于有效就绪比例 = ready_observed / scan_rounds）。 */
    void set_ready(bool ready) noexcept { ready_ = ready; }

    /* 新增（R0-9）：在遍历结束、`set_ready` 前后皆可调用一次。语义：
     *   ready_routes          本轮**新入队**的待处理 route 数（即调用点的 `queued` 计数）
     *   deferred_depth_after  扫描**结束后**、运行预算**之前**的 `deferred.size()`
     * 与 `set_ready()` 并存（同一事实的两种精度）；`ready_routes > 0` 与
     * `set_ready(true)` 应当同时出现，不互相替代、不互相覆盖。
     * 幂等：重复调用以**最后一次**为准（不重复累加）。 */
    void finish(std::size_t ready_routes, std::size_t deferred_depth_after) noexcept
    {
        ready_routes_ = ready_routes;
        deferred_depth_after_ = deferred_depth_after;
        finished_ = true;
        /* ⛔ 诊断关闭时**不读扫描时钟**（§10.2 / 本任务要求 7）：时钟读取必须完全
         * 落在 `if (on_)` 内，否则关闭档会平白多一次 `clock_gettime`
         * （实测会让 2.5 ns/轮 变成 16.7 ns/轮，等于门控失效）。 */
        if (!on_) return;
        CounterRegistry& r = CounterRegistry::instance();
        /* `_last` **无条件**写（含 0）：扫描后清空到 0 是正常状态，
         * 若只在 >0 时写，gauge 会保留上一轮的陈旧值（静默误导）。 */
        r.set_gauge(CounterId::deferred_depth_after_last,
                    static_cast<std::uint64_t>(deferred_depth_after));
        r.max_gauge(CounterId::deferred_depth_after_max,
                    static_cast<std::uint64_t>(deferred_depth_after));
        finish_ns_ = monotonic_now_ns();
    }

    /* 结束值快照（**不修改任何全局计数**；可在 finish() 后任意次调用）。
     * ⛔ 多 worker 场景下请用 ScanRoundAccumulator 汇总，不要把这个快照当全池总量。 */
    ScanRoundResult result() const noexcept
    {
        ScanRoundResult r;
        r.scanned_routes        = scanned_routes_;
        r.ready_routes          = ready_routes_;
        r.deferred_depth_before = deferred_depth_;
        r.deferred_depth_after  = finished_ ? deferred_depth_after_ : deferred_depth_;
        r.diagnostics_enabled   = on_;
        r.elapsed_ns_collected  = on_ && finished_;
        r.elapsed_ns            = (on_ && finished_ && finish_ns_ >= start_ns_)
                                ? (finish_ns_ - start_ns_) : 0ull;
        return r;
    }

    /* 是否已给出结束值（R0-9 的取证完备性判据：判 read 时必须为 true）。 */
    bool finished() const noexcept { return finished_; }
    /* ⛔ 兼容用途：`finish()` 未调用时"扫描后"= "扫描前"（避免给出假值）。 */
    void finish_with_before_depth(std::size_t ready_routes) noexcept
    {
        finish(ready_routes, deferred_depth_);
    }

private:
    bool          on_{false};
    bool          ready_{false};
    bool          finished_{false};
    std::size_t   scanned_routes_{0};
    std::size_t   ready_routes_{0};
    std::size_t   deferred_depth_{0};
    std::size_t   deferred_depth_after_{0};
    std::uint64_t start_ns_{0};
    std::uint64_t finish_ns_{0};
};

/* ---------------------------------------------------------------------------
 * R0-10：**按 worker** 的扫描结束值累加器（每 worker 一个实例；⛔ 不写全局 gauge）。
 *
 * 全池聚合纪律（R0-10 原文）：
 *   · `deferred_depth_after_total`：**sum**（唯一可安全求和的深度量）
 *   · `deferred_depth_after_last` ：**max**（与既有 `deferred_depth_last` 同一聚合纪律）
 *   · `deferred_depth_after_max`  ：**max**
 *   · `scan_ready_routes_total`   ：**sum**（route 数）
 *   · 全池**当前**总深度 = Σ(每 worker 的 `deferred_depth_after_last`) ⇒ 用
 *     `ScanRoundResult::deferred_depth_after` 逐 worker 相加，⛔ 不得读任一 gauge。
 *
 * 线程纪律：一个实例只由**拥有该 worker 的那条线程**使用（与 RecvWorker 的单消费者
 * 模型一致）⇒ 只用 relaxed 原子，无锁。
 * ------------------------------------------------------------------------- */
class ScanRoundAccumulator
{
public:
    void add(const ScanRoundResult& r) noexcept
    {
        scanned_routes_total_.fetch_add(r.scanned_routes, std::memory_order_relaxed);
        ready_routes_total_.fetch_add(r.ready_routes, std::memory_order_relaxed);
        deferred_depth_after_total_.fetch_add(r.deferred_depth_after, std::memory_order_relaxed);
        rounds_.fetch_add(1u, std::memory_order_relaxed);
        if (r.ready_routes > 0) ready_rounds_.fetch_add(1u, std::memory_order_relaxed);
        store_max(deferred_depth_after_last_, r.deferred_depth_after);
        store_max(deferred_depth_after_max_, r.deferred_depth_after);
        if (r.elapsed_ns_collected) elapsed_ns_total_.fetch_add(r.elapsed_ns, std::memory_order_relaxed);
    }

    /* 仅当调用点把 `ScanRoundResult` 交回时才用得上；也可直接 add(scope.result())。 */
    void add(const ScanRoundScope& scope) noexcept { add(scope.result()); }

    std::uint64_t rounds() const noexcept { return rounds_.load(std::memory_order_relaxed); }
    std::uint64_t scanned_routes_total() const noexcept
    { return scanned_routes_total_.load(std::memory_order_relaxed); }
    std::uint64_t ready_routes_total() const noexcept
    { return ready_routes_total_.load(std::memory_order_relaxed); }
    std::uint64_t ready_rounds() const noexcept
    { return ready_rounds_.load(std::memory_order_relaxed); }
    std::uint64_t deferred_depth_after_total() const noexcept
    { return deferred_depth_after_total_.load(std::memory_order_relaxed); }
    /* ⛔ 本 worker 的"最近一轮扫描后深度"；全池当前总深度 = Σ 各 worker 的本值。 */
    std::uint64_t deferred_depth_after_last() const noexcept
    { return deferred_depth_after_last_.load(std::memory_order_relaxed); }
    std::uint64_t deferred_depth_after_max() const noexcept
    { return deferred_depth_after_max_.load(std::memory_order_relaxed); }
    std::uint64_t elapsed_ns_total() const noexcept
    { return elapsed_ns_total_.load(std::memory_order_relaxed); }

    /* 平均每轮新入队 route 数（route 数口径；⛔ 与 ready_observed/scan_rounds 的
     * "就绪轮占比"语义不同，两者不得相加）。 */
    double mean_ready_routes_per_round() const noexcept
    {
        const std::uint64_t n = rounds();
        return n ? static_cast<double>(ready_routes_total()) / static_cast<double>(n) : 0.0;
    }
    double mean_scanned_routes_per_round() const noexcept
    {
        const std::uint64_t n = rounds();
        return n ? static_cast<double>(scanned_routes_total()) / static_cast<double>(n) : 0.0;
    }
    double mean_deferred_depth_after() const noexcept
    {
        const std::uint64_t n = rounds();
        return n ? static_cast<double>(deferred_depth_after_total()) / static_cast<double>(n) : 0.0;
    }

private:
    static void store_max(std::atomic<std::uint64_t>& slot, std::uint64_t v) noexcept
    {
        std::uint64_t cur = slot.load(std::memory_order_relaxed);
        while (v > cur && !slot.compare_exchange_weak(cur, v, std::memory_order_relaxed)) {}
    }

    std::atomic<std::uint64_t> rounds_{0};
    std::atomic<std::uint64_t> scanned_routes_total_{0};
    std::atomic<std::uint64_t> ready_routes_total_{0};
    std::atomic<std::uint64_t> ready_rounds_{0};
    std::atomic<std::uint64_t> deferred_depth_after_total_{0};
    std::atomic<std::uint64_t> deferred_depth_after_last_{0};
    std::atomic<std::uint64_t> deferred_depth_after_max_{0};
    std::atomic<std::uint64_t> elapsed_ns_total_{0};
};

/* 全池聚合：Σ 每 worker（R0-10 唯一允许的总深度求法）。
 * `per_worker_after_last` 为各 worker 的"最近一轮扫描后深度"。 */
struct ScanRoundPoolAggregate
{
    std::uint64_t rounds_sum{0};
    std::uint64_t scanned_routes_sum{0};
    std::uint64_t ready_routes_sum{0};
    std::uint64_t ready_rounds_sum{0};
    std::uint64_t deferred_depth_after_total_sum{0};
    std::uint64_t deferred_depth_after_max{0};
    std::uint64_t deferred_depth_after_last_sum{0};  ///< = 全池**当前**总深度（Σ 每 worker）

    double mean_ready_routes_per_round() const noexcept
    {
        return rounds_sum ? static_cast<double>(ready_routes_sum) / static_cast<double>(rounds_sum) : 0.0;
    }
    double mean_deferred_depth_after() const noexcept
    {
        return rounds_sum ? static_cast<double>(deferred_depth_after_total_sum)
                          / static_cast<double>(rounds_sum) : 0.0;
    }
};

inline ScanRoundPoolAggregate aggregate_scan_rounds(const std::vector<ScanRoundResult>& per_worker_last,
                                                   const std::vector<std::uint64_t>& per_worker_ready_total,
                                                   const std::vector<std::uint64_t>& per_worker_depth_after_total,
                                                   const std::vector<std::uint64_t>& per_worker_rounds,
                                                   const std::vector<std::uint64_t>& per_worker_ready_rounds)
{
    ScanRoundPoolAggregate a;
    const std::size_t n = per_worker_last.size();
    for (std::size_t i = 0; i < n; ++i) {
        const ScanRoundResult& w = per_worker_last[i];
        a.deferred_depth_after_last_sum += w.deferred_depth_after;
        if (w.deferred_depth_after > a.deferred_depth_after_max) a.deferred_depth_after_max = w.deferred_depth_after;
        if (i < per_worker_ready_total.size())      a.ready_routes_sum += per_worker_ready_total[i];
        if (i < per_worker_depth_after_total.size()) a.deferred_depth_after_total_sum += per_worker_depth_after_total[i];
        if (i < per_worker_rounds.size())           a.rounds_sum += per_worker_rounds[i];
        if (i < per_worker_ready_rounds.size())     a.ready_rounds_sum += per_worker_ready_rounds[i];
        a.scanned_routes_sum += w.scanned_routes;
    }
    return a;
}

/* ---------------------------------------------------------------------------
 * `scan_time_ns_total` 的区间定义 —— **冻结**（R0-9 要求明确并冻结）
 * ---------------------------------------------------------------------------
 * 【区间】`ScanRoundScope` **构造**（`collect_pending()` 内、**取到 `mtx` 之后**）
 *        → `finish()`（新增）被调用的那一点，且**含**遍历本身与调用方在
 *        `finish()` **之前**所做的一切（在当前接线中是 `for` 遍历 + `set_ready()`
 *        + 常驻孪生 5 次写）。
 *
 * 【实测分量】（t42 在本仓实测，见 `artifacts/perf/20260930-r42-W06-R2/`）：
 *        diag=on  ≈ 45.5 ns/轮（其中时钟 ≈35 ns、常驻孪生 ≈10 ns）
 *        diag=off ≈ 10.7 ns/轮（不取时钟，仅常驻孪生）
 *
 * 【与 R0-9「取得锁后 → 遍历结束」的关系 —— 如实登记，不宣称相符】
 *        当前区间**比** R0-9 的期望**多喂了**两段：
 *          (a) `set_ready()`（一次 relaxed 布尔写，亚 ns 级）；
 *          (b) 常驻孪生 5 次写（≈10 ns/轮，见上）。
 *        两者都**不属于**业务遍历，因此 `scan_time_ns_total` **含诊断/常驻自身开销**。
 *
 * 【冻结决定】W03 保持**既有区间**（构造 → 析构/finish），理由：
 *  1. 改区间会**改变既有读数的可比性**：t30/t42/R2 的历史 `scan_time_ns_total`
 *     都是"构造 → 析构"口径，中途改区间会让新旧数据不可比（方案 §12 的只追加纪律）。
 *  2. 区间若要精确到"取锁后 → 遍历结束"，需要调用点在遍历前后各打一次点并**只**把
 *     遍历区间交给库 —— 那是**接线**侧的事（R0-11：`recv_worker.cc` 写入权归共享层
 *     负责人），不属 W03 头文件的职责；W03 只提供可表达该区间的通道（`result().elapsed_ns`
 *     的区间与 `scan_time_ns_total` **同为构造 → finish**，因此两者可直接对账）。
 *  3. 取而代之的**守恒性核对**已具备：`diag=on` 时
 *     `Σ result().elapsed_ns` 应 ≈ `scan_time_ns_total`（同区间、同门控），
 *     ⛔ 不相等的读取结果本身就是"接线把 finish 放在别处"的证据。
 *
 * 【三条限定（引用时必须同时带）】
 *  1. ⛔ 不得用 `scan_time_ns_total` 换算 CPU core（它只覆盖 worker 线程被调度着跑
 *     的那一小段扫描区间，不含阻塞等待与调度延迟）。
 *  2. ⛔ 含诊断自身开销（≈20 ns/轮，见上），因此"开诊断"与"关诊断"的读数不同源。
 *  3. ⛔ **在本接口（`finish()`）落地前**采集的 `scan_time_ns_total` 不得用于判据；
 *     落地后仅当 `ScanRoundScope::finished() == true` 的轮次可判据（未 `finish()` 的轮
 *     其 `result().elapsed_ns_collected == false`，属**未采集**）。
 * ------------------------------------------------------------------------- */

/* ---------------------------------------------------------------------------
 * 计数开销对照（§4 W03 交付「观测开销对照」的机器可复现值）。
 * 全部单位 ns/次。
 * ------------------------------------------------------------------------- */
struct CounterOverhead
{
    std::uint64_t iterations{0};
    double counter_inc_ns{0.0};            ///< 常驻计数一次 inc（含单例访问）
    double counter_inc_cached_ns{0.0};     ///< 已持引用的一次 inc（热路径实际形态）
    double diag_inc_enabled_ns{0.0};
    double diag_inc_disabled_ns{0.0};
    double scan_scope_enabled_ns{0.0};       ///< 构造→finish→set_ready→析构 (当前接线形态, 诊断开)
    double scan_scope_disabled_ns{0.0};      ///< 同上 (诊断关, 仅常驻孪生)
    double scan_scope_enabled_no_finish_ns{0.0};   ///< 构造→析构 (历史口径, 诊断开; 与旧读数可比)
    double scan_scope_disabled_no_finish_ns{0.0};  ///< 构造→析构 (历史口径, 诊断关)
    double clock_now_ns{0.0};
    double proc_self_stat_read_ns{0.0};    ///< 读一次 /proc/self/stat 的成本(对照 /proc 采样)
    double proc_task_status_all_ns{0.0};   ///< 读一次本进程全部 TID 的 status 成本
};

/* 注意：measure_counter_overhead() 的声明与定义都在 proc_sampler.h（它需要 /proc
 * 读取助手）。measurement.h 按 counters.h → proc_sampler.h 的顺序包含。 */

}   // namespace measure
}   // namespace dzIPC

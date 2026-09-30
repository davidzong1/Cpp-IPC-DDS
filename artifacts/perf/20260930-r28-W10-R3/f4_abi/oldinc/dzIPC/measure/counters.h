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
 * §10.2 每轮扫描的 RAII 观测。集成点（由 W06 负责接线，W03 只提供实现）：
 *
 *   void RecvWorker::Impl::collect_pending() {
 *       const std::size_t n = snapshot_route_count();     // 本轮扫描 route 数
 *       dzIPC::measure::ScanRoundScope scan(n, deferred.size());
 *       ... 既有全扫逻辑 ...
 *   }   // 析构时累加 scan_rounds / scanned_routes_total / scan_time_ns_total /
 *       // deferred_depth_last / deferred_depth_max，并统计"有效就绪比例"
 *
 * 关闭诊断时构造 + 析构各只有一次 relaxed 布尔读，不取时钟、不写计数。
 * ready_routes 由调用方在排空后回填（`set_ready()`），用于 ready_observed 与
 * 有效就绪比例（= ready_observed / scan_rounds）。
 * ------------------------------------------------------------------------- */
class ScanRoundScope
{
public:
    ScanRoundScope(std::size_t scanned_routes, std::size_t deferred_depth) noexcept
        : on_(CounterRegistry::instance().diagnostics_enabled())
    {
        if (!on_) return;
        scanned_routes_ = scanned_routes;
        deferred_depth_ = deferred_depth;
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
    }

    ScanRoundScope(const ScanRoundScope&) = delete;
    ScanRoundScope& operator=(const ScanRoundScope&) = delete;

    /* 本轮是否有就绪内容被处理（用于有效就绪比例）。 */
    void set_ready(bool ready) noexcept { ready_ = ready; }

private:
    bool          on_{false};
    bool          ready_{false};
    std::size_t   scanned_routes_{0};
    std::size_t   deferred_depth_{0};
    std::uint64_t start_ns_{0};
};

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
    double scan_scope_enabled_ns{0.0};
    double scan_scope_disabled_ns{0.0};
    double clock_now_ns{0.0};
    double proc_self_stat_read_ns{0.0};    ///< 读一次 /proc/self/stat 的成本(对照 /proc 采样)
    double proc_task_status_all_ns{0.0};   ///< 读一次本进程全部 TID 的 status 成本
};

/* 注意：measure_counter_overhead() 的声明与定义都在 proc_sampler.h（它需要 /proc
 * 读取助手）。measurement.h 按 counters.h → proc_sampler.h 的顺序包含。 */

}   // namespace measure
}   // namespace dzIPC

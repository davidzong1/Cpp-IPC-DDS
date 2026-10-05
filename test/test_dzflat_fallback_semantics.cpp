/* W09/t19 聚焦判据：DZFlat 发布尝试的**三种语义互不混算**（方案 §4 W09 第 5 条 + 队长裁决 D-16）。
 *
 * 存在理由（失效方式全部静默）：
 *   ① `try_publish_dzflat()` 返回 `bool`，从返回值看不出"路径选择 / 真回退 / B 借样失败"
 *      ⇒ W08 的 tlv 档出现 `fallback_total=0` 与 `DzFlatFallbackCount=120` 并存的歧义
 *      （两者都不假、含义不同）。没有判据 ⇒ 接线后再漂移也无人发现。
 *   ② 把「路径选择」计入 fallback ⇒ tlv 档凭空出现 N 次"回退"，W10 会用假证据判规模；
 *      反过来漏记「启用了但类型不支持」⇒ 真回退被吞掉。
 *   ③ 把「B 借样失败」并进 fallback ⇒ fallback/(dzflat+fallback) 比值失真（W08 已登记同一纪律）。
 *   ④ 无法精确判定的原因若"猜一个"（如 A 路径的池耗尽猜成 pool_exhausted），报告会给出
 *      不可复核的分解 —— 本包强制它落 `*_reason_unknown`。
 *
 * 本文件用**纯函数**（classify_dzflat_attempt / note_dzflat_borrow_failed）穷举三语义与
 * 全部原因分支，因此不需要跨进程、不需要真共享内存，毫秒级、无 flaky。
 */
#include <cstdint>
#include <cstdio>
#include <string>

#include "dzIPC/measure/counters.h"

#include <gtest/gtest.h>

namespace {

using namespace dzIPC::measure;

class RegistryReset
{
public:
    RegistryReset() { CounterRegistry::instance().reset(); }
    ~RegistryReset() { CounterRegistry::instance().reset(); }
};

dzIPC::measure::DzFlatAttempt attempt_tlv_off()
{
    DzFlatAttempt a;
    a.dzflat_enabled = false;
    a.type_supported = true;
    return a;
}

}   // namespace

/* ---- (a) 路径选择：⛔ 一个 fallback_* 都不得 +1，且 tlv 档期望值可写死 ---- */

TEST(DzFlatFallbackSemantics, PathSelectionNeverCountsAsFallback)
{
    RegistryReset guard;
    /* tlv 档：DZFlat 关闭，120 条消息。 */
    for (int i = 0; i < 120; ++i)
    {
        note_dzflat_attempt(attempt_tlv_off());
    }
    const auto s = CounterRegistry::instance().snapshot();
    EXPECT_EQ(s.get(CounterId::fallback_total), 0u)
        << "tlv 档（DZFlat 关闭）fallback_total 必须为 0 —— 这是**正确值**而不是「未接线」";
    for (const auto id : {CounterId::fallback_backend_unavailable, CounterId::fallback_capacity_full,
                          CounterId::fallback_type_incompatible, CounterId::fallback_oversized,
                          CounterId::fallback_pool_exhausted, CounterId::fallback_reason_unknown})
    {
        EXPECT_EQ(s.get(id), 0u) << "路径选择不得计入回退原因 " << counter_name(id);
    }
    EXPECT_EQ(s.get(CounterId::path_selection_dzflat_disabled), 120u)
        << "路径选择应单独成组（解释 tlv_messages 的来源构成）";
    /* 关闭时"类型不支持"也不得计入 fallback（"预期走 DZFlat"这个前提不成立）。 */
    DzFlatAttempt off_unsupported;
    off_unsupported.dzflat_enabled = false;
    off_unsupported.type_supported = false;
    note_dzflat_attempt(off_unsupported);
    const auto s2 = CounterRegistry::instance().snapshot();
    EXPECT_EQ(s2.get(CounterId::fallback_total), 0u);
    EXPECT_EQ(s2.get(CounterId::path_selection_type_unsupported), 1u)
        << "(a) 关闭且类型无平坦布局：单独成组，对运维的含义是「开启也无收益」";
    /* (a) 两组**二选一**、合计 = 关闭态下的 TLV 条数（可与 tlv_messages 对账）。 */
    EXPECT_EQ(s2.get(CounterId::path_selection_dzflat_disabled)
                  + s2.get(CounterId::path_selection_type_unsupported),
              121u);
}

/* ---- (b) 真回退：启用且尝试过 ⇒ fallback_total 与原因**同时** +1 ---- */

TEST(DzFlatFallbackSemantics, RealFallbackCountsTotalAndReasonTogether)
{
    RegistryReset guard;
    /* 启用 + 类型不支持 ⇒ 一次真实降级（队长 D-16 精确化）。 */
    DzFlatAttempt unsupported;
    unsupported.dzflat_enabled = true;
    unsupported.type_supported = false;
    for (int i = 0; i < 7; ++i) note_dzflat_attempt(unsupported);

    /* 启用 + 类型支持 + 超尺寸档 ⇒ oversize（可精确判定）。 */
    DzFlatAttempt oversized;
    oversized.dzflat_enabled = true;
    oversized.type_supported = true;
    oversized.need_bytes = 2000;
    oversized.capacity_bytes = 1024;
    note_dzflat_attempt(oversized);

    const auto s = CounterRegistry::instance().snapshot();
    EXPECT_EQ(s.get(CounterId::fallback_total), 8u) << "fallback_total 与原因必须同时记（首个失败资源语义）";
    EXPECT_EQ(s.get(CounterId::fallback_type_incompatible), 7u);
    EXPECT_EQ(s.get(CounterId::fallback_oversized), 1u);
    EXPECT_EQ(s.get(CounterId::path_selection_dzflat_disabled), 0u)
        << "回退不得计入路径选择组（两组互斥）";
}

/* ---- (b) 不可精确判定的原因必须落 unknown，⛔ 不得猜分 ---- */

TEST(DzFlatFallbackSemantics, UnresolvableReasonIsRegisteredAsUnknownNotGuessed)
{
    RegistryReset guard;
    /* 启用 + 类型支持 + 有接收方 + 无超尺寸证据：A 路径的池耗尽/无接收方/单生产端 flag
     * 三态混在 `loan()` 的一个 false 里 ⇒ 本层无法区分 ⇒ 必须 unknown。 */
    DzFlatAttempt unknown_case;
    unknown_case.dzflat_enabled = true;
    unknown_case.type_supported = true;
    unknown_case.had_receiver = true;
    note_dzflat_attempt(unknown_case);

    const auto s = CounterRegistry::instance().snapshot();
    EXPECT_EQ(s.get(CounterId::fallback_total), 1u);
    EXPECT_EQ(s.get(CounterId::fallback_reason_unknown), 1u);
    EXPECT_EQ(s.get(CounterId::fallback_pool_exhausted), 0u)
        << "⛔ 不得把不可判定的原因猜成 pool_exhausted —— 报告会给出不可复核的分解";

    /* 无接收方 ⇒ 可精确判定为容量/前置失败（had_receiver=false 是调用点已知事实）。 */
    DzFlatAttempt no_rx;
    no_rx.dzflat_enabled = true;
    no_rx.type_supported = true;
    no_rx.had_receiver = false;
    note_dzflat_attempt(no_rx);
    const auto s2 = CounterRegistry::instance().snapshot();
    EXPECT_EQ(s2.get(CounterId::fallback_total), 2u);
    EXPECT_EQ(s2.get(CounterId::fallback_capacity_full), 1u);
    EXPECT_EQ(s2.get(CounterId::fallback_reason_unknown), 1u);
}

/* ---- (c) B 借样失败：单独成类，⛔ 绝不进 fallback_total ---- */

TEST(DzFlatFallbackSemantics, BorrowFailureIsSeparateFromTransportFallback)
{
    RegistryReset guard;
    /* 一条真回退 + 三条借样失败：两组必须分列。 */
    DzFlatAttempt fb;
    fb.dzflat_enabled = true;
    fb.type_supported = false;
    note_dzflat_attempt(fb);

    note_dzflat_borrow_failed(/*had_receiver=*/false, false, false);   // 无接收方
    note_dzflat_borrow_failed(true, false, false);                     // loan 三态不可分
    note_dzflat_borrow_failed(true, true, false);                      // publish_loan 失败
    note_dzflat_borrow_failed(true, false, true);                      // 语义非法组合 ⇒ unknown

    const auto s = CounterRegistry::instance().snapshot();
    EXPECT_EQ(s.get(CounterId::fallback_total), 1u)
        << "B 借样失败**不得**计入 fallback_total（方案 §4 W09 第 5 条）";
    EXPECT_EQ(s.get(CounterId::borrow_failed_no_receiver), 1u);
    EXPECT_EQ(s.get(CounterId::borrow_failed_reason_unknown), 2u);
    EXPECT_EQ(s.get(CounterId::borrow_failed_publish), 1u);
    EXPECT_EQ(s.get(CounterId::fallback_capacity_full), 0u);
}

/* ---- 成功路径不得被记成回退（防"漏填一个字段就静默多记一次回退"） ---- */

TEST(DzFlatFallbackSemantics, DeliveredAttemptIsNeverCountedAsFallback)
{
    RegistryReset guard;
    /* B 路径成功：三个条件全真 —— 即使调用点忘了回填 dzflat_delivered，分类器也必须自洽。 */
    DzFlatAttempt ok;
    ok.dzflat_enabled = true;
    ok.type_supported = true;
    ok.borrow_requested = true;
    ok.borrow_ok = true;
    ok.write_ok = true;
    ok.publish_loan_ok = true;
    const auto c = classify_dzflat_attempt(ok);
    EXPECT_EQ(c.outcome, DzFlatOutcome::dzflat_delivered);
    EXPECT_FALSE(c.is_fallback);
    note_dzflat_attempt(ok);
    const auto s = CounterRegistry::instance().snapshot();
    EXPECT_EQ(s.get(CounterId::fallback_total), 0u) << "成功尝试不得记回退";
    EXPECT_EQ(s.get(CounterId::borrow_failed_reason_unknown), 0u);

    /* 显式 dzflat_delivered=true 亦同。 */
    DzFlatAttempt ok2;
    ok2.dzflat_enabled = true;
    ok2.type_supported = true;
    ok2.dzflat_delivered = true;
    EXPECT_EQ(classify_dzflat_attempt(ok2).outcome, DzFlatOutcome::dzflat_delivered);
}

/* ---- 追加一律 append-only：既有枚举值与总数口径（代码为准） ----
 *
 * 变更史（本判据是关键路径，每次追加必须同步，且必须在交付里写明）：
 *   54 项  = W03 交付态（W09 按当前文件逐行扣除其 8 项追加得出；W03 文档写 47 与代码不符，
 *            差额登记为 W19-F1，一律以代码为准）
 *   +8     = W09/t19（path_selection_* / fallback_reason_unknown / borrow_failed_*）
 *   +4     = W03/t44（R0-9/R0-10 结束值接口：scan_ready_routes_total /
 *            deferred_depth_after_last / deferred_depth_after_max / deferred_depth_after_total）
 *   +3     = e887d5e（hybrid_publish_calls / hybrid_shm_sends / hybrid_udp_sends）
 *   ⇒ 69
 *
 * 本条之所以必须同步：判据本身用来证明"追加不移动既有 ID"，因此它的常量是**代码镜像**，
 * 代码追加而它不改时它会**正确报红**（本次即如此），说明闸门有效。
 * ⚠️ 若未来追加频繁，请把本行的常量改为从 `counter_table()` 派生，而不是删除断言。 */

TEST(DzFlatFallbackSemantics, AppendedCountersDoNotShiftExistingIds)
{
    /* W03 已交付的字段名与顺序是历史对比的键 ⇒ 追加不得改变既有项。
     * ⚠️ W03 交付文档写"47 个计数"，但代码里 counter_table 实际是 54 项
     * （差额 7 已登记为 W19-F1：文档与代码不一致）——本判据按**代码**为准。 */
    EXPECT_EQ(kCounterCount, static_cast<std::size_t>(54 + 8 + 4 + 3))
        << "计数总数已变: 请按变更史同步本行 (54=W03, +8=W09/t19, +4=W03/t44, +3=e887d5e) 并写进交付";
    EXPECT_EQ(CounterId::hybrid_publish_calls, static_cast<CounterId>(66));
    EXPECT_EQ(CounterId::hybrid_shm_sends, static_cast<CounterId>(67));
    EXPECT_EQ(CounterId::hybrid_udp_sends, static_cast<CounterId>(68));
    EXPECT_EQ(CounterId::tlv_messages, static_cast<CounterId>(0));
    EXPECT_EQ(CounterId::fallback_total, static_cast<CounterId>(8));
    /* t44 追加项必须排在既有项之后（不插入、不重排），且与 scan 族一致地受诊断门控。 */
    EXPECT_GT(static_cast<std::size_t>(CounterId::scan_ready_routes_total),
              static_cast<std::size_t>(CounterId::borrow_failed_reason_unknown));
    EXPECT_TRUE(counter_is_diagnostics_only(CounterId::scan_ready_routes_total));
    EXPECT_TRUE(counter_is_diagnostics_only(CounterId::deferred_depth_after_total));
    /* 追加项必须排在既有项之后（不插入、不重排）。 */
    EXPECT_GT(static_cast<std::size_t>(CounterId::path_selection_dzflat_disabled),
              static_cast<std::size_t>(CounterId::payload_checksum_bad));
    for (const auto id : {CounterId::path_selection_dzflat_disabled,
                          CounterId::path_selection_type_unsupported,
                          CounterId::fallback_reason_unknown,
                          CounterId::borrow_failed_no_receiver,
                          CounterId::borrow_failed_pool_exhausted,
                          CounterId::borrow_failed_oversized,
                          CounterId::borrow_failed_publish,
                          CounterId::borrow_failed_reason_unknown})
    {
        EXPECT_FALSE(counter_is_diagnostics_only(id));
        EXPECT_STRNE(counter_name(id), "");
        EXPECT_STREQ(counter_unit(id), "count");
    }
    /* tlv 档期望值可写死（队长 D-16 要求）：fallback_total=0 是正确值。 */
    EXPECT_STREQ(counter_name(CounterId::fallback_total), "fallback_total");
}

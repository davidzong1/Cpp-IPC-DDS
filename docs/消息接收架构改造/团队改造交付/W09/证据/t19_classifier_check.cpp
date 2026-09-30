/* t19 分类器的独立可执行自检（不依赖 build/lib：counters.h + monotonic_clock.h 是 header-only）。
 * 穷举三语义与全部原因分支，逐条断言"记哪一组、不记哪一组"。 */
#include <cstdio>
#include <string>
#include <vector>

#include "dzIPC/measure/counters.h"

using namespace dzIPC::measure;

static int g_fail = 0;
static void expect(bool cond, const char* what)
{
    if (!cond) { std::printf("  FAIL: %s\n", what); ++g_fail; }
}

int main()
{
    /* ---- (a) 路径选择：开关关 / 类型不支持 ---- */
    {
        DzFlatAttempt a;
        a.dzflat_enabled = false;
        a.type_supported = true;
        auto c = classify_dzflat_attempt(a);
        expect(c.outcome == DzFlatOutcome::path_selection, "(a) OFF+supported => path_selection");
        expect(!c.is_fallback, "(a) 不得计入 fallback");
        expect(c.detail == CounterId::path_selection_dzflat_disabled, "(a) detail=disabled");
    }
    {
        /* 关键判据（队长 D-16 精确化）：**开关 ON 且类型不支持 ⇒ 计入 fallback** */
        DzFlatAttempt a;
        a.dzflat_enabled = true;
        a.type_supported = false;
        auto c = classify_dzflat_attempt(a);
        expect(c.outcome == DzFlatOutcome::fallback, "(b) ON+unsupported => fallback");
        expect(c.detail == CounterId::fallback_type_incompatible, "(b) detail=type_incompatible");
        /* 而开关 OFF 时"类型不支持"**不得**计入任何 fallback */
        a.dzflat_enabled = false;
        auto c2 = classify_dzflat_attempt(a);
        expect(c2.outcome == DzFlatOutcome::path_selection && !c2.is_fallback,
               "(a) OFF+unsupported => path_selection（⛔不进 fallback）");
        expect(c2.detail == CounterId::path_selection_type_unsupported,
               "(a) OFF+unsupported => 单独成组(开启亦无收益)");
    }

    /* ---- (b) 真回退：四种原因 ---- */
    {
        DzFlatAttempt a; a.dzflat_enabled = true; a.type_supported = true; a.had_receiver = false;
        auto c = classify_dzflat_attempt(a);
        expect(c.outcome == DzFlatOutcome::fallback && c.is_fallback, "(b) 无接收方 => fallback");
    }
    {
        DzFlatAttempt a; a.dzflat_enabled = true; a.type_supported = true;
        a.need_bytes = 2000; a.capacity_bytes = 1024;
        auto c = classify_dzflat_attempt(a);
        expect(c.outcome == DzFlatOutcome::fallback && c.detail == CounterId::fallback_oversized,
               "(b) 超尺寸档 => fallback_oversized");
    }
    {
        /* A 路径池耗尽：bool 不可判定 ⇒ 必须落 unknown，⛔ 不得猜成 pool_exhausted */
        DzFlatAttempt a; a.dzflat_enabled = true; a.type_supported = true; a.had_receiver = true;
        auto c = classify_dzflat_attempt(a);
        expect(c.outcome == DzFlatOutcome::fallback, "(b) 其余 => fallback");
        expect(c.detail == CounterId::fallback_reason_unknown,
               "(b) 不可精确判定的原因必须落 unknown（不猜 pool_exhausted）");
    }

    /* ---- (c) B 借样失败：与 (b) 分列 ---- */
    {
        DzFlatAttempt a; a.dzflat_enabled = true; a.type_supported = true;
        a.borrow_requested = true; a.borrow_ok = false; a.had_receiver = false;
        auto c = classify_dzflat_attempt(a);
        expect(c.outcome == DzFlatOutcome::borrow_failed && !c.is_fallback,
               "(c) 无接收方 => borrow_failed 且 ⛔ 不进 fallback");
        expect(c.detail == CounterId::borrow_failed_no_receiver, "(c) detail=no_receiver");
    }
    {
        /* loan 的 bool 判不出 单生产端flag/无接收方/池耗尽 三态 ⇒ unknown */
        DzFlatAttempt a; a.dzflat_enabled = true; a.type_supported = true;
        a.borrow_requested = true; a.borrow_ok = false; a.had_receiver = true;
        auto c = classify_dzflat_attempt(a);
        expect(c.detail == CounterId::borrow_failed_reason_unknown,
               "(c) loan 三态不可分 => borrow_failed_reason_unknown（如实登记）");
    }
    {
        DzFlatAttempt a; a.dzflat_enabled = true; a.type_supported = true;
        a.borrow_requested = true; a.borrow_ok = true; a.write_ok = false;
        auto c = classify_dzflat_attempt(a);
        expect(c.detail == CounterId::borrow_failed_oversized, "(c) finalize 失败 => oversized");
    }
    {
        DzFlatAttempt a; a.dzflat_enabled = true; a.type_supported = true;
        a.borrow_requested = true; a.borrow_ok = true; a.write_ok = true; a.publish_loan_ok = false;
        auto c = classify_dzflat_attempt(a);
        expect(c.detail == CounterId::borrow_failed_publish, "(c) publish_loan 失败 => publish");
    }
    {
        DzFlatAttempt a; a.dzflat_enabled = true; a.type_supported = true;
        a.borrow_requested = true; a.borrow_ok = true; a.write_ok = true; a.publish_loan_ok = true;
        auto c = classify_dzflat_attempt(a);
        expect(c.outcome == DzFlatOutcome::dzflat_delivered, "(c) 借样成功 => delivered");
    }

    /* ---- 落计数：三语义互不混算（对账） ---- */
    {
        CounterRegistry& r = CounterRegistry::instance();
        r.reset();
        DzFlatAttempt off; off.dzflat_enabled = false; off.type_supported = true;
        for (int i = 0; i < 120; ++i) note_dzflat_attempt(off);          // tlv 档 120 条
        auto s = r.snapshot();
        expect(s.get(CounterId::fallback_total) == 0, "tlv 档: fallback_total 必须为 0（正确值）");
        expect(s.get(CounterId::path_selection_dzflat_disabled) == 120,
           "tlv 档: 路径选择(disabled) 记 120");
    expect(s.get(CounterId::path_selection_type_unsupported) == 0,
           "tlv 档: 类型支持 ⇒ 不进 type_unsupported 组");
        expect(s.get(CounterId::borrow_failed_reason_unknown) == 0, "tlv 档: 借样失败 0");

        r.reset();
        DzFlatAttempt fb; fb.dzflat_enabled = true; fb.type_supported = false;
        for (int i = 0; i < 7; ++i) note_dzflat_attempt(fb);
        DzFlatAttempt bf; bf.dzflat_enabled = true; bf.type_supported = true;
        bf.borrow_requested = true; bf.borrow_ok = true; bf.write_ok = true; bf.publish_loan_ok = false;
        for (int i = 0; i < 3; ++i) note_dzflat_attempt(bf);
        s = r.snapshot();
        expect(s.get(CounterId::fallback_total) == 7, "回退 7 条");
        expect(s.get(CounterId::fallback_type_incompatible) == 7, "原因 id 同步 7");
        expect(s.get(CounterId::borrow_failed_publish) == 3, "B 借样失败 3 条独立记");
        std::printf("对账: fallback_total=%llu type_incompatible=%llu borrow_failed_publish=%llu\n",
                    (unsigned long long)s.get(CounterId::fallback_total),
                    (unsigned long long)s.get(CounterId::fallback_type_incompatible),
                    (unsigned long long)s.get(CounterId::borrow_failed_publish));
    }

    /* ⚠️ 实测 W03 交付时的**实际**计数项数是 **54**（counter_table 逐行数），
     * 而 W03 交付文档写的是 47 —— 差额 7 已登记为 W19-F1（文档与代码不一致），
     * 本包按**代码**为准：54 + t19 追加 8 = 62。 */
    /* ---- (c) 专用落点 note_dzflat_borrow_failed 的四条分支 ---- */
    {
        CounterRegistry& r = CounterRegistry::instance();
        r.reset();
        note_dzflat_borrow_failed(/*had_receiver=*/false, /*finalize_ok=*/false, /*publish_ok=*/false);
        note_dzflat_borrow_failed(true, false, false);
        note_dzflat_borrow_failed(true, false, false);
        note_dzflat_borrow_failed(true, true, false);
        const auto s2 = r.snapshot();
        expect(s2.get(CounterId::borrow_failed_no_receiver) == 1, "(c) 无接收方 1");
        expect(s2.get(CounterId::borrow_failed_reason_unknown) == 2, "(c) unknown 2");
        expect(s2.get(CounterId::borrow_failed_publish) == 1, "(c) publish 失败 1");
        expect(s2.get(CounterId::fallback_total) == 0,
               "(c) ⛔ 借样失败绝不计入 fallback_total");
        std::printf("(c) 落点对账: no_receiver=1 unknown=2 publish=1 fallback_total=0\n");
    }

    std::printf("kCounterCount=%zu (W03 代码实际 54 + t19 追加 8 = %zu)\n", kCounterCount,
                static_cast<std::size_t>(54 + 8));
    expect(kCounterCount == 54 + 8, "计数总数 = 54 + 8");
    std::printf("%s\n", g_fail == 0 ? "CLASSIFIER_CHECK: PASS" : "CLASSIFIER_CHECK: FAIL");
    return g_fail == 0 ? 0 : 1;
}

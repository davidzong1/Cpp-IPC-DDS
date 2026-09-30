/* t46 关键证据：**t19 的两处"同一张判定表"实现并不等价**。
 *
 * 同一个逻辑事件（B 路径借样成功、但封口 finalize 失败 = 超变长预算 / oversized）：
 *   · 旁路：classify_dzflat_attempt({borrow_requested=true, borrow_ok=true, write_ok=false})
 *           ⇒ 返回 _oversized  ✅
 *   · 实测落点：note_dzflat_borrow_failed(had_receiver=true, finalize_ok=false, publish_ok=false)
 *           ⇒ 记 _reason_unknown  ❌（⛔ 落点拿不到 write_ok 这一维）
 * ⇒ borrow_failed_oversized 在**唯一活着的生产落点**上结构不可达，其缺口不在 libipc，
 *    而在落点参数映射（counters.h，W03 维护）或调用点（shm_pub_sub_ipc.h，SHM接入负责人）。
 * 每行一个全新进程跑（注册表是进程级单例），避免互相污染。
 * usage: divergence_classifier_vs_site <classifier|site>
 */
#include <cstdio>
#include <cstring>
#include "dzIPC/measure/counters.h"

using namespace dzIPC::measure;

static const char* dump_after(const char* tag)
{
    static char buf[256];
    buf[0] = '\0';
    for (std::size_t i = 0; i < kCounterCount; ++i)
    {
        if (CounterRegistry::instance().get(static_cast<CounterId>(i)) != 0)
        {
            std::strncat(buf, counter_name(static_cast<CounterId>(i)), sizeof(buf) - std::strlen(buf) - 1);
            std::strncat(buf, " ", sizeof(buf) - std::strlen(buf) - 1);
        }
    }
    if (buf[0] == '\0') std::strcpy(buf, "(无)");
    std::printf("%s => produced: %s\n", tag, buf);
    return buf;
}

int main(int argc, char** argv)
{
    const bool via_classifier = (argc > 1) && (std::strcmp(argv[1], "classifier") == 0);
    std::printf("事件 = B 路径借样成功、封口 finalize 失败（超变长预算）, had_receiver=true\n");
    if (via_classifier)
    {
        /* 旁路：调用点**有** write_ok 这一维（finalize 的返回值就是它）。 */
        DzFlatAttempt a;
        a.dzflat_enabled = true;
        a.type_supported = true;
        a.had_receiver   = true;
        a.borrow_requested = true;
        a.borrow_ok        = true;
        a.write_ok         = false;   /* finalize 失败 */
        a.publish_loan_ok  = false;
        a.need_bytes = 2000;
        a.capacity_bytes = 1024;
        const auto c = classify_dzflat_attempt(a);
        std::printf("输入 = {borrow_requested=T, borrow_ok=T, write_ok=F}\n");
        std::printf("分类器返回 detail = %s | outcome=%d need_dzflat=%d\n",
                    counter_name(c.detail), (int)c.outcome, (int)c.is_fallback);
        note_dzflat_attempt(a);
        dump_after("classify_dzflat_attempt");
    }
    else
    {
        /* 实测落点：publish_loaned 在 !lo.finalize() 分支**实际调用**的形态。 */
        std::printf("输入 = note_dzflat_borrow_failed(had_receiver=T, finalize_ok=F, publish_ok=F)\n");
        note_dzflat_borrow_failed(true, false, false);
        dump_after("note_dzflat_borrow_failed");
    }
    std::printf("DIVERGENCE_ARM_DONE\n");
    return 0;
}

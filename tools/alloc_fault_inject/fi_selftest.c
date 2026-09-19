/* 工装自测 —— 确定性断言"尺寸档 [lo,hi) + 起始命中序号 N"的语义 (UF-000 / 0.3.1 第 1 条)
 * =====================================================================================
 * 这个程序不需要 libipc, 也不需要知道任何产品对象的尺寸: 它直接用裸 malloc 摆出确定的
 * 分配序列, 然后断言返回值与工装自报的三个计数**逐一对上**。
 *
 * 为什么用 `dzipc_fi_arm()` 而不是纯 env: 进程启动期(stdio/区域/locale)自己会在不少
 * 尺寸档上分配, 用 env 的 SKIP_N 计数必然被它们吃掉 ⇒ 结果不可预测。arm() 会**清零计数**
 * 并设档, 于是"arm 之后第 k 次档内分配"是确定的。
 *
 * ⛔ 一条纪律: 本程序若**未被注入**, 不会静默通过 —— 它直接报 BLOCKED 并返回 2。
 *
 * 运行: LD_PRELOAD=tools/alloc_fault_inject/bin/liballoc_fault_inject.so \
 *         tools/alloc_fault_inject/bin/fi_selftest
 * 期望: 全部 `SELFTEST ok`, 退出码 0
 * ===================================================================================== */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* weak: 工装没被 LD_PRELOAD 时, 这些符号为 NULL ⇒ 能打出人话而不是链接期报错 */
extern void          dzipc_fi_arm(unsigned long, unsigned long, long, long) __attribute__((weak));
extern void          dzipc_fi_disable(void)                                __attribute__((weak));
extern unsigned long dzipc_fi_band_calls(void)                             __attribute__((weak));
extern unsigned long dzipc_fi_skipped(void)                                __attribute__((weak));
extern unsigned long dzipc_fi_hits(void)                                   __attribute__((weak));
extern unsigned long dzipc_fi_total_calls(void)                            __attribute__((weak));

static int g_failed = 0;

/* 用 write(2) 直出: 不走 stdio(stdio 自己会 malloc, 会把计数搅乱) */
static void say(const char *s)
{
    size_t n = strlen(s);
    ssize_t r = write(2, s, n);
    (void)r;
}

static void chk(int cond, const char *what)
{
    if (cond) { say("SELFTEST ok  : "); say(what); say("\n"); }
    else      { g_failed = 1; say("SELFTEST FAIL: "); say(what); say("\n"); }
}

int main(void)
{
    if (dzipc_fi_arm == NULL || dzipc_fi_hits == NULL) {
        say("SELFTEST BLOCKED: 注入工装未加载(缺 LD_PRELOAD) —— 未执行任何断言, "
            "不得当通过\n");
        return 2;
    }

    /* ---------------------------------------------------------------------------
     * 阶段 1: 档 [100,200), SKIP_N=2, MAX_FAILS=1
     *   档内次序:  #1 放过(SKIP)  #2 放过(SKIP)  #3 拦截  #4 起放过(MAX_FAILS 用尽)
     *   档外:      malloc(50) 不计数
     * ------------------------------------------------------------------------- */
    dzipc_fi_arm(100, 200, 2, 1);

    void *p1 = malloc(150);            /* 档内 #1 */
    void *p2 = malloc(150);            /* 档内 #2 */
    void *p3 = malloc(150);            /* 档内 #3 ⇒ 拦截 */
    void *p4 = malloc(150);            /* 档内 #4 ⇒ MAX_FAILS 已用尽, 放过 */
    void *q  = malloc(50);             /* 档外 ⇒ 不计数 */
    void *r  = calloc(1, 150);         /* 档内 #5 ⇒ 放过 */

    /* 先把数全取出来, 再打印(打印会 malloc, 会污染后续阶段的计数) */
    unsigned long band = dzipc_fi_band_calls();
    unsigned long skip = dzipc_fi_skipped();
    unsigned long hit  = dzipc_fi_hits();

    say("--- 阶段1: band=[100,200) skip_n=2 max_fails=1 ---\n");
    chk(p1 != NULL, "档内 #1(SKIP 内)未拦截");
    chk(p2 != NULL, "档内 #2(SKIP 内)未拦截");
    chk(p3 == NULL, "档内 #3 被拦截(返回 NULL)");
    chk(p4 != NULL, "档内 #4 未拦截(MAX_FAILS=1 已用尽)");
    chk(q  != NULL, "档外 malloc(50) 不受影响");
    chk(r  != NULL, "档内 #5(通过 calloc)未拦截");
    chk(band == 5, "band_calls == 5(4×malloc + 1×calloc, 档外不计)");
    chk(skip == 2, "skipped == 2");
    chk(hit  == 1, "hits == 1");

    /* ---------------------------------------------------------------------------
     * 阶段 2: MAX_FAILS=-1(不限) + disable 后恢复
     * ------------------------------------------------------------------------- */
    dzipc_fi_arm(300, 400, 0, -1);
    void *a = malloc(350);             /* 档内 #1 ⇒ 拦截 */
    void *b = malloc(350);             /* 档内 #2 ⇒ 拦截(不限次) */
    unsigned long h2 = dzipc_fi_hits();
    dzipc_fi_disable();
    void *c = malloc(350);             /* 已 disable ⇒ 放过 */
    unsigned long h3 = dzipc_fi_hits();

    say("--- 阶段2: band=[300,400) skip_n=0 max_fails=-1, 随后 disable ---\n");
    chk(a == NULL, "a: 档内 #1 被拦截");
    chk(b == NULL, "b: 档内 #2 被拦截(max_fails=-1 不限次)");
    chk(h2 == 2, "h2: hits == 2");
    chk(c != NULL, "c: disable() 之后放过");
    chk(h3 == 2, "h3: disable() 之后 hits 不再增长");

    /* ---------------------------------------------------------------------------
     * 阶段 3: SKIP_N 跨越 —— 档内前 N 次放过则一次都不拦
     * ------------------------------------------------------------------------- */
    dzipc_fi_arm(500, 600, 3, 1);
    void *s1 = malloc(550);
    void *s2 = malloc(550);
    void *s3 = malloc(550);
    unsigned long h4 = dzipc_fi_hits();
    unsigned long b4 = dzipc_fi_band_calls();
    dzipc_fi_disable();

    say("--- 阶段3: band=[500,600) skip_n=3 max_fails=1, 只做 3 次档内分配 ---\n");
    chk(s1 != NULL && s2 != NULL && s3 != NULL, "SKIP_N 用尽前全部放过");
    chk(h4 == 0, "hits == 0(还没轮到拦)");
    chk(b4 == 3, "band_calls == 3");

    if (g_failed) { say("SELFTEST RESULT: FAIL\n"); return 1; }
    say("SELFTEST RESULT: PASS (工装语义与计数全部对上)\n");
    return 0;
}

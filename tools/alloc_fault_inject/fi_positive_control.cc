/* 阳性对照 —— "工装是否有效"的唯一证据 (UF-000 / docs/unfixed_defects.md 0.3.1 第 2 条)
 * =====================================================================================
 * 在**已知含缺陷的旧态**上跑真实注入, 目标路径**必须崩**。
 *
 * ⛔ 崩不了 ⇒ 工装没生效; 此后所有"注入后全绿"都是**假绿**。
 *    所以本程序刻意把"没崩"实现成**显式失败返回**(而不是静默 0)。
 *
 * 被测缺陷(UF-002 / 本文档 §2): `ipc::shm::handle` 的 pimpl 走"不舒服"分支 ⇒
 * `mem::alloc<handle_>()` 分配失败时 `p_ == nullptr`(§7 已加的就地构造前判空),
 * 而 `~handle()` 第一句就是 `release()` → `impl(p_)->id_`(**先解引用 p_ 再判 id_**)
 * ⇒ 读地址 0 ⇒ SIGSEGV。`valid()` 走 `impl(p_)->m_` ⇒ 读 **0x8**, 同族崩点。
 *
 * 自标定: 不给 lo/hi 时, 本程序先用"只计数不拦截"(max_fails=0)的宽档测出
 * `sizeof(handle_)`, 再用**精确到那一个尺寸**的档去拦。这样 sizeof 变了也不会用错档,
 * 且"宽档里恰好只有 1 次分配"这个前提会被当场断言 —— 否则直接判工装不可信。
 *
 * 运行(自标定):
 *   LD_PRELOAD=tools/alloc_fault_inject/bin/liballoc_fault_inject.so \
 *     tools/alloc_fault_inject/bin/fi_positive_control
 * 运行(显式档, 尺寸与 N 都写死):
 *   LD_PRELOAD=tools/alloc_fault_inject/bin/liballoc_fault_inject.so \
 *     tools/alloc_fault_inject/bin/fi_positive_control 56 57 0 dtor
 *
 * 退出码:
 *   139 / 其它信号终止 = **阳性对照 PASS**(旧态在真实注入下崩了 —— 这是要的红)
 *   0..4              = **FAIL**(没崩: 工装没生效, 或缺陷已被修掉)
 *   2                 = 环境错误(工装没加载 / 参数不对) —— 不是 PASS 也不是 FAIL 判定
 * ===================================================================================== */
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unistd.h>

#include "libipc/shm.h"

extern "C" {
void          dzipc_fi_arm(unsigned long, unsigned long, long, long) __attribute__((weak));
unsigned long dzipc_fi_band_calls(void)                              __attribute__((weak));
unsigned long dzipc_fi_last_band_size(void)                          __attribute__((weak));
unsigned long dzipc_fi_hits(void)                                    __attribute__((weak));
}

namespace {

void say(const char *s)
{
    ssize_t r = write(2, s, strlen(s));
    (void)r;
}

void say_num(const char *label, unsigned long v)
{
    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%s=%lu\n", label, v);
    if (n > 0) { ssize_t r = write(2, buf, static_cast<size_t>(n)); (void)r; }
}

/* 自标定: 用"只计数不拦截"的宽档测出 handle_ 的分配尺寸。
 * 返回 0 成功; 并把尺寸写进 *sz、把宽档命中次数写进 *calls。 */
int calibrate(unsigned long *sz, unsigned long *calls)
{
    dzipc_fi_arm(1, 4096, 0, /*max_fails=*/0);   /* 只计数, 不拦截 */

    {
        ipc::shm::handle probe;                  /* 唯一一次档内分配就是 handle_ 那一块 */
        *calls = dzipc_fi_band_calls();
        *sz    = dzipc_fi_last_band_size();
        (void)probe;                             /* 空 handle: id_ == nullptr, 析构不碰 /dev/shm */
    }
    return 0;
}

}   // namespace

int main(int argc, char **argv)
{
    if (dzipc_fi_arm == nullptr || dzipc_fi_hits == nullptr) {
        say("POSCTL BLOCKED: 注入工装未加载 —— 请加 LD_PRELOAD(见本文件头注释)\n");
        return 2;
    }

    unsigned long lo = 0, hi = 0;
    long          skip_n = 0;
    const char   *mode   = "dtor";

    /* 预热: 关掉注入, 先构造+析构一个 handle —— 逼出 libipc 的静态初始化与任何
     * "一生只做一次"的分配。⛔ 少了这一步, 显式档路径下第一个落在档内的分配可能是
     * 初始化期的, 而不是 handle_ 的 ⇒ 命中判据不可信。 */
    dzipc_fi_arm(0, 0, 0, 0);
    { ipc::shm::handle warm; (void)warm; }
    say("POSCTL INFO: 预热完成(libipc 初始化期分配已排除)\n");

    if (argc >= 3) {
        lo = strtoul(argv[1], nullptr, 0);
        hi = strtoul(argv[2], nullptr, 0);
        if (argc >= 4) skip_n = strtol(argv[3], nullptr, 0);
        if (argc >= 5) mode   = argv[4];
        say("POSCTL INFO: 使用命令行给定的尺寸档(自标定已跳过)\n");
    } else {
        unsigned long sz = 0, calls = 0;
        calibrate(&sz, &calls);
        say_num("POSCTL INFO: 自标定 sizeof(handle_) 尺寸", sz);
        say_num("POSCTL INFO: 宽档 [1,4096) 内构造期分配次数", calls);
        if (calls != 1) {
            /* 宽档里不止一次分配 ⇒ 无法确定哪一次是 handle_ ⇒ 标定不可信, 不得继续 */
            say("POSCTL FAIL: 宽档内构造期分配不是 1 次 ⇒ 自标定不可信, 请改用显式 lo/hi\n");
            return 2;
        }
        lo = sz;
        hi = sz + 1;
    }

    {
        char buf[256];
        int n = snprintf(buf, sizeof(buf),
                         "POSCTL INFO: 注入档 band=[%lu,%lu) skip_n=%ld max_fails=1 mode=%s\n",
                         lo, hi, skip_n, mode);
        if (n > 0) { ssize_t r = write(2, buf, static_cast<size_t>(n)); (void)r; }
    }

    dzipc_fi_arm(lo, hi, skip_n, 1);

    if (strcmp(mode, "valid") == 0) {
        ipc::shm::handle h;
        if (dzipc_fi_hits() == 0) {
            /* ⛔ 未命中 ⇒ 不许当通过 */
            say("POSCTL FAIL: 尺寸档**未命中** ⇒ 工装没生效(不得当通过)\n");
            return 3;
        }
        say("POSCTL INFO: 已命中, 调用 valid() —— 旧态应在此读地址 0x8 崩\n");
        (void)h.valid();
        say("POSCTL FAIL: valid() 走完却没崩 ⇒ 旧态未复现或已被修\n");
        return 4;
    }

    /* 默认 mode=dtor: 构造 + 析构。旧态崩在析构(release() → impl(p_)->id_)。 */
    {
        ipc::shm::handle sh;
        unsigned long calls = dzipc_fi_band_calls();
        unsigned long hits  = dzipc_fi_hits();
        say_num("POSCTL INFO: 构造期档内分配次数", calls);
        say_num("POSCTL INFO: 实际拦截次数", hits);
        if (hits == 0) {
            say("POSCTL FAIL: 尺寸档**未命中** ⇒ 工装没生效(不得当通过)\n");
            return 3;
        }
        if (calls != 1) {
            say("POSCTL FAIL: 构造期档内分配不是 1 次 ⇒ 命中可能不是 handle_ 那一块, "
                "结论不可信(请收窄尺寸档)\n");
            return 2;
        }
        say("POSCTL INFO: 已命中且唯一, 离开作用域 —— 旧态应在此崩"
            "(~handle → release → impl(p_)->id_)\n");
    }

    say("POSCTL FAIL: 构造+析构走完却**没崩** ⇒ 阳性对照不成立\n"
        "             两种可能: ①工装没拦到目标分配 ②该缺陷已被修掉(那对 UF-002 是好事, "
        "但那时必须让 test_alloc_fault_inject 转绿, 而不是让本对照沉默)\n");
    return 4;
}

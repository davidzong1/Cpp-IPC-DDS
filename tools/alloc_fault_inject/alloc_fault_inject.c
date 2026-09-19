/* 分配失败注入工装 —— UF-000
 * =====================================================================================
 * 目的: 把"真实 OOM"变成**可重复、可观测**的实验, 让 UF-001(allocator noexcept→nullptr)
 *       与 UF-002(pimpl make() 空解引用)的判据**可执行**。
 *
 * 形态: LD_PRELOAD 的 malloc 族拦截器。按**尺寸档 [lo,hi) + 起始命中序号 N** 返回 NULL。
 *       ⛔ 不改任何产品码 —— 纯进程外注入, `src/libipc/**` 一个字节都不动。
 *
 * 为什么必须存在(见 docs/unfixed_defects.md 0.3.1 第 3 条): 没有"尺寸档拦截计数",
 *       "没注入"与"注入了但代码正确"**不可区分** ⇒ UF-001/UF-002 的判据会退化成
 *       "不注入时全绿", 那是**回归不是判据**。故本工装把三类计数(总调用 / 档内调用 /
 *       实际拦截)全部导出, 并在退出时打到 stderr。
 *
 * ⛔ 一条纪律: **崩不了 ⇒ 工装没生效**。阳性对照(fi_positive_control)必须先给出
 *       "旧态在真实注入下崩溃"的实测红, 否则之后所有"注入后全绿"都是假绿。
 *
 * -------------------------------------------------------------------------------------
 * 环境变量(全部可选; 不给 LO/HI 则工装**不激活**)
 *   DZIPC_FI_ALLOC_SIZE_LO    尺寸档下界(inclusive, 字节)
 *   DZIPC_FI_ALLOC_SIZE_HI    尺寸档上界(exclusive, 字节)   —— 两者都须给且 HI>LO
 *   DZIPC_FI_ALLOC_SKIP_N     放过前 N 次**档内**分配, 从第 N+1 次开始拦; 默认 0
 *   DZIPC_FI_ALLOC_MAX_FAILS  最多拦几次; 默认 1; -1 = 不限
 *   DZIPC_FI_TRACE            非 0 ⇒ 每次档内分配打一行(尺寸 + 序号)到 stderr
 *   DZIPC_FI_HISTOGRAM        非 0 ⇒ 退出时打印档内尺寸直方图(用来发现某个对象的尺寸)
 *   DZIPC_FI_REPORT=<path>    把 FINAL 行同时写进该文件(供脚本解析)
 *
 * 进程内接口(供 test_alloc_fault_inject.cpp 用; 用 weak 符号引用, 不依赖 -ldl):
 *   void          dzipc_fi_arm(unsigned long lo, unsigned long hi, long skip_n, long max_fails);
 *                 // 设档 + **清零计数**(让"装载后的第一次档内分配"确定可预测) + 激活
 *   void          dzipc_fi_disable(void);
 *   void          dzipc_fi_reset_counters(void);
 *   unsigned long dzipc_fi_total_calls(void);
 *   unsigned long dzipc_fi_band_calls(void);
 *   unsigned long dzipc_fi_skipped(void);
 *   unsigned long dzipc_fi_hits(void);
 *   int           dzipc_fi_active(void);
 *   unsigned long dzipc_fi_last_band_size(void);   // 最近一次档内分配的尺寸(发现尺寸用)
 *
 * 覆盖边界(如实声明): 只拦 malloc/calloc/realloc。**不拦** aligned_alloc / posix_memalign /
 *   memalign / valloc 与 mmap。libipc 的 `static_alloc::alloc` 是 `std::malloc`
 *   (src/libipc/memory/alloc.h:20-27), 故本条边界不影响 UF-001/UF-002。
 *   `free` **被拦但只做引导期豁免与转发**, 不参与注入判定。
 * =====================================================================================
 */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ===================== 配置 ======================================================== */
static int           g_inited      = 0;
static int           g_active      = 0;
static unsigned long g_lo          = 0;
static unsigned long g_hi          = 0;
static long          g_skip_n      = 0;
static long          g_max_fails   = 1;
static int           g_trace       = 0;
static int           g_histogram   = 0;
static const char   *g_report_path = NULL;

/* ===================== 计数 ======================================================== */
static unsigned long g_total_calls    = 0;   /* malloc 族调用总数(不论是否在档内) */
static unsigned long g_band_calls     = 0;   /* 尺寸落在 [lo,hi) 的调用数 */
static unsigned long g_skipped        = 0;   /* 其中因 SKIP_N 被放过的 */
static unsigned long g_hits           = 0;   /* 实际返回 NULL 的次数 = 拦截数 */
static unsigned long g_last_band_size = 0;

/* 固定数组, 保证直方图本身不在 malloc 里再分配(否则递归) */
#define FI_HIST_MAX 4096
static unsigned long g_hist[FI_HIST_MAX];

/* ===================== 真实分配器 / 重入保护 ======================================= */
static void *(*real_malloc )(size_t)         = NULL;
static void *(*real_calloc )(size_t, size_t) = NULL;
static void *(*real_realloc)(void *, size_t) = NULL;
static void  (*real_free   )(void *)         = NULL;

/* ⛔ 两个重入场景必须与正常路径分开, 否则工装自己会死:
 *    g_resolving —— `dlsym()` 内部可能调 calloc/realloc。此时真实分配器还没拿到(或正在
 *                   被解析), 只能给静态缓冲。
 *    g_in_hook   —— 我们自己的 fprintf/fopen 也走 malloc。此时真实分配器**已经**有了,
 *                   直接转发即可(既不递归, 也不把自己日志要的内存拦掉)。
 *  两者都不参与注入判定, 也都不计入计数器。 */
static int    g_resolving = 0;
static __thread int g_in_hook = 0;

static char   g_boot[1 << 16];
static size_t g_boot_used = 0;

static int in_boot(const void *p)
{
    const char *c = (const char *)p;
    return c >= g_boot && c < (g_boot + sizeof(g_boot));
}

static void *boot_alloc(size_t n)
{
    size_t aligned = (n + 15u) & ~(size_t)15u;
    if (g_boot_used + aligned > sizeof(g_boot)) return NULL;
    void *p = g_boot + g_boot_used;
    g_boot_used += aligned;
    return p;
}

static void fi_resolve(void)
{
    if (real_malloc != NULL) return;
    g_resolving = 1;
    real_malloc  = (void *(*)(size_t))        dlsym(RTLD_NEXT, "malloc");
    real_calloc  = (void *(*)(size_t, size_t))dlsym(RTLD_NEXT, "calloc");
    real_realloc = (void *(*)(void *, size_t))dlsym(RTLD_NEXT, "realloc");
    real_free    = (void  (*)(void *))        dlsym(RTLD_NEXT, "free");
    g_resolving = 0;
}

/* 尽早解析, 让正常路径第一次分配就不落在重入窗口里。构造期 dlsym 是安全的:
 * 此时动态链接已完成, 且递归由 g_resolving 兜住。 */
__attribute__((constructor)) static void fi_ctor(void)
{
    fi_resolve();
}

/* ===================== env 解析(不分配) =========================================== */
static unsigned long env_ul(const char *key, unsigned long dflt, int *found)
{
    const char *s = getenv(key);
    if (s == NULL || *s == '\0') { if (found) *found = 0; return dflt; }
    if (found) *found = 1;
    return strtoul(s, NULL, 0);
}

static long env_l(const char *key, long dflt, int *found)
{
    const char *s = getenv(key);
    if (s == NULL || *s == '\0') { if (found) *found = 0; return dflt; }
    if (found) *found = 1;
    return strtol(s, NULL, 0);
}

static void fi_init(void)
{
    if (g_inited) return;
    g_inited = 1;

    int f_lo = 0, f_hi = 0;
    unsigned long lo = env_ul("DZIPC_FI_ALLOC_SIZE_LO", 0, &f_lo);
    unsigned long hi = env_ul("DZIPC_FI_ALLOC_SIZE_HI", 0, &f_hi);

    if (f_lo && f_hi && hi > lo) {
        g_lo = lo;
        g_hi = hi;
        g_active = 1;
    } else {
        g_active = 0;
        return;
    }

    g_skip_n      = env_l("DZIPC_FI_ALLOC_SKIP_N",    0, NULL);
    g_max_fails   = env_l("DZIPC_FI_ALLOC_MAX_FAILS", 1, NULL);
    g_trace       = env_ul("DZIPC_FI_TRACE",     0, NULL) != 0;
    g_histogram   = env_ul("DZIPC_FI_HISTOGRAM", 0, NULL) != 0;
    g_report_path = getenv("DZIPC_FI_REPORT");
}

/* ===================== 判定 ======================================================== */
static int fi_should_fail(size_t n)
{
    if (!g_active)                                      return 0;
    if (n < (size_t)g_lo || n >= (size_t)g_hi)          return 0;

    __atomic_add_fetch(&g_band_calls, 1, __ATOMIC_RELAXED);
    g_last_band_size = (unsigned long)n;
    if (g_histogram && n < FI_HIST_MAX) {
        __atomic_add_fetch(&g_hist[n], 1, __ATOMIC_RELAXED);
    }

    if (g_trace) {
        g_in_hook = 1;
        fprintf(stderr, "[alloc_fault_inject] band-call #%lu size=%zu\n",
                (unsigned long)__atomic_load_n(&g_band_calls, __ATOMIC_RELAXED), n);
        g_in_hook = 0;
    }

    if ((long)__atomic_load_n(&g_skipped, __ATOMIC_RELAXED) < g_skip_n) {
        __atomic_add_fetch(&g_skipped, 1, __ATOMIC_RELAXED);
        return 0;
    }
    if (g_max_fails >= 0 &&
        (long)__atomic_load_n(&g_hits, __ATOMIC_RELAXED) >= g_max_fails) {
        return 0;
    }
    __atomic_add_fetch(&g_hits, 1, __ATOMIC_RELAXED);
    return 1;
}

/* ===================== 拦截器 ====================================================== */
void *malloc(size_t n)
{
    if (__builtin_expect(g_in_hook, 0)) {
        return real_malloc ? real_malloc(n) : boot_alloc(n);
    }
    if (__builtin_expect(g_resolving, 0)) return boot_alloc(n);
    if (real_malloc == NULL) fi_resolve();
    if (real_malloc == NULL) return NULL;          /* dlsym 失败: 无处可退 */

    fi_init();
    __atomic_add_fetch(&g_total_calls, 1, __ATOMIC_RELAXED);
    if (fi_should_fail(n)) { errno = ENOMEM; return NULL; }
    return real_malloc(n);
}

void *calloc(size_t nmemb, size_t size)
{
    size_t total = nmemb * size;

    if (__builtin_expect(g_in_hook, 0)) {
        if (real_calloc) return real_calloc(nmemb, size);
        void *p = boot_alloc(total);
        if (p != NULL) memset(p, 0, total);
        return p;
    }
    if (__builtin_expect(g_resolving, 0)) {
        void *p = boot_alloc(total);
        if (p != NULL) memset(p, 0, total);
        return p;
    }
    if (real_calloc == NULL) fi_resolve();
    if (real_calloc == NULL) return NULL;

    fi_init();
    __atomic_add_fetch(&g_total_calls, 1, __ATOMIC_RELAXED);
    if (fi_should_fail(total)) { errno = ENOMEM; return NULL; }
    return real_calloc(nmemb, size);
}

void *realloc(void *p, size_t n)
{
    /* 引导期静态缓冲上的 realloc: 就地搬走(它不归 libc 管, 不能交给 real_realloc)。 */
    if (p != NULL && in_boot(p)) {
        void *q = boot_alloc(n);
        if (q != NULL) memcpy(q, p, n);
        return q;
    }

    if (__builtin_expect(g_in_hook, 0)) {
        return real_realloc ? real_realloc(p, n) : boot_alloc(n);
    }
    if (__builtin_expect(g_resolving, 0)) return boot_alloc(n);
    if (real_realloc == NULL) fi_resolve();
    if (real_realloc == NULL) return NULL;

    fi_init();
    __atomic_add_fetch(&g_total_calls, 1, __ATOMIC_RELAXED);
    if (fi_should_fail(n)) { errno = ENOMEM; return NULL; }
    return real_realloc(p, n);
}

/* free 不参与注入判定 —— 只做两件事: 豁免引导期静态缓冲(交给 libc 会 abort),
 * 以及确保我们自己的日志路径能正常释放。注意本函数**不增加 g_total_calls**。 */
void free(void *p)
{
    if (p == NULL) return;
    if (in_boot(p)) return;                       /* 静态缓冲: 无人释放, 由进程存活期持有 */

    if (__builtin_expect(g_in_hook, 0) || __builtin_expect(g_resolving, 0)) {
        if (real_free) real_free(p);
        return;
    }
    if (real_free == NULL) fi_resolve();
    if (real_free) real_free(p);
}

/* ===================== 导出接口 ==================================================== */
unsigned long dzipc_fi_total_calls(void)    { return __atomic_load_n(&g_total_calls, __ATOMIC_RELAXED); }
unsigned long dzipc_fi_band_calls(void)     { return __atomic_load_n(&g_band_calls,  __ATOMIC_RELAXED); }
unsigned long dzipc_fi_skipped(void)        { return __atomic_load_n(&g_skipped,     __ATOMIC_RELAXED); }
unsigned long dzipc_fi_hits(void)           { return __atomic_load_n(&g_hits,        __ATOMIC_RELAXED); }
int           dzipc_fi_active(void)         { fi_init(); return g_active; }
unsigned long dzipc_fi_last_band_size(void) { return g_last_band_size; }

void dzipc_fi_reset_counters(void)
{
    __atomic_store_n(&g_total_calls,    0ul, __ATOMIC_RELAXED);
    __atomic_store_n(&g_band_calls,     0ul, __ATOMIC_RELAXED);
    __atomic_store_n(&g_skipped,        0ul, __ATOMIC_RELAXED);
    __atomic_store_n(&g_hits,           0ul, __ATOMIC_RELAXED);
    g_last_band_size = 0;
}

void dzipc_fi_arm(unsigned long lo, unsigned long hi, long skip_n, long max_fails)
{
    g_inited      = 1;
    g_lo          = lo;
    g_hi          = hi;
    g_skip_n      = skip_n;
    g_max_fails   = max_fails;
    g_active      = (hi > lo);
    dzipc_fi_reset_counters();
}

void dzipc_fi_disable(void)
{
    g_active = 0;
}

/* ===================== 退出报告 ==================================================== */
__attribute__((destructor)) static void fi_fini(void)
{
    if (!g_inited) return;

    int was_active = g_active;
    g_active  = 0;       /* ⛔ 先关拦截: 下面的 fprintf/fopen 自己也要 malloc */
    g_in_hook = 1;

    fprintf(stderr,
            "[alloc_fault_inject] FINAL active=%d band=[%lu,%lu) skip_n=%ld max_fails=%ld "
            "total_calls=%lu band_calls=%lu skipped=%lu hits=%lu\n",
            was_active,
            g_lo, g_hi, g_skip_n, g_max_fails,
            __atomic_load_n(&g_total_calls, __ATOMIC_RELAXED),
            __atomic_load_n(&g_band_calls,  __ATOMIC_RELAXED),
            __atomic_load_n(&g_skipped,     __ATOMIC_RELAXED),
            __atomic_load_n(&g_hits,        __ATOMIC_RELAXED));

    if (g_histogram) {
        for (unsigned long s = 0; s < FI_HIST_MAX; ++s) {
            unsigned long c = __atomic_load_n(&g_hist[s], __ATOMIC_RELAXED);
            if (c != 0) {
                fprintf(stderr, "[alloc_fault_inject] HIST size=%lu count=%lu\n", s, c);
            }
        }
    }

    if (g_report_path != NULL) {
        FILE *f = fopen(g_report_path, "w");
        if (f != NULL) {
            fprintf(f, "band_lo=%lu\nband_hi=%lu\nskip_n=%ld\nmax_fails=%ld\n"
                       "total_calls=%lu\nband_calls=%lu\nskipped=%lu\nhits=%lu\n",
                    g_lo, g_hi, g_skip_n, g_max_fails,
                    __atomic_load_n(&g_total_calls, __ATOMIC_RELAXED),
                    __atomic_load_n(&g_band_calls,  __ATOMIC_RELAXED),
                    __atomic_load_n(&g_skipped,     __ATOMIC_RELAXED),
                    __atomic_load_n(&g_hits,        __ATOMIC_RELAXED));
            fclose(f);
        }
    }
}

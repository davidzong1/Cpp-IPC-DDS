/* UF-001 / UF-002 的**注入态**判据 —— 用 UF-000 工装驱动 (docs/unfixed_defects.md 0.1 表)
 * =====================================================================================
 * ⛔ 本文件的头号纪律(0.3.1 第 3 条): **没有命中计数, "没注入"与"注入了但代码正确"
 *    不可区分** ⇒ 每个用例在断言业务行为之前, 先硬断言 `hits >= 1`。
 *    命中为 0 时**判 FAIL, 不是判 PASS** —— 否则"注入后全绿"是假绿。
 *
 * ⛔ 第二条(0.3.1 第 2 条): 工装未加载时本文件 **GTEST_SKIP**, 绝不绿色通过。
 *    SKIP 的含义是"本用例无判据力, 不得当验收证据引用"。
 *
 * 运行(必须带 LD_PRELOAD, 否则全部 SKIP):
 *   LD_PRELOAD=tools/alloc_fault_inject/bin/liballoc_fault_inject.so \
 *     ./build/bin/test_alloc_fault_inject
 * 只跑一条:
 *   ... --gtest_filter='AllocFaultInject.HandlePimplAllocFailureMustNotCrash'
 *
 * ⚠️ 今天(未修)的**预期**结果: `HandlePimplAllocFailureMustNotCrash` 与
 *    `StringGrowAllocFailureMustNotCrash` **以 SIGSEGV 终止进程**(退出码 139) ——
 *    这就是 UF-001/UF-002 的实测红。修好之后它们才应转绿。同一条红线由
 *    `tools/alloc_fault_inject/fi_positive_control` 独立复现(阳性对照)。
 *
 * 尺寸档的取法: `handle` 那条**自标定**(先只计数不拦截测出 sizeof(handle_), 再用精确到
 * 那一个字节的档去拦), 所以 sizeof 变了也不会用错档; "宽档内构造期分配恰好 1 次"这个前提
 * 会被当场断言。`ipc::string` 那条用一个**只可能属于本用例**的大尺寸档。
 * ===================================================================================== */
#include <cstdlib>
#include <cstring>
#include <exception>
#include <iostream>
#include <string>

#include "gtest/gtest.h"

#include "libipc/memory/resource.h"   // ipc::string
#include "libipc/shm.h"               // ipc::shm::handle

/* ---------------------------------------------------------------------------
 * 工装接口: 用 **weak 符号**引用, 因此不需要 -ldl、不需要改 test/CMakeLists.txt,
 * 也不需要把工装链进本二进制。工装没被 LD_PRELOAD 时这些地址就是 nullptr。
 *
 * ⛔ 两个必须: ① `extern "C"` —— 工装定义在 .c 里, 没有它会去找 C++ 修饰名;
 *             ② **放在匿名 namespace 之外** —— 放进去就变成内部链接, 根本不会去
 *                动态符号表里找, 链接期直接报未定义。
 * ------------------------------------------------------------------------- */
extern "C" {
void          dzipc_fi_arm(unsigned long, unsigned long, long, long) __attribute__((weak));
void          dzipc_fi_disable(void)                                 __attribute__((weak));
unsigned long dzipc_fi_band_calls(void)                              __attribute__((weak));
unsigned long dzipc_fi_hits(void)                                    __attribute__((weak));
unsigned long dzipc_fi_last_band_size(void)                          __attribute__((weak));
}

namespace {

bool harness_loaded()
{
    return dzipc_fi_arm != nullptr && dzipc_fi_hits != nullptr && dzipc_fi_band_calls != nullptr;
}

constexpr const char *kSkipMsg =
    "分配失败注入工装未加载 ⇒ 本用例无判据力(0.3.1 第 3 条), ⛔ 不得当绿引用。"
    "运行方式见 tools/alloc_fault_inject/README.md; 漏了 LD_PRELOAD 就会走到这里。";

/* 只计数不拦截的宽档, 用来找出 handle_ 的分配尺寸。 */
constexpr unsigned long kProbeLo = 1;
constexpr unsigned long kProbeHi = 4096;

/* ipc::string 那条专用的大尺寸档: 1.5e6 字节的分配只可能来自本用例。 */
constexpr unsigned long kStrLo  = 1000000;
constexpr unsigned long kStrHi  = 2000000;
constexpr std::size_t   kStrLen = 1500000;

/* 拿到 handle_ 的分配尺寸; 返回 0 表示标定不可信(调用方应判 FAIL)。 */
unsigned long calibrate_handle_size()
{
    dzipc_fi_arm(kProbeLo, kProbeHi, 0, /*max_fails=*/0);   // 只计数

    unsigned long calls = 0, sz = 0;
    {
        ipc::shm::handle probe;
        calls = dzipc_fi_band_calls();
        sz    = dzipc_fi_last_band_size();
        (void)probe;
    }
    if (calls != 1) return 0;   // 宽档里不止一次 ⇒ 分不清哪一次是 handle_
    return sz;
}

}   // namespace

/* ---------------------------------------------------------------------------
 * 0) 工装健全性: 关掉之后不得再拦任何东西。
 *    这条守的是"工装把一切都拦掉"这个会让其余用例全部失去意义的情形。
 * ------------------------------------------------------------------------- */
TEST(AllocFaultInject, DisabledHarnessDoesNotIntervene)
{
    if (!harness_loaded()) GTEST_SKIP() << kSkipMsg;

    dzipc_fi_arm(0, 0, 0, 0);                    // hi <= lo ⇒ 不激活
    for (int i = 0; i < 16; ++i) {
        void *p = std::malloc(96);
        ASSERT_NE(p, nullptr) << "工装未激活时仍在拦截 ⇒ 工装失效";
        std::free(p);
    }
    EXPECT_EQ(dzipc_fi_hits(), 0u);
}

/* ---------------------------------------------------------------------------
 * 1) UF-002: pimpl 的"不舒服"分支分配失败后, handle 必须**不崩**、valid() 为 false、
 *    析构安全、入口空转。
 *
 * 旧态(今天): `~handle()` → `release()` → `impl(p_)->id_` —— **先解引用 p_ 再判 id_**,
 * 而 p_ == nullptr ⇒ 读地址 0 ⇒ SIGSEGV(本用例以退出码 139 终止, 这就是实测红)。
 * ------------------------------------------------------------------------- */
TEST(AllocFaultInject, HandlePimplAllocFailureMustNotCrash)
{
    if (!harness_loaded()) GTEST_SKIP() << kSkipMsg;

    /* 预热: 逼出 libipc 的静态初始化与一次性分配, 否则第一个落档的可能是初始化期的。 */
    dzipc_fi_arm(0, 0, 0, 0);
    { ipc::shm::handle warm; (void)warm; }

    const unsigned long sz = calibrate_handle_size();
    ASSERT_NE(sz, 0u) << "自标定失败: 宽档 [" << kProbeLo << "," << kProbeHi
                      << ") 内构造期分配不是恰好 1 次 ⇒ 分不清哪一次是 handle_, "
                         "本用例结论不可信";

    dzipc_fi_arm(sz, sz + 1, 0, /*max_fails=*/1);

    {
        ipc::shm::handle sh;                     // ← 唯一一次 sz 字节分配 ⇒ 必被拦

        const unsigned long calls = dzipc_fi_band_calls();
        const unsigned long hits  = dzipc_fi_hits();

        /* ⛔ 先立"确实注入了"这条, 再谈业务行为 —— 顺序不能反。 */
        ASSERT_GE(hits, 1u)
            << "尺寸档 [" << sz << "," << sz + 1 << ") 一次都没命中 ⇒ **假绿**, 判 FAIL。"
               "此时既不能说 handle 安全, 也不能说它危险。";
        ASSERT_EQ(calls, 1u)
            << "构造期档内分配不是 1 次 ⇒ 命中可能不是 handle_ 那一块, 结论不可信";

        /* UF-002 的行为判据(见 docs/unfixed_defects.md §2「判据」)。 */
        EXPECT_FALSE(sh.valid()) << "分配失败后 valid() 必须为 false";
        EXPECT_EQ(sh.size(), 0u);
        EXPECT_EQ(sh.detach(), nullptr) << "无 pimpl 时 detach() 应安全空转";
        EXPECT_EQ(sh.release(), -1)     << "无 pimpl 时 release() 应返回失败码而非崩";
    }
    /* ↑ 析构在此发生 —— 旧态就是在这里崩的 */
}

/* ---------------------------------------------------------------------------
 * 2) UF-001: allocator 在分配失败时 `noexcept` 返回 nullptr ⇒ libstdc++ 拿不到异常,
 *    直接在 nullptr 上 memmove ⇒ 写地址 0 的 SIGSEGV。
 *
 * 本用例只断言"**不崩**": 修法有选项 1(抛)/2(抛越界+记日志)/3(入口加闸)三种口径
 * 未拍(docs/unfixed_defects.md 0.5 第 1 条), 所以"抛不抛"不能写死在这条用例里 ——
 * 写死了会把"选了另一个选项"误判成回归。抛了也接受, 并打印实际形态。
 * ------------------------------------------------------------------------- */
TEST(AllocFaultInject, StringGrowAllocFailureMustNotCrash)
{
    if (!harness_loaded()) GTEST_SKIP() << kSkipMsg;

    dzipc_fi_arm(0, 0, 0, 0);
    { ipc::string warm; warm.resize(64); }        // 预热: 排除初始化期分配

    dzipc_fi_arm(kStrLo, kStrHi, 0, /*max_fails=*/1);

    bool threw = false;
    std::string what;
    try {
        ipc::string s;
        s.resize(kStrLen);                        // ← 唯一一次落档的分配 ⇒ 必被拦
    } catch (const std::exception &e) {
        threw = true;
        what = e.what();
    } catch (...) {
        threw = true;
        what = "(非 std::exception)";
    }

    const unsigned long hits = dzipc_fi_hits();
    ASSERT_GE(hits, 1u)
        << "尺寸档 [" << kStrLo << "," << kStrHi << ") 一次都没命中 ⇒ **假绿**, 判 FAIL";

    /* 执行到本行**本身**就是判据: 崩了就到不了这里, 而 libstdc++ 拿不到异常时会在
     * nullptr 上 memmove ⇒ 根本走不到下一句。
     *
     * ⛔ 这里刻意**不**断言 "长度保持 0" 之类的强保证: UF-001 的修法有选项 1(抛)/
     *    2(越界抛 + 失败记日志)/3(入口加闸)三种口径未拍(0.5 第 1 条), 写死了会把
     *    "选了另一个选项"误判成回归。只记录**实际形态**, 供 UF-001 落地后核对。 */
    testing::Test::RecordProperty(
        "uf001_failure_form",
        threw ? ("exception: " + what) : std::string("no-exception"));
    std::cout << "[UF-001] 分配失败后未崩 —— 报出形态: "
              << (threw ? ("异常: " + what)
                        : std::string("非异常(选项 2/3 的形态, 或仍为静默返回 nullptr)"))
              << "\n";
}

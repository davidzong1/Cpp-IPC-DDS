# 分配失败注入工装 (UF-000)

> 面向 [docs/unfixed_defects.md](../../docs/unfixed_defects.md) 的 **UF-000**。它的存在理由是
> **0.3.1 第 3 条**: 没有"尺寸档拦截计数", **"没注入"与"注入了但代码正确"不可区分** ——
> UF-001/UF-002 的判据会退化成"不注入时全绿", 而那是**回归不是判据**。
>
> ⛔ 本工装**不改任何产品码**(`src/libipc/**` 一个字节都不动), 也不进 CMake 树
> (与 `tools/sercli_live_probe` 同约定: 自带 `build.sh`, 拿仓库已有的头与
> `build/lib/libipc.so` 编)。

## 1. 两个命令 (UF-000 的可粘贴验收命令, 0.3.1 第 1 条)

```bash
# ① 构建工装(产物在 tools/alloc_fault_inject/bin/)
tools/alloc_fault_inject/build.sh

# ② 在被注入进程上跑 —— **不给档位 = 自标定**(见 §4); 给了档就是显式档
LD_PRELOAD=tools/alloc_fault_inject/bin/liballoc_fault_inject.so \
  tools/alloc_fault_inject/bin/fi_positive_control
```

第二条的形参是 `fi_positive_control <lo> <hi> [skip_n] [mode]`; `skip_n` = 放过前几次
**档内**分配(`0` = 一次都不放过), `mode` = `dtor`(默认, 构造+析构) / `valid` / `acquire`。

⛔ **`lo`/`hi` 不要写死 —— 尺寸会变, 写死就会用错档。**
`sizeof(ipc::shm::handle::handle_)` 随头文件演化, 且**没有任何常量能替你说它现在是多少**:

| 值 | 来源 | 现状 |
|---|---|---|
| **64** | 2026-09-18 **实测** —— **唯一**来源是阳性对照的**自标定行** `POSCTL INFO: 自标定 sizeof(handle_) 尺寸=64` | ✅ 当前真值 ⇒ 精确单字节档 = `[64,65)`. ⚠️ 直方图那条路**本轮未产出**任何直方图(见下方失效表), 故**不得**说"直方图已验证 64" |
| ~~56~~ | 本文 2026-09-18 **之前**的示例 | ❌ **过期**(当初是 `id_t(8)+void*(8)+ipc::string(32)+size_t(8)` 的**推算**, 从未实测) |

拿过期的 `56 57` 跑会得到 `rc=3`, 工装自报 `尺寸档未命中 ⇒ 工装没生效(不得当通过)` ——
⛔ 那是**工装没用上档**, **不是**"缺陷已修好"(后者是 `rc=4`)。两者含义相反, 别读反。
⇒ **默认不给参数**让程序自己标定, 尺寸变了也不会用错档。

一键重放上面全部(含自测、阴性对照、直方图、判据用例、`git diff --check`):

```bash
tools/alloc_fault_inject/run_acceptance.sh    # 全过程写 /tmp/uf000_acceptance.log
```

⛔ 读结果时看每条的 `### rc=`: **阳性对照 rc=139 才是通过**; rc=0/3/4 是"没崩"= FAIL。

⛔ **重放脚本自身有两处已知失效 + 一处残留过期示例**(2026-09-18 实测; 本轮**只订正了本文**,
那几处不在本轮写边界内, 均**待订正**):

| 位置 | 问题 | 后果 |
|---|---|---|
| `run_acceptance.sh:47-48` | 步骤 3c banner 写"期望 rc=139", 档位却写死过期的 `56 57` | 每轮必得 `rc=3`, 读起来像**验收失败**; 实际是**档过期**, 不是缺陷信号 |
| `run_acceptance.sh:50-53` | 步骤 3d 要打尺寸直方图, 但**给了 argv** ⇒ `fi_positive_control.cc:121` 固定 `max_fails=1`, 覆盖 `DZIPC_FI_ALLOC_MAX_FAILS=0` | 该步**必然崩在打印直方图之前** ⇒ **永远产不出直方图**; 尺寸自证只能取步骤 3 的自标定行 |
| `fi_positive_control.cc:22` | 头注释里的显式档示例写死 `56 57 0 dtor` | 照抄 = 踩 §1 那个过期档 |

⇒ 2026-09-18 那次验收里, **步骤 3(自标定, rc=139)** 才是**承重**的阳性对照; 3c / 3d 是辅助且
**两处都失效**。辅助档失效**不**推翻阳性对照(它们只是"再来一次"的不同形式)。

## 2. 三个程序

| 程序 | 作用 | 期望结果 |
|---|---|---|
| `bin/liballoc_fault_inject.so` | LD_PRELOAD 拦截器本体(malloc/calloc/realloc) | — |
| `bin/fi_selftest` | **工装自测**: 用裸 malloc 摆出确定序列, 断言"尺寸档 + SKIP_N + MAX_FAILS"语义与三个计数逐一对上 | 全部 `SELFTEST ok`, 退出码 0 |
| `bin/fi_positive_control` | **阳性对照**: 在已知含缺陷的旧态上跑真实注入, 目标路径必须崩 | **SIGSEGV(退出码 139)**; 退出码 0..4 = FAIL |

```bash
# 工装自测(必须带 LD_PRELOAD; 不带会报 BLOCKED 并返回 2, 不会静默通过)
LD_PRELOAD=tools/alloc_fault_inject/bin/liballoc_fault_inject.so \
  tools/alloc_fault_inject/bin/fi_selftest

# 阳性对照: mode 可选 dtor(默认) / valid / acquire
LD_PRELOAD=tools/alloc_fault_inject/bin/liballoc_fault_inject.so \
  tools/alloc_fault_inject/bin/fi_positive_control            # 自标定(默认 dtor)
LD_PRELOAD=tools/alloc_fault_inject/bin/liballoc_fault_inject.so \
  tools/alloc_fault_inject/bin/fi_positive_control 64 65 0 valid   # 64 = 2026-09-18 自标定实测值
```

⛔ `mode` **只能**由 argv 给 —— 不给参数就是"自标定 + `dtor`"。所以要换 mode 时, 先跑一次
**不带参数**的自标定把**当前**尺寸读出来, 再把那个数填进 `lo`/`hi`; **别用本文任何历史数字**
(见 §1 的尺寸表)。

⛔ **阳性对照的读法**(0.3.1 第 2 条): **崩了才算工装有效**。
`退出码 0/3/4` 表示"没崩", 含义是**工装没生效或缺陷已被修掉** —— 绝不可当通过。
`退出码 2` 是环境错误(工装没加载 / 自标定不可信), 既不是 PASS 也不是 FAIL 判定。

## 3. 环境变量与进程内接口

| 环境变量 | 含义 | 默认 |
|---|---|---|
| `DZIPC_FI_ALLOC_SIZE_LO` | 尺寸档下界(inclusive) | 无 ⇒ **工装不激活** |
| `DZIPC_FI_ALLOC_SIZE_HI` | 尺寸档上界(exclusive), 须 > LO | 无 |
| `DZIPC_FI_ALLOC_SKIP_N` | 放过前 N 次**档内**分配, 从第 N+1 次开始拦 | `0` |
| `DZIPC_FI_ALLOC_MAX_FAILS` | 最多拦几次; `-1` = 不限 | `1` |
| `DZIPC_FI_TRACE` | 非 0 ⇒ 每次档内分配打一行(尺寸 + 序号) | `0` |
| `DZIPC_FI_HISTOGRAM` | 非 0 ⇒ 退出时打印档内**尺寸直方图**(用来发现某对象的尺寸) | `0` |
| `DZIPC_FI_REPORT` | 把 FINAL 行同时写进该文件(供脚本解析) | 无 |

退出时**总是**往 stderr 打一行汇总(0.3.1 第 3 条要求的"自报拦截计数"):

```
[alloc_fault_inject] FINAL active=1 band=[64,65) skip_n=0 max_fails=1 total_calls=1234 band_calls=1 skipped=0 hits=1
```

进程内接口(供 `test/test_alloc_fault_inject.cpp` 用, **weak 符号引用** ⇒ 不需要
`-ldl`、不需要改 `test/CMakeLists.txt`):

```c
void          dzipc_fi_arm(unsigned long lo, unsigned long hi, long skip_n, long max_fails);
              /* 设档 + 清零计数 + 激活。用它是为了"arm 之后第 k 次档内分配"确定可预测 ——
                 进程启动期(stdio/locale/区域)自己会在不少尺寸档上分配, 纯 env 的 SKIP_N
                 计数会被它们吃掉。 */
void          dzipc_fi_disable(void);
void          dzipc_fi_reset_counters(void);
unsigned long dzipc_fi_total_calls(void);   /* malloc 族调用总数 */
unsigned long dzipc_fi_band_calls(void);    /* 尺寸落在档内的调用数 */
unsigned long dzipc_fi_skipped(void);       /* 其中被 SKIP_N 放过的 */
unsigned long dzipc_fi_hits(void);          /* 实际返回 NULL 的次数 = 拦截数 */
unsigned long dzipc_fi_last_band_size(void);/* 最近一次档内分配的尺寸 */
int           dzipc_fi_active(void);
```

## 4. UF-001 / UF-002 怎么用它

**判据用例**: [`test/test_alloc_fault_inject.cpp`](../../test/test_alloc_fault_inject.cpp)
(由 `test/CMakeLists.txt` 的 `file(GLOB)` 自动收进构建树)。
⛔ 新增 `.cpp` 后**必须先重跑 `cmake -S . -B build`**, 否则改的是源码、跑的是旧二进制。

```bash
cmake -S . -B build && cmake --build build -j8 --target test_alloc_fault_inject
LD_PRELOAD=tools/alloc_fault_inject/bin/liballoc_fault_inject.so \
  ./build/bin/test_alloc_fault_inject
# 只跑一条:
LD_PRELOAD=tools/alloc_fault_inject/bin/liballoc_fault_inject.so \
  ./build/bin/test_alloc_fault_inject \
  --gtest_filter='AllocFaultInject.HandlePimplAllocFailureMustNotCrash'
```

判据文件里两条硬门, 缺一即**不得当验收证据**:

1. **未加载工装 ⇒ `GTEST_SKIP`**, 不是绿色通过。SKIP 的含义是"本用例无判据力"。
2. **`hits >= 1` 先于一切业务断言**。命中为 0 时判 **FAIL**(假绿),
   因为此时既不能说目标安全、也不能说它危险。

`handle` 那条**自标定**: 先用"只计数不拦截"的宽档 `[1,4096)` 测出 `sizeof(handle_)`,
再用精确到那一个字节的档去拦; 并当场断言"宽档内构造期分配恰好 1 次", 否则判结论不可信。
所以 `sizeof(handle_)` 变化时用例不会用错档。

**当前(未修)预期**: 两条业务用例以 **SIGSEGV(139)** 终止 —— 这就是 UF-001/UF-002 的实测红。
修好之后才应转绿。

## 5. 覆盖边界(如实声明)

- 只拦 `malloc` / `calloc` / `realloc`。**不拦** `aligned_alloc` / `posix_memalign` /
  `memalign` / `valloc` / `mmap`。libipc 的 `static_alloc::alloc` 就是 `std::malloc`
  (`src/libipc/memory/alloc.h:20-27`), 故这条边界不影响 UF-001/UF-002。
- 引导期(dlsym 自己可能调 `calloc`)走 64 KiB 静态缓冲, **不计入**任何计数。
- 工装自己的日志(`TRACE` / `FINAL` / 直方图)在 `g_in_hook` 闸门下走真实分配器,
  既不递归、也不把自己的日志内存拦掉。
- 多线程下计数用 `__atomic`, 但"第 N 次命中"本身是全局序号 ⇒ 并发用例中命中**归属**不确定。
  并发场景请用进程内 `arm()` 并在单线程窗口内做被测动作。

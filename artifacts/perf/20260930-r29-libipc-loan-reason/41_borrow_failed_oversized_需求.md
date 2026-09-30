# 接口需求（转 W03 / counters.h 维护者）：`borrow_failed_oversized` 的落点改判

## 1. 事实（本包实测，两路互证）

| 证据 | 文件 | 结论 |
|---|---|---|
| 8 组合穷举 | `01_classifier_exhaustion.log` | `note_dzflat_borrow_failed` 的 2³ 输入空间可达集合 = `{no_receiver, publish, reason_unknown}`；**`oversized` 恒不可达** |
| 分类器 vs 落点分歧 | `03_divergence.log` | 同一逻辑事件（借样成功、`finalize` 失败 = 超变长预算）：旁路 `classify_dzflat_attempt({borrow_requested=T, borrow_ok=T, write_ok=F})` ⇒ **`borrow_failed_oversized`**；真实落点 `note_dzflat_borrow_failed(had_receiver=T, finalize_ok=F, publish_ok=F)` ⇒ **`borrow_failed_reason_unknown`** |
| 调用点自称语义 | `include/dzIPC/measure/counters.h:641` | 注释写 "`finalize_ok=false` ⇒ 超变长预算（调用点语义确定）⇒ `borrow_failed_oversized`" —— 但函数体在 `!finalize_ok && !publish_ok` 时走的是 `had_receiver ? reason_unknown : no_receiver`，**实现与自己的注释不一致** |
| 调用点来源 | `include/dzIPC/shm_pub_sub_ipc.h:126` | `publish_loaned` 在 `if (!lo.finalize())` 分支传 `(had_receiver, false, false)` —— 该事件**语义确定**是超预算 |

**根因**：唯一活着的生产落点 `note_dzflat_borrow_failed(bool,bool,bool)` 只有 3 个 bool，
其中 `finalize_ok=false` 被**两个不同事件**共用：
（i）`lo` 无效（借样没成功）与（ii）`lo` 有效但 `finalize` 失败（超预算）。
现有实现用 `had_receiver` 再分，把 (ii) 也吞进了 `reason_unknown`。
另一处 `classify_dzflat_attempt` **有** `write_ok` 这一维，所以它能判对 ——
但它的 `(c)` 分支在真路径上**根本不执行**（`borrow_requested` 在 `src/` 命中 **0**，
三个 `note_dzflat_attempt` 调用点都没填），故那条正确路径是死代码。

## 2. 为什么本任务不能自己改

`note_dzflat_borrow_failed` 的判定表在 **`include/dzIPC/measure/counters.h`** —— 该文件由
**W03 维护**，t46 的边界明确写 "⛔ 不改 `counters.h`"。改它属于接口/口径变更，须走 W03。

## 3. 请求的改动（最小形态，任选其一）

**方案 ①（推荐，改落点参数映射，不动签名）**
把"两个出口合流"拆开：`publish_loaned` 的三处调用点已经**能**区分它们
（`!lo.valid()` vs `!lo.finalize()` vs `!publish_loan`）。给落点加一个显式的原因参数：

```cpp
/* 新增重载；旧三 bool 版保留（源兼容），内部转发到新重载 mapped 到现有语义。 */
inline void note_dzflat_borrow_failed(bool had_receiver, bool finalize_ok,
                                      bool publish_ok,
                                      bool loan_ok = true) noexcept
{
    if (loan_ok && !finalize_ok && !publish_ok) { r.inc(borrow_failed_oversized); return; }
    ... 其余分支不变 ...
}
```
调用点（`include/dzIPC/shm_pub_sub_ipc.h`，**本包已提供补丁 40_ 的框架**）：
* `!lo.valid()`         ⇒ `loan_ok=false, finalize_ok=false`（保持今天的 `no_receiver`/`unknown` 分流）
* `!lo.finalize()`      ⇒ `loan_ok=true, finalize_ok=false` ⇒ **`borrow_failed_oversized`** ✅
* `!publish_loan`       ⇒ `finalize_ok=true`（今天的 `borrow_failed_publish`，不变）

**方案 ②（更小，只改判据顺序）**
在现函数里把"两个出口合流"那一支改为：`had_receiver` 且**调用点声明是超预算**时记 oversized。
需要调用点传第 4 个 bool，与方案 ① 实质相同。

**方案 ③（不建议）** 让落点反过来调用 `classify_dzflat_attempt`。
会增加一次结构体构造（热路径已有 3 个 bool，构造 `DzFlatAttempt` 是 6 个 bool + 2 个 u32），
且 `classify` 的 `(c)` 语义更宽，会改变 `no_receiver`/`unknown` 的既有读数 ⇒ 有口径漂移风险。

## 4. 验收判据（W03 落地后本包可直接复算）

```bash
# 本包提供的 8 组合穷举：改后 `borrow_failed_oversized` 必须出现在可达集合里
artifacts/perf/20260930-r29-libipc-loan-reason/classifier_exhaustion <hr> <fo> <po>
# 同臂：借样成功 + 超变长预算 ⇒ oversized 0→非 0，且 publish 族不被污染
```
落点改后，**`borrow_requested` 的死代码问题**也应一并处置（否则 `classify_dzflat_attempt`
的 `(c)` 分支仍是永不执行的路径，将来改它的人会以为它有效）——建议二选一：
(a) 三个 `note_dzflat_attempt` 调用点补填 `borrow_requested`；或
(b) 在 `counters.h` 注释里**显式登记**该分支为"仅供单测穷举、生产路径未启用"，避免误读。

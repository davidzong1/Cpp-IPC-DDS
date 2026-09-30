# `deferred_depth_*` 结束值接口 —— 规格提交（W06/R2 → W03）

> 提出方：**接收池负责人**（W06/接收侧，t42 / attempt `639720ce-5384-4501-bb64-cb1ba96889e7`）。
> 受理方：**测量统计负责人**（W03，维护 `include/dzIPC/measure/**` 与字段语义登记）。
> 依据：`W10/R0_口径与接口冻结.md` **R0-9 / R0-10**（§5.2 冻结条款、§6.1 R0-11 写入权、§7 ABI 纪律）。
> 状态：**规格（未落地）**。⛔ 本文件不构成"已实现"；落地后由我在 `recv_worker.cc` 接线，再出新 run 验收。
> 完整上下文（含 §6.2 四项最小验证与缺失反例重做）见同目录 `R2_T30补齐_交付.md`。

---

## 规格正文（原 R2/T30 交付文档 §4，编号已就地归一）

### 3b.0 缺口描述：`deferred_depth_*` 的**结束值接口**（提交 W03，我随后接线）

> ⛔ 本包**不改** `include/dzIPC/measure/counters.h`（W03 维护）。以下规格按"字段名 / 语义 / 单位 / nullable / 写入点 / 兼容 / 验收"给出，W03 可直接据此落地。

### S0 现状为什么不够（R0-10 的三条）

| 现状 | 问题 |
|---|---|
| `ScanRoundScope(scanned, deferred_depth)` 在**构造时**固定深度（`counters.h:646-655`） | 取的是**入队前**的 `deferred.size()`（`recv_worker.cc:232`），方案 §6.1 要的是**扫描后**的深度 |
| 析构写**全局 gauge**（`:662-666`） | 多 worker 同时有深度时，全局 gauge 只剩"最后写入者"的值；全池总量 = Σ(每 worker)，⛔ 不能由任一 gauge 表达 |
| 只有 `deferred_depth_last/_max` | 无法表达「扫描**后**队列深度」与「本轮**入队的就绪 route 数**」 |

### S1–S2 规格（可直接落地）

**S1｜`ScanRoundScope` 结束值出参**

```cpp
class ScanRoundScope {
public:
    ScanRoundScope(std::size_t scanned_routes, std::size_t deferred_depth_before) noexcept;  // 既有签名，不改
    /* 新增（追加，不改既有成员/语义）：在遍历结束、常驻写之前调用一次。
     *   ready_routes        本轮**新入队**的待处理 route 数（即当前 queued 计数）
     *   deferred_depth_after 扫描**结束后**、运行预算**之前**的 deferred.size()
     * 语义：与既有 set_ready(bool) 并存；先调 finish() 再调 set_ready() 或反之都允许，
     *       但 ready_routes > 0 与 set_ready(true) 必须同时出现（同一事实的两种精度）。 */
    void finish(std::size_t ready_routes, std::size_t deferred_depth_after) noexcept;
};
```

| 建议新增 `CounterId`（**追加在 `count` 之前**，不改既有枚举值） | 名称 | 单位 | category | nullable | 语义（写清才不歧义） |
|---|---|---|---|---|---|
| `scan_ready_routes_total` | `scan_ready_routes_total` | count | scan | false（无证据时写 0，与其它 count 一致） | Σ 每轮 `ready_routes`；`scan_ready_routes_total / scan_rounds` = **平均每轮新入队 route 数**（比既有 0/1 二值的 `ready_observed` 精度更高，⛔ 两者**不得混算**：前者是计数、后者是轮数） |
| `deferred_depth_after_last` | `deferred_depth_after_last` | count | scan | false | 最近一轮**扫描后**的 deferred 深度（全局 gauge，⛔ 与既有 `deferred_depth_last`(入队前)**并存且语义不同**，报告须分列） |
| `deferred_depth_after_max` | `deferred_depth_after_max` | count | scan | false | 扫描后深度的最大值（全局 gauge，兼容用途） |
| `deferred_depth_after_total` | `deferred_depth_after_total` | count | scan | false | Σ 每轮扫描后深度；`/ scan_rounds` = 平均扫描后深度（**按 worker 汇总时可直接相加**，这是唯一可安全求和的深度量） |

**S2｜`ScanRoundScope` 的**每 worker 出参**（供 `RecvWorkerStats` 承接，而非全局 gauge）**

> R0-10 已冻结：「谁导出 = 接收池负责人给需求与结果；共享层串行集成到 `RecvWorkerStats`（追加字段）；W03 维护头文件与字段语义登记」。
> 因此 **`ScanRoundScope` 只需把「扫描后深度」与「本轮就绪数」变成**返回值/出参**，per-worker 的累积由 `RecvWorkerStats` 做（不受 `diagnostics_only` 门控）。建议 W03 提供最小组件：

```cpp
/* 追加（不改既有 ScanRoundScope 用法）：一次扫描的结束值快照，**不写全局计数**。 */
struct ScanRoundResult {
    std::size_t scanned_routes{0};          // 本轮遍历 route 数
    std::size_t ready_routes{0};            // 本轮新入队 route 数
    std::size_t deferred_depth_before{0};   // 扫描前深度
    std::size_t deferred_depth_after{0};    // 扫描后深度  ← R0-10 要的量
    std::uint64_t elapsed_ns{0};            // 区间耗时（仅诊断开启时非零；0 = 未采集，⛔ 非"零成本"）
};
/* 便捷写法（一次调用完成既有 + 新增语义；⛔ 不替代既有 ScanRoundScope，只并列提供）。 */
ScanRoundResult complete_scan_round(std::size_t scanned_routes,
                                    std::size_t deferred_depth_before,
                                    std::size_t ready_routes,
                                    std::size_t deferred_depth_after) noexcept;
```

**S3｜写入点（我随后接线，逐点写明以便 W03 与我并行）**

| 量 | 写入点（我将落地的位置） | 门控 |
|---|---|---|
| `scan_time_ns_total`（既有） | `recv_worker.cc:268`（`ScanRoundScope` 析构） | 诊断门控（不变） |
| `scan_ready_routes_total` | `:248` 之后、`set_ready` 同一位置 | 诊断门控 |
| `deferred_depth_after_last/_max/_total` | `:248` 之后（**扫描后**取 `deferred.size()`） | 诊断门控 |
| `RecvWorkerStats::{scan_rounds, scanned_routes_total, scan_ready_rounds, deferred_depth_after_last, deferred_depth_after_max, deferred_depth_after_total}` | 同一位置（无时钟、无门控；`fetch_add`/`store`/CAS） | **常驻** |
| `RecvWorkerPool::stats()` | `deferred_depth_after_*` 取 **max**（last/max）、`_total` 取 **sum** —— 与既有 `deferred_depth_last/_max` 同一聚合纪律 | — |

**S4｜兼容与纪律**

- ⛔ 既有 `ready_observed`、`deferred_depth_last`、`deferred_depth_max` **一个都不删、不改语义**（只追加）。R0-8 已冻结 `ready_observed = set_ready(queued>0) 每轮最多 1`，保持不变。
- ⛔ 新增 ID 一律 `diagnostics_only = true`（与 scan 族一致）；常驻面走 `RecvWorkerStats` 追加字段（**ABI 纪律**：追加 ⇒ 引用方必须随库重编，见 R0 §7）。
- ⛔ 两个 gauge **语义不同不得互相覆盖**：`deferred_depth_last` = 入队前、`deferred_depth_after_last` = 扫描后；报告必须分列并写明。

**S5｜W03 落地后我的验收断言（我先写好，落地即可跑）**

| 断言 | 判据 |
|---|---|
| 1 结束值单调可核对 | 单 route 灌 N 条、每轮预算 1：`Σ scan_ready_routes_total == N`（每轮恰好重新发现 1 条），`deferred_depth_after_last ≥ deferred_depth_last` |
| 2 深度可求和 | 多 worker 同时有深度时，`pool.stats().deferred_depth_after_total ≥ max(各 worker 的 _total)`，而**任一** gauge 值不得被当全池总量 |
| 3 诊断门控仍有效 | `diag=off` ⇒ 4 个新门控计数恒 0、新常驻字段非零（两套同源判据沿用 t30 的比值判据） |
| 4 语义不混算 | 报告/判据中 `ready_observed` 与 `scan_ready_routes_total` **不得相加**；`deferred_depth_last` 与 `deferred_depth_after_last` **不得混列一列** |

---


---

## 附：本规格的验收断言（落地后由我做，写在此处便于 W03 自测）

| 断言 | 判据 |
|---|---|
| 1 结束值单调可核对 | 单 route 灌 N 条、每轮预算 1：`Σ scan_ready_routes_total == N`，且 `deferred_depth_after_last ≥ deferred_depth_last` |
| 2 深度可求和 | 多 worker 同时有深度：`pool.stats().deferred_depth_after_total ≥ max(各 worker _total)`；⛔ 任一 gauge 不得当全池总量 |
| 3 门控仍有效 | `diag=off` ⇒ 新门控计数恒 0、新常驻字段非零（沿用 t30 的比值自洽判据） |
| 4 不得混算 | `ready_observed` 与 `scan_ready_routes_total` 不得相加；`deferred_depth_last` 与 `deferred_depth_after_last` 不得混列 |
| 5 ABI 纪律 | `RecvWorkerStats` 追加字段后，**所有引用方（含工装）必须随库重编**（R0 §7 / W00 §11.4 D-29） |

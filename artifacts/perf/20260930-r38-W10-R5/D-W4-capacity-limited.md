# W=4 容量受限档（单独实验，D 档）

## 现象
- 容量模型 127×4 = **508**（recv_wait_set kMaxRoutes=127/worker）
- n=1000 ⇒ **492 条溢出**，seam 报文：`shm recv worker unusable (wait_set_full (per-worker 127 token capacity exceeded)); receive on per-topic compat thread`
- 该 run 的窗口 routes = **508**（不是 1000）⇒ 池内归属只剩 508，其余 492 走**显式回退**的 per-topic 兼容线程
- 但 **recover_ok_routes = 1000/1000**（含回退线程接住的那部分）⇒ 规模语义**未被容量边界破坏**，只是线程账不同

## 判据分两支（方案 §6.3 要求）
| 档 | 容量 | routes(窗口) | 判据 |
|---|---|---|---|
| 不受限（W=8/16/32, n=1000） | ≥1000 | 1000 | `residual==0`（全部归属 worker） |
| **受限（W=4, n=1000）** | 508 | **508** | `residual==overflow==492`（**不得**判为泄漏或未归属） |

⇒ 受限档的 `scan/round = 127.000000` **正是**每 worker 满载 127 条的证据（非应该 250 却只有 127）。

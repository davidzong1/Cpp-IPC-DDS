# R2/t42 §6.2 最小验证矩阵（缺失 4 项）逐轮结果

> 每轮 = 一次完整 `w10_r2_driver.sh`（5 个独立进程：case1/2/3/4-对照/4-注入）

| 轮 | 用例 1 无 route | 2 预算耗尽重入 deferred | 3 断开/注销 | 4 后端错误(对照) | 4 后端错误(注入) |
|---|---|---|---|---|---|
| round1 | PASS | PASS | PASS | PASS | PASS |
| round2 | PASS | PASS | PASS | PASS | PASS |
| round3 | PASS | PASS | PASS | PASS | PASS |
| round4 | PASS | PASS | PASS | PASS | PASS |
| round5 | PASS | PASS | PASS | PASS | PASS |

**总判定：PASS**（25/25 项 PASS）

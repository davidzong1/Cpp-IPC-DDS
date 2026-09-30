# W10 independent / shm — 判定

- run_id: `20260929-r25-W10-shm-ind-1`
- 拓扑: `independent`（1 个话题，每 route 3 条，载荷 64 B）
- 判定: **通过**

## §13.2 六条

| # | 条件 | 实测 | 判定 |
|---|---|---|---|
| 1 | registered_count == expected | 1/1 | ✅ |
| 2 | valid_rx_count == expected（逐 route 序号+载荷校验） | 1/1 | ✅ |
| 3 | fallback_count == 0 | 0 | ✅ |
| 4 | worker/控制线程符合冻结配置 | worker_path=1 threads=2（create 后） / 3（reclaim 后） | ✅ |
| 5 | 关闭/恢复在冻结超时内且旧 generation 无投递 | recover_ok=1 lost=0 | ✅ |
| 6 | 结束时 route/token/fd/队列/chunk 回基线（±5%） | routes 0→0，fd 4→4 | 见下 |

## 失败清单（0 条）

（无）

## 既有缺陷侦察（⛔ 与本轮改造引入的回归分开登记）

（本次未触发）

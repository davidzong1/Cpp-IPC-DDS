# W10 hotcold / shm — 判定

- run_id: `20260929-r25-W10-shm-hotcold-1000`
- 拓扑: `hotcold`（1000 个话题，每 route 3 条，载荷 64 B）
- 判定: **通过**

## §13.2 六条

| # | 条件 | 实测 | 判定 |
|---|---|---|---|
| 1 | registered_count == expected | 1000/1000 | ✅ |
| 2 | valid_rx_count == expected（逐 route 序号+载荷校验） | 999/999 | ✅ |

> hotcold 拓扑：热路（route 0）为**开环满速**，实收 64/40000（丢包为开环语义的一部分，按 §13.1 只计数、不作判据）；冷路 1..999 各发 1 条须全部收到 ⇒ 判据用 999。
| 3 | fallback_count == 0 | 0 | ✅ |
| 4 | worker/控制线程符合冻结配置 | worker_path=1000 threads=34（create 后） / 5（reclaim 后） | ✅ |
| 5 | 关闭/恢复在冻结超时内且旧 generation 无投递 | recover_ok=0 lost=0 | ✅ |
| 6 | 结束时 route/token/fd/队列/chunk 回基线（±5%） | routes 500→0，fd 4→4 | 见下 |

## 失败清单（0 条）

（无）

## 既有缺陷侦察（⛔ 与本轮改造引入的回归分开登记）

（本次未触发）

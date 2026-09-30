# W10 broadcast / shm — 判定

- run_id: `h-bc-32`
- 拓扑: `broadcast`（32 个话题，每 route 3 条，载荷 64 B）
- 判定: **通过**

## §13.2 六条

| # | 条件 | 实测 | 判定 |
|---|---|---|---|
> **H6 机械断言**（c1..c6 由总判定直接产生，⛔ 不用日志提示代替失败状态）：c1=1 c2=1 c3=1 c4=1 c5=1 c6=部分测量（不可判定）

| 1 | registered_count == expected | 32/32 | ✅ |
| 2 | valid_rx_count == expected（逐 route 序号+载荷校验） | 32/32；dup=0 ooo=0 corrupt=0 timeout_routes=0 | ✅ |
| 3 | fallback_count == 0 | 0 | ✅ |
| 4 | worker/控制线程符合冻结配置（⛔ 无 per-route 补齐） | worker_path=32（seam）pool_routes_alive=32 threads=2（create 后） / 3（reclaim 后） | ❌ |
| 5 | 关闭/恢复在冻结超时内且旧 generation 无投递 | recover_ok=1 lost=0 | ✅ |
| 6 | 结束时 route/token/fd/队列/chunk 回基线（±5%） | routes 16→0，fd 4→4，**token（代理：存活期池内在册数→销毁后 0）** 16→0；queue/chunk 见下 | 部分测量 |

> §13.2 #6 的**逐类结论**（⛔ 不接受把未测项打成 ✅）：

| 资源 | 实测 | 判定 |
|---|---|---|
| route | 16→0 | ✅ 已测 |
| token | 池内在册 **16→0**（与 token 一一对应：add_route 取 token、remove_route 同步摘除） | ✅ 已测（代理） |
| fd | 4→4 | ✅ 已测 |
| queue（view/adopt 深度） | ⛔ **无公开读数 API** | **未测（不可判定）** |
| chunk（池内占用） | ⛔ **无公开读数 API** | **未测（不可判定）** |

## 失败清单（0 条）

（无）

## 载荷校验失败明细（首次，最多每 route 一条）

（无）

## 既有缺陷侦察（⛔ 与本轮改造引入的回归分开登记）

（本次未触发）

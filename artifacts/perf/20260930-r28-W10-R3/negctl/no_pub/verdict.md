# W10 independent / socket — 判定

- run_id: `negctl`
- 拓扑: `independent`（20 个话题，每 route 1 条，载荷 64 B）
- 判定: **规模试验失败**

## §13.2 六条

| # | 条件 | 实测 | 判定 |
|---|---|---|---|
> **H6 机械断言**（c1..c6 由总判定直接产生，⛔ 不用日志提示代替失败状态）：c1=0 c2=0 c3=1 c4=0 c5=0 c6=部分测量（不可判定）

| 1 | registered_count == expected | 0/20 | ❌ |
| 2 | valid_rx_count == expected（逐 route 序号+载荷校验） | 0/20；dup=0 ooo=0 corrupt=0 timeout_routes=20 | ❌ |
| 3 | fallback_count == 0 | 0 | ✅ |
| 4 | worker/控制线程符合冻结配置（⛔ 无 per-route 补齐） | socket_pool_routes_alive=20（池内归属，socket 无 seam） threads=33（create 后） / 4（reclaim 后） | ✅ |
| 5 | 关闭/恢复在冻结超时内且旧 generation 无投递 | recover_ok=0 lost=6 | ❌ |
| 6 | 结束时 route/token/fd/队列/chunk 回基线（±5%） | routes 10→0，fd 108→68，**token（代理：存活期池内在册数→销毁后 0）** 10→0；queue/chunk 见下 | 部分测量 |

> §13.2 #6 的**逐类结论**（⛔ 不接受把未测项打成 ✅）：

| 资源 | 实测 | 判定 |
|---|---|---|
| route | 10→0 | ✅ 已测 |
| token | 池内在册 **10→0**（与 token 一一对应：add_route 取 token、remove_route 同步摘除） | ✅ 已测（代理） |
| fd | 108→68 | ✅ 已测 |
| queue（view/adopt 深度） | ⛔ **无公开读数 API** | **未测（不可判定）** |
| chunk（池内占用） | ⛔ **无公开读数 API** | **未测（不可判定）** |

## 失败清单（27 条）

- §13.2#1 registered_count=0/20（台账重算 0）
- §13.2#1 route w10_socket_independent_5302_0 未收到确认帧（池内在册=20）
- §13.2#1 route w10_socket_independent_5302_2 未收到确认帧（池内在册=20）
- §13.2#1 route w10_socket_independent_5302_3 未收到确认帧（池内在册=20）
- §13.2#1 route w10_socket_independent_5302_4 未收到确认帧（池内在册=20）
- §13.2#1 route w10_socket_independent_5302_7 未收到确认帧（池内在册=20）
- §13.2#1 route w10_socket_independent_5302_9 未收到确认帧（池内在册=20）
- §13.2#1 route w10_socket_independent_5302_10 未收到确认帧（池内在册=20）
- §13.2#1 route w10_socket_independent_5302_14 未收到确认帧（池内在册=20）
- §13.2#1 route w10_socket_independent_5302_16 未收到确认帧（池内在册=20）
- §13.2#1 route w10_socket_independent_5302_17 未收到确认帧（池内在册=20）
- §13.2#1 route w10_socket_independent_5302_18 未收到确认帧（池内在册=20）
- §13.2#1 route w10_socket_independent_5302_21 未收到确认帧（池内在册=20）
- §13.2#1 route w10_socket_independent_5302_22 未收到确认帧（池内在册=20）
- §13.2#1 route w10_socket_independent_5302_24 未收到确认帧（池内在册=20）
- §13.2#1 route w10_socket_independent_5302_28 未收到确认帧（池内在册=20）
- §13.2#1 route w10_socket_independent_5302_31 未收到确认帧（池内在册=20）
- §13.2#1 route w10_socket_independent_5302_34 未收到确认帧（池内在册=20）
- §13.2#1 route w10_socket_independent_5302_35 未收到确认帧（池内在册=20）
- §13.2#1 route w10_socket_independent_5302_37 未收到确认帧（池内在册=20）
- §13.2#1 route w10_socket_independent_5302_39 未收到确认帧（池内在册=20）
- §13.2#2 valid_rx_count=0 != expected 20
- §13.2#2 超期未收满 timeout_routes=20
- §13.2#4 存活期池 route_count=20 != registered=0（有 route 未归属 worker）
- §13.2#5 §10.1 恢复首包失败（delay_us=-1，阶段耗时=7002616）
- §13.2#5 恢复延迟为负（-1）
- §13.2#5 恢复阶段丢包=6

## 载荷校验失败明细（首次，最多每 route 一条）

（无）

## 既有缺陷侦察（⛔ 与本轮改造引入的回归分开登记）

（本次未触发）

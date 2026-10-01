# t84 反向验证（判据的判据）：三次消融，每次只改一处

常驻用例 = `test/test_w05_stale_slot_gate_arm.cpp`（**双臂**：默认臂 + L1 回退臂），
每条用例在**子进程**里跑（环境变量是进程内只读一次的静态量）。

| # | 消融点（只改一处） | 默认臂 | L1 回退臂 | 日志 |
|---|---|---|---|---|
| ① | 调度器 `dispatch()` 改回 `if (has_peers) { stale_scan }` | **FAILED**（码 1 = 未回收） | PASSED（未受影响） | `neg1_scheduler_arm.log/.diff` |
| ② | `compat_control_loop()` 改回同形态 | PASSED（未受影响） | **FAILED**（码 1） | `neg2_compat_arm.log/.diff` |
| ③ | **唯一判据** `PubControlState::on_pub_tick()` 内部兜底分支删掉 | **FAILED**（码 1） | **FAILED**（码 1） | `neg3_single_judgement.log` |

⛔ **③ 是本轮承重证据**：判据上收成**一处**之后，消融那一处 ⇒ **两臂同时变红**
（修复前是"消融调度器只红一臂"，正是 N1 的形态）。⇒ 结构上不可能再"修一条漏一条"。

fail 码含义：1 = 陈旧槽位未被回收；2 = 活订阅者被误断；3 = 用例前提不成立；-1 = 子进程异常。

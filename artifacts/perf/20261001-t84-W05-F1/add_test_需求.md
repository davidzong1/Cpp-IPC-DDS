# 给架构负责人：W05 双臂常驻用例的 `add_test` 需求（t84）

**背景**：队长 W05 第 3 轮裁定 / R1 第 3 轮 N1 查明 —— W05 有**两条驱动源**（进程级调度器 +
每话题兼容线程，后者是 `DZIPC_SHM_CONTROL_SCHEDULER=1` 的 **L1 运行时回退**），
t77 只修了前者 ⇒ 回退臂 **400 发 0 收**（比基线 400/400 更差）。t84 把判据收敛成唯一自由函数
`pub_control_tick()` 并双臂闭合。

⇒ 需要两条常驻用例（**各自 arm**），target 均已由 `test/CMakeLists.txt` 的 `file(GLOB)` 自动生成：

```cmake
# t84: W05 控制面 stale 门控 —— **双臂**回归（默认臂 + L1 回退臂）。
# 依据队长 W05 第 3 轮裁定 / R1 第 3 轮 N1：回退臂属 §4.2 的 L1 回滚入口，⛔ 不得比基线更差。
# 两条用例各自 fork 子进程（环境变量是进程内只读一次的静态量），等待 peer_dead_timeout(2s) 量级。
# 反向验证（消融唯一判据 pub_control_tick ⇒ 两臂同时红）见
# artifacts/perf/20261001-t84-W05-F1/negative/。
add_test(NAME test_w05_stale_slot_gate_arm COMMAND test_w05_stale_slot_gate_arm)
set_tests_properties(test_w05_stale_slot_gate_arm PROPERTIES RUN_SERIAL TRUE TIMEOUT 180)

# t77 的端到端用例（同样覆盖双臂逻辑的单臂版本；保留）
add_test(NAME test_w05_stale_slot_gate COMMAND test_w05_stale_slot_gate)
set_tests_properties(test_w05_stale_slot_gate PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)
```

**为什么 `RUN_SERIAL`**：两条用例都**真实建控制面段并等到 `peer_dead_timeout`(2s) 量级**，
与其它建段的用例并发跑会互相干扰（同 W04 对共享段类用例的结论）。

**验收（落地后请复核）**：
```bash
cmake -S . -B build && ctest --test-dir build -N | grep w05_stale_slot_gate   # 期望两条都在
ctest --test-dir build -R test_w05_stale_slot_gate                             # 期望 3/3 Passed（arm 2 条 + 单臂 1 条）
```
⛔ 不改 `test/CMakeLists.txt`（归架构负责人）。

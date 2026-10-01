# 给架构负责人：`test_w05_stale_slot_gate` 的 `add_test` 需求（t77）

**背景**：t77 按队长裁定 §4.4 修掉了「`peer_count()==0` 时整条跳过 stale 扫描」的正确性回归，
新增常驻用例 `test/test_w05_stale_slot_gate.cpp` 覆盖「`peer_count()==0` + 陈旧 `in_use` 槽位」。
target 由 `test/CMakeLists.txt` 的 `file(GLOB)` **自动生成**（已可 `make -C build -j16 test_w05_stale_slot_gate`），
但 `file(GLOB)` 造出的 target **默认不进 CTest** ⇒ 还差一行 `add_test`。

**请求**（在 `test/CMakeLists.txt` 里，紧邻既有 shm 控制面块，如 `:139` 的
`add_test(NAME test_shm_control_scheduler ...)` 之后）：

```cmake
# t77: W05 控制面 stale 门控回归（peer_count()==0 + 陈旧 in_use 槽位 ⇒ 必须回收且不误断活订阅者）。
# 依据队长裁定 §4.4 / R1-W05-F1；反向验证（消融回修复前 ⇒ 变红）见
# artifacts/perf/20261001-t77-W05-F1/negative/。用 RUN_SERIAL：本用例建真实 shm 段 + 等 2s 量级超时。
add_test(NAME test_w05_stale_slot_gate COMMAND test_w05_stale_slot_gate)
set_tests_properties(test_w05_stale_slot_gate PROPERTIES RUN_SERIAL TRUE TIMEOUT 120)
```

**为什么 `RUN_SERIAL`**：本用例会**真实建控制面段并等到 `peer_dead_timeout`(2s) 量级**
（拿共享 `/dev/shm` 与 notify 唤醒），与其它建段的用例并发跑会互相干扰（同 W04 对共享段类用例的结论）。

**验收（落地后请复核）**：
```bash
cmake -S . -B build && ctest --test-dir build -N | grep w05_stale_slot_gate     # 期望出现该测试
ctest --test-dir build -R test_w05_stale_slot_gate                              # 期望 1/1 Passed（≈4.1 s）
```
⛔ 不改 `test/CMakeLists.txt`（归架构负责人）。

**反向验证摘要**（证明它真进了被测代码路径）：把 `shm_control_scheduler.cc` 的兜底 `else if` 分支删掉后，
本用例变红并把两条判据都打出来（未回收 + 误断 37/100），见
`artifacts/perf/20261001-t77-W05-F1/negative/negative_run.log`。

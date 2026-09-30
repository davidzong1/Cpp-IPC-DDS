# 提交给架构负责人（`test/CMakeLists.txt` 归其写入，⛔ 本任务不自行改）

## 需求（F4 / R0-A1 硬闸 ③d）：把 W10 工装纳入 CMake，保证「库变即重编」
理由：`RecvWorkerPool::stats()` **按值返回** `RecvWorkerStats`，t30 在结构体**尾部**追加了 5 个
`uint64`。手工编译的工装与库不同步时会**越界写返回槽**（实测 t30：功能读数对、退出时
`__libc_free ← main` SIGSEGV；t31：同一二进制重跑任意 SHM 臂 **6/6 rc=139**；同源重编后 rc=0）。

## 期望的最小改动（二选一，推荐 A）
### A. 纳入 CMake（与库同一次构建 ⇒ 天然配对）
```cmake
# test/CMakeLists.txt 追加（放在既有 add_executable 之后；链接与既有测试同款）
set(W10_HARNESSES
    w10_matrix w10_idle w10_scancost w10_threadscan
    w10_faults w10_wakescan w10_mixwake w10_tickcost
    w10_portconflict w10_portconflict_scope w10_rebuild_crash
    w10_r3_counterexamples)
foreach(h ${W10_HARNESSES})
  add_executable(${h} perf/w10/${h}.cpp)
  target_link_libraries(${h} PRIVATE ipc pthread)
  set_target_properties(${h} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endforeach()
```
注：本仓库既有测试用的是 `add_executable(<t> test_xxx.cpp)` + `target_link_libraries(<t> PRIVATE ipc pthread)`
形态（见 `test/CMakeLists.txt` 既有块）；上面按同一形态书写，仅多一层 `foreach` 与 `perf/w10/` 前缀。
**是否注册为 `add_test` 由你决定** —— 建议 **不** 注册（工装是长跑规模验收，不进默认 ctest 集），
只保证"随库重建"。若你希望进 ctest，建议加 `RUN_SERIAL TRUE`（⛔ 不用 `RESOURCE_LOCK`）。
个别工装需要额外依赖：`w10_scancost.cpp` 需要 `dzIPC/measure/**` 头（已在 `include/` 内），
`w10_r3_counterexamples.cpp` **无库依赖**（纯对拍），可单独 `add_executable` 且不链接 ipc。

### B. 不纳入 CMake，但把「配对前置检查」做成硬闸
```cmake
add_custom_target(w10_harness_pairing_check ALL
  COMMAND ${CMAKE_COMMAND} -E echo "W10 工装必须随库重编：见 test/perf/w10/BUILD.md"
  DEPENDS ipc)
```
配合脚本：`test/perf/w10/build_harnesses.sh`（本任务已提供）在执行前比较
`libipc.so.1.3.0` 与各工装二进制的 mtime，库更新则**强制重编**；`w10_matrix` 另有
`--require-lib-sha256/--require-bin-sha256` 运行时配对断言，不配对即**非零退出且不出结论**。

## 已就位的替代机制（即使 A/B 都暂缓，F4 也不再是"裸奔"）
1. `test/perf/w10/build_harnesses.sh`：一条命令重编全部工装，并在开头做 mtime 配对检查；
2. `w10_matrix --require-lib-sha256 <hex> --require-bin-sha256 <hex>`：运行时断言（已实测
   指纹不符 ⇒ rc=1 且不产出 verdict）；
3. `w10_run_matrix.sh` 每个子运行自动带上述两个断言，并把配对结论写进 `fingerprint.txt`。

## 请回复
- 采用 A 还是 B；
- 若采用 A，`add_test` 是否注册、以及 `RUN_SERIAL` 的取值；
- 若两者都不采用，请给出替代的"库变即重编"机制要求，我在工装侧适配。

---

## 精确改动块（可直接粘贴到 `test/CMakeLists.txt` 末尾）

```cmake
# ---------------- W10 验收工装（t37 / F4 / R0-A1） ----------------
# 为什么必须进 CMake：`RecvWorkerPool::stats()` **按值返回** `RecvWorkerStats`，
# W06/t30 在结构体**尾部追加**了 5 个 uint64。C++ by-value 返回由**调用方**按自己看到的
# sizeof 分配返回槽 ⇒ 陈旧调用方（手工编译的工装）+ 新库 = **越界写**（实测 rc=139，
# __libc_free ← Harness::run ← main）。纳入 CMake 后「库变即重编」由构建图保证。
#
# ⛔ 不注册为 add_test（工装是长跑规模验收，不进默认 ctest 集）；若要注册，
#    请加 RUN_SERIAL TRUE（⛔ 不用 RESOURCE_LOCK）。
set(W10_HARNESSES
    w10_matrix w10_idle w10_scancost w10_threadscan w10_faults
    w10_wakescan w10_mixwake w10_tickcost w10_portconflict
    w10_portconflict_scope w10_rebuild_crash)
foreach(h ${W10_HARNESSES})
  add_executable(${h} perf/w10/${h}.cpp)
  target_link_libraries(${h} PRIVATE ipc pthread)
  set_target_properties(${h} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endforeach()
# 纯对拍件：无库依赖（不链接 ipc，避免随库 ABI 变化而失效）
add_executable(w10_r3_counterexamples perf/w10/w10_r3_counterexamples.cpp)
set_target_properties(w10_r3_counterexamples PROPERTIES
                      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
```

注：本仓库既有测试块用的是 `add_executable(<t> test_xxx.cpp)` + `target_link_libraries(<t> PRIVATE ipc pthread)`
形态（见 `test/CMakeLists.txt` 既有块）。上面按同一形态书写，仅多一层 `foreach` 与 `perf/w10/` 前缀。
若既有块用的是 `link_libraries(...)` 之类的**目录级**写法，请以既有块为准改写这两行。

## 已实测的替代机制（架构负责人可在 A/B 之外独立复核）

| 机制 | 命令 | 判据 |
|---|---|---|
| 重编 + mtime 配对检查 | `bash test/perf/w10/build_harnesses.sh [--check-only]` | `--check-only` 发现 `STALE` 即 rc=1 |
| 运行时指纹断言 | `./build/bin/w10_matrix ... --require-lib-sha256 <hex> --require-bin-sha256 <hex>` | 不符即 `FINGERPRINT_MISMATCH` + rc=1，**不产出 verdict** |
| 批跑内建断言 | `bash test/perf/w10/w10_run_matrix.sh <root> --run-id-prefix <p>` | 每个子运行自动带上面两个断言；配对结论写入 `fingerprint.txt` |

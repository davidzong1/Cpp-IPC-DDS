# T09 · 提交前闸门逐条判定（方案 §11.3 / §12）

- 任务 `t10 / T09`（队长 / 集成负责人），attempt_id `ba543ad0-f93d-43ae-8fb2-f75f66ba66e3`
- 冻结点 `f548cd71bf8b00cbf72fab0d8e0b72cdcf7ff34f`
- 主报告：`integration_report.md`（§3 为本文的同源展开）

## 闸门判定表

| # | 闸门项 | 判定 | 命令 / 证据 | 观察值 |
|---|---|---|---|---|
| 1 | 没有基线文件被 staged 回退 | ✅ 通过 | `git diff --cached --stat` | 输出为空 ⇒ 无 staged 内容，无回退可能 |
| 2 | W05 双臂承重用例已进 CTest | ✅ 通过 | `ctest --test-dir build -N` | `Total Tests: 31`；`Test #3: test_w05_stale_slot_gate`、`Test #4: test_w05_stale_slot_gate_arm`；`RUN_SERIAL TRUE TIMEOUT 120`；全 31 项 `RETRY`/`REPEAT` 命中 **0** |
| 3 | W05 反向消融成立 | ✅ 通过 | `ablation/ablation_matrix.csv` + T04 独立消融树 | ①删公共兜底⇒**两臂同时失败**；②只删默认驱动⇒默认失败/L1 通过；③只删 L1 驱动⇒默认通过/L1 失败；T04 另建两树，判据① 3/3 稳定变红 |
| 4 | W09 FIX/NEG 同一测试二进制换库验证成立 | ✅ 通过 | `20261001-w12-T05/summary_matrix.tsv` + `T06_report.md` | 同二进制 `08b803b1…`；FIX `0/0/0`、FIX_RB `0/0/0`、V2NEG `1/1/1`、FIXNEG `1/1/1`、BASELINE `0/0/0`、ABLATE `0/0/0`；T06 verdict = **pass** |
| 5 | W11 round-4 PASS 已同步 | ✅ 通过 | `T08_W11_round4状态同步.md` + W12 三份正文 | 27 张表 **27/27 同批**、混批 **0**；§3.4 **54 格 54/54** 命中新批；`W12/当前问题总清单.md:21/:30` |
| 6 | 所有性能读数绑定采集时 `fingerprint.txt` | ✅ 通过 | `gate_fingerprint.txt` | CPU/扫描批 `cf209393…`、旧批 `cf209393…`、新批 `f0ebc3ef…`（**不取**裸 `manifest.binary_sha256=0298b1df…`）；`library_binding_authority` 在位 |
| 7 | 所有新增文件均在 `inScope` 白名单内 | ✅ 通过 | `gate_scope.txt` | 未跟踪新增 = 7 个 `20261001-w12-*` run 目录 + 方案文档；已跟踪改动 = 10 文件（T03 5 / T07 4 / T11 1），全在各自 inScope；`git status --porcelain -- src/dzIPC src/libipc` = **0** |
| 8 | `git diff --cached --check` 无输出 | ✅ 通过 | `git diff --cached --check` | **exit 0、无输出** |
| 9 | 关键文件 SHA 与预期一致 | ✅ 通过 | `file_sha_compare.txt` | 3/5 逐字节 MATCH；`shm_control_scheduler.h` 为 T03 **纯注释**契约修订（非注释变更行 **0**）；`ipc.cpp` MATCH |
| 10 | W12 S4 清单已实际收口 | ✅ 通过 | `t8` evidence_note + T07 交付 | 错误语义 grep **exit=1**；`bdad6093` 全仓 **0 命中**；`t75_` 在 inScope 内 **0 命中**；T07 实改 4 文件 516+/155− |

## 附加核对（队长裁定 R-5：T03 无独立 review 覆盖的补足）

| 核对项 | 命令 | 观察值 |
|---|---|---|
| CTest 注册增量恰为 2 | `ctest -N`（对照 T01 基线 29） | 31 − 29 = **2** ✅ |
| 无重试机制 | `ctest -N \| grep -ci retry\|repeat` | **0** ✅ |
| CMakeLists 纯追加 | `git diff -- test/CMakeLists.txt \| grep -c "^-[^-]"` | **0** ✅ |
| 头文件仅注释 | `git diff -U0` 剔除注释/空行后计数 | **0** ✅ |
| 防批量回退 | `git status --porcelain -- src/dzIPC src/libipc` | **0** ✅ |
| t84 修复判据在位 | `grep -c pub_control_tick`（.h/.cc/shm_pub_sub_ipc.cc） | 2 / 4 / 7 = **13** ✅ |
| 旧门控未复活 | `grep -n "has_peers()"` | 仅判据内部 `:208` 调用 1 处 ✅ |
| 重编后库指纹 = 冻结指纹 | `sha256sum build/lib/libipc.so.1.3.0` | `813fab5be886ef6e…` ✅ |

## 已知限制（必须随闸门一起引用）

1. **非零偶发**：`test_w05_stale_slot_gate{,_arm}` 的 L1 臂实测 ~1.25–1.50%（T02 158 次 1 次；T04 独立复现 3/200），**失败点恒在判据②**，判据① 0 次失败；机制未定位。⇒ ⛔ 不得声称 ctest 恒全绿；⛔ 未使用 RETRY/`--repeat until-pass`。
2. **两项不可复算**：W11 round-4 的 `ctest 29/29` 无独立复算；**同库复跑不可得**（负控 `FINGERPRINT_MISMATCH actual=813fab5b…`、exit=1）⇒ ⛔ 不得声称「已同库复跑验证」。
3. **量具假红（low）**：`test_chunk_capacity_backpressure` 的 `clear_storage` 不含 chunk 池段 ⇒ 连续运行对**无 reclaim** 库产生假红。处置：运行前 `rm -f /dev/shm/w09c9alias__IPC_SHM__*` 并显式声明（本轮已登记 `cleanup_log.txt` / `cleanup_focused.txt`）。登记于 `W09/容量与背压_交付.md` §12（:478 起）。
4. **证据可持续性**：库绑定 `0186853e…` 位于未跟踪的 `.gitignore:97 tmp/**` 下，一旦清理即不可复核。
5. **不可声明项**：跨进程千路未验证；千订阅工程可用不成立；G2/G4 未关闭；§13.2 未闭合项按现有报告保留。

# t83 · 反向验证读数（「判据的判据」）—— C9 常驻用例是否有牙

**判据**：`ChunkCapacityBackpressure.OrphanResetDoesNotAliasInflightLoansAcrossTopics`
（`test/test_chunk_capacity_backpressure.cpp`，t83 落盘；**1200 轮 / 专属前缀 `w09c9alias` / fork-per-round + 每轮种子进程**）

**方法**：**同一二进制**（`build/bin/test_chunk_capacity_backpressure`，sha256[0:16] 见下），
**唯一变量 = `libipc.so.3`**。脚本 `t83_reverse_validation.sh`（产品源码改动为**瞬态**，`trap EXIT` 保证还原并重编）。

## 一、库指纹

| 臂 | 库 sha256[0:16] | 含义 |
|---|---|---|
| `FIX` (= 当前 `build/lib`) | `36c17c7ba941e44b` | 含 T73 修复：素净判定 + 快照在 `handles_` 的 `lock_` **之内** |
| `NEG` | `9365611342152d93` | 把该快照**移出** `lock_`（= T73 之前行为）⇒ 唯一差异就是这一处修饰 |

归档副本：`lib_fixed_t83/`、`lib_negative_t83/`（可**离线**复跑，⛔ 不需再改源码）。

## 二、读数（同二进制，只换库）

| 臂 | run1 | run2 | run3 | 结论 |
|---|---|---|---|---|
| `FIX` | `[  OK  ]` `bad_rounds=0` | `[  OK  ]` `bad_rounds=0` | `[  OK  ]` `bad_rounds=0` | **3/3 PASSED** ✅ |
| `NEG` | `[ FAILED ]` `bad_rounds=1` | `[ FAILED ]` `bad_rounds=1` | `[ FAILED ]` `bad_rounds=1` | **3/3 FAILED** ❌ |

（`NEG` 的失败行逐字：`[  FAILED  ] ChunkCapacityBackpressure.OrphanResetDoesNotAliasInflightLoansAcrossTopics`，
`W09-C9] 专属前缀 w09c9alias 的 8 话题并发首借 ×1200 个独立进程：bad_rounds=1 skipped=0`）

## 三、离线复跑（用归档库，⛔ 完全不碰源码）

```
LD_LIBRARY_PATH=$PWD/artifacts/perf/20261001-t73-W09-F1/lib_fixed_t83    build/bin/test_chunk_capacity_backpressure --gtest_filter='*OrphanReset*'  ⇒ bad_rounds=0
LD_LIBRARY_PATH=$PWD/artifacts/perf/20261001-t73-W09-F1/lib_negative_t83 build/bin/test_chunk_capacity_backpressure --gtest_filter='*OrphanReset*'  ⇒ bad_rounds=1
```

## 四、还原确认

```
RESTORED ipc.cpp sha256=9f936b25b5eb76d7 (期望 9f936b25b5eb76d7) OK
（含 'NEGATIVE CONTROL' 串计数 = 0 ⇒ 无遗留补丁）
```

## 五、判读

⇒ 该用例**有牙**：回退被检验的那一处修饰，用例**必然变红**（3/3），且 `bad_rounds=1` 即
「两个不同话题拿到同一块 chunk（同 id / 同 data 指针）」——正是 F1 的失效形态。⛔ 不是「写了就绿」。

⚠️ 另注（t83 复现的历史现象）：**负控变红具有概率性**（每轮窗口仅几微秒），故轮数取 **1200**
（600 轮时曾观测到漏判；3000 轮 12.7 s 收益不匹配）。当前配置下负控 **3/3 命中**。

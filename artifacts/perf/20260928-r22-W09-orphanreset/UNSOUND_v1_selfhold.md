# 记录：第一版「池穷尽路径段级复位」不成立（被自己的对照实验证伪）

## 我的第一版做法
在 `acquire_storage` 的**池穷尽分支**里：若 `scan_segment_mappers(本段) == orphaned`（除本进程外无人映射）
⇒ `pool_.reset_free_chain()` 复位空闲链，然后重试 acquire。

## 为什么它不成立
`scan_segment_mappers` **只排除本进程**，于是它只能证明"别的进程没映射"，
**证明不了"本进程此刻没有在飞借样"**。当池是被**本进程自己**借空时（完全正当的场景），
判据仍然成立 ⇒ 复位把**自己正持有的 id 重新发出去** ⇒ 双重分配/踩踏。

## 被证伪的现场证据（本目录）
- `solo_chunk_capacity.log`：`test_chunk_capacity_backpressure` 5/5 FAILED，
  日志里能直接看到 `chunk pool orphan segment reset: ... prefix = 'w09_c1_0'`
  —— 复位发生在"本进程借空 40 块"的正当场景上；
  > ⚠️ **更正（t73，按实物）**：上面这句「5/5 FAILED」与原始日志不符。实物
  > `artifacts/perf/20260928-r22-W09-orphanreset/solo_chunk_capacity.log` 是
  > **`[  PASSED  ] 5 tests` + `[  FAILED  ] 2 tests`** ⇒ 准确表述 = **7 条中 2 败**：
  > `ChunkCapacityBackpressure.PerClassBlockCountIsLargeMsgCacheAndExhaustionIsRefusalNotBlock`
  > 与 `ChunkCapacityBackpressure.SamePayloadLandsInDifferentSizeClassesForLoanAndTlvSend`
  > （复算命令：`grep -oE '^\[  FAILED  \] +[A-Za-z][A-Za-z0-9_]*\.[A-Za-z0-9_]+' <log> | sort -u`）。
  > 原句按「保留 + 紧随更正行」体例保留（同 D-20）。**结论不变**：v1 判据仍被证伪，那 2 条
  > 失败的正是"本进程借空 40 块构造耗尽"的用例。
- `solo_dzflat_transport.log`：`DzFlatTransport.ChunkPoolExhaustionFallsBackToTlv` FAILED
  （该用例正是"本进程借空 40 块构造耗尽场景"）。
⇒ 这两条失败不是"测试不适配"，而是**判据本身不安全**（自持借样被误判为孤儿）。

## 修正方向（见 ipc.cpp 的最终实现）
把复位从"池穷尽路径"挪到**首次 attach 该 (prefix, chunk_size) 段的那一刻**：
- 那一刻**本进程尚未持有本档任何借样**（映射刚建立）⇒ 自持借样陷阱不存在；
- 其它进程若不映射该段则不可能持有（持块必须先 mmap）⇒ 判据充分。
于是"复位"的完整前提 = 「本进程此刻无持有」+「别进程未映射」，两者都成立才复位。

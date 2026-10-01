# 20261001-t77-W05-F1 —— W05 控制面 stale 门控：实体修复 + 反向验证

> 任务 `t77`（repair-round-3，socket与数据面负责人）；依据 = **队长裁定 §4.4**
> （`W12/任务失败与事故报告.md:139-143`，12:11:44）+ R1-W05-F1（high）/ N1（blocker）。
> **性质**：本目录是**新 run**，⛔ 不覆盖任何既有 run；既有 run `20260928-r23-W05/` 只被
> **追加**了一项校正记录（`source-status.txt` 尾部 4 行注释）+ 把被 t75 就地改写的
> `.W05` **复原**为交付时点原值（见下）。

## 1. 修的是什么（一句话）

`shm_control_scheduler.cc::dispatch()` 里 stale 扫描的门控
`if (has_peers) { on_pub_stale_scan(...); }` —— `peer_count()==0` 时**整条跳过**扫描。
其理由「无 peer 时扫描结果必然是 0」**为假**：`collect_stale_peers()` 判死的是 **PeerSlot**，
而清理顺序 `remove_peer()` → `release_peer_slot()` **两步**之间有窗口 ⇒ 会留下
`peer_count==0 + slot.in_use==1 + 心跳陈旧`，该状态下的槽位/`cc_id` 位**永不回收**，
且回收被推迟到"活订阅者已挂上"之后 ⇒ 旧 `cc_id` 位已被复用 ⇒ `disconnect_receivers()` **永久误断**。

**修法**（队长裁定第一条路径 = W04 接口修订）：
- 接口层：`shm_control_scheduler.h` 的 `on_pub_stale_scan` 契约修订（写明原前提为假 + 新节奏），
  同步 `接口责任表.md` 行 + 新增 §2.0 修订记录；
- 实现层：`Entry::next_stale_due` + `dispatch()` 的**低频兜底**分支
  （`has_peers()` 真 ⇒ `pub_heartbeat` 50ms 每拍；假 ⇒ `peer_dead_timeout` 2s 同量级兜底）。

⛔ **不是降频优化**：扫 64 槽是常数开销（µs 级），**不得**据此声称任何 CPU/吞吐收益。

## 2. 端到端：原最小反例由 0/400 → **400/400**

```
W05 生效（w05only 变体树，库加修复后 = 8dfde8b9…），craft=1，hold=4500ms：
crafted peer_count=0 slot=0 in_use=1 cc_id=1
reaped dead subscriber connection(s), cc_ids = 0x1
after_hold(4500ms) peer_count=0 slot_in_use=0 cc=0      ← ★ 陈旧槽位已回收
live_slots: [0]=cc1
published=400 received=400 mismatch=0                   ← ★ 活订阅者未被误断（修复前 0/400）
```
原始输出：`e2e_fixed_harm.log`（探针 = `build/t58/bin/t58_harm_w05only`，t77 只重编其库）。

## 3. 常驻用例 + **反向验证（判据的判据）**

| 用例 | 修复后 | **消融回修复前**（删掉 else 分支） |
|---|---|---|
| `test_w05_stale_slot_gate`（新增，端到端） | **PASSED ×3**（≈4.1 s/次） | **FAILED**（① 未回收；② 误断 **37/100**） |
| `test_shm_control_scheduler.PublisherHeartbeatWithoutPeers`（断言同步更正） | **PASSED**（2.1 s） | **FAILED**（「stale 扫描被整条跳过」） |

- 负控原始日志：`negative/negative_run.log`（端到端用例的完整失败输出）
- 负控 diff（**唯一差异 = 删掉 `else if` 分支**）：`negative/negative_gate_single_line.diff`
- 负控小结：`negative/README.md`
⇒ 两条用例**都有牙**：修复前红、修复后绿。

## 4. 复算件（`recompute/`）

| 文件 | 用途 |
|---|---|
| `base.cc` | 基线 `e800ccc…:src/dzIPC/shm_pub_sub_ipc.cc` |
| `orig_W05.cc` | 交付时点归档快照原值（= `HEAD:…cc.W05`，sha `256bab86…`） |
| `t75_edited.cc` | 复原的 **t75 就地注释版**（供 §1 的「两种口径」复算） |

复算命令（⛔ 不依赖任何未入库文件）：
```bash
git diff --no-index --numstat recompute/base.cc recompute/orig_W05.cc   # ⇒ 654 192
git diff --no-index --numstat recompute/base.cc recompute/t75_edited.cc # ⇒ 666 192（见交付 §1 注）
```

## 5. 归档快照的复原（N3）

t75 曾**就地**改写 `artifacts/perf/20260928-r23-W05/shm_pub_sub_ipc.cc.W05`（`256bab86…`→`46f60f7d…`）。
t77 已**复原为原值**（来源：`build/t58/w05only/` 复算树里的**未编辑**副本，
`git diff --no-index` 与 `HEAD` 版本**零差异**）⇒ 同目录 `binary-fingerprint.txt` /
`manifest.json` / `command.txt` 三处自述重新与实物一致。
`46f60f7d…` 那一版**全仓已无副本**（如实登记）；其内容可用 §10.5 同款注释更正复算。

## 6. 已知残余

1. `test_w05_stale_slot_gate` **尚未进 ctest**（`add_test` 归架构负责人）⇒ 见交付 §7 的 R-7。
2. `src/dzIPC/shm_pub_sub_ipc.cc` 的注释措辞更正仍在**建议补丁**形态（该文件不在 t77 写入面）；
   `include/dzIPC/shm_pub_sub_ipc.h` 已在 t77 **直接更正**。
3. `src/dzIPC/threepools/shm_control_scheduler.h` 的 `:118-124` 契约文本已随修订改写 ——
   R1/R2 报告里引用的**旧行号**会漂移，按内容检索。

# T06 量具卫生事实：C9 用例的跨臂段污染（本 run 实测，⛔ 非产品缺陷）

## 现象
同一测试二进制、**同一库** `BASELINE`（sha256 `4fc0e95d…`，无 reclaim）在两个上下文给出相反结论：

| 上下文 | rc | gtest | bad_rounds |
|---|---|---|---|
| 紧接 `V2NEG`（负控）之后跑 | 1 | FAILED | **1** |
| 干净 `/dev/shm`（无本用例残留段）上跑 | 0 | PASSED | 0 |
| 干净态跑完 `ABLATE` 之后再跑 | 0 | PASSED | 0 |

本 run 的**最小复现三连**（同一二进制、同一库，窗口见各自 `fingerprint.txt`）：

```
official-arm/POLDEMO-V2NEG              rc=1  bad_rounds=1     (负控臂，制造重复 id)
official-arm/POLDEMO-BASELINE-polluted  rc=1  bad_rounds=1     (不清段 ⇒ 继承重复 id)
official-arm/POLDEMO-BASELINE-clean     rc=0  bad_rounds=0     (清本用例专属前缀池段)
```

## 机械根因（两处源码锚点）
1. 用例开头只清**控制面**：
   `ipc::route::clear_storage(prefix, "w09c9_seed")` → `ipc.cpp:210-222` 只 unlink
   `CC_CONN__ / WT_CONN__ / RD_CONN__ / AC_CONN__` —— **不含** chunk 池段。
2. chunk 池段名 = `make_prefix(pref, {"CHUNK_INFO__", chunk_size, "__C", capacity})`
   （`ipc.cpp:373-378`）⇒ `/dev/shm/w09c9alias__IPC_SHM__CHUNK_INFO__9216__C40` **不被清理**。

## 量化（`probe/seg_probe chain`，只 fopen 段文件）
负控臂跑完后残留段空闲链已损坏/重复可达（`ipc.cpp:102-106 reset_free_chain` 不变量被破坏）：

```
PROBE_CHAIN next=[2,2,0,1,5,3,7,8,...]            cycle_closed=1 reachable_ids=6
PROBE_CHAIN next=[4,0,1,2,3,6,7,5,9,...]          cycle_closed=1 reachable_ids=5
PROBE_CHAIN next=[1,2,3,9,8,6,4,11,3,7,...]       cycle_closed=0 reachable_ids=37   (BASELINE 之后)
```

无 reclaim 的库**永远修不回**该状态（`reset_free_chain` 不存在），而 C9 的判据要求
"两个不同话题不得拿到相同 id" ⇒ 继承来的重复 id 立刻表现为 `bad_rounds=1`。

## 本 run 的处置（⛔ 不改用例/产品/工装）
- `scripts/run_official_matrix.sh` 在**每臂之前**只删本用例专属前缀段：
  `rm -f /dev/shm/w09c9alias__IPC_SHM__*`，并逐臂记录清空前的段状态（`matrix.driver.log` 的 `[pre]` 行）。
- **臂内 1200 轮一次都不清**（用例本体行为未改）：清的是**跨臂的量具卫生**，不是"为了取绿"。
- 设 `BASELINE-polluted` 臂（紧接 V2NEG、**不清段**）把该事实钉死：同库 `bad_rounds=1`，
  而 `BASELINE-1`（清段）为 `bad_rounds=0`。
- 产品臂 `FIX` 在**污染态与干净态下都是绿**（干净态见
  `official-matrix/FIX-{1,2,3}/`；早期卫生修正前的目录见 `official-arm/README_superseded.md`）⇒ 污染不是 FIX 变绿的原因。

## 对结论的影响
无。四类场景判定全部用**清段后**的 arm（`official-matrix/`）与自建探针（`scenarios/`，
每场景自带段生命周期、`clear_storage` 只在 race 开跑前一次），并额外给出污染对照臂。
⛔ 该缺陷属**用例/量具**，属 T05 报告未登记项（本 run 新增登记，low）。

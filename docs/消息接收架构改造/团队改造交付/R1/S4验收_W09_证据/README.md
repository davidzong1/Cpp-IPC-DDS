# t65 · W09 S4 独立验收 —— 证据清单（只读复核；⛔ 未改产品代码/W09 交付物/既有 run）

复核者：验证与性能负责人（未参与 W09/t10、t22、t19 实现）；时间：2026-10-01

## 一、判决性实验（leaker + SIGKILL ⇒ 残留段 ⇒ 单跑 0% ⇒ 清理后 100%）
- 脚本：`test/perf/w09/w09_decisive_repro.sh`（⛔ 必须在**单次 bash 调用**内跑完：/dev/shm 不跨调用保留）
- 输出：`artifacts/perf/20260930-t65-W09-S4/repro/{run.log,results.tsv,*.log}`
- 三库：FIX=当前库 `f0ebc3ef...`；ABL=**消融库**（与 FIX 源码只差 1 个调用点，`c02b3c85...`）；BASE=基线库（源= `ipc.cpp.HEAD` 逐字，`314f20cf...`）
- 独立构建脚本：见主报告 §11.2（shadow tree + 单点消融）

## 二、容量模型逐条（运行期）
- `cap_probe`：`large_msg_cache=40`、`id_pool<>::max_count=40`
- `vq_probe`：`ViewQueueCap()=10`、`IsViewQueuePinEnabled()=1`
- `test_chunk_capacity_backpressure` 运行期打印：`[W09-C1] 档 9216 可用块数=40`、`[W09-C4] 广播 1 条/4 接收方：池占用 1 块`、`[W09-C8] 借满=0 → 归还后空闲=40`
- 段名实测：空前缀 `__IPC_SHM__CHUNK_INFO__132096__C40`；命名前缀 `w09_c2_0__IPC_SHM__CHUNK_INFO__9216__C40`

## 三、反例复现件（最小可执行）
- `repro_alias.cpp`（85 行）：**受支持用法**下同档 chunk 被重复分配（同 id / 同 data 指针）；
  已编译二进制在 `artifacts/perf/20260930-t65-W09-S4/repro_alias_{fixed,undersame}`
- 命中率（每库 120 次，3 轮 × 8 话题，每次清空空前缀 9216 段）：**FIX 4/120、UNDERSAME 0/120**
- UNDERSAME = 影子变体（把 reclaim 挪进同一把 `lock_` 内）⇒ 0 命中 ⇒ **窗口定位到 `handles_` 解锁与取池快照之间**

## 四、影子库清单（⛔ 均在 tmp/ 内，未进仓库源）
   tmp/t65/lib_ablate/libipc.so.3 1285960B
   tmp/t65/lib_atomic/libipc.so.3 1291256B
   tmp/t65/lib_baseline/libipc.so.3 1281408B
   tmp/t65/lib_diag/libipc.so.3 1291256B
   tmp/t65/lib_fixed/libipc.so.3 1291168B
   tmp/t65/lib_nopristine/libipc.so.3 1291256B
   tmp/t65/lib_norecheck/libipc.so.3 1291256B
   tmp/t65/lib_sleep/libipc.so.3 1291304B
   tmp/t65/lib_undersame/libipc.so.3 1291256B

## 五、影子库（⛔ 全部在 tmp/t65/**，未进仓库源；构建配方见 artifacts/perf/.../shadow_build.md）
  tmp/t65/lib_ablate/libipc.so.3               1285960 B
  tmp/t65/lib_atomic/libipc.so.3               1291256 B
  tmp/t65/lib_baseline/libipc.so.3             1281408 B
  tmp/t65/lib_diag/libipc.so.3                 1291256 B
  tmp/t65/lib_fixed/libipc.so.3                1291168 B
  tmp/t65/lib_nopristine/libipc.so.3           1291256 B
  tmp/t65/lib_norecheck/libipc.so.3            1291256 B
  tmp/t65/lib_sleep/libipc.so.3                1291304 B
  tmp/t65/lib_undersame/libipc.so.3            1291256 B

## 六、三个必读读数
1. `pristine=0` 在 race_diag 40 轮中出现 **7 轮** ⇒ 复位分支是**常规路径**而非罕见分支
2. 反例命中率：FIX **4/120**、UNDERSAME（reclaim 移入同一 lock_）**0/120**
3. 判决性实验三库 11 臂全部复现（见 `artifacts/perf/20260930-t65-W09-S4/repro/results.tsv`）

# t73 · W09 S4 复核修复（F1）证据目录
# 时点: 2026-10-01；被复核对象: W09（t10/t22）

ipc.cpp.T73FIX                  = 修复版 ipc.cpp（sha256 9f936b25b5eb76d7207f982a4809063adf99fa1c4ef27b9e317260c868d5ebf4）
t73_reclaim_lock_order.patch    = 相对 HEAD 的补丁（+57 行）
fingerprints.txt                = 改动前后指纹
decisive_focused.log            = 判决性实验聚焦臂（三库对齐重编）
decisive_repro*/                = w09_decisive_repro.sh 复跑

关键读数:
  · 复核反例（8 话题并发首借）: 修前 4/120 命中 → 修后 0/120（×3 轮）
  · 常驻用例（fork-per-round ×120）: 修后 5/5 运行 bad_rounds=0；负控 6/6 运行报 FAILED
  · 判决性实验: 注入残留段后 ABL rc=1/1 FAILED+exhausted，FIX rc=0/9 OK+orphan reset；leaker 活着时 FIX 仍 FAIL（判据承重）

---

## t83（范围外落地）新增

| 文件 | 说明 |
|---|---|
| `t83_reverse_validation.sh` | **反向验证脚本**（「判据的判据」）：瞬态回退 T73 修复 → 断言 C9 变红；`trap EXIT` 保证还原并重编 |
| `t83_reverse_readings.md` | 反向验证**前后读数**（同二进制 / 只换库）：FIX 3/3 `bad_rounds=0` PASSED vs NEG 3/3 `bad_rounds=1` FAILED |
| `t83_archive_check.md` | 归档核对：`artifacts/perf/20260928-r23-W05/shm_pub_sub_ipc.cc.W05` = **`256bab86…`**（未被就地改写） |
| `lib_fixed_t83/` | 修复库副本（`36c17c7b…`），供**离线**复跑（⛔ 不再改源码） |
| `lib_negative_t83/` | 负控库副本（`93656113…`），同上 |

落地结论：C9 已落进 `test/test_chunk_capacity_backpressure.cpp`（8 条用例）；`容量与背压_交付.md` §7.5 已落地并按 t74 的 W09-R2-F3 实测值更正（1200 轮 / 7.7 s / 专属前缀 `w09c9alias`）；§9.1.1 登记「库绑定权威只认 `fingerprint.txt`」纪律。⛔ 未改 `test/CMakeLists.txt`、未改产品代码。

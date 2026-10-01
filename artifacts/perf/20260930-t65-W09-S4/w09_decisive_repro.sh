#!/bin/bash
# t65 · W09 判决性实验独立复现（⛔ 必须在**单次 bash 调用**内跑完：/dev/shm 不跨调用保留）
#
# 设计要点（第一版脚本的缺陷已修正）：**每个库在单跑前各自重新注入一次泄漏**。
# 原因（第一版实测发现）：修复库单跑成功后，残留段已被它自己的 `reclaim_orphan_segment`
# 复位成"干净可用"状态 ⇒ 后续基线库看到的是**干净段** ⇒ 假 PASS（顺序污染）。
#
# 三个库，唯一变量 = 库
#   FIX  = 当前树 build/lib/libipc.so.3（含 v2 修复）
#   ABL  = 消融库：与 FIX 的源码**只差一个调用点**（不调用 reclaim_orphan_segment）
#   BASE = 基线库：源 = ipc.cpp.HEAD / id_pool.h.HEAD 逐字快照，独立重编
#
# 臂
#   A 基线（无泄漏）                        × {FIX, ABL, BASE} ⇒ 期望全 PASS
#   对每个库 L：B 注入 leaker 40 unpublished + SIGKILL ⇒ 段残留
#               C 有残留段时单跑 L          ⇒ 期望 ABL/BASE FAIL、FIX PASS
#               E 只删该残留段后单跑 L      ⇒ 期望 PASS
#   F 对照：leaker **活着** → 修复库**不得**复位 ⇒ 期望 FAIL（判据承重）
#
# 用法: w09_decisive_repro.sh <out_dir>
set -u
OUT=${1:?usage: w09_decisive_repro.sh <out_dir>}
cd /home/zwc/cpp_ipc_dds || exit 1
mkdir -p "$OUT"
ABL=$PWD/tmp/t65/lib_ablate
FIX=$PWD/tmp/t65/lib_fixed
BASE=$PWD/tmp/t65/lib_baseline
LEAKER=$PWD/tmp/t65/exp/leaker
SEG=/dev/shm/__IPC_SHM__CHUNK_INFO__132096__C40
BIN=build/bin/test_dzflat_builder

say() { printf '%s\n' "$*" | tee -a "$OUT/run.log"; }

run_solo() {  # run_solo <libdir> <tag>  —— 直接跑二进制（保留 stderr，不靠 ctest 摘要）
  local lib=$1 tag=$2
  local out="$OUT/$tag.log"
  LD_LIBRARY_PATH=$lib timeout 120 "$BIN" --gtest_color=no > "$out" 2>&1
  local rc=$?
  local passed failed ex orr
  passed=$(grep -c '^\[       OK \]' "$out" 2>/dev/null)
  # ⛔ 只数**去重后的用例名**：gtest 会打印三次（RUN 行 / "listed below:" 行 / 末尾汇总），
  #    且末尾那个还带 "(1105 ms)"。故取 `[  FAILED  ] <Suite>.<Case>` 的**唯一名字**再计数。
  failed=$(grep -oE '^\[  FAILED  \] +[A-Za-z][A-Za-z0-9_]*\.[A-Za-z0-9_]+' "$out" 2>/dev/null \
           | sed 's/^\[  FAILED  \] *//' | sort -u | wc -l)
  ex=$(grep -c 'chunk pool exhausted' "$out" 2>/dev/null)
  orr=$(grep -c 'orphan segment reset' "$out" 2>/dev/null)
  local names; names=$(grep -oE '^\[  FAILED  \] +[A-Za-z][A-Za-z0-9_]*\.[A-Za-z0-9_]+' "$out" 2>/dev/null | sed 's/^\[  FAILED  \] *//' | sort -u | tr '\n' ',')
  printf '%-40s lib=%-13s rc=%-3s passed=%s failed=%s exhausted=%s orphan_reset=%s  %s\n' \
      "$tag" "$(basename "$lib")" "$rc" "$passed" "$failed" "$ex" "$orr" "${names%%,}" | tee -a "$OUT/run.log"
  echo "$tag|$rc|$passed|$failed|$ex|$orr|$(basename "$lib")" >> "$OUT/results.tsv"
}

inject_leak() {  # inject_leak <tag> —— 借 40 块不发布 + SIGKILL；打印段残留字节数
  local tag=$1
  rm -f "$SEG" 2>/dev/null
  "$LEAKER" 40 > "$OUT/$tag.leaker.out" 2>&1 &
  local lp=$!
  for i in $(seq 1 100); do grep -q 'loaned=' "$OUT/$tag.leaker.out" 2>/dev/null && break; sleep 0.2; done
  local n; n=$(grep -oE 'loaned=[0-9]+' "$OUT/$tag.leaker.out" | head -1)
  local alive; alive=$(cat /proc/$lp/comm 2>/dev/null)
  kill -9 $lp 2>/dev/null; wait $lp 2>/dev/null
  local sz; sz=$(stat -c%s "$SEG" 2>/dev/null || echo 0)
  printf '  [注入 %-14s] %s  进程comm=%-8s 段残留=%s B\n' "$tag" "$n" "${alive:-<已退>}" "$sz" | tee -a "$OUT/run.log"
  echo "$sz"
}

: > "$OUT/run.log"; : > "$OUT/results.tsv"
say "=== t65 W09 判决性实验独立复现（$(date '+%F %T')）==="
for L in ABL FIX BASE; do eval "d=\$$L"
  say "$L = $(basename $d)  sha256=$(sha256sum $d/libipc.so.3 | cut -d' ' -f1 | cut -c1-32)  orphan_str=$(strings $d/libipc.so.3 | grep -c 'orphan segment reset')"
done
say "ABL↔FIX 源码差异行数（应只有 1 处调用点）= $(diff tmp/t65/ablate/src/libipc/ipc.cpp src/libipc/ipc.cpp | grep -c '^[<>]')"
diff tmp/t65/ablate/src/libipc/ipc.cpp src/libipc/ipc.cpp | sed 's/^/    /' >> "$OUT/ablate_diff.txt"
say "初始 /dev/shm 段数: $(ls /dev/shm 2>/dev/null | wc -l)"
rm -f "$SEG" 2>/dev/null

say ""
say "── A. 基线（无泄漏）──"
run_solo "$FIX"  "A1_baseline_fixed"
run_solo "$ABL"  "A2_baseline_ablate"
run_solo "$BASE" "A3_baseline_HEAD"

for L in ABL BASE FIX; do
  eval "d=\$$L"
  say ""
  say "── $L：每库独立注入一次泄漏 ──"
  inject_leak "inj_$L" > /dev/null
  say "  注入后段存在性：$(ls -la "$SEG" 2>/dev/null | awk '{print $5" B"}' || echo '不存在')  /dev/shm 段数=$(ls /dev/shm 2>/dev/null | wc -l)"
  run_solo "$d" "C_after_leak_$L"
  say "  ── 只删该残留段 ──"
  rm -f "$SEG"
  say "  删除后：$(ls "$SEG" 2>/dev/null || echo '不存在')"
  run_solo "$d" "E_after_clean_$L"
done

say ""
say "── F. 对照：leaker **活着**（持段未死）→ 修复库**不得**复位 ──"
rm -f "$SEG" 2>/dev/null
"$LEAKER" 40 > "$OUT/F1_leaker_alive.out" 2>&1 &
LP2=$!
for i in $(seq 1 100); do grep -q 'loaned=' "$OUT/F1_leaker_alive.out" 2>/dev/null && break; sleep 0.2; done
say "  $(cat "$OUT/F1_leaker_alive.out")  仍活着 pid=$LP2 comm=$(cat /proc/$LP2/comm 2>/dev/null)  段存在=$([ -e "$SEG" ] && echo yes || echo no)"
run_solo "$FIX" "F2_solo_leaker_alive_fixed"     # 期望 FAIL：判据承重，不是"看到池空就重置"
kill -9 $LP2 2>/dev/null; wait $LP2 2>/dev/null
rm -f "$SEG" 2>/dev/null

say ""
say "=== 汇总 ==="
column -t -s'|' "$OUT/results.tsv" | tee -a "$OUT/run.log"
say "最终 /dev/shm 段数: $(ls /dev/shm 2>/dev/null | wc -l)"

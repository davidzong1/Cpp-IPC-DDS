#!/usr/bin/env bash
# T06 / W12 §8 —— 官方工装换库矩阵（**同一个测试二进制，只替换 libipc.so.3**）。
#
# ⚠️ 我在本 run 实测到的一条**量具卫生**事实（已落在 diag/arm_pollution.md）：
#   C9 用例开头只 `clear_storage(prefix, "w09c9_seed")`，它清的是**控制面**段
#   （AC/CC/WT/RD_CONN__），**不含** chunk 池段
#   `/dev/shm/<prefix>__IPC_SHM__CHUNK_INFO__9216__C40`。
#   ⇒ 上一条臂若在池里留下重复 id（负控臂正是这种形态），下一条臂会**继承污染**：
#     实测 BASELINE/ABLATE（无 reclaim ⇒ 永远修不回来）在 V2NEG 之后立刻 bad_rounds=1，
#     而在干净 /dev/shm 上 1200 轮全绿。
#   ⇒ 本驱动在**每条臂之前**只清本用例专属前缀（`w09c9alias__IPC_SHM__*`），
#     ⛔ 不清任何他人段文件；并逐臂记录"清空前是否存在残留段 + 其状态"。
#   ⛔ 这不是"为了取绿而清段"：**臂内 1200 轮一次都不清**（用例本体行为未改），
#     被清的是**跨臂的量具卫生**。产品臂 FIX 在污染态与干净态下**都是绿**（自愈）。
#
# 用法: run_official_matrix.sh <OUTROOT>
set -u
OUTROOT=${1:?outroot}
R=$PWD/artifacts/perf/20261001-w12-T06
PROBE=$R/probe/seg_probe
SEG=/dev/shm/w09c9alias__IPC_SHM__CHUNK_INFO__9216__C40
mkdir -p "$OUTROOT"

note() { echo "$*" | tee -a "$OUTROOT/matrix.driver.log"; }

note "=== T06 official-arm matrix start $(date '+%F %T %z') ==="
note "hygiene: 每臂前仅删除本用例专属前缀段（w09c9alias__IPC_SHM__*），⛔ 不动他人段"

run_arm() {
  local arm=$1
  local dir=$OUTROOT/$2        # ⛔ 目录名必须唯一：同一臂重复跑会互相覆盖指纹
  local libarm=$arm
  mkdir -p "$dir"
  local mode=${3:-clean}   # ⛔ mode 必须是**独立参数**：它曾被 ${2} 顶替，
                           #    导致除首臂外全部走"污染态"分支（红臂=继承上一臂的残留段）。
  if [ "$mode" = clean ]; then
    if [ -e "$SEG" ]; then
      note "  [pre] $arm 残留段存在：$("$PROBE" state "$SEG" 40 | sed 's/PROBE_HEAD48.*//')"
      rm -f /dev/shm/w09c9alias__IPC_SHM__*
    else
      note "  [pre] $arm 无残留段（/dev/shm 中无 w09c9alias__IPC_SHM__*）"
    fi
  else
    note "  [pre] $arm 污染态：保留上一臂残留段 $("$PROBE" state "$SEG" 40 | sed 's/PROBE_HEAD48.*//')"
  fi
  bash "$R/scripts/run_official_arm.sh" "$arm" "$R/libs/$libarm" "$dir" > "$dir/driver.stdout" 2>&1
  local rc=$?
  {
    echo "segment_policy  = $([ "$mode" = clean ] && echo 'arm 前清本用例专属前缀（跨臂卫生）' || echo 'arm 前保留残留段（故意污染）')"
    echo "segment_after   = $("$PROBE" state "$SEG" 40 | sed 's/PROBE_HEAD48.*//')"
    echo "segment_after_head = $("$PROBE" state "$SEG" 40 | sed 's/^[^ ]* //' | grep -o 'PROBE_HEAD48.*')"
  } >> "$dir/fingerprint.txt"
  note "  [post] $arm rc=$rc $(grep -oE 'W09-C9\].*' "$dir/run.log" | tail -1)"
  return $rc
}

# 1) 干净态逐臂（顺序与 T05 不同：把两个负控臂放在绿色臂**之前**，
#    以证明绿色不是"没被污染过"的产物）
run_arm V2NEG    V2NEG-1    clean || true
run_arm FIXNEG   FIXNEG-1   clean || true
run_arm BASELINE BASELINE-1 clean
run_arm ABLATE   ABLATE-1   clean
run_arm FIX_RB   FIX_RB-1   clean
# 2) 产品臂重复 3 次（稳定性；每次先清专属前缀段）
run_arm FIX FIX-1 clean
run_arm FIX FIX-2 clean
run_arm FIX FIX-3 clean
# 3) 故意污染对照：紧接负控臂之后跑 BASELINE（不清段）
#    预期：继承重复 id ⇒ bad_rounds=1；用来证明"跨臂段残留会污染读数"这一量具事实。
run_arm V2NEG V2NEG-2 clean || true
run_arm BASELINE BASELINE-polluted polluted

note "=== T06 official-arm matrix end $(date '+%F %T %z') ==="

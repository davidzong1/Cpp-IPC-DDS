#!/bin/bash
# W11 (t12) —— **串行配对实验**：控制面 CPU（tick 率归一化 + 冻结三元组）+ 扫描成本（§10.2 五量 + t44 结束值四量）。
#
# 硬约束（违反则读数不作数）：
#   ① **独占窗口**：任一时刻只有一个被测进程；逐 run 记录 loadavg（⛔ 不挑选最优轮）；
#   ② **冻结三元组**（W00 §11.3 R-3）：窗口 `--window-s 60` + `--settle-s 4`；
#      项数 = **注册项数**（state2 = n、state3 = 2n）；tick = `stats().tick_count` 同窗口差值 ÷ wall；
#   ③ **同库同工装**：全批共用 `--require-lib-sha256`，逐 run 落 lib/tool 双指纹；
#   ④ ⛔ 不同窗口 / 不同代的读数**不得直接比较**（本批同窗口同库，故批内可比）。
#
# 用法: w11_cpu_scan_sweep.sh <out_root> [--repeats 5] [--window-s 60] [--settle-s 4] [--phase cpu|scan|all]
set -u
ROOT=""; REP=5; WIN=60; SETTLE=4; PHASE=all
while [ $# -gt 0 ]; do
  case "$1" in
    --repeats) REP=$2; shift 2;;
    --window-s) WIN=$2; shift 2;;
    --settle-s) SETTLE=$2; shift 2;;
    --phase) PHASE=$2; shift 2;;
    *) ROOT=$1; shift;;
  esac
done
ROOT=${ROOT:?usage: w11_cpu_scan_sweep.sh <out_root> [--repeats 5]}
cd "$(dirname "$0")/../../.." || exit 1

BIN=build/bin/w10_r5_matrix
LIB=build/lib/libipc.so.1.3.0
[ -x "$BIN" ] || { echo "REFUSE: 缺 $BIN"; exit 2; }
[ -f "$LIB" ] || { echo "REFUSE: 缺 $LIB"; exit 2; }
if [ -d "$ROOT" ] && [ -n "$(ls -A "$ROOT" 2>/dev/null)" ]; then echo "REFUSE: $ROOT 非空（只追加）"; exit 2; fi
mkdir -p "$ROOT"

LIB_SHA=$(sha256sum "$LIB" | cut -d' ' -f1)
BIN_SHA=$(sha256sum "$BIN" | cut -d' ' -f1)
{
  echo "# W11 串行配对实验（控制面 CPU + 扫描成本）"
  echo "started=$(date '+%Y-%m-%d %H:%M:%S %z')"
  echo "harness=w10_r5_matrix（唯一工装；与 W10-R5/t38 同工装，可比）"
  echo "libipc.so.1.3.0 $LIB_SHA"
  echo "w10_r5_matrix   $BIN_SHA"
  echo "冻结三元组: 窗口 --window-s $WIN + --settle-s $SETTLE ; 项数=注册项数(state2=n, state3=2n) ; tick=stats().tick_count 同窗差值÷wall"
  echo "CPU 口径: 进程内 /proc/self/stat token[11]+token[12]（utime+stime；t51 量纲更正后同口径）"
  echo "每配置轮数: $REP"
  echo "nproc=$(nproc)"
} > "$ROOT/fingerprint.txt"

: > "$ROOT/runs.tsv"
# 表头（t68/F7+F8 追加第 14/15 列；旧批无表头 ⇒ 用首行注释区分）
printf '#sub\tstate\tn\tentries\tW\tdiag\trc\tcpu\ttick_per_s\ttick_max_us\tthreads\twall_s\tloadavg_before\tloadavg_after\tdomain\n' >> "$ROOT/runs.tsv"
DOM=700000
FAILED=0; N=0

# 每 run 一个独立进程（池 worker 数由首个 start() 决定 ⇒ 同进程跑多档会串档）
run() {  # run <phase> <state> <n> <workers> <diag> <rep>
  local ph=$1 st=$2 n=$3 w=$4 dg=$5 rp=$6
  local sub="${ph}-st${st}-n${n}-W${w}-diag${dg}-r${rp}"
  local d="$ROOT/$ph/$sub"
  [ -e "$d" ] && { echo "SKIP $sub"; return 0; }
  DOM=$((DOM+1)); local dom=$DOM
  local la_before; la_before=$(cut -d' ' -f1-3 /proc/loadavg)
  local t0 t1
  t0=$(date +%s)
  timeout 900 "$BIN" --n "$n" --workers "$w" --diag "$dg" --state "$st" --domain "$dom" \
      --window-s "$WIN" --settle-s "$SETTLE" --load-msgs 0 --round "$rp" \
      --run-id "$sub" --out "$d" --require-lib-sha256 "$LIB_SHA" > "$ROOT/$ph/$sub.log" 2>&1
  local rc=$?
  t1=$(date +%s)
  local la_after; la_after=$(cut -d' ' -f1-3 /proc/loadavg)
  local cpu="" tick="" tmax="" threads="" wall=""
  if [ -s "$d/windows.csv" ]; then
    read cpu tick tmax threads wall < <(python3 -c "
import csv
r=list(csv.DictReader(open('$d/windows.csv')))[0]
print(r['cpu_cores'], r['tick_per_s'], r['sched_tick_max_us'], r['threads'], r['wall_s'])")
  fi
  # ⛔ 列序（t68/F7+F8 起固定，第 14/15 列为 t68 追加）：
  #  1 sub  2 state  3 n  4 注册项数  5 W  6 diag  7 rc  8 cpu  9 tick/s  10 tick_max_us
  #  11 threads  12 墙钟 s  13 loadavg(before)  14 loadavg(after)  15 domain（逐 run 唯一，可第三方核验）
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$sub" "$st" "$n" "$((st==3 ? 2*n : n))" "$w" "$dg" "$rc" "${cpu:-}" "${tick:-}" \
    "${tmax:-}" "${threads:-}" "$((t1-t0))" "$la_before" "$la_after" "$dom" >> "$ROOT/runs.tsv"
  printf '  %-40s rc=%-3s cpu=%-10s tick/s=%-9s W=%-3s %ss load=%s\n' \
    "$sub" "$rc" "${cpu:-?}" "${tick:-?}" "$w" "$((t1-t0))" "$la_before"
  [ "$rc" -eq 0 ] || FAILED=$((FAILED+1))
  N=$((N+1))
}

mkdir -p "$ROOT/cpu" "$ROOT/scan"

# ── 组 1：控制面 CPU（diag=off；冻结三元组）────────────────────────────────
if [ "$PHASE" = all ] || [ "$PHASE" = cpu ]; then
  echo "== 组 1：控制面 CPU（diag=off）=="
  for rp in $(seq 1 "$REP"); do run cpu 1 1000 32 off "$rp"; done           # 态1：无 route
  for n in 100 500 1000; do for rp in $(seq 1 "$REP"); do run cpu 2 "$n" 32 off "$rp"; done; done
  for n in 100 500 1000; do for rp in $(seq 1 "$REP"); do run cpu 3 "$n" 32 off "$rp"; done; done
  # worker 数对照（W11 §"worker 数量扫描"；同 n=1000 同态，一次只改一个变量）
  for w in 4 32; do for rp in $(seq 1 "$REP"); do run cpu 3 1000 "$w" off "$rp"; done; done
fi

# ── 组 2：扫描成本（diag=on；§10.2 五量 + t44 结束值四量）──────────────────
if [ "$PHASE" = all ] || [ "$PHASE" = scan ]; then
  echo "== 组 2：扫描成本（diag=on）=="
  for n in 1 100 500 1000; do for rp in $(seq 1 "$REP"); do run scan 3 "$n" 32 on "$rp"; done; done
fi

{ echo "# 汇总"; echo "runs=$N failed=$FAILED"; echo "end=$(date '+%Y-%m-%d %H:%M:%S %z')"; } >> "$ROOT/fingerprint.txt"
if [ "$FAILED" -gt 0 ]; then echo "W11_SWEEP_FAIL runs=$N failed=$FAILED"; exit 1; fi
echo "W11_SWEEP_OK runs=$N"

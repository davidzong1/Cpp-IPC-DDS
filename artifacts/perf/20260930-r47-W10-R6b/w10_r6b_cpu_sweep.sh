#!/bin/bash
# t45 单工装同库重采：w10_r5_matrix（**唯一**仲裁工装）在 state1/2/3 与项数分档下重采。
# 用法: w10_r6b_cpu_sweep.sh <out_root> [--win-s 60] [--repeats 5]
set -u
ROOT=""; WIN=60; REP=5
while [ $# -gt 0 ]; do case "$1" in --win-s) WIN=$2; shift 2;; --repeats) REP=$2; shift 2;; *) ROOT=$1; shift;; esac; done
ROOT=${ROOT:?usage: w10_r6b_cpu_sweep.sh <out_root> [--win-s 60] [--repeats 5]}
cd "$(dirname "$0")/../../.." || exit 1
BIN=build/bin/w10_r5_matrix
[ -x "$BIN" ] || { echo "REFUSE: 缺 $BIN"; exit 2; }
if [ -d "$ROOT" ] && [ -n "$(ls -A "$ROOT" 2>/dev/null)" ]; then echo "REFUSE: $ROOT 非空"; exit 2; fi
mkdir -p "$ROOT"
LIB=build/lib/libipc.so.1.3.0
LIB_SHA=$(sha256sum "$LIB" | cut -d' ' -f1); BIN_SHA=$(sha256sum "$BIN" | cut -d' ' -f1)
{
  echo "# t45 单工装重采前置（$(date '+%Y-%m-%d %H:%M:%S %z')）"
  echo "仲裁工装 = w10_r5_matrix（唯一；⛔ 不用第二工装做对比）"
  echo "libipc.so.1.3.0 $LIB_SHA"; echo "w10_r5_matrix   $BIN_SHA"
  echo "窗口定义: --window-s $WIN --settle-s 4（窗口前静置，不含启动瞬态）"
  echo "CPU 口径: 进程内 /proc/self/stat token[11]+token[12]（utime+stime）"
  echo "重复次数/格: $REP"
  echo "## 机器"; echo "nproc=$(nproc) loadavg=$(cut -d' ' -f1-3 /proc/loadavg)"
} > "$ROOT/fingerprint.txt"
: > "$ROOT/summary.tsv"
DOM=60000; FAILED=0; N=0
run() {  # run <sub> <state> <n>
  local sub=$1 st=$2 n=$3; local d="$ROOT/$sub"; [ -e "$d" ] && return 0
  DOM=$((DOM+1)); local dom=$DOM
  timeout 900 "$BIN" --n "$n" --workers 32 --diag off --state "$st" --domain "$dom" \
      --window-s "$WIN" --settle-s 4 --load-msgs 0 --round 1 --run-id "$sub" --out "$d" \
      --require-lib-sha256 "$LIB_SHA" > "$ROOT/$sub.log" 2>&1
  local rc=$?
  local cpu="" tick="" tm=""
  if [ -s "$d/windows.csv" ]; then
    read cpu tick tm < <(python3 -c "
import csv;r=list(csv.DictReader(open('$d/windows.csv')))[0]
print(r['cpu_cores'], r['tick_per_s'], r['sched_tick_max_us'])")
  fi
  printf '%-26s st=%d n=%-5s rc=%-3s cpu=%-9s tick/s=%-9s tick_max_us=%s\n' "$sub" "$st" "$n" "$rc" "${cpu:-?}" "${tick:-?}" "${tm:-?}"
  printf '%s\tst=%s\tn=%s\trc=%s\tcpu=%s\ttick_s=%s\ttick_max_us=%s\n' "$sub" "$st" "$n" "$rc" "${cpu:-}" "${tick:-}" "${tm:-}" >> "$ROOT/summary.tsv"
  [ "$rc" -eq 0 ] || { FAILED=$((FAILED+1)); }
  N=$((N+1))
}
# A. 三态 × REP（n=1000）
for st in 1 2 3; do for i in $(seq 1 "$REP"); do run "A-st${st}-n1000-r${i}" "$st" 1000; done; done
# B. 项数分档 × REP（state2）—— §10.2 的 route 数分档
for n in 1 100 500 1000; do for i in $(seq 1 "$REP"); do run "B-st2-n${n}-r${i}" 2 "$n"; done; done
# C. 同配置重复（state3，地板对照）
for i in $(seq 1 "$REP"); do run "C-st3-n1000-r${i}" 3 1000; done
{ echo "# 汇总"; echo "runs=$N failed=$FAILED"; } >> "$ROOT/batch_summary.txt"
if [ "$FAILED" -gt 0 ]; then echo "CPU_SWEEP_FAIL failed=$FAILED runs=$N"; exit 1; fi
echo "CPU_SWEEP_OK runs=$N"

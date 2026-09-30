#!/bin/bash
# t45 外部逐 TID 采样（**上位口径**）：对**同一个**仲裁工装 w10_r5_matrix 的同一窗口
# 做外部逐 TID 采样，并与进程内 /proc/self/stat 同窗口读数并列。
# 用法: w10_r6b_extcpu.sh <out_root> <sub> <state> <n> [win_s] [settle_s]
set -u
ROOT=${1:?}; SUB=${2:?}; ST=${3:?}; N=${4:?}; WIN=${5:-30}; SETTLE=${6:-4}
cd "$(dirname "$0")/../../.." || exit 1
BIN=build/bin/w10_r5_matrix
mkdir -p "$ROOT/ext-cpu"
D="$ROOT/ext-cpu/$SUB"; [ -e "$D" ] && { echo "SKIP(已存在) $SUB"; exit 0; }
LIB_SHA=$(sha256sum build/lib/libipc.so.1.3.0 | cut -d' ' -f1)
timeout 900 "$BIN" --n "$N" --workers 32 --diag off --state "$ST" --domain $((70000+RANDOM%9000)) \
   --window-s "$WIN" --settle-s "$SETTLE" --run-id "$SUB" --out "$D" \
   --require-lib-sha256 "$LIB_SHA" > "$ROOT/ext-cpu/$SUB.log" 2>&1 &
JOB=$!
sleep $((SETTLE + 5))
PID=$(for p in $(ls /proc | grep -E '^[0-9]+$'); do c=$(cat /proc/$p/comm 2>/dev/null); [ "$c" = "w10_r5_matrix" ] && echo $p; done | head -1)
SAMPLE=$((WIN - SETTLE - 8)); [ "$SAMPLE" -lt 5 ] && SAMPLE=5
if [ -n "${PID:-}" ]; then
  python3 test/perf/w10/w10_r5_tidcpu.py "$PID" "$SAMPLE" --label "$SUB" --json > "$D.ext.json" 2>&1
fi
wait $JOB 2>/dev/null
python3 - "$D" "$SUB" <<'PY'
import csv, json, os, sys
D, SUB = sys.argv[1], sys.argv[2]
inproc = None
p = os.path.join(D, 'windows.csv')
if os.path.exists(p):
    r = list(csv.DictReader(open(p)))[0]
    inproc = (float(r['wall_s']), float(r['cpu_cores']), r['tick_per_s'], r['threads'])
ext = None
try: ext = json.load(open(D + '.ext.json'))
except Exception: pass
if inproc:
    print(f"  {SUB:<24} 进程内 cpu={inproc[1]:.5f} core (window {inproc[0]:.1f}s, tick/s={float(inproc[2]):.0f}, threads={inproc[3]})")
if ext:
    ratio = (ext['cpu_cores']/inproc[1]) if inproc and inproc[1] else float('nan')
    print(f"  {'':<24} 外部逐TID cpu={ext['cpu_cores']:.5f} core（{ext['nonzero_tids']} 非零 tid）⇒ 比值 {ratio:.4f}")
PY

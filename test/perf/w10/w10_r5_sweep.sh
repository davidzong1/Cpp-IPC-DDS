#!/bin/bash
# W10-R5 §10.2 + 三态空闲**正式复验矩阵**（t38 / 方案 §6.3）。
#
# 矩阵（每个配置一个**独立进程** —— 池的 worker 数是"首个成功 start() 的调用方"
# 一次性决定的，同进程跑多档会串档）：
#   A. 主对照：state 3 × n∈{1,100,500,1000} × diag∈{off,on} × ≥5 轮（交替采集）
#   B. 三态补齐：state∈{1,2} × n∈{1,100,500,1000} × diag=on × 1 轮
#   C. 有效消息负载：state 3 + 持续发布 × n∈{100,1000} × diag∈{off,on} × 3 轮
#   D. worker 对照（单独实验）：W∈{8,16,32}（不受容量限）+ W=4（受限档） × n=1000 × diag=on
#
# 用法: w10_r5_sweep.sh <out_root> [--window-s 60] [--rounds 5] [--fast]
set -u
ROOT=""
WIN=60
ROUNDS=5
FAST=0
PRE=0
while [ $# -gt 0 ]; do
  case "$1" in
    --window-s) WIN="$2"; shift 2;;
    --rounds) ROUNDS="$2"; shift 2;;
    --fast) FAST=1; shift;;
    --pre) PRE=1; shift;;
    *) ROOT="$1"; shift;;
  esac
done
ROOT=${ROOT:?usage: w10_r5_sweep.sh <out_root> [--window-s 60] [--rounds 5] [--fast]}
cd "$(dirname "$0")/../../.." || exit 1
BIN=build/bin/w10_r5_matrix
[ -x "$BIN" ] || { echo "REFUSE: 未找到 $BIN（先跑 build_harnesses.sh）"; exit 2; }
if [ -d "$ROOT" ] && [ -n "$(ls -A "$ROOT" 2>/dev/null)" ]; then
  echo "REFUSE: $ROOT 非空（只追加）"; exit 2
fi
mkdir -p "$ROOT"
LIB=build/lib/libipc.so.1.3.0
LIB_SHA=$(sha256sum "$LIB" 2>/dev/null | cut -d' ' -f1)
BIN_SHA=$(sha256sum "$BIN" | cut -d' ' -f1)

# ---------- 前置指纹与判据版本 ----------
{
  echo "# W10-R5 复验前置（$(date '+%Y-%m-%d %H:%M:%S %z')）"
  echo "## 库/工装指纹（成对，R0-A1）"
  echo "libipc.so.1.3.0 $LIB_SHA"
  echo "w10_r5_matrix   $BIN_SHA"
  echo "## 配对检查（库/头文件是否比工装新）"
  stale=0
  for t in src/dzIPC/threepools/recv_worker.cc src/dzIPC/threepools/recv_worker.h \
           include/dzIPC/threepools/recv_worker.h; do
    [ -f "$t" ] || continue
    [ "$t" -nt "$BIN" ] && { echo "STALE: $t"; stale=1; }
  done
  [ "$LIB" -nt "$BIN" ] && { echo "STALE: 库晚于工装"; stale=1; }
  [ "$stale" -eq 0 ] && echo "PAIRING: OK"
  echo "## 实际加载库"; ldd "$BIN" | grep -i ipc || true
  echo "## git"; git rev-parse HEAD; git status --short -- src include test | head -20
  echo "## 机器"; echo "nproc=$(nproc) loadavg=$(cut -d' ' -f1-3 /proc/loadavg)"
  echo "## 继承环境清理声明"
  echo "DZIPC_SHM_RECV_COMPAT=${DZIPC_SHM_RECV_COMPAT:-<unset>} DZIPC_SHM_RECV_WORKERS=${DZIPC_SHM_RECV_WORKERS:-<unset>} DZIPC_SHM_RECV_BUDGET_MSGS=${DZIPC_SHM_RECV_BUDGET_MSGS:-<unset>}"
  echo "## 判据版本"; echo "W10-R5/§6.2-§6.3/v1"
  echo "## 窗口"; echo "window_s=$WIN rounds=$ROUNDS fast=$FAST"
} > "$ROOT/fingerprint.txt"
unset DZIPC_SHM_RECV_COMPAT DZIPC_SHM_RECV_WORKERS DZIPC_SHM_RECV_BUDGET_MSGS

: > "$ROOT/summary.tsv"
FAILED=0
DOMBASE=9000
DOMSEQ=0
n_runs=0

run1() {  # run1 <sub> <state> <n> <workers> <diag> <load_msgs> <round>
  local sub="$1" st="$2" n="$3" w="$4" diag="$5" load="$6" round="$7"
  # ⛔ domain 必须**逐 run 唯一**：SHM 的 topic 段名与 domain 绑定，两个 run 用同一 domain
  # 会互相看到对方的残留段（n=500 与 n=1000 在旧的取模式里恰好同域 —— 已修）。
  DOMSEQ=$((DOMSEQ + 1))
  local dom=$((DOMBASE + DOMSEQ * 7))
  local d="$ROOT/$sub"
  [ -e "$d" ] && { echo "SKIP(已存在) $sub"; return 0; }
  local t0=$(date +%s)
  timeout 1200 "$BIN" --n "$n" --workers "$w" --diag "$diag" --state "$st" --domain "$dom" \
      --window-s "$WIN" --load-msgs "$load" --round "$round" --run-id "$sub" --out "$d" \
      --require-lib-sha256 "$LIB_SHA" \
      > "$ROOT/$sub.log" 2>&1
  local rc=$?
  local verdict
  verdict=$(grep -oE 'verdict=(PASS|FAIL) failures=[0-9]+' "$ROOT/$sub.log" | tail -1)
  local missing=""
  for f in manifest.json phases.csv windows.csv windows.jsonl counters.json threestate.json verdict.md; do
    [ -s "$d/$f" ] || missing="$missing$f,"
  done
  local ok=1
  [ "$rc" -eq 0 ] || ok=0
  case "$verdict" in *verdict=PASS*) ;; *) ok=0;; esac
  [ -z "$missing" ] || ok=0
  [ "$rc" -eq 124 ] && ok=0
  # 工装自身的指纹自证失败（rc=1 且日志含 FINGERPRINT_MISMATCH）也计入失败
  grep -q "FINGERPRINT_MISMATCH" "$ROOT/$sub.log" 2>/dev/null && ok=0
  local el=$(( $(date +%s) - t0 ))
  # 运行时自证：从 manifest 取该 run 自己记录的库/工装指纹（缺字段 = 未自证）
  local rlib rtool
  rlib=$(python3 -c "import json,sys;print(json.load(open('$d/manifest.json')).get('lib_sha256','MISSING'))" 2>/dev/null || echo ERR)
  rtool=$(python3 -c "import json,sys;print(json.load(open('$d/manifest.json')).get('tool_sha256','MISSING'))" 2>/dev/null || echo ERR)
  if [ "$rlib" != "MISSING" ] && [ "$rlib" != "ERR" ] && [ "$rlib" != "$LIB_SHA" ]; then
    echo "  ⚠️ 库指纹漂移：run=$rlib 采集起始=$LIB_SHA（跨 run 不可比）"
    echo "${sub}	DRIFT	$rlib	$LIB_SHA" >> "$ROOT/lib_drift.txt"
  fi
  printf 'lib_sha=%s tool_sha=%s\n' "$rlib" "$rtool" >> "$ROOT/$sub.fingerprint"
  printf '%-40s st=%d n=%-5s W=%-3s diag=%-3s load=%-3s round=%s rc=%-3s %-28s %ss missing=%s\n' \
      "$sub" "$st" "$n" "$w" "$diag" "$load" "$round" "$rc" "${verdict:-<none>}" "$el" "${missing:-none}"
  printf '%s\tst=%s\tn=%s\tW=%s\tdiag=%s\tload=%s\tround=%s\trc=%s\tok=%s\t%s\tmissing=%s\n' \
      "$sub" "$st" "$n" "$w" "$diag" "$load" "$round" "$rc" "$ok" "${verdict:-none}" "${missing:-none}" \
      >> "$ROOT/summary.tsv"
  [ "$ok" -eq 0 ] && { FAILED=$((FAILED+1)); echo "  ↑ 失败子项已计入批次失败"; }
  n_runs=$((n_runs+1))
}

if [ "$PRE" -eq 1 ]; then
  # 预演：每类各一档，验链路与落盘字段（**不作正式证据**）
  run1 pre-A-st3-n100-diagoff 3 100 32 off 0 1
  run1 pre-A-st3-n100-diagon  3 100 32 on  0 1
  run1 pre-B-st1-n100-diagon  1 100 32 on  0 1
  run1 pre-B-st2-n100-diagon  2 100 32 on  0 1
  run1 pre-C-load-n100-diagon 3 100 32 on  1 1
  run1 pre-D-W8-n1000-diagon  3 1000 8 on 0 1
elif [ "$FAST" -eq 1 ]; then
  # 快速链路验证：每类各一档
  run1 fast-st3-n100-diagoff 3 100 32 off 0 1
  run1 fast-st3-n100-diagon  3 100 32 on  0 1
  run1 fast-st1-n100-diagon  1 100 32 on  0 1
  run1 fast-st2-n100-diagon  2 100 32 on  0 1
  run1 fast-st3-n100-load    3 100 32 on  1 1
else
  # ---------- A. 主对照：state 3 × n × diag × ROUNDS 轮（交替 off/on） ----------
  for r in $(seq 1 "$ROUNDS"); do
    for n in 1 100 500 1000; do
      for diag in off on; do
        run1 "A-st3-n${n}-diag${diag}-r${r}" 3 "$n" 32 "$diag" 0 "$r"
      done
    done
  done
  # ---------- B. 三态补齐：state 1/2 ----------
  for st in 1 2; do
    for n in 1 100 500 1000; do
      run1 "B-st${st}-n${n}-diagon-r1" "$st" "$n" 32 on 0 1
    done
  done
  # ---------- C. 有效消息负载（任务原文：1/100/500/1000 都要采） ----------
  for n in 1 100 500 1000; do
    for diag in off on; do
      for r in 1 2; do
        run1 "C-load-n${n}-diag${diag}-r${r}" 3 "$n" 32 "$diag" 5 "$r"
      done
    done
  done
  # ---------- D. worker 对照（单独实验；W=4 为容量受限档） ----------
  for w in 4 8 16 32; do
    for r in 1 2; do
      run1 "D-W${w}-n1000-diagon-r${r}" 3 1000 "$w" on 0 "$r"
    done
  done
fi

{
  echo "# W10-R5 采集汇总"
  echo "runs=$n_runs failed=$FAILED window_s=$WIN rounds=$ROUNDS"
  echo "## 逐子项（见 summary.tsv）"
  cat "$ROOT/summary.tsv"
} > "$ROOT/batch_summary.txt"

if [ "$FAILED" -gt 0 ]; then
  echo "W10_R5_SWEEP_FAIL failed=$FAILED runs=$n_runs"
  echo "W10_R5_SWEEP_DONE"
  exit 1
fi
echo "W10_R5_SWEEP_OK runs=$n_runs"
echo "W10_R5_SWEEP_DONE"
exit 0

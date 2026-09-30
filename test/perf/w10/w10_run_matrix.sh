#!/bin/bash
# W10 正式矩阵批跑（t37/R3 修正版）—— 证据目录只追加，**失败必须传播**。
#
# 用法: w10_run_matrix.sh <run_root> [--build <build_dir>] [--run-id-prefix <pfx>]
#                          [--topic-manifest <file>] [--only <sub,sub,...>]
#                          [--inject-fail <sub>] [--expect-fail <sub,sub>]
#
# R3 修正（H5 / 方案 §5.3）：
#   ① **子运行退出码 + 判定 + 必需文件**三者共同决定批次退出码（修前只看最后一条 echo）；
#   ② 落盘失败（ARTIFACT_DIR_NOT_EMPTY / 文件缺失）**也是失败**；
#   ③ 区分「预期故障已正确处理」与「规模运行失败」（`--expect-fail`）；
#   ④ 参数化构建路径 / run_id 前缀 / 话题清单，删除 r25 硬编码；
#   ⑤ `rc=124`（watchdog 超时）或 `rc=139`（ABI 不匹配）⇒ 该 run 直接判失败（R0-3/R0-A1）；
#   ⑥ 每个子运行做**库/工装指纹配对**前置检查（--require-*-sha256），不配对即不出结论。
set -u

ROOT=""
BUILD_DIR="$PWD/build"
RUN_PREFIX=""
MANIFEST=""
ONLY=""
INJECT=""
EXPECT_FAIL=""
while [ $# -gt 0 ]; do
  case "$1" in
    --build) BUILD_DIR="$2"; shift 2;;
    --run-id-prefix) RUN_PREFIX="$2"; shift 2;;
    --topic-manifest) MANIFEST="$2"; shift 2;;
    --only) ONLY="$2"; shift 2;;
    --inject-fail) INJECT="$2"; shift 2;;
    --expect-fail) EXPECT_FAIL="$2"; shift 2;;
    -h|--help) sed -n '2,20p' "$0"; exit 0;;
    *) ROOT="$1"; shift;;
  esac
done
ROOT=${ROOT:?usage: w10_run_matrix.sh <run_root> [--build D] [--run-id-prefix P] [--only a,b]}
cd "$(dirname "$0")/../../.." || exit 1
BUILD_DIR="$(cd "$BUILD_DIR" && pwd)"
BIN="$BUILD_DIR/bin/w10_matrix"
IDLE="$BUILD_DIR/bin/w10_idle"
[ -x "$BIN" ] || { echo "REFUSE: 未找到 $BIN"; exit 2; }
[ -x "$IDLE" ] || { echo "REFUSE: 未找到 $IDLE"; exit 2; }

[ -n "$RUN_PREFIX" ] || RUN_PREFIX="$(date '+%Y%m%d')-r$$-W10"
LIB="$BUILD_DIR/lib/libipc.so.1.3.0"
[ -f "$LIB" ] || { echo "REFUSE: 未找到 $LIB"; exit 2; }
LIB_SHA="$(sha256sum "$LIB" | cut -d' ' -f1)"
BIN_SHA="$(sha256sum "$BIN" | cut -d' ' -f1)"
IDLE_SHA="$(sha256sum "$IDLE" | cut -d' ' -f1)"

# ---------- 前置：指纹 + **配对检查**（F4；⛔ 不配对即拒绝出结论） ----------
if [ -d "$ROOT" ] && [ -n "$(ls -A "$ROOT" 2>/dev/null)" ]; then
  echo "REFUSE: $ROOT 非空（只追加）"; exit 2
fi
mkdir -p "$ROOT"
{
  echo "# W10 批跑前置（$(date '+%Y-%m-%d %H:%M:%S %z')）"
  echo "## 构建目录"; echo "$BUILD_DIR"
  echo "## 库指纹（libipc.so.1.3.0）"; echo "$LIB_SHA"
  echo "## 工装指纹"; echo "w10_matrix $BIN_SHA"; echo "w10_idle $IDLE_SHA"
  echo "## 配对检查（ABI 纪律 R0-A1：按值返回结构体加字段后，陈旧调用方必须重编）"
  stale=0
  for t in src/dzIPC/threepools/recv_worker.h src/dzIPC/threepools/recv_worker.cc \
           include/dzIPC/threepools/recv_worker.h include/dzIPC/threepools/socket_recv_worker.h; do
    [ -f "$t" ] || continue
    if [ "$t" -nt "$BIN" ]; then echo "STALE: $t 晚于 $BIN ⇒ 必须重编工装"; stale=1; fi
  done
  if [ "$LIB" -nt "$BIN" ]; then echo "STALE: 库晚于工装 ⇒ 必须重编工装（ABI 风险）"; stale=1; fi
  [ "$stale" -eq 0 ] && echo "PAIRING: OK（无比工装更新的库/头文件）"
  echo "## 实际加载库"; ldd "$BIN" | grep -i ipc || true
  echo "## git"; git rev-parse HEAD; git status --short -- src include test | head -20
  echo "## 机器"; echo "nproc=$(nproc) loadavg=$(cut -d' ' -f1-3 /proc/loadavg)"
  echo "## 继承环境变量清理声明"
  echo "DZIPC_SHM_RECV_COMPAT=${DZIPC_SHM_RECV_COMPAT:-<unset>} DZIPC_SHM_RECV_WORKERS=${DZIPC_SHM_RECV_WORKERS:-<unset>} DZIPC_SOCKET_COMPAT_THREAD=${DZIPC_SOCKET_COMPAT_THREAD:-<unset>} DZIPC_SOCKET_RECV_WORKERS=${DZIPC_SOCKET_RECV_WORKERS:-<unset>}"
} > "$ROOT/fingerprint.txt"

unset DZIPC_SHM_RECV_COMPAT DZIPC_SHM_RECV_WORKERS DZIPC_SOCKET_COMPAT_THREAD DZIPC_SOCKET_RECV_WORKERS

REQUIRED_TOPOLOGY="ledger.csv,phases.csv,samples.jsonl,counters.json,manifest.json,verdict.md"
declare -a RESULTS=()
declare -a EXPECTED_FAILS=()

sub_in_list() { [ -z "$2" ] && return 1; case ",$2," in *",$1,"*) return 0;; *) return 1;; esac; }
select_sub()   { [ -z "$ONLY" ] && return 0; sub_in_list "$1" "$ONLY"; }

record_and_tally() {  # record_and_tally <sub> <rc> <verdict> <dir> <expect_fail>
  local sub="$1" rc="$2" verdict="$3" dir="$4" expect_fail="$5"
  local missing=""
  if [ -n "$dir" ] && [ -d "$dir" ]; then
    local IFS=','
    # ⛔ 必须是普通文件：同名目录会被 -f 过滤掉；另要求非零大小（零字节占位不算有效读数）。
    for f in $REQUIRED_TOPOLOGY; do { [ -f "$dir/$f" ] && [ -s "$dir/$f" ]; } || missing="$missing$f,"; done
  else
    missing="(no dir)"
  fi
  local ok=1
  [ "$rc" -eq 0 ] || ok=0
  case "$verdict" in *verdict=PASS*) ;; *) ok=0;; esac
  [ -z "$missing" ] || ok=0
  [ "$rc" -eq 124 ] && ok=0      # watchdog 超时 ⇒ 直接失败（R0-3）
  [ "$rc" -eq 139 ] && ok=0      # ABI 不匹配 SIGSEGV ⇒ 直接失败（R0-A1）

  local tag="OK"
  if [ "$expect_fail" -eq 1 ]; then
    if [ "$ok" -eq 0 ]; then tag="EXPECTED_FAIL(正确识别)"; ok=1
    else tag="UNEXPECTED_PASS"; ok=0; fi
  elif [ "$ok" -eq 0 ]; then tag="FAIL"; fi

  printf '%-24s rc=%-4s %-28s %-24s missing=%s\n' "$sub" "$rc" "${verdict:-<no verdict>}" "$tag" "${missing:-none}"
  printf '%s\trc=%s\tok=%s\t%s\tmissing=%s\n' "$sub" "$rc" "$ok" "${verdict:-none}" "${missing:-none}" >> "$ROOT/summary.tsv"
  if [ "$ok" -eq 0 ]; then RESULTS+=("$sub"); else EXPECTED_FAILS+=("$sub"); fi
}

run() {  # run <子目录> <可执行> <expect_fail 0|1> <参数...>
  local sub="$1"; shift
  local exe="$1"; shift
  local expect_fail="$1"; shift
  select_sub "$sub" || return 0
  echo "=== $sub ==="
  local extra=()
  [ "$exe" = "$BIN" ] && extra+=(--require-lib-sha256 "$LIB_SHA" --require-bin-sha256 "$BIN_SHA")
  if [ "$sub" = "$INJECT" ]; then extra+=(--force-fail "t37 验收注入：验证批次失败传播"); fi
  timeout 1800 "$exe" "$@" "${extra[@]}" --out "$ROOT/$sub" > "$ROOT/$sub.log" 2>&1
  local rc=$?
  local verdict
  verdict=$(grep -oE 'verdict=(PASS|FAIL) failures=[0-9]+' "$ROOT/$sub.log" | tail -1)
  record_and_tally "$sub" "$rc" "$verdict" "$ROOT/$sub" "$expect_fail"
}

: > "$ROOT/summary.tsv"
[ -n "$MANIFEST" ] && cp "$MANIFEST" "$ROOT/topic_manifest.txt"

# ---------- SHM 拓扑 ----------
run shm-ind-1        "$BIN" 0 --transport shm --topology independent --n 1    --domain 3001 --msgs 3 --payload 64 --run-id "$RUN_PREFIX-shm-ind-1"
run shm-ind-100      "$BIN" 0 --transport shm --topology independent --n 100  --domain 3002 --msgs 3 --payload 64 --run-id "$RUN_PREFIX-shm-ind-100"
run shm-ind-1000     "$BIN" 0 --transport shm --topology independent --n 1000 --domain 3003 --msgs 3 --payload 64 --run-id "$RUN_PREFIX-shm-ind-1000"
run shm-bcast-32     "$BIN" 0 --transport shm --topology broadcast   --n 32   --domain 3010 --msgs 3 --payload 64 --run-id "$RUN_PREFIX-shm-bcast-32"
run shm-hotcold-1000 "$BIN" 0 --transport shm --topology hotcold     --n 1000 --domain 3020 --hot-msgs 40000 --run-id "$RUN_PREFIX-shm-hotcold-1000"
# ---------- socket ----------
run socket-ind-1     "$BIN" 0 --transport socket --topology independent --n 1   --domain 3101 --msgs 3 --payload 64 --run-id "$RUN_PREFIX-socket-ind-1"
run socket-ind-100   "$BIN" 0 --transport socket --topology independent --n 100 --domain 3102 --msgs 3 --payload 64 --run-id "$RUN_PREFIX-socket-ind-100"
# socket 千路：**有界 watchdog**（rc=124 ⇒ 失败，已由 record_and_tally 机械处理）
if select_sub socket-ind-1000; then
  echo "=== socket-ind-1000 ==="
  timeout 1500 "$BIN" --transport socket --topology independent --n 1000 --domain 3103 --msgs 1 --payload 64 \
    --silence-ms 2000 --run-id "$RUN_PREFIX-socket-ind-1000" \
    --require-lib-sha256 "$LIB_SHA" --require-bin-sha256 "$BIN_SHA" \
    --out "$ROOT/socket-ind-1000" > "$ROOT/socket-ind-1000.log" 2>&1
  record_and_tally socket-ind-1000 "$?" \
    "$(grep -oE 'verdict=(PASS|FAIL) failures=[0-9]+' "$ROOT/socket-ind-1000.log" | tail -1)" \
    "$ROOT/socket-ind-1000" 0
fi
# ---------- §10.1 三态空闲 ----------
run idle-state3-1000 "$IDLE" 0 --n 1000 --domain 3203 --state 3 --windows 1 --win-s 60
run idle-state2-1000 "$IDLE" 0 --n 1000 --domain 3202 --state 2 --windows 1 --win-s 60
run idle-state1-1000 "$IDLE" 0 --n 1000 --domain 3201 --state 1 --windows 1 --win-s 60

# ---------- 批次退出码：**由子结果共同决定**（H5；⛔ 不由最后一条 echo 决定） ----------
if [ "${#RESULTS[@]}" -gt 0 ]; then
  echo "W10_MATRIX_BATCH_FAIL sub=${#RESULTS[@]}: ${RESULTS[*]}"
  echo "W10_MATRIX_BATCH_DONE"
  exit 1
fi
echo "W10_MATRIX_BATCH_OK sub=0"
echo "W10_MATRIX_BATCH_DONE"
exit 0

#!/bin/bash
# W11 (t12) —— DZFlat TLV/A/B **串行配对实验**（≥N 轮；独占窗口；逐轮独立进程）。
#
# 为什么这样驱动：A/B 的权威工装是 W08 的 `test/test_w08_dzflat_ab.cpp`（跨进程 + 逐样本
# 完整载荷校验 + 三路径可分计数）。该文件的 run_id 是**内嵌常量**（W08 交付纪律），
# ⛔ 本任务不改他人 WP（`test/test_w08_dzflat_ab.cpp` 不在 `test/perf/**` 内）。
# 因此用 W08 自带的**落点覆盖点** `W08_ARTIFACT_ROOT` 把每一轮隔离到自己的目录：
#   <root>/round<K>/artifacts/perf/<内嵌 run_id>/
# ⇒ 每轮一个独立进程、一个独立落点，轮与轮之间不覆盖；W11 的归类键 = **round**。
#
# 口径（方案 §10.7，工装自身已按此落列）：
#   transport_ns = transport_done_ns − publish_enter_ns   ← 实验组①「传输机制」
#   app_read_ns  = fully_consumed_ns − app_obtained_ns     ← 实验组②「完整读取」
#   e2e_ns       = fully_consumed_ns − produced_ns         ← 实验组③「生产到消费」
#   delivery_ns  = app_obtained_ns − transport_done_ns     ← 通知与交付（不计入三组）
#
# 用法: w11_dzflat_pair.sh <out_root> [--rounds 5]
set -u
ROOT=""; ROUNDS=5
while [ $# -gt 0 ]; do case "$1" in --rounds) ROUNDS=$2; shift 2;; *) ROOT=$1; shift;; esac; done
ROOT=${ROOT:?usage: w11_dzflat_pair.sh <out_root> [--rounds 5]}
cd "$(dirname "$0")/../../.." || exit 1

TESTBIN=build/bin/test_w08_dzflat_ab
LIB=build/lib/libipc.so.1.3.0
[ -x "$TESTBIN" ] || { echo "REFUSE: 缺 $TESTBIN（先 make test_w08_dzflat_ab）"; exit 2; }
[ -f "$LIB" ] || { echo "REFUSE: 缺 $LIB"; exit 2; }
mkdir -p "$ROOT"

LIB_SHA=$(sha256sum "$LIB" | cut -d' ' -f1)
TST_SHA=$(sha256sum "$TESTBIN" | cut -d' ' -f1)
{
  echo "# W11 DZFlat TLV/A/B 串行配对（独占窗口，逐轮独立进程）"
  echo "harness=test_w08_dzflat_ab（W08 权威工装；本任务 ⛔ 未改其源码）"
  echo "libipc.so.1.3.0   $LIB_SHA"
  echo "test_w08_dzflat_ab $TST_SHA"
  echo "rounds=$ROUNDS  samples/round=6 配置 × 3 尺寸档 × 40 条 = 720 样本/轮"
  echo "落点规则: <root>/round<K>/artifacts/perf/<工装内嵌 run_id>/（W08_ARTIFACT_ROOT 逐轮隔离）"
  echo "口径: transport=①传输机制 / app_read=②完整读取 / e2e=③生产到消费（§10.7）"
  echo "started=$(date '+%Y-%m-%d %H:%M:%S %z')"
} > "$ROOT/fingerprint.txt"

FAILED=0; PASSED=0
for k in $(seq 1 "$ROUNDS"); do
  RD="$ROOT/round$k"
  if [ -n "$(ls -A "$RD" 2>/dev/null)" ]; then echo "SKIP round$k（已存在）"; continue; fi
  mkdir -p "$RD"
  LA=$(cut -d' ' -f1-3 /proc/loadavg)
  T0=$(date +%s)
  W08_ARTIFACT_ROOT="$PWD/$RD" "$TESTBIN" \
      --gtest_filter='W08DzFlatAB.CrossProcessPerSampleEvidenceIsWrittenAndPayloadFullyVerified' \
      > "$ROOT/round$k.log" 2>&1
  RC=$?
  T1=$(date +%s)
  if [ "$RC" -eq 0 ]; then PASSED=$((PASSED+1)); else FAILED=$((FAILED+1)); fi
  printf '  round%-2s rc=%-3s %ss load=%s %s\n' "$k" "$RC" "$((T1-T0))" "$LA" \
      "$(grep -c '^\[       OK \]' "$ROOT/round$k.log" 2>/dev/null) OK-line"
done

{ echo "rounds_ok=$PASSED rounds_failed=$FAILED"; echo "end=$(date '+%Y-%m-%d %H:%M:%S %z')"; } >> "$ROOT/fingerprint.txt"
if [ "$FAILED" -gt 0 ]; then echo "W11_DZFLAT_FAIL ok=$PASSED failed=$FAILED"; exit 1; fi
echo "W11_DZFLAT_OK rounds=$PASSED"

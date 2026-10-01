#!/bin/bash
# T05 §7.3.4/§7.3.5 —— 同二进制换库的五库矩阵（专属前缀 / 8 话题并发首借 / 1200 轮）
# 唯一变量 = LD_LIBRARY_PATH 指向的 libipc.so.1.3.0；工装二进制与探针二进制全程不变。
# 每个臂各自一个 run 目录：artifacts/perf/20261001-w12-T05/arms-run/<ARM>-<k>/
set -u
ROOT=/home/zwc/cpp_ipc_dds
cd "$ROOT" || exit 1
RUN=$ROOT/build/T05/37e6b190-b12b-4128-b770-917b299297aa
ARMS=$ROOT/artifacts/perf/20261001-w12-T05/arms
OUTROOT=$ROOT/artifacts/perf/20261001-w12-T05/arms-run
PROBE=$ROOT/artifacts/perf/20261001-w12-T05/probe/t05_alias_probe
ROUNDS=${ROUNDS:-1200}
mkdir -p "$OUTROOT"
: > "$OUTROOT/matrix.raw.tsv"
: > "$OUTROOT/matrix.driver.log"

say() { printf '%s\n' "$*" | tee -a "$OUTROOT/matrix.driver.log"; }

say "=== T05 五库矩阵（同二进制换库，唯一变量 = libipc.so.1.3.0）$(date '+%F %T %z') ==="
say "探针二进制 = $PROBE sha256=$(sha256sum $PROBE | cut -d' ' -f1)"
say "轮数 = $ROUNDS，话题数 = 8，借样 = 8000 B（档 9216），前缀 = 各臂专属"
say "/dev/shm 初始段数 = $(ls /dev/shm 2>/dev/null | wc -l)"

for spec in "$@"; do
  ARM=${spec%%:*}
  K=${spec##*:}
  PREFIX="w12t05_${ARM}_${K}"
  D="$OUTROOT/${ARM}-${K}"
  mkdir -p "$D"
  START=$(date '+%F %T %z'); START_EPOCH=$(date +%s)
  CMD="LD_LIBRARY_PATH=${ARMS#$ROOT/}/$ARM timeout 1800 ${PROBE#$ROOT/} --rounds $ROUNDS --prefix $PREFIX"
  set +e
  LD_LIBRARY_PATH="$ARMS/$ARM" timeout 1800 "$PROBE" --rounds "$ROUNDS" --prefix "$PREFIX" \
      > "$D/run.log" 2>&1
  RC=$?
  set -e
  END=$(date '+%F %T %z'); END_EPOCH=$(date +%s)
  RES=$(grep -m1 '^T05_RESULT' "$D/run.log" || echo "T05_RESULT <missing>")
  FP=$(grep -m1 '^T05_FINGERPRINT' "$D/run.log" || echo "T05_FINGERPRINT <missing>")
  HIT=$(grep -m1 '^T05_ALIAS_HIT' "$D/run.log" || echo "T05_ALIAS_HIT none")
  ABN=$(grep -m1 '^T05_ABNORMAL' "$D/run.log" || echo "T05_ABNORMAL none")
  ORPHAN=$(grep -c 'orphan segment reset' "$D/run.log" || true)
  f() { echo "$RES" | grep -oE "$1=[0-9-]+" | head -1 | cut -d= -f2; }
  BAD=$(f bad_rounds); SKIP=$(f skipped_rounds); EXEC=$(f executed); CRASH=$(f crash_round)
  IDS=$(f same_id_pairs); PTRS=$(f same_data_ptr)
  {
    echo "# T05 单臂 run 指纹（读数与库的绑定只认本文件）"
    echo "arm              = $ARM"
    echo "run_dir          = ${D#$ROOT/}"
    echo "cmdline          = $CMD"
    echo "lib_dir          = ${ARMS#$ROOT/}/$ARM"
    echo "lib_real_path    = $(readlink -f "$ARMS/$ARM/libipc.so.1.3.0")"
    echo "lib_sha256       = $(sha256sum "$ARMS/$ARM/libipc.so.1.3.0" | cut -d' ' -f1)"
    echo "lib_size         = $(stat -c%s "$ARMS/$ARM/libipc.so.1.3.0")"
    echo "binary           = build/bin/test_chunk_capacity_backpressure"
    echo "binary_sha256    = $(sha256sum build/bin/test_chunk_capacity_backpressure | cut -d' ' -f1)"
    echo "probe_binary     = ${PROBE#$ROOT/}"
    echo "probe_sha256     = $(sha256sum "$PROBE" | cut -d' ' -f1)"
    echo "probe_source     = artifacts/perf/20261001-w12-T05/probe/t05_alias_probe.cpp"
    echo "probe_source_sha256 = $(sha256sum artifacts/perf/20261001-w12-T05/probe/t05_alias_probe.cpp | cut -d' ' -f1)"
    echo "product_ipc_cpp  = src/libipc/ipc.cpp"
    echo "product_ipc_sha256 = $(sha256sum src/libipc/ipc.cpp | cut -d' ' -f1)"
    echo "rounds_requested = $ROUNDS"
    echo "rounds_executed  = $EXEC"
    echo "topics           = 8"
    echo "prefix           = $PREFIX   (非空前缀)"
    echo "clear_storage    = once-before-first-round  (⛔ 不每轮清段)"
    echo "collect_start    = $START (epoch $START_EPOCH)"
    echo "collect_end      = $END (epoch $END_EPOCH)"
    echo "probe_exit_code  = $RC"
    echo "bad_rounds       = $BAD"
    echo "skipped_rounds   = $SKIP"
    echo "same_id_pairs    = $IDS"
    echo "same_data_ptr    = $PTRS"
    echo "crash_round      = $CRASH"
    echo "orphan_segment_reset_count = $ORPHAN"
    echo "alias_hit_line   = $HIT"
    echo "abnormal_line    = $ABN"
    echo "self_fingerprint = $FP"
    echo "result           = $RES"
    echo "host             = nproc=$(nproc) $(uname -srm)"
  } > "$D/fingerprint.txt"
  printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' \
    "$ARM" "$K" "$RC" \
    "$(sha256sum "$ARMS/$ARM/libipc.so.1.3.0" | cut -c1-16)" \
    "$BAD" "$SKIP" "$IDS" "$PTRS" "$ORPHAN" "$(basename $D)" \
    >> "$OUTROOT/matrix.raw.tsv"
  say "$(printf '%-9s k=%s rc=%-3s lib=%s orphan_reset=%-2s %s' "$ARM" "$K" "$RC" \
      "$(sha256sum "$ARMS/$ARM/libipc.so.1.3.0" | cut -c1-16)" "$ORPHAN" "$RES")"
done
say "=== 末尾 /dev/shm 段数 = $(ls /dev/shm 2>/dev/null | wc -l) ==="

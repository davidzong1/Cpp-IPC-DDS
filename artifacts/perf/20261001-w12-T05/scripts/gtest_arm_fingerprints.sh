#!/bin/bash
# T05 —— 为「官方常驻用例 × 五库」的每臂 run 目录补 fingerprint.txt（读数与库的绑定只认它）
set -u
ROOT=/home/zwc/cpp_ipc_dds
cd "$ROOT" || exit 1
R=$ROOT/artifacts/perf/20261001-w12-T05
for arm in BASELINE ABLATE V2NEG FIXNEG FIX; do
  d=$R/gtest-arm/$arm
  lib=$R/arms/$arm/libipc.so.1.3.0
  {
    echo "# T05 官方常驻判据（build/bin/test_chunk_capacity_backpressure --gtest_filter='*OrphanReset*'）单臂指纹"
    echo "arm              = $arm"
    echo "run_dir          = ${d#$ROOT/}"
    echo "cmdline          = LD_LIBRARY_PATH=artifacts/perf/20261001-w12-T05/arms/$arm timeout 900 build/bin/test_chunk_capacity_backpressure --gtest_filter='*OrphanReset*' --gtest_color=no > $arm/run.log 2>&1"
    echo "lib_real_path    = $(readlink -f "$lib")"
    echo "lib_sha256       = $(sha256sum "$lib" | cut -d' ' -f1)"
    echo "lib_size         = $(stat -c%s "$lib")"
    echo "binary           = build/bin/test_chunk_capacity_backpressure"
    echo "binary_sha256    = $(sha256sum build/bin/test_chunk_capacity_backpressure | cut -d' ' -f1)"
    echo "exe_rc           = $( [ "$(grep -c '\[  FAILED  \]' "$d/run.log")" -gt 0 ] && echo 1 || echo 0 )"
    echo "gtest_case       = ChunkCapacityBackpressure.OrphanResetDoesNotAliasInflightLoansAcrossTopics"
    echo "kRounds          = 1200 (源内常量)"
    echo "prefix           = w09c9alias (用例内的专属前缀；非空)"
    echo "topics           = 8 话题并发首借, 每轮两个独立 fork 进程"
    echo "clear_storage    = 开跑前一次（⛔ 非每轮清段）"
    echo "gtest_verdict    = $(grep -oE '\[  (PASSED|FAILED)  \]' "$d/run.log" | head -1)"
    echo "reading          = $(grep -oE 'W09-C9\].*' "$d/run.log" | head -1)"
    echo "orphan_segment_reset_count = $(grep -c 'orphan segment reset' "$d/run.log" || true)"
    echo "elapsed          = $(tail -1 "$d/time.txt" 2>/dev/null | tr -d '\n')"
    echo "host             = nproc=$(nproc) $(uname -srm)"
  } > "$d/fingerprint.txt"
  echo "written $d/fingerprint.txt"
done

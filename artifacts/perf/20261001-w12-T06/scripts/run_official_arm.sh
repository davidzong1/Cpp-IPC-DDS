#!/usr/bin/env bash
# T06 / W12 §8 —— **官方工装**换库跑（同一个测试二进制，只替换 libipc.so.3）。
#
# ⛔ 不改产品代码、不改 test/CMakeLists.txt、不改 W05/W11/W12 文档。
# ⛔ 不声称 `--require-lib-sha256` 被本工装接受（该参数属 W10/W11 perf 驱动器，
#    本二进制不支持 —— T05 实测被 gtest 忽略，本 run 复核了这一点，见 diag/）。
#
# 用法: run_official_arm.sh <ARM> <LIBDIR> <OUTDIR>
set -u
ARM=${1:?arm}
LIBDIR=${2:?libdir}
OUT=${3:?outdir}
BIN=$PWD/build/bin/test_chunk_capacity_backpressure
mkdir -p "$OUT"

{
  echo "# T06 官方工装换库单臂指纹（读数与库绑定的唯一凭据）"
  echo "arm              = $ARM"
  echo "run_dir          = $OUT"
  echo "cmdline          = LD_LIBRARY_PATH=$LIBDIR $BIN --gtest_filter='*OrphanReset*' --gtest_color=no"
  echo "lib_real_path    = $LIBDIR/libipc.so.1.3.0"
  echo "lib_sha256       = $(sha256sum "$LIBDIR/libipc.so.1.3.0" | awk '{print $1}')"
  echo "lib_size         = $(stat -c%s "$LIBDIR/libipc.so.1.3.0")"
  echo "binary           = $BIN"
  echo "binary_sha256    = $(sha256sum "$BIN" | awk '{print $1}')"
  echo "product_lib_sha256 = $(sha256sum build/lib/libipc.so.1.3.0 | awk '{print $1}')"
  echo "collect_start    = $(date '+%Y-%m-%d %H:%M:%S %z')"
} > "$OUT/fingerprint.txt"

env LD_LIBRARY_PATH="$LIBDIR" timeout 900 "$BIN" --gtest_filter='*OrphanReset*' \
    --gtest_color=no > "$OUT/run.log" 2>&1
RC=$?

{
  echo "exe_rc           = $RC"
  echo "gtest_verdict    = $(grep -E '\[  (PASSED|FAILED)  \]' "$OUT/run.log" | tail -1 | sed 's/^ *//')"
  echo "reading          = $(grep -oE 'W09-C9\].*' "$OUT/run.log" | tail -1)"
  echo "orphan_segment_reset_count = $(grep -c 'orphan segment reset' "$OUT/run.log")"
  echo "loan_fail_lines  = $(grep -c 'fail: loan, que->ready_sending' "$OUT/run.log")"
  echo "collect_end      = $(date '+%Y-%m-%d %H:%M:%S %z')"
} >> "$OUT/fingerprint.txt"

echo "=== $ARM rc=$RC $(grep -oE 'W09-C9\].*' "$OUT/run.log" | tail -1)"
grep -c 'orphan segment reset' "$OUT/run.log" | sed "s/^/$ARM orphan_reset_lines=/"
# ⛔ 必须把 gtest 的退出码透传给调用方：否则驱动脚本会把红臂记成 rc=0
#    （实测踩过：末条 grep 的退出码被当成臂的退出码）。
exit $RC

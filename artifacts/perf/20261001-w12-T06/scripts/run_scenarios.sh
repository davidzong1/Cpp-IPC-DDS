#!/usr/bin/env bash
# T06 / W12 §8 四类场景驱动（同一工装探针二进制，只换库）。
#
# 用法: run_scenarios.sh <ARM> <LIBDIR> <ROOT> [RACE_ROUNDS] [TOPICS]
#   ARM        臂名（读数组名）
#   LIBDIR     含 libipc.so.1.3.0 的目录（只换库，不改二进制）
#   ROOT       证据根目录（默认 artifacts/perf/20261001-w12-T06/scenarios）
#   RACE_ROUNDS race 场景轮数（默认 300）
#   TOPICS     race 场景每轮并发话题数（默认 8）
#
# 产物: $ROOT/<ARM>/{fresh,live,dead,race}.log + summary.txt + lib_sha256.txt
# ⛔ 不写 /dev/shm 之外的任何产品/工装路径；⛔ 不调用 cmake/make。
set -u
ARM=${1:?arm}
LIBDIR=${2:?libdir}
ROOT=${3:-artifacts/perf/20261001-w12-T06/scenarios}
RACE_ROUNDS=${4:-300}
TOPICS=${5:-8}

RUN=$PWD
OUT=$ROOT/$ARM
mkdir -p "$OUT"
PROBE=$RUN/artifacts/perf/20261001-w12-T06/probe/t06_scenario_probe

LIB_SHA=$(sha256sum "$LIBDIR/libipc.so.1.3.0" | awk '{print $1}')
PROBE_SHA=$(sha256sum "$PROBE" | awk '{print $1}')
CHECKSUM=$(ldd "$PROBE" | awk '/libipc/ {print $3}')

{
  echo "arm            = $ARM"
  echo "lib_dir        = $LIBDIR"
  echo "lib_real_path  = $LIBDIR/libipc.so.1.3.0"
  echo "lib_sha256     = $LIB_SHA"
  echo "lib_size       = $(stat -c%s "$LIBDIR/libipc.so.1.3.0")"
  echo "probe_binary   = $PROBE"
  echo "probe_sha256   = $PROBE_SHA"
  echo "probe_ldd_libipc = $CHECKSUM"
  echo "official_binary = build/bin/test_chunk_capacity_backpressure"
  echo "official_binary_sha256 = $(sha256sum build/bin/test_chunk_capacity_backpressure | awk '{print $1}')"
  echo "product_lib_sha256 = $(sha256sum build/lib/libipc.so.1.3.0 | awk '{print $1}')"
  echo "race_rounds    = $RACE_ROUNDS"
  echo "topics         = $TOPICS"
  echo "ld_library_path= $LIBDIR"
  echo "collect_start  = $(date '+%Y-%m-%d %H:%M:%S %z')"
} > "$OUT/fingerprint.txt"

run_one() {
  local scen=$1; shift
  local log=$OUT/$scen.log            # 量具 stdout（父进程）
  local dlog=$OUT/$scen.driver.log    # 外层驱动转录（tee 目标）
  echo "=== [$(date '+%H:%M:%S')] $scen: $*" | tee -a "$OUT/driver.log"
  # ⛔ 量具的 stdout 与子进程的 stdout **必须是两个文件**：子进程的 orphan reset 行
  #    总量按"本轮新增字节"计数，若 tee 也在写同一个文件，计数基线会被 tee 的输出推动
  #    ⇒ 逐轮计数可能被吞（量具自检 T06_MEAS_ERR 会拦住这种情况，但不应让它发生）。
  env LD_LIBRARY_PATH="$LIBDIR" T06_LOG_DIR="$OUT" \
      "$PROBE" "$scen" "t06${ARM,,}_$scen" "$@" > "$log" 2>&1
  echo "probe_exit=$?" >> "$log"
  cat "$log" | tee "$dlog" | tail -2
}

run_one fresh 10
run_one live 10
run_one dead 10 1
run_one race "$RACE_ROUNDS" "$TOPICS"

{
  echo "collect_end    = $(date '+%Y-%m-%d %H:%M:%S %z')"
  for f in fresh live dead race; do
    echo "--- $f"
    grep -E "T06_[A-Z_]*RESULT|T06_MEAS_ERR" "$OUT/$f.log" | tail -3
  done
} >> "$OUT/fingerprint.txt"

grep -hE "T06_[A-Z_]*RESULT|T06_MEAS_ERR" "$OUT"/{fresh,live,dead,race}.log

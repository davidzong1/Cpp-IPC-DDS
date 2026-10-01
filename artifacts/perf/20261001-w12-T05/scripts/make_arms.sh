#!/bin/bash
# T05 —— 组装「同二进制换库」的五个臂目录；FIX 臂直接用**冻结的产品库本体**。
# 每个臂目录只放 libipc 三件套（libipc.so.1.3.0 + 两个符号链接），供 LD_LIBRARY_PATH 选库。
set -u
ROOT=/home/zwc/cpp_ipc_dds
ATT=37e6b190-b12b-4128-b770-917b299297aa
RUN=$ROOT/build/T05/$ATT
ARMS=$ROOT/artifacts/perf/20261001-w12-T05/arms
mkdir -p "$ARMS"

mk() { # mk <ARM> <源库文件>
  local arm=$1
  local src=$2
  local d="$ARMS/$arm"
  rm -rf "$d"; mkdir -p "$d"
  cp "$src" "$d/libipc.so.1.3.0"
  ln -s libipc.so.1.3.0 "$d/libipc.so.3"
  ln -s libipc.so.3 "$d/libipc.so"
  printf '%-9s %s  %s\n' "$arm" "$(sha256sum "$d/libipc.so.1.3.0" | cut -d' ' -f1)" "$(stat -c%s "$d/libipc.so.1.3.0")"
}

mk BASELINE "$RUN/lib/libipc.so.1.3.0.BASELINE"
mk ABLATE   "$RUN/lib/libipc.so.1.3.0.ABLATE"
mk V2NEG    "$RUN/lib/libipc.so.1.3.0.V2NEG"
mk FIXNEG   "$RUN/lib/libipc.so.1.3.0.FIXNEG"
# FIX = 冻结的产品库本体（T01 已登记 sha256=813fab5b…）
mk FIX      "$ROOT/build/lib/libipc.so.1.3.0"
# FIX_RB = 我按 flags.make/link.txt 逐字重建的 FIX 库（交叉核对：两条独立构建路径同判据）
mk FIX_RB   "$RUN/lib/libipc.so.1.3.0.FIX"

echo "--- 运行时实际命中哪个库（LD_LIBRARY_PATH 优先于 DT_RUNPATH）---"
for arm in BASELINE ABLATE V2NEG FIXNEG FIX FIX_RB; do
  printf '%-9s ' "$arm"
  LD_LIBRARY_PATH="$ARMS/$arm" ldd $ROOT/build/bin/test_chunk_capacity_backpressure | grep libipc.so.3 | awk '{print $3}'
done
echo "--- 未设 LD_LIBRARY_PATH 时命中产品库 ---"
ldd $ROOT/build/bin/test_chunk_capacity_backpressure | grep libipc.so.3 | awk '{print $3}'

#!/bin/bash
# T05 —— 第三方离线重放脚本（只依赖本 run 目录内的归档物 + 冻结的工装二进制）
#
# 用法：
#   bash artifacts/perf/20261001-w12-T05/scripts/replay.sh [ROUNDS]      # 默认 200，快
#   ROUNDS=1200 bash artifacts/perf/20261001-w12-T05/scripts/replay.sh  # 与正式读数同轮数
#
# 前置：/home/zwc/cpp_ipc_dds/build/bin/test_chunk_capacity_backpressure 与
#       artifacts/perf/20261001-w12-T05/probe/t05_alias_probe 已存在（后者可由
#       probe/t05_alias_probe.cpp 重编，编译命令见 T05_报告.md §8）。
set -u
ROOT=/home/zwc/cpp_ipc_dds
R=$ROOT/artifacts/perf/20261001-w12-T05
ROUNDS=${1:-${ROUNDS:-200}}
OUT=${OUT:-/tmp/t05-replay-$$}
mkdir -p "$OUT"
BIN=$ROOT/build/bin/test_chunk_capacity_backpressure
PROBE=$R/probe/t05_alias_probe

echo "工装 binary sha256 = $(sha256sum $BIN | cut -d' ' -f1)  (期望 08b803b169852c7e…)"
echo "探针 sha256        = $(sha256sum $PROBE | cut -d' ' -f1)  (期望 764366d22ce29540…)"
echo "轮数 = $ROUNDS   输出目录 = $OUT"
echo "臂指纹（归档库）:"
sha256sum $R/libs/libipc.so.1.3.0.* | awk '{printf "  %s  %s\n", substr($1,1,16), $2}'

echo
echo "=== A. 我的判据探针（8 话题 / 专属前缀 / 每轮两种 fork 进程）==="
for arm in BASELINE ABLATE V2NEG FIXNEG FIX FIX_RB; do
  LD_LIBRARY_PATH=$R/arms/$arm timeout 1800 "$PROBE" --rounds "$ROUNDS" \
      --prefix "w12t05_replay_${arm}_$$" > "$OUT/$arm.probe.log" 2>&1
  printf '  %-9s rc=%-3s %s\n' "$arm" "$?" "$(grep -m1 '^T05_RESULT' "$OUT/$arm.probe.log")"
done

echo
echo "=== B. 仓内常驻用例（--gtest_filter='*OrphanReset*'，kRounds 由源内常量 1200 决定）==="
for arm in BASELINE ABLATE V2NEG FIXNEG FIX; do
  LD_LIBRARY_PATH=$R/arms/$arm timeout 900 "$BIN" \
      --gtest_filter='*OrphanReset*' --gtest_color=no > "$OUT/$arm.gtest.log" 2>&1
  printf '  %-9s rc=%-3s %s\n' "$arm" "$?" "$(grep -oE 'W09-C9\].*' "$OUT/$arm.gtest.log" | head -1)"
done
echo
echo "确认两个二进制全程未被替换（sha256 应与开头一致）："
sha256sum $BIN $PROBE | awk '{printf "  %s  %s\n", substr($1,1,16), $2}'

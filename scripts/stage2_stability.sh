#!/usr/bin/env bash
# 阶段 2 并发稳定性复跑 —— 落点 docs/消息接收架构改造/阶段2_全量测试方案.md §6.1。
#
# 用途: 暴露偶发死锁、资源污染和竞态。**不能**取代确定性顺序断言, 也不能把
# "N 次未复现"解释成无竞态证明(方案 §6.1 原文)。
#
# 与方案 §6.1 的差异(必须记进报告):
#   ⛔ 原文用 `--gtest_brief=1`, 本仓 gtest 1.10.0 无此旗标 ⇒ 会整目标失败。已去掉。
#   ＋ 除方案点名的两条外, 把 §4 的三个新套件一并纳入复跑(它们各自含真实 pub/sub
#     与状态转换, 是最可能出现竞态的四个套件)。
#
# 用法: scripts/stage2_stability.sh [轮数]   默认 20; 提交前建议 50。
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
ROUNDS="${1:-20}"
OUT_DIR="${OUT_DIR:-/tmp/stage2_stability}"
PER_TEST_TIMEOUT="${PER_TEST_TIMEOUT:-120}"

targets=(
  test_shm_route_session
  test_wakeup_artifact
  test_shm_i5_pop_buffer
  test_shm_sub_dtor_gate
  test_shm_ready_transition
)

mkdir -p "$OUT_DIR"
unset LD_LIBRARY_PATH

echo "===== 稳定性复跑 ====="
echo "轮数: $ROUNDS   目标: ${targets[*]}"
echo "日志: $OUT_DIR"

fail=0
for i in $(seq 1 "$ROUNDS"); do
  for t in "${targets[@]}"; do
    log="$OUT_DIR/$t.$i.log"
    timeout "${PER_TEST_TIMEOUT}s" "$BUILD_DIR/bin/$t" >"$log" 2>&1
    rc=$?
    bad=$(grep -cE '^\[  FAILED  \]' "$log" || true)
    if [ "$rc" -ne 0 ] || [ "$bad" -ne 0 ]; then
      echo "⛔ $t 第 $i 轮失败 (rc=$rc bad=$bad)"
      tail -n 40 "$log"
      fail=1
      break 2
    fi
  done
  echo "  第 $i/$ROUNDS 轮: 全绿"
done

if [ "$fail" -ne 0 ]; then
  echo "结果: 失败(见上)。日志在 $OUT_DIR"
  exit 1
fi
echo "结果: $ROUNDS 轮全绿。注意: 这不等于无竞态证明(方案 §6.1 原文)。"

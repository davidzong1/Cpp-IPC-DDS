#!/usr/bin/env bash
# T06 / W12 §8 全矩阵驱动（**串行**执行，避免并发对 race 场景的时序干扰）。
#
# 臂与用途：
#   FORCE     量具有效性对照臂（自建非法实验库）：去掉"素净"门槛 + 段级探活恒判孤儿。
#             只跑 fresh/live ⇒ 用来证明**这两个场景的判据有牙**（产品臂全绿不是假绿）。
#   FIX       冻结产品库本体（build/lib/libipc.so.1.3.0）—— §8 主臂。
#   FIX_RB    T05 逐字重建的 FIX（独立构建路径交叉核对）。
#   V2NEG     负控：快照移出 handles_ 临界区。
#   FIXNEG    负控：删除 info->lock_ 下的 memcmp 复核。
#   BASELINE  无 reclaim 的历史库（e800ccc 的 ipc.cpp）。
#   ABLATE    保留 reclaim 代码但删除调用点。
#
# ⛔ 同一工装探针二进制（probe/t06_scenario_probe，SHA 记录在每臂 fingerprint.txt）；
#    只换 LD_LIBRARY_PATH 指向的 libipc.so.1.3.0。
#    ⛔ 不改产品代码、不改 test/CMakeLists.txt、不改 W05/W11/W12 文档。
set -u
ROOT=$PWD/artifacts/perf/20261001-w12-T06
ATT=${T06_ATTEMPT_DIR:?set T06_ATTEMPT_DIR to build/T06/<attempt_id>}
FORCE_LIB=$PWD/$ATT/lib
S=$ROOT/scripts/run_scenarios.sh

LONG_ROUNDS=${LONG_ROUNDS:-1200}
SHORT_ROUNDS=${SHORT_ROUNDS:-300}

echo "=== T06 matrix start $(date '+%F %T %z') host=$(hostname) ==="

# ── 0) 量具有效性对照臂（非法实验库，只有 fresh/live 有意义）────────────────
mkdir -p "$ROOT/scenarios/FORCE"
for scen in fresh live; do
  log=$ROOT/scenarios/FORCE/$scen.log
  echo "=== [$(date '+%H:%M:%S')] FORCE $scen" | tee -a "$ROOT/scenarios/FORCE/driver.log"
  env LD_LIBRARY_PATH="$FORCE_LIB" T06_LOG_DIR="$ROOT/scenarios/FORCE" \
      "$ROOT/probe/t06_scenario_probe" "$scen" "t06force_$scen" 10 2>&1 | tee "$log"
  echo "exit=${PIPESTATUS[0]}" >> "$log"
done
{
  echo "arm            = FORCE"
  echo "lib_real_path  = $FORCE_LIB/libipc.so.1.3.0"
  echo "lib_sha256     = $(sha256sum "$FORCE_LIB/libipc.so.1.3.0" | awk '{print $1}')"
  echo "probe_sha256   = $(sha256sum "$ROOT/probe/t06_scenario_probe" | awk '{print $1}')"
  echo "official_binary_sha256 = $(sha256sum build/bin/test_chunk_capacity_backpressure | awk '{print $1}')"
  echo "variant_diff   = $ATT/src/ipc.cpp.FORCE (diff vs src/libipc/ipc.cpp 见 src/FORCE.diff)"
  echo "note           = 非法实验库：仅用于证明 fresh/live 判据有牙，⛔ 不是产品臂"
  echo "collect_end    = $(date '+%Y-%m-%d %H:%M:%S %z')"
} > "$ROOT/scenarios/FORCE/fingerprint.txt"

# ── 1) 产品臂与负控臂（同一驱动脚本，只换库目录）────────────────────────────
#     race 是 1200 轮里最贵的一段；FIX/FIX_RB 走 LONG_ROUNDS，负控走 SHORT_ROUNDS
#     （负控的命中是**确定性**的：FIXNEG 首轮即命中，V2NEG 概率命中且有牙已由 T05 记 3/3）。
for arm in FIX:libs/FIX:$LONG_ROUNDS FIX_RB:libs/FIX_RB:$LONG_ROUNDS \
           V2NEG:libs/V2NEG:$SHORT_ROUNDS FIXNEG:libs/FIXNEG:$SHORT_ROUNDS \
           BASELINE:libs/BASELINE:$SHORT_ROUNDS ABLATE:libs/ABLATE:$SHORT_ROUNDS; do
  name=${arm%%:*}; rest=${arm#*:}; libdir=${rest%%:*}; rounds=${rest##*:}
  echo "=== T06 arm $name (race_rounds=$rounds) $(date '+%H:%M:%S') ==="
  bash "$S" "$name" "$ROOT/$libdir" "$ROOT/scenarios" "$rounds" 8
done

echo "=== T06 matrix end $(date '+%F %T %z') ==="

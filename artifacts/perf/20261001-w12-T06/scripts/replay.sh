#!/usr/bin/env bash
# T06 / W12 §8 一键重放（只读产品库/源码；⛔ 不 cmake、⛔ 不改任何产品/工装/文档）。
#
# 用法: bash replay.sh [ROUNDS] [TOPICS]     默认 1200 8
set -u
ROUNDS=${1:-1200}
TOPICS=${2:-8}
ROOT=$PWD/artifacts/perf/20261001-w12-T06
ATT=${T06_ATTEMPT_DIR:-build/T06/13e0fbea-b9d3-419a-92b9-92381b188d6a}

echo "== 期望指纹（必须与本文档记录一致）"
echo "  product_lib  813fab5be886ef6e27d31043f4610fce886fc465fbbfa5e648773de32ba0c52a"
echo "  official_bin 08b803b169852c7e5c7bdcdf91060b3c8dd1199cd63578167ecd19d40818836f"
echo "  probe        f70017d2e8c5519fee28cac14ecc49eb3cda17a44c71b553c449423b75e66b11"
sha256sum build/lib/libipc.so.1.3.0 build/bin/test_chunk_capacity_backpressure \
          "$ROOT/probe/t06_scenario_probe" | sed 's|.*/||'

echo "== 0) FORCE 有牙对照臂（fresh/live 必须红）"
for s in fresh live; do
  env LD_LIBRARY_PATH="$PWD/$ATT/lib" T06_LOG_DIR="$ROOT/scenarios/FORCE" \
      "$ROOT/probe/t06_scenario_probe" "$s" "t06force_$s" 10 > "$ROOT/scenarios/FORCE/replay-$s.log" 2>&1
  echo "  FORCE $s exit=$? reset_lines=$(grep -c 'orphan segment reset' "$ROOT/scenarios/FORCE/replay-$s.log")"
done

echo "== 1) 四场景矩阵（六库 + FORCE），串行"
nohup env T06_ATTEMPT_DIR="$ATT" LONG_ROUNDS="$ROUNDS" SHORT_ROUNDS=300 \
      bash "$ROOT/scripts/run_all.sh" > "$ROOT/logs/replay.matrix.log" 2>&1 &
wait $!
python3 "$ROOT/scripts/summarize_scenarios.py" "$ROOT/scenarios" | column -t -s$'\t' | cut -c1-200

echo "== 2) 官方工装换库矩阵（含跨臂段卫生 + 故意污染对照）"
bash "$ROOT/scripts/run_official_matrix.sh" "$ROOT/official-matrix"
bash "$ROOT/scripts/summarize_official.sh" "$ROOT/official-matrix" | column -t

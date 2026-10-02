#!/usr/bin/env bash
# 在独立目录复制并插入计时点；不修改产品源码，不修改宿主 RouDi。
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
work="${1:-/var/tmp/cppipc-breakdown-20261002}"
roudi="${ROUDI_BIN:-/home/zwc/branch/lejulab_platform/src/lejusdk/3rd_party/x86_64/iceoryx/bin/iox-roudi}"
python3 -B "$here/instrument.py" "$work"
cmake -S "$here" -B "$work/build"
cmake --build "$work/build" -j8
python3 -B "$here/../transport_comparison/prepare_roudi.py" "$work/roudi.toml"
bash "$here/../transport_comparison/isolated.sh" "$roudi" "$work/roudi.toml" "$work/roudi-smoke.log" \
    python3 -B "$here/run.py" --work "$work" --smoke
bash "$here/../transport_comparison/isolated.sh" "$roudi" "$work/roudi.toml" "$work/roudi-stage.log" \
    python3 -B "$here/run.py" --work "$work"
python3 -B "$here/report.py" --work "$work"
# 复现默认保留原始数据，便于核对和断点续跑；本次交付汇总后清理。

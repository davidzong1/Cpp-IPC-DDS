#!/usr/bin/env bash
# 完整重现入口。依赖 curl/cmake/C++17/Python3/unshare/mount，不需要改动宿主 RouDi。
set -euo pipefail
here="$(cd "$(dirname "$0")" && pwd)"
base="${1:-/var/tmp/cppipc-comparison}"
dds="${DDS_ROOT:-/home/zwc/branch/lejulab_platform/src/lejusdk/3rd_party/x86_64/cyclonedds-0.10.2}"
roudi="${ROUDI_BIN:-/home/zwc/branch/lejulab_platform/src/lejusdk/3rd_party/x86_64/iceoryx/bin/iox-roudi}"
current="$base-current"
scaled="$base-scaled"
deps="$base-scaled-deps"
stock="$base-stock-pool"
python3 -B "$here/test_orchestrator.py"
cmake -S "$here" -B "$current/build" -DDDS_ROOT="$dds"
cmake --build "$current/build" -j8
bash "$here/prepare_scaled_deps.sh" "$deps"
cmake -S "$here" -B "$scaled/build" -DDDS_ROOT="$deps/prefix"
cmake --build "$scaled/build" -j8
python3 "$here/prepare_roudi.py" "$current/roudi.toml"
python3 "$here/prepare_roudi.py" "$scaled/roudi.toml"
run_current() {
  COMPARISON_ROUDI_CONFIG="$current/roudi.toml" bash "$here/isolated.sh" "$roudi" "$current/roudi.toml" "$current/roudi-$1.log" \
    python3 "$here/run.py" "$@" --work "$current" --dds-root "$dds"
}
run_scaled() {
  COMPARISON_ROUDI_CONFIG="$scaled/roudi.toml" bash "$here/isolated.sh" "$deps/prefix/bin/iox-roudi" "$scaled/roudi.toml" "$scaled/roudi-$1.log" \
    python3 "$here/run.py" "$@" --work "$scaled" --dds-root "$deps/prefix"
}
mkdir -p "$stock"
if [[ ! -e "$stock/build" ]]; then ln -s "$current/build" "$stock/build"; fi
python3 "$here/prepare_roudi.py" "$stock/roudi.toml" --original
COMPARISON_ROUDI_CONFIG="$stock/roudi.toml" bash "$here/isolated.sh" "$roudi" "$stock/roudi.toml" "$stock/roudi.log" \
  python3 "$here/run.py" smoke --work "$stock" --dds-root "$dds" --backends dds-iox --sizes 1048576
run_current smoke
run_current speed
run_current stress --backends shm,dds-udp,dds-iox --topics 1,100,1000 --seconds 10
run_scaled smoke --backends dds-udp,dds-iox
run_scaled stress --backends shm,dds-udp,dds-iox --topics 1,100,1000 --seconds 10
python3 -B "$here/audit.py" --current "$current" --scaled "$scaled"
python3 "$here/report.py" --current "$current" --scaled "$scaled" --deps "$deps" --stock "$stock"
# 检查报告完整性后再清理；脚本不默认删除数据，以便中断续跑和复核。

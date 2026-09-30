#!/bin/bash
# 构建 W05 控制面回归探针（证据用，非交付源码；不参与 CMake/CTest）。
# 用法：bash artifacts/w05/probe/build.sh
set -eu
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../../.." && pwd)"
cd "$ROOT"
D="artifacts/w05/probe"
for f in w05_scale w05_ab_counters w05_heartbeat w05_stale_peer w05_fork_gate w05_tick_scale; do
  g++ -std=c++17 -O2 -DNDEBUG -I include -I src "$D/$f.cpp" -o "$D/${f#w05_}" \
      -L build/lib -lipc -lpthread -lrt -Wl,-rpath,"$ROOT/build/lib"
  echo "built $D/${f#w05_}"
done

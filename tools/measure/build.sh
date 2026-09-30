#!/usr/bin/env bash
# W03 测量工装构建脚本（不修改任何共享构建文件，避免多成员同时改 CMakeLists）。
#
# 产物：
#   build/w03/w03_collector        采集器（schema/overhead/clock-check/evidence/selftest）
#   build/w03/w03_thread_workload  已知线程活动的校准负载
#
# 用法：
#   tools/measure/build.sh [OUT_DIR]
# 默认 OUT_DIR=build/w03。依赖：g++ (C++17)、pthread。头文件来自 include/。
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${1:-${ROOT}/build/w03}"
CXX="${CXX:-g++}"

mkdir -p "${OUT}"
INCLUDE="${ROOT}/include"

echo "[w03] root=${ROOT}"
echo "[w03] out=${OUT}"

"${CXX}" -std=c++17 -O2 -pthread -Wall -Wextra -Wno-unused-parameter \
    -I"${INCLUDE}" \
    -o "${OUT}/w03_collector" "${ROOT}/tools/measure/w03_collector.cpp"

"${CXX}" -std=c++17 -O2 -pthread -Wall -Wextra -Wno-unused-parameter \
    -I"${INCLUDE}" \
    -o "${OUT}/w03_thread_workload" "${ROOT}/tools/measure/w03_thread_workload.cpp"

echo "[w03] built:"
ls -l "${OUT}/w03_collector" "${OUT}/w03_thread_workload"

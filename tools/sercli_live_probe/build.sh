#!/usr/bin/env bash
# 编译 sercli_live_probe 的两个工具。**不改任何 CMakeLists**(只读纪律): 直接拿
# 仓库已有的头与 build/lib/libipc.so 编, 因此不会与任何人的 CMake 改动相撞。
#
# 用法:  tools/sercli_live_probe/build.sh
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
OUT="${HERE}/bin"
mkdir -p "${OUT}"

CXX="${CXX:-g++}"
# 与 test/CMakeLists.txt 的 include_directories 保持同一套根; 多加
# exec/dzipc_topic_cat/include 是为了复用产品自己的握手解码器(handshake_probe.h)。
INCS=(-I"${ROOT}/include" -I"${ROOT}/src" -I"${ROOT}/3rdparty" -I"${ROOT}" -I"${ROOT}/exec/dzipc_topic_cat/include")
FLAGS=(-std=c++17 -O2 -DNDEBUG -DUNICODE -D_UNICODE -pthread -Wall -Wextra)

LIBDIR="${ROOT}/build/lib"
if [[ ! -e "${LIBDIR}/libipc.so" ]]; then
  echo "ERROR: ${LIBDIR}/libipc.so 不存在 —— 先在 ${ROOT}/build 里构建 libipc" >&2
  exit 1
fi

for t in sercli_live_driver base2_probe; do
  echo "== compiling ${t}"
  "${CXX}" "${FLAGS[@]}" "${INCS[@]}" "${HERE}/${t}.cc" \
      -L"${LIBDIR}" -lipc -Wl,-rpath,"${LIBDIR}" -o "${OUT}/${t}"
done

echo "== built:"
ls -la "${OUT}"

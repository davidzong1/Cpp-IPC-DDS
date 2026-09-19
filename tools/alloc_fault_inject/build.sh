#!/usr/bin/env bash
# 构建 UF-000 分配失败注入工装。**不改任何 CMakeLists**(与 tools/sercli_live_probe 同约定):
# 直接拿仓库已有的头与 build/lib/libipc.so 编, 不会与任何人的 CMake 改动相撞。
#
# 用法:  tools/alloc_fault_inject/build.sh
# 产物:  tools/alloc_fault_inject/bin/{liballoc_fault_inject.so, fi_selftest, fi_positive_control}
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
OUT="${HERE}/bin"
mkdir -p "${OUT}"

CC="${CC:-gcc}"
CXX="${CXX:-g++}"

echo "== [1/3] 拦截器 .so (纯 C, 不掺 C++ 运行时)"
"${CC}" -std=c11 -O2 -g -fPIC -shared -Wall -Wextra \
    "${HERE}/alloc_fault_inject.c" -o "${OUT}/liballoc_fault_inject.so" -ldl

echo "== [2/3] 工装自测 (纯 C, 不需要 libipc)"
# ⛔ -fno-builtin-{malloc,calloc,realloc,free}: 否则 -O2 下 GCC 可能把 calloc 改写成
#    malloc+memset, 自测断言的"第几次档内分配"就不再对应用例写的那一行。
"${CC}" -std=c11 -O2 -g -Wall -Wextra \
    -fno-builtin-malloc -fno-builtin-calloc -fno-builtin-realloc -fno-builtin-free \
    "${HERE}/fi_selftest.c" -o "${OUT}/fi_selftest"

echo "== [3/3] 阳性对照 (需要 build/lib/libipc.so)"
LIBDIR="${ROOT}/build/lib"
if [[ ! -e "${LIBDIR}/libipc.so" ]]; then
    echo "ERROR: ${LIBDIR}/libipc.so 不存在 —— 先在 ${ROOT}/build 里构建 libipc" >&2
    exit 1
fi
"${CXX}" -std=c++17 -O2 -DNDEBUG -DUNICODE -D_UNICODE -pthread -Wall -Wextra \
    -I"${ROOT}/include" "${HERE}/fi_positive_control.cc" \
    -L"${LIBDIR}" -lipc -Wl,-rpath,"${LIBDIR}" -o "${OUT}/fi_positive_control"

echo "== built:"
ls -la "${OUT}"

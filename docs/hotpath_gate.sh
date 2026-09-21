#!/usr/bin/env bash
#
# 热路径改动闸 —— 任何动 libipc 收/发路径的改法, 合入前必须过这里。
#
# 为什么要有它: 2026-09-21 的"跳过写头那一格"(commit 94b01e2 里带进 HEAD, 后由
# aa7e00c 撤掉)**通过了全部 9 个回归二进制**(chunk_hold/lap_safety/uf007/
# pool_exhaust/loan/dzflat_transport/dzflat_rx/dzflat_sercli/uf011), 却把
# **DZIPC 层的高速 pub/sub 打到零投递**:
#
#     payload   含该改法           撤掉后
#     1 MB      no message recv    1259 msg/s
#     64 B      49 msg/s           163396 msg/s
#
# ⇒ 那 9 个二进制覆盖的是 libipc 的**窄用法**, 而 dzIPC 层的 route/分片重组/用户态
#   队列路径一条都没走到。本脚本把那条路径补成硬门。
#
# 用法: bash docs/hotpath_gate.sh [BUILD_DIR]
#   退出码 0 = 过闸; 非 0 = 有一项没过, 逐项读数见 /tmp/hotpath_gate/
set -uo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="${1:-${REPO_DIR}/build}"
export LD_LIBRARY_PATH="${BUILD_DIR}/lib:${BUILD_DIR}/bin"
OUT=/tmp/hotpath_gate
mkdir -p "$OUT"
fail=0

# 池段是全机共享的: 只清**本脚本自己会用到的档位**, 且先确认没有活进程映射它。
# ⛔ 不用 /dev/shm 通配 —— 同机可能有别人的档位在用。
clear_pool() {
    local seg
    for seg in /dev/shm/*CHUNK_INFO__*; do
        [ -e "$seg" ] || continue
        grep -lq "$(basename "$seg")" /proc/[0-9]*/maps 2>/dev/null || rm -f "$seg"
    done
}

echo "==> 构建"
cmake --build "$BUILD_DIR" --target ipc dzipc_perf_benchmark \
      test_uf011_chunk_return test_chunk_hold test_lap_safety \
      test_uf007_id_pool_double_release test_loan \
      test_dzflat_transport test_dzflat_rx test_dzflat_sercli -j"$(nproc)" \
      >"$OUT/build.log" 2>&1 || { echo "构建失败, 见 $OUT/build.log"; exit 2; }

echo "==> 门 1: DZIPC 层高速 pub/sub 吞吐(A′ 就是死在这里)"
# ⛔ 判据不能只看"有没有报 FAILED" —— A′ 在 64B 档**没有报错**: 它只是把速率从
# 169741 打到 **29 msg/s**。只查错误串的判据会把这一行打上 ✓ 然后放行(实测过)。
# 所以必须带**吞吐下限**。下限取基线的一半 —— 留足机器负载差异的余量, 而 A′ 那种
# 量级(低 4 个数量级)离它十万八千里。
# 基线(2026-09-21, aa7e00c 干净树, 本机, pin=5,6):
#     1 MB → 1534 msg/s ; 64 B → 169741 msg/s
FLOOR_1M=700
FLOOR_64=80000
for p in 1048576 64; do
    clear_pool
    f="$OUT/tput_$p.txt"
    timeout 200 "${BUILD_DIR}/bin/dzipc_perf_benchmark" --cases=pubsub_shm \
        --payloads="$p" --duration=5 --pin=5,6 --out="$OUT/perf_$p" >"$f" 2>&1
    line="$(grep -E "${p}B_tput" "$f" | head -1)"
    rate="$(printf '%s' "$line" | grep -oE '[0-9]+ msg/s' | head -1 | grep -oE '[0-9]+')"
    floor=700; [ "$p" = "64" ] && floor=$FLOOR_64
    if printf '%s' "$line" | grep -qiE "FAILED|no message"; then
        printf '  ✗ payload=%-8s 未投递: %s\n' "$p" "$(printf '%s' "$line" | sed 's/^ *//')"; fail=1
    elif [ -z "$rate" ] || [ "$rate" -lt "$floor" ]; then
        printf '  ✗ payload=%-8s 速率 %s msg/s < 下限 %d —— 没报错但被打塌\n' \
               "$p" "${rate:-NA}" "$floor"; fail=1
    else
        printf '  ✓ payload=%-8s %s\n' "$p" "$(printf '%s' "$line" | sed 's/^ *//')"
    fi
done

echo "==> 门 2: 邻接回归"
for t in test_uf011_chunk_return test_chunk_hold test_lap_safety \
         test_uf007_id_pool_double_release test_loan \
         test_dzflat_transport test_dzflat_rx test_dzflat_sercli; do
    clear_pool
    f="$OUT/$t.txt"
    timeout 600 "${BUILD_DIR}/bin/$t" >"$f" 2>&1
    rc=$?
    ok=$(grep -cE '^\[       OK \]' "$f")
    bad=$(grep -cE '^\[  FAILED  \]' "$f")
    if [ "$rc" -ne 0 ] || [ "$bad" -ne 0 ]; then
        printf '  ✗ %-38s rc=%d OK=%d FAILED=%d\n' "$t" "$rc" "$ok" "$bad"; fail=1
    else
        printf '  ✓ %-38s OK=%d\n' "$t" "$ok"
    fi
done

echo
if [ "$fail" -eq 0 ]; then echo "过闸。"; else echo "⛔ 未过闸 —— 逐项读数在 $OUT/"; fi
exit "$fail"

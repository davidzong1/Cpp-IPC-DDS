#!/bin/bash
# t43 复验驱动：在**当前库**上重跑 t35 的五臂（A=send 腿 / B=loan 腿 / C=队列淘汰 /
# C2=跨二进制淘汰 / D=wait_set_full / E2=wait_token_invalid），并把 BEFORE/AFTER 与
# first_failed_resource 逐臂落盘。
# 用法: run_all_arms.sh <outdir> <tag>
set -u
OUT="$1"; TAG="$2"
B="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)/bin"
mkdir -p "$OUT"
# 前置：把**成对指纹**写进 OUT（R0-A1）；并断言探针实际加载的是 build/lib 的库
{
  echo "# t43 复验前置（$(date '+%Y-%m-%d %H:%M:%S %z')）"
  echo "libipc.so.1.3.0 $(sha256sum build/lib/libipc.so.1.3.0 | cut -d' ' -f1)"
  for p in f1_wiring_probe armD_waitset_full armE2_token_invalid; do
    echo "$p $(sha256sum "$B/$p" | cut -d' ' -f1)"
    ldd "$B/$p" | grep -i libipc | sed "s/^/  ldd[$p]: /"
  done
  echo "counters.h $(sha256sum include/dzIPC/measure/counters.h | cut -d' ' -f1)"
  echo "ipc.cpp    $(sha256sum src/libipc/ipc.cpp | cut -d' ' -f1)"
  echo "circularqueue.h $(sha256sum include/dzIPC/common/circularqueue.h | cut -d' ' -f1)"
} > "$OUT/fingerprint.txt"
for arm in A B C C2 D E2; do
  # 每臂前清 /dev/shm（池为全机共享，残留会让"0→非0"的起点不可复现）
  for seg in /dev/shm/*__IPC_SHM__* /dev/shm/*CHUNK_INFO__*; do [ -e "$seg" ] || continue; rm -f "$seg" 2>/dev/null; done
  case $arm in
    A)  timeout 200 "$B/f1_wiring_probe" A  > "$OUT/${TAG}_armA.log" 2>&1 ;;
    B)  timeout 200 "$B/f1_wiring_probe" B  > "$OUT/${TAG}_armB.log" 2>&1 ;;
    C)  timeout 200 "$B/f1_wiring_probe" C  > "$OUT/${TAG}_armC.log" 2>&1 ;;
    C2) timeout 200 "$B/f1_wiring_probe" C2 > "$OUT/${TAG}_armC2.log" 2>&1 ;;
    D)  DZIPC_SHM_RECV_WORKERS=1 timeout 300 "$B/armD_waitset_full" 140 > "$OUT/${TAG}_armD.log" 2>&1 ;;
    E2) timeout 200 "$B/armE2_token_invalid" state > "$OUT/${TAG}_armE2.log" 2>&1 ;;
  esac
  rc=$?
  echo "$arm rc=$rc" | tee -a "$OUT/${TAG}_arms_rc.txt"
  grep -E "BEFORE-|AFTER-|ArmA |ArmB |ArmC |ArmD |ArmE2 |first_failed_resource|ARMS" "$OUT/${TAG}_arm${arm}.log" 2>/dev/null \
    | sed "s/^/  [$arm] /" | tee -a "$OUT/${TAG}_summary.txt"
done
echo "ARMS_DONE tag=$TAG"

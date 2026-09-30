#!/usr/bin/env bash
# t41 机械核对：t19 两个分类器的调用点是否在位、是否只新增计数器调用点。
# 用法: bash artifacts/perf/20260929-r41-W10-R2b/check_wiring.sh
set -u
ROOT=/home/zwc/cpp_ipc_dds; cd "$ROOT"
fail=0
ck() { if [ "$2" -ge "$3" ]; then echo "  ✓ $1 = $2 (期望 ≥$3)"; else echo "  ✗ $1 = $2 (期望 ≥$3)"; fail=1; fi; }

echo "[1] 调用点落数"
ck "shm_pub_sub_ipc.cc  note_dzflat_attempt"      "$(grep -c 'note_dzflat_attempt' src/dzIPC/shm_pub_sub_ipc.cc)" 3
ck "shm_pub_sub_ipc.h   note_dzflat_borrow_failed" "$(grep -c 'note_dzflat_borrow_failed' include/dzIPC/shm_pub_sub_ipc.h)" 3

echo "[2] 11 个新激活 ID 全部可达（分类器为唯一写入者）"
for id in path_selection_dzflat_disabled path_selection_type_unsupported fallback_reason_unknown \
          fallback_total fallback_capacity_full fallback_oversized fallback_type_incompatible \
          borrow_failed_no_receiver borrow_failed_oversized borrow_failed_publish \
          borrow_failed_reason_unknown; do
  n=$(grep -c "CounterId::$id" include/dzIPC/measure/counters.h)
  if [ "$n" -ge 2 ]; then echo "  ✓ $id (counters.h 内 $n 处)"; else echo "  ✗ $id 不存在"; fail=1; fi
done

echo "[3] 新增 ID 未越界（t41 不动 counters.h）"
h=$(sha256sum include/dzIPC/measure/counters.h | cut -d' ' -f1)
[ "$h" = "1f9c2bd0b9419aea1332c9c295e032ef8e5889e40bf48ebdbf79d8659530b6ae" ] \
  && echo "  ✓ counters.h sha256 与 t35 记录一致（零改动）" || { echo "  ✗ counters.h 被改动: $h"; fail=1; }

echo "[4] t35 家族不被重复计数（调用点不含 chunk_exhausted/chunk_alloc_failed/queue_evicted）"
for id in chunk_exhausted chunk_alloc_failed queue_evicted; do
  n=$(grep -c "$id" src/dzIPC/shm_pub_sub_ipc.cc include/dzIPC/shm_pub_sub_ipc.h | awk -F: '{s+=$2} END{print s}')
  [ "$n" = "0" ] && echo "  ✓ $id 在本次改动面为 0 处（唯一写入点仍在 ipc.cpp/circularqueue.h）" \
                 || { echo "  ✗ $id 出现 $n 处，疑重复计数"; fail=1; }
done

echo "[5] B 借样失败绝不进 fallback_total（分类器内静态核对）"
python3 - <<'PY'
import re,sys
h=open('include/dzIPC/measure/counters.h').read()
i=h.index('inline void note_dzflat_borrow_failed'); j=h.index('\n}\n', i)
body=h[i:j]
bad = ('fallback_total' in body)
print("  ✗ borrow_failed 落点触碰 fallback_total" if bad else "  ✓ borrow_failed 落点不含 fallback_total")
sys.exit(1 if bad else 0)
PY
[ $? -ne 0 ] && fail=1

echo
[ "$fail" = "0" ] && echo "CHECK_WIRING: PASS" || echo "CHECK_WIRING: FAIL"
exit $fail

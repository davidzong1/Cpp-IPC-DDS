#!/bin/bash
# t6 baseline (阶段1-4 回归判据) battery
set -u
out="$1"; mkdir -p "$out"
cd /home/zwc/cpp_ipc_dds
unset DZIPC_SOCKET_COMPAT_THREAD DZIPC_SOCKET_RECV_WORKERS
: > "$out/SUMMARY.txt"
run() { tag="$1"; shift; b="$1"; shift
  timeout 900 "build/bin/$b" "$@" > "$out/$tag.log" 2>&1; rc=$?
  ok=$(grep -c '\[       OK \]' "$out/$tag.log" 2>/dev/null)
  bad=$(grep -c '\[  FAILED  \]' "$out/$tag.log" 2>/dev/null)
  printf '%-30s rc=%-3s ok=%-4s failed=%s\n' "$tag" "$rc" "$ok" "$bad" | tee -a "$out/SUMMARY.txt"
}
run wakeup_artifact      test_wakeup_artifact
run shm_i5_pop_buffer    test_shm_i5_pop_buffer
run adopt_loan_quota     test_adopt_loan_quota
run shm_nodelet          test_shm_nodelet
run nodelet_switch       test_nodelet_switch
run shm_sub_dtor_gate    test_shm_sub_dtor_gate
run shm_receiver_cap     test_shm_receiver_cap
run shm_domain_isolation test_shm_domain_isolation
echo "BASELINE_BATTERY_DONE"

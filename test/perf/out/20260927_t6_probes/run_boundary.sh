#!/bin/bash
set -u
cd /home/zwc/cpp_ipc_dds
D=.t6_probe/logs/boundary; mkdir -p $D
unset DZIPC_SOCKET_COMPAT_THREAD DZIPC_SOCKET_RECV_WORKERS
: > $D/SUMMARY.txt
run() { tag=$1; shift; b="$1"; shift
  timeout 900 "build/bin/$b" "$@" > "$D/$tag.log" 2>&1; rc=$?
  ok=$(grep -c '\[       OK \]' "$D/$tag.log" 2>/dev/null)
  bad=$(grep -c '\[  FAILED  \]' "$D/$tag.log" 2>/dev/null)
  printf '%-32s rc=%-3s ok=%-4s failed=%s\n' "$tag" "$rc" "$ok" "$bad" | tee -a $D/SUMMARY.txt
}
run larger_data_shm_1MiB   test_dzipc_larger_data_shm
run dzipc_shm_full         test_dzipc_shm
run uf009_graceful_exit    test_uf009_graceful_exit
run uf004_shutdown_optout  test_uf004_shutdown_monitor_optout
run dzflat_rx              test_dzflat_rx
run wire_accept            test_wire_accept

echo "=== baseline lib (ABI-valid baseline_probe) scale comparison ==="
B=build_baseline/bin/baseline_probe
for cfg in "1 151" "100 152" "477 153"; do
  set -- $cfg; sc=$1; dm=$2
  timeout 300 env -u DZIPC_SOCKET_COMPAT_THREAD -u DZIPC_SOCKET_RECV_WORKERS $B \
    --mode=socket_pub_sub --scale=$sc --send-msgs=4 --warmup=0 --idle-ms=400 \
    --domain=$dm --prefix=bl$sc --out=$D/base_$sc.json > $D/base_$sc.log 2>&1
  rc=$?
  echo "--- baseline scale=$sc rc=$rc ---"
  grep -E '^(threads_max|threads_min|threads_avg|threads_per_sub|ctx_vol|ctx_nonvol|cpu_cores|received_sub0)=' $D/base_$sc.log
  tail -2 $D/base_$sc.log | grep -v '^$'
done
echo "BOUNDARY_DONE"

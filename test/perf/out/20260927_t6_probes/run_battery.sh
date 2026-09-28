#!/bin/bash
# t6 regression battery. usage: run_battery.sh <worker|compat> <outdir>
set -u
mode="$1"; out="$2"
mkdir -p "$out"
cd /home/zwc/cpp_ipc_dds
BIN=build/bin
unset DZIPC_SOCKET_COMPAT_THREAD DZIPC_SOCKET_RECV_WORKERS
if [ "$mode" = compat ]; then export DZIPC_SOCKET_COMPAT_THREAD=1; fi

: > "$out/SUMMARY.txt"
run() {
  tag="$1"; shift
  b="$1"; shift
  timeout 900 "$BIN/$b" "$@" > "$out/$tag.log" 2>&1
  rc=$?
  ok=$(grep -c '\[       OK \]' "$out/$tag.log" 2>/dev/null || echo 0)
  bad=$(grep -c '\[  FAILED  \]' "$out/$tag.log" 2>/dev/null || echo 0)
  printf '%-34s rc=%-3s ok=%-4s failed=%s\n' "$tag" "$rc" "$ok" "$bad" | tee -a "$out/SUMMARY.txt"
}

if [ "$mode" = worker ]; then
  run shm_rr                test_dzipc_shm --gtest_filter=DzIpcShm.RequestResponse
  run shm_sercli_auto_path  test_sercli_auto_path
  run shm_ser_cli_nodelet   test_shm_ser_cli_nodelet
  run shm_ready_transition  test_shm_ready_transition
  run shm_uf003_crash       test_uf003_crash_reclaim
fi

run socket_dzipc_socket      test_dzipc_socket
run socket_dzipc_pub         test_dzipc_pub
run socket_socket            test_socket
run socket_nodelet           test_socket_nodelet
run socket_borrow            test_socket_borrow
run socket_endpoint_split    test_socket_endpoint_split
run socket_only_transport    test_socket_only_transport
run socket_reliable_crc      test_socket_reliable_crc
run socket_topic_isolation   test_socket_topic_isolation
run sercli_dzipc             test_dzipc
run sercli_dzipc_larger      test_dzipc_larger_data

if [ "$mode" = worker ]; then
  run shared_recv_worker        test_recv_worker
  run shared_socket_wait_set    test_socket_wait_set
  run shared_recv_wait_set      test_recv_wait_set
  run shared_control_scheduler  test_shm_control_scheduler
  run shared_route_session      test_shm_route_session
fi
echo "BATTERY_DONE mode=$mode"

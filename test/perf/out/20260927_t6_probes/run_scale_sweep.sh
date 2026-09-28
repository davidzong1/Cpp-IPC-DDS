#!/bin/bash
set -u
cd /home/zwc/cpp_ipc_dds
D=.t6_probe/logs/scale; mkdir -p $D
P=./.t6_probe/t6scale
run() { tag=$1; mode=$2; shift 2
  if [ "$mode" = compat ]; then export DZIPC_SOCKET_COMPAT_THREAD=1; else unset DZIPC_SOCKET_COMPAT_THREAD; fi
  unset DZIPC_SOCKET_RECV_WORKERS
  timeout 300 $P "$@" > $D/$tag.log 2>&1
  rc=$?
  echo "--- $tag (rc=$rc) ---"
  grep -E '^(scale|threads_max|threads_min|threads_avg|threads_per_sub|ctx_vol|ctx_nonvol|cpu_cores|max_socket_fd|fds_open|sent|received_sub0|idle_window_s)=' $D/$tag.log
}
run worker_1    worker --scale=1    --idle-ms=2000 --sample-ms=200 --domain=120
run worker_100  worker --scale=100  --idle-ms=2000 --sample-ms=200 --domain=121
run worker_477  worker --scale=477  --idle-ms=2000 --sample-ms=200 --domain=122
run worker_1000 worker --scale=1000 --idle-ms=2000 --sample-ms=200 --domain=123
run worker_100_pub worker --scale=100 --idle-ms=2000 --sample-ms=200 --send-msgs=20 --domain=124
run compat_1    compat --scale=1    --idle-ms=2000 --sample-ms=200 --domain=125
run compat_100  compat --scale=100  --idle-ms=2000 --sample-ms=200 --domain=126
run compat_477  compat --scale=477  --idle-ms=2000 --sample-ms=200 --domain=127
run compat_1000 compat --scale=1000 --idle-ms=2000 --sample-ms=200 --domain=128
echo "SCALE_SWEEP_DONE"

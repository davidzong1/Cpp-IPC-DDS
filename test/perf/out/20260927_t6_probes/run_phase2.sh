#!/bin/bash
set -u
cd /home/zwc/cpp_ipc_dds
./.t6_probe/run_battery.sh compat /home/zwc/cpp_ipc_dds/.t6_probe/logs/compat
./.t6_probe/run_baseline_battery.sh /home/zwc/cpp_ipc_dds/.t6_probe/logs/baseline
echo PHASE2_DONE

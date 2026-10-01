#!/bin/bash
# T02 §4.3 复算命令（按方案 §4.3 顺序；可逐条粘贴复跑）
set -u
cd /home/zwc/cpp_ipc_dds
A=artifacts/perf/20261001-w12-T02
RUN=build/T02/43646d77-ba75-4eb1-b8f3-5cdf14bbdb56

# ---- §4.3.1 机械检查发布侧入口（本机无 ripgrep：run 内 shim 用 grep -rnE 等价子集）
# ⛔ 本机未安装 ripgrep：`rg` 直接执行会 exit 127（command not found）。
which rg || echo 'rg NOT INSTALLED (exit 127)'
grep -rnE 'has_peers|on_pub_stale_scan|pub_control_tick' include/dzIPC src/dzIPC   # 等价子集
$RUN/bin/rg -n 'has_peers|on_pub_stale_scan|pub_control_tick' include/dzIPC src/dzIPC  # 同语义 shim（输出已落 logs/mech_rg_publish_side.txt）

# ---- §4.3.2 节拍状态转移四种情形
$RUN/bin/w05_tick_transition   # 期望 tick_transition_fails=0 / exit 0

# ---- §4.3.3 双臂各 3 次（常驻判据）
for i in 1 2 3; do ./build/bin/test_w05_stale_slot_gate;                 done   # 默认臂
for i in 1 2 3; do ./build/bin/test_w05_stale_slot_gate_arm;             done   # 双臂
for i in 1 2 3; do DZIPC_SHM_CONTROL_SCHEDULER=1 ./build/bin/test_w05_stale_slot_gate; done   # L1 回退臂

# ---- §4.3.3' 400 条端到端（双臂各 3 次）
for i in 1 2 3; do $RUN/bin/w05_harm400 $((9000+i*10)) 400 4500;              done   # 默认臂
for i in 1 2 3; do DZIPC_SHM_CONTROL_SCHEDULER=1 $RUN/bin/w05_harm400 $((9100+i*10)) 400 4500; done   # L1 回退臂

# ---- §4.3.4 反向消融矩阵（新树；⛔ 不就地覆写历史变体树）
cmake -S $RUN/ablate-branch-removed   -B $RUN/ablate-branch-removed/b   -DCMAKE_BUILD_TYPE=Release -DLIBIPC_BUILD_PYTHON=OFF -DUPDATA_MSG_SRV_GENERATOR=OFF -DLIBIPC_BUILD_TESTS=ON
cmake -S $RUN/ablate-driver1-removed  -B $RUN/ablate-driver1-removed/b  -DCMAKE_BUILD_TYPE=Release -DLIBIPC_BUILD_PYTHON=OFF -DUPDATA_MSG_SRV_GENERATOR=OFF -DLIBIPC_BUILD_TESTS=ON
cmake -S $RUN/ablate-driver2-removed  -B $RUN/ablate-driver2-removed/b  -DCMAKE_BUILD_TYPE=Release -DLIBIPC_BUILD_PYTHON=OFF -DUPDATA_MSG_SRV_GENERATOR=OFF -DLIBIPC_BUILD_TESTS=ON
cmake --build $RUN/ablate-branch-removed/b  --target test_w05_stale_slot_gate test_w05_stale_slot_gate_arm -j16
cmake --build $RUN/ablate-driver1-removed/b --target test_w05_stale_slot_gate test_w05_stale_slot_gate_arm -j16
cmake --build $RUN/ablate-driver2-removed/b --target test_w05_stale_slot_gate test_w05_stale_slot_gate_arm -j16
$RUN/ablate-branch-removed/b/bin/test_w05_stale_slot_gate_arm   # 期望 2 FAILED
$RUN/ablate-driver1-removed/b/bin/test_w05_stale_slot_gate_arm  # 期望 默认臂 FAILED / L1 PASSED
$RUN/ablate-driver2-removed/b/bin/test_w05_stale_slot_gate_arm  # 期望 默认臂 PASSED / L1 FAILED

# ---- §4.3.5 ABI
g++ -std=c++17 -O2 -I $RUN/baseline-prefix-dcaa0d9/include -I $RUN/baseline-prefix-dcaa0d9/src -I 3rdparty $RUN/src/w05_abi_size_probe.cpp -o $RUN/bin/abi_size_prefix
g++ -std=c++17 -O2 -I include -I src -I 3rdparty $RUN/src/w05_abi_size_probe.cpp -o $RUN/bin/abi_size_post
$RUN/bin/abi_size_prefix; $RUN/bin/abi_size_post   # 两者 sizeof(PubControlState) 均须 = 8
nm -DC $RUN/baseline-prefix-dcaa0d9/b/lib/libipc.so.1.3.0 | awk '{print $NF}' | sort -u > /tmp/nm_pre.txt
nm -DC build/lib/libipc.so.1.3.0                      | awk '{print $NF}' | sort -u > /tmp/nm_post.txt
diff /tmp/nm_pre.txt /tmp/nm_post.txt && echo SYMBOL_NAME_SET_IDENTICAL
nm -DC build/lib/libipc.so.1.3.0 | grep -c pub_control_tick      # 必须为 0（未成为导出符号）

# ---- fingerprint
bash $RUN/collect_fingerprint.sh

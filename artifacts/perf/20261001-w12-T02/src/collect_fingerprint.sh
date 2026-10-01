#!/bin/bash
# T02 fingerprint 采集脚本（只读取数；⛔ 不修改任何源文件、不覆盖任何既有 run）
# 运行方式: bash build/T02/<attempt>/collect_fingerprint.sh
# 产物: artifacts/perf/20261001-w12-T02/fingerprint.txt
set -u
WS=/home/zwc/cpp_ipc_dds
RUN=$WS/build/T02/43646d77-ba75-4eb1-b8f3-5cdf14bbdb56
OUT=$WS/artifacts/perf/20261001-w12-T02/fingerprint.txt
mkdir -p "$(dirname "$OUT")"

sha() { sha256sum "$1" 2>/dev/null | awk '{print $1}'; }
sz() { stat -c '%s' "$1" 2>/dev/null; }

collect_start=$(date -Iseconds)
{
echo "# T02 fingerprint —— W05 产品语义收敛与双臂/消融/ABI 证据"
echo "collect_start=$collect_start"
echo "collect_epoch_start=$(date +%s)"
echo "host=$(hostname)"
echo "kernel=$(uname -sr)"
echo "nproc=$(nproc)"
echo "cpu_model=$(grep -m1 'model name' /proc/cpuinfo | cut -d: -f2- | sed 's/^ //')"
echo "mem_total=$(grep MemTotal /proc/meminfo | awk '{print $2" "$3}')"
echo "pwd=$WS"
echo
echo "## git"
echo "git_head=$(git -C "$WS" rev-parse HEAD)"
echo "git_head_short=$(git -C "$WS" rev-parse --short HEAD)"
echo "git_branch=$(git -C "$WS" rev-parse --abbrev-ref HEAD)"
echo "git_head_subject=$(git -C "$WS" log -1 --pretty=%s)"
echo "git_head_date=$(git -C "$WS" log -1 --pretty=%cI)"
echo "plan_baseline_commit=e800ccc496ac710b711c9346709e86a148c41241"
echo "ab_prefix_commit=dcaa0d9 (方案头部声明的“当前参考 HEAD”，用作 ABI 修复前对照)"
echo "merge_base_baseline_head=$(git -C "$WS" merge-base e800ccc496ac710b711c9346709e86a148c41241 HEAD)"
echo
echo "## §4.2 目标源文件（工作区）"
for f in include/dzIPC/threepools/shm_control_scheduler.h \
         src/dzIPC/threepools/shm_control_scheduler.cc \
         src/dzIPC/shm_pub_sub_ipc.cc \
         include/dzIPC/shm_pub_sub_ipc.h \
         test/test_w05_stale_slot_gate.cpp \
         test/test_w05_stale_slot_gate_arm.cpp ; do
  echo "src_sha256 $f $(sha "$WS/$f")  bytes=$(sz "$WS/$f")  mtime=$(stat -c '%y' "$WS/$f")"
done
echo
echo "## §4.2 目标源文件 vs git blob（证明工作区 == HEAD 提交内容）"
for f in include/dzIPC/threepools/shm_control_scheduler.h \
         src/dzIPC/threepools/shm_control_scheduler.cc \
         src/dzIPC/shm_pub_sub_ipc.cc \
         include/dzIPC/shm_pub_sub_ipc.h \
         test/test_w05_stale_slot_gate.cpp \
         test/test_w05_stale_slot_gate_arm.cpp ; do
  w=$(sha "$WS/$f"); g=$(git -C "$WS" show "HEAD:$f" | sha256sum | awk '{print $1}')
  [ "$w" = "$g" ] && v=SAME || v=DIFF
  echo "blob_cmp $f $v worktree=$(echo "$w" | cut -c1-16) head=$(echo "$g" | cut -c1-16)"
done
echo
echo "## 产品库指纹（修复后）"
echo "lib_realpath=$(readlink -f "$WS/build/lib/libipc.so")"
echo "lib_sha256=$(sha "$(readlink -f "$WS/build/lib/libipc.so")")"
echo "lib_bytes=$(sz "$(readlink -f "$WS/build/lib/libipc.so")")"
echo "lib_mtime=$(stat -c '%y' "$(readlink -f "$WS/build/lib/libipc.so")")"
echo "lib_link_libipc.so=$(readlink "$WS/build/lib/libipc.so")"
echo "lib_link_libipc.so.3=$(readlink "$WS/build/lib/libipc.so.3")"
echo
echo "## 工装指纹（双臂判据 + 消融树 + 探针）"
for b in build/bin/test_w05_stale_slot_gate \
         build/bin/test_w05_stale_slot_gate_arm \
         build/bin/test_shm_control_scheduler \
         build/bin/test_chunk_capacity_backpressure ; do
  echo "tool_sha256 $b $(sha "$WS/$b")  bytes=$(sz "$WS/$b")  mtime=$(stat -c '%y' "$WS/$b")"
done
echo "tool_sha256 build/T02/.../bin/w05_harm400 $(sha "$RUN/bin/w05_harm400")  bytes=$(sz "$RUN/bin/w05_harm400")"
echo "tool_sha256 build/T02/.../bin/w05_tick_transition $(sha "$RUN/bin/w05_tick_transition")  bytes=$(sz "$RUN/bin/w05_tick_transition")"
echo "tool_sha256 build/T02/.../bin/abi_size_prefix $(sha "$RUN/bin/abi_size_prefix")  bytes=$(sz "$RUN/bin/abi_size_prefix")"
echo "tool_sha256 build/T02/.../bin/abi_size_post $(sha "$RUN/bin/abi_size_post")  bytes=$(sz "$RUN/bin/abi_size_post")"
echo "tool_sha256 build/T02/.../bin/w05_clean_baseline $(sha "$RUN/bin/w05_clean_baseline")  bytes=$(sz "$RUN/bin/w05_clean_baseline")"
echo "tool_sha256 build/T02/.../bin/w05_stale_gate_diag $(sha "$RUN/bin/w05_stale_gate_diag")  bytes=$(sz "$RUN/bin/w05_stale_gate_diag")"
echo
echo "## 消融树（新目录；⛔ 未就地覆写任何历史变体树）"
for v in ablate-branch-removed ablate-driver1-removed ablate-driver2-removed baseline-prefix-dcaa0d9 ; do
  d=$RUN/$v
  echo "tree $v header_sha256=$(sha "$d/include/dzIPC/threepools/shm_control_scheduler.h") sched_cc_sha256=$(sha "$d/src/dzIPC/threepools/shm_control_scheduler.cc") pubsub_cc_sha256=$(sha "$d/src/dzIPC/shm_pub_sub_ipc.cc")"
  if [ -f "$d/b/lib/libipc.so.1.3.0" ]; then
    echo "  tree_lib_sha256=$(sha "$d/b/lib/libipc.so.1.3.0") bytes=$(sz "$d/b/lib/libipc.so.1.3.0")"
  fi
  if [ -f "$d/b/bin/test_w05_stale_slot_gate" ]; then
    echo "  tree_gate_sha256=$(sha "$d/b/bin/test_w05_stale_slot_gate") tree_gate_arm_sha256=$(sha "$d/b/bin/test_w05_stale_slot_gate_arm")"
  fi
  echo "  tree_git_status_pristine=$([ -z "$(git -C "$WS" status --porcelain -- "$d" 2>/dev/null)" ] && echo yes || echo 'n/a(untracked)')"
done
echo
echo "## 历史变体树未被触碰的自证（mtime 保持 2026-10-01 15:0x）"
for d in build/t58/w05only build/t58/w05src build/t58/w05offsrc build/t85/head build/t85/cur build/t85/abl_tick build/t85/abl_sched build/t85/abl_compat; do
  echo "untouched $d mtime=$(stat -c '%y' "$WS/$d") lib_mtime=$(stat -c '%y' "$WS/$d/b/lib/libipc.so.1.3.0" 2>/dev/null)"
done
echo
echo "## 构建配置"
echo "cmake_version=$(cmake --version | head -1)"
echo "compiler=$(c++ --version | head -1)"
echo "build_dir=$WS/build (Release / Unix Makefiles)"
echo "build_cache_mtime=$(stat -c '%y' "$WS/build/CMakeCache.txt")"
echo
echo "## 环境变量（关键项）"
echo "DZIPC_SHM_CONTROL_SCHEDULER=${DZIPC_SHM_CONTROL_SCHEDULER-<unset>}"
echo "DZIPC_SOCKET_COMPAT_THREAD=${DZIPC_SOCKET_COMPAT_THREAD-<unset>}"
echo "env_count=$(env | wc -l)"
echo
echo "## §4.3 执行命令（见 commands.sh / README.md）"
echo "cmd1=./build/bin/test_w05_stale_slot_gate                                  (默认臂 gate，3 次)"
echo "cmd2=./build/bin/test_w05_stale_slot_gate_arm                              (双臂 gate，3 次)"
echo "cmd3=DZIPC_SHM_CONTROL_SCHEDULER=1 ./build/bin/test_w05_stale_slot_gate    (L1 回退臂 gate，3 次)"
echo "cmd4=build/T02/.../bin/w05_harm400 <dom> 400 4500                          (400 条端到端，双臂各 3 次)"
echo "cmd5=rg -n \"has_peers|on_pub_stale_scan|pub_control_tick\" include/dzIPC src/dzIPC"
echo "cmd6=build/T02/.../bin/w05_tick_transition                                 (§4.3.2 节拍四分情形)"
echo "cmd7=nm -DC / readelf -S / abi_size_{prefix,post}                          (§4.3.5 ABI)"
collect_end=$(date -Iseconds)
echo
echo "collect_end=$collect_end"
echo "collect_epoch_end=$(date +%s)"
echo "rg_note=本机未安装 ripgrep（which rg 为空, exit 127）；方案 §4.3.1 的同语义命令由 run 内 shim build/T02/<attempt>/bin/rg 执行（grep -rnE 等价子集），输出见 logs/mech_rg_publish_side.txt"
} > "$OUT"
echo "written $OUT"
wc -l "$OUT"

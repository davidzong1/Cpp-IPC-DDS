#!/usr/bin/env bash
# T04 复算脚本（W12 §6 成员三 / W05 独立复核）—— 逐条可粘贴复跑
# ⛔ 不修改任何产品代码；全部产物落在 build/T04/<attempt>/ 与 artifacts/perf/20261001-w12-T04/
set -u
cd /home/zwc/cpp_ipc_dds
A=artifacts/perf/20261001-w12-T04
R=build/T04/84e13e6b-7a26-44d5-80fd-04a1365107d4
FROZEN=$R/wt/b

# ---- §0 冻结提交导出 + 独立配置（f548cd7 = 队长裁定 R-1 冻结点） ----
git archive f548cd7 | tar -x -C $R/frozen
cp -a $R/frozen $R/wt
# f548cd7 未跟踪但构建必需的生成物（含 .gitignore 命中的生成头）：逐文件 sha256 落 $R/logs/supplemental_inputs*.txt
cmake -S $R/wt -B $FROZEN -DCMAKE_BUILD_TYPE=Release -DLIBIPC_BUILD_PYTHON=OFF -DUPDATA_MSG_SRV_GENERATOR=OFF -DLIBIPC_BUILD_TESTS=ON
cmake --build $FROZEN --target test_w05_stale_slot_gate test_w05_stale_slot_gate_arm -j16

# ---- §1 本复核探针（与门控用例独立实现；判据①②的原始输出来源） ----
/usr/bin/c++ $(grep '^CXX_DEFINES' $FROZEN/test/CMakeFiles/test_w05_stale_slot_gate.dir/flags.make | sed 's/^CXX_DEFINES = //') \
  $(grep '^CXX_INCLUDES' $FROZEN/test/CMakeFiles/test_w05_stale_slot_gate.dir/flags.make | sed 's/^CXX_INCLUDES = //') \
  $(grep '^CXX_FLAGS'    $FROZEN/test/CMakeFiles/test_w05_stale_slot_gate.dir/flags.make | sed 's/^CXX_FLAGS = //') \
  -c $A/src/t04_stale_window_probe.cpp -o $R/bin/t04_probe.o
/usr/bin/c++ -O3 -DNDEBUG -O2 $R/bin/t04_probe.o -o $R/bin/t04_probe \
  -Wl,-rpath,$FROZEN/lib $FROZEN/lib/libipc.so.1.3.0 -lpthread -lrt

# ---- §2 验收三条 verify 命令（工作区 T01 冻结工装；原样照抄合同） ----
./build/bin/test_w05_stale_slot_gate                    # 默认臂（判据①②）
./build/bin/test_w05_stale_slot_gate_arm                # 双臂（各判据①②）
DZIPC_SHM_CONTROL_SCHEDULER=1 ./build/bin/test_w05_stale_slot_gate   # L1 回退臂
git status --short include/dzIPC src/dzIPC              # 必须为空 ⇒ 产品代码未改

# ---- §3 独立复核树上的同一判据（判据来源 = 本复核树，⛔ 非 T02 构建树） ----
$FROZEN/bin/test_w05_stale_slot_gate
$FROZEN/bin/test_w05_stale_slot_gate_arm
DZIPC_SHM_CONTROL_SCHEDULER=1 $FROZEN/bin/test_w05_stale_slot_gate

# ---- §4 真实故障窗口 + 400 条端到端（判据①回收耗时、判据② 400/400） ----
$R/bin/t04_probe 7801 400            # 默认臂
DZIPC_SHM_CONTROL_SCHEDULER=1 $R/bin/t04_probe 7811 400   # L1 回退臂
$R/bin/t04_probe 7821 400 nocraft    # 干净对照（无陈旧槽位）

# ---- §5 消融（变体 A = 唯一判据的无 peer 兜底分支删除；变体 B = L1 驱动调用删除） ----
# 源码 diff 见 $A/ablation/*.diff；变红日志见 $A/logs/ablation_*.log
$R/bin/t04_probe_A 7901 400                                  # 期望判据①变红
$R/bin/t04_probe_B 7911 400                                  # 期望默认臂仍绿
DZIPC_SHM_CONTROL_SCHEDULER=1 $R/bin/t04_probe_B 7921 400    # 期望 L1 臂变红

# ---- §6 L1 偶发长跑（独立复现频率；原始日志 flake_*.log，汇总 flake_runs.csv） ----
# for i in $(seq 1 120); do DZIPC_SHM_CONTROL_SCHEDULER=1 $FROZEN/bin/test_w05_stale_slot_gate; done

# ---- §7 指纹（库绑定唯一权威） ----
bash $A/src/collect_fingerprint.sh

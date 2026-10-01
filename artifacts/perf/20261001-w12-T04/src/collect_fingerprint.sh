#!/usr/bin/env bash
# T04（W12 §6 成员三 / W05 独立复核）指纹采集：**库绑定唯一权威 = 本文件**
# 依据：任务合同第 8 条「库绑定只认 run 目录 fingerprint.txt，⛔ 不使用 manifest 生成时字段替代」
set -u
cd /home/zwc/cpp_ipc_dds
A=artifacts/perf/20261001-w12-T04
R=build/T04/84e13e6b-7a26-44d5-80fd-04a1365107d4
OUT=$A/fingerprint.txt
: > "$OUT"
say() { printf '%s\n' "$*" >> "$OUT"; }

say "# T04 W05 独立复核 —— 运行环境与库绑定指纹"
say "# collected_at=$(date -Is)  epoch=$(date +%s)"
say "# host=$(hostname)  nproc=$(nproc)  loadavg=$(cut -d' ' -f1-3 /proc/loadavg)"
say "# HEAD=$(git rev-parse HEAD)  frozen_commit=$(git rev-parse f548cd7)  frozen_tree=$(git rev-parse f548cd7^{tree})"
say "# attempt=84e13e6b-7a26-44d5-80fd-04a1365107d4  run_dir=$R  evidence_dir=$A"
say ""

sha() { sha256sum "$1" 2>/dev/null | awk '{print $1}'; }
line() { # line <label> <path>
    local p="$2"
    if [ -e "$p" ]; then
        printf '%-58s %s  size=%s  mtime=%s  sha256=%s\n' "$1" "$p" "$(stat -c%s "$p")" \
               "$(stat -c%y "$p" | cut -d. -f1)" "$(sha "$p")" >> "$OUT"
    else
        printf '%-58s %s  ABSENT\n' "$1" "$p" >> "$OUT"
    fi
}

say "## 1) 冻结提交独立复核树的构建产物（本复核**判据来源**）"
for f in lib/libipc.so.1.3.0 bin/test_w05_stale_slot_gate bin/test_w05_stale_slot_gate_arm; do line "frozen[$f]" "$R/wt/b/$f"; done
line "frozen[bin/t04_probe]" "$R/bin/t04_probe"
say ""
say "## 2) 解析目标（readlink/ldd：确认运行期真正装载的是本复核树的库）"
say "libipc.so -> $(readlink -f $R/wt/b/lib/libipc.so)"
say "libipc.so.3 -> $(readlink -f $R/wt/b/lib/libipc.so.3)"
say "ldd(frozen gate) : $(ldd $R/wt/b/bin/test_w05_stale_slot_gate | grep -m1 libipc | tr -s ' ')"
say "ldd(frozen arm)  : $(ldd $R/wt/b/bin/test_w05_stale_slot_gate_arm | grep -m1 libipc | tr -s ' ')"
say "ldd(t04_probe)   : $(ldd $R/bin/t04_probe | grep -m1 libipc | tr -s ' ')"
say "RUNPATH(frozen gate) = $(readelf -d $R/wt/b/bin/test_w05_stale_slot_gate | awk '/RUNPATH/{print $NF}')"
say ""
say "## 3) 工作区 build/（T01 冻结登记，**对照用**，非本复核判据来源）"
for f in lib/libipc.so.1.3.0 bin/test_w05_stale_slot_gate bin/test_w05_stale_slot_gate_arm bin/test_shm_control_scheduler; do line "workspace[$f]" "build/$f"; done
say ""
say "## 4) 源码指纹（8 个目标文件：冻结提交 vs 本复核树 vs 工作区）"
for f in include/dzIPC/threepools/shm_control_scheduler.h src/dzIPC/threepools/shm_control_scheduler.cc src/dzIPC/shm_pub_sub_ipc.cc include/dzIPC/shm_pub_sub_ipc.h src/libipc/ipc.cpp test/test_w05_stale_slot_gate.cpp test/test_w05_stale_slot_gate_arm.cpp test/CMakeLists.txt; do
    c=$(git show f548cd7:"$f" 2>/dev/null | sha256sum | awk '{print $1}')
    t=$([ -f "$R/wt/$f" ] && sha "$R/wt/$f" || echo ABSENT)
    w=$([ -f "$f" ] && sha "$f" || echo ABSENT)
    same=$([ "$c" = "$t" ] && echo SAME || echo DIFF)
    printf '%-62s commit=%s run_tree=%s(%s) workspace=%s\n' "$f" "${c:0:16}" "${t:0:16}" "$same" "${w:0:16}" >> "$OUT"
done
say ""
say "## 5) 消融库（变红判据来源）——构建后由 collect_fingerprint.sh 二次采集"
for v in ablate-fallback-branch-removed ablate-l1-driver-removed; do
    for f in lib/libipc.so.1.3.0 b/lib/libipc.so.1.3.0 b/bin/test_w05_stale_slot_gate_arm b/bin/test_w05_stale_slot_gate b/bin/test_w05_stale_slot_gate_unused; do
        [ -e "$R/$v/$f" ] && line "ablate[$v/$f]" "$R/$v/$f"
    done
done
say ""
say "## 5b) 本复核自建探针与工装（逐版本）"
for f in bin/t04_probe bin/t04_probe_A bin/t04_probe_B bin/t04_probe_v2 bin/t04_probe_v3 bin/t04_probe_v4 bin/t04_probe_v5 bin/t04_probe_v6 bin/t04_probe_v7; do
    line "probe[$f]" "$R/$f"
done
say "probe[bin] 与各自绑定的库（ldd）:"
for f in bin/t04_probe bin/t04_probe_A bin/t04_probe_B bin/t04_probe_v2 bin/t04_probe_v6 bin/t04_probe_v7; do
    [ -e "$R/$f" ] && printf '  %-22s -> %s\n' "$f" "$(ldd "$R/$f" | awk '/libipc/{print $3}')" >> "$OUT"
done
say ""
say "## 5c) 逐次读数与汇总文件（sha256；复核报告的每个数字都可追到这些文件）"
for f in flake/gate_runs.csv flake/gate_runs_clean.csv flake/arm_runs_clean.csv flake/probe_runs_clean.csv flake/e400_matrix.csv flake/paired_craft.csv flake/nocraft_runs.csv flake/matrix_runs.csv logs/verify3x_summary.txt logs/verify_workspace_3x.txt logs/ablation_gate.log logs/section_equivalence.txt logs/abi_side_check.txt logs/bdad6093_exists_check.txt; do
    [ -f "$A/$f" ] && line "evidence[$f]" "$A/$f"
done
say ""
say "## 6) 消融树源码 diff 指纹（相对冻结提交；完整 diff 落 $A/ablation/）"
for v in ablate-fallback-branch-removed ablate-l1-driver-removed; do
    for f in include/dzIPC/threepools/shm_control_scheduler.h src/dzIPC/threepools/shm_control_scheduler.cc src/dzIPC/shm_pub_sub_ipc.cc; do
        [ -f "$R/$v/$f" ] || continue
        d=$(diff -q <(git show f548cd7:"$f") "$R/$v/$f" >/dev/null 2>&1 && echo SAME || echo DIFF)
        printf 'ablate[%s] %-60s %s sha256=%s\n' "$v" "$f" "$d" "$(sha "$R/$v/$f")" >> "$OUT"
    done
done
say ""
say "## 7) 历史读数声明"
say "bdad60939e9c75f80eff87c9f669268a09ea9b00e1539240e94a1167eb84a86c = **历史读数**（T58 时点 W05 生效库）"
if find build artifacts -name 'libipc.so.1.3.0' -type f 2>/dev/null | while read -r p; do [ "$(sha "$p")" = "bdad60939e9c75f80eff87c9f669268a09ea9b00e1539240e94a1167eb84a86c" ] && echo FOUND; done | grep -q FOUND; then
    say "search_result=FOUND（存在副本 —— 与 T05 文档 §7.6 的历史结论不符，须复核文档）"
else
    say "search_result=NOT_FOUND（本轮对 build/** 与 artifacts/** 全量遍历：无该 sha256 副本）"
    say "⇒ 本复核**不使用**该库的任何读数；凡引用均为历史读数并显式标注不可复跑（⛔ 不伪造其可用性）"
fi
say ""
say "## 8) 冻结树导出补入清单（f548cd7 未跟踪但构建必需的生成物/输入）"
say "详见 $R/logs/supplemental_inputs.txt 与 supplemental_inputs2.txt（含逐文件 sha256）"
say "working_tree_untracked_inputs=$A/../  : $(wc -l < $R/logs/wt_untracked_inputs.txt) 个 test/ 下未跟踪输入"
say ""
say "## 9) 环境变量（影响驱动路径选择）"
for v in DZIPC_SHM_CONTROL_SCHEDULER DZIPC_VERBOSE LD_PRELOAD; do printf '%-40s = %s\n' "$v" "${!v-<unset>}" >> "$OUT"; done
say "env_total=$(env | wc -l)"
echo "fingerprint written: $OUT ($(wc -l < "$OUT") lines)"

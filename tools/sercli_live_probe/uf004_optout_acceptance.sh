#!/usr/bin/env bash
# uf004_optout_acceptance.sh — UF-004 opt-out 的**预注册验收驱动**(tester-claude)
# ==============================================================================
# 它只做一件事: 把"预先写死的期望"与"实际读数"逐字段比, 打印 PASS/FAIL。
# 期望**先于运行**写在本文件里(下面的期望表), 不是跑完再解释读数 —— 否则就是自证。
#
# 被测对象(锚点逐个登记, 缺一不可):
#   库   libipc.so.3   —— 必须有 DisableShutdownMonitor 符号, 否则**不可判**(不是 PASS)
#   驱动 sercli_live_driver(冻结副本) —— 同一二进制跑全部臂, coder 重建不会让臂漂移
#   探针 uf004_optout_probe —— late / noapp / explicit 三臂驱动**结构性**表达不了
#   变异 libm1_optout_false.so —— LD_PRELOAD 把 opt-out 判定钉死为恒 false
#
# 臂与判据(逐条对应 docs/unfixed_defects.md 0.3.8(2) 的 A0/M2/A1/M1 骨架):
#   A0 默认不变门 : direct 4/4 app_handler_alive+残留0 ; factory 4/4 library_exit0_fast+残留17
#   M2 等价门     : 工具侧新增的 --optout 能力**不传**时, 指纹必须逐字段等于 A0
#   A1 正门       : --optout early ⇒ factory 4/4 翻成 app_handler_alive+残留0, 两端 ret=1
#   M1 变异门     : opt-out 判定恒 false ⇒ A1 **必须转红**(否则 A1 不承重)
#   A1③ 负向门    : 应用**不装**处理器时, opt-out 只解除接管 ⇒ by_signal=15/rc=143 且残留>0
#                   ⛔ 这一条是防"把 opt-out 读成残留已修"的唯一防线, 必须显式断言
#
# 用法: uf004_optout_acceptance.sh [--reuse] [--out DIR] [--expect-red [--expect-red-set FILE]]
#   --reuse: 不重跑矩阵, 直接给已有 arms2/*/matrix.csv 打分(改判据时省机时)
#   --expect-red: 用**变异库/变异 preload**跑时的"预期转红"模式(见下)
#   --expect-red-set FILE: 该支变异**预注册**的红项清单(一行一条); 给了就要求红项集合
#                          与之**逐条相同**(多一条=真缺陷, 少一条=变异未覆盖)
# 退出码: 0=全绿; 1=有 FAIL; 3=不可判(库无符号/锚点缺失); 2=参数错
#         --expect-red 下: 0=如期转红(给了 set 时须与预注册集合一致); 1=意外全绿或集合不符
#
# ⛔ 为什么需要 --expect-red: 变异臂(库判定恒 false)跑出的 verdict 本来就是 FAIL,
#    裸跑日志与"真验收 FAIL"**在文件里长得一模一样** ⇒ 日后必被误引成"UF-004 验收失败"。
#    本模式把"预期红"变成**可机械核对**的断言, 并落盘 EXPECTED_RED.md + 日志首行横幅自证。
#    ⛔ 本模式产出的日志**不是**验收结论, 不得引作 UF-004 的 PASS/FAIL。
# ==============================================================================
set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "${HERE}/../.." && pwd)"
VD="${ROOT}/build/uf004_verify"
OUT="${VD}/arms2"
REUSE=0
EXPECT_RED=0
RED_SET_FILE=""
while [[ $# -gt 0 ]]; do
  case "$1" in
    --reuse) REUSE=1; shift;;
    --out)   OUT="$2"; shift 2;;
    --expect-red) EXPECT_RED=1; shift;;
    --expect-red-set) EXPECT_RED=1; RED_SET_FILE="$2"; shift 2;;
    *) echo "unknown arg $1" >&2; exit 2;;
  esac
done

# 预期转红模式下, 日志首行自证 —— 复制粘贴裸日志的人第一眼就看见这不是验收结论
if [[ ${EXPECT_RED} -eq 1 ]]; then
  echo "################ ⛔ 预期转红(变异臂)运行 —— 本日志不是 UF-004 验收结论 ################"
  echo "#  变异库/变异 preload 下 A1 必须转红; 有效验收见 arms2/(实现库 b7e3c9ca, PASS=28 FAIL=0)"
  echo "####################################################################################"
fi

# ⛔ 库锚点可被 UF004_LIB 覆盖: coder 重建会让树库 md5 漂移, 验收必须能指着**冻结副本**
#    重跑(否则"验过的库"和"树里的库"是两回事, 结论无法回溯)。
LIB="${UF004_LIB:-${VD}/lib_landed_b7e3c9ca/libipc.so.3}"
DRV="${VD}/driver_landed_f295bbb0"
PROBE="${VD}/uf004_optout_probe"
M1="${VD}/libm1_optout_false.so"
SYM="_ZN5dzIPC22DisableShutdownMonitorEv"

PASS=0; FAIL=0
RED_NAMES=""
ok()   { printf '  \033[32mPASS\033[0m %-46s %s\n' "$1" "$2"; PASS=$((PASS+1)); }
bad()  { printf '  \033[31mFAIL\033[0m %-46s %s\n' "$1" "$2"; FAIL=$((FAIL+1)); RED_NAMES="${RED_NAMES}${1}"$'\n'; }
warn() { printf '  \033[33mUNJUDGEABLE\033[0m %s\n' "$1"; }

echo "=== 锚点(md5) ==="
for f in "$LIB" "$DRV" "$PROBE" "$M1" "${HERE}/exit_semantics_matrix.sh"; do
  [[ -e "$f" ]] || { warn "锚点缺失: $f"; exit 3; }
  printf '  %-34s %s\n' "$(basename "$f")" "$(md5sum "$f" | cut -d' ' -f1)"
done
printf '  %-34s %s\n' "acceptance.sh(self)" "$(md5sum "${BASH_SOURCE[0]}" | cut -d' ' -f1)"

# ---- 符号门: 库没落码 ⇒ **不可判**, 绝不静默当通过 -------------------------------
# ⛔ 必须先把 nm 的输出收进变量再 grep: `nm ... | grep -q` 在 set -o pipefail 下,
#    grep 命中即退出会把 nm 打成 SIGPIPE(141) ⇒ 整条管道判失败 ⇒ **门会假阴性**
#    (实测踩过一次: 库明明有符号却被判"未落码")。宁可多一行变量, 不要这个陷阱。
SYMS=$(nm -D --defined-only "${LIB}" 2>/dev/null || true)
if ! grep -q "${SYM}" <<<"${SYMS}"; then
  warn "库无 ${SYM} ⇒ opt-out 未落码, 本次**不可判**(rc=3), 不产出任何 PASS"
  exit 3
fi
ok "symbol_present" "${SYM}"

mkdir -p "${OUT}"

# ---- 矩阵臂: A0 / A1 / M1 ------------------------------------------------------
run_matrix() {   # $1=tag $2...=额外参数
  local tag="$1"; shift
  ( export LD_LIBRARY_PATH="$(dirname "${LIB}")"
    bash "${HERE}/exit_semantics_matrix.sh" --driver "${DRV}" --out "${OUT}/${tag}" \
         --tag "${tag}" "$@" > "${OUT}/${tag}.log" 2>&1 )
}
if [[ ${REUSE} -eq 0 ]]; then
  echo "=== 跑矩阵臂(A0 / A1_early / M1_early) ==="
  run_matrix A0_tool
  run_matrix A1_early --optout early
  run_matrix M1_early --optout early --preload "${M1}"
fi

# 逐行按 (via,role) 比: 期望假设 / SUMMARY / 本进程残留 / opt-out 返回值
rows_bad() {   # $1=csv $2=via $3=role $4=hyp $5=summary $6=residue_own $7=optout_ret
  awk -F, -v via="$2" -v role="$3" -v hyp="$4" -v sum="$5" -v ro="$6" -v oret="$7" '
    NR>1 && $2==via && $3==role {
      n++
      if ($11!=hyp || $8!=sum || $13!=ro || $19!=oret) { bad++; d=d sprintf("r%s:%s/%s/%s/%s ", $1, $11, $8, $13, $19) }
    }
    END { printf "%d %d %s", n+0, bad+0, d }' "$1"
}
grade_via() {  # $1=label $2=csv $3=via $4=hyp $5=summary $6=residue_own $7=oret $8=rounds
  local lbl="$1" csv="$2"
  local r; r=$(rows_bad "$csv" "$3" server "$4" "$5" "$6" "$7")
  local n=${r%% *}; local rest=${r#* }; local b=${rest%% *}; local detail=${rest#* }
  local r2; r2=$(rows_bad "$csv" "$3" client "$4" "$5" "$6" "$7")
  local n2=${r2%% *}; local b2=${r2#* }; b2=${b2%% *}
  if [[ "${n}" == "$8" && "${n2}" == "$8" && "${b}" == "0" && "${b2}" == "0" ]]; then
    ok "${lbl}/${3}" "n=${n}+${n2} 全符合: ${4}/summary=${5}/残留own=${6}/ret=${7}"
  else
    bad "${lbl}/${3}" "不符: server n=${n} bad=${b} client n=${n2} bad=${b2} ${detail}"
  fi
}

echo "=== A0 默认不变门(不传任何 opt-out 参数) ==="
grade_via "A0" "${OUT}/A0_tool/matrix.csv"  direct  app_handler_alive   1 0 na 2
grade_via "A0" "${OUT}/A0_tool/matrix.csv"  factory library_exit0_fast 0 17 na 2

echo "=== A1 正门(--optout early: 应用自制退出, 库放手) ==="
grade_via "A1" "${OUT}/A1_early/matrix.csv" factory app_handler_alive   1 0 1 2
grade_via "A1" "${OUT}/A1_early/matrix.csv" direct  app_handler_alive   1 0 1 2

echo "=== M1 变异门(opt-out 判定恒 false ⇒ A1 必须转红) ==="
grade_via "M1" "${OUT}/M1_early/matrix.csv" factory library_exit0_fast 0 17 0 2

# 残留差集(显式打印: 两臂的段数对比)
# ⛔ 用 `grep -c .`(非空行)而不是 `wc -l`: 矩阵在"空集"时写的是一行空串,
#    wc -l 会数成 1 —— 那就把"零残留"显示成"1 段", 读数本身就错了。
for t in A0_tool A1_early; do
  for r in factory_r1; do
    [[ -f "${OUT}/${t}/${r}/residue_names.txt" ]] || continue
    echo "  [残留集] ${t}/${r}: $(grep -c . "${OUT}/${t}/${r}/residue_names.txt") 段"
  done
done

# ---- 探针臂: early / late / noapp / explicit ------------------------------------
probe_arm() {   # $1=arm $2=preload(可空) -> 打 ARM 行与 SIGCHK 行
  local pre="$2"
  if [[ -n "${pre}" ]]; then
    LD_LIBRARY_PATH="$(dirname "${LIB}")" LD_PRELOAD="${pre}" "${PROBE}" "$1" --hold-ms=6000 --grace-ms=8000 2>&1
  else
    LD_LIBRARY_PATH="$(dirname "${LIB}")" "${PROBE}" "$1" --hold-ms=6000 --grace-ms=8000 2>&1
  fi
}
field() { sed -n "s/.*\b$2=\([^ ]*\).*/\1/p" <<<"$1" | tail -1; }

echo "=== 探针臂(驱动表达不了的三种形态) ==="
P_EARLY=$(probe_arm early "")      ; echo "  early   : $(grep '^ARM ' <<<"${P_EARLY}")"
P_LATE=$(probe_arm late "")        ; echo "  late    : $(grep '^ARM ' <<<"${P_LATE}")"
P_NOAPP=$(probe_arm noapp "")      ; echo "  noapp   : $(grep '^ARM ' <<<"${P_NOAPP}")"
P_EXPL=$(probe_arm explicit "")    ; echo "  explicit: $(grep '^ARM ' <<<"${P_EXPL}")"
P_M1_NOAPP=$(probe_arm noapp "${M1}"); echo "  M1noapp : $(grep '^ARM ' <<<"${P_M1_NOAPP}")"
P_M1_EARLY=$(probe_arm early "${M1}"); echo "  M1early : $(grep '^ARM ' <<<"${P_M1_EARLY}")"

chk() { local lbl="$1" got="$2" want="$3"; [[ "${got}" == "${want}" ]] && ok "${lbl}" "${got}" || bad "${lbl}" "得 ${got} 期望 ${want}"; }

# early: 应用处理器在, 库不接管 ⇒ 收到信号 → SUMMARY → 残留 0
chk "early.rc"        "$(field "$(grep '^ARM ' <<<"${P_EARLY}")" rc)" "0"
chk "early.by_signal" "$(field "$(grep '^ARM ' <<<"${P_EARLY}")" by_signal)" "0"
chk "early.residue"   "$(field "$(grep '^ARM ' <<<"${P_EARLY}")" residue)" "0"
chk "early.optout_ret" "$(field "$(grep 'OPTOUT' <<<"${P_EARLY}")" ret)" "1"
chk "early.post_factory_term" "$(field "$(grep 'phase=post_factory' <<<"${P_EARLY}")" term)" "app"
grep -q '^SERVER SUMMARY' <<<"${P_EARLY}" && ok "early.SUMMARY" "出现" || bad "early.SUMMARY" "缺失"

# late: 太晚 ⇒ ret=0, 库已接管, **行为逐位不变**(无 SUMMARY / 残留 17)
A_LATE=$(grep '^ARM ' <<<"${P_LATE}")
chk "late.optout_ret" "$(field "$(grep 'OPTOUT' <<<"${P_LATE}")" ret)" "0"
chk "late.post_factory_term" "$(field "$(grep 'phase=post_factory' <<<"${P_LATE}")" term)" "other"
chk "late.rc" "$(field "${A_LATE}" rc)" "0"
chk "late.residue" "$(field "${A_LATE}" residue)" "17"
grep -q '^SERVER SUMMARY' <<<"${P_LATE}" && bad "late.SUMMARY" "不该出现(库 std::exit 不会走应用回显)" || ok "late.SUMMARY" "不出现(默认指纹保持)"

# noapp(A1③ 承重): opt-out 已生效但**没有应用处理器** ⇒ 只能被信号杀, 残留照旧
A_NA=$(grep '^ARM ' <<<"${P_NOAPP}")
chk "noapp.rc"        "$(field "${A_NA}" rc)" "143"
chk "noapp.by_signal" "$(field "${A_NA}" by_signal)" "15"
chk "noapp.post_factory_sigcgt" "$(field "$(grep 'phase=post_factory' <<<"${P_NOAPP}")" sigcgt)" "0"
grep -q '^SERVER SUMMARY' <<<"${P_NOAPP}" && bad "noapp.SUMMARY" "不该出现" || ok "noapp.SUMMARY" "不出现"
NR_NA=$(field "${A_NA}" residue)
[[ "${NR_NA}" != "0" ]] && ok "noapp.residue>0(⛔不是残留修复)" "${NR_NA}" || bad "noapp.residue>0" "得 0 —— 若读成'opt-out 修好了残留'即为误报"

# explicit: 公开 API 不回退 —— opt-out 后显式 StartShutdownMonitor() 仍照装
chk "explicit.post_explicit_start_term" "$(field "$(grep 'phase=post_explicit_start' <<<"${P_EXPL}")" term)" "other"
chk "explicit.post_explicit_start_sigcgt" "$(field "$(grep 'phase=post_explicit_start' <<<"${P_EXPL}")" sigcgt)" "1"

# M1 × noapp: 判定恒 false ⇒ 库照装处理器 ⇒ rc 由 143 变 0(负向判据本身随变异翻转)
A_M1NA=$(grep '^ARM ' <<<"${P_M1_NOAPP}")
chk "M1noapp.optout_ret" "$(field "$(grep 'OPTOUT' <<<"${P_M1_NOAPP}")" ret)" "0"
chk "M1noapp.rc" "$(field "${A_M1NA}" rc)" "0"
chk "M1noapp.sigcgt" "$(field "$(grep 'phase=post_factory' <<<"${P_M1_NOAPP}")" sigcgt)" "1"
A_M1E=$(grep '^ARM ' <<<"${P_M1_EARLY}")
chk "M1early.residue" "$(field "${A_M1E}" residue)" "17"

# ---- 预期转红模式: 把"红"变成**可机械核对**的断言 -------------------------------
# ⛔ 两种变异翻红的断言集合**不同**, 所以预注册集合必须是**参数**(SETFILE), 不能写死:
#   变异① 树库 12ae64ca: 返回路径恒 0, 但 disabled 开关**仍置位** ⇒ 行为列全绿,
#          只有"读返回值"的断言变红(A1/factory A1/direct early.optout_ret)。
#   变异② LD_PRELOAD libm1_optout_false.so: 判定恒 false 且**不置位** ⇒ 行为列一起翻,
#          红项集合大得多。
#   ⇒ 谁跑哪支变异, 就必须先给出那支的预注册红集; 没有 SETFILE 时只校验"确实转红了"。
if [[ ${EXPECT_RED} -eq 1 ]]; then
  RED_SORT=$(printf '%s\n' "${RED_NAMES}" | grep . | sort)
  N_RED=$(printf '%s\n' "${RED_SORT}" | grep -c .)
  echo "  [预期转红模式] 红项 ${N_RED} 项: $(printf '%s' "${RED_SORT}" | tr '\n' ' ')"
  printf '%s\n' "${RED_SORT}" > "${OUT}/red_names.txt"
  cat > "${OUT}/EXPECTED_RED.md" <<EOF
# ⛔ 本次运行是「预期转红(变异臂)」——**不是** UF-004 的验收结论

- 库锚点: \`${LIB}\`
- 驱动: \`${DRV}\`   探针: \`${PROBE}\`   变异 preload: \`${M1}\`
- 读数: PASS=${PASS} FAIL=${FAIL} (裸 verdict = $([[ ${FAIL} -eq 0 ]] && echo PASS || echo FAIL))
- 红项明细: 见同目录 \`red_names.txt\`(逐条, 供事后 diff)

## 为什么红是**预期**
本臂用**变异库/变异 preload**(opt-out 判定被钉死), 目的是证明 A1 正门**承重**:
判定一坏, A1 就必须转红。裸日志里的 FAIL 与"真验收 FAIL"长得一样 ⇒ 本文件即标记。

## 引用纪律
⛔ 本目录下 \`acceptance_result.json\` / \`*.log\` **不得**被引作 UF-004 验收证据。
有效验收 = 冻结实现库 \`lib_landed_b7e3c9ca/libipc.so.3\` (\`b7e3c9ca\`) 下的 \`arms2/\`(PASS=28 FAIL=0)。
EOF
  if [[ ${FAIL} -eq 0 ]]; then
    echo "  ⛔ 该跑**全绿** ⇒ 变异没被观测到(A1 不承重或变异未生效), 本臂无效"
    exit 1
  fi
  if [[ -n "${RED_SET_FILE}" ]]; then
    [[ -f "${RED_SET_FILE}" ]] || { echo "  ⛔ 预注册红集不存在: ${RED_SET_FILE}" >&2; exit 2; }
    EXP_SORT=$(grep . "${RED_SET_FILE}" | sort)
    EXTRA=$(comm -23 <(printf '%s\n' "${RED_SORT}") <(printf '%s\n' "${EXP_SORT}"))
    MISSING=$(comm -13 <(printf '%s\n' "${RED_SORT}") <(printf '%s\n' "${EXP_SORT}"))
    [[ -n "${EXTRA}"   ]] && echo "  ⛔ 越界红项(真缺陷, 不许吞): $(printf '%s' "${EXTRA}" | tr '\n' ' ')"
    [[ -n "${MISSING}" ]] && echo "  ⚠️ 未如期转红(变异覆盖不足): $(printf '%s' "${MISSING}" | tr '\n' ' ')"
    if [[ -z "${EXTRA}" && -z "${MISSING}" ]]; then
      echo "  ✅ 如期转红: 红项集合与预注册(${RED_SET_FILE})逐条相同, 且无越界红项"
      exit 0
    fi
    echo "  ⛔ 与预注册不符 ⇒ 本次变异臂读数不可用"
    exit 1
  fi
  echo "  ✅ 如期转红(${N_RED} 项); 未提供 --expect-red-set ⇒ 仅校验方向, 不校验集合"
  exit 0
fi

echo "  PASS=${PASS} FAIL=${FAIL}"
cat > "${OUT}/acceptance_result.json" <<EOF
{
  "lib": "${LIB}", "driver": "${DRV}", "probe": "${PROBE}", "m1_preload": "${M1}",
  "pass": ${PASS}, "fail": ${FAIL},
  "verdict": "$([[ ${FAIL} -eq 0 ]] && echo PASS || echo FAIL)"
}
EOF
[[ ${FAIL} -eq 0 ]] || exit 1
exit 0

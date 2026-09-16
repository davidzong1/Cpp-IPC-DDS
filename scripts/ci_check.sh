#!/usr/bin/env bash
#
# dzIPC 最小 CI 门: 固定解释器 + 依赖可导入 + 验证入口真的能跑
#
# 为什么是脚本而不是直接写 .github/workflows:
#   1) 本仓当前**没有** CI。且 `.gitignore:64` 有一行 `.github/workflows/**` ——
#      就算把 workflow 文件放进去, git 也会静默忽略它: CI 永远不会跑, 而本地
#      `ls .github/workflows` 看起来又是"存在"的。本仓的 CI 入口因此必须是
#      **可被跟踪的脚本**, workflow 只是调用它的薄壳(见 docs/ci.md)。
#   2) 验证入口对解释器有硬要求: 绑定是 CPython **3.10** 构建的
#      (_dzipc_core.cpython-310-x86_64-linux-gnu.so, 且 `*.so` 被 ignore ⇒
#      它是**本机构建产物**, 干净检出里不存在), 而默认 python3 常是 3.12, 在
#      3.12 下 import dzipc 必失败。失败不可怕, **静默跳过才可怕** —— 一个
#      "SKIP" 和 "PASS" 在 CI 里一样是绿的。本脚本把所有跳过变成**显式失败**,
#      只有显式给 --allow-skip 才降级(开发机用途)。
#
# 两层(矩阵与理由见 docs/ci.md):
#   L1 无绑定 —— tools/dzplot/test/test_dzplot.py        任意 CPython3
#   L2 需绑定 —— verify_segment_naming.py / verify_runtime_no_garbage.py /
#                integration_pub_restart.py              必须 CPython 3.10
#
# 退出码: 0=全过  1=有检查失败  2=前置不满足(解释器/绑定缺失/并发占用)
#
# 用法(一律用 `bash` 调用 —— 不依赖文件的执行位):
#   bash scripts/ci_check.sh                 # 全量, 失败即停(默认)
#   bash scripts/ci_check.sh --no-binding    # 只跑 L1, 任意 python3 可跑
#   bash scripts/ci_check.sh --allow-skip    # 前置不满足时不判失败(仅开发机)
#   PY310=/usr/bin/python3.10 bash scripts/ci_check.sh
#
# 环境变量:
#   PY310   CPython 3.10 解释器 (默认 python3.10)
#   PYBASE  L1 用的解释器 (默认 python3)
#
# ⚠️ L2 的三条会起**真实 SHM 段** (/dev/shm), 彼此不可并行: 同机同时跑两个
#    ci_check.sh, 或一边跑 ci_check 一边跑 C++ gtest, 都会互相干扰出假红
#    (已有实测先例: 并发 gtest 的 *_ser_control2 段被误判成本 topic 的垃圾段)。
#    下面用 flock 挡住 ci_check 之间的并发; 与 gtest 的互斥仍靠调用方自觉。

set -euo pipefail

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
PY310="${PY310:-python3.10}"
PYBASE="${PYBASE:-python3}"

NO_BINDING=0
ALLOW_SKIP=0

usage() {
    sed -n '2,40p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --no-binding) NO_BINDING=1 ;;
        --allow-skip) ALLOW_SKIP=1 ;;
        -h|--help)    usage; exit 0 ;;
        *) echo "未知参数: $1 (用 --help 看用法)" >&2; exit 2 ;;
    esac
    shift
done

# ---------------------------------------------------------------- 结果账本
declare -a NAMES=() OUTCOMES=()
record() { NAMES+=("$1"); OUTCOMES+=("$2"); }

print_matrix() {
    echo
    echo "================== 结果矩阵 =================="
    printf '%-4s %-46s %s\n' "层" "检查项" "结果"
    local i
    for i in "${!NAMES[@]}"; do
        printf '%-4s %-46s %s\n' \
            "$(echo "${NAMES[$i]}" | cut -d'|' -f1)" \
            "$(echo "${NAMES[$i]}" | cut -d'|' -f2)" \
            "${OUTCOMES[$i]}"
    done
    echo "=============================================="
}

# ---------------------------------------------------------------- 并发保护
LOCK_FILE="${TMPDIR:-/tmp}/dzipc_ci_check.lock"
if command -v flock >/dev/null 2>&1; then
    exec 9>"$LOCK_FILE"
    if ! flock -n 9; then
        echo "error: 另一个 ci_check.sh 正在运行(L2 会动 /dev/shm, 不可并行)" >&2
        exit 2
    fi
fi

# ---------------------------------------------------------------- 解释器探测
# 先探测再打印: 直接 `$PY310 -V` 会在缺解释器时把 bash 的 "command not found"
# 掺进括号里, 看起来像脚本自己出错, 而真因(缺 3.10)反被淹掉 —— 而"缺 3.10"恰好
# 是 CI 上最可能先撞到的那条路径。
probe_version() {
    if ! command -v "$1" >/dev/null 2>&1; then
        echo "不可用(不在 PATH)"
        return 0
    fi
    "$1" -V 2>&1 | head -1
}

echo "==> 仓库      : $REPO_DIR"
echo "==> L1 解释器 : $PYBASE ($(probe_version "$PYBASE"))"
echo "==> L2 解释器 : $PY310 ($(probe_version "$PY310"))"

# ================================================================ L1 无绑定层
# 这一层不需要 dzipc: test_dzplot.py 是自带的 runner(不是 pytest/unittest),
# 判据是静态的源码/规则核对 + 纯 Python 逻辑, 因此**任意** CPython3 都能跑。
# 也正因如此它能当"旁路哨兵": 即便绑定挂了, 段名规则/sanitize 这类判据仍在守。
echo
echo "===== L1 无绑定: tools/dzplot/test/test_dzplot.py ====="
if ! command -v "$PYBASE" >/dev/null 2>&1; then
    echo "❌ L1 解释器 $PYBASE 不存在。用 PYBASE=/path/to/python3 覆盖。" >&2
    exit 2
fi
L1_RC=0
"$PYBASE" "$REPO_DIR/tools/dzplot/test/test_dzplot.py" || L1_RC=$?
if [[ $L1_RC -eq 0 ]]; then
    record "L1|test_dzplot.py (静态规则 + 纯逻辑)" "PASS"
else
    record "L1|test_dzplot.py (静态规则 + 纯逻辑)" "FAIL(rc=$L1_RC)"
    print_matrix
    echo "❌ L1 失败 —— 失败即停, 不进入 L2。" >&2
    exit 1
fi

if [[ $NO_BINDING -eq 1 ]]; then
    print_matrix
    echo "✅ 只跑了 L1 (--no-binding)。L2 需 CPython 3.10 + 绑定, 未执行。"
    exit 0
fi

# ================================================================ L2 前置
# 绑定可导入性 = 本任务的验收项之一, 单独作为一条检查入账, 而不是"跑挂了才知道"。
echo
echo "===== L2 前置: CPython 3.10 + dzipc._dzipc_core 可导入 ====="
if ! command -v "$PY310" >/dev/null 2>&1; then
    record "L2|CPython 3.10 解释器 ($PY310)" "MISSING"
    print_matrix
    if [[ $ALLOW_SKIP -eq 1 ]]; then
        echo "⚠️  --allow-skip: 解释器缺失降级为跳过。"
        exit 0
    fi
    echo "❌ 找不到 $PY310。绑定是本机 3.10 构建的(*.so 不入库, 干净检出里没有)," >&2
    echo "   必须用 3.10。可 PY310=/path/to/python3.10 覆盖。" >&2
    exit 2
fi

if "$PY310" - "$REPO_DIR" <<'PY'
# 与 main.py 同一条解析顺序: **append**(不是 insert) 仓内 python/, 让显式
# PYTHONPATH / 已安装包优先。这里不能插到最前 —— 那会掩盖 CI 里 pip 装好的
# 那份, 变成"测的是仓内旧产物"。
import sys
sys.path.append(sys.argv[1] + "/python")
try:
    import dzipc
    from dzipc import _dzipc_core
except Exception as exc:                      # noqa: BLE001 — 任何载入失败都算前置不满足
    print(f"  FAIL 绑定不可导入: {type(exc).__name__}: {exc}")
    print(f"       解释器: {sys.version.split()[0]} ({sys.executable})")
    sys.exit(1)
print(f"  PASS 解释器     : {sys.version.split()[0]} ({sys.executable})")
print(f"  PASS dzipc      : {dzipc.__file__}")
print(f"  PASS _dzipc_core: {_dzipc_core.__file__}")
PY
then
    record "L2|绑定可导入(python3.10 + dzipc._dzipc_core)" "PASS"
else
    record "L2|绑定可导入(python3.10 + dzipc._dzipc_core)" "FAIL"
    print_matrix
    if [[ $ALLOW_SKIP -eq 1 ]]; then
        echo "⚠️  --allow-skip: 绑定缺失降级为跳过(开发机用途, CI 不得使用)。"
        exit 0
    fi
    echo "❌ 绑定不可导入。绑定是构建产物(*.so 被 .gitignore 排除), 需要先构建:" >&2
    echo "     $PY310 -m pip install ./python      # 或 scripts/install.sh" >&2
    exit 2
fi

# ================================================================ L2 需绑定层
# 三条都是**真起 SHM 发布端/嗅探器**的运行时核对, 不是字符串自证:
#   verify_segment_naming      —— 推导出的段名是不是传输层真建出的那一个
#   verify_runtime_no_garbage  —— 运行时有没有在 /dev/shm 多建段
#   integration_pub_restart    —— 发布端重启 → generation 变化 → 重挂
# 各自退出码约定 0=全过 / 1=有失败 / 2=SKIP(缺绑定), 本脚本原样透传。
L2_SCRIPTS=(
    "tools/dzplot/test/verify_segment_naming.py"
    "tools/dzplot/test/verify_runtime_no_garbage.py"
    "tools/dzplot/test/integration_pub_restart.py"
)

for rel in "${L2_SCRIPTS[@]}"; do
    echo
    echo "===== L2 需绑定: $rel ====="
    rc=0
    "$PY310" "$REPO_DIR/$rel" || rc=$?
    case $rc in
        0) record "L2|$(basename "$rel")" "PASS" ;;
        2) record "L2|$(basename "$rel")" "SKIP(rc=2)"
           print_matrix
           echo "❌ $rel 自己报了 SKIP。前置检查已过却仍 SKIP ⇒ 说明它看到的绑定" >&2
           echo "   与前置不同(多半是 sys.path 顺序或另一份安装), 按失败处理。" >&2
           exit 2 ;;
        *) record "L2|$(basename "$rel")" "FAIL(rc=$rc)"
           print_matrix
           echo "❌ $rel 失败(rc=$rc) —— 失败即停。" >&2
           exit 1 ;;
    esac
done

print_matrix
echo "✅ 全过: L1 1 项 + L2 4 项。"

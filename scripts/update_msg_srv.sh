#!/bin/bash
# =============================================================================
# msg_srv_update.sh — 更新外部文件夹的 msg 与 srv 文件
#
# 用法:
#   ./msg_srv_update.sh --msg <MSG_PATH> --srv <SRV_PATH>  指定外部路径并更新
#   ./msg_srv_update.sh                                     使用已保存/默认路径更新
#   ./msg_srv_update.sh --reset                             清除外部路径配置
#
# 说明:
#   1. 通过 --msg / --srv 指定的外部路径会以 JSON 形式存入 ./msg/ 和 ./srv/ 目录，
#      下次即使不加参数也能自动识别并同步这些外部路径下的文件。
#   2. 同步策略：将外部路径下的 .msg / .srv 文件（保留子目录结构）复制到本地
#      ./msg/ 和 ./srv/ 目录，然后调用 batch_msg_srv_generator.py 统一生成。
#   3. 不传任何参数时，脚本会自动读取已保存的 JSON 配置进行同步；若从未配置过
#      外部路径，则等价于直接运行 generator（仅使用本地 msg/srv 文件）。
# =============================================================================

set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$SCRIPT_DIR"

# ---- 路径常量 ----
MSG_DIR="./msg"
SRV_DIR="./srv"
MSG_JSON="${MSG_DIR}/external_paths.json"
SRV_JSON="${SRV_DIR}/external_paths.json"

# ---- 颜色 ----
RED='\033[31m'
GREEN='\033[32m'
YELLOW='\033[33m'
CYAN='\033[36m'
NC='\033[0m' # No Color

# ---- Python3（用于 JSON 读写）----
PYTHON_BIN="$(command -v python3 || echo python3)"

# =============================================================================
# 工具函数
# =============================================================================

log_info()  { echo -e "${GREEN}[INFO]${NC}  $*"; }
log_warn()  { echo -e "${YELLOW}[WARN]${NC}  $*"; }
log_error() { echo -e "${RED}[ERROR]${NC} $*"; }
log_step()  { echo -e "${CYAN}[STEP]${NC}  $*"; }

# ---------------------------------------------------------------------------
# 读取 JSON 中保存的外部路径列表
# 参数: $1 = JSON 文件路径
# 输出: 每行一个路径（若无文件或解析失败则无输出）
# ---------------------------------------------------------------------------
read_json_paths() {
    local json_file="$1"
    if [[ ! -f "$json_file" ]]; then
        return 0
    fi
    "$PYTHON_BIN" -c "
import json, sys
try:
    with open('$json_file', 'r') as f:
        data = json.load(f)
    paths = data.get('external_paths', [])
    for p in paths:
        print(p)
except Exception as e:
    sys.stderr.write(f'Warning: failed to read $json_file: {e}\n')
" 2>/dev/null || true
}

# ---------------------------------------------------------------------------
# 将外部路径列表写入 JSON
# 参数: $1 = JSON 文件路径, $2, $3, ... = 路径
# ---------------------------------------------------------------------------
write_json_paths() {
    local json_file="$1"
    shift
    local dir
    dir="$(dirname "$json_file")"
    mkdir -p "$dir"

    # 收集所有非空参数作为路径数组
    local paths_list=""
    for p in "$@"; do
        if [[ -n "$p" ]]; then
            paths_list+="\"$p\", "
        fi
    done
    paths_list="${paths_list%, }"  # 去掉末尾逗号和空格

    "$PYTHON_BIN" -c "
import json, os
paths = [$paths_list]
# 去重并保持顺序
seen = set()
uniq = []
for p in paths:
    if p not in seen:
        seen.add(p)
        uniq.append(p)
data = {'external_paths': uniq}
with open('$json_file', 'w') as f:
    json.dump(data, f, indent=2)
    f.write('\n')
" 2>/dev/null || {
        log_error "写入 JSON 失败: $json_file"
        return 1
    }
}

# ---------------------------------------------------------------------------
# 将外部目录下的 .msg/.srv 文件同步到本地目标目录
# 参数:
#   $1 = 外部源目录
#   $2 = 本地目标目录 (./msg 或 ./srv)
#   $3 = 文件扩展名 (msg 或 srv)
# ---------------------------------------------------------------------------
sync_external_files() {
    local src_dir="$1"
    local dst_dir="$2"
    local ext="$3"

    if [[ ! -d "$src_dir" ]]; then
        log_warn "外部路径不存在，跳过同步: $src_dir"
        return 0
    fi

    log_info "同步外部 .${ext} 文件: ${src_dir} -> ${dst_dir}"

    # 使用 rsync 同步指定扩展名的文件，保留子目录结构
    if command -v rsync &>/dev/null; then
        rsync -av --include='*/' --include="*.${ext}" --exclude='*' \
              --prune-empty-dirs \
              "$src_dir"/ "$dst_dir"/
    else
        # 回退：使用 find + cp
        log_warn "rsync 不可用，使用 cp 回退方案"
        find "$src_dir" -type f -name "*.${ext}" | while read -r src_file; do
            local rel_path="${src_file#$src_dir/}"
            local dst_file="${dst_dir}/${rel_path}"
            mkdir -p "$(dirname "$dst_file")"
            cp "$src_file" "$dst_file"
        done
    fi

    log_info "同步完成: ${src_dir}"
}

# =============================================================================
# 参数解析
# =============================================================================

MSG_EXTERNAL_PATHS=()
SRV_EXTERNAL_PATHS=()
RESET_MODE=false
SHOW_HELP=false

while [[ $# -gt 0 ]]; do
    case "$1" in
        --msg)
            if [[ -z "${2:-}" || "$2" == --* ]]; then
                log_error "--msg 需要一个路径参数"
                exit 1
            fi
            MSG_EXTERNAL_PATHS+=("$2")
            shift 2
            ;;
        --srv)
            if [[ -z "${2:-}" || "$2" == --* ]]; then
                log_error "--srv 需要一个路径参数"
                exit 1
            fi
            SRV_EXTERNAL_PATHS+=("$2")
            shift 2
            ;;
        --reset)
            RESET_MODE=true
            shift
            ;;
        -h|--help)
            SHOW_HELP=true
            shift
            ;;
        *)
            log_error "未知参数: $1"
            SHOW_HELP=true
            shift
            ;;
    esac
done

# ---- 帮助信息 ----
if [[ "$SHOW_HELP" == true ]]; then
    echo "用法: $0 [--msg <MSG_PATH>] [--srv <SRV_PATH>] [--reset]"
    echo ""
    echo "选项:"
    echo "  --msg <PATH>    指定外部 .msg 文件目录（可重复使用以添加多个路径）"
    echo "  --srv <PATH>    指定外部 .srv 文件目录（可重复使用以添加多个路径）"
    echo "  --reset         清除已保存的外部路径配置"
    echo "  -h, --help      显示此帮助信息"
    echo ""
    echo "示例:"
    echo "  $0 --msg ~/project_a/msgs --srv ~/project_a/srvs"
    echo "  $0 --msg ~/project_a/msgs --msg ~/project_b/msgs"
    echo "  $0                    # 使用已保存的配置更新"
    echo "  $0 --reset            # 清除外部路径配置"
    exit 0
fi

# =============================================================================
# 主流程
# =============================================================================

echo ""
echo -e "${GREEN}╔══════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║       dzIPC msg / srv 外部文件同步工具          ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════════════════╝${NC}"
echo ""

# ---- Reset 模式：清除 JSON 配置 ----
if [[ "$RESET_MODE" == true ]]; then
    log_step "清除外部路径配置..."
    rm -f "$MSG_JSON" "$SRV_JSON"
    log_info "已清除配置。接下来将仅使用本地 msg/srv 文件生成。"
    echo ""
fi

# ---- 确定最终要使用的路径 ----
RESOLVED_MSG_PATHS=()
RESOLVED_SRV_PATHS=()

# ---- 合并已保存路径 + 本次命令行路径 ----
_resolve_and_save() {
    # 参数: $1 = JSON 文件路径, $2, $3, ... = 本次命令行传入的新路径
    local json_file="$1"
    shift
    local new_paths=("$@")

    # 1. 先读取已保存的路径
    local saved_paths=()
    mapfile -t saved_paths < <(read_json_paths "$json_file")

    # 2. 合并 (已保存 + 本次新增), 去重
    local all_paths=()
    for p in "${saved_paths[@]}" "${new_paths[@]}"; do
        if [[ -n "$p" ]]; then
            all_paths+=("$p")
        fi
    done

    # 3. 保存合并后的完整列表到 JSON
    if [[ ${#all_paths[@]} -gt 0 ]]; then
        write_json_paths "$json_file" "${all_paths[@]}"
    fi

    # 4. 返回合并后的完整列表 (通过 stdout)
    printf '%s\n' "${all_paths[@]}"
}

if [[ ${#MSG_EXTERNAL_PATHS[@]} -gt 0 ]]; then
    log_step "添加外部 msg 路径 (累加到已有配置)..."
    mapfile -t RESOLVED_MSG_PATHS < <(_resolve_and_save "$MSG_JSON" "${MSG_EXTERNAL_PATHS[@]}")
elif [[ "$RESET_MODE" != true ]]; then
    log_step "未指定 --msg，读取已保存的配置..."
    mapfile -t RESOLVED_MSG_PATHS < <(read_json_paths "$MSG_JSON")
fi

if [[ ${#SRV_EXTERNAL_PATHS[@]} -gt 0 ]]; then
    log_step "添加外部 srv 路径 (累加到已有配置)..."
    mapfile -t RESOLVED_SRV_PATHS < <(_resolve_and_save "$SRV_JSON" "${SRV_EXTERNAL_PATHS[@]}")
elif [[ "$RESET_MODE" != true ]]; then
    log_step "未指定 --srv，读取已保存的配置..."
    mapfile -t RESOLVED_SRV_PATHS < <(read_json_paths "$SRV_JSON")
fi

# ---- 同步外部 msg 文件 ----
if [[ ${#RESOLVED_MSG_PATHS[@]} -gt 0 ]]; then
    echo ""
    log_step "===== 同步外部 .msg 文件 ====="
    for ext_path in "${RESOLVED_MSG_PATHS[@]}"; do
        if [[ -n "$ext_path" ]]; then
            sync_external_files "$ext_path" "$MSG_DIR" "msg"
        fi
    done
else
    log_info "无外部 msg 路径，将仅使用本地 ./msg/ 目录中的文件。"
fi

# ---- 同步外部 srv 文件 ----
if [[ ${#RESOLVED_SRV_PATHS[@]} -gt 0 ]]; then
    echo ""
    log_step "===== 同步外部 .srv 文件 ====="
    for ext_path in "${RESOLVED_SRV_PATHS[@]}"; do
        if [[ -n "$ext_path" ]]; then
            sync_external_files "$ext_path" "$SRV_DIR" "srv"
        fi
    done
else
    log_info "无外部 srv 路径，将仅使用本地 ./srv/ 目录中的文件。"
fi

# ---- 运行 batch_msg_srv_generator.py ----
echo ""
log_step "===== 运行 batch_msg_srv_generator.py ====="
"$PYTHON_BIN" generator/batch_msg_srv_generator.py

echo ""
echo -e "${GREEN}╔══════════════════════════════════════════════════╗${NC}"
echo -e "${GREEN}║           msg / srv 文件更新完成！               ║${NC}"
echo -e "${GREEN}╚══════════════════════════════════════════════════╝${NC}"
echo ""

# ---- 打印当前配置摘要 ----
if [[ -f "$MSG_JSON" ]] || [[ -f "$SRV_JSON" ]]; then
    echo "当前外部路径配置:"
    if [[ -f "$MSG_JSON" ]]; then
        echo "  msg:"
        read_json_paths "$MSG_JSON" | while read -r p; do
            echo "    - $p"
        done
    fi
    if [[ -f "$SRV_JSON" ]]; then
        echo "  srv:"
        read_json_paths "$SRV_JSON" | while read -r p; do
            echo "    - $p"
        done
    fi
fi

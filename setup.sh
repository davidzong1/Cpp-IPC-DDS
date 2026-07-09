#!/bin/bash
# ============================================================================
# dzIPC 工作空间环境设置脚本
# 用法: source setup.sh
#
# 仿 ROS2 工作空间隔离机制——仅在 source 本脚本的 shell 中激活 dzIPC 环境，
# 退出 shell 或新开终端后环境自动恢复，不会污染系统环境。
# ============================================================================

if [ "${BASH_SOURCE[0]}" = "$0" ]; then
    echo "错误: 请使用 source 执行本脚本，不要直接运行。"
    echo "正确用法: source setup.sh"
    exit 1
fi

# ---- 确定脚本所在目录 (DZIPC_ROOT) ----
_DZIPC_SETUP_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
export DZIPC_ROOT="$_DZIPC_SETUP_DIR"
_DZIPC_PREFIX="$DZIPC_ROOT/local"

# ---- 检查是否已安装 ----
if [ ! -d "$_DZIPC_PREFIX" ]; then
    echo "[dzIPC] 尚未安装，请先运行 ./install.sh"
    return 1
fi

# ---- 工具函数: 去重追加到环境变量 ----
_dzipc_append() {
    local _var="$1"
    local _val="$2"
    if [ -z "${!_var}" ]; then
        export "$_var"="$_val"
    else
        case ":${!_var}:" in
            *:"$_val":*) ;;
            *) export "$_var"="${!_var}:$_val" ;;
        esac
    fi
}

# ---- 设置各环境变量 ----

# PATH: 可执行文件 (dzipc_list, dzipc_topic_cat)
_dzipc_append PATH             "$_DZIPC_PREFIX/bin"

# LD_LIBRARY_PATH: 运行时动态库 (libipc.so)
_dzipc_append LD_LIBRARY_PATH  "$_DZIPC_PREFIX/lib"

# C/C++ 头文件路径 (gcc/g++ -I 自动查找)
_dzipc_append CPLUS_INCLUDE_PATH "$_DZIPC_PREFIX/include"
_dzipc_append C_INCLUDE_PATH     "$_DZIPC_PREFIX/include"

# CMAKE_PREFIX_PATH: CMake find_package(cpp-ipc) 查找
_dzipc_append CMAKE_PREFIX_PATH "$_DZIPC_PREFIX"

# PYTHONPATH: Python import dzipc
_dzipc_append PYTHONPATH       "$_DZIPC_PREFIX/lib/python"

# LIBRARY_PATH: 编译时链接库查找
_dzipc_append LIBRARY_PATH     "$_DZIPC_PREFIX/lib"

# ---- 清理内部变量 ----
unset _dzipc_append
unset _DZIPC_SETUP_DIR
unset _DZIPC_PREFIX

# ---- 输出信息 ----
BOX_W=52  # 内框宽度（不含两侧 ║ 和空格）

_dz_print() {
    local _raw="$1"
    # 计算显示宽度: ASCII → 1, CJK/全角 → 2
    local _w=0 _ch _code
    local _len=${#_raw}
    local _i=0
    while [ $_i -lt $_len ]; do
        _ch="${_raw:$_i:1}"
        _code=$(printf '%d' "'$_ch" 2>/dev/null || echo 0)
        if [ "$_code" -gt 255 ] 2>/dev/null; then
            _w=$((_w + 2))
        else
            _w=$((_w + 1))
        fi
        _i=$((_i + 1))
    done
    local _pad=$((BOX_W - _w))
    [ $_pad -lt 0 ] && _pad=0
    printf "║ %s%*s ║\n" "$_raw" $_pad ""
}
_dz_show_result() {
    local _lines=$(tput lines 2>/dev/null || echo 24)
    printf '\n%.0s' $(seq 1 $_lines)   # 推入 scrollback
    printf '\033[H'                    # 光标跳到 (1,1)
    echo -e "$1"
    # printf '\n%.0s' $(seq 1 3)         # 留一点呼吸空间
}
_dz_show_result "╔══════════════════════════════════════════════════════╗"
_dz_print "dzIPC 环境已激活"
echo "╠══════════════════════════════════════════════════════╣"
_dz_print "DZIPC_ROOT = $DZIPC_ROOT"
_dz_print "头文件 → \$DZIPC_ROOT/local/include"
_dz_print "动态库 → \$DZIPC_ROOT/local/lib"
_dz_print "CMake  → \$DZIPC_ROOT/local"
_dz_print "Python → \$DZIPC_ROOT/local/lib/python"
_dz_print "CLI    → \$DZIPC_ROOT/local/bin"
echo "╠══════════════════════════════════════════════════════╣"
_dz_print "退出当前 shell 即可恢复原始环境。"
echo "╚══════════════════════════════════════════════════════╝"
unset _dz_print
unset BOX_W

#!/bin/bash

# 将终端内容推入 scrollback，在可视区顶部打印结果消息
_dz_show_result() {
    local _lines=$(tput lines 2>/dev/null || echo 24)
    printf '\n%.0s' $(seq 1 $_lines)   # 推入 scrollback
    printf '\033[H'                    # 光标跳到 (1,1)
    echo -e "$1"
    # printf '\n%.0s' $(seq 1 3)         # 留一点呼吸空间
}

_dz_show_result "\033[32mStarting installation of dzIPC, Please input configuration parameters:\033[0m"
echo "1. Install all (Include C++ and Python interface)"
echo "2. Install C++ interface only"
echo "3. Install Python interface only"
echo "4. Update dzIPC msg and srv files only (No need to re-install dzIPC)"
echo "5. **Importance** Install dzIPC dispatcher"
echo "6. Uninstall dzIPC"
read -p "Please input the configuration number (1, 2, 3, 4, 5, or 6): " configuration
install_cpp=false
install_python=false
update_msg_srv=false
uninstall=false
install_dispatcher=false
if [ "$configuration" == "1" ]; then
    echo "Installing all interfaces..."
    install_cpp=true
    install_python=true
elif [ "$configuration" == "2" ]; then
    echo "Installing C++ interface only..."
    install_cpp=true
    install_python=false
elif [ "$configuration" == "3" ]; then
    echo "Installing Python interface only..."
    install_cpp=false
    install_python=true
elif [ "$configuration" == "4" ]; then
    echo "Updating dzIPC msg and srv files only..."
    update_msg_srv=true
elif [ "$configuration" == "5" ]; then
    echo "Installing dzIPC dispatcher..."
    install_dispatcher=true
elif [ "$configuration" == "6" ]; then
    echo "Uninstalling dzIPC..."
    uninstall=true

else
    echo "Invalid configuration. Please choose 1, 2, 3, 4, or 5."
    exit 1
fi

if [ "$install_cpp" == true ]; then
    echo "Installing dzIPC..."
    echo "Installing into workspace-local prefix..."
    _DZIPC_PREFIX="$(cd "$(dirname "$0")/.." && pwd)/local"
    _DZIPC_PYTHON="$(command -v python3)"
    mkdir -p build
    cd build

    rm -f "$_DZIPC_PREFIX/lib/python/dzipc"/_dzipc_core*.so

    cmake .. -DCMAKE_BUILD_TYPE=Release \
             -DCMAKE_INSTALL_PREFIX="$_DZIPC_PREFIX" \
             -DPython3_EXECUTABLE="$_DZIPC_PYTHON"
    make -j10
    make install
    echo -e "\033[32mdzIPC installed successfully.\033[0m"
    cd ..
    unset _DZIPC_PREFIX
    unset _DZIPC_PYTHON
fi

if [ "$install_python" == true ]; then
    echo "Installing Python interface..."
    # Python 模块已由 C++ 安装步骤(make install)复制到 local/lib/python/dzipc/
    # 无需 pip install，也无需拷贝到系统 site-packages。
    # 使用方式: source setup.sh 后即可 import dzipc（通过 PYTHONPATH 隔离）
    if [ "$install_cpp" != true ]; then
        # 仅安装 Python 时：单独编译并复制模块到 local/
        _DZIPC_PREFIX="$(cd "$(dirname "$0")/.." && pwd)/local"
        _DZIPC_PYTHON="$(command -v python3)"
        mkdir -p "$_DZIPC_PREFIX/lib/python/dzipc"
        rm -f "$_DZIPC_PREFIX/lib/python/dzipc"/_dzipc_core*.so

        # 编译 Python 绑定模块
        cd python
        "$_DZIPC_PYTHON" -c "import pybind11" 2>/dev/null || "$_DZIPC_PYTHON" -m pip install pybind11
        mkdir -p ../build_py
        cd ../build_py
        cmake .. -DCMAKE_BUILD_TYPE=Release -DLIBIPC_BUILD_PYTHON=ON \
                 -DLIBIPC_BUILD_TESTS=OFF -DLIBIPC_BUILD_DEMOS=OFF \
                 -DCMAKE_INSTALL_PREFIX="$_DZIPC_PREFIX" \
                 -DPython3_EXECUTABLE="$_DZIPC_PYTHON"
        make -j10 _dzipc_core
        cp python/_dzipc_core*.so "$_DZIPC_PREFIX/lib/python/dzipc/"
        cd ..

        # 复制 Python 包文件
        cp -r python/dzipc/*.py "$_DZIPC_PREFIX/lib/python/dzipc/"
        cp -r python/dzipc/gen_msgs "$_DZIPC_PREFIX/lib/python/dzipc/"
        cp -r python/dzipc/gen_srv "$_DZIPC_PREFIX/lib/python/dzipc/"

        cd tool
        pip install -r requirements.txt
        cd ..

        unset _DZIPC_PREFIX
        unset _DZIPC_PYTHON
    fi
    echo -e "\033[32mPython interface installed successfully.\033[0m"
fi

if [ "$update_msg_srv" == true ]; then
    echo "Updating dzIPC msg and srv files..."
    python3 generator/batch_msg_srv_generator.py
    echo -e "\033[32mdzIPC msg and srv files updated successfully.\033[0m"
    cd ..
fi

if [ "$install_dispatcher" == true ]; then
    cd generator
    if bash install_dzipc_dispatch_service.sh; then
        echo -e "\033[32mdzIPC dispatcher installed successfully.\033[0m"
    else
        echo -e "\033[31mdzIPC dispatcher installation failed.\033[0m" >&2
        exit 1
    fi
    cd ..
fi

if [ "$uninstall" == true ]; then
    echo "Uninstalling dzIPC..."
    _DZIPC_PREFIX="$(cd "$(dirname "$0")/.." && pwd)/local"
    # 删除本地安装目录；不调用旧 build cache 中可能指向 /usr/local 的 uninstall target。
    rm -rf "$(cd "$(dirname "$0")/.." && pwd)/build"
    rm -rf "$_DZIPC_PREFIX"
    rm -rf "$(cd "$(dirname "$0")/.." && pwd)/python/dzipc/gen_msgs"
    rm -rf "$(cd "$(dirname "$0")/.." && pwd)/python/dzipc/gen_srv"
    rm -rf "$(cd "$(dirname "$0")/.." && pwd)/python/dzipc.pyi"
    echo -e "\033[32mdzIPC uninstalled successfully.\033[0m"
    unset _DZIPC_PREFIX
fi


# 目标值 512 MB 转换为字节
TARGET_BYTES=$((512 * 1024 * 1024))         # 536870912
# ⚠ `=` 后不能有空格: `VAR= $(...)` 会被解析成"以 VAR= 为环境去执行命令 $(...)",
#   结果是打印 "131072: command not found" 且变量保持为空。
TARGET_PAPER_NUM=$((TARGET_BYTES / 4096))   # 131072 页(= 512MB / 4KB)

# 获取当前值（单位：字节）
CURRENT=$(sysctl -n net.core.rmem_max 2>/dev/null)

# 检查是否获取成功
if [ -z "$CURRENT" ]; then
    echo "错误：无法读取 net.core.rmem_max 当前值"
    exit 1
fi

echo "当前 net.core.rmem_max = $CURRENT 字节"

if [ "$CURRENT" -eq "$TARGET_BYTES" ]; then
    echo "已是 $((TARGET_BYTES / 1024 / 1024))MB，跳过设置。"
    exit 0
else
    echo "当前值不是 $((TARGET_BYTES / 1024 / 1024))MB，准备修改..."
    # 先立即生效(本次运行内)。
    sudo sysctl -w net.core.rmem_max=$TARGET_BYTES
    # 写入持久化配置。
    # 注意: 分隔符不能加引号。写 <<'EOF' 会关掉变量展开, 于是落盘的是字面量
    #   "TARGET_BYTES", 该文件对 sysctl 非法, 下次开机 systemd-sysctl 解析会失败。
    sudo tee /etc/sysctl.d/99-udp-buffers.conf >/dev/null <<EOF
net.core.rmem_max = $TARGET_BYTES
net.core.wmem_max = $TARGET_BYTES
net.core.rmem_default = $TARGET_BYTES
net.core.wmem_default = $TARGET_BYTES
# 可选：系统级 UDP 内存全局上限。注意 udp_mem 是三个值(min pressure max),
#   单位是页, 只给一个数会被内核拒绝。
# net.ipv4.udp_mem = $((TARGET_PAPER_NUM / 2)) $((TARGET_PAPER_NUM * 3 / 4)) $TARGET_PAPER_NUM
EOF
    # 立即生效并顺手校验文件可解析。
    # (不用 sysctl -r: 它的 -r 是 --pattern, 只做匹配显示、不写值, 且须跟一个模式实参。)
    sudo sysctl -q -p /etc/sysctl.d/99-udp-buffers.conf
    echo "修改完成。"
fi

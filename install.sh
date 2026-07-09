# !/bin/bash

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
    echo "Removing old version if exists..."
    mkdir -p build
    cd build
    if [ -d "/usr/local/include/dzIPC" ] || [ -d "/usr/local/lib/ipc_msg" ]||[ -d "/usr/local/lib/ipc_srv" ]||[ -d "/usr/local/lib/libipc" ]; then
        make uninstall
    fi

    cmake .. -DCMAKE_BUILD_TYPE=Release
    make -j10
    make install
    echo -e "\033[32mdzIPC installed successfully.\033[0m"
    cd ..
fi

if [ "$install_python" == true ]; then
    echo "Installing Python interface..."
    # Python 模块已由 C++ 安装步骤(make install)复制到 local/lib/python/dzipc/
    # 无需 pip install，也无需拷贝到系统 site-packages。
    # 使用方式: source setup.sh 后即可 import dzipc（通过 PYTHONPATH 隔离）
    if [ "$install_cpp" != true ]; then
        # 仅安装 Python 时：单独编译并复制模块到 local/
        _DZIPC_PREFIX="$(cd "$(dirname "$0")" && pwd)/local"
        mkdir -p "$_DZIPC_PREFIX/lib/python/dzipc"

        # 编译 Python 绑定模块
        cd python
        python3 -c "import pybind11" 2>/dev/null || pip3 install pybind11
        mkdir -p ../build_py
        cd ../build_py
        cmake .. -DCMAKE_BUILD_TYPE=Release -DLIBIPC_BUILD_PYTHON=ON \
                 -DLIBIPC_BUILD_TESTS=OFF -DLIBIPC_BUILD_DEMOS=OFF
        make -j10 _dzipc_core
        cp python/_dzipc_core*.so "$_DZIPC_PREFIX/lib/python/dzipc/"
        cd ..

        # 复制 Python 包文件
        cp -r python/dzipc/*.py "$_DZIPC_PREFIX/lib/python/dzipc/"
        cp -r python/dzipc/gen_msgs "$_DZIPC_PREFIX/lib/python/dzipc/"

        unset _DZIPC_PREFIX
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
    cd build
    if [ -d "/usr/local/include/dzIPC" ] || [ -d "/usr/local/lib/ipc_msg" ]||[ -d "/usr/local/lib/ipc_srv" ]||[ -d "/usr/local/lib/libipc" ]; then
        make uninstall
    fi
    # 删除本地安装目录
    cd ..
    rm -rf "$(cd "$(dirname "$0")" && pwd)/build"
    rm -rf "$(cd "$(dirname "$0")" && pwd)/local"
    echo -e "\033[32mdzIPC uninstalled successfully.\033[0m"
fi

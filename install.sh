# !/bin/bash
echo "Starting installation of dzIPC, Please input configuration parameters:"
echo "Configuration parameters:"
echo "1. Install all (Include C++ and Python interface)"
echo "2. Install C++ interface (Include update msg and srv files)"
echo "3. Install Python interface only"
echo "4. Uninstall dzIPC"
read configuration
install_cpp=false
install_python=false
uninstall=false
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
    uninstall=true
else
    echo "Invalid configuration. Please choose 1, 2, 3, or 4."
    exit 1
fi
if [ "$install_cpp" == true ]; then
    echo "Installing dzIPC..."
    echo "Removing old version if exists..."
    mkdir -p build
    cd build
    if [ -d "/usr/local/include/dzIPC" ] || [ -d "/usr/local/lib/ipc_msg" ]||[ -d "/usr/local/lib/ipc_srv" ]||[ -d "/usr/local/lib/libipc" ]; then
        sudo make uninstall
    fi

    sudo cmake .. -DCMAKE_BUILD_TYPE=Release
    sudo make -j10
    sudo make install
    echo -e "\033[32mdzIPC installed successfully.\033[0m"
    cd ..
fi
if [ "$install_python" == true ]; then
    echo "Installing Python interface..."
    cd python
    pip install -e .
    echo -e "\033[32mPython interface installed successfully.\033[0m"
    cd ..
fi
if [ "$uninstall" == true ]; then
    echo "Uninstalling dzIPC..."
    cd build
    if [ -d "/usr/local/include/dzIPC" ] || [ -d "/usr/local/lib/ipc_msg" ]||[ -d "/usr/local/lib/ipc_srv" ]||[ -d "/usr/local/lib/libipc" ]; then
        sudo make uninstall
    fi
    pip uninstall dzIPC -y
    echo -e "\033[32mdzIPC uninstalled successfully.\033[0m"
fi
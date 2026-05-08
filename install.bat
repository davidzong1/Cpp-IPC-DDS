@echo off
setlocal enabledelayedexpansion

echo Starting installation of dzIPC, Please input configuration parameters:
echo Configuration parameters:
echo 1. Install all (Include C++ and Python interface)
echo 2. Install C++ interface (Include update msg and srv files)
echo 3. Install Python interface only
echo 4. Uninstall dzIPC
set /p configuration=Choice:

set install_cpp=false
set install_python=false
set uninstall=false

if "%configuration%"=="1" (
    echo Installing all interfaces...
    set install_cpp=true
    set install_python=true
) else if "%configuration%"=="2" (
    echo Installing C++ interface only...
    set install_cpp=true
    set install_python=false
) else if "%configuration%"=="3" (
    echo Installing Python interface only...
    set install_cpp=false
    set install_python=true
) else if "%configuration%"=="4" (
    set uninstall=true
) else (
    echo Invalid configuration. Please choose 1, 2, 3, or 4.
    exit /b 1
)

REM Script root directory and default install prefix
set ROOT_DIR=%~dp0
set INSTALL_PREFIX=C:\Program Files\dzIPC

if "%install_cpp%"=="true" (
    echo Installing dzIPC...
    echo Removing old version if exists...
    if not exist "%ROOT_DIR%build" mkdir "%ROOT_DIR%build"
    pushd "%ROOT_DIR%build"

    if exist "%INSTALL_PREFIX%\include\dzIPC" (
        cmake --build . --target uninstall --config Release
    ) else if exist "%INSTALL_PREFIX%\lib\ipc_msg" (
        cmake --build . --target uninstall --config Release
    ) else if exist "%INSTALL_PREFIX%\lib\ipc_srv" (
        cmake --build . --target uninstall --config Release
    ) else if exist "%INSTALL_PREFIX%\lib\libipc" (
        cmake --build . --target uninstall --config Release
    )

    cmake .. -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="%INSTALL_PREFIX%"
    if errorlevel 1 (
        echo CMake configuration failed.
        popd
        exit /b 1
    )
    cmake --build . --config Release --parallel 10
    if errorlevel 1 (
        echo Build failed.
        popd
        exit /b 1
    )
    cmake --install . --config Release
    if errorlevel 1 (
        echo Install failed. You may need administrator privileges.
        popd
        exit /b 1
    )
    echo [OK] dzIPC installed successfully.
    popd
)

if "%install_python%"=="true" (
    echo Installing Python interface...
    pushd "%ROOT_DIR%python"
    pip install -e .
    if errorlevel 1 (
        echo Python install failed.
        popd
        exit /b 1
    )
    echo [OK] Python interface installed successfully.
    popd
)

if "%uninstall%"=="true" (
    echo Uninstalling dzIPC...
    if exist "%ROOT_DIR%build" (
        pushd "%ROOT_DIR%build"
        if exist cmake_install.cmake (
            cmake --build . --target uninstall --config Release
        )
        popd
    )
    pip uninstall dzipc -y
    echo [OK] dzIPC uninstalled successfully.
)

endlocal

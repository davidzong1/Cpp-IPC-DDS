@echo off
setlocal enabledelayedexpansion

echo Starting installation of dzIPC, Please input configuration parameters:
echo Configuration parameters:
echo 1. Install all (Include C++ and Python interface)
echo 2. Install C++ interface only
echo 3. Install Python interface only
echo 4. Update dzIPC msg and srv files only (No need to re-install dzIPC)
echo 5. **Importance** Install dzIPC dispatcher
echo 6. Uninstall dzIPC
set /p configuration=Choice:

set install_cpp=false
set install_python=false
set update_msg_srv=false
set install_dispatcher=false
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
    echo Updating dzIPC msg and srv files only...
    set update_msg_srv=true
) else if "%configuration%"=="5" (
    echo Installing dzIPC dispatcher...
    set install_dispatcher=true
) else if "%configuration%"=="6" (
    echo Uninstalling dzIPC...
    set uninstall=true
) else (
    echo Invalid configuration. Please choose 1, 2, 3, 4, 5, or 6.
    exit /b 1
)

REM Script root directory and default install prefix
set ROOT_DIR=%~dp0
if "%ROOT_DIR:~-1%"=="\" set ROOT_DIR=%ROOT_DIR:~0,-1%
set INSTALL_PREFIX=%ROOT_DIR%\local

if "%install_cpp%"=="true" (
    echo Installing dzIPC...
    echo Removing old version if exists...
    if not exist "%ROOT_DIR%\build" mkdir "%ROOT_DIR%\build"
    pushd "%ROOT_DIR%\build"

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
    if not "%install_cpp%"=="true" (
        if not exist "%INSTALL_PREFIX%\lib\python\dzipc" mkdir "%INSTALL_PREFIX%\lib\python\dzipc"

        pushd "%ROOT_DIR%\python"
        python -c "import pybind11" >nul 2>nul
        if errorlevel 1 (
            pip install pybind11
            if errorlevel 1 (
                echo Failed to install pybind11.
                popd
                exit /b 1
            )
        )
        popd

        if not exist "%ROOT_DIR%\build_py" mkdir "%ROOT_DIR%\build_py"
        pushd "%ROOT_DIR%\build_py"
        cmake .. -DCMAKE_BUILD_TYPE=Release -DLIBIPC_BUILD_PYTHON=ON -DLIBIPC_BUILD_TESTS=OFF -DLIBIPC_BUILD_DEMOS=OFF -DCMAKE_INSTALL_PREFIX="%INSTALL_PREFIX%"
        if errorlevel 1 (
            echo Python CMake configuration failed.
            popd
            exit /b 1
        )
        cmake --build . --config Release --target _dzipc_core --parallel 10
        if errorlevel 1 (
            echo Python binding build failed.
            popd
            exit /b 1
        )
        for /r "%ROOT_DIR%\build_py\python" %%F in (_dzipc_core*.pyd _dzipc_core*.dll _dzipc_core*.so) do (
            copy /Y "%%F" "%INSTALL_PREFIX%\lib\python\dzipc\" >nul
        )
        popd

        xcopy /Y /Q "%ROOT_DIR%\python\dzipc\*.py" "%INSTALL_PREFIX%\lib\python\dzipc\" >nul
        if exist "%INSTALL_PREFIX%\lib\python\dzipc\gen_msgs" rmdir /S /Q "%INSTALL_PREFIX%\lib\python\dzipc\gen_msgs"
        xcopy /E /I /Y /Q "%ROOT_DIR%\python\dzipc\gen_msgs" "%INSTALL_PREFIX%\lib\python\dzipc\gen_msgs" >nul
    )
    echo [OK] Python interface installed successfully.
)

if "%update_msg_srv%"=="true" (
    echo Updating dzIPC msg and srv files...
    pushd "%ROOT_DIR%\generator"
    python batch_msg_srv_generator.py
    if errorlevel 1 (
        echo dzIPC msg and srv update failed.
        popd
        exit /b 1
    )
    echo [OK] dzIPC msg and srv files updated successfully.
    popd
)

if "%install_dispatcher%"=="true" (
    echo dzIPC dispatcher installation is only supported on Linux/systemd by:
    echo   generator\install_dzipc_dispatch_service.sh
    echo.
    echo On native Windows there is no equivalent SCHED_FIFO/CAP_SYS_NICE permission profile.
    echo Please run the dispatcher installer from Linux or WSL if needed.
    exit /b 1
)

if "%uninstall%"=="true" (
    echo Uninstalling dzIPC...
    if exist "%ROOT_DIR%\build" (
        pushd "%ROOT_DIR%\build"
        if exist cmake_install.cmake (
            cmake --build . --target uninstall --config Release
        )
        popd
    )
    if exist "%INSTALL_PREFIX%" rmdir /S /Q "%INSTALL_PREFIX%"
    echo [OK] dzIPC uninstalled successfully.
)

endlocal

@echo off
setlocal EnableExtensions EnableDelayedExpansion
rem ============================================================================
rem dzIPC workspace environment setup script for Windows cmd.exe
rem
rem Usage:
rem   setup.bat
rem   call setup.bat     (when used from another .bat/.cmd script)
rem
rem This script only updates the current cmd.exe environment. Open a new terminal
rem to return to the original environment.
rem ============================================================================

set "DZIPC_SETUP_DIR=%~dp0"
if "%DZIPC_SETUP_DIR:~-1%"=="\" set "DZIPC_SETUP_DIR=%DZIPC_SETUP_DIR:~0,-1%"
set "DZIPC_ROOT=%DZIPC_SETUP_DIR%"
set "DZIPC_PREFIX=%DZIPC_ROOT%\local"

if not exist "%DZIPC_PREFIX%\" (
    echo [dzIPC] local installation was not found.
    echo [dzIPC] Expected: "%DZIPC_PREFIX%"
    echo [dzIPC] Please run install.bat first.
    endlocal & exit /b 1
)

call :append_env PATH "%DZIPC_PREFIX%\bin"
call :append_env PATH "%DZIPC_PREFIX%\lib"
call :append_env INCLUDE "%DZIPC_PREFIX%\include"
call :append_env CPLUS_INCLUDE_PATH "%DZIPC_PREFIX%\include"
call :append_env C_INCLUDE_PATH "%DZIPC_PREFIX%\include"
call :append_env LIB "%DZIPC_PREFIX%\lib"
call :append_env LIBRARY_PATH "%DZIPC_PREFIX%\lib"
call :append_env CMAKE_PREFIX_PATH "%DZIPC_PREFIX%"
call :append_env PYTHONPATH "%DZIPC_PREFIX%\lib\python"

echo.
echo ============================================================
echo dzIPC environment activated
echo ------------------------------------------------------------
echo DZIPC_ROOT = %DZIPC_ROOT%
echo DZIPC_PREFIX = %DZIPC_PREFIX%
echo Include    = %%DZIPC_ROOT%%\local\include
echo Library    = %%DZIPC_ROOT%%\local\lib
echo CMake      = %%DZIPC_ROOT%%\local
echo Python     = %%DZIPC_ROOT%%\local\lib\python
echo CLI        = %%DZIPC_ROOT%%\local\bin
echo ------------------------------------------------------------
echo Open a new terminal to restore the original environment.
echo ============================================================
echo.

endlocal ^
& set "DZIPC_ROOT=%DZIPC_ROOT%" ^
& set "DZIPC_PREFIX=%DZIPC_PREFIX%" ^
& set "PATH=%PATH%" ^
& set "INCLUDE=%INCLUDE%" ^
& set "CPLUS_INCLUDE_PATH=%CPLUS_INCLUDE_PATH%" ^
& set "C_INCLUDE_PATH=%C_INCLUDE_PATH%" ^
& set "LIB=%LIB%" ^
& set "LIBRARY_PATH=%LIBRARY_PATH%" ^
& set "CMAKE_PREFIX_PATH=%CMAKE_PREFIX_PATH%" ^
& set "PYTHONPATH=%PYTHONPATH%"
exit /b 0

:append_env
set "__var=%~1"
set "__val=%~2"
if "%__val%"=="" exit /b 0

if not defined %__var% (
    set "%__var%=%__val%"
    exit /b 0
)

set "__cur=!%__var%!"
set "__needle=;%__val%;"
set "__haystack=;!__cur!;"
if not "!__haystack:%__needle%=!"=="!__haystack!" exit /b 0

set "%__var%=!__cur!;%__val%"
exit /b 0

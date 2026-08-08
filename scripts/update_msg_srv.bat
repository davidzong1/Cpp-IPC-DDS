@echo off
REM =============================================================================
REM msg_srv_update.cmd — 更新外部文件夹的 msg 与 srv 文件 (Windows CMD)
REM
REM 用法:
REM   msg_srv_update.cmd --msg <MSG_PATH> --srv <SRV_PATH>  指定外部路径并更新
REM   msg_srv_update.cmd                                     使用已保存/默认路径更新
REM   msg_srv_update.cmd --reset                             清除外部路径配置
REM
REM 说明:
REM   1. 通过 --msg / --srv 指定的外部路径会以 JSON 形式存入 .\msg\ 和 .\srv\ 目录，
REM      下次即使不加参数也能自动识别并同步这些外部路径下的文件。
REM   2. 同步策略：将外部路径下的 .msg / .srv 文件（保留子目录结构）复制到本地
REM      .\msg\ 和 .\srv\ 目录，然后调用 batch_msg_srv_generator.py 统一生成。
REM   3. 不传任何参数时，脚本会自动读取已保存的 JSON 配置进行同步；若从未配置过
REM      外部路径，则等价于直接运行 generator（仅使用本地 msg/srv 文件）。
REM =============================================================================

setlocal EnableDelayedExpansion

REM ---- 切换到项目根目录 ----
cd /d "%~dp0.."

REM ---- 路径常量 ----
set "MSG_DIR=.\msg"
set "SRV_DIR=.\srv"
set "MSG_JSON=%MSG_DIR%\external_paths.json"
set "SRV_JSON=%SRV_DIR%\external_paths.json"

REM ---- 查找 Python 解释器 ----
set "PYTHON_BIN="
for %%p in (python python3 py) do (
    if not defined PYTHON_BIN (
        where %%p >nul 2>&1 && set "PYTHON_BIN=%%p"
    )
)
if not defined PYTHON_BIN (
    echo [ERROR] 找不到 Python 解释器 (python / python3 / py^)
    exit /b 1
)

REM =============================================================================
REM 参数解析
REM =============================================================================

set "MSG_COUNT=0"
set "SRV_COUNT=0"
set "RESET_MODE=0"
set "SHOW_HELP=0"

:parse_args
if "%~1"=="" goto :parse_done
if /i "%~1"=="--msg" goto :parse_msg
if /i "%~1"=="-Msg"  goto :parse_msg
if /i "%~1"=="--srv" goto :parse_srv
if /i "%~1"=="-Srv"  goto :parse_srv
if /i "%~1"=="--reset" goto :parse_reset
if /i "%~1"=="-Reset"  goto :parse_reset
if /i "%~1"=="--help" goto :parse_help
if /i "%~1"=="-h"    goto :parse_help
if /i "%~1"=="-Help" goto :parse_help
if /i "%~1"=="-?"    goto :parse_help
echo [ERROR] 未知参数: %~1
set "SHOW_HELP=1"
shift
goto :parse_args

:parse_msg
shift
if "%~1"=="" (
    echo [ERROR] --msg 需要一个路径参数
    exit /b 1
)
echo %~1 | findstr /r /c:"^--" >nul 2>&1
if not errorlevel 1 (
    echo [ERROR] --msg 需要一个路径参数
    exit /b 1
)
set /a MSG_COUNT+=1
set "MSG_%MSG_COUNT%=%~1"
shift
goto :parse_args

:parse_srv
shift
if "%~1"=="" (
    echo [ERROR] --srv 需要一个路径参数
    exit /b 1
)
echo %~1 | findstr /r /c:"^--" >nul 2>&1
if not errorlevel 1 (
    echo [ERROR] --srv 需要一个路径参数
    exit /b 1
)
set /a SRV_COUNT+=1
set "SRV_%SRV_COUNT%=%~1"
shift
goto :parse_args

:parse_reset
set "RESET_MODE=1"
shift
goto :parse_args

:parse_help
set "SHOW_HELP=1"
shift
goto :parse_args

:parse_done

REM ---- 帮助 ----
if "%SHOW_HELP%"=="1" (
    echo 用法: %~nx0 [--msg ^<MSG_PATH^>] [--srv ^<SRV_PATH^>] [--reset]
    echo.
    echo 选项:
    echo   --msg ^<PATH^>    指定外部 .msg 文件目录（可重复使用以添加多个路径）
    echo   --srv ^<PATH^>    指定外部 .srv 文件目录（可重复使用以添加多个路径）
    echo   --reset         清除已保存的外部路径配置
    echo   --help          显示此帮助信息
    echo.
    echo 示例:
    echo   %~nx0 --msg C:\project_a\msgs --srv C:\project_a\srvs
    echo   %~nx0 --msg C:\project_a\msgs --msg C:\project_b\msgs
    echo   %~nx0                    # 使用已保存的配置更新
    echo   %~nx0 --reset            # 清除外部路径配置
    exit /b 0
)

REM =============================================================================
REM 主流程
REM =============================================================================

echo.
echo ================================================
echo        dzIPC msg / srv 外部文件同步工具
echo ================================================
echo.

REM ---- Reset 模式：清除 JSON 配置 ----
if "%RESET_MODE%"=="1" (
    echo [STEP] 清除外部路径配置...
    if exist "%MSG_JSON%" del /f /q "%MSG_JSON%" >nul 2>&1
    if exist "%SRV_JSON%" del /f /q "%SRV_JSON%" >nul 2>&1
    echo [INFO] 已清除配置。接下来将仅使用本地 msg/srv 文件生成。
    echo.
)

REM ---- 读取/合并外部 msg 路径 ----
call :resolve_paths "MSG" "%MSG_JSON%" %MSG_COUNT%

REM ---- 读取/合并外部 srv 路径 ----
call :resolve_paths "SRV" "%SRV_JSON%" %SRV_COUNT%

REM ---- 同步外部 msg 文件 ----
call :sync_all "MSG" "%MSG_DIR%" "msg"

REM ---- 同步外部 srv 文件 ----
call :sync_all "SRV" "%SRV_DIR%" "srv"

REM ---- 运行 batch_msg_srv_generator.py ----
echo.
echo [STEP] ===== 运行 batch_msg_srv_generator.py =====
"%PYTHON_BIN%" generator\batch_msg_srv_generator.py
if errorlevel 1 (
    echo [ERROR] batch_msg_srv_generator.py 执行失败 (errorlevel: %ERRORLEVEL%^)
    exit /b %ERRORLEVEL%
)

echo.
echo ================================================
echo            msg / srv 文件更新完成！
echo ================================================
echo.

REM ---- 打印当前配置摘要 ----
if exist "%MSG_JSON%" goto :print_config_msg
if exist "%SRV_JSON%" goto :print_config_msg
goto :done

:print_config_msg
echo 当前外部路径配置:
if exist "%MSG_JSON%" (
    echo   msg:
    "%PYTHON_BIN%" -c "import json; [print(f'    - {p}') for p in json.load(open('%MSG_JSON%','r',encoding='utf-8')).get('external_paths',[])]"
)
if exist "%SRV_JSON%" (
    echo   srv:
    "%PYTHON_BIN%" -c "import json; [print(f'    - {p}') for p in json.load(open('%SRV_JSON%','r',encoding='utf-8')).get('external_paths',[])]"
)

:done
exit /b 0

REM =============================================================================
REM 子过程
REM =============================================================================

REM ---------------------------------------------------------------------------
REM resolve_paths — 确定最终要使用的路径（合并已保存 + 本次命令行）
REM 参数: %1=类别名(MSG/SRV)  %2=JSON文件路径  %3=命令行传入的路径个数
REM 结果存入: RESOLVED_%1_0~N（以 0 开始计数的环境变量列表）
REM ---------------------------------------------------------------------------
:resolve_paths
set "CATEG=%~1"
set "JSON_FILE=%~2"
set "INPUT_COUNT=%~3"

if not %INPUT_COUNT%==0 (
    echo [STEP] 添加外部 %CATEG% 路径 (累加到已有配置^)...

    REM 将合并写入逻辑写入临时 Python 脚本
    (
        echo import json, os, sys
        echo new = [
        for /l %%i in (1,1,%INPUT_COUNT%) do (
            set "VAL=!%CATEG%_%%i!"
            echo r"!VAL!",
        )
        echo ]
        echo saved = []
        echo try:
        echo     with open(r'%JSON_FILE%', 'r', encoding='utf-8'^) as f:
        echo         saved = json.load(f^).get('external_paths', []^)
        echo except:
        echo     pass
        echo seen = set(^)
        echo paths = []
        echo for p in saved + new:
        echo     if p and p not in seen:
        echo         seen.add(p^)
        echo         paths.append(p^)
        echo target_dir = r'%MSG_DIR%' if '%CATEG%'=='MSG' else r'%SRV_DIR%'
        echo os.makedirs(target_dir, exist_ok=True^)
        echo with open(r'%JSON_FILE%', 'w', encoding='utf-8'^) as f:
        echo     json.dump({'external_paths': paths}, f, indent=2^)
        echo     f.write('\n'^)
        echo for p in paths:
        echo     print(p^)
    ) > "%TEMP%\dzipc_merge_%CATEG%.py"

    "%PYTHON_BIN%" "%TEMP%\dzipc_merge_%CATEG%.py" > "%TEMP%\dzipc_%CATEG%_paths.txt" 2>&1
    if errorlevel 1 (
        type "%TEMP%\dzipc_%CATEG%_paths.txt"
        echo [ERROR] 合并路径失败
        del "%TEMP%\dzipc_merge_%CATEG%.py" >nul 2>&1
        del "%TEMP%\dzipc_%CATEG%_paths.txt" >nul 2>&1
        exit /b 1
    )

    del "%TEMP%\dzipc_merge_%CATEG%.py" >nul 2>&1

    set /a "IDX=0"
    for /f "usebackq delims=" %%p in ("%TEMP%\dzipc_%CATEG%_paths.txt") do (
        set "RESOLVED_%CATEG%_!IDX!=%%p"
        set /a "RESOLVED_%CATEG%_COUNT=!IDX!+1"
        set /a IDX+=1
    )
    del "%TEMP%\dzipc_%CATEG%_paths.txt" >nul 2>&1
) else if not "%RESET_MODE%"=="1" (
    echo [STEP] 未指定 --%CATEG% ，读取已保存的配置...
    if exist "%JSON_FILE%" (
        "%PYTHON_BIN%" -c "import json; [print(p) for p in json.load(open(r'%JSON_FILE%','r',encoding='utf-8')).get('external_paths',[])]" > "%TEMP%\dzipc_%CATEG%_paths.txt" 2>&1
        set /a "IDX=0"
        for /f "usebackq delims=" %%p in ("%TEMP%\dzipc_%CATEG%_paths.txt") do (
            if not "%%p"=="" (
                set "RESOLVED_%CATEG%_!IDX!=%%p"
                set /a "RESOLVED_%CATEG%_COUNT=!IDX!+1"
                set /a IDX+=1
            )
        )
        del "%TEMP%\dzipc_%CATEG%_paths.txt" >nul 2>&1
    ) else (
        set "RESOLVED_%CATEG%_COUNT=0"
    )
) else (
    set "RESOLVED_%CATEG%_COUNT=0"
)
goto :eof

REM ---------------------------------------------------------------------------
REM sync_all — 遍历所有已解析路径执行同步
REM 参数: %1=类别名  %2=本地目标目录  %3=扩展名
REM ---------------------------------------------------------------------------
:sync_all
set "CATEG=%~1"
set "DST_DIR=%~2"
set "EXT=%~3"
set "COUNT_VAR=RESOLVED_%CATEG%_COUNT"
call set "COUNT=%%!COUNT_VAR!%%"

if "%COUNT%"=="" set "COUNT=0"

if %COUNT%==0 (
    echo [INFO] 无外部 %CATEG% 路径，将仅使用本地 %DST_DIR%\ 目录中的文件。
    goto :eof
)

echo.
echo [STEP] ===== 同步外部 .%EXT% 文件 =====

set /a "LIMIT=%COUNT%-1"
for /l %%i in (0,1,%LIMIT%) do (
    call set "EXT_PATH=%%RESOLVED_%CATEG%_%%i%%"
    if not "!EXT_PATH!"=="" call :sync_one "!EXT_PATH!" "%DST_DIR%" "%EXT%"
)
goto :eof

REM ---------------------------------------------------------------------------
REM sync_one — 将单个外部目录的文件同步到本地目标目录
REM ---------------------------------------------------------------------------
:sync_one
set "SRC=%~1"
set "DST=%~2"
set "EXT=%~3"

if not exist "%SRC%\" (
    echo [WARN] 外部路径不存在，跳过同步: %SRC%
    goto :eof
)

echo [INFO] 同步外部 .%EXT% 文件: %SRC% -^> %DST%

REM 确保目标目录存在
if not exist "%DST%\" mkdir "%DST%"

REM 优先使用 robocopy（保留目录结构）
where robocopy >nul 2>&1
if not errorlevel 1 (
    robocopy "%SRC%" "%DST%" "*.%EXT%" /S /NFL /NDL /NJH /NJS /nc /ns /np >nul 2>&1
    if errorlevel 8 (
        echo [WARN] robocopy 返回码 %ERRORLEVEL%，部分文件可能未同步
    )
    goto :eof
)

REM 回退：使用 xcopy（保留目录结构）
where xcopy >nul 2>&1
if not errorlevel 1 (
    xcopy "%SRC%\*.%EXT%" "%DST%\" /S /Y /I /Q >nul 2>&1
    if errorlevel 1 (
        echo [WARN] xcopy 返回码 %ERRORLEVEL%，部分文件可能未同步
    )
    goto :eof
)

REM 最后回退：逐文件复制（不保留目录结构）
echo [WARN] robocopy/xcopy 均不可用，使用逐文件复制
for /r "%SRC%" %%f in (*.%EXT%) do (
    copy /y "%%f" "%DST%\" >nul 2>&1
)
goto :eof

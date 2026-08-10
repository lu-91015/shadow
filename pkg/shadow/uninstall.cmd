@echo off
rem ============================================================
rem Shadow 0.5 卸载脚本（删除文件 + 移除环境变量）
rem 用法: 双击运行，或 cmd 中执行 uninstall.cmd
rem ============================================================
setlocal

rem 当前目录即安装目录（脚本位于安装根）
set "INSTALL_DIR=%~dp0"

echo ============================================
echo  Shadow 0.5 卸载
echo  安装目录: %INSTALL_DIR%
echo ============================================
echo.

rem ---- 1. 移除用户环境变量 ----
echo [1/3] 移除环境变量...
setx shadow "" >nul 2>&1
setx SHADOW_HOME "" >nul 2>&1
setx SHADOW_COMPILER "" >nul 2>&1

rem 从用户 PATH 中移除 bin 路径
set "BIN_DIR=%INSTALL_DIR%bin"
for /f "usebackq tokens=*" %%p in (`powershell -NoProfile -Command "[Environment]::GetEnvironmentVariable('PATH','User')"`) do set "USER_PATH=%%p"
if defined USER_PATH (
    set "NEW_PATH=%USER_PATH:;%BIN_DIR%=%"
    set "NEW_PATH=%NEW_PATH:%%BIN_DIR%%=%"
    powershell -NoProfile -Command "[Environment]::SetEnvironmentVariable('PATH','%NEW_PATH%','User')" >nul 2>&1
)
echo   已移除 shadow / SHADOW_HOME / SHADOW_COMPILER / PATH 中的 bin

rem ---- 2. 删除文件 ----
echo [2/3] 删除文件...
rem 延时等待文件句柄释放（如 LSP 进程）
timeout /t 1 /nobreak >nul 2>&1
cd /d "%TEMP%"
rmdir /s /q "%INSTALL_DIR%" 2>nul
if exist "%INSTALL_DIR%" (
    echo   [警告] 部分文件被占用未能删除，请关闭 shadow 相关进程后重试。
    echo   剩余目录: %INSTALL_DIR%
) else (
    echo   已删除 %INSTALL_DIR%
)

rem ---- 3. 完成 ----
echo [3/3] 卸载完成。
echo.
echo 提示: 环境变量修改对新开的终端生效。
echo ============================================
pause

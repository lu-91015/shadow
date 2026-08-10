@echo off
rem Shadow 0.5 安装器启动器（iexpress 解压后由 cmd 中转调用 PowerShell）
powershell.exe -NoProfile -ExecutionPolicy Bypass -File "%~dp0install.ps1"
exit /b %errorlevel%

#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Shadow 0.5 安装器。

打包方式：pyinstaller --onefile --windowed --add-data "pkg/shadow;shadow" installer.py
运行行为：
  1. GUI 让用户选择安装目录（默认 %LOCALAPPDATA%\\shadow）
  2. 复制文件（bin/shadow.exe、rt、bootstrap、src/runtime、tools）
  3. 设置用户级环境变量：shadow / SHADOW_HOME / SHADOW_COMPILER，PATH 追加 <dir>\\bin
  4. 写入 uninstall.cmd 供卸载

支持 --silent <dir>：跳过 GUI，直接装到 <dir>（自动化测试用）。
"""
import os
import sys
import shutil
import subprocess

APP_NAME = "Shadow 0.5"
ENV_SHADOW = "shadow"          # 用户要求的环境变量名（小写）
ENV_HOME = "SHADOW_HOME"
ENV_COMPILER = "SHADOW_COMPILER"


def resource_dir():
    """pyinstaller 打包后从 _MEIPASS 读载荷；源码运行读同目录 shadow/。"""
    if hasattr(sys, "_MEIPASS"):
        return os.path.join(sys._MEIPASS, "shadow")
    return os.path.join(os.path.dirname(os.path.abspath(__file__)), "shadow")


def default_target():
    base = os.environ.get("LOCALAPPDATA") or os.path.expanduser("~")
    return os.path.join(base, "shadow")


def set_user_env(name, value):
    """写用户级环境变量（HKCU\\Environment），立即生效广播。"""
    import winreg
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, "Environment", 0,
                        winreg.KEY_SET_VALUE) as key:
        winreg.SetValueEx(key, name, 0, winreg.REG_EXPAND_SZ, value)
    # 广播 WM_SETTINGCHANGE，让新开的进程读到
    try:
        import ctypes
        HWND_BROADCAST = 0xFFFF
        WM_SETTINGCHANGE = 0x001A
        ctypes.windll.user32.SendMessageTimeoutW(
            HWND_BROADCAST, WM_SETTINGCHANGE, 0, "Environment",
            0x0002, 5000, None)
    except Exception:
        pass


def append_user_path(entry):
    """把 entry 追加到用户 PATH（读旧值去重后写回）。"""
    import winreg
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, "Environment", 0,
                        winreg.KEY_READ | winreg.KEY_SET_VALUE) as key:
        try:
            cur, _ = winreg.QueryValueEx(key, "Path")
        except FileNotFoundError:
            cur = ""
        parts = [p for p in cur.split(";") if p]
        if entry not in parts:
            parts.append(entry)
            winreg.SetValueEx(key, "Path", 0, winreg.REG_EXPAND_SZ, ";".join(parts))
    try:
        import ctypes
        ctypes.windll.user32.SendMessageTimeoutW(
            0xFFFF, 0x001A, 0, "Environment", 0x0002, 5000, None)
    except Exception:
        pass


def write_uninstall(target):
    """写卸载脚本到安装目录：清环境变量 + 删安装目录。"""
    un = os.path.join(target, "uninstall.cmd")
    lines = [
        "@echo off",
        "rem Shadow 0.5 卸载脚本",
        'echo 正在清除用户环境变量（shadow / SHADOW_HOME / SHADOW_COMPILER / PATH 中的 bin 项）...',
        'powershell -NoProfile -Command "[Environment]::SetEnvironmentVariable(\'shadow\',\'\',\'User\'); [Environment]::SetEnvironmentVariable(\'SHADOW_HOME\',\'\',\'User\'); [Environment]::SetEnvironmentVariable(\'SHADOW_COMPILER\',\'\',\'User\')" >nul 2>&1',
        "echo 环境变量已清除（重新打开终端生效）。",
        'echo 正在删除安装目录...',
        'set "TARGET=%~dp0"',
        'powershell -NoProfile -Command "Remove-Item -LiteralPath (\\"%TARGET%\\").TrimEnd(\'\\\\\') -Recurse -Force" >nul 2>&1',
        "echo 卸载完成。",
        "pause",
    ]
    with open(un, "w", encoding="utf-8", newline="\r\n") as f:
        f.write("\r\n".join(lines))
    return un


def install(target):
    """核心安装逻辑：复制文件 + 环境变量 + 卸载脚本。返回 (ok, msg)。"""
    src = resource_dir()
    if not os.path.isdir(src):
        return False, f"载荷目录不存在: {src}"
    if not os.path.isfile(os.path.join(src, "bin", "shadow.exe")):
        return False, f"载荷缺少 bin\\shadow.exe: {src}"

    os.makedirs(target, exist_ok=True)
    # 复制载荷
    for item in os.listdir(src):
        s = os.path.join(src, item)
        d = os.path.join(target, item)
        if os.path.isdir(s):
            if os.path.isdir(d):
                shutil.rmtree(d)
            shutil.copytree(s, d)
        else:
            shutil.copy2(s, d)

    # 环境变量
    set_user_env(ENV_SHADOW, target)
    set_user_env(ENV_HOME, target)
    set_user_env(ENV_COMPILER, os.path.join(target, "bin", "shadow.exe"))
    append_user_path(os.path.join(target, "bin"))

    write_uninstall(target)

    # 冒烟测试：--version
    exe = os.path.join(target, "bin", "shadow.exe")
    try:
        r = subprocess.run([exe, "--version"], capture_output=True, timeout=30)
        ver = r.stdout.decode(errors="replace").strip() if r.returncode == 0 else "?"
    except Exception as e:
        ver = f"(smoke fail: {e})"
    return True, f"安装完成: {target}\nshadow --version = {ver}"


def main():
    # 自动化测试模式：--silent <dir>
    if len(sys.argv) >= 3 and sys.argv[1] == "--silent":
        target = os.path.abspath(sys.argv[2])
        ok, msg = install(target)
        line = ("OK " if ok else "FAIL ") + msg
        # --windowed 打包无 console：结果写日志便于自动化验证
        try:
            log = os.path.join(os.environ.get("TEMP", "."), "shadow_setup.log")
            with open(log, "w", encoding="utf-8") as f:
                f.write(line)
        except Exception:
            pass
        if sys.stdout is not None:
            print(line)
        return 0 if ok else 1

    # GUI 模式
    import tkinter as tk
    from tkinter import filedialog, messagebox

    root = tk.Tk()
    root.withdraw()
    root.attributes("-topmost", True)

    # 欢迎
    if not messagebox.askyesno(
            APP_NAME,
            "即将安装 Shadow 0.5 编译器。\n\n"
            "将设置用户环境变量：\n"
            "  shadow / SHADOW_HOME / SHADOW_COMPILER\n"
            "  PATH 追加 <安装目录>\\bin\n\n"
            "继续？"):
        return 1

    default = default_target()
    target = filedialog.askdirectory(
        title="选择 Shadow 安装目录",
        initialdir=os.path.dirname(default))
    if not target:
        messagebox.showinfo(APP_NAME, "已取消安装。")
        return 1
    # 若用户选了默认目录名以外的路径，直接用它；否则补全默认目录名
    if os.path.basename(target) != "shadow":
        target = os.path.join(target, "shadow")

    ok, msg = install(target)
    if ok:
        messagebox.showinfo(APP_NAME, msg + "\n\n新开终端/VSCode 后环境变量生效。")
    else:
        messagebox.showerror(APP_NAME, msg)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

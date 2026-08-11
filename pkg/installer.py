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
ENV_LLVM_HOME = "LLVM_HOME"    # 打包的 LLVM 工具链根（llc/clang++/lld-link 所在）
ENV_LIB = "LIB"                # lld 搜索库路径（打包的 CRT/系统库）


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


def append_user_env(name, value):
    """把 value 追加到用户级环境变量 name（旧值保留，去重后写回）。返回新值。"""
    import winreg
    with winreg.OpenKey(winreg.HKEY_CURRENT_USER, "Environment", 0,
                        winreg.KEY_READ | winreg.KEY_SET_VALUE) as key:
        try:
            cur, _ = winreg.QueryValueEx(key, name)
        except FileNotFoundError:
            cur = ""
        parts = [p for p in str(cur).split(";") if p]
        if value not in parts:
            parts.append(value)
            new = ";".join(parts)
            winreg.SetValueEx(key, name, 0, winreg.REG_EXPAND_SZ, new)
    try:
        import ctypes
        ctypes.windll.user32.SendMessageTimeoutW(
            0xFFFF, 0x001A, 0, "Environment", 0x0002, 5000, None)
    except Exception:
        pass
    return new


def remove_user_env(name, remove_part):
    """从用户级环境变量 name 中移除 remove_part（其余保留）。"""
    import winreg
    try:
        with winreg.OpenKey(winreg.HKEY_CURRENT_USER, "Environment", 0,
                            winreg.KEY_READ | winreg.KEY_SET_VALUE) as key:
            try:
                cur, _ = winreg.QueryValueEx(key, name)
            except FileNotFoundError:
                return
            parts = [p for p in str(cur).split(";") if p and p != remove_part]
            new = ";".join(parts)
            if new:
                winreg.SetValueEx(key, name, 0, winreg.REG_EXPAND_SZ, new)
            else:
                winreg.DeleteValue(key, name)
    except Exception:
        pass


def append_user_path(entry):
    """把 entry 追加到用户 PATH（读旧值去重后写回）。"""
    append_user_env("Path", entry)


def write_uninstall(target):
    """写卸载脚本到安装目录：清环境变量 + 删安装目录。"""
    un = os.path.join(target, "uninstall.cmd")
    lines = [
        "@echo off",
        "rem Shadow 0.5 卸载脚本",
        'echo 正在清除用户环境变量（shadow / SHADOW_HOME / SHADOW_COMPILER / LLVM_HOME / LIB / PATH）...',
        'powershell -NoProfile -Command "[Environment]::SetEnvironmentVariable(\'shadow\',\'\',\'User\'); [Environment]::SetEnvironmentVariable(\'SHADOW_HOME\',\'\',\'User\'); [Environment]::SetEnvironmentVariable(\'SHADOW_COMPILER\',\'\',\'User\'); [Environment]::SetEnvironmentVariable(\'LLVM_HOME\',\'\',\'User\')" >nul 2>&1',
        'powershell -NoProfile -Command "$lib=[Environment]::GetEnvironmentVariable(\'LIB\',\'User\'); if($lib){ $new=($lib -split \';\' | Where-Object { $_ -ne \'%TARGET%llvm\\crt\' -and $_ -ne \'' + target.replace("'", "''") + '\\llvm\\crt\' }) -join \';\'; [Environment]::SetEnvironmentVariable(\'LIB\',$new,\'User\') }" >nul 2>&1',
        'powershell -NoProfile -Command "$p=[Environment]::GetEnvironmentVariable(\'Path\',\'User\'); if($p){ $new=($p -split \';\' | Where-Object { $_ -ne \'%TARGET%bin\' -and $_ -ne \'' + target.replace("'", "''") + '\\bin\' -and $_ -ne \'%TARGET%llvm\\bin\' -and $_ -ne \'' + target.replace("'", "''") + '\\llvm\\bin\' }) -join \';\'; [Environment]::SetEnvironmentVariable(\'Path\',$new,\'User\') }" >nul 2>&1',
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
    # LLVM 工具链（打包内嵌，--run 链接需要 LLVM_HOME + LIB）
    set_user_env(ENV_LLVM_HOME, os.path.join(target, "llvm"))
    append_user_env(ENV_LIB, os.path.join(target, "llvm", "crt"))
    append_user_path(os.path.join(target, "bin"))
    append_user_path(os.path.join(target, "llvm", "bin"))

    write_uninstall(target)

    # 冒烟测试：--version
    exe = os.path.join(target, "bin", "shadow.exe")
    try:
        r = subprocess.run([exe, "--version"], capture_output=True, timeout=30)
        ver = r.stdout.decode(errors="replace").strip() if r.returncode == 0 else "?"
    except Exception as e:
        ver = f"(smoke fail: {e})"
    return True, f"安装完成: {target}\nshadow --version = {ver}"


def msg_box(title, text, kind="info"):
    """系统原生消息框（ctypes，不依赖 tkinter）。kind: info/error/yesno。"""
    import ctypes
    MB_OK = 0x0
    MB_YESNO = 0x4
    MB_ICONINFORMATION = 0x40
    MB_ICONERROR = 0x10
    flags = MB_ICONINFORMATION
    if kind == "error":
        flags = MB_ICONERROR
    elif kind == "yesno":
        flags = MB_YESNO | MB_ICONINFORMATION
    r = ctypes.windll.user32.MessageBoxW(None, text, title, flags)
    return r == 6  # IDYES


def pick_folder(title, initial_dir):
    """系统原生目录选择框（SHBrowseForFolder，ctypes）。返回路径或空串。"""
    import ctypes
    from ctypes import wintypes

    BIF_RETURNONLYFSDIRS = 0x0001
    BIF_NEWDIALOGSTYLE = 0x0040
    BIF_EDITBOX = 0x0010

    class BROWSEINFO(ctypes.Structure):
        _fields_ = [
            ("hwndOwner", wintypes.HWND),
            ("pidlRoot", ctypes.c_void_p),
            ("pszDisplayName", wintypes.LPWSTR),
            ("lpszTitle", wintypes.LPWSTR),
            ("ulFlags", wintypes.UINT),
            ("lpfn", ctypes.c_void_p),
            ("lParam", ctypes.c_void_p),
            ("iImage", ctypes.c_int),
        ]

    shell32 = ctypes.windll.shell32
    ole32 = ctypes.windll.ole32
    # 三个 API 都要显式声明 argtypes/restype：64 位下指针值若按默认 c_int 转换会
    # 报 "OverflowError: int too long to convert"（argtypes 需在 BROWSEINFO 定义之后设置）
    shell32.SHBrowseForFolderW.argtypes = [ctypes.POINTER(BROWSEINFO)]
    shell32.SHBrowseForFolderW.restype = ctypes.c_void_p
    shell32.SHGetPathFromIDListW.argtypes = [ctypes.c_void_p, ctypes.c_wchar_p]
    shell32.SHGetPathFromIDListW.restype = wintypes.BOOL
    ole32.CoTaskMemFree.argtypes = [ctypes.c_void_p]
    buf = ctypes.create_unicode_buffer(512)
    bi = BROWSEINFO()
    bi.hwndOwner = None
    bi.pidlRoot = None
    bi.pszDisplayName = ctypes.cast(buf, ctypes.c_wchar_p)
    bi.lpszTitle = title
    bi.ulFlags = BIF_RETURNONLYFSDIRS | BIF_NEWDIALOGSTYLE | BIF_EDITBOX
    bi.lpfn = None
    bi.lParam = None
    bi.iImage = 0
    pidl = shell32.SHBrowseForFolderW(ctypes.byref(bi))
    if not pidl:
        return ""
    try:
        path = ctypes.create_unicode_buffer(1024)
        if shell32.SHGetPathFromIDListW(pidl, path):
            return path.value
        return ""
    finally:
        ole32.CoTaskMemFree(pidl)


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

    # GUI 模式（ctypes 原生控件，无第三方依赖）
    if not msg_box(APP_NAME,
                   "即将安装 Shadow 0.5 编译器（含 LLVM 工具链）。\n\n"
                   "将设置用户环境变量：\n"
                   "  shadow / SHADOW_HOME / SHADOW_COMPILER\n"
                   "  LLVM_HOME（内嵌工具链）/ LIB（内嵌 CRT 库）\n"
                   "  PATH 追加 <安装目录>\\bin 与 <安装目录>\\llvm\\bin\n\n"
                   "继续？", "yesno"):
        return 1

    default = default_target()
    target = pick_folder("选择 Shadow 安装目录", os.path.dirname(default))
    if not target:
        msg_box(APP_NAME, "已取消安装。")
        return 1
    # 若用户选了默认目录名以外的路径，直接用它；否则补全默认目录名
    if os.path.basename(target) != "shadow":
        target = os.path.join(target, "shadow")

    ok, msg = install(target)
    if ok:
        msg_box(APP_NAME, msg + "\n\n新开终端/VSCode 后环境变量生效。")
    else:
        msg_box(APP_NAME, msg, "error")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())

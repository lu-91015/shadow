#!/usr/bin/env bash
# 收集 LLVM 最小工具链 + CRT 库 → pkg/shadow/llvm/（安装器随包分发）。
# 运行前提：打包机装有 LLVM clang+llvm-x64-msvc（D:/llvm/...）与 Visual Studio 2022 + Windows SDK。
#   --run 链接链路（main_link_exe）：llc（IR→obj）+ clang++（driver）+ lld-link（链接器）
#   + LLVM-C.lib/LLVM-C.dll（shadow.exe 编译期依赖）+ MSVC 静态 CRT（/MT）+ UCRT + 系统库。
# 目标机器无需安装 VS/SDK：clang 探测不到 VS 时 lld 从 LIB 环境变量找库（installer 已设）。
set -u
R="$(cd "$(dirname "$0")/.." && pwd)"
DST="$R/pkg/shadow/llvm"

LLVM="D:/llvm/clang+llvm-22.1.0-x86_64-pc-windows-msvc"
MSVC="/c/Program Files/Microsoft Visual Studio/2022/Professional/VC/Tools/MSVC/14.36.32532/lib/x64"
UCRT="/c/Windows Kits/10/Lib/10.0.22000.0/ucrt/x64"
UM="/c/Windows Kits/10/Lib/10.0.22000.0/um/x64"

if [ ! -d "$LLVM/bin" ]; then echo "[X] LLVM 缺失: $LLVM"; exit 1; fi
if [ ! -f "$MSVC/libcmt.lib" ]; then echo "[X] MSVC CRT 缺失: $MSVC"; exit 1; fi
if [ ! -f "$UCRT/libucrt.lib" ]; then echo "[X] UCRT 缺失: $UCRT"; exit 1; fi
if [ ! -f "$UM/kernel32.Lib" ]; then echo "[X] Windows SDK um 缺失: $UM"; exit 1; fi

rm -rf "$DST"
mkdir -p "$DST/bin" "$DST/lib/clang/22/lib/windows" "$DST/crt"

echo "[llvm] 复制工具链 bin/（LLVM-C.dll llc clang++ lld-link）..."
cp -f "$LLVM/bin/LLVM-C.dll" "$LLVM/bin/llc.exe" "$LLVM/bin/clang++.exe" "$LLVM/bin/lld-link.exe" "$DST/bin/"
cp -f "$LLVM/lib/LLVM-C.lib" "$DST/lib/"
cp -f "$LLVM/lib/clang/22/lib/windows/clang_rt.builtins-x86_64.lib" "$DST/lib/clang/22/lib/windows/"
# LLVM-C.dll 同时放 <pkg>/bin/（与 shadow.exe 同目录，运行时 DLL 搜索第一优先）
cp -f "$LLVM/bin/LLVM-C.dll" "$R/pkg/shadow/bin/LLVM-C.dll"

echo "[llvm] 复制 CRT 库（MSVC 静态 /MT + UCRT + 系统库）..."
for f in libcmt.lib libcpmt.lib libvcruntime.lib oldnames.lib; do
    cp -f "$MSVC/$f" "$DST/crt/"
done
for f in libucrt.lib ucrt.lib; do
    cp -f "$UCRT/$f" "$DST/crt/"
done
# lld MSVC 模式默认 defaultlib 的常见系统库（覆盖 shadow rt 层 + 用户程序可能用到的）
for f in uuid.lib ole32.lib shell32.lib gdi32.lib winspool.lib comdlg32.lib \
         oleaut32.lib user32.lib advapi32.lib version.lib ws2_32.lib \
         kernel32.lib ntdll.lib bcrypt.lib winmm.lib; do
    cp -f "$UM/$f" "$DST/crt/"
done

echo "[llvm] 完成: $DST"
du -sh "$DST"

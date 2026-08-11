#!/usr/bin/env bash
# 一键打包 shadow-0.5.0-setup.exe
# 用法: bash pkg/make_setup.sh
set -e
R="$(cd "$(dirname "$0")/.." && pwd)"
R_WIN="$(cygpath -w "$R")"   # Windows 风格路径（pyinstaller 不认 /c/...）
# Python 解释器：CI 用 PYTHON 环境变量覆盖；本地默认 venv（已装 pyinstaller）
VPY="${PYTHON:-C:/Users/cxyu/.workbuddy/binaries/python/envs/default/Scripts/python.exe}"
TS=$(date +%s)
cd "$R"

echo "[1/4] 刷新载荷（build/ 最新产物 → pkg/shadow/）..."
cp -f build/shadow.exe pkg/shadow/bin/shadow.exe
cp -f build/rt/*.o pkg/shadow/bin/rt/
cp -f bootstrap/miniz.o bootstrap/runtime_for_selfhost.o pkg/shadow/bin/bootstrap/
cp -f src/runtime/runtime_lib.shadow pkg/shadow/src/runtime/
cp -f tools/link_rt.py tools/verify_fixed_point.py pkg/shadow/tools/

echo "[2/4] 收集 LLVM 最小工具链 + CRT 库（→ pkg/shadow/llvm/）..."
bash pkg/stage_llvm.sh || exit 1

echo "[3/4] pyinstaller 打包（workpath=build_tmp_$TS）..."
# 先轮换旧产物，避免覆盖被安全机制拦截
[ -f pkg/dist/shadow-0.5.0-setup.exe ] && mv -f pkg/dist/shadow-0.5.0-setup.exe "pkg/dist/shadow-0.5.0-setup.exe.old"
"$VPY" -m PyInstaller --onefile --windowed --name shadow-0.5.0-setup \
  --distpath "pkg/dist" --workpath "pkg/build_tmp_$TS" --specpath "pkg" \
  --add-data "$R_WIN/pkg/shadow;shadow" "$R_WIN/pkg/installer.py"

echo "[4/4] 完成: pkg/dist/shadow-0.5.0-setup.exe"
ls -la pkg/dist/shadow-0.5.0-setup.exe

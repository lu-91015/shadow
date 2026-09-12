#!/usr/bin/env bash
# 一键打包 shadow-<VER>-windows-x86_64.exe（自包含运行环境，支持 shadow -run）。
# 文件名含版本号（从 build/shadow.exe --version 读取），与 --version 输出一致。
#
# 载荷布局（pkg/shadow/ → 安装到 <target>/）：
#   bin/shadow.exe            编译器本体
#   bin/LLVM-C.dll            LLVM-C 运行时（与 exe 同目录，DLL 搜索第一优先）
#   bin/rt/rt_*.o             9 个自研系统调用层对象（shadow -run 链接用）
#   bin/rt/miniz.o    压缩/自举辅助对象（shadow -run 链接用）
#   src/main.shadow         编译器源码根标记（main_load_combined 用它校验
#                            <root>/src 是真实源码根，进而稳定定位
#                            <root>/src/runtime/runtime_lib.shadow；
#                            缺它则只能靠 CWD 相对路径兜底，从 PATH 任意目录
#                            运行会解析失败。与 Linux 包布局对齐。）
#   src/runtime/runtime_lib.shadow   编译器内置 runtime
#   src/std/                 标准库：iter.shadow + 根 std/<pkg> 全部真实包
#                            （合并后，安装到任意目录 import std.* 都能经 main_std_root 解析）
#   llvm/                    LLVM 最小工具链 + CRT/系统库（stage_llvm.sh 收集）
#                            llc/clang++/lld-link/LLVM-C.{dll,lib} + 静态 CRT + UCRT + 系统库
#
# 用法: bash pkg/make_setup.sh
set -e
R="$(cd "$(dirname "$0")/.." && pwd)"
R_WIN="$(cygpath -w "$R")"   # Windows 风格路径（pyinstaller 不认 /c/...）
# Python 解释器：CI 用 PYTHON 环境变量覆盖；本地默认 venv（已装 pyinstaller）
VPY="${PYTHON:-C:/Users/cxyu/.workbuddy/binaries/python/envs/default/Scripts/python.exe}"
TS=$(date +%s)
cd "$R"

# 版本号：从刚编译好的本体读取，保证安装包文件名与 `shadow --version` 完全一致。
VERSION_RAW="$(build/shadow.exe --version 2>/dev/null | head -1)"
VERSION="${VERSION_RAW#shadow }"
VERSION="${VERSION%%[[:space:]]}"
[ -z "$VERSION" ] && VERSION="0.5.2"
INSTALLER="shadow-${VERSION}-windows-x86_64.exe"
echo "目标版本: $VERSION  ->  pkg/dist/$INSTALLER"

PAY="$R/pkg/shadow"

echo "[1/5] 建立载荷目录结构（bin/ src/）..."
# 注：不用 rm -rf 清理旧载荷——部分环境下删除会被安全机制拦截导致脚本中断；
# 改为 mkdir -p + cp -f 幂等覆盖，残留的未引用文件对链接/模块解析无害。
mkdir -p "$PAY/bin/rt" "$PAY/src/std" "$PAY/src/runtime"

echo "[2/5] 复制编译器与运行时链接对象..."
cp -f build/shadow.exe "$PAY/bin/shadow.exe"
# shadow -run 链接所需的 runtime 对象（main_link_exe 精确列表）：
# 统一用户程序 runtime（Windows 与 Linux 同源 cpp 实现）。
# runtime_for_selfhost_user.o 由 build/rt/runtime_for_selfhost.o 经 objcopy 生成
# （冲突符号加 __cpp_ 前缀、GC 保留原名），构建入口见 tools/build_shadow.shadow。
for o in runtime_for_selfhost_user miniz sys_exec_cpa rt_proc_spawn rt_zip shadow_index cxa_atexit_shim; do
  cp -f "build/rt/$o.o" "$PAY/bin/rt/$o.o"
done

echo "[3/5] 复制编译器自带源码（main.shadow 源码根标记 + runtime_lib + 标准库 std.*）..."
# 复制 src/main.shadow：main_load_combined 用它校验 <root>/src 是真实编译器源码根，
# 命中后稳定取 <root>/src/runtime/runtime_lib.shadow。与 Linux 包对齐，避免"只能装在 CWD 运行"。
cp -f src/main.shadow "$PAY/src/"
cp -f src/runtime/runtime_lib.shadow "$PAY/src/runtime/"
cp -f src/std/iter.shadow "$PAY/src/std/"
# 真实标准库包（仓库根 std/<pkg>/）合并进 src/std/，
# 使安装后从任意目录 import std.* 都能经 main_std_root（<exe>/../src）解析。
for d in "$R"/std/*/; do
  [ -d "$d" ] && cp -rf "$d" "$PAY/src/std/"
done

echo "[4/5] 收集 LLVM 最小工具链 + CRT 库（→ pkg/shadow/llvm/）..."
bash pkg/stage_llvm.sh || exit 1

echo "[5/5] pyinstaller 打包（workpath=build_tmp_$TS）..."
# 先轮换旧产物，避免覆盖被安全机制拦截
[ -f "pkg/dist/$INSTALLER" ] && mv -f "pkg/dist/$INSTALLER" "pkg/dist/$INSTALLER.old"
"$VPY" -m PyInstaller --onefile --windowed --name "${INSTALLER%.exe}" \
  --distpath "pkg/dist" --workpath "pkg/build_tmp_$TS" --specpath "pkg" \
  --add-data "$R_WIN/pkg/shadow;shadow" "$R_WIN/pkg/installer.py"

echo "完成: pkg/dist/$INSTALLER"
ls -la "pkg/dist/$INSTALLER"

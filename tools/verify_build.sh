#!/usr/bin/env bash
# Shadow 0.5 —— 构建产物指纹校验（把"自举脚本"钉死成可验证的约束）
#
# 为什么需要：自举 fixed-point 只逐字节比 stage2.ll / stage3.ll，那是**编译器输出的 IR**。
# 链接进 build/shadow.exe 的 runtime（rt/ 与 build/rt/ 的 .o）换了，IR 一个字不变、
# fixed-point 照样 OK —— 这正是 c0819b2 事故的机理（源码修对、.o 陈旧、误判"修了没用"）。
# 本脚本对最终二进制做 sha256 比对，补上这个盲区。
#
# 用法（仓库根）：
#   bash tools/verify_build.sh              # 校验；不一致则退出码 1
#   bash tools/verify_build.sh --update     # 用当前产物刷新 manifest（然后提交它）
#   BUILD_ALLOW_DRIFT=1 bash tools/verify_build.sh   # 只报告不失败（换工具链/跨机器时用）
set -uo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"; cd "$ROOT"
MAN="tools/build_manifest.sha256"
ARTIFACTS=(build/shadow.exe build/run_tests.exe)

if [ "${1:-}" = "--update" ]; then
  : > "$MAN"
  {
    echo "# Shadow 0.5 构建产物指纹（由 tools/verify_build.sh --update 生成，勿手改）"
    echo "# 含义：在 tools/bootstrap.sh 记录的固定工具链与入库 crt 导入库下，"
    echo "#       下列二进制必须逐字节可重现；不一致即存在 runtime/.o 或链接输入漂移。"
    echo "# shadow 版本: $(build/shadow.exe --version 2>/dev/null | tr -d '\r')"
    for a in "${ARTIFACTS[@]}"; do [ -f "$a" ] && sha256sum "$a" | tr -d '*'; done
  } > "$MAN"
  echo "[verify] manifest 已刷新 -> $MAN"
  grep -v '^#' "$MAN" | sed 's/^/    /'
  exit 0
fi

if [ ! -f "$MAN" ]; then
  echo "[verify] 跳过：无 $MAN（用 bash tools/verify_build.sh --update 生成）"
  exit 0
fi

bad=0; checked=0; missing=0
while read -r hash path; do
  case "$hash" in ''|\#*) continue ;; esac
  path="${path#\*}"   # Windows sha256sum 会写成 "hash *path"
  checked=$((checked + 1))
  if [ ! -f "$path" ]; then
    echo "  [skip] $path 不存在（尚未构建）"; missing=$((missing + 1)); continue
  fi
  actual=$(sha256sum "$path" | cut -d' ' -f1)
  if [ "$actual" = "$hash" ]; then
    echo "  [ok]   $path  $(printf '%s' "$hash" | cut -c1-16)…"
  else
    echo "  [X]    $path 指纹不符"
    echo "         manifest: $(printf '%s' "$hash"   | cut -c1-16)…"
    echo "         当前产物: $(printf '%s' "$actual" | cut -c1-16)…"
    bad=$((bad + 1))
  fi
done < <(awk '{print $1, $2}' "$MAN" | grep -v '^#')

if [ "$bad" -eq 0 ] && [ $((checked - missing)) -gt 0 ]; then
  echo "VERIFY_OK 产物指纹一致（实校验 $((checked - missing)) 项，跳过 $missing）"
  exit 0
fi
if [ "$bad" -eq 0 ]; then
  echo "[X] $MAN 列出 $checked 项，但一个都不存在 —— 什么都没校验到，不能算通过。"
  echo "    先跑 bash tools/bootstrap.sh（及 bash tools/build.sh）产出二进制。"
  [ "${BUILD_ALLOW_DRIFT:-0}" = "1" ] && { echo "    BUILD_ALLOW_DRIFT=1 → 放行"; exit 0; }
  exit 1
fi

echo "[X] $bad 项产物与 $MAN 不符 —— 存在链接输入漂移（runtime/.o、crt 或工具链）。"
if [ "${BUILD_ALLOW_DRIFT:-0}" = "1" ]; then
  echo "    BUILD_ALLOW_DRIFT=1 已设 → 不失败。"
  exit 0
fi
echo "    若改动是有意的：bash tools/verify_build.sh --update  然后提交 manifest。"
echo "    若是非预期的：先查 build/rt/ 下的 .o 是否被换过（git diff HEAD -- rt tools/build_manifest.sha256）。"
exit 1

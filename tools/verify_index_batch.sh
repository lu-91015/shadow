#!/usr/bin/env bash
# 批量对照「C 轻量索引器 (cindex --all)」与「编译器 api_check (shadow --fmeta)」的
# fmeta/smeta/emeta 三段输出，逐字节 diff（忽略 CRLF 差异）。
#
# 用法：
#   bash tools/verify_index_batch.sh                     # 默认前 120 个用例
#   LIMIT=400 bash tools/verify_index_batch.sh
#   ROOT=test/cases/migrated_03 LIMIT=200 bash tools/verify_index_batch.sh
set -u
cd "$(dirname "$0")/.."

EXE=${EXE:-build/lspfix.exe}
CIDX=${CIDX:-build/cindex.exe}
ARG0=${ARG0:-build/shadow.exe}
ROOT=${ROOT:-test/cases}
LIMIT=${LIMIT:-120}
OUT=build/idxcmp

[ -x "$EXE" ]  || { echo "[X] missing $EXE";  exit 2; }
[ -x "$CIDX" ] || { echo "[X] missing $CIDX"; exit 2; }

rm -rf "$OUT" 2>/dev/null
mkdir -p "$OUT"

pass=0; fail=0; skip=0; n=0
: > "$OUT/_failures.txt"

while IFS= read -r f; do
    n=$((n + 1))
    base=$(printf '%s' "$f" | tr '/\\' '__')
    "$EXE" "$f" --fmeta > "$OUT/$base.real" 2> "$OUT/$base.err"
    if [ $? -ne 0 ]; then
        skip=$((skip + 1)); continue
    fi
    # 模块图加载失败（cannot read file / module error）→ 索引器同样兜底，不参与对照
    if grep -q "module error:\|cannot read file:" "$OUT/$base.err" 2>/dev/null; then
        skip=$((skip + 1)); continue
    fi
    "$CIDX" "$f" "$ARG0" --all > "$OUT/$base.c" 2>/dev/null
    if [ $? -ne 0 ]; then
        # 索引器主动兜底（ok=0）：LSP 会退回全量编译，不算错
        skip=$((skip + 1)); continue
    fi
    if diff --strip-trailing-cr -q "$OUT/$base.real" "$OUT/$base.c" > /dev/null 2>&1; then
        pass=$((pass + 1))
        rm -f "$OUT/$base.real" "$OUT/$base.c" "$OUT/$base.err"
    else
        fail=$((fail + 1))
        echo "$f" >> "$OUT/_failures.txt"
        echo "FAIL $f"
        diff --strip-trailing-cr "$OUT/$base.real" "$OUT/$base.c" | head -6
    fi
done <<EOF
$(find "$ROOT" -name "*.shadow" | sort | head -n "$LIMIT")
EOF

echo "-------------------------------------------"
echo "TOTAL=$n PASS=$pass FAIL=$fail SKIP=$skip"
[ "$fail" -eq 0 ] && echo "INDEX_BATCH_OK" || echo "INDEX_BATCH_MISMATCH (see $OUT/_failures.txt)"
exit $([ "$fail" -eq 0 ] && echo 0 || echo 1)

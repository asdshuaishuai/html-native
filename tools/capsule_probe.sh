#!/usr/bin/env bash
# tools/capsule_probe.sh — .hnapp 胶囊 v2(ZIP 容器)断言薄封装。
#
# 引擎级: hn_zip 往返逐字节一致/查找/提取(.c 全断言);
# 容器外双保险: 产出的 ZIP 交给**系统 unzip -t**校验结构 + CRC ——
# 引擎写出的包, 生态工具也必须认。
#
# 用法: bash tools/capsule_probe.sh

set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/_engine_sources.sh"

LABEL="capsule_probe.sh"
hn_sources_full
HN_SOURCES=("$HN_TOOLS_DIR/capsule_probe.c" ${HN_SOURCES[@]+"${HN_SOURCES[@]}"})
for w in "${HN_WASM3[@]}"; do HN_SOURCES+=("$HN_WASM3_DIR/$w"); done
hn_sources_check "${HN_SOURCES[@]+"${HN_SOURCES[@]}"}" || exit 1

FT_INC=""
FT_LIB=()
if [ -d /opt/homebrew/include/freetype2 ]; then
  FT_INC="-I/opt/homebrew/include/freetype2"
  FT_LIB=(-L/opt/homebrew/lib -lfreetype)
fi

OUT="${TMPDIR:-/tmp}/hn_capsule_probe_bin"
echo "$LABEL: compile"
"$HN_CC" -std=c99 -O2 -Wall -ffp-contract=off \
    -I "$HN_ENG/include" -I "$HN_ENG" -I "$HN_REPO_ROOT/platform" -I "$HN_WASM3_DIR" \
    ${FT_INC:+"$FT_INC"} \
    "${HN_SOURCES[@]}" "$HN_QUICKJS_A" \
    ${FT_LIB[@]+"${FT_LIB[@]}"} -lm -o "$OUT"

echo "$LABEL: run(engine 断言)"
"$OUT"

echo "$LABEL: 外部校验(引擎产出的包, 生态工具必须认)"
ENGZ="/tmp/hn-capsule-probe.hnapp"
[ -s "$ENGZ" ] || { echo "  X 引擎产物不存在"; exit 1; }
if command -v unzip >/dev/null 2>&1; then
  unzip -t "$ENGZ" >/dev/null && echo "  v unzip -t 结构与 CRC 校验通过"
fi
python3 - "$ENGZ" <<'PY'
import sys, zipfile
z = zipfile.ZipFile(sys.argv[1])
assert z.testzip() is None, "CRC fail"
mb = z.read("manifest.json").decode()
assert '"version":2' in mb, "manifest 内容错"
ih = z.read("index.html").decode()
assert "CAPSULE-V2" in ih, "入口页内容错"
print("  v python zipfile 读回 + 内容校验通过")
PY
rm -f "$ENGZ"

printf '%s\n' "$LABEL: PASS"

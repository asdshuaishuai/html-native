#!/usr/bin/env bash
# tools/reshoot_docs.sh — 用当前引擎重拍 docs/screenshots 六张官网截图
# (ISSUES-1 待办 1: 旧图摄于圆角 SDF 修复前, 几何口径已漂)。
#
# 手法: tools/reshoot_docs.c 以 hn_rt_set_scale(2)+hn_rt_shot 按"真窗
# backing"同口径出 @2x 图(hidpi_probe ③ 同一机制, 真 2x 光栅非拉伸)。
#
# 用法: bash tools/reshoot_docs.sh

set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/_engine_sources.sh"

LABEL="reshoot_docs.sh"
hn_sources_soft
HN_SOURCES=("$HN_TOOLS_DIR/reshoot_docs.c" ${HN_SOURCES[@]+"${HN_SOURCES[@]}"})
hn_sources_check "${HN_SOURCES[@]}"

FT_INC=""
FT_LIB=()
if [ -d /opt/homebrew/include/freetype2 ]; then
  FT_INC="-I/opt/homebrew/include/freetype2"
  FT_LIB=(-L/opt/homebrew/lib -lfreetype)
fi

OUT="${TMPDIR:-/tmp}/hn_reshoot_docs_bin"
echo "$LABEL: compile"
# shellcheck disable=SC2086
"$HN_CC" -std=c99 -O2 -ffp-contract=off -I "$HN_ENG/include" -I "$HN_ENG" -I "$HN_RT_DIR" \
    ${FT_INC:+"$FT_INC"} \
    "${HN_SOURCES[@]}" "$HN_RT_DIR/hn_rt.c" "$HN_RT_DIR/hn_sys.c" "$HN_RT_DIR/hn_http.c" \
    ${FT_LIB[@]+"${FT_LIB[@]}"} -lm -o "$OUT"

echo "$LABEL: shoot"
DOCS_OUT="$PWD/docs/screenshots" "$OUT"
rm -f "$OUT"
printf '%s\n' "$LABEL: PASS(6 张已覆盖)"

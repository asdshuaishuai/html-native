#!/usr/bin/env bash
# tools/render_probe.sh — 光栅层回归(render_probe.c)薄封装。
#
# 覆盖: 圆角盒填充/描边环(CSS border 内带)/圆形裁剪边缘 AA/圆边 4-tap
# 超采样等光栅断言 —— 全部像素级, 语义在 .c 里, 本脚本只编译运行透传
# 退出码(与 md_probe/cubism_mask_probe 同一链接口径)。
#
# 用法: bash tools/render_probe.sh

set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/_engine_sources.sh"

LABEL="render_probe.sh"
hn_sources_soft
HN_SOURCES=("$HN_TOOLS_DIR/render_probe.c" ${HN_SOURCES[@]+"${HN_SOURCES[@]}"})
hn_sources_check "${HN_SOURCES[@]+"${HN_SOURCES[@]}"}" || exit 1

FT_INC=""
FT_LIB=()
if [ -d /opt/homebrew/include/freetype2 ]; then
  FT_INC="-I/opt/homebrew/include/freetype2"
  FT_LIB=(-L/opt/homebrew/lib -lfreetype)
fi

OUT="${TMPDIR:-/tmp}/hn_render_probe_bin"
echo "$LABEL: compile"
"$HN_CC" -std=c99 -O2 -Wall -ffp-contract=off \
    -I "$HN_ENG/include" -I "$HN_ENG" \
    ${FT_INC:+"$FT_INC"} \
    "${HN_SOURCES[@]}" \
    ${FT_LIB[@]+"${FT_LIB[@]}"} -lm -o "$OUT"

echo "$LABEL: run"
"$OUT"
rm -f "$OUT"
printf '%s\n' "$LABEL: PASS"

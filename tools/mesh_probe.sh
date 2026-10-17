#!/usr/bin/env bash
# tools/mesh_probe.sh — 网格变形(Live2D 类原语)断言薄封装。
#
# mesh_probe.c 验证引擎交付的"贴图映射到可变形网格 + 逐顶点驱动"原语
# (解析/anchor/三带摆幅/phase/look/idle), 语义全在 .c 里 —— 本脚本只
# 编译运行透传退出码(此前只能手工编译, 无运行器)。
#
# 用法: bash tools/mesh_probe.sh

set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/_engine_sources.sh"

LABEL="mesh_probe.sh"
hn_sources_core
HN_SOURCES=("$HN_TOOLS_DIR/mesh_probe.c" ${HN_SOURCES[@]+"${HN_SOURCES[@]}"})
hn_sources_check "${HN_SOURCES[@]+"${HN_SOURCES[@]}"}" || exit 1

OUT="${TMPDIR:-/tmp}/hn_mesh_probe_bin"
echo "$LABEL: compile"
"$HN_CC" -std=c99 -O2 -Wall -ffp-contract=off \
    -I "$HN_ENG/include" -I "$HN_ENG" \
    "${HN_SOURCES[@]}" -lm -o "$OUT"

echo "$LABEL: run"
"$OUT"
rm -f "$OUT"
printf '%s\n' "$LABEL: PASS"

#!/usr/bin/env bash
# tools/project_probe.sh — 多 html 项目模式断言薄封装。
#
# project_probe.c 验证阶段1"项目 = 目录"的引擎侧语义:
# hn_rt_desc.path 启用项目内导航 / navigate 换装与拒绝 / 页面栈 back /
# 内联文档(daemon 推流)导航不变。项目根 = 探针临时目录 chdir 口径。
#
# 用法: bash tools/project_probe.sh

set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/_engine_sources.sh"

LABEL="project_probe.sh"
hn_sources_full
HN_SOURCES=("$HN_TOOLS_DIR/project_probe.c" "$HN_RT_DIR/hn_rt.c" "$HN_RT_DIR/hn_sys.c" "$HN_RT_DIR/hn_http.c"
            ${HN_SOURCES[@]+"${HN_SOURCES[@]}"})
for w in "${HN_WASM3[@]}"; do HN_SOURCES+=("$HN_WASM3_DIR/$w"); done
hn_sources_check "${HN_SOURCES[@]+"${HN_SOURCES[@]}"}" || exit 1

HN_CFLAGS=(-std=c99 -O2 -Wall -ffp-contract=off)
HN_DEFS=(-DHN_HAVE_QUICKJS)
HN_INCS=(-I "$HN_ENG/include" -I "$HN_ENG" -I "$HN_REPO_ROOT/platform" -I "$HN_RT_DIR" -I "$HN_WASM3_DIR"
         -I "$HN_QUICKJS_INC" -I /opt/homebrew/include/freetype2)
HN_LIBS=("$HN_QUICKJS_A" -L/opt/homebrew/lib -lfreetype -lm)

OUT="${TMPDIR:-/tmp}/hn_project_probe_bin"
echo "$LABEL: compile"
"$HN_CC" "${HN_CFLAGS[@]}" "${HN_DEFS[@]}" "${HN_INCS[@]}" \
    "${HN_SOURCES[@]}" "${HN_LIBS[@]}" -o "$OUT"

echo "$LABEL: run"
"$OUT"
rm -f "$OUT"
printf '%s\n' "$LABEL: PASS"

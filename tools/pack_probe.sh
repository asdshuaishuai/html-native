#!/usr/bin/env bash
# tools/pack_probe.sh — hn pack 打包管线端到端断言薄封装。
# 覆盖: 资产收集/引用改写/meta 注入/兼容性体检/项目目录/.hnapp 胶囊往返。
# 输入为本地夹具(零网络); URL 拉取路径由 hn_http 自身测试覆盖。
#
# 用法: bash tools/pack_probe.sh

set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/_engine_sources.sh"

LABEL="pack_probe.sh"
hn_sources_core
HN_SOURCES=("$HN_TOOLS_DIR/pack_probe.c" "$HN_ENG/hn_pack.c" "$HN_RT_DIR/hn_http.c"
            ${HN_SOURCES[@]+"${HN_SOURCES[@]}"})
hn_sources_check "${HN_SOURCES[@]+"${HN_SOURCES[@]}"}" || exit 1

OUT="${TMPDIR:-/tmp}/hn_pack_probe_bin"
echo "$LABEL: compile"
$HN_CC -std=c99 -O2 -Wall -I "$HN_ENG/include" -I "$HN_ENG" -I "$HN_RT_DIR" -I "$HN_REPO_ROOT/platform" -I /opt/homebrew/include/freetype2 \
    "${HN_SOURCES[@]}" -L/opt/homebrew/lib -lfreetype -lm -o "$OUT"

echo "$LABEL: run"
"$OUT"
rm -f "$OUT"
printf '%s\n' "$LABEL: PASS"

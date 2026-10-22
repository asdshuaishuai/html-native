#!/usr/bin/env bash
# tools/cubism_mask_probe.sh — Cubism 遮罩/反转遮罩发射语义回归薄封装。
#
# 需要 vendor/cubism(libLive2DCubismCore.a)与 examples/assets/miku 官方
# 样例模型; Core 缺席 → skip(可选依赖缺席不是失败)。
#
# 用法: bash tools/cubism_mask_probe.sh

set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/_engine_sources.sh"

LABEL="cubism_mask_probe.sh"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORE_LIB="$ROOT/vendor/cubism/lib/macos-arm64/libLive2DCubismCore.a"
[ -f "$CORE_LIB" ] || CORE_LIB="$(ls vendor/cubism/lib/*/libLive2DCubismCore.a 2>/dev/null | head -1 || true)"
if [ -z "${CORE_LIB:-}" ] || [ ! -f "$CORE_LIB" ]; then
  echo "$LABEL: SKIP(缺 libLive2DCubismCore —— 运行 tools/fetch-cubism-core.sh)"
  exit 0
fi
if [ ! -f "examples/assets/miku/miku_pro/runtime/miku_sample_t04.model3.json" ]; then
  echo "$LABEL: SKIP(缺 miku 夹具模型)"
  exit 0
fi

hn_sources_core
# 合成序列段直渲需要生产光栅器(hnsoft_render) —— hnsoft 入列
HN_SOURCES=("$HN_TOOLS_DIR/cubism_mask_probe.c" "$HN_ENG/hnsoft.c" ${HN_SOURCES[@]+"${HN_SOURCES[@]}"})
hn_sources_check "${HN_SOURCES[@]+"${HN_SOURCES[@]}"}" || exit 1

FT_INC=""
FT_LIB=()
if [ -d /opt/homebrew/include/freetype2 ]; then
  FT_INC="-I/opt/homebrew/include/freetype2"
  FT_LIB=(-L/opt/homebrew/lib -lfreetype)
fi

OUT="${TMPDIR:-/tmp}/hn_cubism_mask_probe_bin"
echo "$LABEL: compile"
"$HN_CC" -std=c99 -O2 -ffp-contract=off -DHN_HAVE_CUBISM \
    -I "$HN_ENG/include" -I "$HN_ENG" -I "$HN_REPO_ROOT/platform" -I "$ROOT/vendor/cubism/include" \
    ${FT_INC:+"$FT_INC"} \
    "${HN_SOURCES[@]}" "$CORE_LIB" \
    ${FT_LIB[@]+"${FT_LIB[@]}"} -lm -o "$OUT"

echo "$LABEL: run"
"$OUT"
rm -f "$OUT"
printf '%s\n' "$LABEL: PASS"

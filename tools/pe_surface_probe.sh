#!/usr/bin/env bash
# tools/pe_surface_probe.sh — hn_pe 双缓冲 surface 尺寸失败回归薄封装。
#
# 需要 vendor/painterengine 在场(PE 池 32MB 静态缓冲是失败注入的前提:
# 超大 surface 的 PX_SurfaceCreate 必然失败)。SDK 缺席 → skip(退出 0,
# 门禁口径: 可选依赖缺席不是失败)。
#
# 用法: bash tools/pe_surface_probe.sh

set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/_engine_sources.sh"

LABEL="pe_surface_probe.sh"
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
PE="$ROOT/vendor/painterengine"
if [ ! -d "$PE/core" ]; then
  echo "$LABEL: SKIP(缺 vendor/painterengine, PE 可选构建不适用)"
  exit 0
fi

hn_sources_soft
# hn_pe.c 已在引擎核心清单(HN_HAVE_PE 缺省构建时为空操作) —— 这里补宏与
# PE 头/库, 让它以"真 PE 在场"口径编译
HN_SOURCES=("$HN_TOOLS_DIR/pe_surface_probe.c" ${HN_SOURCES[@]+"${HN_SOURCES[@]}"})
hn_sources_check "${HN_SOURCES[@]}"

FT_INC=""
FT_LIB=()
if [ -d /opt/homebrew/include/freetype2 ]; then
  FT_INC="-I/opt/homebrew/include/freetype2"
  FT_LIB=(-L/opt/homebrew/lib -lfreetype)
fi

OUT="${TMPDIR:-/tmp}/hn_pe_surface_probe_bin"
CFLAGS=(-std=c99 -O1 -g -fno-omit-frame-pointer -ffp-contract=off)
SAN=()
if [ "${PE_PROBE_ASAN:-0}" = "1" ]; then
  SAN=(-fsanitize=address,undefined)
fi

echo "$LABEL: compile"
"$HN_CC" "${CFLAGS[@]}" ${SAN[@]+"${SAN[@]}"} -DHN_HAVE_PE -I "$HN_ENG/include" -I "$HN_ENG" \
    -I "$PE" -I "$PE/core" ${FT_INC:+"$FT_INC"} \
    "${HN_SOURCES[@]}" "$PE/core/"*.c \
    ${FT_LIB[@]+"${FT_LIB[@]}"} -lm -o "$OUT"

echo "$LABEL: run"
ASAN_OPTIONS="halt_on_error=1:abort_on_error=1" \
UBSAN_OPTIONS="halt_on_error=1:print_stacktrace=1" "$OUT"
rm -f "$OUT"
printf '%s\n' "$LABEL: PASS"

#!/usr/bin/env bash
# build-pe-dev.sh — 带 PainterEngine + canvas_ity/CG + neb3 的 hnapp 本机快速重编
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
. "$ROOT/tools/_engine_sources.sh"
QJ=/opt/homebrew/opt/quickjs
ZIG="${ZIG:-zig}"
PE="$ROOT/vendor/painterengine"
hn_sources_full
ENGINE_SRC=("${HN_SOURCES[@]}" "$HN_ENG/hn_gpu.c")
for f in "${HN_WASM3[@]}"; do ENGINE_SRC+=("$HN_WASM3_DIR/$f"); done
CANVAS_TMP="$(mktemp -d "${TMPDIR:-/tmp}/hn-pe-canvas-XXXXXX")"
trap 'rm -rf "$CANVAS_TMP"' EXIT
# Preserve this development target's enabled-by-default Canvas backends.
HN_CANVAS_ITY="${HN_CANVAS_ITY:-1}" HN_CANVAS_CG="${HN_CANVAS_CG:-1}" \
  hn_canvas_build aarch64-macos "$CANVAS_TMP" "$ZIG" c++
"$ZIG" cc -target aarch64-macos -std=c99 -O2 -ffp-contract=off \
  -I "$HN_ENG/include" -I "$HN_ENG" -I "$ROOT/rt" \
  -I "$ROOT/platform" -I "$ROOT/app" -I "$ROOT/vendor/wasm3" \
  -I "$PE" -I "$PE/core" -I/opt/homebrew/include/freetype2 \
  -I"$QJ/include/quickjs" -DHN_HAVE_QUICKJS -DHN_HAVE_PE \
  ${HN_CANVAS_DEFS[@]+"${HN_CANVAS_DEFS[@]}"} \
  -DHN_MEDIA_PLATFORM_AVFOUNDATION -DHN_VERSION='"pe"' \
  app/hn_app.c app/hn_media_platform.c app/hn_media_synth.c rt/hn_rt.c rt/hn_sys.c rt/hn_http.c \
  platform/hnp_macos.c platform/hnp_media_macos.c platform/hnp_gpu_metal.c \
  "${ENGINE_SRC[@]}" ${HN_CANVAS_OBJECTS[@]+"${HN_CANVAS_OBJECTS[@]}"} \
  "$PE/core/"*.c \
  -L/opt/homebrew/lib -lfreetype "$QJ/lib/quickjs/libquickjs.a" -lm \
  ${HN_CANVAS_LIBS[@]+"${HN_CANVAS_LIBS[@]}"} \
  -o /tmp/hnapp-pe 2>/tmp/hn_pe_err.txt
echo "OK /tmp/hnapp-pe ($(du -k /tmp/hnapp-pe | cut -f1)KB)"

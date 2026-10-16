#!/usr/bin/env bash
# build-cubism-dev.sh — 带 Cubism Core 的 hnapp 本机快速重编
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
. "$ROOT/tools/_engine_sources.sh"
QJ=/opt/homebrew/opt/quickjs
ZIG="${ZIG:-zig}"
hn_sources_full
ENGINE_SRC=("${HN_SOURCES[@]}" "$HN_ENG/hn_gpu.c")
for f in "${HN_WASM3[@]}"; do ENGINE_SRC+=("$HN_WASM3_DIR/$f"); done
CANVAS_TMP="$(mktemp -d "${TMPDIR:-/tmp}/hn-cubism-canvas-XXXXXX")"
trap 'rm -rf "$CANVAS_TMP"' EXIT
hn_canvas_build aarch64-macos "$CANVAS_TMP" "$ZIG" c++
"$ZIG" cc -target aarch64-macos -std=c99 -O2 -ffp-contract=off \
  -I Sources/CHtmlNative/include -I Sources/CHtmlNative -I rt -I platform -I app \
  -I vendor/wasm3 -I vendor/cubism/include -I/opt/homebrew/include/freetype2 \
  -I"$QJ/include/quickjs" -DHN_HAVE_QUICKJS -DHN_HAVE_CUBISM \
  ${HN_CANVAS_DEFS[@]+"${HN_CANVAS_DEFS[@]}"} \
  -DHN_MEDIA_PLATFORM_AVFOUNDATION -DHN_VERSION='"cubism"' \
  app/hn_app.c app/hn_media_platform.c app/hn_media_synth.c rt/hn_rt.c rt/hn_sys.c rt/hn_http.c \
  platform/hnp_macos.c platform/hnp_media_macos.c platform/hnp_gpu_metal.c \
  "${ENGINE_SRC[@]}" ${HN_CANVAS_OBJECTS[@]+"${HN_CANVAS_OBJECTS[@]}"} \
  vendor/cubism/lib/macos-arm64/libLive2DCubismCore.a \
  -L/opt/homebrew/lib -lfreetype "$QJ/lib/quickjs/libquickjs.a" -lm \
  ${HN_CANVAS_LIBS[@]+"${HN_CANVAS_LIBS[@]}"} -o /tmp/hnapp-cubism
echo "OK /tmp/hnapp-cubism ($(du -k /tmp/hnapp-cubism | cut -f1)KB)"

#!/usr/bin/env bash
# build-pe-dev.sh — 带 PainterEngine + canvas_ity + neb3 的 hnapp 本机快速重编
set -euo pipefail
cd "$(dirname "$0")/.."
QJ=/opt/homebrew/opt/quickjs
HN_DIR="$(pwd)"
PE="$HN_DIR/vendor/painterengine"
ENG="Sources/CHtmlNative"

# canvas_ity 后端(C++17, 单头 vendored; 先独立编 .o, 避免 -std=c99 冲突)
zig c++ -target aarch64-macos -std=c++17 -fno-exceptions -fno-rtti -O2 \
  -I "$HN_DIR/Sources/CHtmlNative/include" -I "$HN_DIR/Sources/CHtmlNative" \
  -I "$HN_DIR/vendor/canvas_ity" -c "$ENG/hn_canvas_ity.cpp" \
  -o "$HN_DIR/hn_canvas_ity.o"

zig cc -target aarch64-macos -std=c99 -O2 -ffp-contract=off \
  -I "$HN_DIR/Sources/CHtmlNative/include" -I "$HN_DIR/Sources/CHtmlNative" -I "$HN_DIR/rt" \
  -I "$HN_DIR/platform" -I "$HN_DIR/app" -I "$HN_DIR/vendor/wasm3" \
  -I "$HN_DIR/vendor/canvas_ity" -I "$HN_DIR/vendor/painterengine" -I "$HN_DIR/vendor/painterengine/core" -I/opt/homebrew/include/freetype2 \
  -I"$QJ/include/quickjs" -DHN_HAVE_QUICKJS -DHN_HAVE_PE \
  -DHN_MEDIA_PLATFORM_AVFOUNDATION -DHN_VERSION='"pe"' \
  app/hn_app.c app/hn_media_platform.c app/hn_media_synth.c rt/hn_rt.c \
  platform/hnp_macos.c platform/hnp_media_macos.c platform/hnp_gpu_metal.c \
  "$HN_DIR/hn_canvas_ity.o" \
  $ENG/hn_pe.c $ENG/hn_cubism.c $ENG/hn_three.c $ENG/neb3_soft.c \
  $ENG/hn_arena.c $ENG/hn_html.c $ENG/hn_md.c $ENG/hn_fmt.c $ENG/hn_css.c \
  $ENG/hn_style.c $ENG/hn_layout.c $ENG/hn_paint.c $ENG/hn_media.c \
  $ENG/hn_context.c $ENG/hn_theme.c $ENG/hn_json.c $ENG/hn_lottie.c \
  $ENG/hn_mesh.c $ENG/hn_png.c $ENG/hn_gif.c $ENG/hn_canvas.c $ENG/hn_gpu.c \
  $ENG/hn_wasm.c $ENG/hnsoft.c \
  vendor/wasm3/m3_bind.c vendor/wasm3/m3_code.c vendor/wasm3/m3_compile.c \
  vendor/wasm3/m3_core.c vendor/wasm3/m3_env.c vendor/wasm3/m3_exec.c \
  vendor/wasm3/m3_function.c vendor/wasm3/m3_info.c vendor/wasm3/m3_module.c \
  vendor/wasm3/m3_parse.c vendor/wasm3/m3_validate.c \
  "$PE/core/"*.c \
  -L/opt/homebrew/lib -lfreetype "$QJ/lib/quickjs/libquickjs.a" -lm \
  -lc++ -lc++abi \
  -o /tmp/hnapp-pe 2>/tmp/hn_pe_err.txt
echo "OK /tmp/hnapp-pe ($(du -k /tmp/hnapp-pe | cut -f1)KB)"

#!/usr/bin/env bash
# build-cubism-dev.sh — 带 Cubism Core 的 hnapp 本机快速重编(开发/调试用)
set -euo pipefail
cd "$(dirname "$0")/.."
QJ=/opt/homebrew/opt/quickjs
zig cc -target aarch64-macos -std=c99 -O2 -ffp-contract=off \
  -I Sources/CHtmlNative/include -I Sources/CHtmlNative -I rt -I platform -I app \
  -I vendor/wasm3 -I vendor/cubism/include -I/opt/homebrew/include/freetype2 \
  -I$QJ/include/quickjs -DHN_HAVE_QUICKJS -DHN_HAVE_CUBISM \
  -DHN_MEDIA_PLATFORM_AVFOUNDATION -DHN_VERSION='"cubism"' \
  app/hn_app.c app/hn_media_platform.c app/hn_media_synth.c rt/hn_rt.c \
  platform/hnp_macos.c platform/hnp_media_macos.c platform/hnp_gpu_metal.c \
  Sources/CHtmlNative/hn_pe.c Sources/CHtmlNative/hn_cubism.c Sources/CHtmlNative/hn_arena.c Sources/CHtmlNative/hn_html.c \
  Sources/CHtmlNative/hn_md.c Sources/CHtmlNative/hn_fmt.c Sources/CHtmlNative/hn_css.c \
  Sources/CHtmlNative/hn_style.c Sources/CHtmlNative/hn_layout.c Sources/CHtmlNative/hn_paint.c \
  Sources/CHtmlNative/hn_media.c Sources/CHtmlNative/hn_context.c Sources/CHtmlNative/hn_theme.c \
  Sources/CHtmlNative/hn_json.c Sources/CHtmlNative/hn_lottie.c Sources/CHtmlNative/hn_mesh.c \
  Sources/CHtmlNative/hn_png.c Sources/CHtmlNative/hn_gif.c Sources/CHtmlNative/hn_canvas.c \
  Sources/CHtmlNative/hn_gpu.c Sources/CHtmlNative/hn_wasm.c Sources/CHtmlNative/hnsoft.c \
  vendor/wasm3/m3_bind.c vendor/wasm3/m3_code.c vendor/wasm3/m3_compile.c vendor/wasm3/m3_core.c \
  vendor/wasm3/m3_env.c vendor/wasm3/m3_exec.c vendor/wasm3/m3_function.c vendor/wasm3/m3_info.c \
  vendor/wasm3/m3_module.c vendor/wasm3/m3_parse.c vendor/wasm3/m3_validate.c \
  vendor/cubism/lib/macos-arm64/libLive2DCubismCore.a \
  -L/opt/homebrew/lib -lfreetype $QJ/lib/quickjs/libquickjs.a -lm \
  -o /tmp/hnapp-cubism
echo "OK /tmp/hnapp-cubism ($(du -k /tmp/hnapp-cubism | cut -f1)KB)"

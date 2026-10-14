#!/usr/bin/env bash
# build-pe-dev.sh — 带 PainterEngine 集成的 hnapp 本机快速重编(开发/调试)
set -euo pipefail
cd "$(dirname "$0")/.."
QJ=/opt/homebrew/opt/quickjs
PE=vendor/painterengine
zig cc -target aarch64-macos -std=c99 -O2 -ffp-contract=off \
  -I Sources/CHtmlNative/include -I Sources/CHtmlNative -I rt -I platform -I app \
  -I vendor/wasm3 -I $PE -I $PE/core -I/opt/homebrew/include/freetype2 \
  -I$QJ/include/quickjs -DHN_HAVE_QUICKJS -DHN_HAVE_PE \
  -DHN_MEDIA_PLATFORM_AVFOUNDATION -DHN_VERSION='"pe"' \
  app/hn_app.c app/hn_media_platform.c app/hn_media_synth.c rt/hn_rt.c \
  platform/hnp_macos.c platform/hnp_media_macos.c platform/hnp_gpu_metal.c \
  Sources/CHtmlNative/hn_pe.c Sources/CHtmlNative/hn_cubism.c Sources/CHtmlNative/hn_arena.c Sources/CHtmlNative/hn_html.c \
  Sources/CHtmlNative/hn_md.c Sources/CHtmlNative/hn_fmt.c Sources/CHtmlNative/hn_css.c \
  Sources/CHtmlNative/hn_style.c Sources/CHtmlNative/hn_layout.c Sources/CHtmlNative/hn_paint.c \
  Sources/CHtmlNative/hn_media.c Sources/CHtmlNative/hn_context.c Sources/CHtmlNative/hn_theme.c \
  Sources/CHtmlNative/hn_json.c Sources/CHtmlNative/hn_lottie.c Sources/CHtmlNative/hn_mesh.c \
  Sources/CHtmlNative/hn_png.c Sources/CHtmlNative/hn_gif.c Sources/CHtmlNative/hn_canvas.c \
  Sources/CHtmlNative/hn_gpu.c Sources/CHtmlNative/hn_wasm.c Sources/CHtmlNative/hnsoft.c \
  $PE/core/PX_Typedef.c $PE/core/PX_MemoryPool.c $PE/core/PX_Surface.c $PE/core/PX_Texture.c \
  $PE/core/PX_PNG.c $PE/core/PX_Color.c $PE/core/PX_Math.c $PE/core/PX_Object.c \
  $PE/core/PX_ImageFilter.c $PE/core/PX_TextureRender.c 2>/dev/null || true
# 上面 PE 单文件清单可能缺/多名不符 — 退化用全量 core:
if [ ! -x /tmp/hnapp-pe ] || true; then :; fi
zig cc -target aarch64-macos -std=c99 -O2 -ffp-contract=off \
  -I Sources/CHtmlNative/include -I Sources/CHtmlNative -I rt -I platform -I app \
  -I vendor/wasm3 -I $PE -I $PE/core -I/opt/homebrew/include/freetype2 \
  -I$QJ/include/quickjs -DHN_HAVE_QUICKJS -DHN_HAVE_PE \
  -DHN_MEDIA_PLATFORM_AVFOUNDATION -DHN_VERSION='"pe"' \
  app/hn_app.c app/hn_media_platform.c app/hn_media_synth.c rt/hn_rt.c \
  platform/hnp_macos.c platform/hnp_media_macos.c platform/hnp_gpu_metal.c \
  Sources/CHtmlNative/hn_pe.c Sources/CHtmlNative/hn_cubism.c Sources/CHtmlNative/hn_arena.c Sources/CHtmlNative/hn_html.c \
  Sources/CHtmlNative/hn_md.c Sources/CHtmlNative/hn_fmt.c Sources/CHtmlNative/hn_css.c \
  Sources/CHtmlNative/hn_style.c Sources/CHtmlNative/hn_layout.c Sources/CHtmlNative/hn_paint.c \
  Sources/CHtmlNative/hn_media.c Sources/CHtmlNative/hn_context.c Sources/CHtmlNative/hn_theme.c \
  Sources/CHtmlNative/hn_json.c Sources/CHtmlNative/hn_lottie.c Sources/CHtmlNative/hn_mesh.c \
  Sources/CHtmlNative/hn_png.c Sources/CHtmlNative/hn_gif.c Sources/CHtmlNative/hn_canvas.c \
  Sources/CHtmlNative/hn_gpu.c Sources/CHtmlNative/hn_wasm.c Sources/CHtmlNative/hnsoft.c \
  $PE/core/*.c \
  vendor/wasm3/m3_bind.c vendor/wasm3/m3_code.c vendor/wasm3/m3_compile.c vendor/wasm3/m3_core.c \
  vendor/wasm3/m3_env.c vendor/wasm3/m3_exec.c vendor/wasm3/m3_function.c vendor/wasm3/m3_info.c \
  vendor/wasm3/m3_module.c vendor/wasm3/m3_parse.c vendor/wasm3/m3_validate.c \
  -L/opt/homebrew/lib -lfreetype $QJ/lib/quickjs/libquickjs.a -lm \
  -o /tmp/hnapp-pe 2>/tmp/hn_pe_err.txt
echo "OK /tmp/hnapp-pe ($(du -k /tmp/hnapp-pe | cut -f1)KB)"

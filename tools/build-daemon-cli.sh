#!/usr/bin/env bash
# build-daemon-cli.sh — C99 常驻宿主 + 命令行客户端 本机构建
#
# 与 build-multiplatform.sh 分开的原因: daemon/cli 只在本机跑(agent 入口),
# 不参与交叉编译; 主脚本保持"交叉产物清单"职责单一。旗子与主脚本的
# hnapp-macos-arm64 目标完全一致(同一引擎源 + hn_rt + QuickJS 探测)。
#
# 产物: dist/hn-daemon(常驻宿主, Unix socket JSON-lines) / dist/hn(CLI 客户端)
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
OUT="$ROOT/dist"
cd "$ROOT"
mkdir -p "$OUT"

. "$ROOT/tools/_engine_sources.sh"
hn_sources_full
ENGINE_SRC=("${HN_SOURCES[@]}")
for f in "${HN_WASM3[@]}"; do ENGINE_SRC+=("$HN_WASM3_DIR/$f"); done
CANVAS_TMP="$(mktemp -d "${TMPDIR:-/tmp}/hn-daemon-canvas-XXXXXX")"
trap 'rm -rf "$CANVAS_TMP"' EXIT
hn_canvas_build native "$CANVAS_TMP" "${CXX:-c++}"
INC="-I Sources/CHtmlNative/include -I Sources/CHtmlNative -I rt -I platform -I vendor/wasm3"
FT_INC=""
if [ -d /opt/homebrew/include/freetype2 ]; then FT_INC="-I/opt/homebrew/include/freetype2"; fi

# QuickJS 探测(与主脚本同口径): 本机有就带 JS 求值, 没有则纯声明式
QJ_DEF="" QJ_INC="" QJ_LIB=""
QJ_OPT="/opt/homebrew/opt/quickjs"
if [ -f "$QJ_OPT/include/quickjs/quickjs.h" ] && [ -f "$QJ_OPT/lib/quickjs/libquickjs.a" ]; then
    QJ_DEF="-DHN_HAVE_QUICKJS"
    QJ_INC="-I$QJ_OPT/include/quickjs"
    QJ_LIB="$QJ_OPT/lib/quickjs/libquickjs.a"
fi

CFLAGS=(-std=c99 -O2 -ffp-contract=off $INC $FT_INC $QJ_INC $QJ_DEF -DHN_VERSION='"0.1.0"')

printf '  %-14s ' "hn-daemon"
cc "${CFLAGS[@]}" tools/hn_daemon.c rt/hn_rt.c rt/hn_sys.c rt/hn_http.c "${ENGINE_SRC[@]}" ${HN_CANVAS_DEFS[@]+"${HN_CANVAS_DEFS[@]}"} ${HN_CANVAS_OBJECTS[@]+"${HN_CANVAS_OBJECTS[@]}"} \
    -L/opt/homebrew/lib -lfreetype $QJ_LIB -lm -pthread ${HN_CANVAS_LIBS[@]+"${HN_CANVAS_LIBS[@]}"} -o "$OUT/hn-daemon"
printf '✓ dist/hn-daemon %s\n' "$([ -n "$QJ_LIB" ] && echo '+QuickJS' || echo '无JS')"

printf '  %-14s ' "hn"
cc -std=c99 -O2 tools/hn_cli.c -o "$OUT/hn"
printf '✓ dist/hn\n'

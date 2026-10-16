#!/usr/bin/env bash
# tools/canvas_probe.sh — <canvas> 2D 探针薄封装(原 canvas_probe.py 的 bash 版)。
#
# 只做三件事: cc 编译 tools/canvas_probe.c(引擎 + rt + hnsoft + wasm3 +
# homebrew QuickJS/FreeType) → 运行 → 透传退出码。语义断言全部在 .c 里,
# 本脚本不解析输出。
#
# 用法: bash tools/canvas_probe.sh [二进制路径]
#   二进制路径可给可不给: 给了且可执行且**确是本探针产物** → 直接运行
#   (门禁可复用已编译产物); 没给/不可用/不是探针 → 忽略该参数,
#   本机 cc 编译再运行(产物缓存在 $TMPDIR, 比全部源新则跳过重编)。
#   身份判据: 探针 banner 字面量 "canvas_probe" 必入 __TEXT。
#
# 门禁口径: 缺源/缺 QuickJS 静态库/编译失败/运行失败/退出码非 0 → exit 1。

set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/_engine_sources.sh"

LABEL="canvas_probe.sh"

# 源清单: 探针 + rt + 引擎核心/hnsoft/hn_wasm + wasm3 11 源
hn_sources_full
HN_SOURCES=("$HN_TOOLS_DIR/canvas_probe.c" "$HN_RT_DIR/hn_rt.c" "$HN_RT_DIR/hn_sys.c" "$HN_RT_DIR/hn_http.c"
            ${HN_SOURCES[@]+"${HN_SOURCES[@]}"})
local_wasm3=""
for local_wasm3 in "${HN_WASM3[@]}"; do
  HN_SOURCES+=("$HN_WASM3_DIR/$local_wasm3")
done

HN_CFLAGS=(-std=c99 -O2 -Wall -ffp-contract=off)
HN_DEFS=(-DHN_HAVE_QUICKJS)
HN_INCS=(-I "$HN_ENG/include" -I "$HN_ENG" -I "$HN_RT_DIR" -I "$HN_WASM3_DIR"
         -I "$HN_QUICKJS_INC" -I /opt/homebrew/include/freetype2)
HN_LIBS=("$HN_QUICKJS_A" -L/opt/homebrew/lib -lfreetype -lm)

hn_sources_check ${HN_SOURCES[@]+"${HN_SOURCES[@]}"} || exit 1
if [ ! -f "$HN_QUICKJS_A" ]; then
  echo "$LABEL: FAIL 缺 QuickJS 静态库 $HN_QUICKJS_A(brew install quickjs)"
  exit 1
fi

# 参数二进制: 可执行且带探针 banner → 复用; 否则本机编译(带缓存)
BIN_ARG="${1:-}"
BIN=""
if [ -n "$BIN_ARG" ] && [ -f "$BIN_ARG" ] && [ -x "$BIN_ARG" ]; then
  if hn_bin_has "$BIN_ARG" "canvas_probe"; then
    BIN="$BIN_ARG"
  else
    echo "$LABEL: $BIN_ARG 不是 canvas_probe 产物, 忽略并重编"
  fi
fi
if [ -z "$BIN" ]; then
  OUT="${TMPDIR:-/tmp}/hn_canvas_probe_bin"
  hn_probe_compile "$LABEL" "$OUT" || exit 1
  BIN="$OUT"
fi

hn_probe_run "$LABEL" "$BIN"

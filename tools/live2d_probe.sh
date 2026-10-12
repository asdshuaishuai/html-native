#!/usr/bin/env bash
# tools/live2d_probe.sh — live2d_probe.c 的门禁薄封装(与 quad3d_probe.sh 同款)。
#
# 只做三件事: 编译 tools/live2d_probe.c + 引擎源 → 运行 → 透传退出码。
# 全部语义断言在 .c 里(Live2D 运行时包解析/motion3 求值器/rig 顶点接线),
# 本脚本不解析输出。
#
# 夹具: examples/assets/miku(官方 Miku 样例 runtime, 真文件)与
# examples/fixtures/live2d(合成四段型 motion3)。探针以 cwd=仓库根运行
# (资产路径都是仓库根相对)。
#
# 用法: bash tools/live2d_probe.sh [二进制路径]
#   给了且可执行且带探针 banner → 直接运行; 否则本机 cc 编译再运行
#   (产物缓存在 $TMPDIR, 比全部源新则跳过重编)。
#
# FreeType: 有则真文本度量, 无则 -DHN_NO_TEXT —— 探针断言不依赖文本度量。
#
# 门禁口径: 缺源/编译失败/运行退出码非 0 → exit 1; 全过 → exit 0。

set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/_engine_sources.sh"

LABEL="live2d_probe.sh"

# 源清单: 探针 + 引擎核心(无 hnsoft/wasm/rt —— 纯引擎语义层)
hn_sources_core
HN_SOURCES=("$HN_TOOLS_DIR/live2d_probe.c" ${HN_SOURCES[@]+"${HN_SOURCES[@]}"})
hn_sources_check ${HN_SOURCES[@]+"${HN_SOURCES[@]}"} || exit 1

FT_INC=""
FT_LIB=()
if [ -d /opt/homebrew/include/freetype2 ]; then
  FT_INC="-I/opt/homebrew/include/freetype2"
  FT_LIB=(-L/opt/homebrew/lib -lfreetype)
elif [ -d /usr/include/freetype2 ]; then
  FT_INC="-I/usr/include/freetype2"
  FT_LIB=(-lfreetype)
fi

OUT="${TMPDIR:-/tmp}/live2d_probe_bin"

compile() {
  local tag="$1"; shift
  echo "$LABEL: 编译$tag: $HN_CC -O2 -std=c99 -I $HN_ENG/include -I $HN_ENG" \
       "$*" ${HN_SOURCES[@]+"${HN_SOURCES[@]}"} -lm -o "$OUT"
  local errlog
  errlog="$(mktemp "${TMPDIR:-/tmp}/hncc-XXXXXX")"
  if "$HN_CC" -O2 -std=c99 -I "$HN_ENG/include" -I "$HN_ENG" "$@" \
       ${HN_SOURCES[@]+"${HN_SOURCES[@]}"} -lm -o "$OUT" 2>"$errlog"; then
    rm -f "$errlog"
    return 0
  fi
  echo "$LABEL: 编译失败$tag" >&2
  [ -s "$errlog" ] && tail -c 4000 "$errlog" >&2
  rm -f "$errlog"
  return 1
}

# 参数二进制: 可执行且带探针 banner → 复用; 否则本机编译(带缓存)
BIN_ARG="${1:-}"
if [ -n "$BIN_ARG" ] && [ -x "$BIN_ARG" ] && hn_bin_has "$BIN_ARG" "live2d_probe"; then
  echo "$LABEL: 复用现成二进制 $BIN_ARG"
  cp "$BIN_ARG" "$OUT"
elif [ ! -x "$OUT" ] || ! hn_up_to_date "$OUT" ${HN_SOURCES[@]+"${HN_SOURCES[@]}"}; then
  if [ -n "$FT_INC" ]; then
    compile "(带 FreeType)" "$FT_INC" ${FT_LIB[@]+"${FT_LIB[@]}"} || \
      compile "(HN_NO_TEXT)" -DHN_NO_TEXT || { echo "$LABEL: FAIL 编译失败"; exit 1; }
  else
    compile "(HN_NO_TEXT)" -DHN_NO_TEXT || { echo "$LABEL: FAIL 编译失败"; exit 1; }
  fi
else
  echo "$LABEL: 缓存命中(产物比全部源新), 跳过编译: $OUT"
fi

hn_probe_run "$LABEL" "$OUT"

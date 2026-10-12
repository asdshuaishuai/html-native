#!/usr/bin/env bash
# tools/quad3d_probe.sh — quad3d_probe.c 的门禁薄封装(原 quad3d_probe.py 的 bash 版)。
#
# 只做三件事: 编译 tools/quad3d_probe.c + 引擎源 → 运行 → 透传退出码。
# 全部语义断言在 .c 里(display list / hnsoft 位图层面的确定性结论),
# 本脚本不解析输出。
#
# cairo 变体: pkg-config 找得到 cairo 时, 额外编一份 -DQUAD3D_HAVE_CAIRO
# + hn_cairo.c 再跑一遍 —— 用例 G(cairo 缝线)只在该变体里真正执行;
# 主运行里 G 恒为 skip。两份任一退出码非 0 → 本脚本 FAIL。
#
# 用法: bash tools/quad3d_probe.sh [二进制路径]
#   二进制路径可给可不给: 给了且可执行且**确是本探针产物** → 直接运行
#   (门禁可复用已编译产物, 跳过编译); 没给/不可用/不是探针 → 忽略该参数,
#   本机 cc 编译再运行(产物缓存在 $TMPDIR, 比全部源新则跳过重编)。
#   身份判据: 探针 banner 字面量 "quad3d_probe"(main 开头)必入 __TEXT ——
#   门禁习惯把 dist 引擎二进制(hncore)也传进来, 盲跑只会撞 usage/exit 2,
#   所以复用前先验身份(与 media_probe.sh 同一口径)。
#
# FreeType: 有则真文本度量, 无则 -DHN_NO_TEXT 走引擎内置等宽估算 ——
# 断言不依赖文本度量, 两种构建语义一致。编译两段式: 先试带 FreeType,
# 失败再试 HN_NO_TEXT, 先成功者用(cairo 变体同样两段式)。
#
# 门禁口径: 缺源/编译失败/运行失败/退出码非 0 → exit 1; 全过 → exit 0。

set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/_engine_sources.sh"

LABEL="quad3d_probe.sh"

# 源清单: 探针 + 引擎核心 + hnsoft(无 wasm/rt)
hn_sources_soft
HN_SOURCES=("$HN_TOOLS_DIR/quad3d_probe.c" ${HN_SOURCES[@]+"${HN_SOURCES[@]}"})

HN_CAIRO_SRC="$HN_ENG/hn_cairo.c"

# cairo 的 cflags+libs; 本机没有则为空(用例 G 由探针以 skip 记)
pkg_config_cairo() {
  pkg-config --cflags --libs cairo 2>/dev/null || true
}

# freetype2 头在 homebrew 的子目录里(与 tools/build-multiplatform.sh 同口径)
FT_INC=""
FT_LIB=()
if [ -d /opt/homebrew/include/freetype2 ]; then
  FT_INC="-I/opt/homebrew/include/freetype2"
  FT_LIB=(-L/opt/homebrew/lib -lfreetype)
elif [ -d /usr/include/freetype2 ]; then
  FT_INC="-I/usr/include/freetype2"
  FT_LIB=(-lfreetype)
fi

# 编译(两段式): 先带 FreeType, 失败再 -DHN_NO_TEXT; 先成功者用。
# 第 2 参数 = cairo flags(非空 → 追加 -DQUAD3D_HAVE_CAIRO + hn_cairo.c,
# 与原版同序拼在引擎源之后)。
hn_quad_compile() {
  local out="$1" cairo_flags="${2:-}"
  local extra=()
  if [ -n "$cairo_flags" ]; then
    extra=(-DQUAD3D_HAVE_CAIRO $cairo_flags "$HN_CAIRO_SRC")
  fi
  local pass tag
  for pass in 1 2; do
    local args=()
    if [ "$pass" = 1 ] && [ -n "$FT_INC" ]; then
      tag="(带 FreeType)"
      args=(-O2 -std=c99 -I "$HN_ENG/include" -I "$HN_ENG" "$FT_INC"
            ${FT_LIB[@]+"${FT_LIB[@]}"})
    else
      tag="(HN_NO_TEXT)"
      args=(-O2 -std=c99 -I "$HN_ENG/include" -I "$HN_ENG" -DHN_NO_TEXT)
    fi
    echo "$LABEL: 编译: $HN_CC ${args[*]} ${HN_SOURCES[*]} ${extra[*]+"${extra[*]}"} -lm -o $out"
    local errlog
    errlog="$(mktemp "${TMPDIR:-/tmp}/hncc-XXXXXX")"
    if "$HN_CC" ${args[@]+"${args[@]}"} ${HN_SOURCES[@]+"${HN_SOURCES[@]}"} \
         ${extra[@]+"${extra[@]}"} -lm -o "$out" 2>"$errlog"; then
      rm -f "$errlog"
      return 0
    fi
    echo "$LABEL: 编译失败$tag" >&2
    [ -s "$errlog" ] && tail -c 4000 "$errlog" >&2
    rm -f "$errlog"
  done
  return 1
}

hn_sources_check ${HN_SOURCES[@]+"${HN_SOURCES[@]}"} || exit 1

# 参数二进制: 可执行且带探针 banner → 复用; 否则本机编译(带缓存)
BIN_ARG="${1:-}"
BIN=""
if [ -n "$BIN_ARG" ] && [ -f "$BIN_ARG" ] && [ -x "$BIN_ARG" ]; then
  if hn_bin_has "$BIN_ARG" "quad3d_probe"; then
    echo "$LABEL: 复用现成二进制 $BIN_ARG"
    BIN="$BIN_ARG"
  else
    echo "$LABEL: 参数 $BIN_ARG 无探针标识(是别的产物), 忽略之, 走本机编译"
  fi
elif [ -n "$BIN_ARG" ]; then
  echo "$LABEL: 参数 $BIN_ARG 不是可执行文件, 忽略之, 走本机编译"
fi

# 主变体编译(带缓存)
if [ -z "$BIN" ]; then
  OUT="${TMPDIR:-/tmp}/quad3d_probe_bin"
  if ! hn_up_to_date "$OUT" ${HN_SOURCES[@]+"${HN_SOURCES[@]}"}; then
    hn_quad_compile "$OUT" "" || { echo "$LABEL: FAIL 编译失败"; exit 1; }
  else
    echo "$LABEL: 缓存命中(产物比全部源新), 跳过编译: $OUT"
  fi
  BIN="$OUT"
fi

echo "$LABEL: 运行 $BIN"
rc=0
"$BIN" || rc=$?
if [ "$rc" -ne 0 ]; then
  echo "$LABEL: 主变体退出码 $rc(非 0) —— 存在失败断言" \
       "(修复 3D 边界/遗留清单前这是预期; 继续跑 cairo 变体以暴露全部结果)"
fi

# cairo 变体(用例 G): 本机有 cairo 才多跑一份; 任一退出码非 0 → FAIL。
# 主变体已红也照跑 —— 一轮门禁同时暴露两份结果。
CAIRO_FLAGS="$(pkg_config_cairo)"
if [ -n "$CAIRO_FLAGS" ]; then
  CAIRO_OUT="${TMPDIR:-/tmp}/quad3d_probe_bin_cairo"
  if ! hn_up_to_date "$CAIRO_OUT" ${HN_SOURCES[@]+"${HN_SOURCES[@]}"} "$HN_CAIRO_SRC"; then
    hn_quad_compile "$CAIRO_OUT" "$CAIRO_FLAGS" || {
      echo "$LABEL: FAIL cairo 变体编译失败"; exit 1; }
  else
    echo "$LABEL: 缓存命中(cairo 变体): $CAIRO_OUT"
  fi
  echo "$LABEL: 运行 $CAIRO_OUT(cairo 变体, 用例 G 生效)"
  crc=0
  "$CAIRO_OUT" || crc=$?
  if [ "$crc" -ne 0 ]; then
    echo "$LABEL: FAIL cairo 变体退出码 $crc(非 0)"
    exit 1
  fi
else
  echo "$LABEL: 本机无 cairo —— 用例 G 在探针里以 skip 记"
fi

if [ "$rc" -ne 0 ]; then
  echo "$LABEL: FAIL 主变体退出码 $rc(非 0)"
  exit 1
fi
echo "$LABEL: PASS(两份变体退出码均 0)"

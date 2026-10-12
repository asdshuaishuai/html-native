#!/usr/bin/env bash
# tools/md_probe.sh — Markdown 直染探针薄封装(原 md_probe.py 的 bash 版)。
#
# 只做三件事: cc 编译 tools/md_probe.c(引擎 + hnsoft + FreeType, 与
# render_probe 同一链接口径) → 运行 → 透传退出码。语义断言全部在 .c 里,
# 本脚本不解析输出。不依赖 QuickJS/wasm3(直染是纯引擎语义, 不涉脚本桥)。
#
# 用法: bash tools/md_probe.sh [二进制路径]
#   二进制路径可给可不给: 给了且可执行且**确是本探针产物** → 直接运行
#   (门禁可复用已编译产物); 没给/不可用/不是探针 → 忽略该参数,
#   本机 cc 编译再运行(产物缓存在 $TMPDIR, 比全部源新则跳过重编)。
#   身份判据: 探针 banner 字面量 "md_probe" 必入 __TEXT。
#
# 门禁口径: 缺源/编译失败/运行失败/退出码非 0 → exit 1; 全过 → exit 0。

set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/_engine_sources.sh"

LABEL="md_probe.sh"

# 源清单: 探针 + 引擎核心 + hnsoft(无 wasm/rt)
hn_sources_soft
HN_SOURCES=("$HN_TOOLS_DIR/md_probe.c" ${HN_SOURCES[@]+"${HN_SOURCES[@]}"})

HN_CFLAGS=(-std=c99 -O2 -Wall -ffp-contract=off)
HN_INCS=(-I "$HN_ENG/include" -I "$HN_ENG")
HN_LIBS=(-L/opt/homebrew/lib -lfreetype -lm)

# FreeType 头目录(homebrew 常见前缀); 找不到就不加(编译会自己报)
if FT_INC="$(hn_freetype_inc)"; then
  HN_INCS+=(-I "$FT_INC")
fi

hn_sources_check ${HN_SOURCES[@]+"${HN_SOURCES[@]}"} || exit 1

# 参数二进制: 可执行且带探针 banner → 复用; 否则本机编译(带缓存)
BIN_ARG="${1:-}"
BIN=""
if [ -n "$BIN_ARG" ] && [ -f "$BIN_ARG" ] && [ -x "$BIN_ARG" ]; then
  if hn_bin_has "$BIN_ARG" "md_probe"; then
    BIN="$BIN_ARG"
  else
    echo "$LABEL: $BIN_ARG 不是 md_probe 产物, 忽略并重编"
  fi
fi
if [ -z "$BIN" ]; then
  OUT="${TMPDIR:-/tmp}/hn_md_probe_bin"
  hn_probe_compile "$LABEL" "$OUT" || exit 1
  BIN="$OUT"
fi

hn_probe_run "$LABEL" "$BIN"

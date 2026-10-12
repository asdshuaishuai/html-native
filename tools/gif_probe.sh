#!/usr/bin/env bash
# tools/gif_probe.sh — GIF 动图全链路探针薄封装(原 gif_probe.py 的 bash 版)。
#
# 编译并运行 tools/gif_probe.c(引擎 + hnsoft, 与 hncore 同一条链接口径)。
#
# 夹具说明(历史口径, 原样保留): 夹具是**内嵌在 C 探针里的字节数组**, 由
# 原 gif_probe.py 头部那段纯 python LZW 编码器生成(不依赖 PIL —— 门禁在
# 任意机器上都能复现同一份字节)。生成器已随 .py 退场; 要改夹具时按标准
# GIF89a 编码路径重造字节, 并把输出的 C 数组替换回 tools/gif_probe.c 的
# 夹具段。校验口径(开发期做过, 不进门禁): 夹具与 PIL 解码结果逐像素一致
# —— 包括 disposal 合成、透明索引与 NETSCAPE 循环计数字节序。
#
# 用法: bash tools/gif_probe.sh [二进制路径]
#   二进制路径可给可不给: 给了且可执行且**确是本探针产物** → 直接运行;
#   没给/不可用/不是探针 → 忽略该参数, 本机 cc 编译再运行
#   (产物缓存在 $TMPDIR, 比全部源新则跳过重编)。
#   身份判据: 探针 banner 字面量 "gif_probe" 必入 __TEXT。
#
# 门禁口径: 缺源/编译失败/运行失败/退出码非 0 → exit 1; 全过 → exit 0。

set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/_engine_sources.sh"

LABEL="gif_probe.sh"

# 源清单: 探针 + 引擎核心 + hnsoft(无 wasm/rt)
hn_sources_soft
HN_SOURCES=("$HN_TOOLS_DIR/gif_probe.c" ${HN_SOURCES[@]+"${HN_SOURCES[@]}"})

HN_CFLAGS=(-std=c99 -O2 -Wall -ffp-contract=off)
HN_INCS=(-I "$HN_ENG/include" -I "$HN_ENG"
         -I /opt/homebrew/include/freetype2)
HN_LIBS=(-L/opt/homebrew/lib -lfreetype -lm)

hn_sources_check ${HN_SOURCES[@]+"${HN_SOURCES[@]}"} || exit 1

# 参数二进制: 可执行且带探针 banner → 复用; 否则本机编译(带缓存)
BIN_ARG="${1:-}"
BIN=""
if [ -n "$BIN_ARG" ] && [ -f "$BIN_ARG" ] && [ -x "$BIN_ARG" ]; then
  if hn_bin_has "$BIN_ARG" "gif_probe"; then
    BIN="$BIN_ARG"
  else
    echo "$LABEL: $BIN_ARG 不是 gif_probe 产物, 忽略并重编"
  fi
fi
if [ -z "$BIN" ]; then
  OUT="${TMPDIR:-/tmp}/hn_gif_probe_bin"
  hn_probe_compile "$LABEL" "$OUT" || exit 1
  BIN="$OUT"
fi

hn_probe_run "$LABEL" "$BIN"

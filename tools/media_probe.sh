#!/usr/bin/env bash
# tools/media_probe.sh — 媒体真机探针薄封装(原 media_probe.py 的 bash 版;
# docs/media-design.md §8.2)。
#
# 只做三件事: 编译 tools/media_probe.c + platform/hnp_media_macos.c →
# 运行 → 断言退出码 0。语义断言全部在 .c 里, 本脚本不解析输出。
#
# 用法: bash tools/media_probe.sh [二进制路径]
#   二进制路径可给可不给: 给了且可执行且**确是本探针产物** → 直接运行
#   (门禁可复用已编译产物, 此时跳过编译); 没给/不可用/不是探针 → 忽略
#   该参数, 本机 cc 编译再运行(产物缓存在 $TMPDIR, 比全部源新则跳过重编)。
#   身份判据: 探针 banner 字面量 "media_probe"(media_probe.c main 开头)必入
#   __TEXT —— 门禁习惯把 dist 引擎二进制(hncore)也一并传进来, 盲跑只会
#   撞 usage / exit 2, 所以复用前先验身份。
#
# 门禁口径: 非 macOS/缺源/编译失败/运行失败/退出码非 0 → exit 1; 全过 → exit 0。

set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/_engine_sources.sh"

LABEL="media_probe.sh"

# 本探针是 macOS(AVFoundation)专属; 其他平台没有 hnp_media 实现
# (supported()=0 留位), 被调到即是门禁配置错误。(与原版同序: 平台检查最先)
if [ "$(uname -s)" != "Darwin" ]; then
  echo "$LABEL: FAIL 平台 $(uname -s) 非 macOS, 本探针不适用"
  exit 1
fi

HN_SOURCES=("$HN_TOOLS_DIR/media_probe.c" "$HN_REPO_ROOT/platform/hnp_media_macos.c")

hn_sources_check ${HN_SOURCES[@]+"${HN_SOURCES[@]}"} || exit 1

# 参数二进制: 可执行且带探针 banner → 复用; 否则本机编译(带缓存)
BIN_ARG="${1:-}"
BIN=""
if [ -n "$BIN_ARG" ] && [ -f "$BIN_ARG" ] && [ -x "$BIN_ARG" ]; then
  if hn_bin_has "$BIN_ARG" "media_probe"; then
    echo "$LABEL: 复用现成二进制 $BIN_ARG"
    BIN="$BIN_ARG"
  else
    echo "$LABEL: 参数 $BIN_ARG 无探针标识(是别的产物), 忽略之, 走本机编译"
  fi
elif [ -n "$BIN_ARG" ]; then
  echo "$LABEL: 参数 $BIN_ARG 不是可执行文件, 忽略之, 走本机编译"
fi

if [ -z "$BIN" ]; then
  OUT="${TMPDIR:-/tmp}/hnp_media_probe_bin"
  hn_probe_compile "$LABEL" "$OUT" || { echo "$LABEL: FAIL 编译失败"; exit 1; }
  BIN="$OUT"
fi

echo "$LABEL: 运行 $BIN"
rc=0
"$BIN" || rc=$?
if [ "$rc" -ne 0 ]; then
  echo "$LABEL: FAIL 探针退出码 $rc(非 0)"
  exit 1
fi
echo "$LABEL: PASS(退出码 0)"

#!/usr/bin/env bash
# tools/wasm_probe.sh — WASM 缝探针薄封装(原 wasm_probe.py 的 bash 版;
# WASM 实现缝, media-design §7 验证路径)。
#
# 只做三件事: 编译 tools/wasm_probe.c + vendor/wasm3 11 源 + hn_wasm.c →
# 运行 → 断言退出码 0(即 add(2,3)==5 等全部断言在 .c 里, 本脚本不解析
# 输出语义 —— 与 media_probe.sh 同一口径)。
#
# 用法: bash tools/wasm_probe.sh [探针二进制] [demo .wasm 路径]
#   第 1 参数给了且可执行且带探针标识("wasm_probe" 字面量在 __TEXT)→ 直接
#   复用(门禁可复用已编译产物); 否则本机 cc 编译再运行
#   (产物缓存在 $TMPDIR, 比全部源新则跳过重编)。
#   第 2 参数传给探针(demo 模块缺省 examples/wasm/add.wasm, 产物已入库)。
#
# 门禁口径: 缺源/缺 demo 模块/编译失败/运行失败/退出码非 0 → exit 1。

set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/_engine_sources.sh"

LABEL="wasm_probe.sh"

# wasm3 源按名排序收集(glob); 数目漂移说明 vendor 目录被动过
HN_SOURCES=()
local_f=""
for local_f in $(printf '%s\n' "${HN_WASM3[@]}" | LC_ALL=C sort); do
  HN_SOURCES+=("$HN_WASM3_DIR/$local_f")
done
N_WASM3=${#HN_SOURCES[@]}
echo "$LABEL: wasm3 源 $N_WASM3 个(契约最小集 $HN_WASM3_EXPECT)"
if [ "$N_WASM3" -ne "$HN_WASM3_EXPECT" ]; then
  echo "$LABEL: FAIL vendor/wasm3 源数 $N_WASM3 != $HN_WASM3_EXPECT" \
       "(vendor 目录与契约 §7 最小集不符)"
  exit 1
fi

HN_SOURCES=("$HN_TOOLS_DIR/wasm_probe.c" ${HN_SOURCES[@]+"${HN_SOURCES[@]}"}
            "$HN_ENG/hn_wasm.c")

# 第 2 参数 = demo 模块路径(文件头用法口径); 缺省入库产物。
DEMO_WASM="$HN_REPO_ROOT/examples/wasm/add.wasm"
if [ "${2:-}" != "" ] && [ "$2" != "$DEMO_WASM" ]; then
  DEMO_WASM="$(cd "$(dirname "$2")" && pwd)/$(basename "$2")"
  echo "$LABEL: demo 模块改用 $DEMO_WASM"
fi

hn_sources_check ${HN_SOURCES[@]+"${HN_SOURCES[@]}"} "$DEMO_WASM" || exit 1

# 参数二进制: 可执行且带探针 banner → 复用; 否则本机编译(带缓存)
BIN_ARG="${1:-}"
BIN=""
if [ -n "$BIN_ARG" ] && [ -f "$BIN_ARG" ] && [ -x "$BIN_ARG" ]; then
  if hn_bin_has "$BIN_ARG" "wasm_probe"; then
    echo "$LABEL: 复用现成二进制 $BIN_ARG"
    BIN="$BIN_ARG"
  else
    echo "$LABEL: 参数 $BIN_ARG 无探针标识(是别的产物), 忽略之, 走本机编译"
  fi
elif [ -n "$BIN_ARG" ]; then
  echo "$LABEL: 参数 $BIN_ARG 不是可执行文件, 忽略之, 走本机编译"
fi

if [ -z "$BIN" ]; then
  OUT="${TMPDIR:-/tmp}/hn_wasm_probe_bin"
  if hn_up_to_date "$OUT" ${HN_SOURCES[@]+"${HN_SOURCES[@]}"}; then
    echo "$LABEL: 缓存命中(产物比全部源新), 跳过编译: $OUT"
  else
    echo "$LABEL: 编译: $HN_CC -std=c99 -O1 -Wall -I $HN_ENG/include -I $HN_WASM3_DIR ${HN_SOURCES[*]} -o $OUT"
    ERRLOG="$(mktemp "${TMPDIR:-/tmp}/hncc-XXXXXX")"
    if "$HN_CC" -std=c99 -O1 -Wall -I "$HN_ENG/include" -I "$HN_WASM3_DIR" \
         ${HN_SOURCES[@]+"${HN_SOURCES[@]}"} -o "$OUT" 2>"$ERRLOG"; then
      # 编译成功但有告警 → 原样透出(原版同口径)
      if [ -s "$ERRLOG" ]; then
        echo "$LABEL: 编译警告:"
        cat "$ERRLOG"
      fi
      rm -f "$ERRLOG"
    else
      echo "$LABEL: FAIL 编译失败"
      [ -s "$ERRLOG" ] && tail -c 2000 "$ERRLOG"
      rm -f "$ERRLOG"
      exit 1
    fi
  fi
  BIN="$OUT"
fi

echo "$LABEL: 运行 $BIN $DEMO_WASM"
rc=0
"$BIN" "$DEMO_WASM" || rc=$?
if [ "$rc" -ne 0 ]; then
  echo "$LABEL: FAIL 探针退出码 $rc(非 0)"
  exit 1
fi
echo "$LABEL: PASS(退出码 0, add(2,3)==5 已钉死)"

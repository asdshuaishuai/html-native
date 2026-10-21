#!/usr/bin/env bash
# tools/freeze_l2_probe.sh — 冻结布局 L2 前置(文本测量缓存)探针薄封装
# (照 dom_probe.sh 的手法: 编译 freeze_l2_probe.c → 跑断言 → 透传退出码)。
#
# 断言面(明细见 freeze_l2_probe.c 头注):
#   A. 逐位一致: 缓存关(HN_MEASURE_CACHE=0)与开(缺省)的布局结果转储
#      cmp 逐字节相同 —— 盒/runs/显示列表全按位打印;
#   B. 命中: 第二遍起 hits>0; 改一处文本那遍 hits 净增(旧文本零测量);
#   C. 提速: 300 遍全量重排的开关计时(信息值, 不做硬线)。
#
# 用法: bash tools/freeze_l2_probe.sh
# 门禁口径: 编译失败/断言失败/转储不一致 → exit 1。

set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/_engine_sources.sh"

LABEL="freeze_l2_probe.sh"

# 源清单: 探针 + 引擎核心 + hnsoft(文本测量走 FreeType, 与生产同口径)
hn_sources_soft
HN_SOURCES=("$HN_TOOLS_DIR/freeze_l2_probe.c" ${HN_SOURCES[@]+"${HN_SOURCES[@]}"})
HN_CFLAGS=(-std=c99 -O2 -Wall -ffp-contract=off)
HN_INCS=(-I "$HN_ENG/include" -I "$HN_ENG" -I /opt/homebrew/include/freetype2)
HN_LIBS=(-L/opt/homebrew/lib -lfreetype -lm)

hn_sources_check ${HN_SOURCES[@]+"${HN_SOURCES[@]}"} || exit 1

D="$(hn_tmpd probe)"
BIN="$D/freeze_l2_probe"
hn_probe_compile "$LABEL" "$BIN"

# ---- A. 逐位一致: 关 vs 开的两份转储 cmp ----
HN_MEASURE_CACHE=0 "$BIN" dump > "$D/off.dump" 2> "$D/off.err"
"$BIN" dump > "$D/on.dump" 2> "$D/on.err"

NPASS=0; NFAIL=0; FAILED_DESCS=()

if cmp -s "$D/off.dump" "$D/on.dump"; then
  NPASS=$((NPASS + 1))
  echo "  ✔ 缓存开/关布局结果逐位一致(cmp)"
else
  NFAIL=$((NFAIL + 1))
  FAILED_DESCS+=("  ✘ 缓存开/关布局转储不一致(diff 见 $D)")
  diff "$D/off.dump" "$D/on.dump" | head -20 >&2 || true
fi

# C 侧断言(命中/结构自检)已随 "$BIN" dump 的退出码执行(set -e 兜底)

# ---- C. 提速口径(信息值) ----
ms_off="$(HN_MEASURE_CACHE=0 "$BIN" bench 2>/dev/null | awk '{print $2}' | cut -d= -f2)"
ms_on="$("$BIN" bench 2>/dev/null | awk '{print $2}' | cut -d= -f2)"
ratio="$(awk -v a="$ms_off" -v b="$ms_on" 'BEGIN { if (b > 0) printf "%.2f", a / b; else print "?" }')"
echo "  ℹ 300 遍全量重排: 关=${ms_off}ms 开=${ms_on}ms (提速 ${ratio}x, 信息值)"

hn_harness_report

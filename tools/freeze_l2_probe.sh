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

# ---- A. 一致性对照 ----
# exact(同输入双遍 → 命中路径): 两臂逐字节 cmp;
# approx(突变遍 → 含平移回放): 浮点与全新布局存在 ~1ulp 结合序差
#   (数学等价、像素不可见), 按 ≤0.01px 容差逐 token 对照。
HN_MEASURE_CACHE=0 HN_LAYOUT_SKIP=0 "$BIN" dump exact > "$D/off_exact.dump" 2> "$D/off.err"
HN_MEASURE_CACHE=0 HN_LAYOUT_SKIP=0 "$BIN" dump approx > "$D/off_approx.dump" 2>> "$D/off.err"
"$BIN" dump exact > "$D/on_exact.dump" 2> "$D/on.err"
"$BIN" dump approx > "$D/on_approx.dump" 2>> "$D/on.err"

NPASS=0; NFAIL=0; FAILED_DESCS=()

if cmp -s "$D/off_exact.dump" "$D/on_exact.dump"; then
  NPASS=$((NPASS + 1))
  echo "  ✔ 命中路径逐位一致(cmp: 同输入双遍转储)"
else
  NFAIL=$((NFAIL + 1))
  FAILED_DESCS+=("  ✘ 命中路径转储不一致(diff 见 $D)")
  diff "$D/off_exact.dump" "$D/on_exact.dump" | head -20 >&2 || true
fi

tokdiff="$(awk '
  NR == FNR { a[NR] = $0; na = NR; next }
  { nb++; b[nb] = $0 }
  END {
    if (na != nb) { print "LINECOUNT", na, nb; exit }
    bad = 0
    for (i = 1; i <= na; i++) {
      n = split(a[i], A, " "); m = split(b[i], B, " ")
      if (n != m) { print "TOKENS", i; bad++; continue }
      for (j = 1; j <= n; j++) {
        if (A[j] == B[j]) continue
        if (A[j] ~ /^-?[0-9.]+$/ && B[j] ~ /^-?[0-9.]+$/) {
          d = A[j] - B[j]; if (d < 0) d = -d
          if (d > 0.01) { print "FLOAT", i, j, A[j], B[j]; bad++ }
        } else { print "STR", i, j, A[j], B[j]; bad++ }
      }
    }
    print "BAD", bad
  }' "$D/off_approx.dump" "$D/on_approx.dump" | tail -1)"
if [ "$tokdiff" = "BAD 0" ]; then
  NPASS=$((NPASS + 1))
  echo "  ✔ 突变序列容差一致(≤0.01px, 逐 token)"
else
  NFAIL=$((NFAIL + 1))
  FAILED_DESCS+=("  ✘ 突变序列对照超容差: $tokdiff (diff 见 $D)")
  diff "$D/off_approx.dump" "$D/on_approx.dump" | head -20 >&2 || true
fi

# C 侧断言(命中/平移/结构自检)已随 "$BIN" dump 的退出码执行(set -e 兜底)

# ---- D. 提速口径(信息值): 三种负载各 300 遍纯布局 ----
for w in static stream resize; do
  ms_off="$(HN_MEASURE_CACHE=0 HN_LAYOUT_SKIP=0 "$BIN" bench 2>/dev/null | grep "bench-$w" | awk '{print $2}' | cut -d= -f2)"
  ms_on="$("$BIN" bench 2>/dev/null | grep "bench-$w" | awk '{print $2}' | cut -d= -f2)"
  ratio="$(awk -v a="$ms_off" -v b="$ms_on" 'BEGIN { if (b > 0) printf "%.2f", a / b; else print "?" }')"
  echo "  ℹ bench-$w 300 遍纯布局: 关=${ms_off}ms 开=${ms_on}ms (提速 ${ratio}x, 信息值)"
done

hn_harness_report

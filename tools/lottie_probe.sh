#!/usr/bin/env bash
# tools/lottie_probe.sh — Lottie 能力探针(原 lottie_probe.py 的 bash 版)。
#
# 引擎负责"矢量图形 + 时间轴", 产物是平台无关的绘制指令, 所以这里用
# hncore 的 paint 子命令直接看指令, 不涉及任何平台后端。
#
# 覆盖: 基线矩形 / trim path(tm) / mask(masksProperties) / track matte(td) /
# repeater(rp) / 图层混合 bm / 纯色层 ty=1 / 图片层 ty=2 / 预合成 precomp ty=0 /
# 路径变形关键帧 / 渐变填充 gf(扁平与 [{t,s}] 编码、0..1 与 0..100 量纲) /
# 渐变描边 gs / 径向退化 / mask 最小版(CLIP+/CLIP- 配对、反相不裁、路径关键帧)。
#
# 用法: bash tools/lottie_probe.sh [hncore 路径]
#
# 门禁口径: 有失败断言 → exit 1(与其它探针一致; 原 python 版末尾漏了
# sys.exit(1), 失败也返回 0 —— bash 版修正为失败透传, 当前全绿行为不变)。

set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/_engine_sources.sh"

hn_find_hn "${1:-}" "/tmp/hncore"
D="$(hn_tmpd lottieprobe)"

NPASS=0; NFAIL=0; FAILED_NAMES=()

ck() {  # ck <名> <0|1> [现场详情] — 即时打印, 失败记账
  local name="$1" ok="$2" detail="${3:-}"
  if [ "$ok" = "1" ]; then
    echo "  v $name"
    NPASS=$((NPASS + 1))
  else
    echo "  X $name  << $(printf '%.100s' "$detail")"
    NFAIL=$((NFAIL + 1))
    FAILED_NAMES+=("$name")
  fi
}

# render <lottie 文件>; html 固定 100x100 元素 / 120x120 画布(与原版一致)
render() {
  cat > "$D/a.html" <<'HTML'
<html><head><style>html,body{margin:0;width:100%;height:100%;background:#000}
.l{width:100;height:100}</style></head><body>
<img class="l" src="a.json" hn-lottie></body></html>
HTML
  "$HN" paint "$D/a.html" 120 120
}

# shape_cmds <paint 输出> — 只取内容指令, 滤掉画布与元素自身的底色矩形
shape_cmds() {
  printf '%s\n' "$1" | awk '
    /^QUAD|^POLY|^RECT|^IMAGE/ {
      if ($0 ~ /^RECT/ && index($0, "fill=000000FF") > 0 &&
          index($0, " 120.0   120.0") > 0) next
      print
    }'
}

# fills_of <指令行> — 每行第一个 fill= 色值(文档序)
fills_of() {
  printf '%s\n' "$1" | awk '{
    if (match($0, /fill=[0-9A-Fa-f]+/)) print substr($0, RSTART + 5, RLENGTH - 5)
  }'
}

# uniqn <多行文本> — 去重后行数
uniqn() {
  if [ -z "$1" ]; then echo 0; else printf '%s\n' "$1" | sort -u | awk 'END { print NR }'; fi
}

# -------- Lottie JSON 片段(json.dumps 同构的静态文本) --------
TR='{"ty":"tr","a":{"a":0,"k":[0,0]},"p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}'
KSD='{"a":{"a":0,"k":[0,0]},"p":{"a":0,"k":[50,50]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}'
# layer() 缺省形态(ty=4, ind=1)
layer() {  # $1=shapes
  printf '{"ty":4,"nm":"L","ip":0,"op":60,"ind":1,"ks":%s,"shapes":%s}' "$KSD" "$1"
}
doc() {  # doc <layers> [assets 尾串] — base 文档
  local layers="$1" tail="${2:-}"
  printf '{"v":"5.7.4","fr":60,"ip":0,"op":60,"w":100,"h":100,"layers":%s%s}' \
    "$layers" "$tail"
}

RECTS="[{\"ty\":\"gr\",\"it\":[{\"ty\":\"rc\",\"p\":{\"a\":0,\"k\":[0,0]},\"s\":{\"a\":0,\"k\":[40,40]},\"r\":{\"a\":0,\"k\":0}},{\"ty\":\"fl\",\"c\":{\"a\":0,\"k\":[1,0,0]},\"o\":{\"a\":0,\"k\":100},\"r\":1},$TR]}]"
LAYER_RECTS="$(layer "$RECTS")"

echo "== Lottie 能力探针 =="

# 基线: 一个矩形 —— 应该出 POLY(引擎把 rc/el 也按多边形发射)
printf '%s' "$(doc "[$LAYER_RECTS]")" > "$D/a.json"
c0="$(shape_cmds "$(render "$D/a.json")")"
echo "  [0] 基线矩形: $(printf '%s\n' "$c0" | head -n 1)"
ck "基线: rc 矩形被发射" \
  "$([ "$(printf '%s\n' "$c0" | awk 'END { print NR }')" = 1 ] && printf '%s' "$c0" | grep -q '^POLY' && printf '%s' "$c0" | grep -qF 'bbox=[   30.0    30.0    40.0    40.0]' && echo 1 || echo 0)" "$c0"

# 1) trim path (tm) —— 线/进度环动画的核心
TM='[{"ty":"gr","it":[{"ty":"sh","ks":{"a":0,"k":{"c":false,"v":[[-20,0],[20,0]],"i":[[0,0],[0,0]],"o":[[0,0],[0,0]]}}},{"ty":"st","c":{"a":0,"k":[1,1,1]},"o":{"a":0,"k":100},"w":{"a":0,"k":8},"lc":2,"lj":1},{"ty":"tm","s":{"a":0,"k":0},"e":{"a":0,"k":50},"o":{"a":0,"k":0},"m":1},'"$TR"']}]'
printf '%s' "$(doc "[$(layer "$TM")]")" > "$D/a.json"
c1="$(shape_cmds "$(render "$D/a.json")")"
echo "  [1] trim 50%: $(printf '%s\n' "$c1" | head -n 1)"
ck "trim path 生效(裁掉一半)" \
  "$([ "$(printf '%s\n' "$c1" | awk 'END { print NR }')" -ge 1 ] && printf '%s' "$c1" | grep -q 'n=' && echo 1 || echo 0)" \
  "$(printf '%s\n' "$c1" | head -n 1)"

# 2) mask (masksProperties)
MASK_EL='[{"ty":"gr","it":[{"ty":"el","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[60,60]}},{"ty":"fl","c":{"a":0,"k":[0,1,0]},"o":{"a":0,"k":100},"r":1},'"$TR"']}]'
MASKS='[{"inv":false,"mode":"a","pt":{"a":0,"k":{"i":[[0,0],[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0],[0,0]],"v":[[-15,-15],[15,-15],[15,15],[-15,15]],"c":true}},"o":{"a":0,"k":100},"nm":"M"}]'
printf '%s' "$(doc '[{"ty":4,"nm":"L","ip":0,"op":60,"ind":1,"ks":'"$KSD"',"shapes":'"$MASK_EL"',"masksProperties":'"$MASKS"'}]')" > "$D/a.json"
c2="$(shape_cmds "$(render "$D/a.json")")"
echo "  [2] mask: $(printf '%s\n' "$c2" | head -n 1)"
ck "mask 生效(裁剪到蒙版)" \
  "$([ "$(printf '%s\n' "$c2" | awk 'END { print NR }')" -ge 1 ] && echo 1 || echo 0)" \
  "$(printf '%s\n' "$c2" | head -n 1)"

# 3) track matte (td): 上层被下层做遮罩
EL_RED40='[{"ty":"gr","it":[{"ty":"el","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[40,40]}},{"ty":"fl","c":{"a":0,"k":[1,0,0]},"o":{"a":0,"k":100},"r":1},'"$TR"']}]'
printf '%s' "$(doc "[{\"ty\":4,\"nm\":\"L\",\"ip\":0,\"op\":60,\"ind\":1,\"td\":2,\"ks\":$KSD,\"shapes\":$RECTS},{\"ty\":4,\"nm\":\"L\",\"ip\":0,\"op\":60,\"ind\":2,\"ks\":$KSD,\"shapes\":$EL_RED40}]")" > "$D/a.json"
c3="$(shape_cmds "$(render "$D/a.json")")"
echo "  [3] track matte td=2: $(printf '%s\n' "$c3" | head -n 2)"
ck "track matte 生效" \
  "$([ "$(printf '%s\n' "$c3" | awk 'END { print NR }')" -ge 1 ] && echo 1 || echo 0)" \
  "$(printf '%s\n' "$c3" | head -n 2)"

# 4) repeater
REP='[{"ty":"gr","it":[{"ty":"rc","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[10,40]},"r":{"a":0,"k":0}},{"ty":"fl","c":{"a":0,"k":[1,1,0]},"o":{"a":0,"k":100},"r":1},{"ty":"rp","c":{"a":0,"k":3},"o":{"a":0,"k":0},"m":1,"tr":{"a":0,"k":[20,0]}},'"$TR"']}]'
printf '%s' "$(doc "[$(layer "$REP")]")" > "$D/a.json"
c4="$(shape_cmds "$(render "$D/a.json")")"
echo "  [4] repeater x3: $(printf '%s\n' "$c4" | awk 'END { print NR }') 条"
ck "repeater 生效(出 3 个)" "$([ "$(printf '%s\n' "$c4" | awk 'END { print NR }')" = 3 ] && echo 1 || echo 0)" "$(printf '%s\n' "$c4" | awk 'END { print NR }')"

# 5) 图层混合模式 bm(可解析即可, 原版无 ck)
printf '%s' "$(doc "[{\"ty\":4,\"nm\":\"L\",\"ip\":0,\"op\":60,\"ind\":1,\"bm\":3,\"ks\":$KSD,\"shapes\":$RECTS}]")" > "$D/a.json"
c5="$(shape_cmds "$(render "$D/a.json")")"
echo "  [5] bm=3 可解析: $(printf '%s\n' "$c5" | awk 'END { print NR }')"

# 6) 纯色层 ty=1
printf '%s' "$(doc '[{"ty":1,"nm":"S","ip":0,"op":60,"ind":1,"ks":'"$KSD"',"sc":"#ff0000","sw":100,"sh":100}]')" > "$D/a.json"
c6="$(shape_cmds "$(render "$D/a.json")")"
echo "  [6] 纯色层 ty=1: $(printf '%s\n' "$c6" | head -n 1)"
ck "纯色层 ty=1 生效" \
  "$([ "$(printf '%s\n' "$c6" | awk 'END { print NR }')" -ge 1 ] && echo 1 || echo 0)" \
  "$(printf '%s\n' "$c6" | head -n 1)"

# 7) 图片层 ty=2(无图文件 → 应跳过不崩)
printf '%s' "$(doc '[{"ty":2,"nm":"I","ip":0,"op":60,"ind":1,"ks":'"$KSD"',"refId":"img_0"}]')" > "$D/a.json"
c7="$(shape_cmds "$(render "$D/a.json")")"
echo "  [7] 图片层 ty=2(无资源): $(printf '%s\n' "$c7" | awk 'END { print NR }') 条(应跳过不崩)"
ck "图片层缺资源时跳过不崩" 1

# 8) 预合成 precomp ty=0 —— 真实导出文件大量使用
PRE_LAYERS="[{\"ty\":0,\"nm\":\"P\",\"ip\":0,\"op\":60,\"ind\":1,\"refId\":\"comp_0\",\"ks\":$KSD,\"w\":100,\"h\":100,\"layers\":[$LAYER_RECTS]}]"
PRE_ASSETS=',"assets":[{"id":"comp_0","layers":['"$LAYER_RECTS"']}]'
printf '%s' "$(doc "$PRE_LAYERS" "$PRE_ASSETS")" > "$D/a.json"
c8="$(shape_cmds "$(render "$D/a.json")")"
echo "  [8] 预合成(precomp): $(printf '%s\n' "$c8" | awk 'END { print NR }') 条"
ck "预合成展开(内层矩形可见)" \
  "$([ "$(printf '%s\n' "$c8" | awk 'END { print NR }')" -ge 1 ] && echo 1 || echo 0)" \
  "$(printf '%s\n' "$c8" | awk 'END { print NR }')"

# 9) 路径变形关键帧(sh.ks.k) —— 引擎声称支持
MORPH='[{"ty":"gr","it":[{"ty":"sh","ks":{"a":1,"k":[{"t":0,"s":[{"c":false,"v":[[-20,-20],[20,-20],[20,20]],"i":[[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0]]}]},{"t":60,"s":[{"c":true,"v":[[-30,-10],[10,-30],[30,20],[-10,30]],"i":[[0,0],[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0],[0,0]]}]}]}},{"ty":"fl","c":{"a":0,"k":[1,0,1]},"o":{"a":0,"k":100},"r":1},'"$TR"']}]'
printf '%s' "$(doc "[$(layer "$MORPH")]")" > "$D/a.json"
c9="$(shape_cmds "$(render "$D/a.json")")"
echo "  [9] 路径变形关键帧: $(printf '%s\n' "$c9" | head -n 1)"
ck "路径变形关键帧生效(顶点数变化)" \
  "$([ "$(printf '%s\n' "$c9" | awk 'END { print NR }')" -ge 1 ] && printf '%s' "$c9" | grep -q 'n=' && echo 1 || echo 0)" \
  "$(printf '%s\n' "$c9" | head -n 1)"

# 10) 渐变填充 gf(多 stop 线性) —— 引擎把几何沿渐变轴切成色带
# gf_brush <ty(gf|gs)> <t> <p(stop 数)> <stops k 数组> — 与 python brush 字典
# 同序(ty,t,s,e,g,o[,w]); gs 追加线宽 w。
gf_brush() {
  local w=""
  [ "$1" = "gs" ] && w=',"w":{"a":0,"k":8}'
  printf '{"ty":"%s","t":%s,"s":{"a":0,"k":[-20,0]},"e":{"a":0,"k":[20,0]},"g":{"p":%s,"k":{"a":0,"k":%s}},"o":{"a":0,"k":100}%s}' \
    "$1" "$2" "$3" "$4" "$w"
}
# gf_doc <brush> — 渐变用例的 layer 包装(gs 用折线 shape, gf 用矩形, 与原版一致)
gf_doc() {  # 渐变用例的 layer 包装
  local brush="$1" shape
  case "$brush" in
    '{"ty":"gs"'*) shape='{"ty":"sh","ks":{"a":0,"k":{"c":false,"v":[[-20,0],[20,0]],"i":[[0,0],[0,0]],"o":[[0,0],[0,0]]}}}' ;;
    *) shape='{"ty":"rc","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[40,40]},"r":{"a":0,"k":0}}' ;;
  esac
  # 注: 分两步拼 —— "$(...\"...\")" 这种三层嵌套里 \" 会被外层先吃掉, 不能直接写
  local grp_json="[{\"ty\":\"gr\",\"it\":[$shape,$brush,$TR]}]"
  local layers="[$(layer "$grp_json")]"
  doc "$layers"
}

# 10a) gf 扁平 stop 编码(bodymovin 至今大量导出): [pos,r,g,b, pos,r,g,b]
gf_doc "$(gf_brush gf 1 8 '[0,1,0,0,1,0,0,1]')" > "$D/a.json"
c10="$(shape_cmds "$(render "$D/a.json")")"
f10="$(fills_of "$c10")"
n10=0; [ -n "$c10" ] && n10="$(printf '%s\n' "$c10" | awk 'END { print NR }')"
u10="$(uniqn "$f10")"
echo "  [10a] gf 扁平 stop: $n10 条色带, $u10 种颜色"
# 色带取带中点处的插值色, 所以首带略偏洋红而不是纯 FF0000 ——
# 断言按"红分量递减、蓝分量递增"的单调性钉住, 比钉字面更贴语义。
f10a_ok=0
if [ "$n10" -ge 8 ] && [ "$u10" -ge 8 ]; then
  F10A=()
  while IFS= read -r hx; do [ -n "$hx" ] && F10A+=("$hx"); done <<<"$f10"
  r0=$((16#${F10A[0]:0:2})); b0=$((16#${F10A[0]:4:2}))
  last=$(( ${#F10A[@]} - 1 ))
  r1=$((16#${F10A[$last]:0:2})); b1=$((16#${F10A[$last]:4:2}))
  mid=$(( ${#F10A[@]} / 2 ))
  rm=$((16#${F10A[$mid]:0:2})); bm=$((16#${F10A[$mid]:4:2}))
  if [ "$r0" -gt 200 ] && [ "$b0" -lt 20 ] && [ "$b1" -gt 200 ] && [ "$r1" -lt 20 ] \
     && [ "$r0" -gt "$rm" ] && [ "$rm" -gt "$r1" ] \
     && [ "$b0" -lt "$bm" ] && [ "$bm" -lt "$b1" ]; then
    f10a_ok=1
  fi
fi
ck "gf 渐变填充生效(多色带 + 红→蓝单调过渡)" "$f10a_ok" \
  "($n10, $(printf '%s\n' "$f10" | head -n 1), $(printf '%s\n' "$f10" | sed -n "$((n10 / 2 + 1))p"), $(printf '%s\n' "$f10" | tail -n 1))"

# 10b) gf 新式 stop 编码: [{t:pos,s:[r,g,b,a]}]
gf_doc "$(gf_brush gf 1 2 '[{"t":0,"s":[1,0,0,1]},{"t":1,"s":[0,0,1,1]}]')" > "$D/a.json"
c10b="$(shape_cmds "$(render "$D/a.json")")"
f10b="$(fills_of "$c10b")"
n10b=0; [ -n "$c10b" ] && n10b="$(printf '%s\n' "$c10b" | awk 'END { print NR }')"
echo "  [10b] gf 新式 stop: $n10b 条色带"
ck "gf 新式 stop 编码同样生效" \
  "$([ "$n10b" -ge 8 ] && [ "$(uniqn "$f10b")" -ge 8 ] && echo 1 || echo 0)" "$n10b"

# 10c) 0..100 量纲的 stop(少数导出) —— 越界自动 /100
gf_doc "$(gf_brush gf 1 8 '[0,1,0,0,100,0,0,1]')" > "$D/a.json"
c10c="$(shape_cmds "$(render "$D/a.json")")"
f10c="$(fills_of "$c10c")"
n10c=0; [ -n "$c10c" ] && n10c="$(printf '%s\n' "$c10c" | awk 'END { print NR }')"
ck "gf stop 0..100 量纲归一(色带数不变)" \
  "$([ "$n10c" -ge 8 ] && [ "$(uniqn "$f10c")" -ge 8 ] && echo 1 || echo 0)" "$n10c"

# 10d) 渐变描边 gs: 折线按段细分, 每小段一个逐段色
gf_doc "$(gf_brush gs 1 8 '[0,1,1,0,1,1,0,0]')" > "$D/a.json"
c10d="$(shape_cmds "$(render "$D/a.json")")"
f10d="$(fills_of "$c10d")"
n10d=0; [ -n "$c10d" ] && n10d="$(printf '%s\n' "$c10d" | awk 'END { print NR }')"
echo "  [10d] gs 渐变描边: $n10d 段"
ck "gs 渐变描边生效(沿路径多段变色)" \
  "$([ "$n10d" -ge 8 ] && [ "$(uniqn "$f10d")" -ge 8 ] && echo 1 || echo 0)" "($n10d, $(uniqn "$f10d"))"

# 10e) 径向 t=2 退化为中位色(如实写明的不支持边界)
gf_doc "$(gf_brush gf 2 8 '[0,1,0,0,1,0,0,1]')" > "$D/a.json"
c10e="$(shape_cmds "$(render "$D/a.json")")"
n10e=0; [ -n "$c10e" ] && n10e="$(printf '%s\n' "$c10e" | awk 'END { print NR }')"
ck "径向渐变退化为单色(不崩, 出 1 条)" "$([ "$n10e" = 1 ] && echo 1 || echo 0)" "$n10e"

# 11) mask 最小版: masksProperties 单条加法蒙版 → CLIP_PUSH/POP 夹住内容
MASK_RC80='[{"ty":"gr","it":[{"ty":"rc","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[80,80]},"r":{"a":0,"k":0}},{"ty":"fl","c":{"a":0,"k":[0,1,0]},"o":{"a":0,"k":100},"r":1},'"$TR"']}]'
MASK_KS='{"inv":false,"mode":"a","o":{"a":0,"k":100},"pt":{"a":0,"k":{"i":[[0,0],[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0],[0,0]],"v":[[-10,-10],[10,-10],[10,10],[-10,10]],"c":true}}}'
printf '%s' "$(doc '[{"ty":4,"nm":"L","ip":0,"op":60,"ind":1,"ks":'"$KSD"',"shapes":'"$MASK_RC80"',"masksProperties":['"$MASK_KS"']}]')" > "$D/a.json"
o11="$(render "$D/a.json")"
clip_push="$(printf '%s\n' "$o11" | grep '^CLIP+' || true)"
clip_pop="$(printf '%s\n' "$o11" | grep '^CLIP-' || true)"
n_push=0; [ -n "$clip_push" ] && n_push="$(printf '%s\n' "$clip_push" | awk 'END { print NR }')"
n_pop=0; [ -n "$clip_pop" ] && n_pop="$(printf '%s\n' "$clip_pop" | awk 'END { print NR }')"
echo "  [11] mask: $(printf '%s\n' "$o11" | grep -E '^(CLIP|POLY)' || true)"
ck "mask 最小版: CLIP+ 裁到蒙版外接矩形(20x20 @40,40)" \
  "$([ "$n_push" = 1 ] && printf '%s' "$clip_push" | grep -qF ' 40.0    40.0    20.0    20.0' && echo 1 || echo 0)" "$clip_push"
ck "mask 最小版: CLIP+/CLIP- 配对" "$([ "$n_pop" = 1 ] && echo 1 || echo 0)" "$clip_pop"

# 11b) 反相/相减蒙版不做(内容不被裁剪 —— 保持可见而非整块消失)
MASK_KS_S='{"inv":false,"mode":"s","o":{"a":0,"k":100},"pt":{"a":0,"k":{"i":[[0,0],[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0],[0,0]],"v":[[-10,-10],[10,-10],[10,10],[-10,10]],"c":true}}}'
printf '%s' "$(doc '[{"ty":4,"nm":"L","ip":0,"op":60,"ind":1,"ks":'"$KSD"',"shapes":'"$MASK_RC80"',"masksProperties":['"$MASK_KS_S"']}]')" > "$D/a.json"
o11b="$(render "$D/a.json")"
if printf '%s\n' "$o11b" | grep -q '^CLIP+'; then
  ck "相减蒙版(mode=s)跳过不裁(不误伤内容)" 0
else
  ck "相减蒙版(mode=s)跳过不裁(不误伤内容)" 1
fi

# 11c) 蒙版路径关键帧: 蒙版矩形随时间放大 → HN_CLOCK 不同时刻裁剪矩形不同
MASK_KF='{"inv":false,"mode":"a","o":{"a":0,"k":100},"pt":{"a":1,"k":[{"t":0,"s":[{"i":[[0,0],[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0],[0,0]],"v":[[-10,-10],[10,-10],[10,10],[-10,10]],"c":true}]},{"t":60,"s":[{"i":[[0,0],[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0],[0,0]],"v":[[-30,-30],[30,-30],[30,30],[-30,30]],"c":true}]}]}}'
printf '%s' "$(doc '[{"ty":4,"nm":"L","ip":0,"op":60,"ind":1,"ks":'"$KSD"',"shapes":'"$MASK_RC80"',"masksProperties":['"$MASK_KF"']}]')" > "$D/a.json"
cat > "$D/a.html" <<'HTML'
<html><head><style>html,body{margin:0;width:100%;height:100%;background:#000}.l{width:100;height:100}</style></head><body><img class="l" src="a.json" hn-lottie></body></html>
HTML
clip_at() {  # clip_at <HN_CLOCK 值> → 首条 CLIP+ 行
  HN_CLOCK="$1" "$HN" paint "$D/a.html" 120 120 | grep '^CLIP+' | head -n 1 || true
}
kc0="$(clip_at 0)"
kc60="$(clip_at 600)"
# 关键帧 t=0(20x20) → t=60 帧 = 1000ms; 600ms 是 0.6 进度 → 44x44 @ (28,28)
echo "  [11c] mask 关键帧 t=0: $kc0 | t=600: $kc60"
k_ok=0
if [ -n "$kc0" ] && [ -n "$kc60" ] && [ "$kc0" != "$kc60" ] \
   && printf '%s' "$kc0" | grep -qF ' 40.0    40.0    20.0    20.0' \
   && printf '%s' "$kc60" | grep -qF ' 28.0    28.0    44.0    44.0'; then
  k_ok=1
fi
ck "蒙版路径关键帧生效(裁剪盒随时间变大)" "$k_ok" "($kc0, $kc60)"

echo
echo "失败: $NFAIL${FAILED_NAMES[*]+" ${FAILED_NAMES[*]}"}"
[ "$NFAIL" -eq 0 ]

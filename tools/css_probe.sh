#!/usr/bin/env bash
# tools/css_probe.sh — CSS/HTML 规范符合性探针 v2(原 css_probe.py 的 bash 版)
# —— 只测"该对却可能错了"的地方。
#
# 与 layout_probe.sh 的分工: 那个测盒几何, 这个测**规则与解析**
# (选择器优先级、简写展开、颜色格式、单位换算、继承、文本属性、属性选择器、
#  :not()/nth-of-type/calc、非等比缩放与 transform-origin、3D 投影、透明背板)。
# 全部用期望值对照, 找出静默错误。
#
# 用法: bash tools/css_probe.sh [hncore 路径]
#
# 门禁口径: 任何断言失败 → exit 1; 全过 → exit 0。
# 透明背板两断言用现场编译的 hn-pngstats(链引擎自带 hn_png.c, 见
# _engine_sources.sh)逐像素统计 alpha, 替代原 python 版内嵌的 zlib 解码器。

set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/_engine_sources.sh"

hn_find_hn "${1:-}" "/tmp/hncore"
D="$(hn_tmpd css)"

NPASS=0; NFAIL=0; FAILED_DESCS=()

# ck <名> <got> <want> [tol] — got==want 精确; 或两边都是数/四元组时按 tol
# (四元组缺省 tol=0 与 python 元组精确相等同口径, 单值缺省 1.0)。
ck() {
  local name="$1" got="$2" want="$3" tol="${4:-}"
  if [ "$got" = "$want" ]; then
    NPASS=$((NPASS + 1)); return
  fi
  if hn_val_eq "$got" "$want"; then NPASS=$((NPASS + 1)); return; fi
  local isnum='^-?[0-9.]+$'
  if printf '%s' "$got" | grep -qE -- "$isnum" && printf '%s' "$want" | grep -qE -- "$isnum"; then
    local t="${tol:-1}"
    if awk -v a="$got" -v b="$want" -v t="$t" 'BEGIN { d = a - b; if (d < 0) d = -d; exit !(d <= t + 1e-9) }'; then
      NPASS=$((NPASS + 1)); return
    fi
  fi
  hn_fail "$name" "$want" "$got" 36
}

# ck_bool <名> <0|1>
ck_bool() {
  ck "$1" "${2:-0}" "1"
}

# page <css> <body> → html 文档
page() {
  printf '<html><head><style>html,body{margin:0}%s</style></head><body>%s</body></html>' "$1" "$2"
}

# run_boxes <html> → "id x y w h" 行
run_boxes() {
  printf '%s' "$1" > "$D/c.html"
  "$HN" boxes "$D/c.html" 400 300 | hn_boxes_lines
}

# box1 <html> <id> — 第一条盒(python r.get(id)[0])
box1() {
  hn_box_of "$(run_boxes "$1")" "$2" first
}

# runs_raw <html> → "run ..." 原始行
runs_raw() {
  printf '%s' "$1" > "$D/t.html"
  "$HN" text "$D/t.html" 400 300 | grep '^run ' || true
}

# runs_n <html> → run 行数
runs_n() {
  local r
  r="$(runs_raw "$1")"
  if [ -z "$r" ]; then echo 0; else printf '%s\n' "$r" | awk 'END { print NR }'; fi
}

# run_field <runs 行> <键> → 数值(缺 → "")
run_field() {
  printf '%s\n' "$2" | awk -v key="$1" '{
    n = split($0, T, /[ \t]+/)
    for (i = 1; i <= n; i++) {
      t = T[i]
      if (t ~ /=$/) { k = substr(t, 1, length(t) - 1); v = T[i + 1]; i++ }
      else if (t ~ /=/) { eq = index(t, "="); k = substr(t, 1, eq - 1); v = substr(t, eq + 1) }
      else continue
      if (k == key && v ~ /^-?[0-9.]+$/) { print v; exit }
    }
  }'
}

# paint_run <html> — hncore paint 输出
paint_run() {
  printf '%s' "$1" > "$D/p.html"
  "$HN" paint "$D/p.html" 400 300
}

# paint_fill <css 声明> — 尺寸 100x50 的 RECT 的 fill 色值(大写 8 hex; 缺 → "")
paint_fill() {
  printf '%s' "$(page ".a{width:100;height:50;$1}" '<div class="a" id="x"></div>')" > "$D/p.html"
  "$HN" paint "$D/p.html" 400 300 | awk '
    /^RECT/ && index($0, "100.0") && index($0, "50.0") {
      if (match($0, /fill=[0-9A-Fa-f]{8}/))
        { s = toupper(substr($0, RSTART + 5, 8)); print s; exit }
    }'
}

# rect_stroke <paint 输出> — 首条 RECT 的 stroke 色值(大写; 缺 → "")
rect_stroke() {
  printf '%s\n' "$1" | awk '
    /^RECT/ {
      if (match($0, /stroke=[0-9A-Fa-f]{8}/))
        { print toupper(substr($0, RSTART + 7, 8)); exit }
    }'
}

# quads <html> — paint 输出里 RECT/QUAD 行(变换是否真应用的判据)
quads() {
  printf '%s' "$1" > "$D/t.html"
  "$HN" paint "$D/t.html" 400 300 | grep -E '^(RECT|QUAD)' || true
}

# ============ margin/padding 简写展开 ============
# 盒的 h 是 height 声明值(20); margin 不进入自身盒子, padding 会
for spec in ".a{margin:10 20}|20 10 100 20|简写 2 值 (上下10 左右20)" \
            ".a{margin:10 20 30}|20 10 100 20|简写 3 值 (上10 左右20 下30)" \
            ".a{margin:10 20 30 40}|40 10 100 20|简写 4 值 (TRBL)" \
            ".a{padding:10 20}|0 0 140 40|padding 2 值 (10 上下, 20 左右)"; do
  decl="${spec%%|*}"; rest="${spec#*|}"; want="${rest%%|*}"; label="${rest#*|}"
  got="$(box1 "$(page "$decl.a{width:100;height:20}" '<div class="a" id="x"></div>')" x)"
  ck "$label" "$got" "$want" 0
done

# ============ 单位换算 ============
for spec in ".a{font-size:20;width:10em}|200|em 基于本元素字号" \
            ".a{font-size:20;margin-top:50%}|200|margin % 基于容器宽(400×50%)" \
            ".a{font-size:20;width:50pt}|66.67|pt → px (×96/72)"; do
  decl="${spec%%|*}"; rest="${spec#*|}"; want="${rest%%|*}"; label="${rest#*|}"
  got="$(box1 "$(page "$decl.a{height:10}" '<div class="p" style="width:400;height:400"><div class="a" id="x"></div></div>')" x)"
  if [ "$got" != "MISSING" ]; then
    # python 口径: css 里有 width/em/pt → 比宽度(分量 3), 否则比 y(分量 2)
    case "$decl" in
      *width*|*em*|*pt*) fld=3 ;;
      *) fld=2 ;;
    esac
    ck "$label" "$(printf '%s' "$got" | awk -v f="$fld" '{print $f}')" "$want" 1.5
  fi
done

# rem 基于根字号
ck "rem 基于根字号" \
  "$(box1 "$(page "html{font-size:25}.a{height:4rem;width:4rem}" '<div class="a" id="x"></div>')" x)" \
  "0 0 100 100" 0

# ============ 颜色解析(经 background 落到指令) ============
for spec in "background:#f00|FF0000FF|hex 3 位" \
            "background:#ff0000|FF0000FF|hex 6 位" \
            "background:#ff000080|FF000080|hex 8 位(带 alpha)" \
            "background:#ff0000ff|FF0000FF|hex 8 位(全不透明)" \
            "background:rgb(0,128,255)|0080FFFF|rgb()" \
            "background:rgba(0,128,255,0.5)|0080FF7F|rgba() 50%" \
            "background:rgba(0 128 255 / 0.5)|0080FF7F|rgba() 空格+斜杠写法" \
            "background:rgb(0 128 255)|0080FFFF|rgb() 空格写法" \
            "background:rebeccapurple|663399FF|命名色 rebeccapurple"; do
  decl="${spec%%|*}"; rest="${spec#*|}"; want="${rest%%|*}"; label="${rest#*|}"
  ck "$label" "$(paint_fill "$decl")" "$want"
done

# ============ 选择器优先级 ============
# 同优先级后者胜; id > class > tag
ck "id 选择器优先于 class" \
  "$(box1 "$(page '#x{background:#f00;width:50;height:20}.a{width:200;height:20}' '<div class="a" id="x"></div>')" x)" \
  "0 0 50 20" 0

ck "同特异性后定义者胜" \
  "$(box1 "$(page '.a{width:50;height:20}.b{width:200;height:20}' '<div class="a b" id="x"></div>')" x)" \
  "0 0 200 20" 0

ck "class 优先于 tag" \
  "$(box1 "$(page 'div{width:50;height:20}.a{width:200;height:20}' '<div class="a" id="x"></div>')" x)" \
  "0 0 200 20" 0

ck "内联 style 最高优先" \
  "$(box1 "$(page '.a{width:50;height:20}' '<div class="a" id="x" style="width:200;height:20"></div>')" x)" \
  "0 0 200 20" 0

# ============ 组合器 ============
ck "子代组合器 >" \
  "$(box1 "$(page '.p > .c{width:200;height:20}.q .c{width:50;height:20}' '<div class="p"><div class="c" id="x"></div></div>')" x)" \
  "0 0 200 20" 0

ck "后代组合器(任意深度)" \
  "$(box1 "$(page '.p .c{width:200;height:20}' '<div class="p"><div class="wrap"><div class="c" id="x"></div></div></div>')" x)" \
  "0 0 200 20" 0

# ============ nth-child ============
r="$(run_boxes "$(page '.c:nth-child(2){width:200;height:20}.c{width:50;height:20}' '<div class="p"><div class="c" id="a"></div><div class="c" id="b"></div></div>')")"
ck ":nth-child(2) 命中第二个" "$(hn_box_of "$r" b first)" "0 20 200 20" 0

r="$(run_boxes "$(page '.c:nth-child(even){width:200;height:20}.c{width:50;height:20}' '<div class="p"><div class="c" id="a"></div><div class="c" id="b"></div><div class="c" id="c"></div><div class="c" id="d"></div></div>')")"
ck ":nth-child(even) 命中 2/4" "$(hn_box_of "$r" b first)" "0 20 200 20" 0
ck ":nth-child(even) 跳过 1" "$(hn_box_of "$r" a first)" "0 0 50 20" 0
ck ":nth-child(even) 命中 4" "$(hn_box_of "$r" d first)" "0 60 200 20" 0

# ============ flex: align-self 覆盖 align-items ============
ck "align-self 覆盖 align-items" \
  "$(box1 "$(page '.f{display:flex;width:300;height:100;align-items:flex-start}.c{width:50;height:30}.s{align-self:flex-end}' '<div class="f"><div class="c s" id="x"></div></div>')" x)" \
  "0 70 50 30" 0

# ============ flex: order ============
ck "order:-1 排到最前" \
  "$(box1 "$(page '.f{display:flex;width:300;height:50}.c{width:50;height:20}.z{order:-1}' '<div class="f"><div class="c" id="a"></div><div class="c z" id="b"></div>')" b)" \
  "0 0 50 20" 0

# ============ flex-basis ============
bx="$(box1 "$(page '.f{display:flex;width:300;height:50}.a{flex-basis:200;height:20}.b{flex-grow:1;height:20}' '<div class="f"><div class="a" id="x"></div><div class="b" id="y"></div>')" x)"
if [ "$bx" != "MISSING" ]; then
  ck "flex-basis:200 作初始主轴尺寸" "$(printf '%s' "$bx" | awk '{print $3}')" "200" 2
fi
ck "flex-grow 吃剩余" \
  "$(box1 "$(page '.f{display:flex;width:300;height:50}.a{flex-basis:200;height:20}.b{flex-grow:1;height:20}' '<div class="f"><div class="a" id="x"></div><div class="b" id="y"></div>')" y)" \
  "200 0 100 20" 0

# ============ 继承 ============
bx="$(box1 "$(page '.p{font-size:30;color:#f00;line-height:2}.c{width:100;font-size:inherit}' '<div class="p"><div class="c" id="x">文</div>')" x)"
# 行高 2 × 30 = 60 → 盒高至少 60
if [ "$bx" != "MISSING" ]; then
  ck "line-height 倍数随字号生效" "$(printf '%s' "$bx" | awk '{print $4}')" "60" 4
fi

bx="$(box1 "$(page 'html{font-size:20}.p{font-size:200%;line-height:1}.c{width:100}' '<div class="p"><div class="c" id="x">文</div>')" x)"
if [ "$bx" != "MISSING" ]; then
  ck "font-size:200% 基于父字号" "$(printf '%s' "$bx" | awk '{print $4}')" "40" 4
fi

# ============ 文本属性 ============
# white-space:nowrap 不换行
PRE_TXT='<div class="p">一二三四五六七八</div>'
ck_bool "默认自动换行(多行)" \
  "$([ "$(runs_n "$(page '.p{width:40;font-size:14}' "$PRE_TXT")")" -gt 1 ] && echo 1 || echo 0)"
ck "white-space:nowrap 不换行" \
  "$(runs_n "$(page '.p{width:40;font-size:14;white-space:nowrap}' "$PRE_TXT")")" "1"

# text-align:center 使行起点 > 0
rr="$(runs_raw "$(page '.p{width:200;font-size:14;text-align:center}' '<div class="p">居中文本</div>')")"
if [ -n "$rr" ]; then
  rx="$(run_field x "$(printf '%s\n' "$rr" | head -n 1)")"
  if awk -v v="$rx" 'BEGIN { exit !(v > 5) }'; then
    ck_bool "text-align:center 行有左边距" 1
  else
    ck_bool "text-align:center 行有左边距" 0
  fi
fi

rr="$(runs_raw "$(page '.p{width:200;font-size:14;text-align:right}' '<div class="p">右对齐</div>')")"
if [ -n "$rr" ]; then
  rl="$(printf '%s\n' "$rr" | head -n 1)"
  rw="$(run_field w "$rl")"; rx="$(run_field x "$rl")"
  if awk -v x="$rx" -v w="$rw" 'BEGIN { d = (x + w) - 200; if (d < 0) d = -d; exit !(d < 3) }'; then
    ck_bool "text-align:right 贴右边" 1
  else
    ck_bool "text-align:right 贴右边" 0
  fi
fi

# ============ HTML 语义: 嵌套列表 ============
bx="$(box1 "$(page 'ul,ol{margin:0;padding-left:20}li{list-style:none;width:50;height:16}' '<ul><li id="a">一<ul><li id="b">二</li></ul></li></ul>')" b)"
if [ "$bx" != "MISSING" ]; then
  ck "嵌套列表缩进累加(父20+子20)" "$(printf '%s' "$bx" | awk '{print $1}')" "40" 2
fi

# ============ overflow 容器不压缩内容 ============
r="$(run_boxes "$(page '.c{width:100;height:100;overflow:hidden}.k{width:80;height:80;margin:0}' '<div class="c" id="box"><div class="k" id="x"></div>')")"
ck "overflow 容器尺寸不受内容影响" "$(hn_box_of "$r" box first)" "0 0 100 100" 0

# ============ 排版单位语义(来自 css_probe_v3) ============
lineh() {  # lineh <decl> → 盒 "x y w h"(python lineh())
  box1 "$(page "$1.k{width:100}" "<div class='p'><div class='k' id='x'>文</div>")" x
}

# line-height 三种单位: 无单位=倍数 / px=绝对 / %=相对字号 / em=相对字号
for spec in ".p{font-size:20;line-height:1.5}|30|line-height:1.5 → 30px" \
            ".p{font-size:20;line-height:150%}|30|line-height:150% → 30px" \
            ".p{font-size:20;line-height:30px}|30|line-height:30px → 30px" \
            ".p{font-size:20;line-height:2em}|40|line-height:2em → 40px"; do
  decl="${spec%%|*}"; rest="${spec#*|}"; want="${rest%%|*}"; label="${rest#*|}"
  bx="$(lineh "$decl")"
  if [ "$bx" != "MISSING" ]; then
    ck "$label" "$(printf '%s' "$bx" | awk '{print $4}')" "$want" 5
  fi
done

# 无单位数字是倍数(CSS 语义) —— 不是"像素"
bx="$(lineh '.p{font-size:20;line-height:30}')"
if [ "$bx" != "MISSING" ]; then
  ck "line-height:30 是倍数(30×20=600)" "$(printf '%s' "$bx" | awk '{print $4}')" "600" 5
fi

# box-sizing 与 padding+border 叠加
ck "border-box: 宽含 padding+border" \
  "$(box1 "$(page '.a{width:100;height:100;padding:10;border:5 solid #f00;box-sizing:border-box}' "<div class='a' id='x'></div>")" x)" \
  "0 0 100 100" 0
ck "content-box: 宽加 padding+border" \
  "$(box1 "$(page '.a{width:100;height:100;padding:10;border:5 solid #f00}' "<div class='a' id='x'></div>")" x)" \
  "0 0 130 130" 0

# 逐侧 padding 长写
ck "padding-top 长写" \
  "$(box1 "$(page '.a{width:100;height:100;padding-top:40}' "<div class='a' id='x'></div>")" x)" \
  "0 0 100 140" 0
ck "padding-left 长写" \
  "$(box1 "$(page '.a{width:100;height:100;padding-left:40}' "<div class='a' id='x'></div>")" x)" \
  "0 0 140 100" 0

# 通配符与选择器组
ck "通配符 * 应用 box-sizing" \
  "$(box1 "$(page '*{box-sizing:border-box}.a{width:100;height:100;padding:20}' "<div class='a' id='x'></div>")" x)" \
  "0 0 100 100" 0
r="$(run_boxes "$(page '.a, .b{width:200;height:20}' "<div class='a' id='x'></div><div class='b' id='y'></div>")")"
ck "选择器组 命中 a" "$(hn_box_of "$r" x first)" "0 0 200 20" 0
ck "选择器组 命中 b" "$(hn_box_of "$r" y first)" "0 20 200 20" 0

# 相邻兄弟 +
r="$(run_boxes "$(page '.a + .b{width:200;height:20}.a{width:50;height:20}' "<div class='a' id='a'></div><div class='b' id='b'></div>")")"
ck "相邻兄弟 + 命中紧邻" "$(hn_box_of "$r" b first)" "0 20 200 20" 0
r="$(run_boxes "$(page '.a + .b{width:200;height:20}' "<div class='a' id='a'></div><div class='c'></div><div class='b' id='b'></div>")")"
bb="$(hn_box_of "$r" b first)"
if [ "$bb" != "MISSING" ]; then
  ck "相邻兄弟 + 不命中非紧邻(宽度回落到块级默认 400)" "$(printf '%s' "$bb" | awk '{print $3}')" "400" 1
fi

# column-gap
r="$(run_boxes "$(page '.a{display:flex;width:300;height:50;column-gap:20}.c{width:50;height:20}' "<div class='a'><div class='c' id='x'></div><div class='c' id='y'></div>")")"
ck "column-gap 生效" "$(hn_box_of "$r" y first)" "70 0 50 20" 0

# flex-wrap
bb="$(box1 "$(page '.f{display:flex;width:100;height:200;flex-wrap:wrap}.c{width:60;height:20}' "<div class='f'><div class='c' id='x'></div><div class='c' id='y'></div>")" y)"
if [ "$bb" != "MISSING" ]; then
  if awk -v v="$(printf '%s' "$bb" | awk '{print $2}')" 'BEGIN { exit !(v > 10) }'; then
    ck_bool "flex-wrap 换到第二行" 1
  else
    ck_bool "flex-wrap 换到第二行" 0
  fi
fi

# ============ 级联与继承(系统性逐属性覆盖检查) ============
# 逐属性用 .a{prop:v1} .b{prop:v2} 验证后声明者胜出 —— 这是抓"守卫挡住赋值"
# 这类 bug 最直接的办法(height 就曾因 HN_U_AUTO 守卫而只认第一条规则)。
paint_of() {  # paint_of <html> — pz.html 版 paint(python paint_of())
  printf '%s' "$1" > "$D/pz.html"
  "$HN" paint "$D/pz.html" 400 300
}

# 颜色类覆盖: 红 → 绿
for prop in "background" "background-color"; do
  o="$(paint_of "$(page ".a{$prop:#ff0000}.b{$prop:#00ff00}.c{width:100;height:50}" "<div class='a b c'></div>")")"
  if printf '%s' "$o" | grep -q '00FF00'; then
    ck_bool "级联: $prop 后声明者胜" 1
  else
    ck_bool "级联: $prop 后声明者胜" 0
  fi
done

# color 落在 TEXT 指令的 fill 上
o="$(paint_of "$(page '.a{color:#ff0000}.b{color:#00ff00}' "<div class='a b'>文</div>")")"
if printf '%s' "$o" | grep -q 'fill=00FF00FF'; then
  ck_bool "级联: color 后声明者胜(TEXT fill)" 1
else
  ck_bool "级联: color 后声明者胜(TEXT fill)" 0
fi

# border-color 落在 RECT 的 stroke 上
o="$(paint_of "$(page '.a{border:5 solid #ff0000}.b{border-color:#00ff00}.c{width:100;height:50}' "<div class='a b c'></div>")")"
ck "级联: border-color 后声明者胜(RECT stroke)" "$(rect_stroke "$o")" "00FF00FF"

# 渐变覆盖
o="$(paint_of "$(page '.a{background:linear-gradient(0deg,#f00,#00f)}.b{background:linear-gradient(90deg,#0f0,#ff0)}.c{width:100;height:50}' "<div class='a b c'></div>")")"
if printf '%s' "$o" | grep -q 'gradient'; then
  ck_bool "级联: 渐变后声明者胜" 1
else
  ck_bool "级联: 渐变后声明者胜" 0
fi

# 继承链(含隔代)与子级覆盖
bx="$(box1 "$(page '.p{font-size:40}.k{width:100}' "<div class='p'><div class='k' id='x'>文</div>")" x)"
if [ "$bx" != "MISSING" ]; then
  ck "继承: font-size 传到子元素(行高随之)" "$(printf '%s' "$bx" | awk '{print $4}')" "58" 8
fi
bx="$(box1 "$(page '.p{font-size:30}.m{}.k{width:100}' "<div class='p'><div class='m'><div class='k' id='x'>文</div>")" x)"
if [ "$bx" != "MISSING" ]; then
  ck "继承: 隔代传递(孙级)" "$(printf '%s' "$bx" | awk '{print $4}')" "43.5" 6
fi
bx="$(box1 "$(page '.p{font-size:40}.k{font-size:10;width:100}' "<div class='p'><div class='k' id='x'>文</div>")" x)"
if [ "$bx" != "MISSING" ]; then
  ck "继承: 子级显式值覆盖继承值" "$(printf '%s' "$bx" | awk '{print $4}')" "14.5" 4
fi

# 特异性: id > 多 class
ck "特异性: id 高于任意数量 class" \
  "$(box1 "$(page '#x{height:200}.a{height:50}.b{height:80}' "<div class='a b' id='x'></div>")" x)" \
  "0 0 400 200" 0

# ============ 选择器引擎强化(2026-09-19) ============
# 这一批是先探针后实现: 声明了不生效也不报错的静默失败组。

# 属性选择器六种操作符
ck "[attr] 存在选择器" \
  "$(box1 "$(page '[data-on]{height:30}' "<div data-on='1' id='x'></div><div id='y'></div>")" x)" \
  "0 0 400 30" 0
ck "[attr=v] 全等" \
  "$(box1 "$(page '[data-k="v"]{height:30}' "<div data-k='v' id='x'></div><div data-k='w' id='y'></div>")" x)" \
  "0 0 400 30" 0
ck "[attr^=] 前缀" \
  "$(box1 "$(page '[href^="ht"]{height:30}' "<div href='https://a' id='x'></div>")" x)" \
  "0 0 400 30" 0
ck "[attr$=] 后缀" \
  "$(box1 "$(page '[href$="cn"]{height:30}' "<div href='https://a.cn' id='x'></div>")" x)" \
  "0 0 400 30" 0
ck "[attr*=] 子串" \
  "$(box1 "$(page '[data-k*="mid"]{height:30}' "<div data-k='a-mid-b' id='x'></div>")" x)" \
  "0 0 400 30" 0
ck "[attr~=] 空白分词之一全等" \
  "$(box1 "$(page '[rel~="no"]{height:30}' "<div rel='yes no maybe' id='x'></div><div rel='yesno' id='y'></div>")" x)" \
  "0 0 400 30" 0

# :not()
r="$(run_boxes "$(page '.i{height:20}.i:not(.x){height:40}' "<div class='i a' id='x'></div><div class='i x' id='y'></div>")")"
ck ":not(.x) 否定 class" "$(hn_box_of "$r" x first)" "0 0 400 40" 0
ck ":not() 不影响未否定的" "$(hn_box_of "$r" y first)" "0 40 400 20" 0
r="$(run_boxes "$(page '.i{height:20}.i:not(#z){height:40}' "<div class='i' id='x'></div><div class='i' id='z'></div>")")"
ck ":not(#z) 否定 id" "$(hn_box_of "$r" x first)" "0 0 400 40" 0

# :nth-of-type(只数同标签; 与 nth-child 混用是表格/列表错位的根源)
r="$(run_boxes "$(page '.i{height:10}.i:nth-of-type(2){height:50}' "<div><div class='i' id='a'></div><div class='i' id='b'></div><div class='i' id='c'></div></div>")")"
ck ":nth-of-type(2) 命中第二个" "$(hn_box_of "$r" b first)" "0 10 400 50" 0
ck ":nth-of-type(2) 不影响其他" \
  "$(printf '%s' "$(hn_box_of "$r" a first)" | awk '{print $4}')" "10" 0
# 混排: 中间插一个不同标签的元素, nth-of-type 应忽略它, nth-child 应算它
r="$(run_boxes "$(page 'div.t{height:10}span.s{height:5;display:block}div.t:nth-of-type(2){height:50}' "<div><div class='t' id='a'></div><span class='s'></span><div class='t' id='b'></div>")")"
ck "nth-of-type 忽略异类兄弟" \
  "$(printf '%s' "$(hn_box_of "$r" b first)" | awk '{print $4}')" "50" 0
r="$(run_boxes "$(page 'div.t{height:10}span.s{height:5;display:block}div.t:nth-child(2){height:50}' "<div><div class='t' id='a'></div><span class='s'></span><div class='t' id='b'></div>")")"
ck "nth-child 把异类兄弟计入" \
  "$(printf '%s' "$(hn_box_of "$r" b first)" | awk '{print $4}')" "10" 0

# calc()
ck "calc(100% - 40px)" \
  "$(box1 "$(page '#x{width:calc(100% - 40px);height:10}' "<div id='x'></div>")" x)" \
  "0 0 360 10" 0
ck "calc(50% + 10px) 按包含块" \
  "$(box1 "$(page '#p{width:200}#x{width:calc(50% + 10px);height:10}' "<div id='p'><div id='x'></div>")" x)" \
  "0 0 110 10" 0
ck "calc 纯绝对值就地求值" \
  "$(box1 "$(page '#x{width:calc(100px + 20px);height:10}' "<div id='x'></div>")" x)" \
  "0 0 120 10" 0
cw="$(printf '%s' "$(box1 "$(page '#x{width:calc(100% - 900px)}' "<div id='x'></div>")" x)" | awk '{print $3}')"
if [ "$cw" != "MISSING" ] && [ -n "$cw" ] && awk -v v="$cw" 'BEGIN { exit !(v >= 0) }'; then
  ck_bool "calc 负值 clamp 到 0" 1
else
  ck_bool "calc 负值 clamp 到 0" 0
fi

# ============ 变换引擎(非等比缩放 / transform-origin) ============
# 先探针后实现: 三项都是"写了不生效"且不报错。
R="<div id='c' style='width:100;height:40;background:#ff0000;%s'></div>"

q="$(quads "$(printf "$R" "transform:scale(2,1)")")"
nq=0; [ -n "$q" ] && nq="$(printf '%s\n' "$q" | awk 'END { print NR }')"
if [ "$nq" = 1 ] && printf '%s' "$q" | grep -q ' 200.0' && printf '%s' "$q" | grep -q ' 40.0'; then
  ck_bool "scale(x,y) 非等比(200x40)" 1
else
  ck_bool "scale(x,y) 非等比(200x40)" 0
fi
q="$(quads "$(printf "$R" "transform:scaleX(2)")")"
nq=0; [ -n "$q" ] && nq="$(printf '%s\n' "$q" | awk 'END { print NR }')"
if [ "$nq" = 1 ] && printf '%s' "$q" | grep -q ' 200.0' && printf '%s' "$q" | grep -q ' 40.0'; then
  ck_bool "scaleX(2) 生效(200x40)" 1
else
  ck_bool "scaleX(2) 生效(200x40)" 0
fi
q="$(quads "$(printf "$R" "transform:scaleY(2)")")"
nq=0; [ -n "$q" ] && nq="$(printf '%s\n' "$q" | awk 'END { print NR }')"
if [ "$nq" = 1 ] && printf '%s' "$q" | grep -q ' 100.0' && printf '%s' "$q" | grep -q ' 80.0'; then
  ck_bool "scaleY(2) 生效(100x80)" 1
else
  ck_bool "scaleY(2) 生效(100x80)" 0
fi
# transform-origin: 0 0 绕左上角转 90° —— 100x40 应变成 40x100 且向左下延伸
q0="$(quads "$(printf "$R" "transform:rotate(90deg);transform-origin:0 0")")"
qc="$(quads "$(printf "$R" "transform:rotate(90deg)")")"
nq0=0; [ -n "$q0" ] && nq0="$(printf '%s\n' "$q0" | awk 'END { print NR }')"
if [ "$q0" != "$qc" ] && [ "$nq0" = 1 ]; then
  ck_bool "transform-origin 生效(与缺省输出不同)" 1
else
  ck_bool "transform-origin 生效(与缺省输出不同)" 0
fi
# 绕左上角转 90°: 角点应落在 (0,0) 与 (-40,100) 上
if [ "$nq0" = 1 ] && printf '%s' "$q0" | grep -qF '(0.0,0.0)' && printf '%s' "$q0" | grep -qF '(-40.0,100.0)'; then
  ck_bool "transform-origin:0 0 的旋转角点正确" 1
else
  ck_bool "transform-origin:0 0 的旋转角点正确" 0
fi
# 缺省 origin(盒中心)旋转后中心应仍约在原盒中心
if [ -n "$qc" ] && [ "$(printf '%s\n' "$qc" | awk 'END { print NR }')" = 1 ] && printf '%s' "$qc" | grep -qF '(30.0,70.0)'; then
  ck_bool "缺省 transform-origin 仍为盒中心" 1
else
  ck_bool "缺省 transform-origin 仍为盒中心" 0
fi

# ============ 3D 投影(translateZ / rotate3d) ============
# 消失点是 perspective-origin = 父盒中心(CSS 口径)。父 300x200 在 (0,0),
# 中心 (150,100)。translateZ(200) → k=2, translateZ(-200) → k=2/3。
P3="<div id='p' style='width:300;height:200;perspective:400'><div id='c' style='width:100;height:60;background:#ff0000;%s'></div></div>"

q="$(quads "$(printf "$P3" "")")"
if [ "$(printf '%s\n' "$q" | awk 'END { print NR }')" = 1 ] && printf '%s' "$q" | grep -q '^RECT'; then
  ck_bool "3D 基线: 无变换走 RECT" 1
else
  ck_bool "3D 基线: 无变换走 RECT" 0
fi
q="$(quads "$(printf "$P3" "transform:translateZ(200px)")")"
if [ "$(printf '%s\n' "$q" | awk 'END { print NR }')" = 1 ] && printf '%s' "$q" | grep -q '^QUAD'; then
  ck_bool "translateZ(200) 走四边形(投影生效)" 1
else
  ck_bool "translateZ(200) 走四边形(投影生效)" 0
fi
# k=2 绕 (150,100): 角点 (0,0)→(-150,-100), (100,60)→(50,20)
if [ "$(printf '%s\n' "$q" | awk 'END { print NR }')" = 1 ] && printf '%s' "$q" | grep -qF '(-150.0,-100.0)' && printf '%s' "$q" | grep -qF '(50.0,20.0)'; then
  ck_bool "translateZ(200) 缩放 k=2(角点正确)" 1
else
  ck_bool "translateZ(200) 缩放 k=2(角点正确)" 0
fi
q="$(quads "$(printf "$P3" "transform:translateZ(-200px)")")"
# k = 400/(400+200) = 2/3 绕 (150,100): (0,0)→(50.0,33.3), (100,60)→(116.7,73.3)
if [ "$(printf '%s\n' "$q" | awk 'END { print NR }')" = 1 ] && printf '%s' "$q" | grep -qF '(50.0,33.3)' && printf '%s' "$q" | grep -qF '(116.7,73.3)'; then
  ck_bool "translateZ(-200) 缩小 k=2/3(角点正确)" 1
else
  ck_bool "translateZ(-200) 缩小 k=2/3(角点正确)" 0
fi

# rotate3d(0,1,0,45deg) 与 rotateY(45deg) 必须**完全一致**(绕 Y 轴就是 rotateY)
q1="$(quads "$(printf "$P3" "transform:rotate3d(0,1,0,45deg)")")"
q2="$(quads "$(printf "$P3" "transform:rotateY(45deg)")")"
if [ "$q1" = "$q2" ] && [ "$(printf '%s\n' "$q1" | awk 'END { print NR }')" = 1 ]; then
  ck_bool "rotate3d(0,1,0,45) 与 rotateY(45) 输出一致" 1
else
  ck_bool "rotate3d(0,1,0,45) 与 rotateY(45) 输出一致" 0
fi
if [ "$(printf '%s\n' "$q1" | awk 'END { print NR }')" = 1 ] && printf '%s' "$q1" | grep -q '^QUAD'; then
  ck_bool "rotate3d 走四边形" 1
else
  ck_bool "rotate3d 走四边形" 0
fi

# ============ 透明背景层(端到端) ============
# 这是核心卖点之一: hn-transparent 的文档必须真的逐像素透明。
# 两个曾导致"声明了透明却是一块实色"的 bug 都在光栅器里(见
# tools/hnsoft_alpha_probe.c), 所以这里用真实示例做端到端断言。
PNGSTATS="$(hn_pngstats_build)" || PNGSTATS=""
alpha_stats() {  # alpha_stats <png> → "trpct papct"(每 7 像素抽样)
  "$PNGSTATS" "$1" | awk '{ print $6, $7 }' | tr ',' '\n' | sed 's/trpct=//;s/papct=//' | paste -sd' ' -
}

_tr="$D/tr.png"
render_rc=0
(cd "$HN_REPO_ROOT" && "$HN" render "examples/transparent.html" "$_tr" 460 560) >/dev/null 2>&1 || render_rc=$?
if [ "$render_rc" = 0 ] && [ -s "$_tr" ] && [ -n "$PNGSTATS" ]; then
  stats="$(alpha_stats "$_tr")"
  trpct="${stats%% *}"; papct="${stats#* }"
  if awk -v v="$trpct" 'BEGIN { exit !(v > 60.0) }'; then
    ck_bool "透明背板: 文档大面积真透明(>60%)" 1
  else
    ck_bool "透明背板: 文档大面积真透明(>60%)" 0
  fi
  if awk -v v="$papct" 'BEGIN { exit !(v > 1.0) }'; then
    ck_bool "透明背板: 有半透明过渡(抗锯齿)" 1
  else
    ck_bool "透明背板: 有半透明过渡(抗锯齿)" 0
  fi
else
  ck_bool "透明背板: 示例可渲染" 0
fi

# 未声明透明的文档必须仍然完全不透明(别把普通页面也弄透明了)
_op="$D/op.png"
render_rc=0
(cd "$HN_REPO_ROOT" && "$HN" render "examples/dashboard.html" "$_op" 460 560) >/dev/null 2>&1 || render_rc=$?
if [ "$render_rc" = 0 ] && [ -s "$_op" ] && [ -n "$PNGSTATS" ]; then
  stats="$(alpha_stats "$_op")"
  trpct2="${stats%% *}"
  if awk -v v="$trpct2" 'BEGIN { exit !(v + 0 == 0.0) }'; then
    ck_bool "普通文档仍完全不透明(0% 透明)" 1
  else
    ck_bool "普通文档仍完全不透明(0% 透明)" 0
  fi
else
  ck_bool "普通文档仍完全不透明(0% 透明)" 0
fi

# ============ H5 标准扩展(aspect-ratio / :root / :only-child) ============
# aspect-ratio: 一维定、另一维 auto → 按 w/h 补齐(布局近似盒尺寸口径)。
r="$(box1 '<html><head><style>html,body{margin:0}
.a{aspect-ratio:16/9;width:320}
</style></head><body><div class="a" id="a"></div></body></html>' a)"
ck "aspect-ratio 16/9 → 高=宽*9/16" "$r" "0 0 320 180"
r="$(box1 '<html><head><style>html,body{margin:0}
.b{aspect-ratio:2;height:60}
</style></head><body><div class="b" id="b"></div></body></html>' b)"
ck "aspect-ratio 反向(高定宽补)" "$r" "0 0 120 60"
# 显式两维都在 → ratio 不插手
r="$(box1 '<html><head><style>html,body{margin:0}
.b{aspect-ratio:2;width:120;height:60}
</style></head><body><div class="b" id="b"></div></body></html>' b)"
ck "aspect-ratio 两维显式不覆盖" "$r" "0 0 120 60"

# :root 命中真实 html(包裹根不算); 作者变量随根生效
r="$(run_boxes '<html><head><style>:root{font-size:20px}
.a{width:5rem;height:10}
</style></head><body><div class="a" id="a"></div></body></html>')"
ck ":root 字号 → rem 基准 20px" "$(hn_box_of "$r" a first)" "8 8 100 10"

# :only-child / :first-child / :last-child 协同
r="$(run_boxes '<html><head><style>html,body{margin:0}
div{height:10}
.oc:only-child{background:#00ff00}
</style></head><body><div><div class="oc" id="a"></div></div>
<div><p></p><div id="b"></div></div></body></html>')"
got="$(hn_box_of "$r" a first)"
if [ "$got" != "MISSING" ]; then NPASS=$((NPASS+1)); else hn_fail ":only-child 命中(唯一子)" "存在" "MISSING"; fi
r="$(box1 '<html><head><style>html,body{margin:0}
.oc:only-child{width:88;height:10}
</style></head><body><div><div class="oc" id="a"></div></div></body></html>' a)"
ck ":only-child 样式生效" "$r" "0 0 88 10"

# ============ H5 第二轮(min/max/clamp / visibility 保布局) ============
r="$(box1 '<html><head><style>html,body{margin:0}
.a{width:min(80vw,300px);height:max(40px,10vh)}
</style></head><body><div class="a" id="a"></div></body></html>' a)"
ck "min(80vw,300px)/max(40px,10vh)" "$r" "0 0 300 40"
r="$(box1 '<html><head><style>html,body{margin:0}
.b{width:clamp(100px,50vw,260px);height:20px}
</style></head><body><div class="b" id="b"></div></body></html>' b)"
ck "clamp(100px,50vw,260px) → 200" "$r" "0 0 200 20"
# clamp(MIN,VAL,MAX) ≡ max(MIN,min(VAL,MAX)): MAX<MIN 时 MIN 赢(CSS 规范)
r="$(box1 '<html><head><style>html,body{margin:0}
.b{width:clamp(300px,10vw,260px);height:10px}
</style></head><body><div class="b" id="b"></div></body></html>' b)"
ck "clamp MAX<MIN → MIN 赢(规范语义)" "$r" "0 0 300 10"
# visibility:hidden 保布局(盒还在, display:none 才摘)
r="$(box1 '<html><head><style>html,body{margin:0}
.hid{visibility:hidden;width:120;height:40}
.nx{width:90;height:30}
</style></head><body><div class="hid"></div><div class="nx" id="nx"></div></body></html>' nx)"
ck "visibility:hidden 保布局(兄弟位置不被上提)" "$r" "0 40 90 30"

# ============ 伪元素 ::before/::after(生成内容) ============
# content 字面文本照画(文档序: before → 内容 → after)
printf '<html><head><style>html,body{margin:0;background:#000;color:#fff}.tag::after{content:"NEW";font-size:10px;color:#e5484d}.tag::before{content:"[ ";color:#999}</style></head><body><span class="tag">功能名称</span></body></html>' > "$D/gen.html"
if "$HN" paint "$D/gen.html" 300 100 | grep '^TEXT' | grep -q 'NEW'; then NPASS=$((NPASS+1));
else hn_fail "::after content 文本绘制" "NEW" "未命中"; fi
if "$HN" paint "$D/gen.html" 300 100 | grep '^TEXT' | grep -q '\['; then NPASS=$((NPASS+1));
else hn_fail "::before content 文本绘制" "[ " "未命中"; fi
# 伪元素声明转内联样式: ::after 的 font-size:10 生效(NEW 用 sz=10 而非继承 16)
if "$HN" paint "$D/gen.html" 300 100 | grep '^TEXT' | grep 'NEW' | grep -q 'sz=10'; then NPASS=$((NPASS+1));
else hn_fail "伪元素声明转节点样式(font-size)" "sz=10" "未生效"; fi
if "$HN" paint "$D/gen.html" 300 100 | grep '^TEXT' | grep 'NEW' | grep -q 'E5484D'; then NPASS=$((NPASS+1));
else hn_fail "伪元素声明转节点样式(color)" "E5484D" "未生效"; fi
# content:none / 缺 content = 不生成
printf '<html><head><style>html,body{margin:0}.n::after{content:none}</style></head><body><span class="n">x</span></body></html>' > "$D/gennone.html"
_n=$( "$HN" paint "$D/gennone.html" 200 100 | grep -c '^TEXT' || true )
if [ "$_n" = "1" ]; then NPASS=$((NPASS+1));
else hn_fail "content:none 不生成(仅元素自身文本 1 条)" "1" "$_n"; fi
# 子级复显: hidden 父下 visible 子级的文本照画(标准继承语义)
printf '<html><head><style>html,body{margin:0;background:#000;color:#fff}.h{visibility:hidden}.h .show{visibility:visible}</style></head><body><div class="h">ghost<div class="show">复显</div></div></body></html>' > "$D/vo.html"
if "$HN" paint "$D/vo.html" 200 100 | grep '^TEXT' | grep -q '复显'; then NPASS=$((NPASS+1));
else hn_fail "子级 visible 复显(文本绘制)" "复显" "未命中"; fi
if "$HN" paint "$D/vo.html" 200 100 | grep '^TEXT' | grep -q 'ghost'; then hn_fail "hidden 文本不画" "无 ghost" "有";
else NPASS=$((NPASS+1)); fi

# ============ 报告 ============
hn_harness_report

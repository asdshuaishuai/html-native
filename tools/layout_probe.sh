#!/usr/bin/env bash
# tools/layout_probe.sh — 布局/排版探针(原 layout_probe.py 的 bash 版)。
# 用期望值对照, 找出真实的排版/布局缺陷。
#
# 用法:
#     bash tools/layout_probe.sh [hncore 路径]
#
# 数据源是 hncore(纯 C 引擎 CLI): 不经过平台运行时, 因此探到的就是
# 引擎核心的行为, 也是 macOS / Linux / Windows 三平台共用的那部分。
#
# 存在的意义: 以下三类缺陷都属于"写了样式却毫无反应"——
# 不报错、不崩溃, 只能靠对照期望值才能发现:
#   1. margin 长写(margin-top/left/…)全部失效(token 被 auto 判定吃掉)
#   2. 相邻 margin 不折叠(间距凭空翻倍, 破坏垂直节奏)
#   3. flex-shrink 不生效(子项溢出并互相重叠)
#   4. justify-content: space-around / space-evenly 静默退化为 flex-start
#   5. 脱离流子元素嵌在定位父里时, 文字被排两遍(绘制指令里同一句话出现
#      两次, 第二份整体偏移 (left, top); 盒坐标正常, 只有指令数翻倍)
# 所以每次改布局/样式代码都值得跑一遍。

set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/_engine_sources.sh"

hn_find_hn "${1:-}" "/tmp/hncore"
D="$(hn_tmpd probe)"

NPASS=0; NFAIL=0; FAILED_DESCS=()

# boxes <html> [w] [h] — 跑 hncore boxes, 返回 "id x y w h" 行(每 id 取最后,
# 与原 python dict 覆盖语义一致)
boxes() {
  printf '%s' "$1" > "$D/case.html"
  "$HN" boxes "$D/case.html" "${2:-400}" "${3:-300}" | hn_boxes_lines
}

# ck <名> <got-quad|MISSING> <wx> <wy> <ww> <wh> — 四元组精确相等
ck() {
  if hn_quad_eq "$2" "$3" "$4" "$5" "$6"; then
    NPASS=$((NPASS + 1))
  else
    hn_fail "$1" "($3, $4, $5, $6)" "$2"
  fi
}

# near <名> <got-quad|MISSING> <wx> <wy> <ww> <wh> [tol=1]
near() {
  local tol="${7:-1}"
  if hn_quad_near "$2" "$3" "$4" "$5" "$6" "$tol"; then
    NPASS=$((NPASS + 1))
  else
    hn_fail "$1" "($3, $4, $5, $6)" "$2"
  fi
}

# get <boxes 行> <id> — res.get(id, "MISSING")
get() {
  hn_box_of "$1" "$2"
}

# ============ flex: 主轴对齐 ============
flex_case() {  # $1=extra 样式  $2..=子项 id 列表
  local extra="$1"; shift
  local ids="$*" body="" i
  for i in $ids; do
    body="$body"'<div class="c" id="'"$i"'"></div>'
  done
  printf '<html><head><style>html,body{margin:0}.f{display:flex;width:300;height:100%s}.c{width:50;height:30}</style></head><body><div class="f">%s</div></body></html>' \
    "$extra" "$body"
}

# 注意: .c 给了显式 height:30, 因此 align-items 默认 stretch 不拉伸(有定高就不拉)
r="$(boxes "$(flex_case ";justify-content:flex-start" a)")"
near "justify flex-start" "$(get "$r" a)" 0 0 50 30

r="$(boxes "$(flex_case ";justify-content:center" a)")"
near "justify center" "$(get "$r" a)" 125 0 50 30

r="$(boxes "$(flex_case ";justify-content:flex-end" a)")"
near "justify flex-end" "$(get "$r" a)" 250 0 50 30

r="$(boxes "$(flex_case ";justify-content:space-between" a b c)")"
near "space-between a" "$(get "$r" a)" 0 0 50 30
near "space-between b" "$(get "$r" b)" 125 0 50 30
near "space-between c" "$(get "$r" c)" 250 0 50 30

r="$(boxes "$(flex_case ";justify-content:space-around" a b)")"
# 两侧 = 间隙的一半: 间隙=(300-100)/2=100 → a 在 50, b 在 200
near "space-around a" "$(get "$r" a)" 50 0 50 30
near "space-around b" "$(get "$r" b)" 200 0 50 30

r="$(boxes "$(flex_case ";justify-content:space-evenly" a b)")"
# 三段各 100/3≈33.3 → a 在 66.7, b 在 183.3
near "space-evenly a" "$(get "$r" a)" 66.67 0 50 30
near "space-evenly b" "$(get "$r" b)" 183.33 0 50 30

# ============ flex: 交叉轴 ============
# (align-items 各值, 期望 x/y/w/h)
for al in flex-start flex-end center stretch; do
  case "$al" in
    flex-start) exp="0 0 50 30" ;;
    flex-end)   exp="0 70 50 30" ;;
    center)     exp="0 35 50 30" ;;
    stretch)    exp="0 0 50 30" ;;
  esac
  r="$(boxes "$(flex_case ";align-items:$al" a)")"
  # shellcheck disable=SC2086
  near "align-items:$al" "$(get "$r" a)" $exp
done

# ============ flex: 列方向 ============
col_case() {  # $1=extra 样式  $2..=子项 id 列表
  local extra="$1"; shift
  local ids="$*" body="" i
  for i in $ids; do
    body="$body"'<div class="c" id="'"$i"'"></div>'
  done
  printf '<html><head><style>html,body{margin:0}.f{display:flex;flex-direction:column;width:100;height:200%s}.c{width:50;height:30}</style></head><body><div class="f">%s</div></body></html>' \
    "$extra" "$body"
}

r="$(boxes "$(col_case ";justify-content:flex-end" a b)")"
near "column flex-end a" "$(get "$r" a)" 0 140 50 30
near "column flex-end b" "$(get "$r" b)" 0 170 50 30

r="$(boxes "$(col_case ";justify-content:space-between" a b)")"
near "column space-between a" "$(get "$r" a)" 0 0 50 30
near "column space-between b" "$(get "$r" b)" 0 170 50 30

# ============ grow / shrink ============
r="$(boxes '<html><head><style>html,body{margin:0}
.f{display:flex;width:300;height:100}.a{flex:1;height:20}.b{flex:3;height:20}
</style></head><body><div class="f"><div class="a" id="a"></div>
<div class="b" id="b"></div></div></body></html>')"
near "grow 1:3 a" "$(get "$r" a)" 0 0 75 20
near "grow 1:3 b" "$(get "$r" b)" 75 0 225 20

r="$(boxes '<html><head><style>html,body{margin:0}
.f{display:flex;width:200;height:100}
.a{width:150;height:20;flex-shrink:1}.b{width:150;height:20;flex-shrink:1}
</style></head><body><div class="f"><div class="a" id="a"></div>
<div class="b" id="b"></div></div></body></html>')"
near "shrink 1:1 a" "$(get "$r" a)" 0 0 100 20
near "shrink 1:1 b" "$(get "$r" b)" 100 0 100 20

r="$(boxes '<html><head><style>html,body{margin:0}
.f{display:flex;width:200;height:100}
.a{width:150;height:20;flex-shrink:1}.b{width:150;height:20;flex-shrink:3}
</style></head><body><div class="f"><div class="a" id="a"></div>
<div class="b" id="b"></div></div></body></html>')"
# 溢出 100, 按 1:3 → a 缩 25, b 缩 75 → 125 / 75
near "shrink 1:3 a" "$(get "$r" a)" 0 0 125 20
near "shrink 1:3 b" "$(get "$r" b)" 125 0 75 20

# ============ gap ============
r="$(boxes "$(flex_case ";gap:20" a b)")"
near "gap a" "$(get "$r" a)" 0 0 50 30
near "gap b" "$(get "$r" b)" 70 0 50 30

# ============ margin 折叠 ============
r="$(boxes '<html><head><style>html,body{margin:0}
.a{width:100;height:30;margin:20}
</style></head><body><div class="a" id="a"></div><div class="a" id="b"></div></body></html>')"
near "margin 起点 a" "$(get "$r" a)" 20 20 100 30
near "margin 折叠 b(取最大值 40 而非相加 20)" "$(get "$r" b)" 20 70 100 30

# ============ 百分比 ============
r="$(boxes '<html><head><style>html,body{margin:0}
.p{width:400;height:200}.c{width:50%;height:50}
</style></head><body><div class="p"><div class="c" id="a"></div></div></body></html>')"
near "百分比宽" "$(get "$r" a)" 0 0 200 50

# ============ box-sizing ============
r="$(boxes '<html><head><style>html,body{margin:0}
.c{width:100;height:100;padding:10;box-sizing:border-box}
</style></head><body><div class="c" id="a"></div></body></html>')"
near "border-box" "$(get "$r" a)" 0 0 100 100

r="$(boxes '<html><head><style>html,body{margin:0}
.c{width:100;height:100;padding:10}
</style></head><body><div class="c" id="a"></div></body></html>')"
near "content-box" "$(get "$r" a)" 0 0 120 120

# ============ min/max 约束 ============
r="$(boxes '<html><head><style>html,body{margin:0}
.f{display:flex;width:300;height:100}.a{width:400;max-width:120;height:20}
</style></head><body><div class="f"><div class="a" id="a"></div></div></body></html>')"
near "max-width 生效" "$(get "$r" a)" 0 0 120 20

r="$(boxes '<html><head><style>html,body{margin:0}
.f{display:flex;width:300;height:100}.a{width:20;min-width:120;height:20}
</style></head><body><div class="f"><div class="a" id="a"></div></div></body></html>')"
near "min-width 生效" "$(get "$r" a)" 0 0 120 20

# ============ 脱离流嵌套: 同一句文字只许进指令表一次 ============
# 回归用例。layout_out_of_flow 曾在摆放完 ch 之后又手动递归一次, 而
# layout_box 尾部**已经**调用过 layout_out_of_flow(容器尺寸已定才摆浮层),
# 于是 ch 的脱离流后代被整棵重排: 盒坐标因 translate 幂等而看不出异常,
# 但 run_push 只追加不清空 —— TEXT 指令直接翻倍(背景 RECT 仍只有一份),
# 第二份整体偏移 (left, top)。只能数绘制指令才发现。
#
# 判据: 同一段文字, "绝对定位元素直接挂 body 下"与"嵌在绝对定位父容器里"
# 的 TEXT 条数必须相等。不去硬编码条数 —— 句子在字体回退下的切段数随
# FreeType 有无而变(中文可能切成多段), 相等性才是与字体无关的不变量。
text_cmds() {  # text_cmds <body 片段> <额外样式> — TEXT 指令条数
  printf '<html><head><style>html,body{margin:0}%s</style></head><body>%s</body></html>' \
    "$2" "$1" > "$D/case.html"
  "$HN" paint "$D/case.html" 240 120 | grep -c '^TEXT' || true
}

NEST_STYLE='.chip{position:absolute;left:22;top:20;font-size:12px;color:#fff}
.stage{position:absolute;left:0;top:0;width:240;height:120;background:#101828}
.rel{position:relative;left:0;top:0;width:240;height:120;background:#101828}'

n_flat="$(text_cmds '<div class="chip" id="chip">网格变形</div>' "$NEST_STYLE")"
n_abs="$(text_cmds '<div class="stage"><div class="chip" id="chip">网格变形</div></div>' "$NEST_STYLE")"
n_rel="$(text_cmds '<div class="rel"><div class="chip" id="chip">网格变形</div></div>' "$NEST_STYLE")"

# <名> <got> <want>: 相等即通过。hn_fail 恒返回 0, set -e 下可安全置于 else。
eqint() {
  if [ "$2" = "$3" ]; then NPASS=$((NPASS + 1)); else hn_fail "$1" "$3" "$2"; fi
}

if [ "$n_flat" -gt 0 ]; then NPASS=$((NPASS + 1)); else hn_fail "文字指令基线(须 >0)" ">0" "$n_flat"; fi
eqint "absolute 嵌 absolute: 文字不得翻倍" "$n_abs" "$n_flat"
eqint "absolute 嵌 relative: 文字不得翻倍" "$n_rel" "$n_flat"

# 脱离流子元素的盒位置本身仍要正确(父类 padding 参与参照块)
r="$(boxes '<html><head><style>html,body{margin:0}
.p{position:absolute;left:0;top:0;width:300;height:200;padding:10}
.c{position:absolute;left:5;top:7;width:40;height:20}
</style></head><body><div class="p"><div class="c" id="a"></div></div></body></html>')"
ck "absolute 参照父 padding box" "$(get "$r" a)" 15 17 40 20

# 孙级脱离流元素: 相对最近的定位祖先, 不叠加中间 static 祖先的偏移
r="$(boxes '<html><head><style>html,body{margin:0}
.p{position:absolute;left:0;top:0;width:300;height:200}
.m{width:100;height:100}
.c{position:absolute;left:11;top:13;width:40;height:20}
</style></head><body><div class="p"><div class="m"><div class="c" id="a"></div></div></div></body></html>')"
ck "跳过 static 中间层找定位祖先" "$(get "$r" a)" 11 13 40 20

# ============ 百分比高度链(html,body{height:100%} → 子元素 100%) ============
# 回归用例(ISSUES-1 P1-C 的根因代理)。透明真窗 + Lottie 桌宠页曾只画一角:
# `html,body{margin:0;width:100%;height:100%}` + `img{width:100%;height:100%}`
# —— 文档包裹根(html)曾参与作者 CSS 匹配 + body 百分比高解析为 0, 于是 img
# 的 height:100% 拿到 0, 播放器只按退化盒画出一角。
# 判据: 952x784 视口里, 这条链上的 img 盒必须铺满整个视口。
# (同一链路也是 `html,body{height:100%}` + flex 居中的通用页面骨架。)
r="$(boxes '<html><head><style>html,body{margin:0;width:100%;height:100%}
img{width:100%;height:100%;display:block}
</style></head><body><img id="a" src="x.json" hn-lottie></body></html>' 952 784)"
ck "body height:100% → img 100% 铺满视口" "$(get "$r" a)" 0 0 952 784

# 同链路 + flex 居中(桌宠面板写法): 子盒必须在 952x784 内居中
r="$(boxes '<html><head><style>html,body{margin:0;width:100%;height:100%}
body{display:flex;align-items:center;justify-content:center}
.stage{width:300;height:200}
</style></head><body><div class="stage" id="a"></div></body></html>' 952 784)"
ck "html,body 100% + flex 居中" "$(get "$r" a)" 326 292 300 200

# ============ 按钮控件(shrink-to-fit + 居中标签) ============
# <button> 元素与 <input type=button|submit|reset> 按标签内容收缩(浏览器
# 惯例); 标签在收缩后的盒内水平居中。曾按可用宽排版标签再收缩盒,
# 标签落在盒外(实测复现)。
r="$(boxes '<html><head><style>html,body{margin:0}
button{font-size:16px}
</style></head><body><button id="a">按钮</button></body></html>' 400 300)"
got="$(get "$r" a)"
if [ "$got" = "MISSING" ]; then hn_fail "button 元素盒存在" "存在" "MISSING";
else
  bw="$(printf '%s' "$got" | awk '{print $3}')"
  if awk -v w="$bw" 'BEGIN{exit !(w>30 && w<200)}'; then NPASS=$((NPASS+1));
  else hn_fail "button 按内容收缩(非整行)" "30<宽<200" "$bw"; fi
fi
r="$(boxes '<html><head><style>html,body{margin:0}
</style></head><body><input type="button" id="a" value="OK">
<input type="text" id="b" value="x"></body></html>' 400 300)"
near "input type=button 收缩(非 180 缺省)" "$(get "$r" a)" 0 0 44 51 4
r="$(boxes '<html><head><style>html,body{margin:0}
</style></head><body><input type="text" id="b" value="x"></body></html>' 400 300)"
near "input type=text 保持 180 缺省宽" "$(get "$r" b)" 0 0 200 51 4

# ============ 报告 ============
hn_harness_report

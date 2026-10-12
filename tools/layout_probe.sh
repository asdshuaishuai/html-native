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

# ============ 报告 ============
hn_harness_report

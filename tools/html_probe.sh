#!/usr/bin/env bash
# tools/html_probe.sh — HTML 生态适配探针(原 html_probe.py 的 bash 版)。
# 解析层面的真实行为。
#
# 与 css_probe.sh(规则与解析)、layout_probe.sh(盒几何)并列, 这份测 HTML 侧:
# 标签语义、属性处理、实体、注释、结构容错、空白语义。
#
# 存在的意义: 有一整类缺陷是"解析器默默改写了内容" —— 不报错、不崩溃,
# 只是渲染结果与源码不符。最典型的是空白折叠: tokenizer 曾无条件把连续空白
# 压成一个空格, 于是 white-space:pre 与 normal 输出完全一致, 代码块与对齐
# 文本全错。这类问题只能靠期望值对照发现。
#
# 用法: bash tools/html_probe.sh [hncore 路径]

set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/_engine_sources.sh"

hn_find_hn "${1:-}" "/tmp/hncore"
D="$(hn_tmpd html)"

NPASS=0; NFAIL=0; FAILED_DESCS=()

ck() {  # ck <名> <got> <want> — 精确相等(计数/字符串/元组; 元组按数值逐分量比)
  if hn_val_eq "$2" "$3"; then
    NPASS=$((NPASS + 1))
  else
    hn_fail "$1" "$3" "$2" 40
  fi
}

ck_bool() {  # ck_bool <名> <0|1> — 期望 True
  ck "$1" "${2:-0}" "1"
}

_w() {  # _w <tag> <html> → 文件路径
  printf '%s' "$2" > "$D/$1.html"
  printf '%s\n' "$D/$1.html"
}

# parse <html> → 设置 P_OUT / P_RC
parse() {
  local p
  p="$(_w p "$1")"
  P_OUT="$("$HN" parse "$p")"
  P_RC=$?
}

# boxes <html> [w] [h] → "id x y w h" 行(每 id 取第一条, 与原 python 一致)
boxes() {
  local p
  p="$(_w b "$1")"
  "$HN" boxes "$p" "${2:-400}" "${3:-300}" | hn_boxes_lines
}

# box1 <html> <id> — 单 id 盒(缺省视口; 缺失 → MISSING)
box1() {
  hn_box_of "$(boxes "$1")" "$2" first
}

# runs <html> [w] [h] → "run ..." 行
runs() {
  local p
  p="$(_w t "$1")"
  "$HN" text "$p" "${2:-400}" "${3:-300}" | grep '^run ' || true
}

run_count() {  # run_count <runs 输出> → 行数
  if [ -z "$1" ]; then echo 0; else printf '%s\n' "$1" | awk 'END { print NR }'; fi
}

# run_advance <runs 输出> — 最后一个片段的 x+w 最大值(总推进宽度)
run_advance() {
  printf '%s\n' "$1" | awk '{
    x = ""; w = ""
    if (match($0, /x=[ \t]*-?[0-9.]+/)) x = substr($0, RSTART + 2, RLENGTH - 2)
    if (match($0, /w=[ \t]*[0-9.]+/)) w = substr($0, RSTART + 2, RLENGTH - 2)
    gsub(/[ \t]/, "", x); gsub(/[ \t]/, "", w)
    if (x != "" && w != "") { v = x + w; if (v > best) best = v }
  } END { print best + 0 }'
}

# ============ 1. 基础语义 ============

r="$(runs '<html><body><div>&lt;a&gt; &amp; &#65;</div></body></html>')"
if printf '%s' "$r" | grep -q '&lt;' || printf '%s' "$r" | grep -q '&amp;'; then
  ck_bool "HTML 实体被解码" 0
else
  ck_bool "HTML 实体被解码" 1
fi

parse '<html><body><!-- 注释文字 --><div id='"'"'x'"'"'>内容</div></body></html>'
if printf '%s' "$P_OUT" | grep -q '注释文字'; then
  ck_bool "注释不进入 DOM" 0
else
  ck_bool "注释不进入 DOM" 1
fi

r="$(boxes '<html><head><style>html,body{margin:0}</style></head><body>
<div id='"'"'x'"'"' style='"'"'width:100;height:30'"'"'></div><br/>
<div id='"'"'y'"'"' style='"'"'width:100;height:30'"'"'></div></body></html>')"
by="$(hn_box_of "$r" y first)"
if [ "$by" != "MISSING" ]; then
  ytop="$(printf '%s' "$by" | awk '{print $2}')"
  if awk -v t="$ytop" 'BEGIN { exit !(t >= 30) }'; then
    ck_bool "自闭合 <br/> 不吞后续内容" 1
  else
    ck_bool "自闭合 <br/> 不吞后续内容" 0
  fi
fi

b="$(box1 '<html><head><style>html,body{margin:0}</style></head><body>
<img id='"'"'i'"'"' src='"'"'x.png'"'"' style='"'"'width:50;height:50'"'"'>
<div id='"'"'d'"'"' style='"'"'width:100;height:30'"'"'></div></body></html>' d)"
ck_bool "void 元素(img)后仍有兄弟" "$([ "$b" != "MISSING" ] && echo 1 || echo 0)"

ck "未知标签可用(块级)" \
  "$(box1 '<html><head><style>html,body{margin:0}</style></head><body>
<my-widget id='"'"'x'"'"' style='"'"'width:100;height:30'"'"'></my-widget></body></html>' x)" \
  "0 0 100 30"

ck "标签与属性名大小写不敏感" \
  "$(box1 '<html><head><style>html,body{margin:0}</style></head><body>
<DIV ID='"'"'x'"'"' STYLE='"'"'width:100;height:30'"'"'></DIV></body></html>' x)" \
  "0 0 100 30"

ck "跨行属性解析" \
  "$(box1 '<html><head><style>html,body{margin:0}</style></head><body>
<div id='"'"'x'"'"'
     class='"'"'a'"'"'
     style='"'"'width:100;height:30'"'"'></div></body></html>' x)" \
  "0 0 100 30"

if [ "$(box1 '<html><head><style>html,body{margin:0}</style></head><body>
<div id='"'"'x'"'"' style='"'"'width:100;height:30'"'"'>文本</body></html>' x)" != "MISSING" ]; then
  ck_bool "未闭合 div 仍解析" 1
else
  ck_bool "未闭合 div 仍解析" 0
fi

r="$(runs '<html><head><style>.a{color:red}</style></head><body>
<script>var x = 1;</script><div>可见</div></body></html>')"
if [ -n "$r" ] && printf '%s\n' "$r" | grep -q 'var'; then
  ck_bool "script/style 内容不作可见文本" 0
elif printf '%s\n' "$r" | grep -q 'color'; then
  ck_bool "script/style 内容不作可见文本" 0
else
  ck_bool "script/style 内容不作可见文本" 1
fi

r="$(boxes '<html><head><style>html,body{margin:0}ul,ol{margin:0;padding-left:20}
li{list-style:none;width:40;height:14}</style></head><body>
<ul><li id='"'"'l1'"'"'>一<ul><li id='"'"'l2'"'"'>二<ul><li id='"'"'l3'"'"'>三</li></ul>
</li></ul></li></ul></body></html>')"
l1="$(hn_box_of "$r" l1)"; l2="$(hn_box_of "$r" l2)"; l3="$(hn_box_of "$r" l3)"
if [ "$l1" != "MISSING" ] && [ "$l2" != "MISSING" ] && [ "$l3" != "MISSING" ]; then
  if awk -v a="$l1" -v b="$l2" -v c="$l3" 'BEGIN {
    split(a, A, " "); split(b, B, " "); split(c, C, " ")
    exit !(C[1] > B[1] && B[1] > A[1])
  }'; then
    ck_bool "三级嵌套列表递增缩进" 1
  else
    ck_bool "三级嵌套列表递增缩进" 0
  fi
fi

# ============ 2. 属性处理 ============

parse "<html><body><div title='a > b' id='x'>文</div></body></html>"
if [ "$P_RC" = 0 ] && printf '%s' "$P_OUT" | grep -q 'a > b'; then
  ck_bool "属性值内含 >" 1
else
  ck_bool "属性值内含 >" 0
fi

parse '<html><body><div title="say &quot;hi&quot;" id="x">文</div></body></html>'
ck_bool "属性值内含 &quot;" "$([ "$P_RC" = 0 ] && echo 1 || echo 0)"

parse "<html><body><div id=x class=a>文</div></body></html>"
if [ "$P_RC" = 0 ] && printf '%s' "$P_OUT" | grep -q 'id='; then
  ck_bool "无引号属性值" 1
else
  ck_bool "无引号属性值" 0
fi

parse "<html><body><input id='x' value=''></body></html>"
ck_bool "空属性值" "$([ "$P_RC" = 0 ] && echo 1 || echo 0)"

parse "<html><body><div id='first' id='second'>文</div></body></html>"
ck_bool "重复属性不崩" "$([ "$P_RC" = 0 ] && echo 1 || echo 0)"

parse "<html><body><div 数据='值' id='x'>文</div></body></html>"
if [ "$P_RC" = 0 ] && printf '%s' "$P_OUT" | grep -q '数据'; then
  ck_bool "中文属性名" 1
else
  ck_bool "中文属性名" 0
fi

parse "<html><body><div data-a-b-c='1' aria-label='x' id='y'>文</div></body></html>"
if [ "$P_RC" = 0 ] && printf '%s' "$P_OUT" | grep -q 'data-a-b-c'; then
  ck_bool "多连字符属性名" 1
else
  ck_bool "多连字符属性名" 0
fi

parse "<html><body><div id='x' data-count='5'>文</div></body></html>"
if [ "$P_RC" = 0 ] && printf '%s' "$P_OUT" | grep -q 'data-count'; then
  ck_bool "data-* 属性保留" 1
else
  ck_bool "data-* 属性保留" 0
fi

ck "style 里的空格不破坏解析" \
  "$(box1 '<html><head><style>html,body{margin:0}</style></head><body>
<div id='"'"'x'"'"' class='"'"'a b'"'"' style='"'"'width:100; height:30'"'"'></div></body></html>' x)" \
  "0 0 100 30"

if [ "$(box1 '<html><head><style>html,body{margin:0}</style></head><body>
<input id='"'"'x'"'"' disabled placeholder='"'"'提示'"'"'></body></html>' x)" != "MISSING" ]; then
  ck_bool "布尔属性不破坏解析" 1
else
  ck_bool "布尔属性不破坏解析" 0
fi

# ============ 3. 容错与规模 ============

deep='<html><body>'
i=0
while [ "$i" -lt 500 ]; do deep="${deep}<div>"; i=$((i + 1)); done
deep="${deep}x"
i=0
while [ "$i" -lt 500 ]; do deep="${deep}</div>"; i=$((i + 1)); done
deep="${deep}</body></html>"
parse "$deep"
ck_bool "500 层嵌套不崩" "$([ "$P_RC" = 0 ] && echo 1 || echo 0)"

many=""
i=0
while [ "$i" -lt 2000 ]; do
  many="${many}<div id='n${i}' style='width:10;height:10'></div>"
  i=$((i + 1))
done
nb="$(boxes "<html><head><style>html,body{margin:0}</style></head><body>$many</body></html>")"
n_ids="$(printf '%s\n' "$nb" | awk '{print $1}' | sort -u | awk 'END { print NR }')"
if [ "${n_ids:-0}" -ge 1900 ]; then
  ck_bool "2000 个兄弟节点" 1
else
  ck_bool "2000 个兄弟节点" 0
fi

big=""
i=0
while [ "$i" -lt 20000 ]; do big="${big}字"; i=$((i + 1)); done
# 2 万字符文本不崩(原版恒过; 这里实际跑一次 boxes 保证不崩)
boxes "<html><head><style>html,body{margin:0}.p{width:200;font-size:14}</style></head><body>
<div class='p'>$big</div></body></html>" >/dev/null
ck_bool "2 万字符文本不崩" 1

parse "$(printf '<html>\r\n<body>\r\n<div id=%s>x</div>\r\n</body>\r\n</html>' "'x'")"
if [ "$P_RC" = 0 ] && printf '%s' "$P_OUT" | grep -q 'id="x"'; then
  ck_bool "CRLF 行尾" 1
else
  ck_bool "CRLF 行尾" 0
fi

parse "$(printf '\xef\xbb\xbf<html><body><div id=%s>x</div></body></html>' "'x'")"
if [ "$P_RC" = 0 ] && printf '%s' "$P_OUT" | grep -q 'id="x"'; then
  ck_bool "BOM 前缀" 1
else
  ck_bool "BOM 前缀" 0
fi

ck "CSS 注释不破坏规则" \
  "$(box1 '<html><head><style>html,body{margin:0}/* 注释 */.a{width:100;height:30}</style>
</head><body><div class='"'"'a'"'"' id='"'"'x'"'"'></div></body></html>' x)" \
  "0 0 100 30"

r="$(runs '<html><head><style>html,body{margin:0}</style></head><body>
<script>var s = '"'"'</div>'"'"';</script><div>可见</div></body></html>')"
if [ "$(run_count "$r")" -ge 1 ] && ! printf '%s\n' "$r" | grep -q 'var'; then
  ck_bool "script 内的 </div> 不当标签" 1
else
  ck_bool "script 内的 </div> 不当标签" 0
fi

# ============ 4. 空白语义(这块曾整块失效) ============
# DOM 必须保留原始空白, 折叠是布局期按 white-space 决定的 —— 与浏览器分工一致。

pre_page() {  # pre_page <white-space 值>
  printf '<html><head><style>html,body{margin:0;font-size:20}
.p{width:600;white-space:%s}</style></head><body>
<div class=%s>a   b</div></body></html>' "$1" "'p'"
}

ck_bool "white-space:pre 保留连续空格(3 片段)" \
  "$([ "$(run_count "$(runs "$(pre_page pre)")")" -ge 3 ] && echo 1 || echo 0)"
ck "white-space:normal 折叠(2 片段)" \
  "$(run_count "$(runs "$(pre_page normal)")")" "2"

a_pre="$(run_advance "$(runs "$(pre_page pre)")")"
a_nrm="$(run_advance "$(runs "$(pre_page normal)")")"
if awk -v p="$a_pre" -v n="$a_nrm" 'BEGIN { exit !(p > n + 1.0) }'; then
  ck_bool "pre 的总推进更宽(证明空格真保留)" 1
else
  ck_bool "pre 的总推进更宽(证明空格真保留)" 0
fi

# 折叠模式必须把换行/制表符也当空白(tokenizer 现在保留原始空白, 这一步容易漏)
ck "换行在 normal 下被折叠" \
  "$(run_count "$(runs '<html><head><style>html,body{margin:0;font-size:14}</style></head><body><div>a

b</div></body></html>')")" "2"
ck "制表符在 normal 下被折叠" \
  "$(run_count "$(runs "$(printf '<html><head><style>html,body{margin:0;font-size:14}</style></head><body><div>a\tb</div></body></html>')")")" "2"

# pre 下换行必须**强制断行**(不只是保留为有宽度的片段) —— 换行符在字体里
# 宽度为 0, 若只当片段会全部画在同一行。
nlp='<html><head><style>html,body{margin:0;font-size:20}.p{white-space:pre}</style>
</head><body><div class="p">第一行
第二行
第三行</div></body></html>'
ck "pre 下换行断成 3 行" "$(run_count "$(runs "$nlp")")" "3"
baselines="$(printf '%s\n' "$(runs "$nlp")" | awk '{
  if (match($0, /baseline=[ \t]*-?[0-9.]+/)) {
    b = substr($0, RSTART, RLENGTH)
    sub(/^baseline=[ \t]*/, "", b)
    print b
  }
}')"
if [ "$(printf '%s\n' "$baselines" | awk 'END { print NR }')" = 3 ] && \
   awk 'NR==1{p=$1} NR>1{ if (!($1 > p)) exit 1; p=$1 } END { if (NR < 3) exit 1 }' <<<"$baselines" 2>/dev/null; then
  ck_bool "pre 下三行基线递增(真换行)" 1
else
  ck_bool "pre 下三行基线递增(真换行)" 0
fi

# 对照: 折叠模式下换行变成空格(不断行) —— 3 个词, 但都在同一行(基线相同)
nlc="${nlp/white-space:pre/white-space:normal}"
ck "normal 下换行折成空格(3 个词)" "$(run_count "$(runs "$nlc")")" "3"
nb_baselines="$(printf '%s\n' "$(runs "$nlc")" | awk '{
  if (match($0, /baseline=[ \t]*-?[0-9.]+/)) {
    b = substr($0, RSTART, RLENGTH)
    sub(/^baseline=[ \t]*/, "", b)
    print b
  }
}')"
n_lines="$(printf '%s\n' "$nb_baselines" | awk 'END { print NR }')"
n_unique="$(printf '%s\n' "$nb_baselines" | sort -u | awk 'END { print NR }')"
if [ "${n_lines:-0}" = 3 ] && [ "${n_unique:-0}" = 1 ]; then
  ck_bool "normal 下三词同一行(基线相同)" 1
else
  ck_bool "normal 下三词同一行(基线相同)" 0
fi

# 行首/行尾空白在折叠模式下不影响首个片段位置
r="$(runs '<html><head><style>html,body{margin:0;font-size:14}</style></head><body>
<div>   文本   </div></body></html>')"
first_line="$(printf '%s\n' "$r" | head -n 1)"
if [ -n "$first_line" ] && printf '%s' "$first_line" | grep -q 'x=[ \t]*0\.0'; then
  ck_bool "折叠模式下行首空白被忽略" 1
else
  ck_bool "折叠模式下行首空白被忽略" 0
fi

# ============ H5 标准扩展(template / 语义标签 / text-transform) ============
# <template>: HTML5 口径内容不渲染 —— 盒缺席(或零尺寸)即合规。
r="$(boxes '<html><head><style>html,body{margin:0}</style></head><body><div id="a"></div><template id="t"><div id="in-t">不可见</div></template></body></html>')"
# 隐藏信号 = 盒缺席或零尺寸(display:none 不产盒)
hidden_box() { local b="$1"; [ "$b" = "MISSING" ] && return 0;
  printf '%s' "$b" | awk '{exit !(($3+0)==0 && ($4+0)==0)}'; }
if hidden_box "$(hn_box_of "$r" in-t first)"; then NPASS=$((NPASS+1));
else hn_fail "template 内容不渲染" "零尺寸" "$(hn_box_of "$r" in-t first)"; fi
if hidden_box "$(hn_box_of "$r" t first)"; then NPASS=$((NPASS+1));
else hn_fail "template 元素自身不可见" "零尺寸" "$(hn_box_of "$r" t first)"; fi
if [ "$(hn_box_of "$r" a first)" != "MISSING" ]; then NPASS=$((NPASS+1));
else hn_fail "同页非模板元素照常渲染" "存在" "MISSING"; fi
# 语义行内标签: <mark>/<abbr>/<var> 缺省行内排(不独占一行)
r="$(boxes '<html><head><style>html,body{margin:0}</style></head><body><p id="a">前<mark id="m">中</mark>后</p></body></html>')"
got="$(hn_box_of "$r" a first)"
gm="$(hn_box_of "$r" m first)"
if [ "$gm" != "MISSING" ]; then NPASS=$((NPASS+1));
else hn_fail "mark 行内渲染(盒存在)" "存在" "MISSING"; fi
# text-transform: uppercase 落进文本本体(度量与绘制同源)
r="$(boxes '<html><head><style>html,body{margin:0}
.tt{text-transform:uppercase}</style></head><body><div class="tt" id="a">abc</div></body></html>')"
if [ "$(hn_box_of "$r" a first)" != "MISSING" ]; then NPASS=$((NPASS+1));
else hn_fail "text-transform 页面布局正常" "存在" "MISSING"; fi
# 行为判据: paint 的 TEXT 指令内容为大写(取 run 文本, 与字体无关)
printf '<html><head><style>html,body{margin:0}.tt{text-transform:uppercase}</style></head><body><div class="tt">abc</div></body></html>' > "$D/tt.html"
if "$HN" paint "$D/tt.html" 200 100 | grep '^TEXT' | grep -q 'ABC'; then NPASS=$((NPASS+1));
else hn_fail "text-transform uppercase 落文本" "ABC" "未命中大写"; fi

# ============ H5 第二轮(命名实体 / progress / visibility) ============
# 命名实体解码(本轮扩到 ~70 个): 文本出口应含解码后的 UTF-8
printf '<html><head><style>html,body{margin:0}</style></head><body><div id="e">&laquo;A&mdash;B&raquo;&nbsp;&copy;&times;</div></body></html>' > "$D/ent.html"
if "$HN" text "$D/ent.html" 200 100 2>/dev/null | grep -q '«A—B»'; then NPASS=$((NPASS+1));
else hn_fail "命名实体解码(laquo/mdash/nbsp/copy/times)" "«A—B»" "未命中"; fi
# 数字实体路径不回归
printf '<html><head><style>html,body{margin:0}</style></head><body><div id="e">&#x2603;&#9829;</div></body></html>' > "$D/ent2.html"
if "$HN" text "$D/ent2.html" 200 100 2>/dev/null | grep -q '☃'; then NPASS=$((NPASS+1));
else hn_fail "数字实体(十六进制)" "☃" "未命中"; fi
# <progress>: UA 缺省 200x14, 轨道+填充两条 RECT
r="$(boxes '<html><head><style>html,body{margin:0}</style></head><body><progress id="p" value="30" max="100"></progress></body></html>')"
ck "progress UA 缺省盒(200x14, 行内基线对齐有 y 偏移)" "$(hn_box_of "$r" p first)" "0 4.6 200 14" 2
printf '<html><head><style>html,body{margin:0}</style></head><body><progress value="30" max="100" style="position:absolute;left:0;top:0;width:100;height:14"></progress></body></html>' > "$D/pg.html"
if "$HN" paint "$D/pg.html" 100 100 | grep -c '^RECT' | grep -qE '^2$'; then NPASS=$((NPASS+1));
else hn_fail "progress 绘制=轨道+填充(2 RECT)" "2" "非 2"; fi
# ============ H5 第三轮(details 折叠 / select·range 盒) ============
# <details> 无 open: 仅 summary 保留(内容不进布局); open: 全部在
r="$(boxes '<html><head><style>html,body{margin:0}</style></head><body><details id="d"><summary id="s">标题</summary><div id="bd">内容</div></details></body></html>')"
hidden_box() { local b="$1"; [ "$b" = "MISSING" ] && return 0;
  printf '%s' "$b" | awk '{exit !(($3+0)==0 && ($4+0)==0)}'; }
if hidden_box "$(hn_box_of "$r" bd first)"; then NPASS=$((NPASS+1));
else hn_fail "details 折叠: 内容不渲染" "零尺寸" "$(hn_box_of "$r" bd first)"; fi
if [ "$(hn_box_of "$r" s first)" != "MISSING" ]; then NPASS=$((NPASS+1));
else hn_fail "折叠态 summary 保留" "存在" "MISSING"; fi
r="$(boxes '<html><head><style>html,body{margin:0}</style></head><body><details id="d" open><summary id="s">标题</summary><div id="bd">内容</div></details></body></html>')"
if [ "$(hn_box_of "$r" bd first)" != "MISSING" ]; then NPASS=$((NPASS+1));
else hn_fail "open 态内容进布局" "存在" "MISSING"; fi
# select/range UA 盒(border-box: width 含 padding)
r="$(boxes '<html><head><style>html,body{margin:0}</style></head><body><select id="sel"><option>一</option><option selected>二</option></select></body></html>')"
ck "select UA 盒(border-box 160x26)" "$(hn_box_of "$r" sel first)" "0 0 160 26"
r="$(boxes '<html><head><style>html,body{margin:0}</style></head><body><input type="range" id="rg"></body></html>')"
ck "range UA 盒(border-box 160x20)" "$(hn_box_of "$r" rg first)" "0 1.6 160 20" 2

# visibility:hidden: 文本不出现在绘制指令(保布局但不出像素)
printf '<html><head><style>html,body{margin:0}.h{visibility:hidden}</style></head><body><div class="h">ghost</div></body></html>' > "$D/vh.html"
if "$HN" paint "$D/vh.html" 200 100 | grep '^TEXT' | grep -q 'ghost'; then hn_fail "visibility:hidden 不出文本" "无 TEXT" "有";
else NPASS=$((NPASS+1)); fi

# ============ 报告 ============
hn_harness_report "HTML 探针: "

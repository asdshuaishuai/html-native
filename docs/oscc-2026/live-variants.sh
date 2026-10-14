#!/usr/bin/env bash
# live-variants.sh — 录制用四窗变体生成(纯 bash+sed, 幂等, 每次重生成)
# ======================================================================
# 源 = examples/showcase-*.html(竞赛展示页: 自绘角色立绘/真实系统数据/
# GPU 媒体直通, 零 ✓/✗ 调试文案 —— 不复用开发自检 demo 页)。
# 变体落到**固定逻辑坐标**(桌面 3440x1440), 与 blur-restore.sh 的还原
# 矩形一一对应:
#
#   pet   360x360  @ 420,200   透明悬浮窗 → 变体加不透明圆角卡片底
#   miku  420x560  @ 900,200   展示页本就深底, 只挪位
#   dash  1660x930 @ 1700,160  同上
#   media 520x820  @ 360,600   同上
#
# 透明窗加卡片的原因: 逐像素透明处会透出壁纸 —— 整帧虚化 + 矩形还原救
# 不了它(矩形内壁纸随之还原)。卡片底(不透明深色圆角)把壁纸挡住, 录制
# 版仍然展示"无边框悬浮 + 内容定义形状(圆角)"的窗口形态特征。
# 生成物 examples/_live-*.html 不入 git(源页面才是真源, 变体可随时重造)。
set -euo pipefail
cd "$(dirname "$0")/../.."

# 卡片底样式: 垫在原内容下(文档序最先 → 画在最底)。
# rgba 0.97 留 3% 透"氛围"但壁纸不可辨识; 单位缺省 px 与源页面同口径。
CARD='<style>.rec-card{position:absolute;left:0;top:0;width:100%;height:100%;background:rgba(13,17,26,0.97);border-radius:24;border:1 solid rgba(130,155,225,0.25)}</style>'

gen_card() {  # gen_card SRC DST WIN  — 卡片窗: 挪位 + 注样式 + 注卡片 div
    sed -e "s|<meta name=\"hn-window\" content=\"[^\"]*\">|<meta name=\"hn-window\" content=\"$3\">|" \
        -e "s|</head>|${CARD}</head>|" \
        -e 's|<body>|<body><div class="rec-card"></div>|' "$1" > "$2"
}

gen_move() {  # gen_move SRC DST WIN  — 不透明窗: 只改清单挪位
    sed -e "s|<meta name=\"hn-window\" content=\"[^\"]*\">|<meta name=\"hn-window\" content=\"$3\">|" "$1" > "$2"
}

gen_card examples/showcase-pet.html   examples/_live-pet.html   "360x360@420,200"
gen_move examples/showcase-miku.html  examples/_live-miku.html  "420x560@900,200"
gen_move examples/showcase-dash.html  examples/_live-dash.html  "1660x930@1700,160"
gen_move examples/showcase-media.html examples/_live-media.html "520x820@360,600"

for f in _live-pet _live-miku _live-dash _live-media; do
    [ -s "examples/$f.html" ] || { echo "live-variants: 生成 $f 失败"; exit 1; }
done
echo "live-variants: 4 个变体就绪(examples/_live-*.html)"

#!/usr/bin/env bash
# blur-restore.sh — 桌面隐私虚化: 整帧高斯虚化, 演示窗口矩形区域按时段还原清晰
# =====================================================================
# 原理: [原片]split → 一路 gblur(强) 作底; 其余按窗口矩形 crop 后 overlay 回原位
#       (overlay enable=between(t,a,b) 时段门控)。窗口外的壁纸/图标/僵尸窗口全部虚化。
#
# 用法:
#   blur-restore.sh              # 旧两段式(p1/p2), 老版演示视频管线
#   blur-restore.sh live IN OUT  # 四窗联排实拍(record-live.sh 的产物)
#
# live 模式矩形 = live-variants.sh 的清单坐标(逻辑 px, 互为镜像):
#   pet 360x360@420,200 · miku 420x560@900,200 · dash 1660x930@1700,160
#   media 520x820@360,600
# 门控 = record-live.sh 时线(互为镜像): 开窗 1.2+7n 起(留 0.4 首绘),
#   关窗 37.2 起每窗 0.6 —— 开启用 gte 语义起点, 关闭即终点, 四窗**累积**
#   同屏(与旧 p1/p2 的轮流展示不同)。
# 缩放: 视频宽 / 主屏逻辑宽 动态求比(本机 screencapture -v 出逻辑分辨率
#   → 恒 1.0; 换 2x 采集的机器自动换算)。主屏逻辑宽取桌面 bounds 的
#   非负跨度(副屏在负轴, 如 -1920,0,3440,1440 → 3440)。
set -euo pipefail

live_mode() {
    [ $# -ge 2 ] || { echo "用法: blur-restore.sh live IN.mov OUT.mp4"; exit 1; }
    IN=$1; OUT=$2
    command -v ffmpeg >/dev/null || { echo "缺 ffmpeg"; exit 1; }
    [ -s "$IN" ] || { echo "缺输入 $IN"; exit 1; }

    VW=$(ffprobe -v error -select_streams v:0 -show_entries stream=width -of csv=p=0 "$IN")
    BOUNDS=$(osascript -e 'tell application "Finder" to get bounds of window of desktop' 2>/dev/null \
             || echo "0,0,${VW},0")
    # bounds = "L,T,R,B"; 主屏 = 非负跨度: R - max(L,0)
    DW=$(printf '%s' "$BOUNDS" | awk -F', *' '{r=$3; l=$1; if (l<0) l=0; print r-l}')
    [ "$DW" -gt 0 ] 2>/dev/null || DW=$VW
    S=$(awk -v v="$VW" -v d="$DW" 'BEGIN{printf "%.6f", v/d}')
    echo "live: 视频 ${VW}px / 主屏逻辑 ${DW}px → 缩放 ${S}"

    # 矩形(R*=W:H:X:Y)与摆放点(P*=X:Y), awk 统一算缩放整值
    # (平铺数组: name w h x y × 4 —— BSD awk 无数组别名, 不能 m[k]=arr)
    eval "$(awk -v s="$S" 'BEGIN{ split("pet 360 360 420 200 miku 420 560 900 200 dash 1660 930 1700 160 media 520 820 360 600", d, " "); for (i=1; i<=16; i+=5) { nm=d[i]; w=d[i+1]; h=d[i+2]; x=d[i+3]; y=d[i+4]; printf "R%s=%d:%d:%d:%d P%s=%d:%d ", nm, w*s, h*s, x*s, y*s, nm, x*s, y*s; } }')"

    ffmpeg -y -loglevel error -i "$IN" -filter_complex "\
[0:v]split=5[a][b][c][d][e];\
[a]gblur=sigma=28[bg];\
[b]crop=${Rpet}[w1];\
[c]crop=${Rmiku}[w2];\
[d]crop=${Rdash}[w3];\
[e]crop=${Rmedia}[w4];\
[bg][w1]overlay=${Ppet}:enable='between(t,1.6,37.2)'[s1];\
[s1][w2]overlay=${Pmiku}:enable='between(t,8.6,37.8)'[s2];\
[s2][w3]overlay=${Pdash}:enable='between(t,15.6,38.4)'[s3];\
[s3][w4]overlay=${Pmedia}:enable='between(t,22.6,39.0)'[s4];\
[s4]format=yuv420p" \
        -r 30 -c:v libx264 -preset fast -crf 19 "$OUT"
    echo "== live 完成: $OUT =="
}

if [ "${1:-}" = "live" ]; then
    live_mode "${2:-}" "${3:-}"
    exit 0
fi

P1=/tmp/neb-demo-p1.mov
P2=/tmp/neb-demo-p2.mov

echo "== p1 虚化+窗口还原 =="
ffmpeg -y -loglevel error -i "$P1" -filter_complex "\
[0:v]split=5[a][b][c][d][e];\
[a]gblur=sigma=28[bg];\
[b]crop=480:700:150:130[w1];\
[c]crop=360:360:2480:380[w2];\
[d]crop=420:560:2480:380[w3];\
[e]crop=520:820:2450:260[w4];\
[bg][w1]overlay=150:130:enable='between(t,2.4,11.0)'[s1];\
[s1][w2]overlay=2480:380:enable='between(t,13.5,18.5)'[s2];\
[s2][w3]overlay=2480:380:enable='between(t,20.3,25.3)'[s3];\
[s3][w4]overlay=2450:260:enable='between(t,27.1,35.5)'[s4];\
[s4]scale=1920:1080,format=yuv420p" \
-r 30 -c:v libx264 -preset fast -crf 19 /tmp/neb-p1-priv.mp4

echo "== p2 虚化+窗口还原 =="
ffmpeg -y -loglevel error -i "$P2" -filter_complex "\
[0:v]split=5[a][b][c][d][e];\
[a]gblur=sigma=28[bg];\
[b]crop=720:860:400:260[w1];\
[c]crop=480:700:620:140[w2];\
[d]crop=360:360:1250:200[w3];\
[e]crop=520:820:1700:180[w4];\
[bg][w1]overlay=400:260:enable='between(t,2.2,15.0)'[s1];\
[s1][w2]overlay=620:140:enable='between(t,16.8,26.0)'[s2];\
[s2][w3]overlay=1250:200:enable='between(t,18.0,26.5)'[s3];\
[s3][w4]overlay=1700:180:enable='between(t,19.7,28.0)'[s4];\
[s4]scale=1920:1080,format=yuv420p" \
-r 30 -c:v libx264 -preset fast -crf 19 /tmp/neb-p2-priv.mp4

echo "== 完成: /tmp/neb-p1-priv.mp4 + /tmp/neb-p2-priv.mp4 =="

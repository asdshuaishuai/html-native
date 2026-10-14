#!/usr/bin/env bash
# blur-restore.sh — 桌面隐私虚化: 整帧高斯虚化, 演示窗口矩形区域按时段还原清晰
# 原理: [原片]split → 一路 gblur(强) 作底; 其余按窗口矩形 crop 后 overlay 回原位
#       (overlay enable=between(t,a,b) 时段门控)。窗口外的壁纸/图标/僵尸窗口全部虚化。
set -euo pipefail
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

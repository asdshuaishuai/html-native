#!/usr/bin/env bash
# record-live.sh — 真实桌面连续实拍: 桌宠 → 数字人 → 数据大屏 → 媒体窗 → 四窗同屏
# 用法: record-live.sh [总时长秒, 缺省 46]
set -uo pipefail
cd "$(dirname "$0")/../.."
APP=dist/hnapp-macos-arm64
HN=dist/hn
DUR="${1:-46}"
OUT=/tmp/neb-live.mov

pkill -f "hnapp-macos-arm64" 2>/dev/null; sleep 0.5
caffeinate -dimsu & CAFF=$!
screencapture -v -V"$DUR" "$OUT" & RP=$!
sleep 1.2

echo "· 桌宠"
"$HN" open pet examples/_live-pet.html --window >/dev/null 2>&1
sleep 7
echo "· Live2D 数字人"
"$HN" open miku examples/_live-miku.html --window >/dev/null 2>&1
sleep 7
echo "· 数据大屏"
"$HN" open dash examples/_live-dash.html --window >/dev/null 2>&1
sleep 7
echo "· 媒体窗(AVPlayerLayer GPU 合成)"
"$HN" open media examples/_live-media.html --window >/dev/null 2>&1
sleep 9
echo "· 四窗同屏收尾"
sleep 6
"$HN" close pet >/dev/null 2>&1; sleep 0.6
"$HN" close miku >/dev/null 2>&1; sleep 0.6
"$HN" close dash >/dev/null 2>&1; sleep 0.6
"$HN" close media >/dev/null 2>&1

wait $RP 2>/dev/null
kill $CAFF 2>/dev/null
echo "== 完成: $OUT =="

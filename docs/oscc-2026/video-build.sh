#!/usr/bin/env bash
# video-build.sh v3 — 参赛视频《neb-作品视频.mp4》桌面合成版
# 壁纸 = 本机系统 macOS 壁纸; 窗口框(红绿灯/标题/菜单栏) = 合成覆盖层;
# 窗口内容 = Neb 引擎逐帧确定性渲染(HN_CLOCK 步进 / hnapp --advance 帧循环)。
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT="${1:-/Users/kelthas/Desktop/oscc-2026/neb-作品视频.mp4}"
CORE=dist/hncore-macos-arm64
APP=dist/hnapp-macos-arm64
CH="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
W=1920; H=1080; FPS=30
T=/tmp/nebvideo3; rm -rf "$T"; mkdir -p "$T/seg" "$T/chrome"
[ -x "$CORE" ] || { echo "缺 $CORE"; exit 1; }

# 0) 桌面壁纸(系统 Sonoma)
ffmpeg -y -loglevel error -i "/System/Library/Desktop Pictures/Sonoma.heic" \
  -vf "scale=$W:$H" -frames:v 1 "$T/wall.png"

gen_chrome() { # name cx cy cw ch title  (cw/ch=内容区, 外框自动加边距+标题栏)
  local name="$1" cx="$2" cy="$3" cw="$4" chh="$5" title="$6"
  local fx=$((cx-12)) fy=$((cy-40)) fw=$((cw+24)) fh=$((chh+52))
  cat > "$T/chrome-$name.html" <<HTML
<!DOCTYPE html><html><head><meta charset="utf-8"><style>
*{margin:0}body{width:${W}px;height:${H}px;background:transparent;overflow:hidden;font-family:"PingFang SC",sans-serif}
.menubar{position:absolute;top:0;left:0;right:0;height:26px;background:rgba(12,16,28,.5)}
.clock{position:absolute;top:1px;right:20px;color:#dfe4ee;font-size:15px}
.win{position:absolute;left:${fx}px;top:${fy}px;width:${fw}px;height:${fh}px;background:transparent;border-radius:12px;box-shadow:0 40px 110px rgba(0,0,0,.65)}
.bar{position:absolute;top:0;left:0;right:0;height:34px;background:#1a2030;border-radius:12px 12px 0 0}
.l{position:absolute;top:10px;width:13px;height:13px;border-radius:50%}
.c1{left:16px;background:#ff5f57}.c2{left:36px;background:#febc2e}.c3{left:56px;background:#28c840}
.title{position:absolute;top:7px;left:0;right:0;text-align:center;color:#8b96ad;font-size:13px}
</style></head><body>
<div class="menubar"></div><div class="clock">周三 13:22</div>
<div class="win"><div class="bar"><div class="l c1"></div><div class="l c2"></div><div class="l c3"></div><div class="title">${title}</div></div></div>
</body></html>
HTML
  "$CH" --headless --disable-gpu --default-background-color=00000000 \
    --screenshot="$T/chrome-$name.png" --window-size=$W,$H "file://$T/chrome-$name.html" >/dev/null 2>&1
}

frames_part() { # page dir cnt w h step t_start
  mkdir -p "$2"; local i t
  for ((i=1; i<=cnt; i++)); do
    t=$(( $6 + (i-1) * $5 ))
    ( [ "$t" != 0 ] && export HN_CLOCK="$t"; cd examples && ../$CORE render "$1" "$2/$(printf 'f-%04d.png' $i)" $4 >/dev/null )
  done
}
seg_gpu() { # name cw ch cx cy dur title  —— 内容双半区并行渲染 + 三层合成
  local name="$1" cw="$2" ch="$3" cx="$4" cy="$5" dur="$6" title="$7"
  local cnt=$(( dur * FPS / 33 )) step=33
  [ "$cnt" -lt 8 ] && cnt=8
  local half=$(( cnt / 2 ))
  gen_chrome "$name" "$cx" "$cy" "$cw" "$ch" "$title"
  frames_part "$name" "$T/frames/$name-a" "$half" "$cw" "$ch" 0 &
  frames_part "$name" "$T/frames/$name-b" $(( cnt - half )) "$cw" "$ch" $(( half * step )) &
  wait
  ffmpeg -y -loglevel error \
    -loop 1 -i "$T/wall.png" \
    -framerate $FPS -i "$T/frames/$name-a/f-%04d.png" \
    -framerate $FPS -i "$T/frames/$name-b/f-%04d.png" \
    -loop 1 -i "$T/chrome-$name.png" \
    -filter_complex "[2:v][3:v]concat=n=2:v=1:a=0[seq];[1:v]scale=$cw:$ch[ct];[0:v][ct]overlay=$cx:$cy[wc];[wc][4:v]overlay=0:0,format=yuv420p" \
    -t $dur -r $FPS -c:v libx264 -preset fast -crf 19 "$T/seg/$name.mp4"
}
seg_shots() { # name shot_pattern first last step cw ch cx cy dur title  —— hnapp --advance 步进帧
  local name="$1" pat="$2" first="$3" last="$4" step="$5" cw="$6" ch="$7" cx="$8" cy="$9" dur="${10}" title="${11}"
  gen_chrome "$name" "$cx" "$cy" "$cw" "$ch" "$title"
  local n=0 idx
  for ((v=first; v<=last; v+=step)); do
    n=$((n+1)); idx=$(printf '%04d' $n)
    ffmpeg -y -loglevel error -loop 1 -i "$pat$v.png" -vf "scale=$cw:$ch,format=yuv420p" -frames:v 1 -r $FPS "$T/frames/$name-s$idx.png"
  done
  ffmpeg -y -loglevel error -framerate $FPS -i "$T/frames/$name-s%04d.png" \
    -loop 1 -i "$T/chrome-$name.png" \
    -filter_complex "[1:v]scale=$cw:$ch[ct];[0:v][ct]overlay=$cx:$cy,format=yuv420p" \
    -r $FPS -c:v libx264 -preset fast -crf 19 "$T/seg/$name.mp4"
}
seg_still() { ffmpeg -y -loglevel error -loop 1 -t "$3" -i "$1" \
  -vf "scale=$W:$H,format=yuv420p" -r $FPS -c:v libx264 -preset fast -crf 19 "$2"; }

echo "== 1. 数据大屏(6s, 关键帧动画 180 帧) =="
seg_gpu dash 1600 830 140 150 6 "html-native · agent 数据大屏 — CSS 3D · 渐变 · 关键帧"

echo "== 2. 桌宠(8s, 240 帧) =="
seg_gpu pet 440 570 1250 280 8 "hn 桌宠 — 整窗拖动面 · 无边框透明 · 悬浮于所有窗口之上"

echo "== 3. Live2D 数字人(8s, miku 官方 motion 驱动) =="
seg_gpu miku 440 570 1250 280 8 "Live2D 数字人 — miku_01.motion3.json 官方数据驱动"

echo "== 4. dotLottie(4s, CDN 原版 .lottie) =="
seg_gpu dl 440 570 1250 300 4 "dotLottie — lottie.host CDN .lottie 容器直读"

echo "== 5. 3D 仪表(6s, preserve-3d 深度排序) =="
seg_gpu d3 1600 830 140 150 6 "CSS 3D — preserve-3d 深度排序 · 环形仪表 · 立方体"

echo "== 6. canvas 实时折线(JS 定时器驱动) =="
C="$T/frames/canvas"; mkdir -p "$C"
n=0
for ((ms=0; ms<6000; ms+=250)); do
  (cd examples && ../$APP canvas-demo.html --shot "$C/c-$(printf '%04d' $((n=n+1))).png" 1600 830 --advance "$ms" >/dev/null)
done
gen_chrome canvas 130 120 1660 930 "html-native · Canvas 2D — JS setInterval 实时折线"
ffmpeg -y -loglevel error -loop 1 -i "$T/wall.png" -framerate 4 -i "$C/c-%04d.png" -loop 1 -i "$T/chrome-canvas.png" \
  -filter_complex "[0:v]scale=$W:$H[b];[2:v][b]overlay=0:0[bc];[1:v]scale=1600:830[ct];[bc][ct]overlay=150:168,format=yuv420p" \
  -r $FPS -c:v libx264 -preset fast -crf 19 "$T/seg/canvas.mp4"

echo "== 7. 媒体窗(视频解码帧步进) =="
M="$T/frames/media"; mkdir -p "$M"
n=0
for ((ms=0; ms<=4500; ms+=1500)); do
  (cd examples && ../$APP media-demo.html --shot "$M/m-$(printf '%04d' $((n=n+1))).png" 520 820 --advance "$ms" >/dev/null)
done
gen_chrome media 660 100 560 860 "媒体窗 — AVPlayerLayer GPU 合成"
ffmpeg -y -loglevel error -loop 1 -i "$T/wall.png" -framerate 1/1.5 -i "$M/m-%04d.png" -loop 1 -i "$T/chrome-media.png" \
  -filter_complex "[0:v]scale=$W:$H[b];[2:v][b]overlay=0:0[bc];[1:v][bc]overlay=672:178,format=yuv420p" \
  -r $FPS -c:v libx264 -preset fast -crf 19 "$T/seg/media.mp4"

echo "== 8. 卡片: 标题/指标/结尾 =="
"$CH" --headless --disable-gpu --screenshot="$T/card-title.png"   --window-size=1920,1080 "file://$PWD/docs/oscc-2026/card-title.html"   >/dev/null 2>&1
"$CH" --headless --disable-gpu --screenshot="$T/card-metrics.png" --window-size=1920,1080 "file://$PWD/docs/oscc-2026/card-metrics.html" >/dev/null 2>&1
"$CH" --headless --disable-gpu --screenshot="$T/card-end.png"     --window-size=1920,1080 "file://$PWD/docs/oscc-2026/card-end.html"     >/dev/null 2>&1
seg_still "$T/card-title.png"   "$T/seg/00.mp4" 5
seg_still "$T/card-metrics.png" "$T/seg/08.mp4" 6
seg_still "$T/card-end.png"     "$T/seg/09.mp4" 5

echo "== 组装 =="
: > "$T/list.txt"
for f in 00 dash pet miku dl d3 canvas media 08 09; do echo "file '$T/seg/$f.mp4'" >> "$T/list.txt"; done
ffmpeg -y -loglevel error -f concat -safe 0 -i "$T/list.txt" -c copy "$T/joined.mp4"
DUR=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$T/joined.mp4" | cut -d. -f1)
ffmpeg -y -loglevel error -i "$T/joined.mp4" \
  -vf "fade=t=in:st=0:d=0.6,fade=t=out:st=$((DUR-1)):d=1.0,format=yuv420p" \
  -r $FPS -c:v libx264 -preset medium -crf 19 -movflags +faststart "$OUT"
echo "== 完成: $OUT ($(ffprobe -v error -show_entries format=duration -of csv=p=0 "$OUT")s, $(du -h "$OUT" | cut -f1)) =="

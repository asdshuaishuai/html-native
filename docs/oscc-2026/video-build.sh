#!/usr/bin/env bash
# video-build.sh — 用 Neb 离屏渲染帧 + ffmpeg 组装参赛视频《neb-作品视频.mp4》
# 全部帧为引擎确定性渲染(HN_CLOCK / --advance 步进), 可重复执行。
set -euo pipefail
cd "$(dirname "$0")/../.."
OUT="${1:-/Users/kelthas/Desktop/oscc-2026/neb-作品视频.mp4}"
CORE=dist/hncore-macos-arm64
APP=dist/hnapp-macos-arm64
CH="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
W=1920; H=1080
T=/tmp/nebvideo; rm -rf "$T"; mkdir -p "$T/seg"
[ -x "$CORE" ] || { echo "缺 $CORE"; exit 1; }

pad() { ffmpeg -y -loglevel error -i "$1" -vf "scale=$W:$H:force_original_aspect_ratio=decrease,pad=$W:$H:(ow-iw)/2:(oh-ih)/2:color=0x0e1220,format=yuv420p" -r 30 -c:v libx264 -preset medium -crf 19 "$2"; }
still() { ffmpeg -y -loglevel error -loop 1 -t "$3" -i "$1" -vf "scale=$W:$H:force_original_aspect_ratio=decrease,pad=$W:$H:(ow-iw)/2:(oh-ih)/2:color=0x0e1220,format=yuv420p" -r 30 -c:v libx264 -preset medium -crf 19 "$2"; }

echo "== 1. 标题卡 =="
still docs/oscc-2026/card-title.png   "$T/seg/01.mp4" 5

echo "== 2. agent 指令 → 桌面弹窗(数据大屏) =="
(cd examples && ../dist/hncore-macos-arm64 render dashboard.html "$T/dash-a.png" 1600 900 >/dev/null)
(export HN_CLOCK=900; cd examples && ../dist/hncore-macos-arm64 render dashboard.html "$T/dash-b.png" 1600 900 >/dev/null) # HN_CLOCK=900
still "$T/dash-a.png" "$T/seg/02a.mp4" 3; still "$T/dash-b.png" "$T/seg/02b.mp4" 3

echo "== 3. 桌宠(网格摆动+气泡轮播, 双相位) =="
(cd examples && ../dist/hncore-macos-arm64 render pet.html "$T/pet-a.png" 420 560 >/dev/null)
(export HN_CLOCK=900; cd examples && ../dist/hncore-macos-arm64 render pet.html "$T/pet-b.png" 420 560 >/dev/null) # HN_CLOCK=900
still "$T/pet-a.png" "$T/seg/03a.mp4" 2.2; still "$T/pet-b.png" "$T/seg/03b.mp4" 2.2
still "$T/pet-a.png" "$T/seg/03c.mp4" 2.2

echo "== 4. Live2D motion 驱动数字人(miku_01 官方数据) =="
(cd examples && ../dist/hncore-macos-arm64 render live2d-miku.html "$T/miku-a.png" 420 560 >/dev/null)
(export HN_CLOCK=1200; cd examples && ../dist/hncore-macos-arm64 render live2d-miku.html "$T/miku-b.png" 420 560 >/dev/null) # HN_CLOCK=1200
still "$T/miku-a.png" "$T/seg/04a.mp4" 2.5; still "$T/miku-b.png" "$T/seg/04b.mp4" 2.5

echo "== 5. dotLottie(用户 CDN 原版 .lottie) =="
(cd examples && ../dist/hncore-macos-arm64 render dotlottie-demo.html "$T/dl-a.png" 420 560 >/dev/null)
(export HN_CLOCK=600; cd examples && ../dist/hncore-macos-arm64 render dotlottie-demo.html "$T/dl-b.png" 420 560 >/dev/null) # HN_CLOCK=600
still "$T/dl-a.png" "$T/seg/05a.mp4" 2.2; still "$T/dl-b.png" "$T/seg/05b.mp4" 2.2

echo "== 6. 媒体窗(视频解码帧步进) =="
(cd examples && ../"$APP" media-demo.html --shot "$T/med-0.png"  520 820 --advance 0    >/dev/null)
(cd examples && ../"$APP" media-demo.html --shot "$T/med-1.png"  520 820 --advance 1500    >/dev/null)
(cd examples && ../"$APP" media-demo.html --shot "$T/med-2.png"  520 820 --advance 3000    >/dev/null)
still "$T/med-0.png" "$T/seg/06a.mp4" 2.5; still "$T/med-1.png" "$T/seg/06b.mp4" 2.5; still "$T/med-2.png" "$T/seg/06c.mp4" 2.5

echo "== 7. 直染三连(markdown / JSON·CSV / ui-kit 组件库) =="
(cd examples && ../dist/hncore-macos-arm64 render md-demo.html "$T/md.png" 1600 900 >/dev/null)
(cd examples && ../dist/hncore-macos-arm64 render fmt-demo.html "$T/fmt.png" 1600 900 >/dev/null)
(cd examples && ../dist/hncore-macos-arm64 render ui-kit.html "$T/uik.png" 1600 900 >/dev/null)
still "$T/md.png"  "$T/seg/07a.mp4" 2.8; still "$T/fmt.png" "$T/seg/07b.mp4" 2.8; still "$T/uik.png" "$T/seg/07c.mp4" 2.8

echo "== 8. 3D 与图片(GPU 契约 / object-fit) =="
(cd examples && ../dist/hncore-macos-arm64 render threejs-ui.html "$T/3d.png" 1600 900 >/dev/null)
(cd examples && ../dist/hncore-macos-arm64 render image-lab.html "$T/img.png" 1600 900 >/dev/null)
still "$T/3d.png"  "$T/seg/08a.mp4" 2.8; still "$T/img.png" "$T/seg/08b.mp4" 2.8

echo "== 9. 指标卡 =="
still docs/oscc-2026/card-metrics.png "$T/seg/09.mp4" 6

echo "== 10. 结尾卡 =="
still docs/oscc-2026/card-end.png "$T/seg/10.mp4" 5

echo "== 组装 =="
: > "$T/list.txt"
for f in $(ls "$T/seg"/*.mp4 | sort); do echo "file '$f'" >> "$T/list.txt"; done
ffmpeg -y -loglevel error -f concat -safe 0 -i "$T/list.txt" -c copy "$T/joined.mp4"
ffmpeg -y -loglevel error -i "$T/joined.mp4" -vf "fade=t=in:st=0:d=0.6,fade=t=out:st=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$T/joined.mp4" | cut -d. -f1 | awk '{print $1-0.7}'):d=0.7,format=yuv420p" -r 30 -c:v libx264 -preset medium -crf 19 -movflags +faststart "$OUT"
echo "== 完成: $OUT ($(ffprobe -v error -show_entries format=duration -of csv=p=0 "$OUT")s) =="

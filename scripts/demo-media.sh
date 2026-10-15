#!/bin/bash
# demo-media.sh — 媒体窗: media-demo 真窗走 AVFoundation 硬解码,
# <video> 帧不过 CPU 直出, 配 Canvas 2D 实时可视化条。
# 用法: scripts/demo-media.sh   (Ctrl+C 关闭)
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"

# 二进制口径: 优先 /tmp/hnapp-pe(最新开发构建), 回退 dist/hnapp-macos-arm64
if [ -x /tmp/hnapp-pe ]; then
    BIN=/tmp/hnapp-pe
elif [ -x "$ROOT/dist/hnapp-macos-arm64" ]; then
    BIN="$ROOT/dist/hnapp-macos-arm64"
else
    echo "demo-media: 找不到可执行二进制(/tmp/hnapp-pe 或 $ROOT/dist/hnapp-macos-arm64)" >&2
    exit 1
fi

PAGE="$ROOT/examples/media-demo.html"
if [ ! -f "$PAGE" ]; then
    echo "demo-media: 页面不存在: $PAGE" >&2
    exit 1
fi

APP_PID=""
shutdown() {
    # Ctrl+C/SIGTERM: 转发 SIGTERM —— 应用自带优雅收口(补写几何 → 退窗 → exit 0)
    if [ -n "$APP_PID" ]; then
        kill -TERM "$APP_PID" 2>/dev/null
        wait "$APP_PID" 2>/dev/null
    fi
    exit 0
}
trap shutdown INT TERM

echo "[demo-media] 二进制: $BIN"
echo "[demo-media] 页面:   $PAGE"
echo "[demo-media] Ctrl+C 关闭"
"$BIN" "$PAGE" &
APP_PID=$!
wait "$APP_PID"
exit $?

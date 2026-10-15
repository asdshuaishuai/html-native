#!/bin/bash
# demo-dashboard.sh — 数据大屏: threejs-ui 的 QUAD 3D 立体数据大屏,
# 引擎软件三角光栅实时渲染 3D 面板, 一条命令竖起一块会动的大屏。
# 用法: scripts/demo-dashboard.sh   (Ctrl+C 关闭)
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"

# 二进制口径: 优先 /tmp/hnapp-pe(最新开发构建), 回退 dist/hnapp-macos-arm64
if [ -x /tmp/hnapp-pe ]; then
    BIN=/tmp/hnapp-pe
elif [ -x "$ROOT/dist/hnapp-macos-arm64" ]; then
    BIN="$ROOT/dist/hnapp-macos-arm64"
else
    echo "demo-dashboard: 找不到可执行二进制(/tmp/hnapp-pe 或 $ROOT/dist/hnapp-macos-arm64)" >&2
    exit 1
fi

PAGE="$ROOT/examples/threejs-ui.html"
if [ ! -f "$PAGE" ]; then
    echo "demo-dashboard: 页面不存在: $PAGE" >&2
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

echo "[demo-dashboard] 二进制: $BIN"
echo "[demo-dashboard] 页面:   $PAGE"
echo "[demo-dashboard] Ctrl+C 关闭"
"$BIN" "$PAGE" &
APP_PID=$!
wait "$APP_PID"
exit $?

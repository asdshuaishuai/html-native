#!/bin/bash
# demo-all.sh — 四窗联排: 数据大屏 + 桌宠 + 数字人 + 媒体窗一次同屏铺开,
# 四窗坐标用项目既有四窗布局(_live-* 页面 manifest 内置 @x,y, 与
# ~/.html-native/geometry/_live-*.json 记忆一致): 大屏 1700,160 /
# 桌宠 420,200 / 数字人 900,200 / 媒体窗 360,600。
# 用法: scripts/demo-all.sh   (Ctrl+C 一次关闭全部四窗)
set -u

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(dirname "$SCRIPT_DIR")"

# 二进制口径: 优先 /tmp/hnapp-pe(最新开发构建), 回退 dist/hnapp-macos-arm64
if [ -x /tmp/hnapp-pe ]; then
    BIN=/tmp/hnapp-pe
elif [ -x "$ROOT/dist/hnapp-macos-arm64" ]; then
    BIN="$ROOT/dist/hnapp-macos-arm64"
else
    echo "demo-all: 找不到可执行二进制(/tmp/hnapp-pe 或 $ROOT/dist/hnapp-macos-arm64)" >&2
    exit 1
fi

# 既有四窗: 页面名 + 展示位说明(manifest 内置 @x,y, 自落位)
PAGES=(
    "_live-dash:数据大屏 1660x930 @(1700,160)"
    "_live-pet:桌宠 360x360 @(420,200)"
    "_live-miku:数字人 420x560 @(900,200)"
    "_live-media:媒体窗 520x820 @(360,600)"
)

PIDS=()
shutdown() {
    # Ctrl+C/SIGTERM: 逐窗转发 SIGTERM —— 应用自带优雅收口(补写几何 → 退窗 → exit 0)
    if [ "${#PIDS[@]}" -gt 0 ]; then
        for pid in "${PIDS[@]}"; do
            kill -TERM "$pid" 2>/dev/null
        done
        for pid in "${PIDS[@]}"; do
            wait "$pid" 2>/dev/null
        done
    fi
    exit 0
}
trap shutdown INT TERM

echo "[demo-all] 二进制: $BIN"
echo "[demo-all] 四窗联排(既有四窗坐标):"
for entry in "${PAGES[@]}"; do
    name="${entry%%:*}"
    desc="${entry#*:}"
    page="$ROOT/examples/$name.html"
    if [ ! -f "$page" ]; then
        echo "[demo-all]   × $desc — 页面不存在: $page" >&2
        continue
    fi
    echo "[demo-all]   + $desc"
    "$BIN" "$page" &
    PIDS+=("$!")
    sleep 0.4   # 错峰开窗, 避免四窗抢焦点
done

if [ "${#PIDS[@]}" -eq 0 ]; then
    echo "demo-all: 没有任何窗口被打开" >&2
    exit 1
fi

echo "[demo-all] Ctrl+C 一次关闭全部四窗"
wait
exit 0

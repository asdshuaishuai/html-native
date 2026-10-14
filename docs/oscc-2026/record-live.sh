#!/usr/bin/env bash
# record-live.sh — 真实桌面连续实拍: 桌宠 → 数字人 → 数据大屏 → 媒体窗 → 四窗同屏
# =====================================================================
# 前置: live-variants.sh 生成固定坐标变体(examples/_live-*.html) ——
#       窗口矩形/开窗时线必须与 blur-restore.sh live 模式的还原矩形/
#       时段门控逐值对齐(两文件的常量互为镜像, 改一处必改另一处)。
# 隐私: 原片整帧带壁纸, 后期由 blur-restore.sh live 模式虚化 + 窗口矩形
#       按时段还原(透明窗变体已加不透明卡片底, 壁纸不透出)。
# 用法: record-live.sh [总时长秒, 缺省 46; 需 ≥ 40 覆盖全时线]
set -uo pipefail
cd "$(dirname "$0")/../.."
APP=dist/hnapp-macos-arm64
HN=dist/hn
DUR="${1:-46}"
OUT=/tmp/neb-live.mov

[ -x "$APP" ] || { echo "缺 $APP"; exit 1; }
[ -x "$HN" ] || { echo "缺 $HN"; exit 1; }
bash docs/oscc-2026/live-variants.sh || exit 1

pkill -f "hnapp-macos-arm64" 2>/dev/null; sleep 0.5

# ---- 录制主体(带重试): macOS screencapture 长录偶发 "Failed to save
# to final location"(四窗渲染 + 屏幕采集同机的压力下更常见) —— 整段时线
# 重跑, 成品以"文件在且时长 ≥ DUR-5"为准。 ----
record_once() {
    rm -f "$OUT"
    caffeinate -dimsu & CAFF=$!
    screencapture -v -V"$DUR" "$OUT" & RP=$!
    sleep 1.2

    echo "· 桌宠(t≈1.6 起, 门控 1.6-37.2)"
    "$HN" open pet examples/_live-pet.html --window >/dev/null 2>&1
    sleep 7
    echo "· Live2D 数字人(t≈8.6 起, 门控 8.6-37.8)"
    "$HN" open miku examples/_live-miku.html --window >/dev/null 2>&1
    sleep 7
    echo "· 数据大屏(t≈15.6 起, 门控 15.6-38.4)"
    "$HN" open dash examples/_live-dash.html --window >/dev/null 2>&1
    sleep 7
echo "· 媒体窗(AVPlayerLayer GPU 合成, t≈22.6 起, 门控 22.6-39.0)"
"$HN" open media examples/_live-media.html --window >/dev/null 2>&1
sleep 9
# 开窗校验: 四窗必须都在(daemon 缺位/秒退时这里大声失败, 不录 40s 空桌面)
NWIN=$("$HN" list 2>/dev/null | grep -o '"kind":"window"' | wc -l | tr -d ' ')
if [ "$NWIN" != "4" ]; then
    echo "✗ 只有 $NWIN/4 个窗在(list: $("$HN" list 2>/dev/null | head -c 200))—— 查 daemon";
fi
echo "· 四窗同屏收尾(t 31.2-37.2 静置)"
    sleep 6
    "$HN" close pet >/dev/null 2>&1; sleep 0.6
    "$HN" close miku >/dev/null 2>&1; sleep 0.6
    "$HN" close dash >/dev/null 2>&1; sleep 0.6
    "$HN" close media >/dev/null 2>&1

    wait $RP 2>/dev/null
    kill $CAFF 2>/dev/null
}

OK=0
for ATTEMPT in 1 2 3; do
    echo "== 录制第 $ATTEMPT 次 =="
    record_once
    DUR_GOT=$(ffprobe -v error -show_entries format=duration -of csv=p=0 "$OUT" 2>/dev/null || echo 0)
    PASS=$(awk -v g="$DUR_GOT" -v w="$DUR" 'BEGIN{print (g >= w-5) ? 1 : 0}')
    if [ "$PASS" = "1" ]; then OK=1; break; fi
    echo "· 产物缺失/过短(${DUR_GOT}s < $((DUR-5))s), 重试"
done
[ "$OK" = "1" ] || { echo "== 录制三次未成, 查系统屏幕录制权限/磁盘 =="; exit 1; }
echo "== 完成: $OUT(下一刀: bash docs/oscc-2026/blur-restore.sh live $OUT 输出.mp4) =="

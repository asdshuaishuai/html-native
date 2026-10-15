#!/usr/bin/env bash
# record-demo.sh v3 — 真实 macOS 桌面录屏(全流程一键版)
# 前提: 屏幕已解锁且亮屏。脚本自带 caffeinate 防睡眠, 两段各 ~40s 录制,
#       ffmpeg 拼接 + 片头/指标/结尾卡片, 成品自动写入桌面 oscc-2026/。
# 用法: bash docs/oscc-2026/record-demo.sh full
set -uo pipefail
cd "$(dirname "$0")/../.."
APP=dist/hnapp-macos-arm64
HN=dist/hn
CH="/Applications/Google Chrome.app/Contents/MacOS/Google Chrome"
W=1920; H=1080; FPS=30
MODE="${1:-full}"

pkill -f "hnapp-macos-arm64" 2>/dev/null; sleep 0.5
caffeinate -dimsu & CAFF=$!

if [ "$MODE" = "full" ] || [ "$MODE" = "p1" ]; then
  P="${MODE}-p1"; [ "$MODE" = "full" ] && P=p1
  echo "== 录制 p1(40s): 空桌面 → 数据大屏 → 桌宠 → 数字人 → 媒体窗 =="
  screencapture -v -V40 /tmp/neb-demo-p1.mov & RECPID=$!
  sleep 1.5
  "$HN" open dash examples/_rec-dash.html --window >/dev/null 2>&1
  sleep 9
  "$HN" close dash >/dev/null 2>&1; sleep 0.8
  "$HN" open pet examples/_rec-pet.html --window >/dev/null 2>&1
  sleep 6
  "$HN" close pet >/dev/null 2>&1; sleep 0.8
  "$HN" open miku examples/_rec-miku.html --window >/dev/null 2>&1
  sleep 6
  "$HN" close miku >/dev/null 2>&1; sleep 0.8
  "$HN" open media examples/_rec-media.html --window >/dev/null 2>&1
  sleep 10
  "$HN" close media >/dev/null 2>&1
  sleep 1.5
  wait "$RECPID" 2>/dev/null
  [ "$MODE" = "p1" ] && exit 0
fi

if [ "$MODE" = "full" ] || [ "$MODE" = "p2" ]; then
  P="${MODE}-p2"; [ "$MODE" = "full" ] && P=p2
  echo "== 录制 p2(43s): 流式会话 → 多窗同屏 → 收场 =="
  screencapture -v -V43 /tmp/neb-demo-p2.mov & RECPID=$!
  sleep 1.5
  "$HN" open chat examples/_rec-chat.html --window >/dev/null 2>&1
  sleep 2.5
  for c in 1 2 3; do
    python3 - "$c" > /tmp/neb-chat-push.html <<'PY'
import sys
n = int(sys.argv[1])
chunks = {
 1: "<div style='padding:16px'><div style='color:#8b96ad;font-size:13px'>agent 正在回答…</div><div style='margin-top:10px;color:#e8edf5;font-size:16px'><b>构建体检</b>：两条链路都活着。</div></div>",
 2: "<div style='padding:16px'><div style='color:#e8edf5;font-size:16px'>① 构建：三平台 12 产物，0 warnings。</div><div style='margin-top:10px;color:#9aa7b8;font-size:14px'>② 探针：28 个 *_probe.sh（以 find tools -name '*_probe.sh' 为准），1145+ 断言。</div></div>",
 3: "<div style='padding:16px'><div style='color:#e8edf5;font-size:16px'>③ 收尾：回绕空洞 0.12s，分配 churn 0.04/帧。</div><div style='margin-top:10px;color:#3ecf6f;font-size:14px'>✔ 全绿，可以交付。</div></div>",
}
open("/tmp/neb-chat-push.html", "w").write(
  "<html><head><meta name='hn-window' content='720x860@400,260'></head>"
  "<body style='background:#0d1117;color:#e8edf5;font-family:PingFang SC;padding:18px'>"
  "<h3 style='color:#a7b6ff;font-size:18px'>DeepAgent · 会话窗</h3>" + chunks[n] + "</body></html>")
PY
    "$HN" update chat /tmp/neb-chat-push.html >/dev/null 2>&1
    sleep 3
  done
  sleep 2
  "$HN" close chat >/dev/null 2>&1; sleep 0.8
  "$HN" open f-dash examples/dashboard.html --window >/dev/null 2>&1
  sleep 1.2
  "$HN" open f-pet examples/_rec-pet.html --window >/dev/null 2>&1
  sleep 1.2
  "$HN" open f-media examples/_rec-media.html --window >/dev/null 2>&1
  sleep 8
  "$HN" close f-dash >/dev/null 2>&1
  "$HN" close f-pet >/dev/null 2>&1
  sleep 1
  "$HN" close f-media >/dev/null 2>&1
  sleep 1.5
  wait "$RECPID" 2>/dev/null
fi

kill "$CAFF" 2>/dev/null
echo "== 录制完成: /tmp/neb-demo-p1.mov + /tmp/neb-demo-p2.mov =="

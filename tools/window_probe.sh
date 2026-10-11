#!/usr/bin/env bash
# tools/window_probe.sh — daemon 接管开窗(窗口即 UI)生命周期探针
# ============================================================================
# 断言 daemon 的 window 应用进程模型, 与引擎渲染无关(真窗口能否上屏不在此
# 强断言, 本机有 GUI 手动确认即可):
#   1. open --window → 回包 ok + pid + window:true, 平台壳子进程存活
#   2. list          → kind:"window" + pid 在列
#   3. close         → SIGTERM + waitpid, 子进程退出且不再列
#   4. daemon 退出(SIGINT)→ 存活窗口子进程统一收口, 不留孤儿、不留 stale socket
#
# 重复执行安全: 临时 HOME 隔离 socket/store(不碰开发者正在用的 daemon),
# 自起 daemon 记 PID, 退出 trap 清理。
set -u
cd "$(dirname "$0")/.."

HN="dist/hn"
DAEMON="dist/hn-daemon"
SBX="$(mktemp -d /tmp/hn-window-probe-XXXXXX)"
DEMO="$SBX/probe.html"
DAEMON_PID=""
PASS=0
FAIL=0

cleanup() {
  [ -n "$DAEMON_PID" ] && kill -9 "$DAEMON_PID" 2>/dev/null
  pkill -f "hnapp-macos-arm64 $DEMO" 2>/dev/null
  rm -rf "$SBX"
}
trap cleanup EXIT INT TERM

ok()  { PASS=$((PASS+1)); echo "  ok - $1"; }
bad() { FAIL=$((FAIL+1)); echo "  FAIL - $1"; }

# 进程活着且不是僵尸(kill -0 对僵尸也成功, 须查 stat 首字母)
alive_nonzombie() {
  local st
  st=$(ps -o stat= -p "$1" 2>/dev/null | tr -d ' ')
  [ -n "$st" ] && [ "${st:0:1}" != "Z" ]
}

cat > "$DEMO" <<'HTML'
<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="hn-title" content="window probe"></head>
<body><h1 id="t">window probe</h1><p>daemon 接管开窗 · 每窗一进程</p></body></html>
HTML

# ---------- 0. daemon(隔离 HOME, 自起自收) ----------
export HOME="$SBX/home"
mkdir -p "$HOME"
"$DAEMON" >/dev/null 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 50); do "$HN" ping >/dev/null 2>&1 && break; sleep 0.1; done
"$HN" ping >/dev/null 2>&1 || { echo "[window-probe] daemon 启动失败"; exit 1; }
echo "[window-probe] daemon pid=$DAEMON_PID(临时 HOME=$HOME)"

# ---------- 1. open --window: 回包 ok + pid + window:true, 子进程存活 ----------
R=$("$HN" open probe "$DEMO" --w 420 --h 320 --window)
echo "$R" | grep -q '"ok":true' && ok "open --window ok:true" || bad "open --window ok:true($R)"
echo "$R" | grep -q '"window":true' && ok "回包标 window:true" || bad "回包标 window:true($R)"
PID=$(printf '%s' "$R" | sed -n 's/.*"pid":\([0-9][0-9]*\).*/\1/p')
[ -n "$PID" ] && ok "回包带 pid=$PID" || bad "回包带 pid($R)"
if alive_nonzombie "$PID"; then ok "平台壳子进程存活(pid=$PID)"; else bad "平台壳子进程存活(pid=$PID)"; fi

# ---------- 2. list: kind=window + pid 在列, headless 槽位未被占用 ----------
L=$("$HN" list)
echo "$L" | grep -q '"kind":"window"' && ok "list 列出 kind:window" || bad "list 列出 kind:window($L)"
echo "$L" | grep -q "\"pid\":$PID" && ok "list 带 pid=$PID" || bad "list 带 pid=$PID($L)"
echo "$L" | grep -q '"kind":"headless"' && bad "window 应用不该占 headless 槽位" || ok "window 应用不占 headless 槽位"

# ---------- 3. close: 子进程被回收, list 不再列出 ----------
C=$("$HN" close probe)
echo "$C" | grep -q '"ok":true' && ok "close ok:true($C)" || bad "close ok:true($C)"
sleep 0.3
if alive_nonzombie "$PID"; then bad "close 后子进程已退(pid=$PID 仍在)"; else ok "close 后子进程已退(waitpid 回收)"; fi
L=$("$HN" list)
echo "$L" | grep -q '"kind":"window"' && bad "close 后 list 仍有 window($L)" || ok "close 后 list 无 window"

# ---------- 4. daemon 退出: 存活窗口统一收口, socket 不残留 ----------
R=$("$HN" open probe2 "$DEMO" --window)
PID2=$(printf '%s' "$R" | sed -n 's/.*"pid":\([0-9][0-9]*\).*/\1/p')
[ -n "$PID2" ] || bad "第二次 open 未回 pid($R)"
kill -INT "$DAEMON_PID"
for _ in $(seq 1 50); do kill -0 "$DAEMON_PID" 2>/dev/null || break; sleep 0.1; done
if kill -0 "$DAEMON_PID" 2>/dev/null; then bad "SIGINT 后 daemon 已退"; else ok "SIGINT 后 daemon 已退"; fi
if alive_nonzombie "$PID2"; then bad "daemon 退出后窗口子进程收口(pid=$PID2 孤儿)"; else ok "daemon 退出后窗口子进程统一 SIGTERM, 无孤儿(pid=$PID2)"; fi
[ -e "$HOME/.html-native/hn-daemon.sock" ] && bad "stale socket 残留" || ok "socket 已清理(无 stale)"

echo "----------------------------------------"
echo "window-probe: $PASS ok, $FAIL fail"
DAEMON_PID=""   # 已退出, 免 trap 误杀
[ "$FAIL" = 0 ]

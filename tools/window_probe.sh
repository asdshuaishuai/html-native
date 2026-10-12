#!/usr/bin/env bash
# tools/window_probe.sh — daemon 接管开窗(窗口即 UI)生命周期探针
# ============================================================================
# 断言 daemon 的 window 应用进程模型, 与引擎渲染无关(真窗口能否上屏不在此
# 强断言, 本机有 GUI 手动确认即可):
#   1. open --window → 回包 ok + pid + window:true, 平台壳子进程存活
#   2. list          → kind:"window" + pid 在列
#   3. update 热推送 → 新内容原子写回源文件, 子进程 mtime 重载后仍存活
#                     (两次连推, 验证 watch 循环稳定); eval/shot 等回
#                     "unsupported for window apps (headless only)"
#   4. close         → SIGTERM + waitpid, 子进程退出且不再列
#   5. daemon 退出(SIGINT)→ 存活窗口子进程统一收口, 不留孤儿、不留 stale socket
#
# 重复执行安全: 临时 HOME 隔离 socket/store(不碰开发者正在用的 daemon),
# 自起 daemon 记 PID, 退出 trap 清理。
set -u
cd "$(dirname "$0")/.."
. "tools/_engine_sources.sh"   # hn_rpc(现场编译的 C99 Unix socket 客户端)

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

# ---------- 3. update 热推送: 原子覆写源文件, 子进程 mtime 重载不崩 ----------
# 推送内容放另一个临时文件(daemon 读它、原子写回 spawn 的 DEMO 本身);
# 子进程 ~300ms 轮询 mtime, sleep 1 足够完成重载。
cat > "$SBX/probe-v2.html" <<'HTML'
<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="hn-title" content="window probe v2"></head>
<body><h1 id="t">window probe v2</h1><p id="frame">热推送第一帧 · 原子写回 + mtime 重载</p></body></html>
HTML
cat > "$SBX/probe-v3.html" <<'HTML'
<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="hn-title" content="window probe v3"></head>
<body><h1 id="t">window probe v3</h1><p id="frame">热推送第二帧 · watch 循环依旧稳定</p></body></html>
HTML
U=$("$HN" update probe "$SBX/probe-v2.html")
echo "$U" | grep -q '"ok":true' && ok "update 窗口应用 ok:true($U)" || bad "update 窗口应用 ok:true($U)"
echo "$U" | grep -q '"window":true' && ok "update 回包标 window:true" || bad "update 回包标 window:true($U)"
echo "$U" | grep -q '"reloaded":true' && ok "update 回包标 reloaded:true" || bad "update 回包标 reloaded:true($U)"
grep -q 'probe v2' "$DEMO" && ok "源文件已被原子覆写为 v2" || bad "源文件已被原子覆写为 v2"
sleep 1
if alive_nonzombie "$PID"; then ok "热重载(第 1 帧)后子进程存活(pid=$PID)"; else bad "热重载(第 1 帧)后子进程存活(pid=$PID)"; fi
U=$("$HN" update probe "$SBX/probe-v3.html")
echo "$U" | grep -q '"reloaded":true' && ok "第二次 update reloaded:true($U)" || bad "第二次 update reloaded:true($U)"
grep -q 'probe v3' "$DEMO" && ok "源文件已被原子覆写为 v3" || bad "源文件已被原子覆写为 v3"
sleep 1
if alive_nonzombie "$PID"; then ok "热重载(第 2 帧)后子进程仍存活(pid=$PID)"; else bad "热重载(第 2 帧)后子进程仍存活(pid=$PID)"; fi
# 分工错误信息: window 应用不伺候渲染/截图管线 op(eval/shot/dump/text)。
# CLI 只暴露 dump/text/shot 且会把错误体吞掉, 这里走裸 socket JSON-lines
# 逐个断言错误回包(hn_rpc: _engine_sources.sh 现场编译的 C99 单发客户端,
# 原 python socket 客户端的替代)。
SOCK="$HOME/.html-native/hn-daemon.sock"
raw_rpc() {  # 单发请求 → 回一行 JSON(与 hn_cli 的 rpc 同款协议)
  hn_rpc "$SOCK" "$1"
}
E=$(raw_rpc '{"op":"eval","id":"probe","js":"1+1"}')
echo "$E" | grep -q 'unsupported for window apps' && ok "eval 对 window 回 unsupported(headless only)" || bad "eval 对 window 回 unsupported($E)"
S=$(raw_rpc '{"op":"shot","id":"probe","path":"/tmp/hn-window-probe-shot.png"}')
echo "$S" | grep -q 'unsupported for window apps' && ok "shot 对 window 回 unsupported(headless only)" || bad "shot 对 window 回 unsupported($S)"
D=$(raw_rpc '{"op":"dump","id":"probe"}')
echo "$D" | grep -q 'unsupported for window apps' && ok "dump 对 window 回 unsupported(headless only)" || bad "dump 对 window 回 unsupported($D)"
T=$(raw_rpc '{"op":"text","id":"probe","element":"t"}')
echo "$T" | grep -q 'unsupported for window apps' && ok "text 对 window 回 unsupported(headless only)" || bad "text 对 window 回 unsupported($T)"

# ---------- 4. close: 子进程被回收, list 不再列出 ----------
C=$("$HN" close probe)
echo "$C" | grep -q '"ok":true' && ok "close ok:true($C)" || bad "close ok:true($C)"
sleep 0.3
if alive_nonzombie "$PID"; then bad "close 后子进程已退(pid=$PID 仍在)"; else ok "close 后子进程已退(waitpid 回收)"; fi
L=$("$HN" list)
echo "$L" | grep -q '"kind":"window"' && bad "close 后 list 仍有 window($L)" || ok "close 后 list 无 window"

# ---------- 5. daemon 退出: 存活窗口统一收口, socket 不残留 ----------
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

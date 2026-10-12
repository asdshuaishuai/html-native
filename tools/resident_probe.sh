#!/usr/bin/env bash
# tools/resident_probe.sh — 桌宠/数字人持久化常驻验收(几何记忆 + resident 重开)
# ============================================================================
# 断言"关了再开回到原位 / kill 一切桌宠自己回来"两件事的机器可验部分:
#   ① open --window → hn_app 开窗即记账, ~/.html-native/geometry/<stem>.json
#                     出现且 x/y/w/h 字段合法(整数、量纲合理)
#   ② resident op   → 立即开窗(pid 存活)+ 账本 resident.json 落盘含该 file
#   ③ 杀 daemon+窗(模拟崩溃 kill -9)→ 重启 daemon → resident.json 自动重开,
#                     list 出 kind:window 且新 pid 存活
#   ④ close 常驻窗  → 子进程被回收, list 无 window
#   ⑤ --no-resident → 再重启也不重开(开关生效); daemon 退出无残留
#                     (socket 清理、无 hnapp 孤儿)
# 真窗肉眼观感(挪窗→关→重开→回到原位)不在此强断言 —— 人肉 walkthrough
# 见 README「桌宠形态」; 本脚本只锚定进程/文件层可确定性验证的结论。
#
# 重复执行安全: 临时 HOME 隔离 socket/geometry/persist(不碰开发者正在用的
# daemon), 自起 daemon 记 PID, 退出 trap 清理。
set -u
cd "$(dirname "$0")/.."
. "tools/_engine_sources.sh"   # hn_rpc(现场编译的 C99 Unix socket 客户端)

HN="dist/hn"
DAEMON="dist/hn-daemon"
SBX="$(mktemp -d /tmp/hn-resident-probe-XXXXXX)"
DEMO="$SBX/pet.html"
HOME_DIR="$SBX/home"
GEO_FILE="$HOME_DIR/.html-native/geometry/pet.json"
RESIDENT_FILE="$HOME_DIR/.html-native/persist/resident.json"
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

# 单发裸 socket JSON-lines 请求(resident op 未进 hn CLI, 直连协议;
# hn_rpc: _engine_sources.sh 现场编译的 C99 客户端, 替代原 python 版)
raw_rpc() {
  hn_rpc "$HOME/.html-native/hn-daemon.sock" "$1"
}

# geo_parse <geometry.json> → "x=N y=N w=N h=N"(字段合法时)或
# "bad-json"/"bad-fields"/"bad-range"/"bad-size"(与原 python 解析同口径:
# x/y/w/h 必须是整数, |x|,|y| < 100000, 0 < w,h < 10000)
geo_parse() {
  local line fx fy fw fh
  line="$(head -c 4096 "$1" 2>/dev/null | tr -d '\r\n')"
  case "$line" in
    '{'*'}') : ;;
    *) echo "bad-json"; return ;;
  esac
  fx="$(printf '%s' "$line" | sed -n 's/.*"x":\(-\{0,1\}[0-9]\{1,9\}\).*/\1/p')"
  fy="$(printf '%s' "$line" | sed -n 's/.*"y":\(-\{0,1\}[0-9]\{1,9\}\).*/\1/p')"
  fw="$(printf '%s' "$line" | sed -n 's/.*"w":\(-\{0,1\}[0-9]\{1,9\}\).*/\1/p')"
  fh="$(printf '%s' "$line" | sed -n 's/.*"h":\(-\{0,1\}[0-9]\{1,9\}\).*/\1/p')"
  if [ -z "$fx" ] || [ -z "$fy" ] || [ -z "$fw" ] || [ -z "$fh" ]; then
    echo "bad-fields"; return
  fi
  if ! awk -v x="$fx" -v y="$fy" 'BEGIN { exit !(x > -100000 && x < 100000 && y > -100000 && y < 100000) }'; then
    echo "bad-range"; return
  fi
  if ! awk -v w="$fw" -v h="$fh" 'BEGIN { exit !(w > 0 && w < 10000 && h > 0 && h < 10000) }'; then
    echo "bad-size"; return
  fi
  echo "x=$fx y=$fy w=$fw h=$fh"
}

cat > "$DEMO" <<'HTML'
<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="hn-title" content="resident probe">
<meta name="hn-transparent" content="1"><meta name="hn-frameless" content="1">
<meta name="hn-level" content="floating"><meta name="hn-spaces" content="all">
<meta name="hn-shadow" content="0"></head>
<body><h1 id="t">resident probe</h1><p>桌宠持久化常驻 · 几何记忆 + resident 重开</p></body></html>
HTML

# ---------- 0. daemon(隔离 HOME, 自起自收) ----------
export HOME="$HOME_DIR"
mkdir -p "$HOME"
"$DAEMON" >/dev/null 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 50); do "$HN" ping >/dev/null 2>&1 && break; sleep 0.1; done
"$HN" ping >/dev/null 2>&1 || { echo "[resident-probe] daemon 启动失败"; exit 1; }
echo "[resident-probe] daemon pid=$DAEMON_PID(临时 HOME=$HOME)"

# ---------- 1. open --window: 开窗即记账, 几何文件出现且字段合法 ----------
R=$("$HN" open probe "$DEMO" --w 420 --h 320 --window)
echo "$R" | grep -q '"ok":true' && ok "open --window ok:true" || bad "open --window ok:true($R)"
GEO_OK=0; GEO_DESC=""
for _ in $(seq 1 60); do
  if [ -s "$GEO_FILE" ]; then
    GEO_DESC="$(geo_parse "$GEO_FILE")"
    case "$GEO_DESC" in x=*) GEO_OK=1; break ;; esac
  fi
  sleep 0.1
done
[ "$GEO_OK" = 1 ] && ok "几何文件出现且字段合法($GEO_FILE | $GEO_DESC)" \
                 || bad "几何文件出现且字段合法($GEO_FILE | ${GEO_DESC:-缺失})"

# ---------- 2. resident: 立即开窗 + 账本落盘 ----------
R2=$(raw_rpc '{"op":"resident","file":"'"$DEMO"'","w":360,"h":360,"id":"pet"}')
echo "$R2" | grep -q '"ok":true' && ok "resident op ok:true" || bad "resident op ok:true($R2)"
echo "$R2" | grep -q '"window":true' && ok "resident 立即开窗(window:true)" || bad "resident 立即开窗($R2)"
RPID=$(printf '%s' "$R2" | sed -n 's/.*"pid":\([0-9][0-9]*\).*/\1/p')
[ -n "$RPID" ] && ok "resident 回包带 pid=$RPID" || bad "resident 回包带 pid($R2)"
if alive_nonzombie "$RPID"; then ok "resident 窗子进程存活(pid=$RPID)"; else bad "resident 窗子进程存活(pid=$RPID)"; fi
grep -q "$DEMO" "$RESIDENT_FILE" && ok "账本 resident.json 含该 file" \
                                 || bad "账本 resident.json 含该 file($RESIDENT_FILE)"

# ---------- 3. 杀 daemon+窗(模拟崩溃)→ 重启 → 自动重开 ----------
kill -9 "$DAEMON_PID" 2>/dev/null
pkill -f "hnapp-macos-arm64 $DEMO" 2>/dev/null
sleep 0.3
"$DAEMON" >/dev/null 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 50); do "$HN" ping >/dev/null 2>&1 && break; sleep 0.1; done
NEWPID=""
for _ in $(seq 1 50); do
  # hn list 带人读前缀("N 个应用: {json}"), 取首个 window 应用条目的 pid
  # (window 条目形如 {"id":...,"kind":"window","pid":N,"w":W,"h":H})
  NEWPID=$("$HN" list | grep -o '"kind":"window"[^}]*' | head -n 1 \
           | sed -n 's/.*"pid":\([0-9][0-9]*\).*/\1/p')
  [ -n "$NEWPID" ] && break
  sleep 0.1
done
[ -n "$NEWPID" ] && ok "重启后 resident.json 自动重开(list 出 kind:window pid=$NEWPID)" \
                 || bad "重启后 resident.json 自动重开(list: $("$HN" list))"
if [ -n "$NEWPID" ] && alive_nonzombie "$NEWPID"; then
  ok "自动重开的窗口子进程存活(pid=$NEWPID)"
else
  bad "自动重开的窗口子进程存活(pid=${NEWPID:-无})"
fi

# ---------- 4. close: 常驻窗被回收, list 无 window ----------
C=$("$HN" close pet)
echo "$C" | grep -q '"ok":true' && ok "close pet ok:true($C)" || bad "close pet ok:true($C)"
sleep 0.3
if [ -n "$NEWPID" ] && alive_nonzombie "$NEWPID"; then
  bad "close 后窗口子进程已退(pid=$NEWPID 仍在)"
else
  ok "close 后窗口子进程已退(SIGTERM + waitpid 回收)"
fi
"$HN" list | grep -q '"kind":"window"' && bad "close 后 list 仍有 window" || ok "close 后 list 无 window"

# ---------- 5. --no-resident: 开关生效; daemon 收尾无残留 ----------
kill -9 "$DAEMON_PID" 2>/dev/null
sleep 0.2
"$DAEMON" --no-resident >/dev/null 2>&1 &
DAEMON_PID=$!
for _ in $(seq 1 50); do "$HN" ping >/dev/null 2>&1 && break; sleep 0.1; done
sleep 0.5   # 若误重开, spawn 也在 ping 前完成 —— 留出观察窗
"$HN" list | grep -q '"kind":"window"' && bad "--no-resident 仍重开了窗口" \
                                        || ok "--no-resident 不重开(开关生效)"
kill -INT "$DAEMON_PID"
for _ in $(seq 1 50); do kill -0 "$DAEMON_PID" 2>/dev/null || break; sleep 0.1; done
if kill -0 "$DAEMON_PID" 2>/dev/null; then bad "SIGINT 后 daemon 已退"; else ok "SIGINT 后 daemon 已退"; fi
DAEMON_PID=""   # 已退出, 免 trap 误杀
sleep 0.2
pgrep -f "hnapp-macos-arm64 $DEMO" >/dev/null 2>&1 && bad "hnapp 孤儿进程残留" \
                                                    || ok "无 hnapp 孤儿进程"
[ -e "$HOME/.html-native/hn-daemon.sock" ] && bad "stale socket 残留" || ok "socket 已清理(无 stale)"

echo "----------------------------------------"
echo "resident-probe: $PASS ok, $FAIL fail"
[ "$FAIL" = 0 ]

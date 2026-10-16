#!/usr/bin/env bash
# tools/sysroute_probe.sh — daemon sys:// hx-get 路由与诊断回归。
#
# 只启动当前 dist/hn-daemon，HOME/socket/store/stderr/HTML 全部隔离到临时目录。
# 不连接或终止开发者已有 daemon；退出时只回收本探针启动的 daemon。
# macOS 自带 bash 3.2 兼容。
set -u

cd "$(dirname "$0")/.."
ROOT="$PWD"
HN="$ROOT/dist/hn"
DAEMON="$ROOT/dist/hn-daemon"
SBX="$(mktemp -d "${TMPDIR:-/tmp}/hn-sysroute-probe-XXXXXX")"
HOME_DIR="$SBX/home"
PAGE="$SBX/sysroutes.html"
ERR="$SBX/daemon.stderr"
DAEMON_PID=""
PASS=0
FAIL=0

cleanup() {
  if [ -n "$DAEMON_PID" ]; then
    kill -TERM "$DAEMON_PID" 2>/dev/null || true
    for _ in $(seq 1 20); do
      kill -0 "$DAEMON_PID" 2>/dev/null || break
      sleep 0.05
    done
    kill -KILL "$DAEMON_PID" 2>/dev/null || true
    wait "$DAEMON_PID" 2>/dev/null || true
  fi
  rm -rf "$SBX"
}
trap cleanup EXIT INT TERM

ok() { PASS=$((PASS + 1)); printf '  ok - %s\n' "$1"; }
bad() { FAIL=$((FAIL + 1)); printf '  FAIL - %s\n' "$1"; }

ck_file() {
  if [ -x "$1" ]; then ok "$2"; else bad "$2 ($1)"; fi
}

# Do not silently validate an old daemon against newer routing sources.
stale=0
for src in tools/hn_daemon.c rt/hn_sys.c rt/hn_sys.h tools/hn_cli.c; do
  if [ "$DAEMON" -ot "$src" ]; then stale=1; fi
done
if [ "$stale" -ne 0 ]; then
  printf 'sysroute-probe: SKIP dist/hn-daemon 过时，请先运行 tools/build-daemon-cli.sh\n'
  printf 'sysroute-probe: dist=%s\n' "$DAEMON"
  for src in tools/hn_daemon.c rt/hn_sys.c rt/hn_sys.h tools/hn_cli.c; do
    printf 'sysroute-probe: source=%s\n' "$src"
  done
  exit 0
fi

ck_file "$HN" "dist/hn 可执行"
ck_file "$DAEMON" "dist/hn-daemon 可执行"
[ "$FAIL" -eq 0 ] || exit 1

cat > "$PAGE" <<'HTML'
<!doctype html>
<html><head><meta charset="utf-8"><meta name="hn-title" content="sysroute probe"></head>
<body>
  <div id="host" hx-get="sys://host" hx-trigger="load">HOST_PENDING</div>
  <div id="battery" hx-get="sys://battery" hx-trigger="load">BATTERY_PENDING</div>
  <div id="unknown" hx-get="sys://route-that-does-not-exist" hx-trigger="load">UNKNOWN_PENDING</div>
  <div id="external" hx-get="ftp://example.com/fragment" hx-trigger="load">EXTERNAL_PENDING</div>
</body></html>
HTML

export HOME="$HOME_DIR"
mkdir -p "$HOME"
: > "$ERR"
"$DAEMON" > /dev/null 2> "$ERR" &
DAEMON_PID=$!

ready=0
for _ in $(seq 1 50); do
  if "$HN" ping > /dev/null 2>&1; then ready=1; break; fi
  sleep 0.1
done
if [ "$ready" -eq 1 ]; then ok "隔离 HOME 下 daemon 启动"; else bad "隔离 HOME 下 daemon 启动"; exit 1; fi

OPEN_OUT=$("$HN" open sysroute "$PAGE" --w 420 --h 240 2>> "$ERR")
printf '%s\n' "$OPEN_OUT"
case "$OPEN_OUT" in
  *sysroute*) ok "headless 页面打开" ;;
  *) bad "headless 页面打开 ($OPEN_OUT)" ;;
esac

HOST_OUT=$("$HN" text sysroute --element host 2>> "$ERR")
BATTERY_OUT=$("$HN" text sysroute --element battery 2>> "$ERR")
case "$HOST_OUT" in
  ""|HOST_PENDING) bad "真实 hx-get sys://host 换入 ($HOST_OUT)" ;;
  *) ok "真实 hx-get sys://host 换入 ($HOST_OUT)" ;;
esac
case "$BATTERY_OUT" in
  ""|BATTERY_PENDING) bad "真实 hx-get sys://battery 换入 ($BATTERY_OUT)" ;;
  *) ok "真实 hx-get sys://battery 换入 ($BATTERY_OUT)" ;;
esac

if grep -Fq '[hx] 未知 sys:// 路由: sys://route-that-does-not-exist' "$ERR"; then
  ok "未知 sys:// 路由写入 stderr 诊断"
else
  bad "未知 sys:// 路由写入 stderr 诊断"
fi
if grep -Fq '[hx] fetch 不支持: "ftp://example.com/fragment"' "$ERR"; then
  ok "非法 URL scheme 写入 stderr 诊断"
else
  bad "非法 URL scheme 写入 stderr 诊断"
fi

CLOSE_OUT=$("$HN" close sysroute 2>> "$ERR")
case "$CLOSE_OUT" in
  *'"ok":true'*) ok "探针应用关闭" ;;
  *) bad "探针应用关闭 ($CLOSE_OUT)" ;;
esac

printf '%s\n' '----------------------------------------'
printf 'sysroute-probe: %d ok, %d fail\n' "$PASS" "$FAIL"
[ "$FAIL" -eq 0 ]

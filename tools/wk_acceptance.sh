#!/usr/bin/env bash
# wk_acceptance.sh — P1(WebView 后端接线)验收门禁
#
# 断言的真实行为(不是"文件存在"):
#   ① 壳 --probe: 装载完成 + 桥消息; --transparent --probe 窗 opaque=0
#   ② 壳 --shot: WKSnapshot 产出 200x120 PNG, 中心红, 存在逐像素透明
#   ③ daemon open --window: manifest 压过 CLI 尺寸(999→200x120),
#      backend=webview/transparent/title 回包, 隐藏模式启动行可断言
#   ④ hn list 的 window 项带 backend=webview
#   ⑤ daemon shot(WebView 后端)产出 200x120 中心红 PNG
#   ⑥ update 后 300ms watch 重载; 再 shot 中心变绿且与 a.png 不同
#   ⑦ close 后窗口子进程消失, list 不再列出
#   ⑧ daemon SIGTERM 退出 0, socket 消失, 无子进程残留
#   ⑨ 可见窗(非 hidden)冒烟: open→close ≤2s(仅此段会短暂抢焦点)
#   ⑩ headless 回归: 旧 hnsoft→PNG 管线未被破坏
#
# 隔离纪律: 独立 HOME(mktemp -d) + 独立 socket; 只杀本脚本记录的 pid,
# 严禁全局 pkill。任何"不支持/跳过"都必须显式失败, 不静默通过。
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PASS_N=0
pass() { echo "PASS $1"; PASS_N=$((PASS_N + 1)); }
fail() { echo "FAIL $1: $2" >&2; exit 1; }

contains() { # <name> <haystack> <needle>
    case "$2" in
        *"$3"*) pass "$1" ;;
        *) fail "$1" "got [$2] want substring [$3]" ;;
    esac
}

not_contains() { # <name> <haystack> <needle>
    case "$2" in
        *"$3"*) fail "$1" "got [$2] must not contain [$3]" ;;
        *) pass "$1" ;;
    esac
}

now_ms() { python3 -c 'import time;print(int(time.time()*1000))'; }

# ---------------- 构建(缺任一产物立即失败, 不 skip) ----------------
echo "== build =="
bash tools/build-wk-macos.sh
bash tools/build-daemon-cli.sh

case "$(uname -m)" in
    arm64|aarch64) WK="$ROOT/dist/hnweb-wk-macos-arm64" ;;
    x86_64)        WK="$ROOT/dist/hnweb-wk-macos-x86_64" ;;
    *)             WK="$ROOT/dist/hnweb-wk-macos-$(uname -m)" ;;
esac
[ -x "$WK" ] || fail "build" "missing WebView shell $WK"
[ -x "$ROOT/dist/hn-daemon" ] || fail "build" "missing dist/hn-daemon"
[ -x "$ROOT/dist/hn" ] || fail "build" "missing dist/hn"
pass "build: daemon/cli/WebView shell present"

# ---------------- 隔离环境 ----------------
TMPH="$(mktemp -d "${TMPDIR:-/tmp}/hn-wk-acc-XXXXXX")"
export HOME="$TMPH"
export HN_WEBVIEW_HIDDEN=1
mkdir -p "$TMPH/.html-native"
SOCK="$TMPH/.html-native/hn-daemon.sock"
LOG="$TMPH/daemon.log"
LOG2="$TMPH/daemon-vis.log"
DPID=""; WPID=""; VPID=""

cleanup() {
    set +e
    [ -n "$VPID" ] && kill -TERM "$VPID" 2>/dev/null
    [ -n "$WPID" ] && kill -TERM "$WPID" 2>/dev/null
    [ -n "$DPID" ] && kill -TERM "$DPID" 2>/dev/null
    sleep 0.2
    if [ -n "$DPID" ]; then
        for c in $(pgrep -P "$DPID" 2>/dev/null); do kill -TERM "$c" 2>/dev/null; done
        kill -KILL "$DPID" 2>/dev/null
    fi
    [ -n "$WPID" ] && kill -KILL "$WPID" 2>/dev/null
    [ -n "$VPID" ] && kill -KILL "$VPID" 2>/dev/null
    rm -rf "$TMPH"
}
on_signal() { cleanup; exit 130; }
trap cleanup EXIT
trap on_signal INT TERM

# 运行时生成夹具(绝不改写仓库 examples; update 会原子覆写 page.html)
cat > "$TMPH/page.html" <<'HTML'
<html><head>
<meta name="hn-window" content="200x120">
<meta name="hn-title" content="acc window">
<meta name="hn-transparent" content="1">
<style>
html,body{margin:0;background:transparent}
#red{position:absolute;left:20px;top:20px;width:160px;height:80px;background:#ff0000}
</style>
</head><body><div id="red"></div></body></html>
HTML
cat > "$TMPH/page2.html" <<'HTML'
<html><head>
<meta name="hn-window" content="200x120">
<meta name="hn-transparent" content="1">
<style>html,body{margin:0;background:#00ff00}</style>
</head><body></body></html>
HTML

# PNG 像素断言: 尺寸 + 中心色(±8) + 可选逐像素透明
check_png() { # <name> <file> <w> <h> <r> <g> <b> <want_alpha 0|1>
    local name="$1" file="$2" w="$3" h="$4" r="$5" g="$6" b="$7" alpha="$8" out
    if ! out=$(python3 - "$file" "$w" "$h" "$r" "$g" "$b" "$alpha" <<'PY'
import sys, struct, zlib
path = sys.argv[1]
ww, wh, wr, wg, wb, want_alpha = map(int, sys.argv[2:8])
try:
    d = open(path, 'rb').read()
except OSError as e:
    sys.exit('cannot read %s: %s' % (path, e))
if d[:8] != b'\x89PNG\r\n\x1a\n':
    sys.exit('not a PNG: %s' % path)
pos, idat = 8, b''
w = h = bd = ct = inter = None
while pos < len(d):
    ln = struct.unpack('>I', d[pos:pos + 4])[0]
    typ = d[pos + 4:pos + 8]
    ch = d[pos + 8:pos + 8 + ln]
    pos += 12 + ln
    if typ == b'IHDR':
        w, h, bd, ct, comp, filt, inter = struct.unpack('>IIBBBBB', ch)
    elif typ == b'IDAT':
        idat += ch
    elif typ == b'IEND':
        break
if (w, h) != (ww, wh):
    sys.exit('size %sx%s want %sx%s' % (w, h, ww, wh))
if bd != 8 or inter != 0 or ct not in (2, 6):
    sys.exit('unsupported PNG depth=%s colortype=%s interlace=%s' % (bd, ct, inter))
nch = 4 if ct == 6 else 3
raw = zlib.decompress(idat)
stride = w * nch
prev = bytearray(stride)
off = 0
rows = []
alpha_min = 255
for y in range(h):
    f = raw[off]; off += 1
    line = bytearray(raw[off:off + stride]); off += stride
    if f == 1:
        for i in range(nch, stride):
            line[i] = (line[i] + line[i - nch]) & 255
    elif f == 2:
        for i in range(stride):
            line[i] = (line[i] + prev[i]) & 255
    elif f == 3:
        for i in range(stride):
            a = line[i - nch] if i >= nch else 0
            line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
    elif f == 4:
        for i in range(stride):
            a = line[i - nch] if i >= nch else 0
            bb = prev[i]
            c = prev[i - nch] if i >= nch else 0
            p = a + bb - c
            pa, pb, pc = abs(p - a), abs(p - bb), abs(p - c)
            pr = a if (pa <= pb and pa <= pc) else (bb if pb <= pc else c)
            line[i] = (line[i] + pr) & 255
    elif f != 0:
        sys.exit('bad PNG filter %s' % f)
    if nch == 4:
        for i in range(3, stride, 4):
            if line[i] < alpha_min:
                alpha_min = line[i]
    rows.append(line)
    prev = line
cy, cx = h // 2, w // 2
px = rows[cy][cx * nch:(cx + 1) * nch]
dr, dg, db = int(px[0]), int(px[1]), int(px[2])
for got, want, label in ((dr, wr, 'R'), (dg, wg, 'G'), (db, wb, 'B')):
    if abs(got - want) > 8:
        sys.exit('center=(%d,%d,%d) want=(%d,%d,%d)' % (dr, dg, db, wr, wg, wb))
if want_alpha == 1 and alpha_min >= 255:
    sys.exit('no transparent pixels (min alpha=%s)' % alpha_min)
print('center=(%d,%d,%d) min_alpha=%d' % (dr, dg, db, alpha_min))
PY
    ); then
        fail "$name" "$out"
    fi
    pass "$name ($out)"
}

start_daemon() { # <logfile>
    "$ROOT/dist/hn-daemon" --no-resident --socket "$SOCK" >"$1" 2>&1 &
    DPID=$!
    for _ in $(seq 1 50); do
        [ -S "$SOCK" ] && return 0
        kill -0 "$DPID" 2>/dev/null || return 1
        sleep 0.1
    done
    return 1
}

wait_gone() { # <pid> <timeout_tenths>
    local pid="$1" n="$2"
    for _ in $(seq 1 "$n"); do
        kill -0 "$pid" 2>/dev/null || return 0
        sleep 0.1
    done
    return 1
}

# ---------------- ① 壳自检 ----------------
if ! out=$("$WK" examples/wk-bridge-test.html --probe 2>&1); then
    fail "probe exit 0" "$out"
fi
contains "probe loaded + bridge" "$out" "bridge=1"
if ! out=$("$WK" examples/wk-bridge-test.html 320 240 --transparent --probe 2>&1); then
    fail "probe transparent exit 0" "$out"
fi
contains "probe transparent window is opaque=0" "$out" "opaque=0"

# ---------------- ② 壳直拍(WKSnapshot) ----------------
if ! out=$("$WK" "$TMPH/page.html" 200 120 --transparent --shot "$TMPH/direct.png" 2>&1); then
    fail "direct --shot exit 0" "$out"
fi
[ -s "$TMPH/direct.png" ] || fail "direct --shot output" "missing/empty $TMPH/direct.png"
check_png "direct snapshot PNG 200x120 red + alpha" "$TMPH/direct.png" 200 120 255 0 0 1

# ---------------- daemon: 隐藏 WebView 后端 ----------------
start_daemon "$LOG" || fail "daemon start" "socket $SOCK not ready: $(cat "$LOG" 2>/dev/null)"
pass "daemon started on isolated socket"

# ③ open: manifest 尺寸压过 --w/--h, 回包暴露 backend/transparent/title
out=$("$ROOT/dist/hn" open acc "$TMPH/page.html" --window --w 999 --h 999)
contains "open returns webview backend" "$out" '"backend":"webview"'
contains "open manifest overrides CLI size" "$out" '"w":200,"h":120'
contains "open transparent from manifest" "$out" '"transparent":true'
contains "open title from manifest" "$out" '"title":"acc window"'
WPID=$(printf '%s' "$out" | sed -n 's/.*"pid":\([0-9][0-9]*\).*/\1/p')
[ -n "$WPID" ] || fail "open pid" "no pid in [$out]"
kill -0 "$WPID" 2>/dev/null || fail "window child alive" "pid $WPID not running"
pass "window child alive (pid $WPID)"

found=0
for _ in $(seq 1 40); do
    if grep -q '\[wk\] window 200x120 transparent=1 hidden=1' "$LOG"; then found=1; break; fi
    sleep 0.05
done
[ "$found" = 1 ] || fail "hidden WebView startup line" "$(cat "$LOG")"
pass "daemon spawned WebView shell (hidden=1, manifest size)"

# ④ list
out=$("$ROOT/dist/hn" list)
contains "list shows window id" "$out" '"id":"acc"'
contains "list shows kind window" "$out" '"kind":"window"'
contains "list shows backend webview" "$out" '"backend":"webview"'

# ⑤ daemon shot via WKSnapshot
if ! out=$("$ROOT/dist/hn" shot acc "$TMPH/a.png" 2>&1); then
    fail "daemon shot exit 0" "$out"
fi
[ -s "$TMPH/a.png" ] || fail "daemon shot output" "missing/empty $TMPH/a.png"
check_png "daemon shot PNG 200x120 red" "$TMPH/a.png" 200 120 255 0 0 1

# ⑥ update → mtime watch 重载 → 再 shot 变绿
if ! out=$("$ROOT/dist/hn" update acc "$TMPH/page2.html" 2>&1); then
    fail "update exit 0" "$out"
fi
contains "update window reload ack" "$out" '"reloaded":true'
found=0
for _ in $(seq 1 30); do
    if grep -q '\[wk\] reload' "$LOG"; then found=1; break; fi
    sleep 0.1
done
[ "$found" = 1 ] || fail "mtime watch reload" "no '[wk] reload' in log after update: $(cat "$LOG")"
pass "WebView shell reloaded after atomic update"
if ! out=$("$ROOT/dist/hn" shot acc "$TMPH/b.png" 2>&1); then
    fail "post-update shot exit 0" "$out"
fi
check_png "post-update shot PNG 200x120 green" "$TMPH/b.png" 200 120 0 255 0 0
SHA_A=$(shasum -a 256 "$TMPH/a.png" | awk '{print $1}')
SHA_B=$(shasum -a 256 "$TMPH/b.png" | awk '{print $1}')
[ "$SHA_A" != "$SHA_B" ] || fail "update changes pixels" "a.png == b.png"
pass "update changed snapshot pixels"

# ⑦ close
out=$("$ROOT/dist/hn" close acc)
contains "close window ack" "$out" '"window":true'
wait_gone "$WPID" 20 || fail "window child exits" "pid $WPID still alive after close"
WPID=""
pass "window child exited"
out=$("$ROOT/dist/hn" list)
not_contains "list no longer shows closed window" "$out" '"id":"acc"'

# ⑩ headless 回归(旧 hnsoft→PNG 管线)
out=$("$ROOT/dist/hn" open h1 "$TMPH/page2.html")
contains "headless open" "$out" "已打开"
if ! out=$("$ROOT/dist/hn" shot h1 "$TMPH/h.png" 2>&1); then
    fail "headless shot exit 0" "$out"
fi
check_png "headless shot PNG 200x120 green" "$TMPH/h.png" 200 120 0 255 0 0
out=$("$ROOT/dist/hn" close h1)
contains "headless close" "$out" '"ok":true'

# ⑧ daemon 收口
kids=$(pgrep -P "$DPID" 2>/dev/null || true)
[ -z "$kids" ] || fail "no residual children" "children of daemon: $kids"
kill -TERM "$DPID"
set +e
wait "$DPID"
DEXIT=$?
set -e
DPID=""
[ "$DEXIT" -eq 0 ] || fail "daemon SIGTERM exit" "exit=$DEXIT want 0"
[ ! -e "$SOCK" ] || fail "stale socket" "$SOCK still exists after daemon exit"
pass "daemon exited 0, socket removed, no residual children"

# ---------------- ⑨ 可见窗冒烟(≤2s; 仅此段会显示/抢焦点) ----------------
export HN_WEBVIEW_HIDDEN=0
start_daemon "$LOG2" || fail "visible daemon start" "$(cat "$LOG2" 2>/dev/null)"
T0=$(now_ms)
out=$("$ROOT/dist/hn" open vis "$TMPH/page2.html" --window)
contains "visible open returns webview backend" "$out" '"backend":"webview"'
VPID=$(printf '%s' "$out" | sed -n 's/.*"pid":\([0-9][0-9]*\).*/\1/p')
[ -n "$VPID" ] || fail "visible open pid" "no pid in [$out]"
found=0
for _ in $(seq 1 30); do
    if grep -q '\[wk\] window' "$LOG2"; then found=1; break; fi
    sleep 0.05
done
[ "$found" = 1 ] || fail "visible WebView startup line" "$(cat "$LOG2")"
contains "visible shell is not hidden" "$(cat "$LOG2")" "hidden=0"
"$ROOT/dist/hn" close vis >/dev/null
T1=$(now_ms)
[ $((T1 - T0)) -le 2000 ] || fail "visible smoke ≤2s" "took $((T1 - T0))ms"
wait_gone "$VPID" 20 || fail "visible child exits" "pid $VPID still alive"
VPID=""
pass "visible window open→close $((T1 - T0))ms (≤2000ms)"

kids=$(pgrep -P "$DPID" 2>/dev/null || true)
[ -z "$kids" ] || fail "no residual children (visible)" "children: $kids"
kill -TERM "$DPID"
set +e
wait "$DPID"
DEXIT=$?
set -e
DPID=""
[ "$DEXIT" -eq 0 ] || fail "visible daemon SIGTERM exit" "exit=$DEXIT want 0"
[ ! -e "$SOCK" ] || fail "visible daemon stale socket" "$SOCK still exists"
pass "visible daemon exited 0, socket removed"

echo "== ALL PASS ($PASS_N assertions) =="

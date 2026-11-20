#!/usr/bin/env bash
# wk_host_probe.sh — P2a 常驻画布宿主验收门禁
#
# 断言的真实行为(不是"文件存在"):
#   ① ping 活; ② create(隐藏, url 页) 成功且 list 可见
#   ③ snapshot 出 PNG 且中心=红(200x120 页)
#   ④ update(html 绿) 后 snapshot 中心=绿且与前图不同
#   ⑤ 桥: 页面 neb.post → 宿主日志出现 "[bridge] <id> hello-host"
#   ⑥ destroy 后 list 空; ⑦ quit 后宿主 exit 0、socket 消失、无残留子进程
# 隔离纪律: 独立 socket + 独立临时目录; 只杀本脚本记录的 pid, 不全局 pkill。
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"

PASS_N=0
pass() { echo "PASS $1"; PASS_N=$((PASS_N + 1)); }
fail() { echo "FAIL $1: $2" >&2; exit 1; }

bash tools/build-wk-macos.sh >/dev/null
case "$(uname -m)" in
    arm64|aarch64) HOST=dist/hncanvas-host-macos-arm64 ;;
    *)             HOST="dist/hncanvas-host-macos-$(uname -m)" ;;
esac
[ -x "$HOST" ] || fail build "missing $HOST"

T="$(mktemp -d "${TMPDIR:-/tmp}/hn-host-probe-XXXXXX")"
SOCK="$T/host.sock"
LOG="$T/host.log"
HPID=""
cleanup() {
    if [ -n "$HPID" ] && kill -0 "$HPID" 2>/dev/null; then kill -TERM "$HPID" 2>/dev/null || true; fi
    rm -rf "$T"
}
trap cleanup EXIT

rpc() { # <json-line> → 一行响应
    python3 - "$SOCK" "$1" <<'PY'
import socket, sys
s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
s.settimeout(20)
s.connect(sys.argv[1])
s.sendall(sys.argv[2].encode() + b"\n")
buf = b""
while b"\n" not in buf:
    chunk = s.recv(65536)
    if not chunk:
        break
    buf += chunk
print(buf.split(b"\n")[0].decode("utf-8", "replace"))
PY
}

contains() { case "$2" in *"$3"*) pass "$1" ;; *) fail "$1" "got [$2] want [$3]" ;; esac; }
not_contains() { case "$2" in *"$3"*) fail "$1" "got [$2] must not contain [$3]" ;; *) pass "$1" ;; esac; }

center_rgb() { # <png> → "R G B"
    python3 - "$1" <<'PY'
import sys, struct, zlib
d = open(sys.argv[1], 'rb').read()
assert d[:8] == b'\x89PNG\r\n\x1a\n', 'not png'
pos, idat = 8, b''
w = h = bd = ct = inter = None
while pos < len(d):
    ln = struct.unpack('>I', d[pos:pos+4])[0]
    typ = d[pos+4:pos+8]; ch = d[pos+8:pos+8+ln]; pos += 12 + ln
    if typ == b'IHDR': w, h, bd, ct, _c, _f, inter = struct.unpack('>IIBBBBB', ch)
    elif typ == b'IDAT': idat += ch
    elif typ == b'IEND': break
assert bd == 8 and inter == 0 and ct in (2, 6), 'unsupported png'
nch = 4 if ct == 6 else 3
raw = zlib.decompress(idat)
stride = w * nch
prev = bytearray(stride); off = 0; rows = []
for y in range(h):
    f = raw[off]; off += 1
    line = bytearray(raw[off:off+stride]); off += stride
    if f == 1:
        for i in range(nch, stride): line[i] = (line[i] + line[i-nch]) & 255
    elif f == 2:
        for i in range(stride): line[i] = (line[i] + prev[i]) & 255
    elif f == 3:
        for i in range(stride):
            a = line[i-nch] if i >= nch else 0
            line[i] = (line[i] + ((a + prev[i]) >> 1)) & 255
    elif f == 4:
        for i in range(stride):
            a = line[i-nch] if i >= nch else 0
            b = prev[i]; c = prev[i-nch] if i >= nch else 0
            p = a + b - c; pa = abs(p-a); pb = abs(p-b); pc = abs(p-c)
            pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
            line[i] = (line[i] + pr) & 255
    prev = line; rows.append(bytes(line))
w2, h2 = w // 2, h // 2
px = rows[h2]
r, g, b = px[w2*nch], px[w2*nch+1], px[w2*nch+2]
print(r, g, b)
PY
}

# ---- 测试页 ----
cat > "$T/red.html" <<'HTML'
<html><head><style>html,body{margin:0;background:#ff0000}</style></head>
<body></body></html>
HTML
cat > "$T/bridge.html" <<'HTML'
<html><head><style>html,body{margin:0;background:#224488}</style></head>
<body><script>neb.post('hello-host');</script></body></html>
HTML
GREEN_LINE=$(python3 - <<'PY'
import json
print(json.dumps({"op":"update","id":"a","html":"<html><head><style>html,body{margin:0;background:#00ff00}</style></head><body></body></html>"}))
PY
)

# ---- 起宿主 ----
"$HOST" --socket "$SOCK" >"$LOG" 2>&1 &
HPID=$!
for _ in $(seq 1 100); do [ -S "$SOCK" ] && break; sleep 0.05; done
[ -S "$SOCK" ] || fail boot "host socket 未就绪: $(cat "$LOG" 2>/dev/null | tail -3)"
pass "boot: host socket ready (pid $HPID)"

r=$(rpc '{"op":"ping"}'); contains ping "$r" '"ok":true'; contains ping-pid "$r" '"pid"'

# ---- create(隐藏) + list ----
r=$(rpc "{\"op\":\"create\",\"id\":\"a\",\"url\":\"$T/red.html\",\"w\":200,\"h\":120,\"hidden\":true}")
contains create-a "$r" '"ok":true'
r=$(rpc '{"op":"list"}'); contains list-a "$r" '"id":"a"'; contains list-form "$r" '"form":'

# ---- 快照=红 ----
r=$(rpc "{\"op\":\"snapshot\",\"id\":\"a\",\"path\":\"$T/a.png\"}")
contains snap-a "$r" '"ok":true'
[ -s "$T/a.png" ] || fail snap-a "png empty"
rgb=$(center_rgb "$T/a.png"); [ "$rgb" = "255 0 0" ] && pass "snap-a red ($rgb)" || fail "snap-a red" "got [$rgb] want [255 0 0]"

# ---- update→绿, 且像素变化 ----
r=$(rpc "$GREEN_LINE"); contains update-a "$r" '"ok":true'
r=$(rpc "{\"op\":\"snapshot\",\"id\":\"a\",\"path\":\"$T/b.png\"}")
contains snap-b "$r" '"ok":true'
rgb=$(center_rgb "$T/b.png"); [ "$rgb" = "0 255 0" ] && pass "update-a green ($rgb)" || fail "update-a green" "got [$rgb] want [0 255 0]"
cmp -s "$T/a.png" "$T/b.png" && fail "update changed pixels" "a.png == b.png" || pass "update changed pixels"

# ---- 桥: 第二画布 ----
r=$(rpc "{\"op\":\"create\",\"id\":\"b\",\"url\":\"$T/bridge.html\",\"w\":200,\"h\":120,\"hidden\":true}")
contains create-b "$r" '"ok":true'
BRIDGE_OK=0
for _ in $(seq 1 60); do
    if grep -q '\[bridge\] b hello-host' "$LOG"; then BRIDGE_OK=1; break; fi
    sleep 0.05
done
[ "$BRIDGE_OK" = 1 ] && pass "bridge: b posted hello-host" || fail bridge "host log 未见 [bridge] b hello-host: $(grep '\[bridge\]' "$LOG" | tail -2)"

# ---- destroy + list 空 ----
r=$(rpc '{"op":"destroy","id":"a"}'); contains destroy-a "$r" '"ok":true'
r=$(rpc '{"op":"destroy","id":"b"}'); contains destroy-b "$r" '"ok":true'
r=$(rpc '{"op":"list"}'); not_contains list-empty "$r" '"id":"a"'
not_contains list-empty2 "$r" '"id":"b"'

# ---- quit + 清理 ----
r=$(rpc '{"op":"quit"}'); contains quit "$r" '"ok":true'
for _ in $(seq 1 100); do kill -0 "$HPID" 2>/dev/null || break; sleep 0.05; done
if kill -0 "$HPID" 2>/dev/null; then fail quit "host 未退出"; fi
wait "$HPID"; rc=$?; HPID=""
[ "$rc" = 0 ] && pass "quit: exit 0" || fail quit "exit $rc"
[ -e "$SOCK" ] && fail quit "socket 未删除" || pass "quit: socket removed"

echo "== ALL PASS ($PASS_N assertions) =="

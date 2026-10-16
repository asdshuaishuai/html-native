#!/usr/bin/env bash
# tools/http_probe.sh -- runtime HTTP/hx/QuickJS gate
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT"
if ! command -v python3 >/dev/null 2>&1; then
  echo "http_probe: SKIP python3 unavailable"
  exit 0
fi
if [ ! -x "$ROOT/dist/hn-daemon" ]; then
  echo "http_probe: FAIL missing dist/hn-daemon (run tools/build-daemon-cli.sh)" >&2
  exit 1
fi
. "$ROOT/tools/_engine_sources.sh"
RPC="$(hn_rpc_build)"
TMPD="$(mktemp -d "/tmp/hn-http-probe.XXXXXX")"
PORT="$(python3 - "$TMPD" <<'PY'
import socket
s = socket.socket(); s.bind(('127.0.0.1', 0)); print(s.getsockname()[1]); s.close()
PY
)"
SERVER_PID=""
DAEMON_PID=""
cleanup() {
  [ -n "$DAEMON_PID" ] && kill "$DAEMON_PID" 2>/dev/null || true
  [ -n "$SERVER_PID" ] && kill "$SERVER_PID" 2>/dev/null || true
  rm -rf "$TMPD"
}
trap cleanup EXIT
mkdir -p "$TMPD/www"
printf '%s\n' '<span class="remote-frag">remote-fragment</span>' >"$TMPD/www/frag.html"
printf '%s\n' '{"probe":true,"value":7}' >"$TMPD/www/data.json"
python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$TMPD/www" >"$TMPD/http.log" 2>&1 &
SERVER_PID=$!
sleep 0.3
start_daemon() {
  local allow="${1:-}"
  PROBE_HOME="$TMPD/home-$RANDOM"
  mkdir -p "$PROBE_HOME/.html-native"
  if [ -n "$allow" ]; then
    env HN_FETCH_ALLOW="$allow" HOME="$PROBE_HOME" "$ROOT/dist/hn-daemon" --no-resident >"$TMPD/daemon.log" 2>&1 &
  else
    env HOME="$PROBE_HOME" "$ROOT/dist/hn-daemon" --no-resident >"$TMPD/daemon.log" 2>&1 &
  fi
  DAEMON_PID=$!
  PROBE_SOCK="$PROBE_HOME/.html-native/hn-daemon.sock"
  for _ in 1 2 3 4 5 6 7 8 9 10; do
    [ -S "$PROBE_SOCK" ] && return 0
    sleep 0.1
  done
  echo "http_probe: daemon did not start" >&2
  return 1
}
new_page() {
  local name="$1" body="$2"
  printf '%s\n' "$body" >"$TMPD/$name.html"
}
assert_contains() {
  local label="$1" hay="$2" needle="$3"
  case "$hay" in *"$needle"*) printf 'http_probe: OK %s\n' "$label";; *) printf 'http_probe: FAIL %s (missing %s)\n' "$label" "$needle" >&2; return 1;; esac
}

new_page hx '<html><body><div id="carrier" hx-get="http://127.0.0.1:'"$PORT"'/frag.html" hx-trigger="load" hx-target="out"></div><div id="out">placeholder</div></body></html>'
start_daemon
sock="$PROBE_SOCK"
open_req="{\"op\":\"open\",\"id\":\"hx\",\"html\":$(python3 -c 'import json;print(json.dumps(open("'"$TMPD"'/hx.html").read()))'),\"w\":320,\"h\":120}"
"$RPC" "$sock" "$open_req" >/dev/null
text="$($RPC "$sock" '{"op":"text","id":"hx","element":"out"}')"
assert_contains "hx-get 200 swap" "$text" "remote-fragment"

new_page missing '<html><body><div id="carrier" hx-get="http://127.0.0.1:'"$PORT"'/missing.html" hx-trigger="load" hx-target="out"></div><div id="out">placeholder-404</div></body></html>'
open_req="{\"op\":\"open\",\"id\":\"missing\",\"html\":$(python3 -c 'import json;print(json.dumps(open("'"$TMPD"'/missing.html").read()))'),\"w\":320,\"h\":120}"
"$RPC" "$sock" "$open_req" >/dev/null
text="$($RPC "$sock" '{"op":"text","id":"missing","element":"out"}')"
assert_contains "404 target unchanged" "$text" "placeholder-404"
assert_contains "404 stderr diagnostic" "$(tr '\n' ' ' <"$TMPD/daemon.log")" "[hx] http 失败"
kill "$DAEMON_PID" 2>/dev/null || true; wait "$DAEMON_PID" 2>/dev/null || true; DAEMON_PID=""

start_daemon example.com
sock="$PROBE_SOCK"
new_page allow '<html><body><div id="carrier" hx-get="http://127.0.0.1:'"$PORT"'/frag.html" hx-trigger="load" hx-target="out"></div><div id="out">placeholder-allow</div></body></html>'
open_req="{\"op\":\"open\",\"id\":\"allow\",\"html\":$(python3 -c 'import json;print(json.dumps(open("'"$TMPD"'/allow.html").read()))'),\"w\":320,\"h\":120}"
"$RPC" "$sock" "$open_req" >/dev/null
text="$($RPC "$sock" '{"op":"text","id":"allow","element":"out"}')"
assert_contains "allowlist target unchanged" "$text" "placeholder-allow"
assert_contains "allowlist stderr diagnostic" "$(tr '\n' ' ' <"$TMPD/daemon.log")" "host 不在白名单"
kill "$DAEMON_PID" 2>/dev/null || true; wait "$DAEMON_PID" 2>/dev/null || true; DAEMON_PID=""

new_page fetch '<html><body><div id="out">pending</div><script>hn.fetch("http://127.0.0.1:'"$PORT"'/data.json").then(function(r){return r.json();}).then(function(j){document.getElementById("out").textContent="RESOLVE:"+j.value;}).catch(function(e){document.getElementById("out").textContent="REJECT:"+e.status;});</script></body></html>'
new_page reject '<html><body><div id="out">pending</div><script>hn.fetch("http://127.0.0.1:'"$PORT"'/missing.json").then(function(){document.getElementById("out").textContent="BAD";}).catch(function(e){document.getElementById("out").textContent="REJECT:"+e.status;});</script></body></html>'
start_daemon
sock="$PROBE_SOCK"
for pair in fetch reject; do
  open_req="{\"op\":\"open\",\"id\":\"$pair\",\"html\":$(python3 -c 'import json;print(json.dumps(open("'"$TMPD"'/'"$pair"'.html").read()))'),\"w\":320,\"h\":120}"
  "$RPC" "$sock" "$open_req" >/dev/null
done
text="$($RPC "$sock" '{"op":"text","id":"fetch","element":"out"}')"; assert_contains "hn.fetch resolve" "$text" "RESOLVE:7"
text="$($RPC "$sock" '{"op":"text","id":"reject","element":"out"}')"; assert_contains "hn.fetch reject" "$text" "REJECT:404"
printf '%s\n' 'http_probe: PASS'

#!/usr/bin/env bash
# media_window_probe.sh — 真窗口媒体接线的验收探针(视频/音频/降级)
# ============================================================================
# 断言"媒体宿主真的接进了 hn_app 主循环"(此前只有 ABI/后端, 没有宿主装配):
#   ① 真窗起 examples/media-demo.html 3 秒 kill, 进程不崩, 且 HN_MEDIA_TRACE
#      的 [media] 状态行显示 state=PLAYING 且 t 前进(AVFoundation 宿主在跑)。
#   ② 音频: 合成 WAV(本脚本生成 440Hz 2s) + <audio autoplay loop> 真窗
#      播 3 秒不崩, [media] 行 dur≈2.0 且 t 前进(AVPlayer 音频路径).
#   ③ 帧像素: 对同页 --shot 两次(不同 --advance), 解码 PNG 取 <video> 盒
#      中心/区域像素, 断言两帧不同 —— "视频在动"的确定性像素证据。
#      (daemon 路径对 window 应用不伺候 shot, 故用 hnapp 派生对比路线。)
#   ④ headless 平台 --probe 不崩 + --shot --advance(合成宿主)出图 —— 平台
#      媒体缺席时的降级语义(still plays: 合成梯度帧)达成。
#   ⑤ 媒体宿主装配缺失的降级: 造一份 src 指向不存在文件的文档, 真窗 3 秒
#      不崩(open 失败 → IDLE + 占位 RECT + stderr 一行, 不静默空白)。
#
# 用法: bash tools/media_window_probe.sh [dist_dir]
#   缺省 dist_dir = $ROOT/dist; 二进制名按 build-multiplatform.sh 产物名。
#   macOS 真窗档需要图形会话; 无 GUI 时该档跳过(其余照跑)。
set -u  # 不用 -e: 断言里自己收 failed 计数

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
DIST="${1:-$ROOT/dist}"
HN_APP="$DIST/hnapp-macos-arm64"
HN_HEADLESS="$DIST/hnapp-macos-headless"
DEMO="$ROOT/examples/media-demo.html"
SBX="$(mktemp -d /tmp/hn-media-probe-XXXXXX)"
fails=0

ck() { # ck <描述> <0|1 通过>
    if [ "$2" = "1" ]; then printf '  v %s\n' "$1"
    else printf '  X %s\n' "$1"; fails=$((fails + 1)); fi
}

cleanup() {
    [ -n "${WPID:-}" ] && kill -9 "$WPID" 2>/dev/null
    rm -rf "$SBX"
}
trap cleanup EXIT INT TERM

echo "== 真窗口媒体接线验收 (media_window_probe) =="

if [ ! -x "$HN_APP" ]; then
    echo "  (跳过真窗档: 缺 $HN_APP —— 先跑 tools/build-multiplatform.sh)"
fi
if [ ! -f "$DEMO" ]; then
    echo "  X 缺少 $DEMO"; exit 1
fi

# ---------- 合成 WAV(与 tools/media_probe.c 的生成逻辑同口径) ----------
python3 - "$SBX/tone.wav" <<'PY'
import math, struct, sys, wave
w = wave.open(sys.argv[1], "wb")
w.setnchannels(1); w.setsampwidth(2); w.setframerate(44100)
n = int(44100 * 2.0)
fr = bytearray()
for i in range(n):
    s = math.sin(2.0 * math.pi * 440.0 * i / 44100.0) * 0.4
    fr += struct.pack("<h", int(s * 32767.0))
w.writeframes(bytes(fr)); w.close()
PY
[ -s "$SBX/tone.wav" ] && ck "合成 440Hz/2s WAV 就绪" 1 || ck "合成 440Hz/2s WAV 就绪" 0

# ---------- ① 真窗起 media-demo 3 秒: 不崩 + PLAYING + t 前进 ----------
if [ -x "$HN_APP" ]; then
    LOG="$SBX/video-win.log"
    (cd "$ROOT/examples" && HN_MEDIA_TRACE=1 "$HN_APP" media-demo.html 520 820) >"$LOG" 2>&1 &
    WPID=$!
    sleep 3
    ALIVE=0
    kill -0 "$WPID" 2>/dev/null && ALIVE=1
    kill "$WPID" 2>/dev/null; wait "$WPID" 2>/dev/null; WPID=""
    ck "① <video> 真窗起 3 秒仍存活(未崩)" "$ALIVE"

    grep -q "宿主=platform" "$LOG" && ck "① 宿主类型 = platform(AVFoundation)" 1 \
        || ck "① 宿主类型 = platform(AVFoundation)" 0
    # PLAYING + t 前进: 取首末两条 [media] 行比较 t
    TS=$(grep -o "state=PLAYING t=[0-9.]*" "$LOG" | sed 's/.*t=//')
    NTS=$(printf '%s\n' "$TS" | grep -c . || true)
    ADV=0
    if [ "$NTS" -ge 2 ]; then
        T0=$(printf '%s\n' "$TS" | head -1)
        T1=$(printf '%s\n' "$TS" | tail -1)
        ADV=$(python3 -c "print(1 if float('$T1') > float('$T0') + 0.2 else 0)")
    fi
    ck "① state=PLAYING 且 t 前进(样本 $NTS 条, Δt>0.2s)" "$ADV"
    grep -q "state=PLAYING" "$LOG" || sed 's/^/      /' "$LOG" | head -6
else
    echo "  (跳过 ①: 无真窗二进制)"
fi

# ---------- ② 音频: 合成 WAV 真窗播 3 秒不崩 + dur/t 前进 ----------
cat > "$SBX/audio.html" <<'HTML'
<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="hn-window" content="420x300"></head>
<body style="background:#101418">
<h3 style="color:#e8edf5; font-size:14">audio 真窗探针</h3>
<audio id="au" src="tone.wav" autoplay loop volume="0.6"></audio>
<p style="color:#9aa7b8; font-size:11">AVPlayer 直出(静音跑, 探针不吵)</p>
</body></html>
HTML
if [ -x "$HN_APP" ]; then
    LOG="$SBX/audio-win.log"
    (cd "$SBX" && HN_MEDIA_TRACE=1 HN_MEDIA_MUTE=1 "$HN_APP" audio.html 420 300) >"$LOG" 2>&1 &
    WPID=$!
    sleep 3
    ALIVE=0
    kill -0 "$WPID" 2>/dev/null && ALIVE=1
    kill "$WPID" 2>/dev/null; wait "$WPID" 2>/dev/null; WPID=""
    ck "② <audio> 真窗起 3 秒仍存活(未崩)" "$ALIVE"
    # dur≈2.0(合成 WAV 元数据被 AVPlayer 解析出来)
    grep -q "tag=audio .*dur=2\.0" "$LOG" && ck "② audio dur≈2.0(元数据真解析)" 1 \
        || ck "② audio dur≈2.0(元数据真解析)" 0
    # t 前进(播过一段; loop 会回绕, 取"出现过 t>=0.5 即算前进")
    grep -Eq "tag=audio .*t=[0-9]+\.[5-9]" "$LOG" && ck "② audio t 前进(播到 t≥0.5)" 1 \
        || ck "② audio t 前进(播到 t≥0.5)" 0
    grep -q "tag=audio" "$LOG" || sed 's/^/      /' "$LOG" | head -6
else
    echo "  (跳过 ②: 无真窗二进制)"
fi

# ---------- ③ 帧像素: --shot 两次(不同 --advance) → video 盒区域必须不同 ----------
if [ -x "$HN_APP" ]; then
    S0="$SBX/f0.png"; S1="$SBX/f1500.png"
    O0=$(cd "$ROOT/examples" && "$HN_APP" media-demo.html --shot "$S0" 520 820 2>&1)
    O1=$(cd "$ROOT/examples" && "$HN_APP" media-demo.html --shot "$S1" 520 820 --advance 1500 2>&1)
    echo "$O0" | grep -q "shot OK" && ck "③ --shot advance=0 落盘" 1 || ck "③ --shot advance=0 落盘" 0
    echo "$O1" | grep -q "shot OK" && ck "③ --shot advance=1500 落盘" 1 || ck "③ --shot advance=1500 落盘" 0
    if [ -s "$S0" ] && [ -s "$S1" ]; then
        REPORT=$(python3 - "$S0" "$S1" <<'PYEOF'
import struct, zlib, sys
def pixels(p):
    d = open(p, "rb").read()
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(d):
        ln = struct.unpack(">I", d[pos:pos+4])[0]
        t = d[pos+4:pos+8]
        if t == b"IHDR": w, h = struct.unpack(">II", d[pos+8:pos+16])
        if t == b"IDAT": idat += d[pos+8:pos+8+ln]
        pos += 12 + ln
    raw = zlib.decompress(idat); stride = w*4
    o = bytearray(); prev = bytearray(stride); i = 0
    for _y in range(h):
        f = raw[i]; i += 1
        line = bytearray(raw[i:i+stride]); i += stride
        for x in range(stride):
            a = line[x-4] if x >= 4 else 0
            b = prev[x]; c = prev[x-4] if x >= 4 else 0
            if f == 1: line[x] = (line[x]+a) & 255
            elif f == 2: line[x] = (line[x]+b) & 255
            elif f == 3: line[x] = (line[x]+(a+b)//2) & 255
            elif f == 4:
                pp = a+b-c; pa, pb, pc = abs(pp-a), abs(pp-b), abs(pp-c)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c)
                line[x] = (line[x]+pr) & 255
        o += line; prev = line
    return w, h, o

# <video> 盒 = 44,62 432x243(examples/media-demo.html 的属性尺寸,
# 与 hncore boxes 输出逐值一致); 取内缩一点的区域避开圆角/边框
X0, Y0, X1, Y1 = 60, 80, 460, 290
w, h, a = pixels(sys.argv[1])
_, _, b = pixels(sys.argv[2])
diff = tot = 0
for y in range(Y0, Y1, 4):
    for x in range(X0, X1, 4):
        i = (y*w+x)*4
        tot += 1
        if a[i:i+4] != b[i:i+4]: diff += 1
cx, cy = 260, 183   # 视频盒中心
ci = (cy*w+cx)*4
print("video区 diff=%d/%d  center f0=%d,%d,%d,%d f1=%d,%d,%d,%d" % (
    diff, tot, a[ci], a[ci+1], a[ci+2], a[ci+3], b[ci], b[ci+1], b[ci+2], b[ci+3]))
sys.exit(0 if diff > tot * 0.5 else 1)
PYEOF
)
        if [ $? -eq 0 ]; then
            ck "③ video 盒区域两帧不同(>50% 像素变) | $REPORT" 1
        else
            ck "③ video 盒区域两帧不同(>50% 像素变) | $REPORT" 0
        fi
    else
        ck "③ 两帧落盘非空" 0
    fi
else
    echo "  (跳过 ③: 无真窗二进制)"
fi

# ---------- ④ headless: --probe 不崩 + 合成宿主 --shot --advance 出图 ----------
if [ -x "$HN_HEADLESS" ]; then
    if OUT=$(cd "$ROOT/examples" && "$HN_HEADLESS" media-demo.html --probe 2>&1); then
        echo "$OUT" | grep -q "^platform=headless" && ck "④ headless --probe 正常输出" 1 \
            || ck "④ headless --probe 正常输出" 0
        echo "$OUT" | grep -q "probe OK" && ck "④ headless 收下媒体元素不崩" 1 \
            || ck "④ headless 收下媒体元素不崩" 0
        echo "$OUT" | grep -q "media=synth" && ck "④ 平台缺席 → 合成宿主(synth)" 1 \
            || ck "④ 平台缺席 → 合成宿主(synth)" 0
    else
        ck "④ headless --probe 退出码 0" 0
    fi
    HS="$SBX/headless.png"
    if (cd "$ROOT/examples" && "$HN_HEADLESS" media-demo.html --shot "$HS" 520 820 --advance 1200) >/dev/null 2>&1 \
       && [ -s "$HS" ]; then
        ck "④ headless --shot --advance(合成宿主)出图" 1
    else
        ck "④ headless --shot --advance(合成宿主)出图" 0
    fi
else
    echo "  (跳过 ④: 缺 $HN_HEADLESS)"
fi

# ---------- ⑤ 降级: open 失败(src 不存在)真窗 3 秒不崩 ----------
if [ -x "$HN_APP" ]; then
    cat > "$SBX/missing.html" <<'HTML'
<!DOCTYPE html>
<html><head><meta charset="utf-8"><meta name="hn-window" content="400x240"></head>
<body style="background:#101418">
<video id="bad" src="no-such-file.mp4" autoplay width="320" height="180"></video>
</body></html>
HTML
    LOG="$SBX/missing-win.log"
    (cd "$SBX" && HN_MEDIA_TRACE=1 "$HN_APP" missing.html 400 240) >"$LOG" 2>&1 &
    WPID=$!
    sleep 3
    ALIVE=0
    kill -0 "$WPID" 2>/dev/null && ALIVE=1
    kill "$WPID" 2>/dev/null; wait "$WPID" 2>/dev/null; WPID=""
    ck "⑤ src 缺失: 真窗 3 秒不崩(open 失败 → IDLE 占位)" "$ALIVE"
    grep -q "open 失败" "$LOG" && ck "⑤ 降级语义: stderr 一行 open 失败(不静默)" 1 \
        || ck "⑤ 降级语义: stderr 一行 open 失败(不静默)" 0
    grep -q "state=IDLE" "$LOG" && ck "⑤ 会话回 IDLE(占位 RECT 保留)" 1 \
        || ck "⑤ 会话回 IDLE(占位 RECT 保留)" 0
else
    echo "  (跳过 ⑤: 无真窗二进制)"
fi

echo "----------------------------------------"
if [ "$fails" = 0 ]; then echo "media-window-probe: 全部通过"; else echo "media-window-probe: 失败 $fails 项"; fi
exit $([ "$fails" = 0 ] && echo 0 || echo 1)

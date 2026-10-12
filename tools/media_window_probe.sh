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

# 临时 HOME 隔离: 探针起的所有 hnapp 的 geometry/store 都落在沙箱,
# 与开发者正在用的窗口/daemon 账本互不可见(收口也不会误伤)。
export HOME="$SBX/home"
mkdir -p "$HOME"

# 收口纪律(性能治理同期修复: 此前 kill 的是 (…) 子壳, hnapp 本体成了
# 孤儿常驻 —— 真窗一开就是一晚上的"假死"元凶之一):
#   1. 后台启动一律 `exec env …`, 子壳当场被 hnapp 本体替换 → $!/WPID
#      就是 hnapp 的真 PID, kill 打得中;
#   2. cleanup 先 TERM(优雅关闭路径, 与 daemon win_kill 同一信号)再
#      KILL 兜底, 最后按"本 dist 二进制 + 本脚本启动的文档名"pkill 扫尾
#      —— 模式带探针特有参数, 不会命中用户自己开的窗口(pet.html 等)。
cleanup() {
    if [ -n "${WPID:-}" ]; then
        kill "$WPID" 2>/dev/null
        sleep 0.2
        kill -9 "$WPID" 2>/dev/null
        WPID=""
    fi
    pkill -f "hnapp-macos-arm64 media-demo.html" 2>/dev/null
    pkill -f "hnapp-macos-arm64 audio.html" 2>/dev/null
    pkill -f "hnapp-macos-arm64 missing.html" 2>/dev/null
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

# ---------- 合成 WAV(与 tools/media_probe.c 的生成逻辑同口径; C99 现场编译, NUL 安全) ----------
cat > "$SBX/tonegen.c" <<'CEOF'
/* tonegen — 合成 440Hz/2s 单声道 16bit PCM WAV */
#include <stdio.h>
#include <math.h>
static void wr4(FILE *w, long x) {
    fputc((int)(x & 255), w); fputc((int)((x >> 8) & 255), w);
    fputc((int)((x >> 16) & 255), w); fputc((int)((x >> 24) & 255), w);
}
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    FILE *w = fopen(argv[1], "wb"); if (!w) return 2;
    int sr = 44100; long n = (long)sr * 2;
    fputc('R',w); fputc('I',w); fputc('F',w); fputc('F',w); wr4(w, 36 + n * 2);
    fputc('W',w); fputc('A',w); fputc('V',w); fputc('E',w);
    fputc('f',w); fputc('m',w); fputc('t',w); fputc(' ',w); wr4(w, 16);
    fputc(1,w); fputc(0,w); fputc(1,w); fputc(0,w); wr4(w, sr); wr4(w, sr * 2);
    fputc(2,w); fputc(0,w); fputc(16,w); fputc(0,w);
    fputc('d',w); fputc('a',w); fputc('t',w); fputc('a',w); wr4(w, n * 2);
    for (long i = 0; i < n; i++) {
        int v = (int)(sin(2 * 3.14159265358979 * 440.0 * i / sr) * 0.4 * 32767.0);
        fputc(v & 255, w); fputc((v >> 8) & 255, w);
    }
    fclose(w); return 0;
}
CEOF
cc -O2 "$SBX/tonegen.c" -o "$SBX/tonegen" -lm && "$SBX/tonegen" "$SBX/tone.wav"
[ -s "$SBX/tone.wav" ] && ck "合成 440Hz/2s WAV 就绪" 1 || ck "合成 440Hz/2s WAV 就绪" 0

# ---------- ① 真窗起 media-demo 3 秒: 不崩 + PLAYING + t 前进 ----------
if [ -x "$HN_APP" ]; then
    LOG="$SBX/video-win.log"
    (cd "$ROOT/examples" && exec env HN_MEDIA_TRACE=1 "$HN_APP" media-demo.html 520 820) >"$LOG" 2>&1 &
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
        ADV=$(awk -v a="$T0" -v b="$T1" 'BEGIN { print (b > a + 0.2) ? 1 : 0 }')
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
    (cd "$SBX" && exec env HN_MEDIA_TRACE=1 HN_MEDIA_MUTE=1 "$HN_APP" audio.html 420 300) >"$LOG" 2>&1 &
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
        # C99 像素对比器(链引擎自带 hn_png.c, 与 hn-pngstats 同手法; 现场编译进沙箱)
        cat > "$SBX/pngdiff.c" <<'CEOF'
/* pngdiff — 两张 RGBA PNG 的矩形区域逐像素比较(步进 4 采样) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn_png.h"
static unsigned char *load(const char *p, int *w, int *h) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    unsigned char *px = hn_png_decode(buf, (size_t)n, w, h);
    free(buf); return px;
}
int main(int argc, char **argv) {
    if (argc < 7) { fprintf(stderr, "usage: pngdiff a.png b.png x0 y0 x1 y1\n"); return 2; }
    int wa, ha, wb, hb;
    unsigned char *A = load(argv[1], &wa, &ha), *B = load(argv[2], &wb, &hb);
    if (!A || !B || wa != wb || ha != hb) { fprintf(stderr, "load/size mismatch\n"); return 2; }
    int x0 = atoi(argv[3]), y0 = atoi(argv[4]), x1 = atoi(argv[5]), y1 = atoi(argv[6]);
    long diff = 0, tot = 0;
    for (int y = y0; y < y1 && y < ha; y += 4)
        for (int x = x0; x < x1 && x < wa; x += 4) {
            long i = ((long)y * wa + x) * 4; tot++;
            if (memcmp(A + i, B + i, 4) != 0) diff++;
        }
    long ci = ((long)183 * wa + 260) * 4;   /* 视频盒中心(44,62 432x243) */
    printf("video区 diff=%ld/%ld  center f0=%d,%d,%d,%d f1=%d,%d,%d,%d\n",
           diff, tot, A[ci], A[ci+1], A[ci+2], A[ci+3], B[ci], B[ci+1], B[ci+2], B[ci+3]);
    return diff > tot / 2 ? 0 : 1;
}
CEOF
        cc -O2 -I "$ROOT/Sources/CHtmlNative/include" -I "$ROOT/Sources/CHtmlNative" \
           "$SBX/pngdiff.c" "$ROOT/Sources/CHtmlNative/hn_png.c" -o "$SBX/pngdiff" || true
        REPORT=$([ -x "$SBX/pngdiff" ] && "$SBX/pngdiff" "$S0" "$S1" 60 80 460 290 2>&1)

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
    (cd "$SBX" && exec env HN_MEDIA_TRACE=1 "$HN_APP" missing.html 400 240) >"$LOG" 2>&1 &
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

# ---------- ⑥ 收口自查: 无 hnapp 孤儿残留 ----------
# 本脚本启动的 hnapp 全部带"探针特有文档名"参数 —— 按同模式 pgrep,
# 有命中即说明收口漏了(exec 替换 + trap kill 的完整链路失效)。
sleep 0.5
LEFT=$(pgrep -f "hnapp-macos-arm64 (media-demo|audio|missing)\.html" 2>/dev/null | grep -c . || true)
[ "$LEFT" = "0" ] && ck "⑥ 无 hnapp 孤儿残留(exec 收口 + trap 扫尾)" 1 \
                  || ck "⑥ 无 hnapp 孤儿残留(残留 $LEFT 个)" 0

echo "----------------------------------------"
if [ "$fails" = 0 ]; then echo "media-window-probe: 全部通过"; else echo "media-window-probe: 失败 $fails 项"; fi
exit $([ "$fails" = 0 ] && echo 0 || echo 1)

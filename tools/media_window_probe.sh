#!/usr/bin/env bash
# media_window_probe.sh — 真窗口媒体接线的验收探针(视频/音频/降级/资源有界)
# ============================================================================
# 断言"媒体宿主真的接进了 hn_app 主循环"(此前只有 ABI/后端, 没有宿主装配):
#   ① 真窗起 examples/media-demo.html 3 秒 kill, 进程不崩, 且 HN_MEDIA_TRACE
#      的 [media] 状态行显示 state=PLAYING 且 t 前进(AVFoundation 宿主在跑)。
#   ② 音频: 合成 WAV(本脚本生成 440Hz 2s) + <audio autoplay loop> 真窗
#      播 3 秒不崩, [media] 行 dur≈2.0 且 t 前进(AVPlayer 音频路径).
#   ③ 帧像素: 对同页 --shot 两次(不同 --advance), 解码 PNG 取 <video> 盒
#      中心/区域像素, 断言两帧不同 —— "视频在动"的确定性像素证据
#      (事故硬断言⑦"帧仍在动"由本档钉住, 原样保留)。
#      (daemon 路径对 window 应用不伺候 shot, 故用 hnapp 派生对比路线。)
#   ④ headless 平台 --probe 不崩 + --shot --advance(合成宿主)出图 —— 平台
#      媒体缺席时的降级语义(still plays: 合成梯度帧)达成。
#   ⑤ 媒体宿主装配缺失的降级: 造一份 src 指向不存在文件的文档, 真窗 3 秒
#      不崩(open 失败 → IDLE + 占位 RECT + stderr 一行, 不静默空白)。
#   ⑦⑧⑨ 2026-10 事故硬断言(RSS 有界 / CPU 有界 / 非绿屏):
#      真窗播 10s 的 RSS 差 < 300MB(事故②逐帧泄漏 22MB/s → 24.5GB 假死)、
#      真窗**图层模式**(AVPlayerLayer GPU 合成, 缺省)播 10s CPU 平均 < 60%
#      (CPU 逐帧路径实测 ~34% 全在解码拷贝+转换+软光栅; 图层直通后像素
#      不过 CPU, 应降到个位数~低两位数)、⑧b 对照档 HN_MEDIA_LAYER=0(CPU
#      逐帧回退路径)< 250%(旧口径, 回退路径不锈)、--shot 解码帧 video 盒
#      饱和纯绿占比 < 30%(事故①NV12 色度行距推错的雪花指纹)。
#   ⑪ GPU 合成视觉证据: 真窗图层模式播 <video>, CGWindowList 按属主 PID
#      定位窗口 → screencapture 截屏两帧(间隔 1s)→ video 盒区域帧差断言
#      (>50% 采样像素不同 = 图层合成下画面仍在动); 同时 trace 行
#      带 layer 尾标、无 [media-pb](CPU 帧转换痕迹)—— 双证据钉死"真的走
#      了 GPU 合成, 不是悄悄退回 CPU"。
#   ⑫ loop 回绕连续性(根治验收): demo.mp4 4s 真窗跑 19s → 到尾 4 次
#      (4/8/12/16s, 首圈引擎闩锁, 3 圈通知驱动)。断言 [media-end] ≥3
#      (到尾通知在投递)、[media-wrap] ≥2(通知驱动回绕在跑)、t 序列回绕处
#      无 >0.5s 停滞空洞(相邻样本 Δwall-Δt, 高精度墙钟前缀)、无 seek 风暴
#      (任意 1s 滑窗 [media-seek] ≤4 —— 旧病实测 60 次/秒); CPU 峰值/均值
#      为信息值不判。
#   ⑩ 收口自查: 无 hnapp 孤儿残留。
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
    if (x0 < 0) x0 = 0; if (y0 < 0) y0 = 0;   /* 负坐标(窗在主屏之外)钳 0 防 OOB */
    if (x1 > wa) x1 = wa; if (y1 > ha) y1 = ha;
    if (x1 <= x0 || y1 <= y0) { fprintf(stderr, "empty rect\n"); return 2; }
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

# ---------- ⑦ 事故硬断言④: 真窗播 video 10 秒, 进程 RSS 有界 ----------
# 2026-10 事故②: 真窗 media-demo 逐帧泄 CA 背衬(壳主循环无 autorelease
# 池), 22MB/s → 24.5GB 假死。修后 RSS 必须有界: 起播稳定后(2s)与 12s 各
# 采一次, 差 < 300MB(10s × 旧泄漏率 22MB/s = 220MB, 有界口径取 300MB)。
# ---------- ⑧ CPU 收紧: 真窗**图层模式**(GPU 合成, 缺省)10s 平均 < 60% ----------
# CPU 逐帧路径实测 ~34%(AVFoundation 解码拷贝 + NV12/BGRA→RGBA 转换 +
# 软件光栅重画); AVPlayerLayer 直通后像素不过 CPU, 应降到个位数~低两位数。
# ---------- ⑧b 对照档: HN_MEDIA_LAYER=0(CPU 逐帧回退)10s < 250% ----------
# 旧口径保底 —— 回退路径不能因图层模式引入而锈掉; 同时验 trace 无 layer
# 尾标、[media-pb] 在场(CPU 帧转换痕迹 = 真的还在走 CPU)。
if [ -x "$HN_APP" ]; then
    cpu_sample() { # cpu_sample <log> <env...> — 起真窗 12s, 打 "RSS差 CPU均值"
        local log="$1"; shift
        (cd "$ROOT/examples" && exec env HN_MEDIA_TRACE=1 HN_MEDIA_MUTE=1 "$@" \
            "$HN_APP" media-demo.html 520 820) >"$log" 2>&1 &
        WPID=$!
        sleep 2
        local r0 c0 r1 c1 alive=0
        r0=$(ps -o rss= -p "$WPID" 2>/dev/null | tr -d ' ')
        c0=$(ps -o cputime= -p "$WPID" 2>/dev/null | tr -d ' ')
        sleep 10
        r1=$(ps -o rss= -p "$WPID" 2>/dev/null | tr -d ' ')
        c1=$(ps -o cputime= -p "$WPID" 2>/dev/null | tr -d ' ')
        kill -0 "$WPID" 2>/dev/null && alive=1
        kill "$WPID" 2>/dev/null; wait "$WPID" 2>/dev/null; WPID=""
        [ "$alive" = "1" ] || { echo "DEAD - -"; return; }
        local drss=0
        if [ -n "$r0" ] && [ -n "$r1" ]; then
            drss=$((r1 - r0)); [ "$drss" -lt 0 ] && drss=0
        fi
        awk -v a="$c0" -v b="$c1" -v d="$drss" 'BEGIN {
            split(a, x, ":"); split(b, y, ":")
            s0 = x[1]*60 + x[2]; s1 = y[1]*60 + y[2]
            printf "%.3f %d\n", (s1 - s0) / 10.0, d
        }'
    }
    CPU_L=$(cpu_sample "$SBX/bounded-win.log")            # 图层模式(缺省)
    CPU_C=$(cpu_sample "$SBX/cpu-win.log" HN_MEDIA_LAYER=0)  # CPU 对照档
    CL=$(printf '%s' "$CPU_L" | awk '{print $1}')
    DRSS=$(printf '%s' "$CPU_L" | awk '{print $2}')
    CC=$(printf '%s' "$CPU_C" | awk '{print $1}')
    if [ "$CL" != "" ] && [ "$CL" != "DEAD" ]; then
        [ "$DRSS" -lt 307200 ] && CK_RSS=1 || CK_RSS=0
        ck "⑦ 真窗播 10s RSS 有界(Δ=$((DRSS / 1024))MB < 300MB)" "$CK_RSS"
        ck "⑧ 图层模式 10s CPU 平均 <60%(实测 ${CL} cores; CPU 档 ${CC})" \
           "$(awk -v c="$CL" 'BEGIN { print (c < 0.6) ? 1 : 0 }')"
    else
        ck "⑦ 真窗播 10s RSS 有界(采样失败)" 0
        ck "⑧ 图层模式 10s CPU 平均 <60%(采样失败)" 0
    fi
    if [ "$CC" != "" ] && [ "$CC" != "DEAD" ]; then
        ck "⑧b CPU 回退档(HN_MEDIA_LAYER=0)10s CPU <250%(实测 ${CC})" \
           "$(awk -v c="$CC" 'BEGIN { print (c < 2.5) ? 1 : 0 }')"
        # 回退路径行为钉死: trace 无 layer 尾标 + [media-pb] 在场(CPU 转换痕迹)
        grep -q " layer$" "$SBX/cpu-win.log" \
            && ck "⑧b 回退档 trace 无 layer 尾标" 0 \
            || ck "⑧b 回退档 trace 无 layer 尾标" 1
        grep -q "media-pb" "$SBX/cpu-win.log" \
            && ck "⑧b 回退档 CPU 帧转换在场([media-pb])" 1 \
            || ck "⑧b 回退档 CPU 帧转换在场([media-pb])" 0
    else
        ck "⑧b CPU 回退档 10s CPU <250%(采样失败)" 0
    fi
else
    echo "  (跳过 ⑦⑧⑧b: 无真窗二进制)"
fi

# ---------- ⑨ 事故硬断言⑥: 非绿屏(video 盒饱和纯绿占比 < 30%) ----------
# 事故①: NV12 色度行距推错 → 雪花帧里饱和纯绿(>200,80,80)占比 >70%。
# 实测修复后 0.0%。--shot --advance 取真解码帧, C99 采样器现场编译。
if [ -x "$HN_APP" ]; then
    SG="$SBX/green.png"
    (cd "$ROOT/examples" && HN_MEDIA_MUTE=1 "$HN_APP" media-demo.html --shot "$SG" 520 820 --advance 1500 >/dev/null 2>&1)
    if [ -s "$SG" ]; then
        cat > "$SBX/pnggreen.c" <<'CEOF'
/* pnggreen — 统计 PNG 矩形区域饱和纯绿(G>200, R<80, B<80)采样占比 */
#include <stdio.h>
#include <stdlib.h>
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
    if (argc < 6) return 2;
    int w, h;
    unsigned char *A = load(argv[1], &w, &h);
    if (!A) return 2;
    int x0 = atoi(argv[2]), y0 = atoi(argv[3]), x1 = atoi(argv[4]), y1 = atoi(argv[5]);
    long green = 0, tot = 0;
    for (int y = y0; y < y1 && y < h; y += 2)
        for (int x = x0; x < x1 && x < w; x += 2) {
            long i = ((long)y * w + x) * 4; tot++;
            if (A[i+1] > 200 && A[i] < 80 && A[i+2] < 80) green++;
        }
    printf("green=%ld/%ld (%.1f%%)", green, tot, 100.0 * green / (tot ? tot : 1));
    return 0;
}
CEOF
        cc -O2 -I "$ROOT/Sources/CHtmlNative/include" -I "$ROOT/Sources/CHtmlNative" \
           "$SBX/pnggreen.c" "$ROOT/Sources/CHtmlNative/hn_png.c" -o "$SBX/pnggreen" || true
        if [ -x "$SBX/pnggreen" ]; then
            GR=$("$SBX/pnggreen" "$SG" 60 80 460 290)
            GP=$(printf '%s' "$GR" | sed -E 's/.*\(([0-9.]+)%\)/\1/')
            ck "⑨ video 盒非绿屏(纯绿占比 ${GP}% < 30%) | $GR" \
               "$(awk -v p="$GP" 'BEGIN { print (p < 30.0) ? 1 : 0 }')"
        else
            ck "⑨ video 盒非绿屏(采样器编译失败)" 0
        fi
    else
        ck "⑨ video 盒非绿屏(--shot 落盘失败)" 0
    fi
else
    echo "  (跳过 ⑨: 无真窗二进制)"
fi

# ---------- ⑪ GPU 合成视觉证据: 图层模式下画面仍在动(截图帧差) ----------
# 软件证据(trace layer 尾标 / pf_frame 早退)之外要**像素证据**: 真窗图层
# 模式播 3s 后, CGWindowList 按属主 PID 定位窗口(参照 winfind 手法,
# kCGWindowBounds 与全局左上原点坐标系), screencapture 截屏两帧(间隔 1s)
# → video 盒区域帧差 —— 图层合成下画面在动, 两帧必须不同。
# 环境防线(实测本机两坑): ① 显示器睡眠时 screencapture 全黑帧 → 截屏前
# caffeinate -u 唤醒; ② 调用方无"屏幕录制"权限时 screencapture 只含壁纸
# (-R 区域截取更是恒失败)→ 用"窗口开/关各截一张, 窗口矩形无差 = 截图
# 根本看不见窗口"甄别, 此时如实报环境跳过(绝不拿壁纸当视频充数); 在
# 交互会话(终端已授录屏权限)上它是硬断言。
if [ -x "$HN_APP" ]; then
    LOG="$SBX/layer-win.log"
    caffeinate -u -t 3 >/dev/null 2>&1; sleep 1   # 先唤醒再开窗(合成/窗口记录稳定)
    (cd "$ROOT/examples" && exec env HN_MEDIA_TRACE=1 HN_MEDIA_MUTE=1 "$HN_APP" media-demo.html 520 820) >"$LOG" 2>&1 &
    WPID=$!
    sleep 3
    cat > "$SBX/winrect.c" <<'CEOF'
/* winrect — CGWindowListCopyWindowInfo 按属主 PID 找最大窗口, 打印
   "wx wy ww wh screenw"(窗口 bounds + 主屏逻辑宽; 全局左上原点) */
#include <stdio.h>
#include <stdlib.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>
int main(int argc, char **argv) {
    if (argc < 2) return 2;
    int pid = atoi(argv[1]);
    CFArrayRef list = CGWindowListCopyWindowInfo(
        kCGWindowListOptionAll | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    if (!list) return 1;
    double best = 0; CGRect br = CGRectNull;
    for (CFIndex i = 0; i < CFArrayGetCount(list); i++) {
        CFDictionaryRef d = (CFDictionaryRef)CFArrayGetValueAtIndex(list, i);
        CFNumberRef owner = (CFNumberRef)CFDictionaryGetValue(d, kCGWindowOwnerPID);
        int opid = 0;
        if (!owner || !CFNumberGetValue(owner, kCFNumberIntType, &opid) || opid != pid) continue;
        CFDictionaryRef b = (CFDictionaryRef)CFDictionaryGetValue(d, kCGWindowBounds);
        CGRect r;
        if (!b || !CGRectMakeWithDictionaryRepresentation(b, &r)) continue;
        double area = r.size.width * r.size.height;
        if (area > best && r.size.width >= 50 && r.size.height >= 50) { best = area; br = r; }
    }
    if (CGRectIsNull(br)) { CFRelease(list); return 1; }
    CGRect screen = CGDisplayBounds(CGMainDisplayID());
    printf("%d %d %d %d %d\n", (int)br.origin.x, (int)br.origin.y,
           (int)br.size.width, (int)br.size.height, (int)screen.size.width);
    CFRelease(list);
    return 0;
}
CEOF
    if cc -O2 "$SBX/winrect.c" -o "$SBX/winrect" \
           -framework CoreGraphics -framework CoreFoundation 2>/dev/null \
       && WRECT=$("$SBX/winrect" "$WPID" 2>&1) && [ -n "$WRECT" ]; then
        read -r WX WY WW WH SW <<< "$WRECT"
        if [ "$WX" -lt 0 ] || [ "$WY" -lt 0 ] || [ "$((WX + WW))" -gt "$SW" ]; then
            ck "⑪ 视频盒两帧不同(窗口不在主屏: $WRECT, 截图只含主屏)" 0
            VL_ALIVE=0
        else
            F0="$SBX/win-f0.png"; F1="$SBX/win-f1.png"; F2="$SBX/win-f2.png"
            screencapture -x "$F0" 2>/dev/null
            sleep 1
            screencapture -x "$F1" 2>/dev/null
            VL_ALIVE=0; kill -0 "$WPID" 2>/dev/null && VL_ALIVE=1
            kill "$WPID" 2>/dev/null; wait "$WPID" 2>/dev/null; WPID=""
            caffeinate -u -t 2 >/dev/null 2>&1; sleep 0.5
            screencapture -x "$F2" 2>/dev/null
            if [ ! -x "$SBX/pngdiff" ]; then
                cc -O2 -I "$ROOT/Sources/CHtmlNative/include" -I "$ROOT/Sources/CHtmlNative" \
                   "$SBX/pngdiff.c" "$ROOT/Sources/CHtmlNative/hn_png.c" -o "$SBX/pngdiff" 2>/dev/null
            fi
            if [ "$VL_ALIVE" = "1" ] && [ -s "$F0" ] && [ -s "$F1" ] && [ -s "$F2" ] \
               && [ -x "$SBX/pngdiff" ] \
               && [ "$(stat -f%z "$F0")" -gt 300000 ] && [ "$(stat -f%z "$F1")" -gt 300000 ]; then
                # video 盒窗口内逻辑取框(与 ③ 同一矩形); CGWindowBounds 是
                # 外框(含标题条), 内容上沿 = wy + (WH − 820)
                read -r RX0 RY0 RX1 RY1 WTX0 WTY0 WTX1 WTY1 <<< "$(awk \
                    -v wx="$WX" -v wy="$WY" -v ww="$WW" -v wh="$WH" -v sw="$SW" \
                    -v pw="$(sips -g pixelWidth "$F0" 2>/dev/null | awk '/pixelWidth/{print $2}')" \
                    'BEGIN { s = (sw > 0) ? pw / sw : 1; tb = wh - 820
                             printf "%d %d %d %d %d %d %d %d",
                                    int((wx + 60)*s), int((wy + tb + 80)*s),
                                    int((wx + 460)*s), int((wy + tb + 290)*s),
                                    int(wx*s), int((wy + tb)*s),
                                    int((wx + ww)*s), int((wy + wh)*s) }')"
                # 甄别: 关窗前后窗口矩形无差 = 截图看不见窗口(无录屏权限, 只截到壁纸)
                "$SBX/pngdiff" "$F0" "$F2" "$WTX0" "$WTY0" "$WTX1" "$WTY1" >/dev/null 2>&1
                SEEN=$?
                if [ "$SEEN" = "0" ]; then
                    REPORT=$("$SBX/pngdiff" "$F0" "$F1" "$RX0" "$RY0" "$RX1" "$RY1" 2>&1)
                    if [ $? -eq 0 ] && [ -n "$REPORT" ]; then
                        ck "⑪ 视频盒两帧不同(图层合成画面在动) | win=$WX,$WY,$WW,$WH rect=$RX0,$RY0-$RX1,$RY1 | $REPORT" 1
                    else
                        ck "⑪ 视频盒两帧不同(图层合成画面在动) | win=$WX,$WY,$WW,$WH | $REPORT" 0
                    fi
                else
                    ck "⑪ 视频盒两帧不同 ⏸ 环境跳过: 截图不含窗口(无录屏权限, 只截到壁纸) — layer/media-pb 两证照常钉" 1
                fi
            else
                ck "⑪ 视频盒两帧不同(截图不可用: 落盘失败/黑帧=显示器睡眠或无录屏权限)" 0
            fi
        fi
    else
        ck "⑪ 视频盒两帧不同(CGWindowList 定位失败)" 0
        VL_ALIVE=0
    fi
    # 尾部共用: 存活/layer 尾标/无 [media-pb](主路径已在帧差后收窗, 幂等)
    local_alive="$VL_ALIVE"
    if [ -n "${WPID:-}" ]; then
        kill -0 "$WPID" 2>/dev/null && local_alive=1
        kill "$WPID" 2>/dev/null; wait "$WPID" 2>/dev/null; WPID=""
    fi
    ck "⑪ 图层模式真窗 6 秒仍存活(未崩)" "$local_alive"
    grep -q "video-layer\] attach" "$LOG" \
        && ck "⑪ 图层已挂载(attach 行)" 1 \
        || ck "⑪ 图层已挂载(attach 行)" 0
    grep -q "video-layer\] ready" "$LOG" \
        && ck "⑪ 图层首帧可呈现(AVPlayerLayer.isReadyForDisplay)" 1 \
        || ck "⑪ 图层首帧可呈现(AVPlayerLayer.isReadyForDisplay)" 0
    grep -q " layer$" "$LOG" \
        && ck "⑪ trace 带 layer 尾标(图层模式生效)" 1 \
        || ck "⑪ trace 带 layer 尾标(图层模式生效)" 0
    grep -q "media-pb" "$LOG" \
        && ck "⑪ 无 CPU 帧转换痕迹(日志无 [media-pb])" 0 \
        || ck "⑪ 无 CPU 帧转换痕迹(日志无 [media-pb])" 1
    grep -q "state=PLAYING" "$LOG" || sed 's/^/      /' "$LOG" | head -6
else
    echo "  (跳过 ⑪: 无真窗二进制)"
fi
# ---------- ⑫ loop 回绕连续性(根治验收): 3 圈通知驱动回绕 ----------
# 根治后口径: demo.mp4 4s 真窗跑 19s → 到尾 4 次(4/8/12/16s), 首圈引擎
# 闩锁(平台无从预知 loop 属性), 之后 3 圈由到尾通知直接 seek(0)+play。
# 断言:
#   a) [media-end] ≥3      —— 到尾通知真的在投递(观察者链路活);
#   b) [media-wrap] ≥2     —— 通知驱动回绕真的在跑(启发式已退役为主路径);
#   c) t 序列回绕处无停滞空洞 —— 相邻 PLAYING 样本 hole=Δwall-Δt(回卷按
#      dur 调整)全部 ≤0.55s。旧病: 落位窗口 position≈duration → 每 16ms
#      重触发 seek + 撞尾自动 pause 吃补发 play, 表现即秒级空洞;
#   d) 无 seek 风暴 —— 任意 1s 滑窗 [media-seek] ≤4 且全程 ≤12(风暴特征
#      是 60 次/秒连发; 健康态 ~每 4s 一次);
#   e) 信息值: 全程 CPU 峰值/均值(回绕瞬间尖峰对照, 不判)。
# 实现注: 每行日志带高精度墙钟(perl Time::HiRes 前缀, fifo 转接保住
# hnapp 真 PID —— hole 口径的 Δwall 来源); [media] 行 app 侧显式 fflush,
# [media-end/-wrap/-seek] 走 stderr(无缓冲), 交错序可信。
if [ -x "$HN_APP" ]; then
    LOG="$SBX/wrap-win.log"; FIFO="$SBX/wrap.fifo"; CPULOG="$SBX/wrap-cpu.log"
    mkfifo "$FIFO"
    (perl -MTime::HiRes=time -ne 'BEGIN{$|=1} printf "%.3f %s", time, $_' \
        <"$FIFO" >"$LOG") &
    RPID=$!
    (cd "$ROOT/examples" && exec env HN_MEDIA_TRACE=1 HN_MEDIA_MUTE=1 \
        "$HN_APP" media-demo.html 520 820) >"$FIFO" 2>&1 &
    WPID=$!
    (while kill -0 "$WPID" 2>/dev/null; do
        printf '%s %s\n' "$(perl -MTime::HiRes=time -e 'printf "%.3f", time')" \
            "$(ps -o %cpu= -p "$WPID" 2>/dev/null | tr -d ' ')"
        sleep 0.25
    done) >"$CPULOG" &
    CPUPID=$!
    RUNS=19
    sleep "$RUNS"
    ALIVE=0; kill -0 "$WPID" 2>/dev/null && ALIVE=1
    kill "$WPID" 2>/dev/null; sleep 0.3; kill -9 "$WPID" 2>/dev/null
    wait "$WPID" 2>/dev/null; WPID=""
    kill "$CPUPID" 2>/dev/null; wait "$CPUPID" 2>/dev/null
    wait "$RPID" 2>/dev/null   # fifo 读端随写端关闭 EOF 退出
    ck "⑫ 真窗回绕 ${RUNS} 秒仍存活(未崩)" "$ALIVE"

    # 单遍 awk 统计: 通知/回绕/seek 计数 + t 序列空洞 + 1s 滑窗风暴峰值
    STATS=$(awk '
        /\[media-end\]/  { ne++ }
        /\[media-wrap\]/ { nw++ }
        /\[media-seek\]/ { ns++; sw[++k]=$1 }
        /\[media\] / && /tag=video/ && /state=PLAYING/ {
            wall=$1; t=-1; dur=-1
            for (i=1; i<=NF; i++) {
                if ($i ~ /^t=/)   t = substr($i, 3) + 0
                if ($i ~ /^dur=/) dur = substr($i, 5) + 0
            }
            if (t < 0 || dur <= 0) next
            if (have) {
                dt = t - pt
                if (dt < -dur / 2) { dt += dur; wraps++ }   # 回卷: t 序列回绕
                hole = (wall - pwall) - dt
                if (hole > maxhole) maxhole = hole
                drift += hole; twall += wall - pwall; n++
            }
            have = 1; pt = t; pwall = wall
        }
        END {
            storm = 0
            for (i = 1; i <= k; i++) {
                c = 0
                for (j = i; j <= k; j++) { if (sw[j] - sw[i] <= 1.0) c++; else break }
                if (c > storm) storm = c
            }
            printf "%d %d %d %d %.3f %.3f %d %d %.1f", \
                   ne + 0, nw + 0, ns + 0, n + 0, maxhole + 0, drift + 0, \
                   storm + 0, wraps + 0, twall + 0
        }' "$LOG")
    read -r NE NW NS N MAXHOLE DRIFT STORM WRAPS TWALL <<< "$STATS"
    [ "${NE:-0}" -ge 3 ] \
        && ck "⑫a 到尾通知在场且 ≥3 次(实测 ${NE}; [media-end])" 1 \
        || ck "⑫a 到尾通知在场且 ≥3 次(实测 ${NE})" 0
    [ "${NW:-0}" -ge 2 ] \
        && ck "⑫b 通知驱动回绕 ≥2 圈(实测 ${NW}; [media-wrap], 首圈闩锁)" 1 \
        || ck "⑫b 通知驱动回绕 ≥2 圈(实测 ${NW})" 0
    if [ "${N:-0}" -ge 20 ] && [ "${WRAPS:-0}" -ge 3 ]; then
        ck "⑫c 回绕无停滞空洞(max hole=${MAXHOLE}s ≤0.55; 样本 ${N}, 回卷 ${WRAPS} 圈)" \
           "$(awk -v h="$MAXHOLE" 'BEGIN { print (h <= 0.55) ? 1 : 0 }')"
    else
        ck "⑫c 回绕无停滞空洞(PLAYING 样本 ${N:-0} <20 或回卷 ${WRAPS:-0} <3 圈, 序列不完整)" 0
    fi
    if [ "${NS:-0}" -le 12 ] && [ "${STORM:-99}" -le 4 ]; then
        ck "⑫d 无 seek 风暴(全程 ${NS} 次, 1s 滑窗峰值 ${STORM} ≤4)" 1
    else
        ck "⑫d 无 seek 风暴(全程 ${NS:-?} 次, 1s 滑窗峰值 ${STORM:-?} >4 = 风暴)" 0
    fi
    CPUSTAT=$(awk 'NF==2 && $2+0==$2 { if ($2+0>m) m=$2+0; s+=$2; n++ }
                   END { if (n) printf "%.0f %.0f", m, s / n; else print "- -" }' "$CPULOG")
    echo "  i ⑫e CPU 信息值(回绕连跑 ${RUNS}s): 峰值/均值 = ${CPUSTAT}% ; 序列墙钟覆盖 ${TWALL:-0}s, 累计 hole(信息) = ${DRIFT:-0}s"
    grep -q "media-end" "$LOG" || { echo "      --- 日志尾([media] 行) ---"; grep "\[media\]" "$LOG" | tail -6 | sed 's/^/      /'; }
else
    echo "  (跳过 ⑫: 无真窗二进制)"
fi

# ---------- ⑩ 收口自查: 无 hnapp 孤儿残留 ----------
# 本脚本启动的 hnapp 全部带"探针特有文档名"参数 —— 按同模式 pgrep,
# 有命中即说明收口漏了(exec 替换 + trap kill 的完整链路失效)。
# (事故断言⑦"帧仍在动"由 ③ 的既有断言钉住 —— 两帧 diff > 50% 原样保留。)
sleep 0.5
LEFT=$(pgrep -f "hnapp-macos-arm64 (media-demo|audio|missing)\.html" 2>/dev/null | grep -c . || true)
[ "$LEFT" = "0" ] && ck "⑩ 无 hnapp 孤儿残留(exec 收口 + trap 扫尾)" 1 \
                  || ck "⑩ 无 hnapp 孤儿残留(残留 $LEFT 个)" 0

echo "----------------------------------------"
if [ "$fails" = 0 ]; then echo "media-window-probe: 全部通过"; else echo "media-window-probe: 失败 $fails 项"; fi
exit $([ "$fails" = 0 ] && echo 0 || echo 1)

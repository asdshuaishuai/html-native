#!/usr/bin/env bash
# hidpi_probe.sh — HiDPI/Retina 2x 渲染验收(真窗高清化 + 离屏口径不破)
#
# 三档断言, 对应"高清化只作用于真窗 blit、探针锚保持 1x"这条硬约束:
#   ① 1x 锚回归: bash tools/pet_probe.sh 全绿 —— hncore render /
#      hnapp --shot / pet_fields 的 360x360 像素口径不被 HiDPI 改动
#      (hn_app 对 --probe/--shot 显式锚定 1x, 与 HN_RENDER_SCALE 环境解耦)。
#   ② 真窗 trace + 平台侧: HN_RENDER_TRACE=1 起 examples/pet.html(360x360),
#      断言末条 [render] 行 = "scale=<min(2,backing)> bmp=<360×scale>²"
#      (缺省 = 平台 backing scale, cap 2.0; 本脚本按 hnp_window_scale 实测
#      推期望, 非 Retina 屏自动降为 scale=1); HN_RENDER_SCALE=2 再起一次
#      断言强制 "scale=2 bmp=720x720"(任何屏上都可观测的 2x 管线),
#      HN_RENDER_SCALE=1 断言压回 "scale=1 bmp=360x360"(环境钮语义)。
#      附带进程内断言(现场编译, 链真实 platform/hnp_macos.c):
#      hnp_window_scale 读到 backing + blit 720x720 位图后 NSImage 点尺寸
#      360x360、位图像素 720x720 —— "视图=逻辑点、位图=物理像素"的
#      1:1 物理呈现(发虚的旧病根是 1x 位图被系统放大)。
#   ③ 离屏 2x 单测(内嵌 C 探针): hn_rt_set_scale(2)+hn_rt_shot → PNG
#      720x720; 同页 1x vs 2x 的字形边缘"中间灰"占比差 > 1.5 倍 ——
#      证明文字是真 2x FreeType 光栅而非 1x 拉伸(拉伸会保持边缘占比,
#      真 2x 光栅的边缘带占整幅比例约减半)。
#
# 用法: bash tools/hidpi_probe.sh [dist_dir]
#   缺省 dist_dir = $ROOT/dist; 二进制名按 build-multiplatform.sh 的产物名。
#   ②③ 需要本机 cc; 真窗档需要图形会话。非 Retina 屏( backing=1 )时
#   ② 的期望值自动降为 scale=1(平台查询是事实断言, 不硬编码 2)。
set -u  # 不用 -e: 断言里自己收 failed 计数

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/_engine_sources.sh"   # hn_sources_full / hn_probe_compile(现场编译口径)
DIST="${1:-$ROOT/dist}"
HN_APP="$DIST/hnapp-macos-arm64"
# 真窗钉副屏(不占主屏); 单屏时为空 → 缺省主屏居中, 行为不变
export HN_WINDOW_POS="$(bash "$ROOT/tools/_secondary_screen.sh" 480 700)"
PET="$ROOT/examples/pet.html"
SBX="$(mktemp -d /tmp/hn-hidpi-probe-XXXXXX)"

fails=0
ck() { # ck <描述> <0|1 通过>
    if [ "$2" = "1" ]; then printf '  v %s\n' "$1"
    else printf '  X %s\n' "$1"; fails=$((fails + 1)); fi
}

cleanup() {
    if [ -n "${WPID:-}" ]; then
        kill "$WPID" 2>/dev/null
        sleep 0.2
        kill -9 "$WPID" 2>/dev/null
        WPID=""
    fi
    rm -rf "$SBX"
}
trap cleanup EXIT INT TERM

echo "== HiDPI/Retina 2x 渲染验收 (hidpi_probe) =="

# ---------- ① 1x 锚回归: pet_probe 全绿(--shot/离屏恒 1x) ----------
if [ -x "$DIST/hncore-macos-arm64" ]; then
    if bash "$ROOT/tools/pet_probe.sh" "$DIST" >"$SBX/pet.log" 2>&1; then
        ck "① pet_probe 全绿(hncore render/--shot/字段 360x360 口径不破)" 1
    else
        ck "① pet_probe 全绿(hncore render/--shot/字段 360x360 口径不破)" 0
        sed 's/^/      /' "$SBX/pet.log" | grep -E "^  X|失败" | head -6
    fi
else
    echo "  (跳过 ①: 缺 $DIST/hncore-macos-arm64 —— 先跑 tools/build-multiplatform.sh)"
fi

# ---------- ② 平台侧进程内断言(backing scale + blit 1:1 呈现) ----------
BACKING="1.0"
if command -v cc >/dev/null 2>&1 && [ -f "$ROOT/platform/hnp_macos.c" ]; then
    TMPD="$SBX/fields"
    mkdir -p "$TMPD"
    cat > "$TMPD/hidpi_fields_probe.c" <<'CEOF'
/* hidpi_fields_probe — 平台侧 HiDPI 断言(hidpi_probe.sh 现场编译)。
   链接真实 platform/hnp_macos.c: ① hnp_window_scale 读到窗口所在屏的
   backingScaleFactor(Retina=2.0, 普通屏=1.0 —— 打印实测值, 门禁按
   >=1.0 断言, 不假设本机是 Retina); ② blit 一张 720x720 位图进
   360x360 **点**的窗口后, NSImage 点尺寸=360x360(逻辑)、位图像素=
   720x720(物理) —— AppKit 恰好 1:1 物理像素呈现, 不经系统放大。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include "hn_platform.h"

typedef void *obj_t; typedef void *sel_t; typedef void *CGImageRef;
typedef struct { double x, y; } CGPoint;
typedef struct { double w, h; } CGSize;
typedef struct { CGPoint origin; CGSize size; } CGRect;
static obj_t (*gc)(const char *);
static sel_t (*sr)(const char *);
static obj_t (*ms)(obj_t, sel_t, ...);
#define M0(R,O,N) ((R (*)(obj_t,sel_t))ms)((O),sr(N))

static int fails = 0;
static void ck(int cond, const char *label) {
    printf("%s %s\n", cond ? "  v" : "  X", label);
    if (!cond) fails++;
}

static obj_t last_window(void) {
    obj_t app = M0(obj_t, gc("NSApplication"), "sharedApplication");
    obj_t wins = M0(obj_t, app, "windows");
    unsigned long n = M0(unsigned long, wins, "count");
    if (!n) return NULL;
    return ((obj_t (*)(obj_t, sel_t, unsigned long))ms)(wins, sr("objectAtIndex:"), n - 1);
}

int main(void) {
    if (!hnp_init(0, NULL)) { puts("hnp_init failed"); return 1; }
    dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_NOW);
    gc = (obj_t (*)(const char *))dlsym(RTLD_DEFAULT, "objc_getClass");
    sr = (sel_t (*)(const char *))dlsym(RTLD_DEFAULT, "sel_registerName");
    ms = (obj_t (*)(obj_t, sel_t, ...))dlsym(RTLD_DEFAULT, "objc_msgSend");
    if (!gc || !sr || !ms) { puts("objc runtime unavailable"); return 1; }

    hnp_window_desc d;
    memset(&d, 0, sizeof(d));
    d.title = "hidpi"; d.width = 360; d.height = 360; d.resizable = 1;
    hnp_window *w = hnp_window_open(&d);
    ck(w != NULL, "hnp_window_open(360x360) 成功");
    if (!w) return 1;
    obj_t win = last_window();
    ck(win != NULL, "NSApp.windows 取回窗口(hnp_window 不透明, 走 AppKit 读回)");
    if (!win) return 1;
    float bs = hnp_window_scale(w);
    printf("backing=%.1f\n", (double)bs);
    ck(bs >= 1.0f, "hnp_window_scale >= 1.0(backing 查询通路)");
    /* 与窗口所在屏的 NSWindow.backingScaleFactor 对账(平台查询的事实性) */
    {
        double raw = ((double (*)(obj_t, sel_t))ms)(win, sr("backingScaleFactor"));
        ck(bs == (float)raw, "hnp_window_scale == NSWindow.backingScaleFactor(实测对账)");
    }

    /* blit 720x720(= 360x360 逻辑的 2x 位图, hn_rt_set_scale(2) 的产物) */
    unsigned char *buf = (unsigned char *)malloc((size_t)720 * 720 * 4);
    if (buf) {
        for (int i = 0; i < 720 * 720; i++) {
            buf[i*4+0] = 0x60; buf[i*4+1] = 0x80; buf[i*4+2] = 0xE0; buf[i*4+3] = 0xFF;
        }
        hnp_window_blit(w, buf, 720, 720, (size_t)720 * 4);
        free(buf);
    }
    obj_t img = M0(obj_t, M0(obj_t, win, "contentView"), "image");
    ck(img != NULL, "blit 720x720 后 contentView.image 非空");
    if (img) {
        CGSize isz = M0(CGSize, img, "size");
        ck(isz.w == 360.0 && isz.h == 360.0, "NSImage 点尺寸 == 360x360(逻辑点)");
        CGImageRef cgimg = ((CGImageRef (*)(obj_t, sel_t, void *, void *, void *))ms)(
            img, sr("CGImageForProposedRect:context:hints:"), NULL, NULL, NULL);
        size_t (*cgw)(CGImageRef) = (size_t (*)(CGImageRef))dlsym(RTLD_DEFAULT, "CGImageGetWidth");
        size_t (*cgh)(CGImageRef) = (size_t (*)(CGImageRef))dlsym(RTLD_DEFAULT, "CGImageGetHeight");
        if (cgimg && cgw && cgh) {
            ck(cgw(cgimg) == 720 && cgh(cgimg) == 720,
               "位图像素 == 720x720(物理) → 1:1 物理呈现, 不放大");
        } else {
            ck(0, "CGImage 尺寸读取");
        }
    }
    hnp_window_close(w);
    hnp_shutdown();
    return fails ? 1 : 0;
}
CEOF
    if cc -O2 -std=c99 -I "$ROOT/platform" "$TMPD/hidpi_fields_probe.c" \
          "$ROOT/platform/hnp_macos.c" -o "$TMPD/hidpi_fields_probe" -ldl 2>"$TMPD/build.log"; then
        FOUT="$("$TMPD/hidpi_fields_probe" 2>&1)"
        echo "$FOUT" | sed 's/^/    /'
        BACKING="$(printf '%s\n' "$FOUT" | sed -n 's/^backing=\([0-9.]*\)$/\1/p')"
        [ -n "$BACKING" ] || BACKING="1.0"
        ck "② 平台侧: backing 查询 + 2x 位图 1:1 物理呈现(点 360/像素 720)" 1
    else
        ck "② 平台侧探针编译(cc + platform/hnp_macos.c)" 0
        sed 's/^/      /' "$TMPD/build.log" | head -5
    fi
else
    echo "  (跳过 ② 平台侧: 无 cc 或 platform/hnp_macos.c 不在)"
fi

# ---------- ②' 真窗 trace: [render] 末态行 = 平台缩放 × 逻辑尺寸 ----------
if [ -x "$HN_APP" ] && [ -f "$PET" ]; then
    # 临时 HOME 隔离 geometry/store(与 media_window_probe 同一纪律)
    export HOME="$SBX/home"
    mkdir -p "$HOME"
    # 期望缩放 = min(2, max(1, backing))(与 hn_app 的解析/cap 逐式一致)
    ES="$(awk -v b="$BACKING" 'BEGIN { s = b + 0; if (s < 1) s = 1; if (s > 2) s = 2;
        printf "%d", (s == (int)s) ? (int)s : 1 }')"
    BP=$((360 * ES))
    LOG="$SBX/win-trace.log"
    (cd "$ROOT/examples" && exec env HN_RENDER_TRACE=1 "$HN_APP" pet.html) >"$LOG" 2>&1 &
    WPID=$!
    sleep 2.5
    ALIVE=0
    kill -0 "$WPID" 2>/dev/null && ALIVE=1
    kill "$WPID" 2>/dev/null; wait "$WPID" 2>/dev/null; WPID=""
    ck "② 真窗起 pet.html 2.5 秒仍存活(未崩)" "$ALIVE"
    LAST="$(grep -F '[render]' "$LOG" | tail -1)"
    WANT="[render] scale=$ES bmp=${BP}x${BP}"
    if [ -n "$LAST" ] && [ "$LAST" = "$WANT" ]; then
        ck "② HN_RENDER_TRACE 末态 | $LAST(期望缺省=backing, cap 2.0)" 1
    else
        ck "② HN_RENDER_TRACE 末态 | 期望「$WANT」实际「${LAST:-无}」" 0
        sed 's/^/      /' "$LOG" | head -4
    fi
    # HN_RENDER_SCALE=1: 环境钮压回 1x(探针/强制场景)
    LOG1="$SBX/win-trace-s1.log"
    (cd "$ROOT/examples" && exec env HN_RENDER_TRACE=1 HN_RENDER_SCALE=1 "$HN_APP" pet.html) >"$LOG1" 2>&1 &
    WPID=$!
    sleep 2.5
    kill "$WPID" 2>/dev/null; wait "$WPID" 2>/dev/null; WPID=""
    LAST1="$(grep -F '[render]' "$LOG1" | tail -1)"
    if [ "$LAST1" = "[render] scale=1 bmp=360x360" ]; then
        ck "② HN_RENDER_SCALE=1 压回 1x | $LAST1" 1
    else
        ck "② HN_RENDER_SCALE=1 压回 1x | 期望「[render] scale=1 bmp=360x360」实际「${LAST1:-无}」" 0
    fi
    # HN_RENDER_SCALE=2: 环境钮强制 2x(任何屏上都可观测的 2x 真窗管线;
    # 任务口径的 [render] scale=2 bmp=720x720 断言点)
    LOG2="$SBX/win-trace-s2.log"
    (cd "$ROOT/examples" && exec env HN_RENDER_TRACE=1 HN_RENDER_SCALE=2 "$HN_APP" pet.html) >"$LOG2" 2>&1 &
    WPID=$!
    sleep 2.5
    ALIVE2=0
    kill -0 "$WPID" 2>/dev/null && ALIVE2=1
    kill "$WPID" 2>/dev/null; wait "$WPID" 2>/dev/null; WPID=""
    ck "② HN_RENDER_SCALE=2 真窗 2.5 秒仍存活(未崩)" "$ALIVE2"
    LAST2="$(grep -F '[render]' "$LOG2" | tail -1)"
    if [ "$LAST2" = "[render] scale=2 bmp=720x720" ]; then
        ck "② HN_RENDER_SCALE=2 强制 2x | $LAST2(位图 720x720 进 360x360 点窗)" 1
    else
        ck "② HN_RENDER_SCALE=2 强制 2x | 期望「[render] scale=2 bmp=720x720」实际「${LAST2:-无}」" 0
        sed 's/^/      /' "$LOG2" | head -4
    fi
else
    echo "  (跳过 ② 真窗 trace: 缺 $HN_APP 或 $PET)"
fi

# ---------- ③ 离屏 2x 单测: hn_rt_set_scale(2)+hn_rt_shot + 字形边缘统计 ----------
if command -v cc >/dev/null 2>&1; then
    TMPD="$SBX/rt"
    mkdir -p "$TMPD"
    cat > "$TMPD/hidpi_rt_probe.c" <<'CEOF'
/* hidpi_rt_probe — 离屏 2x 光栅单测(hidpi_probe.sh 现场编译)。
   走 hn_rt 公开 API: hn_rt_set_scale(1/2) + hn_rt_shot, 断言:
   ① PNG 尺寸: 1x=360x360, 2x=720x720(位图物理 = 逻辑 × scale);
   ② 字形边缘"中间灰"(r==g==b 且 0<v<255, 白字黑底的抗锯齿带)占整幅
      比例: 1x / 2x > 1.5 —— 真 2x FreeType 光栅(字形按 64px 重排重 raster,
      边缘带 1 物理像素、面积 ×4 → 占比约减半); 若实现是"1x 渲染再拉伸",
      边缘占比近似不变(比 ≈ 1), 两者以此区分。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn_rt.h"
#include "hn_png.h"

static const char *PAGE =
    "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
    "<meta name=\"hn-window\" content=\"360x360\"></head>"
    "<body style=\"background:#000000\">"
    "<h1 style=\"color:#ffffff;font-size:32\">HiDPI 锐利度</h1>"
    "<p style=\"color:#ffffff;font-size:16\">The quick brown fox 0123456789</p>"
    "</body></html>";

static unsigned char *load_png(const char *path, int *w, int *h) {
    *w = *h = 0;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    if (!buf || fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    unsigned char *px = hn_png_decode(buf, (size_t)n, w, h);
    free(buf);
    return px;
}

static double midgray_ratio(const unsigned char *px, int w, int h) {
    long total = (long)w * h, mid = 0;
    for (long i = 0; i < total; i++) {
        unsigned char r = px[i * 4], g = px[i * 4 + 1], b = px[i * 4 + 2];
        if (r == g && g == b && r > 0 && r < 255) mid++;
    }
    return total ? (double)mid / (double)total : 0.0;
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: hidpi_rt_probe <outdir>\n"); return 2; }
    const char *dir = argv[1];
    char p1[1024], p2[1024];
    snprintf(p1, sizeof p1, "%s/hidpi-1x.png", dir);
    snprintf(p2, sizeof p2, "%s/hidpi-2x.png", dir);
    hn_rt_desc d = { "hidpi-probe", PAGE, strlen(PAGE), NULL, 0, 360, 360 };
    hn_rt *rt = hn_rt_open(&d);
    if (!rt) { fprintf(stderr, "hn_rt_open failed\n"); return 2; }
    hn_rt_set_scale(rt, 1.0f);
    int ok1 = hn_rt_shot(rt, p1);
    hn_rt_set_scale(rt, 2.0f);
    int ok2 = hn_rt_shot(rt, p2);
    if (!ok1 || !ok2) { fprintf(stderr, "shot failed(%d/%d)\n", ok1, ok2); return 2; }
    int w1 = 0, h1 = 0, w2 = 0, h2 = 0;
    unsigned char *a = load_png(p1, &w1, &h1);
    unsigned char *b = load_png(p2, &w2, &h2);
    if (!a || !b) { fprintf(stderr, "png decode failed\n"); return 2; }
    double m1 = midgray_ratio(a, w1, h1);
    double m2 = midgray_ratio(b, w2, h2);
    double ratio = m2 > 0 ? m1 / m2 : 0.0;
    printf("1x=%dx%d midgray=%.5f | 2x=%dx%d midgray=%.5f | edge-ratio=%.2f\n",
           w1, h1, m1, w2, h2, m2, ratio);
    int ok = (w1 == 360 && h1 == 360 && w2 == 720 && h2 == 720 &&
              m2 > 0.0 && m1 > 1.5 * m2);
    free(a); free(b);
    hn_rt_close(rt);
    return ok ? 0 : 1;
}
CEOF
    # 编译口径 = canvas_probe.sh 同款(rt + 引擎 + hnsoft + wasm3 + QuickJS/FreeType)
    hn_sources_full
    HN_SOURCES=("$TMPD/hidpi_rt_probe.c" "$HN_RT_DIR/hn_rt.c" "$HN_RT_DIR/hn_sys.c" "$HN_RT_DIR/hn_http.c"
                ${HN_SOURCES[@]+"${HN_SOURCES[@]}"})
    local_wasm3=""
    for local_wasm3 in "${HN_WASM3[@]}"; do
        HN_SOURCES+=("$HN_WASM3_DIR/$local_wasm3")
    done
    HN_CFLAGS=(-std=c99 -O2 -Wall -ffp-contract=off)
    HN_DEFS=(-DHN_HAVE_QUICKJS)
    HN_INCS=(-I "$HN_ENG/include" -I "$HN_ENG" -I "$HN_RT_DIR" -I "$HN_WASM3_DIR"
             -I "$HN_QUICKJS_INC" -I /opt/homebrew/include/freetype2)
    HN_LIBS=("$HN_QUICKJS_A" -L/opt/homebrew/lib -lfreetype -lm)
    if hn_sources_check ${HN_SOURCES[@]+"${HN_SOURCES[@]}"} \
       && hn_probe_compile "hidpi_probe③" "$TMPD/hidpi_rt_probe"; then
        if OUT="$("$TMPD/hidpi_rt_probe" "$TMPD" 2>&1)"; then
            ck "③ 离屏 2x 单测: PNG 720x720 + 字形边缘占比 1x/2x > 1.5 | $OUT" 1
        else
            ck "③ 离屏 2x 单测: PNG 720x720 + 字形边缘占比 1x/2x > 1.5 | $OUT" 0
        fi
    else
        ck "③ 离屏 2x 探针编译(rt + 引擎 + hnsoft)" 0
    fi
else
    echo "  (跳过 ③: 无 cc)"
fi

echo "----------------------------------------"
if [ "$fails" = 0 ]; then echo "hidpi-probe: 全部通过"; else echo "hidpi-probe: 失败 $fails 项"; fi
exit $([ "$fails" = 0 ] && echo 0 || echo 1)

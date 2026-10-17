#!/usr/bin/env bash
# macos_deep_probe.sh — macOS 平台深度适配验收(呈现/输入/显示生命周期)
# ============================================================================
# 三块深度的可自动化断言(真窗需真人; 能自动化的全部覆盖):
#
#   ① 键码映射表: 现场编译一个同时包含 hn_platform.h 与 platform/hnp_macos.c
#      的 C 探针, 逐项断言 hnp_key_from_native(本机虚拟键码) == 期望语义键。
#      映射是**单源**的(探针断言的正是运行期实际用的那张表), 漂移即红。
#      同时断言 hn_rt.h 的 HN_KT_* 与 hn_platform.h 的 HNP_KEY_* 逐值一致
#      (跨层枚举对账: 任何一侧加键忘了另一侧, 这里编译期/运行期立刻暴露)。
#
#   ② 呈现路径逐位一致: 同一文档两次 --shot 逐字节相同(确定性防回归);
#      HN_BLIT_FALLBACK=1(旧 NSImage 路径)与缺省(新 drawRect 直呈路径)
#      起真窗同帧 —— 两路径都经平台 blit, 逐位一致性用"离屏渲染产物 +
#      真窗存活 + contentView 内容读回"三路合围。1x 锚: --shot 逐位不变
#      (v1 锚由 pet_probe/hidpi_probe 的 360x360 口径继续守)。
#
#   ③ HNP_EV_SCALE_CHANGED 事件存在性(编译级: 枚举名 + hnp_event.scale
#      字段)+ hn_rt_set_scale 联动(C 单测: set_scale(2) → 位图 720x720 →
#      shot PNG 720x720; set_scale(1) 回 360x360, 幂等/越界钳制)。
#
#   ④ IME 协议方法注册的运行期断言: 起窗后对 contentView 逐选择器
#      respondsToSelector:(NSTextInputClient 必需方法全集) + inputContext
#      非 nil(实测: 必须 class_addProtocol 才非 nil, 见 hnp_macos.c 注释)
#      + conformsToProtocol。
#
#   ⑤ 键/编辑路径的引擎侧行为(C 单测, 离屏): 打字追加(旧实现连打只留
#      末字符的回归)、caret 移动/删除、Tab 焦点轮换、IME 预编辑直插与
#      commit 替换、⌘C/⌘V/⌘A 引擎侧(剪贴板后端在探针环境不可用 → 断言
#      不崩/返回码), 方向键 caret 语义。
#
# 用法: bash tools/macos_deep_probe.sh [dist_dir]
#   缺省 dist_dir = $ROOT/dist。①③④ 需要本机 cc; 真窗档需要图形会话
#   (无会话时自动跳过并如实标注)。非 macOS 上整体跳过(平台探针)。
set -u  # 不用 -e: 断言里自己收 failed 计数

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/_engine_sources.sh"   # 源清单 + hn_probe_compile
DIST="${1:-$ROOT/dist}"
HN_APP="$DIST/hnapp-macos-arm64"
# 真窗钉副屏(不占主屏); 单屏时为空 → 缺省主屏居中, 行为不变
export HN_WINDOW_POS="$(bash "$ROOT/tools/_secondary_screen.sh" 480 700)"
SBX="$(mktemp -d /tmp/hn-macos-deep-XXXXXX)"
PIDS=""
fails=0

ck() { # ck <描述> <0|1 通过>
    if [ "$2" = "1" ]; then printf '  v %s\n' "$1"
    else printf '  X %s\n' "$1"; fails=$((fails + 1)); fi
}

cleanup() {
    for p in $PIDS; do kill "$p" 2>/dev/null; done
    sleep 0.2
    for p in $PIDS; do kill -9 "$p" 2>/dev/null; done
    rm -rf "$SBX"
}
trap cleanup EXIT INT TERM

echo "== macOS 平台深度适配验收 (macos_deep_probe) =="
if [ "$(uname -s)" != "Darwin" ]; then
    echo "  (跳过: 非 macOS —— 平台探针)"
    exit 0
fi

# 临时 HOME 隔离(几何/store 账本不碰开发者正在用的)
export HOME="$SBX/home"
mkdir -p "$HOME"

# ---------- ① 键码映射表 + 跨层枚举对账(现场编译 C 探针) ----------
if command -v cc >/dev/null 2>&1 && [ -f "$ROOT/platform/hnp_macos.c" ]; then
    mkdir -p "$SBX/keymap"
    cat > "$SBX/keymap/keymap_probe.c" <<'CEOF'
/* 键码映射表断言 + 跨层枚举对账(macos_deep_probe.sh 现场编译)。
   链接真实 platform/hnp_macos.c: hnp_key_from_native 是事件路径的同一张
   表(单源)。本机虚拟键码逐值取自 SDK Events.h 的 kVK_*(非凭记忆):
     kVK_Escape=53 kVK_Return=36 kVK_ANSI_KeypadEnter=76 kVK_Tab=48
     kVK_Delete=51(物理 Backspace) kVK_ForwardDelete=117
     kVK_LeftArrow=123 kVK_RightArrow=124 kVK_DownArrow=125 kVK_UpArrow=126
     kVK_Home=115 kVK_End=119 kVK_PageUp=116 kVK_PageDown=121
     kVK_Help=114 kVK_CapsLock=57 kVK_Space=49
     功能键 122/120/99/118/96/97/98/100/101/109/103/111。 */
#include <stdio.h>
#include <string.h>
#include "hn_platform.h"
#include "hn_rt.h"     /* HN_KT_* 跨层对账 */

static int fails = 0;
static void ck(int c, const char *label) {
    printf("%s %s\n", c ? "  v" : "  X", label);
    if (!c) fails++;
}

static void ck_key(unsigned long kc, hnp_key want, const char *name) {
    hnp_key got = hnp_key_from_native(kc);
    char buf[160];
    snprintf(buf, sizeof buf, "hnp_key_from_native(%lu) == %s", kc, name);
    ck(got == want, buf);
}

int main(void) {
    /* 跨层枚举对账: hn_rt.h 的 HN_KT_* 必须与 hnp_key 逐值一致
       (hn_rt 不 include 平台头 —— 这份对账是那道镜像的唯一守卫) */
    ck(HN_KT_NONE == HNP_KEY_NONE, "对账: HN_KT_NONE == HNP_KEY_NONE");
    ck(HN_KT_ESCAPE == HNP_KEY_ESCAPE, "对账: HN_KT_ESCAPE == HNP_KEY_ESCAPE");
    ck(HN_KT_RETURN == HNP_KEY_RETURN, "对账: HN_KT_RETURN == HNP_KEY_RETURN");
    ck(HN_KT_TAB == HNP_KEY_TAB, "对账: HN_KT_TAB == HNP_KEY_TAB");
    ck(HN_KT_BACKSPACE == HNP_KEY_BACKSPACE, "对账: HN_KT_BACKSPACE == HNP_KEY_BACKSPACE");
    ck(HN_KT_DELETE == HNP_KEY_DELETE, "对账: HN_KT_DELETE == HNP_KEY_DELETE");
    ck(HN_KT_LEFT == HNP_KEY_LEFT, "对账: HN_KT_LEFT == HNP_KEY_LEFT");
    ck(HN_KT_RIGHT == HNP_KEY_RIGHT, "对账: HN_KT_RIGHT == HNP_KEY_RIGHT");
    ck(HN_KT_UP == HNP_KEY_UP, "对账: HN_KT_UP == HNP_KEY_UP");
    ck(HN_KT_DOWN == HNP_KEY_DOWN, "对账: HN_KT_DOWN == HNP_KEY_DOWN");
    ck(HN_KT_HOME == HNP_KEY_HOME, "对账: HN_KT_HOME == HNP_KEY_HOME");
    ck(HN_KT_END == HNP_KEY_END, "对账: HN_KT_END == HNP_KEY_END");
    ck(HN_KT_PAGE_UP == HNP_KEY_PAGE_UP, "对账: HN_KT_PAGE_UP == HNP_KEY_PAGE_UP");
    ck(HN_KT_PAGE_DOWN == HNP_KEY_PAGE_DOWN, "对账: HN_KT_PAGE_DOWN == HNP_KEY_PAGE_DOWN");
    ck(HN_KT_PRINTABLE == HNP_KEY_PRINTABLE, "对账: HN_KT_PRINTABLE == HNP_KEY_PRINTABLE");
    ck(HN_KT_SPACE == HNP_KEY_SPACE, "对账: HN_KT_SPACE == HNP_KEY_SPACE");
    ck(HN_KT_INSERT == HNP_KEY_INSERT, "对账: HN_KT_INSERT == HNP_KEY_INSERT");
    ck(HN_KT_ENTER_NUMPAD == HNP_KEY_ENTER_NUMPAD, "对账: HN_KT_ENTER_NUMPAD == HNP_KEY_ENTER_NUMPAD");

    /* 键码映射(方向/Home/End/PageUp/PageDown/Delete/Escape/Tab/Return) */
    ck_key(53, HNP_KEY_ESCAPE, "ESCAPE");
    ck_key(36, HNP_KEY_RETURN, "RETURN");
    ck_key(76, HNP_KEY_ENTER_NUMPAD, "ENTER_NUMPAD(小键盘回车)");
    ck_key(48, HNP_KEY_TAB, "TAB");
    ck_key(51, HNP_KEY_BACKSPACE, "BACKSPACE(kVK_Delete=物理退格)");
    ck_key(117, HNP_KEY_DELETE, "DELETE(kVK_ForwardDelete=前向删除)");
    ck_key(123, HNP_KEY_LEFT, "LEFT");
    ck_key(124, HNP_KEY_RIGHT, "RIGHT");
    ck_key(125, HNP_KEY_DOWN, "DOWN");
    ck_key(126, HNP_KEY_UP, "UP");
    ck_key(115, HNP_KEY_HOME, "HOME");
    ck_key(119, HNP_KEY_END, "END");
    ck_key(116, HNP_KEY_PAGE_UP, "PAGE_UP");
    ck_key(121, HNP_KEY_PAGE_DOWN, "PAGE_DOWN");
    ck_key(114, HNP_KEY_INSERT, "INSERT(kVK_Help)");
    ck_key(49, HNP_KEY_SPACE, "SPACE");
    ck_key(57, HNP_KEY_CAPS_LOCK, "CAPS_LOCK");
    ck_key(122, HNP_KEY_F1, "F1");
    ck_key(111, HNP_KEY_F12, "F12");
    ck_key(0, HNP_KEY_PRINTABLE, "字母键(0='a')→ PRINTABLE");
    ck_key(1, HNP_KEY_PRINTABLE, "'s' → PRINTABLE");
    /* 语义键数值与历史 ABI 的对账(hn_platform.h 的"只增不改"纪律):
       旧枚举值不动, 追加键在尾部 */
    ck(HNP_KEY_ESCAPE == 1 && HNP_KEY_RETURN == 2 && HNP_KEY_TAB == 3 &&
       HNP_KEY_BACKSPACE == 4 && HNP_KEY_DELETE == 5,
       "枚举 ABI: ESCAPE=1 RETURN=2 TAB=3 BACKSPACE=4 DELETE=5(只增不改)");
    ck(HNP_KEY_INSERT == 27 && HNP_KEY_SPACE == 33 && HNP_KEY_ENTER_NUMPAD == 34,
       "追加键值: INSERT=27 SPACE=33 ENTER_NUMPAD=34(尾部追加, 不扰旧值)");
    printf("== %s (%d 项失败) ==\n", fails ? "失败" : "全部通过", fails);
    return fails ? 1 : 0;
}
CEOF
    if cc -O2 -std=c99 -I "$ROOT/platform" -I "$ROOT/rt" \
          -I "$ROOT/Sources/CHtmlNative/include" -I "$ROOT/Sources/CHtmlNative" \
          "$SBX/keymap/keymap_probe.c" "$ROOT/platform/hnp_macos.c" \
          -o "$SBX/keymap/keymap_probe" -ldl 2>"$SBX/keymap/build.log"; then
        if OUT="$("$SBX/keymap/keymap_probe" 2>&1)"; then
            printf '%s\n' "$OUT" | sed 's/^/    /'
            ck "① 键码映射表(方向/Home/End/Page/Delete/Esc/Tab/Return/小键盘)+ HN_KT_*/HNP_KEY_* 跨层对账" 1
        else
            printf '%s\n' "$OUT" | sed 's/^/      /' | grep -E "^  X|失败" | head -12
            ck "① 键码映射表 + 跨层对账" 0
        fi
    else
        ck "① 键码探针编译(cc + platform/hnp_macos.c)" 0
        sed 's/^/      /' "$SBX/keymap/build.log" | grep -E "error|fatal" | head -6
    fi
else
    echo "  (跳过 ①: 无 cc 或 platform/hnp_macos.c 不在)"
fi

# ---------- ② 呈现路径逐位一致(离屏确定性 + 两路径真窗) ----------
# ②a: 同文档同尺寸两次 --shot 逐字节一致(呈现改动不许引入非确定性)
if [ -x "$HN_APP" ] && [ -f "$ROOT/examples/pet.html" ]; then
    S0="$SBX/shot-a.png"; S1="$SBX/shot-b.png"
    (cd "$ROOT/examples" && "$HN_APP" pet.html --shot "$S0" 360 360 >/dev/null 2>&1)
    (cd "$ROOT/examples" && "$HN_APP" pet.html --shot "$S1" 360 360 >/dev/null 2>&1)
    if [ -s "$S0" ] && [ -s "$S1" ] && cmp -s "$S0" "$S1"; then
        ck "② 同文档两次 --shot 逐位一致(pet 360x360; 呈现路径确定性)" 1
    else
        ck "② 同文档两次 --shot 逐位一致(pet 360x360)" 0
    fi
    # ②b: hncore render 与 hnapp --shot 同页逐位一致(双壳同口径; 锚 1x)
    if [ -x "$DIST/hncore-macos-arm64" ]; then
        C0="$SBX/core.png"
        (cd "$ROOT/examples" && "$DIST/hncore-macos-arm64" render pet.html "$C0" 360 360 >/dev/null 2>&1)
        if [ -s "$C0" ] && cmp -s "$C0" "$S0"; then
            ck "② hncore render 与 hnapp --shot 同页逐位一致(1x 锚不破)" 1
        else
            ck "② hncore render 与 hnapp --shot 同页逐位一致(1x 锚不破)" 0
        fi
    else
        echo "  (跳过 ②b: 缺 $DIST/hncore-macos-arm64)"
    fi
    # ②c: 两条呈现路径(新直呈 vs HN_BLIT_FALLBACK 旧 NSImage)起真窗均存活
    for mode in "" "1"; do
        LOG="$SBX/win-${mode:-new}.log"
        (cd "$ROOT/examples" && exec env HN_BLIT_FALLBACK="${mode:-0}" \
            "$HN_APP" pet.html 360 360) >"$LOG" 2>&1 &
        PID=$!; PIDS="$PIDS $PID"
        sleep 2
        ALIVE=0; kill -0 "$PID" 2>/dev/null && ALIVE=1
        kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
        PIDS="${PIDS%% $PID}"; PIDS="${PIDS# }"
        if [ "${mode:-0}" = "0" ]; then
            ck "② 新直呈路径(drawRect)真窗起 pet.html 2 秒存活" "$ALIVE"
        else
            ck "② 回退路径(HN_BLIT_FALLBACK=1 NSImage)真窗 2 秒存活(对照臂)" "$ALIVE"
        fi
    done

    # ②d: **两路径真窗产物逐位一致**(呈现改动最硬的观感断言)。
    # 静态页(无动画) → CGWindowListCreateImage 抓窗口两帧 → 逐像素比较。
    # 需屏幕录制权限: 抓不到内容(全黑)时如实跳过, 不误报。
    STATIC_PAGE="$SBX/static.html"
    cat > "$STATIC_PAGE" <<'HTML'
<!DOCTYPE html><html><head><meta charset="utf-8"><meta name="hn-window" content="360x360"></head>
<body style="background:#0b0e13;margin:0">
<div style="position:absolute;left:20;top:20;width:200;height:100;background:#e05555;border-radius:12"></div>
<div style="position:absolute;left:60;top:160;width:240;height:80;background:#55a0e0"></div>
<h2 style="position:absolute;left:20;top:250;color:#e8eaf5;font-size:24">deep probe</h2>
</body></html>
HTML
    cat > "$SBX/wincap.c" <<'CEOF'
/* 窗口内容抓取(CGWindowListCreateImage; macOS 15 标记 unavailable → dlsym) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <CoreGraphics/CoreGraphics.h>
#include <CoreFoundation/CoreFoundation.h>
#include <ImageIO/ImageIO.h>
typedef CGImageRef (*fn_cgwi)(CGRect, CGWindowListOption, CGWindowID, CGWindowImageOption);
int main(int argc, char **argv) {
    if (argc < 3) return 2;
    int pid = atoi(argv[1]);
    CFArrayRef list = CGWindowListCopyWindowInfo(kCGWindowListOptionOnScreenOnly | kCGWindowListExcludeDesktopElements, kCGNullWindowID);
    if (!list) return 1;
    CGWindowID wid = 0;
    for (CFIndex i = 0; i < CFArrayGetCount(list); i++) {
        CFDictionaryRef d = (CFDictionaryRef)CFArrayGetValueAtIndex(list, i);
        CFNumberRef owner = (CFNumberRef)CFDictionaryGetValue(d, kCGWindowOwnerPID);
        int opid = 0;
        if (!owner || !CFNumberGetValue(owner, kCFNumberIntType, &opid) || opid != pid) continue;
        CFDictionaryRef b = (CFDictionaryRef)CFDictionaryGetValue(d, kCGWindowBounds);
        CGRect r; if (!b || !CGRectMakeWithDictionaryRepresentation(b, &r)) continue;
        if (r.size.width < 50 || r.size.height < 50) continue;
        CFNumberRef num = (CFNumberRef)CFDictionaryGetValue(d, kCGWindowNumber);
        if (num) { int v = 0; CFNumberGetValue(num, kCFNumberIntType, &v); wid = (CGWindowID)v; }
    }
    CFRelease(list);
    if (!wid) return 1;
    fn_cgwi f = (fn_cgwi)dlsym(RTLD_DEFAULT, "CGWindowListCreateImage");
    CGImageRef img = f ? f(CGRectNull, kCGWindowListOptionIncludingWindow, wid, kCGWindowImageBoundsIgnoreFraming) : NULL;
    if (!img) return 1;
    CFURLRef url = CFURLCreateFromFileSystemRepresentation(NULL, (const UInt8 *)argv[2], strlen(argv[2]), false);
    CGImageDestinationRef dest = url ? CGImageDestinationCreateWithURL(url, CFSTR("public.png"), 1, NULL) : NULL;
    int ok = 0;
    if (dest) { CGImageDestinationAddImage(dest, img, NULL); ok = CGImageDestinationFinalize(dest); CFRelease(dest); }
    if (url) CFRelease(url);
    CGImageRelease(img);
    return ok ? 0 : 1;
}
CEOF
    cat > "$SBX/pngdiff2.c" <<'CEOF'
/* 两 PNG **窗口内容区**逐像素比较(链引擎 hn_png.c)。
   裁掉窗口装饰: top 跳过标题栏(h1 − 内容高), 四周 margin 跳过圆角/投影
   抗锯齿带 —— 那些像素随抓屏时机/键窗口态逐次微变(实测: 同路径两次也会
   变), 与呈现路径无关。内容区是呈现路径的产物, 必须逐位一致。
   用法: pngdiff2 a.png b.png <content_top> <margin> */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn_png.h"
static unsigned char *load(const char *p, int *w, int *h) {
    FILE *f = fopen(p, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t)n);
    if (!b || fread(b, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(b); return NULL; }
    fclose(f);
    unsigned char *px = hn_png_decode(b, (size_t)n, w, h);
    free(b); return px;
}
int main(int argc, char **argv) {
    if (argc < 5) { printf("usage: pngdiff2 a b top margin\n"); return 2; }
    int w1, h1, w2, h2;
    unsigned char *a = load(argv[1], &w1, &h1);
    unsigned char *b = load(argv[2], &w2, &h2);
    int top = atoi(argv[3]), m = atoi(argv[4]);
    if (!a || !b) { printf("decode fail\n"); return 2; }
    if (w1 != w2 || h1 != h2) { printf("size %dx%d vs %dx%d\n", w1, h1, w2, h2); return 1; }
    long nonblack = 0, diff = 0, n = 0;
    for (int y = top + m; y < h1 - m; y++)
        for (int x = m; x < w1 - m; x++) {
            int i = (y * w1 + x) * 4;
            if (a[i] | a[i+1] | a[i+2]) nonblack++;
            if (memcmp(a + i, b + i, 4) != 0) diff++;
            n++;
        }
    printf("inner=%ldx1 nonblack=%.1f%% diff_px=%ld\n", n,
           n ? 100.0 * nonblack / (double)n : 0.0, diff);
    free(a); free(b);
    /* 空白帧(抓屏无权限) → 退出码 3 = 环境跳过 */
    if (n == 0 || nonblack * 100 < n * 2) return 3;
    return diff ? 1 : 0;
}
CEOF
    if cc -O2 "$SBX/wincap.c" -o "$SBX/wincap" -framework CoreGraphics -framework CoreFoundation -framework ImageIO 2>/dev/null \
       && cc -O2 -I "$ROOT/Sources/CHtmlNative/include" -I "$ROOT/Sources/CHtmlNative" \
             "$SBX/pngdiff2.c" "$ROOT/Sources/CHtmlNative/hn_png.c" -o "$SBX/pngdiff2" 2>/dev/null; then
        caffeinate -u -t 3 >/dev/null 2>&1; sleep 1
        SHOT_N="$SBX/path-new.png"; SHOT_O="$SBX/path-old.png"
        for mode in 0 1; do
            (cd /tmp && exec env HOME="$SBX/home" HN_BLIT_FALLBACK="$mode" \
                "$HN_APP" "$STATIC_PAGE" 360 360) >"$SBX/path-$mode.log" 2>&1 &
            PID=$!; PIDS="$PIDS $PID"
            sleep 2.5
            [ "$mode" = "0" ] && OUT="$SHOT_N" || OUT="$SHOT_O"
            "$SBX/wincap" "$PID" "$OUT" >/dev/null 2>&1
            kill "$PID" 2>/dev/null; wait "$PID" 2>/dev/null
            PIDS="${PIDS%% $PID}"; PIDS="${PIDS# }"
            sleep 0.6
        done
        if [ -s "$SHOT_N" ] && [ -s "$SHOT_O" ]; then
            # 内容区 top = 窗口高 − 内容高(360): 从图高推算标题栏高度
            IMGH="$(sed -n 's/.*inner=\([0-9]*\)x1.*/\1/p' /dev/null 2>/dev/null)"
            # 用引擎头文件里的 PNG 解码器取图高: 简单做法 —— 交给 pngdiff2
            # 自算(top 传 (h1 − 360)); 这里直接传 h−360 需要 h, 改用 awk 解 PNG IHDR
            IHDR_H="$(od -An -tu1 -j20 -N4 "$SHOT_N" 2>/dev/null | awk '{print $1*16777216+$2*65536+$3*256+$4}')"
            CTOP=28
            if [ -n "$IHDR_H" ] && [ "$IHDR_H" -gt 360 ] 2>/dev/null; then
                CTOP=$((IHDR_H - 360))
            fi
            "$SBX/pngdiff2" "$SHOT_N" "$SHOT_O" "$CTOP" 6
            RC=$?
            DIFFOUT="$("$SBX/pngdiff2" "$SHOT_N" "$SHOT_O" "$CTOP" 6 2>&1)"
            if [ "$RC" = "0" ]; then
                ck "② 新直呈 vs 旧 NSImage 两路径真窗截图逐位一致(内容区) | $DIFFOUT" 1
            elif [ "$RC" = "3" ]; then
                echo "  (跳过 ②d 两路径截图对照: 抓屏全黑 —— 需屏幕录制权限)"
            else
                ck "② 两路径真窗截图逐位一致(内容区) | $DIFFOUT" 0
            fi
        else
            echo "  (跳过 ②d: 窗口抓取失败(屏幕录制权限?))"
        fi
    else
        echo "  (跳过 ②d: wincap/pngdiff2 编译失败)"
    fi
else
    echo "  (跳过 ②: 缺 $HN_APP 或 examples/pet.html)"
fi

# ---------- ③ HNP_EV_SCALE_CHANGED 编译级 + hn_rt_set_scale 联动(C 单测) ----------
mkdir -p "$SBX/scale"
cat > "$SBX/scale/scale_probe.c" <<'CEOF'
/* ③ 事件枚举存在性(编译级) + hn_rt_set_scale 联动(运行期)。
   HNP_EV_SCALE_CHANGED / hnp_event.scale 字段被引用即"编译级存在性"
   (枚举/字段改名 → 本探针编不过 → 门禁红)。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn_platform.h"
#include "hn_rt.h"
#include "hn_png.h"

static int fails = 0;
static void ck(int c, const char *label) {
    printf("%s %s\n", c ? "  v" : "  X", label);
    if (!c) fails++;
}

static void png_size(const char *path, int *w, int *h) {
    *w = *h = -1;
    FILE *f = fopen(path, "rb");
    if (!f) return;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return; }
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    if (buf && fread(buf, 1, (size_t)n, f) == (size_t)n) {
        unsigned char *px = hn_png_decode(buf, (size_t)n, w, h);
        free(px);
    }
    free(buf);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 2) { fprintf(stderr, "usage: scale_probe <outdir>\n"); return 2; }
    const char *dir = argv[1];
    /* ③a 编译级: 事件枚举与字段存在(被引用即断言) */
    hnp_event_kind k = HNP_EV_SCALE_CHANGED;
    hnp_event_kind k2 = HNP_EV_SCREEN_CHANGED;
    hnp_event_kind k3 = HNP_EV_IME_PREEDIT;
    hnp_event_kind k4 = HNP_EV_IME_COMMIT;
    hnp_event_kind k5 = HNP_EV_FOCUS_IN;
    hnp_event e;
    memset(&e, 0, sizeof(e));
    e.kind = k; e.scale = 2.0f; e.cursor = 0;
    ck(k != k2 && k2 != k3 && k3 != k4 && k4 != k5, "事件枚举存在: SCALE/SCREEN/IME_PREEDIT/IME_COMMIT/FOCUS_IN");
    ck(e.scale == 2.0f && e.kind == HNP_EV_SCALE_CHANGED, "hnp_event.scale 字段可用");

    /* ③b 运行期: hn_rt_set_scale 联动(位图尺寸/重渲染/幂等/钳制) */
    const char *PAGE =
        "<!DOCTYPE html><html><head><meta charset=\"utf-8\">"
        "<meta name=\"hn-window\" content=\"360x360\"></head>"
        "<body style=\"background:#101418\"><h2 style=\"color:#e8eaf5\">scale</h2>"
        "</body></html>";
    hn_rt_desc d = { "deep-scale", PAGE, strlen(PAGE), NULL, 0, 360, 360 };
    hn_rt *rt = hn_rt_open(&d);
    ck(rt != NULL, "hn_rt_open(360x360)");
    if (!rt) return 1;
    ck(hn_rt_scale(rt) == 1.0f && hn_rt_bitmap_width(rt) == 360,
       "缺省 scale=1.0 位图 360x360(1x 锚)");
    hn_rt_set_scale(rt, 2.0f);
    ck(hn_rt_scale(rt) == 2.0f && hn_rt_bitmap_width(rt) == 720 && hn_rt_bitmap_height(rt) == 720,
       "set_scale(2) → 位图 720x720(SCALE_CHANGED 处理路径的核心调用)");
    char p2[1024]; snprintf(p2, sizeof p2, "%s/scale-2x.png", dir);
    ck(hn_rt_shot(rt, p2), "2x 落盘");
    int w = 0, h = 0; png_size(p2, &w, &h);
    ck(w == 720 && h == 720, "2x PNG 720x720");
    hn_rt_set_scale(rt, 2.0f);   /* 同值幂等 */
    ck(hn_rt_bitmap_width(rt) == 720, "set_scale 同值幂等");
    hn_rt_set_scale(rt, 0.1f);   /* 越界钳制到 1.0 */
    ck(hn_rt_scale(rt) == 1.0f && hn_rt_bitmap_width(rt) == 360,
       "set_scale(0.1) 钳回 1.0 → 位图 360x360(跨屏降档路径)");
    /* 剪贴板断言需要 AppKit 已装载(clip_set 走 dlsym objc) —— 先 init。 */
    ck(hnp_init(0, NULL), "hnp_init(AppKit 装载, 剪贴板前置)");
    /* ③b' 剪贴板链路(hn_rt.c 的 clip_set/clip_get): 本轮修复了
       dlsym((void*)0,...) 在 macOS 恒 NULL 的隐性 bug(RTLD_DEFAULT 是
       (void*)-2; 此前 ⌘C/⌘V 与 sys://clipboard 静默失效, 界面永远显示
       "clip unavailable")。sys:// 通道往返 + hn_rt_edit(COPY) 两条都要验。 */
    {
        char *frag = hn_rt_sys_fragment("sys://clipboard/set?value=deep-probe-clip", NULL, "dp");
        int w_ok = frag && strstr(frag, "copied") != NULL;
        char *rd = hn_rt_sys_fragment("sys://clipboard/get", NULL, "dp");
        int r_ok = rd && strstr(rd, "deep-probe-clip") != NULL;
        ck(w_ok, "sys://clipboard/set 写入成功(不再是 clip unavailable)");
        ck(r_ok, "sys://clipboard/get 读回一致");
        free(frag); free(rd);
        /* hn_rt_edit(COPY) → 平台剪贴板(编辑路径与 hx 通道同源)。
           注意: input 有 margin/padding, 点 (5,5) 可能落在 body 上 ——
           用节点盒中心命中(与真实点击同一路径)。 */
        const char *P2 = "<html><body><input id=\"c\" value=\"copy-me\"></body></html>";
        hn_rt_desc d2 = { "deep-clip", P2, strlen(P2), NULL, 0, 200, 120 };
        hn_rt *rt2 = hn_rt_open(&d2);
        if (rt2) {
            hn_node *nn = hn_doc_find_by_id(hn_rt_doc(rt2), "c");
            if (nn) {
                float bx2 = 0, by2 = 0, bw2 = 0, bh2 = 0;
                hn_node_box(nn, &bx2, &by2, &bw2, &bh2);
                /* 击左上内边距(命中 input 本体; 中心点会落在其内部文本
                   run 的命中面上, 引擎命中到 <html> —— 与真实点击同路径) */
                hn_rt_button(rt2, 1, bx2 + 4, by2 + 4, 0);
                hn_rt_edit(rt2, HN_EDIT_COPY, NULL);
                char got[128] = { 0 };
                int ok2 = hnp_clipboard_get(NULL, got, sizeof got);
                char mbuf[220];
                snprintf(mbuf, sizeof mbuf,
                    "hn_rt_edit(COPY) → 系统剪贴板(编辑路径剪贴板桥) | got=\"%s\"", got);
                ck(ok2 == 1 && strstr(got, "copy-me") != NULL, mbuf);
            }
            hn_rt_close(rt2);
        }
    }
    /* ③c 屏幕可见区查询存在(真窗才拿得到; headless 返回 0 是契约) */
    hnp_window_desc wd;
    memset(&wd, 0, sizeof(wd));
    wd.width = 200; wd.height = 150;
    hnp_window *win = hnp_window_open(&wd);
    if (win) {
        int vx = 0, vy = 0, vw = 0, vh = 0;
        int ok = hnp_screen_workarea(win, 50, 50, &vx, &vy, &vw, &vh);
        char buf[160];
        snprintf(buf, sizeof buf, "hnp_screen_workarea = %d(%d,%d %dx%d)", ok, vx, vy, vw, vh);
        ck(ok == 1 && vw > 100 && vh > 100, buf);
        /* hnp_window_move 不崩(NULL 与实窗) */
        hnp_window_move(win, vx, vy);
        hnp_window_move(NULL, 0, 0);
        ck(1, "hnp_window_move 实窗/NULL 不崩");

        hnp_window_close(win);
    } else {
        ck(0, "③c 真窗创建(屏幕可见区/移动断言)");
    }
    hnp_shutdown();
    hn_rt_close(rt);
    printf("== %s (%d 项失败) ==\n", fails ? "失败" : "全部通过", fails);
    return fails ? 1 : 0;
}
CEOF
if command -v cc >/dev/null 2>&1; then
    hn_sources_full
    HN_SOURCES=("$SBX/scale/scale_probe.c" "$HN_RT_DIR/hn_rt.c" "$HN_RT_DIR/hn_sys.c" "$HN_RT_DIR/hn_http.c"
                "$ROOT/platform/hnp_macos.c"
                ${HN_SOURCES[@]+"${HN_SOURCES[@]}"})
    for f in "${HN_WASM3[@]}"; do HN_SOURCES+=("$HN_WASM3_DIR/$f"); done
    HN_CFLAGS=(-std=c99 -O2 -Wall -ffp-contract=off)
    HN_DEFS=()
    HN_INCS=(-I "$HN_ENG/include" -I "$HN_ENG" -I "$HN_RT_DIR" -I "$ROOT/platform"
             -I "$HN_WASM3_DIR" -I /opt/homebrew/include/freetype2)
    HN_LIBS=(-L/opt/homebrew/lib -lfreetype -lm -ldl)
    if hn_sources_check ${HN_SOURCES[@]+"${HN_SOURCES[@]}"} \
       && hn_probe_compile "macos_deep_probe③" "$SBX/scale/scale_probe"; then
        if OUT="$("$SBX/scale/scale_probe" "$SBX/scale" 2>&1)"; then
            printf '%s\n' "$OUT" | sed 's/^/    /'
            ck "③ HNP_EV_SCALE_CHANGED 编译级 + hn_rt_set_scale 联动(位图/幂等/钳制)+ 可见区/移动" 1
        else
            printf '%s\n' "$OUT" | sed 's/^/      /' | grep -E "^  X|失败" | head -8
            ck "③ SCALE 事件/联动断言" 0
        fi
    else
        ck "③ scale 探针编译(rt + 引擎 + hnp_macos.c)" 0
    fi
else
    echo "  (跳过 ③: 无 cc)"
fi

# ---------- ④ IME 协议方法注册(真窗运行期断言) ----------
if command -v cc >/dev/null 2>&1 && [ -f "$ROOT/platform/hnp_macos.c" ]; then
    mkdir -p "$SBX/ime"
    cat > "$SBX/ime/ime_probe.c" <<'CEOF'
/* ④ IME 协议接入的运行期断言(现场编译; 链真实 platform/hnp_macos.c)。
   起窗后取 contentView 逐项 respondsToSelector: —— NSTextInputClient 的
   全部必需方法必须都在(类注册时 class_addMethod 的全集); 且
   inputContext 非 nil / conformsToProtocol: 为真。
   实测纪律(写进 hnp_macos.c 的注释): 只 add_method 不 addProtocol 时
   inputContext 为 nil —— 本探针正是那条结论的门禁。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include "hn_platform.h"

typedef void *obj_t; typedef void *sel_t;
typedef struct { double x, y; } CGPoint;
static obj_t (*gc)(const char *);
static sel_t (*sr)(const char *);
static obj_t (*ms)(obj_t, sel_t, ...);
#define M0(R,O,N) ((R (*)(obj_t,sel_t))ms)((O),sr(N))

static int fails = 0;
static void ck(int c, const char *label) {
    printf("%s %s\n", c ? "  v" : "  X", label);
    if (!c) fails++;
}

static int responds(obj_t o, const char *name) {
    return (int)((signed char (*)(obj_t, sel_t, sel_t))ms)(o, sr("respondsToSelector:"), sr(name));
}

int main(void) {
    hnp_event out_ev;   /* ③d 用(SCALE/SCREEN 事件) */
    dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_NOW);
    gc = (obj_t (*)(const char *))dlsym(RTLD_DEFAULT, "objc_getClass");
    sr = (sel_t (*)(const char *))dlsym(RTLD_DEFAULT, "sel_registerName");
    ms = (obj_t (*)(obj_t, sel_t, ...))dlsym(RTLD_DEFAULT, "objc_msgSend");
    if (!gc || !sr || !ms) { puts("objc runtime unavailable"); return 1; }
    if (!hnp_init(0, NULL)) { puts("hnp_init failed"); return 1; }
    hnp_window_desc d;
    memset(&d, 0, sizeof(d));
    d.title = "ime-probe"; d.width = 300; d.height = 200; d.resizable = 1;
    hnp_window *w = hnp_window_open(&d);
    ck(w != NULL, "hnp_window_open 成功");
    if (!w) return 1;
    obj_t app = M0(obj_t, gc("NSApplication"), "sharedApplication");
    obj_t wins = M0(obj_t, app, "windows");
    unsigned long n = M0(unsigned long, wins, "count");
    obj_t win = n ? ((obj_t (*)(obj_t, sel_t, unsigned long))ms)(wins, sr("objectAtIndex:"), n - 1) : NULL;
    ck(win != NULL, "NSApp.windows 取回窗口");
    obj_t iv = win ? M0(obj_t, win, "contentView") : NULL;
    ck(iv != NULL, "contentView 取回");
    if (!iv) return 1;

    /* NSTextInputClient 必需方法集(见 SDK NS 的 NSTextInputClient.h @required) */
    ck(responds(iv, "insertText:replacementRange:"), "responds: insertText:replacementRange:");
    ck(responds(iv, "doCommandBySelector:"), "responds: doCommandBySelector:");
    ck(responds(iv, "setMarkedText:selectedRange:replacementRange:"), "responds: setMarkedText:selectedRange:replacementRange:");
    ck(responds(iv, "unmarkText"), "responds: unmarkText");
    ck(responds(iv, "selectedRange"), "responds: selectedRange");
    ck(responds(iv, "markedRange"), "responds: markedRange");
    ck(responds(iv, "hasMarkedText"), "responds: hasMarkedText");
    ck(responds(iv, "attributedSubstringForProposedRange:actualRange:"), "responds: attributedSubstringForProposedRange:actualRange:");
    ck(responds(iv, "validAttributesForMarkedText"), "responds: validAttributesForMarkedText");
    ck(responds(iv, "firstRectForCharacterRange:actualRange:"), "responds: firstRectForCharacterRange:actualRange:");
    ck(responds(iv, "characterIndexForPoint:"), "responds: characterIndexForPoint:");

    obj_t ctx = M0(obj_t, iv, "inputContext");
    ck(ctx != NULL, "view.inputContext 非 nil(系统认作输入客户端)");
    void *(*gp)(const char *) = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "objc_getProtocol");
    void *proto = gp ? gp("NSTextInputClient") : NULL;
    if (proto) {
        int conf = (int)((signed char (*)(obj_t, sel_t, void *))ms)(iv, sr("conformsToProtocol:"), proto);
        ck(conf, "conformsToProtocol(NSTextInputClient)(class_addProtocol 生效)");
    } else {
        ck(0, "objc_getProtocol(NSTextInputClient)");
    }

    /* 呈现直通(深度 1)运行期: 视图响应 drawRect: 且 blit 后 CGImage 有内容 */
    ck(responds(iv, "drawRect:"), "responds: drawRect:(直呈路径)");
    {
        unsigned char *buf = (unsigned char *)malloc((size_t)300 * 200 * 4);
        if (buf) {
            for (int i = 0; i < 300 * 200; i++) {
                buf[i*4+0] = 0x30; buf[i*4+1] = 0x60; buf[i*4+2] = 0xA0; buf[i*4+3] = 0xFF;
            }
            hnp_window_blit(w, buf, 300, 200, (size_t)300 * 4);
            free(buf);
        }
        obj_t img = M0(obj_t, iv, "image");
        ck(img != NULL, "blit 后 contentView.image 非空(兼容面保持)");
    }
    /* IME 焦点喂入(激活/停用两态)不崩 */
    hnp_ime_focus(w, 10, 10, 120, 24);
    hnp_ime_focus(w, 0, 0, 0, 0);
    hnp_ime_focus(NULL, 1, 2, 3, 4);
    ck(1, "hnp_ime_focus 激活/停用/NULL 不崩");
    /* 剪贴板桥(进程内可达性; 无文本时 get 返回 0 也是契约) */
    {
        char cb[64] = { 0 };
        int g = hnp_clipboard_get(w, cb, sizeof cb);
        int s = hnp_clipboard_set(w, "hn-deep-probe");
        ck(s == 1, "hnp_clipboard_set 写入系统剪贴板");
        if (s) {
            char cb2[64] = { 0 };
            int g2 = hnp_clipboard_get(w, cb2, sizeof cb2);
            ck(g2 == 1 && strcmp(cb2, "hn-deep-probe") == 0, "clipboard 往返一致(set→get)");
        }
        (void)g;
    }
    /* ---- ④b IME 端到端(合成键事件经真实路由) ----
       postEvent: 一个 keyDown 'n'(keyCode 45)进 NSApp 队列 → hnp_pump 的
       keydown 分支把"未组字 + 无修饰 + 可打印"的键交给输入上下文
       (interpretKeyEvents) → 系统回调 insertText → 挂起队列 → 下一个
       hnp_pump 出 HNP_EV_IME_COMMIT "n"。**这是"中文能打出来"链路的
       机械可验证部分**(真拼音输入需真人手动验收: 组字/选字/上屏)。
       注: 目标窗口按"contentView 响应 firstRectForCharacterRange"识别
       (NSApp.windows 里可能有 AppKit 自建窗口; 实测坑)。
       事件窗口查找: 平台侧按 windowNumber(不是指针) —— NSEvent 出队
       实例与构造实例不同, 见 hnp_macos.c 的 g_win_map 注释。 */
    {
        obj_t wins = M0(obj_t, M0(obj_t, gc("NSApplication"), "sharedApplication"), "windows");
        unsigned long nw2 = M0(unsigned long, wins, "count");
        obj_t tw = NULL;
        for (unsigned long i = 0; i < nw2 && !tw; i++) {
            obj_t wi = ((obj_t (*)(obj_t, sel_t, unsigned long))ms)(wins, sr("objectAtIndex:"), i);
            obj_t cv = wi ? M0(obj_t, wi, "contentView") : NULL;
            if (cv && responds(cv, "firstRectForCharacterRange:actualRange:")) tw = wi;
        }
        ck(tw != NULL, "④b 定位本进程 hnp 窗口(NSApp.windows 扫描)");
        if (tw) {
            hnp_ime_focus(w, 10, 30, 200, 24);   /* 可编辑焦点 → IME 激活 */
            long wnum = ((long (*)(obj_t, sel_t))ms)(tw, sr("windowNumber"));
            obj_t chars = ((obj_t (*)(obj_t, sel_t, const char *))ms)(
                gc("NSString"), sr("stringWithUTF8String:"), "n");
            CGPoint pt; pt.x = 10; pt.y = 30;
            obj_t ev = ((obj_t (*)(obj_t, sel_t, unsigned long, CGPoint, unsigned long,
                                   double, long, obj_t, obj_t, obj_t, signed char, unsigned short))ms)(
                gc("NSEvent"), sr("keyEventWithType:location:modifierFlags:timestamp:windowNumber:context:characters:charactersIgnoringModifiers:isARepeat:keyCode:"),
                (unsigned long)10, pt, 0UL, 0.0, wnum, NULL, chars, chars, 0, 45);
            if (ev) {
                ((void (*)(obj_t, sel_t, obj_t, int))ms)(
                    M0(obj_t, gc("NSApplication"), "sharedApplication"),
                    sr("postEvent:atStart:"), ev, 0);
                hnp_event out;
                int commit_ok = 0;
                for (int i = 0; i < 30 && !commit_ok; i++) {
                    if (!hnp_pump(&out, 60)) continue;
                    if (out.kind == HNP_EV_IME_COMMIT &&
                        out.utf8 && strcmp(out.utf8, "n") == 0) commit_ok = 1;
                }
                ck(commit_ok, "④b 合成 keyDown 'n' → IME_COMMIT \"n\"(IME 路由端到端)");
            } else {
                ck(0, "④b 合成 NSEvent 构造");
            }
            hnp_ime_focus(w, 0, 0, 0, 0);
        }
    }
    /* ---- ③d(并入本探针: 这里有 objc 机制) SCALE/SCREEN 事件真窗产出 ----
       合成 viewDidChangeBackingProperties(与 AppKit 跨屏真调用同一 IMP)
       → pump 出 HNP_EV_SCALE_CHANGED; 投 NSScreen 参数通知 → SCREEN_CHANGED。
       无 Retina 副屏时物理跨屏不可复现, 本断言覆盖"回调→脏标志→事件"通路。 */
    {
        while (hnp_pump(&out_ev, 0)) {}   /* 清队列 */
        obj_t wins3 = M0(obj_t, M0(obj_t, gc("NSApplication"), "sharedApplication"), "windows");
        unsigned long n3 = M0(unsigned long, wins3, "count");
        obj_t cv3 = NULL;
        for (unsigned long i = 0; i < n3 && !cv3; i++) {
            obj_t wi = ((obj_t (*)(obj_t, sel_t, unsigned long))ms)(wins3, sr("objectAtIndex:"), i);
            obj_t c = wi ? M0(obj_t, wi, "contentView") : NULL;
            if (c && responds(c, "firstRectForCharacterRange:actualRange:")) cv3 = c;
        }
        if (cv3) {
            ((void (*)(obj_t, sel_t))ms)(cv3, sr("viewDidChangeBackingProperties"));
            int got_scale = 0;
            for (int i = 0; i < 5 && !got_scale; i++)
                if (hnp_pump(&out_ev, 30) && out_ev.kind == HNP_EV_SCALE_CHANGED) got_scale = 1;
            ck(got_scale, "③d 真窗 SCALE_CHANGED 事件产出(viewDidChangeBackingProperties → pump)");
            obj_t nc3 = M0(obj_t, gc("NSNotificationCenter"), "defaultCenter");
            obj_t nm3 = ((obj_t (*)(obj_t, sel_t, const char *))ms)(gc("NSString"),
                sr("stringWithUTF8String:"), "NSApplicationDidChangeScreenParametersNotification");
            ((void (*)(obj_t, sel_t, obj_t, obj_t))ms)(
                nc3, sr("postNotificationName:object:"), nm3, NULL);
            int got_scr = 0;
            for (int i = 0; i < 5 && !got_scr; i++)
                if (hnp_pump(&out_ev, 30) && out_ev.kind == HNP_EV_SCREEN_CHANGED) got_scr = 1;
            ck(got_scr, "③d 真窗 SCREEN_CHANGED 事件产出(NSScreen 通知 → pump)");
        } else {
            ck(0, "③d 真窗视图定位");
        }
    }
    hnp_window_close(w);
    hnp_shutdown();
    printf("== %s (%d 项失败) ==\n", fails ? "失败" : "全部通过", fails);
    return fails ? 1 : 0;
}
CEOF
    if cc -O2 -std=c99 -I "$ROOT/platform" "$SBX/ime/ime_probe.c" \
          "$ROOT/platform/hnp_macos.c" -o "$SBX/ime/ime_probe" -ldl 2>"$SBX/ime/build.log"; then
        if OUT="$("$SBX/ime/ime_probe" 2>&1)"; then
            printf '%s\n' "$OUT" | sed 's/^/    /'
            ck "④ IME 协议方法注册 + inputContext/conform + drawRect + 剪贴板往返" 1
        else
            printf '%s\n' "$OUT" | sed 's/^/      /' | grep -E "^  X|失败|Exception" | head -10
            ck "④ IME/直呈/剪贴板运行期断言" 0
        fi
    else
        ck "④ IME 探针编译(cc + platform/hnp_macos.c)" 0
        sed 's/^/      /' "$SBX/ime/build.log" | head -6
    fi
else
    echo "  (跳过 ④: 无 cc 或 platform/hnp_macos.c 不在)"
fi

# ---------- ⑤ 键/编辑路径引擎侧行为(离屏 C 单测) ----------
if command -v cc >/dev/null 2>&1; then
    mkdir -p "$SBX/edit"
    cat > "$SBX/edit/edit_probe.c" <<'CEOF'
/* ⑤ 引擎侧编辑语义(离屏, 无平台): 打字追加(回归: 旧实现连打只留末字符)、
   caret 移动/删除、Tab 焦点轮换、IME 预编辑直插与 commit、编辑命令。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn_rt.h"
#include "hn_platform.h"

static int fails = 0;
static void ck(int c, const char *label) {
    printf("%s %s\n", c ? "  v" : "  X", label);
    if (!c) fails++;
}
static const char *val(hn_rt *rt, const char *id) {
    static char buf[4096];
    hn_doc *doc = hn_rt_doc(rt);
    hn_node *n = hn_doc_find_by_id(doc, id);
    if (!n) { buf[0] = 0; return buf; }
    size_t vl = 0;
    const char *v = hn_node_value(n, &vl);
    snprintf(buf, sizeof buf, "%s", v ? v : "");
    return buf;
}
static hn_node *node(hn_rt *rt, const char *id) {
    return hn_doc_find_by_id(hn_rt_doc(rt), id);
}

int main(void) {
    const char *html =
        "<html><body>"
        "<input id=\"a\" value=\"\"><input id=\"b\" value=\"\">"
        "<textarea id=\"t\"></textarea>"
        "</body></html>";
    hn_rt_desc d = { "deep-edit", html, strlen(html), NULL, 0, 300, 260 };
    hn_rt *rt = hn_rt_open(&d);
    ck(rt != NULL, "hn_rt_open");
    if (!rt) return 1;
    /* 点 input a 拿焦点 */
    float x, y, w, h; hn_node_box(node(rt, "a"), &x, &y, &w, &h);
    hn_rt_button(rt, 1, x + 4, y + 4, 0);
    hn_rt_key(rt, 1, HN_KT_PRINTABLE, "h");
    hn_rt_key(rt, 1, HN_KT_PRINTABLE, "i");
    hn_rt_key(rt, 1, HN_KT_PRINTABLE, "!");
    ck(!strcmp(val(rt, "a"), "hi!"), "打字追加: h,i,! → \"hi!\"(旧实现只留 \"!\" 的回归)");
    /* caret 应在末尾(3); 左移一次 → 2; 插入 'X' → hiX! */
    ck(hn_node_caret(node(rt, "a")) == 3, "插入后 caret 在末尾(3)");
    hn_rt_key(rt, 1, HN_KT_LEFT, NULL);
    ck(hn_node_caret(node(rt, "a")) == 2, "LEFT → caret=2");
    hn_rt_key(rt, 1, HN_KT_PRINTABLE, "X");
    ck(!strcmp(val(rt, "a"), "hiX!"), "caret 处插入: \"hiX!\"");
    /* HOME/END */
    hn_rt_key(rt, 1, HN_KT_HOME, NULL);
    ck(hn_node_caret(node(rt, "a")) == 0, "HOME → caret=0");
    hn_rt_key(rt, 1, HN_KT_END, NULL);
    ck(hn_node_caret(node(rt, "a")) == 4, "END → caret=4");
    /* Backspace(删 '!') / Delete 前向(在 caret=0 时删 'h') */
    hn_rt_key(rt, 1, HN_KT_BACKSPACE, NULL);
    ck(!strcmp(val(rt, "a"), "hiX"), "BACKSPACE 删末字符 → \"hiX\"");
    hn_rt_key(rt, 1, HN_KT_HOME, NULL);
    hn_rt_key(rt, 1, HN_KT_DELETE, NULL);
    ck(!strcmp(val(rt, "a"), "iX"), "DELETE 前向删首字符(caret 不动) → \"iX\"");
    /* UTF-8 多字节: 插入中文, 左移一次应整字(3 字节) */
    hn_rt_key(rt, 1, HN_KT_END, NULL);
    hn_rt_key(rt, 1, HN_KT_PRINTABLE, "\xE4\xB8\xAD");   /* 中 */
    ck(!strcmp(val(rt, "a"), "iX\xE4\xB8\xAD"), "UTF-8 插入(中)");
    hn_rt_key(rt, 1, HN_KT_LEFT, NULL);
    ck(hn_node_caret(node(rt, "a")) == 2, "LEFT 跨 UTF-8 整字(4→...→2)");
    hn_rt_key(rt, 1, HN_KT_BACKSPACE, NULL);
    ck(!strcmp(val(rt, "a"), "i\xE4\xB8\xAD"), "BACKSPACE 删整字");

    /* Tab 焦点轮换(文档序环形; 焦点盒读回判定当前控件) */
    {
        float ax, ay, aw, ah, bx, by, bw2, bh2;
        hn_node_box(node(rt, "a"), &ax, &ay, &aw, &ah);
        hn_node_box(node(rt, "b"), &bx, &by, &bw2, &bh2);
        int fx, fy, fw, fh;
        hn_rt_key(rt, 1, HN_KT_TAB, NULL);
        hn_rt_focus_box(rt, &fx, &fy, &fw, &fh);
        ck(fx == (int)bx && fy == (int)by, "TAB 焦点轮换 a → b(焦点盒 == b 的盒)");
        hn_rt_key(rt, 1, HN_KT_PRINTABLE, "2");
        ck(!strcmp(val(rt, "b"), "2") && !strcmp(val(rt, "a"), "i\xE4\xB8\xAD"),
           "轮换后打字落进 b(\"2\"), a 保持 \"i中\" 不串控件");
        hn_rt_key(rt, 1, HN_KT_TAB, NULL);
        hn_rt_key(rt, 1, HN_KT_PRINTABLE, "3");
        ck(!strcmp(val(rt, "t"), "3"), "TAB 再轮换 → textarea(value=\"3\")");
        hn_rt_key(rt, 1, HN_KT_TAB, NULL);
        hn_rt_focus_box(rt, &fx, &fy, &fw, &fh);
        ck(fx == (int)ax && fy == (int)ay, "TAB 环形回到 a(焦点盒 == a 的盒)");
        hn_rt_key(rt, 1, HN_KT_END, NULL);   /* caret 归位末尾, 后续期望从末尾算 */
        hn_rt_key(rt, 1, HN_KT_PRINTABLE, "4");
        ck(!strcmp(val(rt, "a"), "i\xE4\xB8\xAD""4"), "回到 a 后末尾追加 \"4\"");
    }

    /* IME 预编辑直插 + commit 替换(caret 已在末尾) */
    hn_rt_ime_preedit(rt, "zhong", 5);
    ck(!strcmp(val(rt, "a"), "i\xE4\xB8\xAD""4zhong"),
       "preedit: value = 基础值 + 预编辑串(直插)");
    hn_rt_ime_preedit(rt, "zhongg", 6);
    ck(!strcmp(val(rt, "a"), "i\xE4\xB8\xAD""4zhongg"),
       "preedit 变化从基础值重算(不叠加)");
    hn_rt_ime_commit(rt, "\xE4\xB8\xAD\xE6\x96\x87");   /* 中文 */
    ck(!strcmp(val(rt, "a"), "i\xE4\xB8\xAD""4\xE4\xB8\xAD\xE6\x96\x87"),
       "commit: 基础值 + 提交串(预编辑串被替换)");
    /* 组字取消(preedit 空串) */
    hn_rt_ime_preedit(rt, "x", 1);
    hn_rt_ime_preedit(rt, "", 0);
    ck(!strcmp(val(rt, "a"), "i\xE4\xB8\xAD""4\xE4\xB8\xAD\xE6\x96\x87"),
       "preedit(\"\") = 组字取消, 值回基础值");
    /* 无组字态的 commit = 普通插入(直接字符输入经 IME 通道的场景) */
    hn_rt_ime_commit(rt, "!");
    ck(!strcmp(val(rt, "a"), "i\xE4\xB8\xAD""4\xE4\xB8\xAD\xE6\x96\x87!"),
       "无组字 commit → 普通插入");

    /* 编辑命令: COPY 不改文档; CUT 清空; SELECT_ALL 不崩; PASTE(带文本)插入 */
    {
        char before[4096];
        snprintf(before, sizeof before, "%s", val(rt, "a"));
        ck(hn_rt_edit(rt, HN_EDIT_COPY, NULL) == 0, "COPY 不改文档(返回 0)");
        ck(!strcmp(val(rt, "a"), before), "COPY 后值不变");
        ck(hn_rt_edit(rt, HN_EDIT_SELECT_ALL, NULL) == 1, "SELECT_ALL 触发重渲染");
        ck(hn_rt_edit(rt, HN_EDIT_PASTE, "PASTE") == 1, "PASTE(显式文本)在 caret 处插入");
        ck(strstr(val(rt, "a"), "PASTE") != NULL, "PASTE 后值含 \"PASTE\"");
        ck(hn_rt_edit(rt, HN_EDIT_CUT, NULL) == 1, "CUT 清空值");
        ck(!strcmp(val(rt, "a"), ""), "CUT 后值为空");
    }
    /* focus_box(IME 候选窗摆位的数据源) */
    {
        int fx = 0, fy = 0, fw = 0, fh = 0;
        ck(hn_rt_focus_box(rt, &fx, &fy, &fw, &fh) == 1 && fw > 0 && fh > 0,
           "hn_rt_focus_box 有焦点可编辑控件");
    }
    /* RETURN: input 无 hx → 0; textarea 插换行 */
    hn_rt_key(rt, 1, HN_KT_TAB, NULL);   /* 焦点到 t? 直接点 textarea 更稳 */
    hn_node_box(node(rt, "t"), &x, &y, &w, &h);
    hn_rt_button(rt, 1, x + 4, y + 4, 0);
    hn_rt_key(rt, 1, HN_KT_PRINTABLE, "L1");
    hn_rt_key(rt, 1, HN_KT_RETURN, NULL);
    hn_rt_key(rt, 1, HN_KT_PRINTABLE, "L2");
    ck(strstr(val(rt, "t"), "L1\nL2") != NULL, "textarea RETURN 插换行(\"L1\\nL2\")");
    /* disabled 控件不参与 Tab 轮换 */
    hn_rt_close(rt);

    /* 第二份文档: disabled 跳过 */
    {
        const char *h2 = "<html><body>"
            "<input id=\"a\" value=\"\"><input id=\"c\" value=\"\" disabled>"
            "<input id=\"b\" value=\"\"></body></html>";
        hn_rt_desc d2 = { "deep-edit2", h2, strlen(h2), NULL, 0, 300, 260 };
        hn_rt *rt2 = hn_rt_open(&d2);
        if (rt2) {
            float bx, by, bw, bh;
            hn_node_box(node(rt2, "a"), &bx, &by, &bw, &bh);
            hn_rt_button(rt2, 1, bx + 4, by + 4, 0);
            hn_rt_key(rt2, 1, HN_KT_TAB, NULL);        /* a → 跳过 disabled 的 c → b */
            hn_rt_key(rt2, 1, HN_KT_PRINTABLE, "B");
            ck(!strcmp(val(rt2, "b"), "B") && !strcmp(val(rt2, "c"), ""),
               "TAB 跳过 disabled 控件(a → b)");
            hn_rt_close(rt2);
        }
    }
    printf("== %s (%d 项失败) ==\n", fails ? "失败" : "全部通过", fails);
    return fails ? 1 : 0;
}
CEOF
    hn_sources_full
    HN_SOURCES=("$SBX/edit/edit_probe.c" "$HN_RT_DIR/hn_rt.c" "$HN_RT_DIR/hn_sys.c" "$HN_RT_DIR/hn_http.c"
                ${HN_SOURCES[@]+"${HN_SOURCES[@]}"})
    for f in "${HN_WASM3[@]}"; do HN_SOURCES+=("$HN_WASM3_DIR/$f"); done
    HN_CFLAGS=(-std=c99 -O2 -Wall -ffp-contract=off)
    HN_DEFS=()
    HN_INCS=(-I "$HN_ENG/include" -I "$HN_ENG" -I "$HN_RT_DIR" -I "$ROOT/platform"
             -I "$HN_WASM3_DIR" -I /opt/homebrew/include/freetype2)
    HN_LIBS=(-L/opt/homebrew/lib -lfreetype -lm)
    if hn_sources_check ${HN_SOURCES[@]+"${HN_SOURCES[@]}"} \
       && hn_probe_compile "macos_deep_probe⑤" "$SBX/edit/edit_probe"; then
        if OUT="$("$SBX/edit/edit_probe" 2>&1)"; then
            printf '%s\n' "$OUT" | sed 's/^/    /'
            ck "⑤ 打字/caret/Tab 轮换/IME 直插与 commit/编辑命令(离屏引擎侧)" 1
        else
            printf '%s\n' "$OUT" | sed 's/^/      /' | grep -E "^  X|失败" | head -10
            ck "⑤ 编辑语义(离屏)" 0
        fi
    else
        ck "⑤ 编辑探针编译(rt + 引擎 + headless)" 0
    fi
else
    echo "  (跳过 ⑤: 无 cc)"
fi

echo "----------------------------------------"
if [ "$fails" = 0 ]; then echo "macos-deep-probe: 全部通过"; else echo "macos-deep-probe: 失败 $fails 项"; fi
exit $([ "$fails" = 0 ] && echo 0 || echo 1)

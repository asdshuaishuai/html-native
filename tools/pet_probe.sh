#!/usr/bin/env bash
# pet_probe.sh — 桌宠形态(无背景 + 无边框 + 悬浮桌面)验收
#
# 四档断言, 对应"桌宠是不是真的能从引擎一路物化到窗口":
#   ① 离屏渲染: pet.html 出图成功, 且四角 alpha=0(逐像素透明口径真的生效),
#      内容非空(角色+气泡确实画出来了)。
#   ② 窗口字段(进程内, 最强): 现场编译一个探针链接真实 platform/hnp_macos.c,
#      建桌宠窗口后从 NSApp.windows 把窗口取回来逐项读回:
#        level=3(floating)/hasShadow=NO/ignoresMouseEvents=YES/
#        collectionBehavior 含 CanJoinAllSpaces|Stationary|IgnoresCycle/
#        styleMask=0/frame 逐像素等于声明/内容图非空(blit 已接通)/
#        运行期 set_click_through 两态切换 —— 以及 decsktop 级(INT32_MIN+25)
#      这是一条"声明→物化"的端到端断言, 比看截图强(截图受屏幕录制权限影响)。
#   ③ 无头平台: hnapp-macos-headless --probe 收下新清单字段(hn-level 等)
#      不崩, 布局管线正常 —— 新字段在 headless 是优雅忽略, 不是解析崩。
#   ④ 真窗口冒烟: hnapp-macos-arm64 后台起 3 秒 kill, 断言进程不崩
#      (无窗口内容断言; 观感由人肉验收, 见 README "桌宠形态")。
#
# 用法: bash tools/pet_probe.sh [dist_dir]
#   缺省 dist_dir = $ROOT/dist; 二进制名按 build-multiplatform.sh 的产物名。
#   ② 需要本机 C 编译器(cc); 无 cc 时该档跳过(其余照跑)。
set -u  # 不用 -e: 断言里要自己收 failed 计数

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
. "$ROOT/tools/_engine_sources.sh"   # hn_pngstats(现场编译的 C99 PNG 统计)
DIST="${1:-$ROOT/dist}"
HN_CORE="$DIST/hncore-macos-arm64"
HN_APP="$DIST/hnapp-macos-arm64"
HN_HEADLESS="$DIST/hnapp-macos-headless"
PET="$ROOT/examples/pet.html"

fails=0
ck() { # ck <描述> <0|1 通过>
    if [ "$2" = "1" ]; then printf '  v %s\n' "$1"
    else printf '  X %s\n' "$1"; fails=$((fails + 1)); fi
}

echo "== 桌宠形态验收 (pet_probe) =="

# ---------- ① 离屏渲染 + 四角 alpha ------
if [ ! -x "$HN_CORE" ]; then
    echo "  X 缺少 $HN_CORE (先跑 tools/build-multiplatform.sh)"; exit 1
fi
if [ ! -f "$PET" ]; then
    echo "  X 缺少 $PET"; exit 1
fi

SHOT="$(mktemp -t hnpet-shot).png"
# 资产按文档相对解析 → 在 examples/ 目录里渲染(与真窗口同口径)
# HN_CLOCK=2000: 第 1 条气泡全显 + 网格摆动到相位(确定性取帧)
if (cd "$ROOT/examples" && HN_CLOCK=2000 "$HN_CORE" render pet.html "$SHOT" 360 360 >/tmp/hn_pet_render.txt 2>&1); then
    ck "hncore render pet.html 出图" 1
else
    ck "hncore render pet.html 出图" 0
    sed 's/^/      /' /tmp/hn_pet_render.txt | head -4
fi

if [ -s "$SHOT" ]; then
    # 四角 alpha + 透明占比 + 尺寸 + 内容量(css_probe 同款逐像素口径;
    # hn-pngstats: _engine_sources.sh 现场编译的 C99 解码器, 链引擎自带的
    # hn_png.c —— 替代原 python zlib 逐行反 filters 版)
    RAW=""
    if PNGSTATS="$(hn_pngstats_build 2>/dev/null)"; then
        RAW="$("$PNGSTATS" "$SHOT" 2>/dev/null || true)"
    fi
    if [ -n "$RAW" ]; then
        REPORT="$(printf '%s\n' "$RAW" | awk '{
            w = $1; sub(/^w=/, "", w)
            h = $2; sub(/^h=/, "", h)
            c = $3; sub(/^corners=/, "", c); gsub(/,/, " ", c)
            t = $4; sub(/^trpct=/, "", t)
            ct = $6; sub(/^content=/, "", ct)
            printf "%dx%d corners=[%s] transparent=%.1f%% content=%d", w, h, c, t, ct
        }')"
        OK="$(printf '%s\n' "$REPORT" | awk '
            {
                t = $0; sub(/.*transparent=/, "", t); sub(/%.*/, "", t)
                c = $0; sub(/.*content=/, "", c)
                ok = ($0 ~ /corners=\[0 0 0 0\]/ && t + 0 > 20 && c + 0 > 4000 &&
                      $0 ~ /^360x360 /)
            }
            END { print ok ? 1 : 0 }')"
        ck "360x360 / 四角 alpha=0 / 透明占比 >20% / 内容非空 | $REPORT" "$OK"
    else
        ck "360x360 / 四角 alpha=0 / 透明占比 >20% / 内容非空" 0
    fi
else
    ck "出图非空" 0
fi
rm -f "$SHOT"

# ---------- ② 窗口字段(进程内探针, 现场编译) ----------
# 探针源码内嵌(不新增仓库文件): 链接真实 platform/hnp_macos.c, 建窗后读回属性。
if command -v cc >/dev/null 2>&1 && [ -f "$ROOT/platform/hnp_macos.c" ]; then
    TMPD="$(mktemp -d -t hnpet-fields)"
    cat > "$TMPD/pet_fields_probe.c" <<'CEOF'
/* 桌宠形态字段的进程内断言(由 tools/pet_probe.sh 现场编译)。
   链接真实 platform/hnp_macos.c; 建窗后从 NSApp.windows 取回窗口逐项读回,
   并验证 hnp_window_set_click_through 运行期切换。常量期望值出处:
   NSFloatingWindowLevel=3(CGWindowLevel.h:68)、kCGDesktopWindowLevel=
   INT32_MIN+5+20(:59-64)、CollectionBehavior 位(NSWindow.h:128-149)。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include "hn_platform.h"

typedef void *obj_t; typedef void *sel_t;
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

    /* 与 hn_app 从 manifest 映射出的桌宠 desc 一致(hn-level=floating 等) */
    hnp_window_desc d;
    memset(&d, 0, sizeof(d));
    d.title = "pet"; d.width = 360; d.height = 360;
    d.transparent = 1; d.resizable = 1;
    d.borderless = 1; d.always_on_top = 1; d.no_shadow = 1;
    d.join_all_spaces = 1; d.click_through = 1;
    hnp_window *w = hnp_window_open(&d);
    ck(w != NULL, "hnp_window_open(桌宠 desc) 成功");
    if (!w) return 1;
    obj_t win = last_window();
    ck(win != NULL, "NSApp.windows 取回窗口");
    if (!win) return 1;

    /* blit 落位: 喂一张纯色 BGRA(与 hn_app 的 blit_now 同路径) */
    {
        unsigned char *buf = (unsigned char *)malloc((size_t)360 * 360 * 4);
        if (buf) {
            for (int i = 0; i < 360 * 360; i++) {
                buf[i*4+0] = 0x60; buf[i*4+1] = 0x80; buf[i*4+2] = 0xE0; buf[i*4+3] = 0xFF;
            }
            hnp_window_blit(w, buf, 360, 360, (size_t)360 * 4);
            free(buf);
        }
        obj_t cv = M0(obj_t, win, "contentView");
        obj_t img = cv ? M0(obj_t, cv, "image") : NULL;
        ck(img != NULL, "contentView.image 非空(hnp_window_blit 路径已接通)");
        if (img) {
            CGSize isz = M0(CGSize, img, "size");
            ck(isz.w == 360.0 && isz.h == 360.0, "内容图 360x360(与 hn-window 声明一致)");
        }
    }

    long long lvl = ((long long (*)(obj_t, sel_t))ms)(win, sr("level"));
    ck(lvl == 3, "always_on_top=1 → level == NSFloatingWindowLevel(3)");
    ck(M0(int, win, "isOpaque") == 0, "transparent=1 → isOpaque == NO");
    ck(M0(int, win, "hasShadow") == 0, "no_shadow=1 → hasShadow == NO");
    ck(M0(int, win, "ignoresMouseEvents") == 1, "click_through=1 → ignoresMouseEvents == YES");
    {
        unsigned long cb = ((unsigned long (*)(obj_t, sel_t))ms)(win, sr("collectionBehavior"));
        ck((cb & 1UL) != 0, "join_all_spaces=1 → collectionBehavior 含 CanJoinAllSpaces(1<<0)");
        ck((cb & (1UL << 4)) != 0, "                       …含 Stationary(1<<4)");
        ck((cb & (1UL << 6)) != 0, "                       …含 IgnoresCycle(1<<6)");
    }
    {
        unsigned long sm = ((unsigned long (*)(obj_t, sel_t))ms)(win, sr("styleMask"));
        ck(sm == 0, "borderless=1 → styleMask == 0(无 Titled/Closable/无 resize 边)");
        CGRect fr = ((CGRect (*)(obj_t, sel_t))ms)(win, sr("frame"));
        ck(fr.size.w == 360.0 && fr.size.h == 360.0, "NSWindow.frame 逐像素 == 360x360");
    }
    ck(M0(int, win, "isMovableByWindowBackground") == 1,
       "borderless=1 → movableByWindowBackground == YES(整窗可拖)");
    ck(M0(int, win, "isMovable") == 1, "窗口 isMovable == YES(服务端拖动前提)");
    {
        obj_t cv = M0(obj_t, win, "contentView");
        ck(cv && M0(int, cv, "mouseDownCanMoveWindow") == 1,
           "contentView.mouseDownCanMoveWindow == YES(覆盖 NSImageView 默认 0)");
    }

    /* 运行期穿透两态(幽灵模式) */
    hnp_window_set_click_through(w, 0);
    ck(M0(int, win, "ignoresMouseEvents") == 0, "set_click_through(w,0) → 恢复收事件");
    hnp_window_set_click_through(w, 1);
    ck(M0(int, win, "ignoresMouseEvents") == 1, "set_click_through(w,1) → 穿透");
    hnp_window_set_click_through(NULL, 1);
    ck(1, "set_click_through(NULL,1) 不崩");
    hnp_window_close(w);

    /* desktop 级(第二档): kCGDesktopWindowLevel = INT32_MIN+25 */
    {
        hnp_window_desc d2;
        memset(&d2, 0, sizeof(d2));
        d2.width = 200; d2.height = 120; d2.borderless = 1; d2.always_on_top = 2;
        hnp_window *w2 = hnp_window_open(&d2);
        ck(w2 != NULL, "desktop 级窗口创建");
        if (w2) {
            obj_t win2 = last_window();
            long long lvl2 = win2 ? ((long long (*)(obj_t, sel_t))ms)(win2, sr("level")) : 0;
            ck(lvl2 == -2147483648LL + 25,
               "always_on_top=2 → level == kCGDesktopWindowLevel(INT32_MIN+25)");
            hnp_window_close(w2);
        }
    }
    hnp_shutdown();
    printf("== %s (%d 项失败) ==\n", fails ? "失败" : "全部通过", fails);
    return fails ? 1 : 0;
}
CEOF
    if cc -O2 -std=c99 -I "$ROOT/platform" "$TMPD/pet_fields_probe.c" \
          "$ROOT/platform/hnp_macos.c" -o "$TMPD/pet_fields_probe" -ldl 2>"$TMPD/build.log"; then
        if "$TMPD/pet_fields_probe" | sed 's/^/    /'; then
            ck "窗口字段进程内断言(level/shadow/穿透/collectionBehavior/拖动)" 1
        else
            ck "窗口字段进程内断言(level/shadow/穿透/collectionBehavior/拖动)" 0
        fi
    else
        ck "窗口字段探针编译(cc + platform/hnp_macos.c)" 0
        sed 's/^/      /' "$TMPD/build.log" | head -5
    fi
    rm -rf "$TMPD"
else
    echo "  (跳过窗口字段探针: 无 cc 或 platform/hnp_macos.c 不在)"
fi

# ---------- ③ 无头平台收下新字段不崩 ------
if [ -x "$HN_HEADLESS" ]; then
    if OUT=$("$HN_HEADLESS" "$PET" --probe 2>&1); then
        echo "$OUT" | grep -q "^platform=headless" && ck "headless --probe 正常输出" 1 || ck "headless --probe 正常输出" 0
        echo "$OUT" | grep -q "probe OK" && ck "headless 收下 hn-level/frameless 等新 meta 不崩" 1 || ck "headless 收下新 meta 不崩" 0
    else
        ck "headless --probe 退出码 0" 0
    fi
else
    echo "  (跳过 headless: 缺 $HN_HEADLESS)"
fi

# ---------- ④ 真窗口冒烟(3 秒 kill, 不崩) ----------
if [ -x "$HN_APP" ]; then
    LOG="$(mktemp -t hnpet-win).log"
    "$HN_APP" "$PET" >"$LOG" 2>&1 &
    PID=$!
    sleep 3
    if kill -0 "$PID" 2>/dev/null; then
        kill "$PID" 2>/dev/null
        wait "$PID" 2>/dev/null
        ck "真窗口起 3 秒仍存活(未崩)" 1
    else
        wait "$PID" 2>/dev/null
        rc=$?
        ck "真窗口起 3 秒仍存活(未崩; 提前退出 rc=$rc)" 0
        sed 's/^/      /' "$LOG" | head -6
    fi
    rm -f "$LOG"
else
    echo "  (跳过真窗口: 缺 $HN_APP)"
fi

echo "== $([ $fails -eq 0 ] && echo '全部通过' || echo "失败 $fails 项") =="
exit $([ $fails -eq 0 ] && echo 0 || echo 1)

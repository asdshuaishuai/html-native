/* hnp_macos.c — macOS 平台实现(纯 C99: dlopen AppKit + objc_msgSend)
 *
 * hn_platform.h 的 macOS 实现。零 ObjC 头文件 / 零 .m / 零 Swift;
 * 连链接期都不需要 framework(全部 dlsym)。
 * 事件常量逐值取自 SDK 的 NSEvent.h(NSEventTypeLeftMouseDown=1 等) ——
 * 不要凭记忆写(旧版枚举混写曾让 KitDefined 落进 keydown 分支崩溃)。
 * 窗口层级/collectionBehavior 常量同样逐值抄自 SDK(CGWindowLevel.h /
 * NSWindow.h, 见下方宏注释): 既有 int 又是 NSUInteger 位集, 凭记忆写
 * 会静默错位。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <stdint.h>
#include <time.h>

#include "hn_platform.h"

typedef void *obj_t;
typedef void *sel_t;

static obj_t (*o_getClass)(const char *);
static sel_t (*o_sel)(const char *);
static obj_t (*o_msgSend)(obj_t, sel_t, ...);

#define MSG0(RET, O, NAME) ((RET (*)(obj_t, sel_t))o_msgSend)((O), o_sel(NAME))
#define MSG1(RET, O, NAME, A1) \
    ((RET (*)(obj_t, sel_t, obj_t))o_msgSend)((O), o_sel(NAME), (obj_t)(A1))
#define MSGI(RET, O, NAME, I) ((RET (*)(obj_t, sel_t, int))o_msgSend)((O), o_sel(NAME), (I))
/* NSInteger/NSUInteger 参数(setLevel: 收 NSInteger, setCollectionBehavior: 收
   NSUInteger)。不能用 MSGI: 变参 ABI 下窄整数的寄存器高位是未定义的,
   负层级(desktop 级在 INT32_MIN 附近)会被读错 —— 必须按 64 位传。 */
#define MSGL(RET, O, NAME, L) \
    ((RET (*)(obj_t, sel_t, long))o_msgSend)((O), o_sel(NAME), (long)(L))
#define MSGUL(RET, O, NAME, UL) \
    ((RET (*)(obj_t, sel_t, unsigned long))o_msgSend)((O), o_sel(NAME), (unsigned long)(UL))

typedef struct { double x, y; } CGPoint;
typedef struct { double w, h; } CGSize;
typedef struct { CGPoint origin; CGSize size; } CGRect;
typedef struct CGImage *CGImageRef;
typedef struct CGDataProvider *CGDataProviderRef;
typedef struct CGColorSpace *CGColorSpaceRef;

/* NSEventType(取自 NSEvent.h:25-43, 见文件头注释) */
#define EV_LDOWN 1
#define EV_LUP 2
#define EV_RDOWN 3
#define EV_MOVE 5
#define EV_LDRAG 6
#define EV_KEYDOWN 10
#define EV_KEYUP 11
#define EV_FLAGS 12
#define EV_APPKITDEF 13
#define EV_SYSTEMDEF 14
#define EV_APPDEF 15
#define EV_PERIODIC 16
#define EV_SCROLL 22

/* ---------------- 桌宠窗口形态常量(逐值抄自 SDK 头, 不凭记忆) ----------------

   NSWindowLevel(NSWindow.h:192 `typedef NSInteger NSWindowLevel`)与
   CGWindowLevel(CGWindowLevel.h:44 `typedef int32_t CGWindowLevel`)同一套值:
     NSNormalWindowLevel   = kCGNormalWindowLevel   = 0    (CGWindowLevel.h:67)
     NSFloatingWindowLevel = kCGFloatingWindowLevel = 3    (CGWindowLevel.h:68)
     "desktop" 级没有 NS 名, 用 CG 宏:
     kCGDesktopWindowLevel = kCGMinimumWindowLevel + 20    (CGWindowLevel.h:64)
     其中 kCGMinimumWindowLevel = kCGBaseWindowLevel + kCGNumReservedBaseWindowLevels
                               = INT32_MIN + 5              (CGWindowLevel.h:59-60)
   即 desktop 级 = INT32_MIN + 25 —— 一个很大的负数, 这正是必须按 NSInteger
   64 位传参的理由(见 MSGL 注释)。 */
#define HN_NS_FLOATING_LEVEL 3
#define HN_NS_DESKTOP_LEVEL  ((long)-2147483648L + 5 + 20)   /* INT32_MIN + 25 */

/* NSWindowCollectionBehavior(NSWindow.h:128-149, 2026-10 抄录):
     CanJoinAllSpaces = 1<<0 (:130)  跟随所有空间(含全屏空间旁边)
     MoveToActiveSpace= 1<<1 (:131)  切空间时搬到当前活动空间
     Managed          = 1<<2 (:133)  参与 spaces/exposé(normal 级默认)
     Transient        = 1<<3 (:134)  在 spaces 中浮动, 被 exposé 隐藏
     Stationary       = 1<<4 (:135)  不被 exposé 带走 —— 桌面窗口语义
     IgnoresCycle     = 1<<6 (:138)  不进 Cmd+` 循环(浮动级默认)
   桌宠组合 = CanJoinAllSpaces|Stationary|IgnoresCycle:
     跟所有空间(所有桌面都看得见) + 不随 Mission Control 位移 + 不参与窗口循环。
   注: CanJoinAllApplications(1<<18, :149)是 macOS 13+ 才有的 beat,
   不开(留 ≤12 可运行)。 */
#define HN_CB_CAN_JOIN_ALL_SPACES (1UL << 0)
#define HN_CB_STATIONARY          (1UL << 4)
#define HN_CB_IGNORES_CYCLE       (1UL << 6)
#define HN_CB_PET (HN_CB_CAN_JOIN_ALL_SPACES | HN_CB_STATIONARY | HN_CB_IGNORES_CYCLE)

static obj_t nsstr(const char *s) {
    return ((obj_t (*)(obj_t, sel_t, const char *))o_msgSend)(
        o_getClass("NSString"), o_sel("stringWithUTF8String:"), s);
}

/* 有界 autorelease 池(与 hnp_media_macos.c 的 media_pump 同款手法)。
   事故②(2026-10 真窗 media-demo 24.5GB 假死)的根因实锤: 本壳手撸 MRC +
   自管事件循环, 主循环**从来没有 NSAutoreleasePool** —— AppKit/CA 内部
   大量按 autorelease 归还的对象(图层背衬、CA 事务、NSImage 拷贝)落不进
   任何池, macOS 运行时对无池 autorelease 的处置是直接泄漏。静态页不
   blit 时 RSS 纹丝不动, pet.html(30fps)与 media-demo(60fps)同速上涨
   (实测 22-26MB/s, vmmap 脏页全堆在 CoreAnimation 段, malloc 堆干净)
   —— 即"每 blit 一次泄一帧背衬"。媒体只是让 blit 连续化, 元凶在壳。 */
static obj_t pool_push(void) {
    obj_t cls = o_getClass("NSAutoreleasePool");
    obj_t o = cls ? MSG0(obj_t, cls, "alloc") : NULL;
    return o ? MSG0(obj_t, o, "init") : NULL;
}
static void pool_pop(obj_t pool) {
    if (pool) MSG0(void, pool, "drain");
}

/* 主屏(frame)高度: AppKit 全局坐标原点在主屏左下, 与引擎的左上原点
   口径互翻时用它(y_top = 主屏高 − y_appkit − h)。取 NSScreen.screens[0]
   (= 带菜单栏的主屏; mainScreen 只是"有键盘焦点的屏", 多屏时不一定主)。
   取不到时回退 mainScreen —— 单屏环境两者相同。 */
static double hn_primary_screen_height(void) {
    obj_t arr = MSG0(obj_t, o_getClass("NSScreen"), "screens");
    obj_t s = arr ? ((obj_t (*)(obj_t, sel_t, int))o_msgSend)(
        arr, o_sel("objectAtIndex:"), 0) : NULL;
    if (!s) s = MSG0(obj_t, o_getClass("NSScreen"), "mainScreen");
    if (!s) return 0;
    CGRect f = MSG0(CGRect, s, "frame");
    return f.size.h;
}

int hnp_init(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_NOW)) return 0;
    dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_NOW);
    /* QuartzCore(CALayer/CATransaction): AppKit 依赖链里本就有, 显式装载
       供视频图层段用(o_getClass("CATransaction") 的 flush 提交)。缺席不致命
       —— 图层段对类缺失逐点优雅降级。 */
    dlopen("/System/Library/Frameworks/QuartzCore.framework/QuartzCore", RTLD_NOW);
    o_getClass = (obj_t (*)(const char *))dlsym(RTLD_DEFAULT, "objc_getClass");
    o_sel = (sel_t (*)(const char *))dlsym(RTLD_DEFAULT, "sel_registerName");
    o_msgSend = (obj_t (*)(obj_t, sel_t, ...))dlsym(RTLD_DEFAULT, "objc_msgSend");
    if (!o_getClass || !o_sel || !o_msgSend) return 0;
    obj_t app = MSG0(obj_t, o_getClass("NSApplication"), "sharedApplication");
    if (!app) return 0;
    MSG0(void, app, "finishLaunching");
    MSGI(void, app, "setActivationPolicy:", 1);   /* accessory: 不占 Dock */
    return 1;
}

void hnp_shutdown(void) {}
const char *hnp_name(void) { return "macos"; }

struct hnp_window {
    obj_t win, iv, img;
    int w, h, transparent;
    int layered;   /* contentView 已 setWantsLayer:YES(视频图层挂载时置位) */
};

/* ---- 原生视频图层: 与媒体后端的对接口 + (win, id) → 子图层 表 ----
 * (实现与坐标/提交细节见文件尾"原生视频图层"段; 表声明在此 ——
 * hnp_window_close 的摘层清理要用。) */

/* 媒体后端查询口(定义在 platform/hnp_media_macos.c)。**弱定义桩**:
   本文件会被 pet_probe/hidpi_probe **单独**链接(不经 hnapp 目标, 无媒体
   后端) —— 那里落空到本文件的弱桩(NULL/无操作), 图层 API 如实返回
   "不支持", 探针照常编译运行; hnapp 目标里媒体后端的**强定义**覆盖弱桩,
   图层查询直达注册表。(weak_import/弱引用声明在 ld64 静态链接下仍报
   未定义, 实测; 弱定义+强定义覆盖是 Mach-O/ELF 通吃的口径。) */
#if defined(__GNUC__)
__attribute__((weak))
#endif
void *hnp_media_player_for_src(const char *src) {
    (void)src;
    return NULL;
}
#if defined(__GNUC__)
__attribute__((weak))
#endif
void hnp_media_set_layer_mode(const char *src, int on) {
    (void)src; (void)on;
}

/* 进程窗口数少(hn_app 一进程一窗), 定长表足够; 满表时 attach 返回 0
   (该会话优雅退 CPU 逐帧)。lx..lh = 最近应用到图层的 frame(同帧跳过
   setFrame + CA flush 的白耗)。 */
#define HN_VL_CAP 8
static struct {
    hnp_window *win; char id[256]; obj_t layer;
    float lx, ly, lw, lh;
} g_vl[HN_VL_CAP];
static int g_vl_n;
static int g_vl_ready;   /* 首帧可呈现诊断已打过(进程一次) */

static int vl_find(hnp_window *w, const char *id) {
    for (int i = 0; i < g_vl_n; i++)
        if (g_vl[i].win == w && strcmp(g_vl[i].id, id) == 0) return i;
    return -1;
}

/* 诊断行开关(与引擎媒体 trace 同一环境钮; 记忆化) */
static int vl_trace(void) {
    static int t = -1;
    if (t < 0) {
        const char *e = getenv("HN_MEDIA_TRACE");
        t = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return t;
}

/* 可打印字符的缓冲(单事件生命周期, 调用方在下次 pump 前消费) */
static char g_utf8[8];

/* 可拖动的内容视图子类: NSImageView.mouseDownCanMoveWindow 实测返回 0
   (/tmp 探针实测: NSView 默认为 1, NSImageView 为 0), 拖动落在图上即失效
   —— 桌宠整窗都是内容, 没有可拖的空白。运行期用 objc_allocateClassPair +
   class_addMethod 建一个覆写该方法的子类(实测返回 1)。延迟注册一次,
   供所有窗口复用; 运行时无这些符号(理论不可能, 同进程必有 libobjc)时返回
   NULL, 调用方回退到原始 NSImageView(退化为"不可拖")。 */
static signed char hn_view_can_move_window(obj_t self, sel_t cmd) {
    (void)self; (void)cmd;
    return 1;   /* 任意位置按下都可拖动窗口 */
}

static obj_t hn_pet_view_class(void) {
    static obj_t cls;
    if (cls) return cls;
    obj_t (*alloc_pair)(obj_t, const char *, size_t) =
        (obj_t (*)(obj_t, const char *, size_t))dlsym(RTLD_DEFAULT, "objc_allocateClassPair");
    signed char (*add_method)(obj_t, sel_t, void (*)(void), const char *) =
        (signed char (*)(obj_t, sel_t, void (*)(void), const char *))dlsym(RTLD_DEFAULT, "class_addMethod");
    void (*reg_pair)(obj_t) = (void (*)(obj_t))dlsym(RTLD_DEFAULT, "objc_registerClassPair");
    if (!alloc_pair || !add_method || !reg_pair) return NULL;
    cls = alloc_pair(o_getClass("NSImageView"), "HNPImageView", 0);
    if (!cls) return NULL;
    /* 类型编码 "c@:" = BOOL 返回, self, _cmd(与 BOOL 的一致编码) */
    if (!add_method(cls, o_sel("mouseDownCanMoveWindow"),
                    (void (*)(void))hn_view_can_move_window, "c@:")) {
        cls = NULL;
        return NULL;
    }
    reg_pair(cls);
    return cls;
}

hnp_window *hnp_window_open(const hnp_window_desc *d) {
    if (!d) return NULL;
    hnp_window *w = (hnp_window *)calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->w = d->width; w->h = d->height; w->transparent = d->transparent;

    /* styleMask(NSWindow.h:55-63):
         Titled=1<<0 Closable=1<<1 Miniaturizable=1<<2 Resizable=1<<3
       现状: 之前透明的窗口被顺手做成 style=0(无边框)——"透明"其实
       隐含了"无标题栏", 但两者是独立的意图(可以有标题栏的透明窗口)。
       桌宠形态把它拆开: borderless 显式声明无边框; transparent 只管
       isOpaque/底色。两者都不声明时保持常规(可调尺寸)窗口。

       注意 borderless 必须 style=0, 不能只留 Resizable: 只带 Resizable
       的无标题窗口会被 AppKit 装上一圈**可拖拽的 resize 边**(实测
       CGWindowList 报的尺寸比声明的少 8px —— 360x360 的桌宠变成 352x352
       内容区 + 4px 死边框)。桌宠的边界必须在文档里看不见, 尺寸变化交给
       文档/运行时(--shot 尺寸、热更新的 hn-window 改写), 不由拖边触发。 */
    unsigned long style = d->resizable ? (1 | 2 | 4 | 8) : (1 | 2 | 4);
    if (d->borderless) style = 0;
    else if (d->transparent) style = 0;   /* 兼容既有: 透明窗口保持无边框 */
    CGRect r = { { 0, 0 }, { (double)d->width, (double)d->height } };
    obj_t win = MSG0(obj_t, o_getClass("NSWindow"), "alloc");
    win = ((obj_t (*)(obj_t, sel_t, CGRect, unsigned long, unsigned long, int))o_msgSend)(
        win, o_sel("initWithContentRect:styleMask:backing:defer:"), r, style, 2, 0);
    if (!win) { free(w); return NULL; }
    MSG1(void, win, "setTitle:", nsstr(d->title ? d->title : "hn"));
    if (d->transparent) {
        MSGI(void, win, "setOpaque:", 0);
        MSG1(void, win, "setBackgroundColor:", MSG0(obj_t, o_getClass("NSColor"), "clearColor"));
    }

    /* ---- 桌宠形态(见 hn_platform.h 的字段注释) ---- */
    /* 层级: floating=NSFloatingWindowLevel(3) / desktop=kCGDesktopWindowLevel
       (INT32_MIN+25)。按 NSInteger 传(MSGL) —— 桌面级是负数, 走 int
       变参在高位未定义时会被读错。 */
    if (d->always_on_top == 1) MSGL(void, win, "setLevel:", HN_NS_FLOATING_LEVEL);
    else if (d->always_on_top == 2) MSGL(void, win, "setLevel:", HN_NS_DESKTOP_LEVEL);
    /* 跨空间: CanJoinAllSpaces|Stationary|IgnoresCycle(值见文件头宏注释) */
    if (d->join_all_spaces) MSGUL(void, win, "setCollectionBehavior:", HN_CB_PET);
    if (d->no_shadow) MSGI(void, win, "setHasShadow:", 0);
    /* 鼠标穿透: 幽灵模式初始态(运行期切换见 hnp_window_set_click_through) */
    if (d->click_through) MSGI(void, win, "setIgnoresMouseEvents:", 1);
    /* 无边框 + 可拖: 桌宠没有标题栏可拖, 内容就是唯一拖动面。
       movableByWindowBackground(NSWindow.h:414) 对 borderless 窗口生效;
       内容视图需 mouseDownCanMoveWindow=YES(NSView 默认为 1, 但
       NSImageView 实测为 0 —— 见 hn_pet_view_class)。 */
    if (d->borderless) MSGI(void, win, "setMovableByWindowBackground:", 1);

    obj_t cls = hn_pet_view_class();
    obj_t iv = cls ? MSG0(obj_t, cls, "alloc") : MSG0(obj_t, o_getClass("NSImageView"), "alloc");
    if (!iv) iv = MSG0(obj_t, o_getClass("NSImageView"), "alloc");
    CGRect cr = { { 0, 0 }, { (double)d->width, (double)d->height } };
    iv = ((obj_t (*)(obj_t, sel_t, CGRect))o_msgSend)(iv, o_sel("initWithFrame:"), cr);
    MSG1(void, win, "setContentView:", iv);
    /* 主屏可见区中央; 几何记忆命中(desc.x/y 非 AUTO)则摆回记忆位置 ——
       左上原点 → AppKit 左下原点按主屏高翻 y。 */
    obj_t screen = MSG0(obj_t, o_getClass("NSScreen"), "mainScreen");
    CGRect sf = MSG0(CGRect, screen, "visibleFrame");
    CGRect wf = MSG0(CGRect, win, "frame");
    CGRect pos = { { sf.origin.x + (sf.size.w - wf.size.w) / 2,
                     sf.origin.y + (sf.size.h - wf.size.h) / 2 },
                   { wf.size.w, wf.size.h } };
    if (d->x != HNP_WINDOW_POS_AUTO || d->y != HNP_WINDOW_POS_AUTO) {
        pos.origin.x = (double)d->x;
        pos.origin.y = hn_primary_screen_height() - (double)d->y - pos.size.h;
    }
    ((void (*)(obj_t, sel_t, CGRect, int))o_msgSend)(win, o_sel("setFrame:display:"), pos, 1);
    MSG1(void, win, "makeKeyAndOrderFront:", (obj_t)1);
    MSG1(void, MSG0(obj_t, o_getClass("NSApplication"), "sharedApplication"),
         "activateIgnoringOtherApps:", (obj_t)1);
    w->win = win; w->iv = iv;
    return w;
}

void hnp_window_close(hnp_window *w) {
    if (!w) return;
    /* 该窗的视频图层先摘(图层强引用着 AVPlayer, 不摘会拖住已 close 的媒体);
       同时撤帧早退标志 —— 否则比窗活得久的会话会陷入"无图层又无 CPU 帧" */
    for (int i = 0; i < g_vl_n; i++) {
        if (g_vl[i].win == w) {
            if (g_vl[i].layer) MSG0(void, g_vl[i].layer, "release");
            hnp_media_set_layer_mode(g_vl[i].id, 0);
            g_vl[i] = g_vl[g_vl_n - 1];
            g_vl_n--;
            i--;
        }
    }
    if (w->img) MSG0(void, w->img, "release");
    if (w->win) { MSG0(void, w->win, "close"); MSG0(void, w->win, "release"); }
    free(w);
}

void hnp_window_resize(hnp_window *w, int nw, int nh) {
    if (!w || !w->iv) return;
    w->w = nw; w->h = nh;
    CGRect cr = { { 0, 0 }, { (double)nw, (double)nh } };
    ((void (*)(obj_t, sel_t, CGRect))o_msgSend)(w->iv, o_sel("setFrame:"), cr);
}

void hnp_window_blit(hnp_window *w, const unsigned char *bgra_pre, int bw, int bh, size_t stride) {
    if (!w || !w->win || !bgra_pre) return;
    /* 事故②修复挂点: blit 全程套有界池(setImage:/display 触发的 AppKit/
       CA 内部 autorelease 对象由此回收), 见 pool_push 注释。 */
    obj_t pool = pool_push();
    /* DeviceRGB 是不可变全局对象: 复用一份, 不再每次 blit 泄一个
       CGColorSpaceRef(旧代码 Create 后无人 Release)。 */
    static CGColorSpaceRef g_cs;
    if (!g_cs)
        g_cs = ((CGColorSpaceRef (*)(void))dlsym(RTLD_DEFAULT, "CGColorSpaceCreateDeviceRGB"))();
    CGDataProviderRef prov = ((CGDataProviderRef (*)(void *, const void *, size_t, void (*)(void *, const void *, size_t)))
        dlsym(RTLD_DEFAULT, "CGDataProviderCreateWithData"))(NULL, bgra_pre, (size_t)bw * bh * 4, NULL);
    /* stride 此处要求 == w*4(hn_rt 保证); kCGImageAlphaPremultipliedFirst|kCGBitmapByteOrder32Little */
    CGImageRef cg = ((CGImageRef (*)(size_t, size_t, size_t, size_t, size_t, CGColorSpaceRef,
                                     uint32_t, CGDataProviderRef, const double *, int, int))
        dlsym(RTLD_DEFAULT, "CGImageCreate"))(
        (size_t)bw, (size_t)bh, 8, 32, stride, g_cs, 2u | 8192u, prov, NULL, 0, 0);
    if (cg) {
        obj_t nsimg = MSG0(obj_t, o_getClass("NSImage"), "alloc");
        /* HiDPI 口径(真窗 2x 渲染的关键一环): NSImage 的**点尺寸**取窗口内容区
           逻辑尺寸, 位图像素照实(bw×bh 可为逻辑的 2 倍) —— AppKit 在 Retina 上
           把 720x720 物理像素画进 360x360 点的视图 = 1:1 物理像素, 不经系统
           放大(1x 屏 bw==w->w, 与旧版行为逐位一致)。拿不到逻辑尺寸时回退
           位图尺寸(探针喂任意位图的防御)。 */
        int lw = w->w > 0 ? w->w : bw;
        int lh = w->h > 0 ? w->h : bh;
        CGSize sz = { (double)lw, (double)lh };
        nsimg = ((obj_t (*)(obj_t, sel_t, CGImageRef, CGSize))o_msgSend)(
            nsimg, o_sel("initWithCGImage:size:"), cg, sz);
        MSG1(void, w->iv, "setImage:", nsimg);
        if (w->img) MSG0(void, w->img, "release");
        w->img = nsimg;
        ((void (*)(CGImageRef))dlsym(RTLD_DEFAULT, "CGImageRelease"))(cg);
    }
    MSG0(void, (obj_t)prov, "release");
    MSG0(void, w->win, "display");
    pool_pop(pool);
}

void hnp_window_invalidate(hnp_window *w) { if (w && w->win) MSG0(void, w->win, "display"); }

void hnp_window_set_title(hnp_window *w, const char *utf8) {
    if (w && w->win) MSG1(void, w->win, "setTitle:", nsstr(utf8 ? utf8 : "hn"));
}

/* 运行期鼠标穿透切换(桌宠幽灵模式)。setIgnoresMouseEvents: 立即生效,
   无需重开窗口; 穿透中窗口收不到任何鼠标事件(点击落到桌面/下层窗口),
   切回 0 恢复。 */
void hnp_window_set_click_through(hnp_window *w, int on) {
    if (w && w->win) MSGI(void, w->win, "setIgnoresMouseEvents:", on ? 1 : 0);
}

/* 几何记忆采样口: 读 NSWindow frame origin(AppKit 全局坐标, 主屏左下
   原点), 按主屏高翻成引擎口径(左上原点, y 向下)。movableByWindowBackground
   的桌宠拖动会实时反映在 frame 上 —— hn_app 2s 轮询这里, 变了就落盘。 */
static int hn_round_d(double v) {
    return (int)(v + (v < 0 ? -0.5 : 0.5));
}

int hnp_window_origin(hnp_window *w, int *x, int *y) {
    if (!w || !w->win || !x || !y) return 0;
    CGRect f = MSG0(CGRect, w->win, "frame");
    *x = hn_round_d(f.origin.x);
    *y = hn_round_d(hn_primary_screen_height() - f.origin.y - f.size.h);
    return 1;
}

/* 设备像素比: NSWindow.backingScaleFactor(10.7+; 返回 CGFloat=double,
   arm64 走 d0 寄存器 —— 必须按 double 收, 按 float 收会读到垃圾)。
   Retina = 2.0; 普通屏 = 1.0。NaN/异常值(<1)钳回 1.0 —— HiDPI 拿不到
   就退 1x, 行为与旧版逐位一致(离屏/shot 的 1x 锚不依赖本函数)。 */
float hnp_window_scale(hnp_window *w) {
    if (!w || !w->win) return 1.0f;
    double s = ((double (*)(obj_t, sel_t))o_msgSend)(w->win, o_sel("backingScaleFactor"));
    if (!(s >= 1.0)) s = 1.0;   /* NaN/0/负 → 1x */
    return (float)s;
}

/* ---------------- 原生视频图层(AVPlayerLayer GPU 合成直通) ----------------
 * "GPU 分担渲染"第一击: 真窗 <video> 不再把解码帧拉回 CPU(NV12/BGRA→RGBA
 * 转换 → HN_CMD_BITMAP 软件光栅), 而是 AVPlayer 解码、AVPlayerLayer 经系统
 * 合成器(WindowServer)直接合成进窗口 —— 像素不过 CPU。
 *
 * 与媒体后端的对接(platform/hnp_media_macos.c): player 存在该文件的会话里,
 * 经上方两个**弱定义查询口**拿(无媒体后端的链接里落空到弱桩)。
 *
 * 坐标口径: 调用方给引擎盒(逻辑 px, 左上原点); NSImageView 缺省不翻转
 * (isFlipped=NO), 其 layer 的子层坐标系原点在左下 —— y 按内容区高度翻转。
 * 提交时机: 本壳自管事件循环, CA 事务不随 runloop 自动 flush —— 每次图层
 * 变更后显式 [CATransaction flush], 帧位置/播放画面立即上屏(真窗手动验收
 * "拖动窗口/滚动页面盒子跟随"的确定性挂点)。 */

/* CATransaction flush(QuartzCore; 类缺失时静默 —— 降级为依赖 runloop 提交) */
static void vl_flush(void) {
    obj_t cls = o_getClass("CATransaction");
    if (cls) MSG0(void, cls, "flush");
}

/* 引擎盒(左上原点) → 图层 frame(左下原点): layer_y = H − (y + fh)。
   尺寸非正的盒子直接跳过(元素还没布局/被布局成 0)。 */
static void vl_apply_frame(obj_t layer, hnp_window *w,
                           float x, float y, float fw, float fh) {
    if (fw <= 0 || fh <= 0) return;
    CGRect f = { { (double)x, (double)w->h - ((double)y + (double)fh) },
                 { (double)fw, (double)fh } };
    ((void (*)(obj_t, sel_t, CGRect))o_msgSend)(layer, o_sel("setFrame:"), f);
}

int hnp_video_layer_attach(hnp_window *w, const char *player_id,
                           float x, float y, float fw, float fh) {
    if (!w || !w->win || !w->iv || !player_id || !*player_id) return 0;
    /* 弱符号缺席(本文件被探针单独链接、无媒体后端): 图层不支持 */
    if (!hnp_media_player_for_src || !hnp_media_set_layer_mode) return 0;
    obj_t player = hnp_media_player_for_src(player_id);
    if (!player) return 0;   /* 会话未 open/open 失败: 保持 CPU/占位路径 */
    obj_t pool = pool_push();
    int ok = 0;
    /* contentView 图层化(一次): AVPlayerLayer 必须挂在 CALayer 树上。
       NSImageView 进 layer-backed 模式后 setImage: 的呈现走同一棵层树,
       位图内容在自身 backing, 子层(AVPlayerLayer)叠加其上。 */
    if (!w->layered) {
        MSGI(void, w->iv, "setWantsLayer:", 1);
        w->layered = 1;
    }
    obj_t root = MSG0(obj_t, w->iv, "layer");
    if (root) {
        int i = vl_find(w, player_id);
        if (i < 0 && g_vl_n < HN_VL_CAP) {
            /* AVPlayerLayer +layer(类方法, autoreleased)→ setPlayer → 挂树。
               resize = "resize" 拉伸铺满: 与 CPU 路径的位图拉伸语义一致
               (AVLayerVideoGravityResize; 缺省 aspect-fit 会信箱化, 观感
               与引擎路径不同)。 */
            obj_t cls = o_getClass("AVPlayerLayer");
            obj_t lay = cls ? MSG0(obj_t, cls, "layer") : NULL;
            if (lay) {
                MSG1(void, lay, "setPlayer:", player);
                MSG1(void, lay, "setVideoGravity:", nsstr("resize"));
                MSG1(void, root, "addSublayer:", lay);
                MSG0(void, lay, "retain");   /* 表持有, detach 时 release 配平 */
                i = g_vl_n++;
                g_vl[i].win = w;
                snprintf(g_vl[i].id, sizeof(g_vl[i].id), "%s", player_id);
                g_vl[i].layer = lay;
            }
        } else if (i >= 0) {
            /* 已挂(热重载同 src 重开很常见): player 换血(旧会话已 close,
               层里还引用着已释放的 AVPlayer —— 换成注册表里的新实例)。 */
            MSG1(void, g_vl[i].layer, "setPlayer:", player);
        }
        if (i >= 0) {
            vl_apply_frame(g_vl[i].layer, w, x, y, fw, fh);
            g_vl[i].lx = x; g_vl[i].ly = y; g_vl[i].lw = fw; g_vl[i].lh = fh;
            hnp_media_set_layer_mode(player_id, 1);   /* 帧不再外送(置早退) */
            vl_flush();
            if (vl_trace())
                fprintf(stderr, "[video-layer] attach id=%s box=%.0f,%.0f %.0fx%.0f\n",
                        player_id, (double)x, (double)y, (double)fw, (double)fh);
            ok = 1;
        }
    }
    pool_pop(pool);
    return ok;
}

void hnp_video_layer_frame(hnp_window *w, const char *player_id,
                           float x, float y, float fw, float fh) {
    if (!w || !player_id) return;
    int i = vl_find(w, player_id);
    if (i < 0) return;   /* 未 attach 过: 静默(调用方时序差异的防御) */
    /* 内容就绪诊断(一次性): AVPlayerLayer.isReadyForDisplay=YES = 第一帧
       视频已可呈现(图层上真的有画面 —— 无截屏权限环境下的验收代理信号)。
       就绪前每拍读一次(便宜的布尔查询), 翻 YES 后永久停。 */
    if (!g_vl_ready) {
        signed char ready = ((signed char (*)(obj_t, sel_t))o_msgSend)(
            g_vl[i].layer, o_sel("isReadyForDisplay"));
        if (ready && vl_trace()) {
            g_vl_ready = 1;
            fprintf(stderr, "[video-layer] ready id=%s(首帧可呈现)\n", player_id);
        } else if (ready) {
            g_vl_ready = 1;
        }
    }
    /* 同帧跳过: 盒没变就不动图层(setFrame + CA flush 每拍白打 60 次是
       纯浪费; 未变时画面由系统合成器自行推进) */
    if (g_vl[i].lx == x && g_vl[i].ly == y && g_vl[i].lw == fw && g_vl[i].lh == fh)
        return;
    g_vl[i].lx = x; g_vl[i].ly = y; g_vl[i].lw = fw; g_vl[i].lh = fh;
    obj_t pool = pool_push();
    vl_apply_frame(g_vl[i].layer, w, x, y, fw, fh);
    vl_flush();
    pool_pop(pool);
}

void hnp_video_layer_detach(hnp_window *w, const char *player_id) {
    if (!w || !player_id) return;
    int i = vl_find(w, player_id);
    if (i < 0) return;   /* 幂等 */
    obj_t pool = pool_push();
    MSG0(void, g_vl[i].layer, "removeFromSuperlayer");
    MSG0(void, g_vl[i].layer, "release");
    /* 撤早退标志(会话还在 → CPU 拉帧路径恢复; 已 close → 查表无果, 无妨) */
    if (hnp_media_set_layer_mode) hnp_media_set_layer_mode(player_id, 0);
    g_vl[i] = g_vl[g_vl_n - 1];
    g_vl_n--;
    vl_flush();
    pool_pop(pool);
}

static hnp_key mac_keycode_to_hnp(unsigned short kc) {
    switch (kc) {
    case 53: return HNP_KEY_ESCAPE;
    case 36: case 76: return HNP_KEY_RETURN;
    case 48: return HNP_KEY_TAB;
    case 51: return HNP_KEY_BACKSPACE;
    case 117: return HNP_KEY_DELETE;
    case 123: return HNP_KEY_LEFT;
    case 124: return HNP_KEY_RIGHT;
    case 125: return HNP_KEY_DOWN;
    case 126: return HNP_KEY_UP;
    case 115: return HNP_KEY_HOME;
    case 119: return HNP_KEY_END;
    case 116: return HNP_KEY_PAGE_UP;
    case 121: return HNP_KEY_PAGE_DOWN;
    case 122: return HNP_KEY_F1;  case 120: return HNP_KEY_F2;  case 99: return HNP_KEY_F3;
    case 118: return HNP_KEY_F4;  case 96: return HNP_KEY_F5;   case 97: return HNP_KEY_F6;
    case 98: return HNP_KEY_F7;   case 100: return HNP_KEY_F8;  case 101: return HNP_KEY_F9;
    case 109: return HNP_KEY_F10; case 103: return HNP_KEY_F11; case 111: return HNP_KEY_F12;
    default: return HNP_KEY_PRINTABLE;
    }
}

int hnp_pump(hnp_event *ev, int timeout_ms) {
    if (!ev) return 0;
    ev->kind = HNP_EV_NONE;
    ev->utf8 = NULL;
    /* 事故②同款修复: 每次泵的 NSDate/NSEvent 及 AppKit 事件分派内部
       autorelease 对象, 落进本次有界池统一回收(无池时逐事件泄漏)。 */
    obj_t pool = pool_push();
    obj_t app = MSG0(obj_t, o_getClass("NSApplication"), "sharedApplication");
    obj_t NSDate = o_getClass("NSDate");
    obj_t lim;
    if (timeout_ms < 0) lim = MSG0(obj_t, NSDate, "distantPast");
    else if (timeout_ms == 0) lim = MSG0(obj_t, NSDate, "distantPast");
    else lim = ((obj_t (*)(obj_t, sel_t, double))o_msgSend)(
        NSDate, o_sel("dateWithTimeIntervalSinceNow:"), timeout_ms / 1000.0);
    obj_t mode = nsstr("kCFRunLoopDefaultMode");
    obj_t e = ((obj_t (*)(obj_t, sel_t, unsigned long, obj_t, obj_t, int))o_msgSend)(
        app, o_sel("nextEventMatchingMask:untilDate:inMode:dequeue:"), ~0UL, lim, mode, 1);
    if (!e) { pool_pop(pool); return 0; }
    unsigned long ty = MSG0(unsigned long, e, "type");
    CGPoint p = MSG0(CGPoint, e, "locationInWindow");
    /* AppKit 原点在左下 → 内容区左上原点(y 翻转由窗口高度决定, 用 iv 的 frame) */
    /* 窗口高度由调用方在 RESIZE 时同步; 这里用事件窗口的 content 尺寸 */
    obj_t win = MSG0(obj_t, e, "window");
    double hh = 600;
    if (win) {
        CGRect cf = MSG0(CGRect, MSG0(obj_t, win, "contentView"), "frame");
        hh = cf.size.h ? cf.size.h : 600;
    }
    int x = (int)p.x, y = (int)(hh - p.y);
    unsigned long flags = MSG0(unsigned long, e, "modifierFlags");
    unsigned mods = ((flags & 0x20000) ? 1 : 0) | ((flags & 0x40000) ? 2 : 0)
                  | ((flags & 0x80000) ? 4 : 0) | ((flags & 0x100000) ? 8 : 0);
    switch (ty) {
    case EV_LDOWN: ev->kind = HNP_EV_BUTTON_DOWN; ev->x = x; ev->y = y; ev->button = 0; ev->mods = mods; break;
    case EV_LUP:   ev->kind = HNP_EV_BUTTON_UP;   ev->x = x; ev->y = y; ev->button = 0; ev->mods = mods; break;
    case EV_RDOWN: ev->kind = HNP_EV_BUTTON_DOWN; ev->x = x; ev->y = y; ev->button = 2; ev->mods = mods; break;
    case EV_MOVE: case EV_LDRAG:
        ev->kind = HNP_EV_POINTER_MOVE; ev->x = x; ev->y = y; ev->mods = mods; break;
    case EV_SCROLL: {
        ev->kind = HNP_EV_SCROLL;
        ev->x = x; ev->y = y;
        ev->dx = (float)MSG0(double, e, "deltaX");
        ev->dy = (float)MSG0(double, e, "deltaY");
        ev->mods = mods;
        break;
    }
    case EV_KEYDOWN: case EV_KEYUP: {
        ev->kind = (ty == EV_KEYDOWN) ? HNP_EV_KEY_DOWN : HNP_EV_KEY_UP;
        unsigned short kc = MSG0(unsigned short, e, "keyCode");
        ev->key = mac_keycode_to_hnp(kc);
        ev->mods = mods;
        /* 可打印字符: charactersIgnoringModifiers */
        if (ev->key == HNP_KEY_PRINTABLE) {
            obj_t ch = MSG0(obj_t, e, "charactersIgnoringModifiers");
            if (ch) {
                const char *s = ((const char *(*)(obj_t, sel_t))o_msgSend)(ch, o_sel("UTF8String"));
                if (s && (unsigned char)s[0] >= ' ') {
                    size_t i = 0;
                    while (s[i] && i < 7) { g_utf8[i] = s[i]; i++; }
                    g_utf8[i] = 0;
                    ev->utf8 = g_utf8;
                }
            }
        }
        break;
    }
    default:
        /* AppKitDefined/SystemDefined/ApplicationDefined/Periodic(13..16)是
           AppKit 内部消息, 不可 sendEvent:(实测抛异常); 跳过。 */
        pool_pop(pool);
        if (ty >= EV_APPKITDEF && ty <= EV_PERIODIC) return 0;
        return 0;
    }
    /* 用户输入交回 AppKit(窗口移动/关闭/重绘) */
    MSG1(void, app, "sendEvent:", e);
    pool_pop(pool);
    return 1;
}

unsigned long hnp_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

void hnp_set_cursor(hnp_window *w, hnp_cursor c) {
    (void)w;
    const char *sel = c == HNP_CURSOR_POINTER ? "pointingHandCursor"
                    : c == HNP_CURSOR_TEXT ? "IBeamCursor" : "arrowCursor";
    obj_t cur = MSG0(obj_t, o_getClass("NSCursor"), sel);
    if (cur) MSG0(void, cur, "set");
}

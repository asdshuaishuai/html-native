/* hnp_macos.c — macOS 平台实现(纯 C99: dlopen AppKit + objc_msgSend)
 *
 * hn_platform.h 的 macOS 实现。零 ObjC 头文件 / 零 .m / 零 Swift;
 * 连链接期都不需要 framework(全部 dlsym)。
 * 事件常量逐值取自 SDK 的 NSEvent.h(NSEventTypeLeftMouseDown=1 等) ——
 * 不要凭记忆写(旧版枚举混写曾让 KitDefined 落进 keydown 分支崩溃)。
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

static obj_t nsstr(const char *s) {
    return ((obj_t (*)(obj_t, sel_t, const char *))o_msgSend)(
        o_getClass("NSString"), o_sel("stringWithUTF8String:"), s);
}

int hnp_init(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_NOW)) return 0;
    dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_NOW);
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
};

/* 可打印字符的缓冲(单事件生命周期, 调用方在下次 pump 前消费) */
static char g_utf8[8];

hnp_window *hnp_window_open(const hnp_window_desc *d) {
    if (!d) return NULL;
    hnp_window *w = (hnp_window *)calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->w = d->width; w->h = d->height; w->transparent = d->transparent;

    unsigned long style = d->resizable ? (1 | 2 | 4 | 8) : (1 | 2 | 4);
    if (d->transparent) style = 0;
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
    obj_t iv = MSG0(obj_t, o_getClass("NSImageView"), "alloc");
    CGRect cr = { { 0, 0 }, { (double)d->width, (double)d->height } };
    iv = ((obj_t (*)(obj_t, sel_t, CGRect))o_msgSend)(iv, o_sel("initWithFrame:"), cr);
    MSG1(void, win, "setContentView:", iv);
    /* 主屏可见区中央 */
    obj_t screen = MSG0(obj_t, o_getClass("NSScreen"), "mainScreen");
    CGRect sf = MSG0(CGRect, screen, "visibleFrame");
    CGRect wf = MSG0(CGRect, win, "frame");
    CGRect pos = { { sf.origin.x + (sf.size.w - wf.size.w) / 2,
                     sf.origin.y + (sf.size.h - wf.size.h) / 2 },
                   { wf.size.w, wf.size.h } };
    ((void (*)(obj_t, sel_t, CGRect, int))o_msgSend)(win, o_sel("setFrame:display:"), pos, 1);
    MSG1(void, win, "makeKeyAndOrderFront:", (obj_t)1);
    MSG1(void, MSG0(obj_t, o_getClass("NSApplication"), "sharedApplication"),
         "activateIgnoringOtherApps:", (obj_t)1);
    w->win = win; w->iv = iv;
    return w;
}

void hnp_window_close(hnp_window *w) {
    if (!w) return;
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
    CGColorSpaceRef cs = ((CGColorSpaceRef (*)(void))dlsym(RTLD_DEFAULT, "CGColorSpaceCreateDeviceRGB"))();
    CGDataProviderRef prov = ((CGDataProviderRef (*)(void *, const void *, size_t, void (*)(void *, const void *, size_t)))
        dlsym(RTLD_DEFAULT, "CGDataProviderCreateWithData"))(NULL, bgra_pre, (size_t)bw * bh * 4, NULL);
    /* stride 此处要求 == w*4(hn_rt 保证); kCGImageAlphaPremultipliedFirst|kCGBitmapByteOrder32Little */
    CGImageRef cg = ((CGImageRef (*)(size_t, size_t, size_t, size_t, size_t, CGColorSpaceRef,
                                     uint32_t, CGDataProviderRef, const double *, int, int))
        dlsym(RTLD_DEFAULT, "CGImageCreate"))(
        (size_t)bw, (size_t)bh, 8, 32, stride, cs, 2u | 8192u, prov, NULL, 0, 0);
    if (!cg) return;
    obj_t nsimg = MSG0(obj_t, o_getClass("NSImage"), "alloc");
    CGSize sz = { (double)bw, (double)bh };
    nsimg = ((obj_t (*)(obj_t, sel_t, CGImageRef, CGSize))o_msgSend)(
        nsimg, o_sel("initWithCGImage:size:"), cg, sz);
    MSG1(void, w->iv, "setImage:", nsimg);
    if (w->img) MSG0(void, w->img, "release");
    w->img = nsimg;
    ((void (*)(CGImageRef))dlsym(RTLD_DEFAULT, "CGImageRelease"))(cg);
    MSG0(void, (obj_t)prov, "release");
    MSG0(void, w->win, "display");
}

void hnp_window_invalidate(hnp_window *w) { if (w && w->win) MSG0(void, w->win, "display"); }

void hnp_window_set_title(hnp_window *w, const char *utf8) {
    if (w && w->win) MSG1(void, w->win, "setTitle:", nsstr(utf8 ? utf8 : "hn"));
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
    if (!e) return 0;
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
        if (ty >= EV_APPKITDEF && ty <= EV_PERIODIC) return 0;
        return 0;
    }
    /* 用户输入交回 AppKit(窗口移动/关闭/重绘) */
    MSG1(void, app, "sendEvent:", e);
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

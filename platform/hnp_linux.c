/* hnp_linux.c — Linux 平台实现(dlopen X11, 零编译期依赖)
 *
 * hn_platform.h 的 Linux 实现。X11 经运行期 dlopen("libX11.so.6"),
 * 手工声明用到的最小 Xlib 函数/结构(ABI 布局依据与断言见 hnweb_linux.c,
 * 本文件复用同一套已验证的 stub)。
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <time.h>
#include <sys/select.h>

#include "hn_platform.h"

/* ================= 最小 Xlib ABI(dlopen) ================= */

typedef struct _XDisplay xdisplay;
typedef unsigned long xwindow, xatom;

/* X11 事件类型 */
enum { XE_KEYPRESS = 2, XE_KEYRELEASE = 3, XE_BUTTONPRESS = 4, XE_BUTTONRELEASE = 5,
       XE_MOTIONNOTIFY = 6, XE_EXPOSE = 12, XE_CONFIGURE = 22, XE_CLIENTMSG = 33 };

typedef struct { int type; unsigned long serial; int send_event;
                 xdisplay *display; xwindow window; } xany_stub;
typedef struct { int type; unsigned long serial; int send_event; xdisplay *display;
                 xwindow window; xwindow subwindow; unsigned long time;
                 int x, y, x_root, y_root; unsigned int state; unsigned int detail;
                 int same_screen; } xdevice_stub;
typedef struct { int type; unsigned long serial; int send_event; xdisplay *display;
                 xwindow window; int x, y, width, height, count; } xexpose_stub;
typedef struct { int type; unsigned long serial; int send_event; xdisplay *display;
                 xwindow window; int x, y, width, height, border_width;
                 xwindow above; int override_redirect; } xconfig_stub;
typedef struct { int type; unsigned long serial; int send_event; xdisplay *display;
                 xwindow window; xatom message_type; int format; long data_l[5]; } xclient_stub;

#define X_EVENT_LONGS 24
typedef union { long align_l[X_EVENT_LONGS];
                unsigned char b[X_EVENT_LONGS * sizeof(long)]; } xevent_buf;

typedef struct { int width, height; int xoffset; int format; char *data;
                 int byte_order, bitmap_unit, bitmap_bit_order, bitmap_pad, depth; } ximage_stub;

/* XVisualInfo(只用到 depth/visual/red_mask 等核心字段) */
typedef struct { void *visual; xwindow visualid; int screen; int depth; } xvisual_stub;

typedef struct { unsigned long red_mask, green_mask, blue_mask; } xformat_stub;
typedef struct { xwindow ext_data; xdisplay *display; xwindow windowid;
                 int x, y, width, height, border_width; int depth; void *visual;
                 xwindow root; int class_; int bit_gravity, win_gravity, backing_store;
                 unsigned long backing_planes, backing_pixel; int save_under;
                 xwindow colormap; int map_installed; } xwinattr_stub;

/* 函数指针表 */
typedef xdisplay *(*fn_XOpenDisplay_t)(const char *);
typedef int (*fn_XCloseDisplay_t)(xdisplay *);
typedef int (*fn_XDefaultScreen_t)(xdisplay *);
typedef xwindow (*fn_XDefaultRootWindow_t)(xdisplay *);
typedef void *(*fn_XDefaultVisual_t)(xdisplay *, int);
typedef int (*fn_XDefaultDepth_t)(xdisplay *, int);
typedef int (*fn_XMatchVisualInfo_t)(xdisplay *, int, int, int, xvisual_stub *);
typedef xwindow (*fn_XCreateColormap_t)(xdisplay *, xwindow, void *, int);
typedef int (*fn_XFreeColormap_t)(xdisplay *, xwindow);
typedef xwindow (*fn_XCreateWindow_t)(xdisplay *, xwindow, int, int, unsigned, unsigned,
                                      int, int, unsigned, void *, unsigned long, void *);
/* 12 参: dpy parent x y w h border depth class visual valuemask attrs */
typedef int (*fn_XDestroyWindow_t)(xdisplay *, xwindow);
typedef int (*fn_XMapWindow_t)(xdisplay *, xwindow);
typedef int (*fn_XStoreName_t)(xdisplay *, xwindow, const char *);
typedef int (*fn_XSelectInput_t)(xdisplay *, xwindow, long);
typedef int (*fn_XSetWMProtocols_t)(xdisplay *, xwindow, xatom *, int);
typedef xatom (*fn_XInternAtom_t)(xdisplay *, const char *, int);
typedef void *(*fn_XCreateGC_t)(xdisplay *, xwindow, unsigned long, void *);
typedef int (*fn_XFreeGC_t)(xdisplay *, void *);
typedef void *(*fn_XCreateImage_t)(xdisplay *, void *, int, int, int, char *,
                                   unsigned, unsigned, int, int);
typedef int (*fn_XPutImage_t)(xdisplay *, xwindow, void *, void *, int, int, int, int, int, int);
typedef int (*fn_XDestroyImage_t)(void *);
typedef int (*fn_XNextEvent_t)(xdisplay *, void *);
typedef int (*fn_XPending_t)(xdisplay *);
typedef int (*fn_XFlush_t)(xdisplay *);
typedef int (*fn_XLookupString_t)(void *, char *, int, xatom *, int *);
typedef int (*fn_XConnectionNumber_t)(xdisplay *);
typedef int (*fn_XGetWindowAttributes_t)(xdisplay *, xwindow, xwinattr_stub *);

static struct {
    void *lib;
    fn_XOpenDisplay_t XOpenDisplay;
    fn_XCloseDisplay_t XCloseDisplay;
    fn_XDefaultScreen_t XDefaultScreen;
    fn_XDefaultRootWindow_t XDefaultRootWindow;
    fn_XMatchVisualInfo_t XMatchVisualInfo;
    fn_XCreateColormap_t XCreateColormap;
    fn_XCreateWindow_t XCreateWindow;
    fn_XDestroyWindow_t XDestroyWindow;
    fn_XMapWindow_t XMapWindow;
    fn_XStoreName_t XStoreName;
    fn_XSelectInput_t XSelectInput;
    fn_XSetWMProtocols_t XSetWMProtocols;
    fn_XInternAtom_t XInternAtom;
    fn_XCreateGC_t XCreateGC;
    fn_XFreeGC_t XFreeGC;
    fn_XCreateImage_t XCreateImage;
    fn_XPutImage_t XPutImage;
    fn_XDestroyImage_t XDestroyImage;
    fn_XNextEvent_t XNextEvent;
    fn_XPending_t XPending;
    fn_XFlush_t XFlush;
    fn_XLookupString_t XLookupString;
    fn_XConnectionNumber_t XConnectionNumber;
    xdisplay *dpy;
    xatom wm_protocols, wm_delete_window;
    int users;
} x11;

#define SYM(N, T) x11.N = (T)dlsym(x11.lib, #N); if (!x11.N) return 0

static int x11_load(void) {
    if (x11.lib) return 1;
    x11.lib = dlopen("libX11.so.6", RTLD_NOW);
    if (!x11.lib) x11.lib = dlopen("libX11.so", RTLD_NOW);
    if (!x11.lib) return 0;
    SYM(XOpenDisplay, fn_XOpenDisplay_t);
    SYM(XCloseDisplay, fn_XCloseDisplay_t);
    SYM(XDefaultScreen, fn_XDefaultScreen_t);
    SYM(XDefaultRootWindow, fn_XDefaultRootWindow_t);
    SYM(XMatchVisualInfo, fn_XMatchVisualInfo_t);
    SYM(XCreateColormap, fn_XCreateColormap_t);
    SYM(XCreateWindow, fn_XCreateWindow_t);
    SYM(XDestroyWindow, fn_XDestroyWindow_t);
    SYM(XMapWindow, fn_XMapWindow_t);
    SYM(XStoreName, fn_XStoreName_t);
    SYM(XSelectInput, fn_XSelectInput_t);
    SYM(XSetWMProtocols, fn_XSetWMProtocols_t);
    SYM(XInternAtom, fn_XInternAtom_t);
    SYM(XCreateGC, fn_XCreateGC_t);
    SYM(XFreeGC, fn_XFreeGC_t);
    SYM(XCreateImage, fn_XCreateImage_t);
    SYM(XPutImage, fn_XPutImage_t);
    SYM(XDestroyImage, fn_XDestroyImage_t);
    SYM(XNextEvent, fn_XNextEvent_t);
    SYM(XPending, fn_XPending_t);
    SYM(XFlush, fn_XFlush_t);
    SYM(XLookupString, fn_XLookupString_t);
    SYM(XConnectionNumber, fn_XConnectionNumber_t);
    return 1;
}

/* ================= hn_platform API ================= */

int hnp_init(int argc, char **argv) {
    (void)argc; (void)argv;
    if (!x11_load()) return 0;
    if (!x11.dpy) {
        x11.dpy = x11.XOpenDisplay(NULL);
        if (!x11.dpy) return 0;
    }
    x11.wm_protocols = x11.XInternAtom(x11.dpy, "WM_PROTOCOLS", 0);
    x11.wm_delete_window = x11.XInternAtom(x11.dpy, "WM_DELETE_WINDOW", 0);
    return 1;
}

void hnp_shutdown(void) {
    if (x11.dpy && x11.users == 0) {
        x11.XCloseDisplay(x11.dpy);
        x11.dpy = NULL;
    }
}

const char *hnp_name(void) { return "linux"; }

struct hnp_window {
    xwindow xw;
    void *gc;
    void *ximage;           /* XDestroyImage 时释放 data */
    unsigned char *buf;     /* XImage data(预乘 BGRA) */
    int w, h;
    char utf8[8];
    int depth;
};

static hnp_window *g_windows[16];
static int g_window_n;

hnp_window *hnp_window_open(const hnp_window_desc *d) {
    if (!d || !x11.dpy) return NULL;
    hnp_window *w = (hnp_window *)calloc(1, sizeof(*w));
    if (!w) return NULL;
    int scr = x11.XDefaultScreen(x11.dpy);
    xwindow root = x11.XDefaultRootWindow(x11.dpy);
    /* CopyFromParent(0) visual/depth: 24 位常规窗口即可渲染;
       32 位透明 visual 需要完整 XSetWindowAttributes(下一阶段接)。 */
    long evmask = 0x1 | 0x2 | 0x4 | 0x8 | 0x10 | 0x40 | 0x8000 | 0x80000;
    w->depth = 24;
    {
        /* CWEventMask 传 XSetWindowAttributes; 用足够大的缓冲承载结构,
           event_mask 在 offset 0(X11 ABI), 后面字段不设(掩码只开这一位)。 */
        unsigned long attrs[16] = { 0 };
        attrs[0] = (unsigned long)evmask;
        w->xw = x11.XCreateWindow(x11.dpy, root, 0, 0, (unsigned)d->width, (unsigned)d->height,
                                  0, 0 /*CopyFromParent*/, 1 /*InputOutput*/, NULL,
                                  0x80000 /*CWEventMask*/, attrs);
    }
    if (!w->xw) { free(w); return NULL; }
    x11.XStoreName(x11.dpy, w->xw, d->title ? d->title : "hn");
    x11.XSelectInput(x11.dpy, w->xw, evmask);
    {
        xatom protos[1] = { x11.wm_delete_window };
        x11.XSetWMProtocols(x11.dpy, w->xw, protos, 1);
    }
    w->gc = x11.XCreateGC(x11.dpy, w->xw, 0, NULL);
    if (!w->gc) { x11.XDestroyWindow(x11.dpy, w->xw); free(w); return NULL; }
    w->w = d->width; w->h = d->height;
    x11.XMapWindow(x11.dpy, w->xw);
    x11.XFlush(x11.dpy);
    if (g_window_n < 16) g_windows[g_window_n++] = w;
    x11.users++;
    return w;
}

void hnp_window_close(hnp_window *w) {
    if (!w) return;
    if (w->ximage) x11.XDestroyImage(w->ximage);
    if (w->gc) x11.XFreeGC(x11.dpy, w->gc);
    x11.XDestroyWindow(x11.dpy, w->xw);
    for (int i = 0; i < g_window_n; i++)
        if (g_windows[i] == w) { g_windows[i] = g_windows[--g_window_n]; break; }
    x11.users--;
    free(w->buf);
    free(w);
}

void hnp_window_resize(hnp_window *w, int nw, int nh) {
    if (!w) return;
    w->w = nw; w->h = nh;
    if (w->ximage) { x11.XDestroyImage(w->ximage); w->ximage = NULL; }
}

void hnp_window_blit(hnp_window *w, const unsigned char *bgra_pre, int bw, int bh, size_t stride) {
    if (!w || !bgra_pre || !x11.dpy) return;
    if (!w->ximage) {
        w->buf = (unsigned char *)malloc((size_t)bw * bh * 4);
        if (!w->buf) return;
        w->ximage = x11.XCreateImage(x11.dpy, NULL, w->depth, 2 /*ZPixmap*/, 0,
                                     (char *)w->buf, (unsigned)bw, (unsigned)bh,
                                     32 /*bitmap_pad*/, (int)stride /*bytes_per_line*/);
        if (!w->ximage) { free(w->buf); w->buf = NULL; return; }
    }
    for (int y = 0; y < bh; y++)
        memcpy(w->buf + (size_t)y * bw * 4, bgra_pre + y * stride, (size_t)bw * 4);
    x11.XPutImage(x11.dpy, w->xw, w->gc, w->ximage, 0, 0, 0, 0, (unsigned)bw, (unsigned)bh);
    x11.XFlush(x11.dpy);
}

void hnp_window_invalidate(hnp_window *w) { (void)w; }

void hnp_window_set_title(hnp_window *w, const char *utf8) {
    if (w && utf8) x11.XStoreName(x11.dpy, w->xw, utf8);
}

static hnp_key keysym_to_hnp(unsigned long ks) {
    switch (ks) {
    case 0xFF1B: return HNP_KEY_ESCAPE;
    case 0xFF0D: return HNP_KEY_RETURN;
    case 0xFF09: return HNP_KEY_TAB;
    case 0xFF08: return HNP_KEY_BACKSPACE;
    case 0xFFFF: return HNP_KEY_DELETE;
    case 0xFF51: return HNP_KEY_LEFT;
    case 0xFF53: return HNP_KEY_RIGHT;
    case 0xFF52: return HNP_KEY_UP;
    case 0xFF54: return HNP_KEY_DOWN;
    case 0xFF50: return HNP_KEY_HOME;
    case 0xFF57: return HNP_KEY_END;
    case 0xFF55: return HNP_KEY_PAGE_UP;
    case 0xFF56: return HNP_KEY_PAGE_DOWN;
    default: return HNP_KEY_PRINTABLE;
    }
}

int hnp_pump(hnp_event *ev, int timeout_ms) {
    if (!ev || !x11.dpy) return 0;
    ev->kind = HNP_EV_NONE;
    ev->utf8 = NULL;
    /* select 等 X 连接可读(带超时); 或立即返回 */
    if (x11.XPending(x11.dpy) == 0 && timeout_ms >= 0 && timeout_ms < 0x7FFFFFFF) {
        int fd = x11.XConnectionNumber(x11.dpy);
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        if (select(fd + 1, &rf, NULL, NULL, timeout_ms > 0 ? &tv : NULL) == 0)
            return 0;
    }
    xevent_buf xb;
    if (!x11.XNextEvent(x11.dpy, &xb)) return 0;
    int type = ((xany_stub *)&xb)->type;
    switch (type) {
    case XE_CLIENTMSG: {
        xclient_stub *cm = (xclient_stub *)&xb;
        if (cm->data_l[0] == (long)x11.wm_delete_window) { ev->kind = HNP_EV_CLOSE; return 1; }
        return 0;
    }
    case XE_EXPOSE: ev->kind = HNP_EV_EXPOSE; return 1;
    case XE_CONFIGURE: {
        xconfig_stub *cf = (xconfig_stub *)&xb;
        ev->kind = HNP_EV_RESIZE;
        ev->x = cf->width; ev->y = cf->height;
        return 1;
    }
    case XE_BUTTONPRESS: case XE_BUTTONRELEASE: {
        xdevice_stub *dv = (xdevice_stub *)&xb;
        ev->kind = (type == XE_BUTTONPRESS) ? HNP_EV_BUTTON_DOWN : HNP_EV_BUTTON_UP;
        ev->x = dv->x; ev->y = dv->y;
        ev->button = (dv->detail == 1) ? 0 : (dv->detail == 3) ? 2 : 1;
        return 1;
    }
    case XE_MOTIONNOTIFY: {
        xdevice_stub *dv = (xdevice_stub *)&xb;
        ev->kind = HNP_EV_POINTER_MOVE;
        ev->x = dv->x; ev->y = dv->y;
        return 1;
    }
    case XE_KEYPRESS: case XE_KEYRELEASE: {
        xdevice_stub *dv = (xdevice_stub *)&xb;
        ev->kind = (type == XE_KEYPRESS) ? HNP_EV_KEY_DOWN : HNP_EV_KEY_UP;
        char buf[8] = { 0 };
        xatom ks = 0;
        int n = x11.XLookupString((void *)&xb, buf, sizeof(buf) - 1, &ks, NULL);
        ev->key = keysym_to_hnp(ks);
        if (ev->key == HNP_KEY_PRINTABLE && n > 0 && (unsigned char)buf[0] >= ' ') {
            static char g_u[8];
            memcpy(g_u, buf, (size_t)n);
            g_u[n] = 0;
            ev->utf8 = g_u;
        }
        return 1;
    }
    default: return 0;
    }
}

unsigned long hnp_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

void hnp_set_cursor(hnp_window *w, hnp_cursor c) { (void)w; (void)c; /* M1: 未接 XCreateFontCursor */ }

/* ---------------- 媒体 ABI 留位(docs/media-design.md §2) ----------------
   Linux 不带媒体实现(Windows Media Foundation / GStreamer 同为"留位不实现",
   见 media-design §9): supported()=0, 引擎/桥据此整体降级(不播, 不炸)。
   其余 hnp_media_* 有意不提供 —— 调用方必须先看 supported(), 因此
   本平台路径对媒体零符号依赖(不走 hn_platform.h 里的 static inline
   兜底: 那会把真实实现静默遮蔽成 0, 属于坑)。 */
int hnp_media_supported(void) { return 0; }

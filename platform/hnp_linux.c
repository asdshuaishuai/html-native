/* hnp_linux.c — Linux 平台实现(dlopen X11/Xext, 零编译期依赖)
 *
 * hn_platform.h 的 Linux 实现。X11 经运行期 dlopen("libX11.so.6"),
 * 手工声明用到的最小 Xlib 函数/结构(ABI 布局依据与断言见 hnweb_linux.c,
 * 本文件复用同一套已验证的 stub)。
 * 窗口形态/桌宠能力(层级/跨空间/无边框/鼠标穿透/位置记忆)与 macOS 侧
 * 同契约: EWMH(_NET_WM_STATE)+ Motif(_MOTIF_WM_HINTS)+ XShape 输入形状,
 * 全部 dlopen 运行期解析; 常量逐值抄录见下方常量块注释(同 hnp_macos.c
 * 的"不凭记忆写常量"纪律)。libXext(XShape)缺席时 click_through 优雅
 * 降级为无穿透(声明照常接受, 物化看平台)。
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

/* ---- 窗口形态/桌宠能力所需(与上面同一解析模式, 运行期 dlsym) ---- */
typedef int (*fn_XMoveWindow_t)(xdisplay *, xwindow, int, int);
typedef int (*fn_XTranslateCoordinates_t)(xdisplay *, xwindow, xwindow, int, int,
                                          int *, int *, xwindow *);
typedef int (*fn_XSendEvent_t)(xdisplay *, xwindow, int, long, xclient_stub *);
typedef int (*fn_XChangeProperty_t)(xdisplay *, xwindow, xatom, xatom, int, int,
                                    const unsigned char *, int);

/* XShape 的矩形 —— X11/extensions/shape.h 的 XRectangle(short 字段) */
typedef struct { short x, y; unsigned short width, height; } xrect_stub;
typedef int (*fn_XShapeCombineRectangles_t)(xdisplay *, xwindow, int /*kind*/,
                                            int /*x_off*/, int /*y_off*/,
                                            xrect_stub * /*rects*/, int /*n*/,
                                            int /*op*/, int /*ordering*/);

/* ---------------- 窗口形态常量(逐值抄录, 不凭记忆 —— 同 hnp_macos.c 纪律) ----------------

   EWMH(freedesktop.org《Extended Window Manager Hints》, _NET_WM_STATE 节):
     _NET_WM_STATE 属性 = XA_ATOM 数组(规范原文 "type ATOM[]"; 注意不是
     XA_WINDOW —— XA_WINDOW 用于 _NET_SUPPORTING_WM_CHECK 一族);
     client message 动作: _NET_WM_STATE_REMOVE=0 / _NET_WM_STATE_ADD=1 /
     _NET_WM_STATE_TOGGLE=2; data.l[3] = source indication, 1 = 普通应用。
     原子一律 XInternAtom 按名字取(规范不定义原子数值 —— 写数字才是凭记忆)。

   Motif WM hints(Xm/MwmUtil.h 逐值抄录; GTK 的 GdkWMHints 同一套):
     MWM_HINTS_FUNCTIONS   = 1<<0
     MWM_HINTS_DECORATIONS = 1<<1   ← borderless 开这一位
     MWM_HINTS_INPUT_MODE  = 1<<2
     MWM_HINTS_STATUS      = 1<<3
   borderless = flags 只开 DECORATIONS + decorations 字段 = 0(GTK 的
   gtk_window_set_decorated(FALSE) 即此法)。"decorations 开关在位 0"是
   误记 —— 位 0 是 FUNCTIONS, 开它而 functions=0 的语义是"去掉全部
   功能钮", 语义不是无装饰。

   XShape(X11/extensions/shape.h 逐值抄录):
     kind: ShapeBounding=0 / ShapeClip=1 / ShapeInput=2
     op  : ShapeSet=0 / ShapeUnion=1 / ShapeIntersect=2 / ShapeSubtract=3 /
           ShapeInvert=4
     ordering: Unsorted=0 / YSorted=1 / YXSorted=2 / YXBanded=3

   X.h 事件掩码(逐值抄录): SubstructureNotifyMask=1<<19,
   SubstructureRedirectMask=1<<20(_NET_WM_STATE client message 发往根窗口
   所需; 常规事件路由按 XE_* 枚举, 与上面 enum 不冲突)。 */
#define X_PROP_REPLACE 0            /* PropModeReplace(X11/X.h) */
#define HN_MWM_HINTS_DECORATIONS (1UL << 1)
#define HN_XSHAPE_INPUT    2
#define HN_XSHAPE_SET      0
#define HN_XSHAPE_UNSORTED 0
#define HN_SUBSTRUCT_NOTIFY   (1L << 19)
#define HN_SUBSTRUCT_REDIRECT (1L << 20)

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
    fn_XGetWindowAttributes_t XGetWindowAttributes;
    fn_XMoveWindow_t XMoveWindow;
    fn_XTranslateCoordinates_t XTranslateCoordinates;
    fn_XSendEvent_t XSendEvent;
    fn_XChangeProperty_t XChangeProperty;
    xdisplay *dpy;
    xatom wm_protocols, wm_delete_window;
    /* 窗口形态原子(hnp_init 里按名字 intern; 0 = 不可用, 调用方跳过) */
    xatom net_wm_state, net_wm_state_above, net_wm_state_below, net_wm_state_sticky;
    xatom motif_wm_hints, atom_atom;
    int users;
} x11;

/* libXext(XShape): 独立句柄 + 独立装配 —— 缺席不连累 libX11 的窗口管线,
   click_through 优雅降级(见 hnp_window_set_click_through 注释)。 */
static struct {
    void *lib;
    fn_XShapeCombineRectangles_t XShapeCombineRectangles;
} xshape;

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
    SYM(XGetWindowAttributes, fn_XGetWindowAttributes_t);
    SYM(XMoveWindow, fn_XMoveWindow_t);
    SYM(XTranslateCoordinates, fn_XTranslateCoordinates_t);
    SYM(XSendEvent, fn_XSendEvent_t);
    SYM(XChangeProperty, fn_XChangeProperty_t);
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
    /* 窗口形态原子(名字按 EWMH/Motif 规范; XInternAtom 的 exist_ok=0
       会在缺席时创建, 因此这里不会拿到 None, 除非 X 分配失败 —— 失败给 0,
       调用方按 0 跳过)。atom_atom = Xlib 预定义原子 XA_ATOM(=4, 经名字
       "ATOM" 取, 不写数字)。 */
    x11.atom_atom = x11.XInternAtom(x11.dpy, "ATOM", 0);
    x11.net_wm_state = x11.XInternAtom(x11.dpy, "_NET_WM_STATE", 0);
    x11.net_wm_state_above = x11.XInternAtom(x11.dpy, "_NET_WM_STATE_ABOVE", 0);
    x11.net_wm_state_below = x11.XInternAtom(x11.dpy, "_NET_WM_STATE_BELOW", 0);
    x11.net_wm_state_sticky = x11.XInternAtom(x11.dpy, "_NET_WM_STATE_STICKY", 0);
    x11.motif_wm_hints = x11.XInternAtom(x11.dpy, "_MOTIF_WM_HINTS", 0);
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

/* EWMH 状态切换的第二条生效路径(freedesktop.org《Extended Window Manager
   Hints》_NET_WM_STATE 节逐值抄录):
     message_type = _NET_WM_STATE, format = 32
     data.l[0] = _NET_WM_STATE_ADD(1); data.l[1] = 状态原子; data.l[2] = 0
     data.l[3] = source indication: 1 = 普通应用
   发往根窗口, 事件掩码 = SubstructureNotify|SubstructureRedirect。
   与 map 前写的 _NET_WM_STATE 属性构成双通道: 属性给"读属性"的 WM,
   message 给"只响应运行期请求"的 WM —— 两条都走(见 hnp_window_open)。 */
static void hn_ewmh_state_msg(xwindow win, xatom state_atom) {
    if (!x11.dpy || !x11.XSendEvent || !x11.net_wm_state || !state_atom) return;
    xevent_buf xb;
    memset(&xb, 0, sizeof xb);          /* 整个 XEvent 缓冲清零再填, 合 ABI */
    xclient_stub *cm = (xclient_stub *)&xb;
    cm->type = XE_CLIENTMSG;
    cm->window = win;
    cm->message_type = x11.net_wm_state;
    cm->format = 32;
    cm->data_l[0] = 1;                  /* _NET_WM_STATE_ADD */
    cm->data_l[1] = (long)state_atom;
    cm->data_l[2] = 0;                  /* 无第二状态原子 */
    cm->data_l[3] = 1;                  /* source = 普通应用 */
    x11.XSendEvent(x11.dpy, x11.XDefaultRootWindow(x11.dpy), 0,
                   HN_SUBSTRUCT_NOTIFY | HN_SUBSTRUCT_REDIRECT, cm);
}

/* XShape 输入形状装配: 独立于 libX11 的懒加载 —— libXext 找不到(或没有
   XShapeCombineRectangles)返回 0, 调用方优雅降级(无穿透, 声明照常通过)。 */
static int xshape_load(void) {
    if (xshape.lib) return 1;
    xshape.lib = dlopen("libXext.so.6", RTLD_NOW);
    if (!xshape.lib) xshape.lib = dlopen("libXext.so", RTLD_NOW);
    if (!xshape.lib) return 0;
    xshape.XShapeCombineRectangles =
        (fn_XShapeCombineRectangles_t)dlsym(xshape.lib, "XShapeCombineRectangles");
    return xshape.XShapeCombineRectangles ? 1 : 0;
}

/* 输入形状切换(kind=ShapeInput): on=1 → 对输入区域组合**空矩形集**
   (n_rects=0, op=ShapeSet)→ 输入区域为空, 整窗穿透(事件落到下层窗口/
   桌面); on=0 → 重设整窗矩形恢复接收。XShape 不经 WM, 立即生效。
   窗口尺寸取 w->w/h(≤65535,XRectangle 是 short 字段)。 */
static void hn_apply_input_shape(hnp_window *w, int on) {
    if (!w || !x11.dpy || !xshape_load()) return;
    if (on) {
        xshape.XShapeCombineRectangles(x11.dpy, w->xw, HN_XSHAPE_INPUT, 0, 0, NULL, 0,
                                       HN_XSHAPE_SET, HN_XSHAPE_UNSORTED);
    } else {
        xrect_stub r;
        r.x = 0; r.y = 0;
        r.width = (unsigned short)w->w;
        r.height = (unsigned short)w->h;
        xshape.XShapeCombineRectangles(x11.dpy, w->xw, HN_XSHAPE_INPUT, 0, 0, &r, 1,
                                       HN_XSHAPE_SET, HN_XSHAPE_UNSORTED);
    }
    x11.XFlush(x11.dpy);
}

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

    /* ---- 桌宠形态: desc 字段消费(契约见 hn_platform.h 字段注释;
       平台不支持时优雅忽略 —— 声明是意图, 物化看平台) ---- */

    /* 层级 + 跨空间: _NET_WM_STATE(XA_ATOM 数组, EWMH 规范原文
       "type ATOM[]")在 **map 前**写入 —— WM 首次管理窗口时读属性,
       无中途跳层闪烁。floating(1)→ABOVE(悬浮于普通窗口之上);
       desktop(2)→BELOW(EWMH 的最近似物, 普通窗口之下 —— 精确的
       "壁纸之上/图标之下"桌面层是各 WM 私有协议, 见能力矩阵注释)。 */
    xatom states[2];
    int nstates = 0;
    if (d->always_on_top == 1) states[nstates++] = x11.net_wm_state_above;
    else if (d->always_on_top == 2) states[nstates++] = x11.net_wm_state_below;
    if (d->join_all_spaces) states[nstates++] = x11.net_wm_state_sticky;
    if (nstates && x11.XChangeProperty && x11.net_wm_state && x11.atom_atom)
        x11.XChangeProperty(x11.dpy, w->xw, x11.net_wm_state, x11.atom_atom, 32,
                            X_PROP_REPLACE, (unsigned char *)states, nstates);

    /* borderless: _MOTIF_WM_HINTS —— 结构 = 5 个 long(flags/functions/
       decorations/input_mode/status), format=32 按 long 数组写 5 元素;
       属性类型就是 "_MOTIF_WM_HINTS" 原子本身(Motif 约定)。flags 只开
       DECORATIONS 位, decorations 字段 = 0 = 不画任何装饰(无边框无
       标题栏; 常量引证见文件头常量块)。 */
    if (d->borderless && x11.XChangeProperty && x11.motif_wm_hints) {
        unsigned long hints[5] = { 0 };
        hints[0] = HN_MWM_HINTS_DECORATIONS;   /* flags: decorations 字段有效 */
        /* hints[2] = decorations = 0(零初始化即无装饰) */
        x11.XChangeProperty(x11.dpy, w->xw, x11.motif_wm_hints, x11.motif_wm_hints,
                            32, X_PROP_REPLACE, (unsigned char *)hints, 5);
    }

    /* d->no_shadow: X11 无标准对应 —— 投影由 compositor 私有协议决定
       (EWMH 未定义), macOS setHasShadow: 的对应物不存在。契约即"优雅
       忽略"; 与 macOS 的差异记录在 hn_platform.h 平台能力注释。 */
    (void)d->no_shadow;

    /* x/y 摆位(几何记忆开窗入参): 内容区(client window)左上原点, 与
       hnp_window_origin 读回同侧。双 AUTO = 维持现状(WM 自行摆位);
       非 AUTO 时 XMoveWindow 两发: map 前(WM 尊重初始位置则不闪)+
       map 后(ConfigureRequest, 复刻 WM 几乎必尊)。frame extents 容差:
       x/y 是内容区口径, 与 macOS setFrame(外框口径)差 WM 装饰厚度;
       记忆往返同侧取样(读回/摆回都是内容区), 误差不累积。 */
    int move = (d->x != HNP_WINDOW_POS_AUTO || d->y != HNP_WINDOW_POS_AUTO);
    if (move) x11.XMoveWindow(x11.dpy, w->xw, d->x, d->y);

    w->gc = x11.XCreateGC(x11.dpy, w->xw, 0, NULL);
    if (!w->gc) { x11.XDestroyWindow(x11.dpy, w->xw); free(w); return NULL; }
    w->w = d->width; w->h = d->height;
    x11.XMapWindow(x11.dpy, w->xw);
    /* map 后补发: EWMH 状态第二条生效路径(不读属性的 WM) + 摆位的
       ConfigureRequest(robust 路径) + 初始鼠标穿透(幽灵模式初始态;
       运行期切换走 hnp_window_set_click_through)。 */
    for (int i = 0; i < nstates; i++) hn_ewmh_state_msg(w->xw, states[i]);
    if (move) x11.XMoveWindow(x11.dpy, w->xw, d->x, d->y);
    if (d->click_through) hn_apply_input_shape(w, 1);
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

/* 运行期鼠标穿透(桌宠幽灵模式): XShape 输入形状 —— on=1 整窗穿透,
   on=0 重设整窗矩形恢复接收(实现见 hn_apply_input_shape; 常量引证见
   文件头常量块)。libXext 缺席时优雅降级(无穿透, 声明照常通过)。
   幂等(重复设同一形状无副作用); NULL 安全。 */
void hnp_window_set_click_through(hnp_window *w, int on) {
    hn_apply_input_shape(w, on ? 1 : 0);
}

/* 几何记忆采样口: XTranslateCoordinates 把窗口内容区 (0,0) 平移到根窗口
   坐标, 即内容区左上原点的屏幕坐标(左上原点, y 向下 —— 与引擎/显示列表
   和 macOS 读 frame 同口径)。有 WM 装饰时与外框原点差 _NET_FRAME_EXTENTS
   (macOS 的 NSWindow frame 是外框口径); 读回与摆回同侧取样, 往返误差
   不累积。返回 1 = x/y 已写出; 0 = 拿不到(调用方整体跳过记忆)。NULL 安全。 */
int hnp_window_origin(hnp_window *w, int *x, int *y) {
    if (!w || !x || !y || !x11.dpy || !x11.XTranslateCoordinates) return 0;
    int rx = 0, ry = 0;
    xwindow child = 0;
    if (!x11.XTranslateCoordinates(x11.dpy, w->xw, x11.XDefaultRootWindow(x11.dpy),
                                   0, 0, &rx, &ry, &child))
        return 0;
    *x = rx;
    *y = ry;
    return 1;
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

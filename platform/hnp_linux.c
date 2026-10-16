/* hnp_linux.c — Linux 平台实现(dlopen X11/Xext, 零编译期依赖)
 *
 * hn_platform.h 的 Linux 实现。X11 经运行期 dlopen("libX11.so.6"),
 * 手工声明用到的最小 Xlib 函数/结构(ABI 布局依据与断言见 hnweb_linux.c,
 * 本文件复用同一套已验证的 stub)。
 * 窗口形态/桌宠能力(层级/跨空间/无边框/鼠标穿透/位置记忆)与 macOS 侧
 * 同契约: EWMH(_NET_WM_STATE)+ Motif(_MOTIF_WM_HINTS)+ XShape 输入形状,
 * 全部 dlopen 运行期解析; 常量逐值抄录见下方常量块注释(同 hnp_macos.c
 * 的"不凭记忆写常量"纪律)。libXext(XShape)缺席时 click_through 优雅
 * 降级为无穿透(声明照常接受, 物化看平台)。原生视频图层(文件尾)同款
 * 手法跑 XVideo 扩展(libXv): 缺席/无可用端口时 attach 如实返回 0,
 * 调用方回退 CPU 逐帧。
 *
 * 与 macOS 侧的对齐矩阵(能对齐的对齐, 不能的如实留位; hn_platform.h
 * 平台能力注释为总表, 此处是 X11 侧的逐项口径):
 *   内容拖拽(borderless/transparent 窗, hn-draggable 默认开)—— macOS 走
 *     NSWindow.movableByWindowBackground(壳零代码); X11 无对应机制,
 *     hnp_pump 手动等价物: 按下记锚 + XGrabPointer + MotionNotify 跟手
 *     XMoveWindow(见 XE_BUTTONPRESS 分支)。命中过滤与 macOS 同口径:
 *     全透明像素不发起拖拽、不进引擎(见"逐像素命中过滤"段)。
 *   逐像素点击穿透 —— macOS 由 WindowServer 对非不透明窗口做系统级
 *     逐像素路由; X11 只能靠 XShape 输入形状按 alpha 掩码重建矩形集
 *     (每 blit 一次, 性能/复杂度都重, 本轮留位)。v1 只有引擎出口过滤,
 *     差异 = 原生点击落在透明处时窗口仍接收/聚焦(无 OS 级穿透)。
 *   跨空间组合 —— macOS 桌宠组合含 FullScreenAuxiliary(全屏空间旁边
 *     可见, NSWindow.h:146); EWMH 无等价物(全屏空间旁浮窗是 WM/合成器
 *     私有协议), _NET_WM_STATE_STICKY 只覆盖普通空间 —— 已知差异。
 *   no_shadow —— compositor 私有(EWMH 未定义), 优雅忽略(见开窗注释)。
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
/* XButtonEvent/XMotionEvent/XKeyEvent 公共前缀(LP64): 字段偏移必须与
 * 真实 Xlib.h 逐一对齐 —— window=32 root=40 subwindow=48 time=56
 * x/y=64/68 x_root/y_root=72/76 state=80, 之后是 button/keycode/is_hint
 * (这里统一叫 detail)与 same_screen。漏 root 会让其后所有字段错位 8
 * 字节(2026-10-14: 拖拽锚点/掩码判定/命中过滤全读到错位数据, 已修)。 */
typedef struct { int type; unsigned long serial; int send_event; xdisplay *display;
                 xwindow window; xwindow root; xwindow subwindow;
                 unsigned long time;
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
/* 视频图层子窗的"位置+尺寸"一步摆位(XMoveWindow 只能动位置; 图层盒
   跟随布局/滚动时位置与尺寸常一起变, 单独一发比两发少一次中间态) */
typedef int (*fn_XMoveResizeWindow_t)(xdisplay *, xwindow, int, int,
                                      unsigned int, unsigned int);
typedef int (*fn_XTranslateCoordinates_t)(xdisplay *, xwindow, xwindow, int, int,
                                          int *, int *, xwindow *);
typedef int (*fn_XSendEvent_t)(xdisplay *, xwindow, int, long, xclient_stub *);
typedef int (*fn_XChangeProperty_t)(xdisplay *, xwindow, xatom, xatom, int, int,
                                    const unsigned char *, int);
/* 内容拖拽(borderless 窗手动拖动; X.h 常量引证见下方常量块) */
typedef int (*fn_XGrabPointer_t)(xdisplay *, xwindow, int, unsigned int, int, int,
                                 xwindow, unsigned long, unsigned long);
typedef int (*fn_XUngrabPointer_t)(xdisplay *, unsigned long);

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
   所需; 常规事件路由按 XE_* 枚举, 与上面 enum 不冲突)。

   X.h 指针抓取/按键掩码(逐值抄录, /opt/homebrew/include/X11/X.h =
   xorgproto 2025.1; 内容拖拽用):
     ButtonReleaseMask  = 1L<<3 (:154)   —— evmask 已含
     PointerMotionMask  = 1L<<6 (:157)   —— evmask 已含
     Button1Mask        = 1<<8   (:246)  —— MotionNotify.state 的"左键按住"位
     GrabModeAsync      = 1      (:319)  —— 抓取不冻结指针/键盘
     GrabSuccess        = 0      (:323)
     CurrentTime        = 0L     (:139)  —— 抓取/解抓的时间戳"现在"哨兵 */
#define X_PROP_REPLACE 0            /* PropModeReplace(X11/X.h) */
#define HN_MWM_HINTS_DECORATIONS (1UL << 1)
#define HN_XSHAPE_INPUT    2
#define HN_XSHAPE_SET      0
#define HN_XSHAPE_UNSORTED 0
#define HN_SUBSTRUCT_NOTIFY   (1L << 19)
#define HN_SUBSTRUCT_REDIRECT (1L << 20)
#define HN_BTN1_MASK     (1 << 8)      /* Button1Mask: MotionNotify.state 左键位 */
#define HN_GRAB_ASYNC    1             /* GrabModeAsync */
#define HN_GRAB_SUCCESS  0
#define HN_CURRENT_TIME  0L
/* 抓取期间要收的事件: ButtonPressMask 1L<<2 | ButtonReleaseMask 1L<<3 |
   PointerMotionMask 1L<<6(X.h 逐值见上)。 */
#define HN_GRAB_EVMASK   ((1L << 2) | (1L << 3) | (1L << 6))

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
    fn_XMoveResizeWindow_t XMoveResizeWindow;
    fn_XTranslateCoordinates_t XTranslateCoordinates;
    fn_XSendEvent_t XSendEvent;
    fn_XChangeProperty_t XChangeProperty;
    fn_XGrabPointer_t XGrabPointer;
    fn_XUngrabPointer_t XUngrabPointer;
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
    SYM(XMoveResizeWindow, fn_XMoveResizeWindow_t);
    SYM(XTranslateCoordinates, fn_XTranslateCoordinates_t);
    SYM(XSendEvent, fn_XSendEvent_t);
    SYM(XChangeProperty, fn_XChangeProperty_t);
    SYM(XGrabPointer, fn_XGrabPointer_t);
    SYM(XUngrabPointer, fn_XUngrabPointer_t);
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
    unsigned char *buf;     /* XImage data(预乘 BGRA, 原点左上) */
    int w, h;
    int transparent;        /* hn-transparent: 逐像素 alpha 命中过滤的开关 */
    int bw, bh;             /* 最近一次 blit 的位图尺寸(alpha 采样口径) */
    char utf8[8];
    int depth;
    /* 内容拖拽(无标题栏窗口的手动拖动, 对齐 macOS movableByWindowBackground;
       见 hnp_pump 的 XE_BUTTONPRESS 分支): borderless/transparent 开窗时置
       draggable; drag_on = 左键按住的拖动进行中; off_* = 按下时指针根坐标
       与窗口原点的偏移(拖动期间保持不动)。 */
    int draggable, drag_on, drag_off_x, drag_off_y;
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

/* ================= 逐像素命中过滤(与 macOS 对齐的引擎出口收口) =================

   契约(hn_platform.h hn-transparent): 全透明像素穿透到桌面, 半透明可点。
   X11 与 macOS 的能力差异(诚实口径):
     macOS 是 WindowServer 对非不透明窗口做**逐像素**输入路由 —— 透明处
     点击在系统层就不投递; X11 **没有**逐像素输入路由, XShape 输入形状
     可以做到, 但要每 blit 后按 alpha 掩码重建 ShapeInput 矩形集(性能/
     复杂度都重), 本轮不做(留位: 形状重建挂在 hnp_window_blit 尾部)。
   v1 的对齐面 = **引擎出口过滤**(与 hnp_macos.c 的机制③同语义): 透明
   窗口里全透明像素上的按下不进引擎、不发起拖拽 —— "透明处点击不落引擎/
   不拖窗"由本文件保证, 不依赖 WM。差异(窗口仍会接收/聚焦透明处的原生
   点击)是 X11 无逐像素路由的已知残留, 随上面的留位一并收口。
   采样源 = blit 拷进 w->buf 的预乘 BGRA(原点左上; alpha 在小端第 4 字节,
   offset 3 —— 与 hnp_macos.c 同一布局口径)。 */

/* 内容区坐标(引擎口径)→ 帧缓冲 alpha。非透明窗/尚无首帧: 恒 255
   (过滤永不触发, 与旧行为逐位一致)。 */
static unsigned char hn_alpha_at(hnp_window *w, int x, int y) {
    if (!w || !w->transparent || !w->buf || w->bw <= 0 || w->bh <= 0) return 255;
    if (x < 0) x = 0;
    if (y < 0) y = 0;
    if (x >= w->bw) x = w->bw - 1;
    if (y >= w->bh) y = w->bh - 1;
    return w->buf[((size_t)y * (size_t)w->bw + (size_t)x) * 4 + 3];
}

/* X 事件窗口 → hnp_window(进程窗口少, 线性扫表)。 */
static hnp_window *hn_hw_from_xwindow(xwindow xw) {
    for (int i = 0; i < g_window_n; i++)
        if (g_windows[i] && g_windows[i]->xw == xw) return g_windows[i];
    return NULL;
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
    w->transparent = d->transparent ? 1 : 0;   /* 逐像素命中过滤的开关 */
    /* 内容拖拽启用面(对齐 macOS setMovableByWindowBackground): borderless
       显式无边框之外, transparent 也隐含无边框(EWMH/Motif 装饰是否真去掉
       看 WM, 但语义同为"无标题栏可拖")。启用还要过 manifest 的 hn-draggable
       总开关(默认开, 页面 hn-draggable="0" 显式关闭 —— 2026-10-14 复审前
       本字段解析后无消费者, 两平台都硬编码 borderless||transparent 即拖);
       拖动实现见 hnp_pump 的 XE_BUTTONPRESS 分支(命中过滤: 全透明像素
       不发起拖拽)。 */
    w->draggable = d->draggable && (d->borderless || d->transparent);
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

/* 视频图层清理(实现见文件尾"原生视频图层"段; 声明提前 —— 窗口关闭
   要先摘图层子窗: 它依赖父窗与 GC 还活着)。 */
static void hn_xvl_close_window(hnp_window *w);

void hnp_window_close(hnp_window *w) {
    if (!w) return;
    hn_xvl_close_window(w);   /* 先摘视频图层(子窗/缓冲/端口引用) */
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
    /* buf 是 XImage 的 data(XDestroyImage 连带释放)—— 指针必须一并清空:
       resize 后到下一次 blit 之间 alpha 采样若读到悬垂指针就是 use-after-free
       (透明像素命中过滤读 buf; 清空后 hn_alpha_at 走"无帧 → 恒 255"分支)。 */
    w->buf = NULL;
}

void hnp_window_blit(hnp_window *w, const unsigned char *bgra_pre, int bw, int bh, size_t stride) {
    if (!w || !bgra_pre || !x11.dpy) return;
    w->bw = bw; w->bh = bh;   /* alpha 采样的位图口径(逐像素命中过滤用) */
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

/* GPU 直呈层: X11 路径暂无 Metal 层宿主, 恒 NULL(壳回落 blit 路径)。 */
void *hnp_window_attach_metal_layer(hnp_window *w, int phys_w, int phys_h) {
    (void)w; (void)phys_w; (void)phys_h; return NULL;
}
void hnp_window_metal_layer_resize(hnp_window *w, int phys_w, int phys_h) {
    (void)w; (void)phys_w; (void)phys_h;
}

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

/* 设备像素比: X11 没有统一的 backing scale 概念(HiDPI 靠 Xft.dpi/缩放
   由 WM/工具包各自为政), 恒 1.0 —— 位图与窗口像素 1:1, 语义不变。
   **无 HNP_EV_SCALE_CHANGED 产出**(永不变化, 契约见 hn_platform.h)。 */
float hnp_window_scale(hnp_window *w) {
    (void)w;
    return 1.0f;
}

/* 窗口移动: XMoveWindow(内容区左上角; 引擎左上原点 y 向下与 X 根窗口
   同系, 无需翻转 —— 与 hnp_window_origin 的 XTranslateCoordinates 同口径)。
   Xlib 缺席/窗口未建 → 静默(声明照常通过)。 */
void hnp_window_move(hnp_window *w, int x, int y) {
    if (!w || !x11.dpy || !w->xw) return;
    if (x11.XMoveWindow) x11.XMoveWindow(x11.dpy, w->xw, x, y);
    if (x11.XFlush) x11.XFlush(x11.dpy);
}

/* 可见区查询: Linux v1 未接 Xinerama/RandR(多屏布局各 WM 口径不一),
   返回 0 —— 调用方跳过几何夹取(声明照常通过, 物化看平台; X11 全屏
   坐标本身没有"工作区"概念, EWMH 的 _NET_WORKAREA 是 WM 私有实现差异区)。
   与 hnp_window_scale 恒 1.0 同一"Linux 能力矩阵留位"口径。 */
int hnp_screen_workarea(hnp_window *w, int px, int py,
                        int *vx, int *vy, int *vw, int *vh) {
    (void)w; (void)px; (void)py; (void)vx; (void)vy; (void)vw; (void)vh;
    return 0;
}

/* IME/剪贴板: Linux v1 空实现(声明照常通过, 物化看平台)。
   真 IME 需 XIM/GTK immodule 运行期装载(路线图); 剪贴板可经 X11 selection
   协议接(同样路线图)—— 都留待输入后端落地时一并接。**无 IME 事件
   产出**(HNP_EV_IME_* / FOCUS_* / SCREEN_CHANGED 在 Linux 侧永不产生,
   引擎侧无预编辑状态, 行为与旧版逐位一致)。 */
void hnp_ime_focus(hnp_window *w, int x, int y, int iw, int ih) {
    (void)w; (void)x; (void)y; (void)iw; (void)ih;
}
int hnp_clipboard_get(hnp_window *w, char *out, size_t cap) {
    (void)w; (void)out; (void)cap;
    return 0;
}
int hnp_clipboard_set(hnp_window *w, const char *utf8) {
    (void)w; (void)utf8;
    return 0;
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
    case 0xFF63: return HNP_KEY_INSERT;      /* XK_Insert */
    case 0xFFE5: return HNP_KEY_CAPS_LOCK;   /* XK_Caps_Lock */
    case 0xFF7F: return HNP_KEY_NUM_LOCK;    /* XK_Num_Lock */
    case 0xFF14: return HNP_KEY_SCROLL_LOCK; /* XK_Scroll_Lock */
    case 0xFF13: return HNP_KEY_PAUSE;       /* XK_Pause */
    case 0x0020: return HNP_KEY_SPACE;       /* XK_space */
    case 0xFF8D: return HNP_KEY_ENTER_NUMPAD;/* XK_KP_Enter */
    default: return HNP_KEY_PRINTABLE;
    }
}

/* 契约导出(hn_platform.h): 与事件路径同一张表(单源)。 */
hnp_key hnp_key_from_native(unsigned long native_code) {
    return keysym_to_hnp(native_code);
}

int hnp_pump(hnp_event *ev, int timeout_ms) {
    if (!ev || !x11.dpy) return 0;
    ev->kind = HNP_EV_NONE;
    ev->utf8 = NULL;
    /* select 等 X 连接可读(带超时); 或立即返回。
       timeout_ms == 0 = 非阻塞排干 —— 传 NULL 超时给 select 是**无限等待**
       (复审修复: 此前 0 走 NULL 分支, 与"立即返回"语义相反, 队尾零超时
       轮询会卡死帧循环)。 */
    if (x11.XPending(x11.dpy) == 0 && timeout_ms >= 0 && timeout_ms < 0x7FFFFFFF) {
        int fd = x11.XConnectionNumber(x11.dpy);
        fd_set rf;
        FD_ZERO(&rf);
        FD_SET(fd, &rf);
        struct timeval tv = { timeout_ms / 1000, (timeout_ms % 1000) * 1000 };
        if (select(fd + 1, &rf, NULL, NULL, &tv) == 0)
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
        hnp_window *hw = hn_hw_from_xwindow(((xany_stub *)&xb)->window);
        /* 逐像素命中过滤(见"逐像素命中过滤"段注释): 透明窗口里全透明
           像素上的**按下**丢弃 —— 不进引擎, 也不作为拖拽起点(半透明
           照常; 非透明窗/无帧时 alpha 恒 255, 行为不变)。抬起不过滤:
           按下在可见像素、滑出透明区再抬起的配对不能拆散。 */
        if (hw && type == XE_BUTTONPRESS && hn_alpha_at(hw, dv->x, dv->y) == 0)
            return 0;
        /* 内容拖拽(对齐 macOS movableByWindowBackground —— 该壳在 macOS
           由 NSWindow 收按下后自动拖窗; X11 没有, 手动等价物):
             按下(左键, 可拖窗, 命中可见像素) → 记指针-窗口偏移并
             XGrabPointer(owner_events=1: 事件照常投递本进程; 快速拖动
             出窗后 MotionNotify 不断流);
             移动(左键按住) → XMoveWindow 按偏移跟手;
             抬起 → 结束并解抓。抓取失败不阻断按下事件本身。 */
        if (hw && type == XE_BUTTONPRESS && hw->draggable && dv->detail == 1) {
            int ox = 0, oy = 0;
            xwindow child = 0;
            if (hw->drag_on) return 0;   /* 重复按下(已抓取中): 不重锚 */
            if (x11.XTranslateCoordinates(x11.dpy, hw->xw,
                                          x11.XDefaultRootWindow(x11.dpy),
                                          0, 0, &ox, &oy, &child) &&
                x11.XGrabPointer(x11.dpy, hw->xw, 1, HN_GRAB_EVMASK,
                                 HN_GRAB_ASYNC, HN_GRAB_ASYNC, 0, 0,
                                 HN_CURRENT_TIME) == HN_GRAB_SUCCESS) {
                hw->drag_on = 1;
                hw->drag_off_x = dv->x_root - ox;
                hw->drag_off_y = dv->y_root - oy;
            }
        }
        if (hw && type == XE_BUTTONRELEASE && dv->detail == 1 && hw->drag_on) {
            hw->drag_on = 0;
            x11.XUngrabPointer(x11.dpy, HN_CURRENT_TIME);
        }
        ev->kind = (type == XE_BUTTONPRESS) ? HNP_EV_BUTTON_DOWN : HNP_EV_BUTTON_UP;
        ev->x = dv->x; ev->y = dv->y;
        ev->button = (dv->detail == 1) ? 0 : (dv->detail == 3) ? 2 : 1;
        return 1;
    }
    case XE_MOTIONNOTIFY: {
        xdevice_stub *dv = (xdevice_stub *)&xb;
        hnp_window *hw = hn_hw_from_xwindow(((xany_stub *)&xb)->window);
        /* 拖动跟手: 左键按住的拖动进行中 → 窗口原点 = 指针根坐标 − 偏移
           (指针根坐标在抓取下不因出窗失真)。非拖动移动照常上报引擎。 */
        if (hw && hw->drag_on && (dv->state & HN_BTN1_MASK)) {
            x11.XMoveWindow(x11.dpy, hw->xw,
                            dv->x_root - hw->drag_off_x,
                            dv->y_root - hw->drag_off_y);
            x11.XFlush(x11.dpy);   /* 跟手要即发(XPending 的隐式 flush 在下一拍) */
        }
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

/* ---------------- GPU 渲染后端(v1 无 GPU 路径) ----------------
   Linux v1 无 GPU 原语表: 返回 NULL —— 呼出方(引擎经 hn_rt_set_gpu_host
   注入槽)据此整体回退 hnsoft(docs/platforms.md 实现矩阵: Linux 的
   GL/mesa 后端是路线图; 与 hnp_media_supported()==0 的 ABI 留位同一口径)。 */
const hnp_gpu_host *hnp_gpu_backend(void) { return NULL; }

/* ---------------- 原生视频图层(X11 对应物: XVideo 扩展, libXv 运行期 dlopen) ----------------
 * hn_platform.h 尾部契约(hnp_video_layer_attach/frame/detach)的 Linux 实现。
 * macOS 侧是 AVPlayerLayer(系统合成器直通); X11 的对应物是 **XVideo 扩展**
 * —— 服务器侧做硬件缩放/颜色空间转换的视频 blitter, 最接近"图层原语"语义
 * 的路径。仍按平台铁律运行期 dlopen("libXv.so.6"), 编译期零依赖(交叉产物
 * strings 里只有 dlsym 名字, 见 linux_parity_check.sh 的视频图层断言组)。
 *
 * 三原语的 X11 形态(本轮 = 图层原语, 不是"视频播放器"):
 *   attach = grab 端口 + 建图层**子窗口** + XvCreateImage(YV12/I420 帧缓冲,
 *            预填视频黑) + 首次 XvPutImage;
 *   frame  = 盒变化时 XMoveResizeWindow 子窗(位置+尺寸一发), 尺寸变化时
 *            重建 XvImage; 每拍 XvPutImage 摆图 —— X11 没有替我们推进画面
 *            的合成器, 摆图就是"内容推进通道": Linux 解码后端落地后往
 *            img->data 写 YV12 帧即可, 本拍摆图自然上屏(**原语即插即用**);
 *            无解码器时重复摆同一黑帧, 开销一次 blit;
 *   detach = XvStopVideo + 销毁子窗 + 释放缓冲; 末层摘除时 XvUngrabPort。
 *
 * 图层层级对齐(与 macOS "sublayer 在位图图层之上"同语义): 图层 = 父窗的
 * 子窗口 —— X11 子窗恒绘制在父窗自身内容之上, 且父窗 XPutImage 被子窗
 * 矩形裁剪(服务器语义, 免维护遮挡表)。子窗不选任何输入事件: 指针事件按
 * X11 规则上浮父窗(坐标已平移), 引擎命中测试口径不变 —— 对应
 * AVPlayerLayer 不吃事件。父窗 resize/expose 后调用方每拍 frame() 跟随;
 * 子窗无 ExposureMask, 内容丢失(最小化恢复等)靠下一拍摆图补 —— 与
 * "EXPOSE 时重 blit"同一节拍。
 *
 * 降级链(诚实口径): libXv 缺席 / 扩展不可用 / 无带 XvImageMask 的适配器 /
 * 端口全被占 / YV12+I420 都不被端口接受 → attach 返回 0, 调用方
 * (app/hn_media_platform.c)自然回退 CPU 逐帧路径。**不**在本轮自造
 * MIT-SHM 视频管线 —— SHM 位图路径是 v2 备选(此注释即留位)。
 *
 * Linux 侧暂无解码后端(hnp_media_supported()==0, 无 player 注册表): 本原语
 * 以 player_id 为图层键自洽成表(同 macOS 的 (win,id) 唯一口径), 不经弱
 * 符号查询口。合成媒体宿主(hnapp-linux)的梯度帧仍走 CPU 位图路径, 被本
 * 图层子窗盖住 —— 解码后端落地前, X11 真窗的视频盒呈现"视频黑", 如实
 * 反映"图层在、解码未接"。
 */

/* Xv 常量(逐值抄录, 不凭记忆 —— 同文件头纪律):
   Xv.h 能力/方向掩码: XvInputMask=1<<0, XvOutputMask=1<<1(方向位);
   XvVideoMask=1<<2, XvImageMask=1<<3, XvAudioMask=1<<4(能力位)。
   本侧只要 image 能力(XvImage blitter), 不碰 XvPutVideo 视频流端口。 */
#define HN_XV_IMAGE_MASK (1L << 3)
/* X.h: Success = 0, CurrentTime = 0L(时间戳"现在"哨兵, grab/ungrab 用) */
#define HN_X_SUCCESS    0
#define HN_XV_CURRENT_TIME 0L
/* YUV/RGB 四元码(FourCC): 字节序 = 'a' 在低字节。只认 YV12/I420 ——
   两者同为"全分辨率 Y + 两个半分辨率 U/V 平面", 仅 U/V 摆放次序不同,
   本原语对两者统一按 planes/pitches/offsets 填黑, 不关心次序。 */
#define HN_XV_FOURCC(a, b, c, d) \
    ((int)(((unsigned)(unsigned char)(a)) | (((unsigned)(unsigned char)(b)) << 8) | \
           (((unsigned)(unsigned char)(c)) << 16) | (((unsigned)(unsigned char)(d)) << 24)))

/* ---- 最小 Xv ABI(抄录 X11/extensions/Xvlib.h 布局; 同 Xlib stub 的
   "手工声明最小面"手法)。XvPortID = XID = unsigned long(LP64)。 ---- */
typedef unsigned long xport_stub;

/* XvImageFormatInfo(88 字节, 全 4 字节对齐无内衬)—— 本侧只声明布局备用,
   判定走 XvCreateImage 的服务器校验(见 xvl_acquire), 不读此结构字段 */
typedef struct {
    int id; int type; int byte_order; char guid[16];
    int bits_per_pixel; int format; int num_planes; int depth;
    unsigned int red_mask, green_mask, blue_mask;
    unsigned int y_sample_bits, u_sample_bits, v_sample_bits;
    unsigned int horz_y_period, horz_u_period, horz_v_period;
    unsigned int vert_y_period, vert_u_period, vert_v_period;
    char component_order[4]; int scanline_order;
} xvfmt_stub;

/* XvAdaptorInfo(LP64 布局: base_id 0 / num_ports 8 / type 16(+7 内衬) /
   name 24 / num_encodings 32 / encodings 40 / num_formats 48 / formats 56 /
   max_width 64 / max_height 68 / put_flags 72 / get_flags 80)。
   只依赖头部三字段(base_id/num_ports/type)—— 它们无内衬歧义; 尾部能力位
   与格式表的抄录差异不进判定路径(纪律: 只读最有把握的 ABI 面)。 */
typedef struct {
    xport_stub base_id;
    unsigned long num_ports;
    char type;                     /* XvInput/Output/Video/Image/AudioMask 位集 */
    char *name;
    unsigned long num_encodings;
    void *encodings;
    unsigned long num_formats;
    xvfmt_stub *formats;
    int max_width, max_height;
    unsigned long put_flags, get_flags;
} xvadaptor_stub;

/* XvImage(XvCreateImage 产出; 56 字节: id 0 / width 4 / height 8 /
   data_size 12 / num_planes 16(+4 内衬) / pitches 24 / offsets 32 /
   data 40 / obdata 48)。data 缓冲由调用方 malloc(XvCreateImage 只给
   data_size), 平面几何经 pitches/offsets 取 —— 不假设 YV12 定长行距。 */
typedef struct {
    int id;
    int width, height;
    int data_size;
    int num_planes;
    int *pitches;
    int *offsets;
    char *data;
    void *obdata;
} xvimage_stub;

typedef unsigned int (*fn_XvQueryExtension_t)(xdisplay *, unsigned int *, unsigned int *,
                                              unsigned int *, unsigned int *, unsigned int *);
typedef int (*fn_XvQueryAdaptors_t)(xdisplay *, xwindow, unsigned long *, xvadaptor_stub **);
typedef int (*fn_XvGrabPort_t)(xdisplay *, xport_stub, unsigned long /*Time*/);
typedef int (*fn_XvUngrabPort_t)(xdisplay *, xport_stub, unsigned long);
typedef xvimage_stub *(*fn_XvCreateImage_t)(xdisplay *, xport_stub, int, char *, int, int);
typedef int (*fn_XvPutImage_t)(xdisplay *, xport_stub, xwindow, void * /*GC*/, xvimage_stub *,
                               int, int, unsigned int, unsigned int,
                               int, int, unsigned int, unsigned int);
typedef int (*fn_XvStopVideo_t)(xdisplay *, xport_stub, xwindow);
typedef void (*fn_XvFreeAdaptorInfo_t)(xvadaptor_stub *);
/* 从 libX11 借的两个口(XvImage 结构释放 / X 错误处理器装配) */
typedef int (*fn_XFree_t)(void *);
typedef void *(*fn_XSetErrorHandler_t)(int (*)(xdisplay *, void *));

static struct {
    void *lib;
    fn_XvQueryExtension_t XvQueryExtension;
    fn_XvQueryAdaptors_t XvQueryAdaptors;
    fn_XvGrabPort_t XvGrabPort;
    fn_XvUngrabPort_t XvUngrabPort;
    fn_XvCreateImage_t XvCreateImage;
    fn_XvPutImage_t XvPutImage;
    fn_XvStopVideo_t XvStopVideo;
    fn_XvFreeAdaptorInfo_t XvFreeAdaptorInfo;
    fn_XFree_t XFree;
    fn_XSetErrorHandler_t XSetErrorHandler;
    unsigned long port;    /* 进程已 grab 的端口(多图层共享; 端口是服务器稀缺资源) */
    int port_valid;
} xvideo;

/* 图层表(同 macOS g_vl 口径: 进程窗口少, 定长表够用)。容量比 macOS 的 8
   收窄到 4 —— Xv 端口更稀缺, 超出时 attach 返回 0 优雅退 CPU 逐帧。 */
#define HN_XVL_CAP 4
static struct {
    hnp_window *win;
    char id[256];
    xwindow child;              /* 图层子窗口(XvPutImage 的摆位目标) */
    xvimage_stub *img;          /* XvImage(XFree 释放; data 缓冲另管) */
    unsigned char *buf;         /* img->data(解码后端落地后的写帧入口) */
    int iw, ih;                 /* 当前图像尺寸(重建判据) */
    int lx, ly, lw, lh;         /* 最近应用的盒(同帧跳过 move/resize/重建) */
} g_xvl[HN_XVL_CAP];
static int g_xvl_n;
static int g_xvl_tried;         /* libXv 装配只试一次: 失败(缺库)不再每会话重试 */

/* 诊断行开关(与 macOS 侧同一环境钮, 记忆化) */
static int hn_xvl_trace(void) {
    static int t = -1;
    if (t < 0) {
        const char *e = getenv("HN_MEDIA_TRACE");
        t = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return t;
}

static int hn_xvl_round(float v) { return (int)(v + (v < 0 ? -0.5f : 0.5f)); }

static int xvl_find(hnp_window *w, const char *id) {
    for (int i = 0; i < g_xvl_n; i++)
        if (g_xvl[i].win == w && strcmp(g_xvl[i].id, id) == 0) return i;
    return -1;
}

/* X 错误处理器(no-op): 图层路径的 Xv 请求(端口/格式/摆位)在驱动差异下
   难免瞬时 BadMatch —— 默认处理器会打印并 exit 杀掉整个进程, 装配图层
   路径时换成"忽略本请求"姿态(GTK/Qt 同款)。X 错误是异步投递的, 没法只
   围单个请求换处理器, 故进程级生效; 不启用视频图层(缺 libXv / 端口缺)
   不走到这里, 窗口路径行为不变。 */
static int hn_xv_ignore_error(xdisplay *d, void *err) { (void)d; (void)err; return 0; }

/* libXv 装配(懒, 一次): 缺库或缺任一符号返回 0(attach 层面降级)。
   XFree/XSetErrorHandler 从已装载的 libX11 借(窗口存在 ⇒ x11.lib 已在)。 */
static int xvl_load(void) {
    if (xvideo.lib) return 1;
    if (g_xvl_tried) return 0;
    g_xvl_tried = 1;
    if (!x11.lib || !x11.dpy) return 0;
    xvideo.lib = dlopen("libXv.so.6", RTLD_NOW);
    if (!xvideo.lib) xvideo.lib = dlopen("libXv.so", RTLD_NOW);
    if (!xvideo.lib) return 0;
    xvideo.XvQueryExtension = (fn_XvQueryExtension_t)dlsym(xvideo.lib, "XvQueryExtension");
    xvideo.XvQueryAdaptors  = (fn_XvQueryAdaptors_t)dlsym(xvideo.lib, "XvQueryAdaptors");
    xvideo.XvGrabPort       = (fn_XvGrabPort_t)dlsym(xvideo.lib, "XvGrabPort");
    xvideo.XvUngrabPort     = (fn_XvUngrabPort_t)dlsym(xvideo.lib, "XvUngrabPort");
    xvideo.XvCreateImage    = (fn_XvCreateImage_t)dlsym(xvideo.lib, "XvCreateImage");
    xvideo.XvPutImage       = (fn_XvPutImage_t)dlsym(xvideo.lib, "XvPutImage");
    xvideo.XvStopVideo      = (fn_XvStopVideo_t)dlsym(xvideo.lib, "XvStopVideo");
    xvideo.XvFreeAdaptorInfo = (fn_XvFreeAdaptorInfo_t)dlsym(xvideo.lib, "XvFreeAdaptorInfo");
    xvideo.XFree = (fn_XFree_t)dlsym(x11.lib, "XFree");
    xvideo.XSetErrorHandler = (fn_XSetErrorHandler_t)dlsym(x11.lib, "XSetErrorHandler");
    if (!xvideo.XvQueryExtension || !xvideo.XvQueryAdaptors || !xvideo.XvGrabPort ||
        !xvideo.XvUngrabPort || !xvideo.XvCreateImage || !xvideo.XvPutImage ||
        !xvideo.XvStopVideo || !xvideo.XvFreeAdaptorInfo || !xvideo.XFree)
        return 0;
    if (xvideo.XSetErrorHandler) xvideo.XSetErrorHandler(hn_xv_ignore_error);
    return 1;
}

/* YV12/I420 预填"视频黑"(BT.601 有限域): Y=16, U=V=128。平面几何逐行按
   pitches/offsets 走(驱动可能加行尾对齐, 不假设定长)。 */
static void xvl_fill_black(xvimage_stub *img) {
    if (!img || !img->data || !img->pitches || !img->offsets || img->num_planes != 3) return;
    unsigned char *d = (unsigned char *)img->data;
    for (int y = 0; y < img->height; y++)
        memset(d + (size_t)img->offsets[0] + (size_t)y * (size_t)img->pitches[0],
               16, (size_t)img->width);
    for (int p = 1; p < 3; p++)
        for (int y = 0; y < img->height / 2; y++)
            memset(d + (size_t)img->offsets[p] + (size_t)y * (size_t)img->pitches[p],
                   128, (size_t)(img->width / 2));
}

/* 在给定端口上建帧缓冲: YV12 → I420 依次试。格式匹配的**权威判定在 X
   服务器**(XvCreateImage 对 port 的图像格式表校验, 不匹配返回 NULL)——
   本侧不读 XvAdaptorInfo 格式表的抄录布局(见 xvadaptor_stub 注释)。
   返回图像并回填 malloc 的 data 缓冲; 失败返回 NULL。 */
static xvimage_stub *xvl_image_new(xport_stub port, int wpx, int hpx,
                                   unsigned char **buf_out) {
    static const int fcc[2] = { HN_XV_FOURCC('Y', 'V', '1', '2'),
                                HN_XV_FOURCC('I', '4', '2', '0') };
    for (int k = 0; k < 2; k++) {
        xvimage_stub *img = xvideo.XvCreateImage(x11.dpy, port, fcc[k], NULL, wpx, hpx);
        if (!img || img->data_size <= 0) {
            if (img) xvideo.XFree(img);
            continue;
        }
        unsigned char *buf = (unsigned char *)malloc((size_t)img->data_size);
        if (!buf) { xvideo.XFree(img); return NULL; }
        img->data = (char *)buf;
        xvl_fill_black(img);
        *buf_out = buf;
        return img;
    }
    return NULL;
}

/* 拿"端口 + 帧": 优先复用进程已 grab 的端口(端口稀缺, 不因单格式不匹配
   轻易换); 无端口则遍历带 XvImageMask 的适配器逐端口 grab, 建不出
   YV12/I420 的端口立即 ungrab 换下一个。全部失败返回 0(attach 层面
   如实报不支持, 调用方回退 CPU 逐帧)。 */
static int xvl_acquire(hnp_window *w, int wpx, int hpx,
                       unsigned char **buf_out, xvimage_stub **img_out) {
    if (xvideo.port_valid) {
        xvimage_stub *img = xvl_image_new(xvideo.port, wpx, hpx, buf_out);
        if (!img) return 0;
        *img_out = img;
        return 1;
    }
    unsigned long n = 0;
    xvadaptor_stub *ads = NULL;
    if (!xvideo.XvQueryAdaptors ||
        xvideo.XvQueryAdaptors(x11.dpy, w->xw, &n, &ads) != HN_X_SUCCESS)
        return 0;
    int got = 0;
    /* 上界防御: n 只驱动循环计数, 巨值视为服务器异常不遍历 */
    if (ads && n > 0 && n < 1024) {
        for (unsigned long a = 0; a < n && !got; a++) {
            xvadaptor_stub *ad = &ads[a];
            if (!(ad->type & HN_XV_IMAGE_MASK)) continue;
            for (unsigned long p = 0; p < ad->num_ports && !got; p++) {
                xport_stub port = ad->base_id + p;
                /* grab 返回值即结果(扩展错误码, 不走 X 错误通道): 0 = Success */
                if (xvideo.XvGrabPort(x11.dpy, port, HN_XV_CURRENT_TIME) != HN_X_SUCCESS)
                    continue;                     /* 被占/无效: 换下一个端口 */
                xvimage_stub *img = xvl_image_new(port, wpx, hpx, buf_out);
                if (img) {
                    xvideo.port = port;
                    xvideo.port_valid = 1;
                    *img_out = img;
                    got = 1;
                } else {
                    xvideo.XvUngrabPort(x11.dpy, port, HN_XV_CURRENT_TIME);
                }
            }
        }
    }
    xvideo.XvFreeAdaptorInfo(ads);
    return got;
}

int hnp_video_layer_attach(hnp_window *w, const char *player_id,
                           float x, float y, float fw, float fh) {
    if (!w || !w->xw || !w->gc || !x11.dpy || !player_id || !*player_id) return 0;
    if (fw <= 0 || fh <= 0) return 0;   /* 未布局/布局成 0: 不挂(macOS 同口径) */
    int i = xvl_find(w, player_id);
    if (i >= 0) {                       /* 已挂(热重载同 src): 重定位即帧路径 */
        hnp_video_layer_frame(w, player_id, x, y, fw, fh);
        return 1;
    }
    if (g_xvl_n >= HN_XVL_CAP) return 0;
    if (!xvl_load()) return 0;          /* libXv 缺席 → CPU 逐帧兜底 */
    unsigned int ver = 0, rev = 0, req = 0, evb = 0, erb = 0;
    if (!xvideo.XvQueryExtension(x11.dpy, &ver, &rev, &req, &evb, &erb)) return 0;
    int bw = hn_xvl_round(fw); if (bw < 1) bw = 1;
    int bh = hn_xvl_round(fh); if (bh < 1) bh = 1;
    int bx = hn_xvl_round(x), by = hn_xvl_round(y);   /* 存 round 值: 与 frame 的
                                                         同盒比较一致, 首拍即跳过多余摆位 */

    /* 图层子窗: 只设背景黑(CWBackPixel=1<<1; XSetWindowAttributes 的
       background_pixel 是第 2 字段 → attrs[1], buffer 已零初始化); 不选
       事件(输入上浮父窗, 见段注释), InputOutput 因 XvPutImage 要 Drawable。 */
    unsigned long attrs[16] = { 0 };
    attrs[1] = 0;                       /* background_pixel = 黑 */
    xwindow child = x11.XCreateWindow(x11.dpy, w->xw, bx, by,
                                      (unsigned)bw, (unsigned)bh,
                                      0, 0 /*CopyFromParent*/, 1 /*InputOutput*/, NULL,
                                      0x2 /*CWBackPixel*/, attrs);
    if (!child) return 0;
    x11.XMapWindow(x11.dpy, child);

    unsigned char *buf = NULL;
    xvimage_stub *img = NULL;
    if (!xvl_acquire(w, bw, bh, &buf, &img)) {
        x11.XDestroyWindow(x11.dpy, child);
        return 0;                        /* 无可用端口/格式: 如实不支持 */
    }
    /* 首摆: 图层画面立起来(视频黑) */
    xvideo.XvPutImage(x11.dpy, xvideo.port, child, w->gc, img,
                      0, 0, (unsigned)bw, (unsigned)bh, 0, 0, (unsigned)bw, (unsigned)bh);
    i = g_xvl_n++;
    g_xvl[i].win = w;
    snprintf(g_xvl[i].id, sizeof(g_xvl[i].id), "%s", player_id);
    g_xvl[i].child = child;
    g_xvl[i].img = img;
    g_xvl[i].buf = buf;
    g_xvl[i].iw = bw; g_xvl[i].ih = bh;
    g_xvl[i].lx = bx; g_xvl[i].ly = by;
    g_xvl[i].lw = bw; g_xvl[i].lh = bh;
    x11.XFlush(x11.dpy);
    if (hn_xvl_trace())
        fprintf(stderr, "[video-layer] attach id=%s box=%.0f,%.0f %.0fx%.0f port=%lu\n",
                player_id, (double)x, (double)y, (double)fw, (double)fh, xvideo.port);
    return 1;
}

void hnp_video_layer_frame(hnp_window *w, const char *player_id,
                           float x, float y, float fw, float fh) {
    if (!w || !player_id || !x11.dpy) return;
    int i = xvl_find(w, player_id);
    if (i < 0) return;                  /* 未 attach 过: 静默(调用方时序防御) */
    if (fw <= 0 || fh <= 0) return;     /* 未布局: 跳过(macOS vl_apply_frame 同款) */
    int bx = hn_xvl_round(x), by = hn_xvl_round(y);
    int bw = hn_xvl_round(fw); if (bw < 1) bw = 1;
    int bh = hn_xvl_round(fh); if (bh < 1) bh = 1;
    if (bx != g_xvl[i].lx || by != g_xvl[i].ly || bw != g_xvl[i].lw || bh != g_xvl[i].lh) {
        x11.XMoveResizeWindow(x11.dpy, g_xvl[i].child, bx, by, (unsigned)bw, (unsigned)bh);
        if (bw != g_xvl[i].iw || bh != g_xvl[i].ih) {
            /* 尺寸变了: XvImage 重建(v1 重填视频黑; 解码后端落地后由后端
               重填新缓冲)。重建失败沿用旧图 —— put 会把旧图缩放进新盒,
               一拍宽高比失真可容忍, 下一拍再试。 */
            unsigned char *buf = NULL;
            xvimage_stub *img = xvl_image_new(xvideo.port, bw, bh, &buf);
            if (img) {
                free(g_xvl[i].buf);
                if (g_xvl[i].img) xvideo.XFree(g_xvl[i].img);
                g_xvl[i].img = img;
                g_xvl[i].buf = buf;
                g_xvl[i].iw = bw;
                g_xvl[i].ih = bh;
            }
        }
        g_xvl[i].lx = bx; g_xvl[i].ly = by; g_xvl[i].lw = bw; g_xvl[i].lh = bh;
    }
    /* 每拍摆图 = 内容推进通道(见段注释); 子窗内容丢失的恢复也走这里 */
    if (xvideo.port_valid && g_xvl[i].img)
        xvideo.XvPutImage(x11.dpy, xvideo.port, g_xvl[i].child, w->gc, g_xvl[i].img,
                          0, 0, (unsigned)g_xvl[i].iw, (unsigned)g_xvl[i].ih,
                          0, 0, (unsigned)bw, (unsigned)bh);
}

void hnp_video_layer_detach(hnp_window *w, const char *player_id) {
    if (!w || !player_id) return;
    int i = xvl_find(w, player_id);
    if (i < 0) return;                  /* 幂等 */
    if (x11.dpy) {
        if (xvideo.XvStopVideo && xvideo.port_valid)   /* overlay 资源收尾(必为幂等) */
            xvideo.XvStopVideo(x11.dpy, xvideo.port, g_xvl[i].child);
        x11.XDestroyWindow(x11.dpy, g_xvl[i].child);
    }
    free(g_xvl[i].buf);
    if (xvideo.XFree && g_xvl[i].img) xvideo.XFree(g_xvl[i].img);
    g_xvl[i] = g_xvl[g_xvl_n - 1];
    g_xvl_n--;
    /* 末层摘除: 端口归还服务器(稀缺资源不留进程) */
    if (g_xvl_n == 0 && xvideo.port_valid && x11.dpy) {
        xvideo.XvUngrabPort(x11.dpy, xvideo.port, HN_XV_CURRENT_TIME);
        xvideo.port_valid = 0;
    }
    if (hn_xvl_trace())
        fprintf(stderr, "[video-layer] detach id=%s\n", player_id);
}

/* 窗口关闭的图层清理(hnp_window_close 开头调用; 此时父窗/GC 还在) */
static void hn_xvl_close_window(hnp_window *w) {
    int again = 1;
    while (again) {                     /* detach 是 swap-removal: 命中即重扫 */
        again = 0;
        for (int i = 0; i < g_xvl_n; i++) {
            if (g_xvl[i].win == w) {
                hnp_video_layer_detach(w, g_xvl[i].id);
                again = 1;
                break;
            }
        }
    }
}

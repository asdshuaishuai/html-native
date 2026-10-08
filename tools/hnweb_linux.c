/* hnweb_linux.c — Linux 运行时: X11 窗口 + hnsoft 软件光栅(M1 + M2)
 *
 * 定位: tools/hnwin.c 的 Linux 镜像 —— 引擎(C99, 平台无关)之上的第一层
 * Linux 表面物化, 也是 tools/hnweb.h 门面的平台实现:
 *   解析 → 级联 → 布局 → display list → hnsoft RGBA 位图 → XImage 上屏
 * 同一条管线, 换的只是窗口/事件/像素搬运这一层(Win32 DIB ↔ XImage ZPixmap)。
 *
 * 零编译期依赖: Xlib 不 include 头文件 —— 运行期 dlopen("libX11.so.6")
 * (回退 libX11.so), 本文件手抄用到的最小 Xlib 声明。所有结构按 X11 数十年
 * 未变的 ABI 逐字段复刻并 _Static_assert 钉死(LP64; ILP32 会让断言在编译期
 * 爆掉, 与 x86_64/aarch64 musl 交叉口径一致)。XEvent 用固定字节缓冲承载
 * (LP64 上 sizeof(XEvent)=192), 按事件结构成员序解释 —— 依据见各 stub 注释。
 * libX11 缺失(无头/CI)时开窗失败返回 0(一行 stderr 原因), 而 --probe /
 * --shot 不触 X11, 照常工作。
 *
 * M1 能力(与 hnwin 对齐 + Linux 侧硬约束):
 *   窗口(manifest: 标题/尺寸/透明背板) · 渲染 · Expose/resize 重排
 *   点击命中测试 · hover 伪类 · 滚轮滚动 · Esc/标题栏关闭退出
 *
 * M2 能力(补齐事件面, 与 macOS HtmlNativeView.swift 同构):
 *   事件派发管道: 引擎只给事件数据与冒泡路径(hn.h:321 的分工 —— 没有
 *   hn_context_emit 这种入口, 管道归运行时; Windows 的 hnwin.c:22 也把它
 *   留给了 M2)。本侧镜像 Swift 的 emit(HtmlNativeView.swift:767):
 *   目标解析(显式 > 命中 > 文档根) → 沿带 id 冒泡路径由内向外 →
 *   hx-trigger 消费(无声明默认 click; hover/hover-out 键分居 enter/leave
 *   侧, 同 :864 triggerMatches)。click/mousedown/mouseup/mouseenter/
 *   mouseleave/focus/blur/keydown/keyup/scroll 全走同一管道; 离散事件
 *   照旧经 hnweb_poll 交还宿主。hx 载体先经 hn_doc_autoid_hx 编址
 *   (hn.h:457, 冒泡路径只认带 id 元素; Swift render():652 每次渲染后补)。
 *   动画帧循环: 实测 dt 推进 hn_context_anim_tick —— 过渡/@keyframes 与
 *   ext_clock(Lottie/hn-mesh 的独立时钟, hn_context.c:754-758)共用;
 *   select 超时取"动画 16ms 一帧 / hx 轮询最近到期 / 无事无限阻塞"的
 *   最小者 —— 空闲零唤醒, 不忙等(hnwin 的 SetTimer 恒 16ms 唤醒, X11
 *   侧 select 既能给出真实帧间隔, 也能真睡)。
 *   --probe 扩展: 内置自测文档, 无窗口断言管道状态变化(:hover 背景切换/
 *   滚动位移/hx 触发计数/键名映射/时钟静止) —— 纯引擎语义, CI 照跑。
 *
 * 路径口径: 启动即 chdir 到 HTML 所在目录(资产/图片/link 相对文档目录,
 * 与 hnwin.c:688-705 一致)。
 *
 * 用法:
 *   hnweb_linux <file.html> [W H]          开窗口
 *   hnweb_linux <file.html> --probe [W H]  无窗口自检(管线统计 + 命中采样 +
 *                                          事件管道断言), 供 CI
 *   hnweb_linux <file.html> --shot <png> [W H]  渲染位图落盘
 *
 * 已知取舍(M2):
 *   - 文本: -DHN_NO_TEXT 交叉口径不链 FreeType, 位图无字形(几何仍正确,
 *     等宽估算使 CJK 偏宽 65%); 部署平台链接 FreeType 去掉宏即可开启
 *   - DPI: 按 CSS px = 物理 px(与 hnwin.c:20 同一口径), 未读 Xft.dpi
 *   - 透明: depth-32 TrueColor visual + 预乘 BGRA(X11/cairo/UpdateLayeredWindow
 *     同一 ARGB32 预乘约定, 需合成器); 无 32 位 visual 时降级为不透明
 *   - sys:// 系统桥未移植(Windows 由 tools/sysbridge.c 提供): hx-* 管道
 *     完整成形(触发/冒泡计数与 Swift 同构), 但片段请求不可得(stderr 一行)
 *   - 键盘: HN_KEY_* + DOM 键名 + 修饰键进管道; 不做键入编辑与输入法
 *     (XFilterEvent/XIM 未接); X11 无 isARepeat 等价物, repeat 恒 0
 *   - mousemove: 高频事件不进 hnweb_poll 队列、不走 hx 派发(管道对它的
 *     消费场景是 JS 监听, C 侧无 JS; sys 桥缺失时会逐像素 stderr 轰炸)
 *   - swap 后旧节点留在 arena(只有 hn_context_compact 回收, 本壳不调),
 *     派发中途触发的 swap 让后续冒泡读到过期但有效的内存 —— 与 Swift
 *     运行时同一取舍
 *   - 单线程使用(引擎非线程安全, 与 hnwin 相同)
 */
#define _POSIX_C_SOURCE 200809L
/* 严格 -std=c99 下 musl/glibc 只暴露 ISO C —— strdup/select/clock_gettime/
   dlopen 都是 POSIX, 必须显式开特性宏(hncore.c:101-103 为 strdup 踩过坑)。 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <ctype.h> /* trig 匹配的大小写折叠(hx-trigger 声明大小写不敏感) */
#include <dlfcn.h>
#include <time.h>
#include <sys/select.h>

#include "hn.h"
/* ---- hnweb.h 门面声明(必须先于 HNWEB_RENDER 缺省定义) ----
 * 门面头(tools/hnweb.h)自带绘制后端切换点: -DHN_USE_CAIRO 时它把
 * HNWEB_RENDER 定义为 hncairo_render(hn_cairo.h:36)。若先在这里定义缺省
 * 宏再包含头文件, 头里的 #ifndef 会看到"已定义"而直接让路 —— cairo 切换
 * 被静默击败。所以顺序: 先门面, 后缺省。 */
#if defined(__has_include)
#if __has_include("hnweb.h")
#include "hnweb.h"
#define HNWEB_FACADE_DECLARED 1
#endif
#endif
/* 绘制后端缺省: hnsoft(纯 C 软件光栅), 与 hnwin.c:39-44 同一宏手法;
   头文件已定义(或构建方 -D 重定义)时不覆盖。 */
#include "hnsoft.h"
#ifdef HN_USE_CAIRO
#include "hn_cairo.h" /* hnweb.h 缺席(仅回退声明路径)时也保证测量函数有声明 */
#endif
#ifndef HNWEB_RENDER
#define HNWEB_RENDER(dl, w, h, bg) hnsoft_render((dl), (w), (h), (bg))
#endif
#ifndef HNWEB_FACADE_DECLARED

typedef struct hnweb_win hnweb_win; /* 不透明句柄 */

typedef struct hnweb_opts {
    const char *title;    /* NULL=取 manifest hn-title */
    float w, h;           /* 0=取 manifest hn-window */
    int transparent;      /* 1=逐像素 alpha 无边框背板(镜像 hn-transparent) */
    int resizable;        /* 0=固定尺寸表面 */
    const char *store_id; /* 本地 KV 隔离 id, NULL="default" */
} hnweb_opts;

typedef struct hnweb_event {
    hn_event_kind kind;   /* 复用 hn.h:323-340 */
    float x, y;           /* 点击/滚动坐标(窗口客户区, y 向下) */
    const char *target_id;/* 命中最深元素 id(NULL=无 id); 两次 poll 之间有效 */
    hn_node *target;      /* 命中最深节点(宿主读属性/续派发用) */
    int key;              /* HN_KEY_* */
} hnweb_event;

int hnweb_open(const hnweb_opts *opts, const char *html, size_t html_len,
               const char *css, size_t css_len, hnweb_win **out);
int hnweb_update(hnweb_win *w, const char *html, size_t len);
int hnweb_resize(hnweb_win *w, int width_px, int height_px);
void hnweb_mouse_move(hnweb_win *w, float x, float y);
void hnweb_mouse_leave(hnweb_win *w);
void hnweb_mouse_down(hnweb_win *w, float x, float y, int button);
void hnweb_scroll(hnweb_win *w, float x, float y, float delta_y);
void hnweb_key(hnweb_win *w, int key, const char *utf8);
int hnweb_poll(hnweb_win *w, hnweb_event *out);
int hnweb_frame(hnweb_win *w, float dt_ms);
void hnweb_run(hnweb_win *w);
void hnweb_close(hnweb_win *w);
hn_context *hnweb_context(hnweb_win *w);
const unsigned char *hnweb_pixels(hnweb_win *w, int *w_out, int *h_out);
int hnweb_shot(hnweb_win *w, const char *png_path);

#endif /* !HNWEB_FACADE_DECLARED */

/* ================= 最小 Xlib ABI(dlopen, 无头文件) =================
 *
 * 字节布局依据(X11/Xlib.h + X11/X.h, 自 X11R5 起稳定):
 *   - XID/Window/Drawable/Colormap/Cursor/Atom/Time/KeySym = unsigned long
 *   - Bool = int; Visual/GC/Display 只以指针出现
 *   - 结构按成员最大对齐(8, 来自 Display 指针与 unsigned long)对齐
 * x86_64 与 aarch64 Linux 同为 LP64, 一份布局喂两个交叉目标;
 * ILP32 下断言按设计直接编译失败(不支持, 且是显式失败而非静默错位)。 */

typedef struct _XDisplay xdisplay;
typedef unsigned long xwindow; /* XID —— 不要写成 uint32_t, 截断会让窗口比较失效 */
typedef unsigned long xatom;

/* 事件类型(X11/X.h; 数值是 ABI 的一部分)。M2 增加 KeyRelease/ButtonRelease
   —— keyup/mouseup 派发需要, M1 只选了按下侧。 */
enum {
    X_EV_KEYPRESS = 2,
    X_EV_KEYRELEASE = 3,
    X_EV_BUTTONPRESS = 4,
    X_EV_BUTTONRELEASE = 5,
    X_EV_MOTION = 6,
    X_EV_LEAVE = 8,
    X_EV_DESTROY = 17,
    X_EV_EXPOSE = 12,
    X_EV_CONFIGURE = 22,
    X_EV_CLIENT = 33
};

enum { X_INPUTOUTPUT = 1, X_COPYFROMPARENT = 0 };
enum { X_TRUECOLOR = 4, X_ALLOCNONE = 0 };
enum { X_ZPIXMAP = 2, X_LSBFIRST = 0 };
enum { X_PROP_REPLACE = 0 };

/* XSetWindowAttributes 值掩码位(CWBackPixel/CWBorderPixel/CWEventMask/CWColormap) */
enum { X_CW_BACKPIXEL = 1L << 1, X_CW_BORDERPIXEL = 1L << 3,
       X_CW_EVENTMASK = 1L << 11, X_CW_COLORMAP = 1L << 13 };
/* XSizeHints.flags 的 PMinSize/PMaxSize */
enum { X_PMINSIZE = 1L << 4, X_PMAXSIZE = 1L << 5 };

/* 事件掩码: KeyPress|KeyRelease|ButtonPress|ButtonRelease|LeaveWindow|
   PointerMotion|Exposure|StructureNotify(位 0/1/2/3/5/6/15/17) */
#define X_EV_MASK ((long)(1L | (1L << 1) | (1L << 2) | (1L << 3) | (1L << 5) | \
                          (1L << 6) | (1L << 15) | (1L << 17)))

/* 键符号(X11/keysymdef.h, 0xFF00 起 = ISO 9995 功能键; 数值即 ABI) */
enum {
    XK_BACKSPACE = 0xFF08, XK_TAB = 0xFF09, XK_RETURN = 0xFF0D,
    XK_ESCAPE = 0xFF1B, XK_HOME = 0xFF50, XK_LEFT = 0xFF51,
    XK_UP = 0xFF52, XK_RIGHT = 0xFF53, XK_DOWN = 0xFF54,
    XK_PRIOR = 0xFF55, XK_NEXT = 0xFF56, XK_END = 0xFF57,
    XK_DELETE = 0xFFFF
};
/* 光标字形(X11/cursorfont.h): XC_left_ptr / XC_hand2 */
enum { XCF_HAND2 = 60, XCF_LEFTPTR = 68 };

/* XAnyEvent 公共前缀 {int type; unsigned long serial; Bool send_event;
   Display *display; Window window} —— LP64 下 4+pad4+8+4+pad4+8+8 = 40 字节。
   每个事件结构都以此为头, window 在 offset 32。 */
typedef struct {
    int type;
    unsigned long serial;
    int send_event;
    xdisplay *display;
    xwindow window;
} xany_stub;
_Static_assert(sizeof(xany_stub) == 40, "XAnyEvent 公共前缀布局漂移(LP64)");

/* KeyPress/ButtonPress/MotionNotify 三类事件 0..88 字节布局一致
   (XKeyEvent/XButtonEvent/XMotionEvent): state@72, keycode/button/is_hint
   同落 offset 76, Bool same_screen@80 —— 一个 stub 复用三种解释。 */
typedef struct {
    int type;
    unsigned long serial;
    int send_event;
    xdisplay *display;
    xwindow window;
    xwindow subwindow;
    unsigned long time;
    int x, y;
    int x_root, y_root;
    unsigned int state;
    unsigned int detail; /* keycode / button / is_hint 同位复用 */
    int same_screen;
} xdevice_stub;
_Static_assert(sizeof(xdevice_stub) == 88, "XKeyEvent/XButtonEvent/XMotionEvent 布局漂移(LP64)");

typedef struct {
    int type;
    unsigned long serial;
    int send_event;
    xdisplay *display;
    xwindow window;
    int x, y, width, height, count;
} xexpose_stub; /* 40+20=60, 按 8 对齐收尾为 64 */
_Static_assert(sizeof(xexpose_stub) == 64, "XExposeEvent 布局漂移(LP64)");

typedef struct {
    int type;
    unsigned long serial;
    int send_event;
    xdisplay *display;
    xwindow window;
    int x, y, width, height, border_width;
    xwindow above; /* offset 64 */
    int override_redirect;
} xconfig_stub; /* 80 收尾 */
_Static_assert(sizeof(xconfig_stub) == 80, "XConfigureEvent 布局漂移(LP64)");

/* ClientMessage(标题栏关闭钮经 WM_PROTOCOLS 走这里):
   message_type@40, format@48, data 联合@56(5×long=40), 共 96。 */
typedef struct {
    int type;
    unsigned long serial;
    int send_event;
    xdisplay *display;
    xwindow window;
    xatom message_type;
    int format;
    long data_l[5];
} xclient_stub;
_Static_assert(sizeof(xclient_stub) == 96, "XClientMessageEvent 布局漂移(LP64)");

/* XEvent 联合承载缓冲: 各事件结构都以 long 对齐, sizeof(XEvent) 在
   LP64 = 192 = 24×sizeof(long)(ILP32 = 96 = 同式)。用 long 数组而非
   char 数组, 保证 cast 到事件 stub 时 8 字节对齐不是 UB。 */
#define X_EVENT_LONGS 24
typedef union {
    long align_l[X_EVENT_LONGS];
    unsigned char b[X_EVENT_LONGS * sizeof(long)];
} xevent_buf;
_Static_assert(sizeof(xevent_buf) == 192, "XEvent 缓冲尺寸漂移(LP64)");

/* XImage 头部前缀。XImage 其余成员(f 块函数指针等)是 Xlib 私有, 不复刻 ——
   用前缀 struct 指针解释 XCreateImage 的返回值, offsetof 断言钉死关键字段。 */
typedef struct {
    int width, height;        /* 0, 4 */
    int xoffset;              /* 8 */
    int format;               /* 12 */
    char *data;               /* 16 */
    int byte_order;           /* 24 */
    int bitmap_unit;          /* 28 */
    int bitmap_bit_order;     /* 32 */
    int bitmap_pad;           /* 36 */
    int depth;                /* 40 */
    int bytes_per_line;       /* 44 */
    int bits_per_pixel;       /* 48 */
    unsigned long red_mask;   /* 56 */
    unsigned long green_mask; /* 64 */
    unsigned long blue_mask;  /* 72 */
} ximg_head;
_Static_assert(offsetof(ximg_head, data) == 16, "XImage.data 偏移漂移");
_Static_assert(offsetof(ximg_head, bytes_per_line) == 44, "XImage.bytes_per_line 偏移漂移");
_Static_assert(offsetof(ximg_head, bits_per_pixel) == 48, "XImage.bits_per_pixel 偏移漂移");
_Static_assert(offsetof(ximg_head, red_mask) == 56, "XImage.red_mask 偏移漂移");

/* XSetWindowAttributes 全量(X11/X.h 成员序; LP64 = 104 字节)。
   Xlib 按它读值, 必须逐字段一致 —— 不能只给前缀。 */
typedef struct {
    unsigned long background_pixmap;
    unsigned long background_pixel;
    unsigned long border_pixmap;
    unsigned long border_pixel;
    int bit_gravity, win_gravity;
    int backing_store;
    unsigned long backing_planes;
    unsigned long backing_pixel;
    int override_redirect, save_under;
    long event_mask;
    long do_not_propagate_mask;
    unsigned long colormap;
    unsigned long cursor;
} xswa_stub;
_Static_assert(sizeof(xswa_stub) == 104, "XSetWindowAttributes 布局漂移(LP64)");

/* XVisualInfo(X11/Xutil.h; LP64 = 64 字节) */
typedef struct {
    void *visual;
    unsigned long visualid;
    int screen;
    unsigned int depth;
    int c_class;
    unsigned long red_mask, green_mask, blue_mask;
    int colormap_size, bits_per_rgb;
} xvisinfo_stub;
_Static_assert(sizeof(xvisinfo_stub) == 64, "XVisualInfo 布局漂移(LP64)");

/* XSizeHints(X11/Xutil.h; LP64 = 120 字节)。注意 aspect 不是 long ——
   是两个 struct {int x, y}(各 4+4 字节): min_aspect@88, max_aspect@96,
   之后 base_width/base_height/win_gravity 三个 int 收尾在 116, 对齐到 120。
   本壳只写 flags 与 min/max(0..71 前缀), 与成员宽窄无关, 但布局必须
   逐字段对 —— 将来若写 aspect 就不会踩进"按 long 算"的坑。 */
typedef struct {
    long flags, x, y, width, height;
    long min_width, min_height, max_width, max_height;
    long width_inc, height_inc;
    int min_aspect_x, min_aspect_y, max_aspect_x, max_aspect_y;
    int base_width, base_height, win_gravity;
} xsizehints_stub;
_Static_assert(sizeof(xsizehints_stub) == 120, "XSizeHints 布局漂移(LP64)");

/* 函数指针 typedef: 与真 Xlib 原型逐参数一致(pointer-ness/const/值宽;
   Display / Visual / GC 在本文件统一以 opaque 指针出现, 调用约定由本侧定义)。 */
typedef void *(*fn_XOpenDisplay_t)(const char *);
typedef int (*fn_XCloseDisplay_t)(xdisplay *);
typedef int (*fn_XDefaultScreen_t)(xdisplay *);
typedef xwindow (*fn_XDefaultRootWindow_t)(xdisplay *);
typedef void *(*fn_XDefaultVisual_t)(xdisplay *, int);
typedef int (*fn_XDefaultDepth_t)(xdisplay *, int);
typedef int (*fn_XMatchVisualInfo_t)(xdisplay *, int, int, int, xvisinfo_stub *);
typedef xwindow (*fn_XCreateColormap_t)(xdisplay *, xwindow, void *, int);
typedef int (*fn_XFreeColormap_t)(xdisplay *, xwindow);
typedef xwindow (*fn_XCreateWindow_t)(xdisplay *, xwindow, int, int, unsigned,
                                      unsigned, unsigned, int, unsigned, void *,
                                      unsigned long, xswa_stub *);
typedef int (*fn_XDestroyWindow_t)(xdisplay *, xwindow);
typedef int (*fn_XMapWindow_t)(xdisplay *, xwindow);
typedef int (*fn_XStoreName_t)(xdisplay *, xwindow, const char *);
typedef int (*fn_XSelectInput_t)(xdisplay *, xwindow, long);
typedef int (*fn_XSetWMProtocols_t)(xdisplay *, xwindow, xatom *, int);
typedef xatom (*fn_XInternAtom_t)(xdisplay *, const char *, int);
typedef int (*fn_XChangeProperty_t)(xdisplay *, xwindow, xatom, xatom, int, int,
                                    const unsigned char *, int);
typedef void (*fn_XSetWMNormalHints_t)(xdisplay *, xwindow, xsizehints_stub *);
typedef void *(*fn_XCreateGC_t)(xdisplay *, xwindow, unsigned long, void *);
typedef int (*fn_XFreeGC_t)(xdisplay *, void *);
typedef ximg_head *(*fn_XCreateImage_t)(xdisplay *, void *, unsigned, int, int,
                                        char *, unsigned, unsigned, int, int);
typedef int (*fn_XPutImage_t)(xdisplay *, xwindow, void *, ximg_head *, int, int,
                              int, int, unsigned, unsigned);
typedef int (*fn_XDestroyImage_t)(ximg_head *);
typedef int (*fn_XNextEvent_t)(xdisplay *, void *); /* void* = XEvent* 缓冲 */
typedef int (*fn_XPending_t)(xdisplay *);
typedef int (*fn_XFlush_t)(xdisplay *);
typedef int (*fn_XLookupString_t)(xdevice_stub *, char *, int, unsigned long *, void *);
typedef unsigned long (*fn_XCreateFontCursor_t)(xdisplay *, unsigned);
typedef int (*fn_XDefineCursor_t)(xdisplay *, xwindow, unsigned long);
typedef int (*fn_XFreeCursor_t)(xdisplay *, unsigned long);
typedef int (*fn_XConnectionNumber_t)(xdisplay *);

/* libX11 绑定表 + 进程级状态(所有窗口共享一个 Display) */
static struct {
    void *lib; /* dlopen 句柄: 装载后不 dlclose(函数指针会悬空) */
    fn_XOpenDisplay_t XOpenDisplay;
    fn_XCloseDisplay_t XCloseDisplay;
    fn_XDefaultScreen_t XDefaultScreen;
    fn_XDefaultRootWindow_t XDefaultRootWindow;
    fn_XDefaultVisual_t XDefaultVisual;
    fn_XDefaultDepth_t XDefaultDepth;
    fn_XMatchVisualInfo_t XMatchVisualInfo;
    fn_XCreateColormap_t XCreateColormap;
    fn_XFreeColormap_t XFreeColormap;
    fn_XCreateWindow_t XCreateWindow;
    fn_XDestroyWindow_t XDestroyWindow;
    fn_XMapWindow_t XMapWindow;
    fn_XStoreName_t XStoreName;
    fn_XSelectInput_t XSelectInput;
    fn_XSetWMProtocols_t XSetWMProtocols;
    fn_XInternAtom_t XInternAtom;
    fn_XChangeProperty_t XChangeProperty;
    fn_XSetWMNormalHints_t XSetWMNormalHints;
    fn_XCreateGC_t XCreateGC;
    fn_XFreeGC_t XFreeGC;
    fn_XCreateImage_t XCreateImage;
    fn_XPutImage_t XPutImage;
    fn_XDestroyImage_t XDestroyImage;
    fn_XNextEvent_t XNextEvent;
    fn_XPending_t XPending;
    fn_XFlush_t XFlush;
    fn_XLookupString_t XLookupString;
    fn_XCreateFontCursor_t XCreateFontCursor;
    fn_XDefineCursor_t XDefineCursor;
    fn_XFreeCursor_t XFreeCursor;
    fn_XConnectionNumber_t XConnectionNumber;
    xdisplay *dpy;
    int users; /* 存活窗口数: 归零时关 Display */
    xatom wm_protocols, wm_delete_window;
    unsigned long cur_arrow, cur_hand; /* 光标随 Display 生死 */
} x11;

static int x11_load(void) {
    if (x11.lib) return 1;
    void *h = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
    if (!h) h = dlopen("libX11.so", RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "hnweb_linux: 无法加载 libX11(%s)—— 无 X 环境, 开窗模式不可用(无头请用 --probe/--shot)\n",
                dlerror());
        return 0;
    }
    int ok = 1;
#define X11_SYM(field, name)                                          \
    do {                                                              \
        x11.field = (fn_##field##_t)dlsym(h, name);                 \
        if (!x11.field) {                                             \
            fprintf(stderr, "hnweb_linux: libX11 缺导出 %s\n", name); \
            ok = 0;                                                   \
        }                                                             \
    } while (0)
    X11_SYM(XOpenDisplay, "XOpenDisplay");
    X11_SYM(XCloseDisplay, "XCloseDisplay");
    X11_SYM(XDefaultScreen, "XDefaultScreen");
    X11_SYM(XDefaultRootWindow, "XDefaultRootWindow");
    X11_SYM(XDefaultVisual, "XDefaultVisual");
    X11_SYM(XDefaultDepth, "XDefaultDepth");
    X11_SYM(XMatchVisualInfo, "XMatchVisualInfo");
    X11_SYM(XCreateColormap, "XCreateColormap");
    X11_SYM(XFreeColormap, "XFreeColormap");
    X11_SYM(XCreateWindow, "XCreateWindow");
    X11_SYM(XDestroyWindow, "XDestroyWindow");
    X11_SYM(XMapWindow, "XMapWindow");
    X11_SYM(XStoreName, "XStoreName");
    X11_SYM(XSelectInput, "XSelectInput");
    X11_SYM(XSetWMProtocols, "XSetWMProtocols");
    X11_SYM(XInternAtom, "XInternAtom");
    X11_SYM(XChangeProperty, "XChangeProperty");
    X11_SYM(XSetWMNormalHints, "XSetWMNormalHints");
    X11_SYM(XCreateGC, "XCreateGC");
    X11_SYM(XFreeGC, "XFreeGC");
    X11_SYM(XCreateImage, "XCreateImage");
    X11_SYM(XPutImage, "XPutImage");
    X11_SYM(XDestroyImage, "XDestroyImage");
    X11_SYM(XNextEvent, "XNextEvent");
    X11_SYM(XPending, "XPending");
    X11_SYM(XFlush, "XFlush");
    X11_SYM(XLookupString, "XLookupString");
    X11_SYM(XCreateFontCursor, "XCreateFontCursor");
    X11_SYM(XDefineCursor, "XDefineCursor");
    X11_SYM(XFreeCursor, "XFreeCursor");
    X11_SYM(XConnectionNumber, "XConnectionNumber");
#undef X11_SYM
    if (!ok) {
        dlclose(h);
        return 0;
    }
    x11.lib = h;
    return 1;
}

static xdisplay *x11_display(void) {
    if (!x11.dpy) x11.dpy = x11.XOpenDisplay(NULL);
    if (!x11.dpy) fprintf(stderr, "hnweb_linux: XOpenDisplay 失败(无 DISPLAY?)\n");
    return x11.dpy;
}

static void x11_release(void) {
    if (x11.dpy && x11.users == 0) {
        /* XCloseDisplay 本会回收全部客户资源, 这里显式释放只为与
           dlsym 绑定成对; 顺序在关 Display 之前。 */
        if (x11.cur_arrow) x11.XFreeCursor(x11.dpy, x11.cur_arrow);
        if (x11.cur_hand) x11.XFreeCursor(x11.dpy, x11.cur_hand);
        x11.XCloseDisplay(x11.dpy);
        x11.dpy = NULL;
        x11.cur_arrow = x11.cur_hand = 0;
    }
}

/* ---------------- 文件读取 / 资产后端(与 hncore.c / hnwin.c 同口径) ---------------- */

static char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    buf[got] = 0;
    fclose(f);
    *len = got;
    return buf;
}

typedef struct { char *path; char *data; size_t len; } asset_ent;
static asset_ent asset_tab[64];
static int asset_n = 0;

static const char *asset_load(void *ctx, const char *path, size_t *len) {
    (void)ctx;
    for (int i = 0; i < asset_n; i++)
        if (!strcmp(asset_tab[i].path, path)) { if (len) *len = asset_tab[i].len; return asset_tab[i].data; }
    if (asset_n >= 64) return NULL;
    size_t n = 0;
    char *d = read_all(path, &n);
    if (!d) return NULL;
    asset_tab[asset_n].path = strdup(path);
    if (!asset_tab[asset_n].path) { free(d); return NULL; } /* strdup 失败则下次重复读盘, 不留 NULL 键 */
    asset_tab[asset_n].data = d;
    asset_tab[asset_n].len = n;
    asset_n++;
    if (len) *len = n;
    return d;
}

/* 收集全部 CSS 并按文档顺序拼接: 同名 .css(约定优先)+ 所有
   <link rel="stylesheet">(hnwin.c:95-169 的实现原样照搬)。 */
static char *load_css(const char *html_path, const char *html, size_t *out_len) {
    *out_len = 0;
    size_t cap = 4096, len = 0;
    char *acc = (char *)malloc(cap);
    if (!acc) return NULL;
    acc[0] = 0;

    #define CSS_APPEND(SRC, N) do {                                         \
        if ((N) > 0) {                                                      \
            if (len + (N) + 2 > cap) {                                      \
                while (len + (N) + 2 > cap) cap *= 2;                       \
                char *na = (char *)realloc(acc, cap);                       \
                if (!na) { free(acc); return NULL; }                        \
                acc = na;                                                   \
            }                                                               \
            memcpy(acc + len, (SRC), (N));                                  \
            len += (N);                                                     \
            acc[len++] = '\n';                                              \
            acc[len] = 0;                                                    \
        }                                                                   \
    } while (0)

    /* 1) 同名 .css */
    size_t plen = strlen(html_path);
    if (plen > 5 && !strcmp(html_path + plen - 5, ".html")) {
        char *css_path = (char *)malloc(plen + 1);
        if (css_path) {
            memcpy(css_path, html_path, plen - 5);
            strcpy(css_path + plen - 5, ".css");
            size_t clen = 0;
            char *css = read_all(css_path, &clen);
            free(css_path);
            if (css) { CSS_APPEND(css, clen); free(css); }
        }
    }
    /* 2) 所有 <link rel="stylesheet"> */
    const char *p = html;
    while (html && (p = strstr(p, "<link")) != NULL) {
        const char *end = strchr(p, '>');
        if (!end) break;
        const char *rel = strstr(p, "stylesheet");
        const char *href = strstr(p, "href");
        if (rel && href && href < end) {
            const char *q = strchr(href, '"');
            if (!q) q = strchr(href, '\'');
            if (q && q < end) {
                char qc = *q++;
                const char *e2 = strchr(q, qc);
                if (e2 && e2 <= end) {
                    size_t n = (size_t)(e2 - q);
                    char *path = (char *)malloc(n + 1);
                    if (path) {
                        memcpy(path, q, n);
                        path[n] = 0;
                        size_t clen = 0;
                        char *css = read_all(path, &clen);
                        free(path);
                        if (css) { CSS_APPEND(css, clen); free(css); }
                    }
                }
            }
        }
        p = end + 1;
    }
    #undef CSS_APPEND
    if (len == 0) { free(acc); return NULL; }
    *out_len = len;
    return acc;
}

/* ---------------- 应用状态 ---------------- */

/* 轮询(hx-trigger="every Ns"): 每 tick 检查到期(镜像 hnwin.c:376-378) */
typedef struct { char id[64]; int ms; unsigned long last; } poll_ent;

/* hnwin 的 app_t 是全局单例(g_app); 门面允许多窗, 状态全部进 win 结构,
   只把跨窗共享的 X11 绑定留在上面的 x11 全局。 */

/* 应用级事件队列容量(点击/滚动/按键经 hnweb_poll 交还) */
#define HNWEB_EVQ_CAP 64

struct hnweb_win {
    hn_context *ctx;
    int w, h;            /* 客户区尺寸(CSS px; X11 请求尺寸即窗口尺寸, 无 AdjustWindowRect 等价物) */
    int transparent;     /* 实际生效(可能因无 ARGB visual 降级为 0) */
    int resizable;
    int closed;          /* Esc / WM_DELETE_WINDOW / DestroyNotify 置位, hnweb_run 据此返回 */
    int dirty;           /* 内容已渲染待上屏(镜像 hnwin 的 g_dirty) */
    int snapped;         /* HNWEB_SHOT 首帧抓屏只做一次 */
    xwindow xwin;
    void *visual;        /* 窗口实际使用的 Visual(32 位 ARGB 或 DefaultVisual) */
    int depth;
    void *gc;            /* GC = _XGC*, 指针 */
    unsigned long colormap;
    ximg_head *img;      /* 常驻 XImage(自持 BGRA 数据, 等价 hnwin 的 DIB) */
    int img_layout_ok;
    unsigned char *px;   /* hnsoft 输出的 RGBA 位图 */
    hn_node *hover;      /* 当前 hover 元素(避免每像素重排) */
    hn_node *focus;      /* 当前焦点 input(回车提交用) */
    int cursor_kind;     /* -1 未设 0=arrow 1=hand */
    /* 应用级事件队列(点击/滚动/按键经 hnweb_poll 交还)。delta 是滚轮
       事件的纵向增量(与 hnwin.c:508 的 48px/格同口径); hnweb_event 门面
       结构没有 delta 槽位, 暂存于此供管道/自检消费 */
    hn_event_kind evq_kind[HNWEB_EVQ_CAP];
    float evq_x[HNWEB_EVQ_CAP], evq_y[HNWEB_EVQ_CAP];
    char evq_id[HNWEB_EVQ_CAP][64];
    hn_node *evq_target[HNWEB_EVQ_CAP];
    int evq_key[HNWEB_EVQ_CAP];
    float evq_delta[HNWEB_EVQ_CAP];
    int evq_head, evq_len;
    char last_id[64];    /* hnweb_poll 交出的 target_id 落点(两次 poll 之间有效) */
    poll_ent polls[16];
    int poll_n;          /* -1 = 未初始化 */
    int anim_running;    /* 最近一次 hn_context_anim_tick 的返回(空闲唤醒判定输入) */
    char *html, *css;    /* 交给引擎的文档源(arena 可能引用, 存活期 = 窗口) */
    char *store_id;
};

/* 布局期文本后端(镜像 hnwin.c:196-209): 不注入则引擎走等宽估算,
   CJK 偏宽 65%, 窄容器内文字被错误地逐字换行。测量随绘制后端同源切换
   (HN_USE_CAIRO → hncairo_measure/metrics, hn_cairo.h:49-50; 测量与绘制
   不同源时字形会落出行盒)。 */
static hn_text_backend g_tb;
static int g_tb_ready;
static float tb_measure(void *ctx, const hn_font_desc *font,
                        const char *utf8, size_t len) {
    (void)ctx;
#ifdef HN_USE_CAIRO
    return hncairo_measure(font, utf8, len);
#else
    return hnsoft_measure(font, utf8, len);
#endif
}
static void tb_metrics(void *ctx, const hn_font_desc *font,
                       float *ascent, float *descent, float *leading) {
    (void)ctx;
#ifdef HN_USE_CAIRO
    hncairo_metrics(font, ascent, descent, leading);
#else
    hnsoft_metrics(font, ascent, descent, leading);
#endif
}
static void tb_setup(void) {
    if (g_tb_ready) return;
    g_tb_ready = 1;
    g_tb.ctx = NULL;
    g_tb.measure = tb_measure;
    g_tb.metrics = tb_metrics;
}

/* RGBA(非预乘, hnsoft 输出) → BGRA, 供 XImage(LE ZPixmap 32bpp = 0xAARRGGBB
   → 内存 B,G,R,A)使用。premul=1 时输出预乘 alpha —— 透明窗口在合成器下遵循
   预乘 ARGB32 约定(与 cairo ARGB32 / Win32 UpdateLayeredWindow 同一约定)。
   与 hnwin.c:223-233 逐字一致。 */
static void rgba_to_bgra(unsigned char *dst, const unsigned char *src, int n, int premul) {
    for (int i = 0; i < n; i++) {
        unsigned char r = src[i * 4], g = src[i * 4 + 1], b = src[i * 4 + 2], a = src[i * 4 + 3];
        if (premul && a != 255) {
            r = (unsigned char)((r * a) / 255);
            g = (unsigned char)((g * a) / 255);
            b = (unsigned char)((b * a) / 255);
        }
        dst[i * 4] = b; dst[i * 4 + 1] = g; dst[i * 4 + 2] = r; dst[i * 4 + 3] = a;
    }
}

static char *str_copy(const char *s) {
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

/* ---- 渲染 + 上屏(镜像 hnwin.c:212-275) ---- */

/* 布局 + 软件光栅。宽高变化或内容变化后调用。 */
static int app_render(hnweb_win *w) {
    hn_context_layout(w->ctx, (float)w->w, (float)w->h, &g_tb);
    const hn_display_list *dl = hn_context_display_list(w->ctx);
    if (!dl) return 0;
    free(w->px);
    w->px = HNWEB_RENDER(dl, w->w, w->h, 0x00000000); /* 透明底色; 透明窗口另由 strip_root_background 剥 body */
    return w->px != NULL;
}

/* 释放 XImage。data 由本侧 malloc —— XDestroyImage 会顺手 free 掉 img->data,
   但静态 musl 的堆与系统 libX11 的堆不是同一个, 跨 libc 的 free 是未定义
   行为: 先把 data 摘走自己 free, 再让 Xlib 销毁结构本身。 */
static void img_destroy(hnweb_win *w) {
    if (!w->img) return;
    char *data = w->img->data;
    w->img->data = NULL;
    x11.XDestroyImage(w->img);
    free(data);
    w->img = NULL;
}

/* 按 w/h 重建背板缓冲(等价 hnwin 的 DeleteObject+CreateDIBSection)。 */
static int img_rebuild(hnweb_win *w) {
    if (!x11.dpy || !w->xwin) return 0;
    img_destroy(w);
    ximg_head *img = x11.XCreateImage(x11.dpy, w->visual, (unsigned)w->depth, X_ZPIXMAP, 0,
                                      NULL, (unsigned)w->w, (unsigned)w->h, 32, 0);
    if (!img) { fprintf(stderr, "hnweb_linux: XCreateImage 失败\n"); return 0; }
    img->data = (char *)malloc((size_t)img->bytes_per_line * (size_t)w->h);
    if (!img->data) {
        x11.XDestroyImage(img); /* data 为 NULL, 只回收 Xlib 侧结构 */
        fprintf(stderr, "hnweb_linux: 背板缓冲分配失败\n");
        return 0;
    }
    if (!w->img_layout_ok) {
        /* 字节布局口径只验一次: 32bpp + 服务器 LSBFirst + R/G/B 从高到低
           —— 内存序即 B,G,R,A, 恰是 rgba_to_bgra 的输出序。现代 x86_64/
           aarch64 服务器恒成立; 不成立宁可不显示也不画错色。32bpp 时
           bytes_per_line 恒等于 w*4(行已 4 对齐), 故可整块紧凑转换。 */
        if (img->bits_per_pixel != 32 || img->byte_order != X_LSBFIRST ||
            img->red_mask != 0xFF0000ul || img->green_mask != 0xFF00ul ||
            img->blue_mask != 0xFFul) {
            fprintf(stderr, "hnweb_linux: 不支持的 XImage 布局 bpp=%d order=%d masks=%06lx/%06lx/%06lx\n",
                    img->bits_per_pixel, img->byte_order,
                    img->red_mask, img->green_mask, img->blue_mask);
            char *d = img->data; /* data 归本侧 free(跨 libc 的 free 见 img_destroy) */
            img->data = NULL;
            x11.XDestroyImage(img);
            free(d);
            return 0;
        }
        w->img_layout_ok = 1;
    }
    w->img = img;
    return 1;
}

/* 把引擎位图送进窗口: XPutImage 整窗直贴(等价 BitBlt SRCCOPY)。
   X11 不透明/透明窗口的上屏动作相同, 差别只在预乘开关与 visual 深度 ——
   hnwin 需要的 UpdateLayeredWindow/BitBlt 双分支在 X11 合并为一条路径。 */
static void present(hnweb_win *w) {
    if (!w->px || !w->img || !x11.dpy || !w->xwin) return;
    rgba_to_bgra((unsigned char *)w->img->data, w->px, w->w * w->h, w->transparent);
    x11.XPutImage(x11.dpy, w->xwin, w->gc, w->img, 0, 0, 0, 0,
                  (unsigned)w->w, (unsigned)w->h);
    x11.XFlush(x11.dpy);
}

/* Expose(WM_PAINT 等价): X server 不保留窗口内容, 重贴常驻 XImage 即可。 */
static void expose_present(hnweb_win *w) {
    if (!w->img || !x11.dpy || !w->xwin) return;
    x11.XPutImage(x11.dpy, w->xwin, w->gc, w->img, 0, 0, 0, 0,
                  (unsigned)w->w, (unsigned)w->h);
    x11.XFlush(x11.dpy);
}

/* ---- hx-* 执行(htmx 语义, 镜像 hnwin.c:299-401) ----
   transport 进程内应答(sys://)由 Windows 的 tools/sysbridge.c 提供,
   Linux 移植是独立工作 —— 管道先成形, 片段不可得按 hnwin 的 !frag
   分支返回 0, 点击/回车轨迹照常打印。 */

static char *sys_fragment(const char *url, const char *form_body, const char *store_id) {
    (void)form_body;
    (void)store_id;
    fprintf(stderr, "hnweb_linux: hx 请求 %s 无传输(sys:// 桥未移植, 见 tools/sysbridge.c)\n", url);
    return NULL;
}

/* M2 起点击的 hx 派发改走统一管道(ev_dispatch): 载体经 hn_doc_autoid_hx
   编址后必然出现在冒泡路径上, hn_carrier 式"向上找第一个 hx-get/post"
   的旁路就此退役 —— 管道还顺带覆盖了 hx-trigger 声明的 hover/keydown 等。 */

static int hx_perform(hnweb_win *w, hn_node *carrier, int direct) {
    const char *post = hn_node_attr(carrier, "hx-post");
    const char *get = hn_node_attr(carrier, "hx-get");
    const char *url = post ? post : get;
    if (!url) return 0;
    hn_doc *doc = hn_context_doc(w->ctx);
    /* 表单参数(POST 发 body / GET 拼 query) */
    char form[4096];
    form[0] = 0;
    hn_doc_form_encode(doc, form, sizeof(form));
    char *frag = sys_fragment(url, form, w->store_id);
    if (!frag) return 0;
    /* 目标: hx-target 指定 id, 缺省载体自身 */
    const char *tid = hn_node_attr(carrier, "hx-target");
    if (!tid || !tid[0] || !strcmp(tid, "this")) tid = hn_node_attr(carrier, "id");
    if (!tid || !tid[0]) { free(frag); return 0; }
    const char *swap_mode = hn_node_attr(carrier, "hx-swap");
    hn_swap_mode m = HN_SWAP_INNER;
    if (swap_mode) {
        if (!strcmp(swap_mode, "outerHTML")) m = HN_SWAP_OUTER;
        else if (!strcmp(swap_mode, "append") || !strcmp(swap_mode, "beforeend")) m = HN_SWAP_APPEND;
        else if (!strcmp(swap_mode, "prepend") || !strcmp(swap_mode, "afterbegin")) m = HN_SWAP_PREPEND;
    }
    int ok = hn_doc_swap(doc, tid, m, frag, strlen(frag));
    free(frag);
    if (ok && app_render(w)) {
        if (direct) {
            present(w);
        } else {
            /* load / poll 路径: 先标脏, 由帧循环统一上屏(镜像 hnwin.c:344-348,
               否则 DOM 更新了但画面不动)。 */
            w->dirty = 1;
        }
    }
    return ok;
}

/* 启动动作: 文档内所有 hx-trigger 含 "load" 的元素立即执行一次 */
static void run_load_actions(hnweb_win *w) {
    hn_doc *doc = hn_context_doc(w->ctx);
    int any = 0;
    for (int i = 0; ; i++) {
        const char *id = NULL;
        if (!hn_doc_load_at(doc, i, &id)) break;
        any++;
        hn_node *n = hn_doc_find_by_id(doc, id);
        printf("[hx-load] id=%s node=%p\n", id, (void *)n);
        if (n) {
            const char *url = hn_node_attr(n, "hx-post") ? hn_node_attr(n, "hx-post")
                                                        : hn_node_attr(n, "hx-get");
            printf("[hx-load] url=%s\n", url ? url : "(无)");
            int ok = hx_perform(w, n, 0);
            printf("[hx-load] swap=%d\n", ok);
        }
    }
    if (!any) printf("[hx-load] 无 load 触发元素\n");
    fflush(stdout);
}

static unsigned long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)ts.tv_sec * 1000ul + (unsigned long)(ts.tv_nsec / 1000000L);
}

/* GetTickCount 的等价计时口径(CLOCK_MONOTONIC); 结构/逻辑与 hnwin 一致 */
static void poll_tick(hnweb_win *w) {
    if (w->poll_n < 0) {
        w->poll_n = 0;
        hn_doc *doc = hn_context_doc(w->ctx);
        for (int i = 0; i < 16; i++) {
            const char *id = NULL; int ms = 0;
            if (!hn_doc_poll_at(doc, i, &id, &ms)) break;
            snprintf(w->polls[w->poll_n].id, 64, "%s", id);
            w->polls[w->poll_n].ms = ms;
            w->polls[w->poll_n].last = now_ms();
            w->poll_n++;
        }
    }
    unsigned long now = now_ms();
    for (int i = 0; i < w->poll_n; i++) {
        if (now - w->polls[i].last >= (unsigned long)w->polls[i].ms) {
            w->polls[i].last = now;
            hn_node *n = hn_doc_find_by_id(hn_context_doc(w->ctx), w->polls[i].id);
            if (n) hx_perform(w, n, 0);
        }
    }
}

/* ---- 事件队列(hnweb_poll 的供给端) ---- */

static void evq_push(hnweb_win *w, hn_event_kind kind, float x, float y,
                     const char *id, hn_node *target, int key, float delta) {
    if (w->evq_len >= HNWEB_EVQ_CAP) {
        static int warned = 0;
        if (!warned) { warned = 1; fprintf(stderr, "hnweb_linux: 事件队列满, 丢弃后续事件\n"); }
        return;
    }
    int i = (w->evq_head + w->evq_len) % HNWEB_EVQ_CAP;
    w->evq_kind[i] = kind;
    w->evq_x[i] = x;
    w->evq_y[i] = y;
    w->evq_id[i][0] = 0;
    if (id) snprintf(w->evq_id[i], 64, "%s", id);
    w->evq_target[i] = target;
    w->evq_key[i] = key;
    w->evq_delta[i] = delta;
    w->evq_len++;
}

/* PNG 落盘: hnweb_shot 与 HNWEB_SHOT 首帧抓屏共用 */
static int write_png(hnweb_win *w, const char *path) {
    if (!w->px) return 0;
    size_t pn = 0;
    unsigned char *png = hnsoft_encode_png(w->px, w->w, w->h, &pn);
    if (!png) { fprintf(stderr, "hnweb_linux: PNG 编码失败\n"); return 0; }
    FILE *of = fopen(path, "wb");
    if (!of) { fprintf(stderr, "hnweb_linux: 无法写 %s\n", path); free(png); return 0; }
    fwrite(png, 1, pn, of);
    fclose(of);
    free(png);
    return 1;
}

/* ---- M2 统一事件管道(镜像 HtmlNativeView.swift:755-888) ----
   引擎给事件数据 + 冒泡路径(hn_event_path_at/len), 管道在本侧:
   目标解析 → 沿带 id 路径由内向外 → hx-trigger 消费。 */

/* hx-trigger 与事件名的匹配(Swift triggerMatches:864 的直译)。大小写
   不敏感; 无声明默认 click; hover 键只在进入侧、hover-out 只在离开侧
   触发(hx 的 hover 语义); keydown 兼容 "enter" 写法。 */
static int ci_contains(const char *hay, const char *needle) {
    if (!hay) return 0;
    size_t nl = strlen(needle);
    for (; *hay; hay++) {
        size_t i = 0;
        while (i < nl && hay[i] &&
               tolower((unsigned char)hay[i]) == tolower((unsigned char)needle[i]))
            i++;
        if (i == nl) return 1;
    }
    return 0;
}

static int trig_matches(const char *trig, const char *ev) {
    if (!trig || !trig[0]) return strcmp(ev, "click") == 0;
    if (!strcmp(ev, "click"))      return ci_contains(trig, "click");
    if (!strcmp(ev, "mouseenter")) return ci_contains(trig, "hover") || ci_contains(trig, "mouseenter");
    if (!strcmp(ev, "mouseleave")) return ci_contains(trig, "hover-out") || ci_contains(trig, "mouseleave");
    if (!strcmp(ev, "keydown"))    return ci_contains(trig, "keydown") || ci_contains(trig, "enter");
    if (!strcmp(ev, "focus"))      return ci_contains(trig, "focus");
    if (!strcmp(ev, "blur"))       return ci_contains(trig, "blur");
    if (!strcmp(ev, "scroll"))     return ci_contains(trig, "scroll");
    if (!strcmp(ev, "submit"))     return ci_contains(trig, "submit");
    if (!strcmp(ev, "change"))     return ci_contains(trig, "change");
    return ci_contains(trig, ev);
}

/* 键名(与 DOM KeyboardEvent.key 对齐; Swift keyName:1085 的直译)。
   特殊键按 HN_KEY_* 给名, 可打印键直接用 XLookupString 的文本。 */
static const char *ev_key_name(int key, const char *utf8) {
    switch (key) {
    case HN_KEY_ENTER:     return "Enter";
    case HN_KEY_ESC:       return "Escape";
    case HN_KEY_TAB:       return "Tab";
    case HN_KEY_BACKSPACE: return "Backspace";
    case HN_KEY_DELETE:    return "Delete";
    case HN_KEY_LEFT:      return "ArrowLeft";
    case HN_KEY_RIGHT:     return "ArrowRight";
    case HN_KEY_UP:        return "ArrowUp";
    case HN_KEY_DOWN:      return "ArrowDown";
    case HN_KEY_HOME:      return "Home";
    case HN_KEY_END:       return "End";
    case HN_KEY_PAGEUP:    return "PageUp";
    case HN_KEY_PAGEDOWN:  return "PageDown";
    case HN_KEY_SPACE:     return " ";
    default:               return (utf8 && utf8[0]) ? utf8 : NULL;
    }
}

/* X11 修饰键状态 → 引擎 HN_MOD_* 掩码(X11/X.h: Shift=1<<0 Ctrl=1<<2
   Mod1=1<<3(Alt) Mod4=1<<6(Super); hn.h:343 的引擎位)。 */
static unsigned xmods_to_hn(unsigned int state) {
    unsigned m = 0;
    if (state & (1u << 0)) m |= HN_MOD_SHIFT;
    if (state & (1u << 2)) m |= HN_MOD_CTRL;
    if (state & (1u << 3)) m |= HN_MOD_ALT;
    if (state & (1u << 6)) m |= HN_MOD_META;
    return m;
}

/* 冒泡派发(Swift emit:805-830 的消费者 2; 消费者 1 是 JS, C 侧无 JS):
   沿带 id 路径由内向外, hx-trigger 命中且带 hx-get/post 即调 fire 钩子。
   fire 允许为 NULL(自检只数不执行)。派发中途的 swap 只摘链不释放
   (arena 存活到 compact), 后续路径读到的是过期但有效的内存。
   返回 1 = 有节点消费了事件(Swift 的 prevented/hxFired 合一)。 */
static int ev_dispatch(hn_context *ctx, hn_node *target, const char *evname,
                       int (*fire)(hn_node *, void *), void *ud,
                       int *depth_out, int *fired_out) {
    (void)ctx;
    int depth = hn_event_path_len(target);
    int fired = 0;
    for (int i = 0; i < depth; i++) {
        hn_node *n = hn_event_path_at(target, i);
        if (!n) continue;
        if (!trig_matches(hn_node_attr(n, "hx-trigger"), evname)) continue;
        if (!hn_node_attr(n, "hx-get") && !hn_node_attr(n, "hx-post")) continue;
        if (fire && fire(n, ud)) fired++;
    }
    if (depth_out) *depth_out = depth;
    if (fired_out) *fired_out = fired;
    return fired > 0;
}

/* 管道的 hx 消费端: 与 M1 点击路径同一句式(打印 + hx_perform 直通) */
static int ev_fire_hx(hn_node *n, void *ud) {
    hnweb_win *w = (hnweb_win *)ud;
    const char *url = hn_node_attr(n, "hx-post") ? hn_node_attr(n, "hx-post")
                                                 : hn_node_attr(n, "hx-get");
    printf("[hx] %s\n", url ? url : "?");
    return hx_perform(w, n, 1);
}

/* 统一派发入口(Swift emit:767 的等价物): 目标解析(显式 > 命中 > 根)
   → 离散事件进应用级队列(hnweb_poll 契约) → 冒泡消费。mousemove 高频
   不进队列; 返回 1 = 事件被 hx 消费(调用方据此跳过默认行为)。 */
static int emit_event(hnweb_win *w, hn_event_kind kind, hn_node *target,
                      float x, float y, int key, unsigned mods, float delta) {
    (void)mods; /* C 侧无 JS 消费者, 修饰键只随 X11 调用方口径入参留位 */
    hn_doc *doc = hn_context_doc(w->ctx);
    hn_node *hit = target;
    if (!hit) hit = (x >= 0.0f && y >= 0.0f) ? hn_context_hit_node(w->ctx, x, y) : NULL;
    if (!hit) hit = hn_doc_root(doc);
    if (!hit) return 0;
    if (kind != HN_EV_MOUSEMOVE) {
        /* 队列 id 口径与 M1 的 hit_test 一致: 冒泡路径第 0 层 = 命中链上
           最深带 id 元素(hit_test 本身就是命中链的 id 视图) */
        hn_node *id0 = hn_event_path_at(hit, 0);
        evq_push(w, kind, x, y, id0 ? hn_node_attr(id0, "id") : NULL, hit, key, delta);
    }
    int depth = 0, fired = 0;
    ev_dispatch(w->ctx, hit, hn_event_name(kind), ev_fire_hx, w, &depth, &fired);
    return fired > 0;
}

/* 按下(M1 on_click 的 M2 形态): M1 的命中打印/焦点保留, 增加 :active
   与 mousedown/click 全走管道(Swift mouseDown:895-921 的次序)。 */
static void on_press(hnweb_win *w, float x, float y, unsigned mods) {
    const char *id = hn_context_hit_test(w->ctx, x, y);
    hn_node *n = hn_context_hit_node(w->ctx, x, y);
    const char *tag = n ? hn_node_tag(n) : NULL;
    printf("[click] (%d,%d) -> <%s id=%s>\n", (int)x, (int)y,
           tag ? tag : "?", id ? id : "(无id)");
    /* input 焦点(回车提交用); 焦点切换本身也是事件(blur 旧的/focus 新的,
       Swift focusInput:944-958) */
    hn_node *newf = n ? hn_node_ancestor_input(n) : NULL;
    if (newf != w->focus) {
        if (w->focus) emit_event(w, HN_EV_BLUR, w->focus, -1, -1, 0, 0, 0);
        w->focus = newf;
        if (newf) emit_event(w, HN_EV_FOCUS, newf, -1, -1, 0, 0, 0);
        hn_context_set_focus(w->ctx, newf);
    }
    /* :active 伪类(按下态高亮), 抬起时清除 */
    hn_context_set_active(w->ctx, n);
    if (app_render(w)) present(w);
    emit_event(w, HN_EV_MOUSEDOWN, n, x, y, 0, mods, 0);
    /* 点击坐标回传: x/y 给几何, id 给语义, 二者都来自引擎命中测试 */
    emit_event(w, HN_EV_CLICK, n, x, y, 0, mods, 0);
    fflush(stdout);
}

/* 抬起: mouseup 派发 + 清 :active(Swift mouseUp:923-930) */
static void on_release(hnweb_win *w, float x, float y, unsigned mods) {
    emit_event(w, HN_EV_MOUSEUP, NULL, x, y, 0, mods, 0);
    hn_context_set_active(w->ctx, NULL);
    if (app_render(w)) present(w);
}

static int keysym_to_hn(unsigned long ks) {
    switch (ks) {
    case XK_RETURN: return HN_KEY_ENTER;
    case XK_ESCAPE: return HN_KEY_ESC;
    case XK_TAB: return HN_KEY_TAB;
    case XK_BACKSPACE: return HN_KEY_BACKSPACE;
    case XK_DELETE: return HN_KEY_DELETE;
    case XK_LEFT: return HN_KEY_LEFT;
    case XK_RIGHT: return HN_KEY_RIGHT;
    case XK_UP: return HN_KEY_UP;
    case XK_DOWN: return HN_KEY_DOWN;
    case XK_HOME: return HN_KEY_HOME;
    case XK_END: return HN_KEY_END;
    case XK_PRIOR: return HN_KEY_PAGEUP;
    case XK_NEXT: return HN_KEY_PAGEDOWN;
    case 0x20: return HN_KEY_SPACE;
    default: return HN_KEY_NONE;
    }
}

/* ---- 窗口表面 ---- */

static void cursor_apply(hnweb_win *w, int want_hand) {
    if (!x11.dpy || !w->xwin) return;
    int kind = want_hand ? 1 : 0;
    if (w->cursor_kind == kind) return;
    if (!x11.cur_arrow) {
        x11.cur_arrow = x11.XCreateFontCursor(x11.dpy, XCF_LEFTPTR);
        x11.cur_hand = x11.XCreateFontCursor(x11.dpy, XCF_HAND2);
    }
    unsigned long cur = want_hand ? x11.cur_hand : x11.cur_arrow;
    if (cur) {
        x11.XDefineCursor(x11.dpy, w->xwin, cur);
        w->cursor_kind = kind;
    }
}

/* 建窗 + 首帧前的表面物化(manifest 已折算进 w 的字段)。
   返回 1 = 可渲染上屏。 */
static int surface_create(hnweb_win *w, const char *title) {
    xdisplay *dpy = x11_display();
    if (!dpy) return 0;
    int scr = x11.XDefaultScreen(dpy);
    xwindow root = x11.XDefaultRootWindow(dpy);
    void *visual = x11.XDefaultVisual(dpy, scr);
    int depth = x11.XDefaultDepth(dpy, scr);
    unsigned long colormap = 0;
    if (w->transparent) {
        /* 透明窗口: 选 depth-32 TrueColor(ARGB)visual —— 与 hnwin 的
           WS_EX_LAYERED、macOS 的 isOpaque=false 同一语义位。 */
        xvisinfo_stub vi;
        memset(&vi, 0, sizeof vi);
        if (x11.XMatchVisualInfo(dpy, scr, 32, X_TRUECOLOR, &vi)) {
            visual = vi.visual;
            depth = 32;
            colormap = x11.XCreateColormap(dpy, root, visual, X_ALLOCNONE);
            if (!colormap) { /* 降级回默认 visual: 连同预乘开关一并关闭 */
                visual = x11.XDefaultVisual(dpy, scr);
                depth = x11.XDefaultDepth(dpy, scr);
                w->transparent = 0;
            }
        } else {
            fprintf(stderr, "hnweb_linux: 无 32 位 TrueColor visual, 透明背板降级为不透明\n");
            w->transparent = 0;
        }
    }
    w->visual = visual;
    w->depth = depth;
    w->colormap = colormap;

    xswa_stub swa;
    memset(&swa, 0, sizeof swa);
    swa.background_pixel = 0; /* 黑底(对齐 hnwin 的 BLACK_BRUSH); ARGB 下 0 = 预乘全透明 */
    swa.border_pixel = 0;     /* 非 DefaultVisual 建窗必须给 border_pixel, 否则 BadMatch */
    swa.event_mask = X_EV_MASK;
    swa.colormap = colormap;
    unsigned long mask = X_CW_BACKPIXEL | X_CW_BORDERPIXEL | X_CW_EVENTMASK |
                         (colormap ? X_CW_COLORMAP : 0);
    xwindow win = x11.XCreateWindow(dpy, root, 0, 0, (unsigned)w->w, (unsigned)w->h, 0,
                                    depth, X_INPUTOUTPUT, visual, mask, &swa);
    if (!win) { fprintf(stderr, "hnweb_linux: XCreateWindow 失败\n"); return 0; }
    w->xwin = win;
    x11.users++;

    w->gc = x11.XCreateGC(dpy, win, 0, NULL);
    if (!w->gc) { fprintf(stderr, "hnweb_linux: XCreateGC 失败\n"); return 0; }

    x11.XSelectInput(dpy, win, X_EV_MASK);

    /* WM_DELETE_WINDOW: 标题栏关闭钮发 ClientMessage 而不是硬断连 */
    if (!x11.wm_protocols) {
        x11.wm_protocols = x11.XInternAtom(dpy, "WM_PROTOCOLS", 0);
        x11.wm_delete_window = x11.XInternAtom(dpy, "WM_DELETE_WINDOW", 0);
    }
    x11.XSetWMProtocols(dpy, win, &x11.wm_delete_window, 1);

    /* 标题: WM_NAME(STRING) 对非拉丁字符不可靠, 追加 _NET_WM_NAME(UTF8_STRING)
       —— 中文标题不乱码(X11 侧对应 hnwin 的 CP_UTF8 宽字符转换)。 */
    x11.XStoreName(dpy, win, title && title[0] ? title : "hnweb");
    xatom utf8 = x11.XInternAtom(dpy, "UTF8_STRING", 0);
    xatom net_name = x11.XInternAtom(dpy, "_NET_WM_NAME", 0);
    if (utf8 && net_name && title && title[0])
        x11.XChangeProperty(dpy, win, net_name, utf8, 8, X_PROP_REPLACE,
                            (const unsigned char *)title, (int)strlen(title));

    if (!w->resizable) {
        xsizehints_stub sh;
        memset(&sh, 0, sizeof sh);
        sh.flags = X_PMINSIZE | X_PMAXSIZE;
        sh.min_width = sh.max_width = w->w;
        sh.min_height = sh.max_height = w->h;
        x11.XSetWMNormalHints(dpy, win, &sh);
    }

    w->cursor_kind = -1;
    cursor_apply(w, 0);
    x11.XMapWindow(dpy, win);
    x11.XFlush(dpy);
    return 1;
}

/* X 事件 → 门面调用(等价 hnwin 的 wnd_proc 427-558)。 */
static int win_resize(hnweb_win *w, int nw, int nh);
static int key_event(hnweb_win *w, int key, const char *utf8, unsigned mods,
                     int release);

static void win_dispatch(hnweb_win *w, const unsigned char *evbuf) {
    const xany_stub *a = (const xany_stub *)evbuf;
    if (a->window != w->xwin) return; /* 共享 Display: 只认自己的窗口 */
    switch (a->type) {
    case X_EV_EXPOSE: {
        const xexpose_stub *e = (const xexpose_stub *)evbuf;
        /* 连片 Expose 只画最后一次(count==0) */
        if (e->count == 0) expose_present(w);
        break;
    }
    case X_EV_CONFIGURE: {
        /* ConfigureNotify ↔ WM_SIZE: X11 的请求尺寸就是窗口自身尺寸
           (WM 装饰在父窗口), 无需 hnwin 的 AdjustWindowRect 等价物。 */
        const xconfig_stub *e = (const xconfig_stub *)evbuf;
        win_resize(w, e->width, e->height);
        break;
    }
    case X_EV_MOTION: {
        const xdevice_stub *e = (const xdevice_stub *)evbuf;
        /* 坐标系: 引擎与 X11 事件同为左上原点 y 向下, 直接透传(全仓唯一
           做 y 翻转的是 macOS 底边原点, Linux 不需要)。 */
        hnweb_mouse_move(w, (float)e->x, (float)e->y);
        break;
    }
    case X_EV_LEAVE:
        /* LeaveNotify ↔ TrackMouseEvent(TME_LEAVE): 光标出窗必须清 hover,
           否则高亮不灭。本窗无子窗口, 任何 Leave 都视为真正离开。 */
        hnweb_mouse_leave(w);
        break;
    case X_EV_BUTTONPRESS: {
        const xdevice_stub *e = (const xdevice_stub *)evbuf;
        if (e->detail == 1) {
            on_press(w, (float)e->x, (float)e->y, xmods_to_hn(e->state)); /* Button1 = 左键 */
        } else if (e->detail == 4) {
            hnweb_scroll(w, (float)e->x, (float)e->y, -48.0f); /* 滚轮上: 一格 48px(hnwin.c:508) */
        } else if (e->detail == 5) {
            hnweb_scroll(w, (float)e->x, (float)e->y, +48.0f);
        }
        break;
    }
    case X_EV_BUTTONRELEASE: {
        /* M2: mouseup 派发 + 清 :active。滚轮的 4/5 也发 Release, 但滚动
           在 Press 侧完成, Release 是空拍, 不派发 */
        const xdevice_stub *e = (const xdevice_stub *)evbuf;
        if (e->detail == 1)
            on_release(w, (float)e->x, (float)e->y, xmods_to_hn(e->state));
        break;
    }
    case X_EV_KEYPRESS: {
        const xdevice_stub *e = (const xdevice_stub *)evbuf;
        char buf[16];
        unsigned long ks = 0;
        int n = x11.XLookupString((xdevice_stub *)e, buf, (int)sizeof buf - 1, &ks, NULL);
        if (n < 0) n = 0;
        buf[n] = 0;
        int key = keysym_to_hn(ks);
        int consumed = key_event(w, key, n > 0 ? buf : NULL, xmods_to_hn(e->state), 0);
        /* M1 口径保留: Esc 退出主循环 —— 仅当管道未消费; 文档用
           hx-trigger="keydown" 接管了 Esc(如关弹窗)就归文档管 */
        if (!consumed && key == HN_KEY_ESC) w->closed = 1;
        break;
    }
    case X_EV_KEYRELEASE: {
        /* M2: keyup 派发(XLookupString 在 Release 上同样给出文本/键符号) */
        const xdevice_stub *e = (const xdevice_stub *)evbuf;
        char buf[16];
        unsigned long ks = 0;
        int n = x11.XLookupString((xdevice_stub *)e, buf, (int)sizeof buf - 1, &ks, NULL);
        if (n < 0) n = 0;
        buf[n] = 0;
        key_event(w, keysym_to_hn(ks), n > 0 ? buf : NULL, xmods_to_hn(e->state), 1);
        break;
    }
    case X_EV_CLIENT: {
        const xclient_stub *e = (const xclient_stub *)evbuf;
        if (e->message_type == x11.wm_protocols &&
            (unsigned long)e->data_l[0] == x11.wm_delete_window)
            w->closed = 1;
        break;
    }
    case X_EV_DESTROY:
        /* WM 强拆(如注销)时也得退出。窗口在服务器侧已销毁: 摘掉句柄,
           hnweb_close 才不会再对死窗口 XDestroyWindow(BadWindow 会让
           默认错误处理器在退出路径上直接杀进程)。users 同步扣减。 */
        w->xwin = 0;
        if (x11.users > 0) x11.users--;
        w->closed = 1;
        break;
    default:
        break;
    }
}

/* 尺寸变了才动作 → 重 layout → 重建背板 → 重渲染上屏(镜像 hnwin.c:434-455)。
   返回 1 表示实际发生了变化。 */
static int win_resize(hnweb_win *w, int nw, int nh) {
    if (nw <= 0 || nh <= 0) return 0;
    if (nw == w->w && nh == w->h) return 0;
    w->w = nw;
    w->h = nh;
    if (app_render(w) && img_rebuild(w)) {
        present(w);
        return 1;
    }
    return 0;
}

/* 帧步进(WM_TIMER 分支 518-536 的等价物): 脏上屏 + HNWEB_SHOT 抓屏 +
   hx 轮询 + 动画推进。返回 1 = 本次有上屏。 */
static int frame_step(hnweb_win *w, float dt_ms) {
    int presented = 0;
    /* load/poll 路径没有直接上屏, 只标了脏; 在这里统一上屏 */
    if (w->dirty) {
        w->dirty = 0;
        present(w);
        presented = 1;
    }
    /* 首帧完成后把上屏像素落一次盘(HNWEB_SHOT=<png>; 对齐 hnwin 的
       HN_WIN_SNAP。抓的是贴进窗口的引擎位图, 不含 WM 装饰)。 */
    if (!w->snapped) {
        w->snapped = 1;
        const char *snap = getenv("HNWEB_SHOT");
        if (snap && *snap) write_png(w, snap);
    }
    /* hx 轮询(every Ns) */
    poll_tick(w);
    /* 过渡动画推进; 返回 1 表示仍在跑 —— 帧循环继续 16ms 一拍(hnwin
       WM_TIMER 的 hn_context_anim_tick(16.0f) 在这里换成调用方实测 dt,
       Lottie/hn-mesh 的 ext_clock 按真实时间推进而非固定 16ms 步长) */
    w->anim_running = hn_context_anim_tick(w->ctx, dt_ms);
    if (w->anim_running) {
        if (app_render(w)) {
            present(w);
            presented = 1;
        }
    }
    return presented;
}

/* 空闲唤醒时刻(select 超时, μs): 脏帧 → 立即; 动画在跑 → 16ms 一拍;
   hx 轮询 → 最近一个到期点; 全无 → -1(无限阻塞, X 事件驱动唤醒 ——
   空闲零唤醒, 替代 M1 恒 60Hz 的空转)。 */
static long idle_timeout_us(hnweb_win *w, unsigned long now) {
    if (w->dirty) return 0;
    long best = -1;
    if (w->anim_running) best = 16L * 1000L;
    for (int i = 0; i < w->poll_n; i++) {
        unsigned long due = w->polls[i].last + (unsigned long)w->polls[i].ms;
        long us = due > now ? (long)(due - now) * 1000L : 0L;
        if (best < 0 || us < best) best = us;
    }
    return best;
}

/* ================= 门面实现 ================= */

/* 开窗口并装载文档: 解析 → 清单 → context 组装 → 建窗 → 首帧
   (相当于 HNEngine.open 首开分支 + hnwin main 的 669-814 段)。
   html/css 内部复制, 调用方随后可 free。 */
int hnweb_open(const hnweb_opts *opts, const char *html, size_t html_len,
               const char *css, size_t css_len, hnweb_win **out) {
    if (out) *out = NULL;
    if (!out || !opts || !html || html_len == 0) {
        fprintf(stderr, "hnweb_linux: hnweb_open 参数缺失\n");
        return 0;
    }
    if (!x11_load()) return 0; /* 一行 stderr 原因 + 返回 0(对齐 hnwebview.c:599-609) */

    tb_setup();
    hnweb_win *w = (hnweb_win *)calloc(1, sizeof(hnweb_win));
    if (!w) { fprintf(stderr, "hnweb_linux: 内存不足\n"); return 0; }
    w->poll_n = -1;
    w->resizable = opts->resizable;
    w->store_id = str_copy(opts->store_id && opts->store_id[0] ? opts->store_id : "default");
    w->html = (char *)malloc(html_len + 1);
    w->css = (css && css_len) ? (char *)malloc(css_len + 1) : NULL;
    if (!w->store_id || !w->html || (css && css_len && !w->css)) {
        fprintf(stderr, "hnweb_linux: 内存不足\n");
        hnweb_close(w);
        return 0;
    }
    memcpy(w->html, html, html_len);
    w->html[html_len] = 0;
    if (css && css_len) {
        memcpy(w->css, css, css_len);
        w->css[css_len] = 0;
    }

    /* 解析 + context 组装(镜像 hnwin.c:706-719; "谁 set 谁放手", doc/sheet
       归 context, 窗口销毁时一并释放 —— hn.h:235-239) */
    hn_doc *doc = hn_parse_html(w->html, html_len);
    if (!doc) { fprintf(stderr, "hnweb_linux: 解析失败\n"); hnweb_close(w); return 0; }
    hn_context *ctx = hn_context_create();
    hn_context_set_doc(ctx, doc);
    if (w->css) {
        hn_sheet *sheet = hn_parse_css(w->css, css_len);
        if (sheet) hn_context_add_sheet(ctx, sheet);
    }
    hn_asset_backend ab;
    memset(&ab, 0, sizeof ab);
    ab.load = asset_load;
    hn_context_set_assets(ctx, &ab);
    hn_context_apply_theme(ctx); /* hn-theme 设计令牌(若文档声明) */
    /* hx 载体自动编 id(hn.h:457-459): M2 事件管道的冒泡路径只含带 id
       元素, 无 id 的 hx-get/post 元素必须先有地址才会被点击/按键触发
       (镜像 Swift render() 每次渲染后 autoid 的口径, HtmlNativeView.swift:652) */
    hn_doc_autoid_hx(doc);
    w->ctx = ctx;

    /* 清单: 标题/尺寸/透明由 hn 编码声明, 引擎解析; opts 显式值优先 */
    hn_manifest man;
    memset(&man, 0, sizeof man);
    hn_doc_manifest(doc, &man);
    w->w = opts->w > 0 ? (int)opts->w : (man.w > 0 ? (int)man.w : 480);
    w->h = opts->h > 0 ? (int)opts->h : (man.h > 0 ? (int)man.h : 700);
    w->transparent = opts->transparent || man.transparent;
    const char *title = opts->title && opts->title[0] ? opts->title
                        : (man.title && man.title[0] ? man.title : "hnweb");

    /* 建窗 + 首帧双动作(镜像 hnwin.c:797-814 native 分支; 本平台不挂
       webview 兜底 —— 系统级 webview 壳是独立工作)。
       剥底色必须在 surface_create 之后: 那里可能因无 32 位 visual 把
       transparent 降级为 0, 而 hn_context_strip_root_background 是持久
       开关(引擎无反悔 API, 每次 layout 重剥, hn_context.c:66) —— 先开
       再降级, 不透明窗口就顶着一块被剥掉底色的 body, 表现为黑块。 */
    if (!surface_create(w, title)) { hnweb_close(w); return 0; }
    if (w->transparent) hn_context_strip_root_background(ctx);
    if (!app_render(w) || !img_rebuild(w)) {
        fprintf(stderr, "hnweb_linux: 首帧渲染失败\n");
        hnweb_close(w);
        return 0;
    }
    present(w);
    run_load_actions(w); /* hx-trigger="load" 立即执行一次(脏标记由首帧循环上屏) */

    *out = w;
    return 1;
}

/* 热更新: 整文档换入、窗口与焦点保持(HNEngine.swift:308-320 语义,
   引擎侧走 hn_context_render)。上屏由 hnweb_frame 完成。 */
int hnweb_update(hnweb_win *w, const char *html, size_t len) {
    if (!w || !w->ctx || !html || len == 0) return 0;
    hn_context_render(w->ctx, html, len);
    /* 换入内容若自带 hx-* 且缺 id, 同样补编址(管道冒泡要靠 id 寻址) */
    hn_doc_autoid_hx(hn_context_doc(w->ctx));
    /* 旧 DOM 的节点指针全部失效: hover/焦点一并清除 */
    w->hover = NULL;
    w->focus = NULL;
    hn_context_set_hover(w->ctx, NULL);
    hn_context_set_focus(w->ctx, NULL);
    /* 事件队列里暂存的 target 同样指向旧 DOM: 整批作废,
       否则下一次 hnweb_poll 交还的是悬空指针 */
    w->evq_head = 0;
    w->evq_len = 0;
    if (!app_render(w)) return 0; /* 渲染失败(内存)不假装已接受 */
    w->dirty = 1;
    return 1;
}

/* 重排入口(WM_SIZE 语义, hnwin.c:434-455): 尺寸变了才动作 → 重 layout →
   重建背板 → 重渲染上屏。返回 1 表示实际发生了变化。 */
int hnweb_resize(hnweb_win *w, int width_px, int height_px) {
    if (!w || !w->ctx) return 0;
    return win_resize(w, width_px, height_px);
}

/* 喂 :hover + cursor + mouseenter/leave 派发(hn_context_set_hover /
   hn_node_cursor; Swift updateHover:713-736 的统一管道: 离开旧的 →
   进入新的, mousemove 本体不派发 —— 见文件头取舍) */
void hnweb_mouse_move(hnweb_win *w, float x, float y) {
    if (!w || !w->ctx) return;
    hn_node *n = hn_context_hit_node(w->ctx, x, y);
    /* hover 变化才重渲染, 否则每像素移动都重排一遍 */
    if (n != w->hover) {
        if (w->hover) emit_event(w, HN_EV_MOUSELEAVE, w->hover, x, y, 0, 0, 0);
        if (n) emit_event(w, HN_EV_MOUSEENTER, n, x, y, 0, 0, 0);
        w->hover = n;
        hn_context_set_hover(w->ctx, n); /* 触发 :hover 重新匹配 */
        if (app_render(w)) present(w);
    }
    cursor_apply(w, n && hn_node_cursor(n) == 1);
}

/* 清 hover(LeaveNotify ↔ hnwin.c:490-497 WM_MOUSELEAVE); 离开也是事件 */
void hnweb_mouse_leave(hnweb_win *w) {
    if (!w || !w->ctx) return;
    if (w->hover) {
        emit_event(w, HN_EV_MOUSELEAVE, w->hover, -1, -1, 0, 0, 0);
        w->hover = NULL;
        hn_context_set_hover(w->ctx, NULL);
        if (app_render(w)) present(w);
    }
}

/* 命中测试 + 焦点 + hx 派发(M2 走统一管道); 事件经 hnweb_poll 交还。
   门面只声明到 down 为止(up 无槽位): mouseup 由 X11 侧 ButtonRelease
   直接进 on_release。 */
void hnweb_mouse_down(hnweb_win *w, float x, float y, int button) {
    if (!w || !w->ctx) return;
    if (button != 1) return; /* 仍只处理左键(镜像 WM_LBUTTONDOWN) */
    on_press(w, x, y, 0);    /* 门面无修饰键槽位, 嵌入者传 0 */
}

/* delta_y 一格 = 48px(hnwin.c:508 口径不变)。内部 scrollable_at/scroll_by,
   位置变了才 repaint(不重排)—— 与 hnwin WM_MOUSEWHEEL 同一口径。M2:
   实际滚动才派发 SCROLL(位移不变的滚轮是噪声; M1 无条件入队), target =
   滚动容器, hx-trigger="scroll" 据此触发。 */
void hnweb_scroll(hnweb_win *w, float x, float y, float delta_y) {
    if (!w || !w->ctx) return;
    hn_node *s = hn_context_scrollable_at(w->ctx, x, y);
    if (!s) return;
    if (hn_node_scroll_by(s, 0, delta_y)) {
        emit_event(w, HN_EV_SCROLL, s, x, y, 0, 0, delta_y);
        hn_context_repaint(w->ctx);
        free(w->px);
        w->px = HNWEB_RENDER(hn_context_display_list(w->ctx), w->w, w->h, 0x00000000);
        present(w);
    }
}

/* 键事件(M2): keydown/keyup 都走管道(Swift keyDown:978-992 / keyUp:1057)。
   返回 1 = keydown 被 hx 消费 —— 调用方据此跳过默认行为(Esc 关窗等)。 */
static int key_event(hnweb_win *w, int key, const char *utf8, unsigned mods,
                     int release) {
    if (!w || !w->ctx) return 0;
    (void)utf8; /* DOM 键名留给 JS 桥(未移植); hnweb_event 门面只有 key 码 */
    /* 键目标: 焦点 input, 无焦点落文档根(Swift keyDown:984 hitForKeys) */
    hn_node *target = w->focus ? w->focus : hn_doc_root(hn_context_doc(w->ctx));
    if (release) {
        emit_event(w, HN_EV_KEYUP, target, -1, -1, key, mods, 0);
        return 0;
    }
    int consumed = emit_event(w, HN_EV_KEYDOWN, target, -1, -1, key, mods, 0);
    if (consumed) return 1; /* 管道已消费(如 hx-trigger="keydown" 接管), 默认行为让位 */
    if (key == HN_KEY_ENTER && w->focus) {
        /* M1 默认行为保留: 回车提交(hnwin.c:538-550)。与管道互补 ——
           hx-trigger="enter/keydown" 声明的载体在管道里已触发并消费,
           不会走到这里, 天然不双发 */
        hn_node *carrier = hn_node_form_carrier(w->focus);
        if (carrier) {
            const char *url = hn_node_attr(carrier, "hx-post") ? hn_node_attr(carrier, "hx-post")
                                                              : hn_node_attr(carrier, "hx-get");
            printf("[hx-enter] %s\n", url ? url : "?");
            hx_perform(w, carrier, 1);
            fflush(stdout);
        }
    }
    return 0;
}

/* key 用 hn.h 的 HN_KEY_*; Enter → 表单载体提交(hnwin.c:538-550 语义) */
void hnweb_key(hnweb_win *w, int key, const char *utf8) {
    key_event(w, key, utf8, 0, 0); /* 门面无修饰键/抬起语义, 按下 + 无修饰 */
}

/* 非阻塞取一条应用级事件; 有则返回 1。target_id 指向 win 内的暂存槽,
   下一次 poll 之前有效(引擎 arena 的 id 在热更新后可能失效, 故已拷贝)。 */
int hnweb_poll(hnweb_win *w, hnweb_event *out) {
    if (!w || !out || w->evq_len == 0) return 0;
    int i = w->evq_head;
    w->evq_head = (w->evq_head + 1) % HNWEB_EVQ_CAP;
    w->evq_len--;
    memset(out, 0, sizeof *out);
    out->kind = w->evq_kind[i];
    out->x = w->evq_x[i];
    out->y = w->evq_y[i];
    out->target = w->evq_target[i];
    out->key = w->evq_key[i];
    if (w->evq_id[i][0]) {
        snprintf(w->last_id, sizeof w->last_id, "%s", w->evq_id[i]);
        out->target_id = w->last_id;
    }
    return 1;
}

/* 推一帧: 动画 tick + 到期 hx-poll + 脏/动画时渲染上屏。返回 1 = 有上屏。
   平台主循环按 ~16ms 调; 要与其它事件源共存就用它, 别用 hnweb_run。 */
int hnweb_frame(hnweb_win *w, float dt_ms) {
    if (!w || !w->ctx) return 0;
    return frame_step(w, dt_ms);
}

/* 委托式主循环: 阻塞等事件 + 帧步进, 窗口关闭(Esc / 标题栏关闭钮)即返回。
   GetMessage 等价 —— XNextEvent 是纯阻塞的, 用 select(ConnectionNumber) 替代
   SetTimer(hnwin 的 GetMessage 循环 + WM_TIMER); 超时不再是恒 16ms 而是
   idle_timeout_us 的动态值: 有动画 16ms 一拍(hnwin.c:431 同拍点), hx 轮询
   卡着到期点醒, 全闲则无限阻塞 —— 等价于 hnwin 的帧循环, 但空闲不忙等。 */
void hnweb_run(hnweb_win *w) {
    if (!w || !w->xwin || !x11.dpy) return;
    int fd = x11.XConnectionNumber(x11.dpy);
    if (fd < 0) return;
    xevent_buf ev;
    unsigned long last = now_ms();
    while (!w->closed) {
        /* 先清空事件队列再步进一帧(消息驱动的"先派发后 tick"次序) */
        while (x11.XPending(x11.dpy) > 0) {
            x11.XNextEvent(x11.dpy, ev.b);
            win_dispatch(w, ev.b);
            if (w->closed) break;
        }
        if (w->closed) break;
        /* 实测 dt: select 真实时长/事件突发都如实交给 anim_tick, 过渡与
           ext_clock 不再按固定 16ms 步进。上限 100ms: 挂起/断点后的长间隔
           不让动画瞬移(hnwin 的 SetTimer 粗时钟给不了这个精度)。 */
        unsigned long now = now_ms();
        float dt = (float)(now - last);
        last = now;
        if (dt > 100.0f) dt = 100.0f;
        frame_step(w, dt);
        long to = idle_timeout_us(w, now_ms());
        fd_set rfds;
        struct timeval tv, *tvp = NULL;
        FD_ZERO(&rfds);
        FD_SET(fd, &rfds);
        if (to >= 0) {
            tv.tv_sec = to / 1000000L;
            tv.tv_usec = to % 1000000L;
            tvp = &tv;
        }
        select(fd + 1, &rfds, NULL, NULL, tvp); /* -1 超时 = 睡到 X 事件到来 */
    }
}

/* 销毁窗口与引擎上下文。所有权对齐 hn.h:235-239 "谁 set 谁放手":
   交给 context 的 doc/sheet 随 hn_context_destroy 释放; html/css 源与
   位图归窗口, 也在这里释放。 */
void hnweb_close(hnweb_win *w) {
    if (!w) return;
    if (x11.dpy) {
        /* GC 与 colormap 是独立于窗口的服务器资源: 窗口被 WM 强拆
           (win_dispatch 已把 xwin 摘零)时它们依然有效, 照常回收。 */
        if (w->gc) { x11.XFreeGC(x11.dpy, w->gc); w->gc = NULL; }
        if (w->xwin) {
            x11.XDestroyWindow(x11.dpy, w->xwin);
            x11.XFlush(x11.dpy);
            w->xwin = 0;
            if (x11.users > 0) x11.users--;
        }
        if (w->colormap) { x11.XFreeColormap(x11.dpy, w->colormap); w->colormap = 0; }
    }
    img_destroy(w);
    if (w->ctx) {
        hn_context_destroy(w->ctx);
        w->ctx = NULL;
    }
    free(w->px);
    free(w->html);
    free(w->css);
    free(w->store_id);
    free(w);
    x11_release();
}

/* 逃生舱: 引擎上下文只读用法(镜像 HNWebKitHost.swift:27-32 的
   engineContextOrNil 判空哲学) */
hn_context *hnweb_context(hnweb_win *w) {
    return w ? w->ctx : NULL;
}

/* 最近上屏帧 RGBA8: 非预乘、自上而下(与 hnsoft 输出同格式) */
const unsigned char *hnweb_pixels(hnweb_win *w, int *w_out, int *h_out) {
    if (w_out) *w_out = 0;
    if (h_out) *h_out = 0;
    if (!w || !w->px) return NULL;
    if (w_out) *w_out = w->w;
    if (h_out) *h_out = w->h;
    return w->px;
}

/* 当前帧编码 PNG(hnsoft_encode_png)—— 嵌入者的 --shot 出口。
   无头环境(main 的 --probe/--shot 路径)不经过本函数。 */
int hnweb_shot(hnweb_win *w, const char *png_path) {
    if (!w || !w->ctx || !png_path || !png_path[0]) return 0;
    if (!w->px && !app_render(w)) return 0;
    return write_png(w, png_path);
}

/* ---------------- --probe/--shot: 无窗口自检(main 直连, 与 hnwin 同源) ----------------
   这两个路径不触 X11 —— dlopen 失败的无头 CI 也能跑。 */

/* --shot: 渲染位图落盘(与窗口管线同源, 验证"窗口会画什么") */
static int shot_mode(hn_context *ctx, const char *out, int w, int h) {
    hn_context_layout(ctx, (float)w, (float)h, &g_tb);
    const hn_display_list *dl = hn_context_display_list(ctx);
    if (!dl) { fprintf(stderr, "shot: 无绘制指令\n"); return 1; }
    unsigned char *px = HNWEB_RENDER(dl, w, h, 0x00000000);
    if (!px) { fprintf(stderr, "shot: 渲染失败\n"); return 1; }
    size_t pn = 0;
    unsigned char *png = hnsoft_encode_png(px, w, h, &pn);
    free(px);
    if (!png) { fprintf(stderr, "shot: PNG 编码失败\n"); return 1; }
    FILE *of = fopen(out, "wb");
    if (!of) { fprintf(stderr, "shot: 无法写 %s\n", out); free(png); return 1; }
    fwrite(png, 1, pn, of);
    fclose(of);
    free(png);
    printf("已截图 %dx%d → %s (%.1f KB)%s\n", w, h, out, (double)pn / 1024.0,
           hnsoft_font_loaded() ? "" : "  [无字体: 文本未渲染]");
    return 0;
}

/* --probe: 管线统计 + 中心/四角五点命中采样 + 引擎命中自洽门禁 */
static int probe_mode(hn_context *ctx, int w, int h) {
    hn_context_layout(ctx, (float)w, (float)h, &g_tb);
    const hn_display_list *dl = hn_context_display_list(ctx);
    if (!dl) { fprintf(stderr, "probe: 无绘制指令\n"); return 1; }
    printf("cmds=%d\n", dl->count);

    unsigned char *px = HNWEB_RENDER(dl, w, h, 0x00000000);
    if (!px) { fprintf(stderr, "probe: 渲染失败\n"); return 1; }
    long visible = 0;
    for (int i = 0; i < w * h; i++)
        if (px[i * 4 + 3] > 8) visible++;
    printf("canvas=%dx%d visible=%.1f%%\n", w, h, (double)visible * 100.0 / (double)(w * h));
    free(px);

    struct { int x, y; } pts[] = { {w/2,h/2}, {8,8}, {w-8,8}, {8,h-8}, {w-8,h-8} };
    for (size_t i = 0; i < sizeof(pts)/sizeof(pts[0]); i++) {
        const char *id = hn_context_hit_test(ctx, (float)pts[i].x, (float)pts[i].y);
        hn_node *n = hn_context_hit_node(ctx, (float)pts[i].x, (float)pts[i].y);
        printf("hit(%d,%d) -> <%s id=%s>\n", pts[i].x, pts[i].y,
               n ? hn_node_tag(n) : "?", id ? id : "(无id)");
    }
    /* id 命中自洽: 每个带 id 元素的盒中心必须命中自身(或带 id 后代) */
    int idn = 0, idbad = hn_context_verify_hits(ctx, &idn);
    printf("id-hits=%d bad=%d\n", idn, idbad);
    if (idbad) return 1;
    printf("probe OK\n");
    return 0;
}

/* --probe 第二段(M2): 事件管道的无窗口断言。内置自测文档独立于用户输入,
   断言全部是"纯引擎 + 管道机制"的可观测量: :hover 级联改写背景、滚动
   位移与钳制、冒泡路径上的 hx 触发计数、键名映射、静止文档无动画。
   全程不触 X11 —— dlopen 失败的 CI 照跑。返回 0 = 健康。 */
static int probe_fire_count(hn_node *n, void *ud) {
    (void)n; (void)ud;
    return 1;
}

static int probe_events(void) {
    static const char DOC[] =
        "<body>"
        "<div id=\"box\">t</div>"
        "<div id=\"scroller\"><div id=\"fill\">x</div></div>"
        "<div id=\"form1\" hx-trigger=\"keydown enter\" hx-get=\"sys://probe-key\""
        " hx-target=\"form1\"><input id=\"in\" value=\"v\"></div>"
        "<button id=\"btn\" hx-get=\"sys://probe-click\">go</button>"
        "</body>";
    static const char CSS[] =
        "#box{width:80px;height:40px;background:#ff0000}"
        "#box:hover{background:#00ff00}"
        "#scroller{width:80px;height:50px;overflow:auto}"
        "#fill{width:70px;height:200px;background:#0000ff}";
    int fail = 0;
#define PE_CHECK(name, cond) do {                                           \
        printf("probe-events: %-14s %s\n", (name), (cond) ? "OK" : "FAIL"); \
        if (!(cond)) fail = 1;                                              \
    } while (0)

    /* 1) 引擎事件名与 DOM 命名对齐(hn.h:366) */
    PE_CHECK("ev-name",
             !strcmp(hn_event_name(HN_EV_CLICK), "click") &&
             !strcmp(hn_event_name(HN_EV_KEYDOWN), "keydown") &&
             !strcmp(hn_event_name(HN_EV_KEYUP), "keyup") &&
             !strcmp(hn_event_name(HN_EV_MOUSEENTER), "mouseenter"));

    hn_doc *doc = hn_parse_html(DOC, strlen(DOC));
    hn_context *ctx = hn_context_create();
    hn_context_set_doc(ctx, doc);
    hn_context_add_sheet(ctx, hn_parse_css(CSS, strlen(CSS)));
    hn_doc_autoid_hx(doc);
    hn_context_layout(ctx, 320.0f, 240.0f, &g_tb);

    /* 2) hover 状态变化: set_hover + 重布局后 :hover 级联改写 #box 背景
       (ff0000 → 00ff00; 与运行时 hnweb_mouse_move 同一调用序) */
    hn_node *box = hn_doc_find_by_id(doc, "box");
    hn_color bg0 = 0, bg1 = 0;
    if (box) {
        hn_node_debug_background(box, &bg0);
        hn_context_set_hover(ctx, box);
        hn_context_layout(ctx, 320.0f, 240.0f, &g_tb);
        hn_node_debug_background(box, &bg1);
        hn_context_set_hover(ctx, NULL);
        hn_context_layout(ctx, 320.0f, 240.0f, &g_tb);
    }
    PE_CHECK("hover-bg",
             box && ((bg0 >> 24) & 0xFF) > 200 &&   /* hn_color = 0xRRGGBBAA: R 在最高字节 */
             ((bg1 >> 24) & 0xFF) < 60 && ((bg1 >> 16) & 0xFF) > 200);

    /* 3) click 冒泡: 路径第 0 层是自身, #btn 无 hx-trigger → 缺省 click
       触发一次(Swift triggerMatches:865 的空声明语义) */
    hn_node *btn = hn_doc_find_by_id(doc, "btn");
    int depth = 0, fired = 0;
    ev_dispatch(ctx, btn, "click", probe_fire_count, NULL, &depth, &fired);
    PE_CHECK("click-bubble",
             btn && depth >= 1 && fired == 1 && hn_event_path_at(btn, 0) == btn);

    /* 4) keydown 冒泡: input 自身无声明不触发, 祖先 form1 的
       hx-trigger="keydown enter" 触发(路径 = [in, form1], 深度 2);
       键名与 DOM KeyboardEvent.key 对齐 */
    hn_node *inp = hn_doc_find_by_id(doc, "in");
    depth = 0; fired = 0;
    ev_dispatch(ctx, inp, "keydown", probe_fire_count, NULL, &depth, &fired);
    PE_CHECK("keydown-bubble", inp && depth == 2 && fired == 1);
    PE_CHECK("key-name",
             !strcmp(ev_key_name(HN_KEY_ENTER, NULL), "Enter") &&
             !strcmp(ev_key_name(HN_KEY_LEFT, NULL), "ArrowLeft") &&
             !strcmp(ev_key_name(HN_KEY_SPACE, NULL), " ") &&
             !strcmp(ev_key_name(0, "a"), "a") &&
             ev_key_name(0, NULL) == NULL);
    /* trig 语义: 无声明默认 click; hover 键分居 enter/leave 侧; 轮询
       声明不挂数击(hnwin/hx 口径: every Ns 只归帧循环) */
    PE_CHECK("trig-match",
             trig_matches(NULL, "click") && !trig_matches(NULL, "keydown") &&
             trig_matches("hover", "mouseenter") &&
             !trig_matches("hover", "mouseleave") &&
             trig_matches("hover-out", "mouseleave") &&
             trig_matches("keydown", "keydown") &&
             !trig_matches("every 2s", "click"));

    /* 5) 滚动状态变化: 点位解析到容器、范围 = 内容高 - 盒高、
       位移生效且下限钳到 0 */
    hn_node *sc = hn_doc_find_by_id(doc, "scroller");
    int scroll_ok = 0;
    if (sc) {
        float bx, by, bw, bh;
        hn_node_box(sc, &bx, &by, &bw, &bh);
        hn_node *at = hn_context_scrollable_at(ctx, bx + bw / 2, by + bh / 2);
        float mx = 0, my = 0, sy = -1;
        hn_node_scroll_range(sc, &mx, &my);
        scroll_ok = (at == sc) && my > 140.0f &&
                    hn_node_scroll_by(sc, 0, 48.0f) == 1;
        hn_node_scroll_get(sc, NULL, &sy);
        scroll_ok = scroll_ok && sy > 47.0f && sy < 49.0f &&
                    hn_node_scroll_by(sc, 0, -1000.0f) == 1;
        hn_node_scroll_get(sc, NULL, &sy);
        scroll_ok = scroll_ok && sy == 0.0f;
    }
    PE_CHECK("scroll-move", scroll_ok);

    /* 6) 动画时钟: 静止文档初始化(-1)与推进(+16)后都无动画在跑 ——
       帧循环据此进入无限阻塞(空闲零唤醒的前提) */
    PE_CHECK("anim-idle", hn_context_anim_tick(ctx, -1.0f) == 0 &&
                          hn_context_anim_tick(ctx, 16.0f) == 0);

    hn_context_destroy(ctx); /* doc/sheet 归 context("谁 set 谁放手") */

    printf("%s\n", fail ? "probe-events FAIL" : "probe-events OK");
#undef PE_CHECK
    return fail;
}

/* ---------------- main(镜像 hnwin.c:669-839) ---------------- */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "用法: hnweb_linux <file.html> [W H]   开窗口\n"
                        "      hnweb_linux <file.html> --probe [W H]  无窗口自检(管线 + 事件管道断言)\n"
                        "      hnweb_linux <file.html> --shot <png> [W H]  渲染位图落盘\n"
                        "      (尺寸缺省取清单 hn-window, 再缺省 480x700)\n");
        return 2;
    }
    const char *path = argv[1];
    int probe = (argc > 2 && !strcmp(argv[2], "--probe"));
    /* hnwin 要求 --shot 后必须跟 W H(argc>4); 这里放宽到只给路径也能出图,
       X11 侧 CreateWindow 宽高 0 是协议级 BadValue(直接被服务器断连),
       不像 Win32 那样只是开个退化窗口 —— 所以尺寸一律有下限保护。 */
    int shot = (argc > 3 && !strcmp(argv[2], "--shot"));
    int W = 480, H = 700;
    int w_given = 0, h_given = 0;
    if (!probe && !shot) {
        if (argc > 2) { W = atoi(argv[2]); w_given = 1; }
        if (argc > 3) { H = atoi(argv[3]); h_given = 1; }
    } else {
        int a = probe ? 3 : 4;
        if (argc > a) { W = atoi(argv[a]); }
        if (argc > a + 1) { H = atoi(argv[a + 1]); }
    }
    if (W <= 0) W = 480;
    if (H <= 0) H = 700;

    /* 路径口径统一到 HTML 所在目录(资产/link/图片都是相对文档的)。
       顺序: 先按原 cwd 读 HTML, 再 chdir, 之后一切相对解析用 basename。 */
    size_t hlen = 0;
    char *html = read_all(path, &hlen);
    if (!html) { fprintf(stderr, "hnweb_linux: 无法读取 %s\n", path); return 1; }
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            size_t dl = (size_t)(p - path) + 1;
            char *dir = (char *)malloc(dl + 1);
            if (dir) {
                memcpy(dir, path, dl);
                dir[dl] = 0;
                if (chdir(dir) != 0) { /* chdir 失败不影响绝对路径场景 */ }
                free(dir);
            }
        }
    }
    hn_doc *doc = hn_parse_html(html, hlen);
    if (!doc) { fprintf(stderr, "hnweb_linux: 解析失败\n"); free(html); return 1; }

    size_t clen = 0;
    char *css = load_css(base, html, &clen);

    tb_setup();
    /* main 这份 context 只服务 --probe/--shot 与清单读取; 开窗路径由
       hnweb_open 内部重新解析并持有自己的 doc(与 hnwin 单 ctx 不同 ——
       门面把解析/建窗收进了 open, 两份解析是这次收口的代价)。 */
    hn_context *ctx = hn_context_create();
    hn_context_set_doc(ctx, doc);
    if (css && clen) hn_context_add_sheet(ctx, hn_parse_css(css, clen));
    hn_asset_backend ab;
    memset(&ab, 0, sizeof ab);
    ab.load = asset_load;
    hn_context_set_assets(ctx, &ab);
    hn_context_apply_theme(ctx);

    /* 清单: 标题/尺寸/透明背板由 hn 编码声明, 引擎解析 */
    hn_manifest man;
    memset(&man, 0, sizeof man);
    hn_doc_manifest(doc, &man);
    if (man.w > 0 && !w_given && !h_given) W = (int)man.w;
    if (man.h > 0 && !h_given) H = (int)man.h;

    if (probe) {
        int rc = probe_mode(ctx, W, H);
        rc |= probe_events(); /* M2: 事件管道断言(独立自测文档, 聚合退出码) */
        hn_context_destroy(ctx);
        free(css); free(html);
        return rc;
    }
    if (shot) {
        int rc = shot_mode(ctx, argv[3], W, H);
        hn_context_destroy(ctx);
        free(css); free(html);
        return rc;
    }

    /* store id: 文件 stem(dashboard.html → dashboard), 与 hnwin 同口径 */
    char stem[256];
    snprintf(stem, sizeof stem, "%s", base);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;

    hnweb_opts opts;
    memset(&opts, 0, sizeof opts);
    opts.title = NULL;              /* 取 manifest hn-title */
    opts.w = w_given ? (float)W : 0.0f;  /* 0 = 取 manifest hn-window */
    opts.h = h_given ? (float)H : 0.0f;
    opts.transparent = 0;           /* 由 manifest 决定(opts 只能加不能减) */
    opts.resizable = 1;             /* M1 常规窗口(WS_OVERLAPPEDWINDOW 等价) */
    opts.store_id = stem;

    hnweb_win *win = NULL;
    if (!hnweb_open(&opts, html, hlen, css, clen, &win)) {
        hn_context_destroy(ctx);
        free(css); free(html);
        return 1;
    }

    int rw = 0, rh = 0;
    (void)hnweb_pixels(win, &rw, &rh);
    printf("[hnweb] 窗口已开: %s (%dx%d)%s\n",
           man.title && man.title[0] ? man.title : "hnweb", rw, rh,
           man.transparent ? " 透明背板" : "");
    printf("[hnweb] 文本: %s\n", hnsoft_font_loaded()
           ? "FreeType 已加载" : "无字体(等宽估算, 位图不含字形)");
    printf("[hnweb] Esc 或标题栏关闭钮退出\n");
    fflush(stdout);

    hnweb_run(win);

    hnweb_close(win);
    hn_context_destroy(ctx); /* probe/shot 同源的那份; 窗口文档已在 close 释放 */
    free(css);
    free(html);
    return 0;
}

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
     CanJoinAllSpaces = 1<<0 (:130)  跟随所有空间(普通空间)
     MoveToActiveSpace= 1<<1 (:131)  切空间时搬到当前活动空间
     Managed          = 1<<2 (:133)  参与 spaces/exposé(normal 级默认)
     Transient        = 1<<3 (:134)  在 spaces 中浮动, 被 exposé 隐藏
     Stationary       = 1<<4 (:135)  不被 exposé 带走 —— 桌面窗口语义
     IgnoresCycle     = 1<<6 (:138)  不进 Cmd+` 循环(浮动级默认)
     FullScreenAuxiliary = 1<<8 (:146, API_AVAILABLE(macos(10.7)))
                                  "can be shown with the fullscreen window"
                                  (NSWindow.h:118 文档原文) —— 全屏空间旁边可见
   桌宠组合 = CanJoinAllSpaces|Stationary|IgnoresCycle|FullScreenAuxiliary:
     跟所有空间(所有桌面都看得见) + 不随 Mission Control 位移 + 不参与窗口
     循环 + **全屏空间旁边也在**(只有 CanJoinAllSpaces 时全屏空间旁不出现
     —— 组合正确性: "跟随所有空间"声明要含全屏, 必须带 Auxiliary 位)。
   注: CanJoinAllApplications(1<<18, :149)是 macOS 13+ 才有的 beat,
   不开(留 ≤12 可运行); FullScreenAuxiliary 10.7+ 可用, 无版本顾虑。 */
#define HN_CB_CAN_JOIN_ALL_SPACES (1UL << 0)
#define HN_CB_STATIONARY          (1UL << 4)
#define HN_CB_IGNORES_CYCLE       (1UL << 6)
#define HN_CB_FULLSCREEN_AUX      (1UL << 8)
#define HN_CB_PET (HN_CB_CAN_JOIN_ALL_SPACES | HN_CB_STATIONARY | \
                   HN_CB_IGNORES_CYCLE | HN_CB_FULLSCREEN_AUX)

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

    /* ---- 呈现直通(深度 1): 视图自持位图 ---- */
    obj_t cg;              /* 当前内容的 CGImage(blit 建; 视图据此重画) */
    unsigned char *frame;  /* 平台自持帧缓冲(CGImage 的稳定数据源) */
    size_t fcap;           /* frame 容量(字节) */
    int cg_bw, cg_bh;      /* 位图像素尺寸(物理) */
    int cg_lw, cg_lh;      /* 逻辑点尺寸(= 内容区 CSS px) */

    /* ---- 显示生命周期(深度 3) ---- */
    float scale;           /* 最近一次读到的 backing scale */
    int   scale_dirty;     /* 视图报 backing 变化 → pump 发 HNP_EV_SCALE_CHANGED */
    int   screen_dirty;    /* NSScreen 参数通知 → pump 发 HNP_EV_SCREEN_CHANGED */
    int   focus_dirty;     /* 键窗口变化 → pump 发 FOCUS_IN/OUT */
    int   focus_now;       /* 当前是否键窗口 */

    /* ---- IME(深度 2) ---- */
    char  marked[512];     /* 预编辑串(组字中); 空 = 无组字 */
    int   marked_len;
    int   marked_cursor;   /* 预编辑串内光标(字节偏移) */
    int   fx, fy, fw, fh;  /* 可编辑焦点盒(逻辑 px, 引擎坐标; fw/fh=0 无焦点) */
    int   ime_active;      /* 输入上下文激活态(焦点盒非空时置位) */
    obj_t ictx;            /* NSTextInputContext(*自己 alloc 的*; 见
                              hn_view_input_context 的实测注释 —— NSImageView
                              的父类实现恒 nil, 必须自建) */
};

/* 前向声明: 视图类注册/窗口映射(在 window_open 之前定义, 但 hnp_window
   结构体引用不透明句柄, 声明放这里)。 */
static void hn_ensure_notifications(void);
static void hn_ime_queue_push(hnp_window *w, const char *s, int cursor, int commit);

/* ---------------- 原生视频图层: 与媒体后端的对接口 + (win, id) → 子图层 表 ----
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
   NULL, 调用方回退到原始 NSImageView(退化为"不可拖")。

   ---- 本轮扩展(呈现直通 + IME; 同一运行期子类追加覆写) ----
   呈现直通(深度 1): drawRect: 用 CGContext 直画窗口持有的 CGImage ——
   位图数据来自 hnp_window_blit 的 BGRA 缓冲, blit 时 CGImageCreate 一次
   (视图持引用), 之后每帧 setNeedsDisplay: 触发系统重画, 不再每帧造
   NSImage/CGBitmapContext。drawRect 里 ctx 的 CTM 做 (0,h) 平移 + y 翻转,
   把"原点左上、y 向下"的位图口径摆正到 AppKit 左下原点坐标系。
   IME(深度 2): NSTextInputClient 关键方法经 class_addMethod 挂在同一类上
   —— **协议方法只需实现即可被系统识别**(AppKit 不看 class_addProtocol,
   只看 respondsToSelector:; NSView.inputContext 非 nil 的前提正是视图
   响应 NSTextInputClient 必需选择器), 因此不 dlopen 协议、不链 .m。
   生命周期(深度 3): viewDidChangeBackingProperties 覆写, 读新 backingScale
   写进窗口侧待决标志, hnp_pump 下一拍发 HNP_EV_SCALE_CHANGED。

   状态归属: 视图方法经 [self window] 拿 NSWindow, 再从 g_win_map 反查
   hnp_window —— 全部状态(位图 CGImage/预编辑串/焦点盒)挂在 hnp_window
   上, 不往运行期类加 ivar(objc_allocateClassPair 的 ivar 偏移管理在
   class_addIvar 路径上繁琐且易错; 本文件既有 g_vl 表的同款"静态映射"手法
   已被验证)。 */

/* NSWindow 指针 → hnp_window 映射(进程一窗, 定长表足够)。
   **注意**: 事件里的窗口经指针比对在多数场景可用, 但 NSEvent 从队列取出
   时 [e window] 返回的 NSWindow 对象与 postEvent: 时构造用的可能不是同一
   实例(实测: 合成事件的路由指针比对落空) —— 因此映射同时记 windowNumber
   (long, NSWindow 稳定编号), 查找优先 windowNumber。视图侧的 [self window]
   走指针(同进程同对象), 两条路径共用同一张表。 */
#define HN_WIN_MAP_CAP 8
static struct { obj_t win; long wnum; hnp_window *hw; } g_win_map[HN_WIN_MAP_CAP];
static int g_win_map_n;

static hnp_window *hn_hw_from_self(obj_t self) {
    if (!self) return NULL;
    obj_t win = MSG0(obj_t, self, "window");
    if (!win) return NULL;
    long num = ((long (*)(obj_t, sel_t))o_msgSend)(win, o_sel("windowNumber"));
    for (int i = 0; i < g_win_map_n; i++)
        if (g_win_map[i].wnum == num) return g_win_map[i].hw;
    return NULL;
}

/* 事件窗口 → hnp_window(优先 windowNumber, 指针兜底) */
static hnp_window *hn_hw_from_event(obj_t e) {
    if (!e) return NULL;
    obj_t win = MSG0(obj_t, e, "window");
    if (!win) return NULL;
    long num = ((long (*)(obj_t, sel_t))o_msgSend)(win, o_sel("windowNumber"));
    for (int i = 0; i < g_win_map_n; i++)
        if (g_win_map[i].wnum == num) return g_win_map[i].hw;
    for (int i = 0; i < g_win_map_n; i++)
        if (g_win_map[i].win == win) return g_win_map[i].hw;
    return NULL;
}

static signed char hn_view_can_move_window(obj_t self, sel_t cmd) {
    (void)self; (void)cmd;
    /* 恒 1 = "可见内容即拖动面"(无边框桌宠没有标题栏)。命中区的收口
       不在这里而在 hitTest 覆写(见下): 全透明像素处 hit 视图缺席,
       AppKit 根本不会带着"这个视图能拖吗"的问题走到本方法 —— 拖拽
       命中区因此只在**有像素的内容上**, 看不见的透明矩形不可拖。 */
    return 1;
}

/* ================= 逐像素命中(透明像素点击穿透) =================

   契约(hn_platform.h hn-transparent / 本次任务): 透明窗口的**全透明
   像素应穿透到桌面/下层窗口, 半透明(alpha>0)可点**。三层机制各管一段:
     ① WindowServer 层(系统免费给): isOpaque=NO 的窗口, WindowServer
        对鼠标按下做**逐像素**路由 —— alpha≈0 处的点击不投递给本应用
        (实测口径: 透明区点击"aren't delivered to your application at
        all", 直接落到下层窗口; 小 alpha(~≤0.05)也有穿透报告 —— 边界
        带以系统为准, 应用层不要依赖窄阈值的精确行为)。
     ② AppKit 视图层(本文件 hitTest 覆写): 事件真被投递进来时(键窗口/
        边界带等场景), hit 视图缺席 → NSWindow 不处理、不发背景拖拽 ——
        与 ① 同向收口: 拖拽/点击的命中区 = 有像素的内容。
     ③ 引擎出口(hnp_pump BUTTON_DOWN 过滤, 见 pump): 引擎用自己的
        x/y 做命中测试, 不经过 AppKit —— 全透明像素的按下在此丢弃,
        保证"透明处点击不落引擎"不依赖系统行为(确定性锚)。
   采样源 = 平台自持帧缓冲 w->frame(BGRA 预乘, 原点左上 —— 与 blit 同
   源同拍, 无额外状态)。BGRA 小端字节序: alpha 在第 4 字节(offset 3);
   预乘不改 alpha 值, 直接读即可。 */

/* 内容区坐标(引擎口径, 左上原点, CSS px)→ 帧缓冲 alpha 采样。
   非透明窗/尚无首帧: 恒 255(与旧行为逐位一致 —— 过滤永不触发)。 */
static unsigned char hn_alpha_at(hnp_window *w, int x, int y) {
    if (!w || !w->transparent || !w->frame ||
        w->cg_bw <= 0 || w->cg_bh <= 0 || w->cg_lw <= 0 || w->cg_lh <= 0)
        return 255;
    int bx = (int)((double)x * (double)w->cg_bw / (double)w->cg_lw);
    int by = (int)((double)y * (double)w->cg_bh / (double)w->cg_lh);
    if (bx < 0) bx = 0;
    if (by < 0) by = 0;
    if (bx >= w->cg_bw) bx = w->cg_bw - 1;
    if (by >= w->cg_bh) by = w->cg_bh - 1;
    return w->frame[((size_t)by * (size_t)w->cg_bw + (size_t)bx) * 4 + 3];
}

/* hitTest: — 视图层的逐像素收口(机制②)。point 在**父视图坐标系**
   (NSView.h:220 的文档口径: "point 位于视图的父视图的坐标系中")。
   注意父视图不总是"铺满内容区的容器": 有标题栏窗口(style=1|2|4|8)的
   contentView 挂在主题框下, frame=(0, 标题栏高) —— 直接拿 point 判界
   会把内容顶部一条带(误判越界)返回 nil, 而标题条区域的点反被本视图
   认领(2026-10-14 复审修复)。故先按自身 frame 把 point 平移到**本视图
   局部坐标**再判界; 无边框窗(透明窗恒 style=0)frame 原点是 (0,0),
   平移为零, 逐像素契约路径逐位不变。
   局部坐标不翻转(原点左下): engine_y = 内容高 − ly。全透明像素返回
   nil(无命中视图 → NSWindow 不路由/不拖拽); 越界与半透明返回 self
   (正常命中)。反查失败(映射满表等)退默认 self —— 不因记账缺失改变
   既有可点行为。 */
static obj_t hn_view_hit_test(obj_t self, sel_t cmd, CGPoint p) {
    (void)cmd;
    hnp_window *w = hn_hw_from_self(self);
    if (!w) return self;
    CGRect f = MSG0(CGRect, self, "frame");
    double lx = p.x - f.origin.x, ly = p.y - f.origin.y;
    if (lx < 0 || ly < 0 || lx >= (double)w->w || ly >= (double)w->h)
        return NULL;
    int ey = w->h - (int)ly;
    if (hn_alpha_at(w, (int)lx, ey) == 0)
        return NULL;   /* 全透明像素: 无命中视图 → 事件不进视图、不拖窗 */
    return self;
}

/* ================= 深度 1: drawRect: 直呈 ================= */

/* drawRect: — 把窗口持有的 CGImage 直接画进当前 CGContext。
   AppKit 视图坐标系原点在左下(y 向上), 位图口径是原点左上(y 向下):
   先把 CTM 平移到 (0, h) 再对 y 取负(scale 1,-1), 此后 ctx 内 y 向下,
   CGContextDrawImage 的目标矩形 (0,0,w,h) 即"左上原点铺满视图"。
   w/h 用窗口的**逻辑点尺寸**(内容区 CSS px)—— 位图可是 2x 物理像素,
   CGImage 自身尺寸与绘制矩形不同时 CG 自动做图像缩放(Retina 上
   1 物理像素落 1 屏像素: 位图 720px 画进 360pt 的 2x 背衬)。 */
static void hn_view_draw_rect(obj_t self, sel_t cmd, CGRect dirty) {
    hnp_window *w = hn_hw_from_self(self);
    /* 无自持位图(回退路径 HN_BLIT_FALLBACK=1 / 尚未首次 blit): 转交
       NSImageView 的默认绘制实现(它负责 setImage: 的来源画)—— 不这么做
       回退路径整窗空白(实测: 覆写 drawRect: 而父类实现不调用, 旧 NSImage
       路径的 image 没有任何人画)。父类 IMP 经 class_getMethodImplementation
       取(避免 objc_msgSendSuper 的 super 结构体)。 */
    if (!w || !w->cg) {
        void *(*get_imp)(obj_t, sel_t) =
            (void *(*)(obj_t, sel_t))dlsym(RTLD_DEFAULT, "class_getMethodImplementation");
        void *imp = get_imp ? get_imp(o_getClass("NSImageView"), o_sel("drawRect:")) : NULL;
        if (imp && imp != (void *)hn_view_draw_rect)
            ((void (*)(obj_t, sel_t, CGRect))imp)(self, o_sel("drawRect:"), dirty);
        return;
    }
    (void)cmd;
    obj_t ctx = MSG0(obj_t, o_getClass("NSGraphicsContext"), "currentContext");
    if (!ctx) return;
    void *cgctx = ((void *(*)(obj_t, sel_t))o_msgSend)(ctx, o_sel("CGContext"));
    if (!cgctx) return;
    double lw = w->cg_lw > 0 ? (double)w->cg_lw : (double)w->cg_bw;
    double lh = w->cg_lh > 0 ? (double)w->cg_lh : (double)w->cg_bh;
    if (lw <= 0 || lh <= 0) return;
    void (*di)(void *, obj_t, CGRect) = (void (*)(void *, obj_t, CGRect))dlsym(RTLD_DEFAULT, "CGContextDrawImage");
    if (!di) return;
    /* **不要翻转**(实测, 2026-10): 视图 drawRect: 的 CGContext 里
       CGContextDrawImage 已按"位图数据行 0 = 显示顶部"落位 —— 按其文档
       口径 CGImage 的 (0,0) 是左上、而 CG 目标矩形也按左上锚, 二者同向。
       加 translate(0,h)+scale(1,-1) 翻转 → 画面整体上下颠倒(本机实测:
       静态页红块从 y≈20 跑到 y≈300, 与旧 NSImage 路径逐像素对照暴露)。
       教训: 别再凭"AppKit 左下原点"直觉加翻转 —— drawRect 的上下文已经
       把坐标系摆成位图口径。 */
    CGRect r = { { 0, 0 }, { lw, lh } };
    di(cgctx, w->cg, r);
}

/* 抗锯齿插值: 位图与视图点尺寸不一致时(2x 位图 → 1x 屏)由 IAD 决定。
   缺省 CG 在放大/缩小时会插值, 观感与旧 NSImage 路径一致(旧路径由
   AppKit 走同一 CG 缩放), 不动它。 */

/* ================= 深度 3: backing scale 变化 ================= */

/* viewDidChangeBackingProperties — 窗口从 Retina 拖到非 Retina 屏(或
   反向)时 AppKit 调它。这里只置脏标志, 不直接回调引擎(引擎在主循环
   里, 视图回调线程 = 主线程; 但保持"事件只从 hnp_pump 出"的单口纪律),
   hnp_pump 下一拍读到脏就发 HNP_EV_SCALE_CHANGED 并附新 scale。 */
static void hn_view_backing_changed(obj_t self, sel_t cmd) {
    (void)cmd;
    hnp_window *w = hn_hw_from_self(self);
    if (!w) return;
    w->scale_dirty = 1;
}

/* ================= 深度 2: NSTextInputClient ================= */

/* 说明(协议识别机制): AppKit 判断一个视图能否作为输入客户端, 走
   -[NSView inputContext] → 内部查 respondsToSelector: 的
   NSTextInputClient 必需方法集(insertText:replacementRange: /
   doCommandBySelector: / setMarkedText:... / unmarkText / selectedRange /
   markedRange / hasMarkedText / attributedSubstringForProposedRange: /
   validAttributesForMarkedText / firstRectForCharacterRange: /
   characterIndexForPoint:)。**不需要** class_addProtocol / 不 dlopen
   Protocol —— 方法实现了就"是"客户端。因此本段全部 class_addMethod。

   类型编码(逐字节抄 objc 规范; BOOL=char 'c', NSRange 是结构体):
     insertText:replacementRange:  v@:@@ 之后的 NSRange 以 {_NSRange=QQ} 拼
     setMarkedText:selectedRange:replacementRange:  v@:@@{_NSRange=QQ}{_NSRange=QQ}
     unmarkText / doCommandBySelector:  v@:@ / v@::
     hasMarkedText  c@: ; markedRange/selectedRange  {_NSRange=QQ}@:
     firstRectForCharacterRange:actualRange:  {CGRect={CGPoint=dd}{CGSize=dd}}@:{_NSRange=QQ}^v
   NSRange 是两个 NSUInteger(arm64 = 两个 unsigned long = {Q,Q})。 */

typedef struct { unsigned long loc, len; } hn_nsrange;
typedef struct { double x, y; } hn_cgpoint2;
typedef struct { double w, h; } hn_cgsize2;
typedef struct { hn_cgpoint2 origin; hn_cgsize2 size; } hn_cgrect2;

/* 从任意对象取 UTF-8 串(String/NSAttributedString)。
   ⚠️ 实测教训: NSString **没有** -string 选择器(那是 NSAttributedString 的),
   对 NSString 直接发 -string 会抛 NSInvalidArgumentException 崩在输入法
   回调里(本机实测: "unrecognized selector sent to instance")。必须先
   respondsToSelector: 探。 */
static const char *hn_nsstr_utf8(obj_t o) {
    if (!o) return NULL;
    obj_t s = o;
    if (((signed char (*)(obj_t, sel_t, sel_t))o_msgSend)(o, o_sel("respondsToSelector:"),
                                                         o_sel("string")))
        s = MSG0(obj_t, o, "string");
    const char *c = ((const char *(*)(obj_t, sel_t))o_msgSend)(s, o_sel("UTF8String"));
    return c;
}

/* 挂起队列: insertText/setMarkedText 是**系统在 interpretKeyEvents 内同步
   回调**我们的, 而本文件的 pump 才有事件出口 —— 回调只把串推进窗口级
   挂起队列, 下一次 hnp_pump 按序取出为 HNP_EV_IME_PREEDIT/COMMIT。
   每窗口 8 槽; IME 事件低频, 同窗同型(preedit/preedit 或 commit/commit)
   合并为"只留最新", 槽满丢最旧。 */
#define HN_IME_Q_CAP 8
typedef struct {
    hnp_window *win; unsigned char used; unsigned char commit;
    int cursor; char utf8[512];
} hn_ime_ent;
static hn_ime_ent g_ime_q[HN_IME_Q_CAP];

static void hn_ime_queue_push(hnp_window *w, const char *s, int cursor, int commit) {
    /* 同窗同型合并(预编辑高频刷新只保留最新; commit 不合并 —— 两次
       提交是两次编辑, 但本引擎同帧内连续 commit 极罕见, 合并也安全) */
    for (int i = 0; i < HN_IME_Q_CAP; i++)
        if (!g_ime_q[i].used) {
            g_ime_q[i].win = w;
            g_ime_q[i].used = 1;
            g_ime_q[i].commit = (unsigned char)(commit ? 1 : 0);
            g_ime_q[i].cursor = cursor;
            snprintf(g_ime_q[i].utf8, sizeof(g_ime_q[i].utf8), "%s", s ? s : "");
            return;
        }
    /* 满: 整体前移一格, 丢最旧 */
    for (int i = 0; i < HN_IME_Q_CAP - 1; i++) g_ime_q[i] = g_ime_q[i + 1];
    g_ime_q[HN_IME_Q_CAP - 1].win = w;
    g_ime_q[HN_IME_Q_CAP - 1].used = 1;
    g_ime_q[HN_IME_Q_CAP - 1].commit = (unsigned char)(commit ? 1 : 0);
    g_ime_q[HN_IME_Q_CAP - 1].cursor = cursor;
    snprintf(g_ime_q[HN_IME_Q_CAP - 1].utf8, sizeof(g_ime_q[HN_IME_Q_CAP - 1].utf8),
             "%s", s ? s : "");
}

/* insertText:replacementRange: — 系统提交最终文本(选字上屏/直接输入)。
   markedText 上屏的语义: 先清预编辑态, 再记挂起提交串。 */
static void hn_view_insert_text(obj_t self, sel_t cmd, obj_t string, hn_nsrange rep) {
    (void)cmd; (void)rep;
    hnp_window *w = hn_hw_from_self(self);
    if (!w) return;
    const char *s = hn_nsstr_utf8(string);
    if (!s) return;
    hn_ime_queue_push(w, "", 0, 0);   /* PREEDIT "" = 组字结束 */
    hn_ime_queue_push(w, s, 0, 1);    /* 1 = commit */
    w->marked[0] = 0;
    w->marked_len = 0;
}

/* setMarkedText:selectedRange:replacementRange: — 组字中间态(拼音串/候选)。
   string 为 nil/空 = 取消组字。 */
static void hn_view_set_marked(obj_t self, sel_t cmd, obj_t string, hn_nsrange sel, hn_nsrange rep) {
    (void)cmd; (void)rep;
    hnp_window *w = hn_hw_from_self(self);
    if (!w) return;
    const char *s = hn_nsstr_utf8(string);
    if (!s) s = "";
    hn_ime_queue_push(w, s, (int)sel.loc, 0);
    size_t n = strlen(s);
    if (n >= sizeof(w->marked)) n = sizeof(w->marked) - 1;
    memcpy(w->marked, s, n);
    w->marked[n] = 0;
    w->marked_len = (int)n;
    w->marked_cursor = (int)sel.loc;
}

/* unmarkText — 系统要求彻底清预编辑(不改 value) */
static void hn_view_unmark(obj_t self, sel_t cmd) {
    (void)cmd;
    hnp_window *w = hn_hw_from_self(self);
    if (!w) return;
    if (w->marked_len) hn_ime_queue_push(w, "", 0, 0);
    w->marked[0] = 0;
    w->marked_len = 0;
}

/* doCommandBySelector: — 命令键(方向/回车/退格等)经键绑定机制回灌。
   这些键**同时**已作为 HNP_EV_KEY_* 事件进引擎(hnp_pump keydown 分支),
   这里不重复上屏(避免双份) —— 只留桩: 组字期间系统会先送命令键,
   引擎侧预编辑由 PREEDIT ""/commit 事件自然收敛。 */
static void hn_view_do_command(obj_t self, sel_t cmd, sel_t action) {
    (void)self; (void)cmd; (void)action;
}

/* hasMarkedText → BOOL */
static signed char hn_view_has_marked(obj_t self, sel_t cmd) {
    (void)cmd;
    hnp_window *w = hn_hw_from_self(self);
    return (w && w->marked_len > 0) ? 1 : 0;
}

/* NSNotFound = NSIntegerMax(64 位)= 0x7FFFFFFFFFFFFFFF */
#define HN_NSNOTFOUND 0x7FFFFFFFFFFFFFFFL
static hn_nsrange hn_view_marked_range(obj_t self, sel_t cmd) {
    (void)cmd;
    hnp_window *w = hn_hw_from_self(self);
    hn_nsrange r;
    if (w && w->marked_len > 0) { r.loc = 0; r.len = (unsigned long)w->marked_len; }
    else { r.loc = HN_NSNOTFOUND; r.len = 0; }
    return r;
}

/* selectedRange → {0,0}(本引擎不向系统暴露文档内容, 见下) */
static hn_nsrange hn_view_selected_range(obj_t self, sel_t cmd) {
    (void)self; (void)cmd;
    hn_nsrange r; r.loc = 0; r.len = 0;
    return r;
}

/* firstRectForCharacterRange:actualRange: — 候选窗/输入法图标的位置。
   返回**屏幕坐标**(AppKit 左下原点全局坐标): 焦点盒(引擎左上原点,
   内容区坐标)→ 视图坐标(未翻转, 原点左下: y = 内容高 − y − h)→
   NSWindow convertRectToScreen:。无焦点盒时返回零矩形(系统把候选窗
   摆在窗口附近; 不是错误)。 */
static hn_cgrect2 hn_view_first_rect(obj_t self, sel_t cmd, hn_nsrange range, void *actual) {
    (void)cmd; (void)range; (void)actual;
    hn_cgrect2 z; z.origin.x = z.origin.y = z.size.w = z.size.h = 0;
    hnp_window *w = hn_hw_from_self(self);
    if (!w || !w->win || w->fw <= 0 || w->fh <= 0) return z;
    double ch = (double)w->h;
    double vx = (double)w->fx;
    double vy = ch - (double)w->fy - (double)w->fh;
    CGRect r = { { vx, vy }, { (double)w->fw, (double)w->fh } };
    CGRect sr = ((CGRect (*)(obj_t, sel_t, CGRect))o_msgSend)(
        w->win, o_sel("convertRectToScreen:"), r);
    hn_cgrect2 out;
    out.origin.x = sr.origin.x; out.origin.y = sr.origin.y;
    out.size.w = sr.size.w; out.size.h = sr.size.h;
    return out;
}

/* characterIndexForPoint: — 鼠标点选组字候选(引擎不映射指针到字符, 返回 0) */
static unsigned long hn_view_char_index(obj_t self, sel_t cmd, hn_cgpoint2 p) {
    (void)self; (void)cmd; (void)p;
    return 0;
}

/* attributedSubstringForProposedRange:actualRange: — 返回 nil:
   引擎不把编辑文本暴露给输入法(拼音输入不需要读回文本; 候选/联想词
   服务会因此退化, 属已知取舍, 注释写明) */
static obj_t hn_view_attr_substring(obj_t self, sel_t cmd, hn_nsrange range, void *actual) {
    (void)self; (void)cmd; (void)range; (void)actual;
    return NULL;
}

/* validAttributesForMarkedText → 空数组(AppKit 期望非 nil) */
static obj_t hn_view_valid_attrs(obj_t self, sel_t cmd) {
    (void)self; (void)cmd;
    return MSG0(obj_t, o_getClass("NSArray"), "array");
}

/* inputContext — **必须自己覆写**。实测(本机 macOS 26, 2026-10):
   NSImageView 覆写了 -inputContext 并恒返回 nil(plain NSImageView 与
   "子类+class_addProtocol"都拿到 nil; 只有父类换成 NSView 时才由 AppKit
   自动建上下文)。所以本类显式实现: 惰性 alloc/initWithClient:(self) 一个
   NSTextInputContext 并缓存(每视图一份; 进程窗口少, 一窗一份足够)。
   缓存槽挂在 hnp_window(经 hn_hw_from_self 反查, 同其他状态的口径)。 */
static obj_t hn_view_input_context(obj_t self, sel_t cmd) {
    (void)cmd;
    hnp_window *w = hn_hw_from_self(self);
    if (!w) return NULL;
    if (!w->ictx) {
        obj_t cls = o_getClass("NSTextInputContext");
        obj_t o = cls ? MSG0(obj_t, cls, "alloc") : NULL;
        if (!o) return NULL;
        obj_t ctx = ((obj_t (*)(obj_t, sel_t, obj_t))o_msgSend)(
            o, o_sel("initWithClient:"), self);
        if (!ctx) { MSG0(void, o, "release"); return NULL; }
        w->ictx = ctx;
    }
    return w->ictx;
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
    /* ---- 逐像素命中(hitTest; 见"逐像素命中"段注释) ----
       "@@:{CGPoint=dd}" = NSView* 返回, self, _cmd, NSPoint 参数。 */
    add_method(cls, o_sel("hitTest:"), (void (*)(void))hn_view_hit_test,
               "@@:{CGPoint=dd}");
    /* ---- 深度 1: 直呈 ---- */
    add_method(cls, o_sel("drawRect:"), (void (*)(void))hn_view_draw_rect,
               "v@:{CGRect={CGPoint=dd}{CGSize=dd}}");
    /* ---- 深度 3: backing 变化 ---- */
    add_method(cls, o_sel("viewDidChangeBackingProperties"),
               (void (*)(void))hn_view_backing_changed, "v@:");
    /* ---- 深度 2: NSTextInputClient ----
       ① class_addProtocol: **实测必需** —— 只 add_method 时
       [view inputContext] 返回 nil(本轮实测), class_addProtocol 后
       inputContext 非 nil 且 conformsToProtocol: 为真。这与"协议方法只需
       respondsToSelector"的常见说法不同: NSView.inputContext 的判定走
       class_conformsToProtocol(在 macOS 14/15 上实测如此), 因此协议本身
       也要挂。objc_getProtocol("NSTextInputClient") 在 AppKit 装载后可用。
       ② 方法编码见上方注释块。 */
    {
        void *(*getproto)(const char *) = (void *(*)(const char *))dlsym(RTLD_DEFAULT, "objc_getProtocol");
        signed char (*addproto)(obj_t, void *) = (signed char (*)(obj_t, void *))dlsym(RTLD_DEFAULT, "class_addProtocol");
        void *proto = getproto ? getproto("NSTextInputClient") : NULL;
        if (proto && addproto) addproto(cls, proto);
    }
    add_method(cls, o_sel("insertText:replacementRange:"),
               (void (*)(void))hn_view_insert_text, "v@:@{_NSRange=QQ}");
    add_method(cls, o_sel("setMarkedText:selectedRange:replacementRange:"),
               (void (*)(void))hn_view_set_marked, "v@:@{_NSRange=QQ}{_NSRange=QQ}");
    add_method(cls, o_sel("unmarkText"), (void (*)(void))hn_view_unmark, "v@:");
    add_method(cls, o_sel("doCommandBySelector:"), (void (*)(void))hn_view_do_command, "v@::");
    add_method(cls, o_sel("hasMarkedText"), (void (*)(void))hn_view_has_marked, "c@:");
    add_method(cls, o_sel("markedRange"), (void (*)(void))hn_view_marked_range, "{_NSRange=QQ}@:");
    add_method(cls, o_sel("selectedRange"), (void (*)(void))hn_view_selected_range, "{_NSRange=QQ}@:");
    add_method(cls, o_sel("firstRectForCharacterRange:actualRange:"),
               (void (*)(void))hn_view_first_rect,
               "{CGRect={CGPoint=dd}{CGSize=dd}}@:{_NSRange=QQ}^v");
    add_method(cls, o_sel("characterIndexForPoint:"), (void (*)(void))hn_view_char_index,
               "Q@:{CGPoint=dd}");
    add_method(cls, o_sel("attributedSubstringForProposedRange:actualRange:"),
               (void (*)(void))hn_view_attr_substring, "@@:{_NSRange=QQ}^v");
    add_method(cls, o_sel("validAttributesForMarkedText"),
               (void (*)(void))hn_view_valid_attrs, "@@:");
    /* inputContext: 必须自建(NSImageView 的父类实现恒 nil; 见
       hn_view_input_context 的实测注释) */
    add_method(cls, o_sel("inputContext"),
               (void (*)(void))hn_view_input_context, "@@:");
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
    /* 无边框 + 可拖: 桌宠没有标题栏可拖, 可见内容就是唯一拖动面。
       movableByWindowBackground(NSWindow.h:414) 生效面 = hitTest 命中的
       视图(逐像素收口见 hn_view_hit_test —— 全透明像素无命中视图,
       不可点也不可拖; 拖拽命中区只在内容上)。启用面: borderless 显式
       无边框之外, transparent 也隐含无边框(style=0, 见上方 styleMask
       注释)却此前不启用拖拽 —— 同样没有标题栏, 一并启用; 再过 manifest
       的 hn-draggable 总开关(默认开, 页面 "0" 显式关闭)。 */
    if ((d->borderless || d->transparent) && d->draggable)
        MSGI(void, win, "setMovableByWindowBackground:", 1);

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
    /* 深度 3: 初始 scale 记账(hnp_pump 只在变化时发 SCALE_CHANGED) */
    {
        double s = ((double (*)(obj_t, sel_t))o_msgSend)(win, o_sel("backingScaleFactor"));
        if (!(s >= 1.0)) s = 1.0;
        w->scale = (float)s;
    }
    /* 窗口→hnp_window 映射(视图回调经 [self window] 反查; 满表丢弃 ——
       表满时视图方法退化为 no-op, 窗口本身照常工作) */
    if (g_win_map_n < HN_WIN_MAP_CAP) {
        g_win_map[g_win_map_n].win = win;
        g_win_map[g_win_map_n].wnum = ((long (*)(obj_t, sel_t))o_msgSend)(win, o_sel("windowNumber"));
        g_win_map[g_win_map_n].hw = w;
        g_win_map_n++;
    }
    hn_ensure_notifications();
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
    /* 窗口映射表摘除(视图回调随后查不到 → no-op) */
    for (int i = 0; i < g_win_map_n; i++) {
        if (g_win_map[i].hw == w) {
            g_win_map[i] = g_win_map[g_win_map_n - 1];
            g_win_map_n--;
            i--;
        }
    }
    /* IME 挂起队列里属于本窗的槽清掉(悬垂指针防线) */
    for (int i = 0; i < HN_IME_Q_CAP; i++)
        if (g_ime_q[i].used && g_ime_q[i].win == w) g_ime_q[i].used = 0;
    /* 呈现直通资源: 先释放 CGImage(数据源指向 frame), 再 free frame */
    if (w->cg) ((void (*)(CGImageRef))dlsym(RTLD_DEFAULT, "CGImageRelease"))(w->cg);
    free(w->frame);
    if (w->ictx) { MSG0(void, w->ictx, "deactivate"); MSG0(void, w->ictx, "release"); }
    if (w->img) MSG0(void, w->img, "release");
    if (w->win) { MSG0(void, w->win, "close"); MSG0(void, w->win, "release"); }
    free(w);
}

void hnp_window_resize(hnp_window *w, int nw, int nh) {
    if (!w || !w->iv) return;
    w->w = nw; w->h = nh;
    /* 逻辑点尺寸跟动(呈现矩形 = drawRect 的绘制矩形) */
    if (w->cg) { w->cg_lw = nw; w->cg_lh = nh; }
    CGRect cr = { { 0, 0 }, { (double)nw, (double)nh } };
    ((void (*)(obj_t, sel_t, CGRect))o_msgSend)(w->iv, o_sel("setFrame:"), cr);
    MSG1(void, w->iv, "setNeedsDisplay:", (obj_t)1);
}

/* 移动窗口(引擎口径左上原点 → AppKit 左下原点, 主屏高翻转)。 */
void hnp_window_move(hnp_window *w, int x, int y) {
    if (!w || !w->win) return;
    CGRect f = MSG0(CGRect, w->win, "frame");
    CGPoint o = { (double)x, hn_primary_screen_height() - (double)y - f.size.h };
    ((void (*)(obj_t, sel_t, CGPoint))o_msgSend)(w->win, o_sel("setFrameOrigin:"), o);
}

/* ================= 呈现: 位图缓冲 → CGImage(一次) =================
   旧路径(现回退路径): 每帧 CGImageCreate + NSImage initWithCGImage:size: +
   setImage: —— 每帧两次对象分配 + NSImage 内部表示/缓存拷贝。
   新路径(缺省): **窗口自持帧缓冲**(cap 增长一次, 之后每帧只 memcpy),
   CGImage 的数据提供者直接引用该稳定缓冲(无释放回调 —— 缓冲与窗口同
   生命周期, close 时先释放 CGImage 再 free 缓冲), 视图持 CGImage 引用,
   drawRect 直画。每帧成本 = 一次 memcpy(必付: 引擎下帧会重写自己的
   缓冲, 平台不拷就会画到半新半旧的数据) + 零分配 + 零 AppKit 图像机制。
   安全性: 缓冲/视图/display 全在主线程同一拍内, 无竞态; EXPOSE 重画时
   读的是平台自持缓冲(不是引擎已释放的旧位图) —— 旧 NSImage 路径靠
   AppKit 内部拷贝达到同样效果, 新路径靠自持缓冲, 等价且更省。 */
static CGImageRef hn_make_cgimage(const unsigned char *buf, int bw, int bh, size_t stride) {
    /* stride 此处要求 == w*4(hn_rt 保证); kCGImageAlphaPremultipliedFirst|kCGBitmapByteOrder32Little */
    static CGColorSpaceRef g_cs;
    if (!g_cs)
        g_cs = ((CGColorSpaceRef (*)(void))dlsym(RTLD_DEFAULT, "CGColorSpaceCreateDeviceRGB"))();
    CGDataProviderRef prov = ((CGDataProviderRef (*)(void *, const void *, size_t, void (*)(void *, const void *, size_t)))
        dlsym(RTLD_DEFAULT, "CGDataProviderCreateWithData"))(NULL, buf, (size_t)bw * bh * 4, NULL);
    if (!prov) return NULL;
    CGImageRef cg = ((CGImageRef (*)(size_t, size_t, size_t, size_t, size_t, CGColorSpaceRef,
                                     uint32_t, CGDataProviderRef, const double *, int, int))
        dlsym(RTLD_DEFAULT, "CGImageCreate"))(
        (size_t)bw, (size_t)bh, 8, 32, stride, g_cs, 2u | 8192u, prov, NULL, 0, 0);
    ((void (*)(CGDataProviderRef))dlsym(RTLD_DEFAULT, "CGDataProviderRelease"))(prov);
    return cg;
}

/* 回退开关(HN_BLIT_FALLBACK=1 强制旧 NSImage 路径; 诊断/对照用) */
static int blit_fallback(void) {
    static int f = -1;
    if (f < 0) {
        const char *e = getenv("HN_BLIT_FALLBACK");
        f = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return f;
}

void hnp_window_blit(hnp_window *w, const unsigned char *bgra_pre, int bw, int bh, size_t stride) {
    if (!w || !w->win || !bgra_pre || bw <= 0 || bh <= 0) return;
    /* 事故②修复挂点: blit 全程套有界池(setNeedsDisplay:/display 触发的
       AppKit/CA 内部 autorelease 对象由此回收), 见 pool_push 注释。 */
    obj_t pool = pool_push();
    /* HiDPI 口径: 位图物理尺寸照实(bw×bh 可为逻辑的 2 倍), 视图按**逻辑点**
       呈现(drawRect 的绘制矩形取内容区 CSS px) —— Retina 上 2x 位图
       1:1 物理像素落进 2x 背衬, 不经系统放大。拿不到逻辑尺寸时回退位图尺寸。 */
    int lw = w->w > 0 ? w->w : bw;
    int lh = w->h > 0 ? w->h : bh;

    if (blit_fallback()) {
        /* ---- 旧路径(NSImage; 保留为回归对照臂: HN_BLIT_FALLBACK=1 启用) ----
           直接把引擎缓冲交给 CGImage(引用不拷), 走 AppKit 图像机制 ——
           与改动前逐字节同码, 供"新老两路径产物一致"的对照实验。 */
        CGImageRef cg = hn_make_cgimage(bgra_pre, bw, bh, stride);
        if (cg) {
            obj_t nsimg = MSG0(obj_t, o_getClass("NSImage"), "alloc");
            CGSize sz = { (double)lw, (double)lh };
            nsimg = ((obj_t (*)(obj_t, sel_t, CGImageRef, CGSize))o_msgSend)(
                nsimg, o_sel("initWithCGImage:size:"), cg, sz);
            MSG1(void, w->iv, "setImage:", nsimg);
            if (w->img) MSG0(void, w->img, "release");
            w->img = nsimg;
            ((void (*)(CGImageRef))dlsym(RTLD_DEFAULT, "CGImageRelease"))(cg);
        }
        MSG0(void, w->win, "display");
        pool_pop(pool);
        return;
    }

    /* ---- 新路径(直呈): 自持帧缓冲 → CGImage(视图持引用) + setNeedsDisplay: ----
       兼容面对账: contentView 是 NSImageView 子类, pet_probe/hidpi_probe 读
       `contentView.image`(非空断言)与 CGImageForProposedRect(像素尺寸)——
       首次/尺寸变化时 setImage: **一次**(NSImage 保留同一 CGImage, CGImage
       的数据提供者指向帧缓冲; 此后每帧 memcpy 即"内容更新", NSImage 无需重建)。 */
    size_t need = (size_t)bw * bh * 4;
    int rebuild = (w->frame == NULL || w->cg_bw != bw || w->cg_bh != bh);
    if (w->fcap < need) {
        unsigned char *nb = (unsigned char *)realloc(w->frame, need);
        if (!nb) { pool_pop(pool); return; }   /* 分配失败: 保留上一帧, 不崩 */
        w->frame = nb;
        w->fcap = need;
        rebuild = 1;
    }
    if (stride == (size_t)bw * 4)
        memcpy(w->frame, bgra_pre, need);
    else
        for (int y = 0; y < bh; y++)
            memcpy(w->frame + (size_t)y * bw * 4, bgra_pre + (size_t)y * stride, (size_t)bw * 4);
    if (rebuild) {
        CGImageRef cg = hn_make_cgimage(w->frame, bw, bh, (size_t)bw * 4);
        if (!cg) { pool_pop(pool); return; }
        if (w->cg) ((void (*)(CGImageRef))dlsym(RTLD_DEFAULT, "CGImageRelease"))(w->cg);
        w->cg = cg;
        w->cg_bw = bw; w->cg_bh = bh;
        w->cg_lw = lw; w->cg_lh = lh;
        /* 兼容面: NSImage(探针读回/AppKit 兜底; CGImage 被它保留) */
        obj_t nsimg = MSG0(obj_t, o_getClass("NSImage"), "alloc");
        CGSize sz = { (double)lw, (double)lh };
        nsimg = ((obj_t (*)(obj_t, sel_t, CGImageRef, CGSize))o_msgSend)(
            nsimg, o_sel("initWithCGImage:size:"), cg, sz);
        if (nsimg) {
            MSG1(void, w->iv, "setImage:", nsimg);
            if (w->img) MSG0(void, w->img, "release");
            w->img = nsimg;
        }
    }
    /* setNeedsDisplay: 标脏; display 立即同步重绘(本壳自管 runloop,
       事件循环不保证跑到 CA 提交时机 —— 与旧 setImage:+display 同拍,
       EXPOSE/缩放的即时性不依赖 runloop 行为) */
    MSG1(void, w->iv, "setNeedsDisplay:", (obj_t)1);
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

/* 本机键码 → 语义键(macOS 虚拟键码逐值抄自 SDK 的 Events.h:
   kVK_Escape=53 kVK_Return=36 kVK_ANSI_KeypadEnter=76 kVK_Tab=48
   kVK_Delete=51(实为 Backspace!) kVK_ForwardDelete=117 kVK_LeftArrow=123
   kVK_RightArrow=124 kVK_DownArrow=125 kVK_UpArrow=126 kVK_Home=115
   kVK_End=119 kVK_PageUp=116 kVK_PageDown=121 kVK_Help=114(Insert)
   F 区 122/120/99/118/96/97/98/100/101/109/103/111。
   注: mac 的 kVK_Delete 是**向后删除**(Backspace), 前进删除是
   kVK_ForwardDelete —— 与 PC 键盘的 Delete 键语义对齐到 HNP_KEY_DELETE。 */
static hnp_key mac_keycode_to_hnp(unsigned short kc) {
    switch (kc) {
    case 53: return HNP_KEY_ESCAPE;
    case 36: return HNP_KEY_RETURN;
    case 76: return HNP_KEY_ENTER_NUMPAD;
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
    case 114: return HNP_KEY_INSERT;      /* Help/Insert 键 */
    case 71: return HNP_KEY_SCROLL_LOCK;  /* Clear 键(近似) */
    case 57: return HNP_KEY_CAPS_LOCK;
    case 122: return HNP_KEY_F1;  case 120: return HNP_KEY_F2;  case 99: return HNP_KEY_F3;
    case 118: return HNP_KEY_F4;  case 96: return HNP_KEY_F5;   case 97: return HNP_KEY_F6;
    case 98: return HNP_KEY_F7;   case 100: return HNP_KEY_F8;  case 101: return HNP_KEY_F9;
    case 109: return HNP_KEY_F10; case 103: return HNP_KEY_F11; case 111: return HNP_KEY_F12;
    case 49: return HNP_KEY_SPACE;
    default: return HNP_KEY_PRINTABLE;
    }
}

/* 契约导出(hn_platform.h): 探针的键码映射断言走这里 —— 与事件路径同一
   张表(单源), 断言的就是运行期行为。 */
hnp_key hnp_key_from_native(unsigned long native_code) {
    return mac_keycode_to_hnp((unsigned short)native_code);
}

/* ================= 深度 2: IME 路由 =================

   键事件的三个去向(按优先级):
     ① 组字中(marked_len > 0): 一切 keyDown 交输入上下文
        (interpretKeyEvents) —— IME 内部消费(改候选/退格/翻页), 回调
        setMarkedText/insertText 落成 PREEDIT/COMMIT 事件; 引擎本拍
        收不到任何键(避免与组字串双份)。Esc/Return 在组字中由 IME
        自己收敛(取消/提交) —— 不触发窗口关闭/hx 提交。
     ② 非组字 + 无 cmd/ctrl + 可打印键(含空格): 同样交输入上下文 ——
        这是"中文能打出来"的入口(拼音/n 键起串); insertText 回调
        产 COMMIT 事件作为该键的文本结果(US 布局下 'a' → COMMIT "a",
        与旧 utf8 直送引擎等效; shift 大小写/option 特殊字符随 IME 正确)。
     ③ 其余(方向/Home/End/PageUp/Down/退格/删除/Tab/Return/F 区/cmd 组合):
        不进 IME, 直接作 HNP_EV_KEY_* 给引擎 —— 语义键与快捷键的既有
        行为零漂移。 */

/* 把 NSEvent 交给输入上下文; 返回 1 = 已路由(调用方不再 sendEvent:/
   不再发引擎键事件)。 */
static int ime_route_event(hnp_window *w, obj_t e) {
    if (!w || !w->iv || !w->win) return 0;
    obj_t ctx = MSG0(obj_t, w->iv, "inputContext");
    if (!ctx) return 0;   /* 无输入上下文(类注册失败): 走引擎路径 */
    obj_t arr = MSG0(obj_t, o_getClass("NSArray"), "array");
    if (!arr) return 0;
    arr = ((obj_t (*)(obj_t, sel_t, obj_t))o_msgSend)(arr, o_sel("arrayByAddingObject:"), e);
    /* interpretKeyEvents:(NSResponder) → 内部按当前输入源解析:
       组字/查表/键绑定; 回调进 insertText:/setMarkedText: */
    ((void (*)(obj_t, sel_t, obj_t))o_msgSend)(w->iv, o_sel("interpretKeyEvents:"), arr);
    return 1;
}

/* IME trace(HN_IME_TRACE=1): 组字/提交链路逐行落日志(真窗手动验收的
   观测点; 与 HN_MEDIA_TRACE 同款环境钮纪律, 记忆化)。 */
static int ime_trace(void) {
    static int t = -1;
    if (t < 0) {
        const char *e = getenv("HN_IME_TRACE");
        t = (e && *e && strcmp(e, "0") != 0) ? 1 : 0;
    }
    return t;
}

int hnp_pump(hnp_event *ev, int timeout_ms) {
    if (!ev) return 0;
    ev->kind = HNP_EV_NONE;
    ev->utf8 = NULL;
    ev->scale = 1.0f;
    ev->cursor = 0;
    /* 事故②同款修复: 每次泵的 NSDate/NSEvent 及 AppKit 事件分派内部
       autorelease 对象, 落进本次有界池统一回收(无池时逐事件泄漏)。 */
    obj_t pool = pool_push();

    /* ---- 深度 2/3 的挂起事件优先出队(不占 AppKit 事件泵) ----
       顺序: IME 事件(组字/上屏) → backing scale → 屏幕参数。
       (挂起事件在下次 pump 前对调用方有效; utf8 指向 g_ime_q 的内部缓冲。) */
    for (int i = 0; i < HN_IME_Q_CAP; i++) {
        if (!g_ime_q[i].used) continue;
        hnp_window *hw = g_ime_q[i].win;
        g_ime_q[i].used = 0;
        if (!hw) continue;
        ev->kind = g_ime_q[i].commit ? HNP_EV_IME_COMMIT : HNP_EV_IME_PREEDIT;
        ev->utf8 = g_ime_q[i].utf8;
        ev->cursor = g_ime_q[i].cursor;
        if (ime_trace())
            fprintf(stderr, "[ime] %s utf8=\"%s\" cursor=%d\n",
                    g_ime_q[i].commit ? "commit" : "preedit",
                    g_ime_q[i].utf8, g_ime_q[i].cursor);
        pool_pop(pool);
        return 1;
    }
    if (g_win_map_n > 0) {
        hnp_window *hw = g_win_map[0].hw;
        if (hw->scale_dirty) {
            /* AppKit 只在 backing 属性**真变化**时调 viewDidChangeBackingProperties
               (跨屏拖动/系统缩放切换/屏色彩空间变化), 所以这里不额外去重 ——
               事件语义 = "平台报告 backing 变了", 值随事件带上; 调用方
               (hn_app)的 hn_rt_set_scale 对新值幂等(同值早退), 无重复成本。
               去重反而会漏: 若视图报变化而我们缓存的旧值恰好相同(例如上一次
               事件被超时丢弃), 静默丢弃会让重渲染停在旧 scale 上。 */
            hw->scale_dirty = 0;
            float ns = hnp_window_scale(hw);
            hw->scale = ns;
            ev->kind = HNP_EV_SCALE_CHANGED;
            ev->scale = ns;
            if (getenv("HN_SCALE_TRACE") || ime_trace())
                fprintf(stderr, "[scale] backing=%.1f\n", (double)ns);
            pool_pop(pool);
            return 1;
        }
        if (hw->screen_dirty) {
            hw->screen_dirty = 0;
            ev->kind = HNP_EV_SCREEN_CHANGED;
            pool_pop(pool);
            return 1;
        }
        if (hw->focus_dirty) {
            hw->focus_dirty = 0;
            ev->kind = hw->focus_now ? HNP_EV_FOCUS_IN : HNP_EV_FOCUS_OUT;
            pool_pop(pool);
            return 1;
        }
    }

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
    case EV_LDOWN: case EV_RDOWN: {
        /* 逐像素穿透的引擎出口收口(机制③, 见"逐像素命中"段注释):
           引擎用自己的 x/y 做命中测试, 不经过 AppKit —— 透明窗口里
           **全透明像素**上的按下在此丢弃(半透明照常)。WindowServer 的
           逐像素路由(机制①)已把多数透明处点击挡在进程外, 这里兜住
           键窗口/边界带等仍会投递进来的场景, 使"透明处点击不落引擎"
           成为不依赖系统行为的确定性锚。不过 sendEvent:(AppKit 侧同样
           不处理 —— hitTest 已返回 nil, 转发也只是空转)。非透明窗/
           尚无首帧时 alpha 恒 255, 过滤永不触发(旧行为逐位一致)。 */
        hnp_window *hw0 = hn_hw_from_event(e);
        if (hw0 && hn_alpha_at(hw0, x, y) == 0) {
            pool_pop(pool);
            return 0;
        }
        ev->kind = HNP_EV_BUTTON_DOWN;
        ev->x = x; ev->y = y; ev->button = (ty == EV_LDOWN) ? 0 : 2;
        ev->mods = mods;
        break;
    }
    case EV_MOVE: case EV_LDRAG:
        ev->kind = HNP_EV_POINTER_MOVE; ev->x = x; ev->y = y; ev->mods = mods; break;
    case EV_LUP:
        /* 左键抬起: 引擎与 AppKit 双路都要见(与按下 case 同口径) ——
           窗口拖动结束、AppKit 控件的 mouse-up 状态机都依赖它;
           合并 LDOWN/RDOWN case 时误删过本分支(2026-10-14 复审恢复)。 */
        ev->kind = HNP_EV_BUTTON_UP;
        ev->x = x; ev->y = y; ev->button = 0; ev->mods = mods;
        break;
    case EV_SCROLL: {
        ev->kind = HNP_EV_SCROLL;
        ev->x = x; ev->y = y;
        ev->dx = (float)MSG0(double, e, "deltaX");
        ev->dy = (float)MSG0(double, e, "deltaY");
        ev->mods = mods;
        break;
    }
    case EV_KEYDOWN: case EV_KEYUP: {
        unsigned short kc = MSG0(unsigned short, e, "keyCode");
        hnp_key k = mac_keycode_to_hnp(kc);
        /* ---- IME 路由(见上方"IME 路由"注释块) ---- */
        hnp_window *hw = hn_hw_from_event(e);
        if (ty == EV_KEYDOWN && hw && hw->ime_active) {
            int cmd_ctrl = (mods & (2 | 8)) != 0;   /* ctrl 或 cmd */
            int composing = hw->marked_len > 0;
            int printable = (k == HNP_KEY_PRINTABLE || k == HNP_KEY_SPACE);
            /* 组字中: 全交输入上下文(Esc 取消组字/Return 提交都由 IME 收口,
               不触发窗口关闭与 hx)。非组字: 无修饰可打印键交输入上下文
               (中文起串/大小写/option 字符), 其余语义键直达引擎。 */
            if (composing || (!cmd_ctrl && printable)) {
                ime_route_event(hw, e);
                /* 组字中键已被 IME 消化(引擎这拍不见它); 非组字可打印键的
                   文本结果将经 insertText → HNP_EV_IME_COMMIT 上行。
                   无论回调是否产出事件, 本键都不再走引擎键路径(避免双份)。 */
                MSG1(void, app, "sendEvent:", e);
                pool_pop(pool);
                return 1;
            }
        }
        ev->kind = (ty == EV_KEYDOWN) ? HNP_EV_KEY_DOWN : HNP_EV_KEY_UP;
        ev->key = k;
        ev->mods = mods;
        /* 可打印字符: charactersIgnoringModifiers(旧路径; 有 IME 时
           上方的输入上下文路线已消费, 这里只在 IME 未激活/非可打印时兜底) */
        if (k == HNP_KEY_PRINTABLE) {
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
        } else if (k == HNP_KEY_SPACE) {
            g_utf8[0] = ' '; g_utf8[1] = 0;
            ev->utf8 = g_utf8;
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

/* ================= 深度 2: IME 焦点 + 剪贴板桥 =================

   hnp_ime_focus: hn_app 在焦点/布局变化时喂当前可编辑盒(引擎口径)。
   - fw/fh > 0 → 输入上下文激活(interpretKeyEvents 路线才生效)并记住
     盒子(firstRectForCharacterRange 把候选窗摆到这里);
   - fw/fh <= 0 → 停用(所有键回引擎直路, 无 IME)。
   实现: 经 NSTextInputContext activate/deactivate(macOS 10.6+);
   activate 之后该窗口的键入才进输入法(中文候选窗/拼音串)。
   hn_app 侧调用点: 点击命中 input/textarea 后、布局重排后、失去焦点时。 */
void hnp_ime_focus(hnp_window *w, int x, int y, int iw, int ih) {
    if (!w) return;
    w->fx = x; w->fy = y; w->fw = iw; w->fh = ih;
    int want = (iw > 0 && ih > 0);
    if (want == w->ime_active) return;
    w->ime_active = want;
    if (!w->iv) return;
    obj_t ctx = MSG0(obj_t, w->iv, "inputContext");
    if (!ctx) return;
    if (want) MSG0(void, ctx, "activate");
    else MSG0(void, ctx, "deactivate");
    /* 焦点切换后系统缓存的字符坐标要失效(invalidateCharacterCoordinates,
       10.6+; 缺了它候选窗会停在上一控件的屏幕位置) */
    MSG0(void, ctx, "invalidateCharacterCoordinates");
}

/* 剪贴板: NSPasteboard generalPasteboard(与 hn_rt.c 的 sys://clipboard
   同源同对象 —— 那里是 hx 数据通道, 这里是 ⌘C/⌘V/⌘X 编辑通道;
   两处都走系统剪贴板, 天然一致)。 */
int hnp_clipboard_get(hnp_window *w, char *out, size_t cap) {
    (void)w;
    if (!out || cap == 0) return 0;
    out[0] = 0;
    obj_t pb = MSG0(obj_t, o_getClass("NSPasteboard"), "generalPasteboard");
    if (!pb) return 0;
    /* clearContents 不调 —— 只读不写, 不清剪贴板(读路径的副作用面
       为零; setString:forType: 才需要 clear) */
    obj_t type = nsstr("public.utf8-plain-text");
    obj_t str = ((obj_t (*)(obj_t, sel_t, obj_t))o_msgSend)(pb, o_sel("stringForType:"), type);
    if (!str) return 0;
    const char *c = ((const char *(*)(obj_t, sel_t))o_msgSend)(str, o_sel("UTF8String"));
    if (!c) return 0;
    snprintf(out, cap, "%s", c);
    return out[0] ? 1 : 0;
}

int hnp_clipboard_set(hnp_window *w, const char *utf8) {
    (void)w;
    if (!utf8) return 0;
    obj_t pb = MSG0(obj_t, o_getClass("NSPasteboard"), "generalPasteboard");
    if (!pb) return 0;
    MSG0(int, pb, "clearContents");
    obj_t str = nsstr(utf8);
    obj_t type = nsstr("public.utf8-plain-text");
    signed char ok = ((signed char (*)(obj_t, sel_t, obj_t, obj_t))o_msgSend)(
        pb, o_sel("setString:forType:"), str, type);
    return ok ? 1 : 0;
}

/* ================= 深度 3: 屏幕参数 + 可见区 ================= */

/* hnp_screen_workarea: 找包含点 (px,py) 的屏(引擎口径左上原点;
   AppKit 原点左下 → pt_y = 主屏高 − py, 全屏坐标), 命中屏取 visibleFrame
   (菜单栏/Dock 已排除), 翻回引擎口径; 点不在任何屏上(屏摘除后的记忆坐标)
   回退主屏可见区 —— 这正是"屏没了, 窗口摆回主屏"的语义。 */
int hnp_screen_workarea(hnp_window *w, int px, int py,
                        int *vx, int *vy, int *vw, int *vh) {
    (void)w;
    if (!vx || !vy || !vw || !vh) return 0;
    double ph = hn_primary_screen_height();
    if (ph <= 0) return 0;
    obj_t arr = MSG0(obj_t, o_getClass("NSScreen"), "screens");
    unsigned long n = arr ? MSG0(unsigned long, arr, "count") : 0;
    CGRect best = { { 0, 0 }, { 0, 0 } };
    CGRect primary_vis = { { 0, 0 }, { 0, 0 } };
    for (unsigned long i = 0; i < n; i++) {
        obj_t s = ((obj_t (*)(obj_t, sel_t, unsigned long))o_msgSend)(arr, o_sel("objectAtIndex:"), i);
        CGRect f = MSG0(CGRect, s, "frame");
        CGRect vf = MSG0(CGRect, s, "visibleFrame");
        if (i == 0) primary_vis = vf;
        double ax = (double)px, ay = ph - (double)py;   /* 点 → AppKit 全局坐标 */
        if (ax >= f.origin.x && ax < f.origin.x + f.size.w &&
            ay >= f.origin.y && ay < f.origin.y + f.size.h) {
            best = vf;
            break;
        }
    }
    if (best.size.w <= 0) best = primary_vis;
    if (best.size.w <= 0) {
        obj_t ms = MSG0(obj_t, o_getClass("NSScreen"), "mainScreen");
        if (!ms) return 0;
        best = MSG0(CGRect, ms, "visibleFrame");
    }
    if (best.size.w <= 0 || best.size.h <= 0) return 0;
    *vx = hn_round_d(best.origin.x);
    *vy = hn_round_d(ph - best.origin.y - best.size.h);
    *vw = hn_round_d(best.size.w);
    *vh = hn_round_d(best.size.h);
    return 1;
}

/* 屏幕参数变化(分辨率/排列/数量)与键窗口变化的通知注册(进程一次)。
   NSNotificationCenter 回调经 class_addMethod 挂在运行期监听类上 ——
   与视图覆写同一手法; 回调只置脏, hnp_pump 出事件。 */
static void hn_on_screen_change(obj_t self, sel_t cmd, obj_t note) {
    (void)cmd; (void)note;
    (void)self;
    for (int i = 0; i < g_win_map_n; i++)
        if (g_win_map[i].hw) g_win_map[i].hw->screen_dirty = 1;
}

/* NSWindowDidChangeBackingPropertiesNotification 的处理: 置 scale 脏
   (与视图覆写 viewDidChangeBackingProperties 同一出口; 双保险 ——
   多数场景视图覆写先到, 本通知在窗口层再报一次, 语义相同)。
   ⚠️ 修复(本轮实测): 此前这个选择器被指向 hn_on_screen_change(错置
   screen_dirty), 且在 objc_registerClassPair **之后**才 add_method ——
   注册后加方法静默失败(class_addMethod 对已注册类返回 NO), 于是
   NSNotificationCenter 分发到 responder 时找不到实现 → 消息转发 →
   崩溃/静默丢事件。现改为注册前一次性挂全。 */
static void hn_on_backing_change(obj_t self, sel_t cmd, obj_t note) {
    (void)cmd;
    (void)self;
    obj_t win = note ? MSG0(obj_t, note, "object") : NULL;
    long num = win ? ((long (*)(obj_t, sel_t))o_msgSend)(win, o_sel("windowNumber")) : -1;
    for (int i = 0; i < g_win_map_n; i++) {
        if (num >= 0 && g_win_map[i].wnum != num) continue;
        if (g_win_map[i].hw) g_win_map[i].hw->scale_dirty = 1;
    }
}
/* 键窗口变化(NSWindowDidBecomeKey/ResignKeyNotification): 置 FOCUS_IN/OUT
   事件位; 失焦时额外停用 IME 上下文(中文候选窗不该残留在非键窗口)。
   查表按 windowNumber(与事件路由同一口径; 通知里的 NSWindow 对象指针
   与事件里的一致, 但全库统一窗口号更稳)。 */
static void hn_on_key_window(obj_t self, sel_t cmd, obj_t note) {
    (void)cmd; (void)self;
    obj_t win = note ? MSG0(obj_t, note, "object") : NULL;
    if (!win) return;
    long num = ((long (*)(obj_t, sel_t))o_msgSend)(win, o_sel("windowNumber"));
    int key = (int)(((signed char (*)(obj_t, sel_t))o_msgSend)(win, o_sel("isKeyWindow")));
    for (int i = 0; i < g_win_map_n; i++) {
        if (!g_win_map[i].hw) continue;
        if (g_win_map[i].wnum != num && g_win_map[i].win != win) continue;
        g_win_map[i].hw->focus_dirty = 1;
        g_win_map[i].hw->focus_now = key;
        if (!key && g_win_map[i].hw->ime_active) {
            obj_t ctx = MSG0(obj_t, g_win_map[i].hw->iv, "inputContext");
            if (ctx) MSG0(void, ctx, "deactivate");
        }
    }
}

static void hn_setup_notifications(void) {
    static int done;
    if (done) return;
    done = 1;
    obj_t (*alloc_pair)(obj_t, const char *, size_t) =
        (obj_t (*)(obj_t, const char *, size_t))dlsym(RTLD_DEFAULT, "objc_allocateClassPair");
    signed char (*add_method)(obj_t, sel_t, void (*)(void), const char *) =
        (signed char (*)(obj_t, sel_t, void (*)(void), const char *))dlsym(RTLD_DEFAULT, "class_addMethod");
    void (*reg_pair)(obj_t) = (void (*)(obj_t))dlsym(RTLD_DEFAULT, "objc_registerClassPair");
    if (!alloc_pair || !add_method || !reg_pair) return;
    obj_t cls = alloc_pair(o_getClass("NSObject"), "HNPNoteObserver", 0);
    if (!cls) return;
    add_method(cls, o_sel("onScreenChange:"), (void (*)(void))hn_on_screen_change, "v@:@");
    add_method(cls, o_sel("onKeyWindow:"), (void (*)(void))hn_on_key_window, "v@:@");
    /* ⚠️ 必须在 objc_registerClassPair **之前**挂(注册后 add_method 返回 NO) */
    add_method(cls, o_sel("onBackingChange:"), (void (*)(void))hn_on_backing_change, "v@:@");
    reg_pair(cls);
    obj_t obs = MSG0(obj_t, cls, "alloc");
    obs = MSG0(obj_t, obs, "init");
    if (!obs) return;
    obj_t nc = MSG0(obj_t, o_getClass("NSNotificationCenter"), "defaultCenter");
    if (!nc) return;
    /* 屏幕参数变化(显示分辨率/排列/数量): AppKit 对这三类只有
       didChangeScreenParameters(没有 willChange)。 */
    obj_t name = nsstr("NSApplicationDidChangeScreenParametersNotification");
    ((void (*)(obj_t, sel_t, obj_t, sel_t, obj_t, obj_t))o_msgSend)(
        nc, o_sel("addObserver:selector:name:object:"), obs, o_sel("onScreenChange:"), name, NULL);
    /* 键窗口变化(FOCUS_IN/OUT) */
    obj_t kname = nsstr("NSWindowDidBecomeKeyNotification");
    obj_t kname2 = nsstr("NSWindowDidResignKeyNotification");
    ((void (*)(obj_t, sel_t, obj_t, sel_t, obj_t, obj_t))o_msgSend)(
        nc, o_sel("addObserver:selector:name:object:"), obs, o_sel("onKeyWindow:"), kname, NULL);
    ((void (*)(obj_t, sel_t, obj_t, sel_t, obj_t, obj_t))o_msgSend)(
        nc, o_sel("addObserver:selector:name:object:"), obs, o_sel("onKeyWindow:"), kname2, NULL);
    /* backingScaleFactor 变化(与视图覆写互为双保险; 视图覆写是主页) */
    obj_t bname = nsstr("NSWindowDidChangeBackingPropertiesNotification");
    ((void (*)(obj_t, sel_t, obj_t, sel_t, obj_t, obj_t))o_msgSend)(
        nc, o_sel("addObserver:selector:name:object:"), obs, o_sel("onBackingChange:"), bname, NULL);
}

/* hnp_init 之后回调一次(hnp_window_open 里调) */
static void hn_ensure_notifications(void) {
    hn_setup_notifications();
}

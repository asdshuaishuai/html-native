/* hnweb_wk_macos.c — macOS WebView 画板壳(WKWebView; 纯 C99, dlopen + objc_msgSend)
 *
 * 定位(2026-11-20 转型): WebView 是**唯一画板** —— 纯 WebView 转型后,
 * 桌面小程序运行时与 GenUI 画布运行时都由本壳承载:
 *   - 真 WebKit 渲染(真 CSS/JS: 阴影模糊/backdrop-filter/@font-face/SVG 全部可用)
 *   - 相对资产按**页面所在目录**解析(loadFileURL:allowingReadAccessToURL:),
 *     不复刻 hncore 的 CWD 解析 bug
 *   - 透明/无边框窗(透明卡片与 GenUI 画布语义); 普通窗带标题栏
 *   - JS→原生桥: 注入 window.hnBridge.post(msg) → 原生计数/打印(daemon 接线前置)
 *   - --probe 无窗口自检: 装载完成 + 桥消息 + 透明标志, 全部可脚本断言
 *   - --shot <png>: 装载稳定后 WKSnapshot 快照落盘(daemon 的窗口 shot 后端)
 *   - 交互模式 300ms 文件指纹 watch: daemon update 原子覆写后 reloadFromOrigin
 *   - HN_WEBVIEW_HIDDEN=1: 不开窗不抢焦点(daemon/验收的隐藏模式)
 *
 * 纪律: 零 ObjC 编译单元 —— AppKit/WebKit 经 dlopen 装载, 所有调用走
 * objc_msgSend(消息发送即普通 C 函数调用, ABI 稳定; 与 hnweb_macos.c 同宗)。
 * 唯一的 clang 扩展是 -fblocks(WKSnapshot 的 completionHandler 需要)。
 *
 * 用法:
 *   hnweb_wk_macos <file.html> [W H] [--transparent] [--frameless] [--title T]  开窗口
 *   hnweb_wk_macos <file.html> [W H] [--transparent] --probe                   自检
 *   hnweb_wk_macos <file.html> [W H] [--transparent] --shot out.png            快照
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdint.h>
#include <time.h>
#include <limits.h>
#include <sys/stat.h>

/* ---------------- ObjC 运行时的最小转发层(与 hnweb_macos.c 同口径) ---------------- */

typedef void *obj_t;
typedef void *sel_t;

static obj_t (*o_getClass)(const char *);
static sel_t (*o_sel)(const char *);
static obj_t (*o_msgSend)(obj_t, sel_t, ...);

#define MSG0(RET, O, NAME) ((RET (*)(obj_t, sel_t))o_msgSend)((O), o_sel(NAME))
#define MSG1(RET, O, NAME, A1) \
    ((RET (*)(obj_t, sel_t, obj_t))o_msgSend)((O), o_sel(NAME), (obj_t)(A1))
#define MSG2(RET, O, NAME, A1, A2) \
    ((RET (*)(obj_t, sel_t, obj_t, obj_t))o_msgSend)((O), o_sel(NAME), (obj_t)(A1), (obj_t)(A2))
#define MSGL(RET, O, NAME, L) \
    ((RET (*)(obj_t, sel_t, long))o_msgSend)((O), o_sel(NAME), (L))

typedef struct { double x, y; } CGPoint;
typedef struct { double w, h; } CGSize;
typedef struct { CGPoint origin; CGSize size; } CGRect;

static obj_t nsstr(const char *s) {
    return ((obj_t (*)(obj_t, sel_t, const char *))o_msgSend)(
        o_getClass("NSString"), o_sel("stringWithUTF8String:"), s);
}
static obj_t nsnum_bool(int b) {
    return ((obj_t (*)(obj_t, sel_t, long))o_msgSend)(
        o_getClass("NSNumber"), o_sel("numberWithBool:"), b ? 1 : 0);
}
static obj_t nsapp(void) { return MSG0(obj_t, o_getClass("NSApplication"), "sharedApplication"); }

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

static int objc_load(void) {
    if (!dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_NOW)) return 0;
    if (!dlopen("/System/Library/Frameworks/WebKit.framework/WebKit", RTLD_NOW)) {
        fprintf(stderr, "hnweb_wk_macos: 无法装载 WebKit\n");
        return 0;
    }
    o_getClass = (obj_t (*)(const char *))dlsym(RTLD_DEFAULT, "objc_getClass");
    o_sel = (sel_t (*)(const char *))dlsym(RTLD_DEFAULT, "sel_registerName");
    o_msgSend = (obj_t (*)(obj_t, sel_t, ...))dlsym(RTLD_DEFAULT, "objc_msgSend");
    return o_getClass && o_sel && o_msgSend;
}

/* ---------------- 运行时类创建(消息处理器; 正规 objc 运行时 API) ---------------- */

typedef obj_t (*fn_class_alloc)(obj_t superclass, const char *name, size_t extra);
typedef int (*fn_class_addMethod)(obj_t cls, sel_t sel, void *imp, const char *types);
typedef void (*fn_class_register)(obj_t cls);

static fn_class_alloc class_alloc;
static fn_class_addMethod class_addMethod;
static fn_class_register class_register;

static int classapi_load(void) {
    class_alloc = (fn_class_alloc)dlsym(RTLD_DEFAULT, "objc_allocateClassPair");
    class_addMethod = (fn_class_addMethod)dlsym(RTLD_DEFAULT, "class_addMethod");
    class_register = (fn_class_register)dlsym(RTLD_DEFAULT, "objc_registerClassPair");
    return class_alloc && class_addMethod && class_register;
}

/* ---- JS→原生桥 ----
 * window.hnBridge.post(msg) → window.webkit.messageHandlers.hn.postMessage(msg)
 * → 本 IMP。body 可能是 NSString/NSDictionary/NSNumber: 一律 description 后打印,
 * 计数供 --probe 断言。P1 接 daemon 时这里换成消息分发。 */
static int g_bridge_msgs = 0;
static int g_quit = 0;

static void bridge_imp(obj_t self, sel_t _cmd, obj_t controller, obj_t message) {
    (void)self; (void)_cmd; (void)controller;
    obj_t name = MSG0(obj_t, message, "name");
    obj_t body = MSG0(obj_t, message, "body");
    const char *ns = name ? MSG0(const char *, name, "UTF8String") : "?";
    const char *bs = "?";
    if (body) {
        obj_t desc = MSG0(obj_t, body, "description");
        if (desc) bs = MSG0(const char *, desc, "UTF8String");
    }
    g_bridge_msgs++;
    printf("[bridge] %s %s\n", ns ? ns : "?", bs ? bs : "?");
    fflush(stdout);
}

/* ---------------- 事件类型(NSEvent.h:25-43; 过滤口径同 hnweb_macos.c) ---------------- */

#define EV_APPKITDEF 13
#define EV_PERIODIC 16
#define EV_KEYDOWN 10
#define EV_KEYCODE_ESC 53

static obj_t pump_event(double timeout_s) {
    obj_t app = nsapp();
    obj_t mode = nsstr("kCFRunLoopDefaultMode");
    obj_t NSDate = o_getClass("NSDate");
    obj_t lim = ((obj_t (*)(obj_t, sel_t, double))o_msgSend)(
        NSDate, o_sel("dateWithTimeIntervalSinceNow:"), timeout_s);
    return ((obj_t (*)(obj_t, sel_t, unsigned long, obj_t, obj_t, int))o_msgSend)(
        app, o_sel("nextEventMatchingMask:untilDate:inMode:dequeue:"), ~0UL, lim, mode, 1);
}

/* ---------------- 装载稳定判据(probe/shot/交互共用) ----------------
 * isLoading 连续 10 拍为假(≈160ms 稳定窗)才算装载完成。快照不等稳定
 * 会拍到纯白图(WKWebView 首帧未上屏), 这是探针实证过的竞态。 */
static int wait_loaded(obj_t app, obj_t wv, double timeout_s, double *elapsed_ms) {
    double t0 = now_s();
    int settle = 0;
    while (now_s() - t0 < timeout_s) {
        obj_t ev = pump_event(0.016);
        if (ev) {
            unsigned long ty = MSG0(unsigned long, ev, "type");
            if (ty < EV_APPKITDEF || ty > EV_PERIODIC)
                MSG1(void, app, "sendEvent:", ev);
        }
        if (!MSG0(char, wv, "isLoading")) {
            if (++settle >= 10) break;
        } else settle = 0;
    }
    if (elapsed_ms) *elapsed_ms = (now_s() - t0) * 1000.0;
    return settle >= 10;
}

/* ---------------- 文件指纹(mtime 纳秒 + 尺寸; 同 app/hn_app.c 口径) ---------------- */

#define WK_WATCH_INTERVAL_S 0.300

typedef struct { long long mtime_ns; long long size; } file_fp;

static int file_fp_of(const char *path, file_fp *fp) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    /* 本壳仅 macOS 构建; st_mtimespec 是 Darwin 口径 */
    fp->mtime_ns = (long long)st.st_mtimespec.tv_sec * 1000000000LL
                 + (long long)st.st_mtimespec.tv_nsec;
    fp->size = (long long)st.st_size;
    return 1;
}

/* ---------------- WKSnapshot 快照(daemon 窗口 shot 后端) ----------------
 * completionHandler 是 block, 因此本文件按 -fblocks 编译(仍是 C99, 无
 * ObjC 编译单元)。回调里只置标志并写 PNG, 主线程在事件泵里等完成 ——
 * 不引入 NSApplication run, 与 probe 的事件泵同款。 */

static int g_snap_done = 0;
static int g_snap_ok = 0;
static const char *g_snap_path = NULL;

static int snap_write_png(obj_t image) {
    if (!image || !g_snap_path) return 0;
    obj_t tiff = MSG0(obj_t, image, "TIFFRepresentation");
    if (!tiff) return 0;
    obj_t rep = MSG1(obj_t, o_getClass("NSBitmapImageRep"), "imageRepWithData:", tiff);
    if (!rep) return 0;
    obj_t props = MSG0(obj_t, o_getClass("NSDictionary"), "dictionary");
    /* NSBitmapImageFileTypePNG = 4(NSBitmapImageRep.h) */
    obj_t png = ((obj_t (*)(obj_t, sel_t, long, obj_t))o_msgSend)(
        rep, o_sel("representationUsingType:properties:"), 4L, props);
    if (!png) return 0;
    return ((char (*)(obj_t, sel_t, obj_t, int))o_msgSend)(
        png, o_sel("writeToFile:atomically:"), nsstr(g_snap_path), 1) ? 1 : 0;
}

static int wk_snapshot(obj_t app, obj_t wv, const char *out) {
    g_snap_path = out;
    g_snap_done = 0;
    g_snap_ok = 0;
    obj_t cfg = MSG0(obj_t, o_getClass("WKSnapshotConfiguration"), "new");
    void (^cb)(obj_t, obj_t) = ^(obj_t image, obj_t err) {
        (void)err;
        g_snap_ok = snap_write_png(image);
        g_snap_done = 1;
    };
    MSG2(void, wv, "takeSnapshotWithConfiguration:completionHandler:", cfg, (obj_t)cb);
    double t0 = now_s();
    while (!g_snap_done && now_s() - t0 < 5.0) {
        obj_t ev = pump_event(0.010);
        if (ev) {
            unsigned long ty = MSG0(unsigned long, ev, "type");
            if (ty < EV_APPKITDEF || ty > EV_PERIODIC)
                MSG1(void, app, "sendEvent:", ev);
        }
    }
    return g_snap_done && g_snap_ok;
}

/* ---------------- 主流程 ---------------- */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr,
                "用法: hnweb_wk_macos <file.html> [W H] [--transparent] [--frameless] [--title T]  开窗口\n"
                "      hnweb_wk_macos <file.html> [W H] [--transparent] --probe                   自检\n"
                "      hnweb_wk_macos <file.html> [W H] [--transparent] --shot out.png            快照\n");
        return 2;
    }
    const char *path = argv[1];
    const char *title = NULL;
    const char *shot = NULL;
    int W = 960, H = 720, transparent = 0, probe = 0, frameless = 0;
    int nums[2], nn = 0;
    /* 单趟解析: 带值的选项先吞掉自己的值, 数字扫描不会误吞标题/路径 */
    for (int i = 2; i < argc; i++) {
        if (!strcmp(argv[i], "--transparent")) transparent = 1;
        else if (!strcmp(argv[i], "--probe")) probe = 1;
        else if (!strcmp(argv[i], "--frameless")) frameless = 1;
        else if (!strcmp(argv[i], "--title")) {
            if (i + 1 >= argc) { fprintf(stderr, "hnweb_wk_macos: --title 需要字符串\n"); return 2; }
            title = argv[++i];
        } else if (!strcmp(argv[i], "--shot")) {
            if (i + 1 >= argc) { fprintf(stderr, "hnweb_wk_macos: --shot 需要输出路径\n"); return 2; }
            shot = argv[++i];
        } else if (argv[i][0] >= '0' && argv[i][0] <= '9' && nn < 2) nums[nn++] = atoi(argv[i]);
    }
    if (nn >= 1 && nums[0] > 0) W = nums[0];
    if (nn >= 2 && nums[1] > 0) H = nums[1];
    if (shot && !shot[0]) { fprintf(stderr, "hnweb_wk_macos: --shot 需要输出路径\n"); return 2; }
    int hidden = 0;
    {
        const char *hv = getenv("HN_WEBVIEW_HIDDEN");
        if (hv && hv[0] && strcmp(hv, "0")) hidden = 1;
    }

    char absfile[PATH_MAX], absdir[PATH_MAX];
    if (!realpath(path, absfile)) {
        fprintf(stderr, "hnweb_wk_macos: 无法解析路径 %s\n", path);
        return 1;
    }
    snprintf(absdir, sizeof(absdir), "%s", absfile);
    char *slash = strrchr(absdir, '/');
    if (slash) *slash = 0;
    else snprintf(absdir, sizeof(absdir), ".");

    if (!objc_load()) return 1;
    if (!classapi_load()) {
        fprintf(stderr, "hnweb_wk_macos: objc 类 API 不可用\n");
        return 1;
    }

    obj_t app = nsapp();
    MSG0(void, app, "finishLaunching");
    MSGL(void, app, "setActivationPolicy:", 1);   /* accessory: 不占 Dock */

    /* ---- WKWebView 装配 ---- */
    obj_t config = MSG0(obj_t, o_getClass("WKWebViewConfiguration"), "new");
    obj_t ucc = MSG0(obj_t, o_getClass("WKUserContentController"), "new");
    MSG1(void, config, "setUserContentController:", ucc);

    /* 桥注入(document start; 页面脚本可立即用 window.hnBridge) */
    static const char *BRIDGE_JS =
        "(function(){if(!window.hnBridge){window.hnBridge={post:function(m){"
        "try{window.webkit.messageHandlers.hn.postMessage(m);}catch(e){}}};}})();";
    obj_t script = MSG0(obj_t, o_getClass("WKUserScript"), "alloc");
    script = ((obj_t (*)(obj_t, sel_t, obj_t, long, long))o_msgSend)(
        script, o_sel("initWithSource:injectionTime:forMainFrameOnly:"),
        nsstr(BRIDGE_JS), 0L /* AtDocumentStart */, 1L);
    MSG1(void, ucc, "addUserScript:", script);

    /* 消息处理器类: 实现 WKScriptMessageHandler 的唯一方法 */
    obj_t hcls = class_alloc(o_getClass("NSObject"), "HnBridgeHandler", 0);
    if (!hcls) { fprintf(stderr, "hnweb_wk_macos: 类注册失败\n"); return 1; }
    class_addMethod(hcls, o_sel("userContentController:didReceiveScriptMessage:"),
                    (void *)bridge_imp, "v@:@@");
    class_register(hcls);
    obj_t handler = MSG0(obj_t, hcls, "alloc");
    handler = MSG0(obj_t, handler, "init");
    MSG2(void, ucc, "addScriptMessageHandler:name:", handler, nsstr("hn"));

    obj_t wv = MSG0(obj_t, o_getClass("WKWebView"), "alloc");
    CGRect cr = { { 0, 0 }, { (double)W, (double)H } };
    wv = ((obj_t (*)(obj_t, sel_t, CGRect, obj_t))o_msgSend)(
        wv, o_sel("initWithFrame:configuration:"), cr, config);
    if (!wv) { fprintf(stderr, "hnweb_wk_macos: WKWebView 创建失败\n"); return 1; }
    MSGL(void, wv, "setAutoresizingMask:", (2 | 16));   /* width|height sizable */

    /* 透明画板: drawsBackground(undocumented 但长期稳定) + underPageBackgroundColor */
    if (transparent) {
        MSG2(void, wv, "setValue:forKey:", nsnum_bool(0), nsstr("drawsBackground"));
        if (MSG1(int, wv, "respondsToSelector:", o_sel("setUnderPageBackgroundColor:")))
            MSG1(void, wv, "setUnderPageBackgroundColor:",
                 MSG0(obj_t, o_getClass("NSColor"), "clearColor"));
    }

    /* ---- 窗口 ---- */
    /* titled|closable|miniaturizable|resizable; frameless/transparent = 无边框
       (引擎 manifest 缺省 frameless=1, daemon 会显式传 --frameless) */
    unsigned long style = 1 | 2 | 4 | 8;
    if (transparent || frameless) style = 0;
    obj_t win = MSG0(obj_t, o_getClass("NSWindow"), "alloc");
    win = ((obj_t (*)(obj_t, sel_t, CGRect, unsigned long, unsigned long, int))o_msgSend)(
        win, o_sel("initWithContentRect:styleMask:backing:defer:"), cr, style, 2, 0);
    if (!win) { fprintf(stderr, "hnweb_wk_macos: NSWindow 创建失败\n"); return 1; }
    MSG1(void, win, "setTitle:", nsstr(title && title[0] ? title : "hn · WebView"));
    if (transparent) {
        MSGL(void, win, "setOpaque:", 0);
        MSG1(void, win, "setBackgroundColor:",
             MSG0(obj_t, o_getClass("NSColor"), "clearColor"));
    }
    MSG1(void, win, "setContentView:", wv);

    /* 居中(mainScreen 可见区; 同 hnweb_macos 口径) */
    obj_t screen = MSG0(obj_t, o_getClass("NSScreen"), "mainScreen");
    CGRect sf = MSG0(CGRect, screen, "visibleFrame");
    CGRect wf = MSG0(CGRect, win, "frame");
    CGRect pos = { { sf.origin.x + (sf.size.w - wf.size.w) / 2,
                     sf.origin.y + (sf.size.h - wf.size.h) / 2 },
                   { wf.size.w, wf.size.h } };
    ((void (*)(obj_t, sel_t, CGRect, int))o_msgSend)(win, o_sel("setFrame:display:"), pos, 1);
    printf("[wk] window %.0fx%.0f transparent=%d hidden=%d frameless=%d\n",
           pos.size.w, pos.size.h, transparent, hidden, frameless);
    fflush(stdout);

    /* ---- 装载本地页面(根 = 页面目录; 相对资产可用) ---- */
    obj_t url_cls = o_getClass("NSURL");
    obj_t furl = ((obj_t (*)(obj_t, sel_t, obj_t, int))o_msgSend)(
        url_cls, o_sel("fileURLWithPath:isDirectory:"), nsstr(absfile), 0);
    obj_t durl = ((obj_t (*)(obj_t, sel_t, obj_t, int))o_msgSend)(
        url_cls, o_sel("fileURLWithPath:isDirectory:"), nsstr(absdir), 1);
    MSG2(void, wv, "loadFileURL:allowingReadAccessToURL:", furl, durl);

    if (probe || shot) {
        double ms = 0;
        int loaded = wait_loaded(app, wv, 15.0, &ms);
        printf("[wk] %s %s in %.0fms, bridge=%d, transparent=%d\n",
               shot ? "shot" : "probe", loaded ? "loaded" : "TIMEOUT", ms,
               g_bridge_msgs, transparent);
        if (transparent)
            printf("[wk] window opaque=%d (want 0)\n", (int)MSG0(char, win, "isOpaque"));
        fflush(stdout);
        int rc = loaded ? 0 : 1;
        if (shot) {
            if (rc == 0) {
                double ts = now_s();
                int ok = wk_snapshot(app, wv, shot);
                printf("[wk] shot %s in %.0fms: %s\n", ok ? "wrote" : "FAILED",
                       (now_s() - ts) * 1000.0, shot);
                fflush(stdout);
                if (!ok) rc = 1;
            }
        }
        MSG0(void, win, "close");
        MSG0(void, win, "release");
        return rc;
    }

    /* ---- 交互模式: 显示 + 事件泵(过滤 AppKit 内部事件, 同 hnweb_macos) ---- */
    if (!hidden) {
        MSG1(void, win, "makeKeyAndOrderFront:", (obj_t)1);
        MSG1(void, app, "activateIgnoringOtherApps:", (obj_t)1);
    }
    file_fp fp = { 0, 0 };
    int have_fp = file_fp_of(absfile, &fp);
    double last_watch = now_s();
    while (!g_quit) {
        obj_t ev = pump_event(0.016);
        if (ev) {
            unsigned long ty = MSG0(unsigned long, ev, "type");
            if (ty == EV_KEYDOWN) {
                long kc = ((long (*)(obj_t, sel_t))o_msgSend)(ev, o_sel("keyCode"));
                if (kc == EV_KEYCODE_ESC) { g_quit = 1; continue; }
            }
            if (ty < EV_APPKITDEF || ty > EV_PERIODIC) MSG1(void, app, "sendEvent:", ev);
        }
        if (!hidden && !MSG0(char, win, "isVisible")) g_quit = 1;
        /* daemon update: 原子 rename 覆写源文件 → 指纹变化 → 整文档重载 */
        double tnow = now_s();
        if (tnow - last_watch >= WK_WATCH_INTERVAL_S) {
            last_watch = tnow;
            file_fp nf;
            if (file_fp_of(absfile, &nf) &&
                (!have_fp || nf.mtime_ns != fp.mtime_ns || nf.size != fp.size)) {
                fp = nf;
                have_fp = 1;
                MSG0(void, wv, "reloadFromOrigin");
                printf("[wk] reload size=%lld\n", nf.size);
                fflush(stdout);
            }
        }
    }
    MSG0(void, win, "close");
    MSG0(void, win, "release");
    printf("[wk] exit\n");
    fflush(stdout);
    return 0;
}

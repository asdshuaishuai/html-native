/* hncanvas_host_macos.c — 常驻画布宿主(P2a; 纯 C99, dlopen WebKit/AppKit, 零 ObjC 编译单元)
 *
 * 定位(2026-11-20 Pure-WebView 转型 P2a): WebView 常驻。
 *   - 一个长期运行的进程, 多画布(每画布 = NSWindow + WKWebView)
 *   - 控制通道: Unix socket, JSON-lines 请求/响应(一行一条)
 *   - 画布形态(我们的价值面): 透明/无边框/层级/点击穿透/隐藏
 *   - 内容=原生 HTML/CSS/JS; 宿主注入极简 window.neb.post(msg) 桥(中性名)
 *   - 快照: WKSnapshot 同步语义(装载稳定后拍), 常驻进程内直接出 PNG
 *   - 生命周期: daemon 拉起/保活/崩溃重启(daemon 侧接线为 P2b); 本机自带
 *     ping/list/create/update/show/hide/snapshot/destroy/quit
 *
 * 用法:
 *   hncanvas_host_macos --socket <unix_socket_path>
 *
 * 协议(每行一条 JSON; 响应一行 JSON):
 *   {"op":"ping"}                                        → {"ok":true,"pid":N,"canvases":[...]}
 *   {"op":"create","id":"a","url":"/abs/page.html",      → {"ok":true,"id":"a","w":..,"h":..}
 *    "w":400,"h":300,"hidden":true,
 *    "form":{"transparent":true,"frameless":true,"level":0,"click_through":false}}
 *   {"op":"create","id":"a","html":"<html…>","base":"/abs/dir","w":..,"h":..}
 *   {"op":"update","id":"a","url":"..."|"html":"..."}    → {"ok":true}
 *   {"op":"reload","id":"a"}                             → {"ok":true}
 *   {"op":"show"|"hide","id":"a"}                        → {"ok":true}
 *   {"op":"snapshot","id":"a","path":"/abs/x.png"}       → {"ok":true,"path":"..."}
 *   {"op":"destroy","id":"a"}                            → {"ok":true}
 *   {"op":"list"}                                        → {"ok":true,"canvases":[{id,w,h,visible,form}]}
 *   {"op":"quit"}                                        → {"ok":true} 然后退出
 *
 * 纪律: 只用系统 WKWebView; 冻结引擎一行不改(仅链接 hn_json.c 做协议解析)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdint.h>
#include <time.h>
#include <signal.h>
#include <errno.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <poll.h>

#include "hn_json.h"

/* ---------------- ObjC 运行时的最小转发层(与 hnweb_wk_macos.c 同宗) ---------------- */

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
    if (!dlopen("/System/Library/Frameworks/WebKit.framework/WebKit", RTLD_NOW)) return 0;
    o_getClass = (obj_t (*)(const char *))dlsym(RTLD_DEFAULT, "objc_getClass");
    o_sel = (sel_t (*)(const char *))dlsym(RTLD_DEFAULT, "sel_registerName");
    o_msgSend = (obj_t (*)(obj_t, sel_t, ...))dlsym(RTLD_DEFAULT, "objc_msgSend");
    return o_getClass && o_sel && o_msgSend;
}

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

/* 事件类型(NSEvent.h:25-43; 过滤口径同 hnweb_wk_macos.c) */
#define EV_APPKITDEF 13
#define EV_PERIODIC 16

static obj_t pump_event(double timeout_s) {
    obj_t app = nsapp();
    obj_t mode = nsstr("kCFRunLoopDefaultMode");
    obj_t NSDate = o_getClass("NSDate");
    obj_t lim = ((obj_t (*)(obj_t, sel_t, double))o_msgSend)(
        NSDate, o_sel("dateWithTimeIntervalSinceNow:"), timeout_s);
    return ((obj_t (*)(obj_t, sel_t, unsigned long, obj_t, obj_t, int))o_msgSend)(
        app, o_sel("nextEventMatchingMask:untilDate:inMode:dequeue:"), ~0UL, lim, mode, 1);
}

static void pump_once(double timeout_s) {
    obj_t ev = pump_event(timeout_s);
    if (ev) {
        unsigned long ty = MSG0(unsigned long, ev, "type");
        if (ty < EV_APPKITDEF || ty > EV_PERIODIC)
            MSG1(void, nsapp(), "sendEvent:", ev);
    }
}

static volatile sig_atomic_t g_quit = 0;
static void on_sig(int s) { (void)s; g_quit = 1; }

/* ---------------- 画布表 ---------------- */

#define HOST_MAX_CANVAS 16
typedef struct {
    char   id[64];
    obj_t  win, wv;
    int    w, h;
    int    visible;
    int    transparent, frameless;
    char   url[1024];     /* 非空 = url 模式(重载用); 空 = html 模式 */
} canvas_t;

static canvas_t g_cv[HOST_MAX_CANVAS];
static int g_cv_n = 0;
static obj_t g_handler = NULL;   /* 桥处理器(所有画布 ucc 共用; main 里创建) */

static canvas_t *cv_find(const char *id) {
    for (int i = 0; i < g_cv_n; i++)
        if (!strcmp(g_cv[i].id, id)) return &g_cv[i];
    return NULL;
}

/* 桥: window.neb.post(msg) → 原生。IMP 需要知道来源画布 → 按 message.webView 反查。 */
static void canvas_bridge_imp(obj_t self, sel_t _cmd, obj_t controller, obj_t message) {
    (void)self; (void)_cmd; (void)controller;
    obj_t wv = MSG0(obj_t, message, "webView");
    const char *cid = "?";
    for (int i = 0; i < g_cv_n; i++)
        if (g_cv[i].wv == wv) { cid = g_cv[i].id; break; }
    obj_t body = MSG0(obj_t, message, "body");
    const char *bs = "?";
    if (body) {
        obj_t desc = MSG0(obj_t, body, "description");
        if (desc) bs = MSG0(const char *, desc, "UTF8String");
    }
    printf("[bridge] %s %s\n", cid, bs ? bs : "?");
    fflush(stdout);
}

/* ---------------- 装载等待 / 快照(与 hnweb_wk_macos.c 同口径) ---------------- */

static int wait_loaded(obj_t wv, double timeout_s, double *elapsed_ms) {
    double t0 = now_s();
    int settle = 0;
    while (now_s() - t0 < timeout_s) {
        pump_once(0.016);
        if (!MSG0(char, wv, "isLoading")) {
            if (++settle >= 10) break;
        } else settle = 0;
    }
    if (elapsed_ms) *elapsed_ms = (now_s() - t0) * 1000.0;
    return settle >= 10;
}

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
    obj_t png = ((obj_t (*)(obj_t, sel_t, long, obj_t))o_msgSend)(
        rep, o_sel("representationUsingType:properties:"), 4L /* PNG */, props);
    if (!png) return 0;
    return ((char (*)(obj_t, sel_t, obj_t, int))o_msgSend)(
        png, o_sel("writeToFile:atomically:"), nsstr(g_snap_path), 1) ? 1 : 0;
}

static int wk_snapshot(obj_t wv, const char *out) {
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
    while (!g_snap_done && now_s() - t0 < 5.0) pump_once(0.010);
    return g_snap_done && g_snap_ok;
}

/* ---------------- 画布操作 ---------------- */

static void cv_load(canvas_t *c) {
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", c->url);
    char *sl = strrchr(dir, '/');
    if (sl) *sl = 0; else snprintf(dir, sizeof(dir), ".");
    obj_t ucls = o_getClass("NSURL");
    obj_t furl = ((obj_t (*)(obj_t, sel_t, obj_t, int))o_msgSend)(
        ucls, o_sel("fileURLWithPath:isDirectory:"), nsstr(c->url), 0);
    obj_t durl = ((obj_t (*)(obj_t, sel_t, obj_t, int))o_msgSend)(
        ucls, o_sel("fileURLWithPath:isDirectory:"), nsstr(dir), 1);
    MSG2(void, c->wv, "loadFileURL:allowingReadAccessToURL:", furl, durl);
}

/* 创建画布。req 由调用方持有(字段已取出)。返回 0 成功。 */
static int cv_create(const char *id, const char *url, const char *html, const char *base,
                     int w, int h, int hidden, int transparent, int frameless,
                     long level, int click_through) {
    if (g_cv_n >= HOST_MAX_CANVAS) return -1;
    if (cv_find(id)) return -2;
    if (w <= 0 || h <= 0) { w = 480; h = 360; }

    obj_t config = MSG0(obj_t, o_getClass("WKWebViewConfiguration"), "new");
    obj_t ucc = MSG0(obj_t, o_getClass("WKUserContentController"), "new");
    MSG1(void, config, "setUserContentController:", ucc);
    static const char *BRIDGE_JS =
        "(function(){if(!window.neb){window.neb={post:function(m){"
        "try{window.webkit.messageHandlers.neb.postMessage(m);}catch(e){}}};}})();";
    obj_t script = MSG0(obj_t, o_getClass("WKUserScript"), "alloc");
    script = ((obj_t (*)(obj_t, sel_t, obj_t, long, long))o_msgSend)(
        script, o_sel("initWithSource:injectionTime:forMainFrameOnly:"), nsstr(BRIDGE_JS), 0L, 1L);
    MSG1(void, ucc, "addUserScript:", script);
    MSG2(void, ucc, "addScriptMessageHandler:name:", g_handler, nsstr("neb"));

    obj_t wv = MSG0(obj_t, o_getClass("WKWebView"), "alloc");
    CGRect cr = { { 0, 0 }, { (double)w, (double)h } };
    wv = ((obj_t (*)(obj_t, sel_t, CGRect, obj_t))o_msgSend)(
        wv, o_sel("initWithFrame:configuration:"), cr, config);
    if (!wv) return -3;
    MSGL(void, wv, "setAutoresizingMask:", (2 | 16));

    if (transparent) {
        MSG2(void, wv, "setValue:forKey:", nsnum_bool(0), nsstr("drawsBackground"));
        if (MSG1(int, wv, "respondsToSelector:", o_sel("setUnderPageBackgroundColor:")))
            MSG1(void, wv, "setUnderPageBackgroundColor:",
                 MSG0(obj_t, o_getClass("NSColor"), "clearColor"));
    }

    unsigned long style = (transparent || frameless) ? 0 : (1 | 2 | 4 | 8);
    obj_t win = MSG0(obj_t, o_getClass("NSWindow"), "alloc");
    win = ((obj_t (*)(obj_t, sel_t, CGRect, unsigned long, unsigned long, int))o_msgSend)(
        win, o_sel("initWithContentRect:styleMask:backing:defer:"), cr, style, 2, 0);
    if (!win) return -4;
    MSG1(void, win, "setTitle:", nsstr(id));
    if (transparent) {
        MSGL(void, win, "setOpaque:", 0);
        MSG1(void, win, "setBackgroundColor:", MSG0(obj_t, o_getClass("NSColor"), "clearColor"));
    }
    if (level != 0) MSGL(void, win, "setLevel:", level);
    if (click_through) MSGL(void, win, "setIgnoresMouseEvents:", 1);
    MSG1(void, win, "setContentView:", wv);

    obj_t screen = MSG0(obj_t, o_getClass("NSScreen"), "mainScreen");
    CGRect sf = MSG0(CGRect, screen, "visibleFrame");
    CGRect pos = { { sf.origin.x + (sf.size.w - w) / 2, sf.origin.y + (sf.size.h - h) / 2 },
                   { (double)w, (double)h } };
    ((void (*)(obj_t, sel_t, CGRect, int))o_msgSend)(win, o_sel("setFrame:display:"), pos, 1);

    canvas_t *c = &g_cv[g_cv_n++];
    memset(c, 0, sizeof(*c));
    snprintf(c->id, sizeof(c->id), "%s", id);
    c->win = win; c->wv = wv; c->w = w; c->h = h;
    c->transparent = transparent;
    c->frameless = frameless;
    if (url && url[0]) snprintf(c->url, sizeof(c->url), "%s", url ? url : "");
    c->visible = hidden ? 0 : 1;

    /* 装载: url 模式走 loadFileURL(页面目录可读); html 模式走 loadHTMLString */
    if (c->url[0]) {
        cv_load(c);
    } else {
        char bdir[1024];
        snprintf(bdir, sizeof(bdir), "%s", (base && base[0]) ? base : ".");
        obj_t burl = ((obj_t (*)(obj_t, sel_t, obj_t, int))o_msgSend)(
            o_getClass("NSURL"), o_sel("fileURLWithPath:isDirectory:"), nsstr(bdir), 1);
        MSG2(void, c->wv, "loadHTMLString:baseURL:", nsstr(html ? html : ""), burl);
    }
    if (!hidden) {
        MSG1(void, win, "makeKeyAndOrderFront:", (obj_t)1);
        MSG1(void, nsapp(), "activateIgnoringOtherApps:", (obj_t)1);
    }
    wait_loaded(c->wv, 8.0, NULL);
    return 0;
}

static void cv_destroy(canvas_t *c) {
    if (c->win) {
        MSG0(void, c->win, "close");
        MSG0(void, c->win, "release");
    }
    int idx = (int)(c - g_cv);
    if (idx >= 0 && idx < g_cv_n - 1) memmove(&g_cv[idx], &g_cv[idx + 1], sizeof(canvas_t) * (size_t)(g_cv_n - idx - 1));
    g_cv_n--;
}

static void cv_apply_mode(canvas_t *c, int show) {
    if (show) {
        MSG1(void, c->win, "makeKeyAndOrderFront:", (obj_t)1);
        MSG1(void, nsapp(), "activateIgnoringOtherApps:", (obj_t)1);
        c->visible = 1;
    } else {
        MSG1(void, c->win, "orderOut:", (obj_t)0);
        c->visible = 0;
    }
}

/* ---------------- 协议: JSON-lines ---------------- */

static void json_escape(char *dst, size_t cap, const char *src) {
    size_t o = 0;
    for (; src && *src && o + 2 < cap; src++) {
        unsigned char ch = (unsigned char)*src;
        if (ch == '"' || ch == '\\') { dst[o++] = '\\'; dst[o++] = (char)ch; }
        else if (ch == '\n') { dst[o++] = '\\'; dst[o++] = 'n'; }
        else if (ch == '\r') { dst[o++] = '\\'; dst[o++] = 'r'; }
        else if (ch < 0x20) { /* 丢弃控制字符 */ }
        else dst[o++] = (char)ch;
    }
    dst[o] = 0;
}

static void reply_ok(int fd, const char *extra) {
    char buf[2048];
    snprintf(buf, sizeof(buf), "{\"ok\":true%s%s}\n", extra && extra[0] ? "," : "", extra ? extra : "");
    (void)write(fd, buf, strlen(buf));
}
static void reply_err(int fd, const char *err) {
    char esc[512], buf[768];
    json_escape(esc, sizeof(esc), err ? err : "error");
    snprintf(buf, sizeof(buf), "{\"ok\":false,\"error\":\"%s\"}\n", esc);
    (void)write(fd, buf, strlen(buf));
}

static void list_json(char *out, size_t cap) {
    size_t o = (size_t)snprintf(out, cap, "\"canvases\":[");
    for (int i = 0; i < g_cv_n && o + 160 < cap; i++) {
        canvas_t *c = &g_cv[i];
        o += (size_t)snprintf(out + o, cap - o,
            "%s{\"id\":\"%s\",\"w\":%d,\"h\":%d,\"visible\":%d,"
            "\"form\":{\"transparent\":%d,\"frameless\":%d}}",
            i ? "," : "", c->id, c->w, c->h, c->visible, c->transparent, c->frameless);
    }
    snprintf(out + o, cap - o, "]");
}

/* 返回: 1 继续, 0 退出 */
static int handle_line(int fd, char *line) {
    char *copy = strdup(line);
    if (!copy) { reply_err(fd, "oom"); return 1; }
    void *arena = NULL;
    hn_json *req = hn_json_parse(copy, strlen(copy), &arena);
    if (!req || req->kind != HN_JSON_OBJ) {
        hn_json_free(arena);
        free(copy);
        reply_err(fd, "bad json");
        return 1;
    }
    const hn_json *opv = hn_json_obj_get(req, "op");
    const char *op = hn_json_str(opv, "");

    if (!strcmp(op, "ping")) {
        char extra[1024], lj[768];
        list_json(lj, sizeof(lj));
        snprintf(extra, sizeof(extra), "\"pid\":%d,%s", (int)getpid(), lj);
        reply_ok(fd, extra);
    } else if (!strcmp(op, "list")) {
        char lj[1024];
        list_json(lj, sizeof(lj));
        reply_ok(fd, lj);
    } else if (!strcmp(op, "create")) {
        const char *id = hn_json_str(hn_json_obj_get(req, "id"), "");
        const char *url = hn_json_str(hn_json_obj_get(req, "url"), "");
        const char *html = hn_json_str(hn_json_obj_get(req, "html"), "");
        const char *base = hn_json_str(hn_json_obj_get(req, "base"), "");
        int w = (int)hn_json_num(hn_json_obj_get(req, "w"), 480);
        int h = (int)hn_json_num(hn_json_obj_get(req, "h"), 360);
        int hidden = hn_json_bool(hn_json_obj_get(req, "hidden"), 0);
        const hn_json *form = hn_json_obj_get(req, "form");
        int transparent = hn_json_bool(hn_json_obj_get(form, "transparent"), 0);
        int frameless = hn_json_bool(hn_json_obj_get(form, "frameless"), 0);
        int click_through = hn_json_bool(hn_json_obj_get(form, "click_through"), 0);
        long level = (long)hn_json_num(hn_json_obj_get(form, "level"), 0);
        if (!id[0]) { reply_err(fd, "missing id"); }
        else if (!url[0] && !html[0]) { reply_err(fd, "missing url|html"); }
        else {
            int rc = cv_create(id, url, html, base, w, h, hidden, transparent, frameless, level, click_through);
            if (rc == 0) {
                char extra[256], esc[128];
                json_escape(esc, sizeof(esc), id);
                canvas_t *c = cv_find(id);
                snprintf(extra, sizeof(extra), "\"id\":\"%s\",\"w\":%d,\"h\":%d",
                         esc, c ? c->w : w, c ? c->h : h);
                printf("[host] create id=%s %dx%d url=%s\n", id, w, h, url[0] ? url : "(html)");
                fflush(stdout);
                reply_ok(fd, extra);
            } else if (rc == -1) reply_err(fd, "canvas table full");
            else if (rc == -2) reply_err(fd, "duplicate id");
            else reply_err(fd, "create failed");
        }
    } else if (!strcmp(op, "update") || !strcmp(op, "reload")) {
        const char *id = hn_json_str(hn_json_obj_get(req, "id"), "");
        const char *url = hn_json_str(hn_json_obj_get(req, "url"), "");
        const char *html = hn_json_str(hn_json_obj_get(req, "html"), "");
        const char *base = hn_json_str(hn_json_obj_get(req, "base"), "");
        canvas_t *c = cv_find(id);
        if (!c) reply_err(fd, "canvas not found");
        else if (!strcmp(op, "reload") || url[0]) {
            if (url[0]) snprintf(c->url, sizeof(c->url), "%s", url);
            cv_load(c);
            wait_loaded(c->wv, 8.0, NULL);
            reply_ok(fd, "");
        } else if (html[0]) {
            c->url[0] = 0;
            char bdir[1024];
            snprintf(bdir, sizeof(bdir), "%s", base[0] ? base : ".");
            obj_t burl = ((obj_t (*)(obj_t, sel_t, obj_t, int))o_msgSend)(
                o_getClass("NSURL"), o_sel("fileURLWithPath:isDirectory:"), nsstr(bdir), 1);
            MSG2(void, c->wv, "loadHTMLString:baseURL:", nsstr(html), burl);
            wait_loaded(c->wv, 8.0, NULL);
            reply_ok(fd, "");
        } else reply_err(fd, "missing url|html");
    } else if (!strcmp(op, "show") || !strcmp(op, "hide")) {
        const char *id = hn_json_str(hn_json_obj_get(req, "id"), "");
        canvas_t *c = cv_find(id);
        if (!c) reply_err(fd, "canvas not found");
        else { cv_apply_mode(c, !strcmp(op, "show")); reply_ok(fd, ""); }
    } else if (!strcmp(op, "snapshot")) {
        const char *id = hn_json_str(hn_json_obj_get(req, "id"), "");
        const char *path = hn_json_str(hn_json_obj_get(req, "path"), "");
        canvas_t *c = cv_find(id);
        if (!c) reply_err(fd, "canvas not found");
        else if (!path[0]) reply_err(fd, "missing path");
        else {
            wait_loaded(c->wv, 8.0, NULL);
            if (wk_snapshot(c->wv, path)) {
                char extra[1200], esc[1024];
                json_escape(esc, sizeof(esc), path);
                snprintf(extra, sizeof(extra), "\"path\":\"%s\"", esc);
                reply_ok(fd, extra);
            } else reply_err(fd, "snapshot failed");
        }
    } else if (!strcmp(op, "destroy")) {
        const char *id = hn_json_str(hn_json_obj_get(req, "id"), "");
        canvas_t *c = cv_find(id);
        if (!c) reply_err(fd, "canvas not found");
        else { cv_destroy(c); reply_ok(fd, ""); }
    } else if (!strcmp(op, "quit")) {
        reply_ok(fd, "");
        g_quit = 1;
    } else {
        reply_err(fd, "unknown op");
    }
    hn_json_free(arena);
    free(copy);
    return 1;
}

/* ---------------- 主循环 ---------------- */

#define HOST_MAX_CONN 8
typedef struct { int fd; char buf[65536]; size_t len; } conn_t;

int main(int argc, char **argv) {
    const char *sock_path = NULL;
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "--socket") && i + 1 < argc) sock_path = argv[++i];
    if (!sock_path) {
        fprintf(stderr, "用法: hncanvas_host_macos --socket <unix_socket_path>\n");
        return 2;
    }

    if (!objc_load() || !classapi_load()) {
        fprintf(stderr, "hncanvas_host_macos: WebKit/AppKit 装载失败\n");
        return 1;
    }
    obj_t app = nsapp();
    MSG0(void, app, "finishLaunching");
    MSGL(void, app, "setActivationPolicy:", 1);

    /* 桥处理器: 注册一次, 所有画布的 ucc 共用(实现按 message.webView 反查画布) */
    obj_t hcls = class_alloc(o_getClass("NSObject"), "NebCanvasBridge", 0);
    if (!hcls) { fprintf(stderr, "host: 类注册失败\n"); return 1; }
    class_addMethod(hcls, o_sel("userContentController:didReceiveScriptMessage:"),
                    (void *)canvas_bridge_imp, "v@:@@");
    class_register(hcls);
    obj_t handler = MSG0(obj_t, hcls, "alloc");
    handler = MSG0(obj_t, handler, "init");
    g_handler = handler;

    /* Unix socket */
    unlink(sock_path);
    int lfd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (lfd < 0) { perror("socket"); return 1; }
    struct sockaddr_un sa;
    memset(&sa, 0, sizeof(sa));
    sa.sun_family = AF_UNIX;
    snprintf(sa.sun_path, sizeof(sa.sun_path), "%s", sock_path);
    if (bind(lfd, (struct sockaddr *)&sa, sizeof(sa)) != 0) { perror("bind"); close(lfd); return 1; }
    listen(lfd, HOST_MAX_CONN);

    signal(SIGTERM, on_sig);
    signal(SIGINT, on_sig);
    printf("[host] listening %s pid=%d\n", sock_path, (int)getpid());
    fflush(stdout);

    conn_t conns[HOST_MAX_CONN];
    memset(conns, 0, sizeof(conns));
    for (int i = 0; i < HOST_MAX_CONN; i++) conns[i].fd = -1;

    while (!g_quit) {
        pump_once(0.005);
        struct pollfd pfd[HOST_MAX_CONN + 1];
        int nfd = 0;
        pfd[nfd].fd = lfd; pfd[nfd].events = POLLIN; pfd[nfd].revents = 0; nfd++;
        for (int i = 0; i < HOST_MAX_CONN; i++) {
            if (conns[i].fd >= 0) {
                pfd[nfd].fd = conns[i].fd; pfd[nfd].events = POLLIN; pfd[nfd].revents = 0;
                nfd++;
            }
        }
        int pr = poll(pfd, (nfds_t)nfd, 10);
        if (pr < 0) { if (errno == EINTR) continue; break; }
        if (pr > 0) {
            if (pfd[0].revents & POLLIN) {
                int cfd = accept(lfd, NULL, NULL);
                if (cfd >= 0) {
                    for (int i = 0; i < HOST_MAX_CONN; i++)
                        if (conns[i].fd < 0) { conns[i].fd = cfd; conns[i].len = 0; break; }
                }
            }
            int pi = 1;
            for (int i = 0; i < HOST_MAX_CONN; i++) {
                if (conns[i].fd < 0) continue;
                struct pollfd *pf = &pfd[pi++];
                if (!(pf->revents & (POLLIN | POLLHUP | POLLERR))) continue;
                ssize_t got = read(conns[i].fd, conns[i].buf + conns[i].len,
                                   sizeof(conns[i].buf) - conns[i].len - 1);
                if (got <= 0) {
                    close(conns[i].fd);
                    conns[i].fd = -1;
                    conns[i].len = 0;
                    continue;
                }
                conns[i].len += (size_t)got;
                conns[i].buf[conns[i].len] = 0;
                char *start = conns[i].buf;
                char *nl;
                while ((nl = strchr(start, '\n')) != NULL) {
                    *nl = 0;
                    if (*start) handle_line(conns[i].fd, start);
                    start = nl + 1;
                }
                size_t rest = conns[i].len - (size_t)(start - conns[i].buf);
                if (rest) memmove(conns[i].buf, start, rest);
                conns[i].len = rest;
            }
        }
    }

    /* 收尾: 画布全关, socket 删除 */
    while (g_cv_n > 0) cv_destroy(&g_cv[0]);
    for (int i = 0; i < HOST_MAX_CONN; i++)
        if (conns[i].fd >= 0) close(conns[i].fd);
    close(lfd);
    unlink(sock_path);
    printf("[host] exit\n");
    fflush(stdout);
    return 0;
}

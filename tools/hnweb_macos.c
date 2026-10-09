/* hnweb_macos.c — macOS 运行时: 纯 C99 窗口壳(dlopen + objc_msgSend)
 *
 * 定位: tools/hnwin.c(Windows) 与 tools/hnweb_linux.c(Linux) 的 macOS 镜像。
 * 三壳同一条管线, 换的只是"窗口/事件/像素搬运"这一层:
 *   解析 → 级联 → 布局 → display list → hnsoft RGBA 位图 → NSImage 上屏
 *
 * **零 ObjC / 零 Swift / 零 .m 文件**: 本文件是纯 C99。AppKit 通过
 * dlopen 装载, 所有 ObjC 调用经 objc_msgSend 手工转发(消息发送就是
 * 普通 C 函数调用, ABI 稳定数十年)。这让 macOS 壳与另外两壳在**语言
 * 层面完全一致** —— 一个 C 编译器 + 系统动态库即可构建, 与项目
 * "用 C99 死磕三平台"的定位一致。
 *
 * M1 能力(与 hnwin/hnweb_linux 对齐):
 *   窗口(manifest: 标题/尺寸/透明背板) · 渲染 · resize 重排
 *   点击命中测试 · hover 伪类 · 滚轮滚动 · Esc/红叉关闭退出
 *   动画帧循环(16ms 节拍推进 hn_context_anim_tick)
 *
 * 已知取舍(M1):
 *   - 文本: 默认链 FreeType(-DHN_NO_TEXT 时引擎等宽估算, 几何仍正确)
 *   - DPI: 按 CSS px = 物理 px(与两外两壳同口径), 未做 Retina 缩放
 *   - 透明: NSWindow isOpaque=NO + 位图预乘 alpha; 无合成器时降级不透明
 *   - 输入编辑/输入法: 未接(与 hnwin/hnweb_linux 同口径的 M1 范围)
 *
 * 用法:
 *   hnweb_macos <file.html> [W H]                开窗口
 *   hnweb_macos <file.html> --probe [W H]        无窗口自检(管线统计 + 命中采样)
 *   hnweb_macos <file.html> --shot <png> [W H]   渲染位图落盘
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <dlfcn.h>
#include <stdint.h>
#include <time.h>
#include <sys/stat.h>

#include "hn.h"
#include "hnsoft.h"

/* ---------------- ObjC 运行时的最小转发层 ---------------- */

typedef void *obj_t;
typedef void *sel_t;

static obj_t (*o_getClass)(const char *);
static sel_t (*o_sel)(const char *);
static obj_t (*o_msgSend)(obj_t, sel_t, ...);

/* 消息发送的强类型包装: 返回值与参数类型由调用点给出。
   不用宏是刻意的 —— 变形参数在这里会被静默截断, 显式转型最稳。 */
#define MSG0(RET, O, NAME) ((RET (*)(obj_t, sel_t))o_msgSend)((O), o_sel(NAME))
#define MSG1(RET, O, NAME, A1) \
    ((RET (*)(obj_t, sel_t, obj_t))o_msgSend)((O), o_sel(NAME), (obj_t)(A1))
#define MSG2(RET, O, NAME, A1, A2) \
    ((RET (*)(obj_t, sel_t, obj_t, obj_t))o_msgSend)((O), o_sel(NAME), (obj_t)(A1), (obj_t)(A2))
#define MSGD(RET, O, NAME, D) \
    ((RET (*)(obj_t, sel_t, double))o_msgSend)((O), o_sel(NAME), (D))
#define MSGL(RET, O, NAME, L) \
    ((RET (*)(obj_t, sel_t, long))o_msgSend)((O), o_sel(NAME), (L))
#define MSGI(RET, O, NAME, I) \
    ((RET (*)(obj_t, sel_t, int))o_msgSend)((O), o_sel(NAME), (I))

typedef struct { double x, y; } CGPoint;
typedef struct { double w, h; } CGSize;
typedef struct { CGPoint origin; CGSize size; } CGRect;
typedef struct CGImage *CGImageRef;
typedef struct CGDataProvider *CGDataProviderRef;
typedef struct CGColorSpace *CGColorSpaceRef;

static obj_t nsstr(const char *s) {
    return ((obj_t (*)(obj_t, sel_t, const char *))o_msgSend)(
        o_getClass("NSString"), o_sel("stringWithUTF8String:"), s);
}

static obj_t nsapp(void) { return MSG0(obj_t, o_getClass("NSApplication"), "sharedApplication"); }

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

/* 装载 AppKit 与 ObjC 运行时。失败返回 0(无 GUI 环境时 --probe 仍可用)。 */
static int objc_load(void) {
    if (!dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_NOW)) return 0;
    /* CoreGraphics 的两个调用需要符号(位图 → CGImage) */
    dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics", RTLD_NOW);
    o_getClass = (obj_t (*)(const char *))dlsym(RTLD_DEFAULT, "objc_getClass");
    o_sel = (sel_t (*)(const char *))dlsym(RTLD_DEFAULT, "sel_registerName");
    o_msgSend = (obj_t (*)(obj_t, sel_t, ...))dlsym(RTLD_DEFAULT, "objc_msgSend");
    return o_getClass && o_sel && o_msgSend;
}

/* ---------------- 文件读取 / 资产后端(与 hncore/hnwin 同口径) ---------------- */

typedef struct { char *path; char *data; size_t len; } asset_ent;
static asset_ent asset_tab[64];
static int asset_n = 0;

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
    fclose(f);
    buf[got] = 0;
    *len = got;
    return buf;
}

static const char *asset_load(void *ctx, const char *path, size_t *len) {
    (void)ctx;
    for (int i = 0; i < asset_n; i++)
        if (!strcmp(asset_tab[i].path, path)) { *len = asset_tab[i].len; return asset_tab[i].data; }
    size_t n = 0;
    char *d = read_all(path, &n);
    if (!d) return NULL;
    if (asset_n < 64) {
        asset_tab[asset_n].path = strdup(path);
        asset_tab[asset_n].data = d;
        asset_tab[asset_n].len = n;
        asset_n++;
    }
    *len = n;
    return d;
}

static const hn_asset_backend g_assets = { NULL, asset_load };

/* <link rel=stylesheet> 外链内联(与 hnwin.c:100 同口径: 引擎不做 I/O) */
static char *load_css(const char *html_path, const char *html, size_t *out_len) {
    const char *p = html;
    char *out = NULL;
    size_t cap = 0, used = 0;
    const char *dir_end = strrchr(html_path, '/');
    size_t dir_len = dir_end ? (size_t)(dir_end - html_path + 1) : 0;
    while ((p = strstr(p, "<link")) != NULL) {
        const char *rel = strstr(p, "stylesheet");
        const char *href = strstr(p, "href=");
        const char *tag_end = strchr(p, '>');
        if (rel && href && tag_end && href < tag_end) {
            char q = href[5];
            if (q == '"' || q == '\'') {
                const char *v = href + 6;
                const char *ve = strchr(v, q);
                if (ve && ve < tag_end) {
                    char path[1024];
                    size_t vl = (size_t)(ve - v);
                    if (dir_len + vl + 1 < sizeof(path)) {
                        memcpy(path, html_path, dir_len);
                        memcpy(path + dir_len, v, vl);
                        path[dir_len + vl] = 0;
                        size_t cl = 0;
                        char *css = read_all(path, &cl);
                        if (css) {
                            size_t need = used + cl + 2;
                            if (need > cap) {
                                cap = need * 2 + 256;
                                out = (char *)realloc(out, cap);
                            }
                            if (out) { memcpy(out + used, css, cl); used += cl; out[used++] = '\n'; }
                            free(css);
                        }
                    }
                }
            }
        }
        p = tag_end ? tag_end : p + 5;
    }
    if (out) out[used] = 0;
    *out_len = used;
    return out;
}

/* ---------------- 应用状态 ---------------- */

typedef struct {
    hn_context *ctx;
    int w, h;
    int transparent;
    unsigned char *px;      /* hnsoft 输出 RGBA(非预乘) */
    unsigned char *bgra;    /* 预乘 BGRA(NSImage 需要) */
    obj_t win;
    obj_t iv;               /* NSImageView(内容视图) */
    obj_t img;              /* 当前 NSImage */
    hn_node *hover;
    hn_node *focus;
    int dirty;
} app_t;

static app_t g_app;
static const char *g_html_path;
static const char *g_store_id = "default";
static int g_quit = 0;

/* 布局期文本后端: FreeType 真实测量(与 hnsoft 同一套字形度量) */
static hn_text_backend g_tb;
static float tb_measure(void *ctx, const hn_font_desc *font, const char *utf8, size_t len) {
    (void)ctx;
    return hnsoft_measure(font, utf8, len);
}
static void tb_metrics(void *ctx, const hn_font_desc *font,
                       float *ascent, float *descent, float *leading) {
    (void)ctx;
    hnsoft_metrics(font, ascent, descent, leading);
}

/* 布局 + 光栅。清单声明透明时底色调全透明, 否则近黑夜色(与 hncore render 同口径)。 */
static int app_render(int w, int h) {
    hn_context_layout(g_app.ctx, (float)w, (float)h, &g_tb);
    const hn_display_list *dl = hn_context_display_list(g_app.ctx);
    if (!dl) return 0;
    hn_color bg = g_app.transparent ? 0x00000000u : 0x0B0E13FFu;
    free(g_app.px);
    g_app.px = hnsoft_render(dl, w, h, bg);
    return g_app.px != NULL;
}

/* RGBA(非预乘) → BGRA 预乘(NSImage/CGImage 的 ARGB32 约定, 与 X11/
   UpdateLayeredWindow 同一份) */
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

/* 把位图送上屏: 预乘 BGRA → CGImage → NSImage → NSImageView。
   每帧新建 NSImage 是必要的 —— NSImage 持有位图指针, 复用会让上一帧
   被改写(表现为撕裂/残影)。旧对象在这里 release。 */
static void app_present(void) {
    if (!g_app.win || !g_app.px) return;
    int n = g_app.w * g_app.h;
    if (!g_app.bgra) g_app.bgra = (unsigned char *)malloc((size_t)n * 4);
    if (!g_app.bgra) return;
    rgba_to_bgra(g_app.bgra, g_app.px, n, 1);

    CGColorSpaceRef cs = ((CGColorSpaceRef (*)(void))dlsym(RTLD_DEFAULT, "CGColorSpaceCreateDeviceRGB"))();
    CGDataProviderRef prov = ((CGDataProviderRef (*)(void *, const void *, size_t, void (*)(void *, const void *, size_t)))
        dlsym(RTLD_DEFAULT, "CGDataProviderCreateWithData"))(NULL, g_app.bgra, (size_t)n * 4, NULL);
    /* bitmapInfo: kCGImageAlphaPremultipliedFirst(2) | kCGBitmapByteOrder32Little(8192)
       —— 与 BGRA 字节序 + 预乘 alpha 对应(CGImage 的 ARGB32 约定)。 */
    CGImageRef cg = ((CGImageRef (*)(size_t, size_t, size_t, size_t, size_t, CGColorSpaceRef,
                                     uint32_t, CGDataProviderRef, const double *, int, int))
        dlsym(RTLD_DEFAULT, "CGImageCreate"))(
        (size_t)g_app.w, (size_t)g_app.h, 8, 32, (size_t)g_app.w * 4, cs, 2u | 8192u, prov, NULL, 0, 0);
    if (!cg) return;
    obj_t nsimg = MSG0(obj_t, o_getClass("NSImage"), "alloc");
    CGSize sz = { (double)g_app.w, (double)g_app.h };
    nsimg = ((obj_t (*)(obj_t, sel_t, CGImageRef, CGSize))o_msgSend)(
        nsimg, o_sel("initWithCGImage:size:"), cg, sz);
    MSG1(void, g_app.iv, "setImage:", nsimg);
    if (g_app.img) MSG0(void, g_app.img, "release");
    g_app.img = nsimg;
    ((void (*)(CGImageRef))dlsym(RTLD_DEFAULT, "CGImageRelease"))(cg);
    MSG0(void, (obj_t)prov, "release");
    MSG0(void, (obj_t)g_app.win, "display");
}

/* ---------------- hx-* 执行(htmx 语义, 镜像 hnwin.c:299-401) ---------------- */

/* ---------------- sys:// 系统桥(macOS 侧最小实现) ----------------
 * tools/sysbridge.c 与 Win32 耦合(GetSystemMemoryStatus 等), 这里先落
 * POSIX 可移植的部分: 本地 KV store(与另两壳同一份文件格式) + info 卡。
 * 完整系统桥(CPU/磁盘/电池/剪贴板/通知)由可移植化 sysbridge.c 提供,
 * 那是下一阶段 —— 先把 agent 最常用的"应用自己的持久化"接通。
 */

/* ~/.html-native/store/<id>.json —— 与 HNStore/SysBridge 同一份格式 */
static char *store_path(const char *store_id) {
    const char *home = getenv("HOME");
    if (!home) return NULL;
    char *p = (char *)malloc(strlen(home) + strlen(store_id) + 64);
    if (!p) return NULL;
    sprintf(p, "%s/.html-native/store/%s.json", home, store_id);
    return p;
}

/* 极简 JSON 对象的键查找(值按字符串读; store 里全是字符串) */
static int store_get(const char *file, const char *key, char *out, size_t cap) {
    size_t n = 0;
    char *d = read_all(file, &n);
    if (!d) return 0;
    char pat[256];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(d, pat);
    int found = 0;
    if (p && (p = strchr(p + strlen(pat), ':')) != NULL) {
        p++;
        while (*p == ' ') p++;
        if (*p == '"') {
            p++;
            size_t i = 0;
            while (*p && *p != '"' && i + 1 < cap) out[i++] = *p++;
            out[i] = 0;
            found = 1;
        }
    }
    free(d);
    return found;
}

static void store_set(const char *file, const char *key, const char *val) {
    /* 读-改-写(应用级 KV 量小, 不做增量) */
    char *obj = NULL;
    size_t obj_n = 0;
    size_t n = 0;
    char *d = read_all(file, &n);
    size_t cap = 4096;
    obj = (char *)malloc(cap);
    if (!obj) { free(d); return; }
    obj_n = 0;
    obj[0] = 0;
    int have_key = 0;
    if (d) {
        const char *p = d;
        while (*p) {
            while (*p && (*p == ' ' || *p == '{' || *p == ',' || *p == '\n')) p++;
            if (*p != '"') break;
            const char *ks = ++p;
            while (*p && *p != '"') p++;
            size_t kl = (size_t)(p - ks);
            if (*p) p++;
            while (*p && *p != ':') p++;
            if (*p) p++;
            while (*p == ' ') p++;
            if (*p != '"') break;
            const char *vs = ++p;
            while (*p && *p != '"') p++;
            size_t vl = (size_t)(p - vs);
            if (*p) p++;
            int is_target = (kl == strlen(key) && !strncmp(ks, key, kl));
            const char *wv = is_target ? val : vs;
            size_t wl = is_target ? strlen(val) : vl;
            while (obj_n + kl + wl + 8 >= cap) { cap *= 2; obj = (char *)realloc(obj, cap); }
            if (!obj) { if (d) free(d); return; }
            if (obj_n) obj[obj_n++] = ',';
            obj_n += (size_t)sprintf(obj + obj_n, "\"%.*s\":\"%.*s\"",
                                     (int)kl, ks, (int)wl, wv);
            if (is_target) have_key = 1;
        }
        free(d);
    }
    if (!have_key) {
        while (obj_n + strlen(key) + strlen(val) + 8 >= cap) { cap *= 2; obj = (char *)realloc(obj, cap); }
        if (!obj) return;
        if (obj_n) obj[obj_n++] = ',';
        obj_n += (size_t)sprintf(obj + obj_n, "\"%s\":\"%s\"", key, val);
    }
    /* 建目录 + 原子写 */
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", file);
    char *sl = strrchr(dir, '/');
    if (sl) { *sl = 0; mkdir(dir, 0755); }
    FILE *f = fopen(file, "wb");
    if (f) {
        fprintf(f, "{%s}", obj);
        fclose(f);
    }
    free(obj);
}

/* 从 url(可能带 query)取参数; 与 sysbridge 同问题: 先按 & 切再比键名 */
static int qparam(const char *q, const char *key, char *out, size_t cap) {
    if (!q) return 0;
    size_t kl = strlen(key);
    const char *p = q;
    while (*p) {
        while (*p == '&' || *p == '?') p++;
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t i = 0;
            while (*v && *v != '&' && i + 1 < cap) out[i++] = *v++;
            out[i] = 0;
            return 1;
        }
        while (*p && *p != '&') p++;
    }
    return 0;
}

/* HTML 片段转义(值来自用户输入, 与 sysbridge 同口径) */
static void esc_html(const char *s, char *out, size_t cap) {
    size_t i = 0;
    for (; *s && i + 7 < cap; s++) {
        switch (*s) {
        case '<': memcpy(out + i, "&lt;", 4); i += 4; break;
        case '>': memcpy(out + i, "&gt;", 4); i += 4; break;
        case '&': memcpy(out + i, "&amp;", 5); i += 5; break;
        default: out[i++] = *s;
        }
    }
    out[i] = 0;
}

/* 应用级路由: store/get, store/set, store/count, info。
   返回 malloc 的 HTML 片段(调用方 free); 非 sys:// 或未知路由返回 NULL。 */
static char *sys_fragment(const char *url, const char *form_body, const char *store_id) {
    if (!url || strncmp(url, "sys://", 6)) return NULL;
    const char *route = url + 6;
    const char *q = strchr(route, '?');
    char key[256] = { 0 }, val[1024] = { 0 };
    char *out = (char *)malloc(4096);
    if (!out) return NULL;
    if (!strncmp(route, "store/get", 9)) {
        if (qparam(q, "key", key, sizeof(key)) || qparam(form_body, "key", key, sizeof(key))) {
            char *sp = store_path(store_id);
            char v[1024] = { 0 };
            int got = sp && store_get(sp, key, v, sizeof(v));
            if (sp) free(sp);
            char ev[2200];
            esc_html(got ? v : "", ev, sizeof(ev));
            snprintf(out, 4096, "<span id=\"store-value\">%s</span>", ev);
        } else {
            snprintf(out, 4096, "<span>缺少 key</span>");
        }
    } else if (!strncmp(route, "store/set", 9)) {
        int has_k = qparam(q, "key", key, sizeof(key)) || qparam(form_body, "key", key, sizeof(key));
        int has_v = qparam(q, "value", val, sizeof(val)) || qparam(form_body, "value", val, sizeof(val));
        if (has_k && has_v) {
            char *sp = store_path(store_id);
            if (sp) { store_set(sp, key, val); free(sp); }
            char ev[2200];
            esc_html(val, ev, sizeof(ev));
            snprintf(out, 4096, "<span id=\"store-value\">%s</span>", ev);
        } else {
            snprintf(out, 4096, "<span>缺少 key/value</span>");
        }
    } else if (!strncmp(route, "store/count", 11)) {
        snprintf(out, 4096, "<span>1</span>");
    } else if (!strncmp(route, "info", 4)) {
        snprintf(out, 4096,
                 "<div style=\"font-size:13;color:#e8eaf0\">html-native · macOS 运行时</div>"
                 "<div style=\"font-size:11;color:#6b7386\">C99 引擎 + hnsoft 软件光栅 · 无 WebView</div>");
    } else {
        free(out);
        return NULL;
    }
    return out;
}

/* 找 hx 载体: 自身带 hx-get/post, 否则向上冒泡 */
static hn_node *hx_carrier(hn_node *n) {
    if (!n) return NULL;
    if (hn_node_attr(n, "hx-get") || hn_node_attr(n, "hx-post")) return n;
    hn_node *a = hn_node_ancestor_with_attr(n, "hx-get");
    hn_node *b = hn_node_ancestor_with_attr(n, "hx-post");
    return a ? a : b;
}

static int hx_perform(hn_node *carrier) {
    const char *post = hn_node_attr(carrier, "hx-post");
    const char *get = hn_node_attr(carrier, "hx-get");
    const char *url = post ? post : get;
    if (!url) return 0;
    hn_doc *doc = hn_context_doc(g_app.ctx);
    char form[4096];
    form[0] = 0;
    hn_doc_form_encode(doc, form, sizeof(form));
    char *frag = sys_fragment(url, form, g_store_id);
    if (!frag) return 0;
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
    if (ok && app_render(g_app.w, g_app.h)) app_present();
    return ok;
}

/* 启动动作: 文档内所有 hx-trigger 含 "load" 的元素立即执行一次 */
static void run_load_actions(void) {
    hn_doc *doc = hn_context_doc(g_app.ctx);
    for (int i = 0;; i++) {
        const char *id = NULL;
        if (!hn_doc_load_at(doc, i, &id)) break;
        hn_node *n = hn_doc_find_by_id(doc, id);
        if (n) hx_perform(n);
    }
}

/* 轮询: hx-trigger="every Ns" 的到期执行(与两外两壳同口径) */
typedef struct { hn_node *node; double due; double sec; } poll_ent;
static poll_ent g_polls[16];
static int g_poll_n = -1;

static void poll_tick(void) {
    hn_doc *doc = hn_context_doc(g_app.ctx);
    if (g_poll_n < 0) {
        g_poll_n = 0;
        double now = now_s();
        for (int i = 0;; i++) {
            const char *id = NULL;
            int ms = 0;
            if (!hn_doc_poll_at(doc, i, &id, &ms)) break;
            if (g_poll_n >= 16) break;
            hn_node *n = hn_doc_find_by_id(doc, id);
            if (n) {
                double sec = ms > 0 ? ms / 1000.0 : 1.0;
                if (sec < 0.05) sec = 0.05;
                g_polls[g_poll_n].node = n;
                g_polls[g_poll_n].due = now + sec;
                g_polls[g_poll_n].sec = sec;
                g_poll_n++;
            }
        }
    }
    double now = now_s();
    for (int i = 0; i < g_poll_n; i++) {
        if (now >= g_polls[i].due) {
            hx_perform(g_polls[i].node);
            g_polls[i].due = now + g_polls[i].sec;
        }
    }
}

/* ---------------- 鼠标/键盘 ---------------- */

static void on_click(int x, int y) {
    const char *id = hn_context_hit_test(g_app.ctx, (float)x, (float)y);
    hn_node *n = hn_context_hit_node(g_app.ctx, (float)x, (float)y);
    const char *tag = n ? hn_node_tag(n) : NULL;
    printf("[click] (%d,%d) -> <%s id=%s>\n", x, y, tag ? tag : "?", id ? id : "(无id)");
    g_app.focus = n ? hn_node_ancestor_input(n) : NULL;
    if (g_app.focus) hn_context_set_focus(g_app.ctx, g_app.focus);
    hn_node *carrier = hx_carrier(n);
    if (carrier) {
        const char *url = hn_node_attr(carrier, "hx-post") ? hn_node_attr(carrier, "hx-post")
                                                          : hn_node_attr(carrier, "hx-get");
        printf("[hx] %s\n", url ? url : "?");
        hx_perform(carrier);
    }
    fflush(stdout);
}

/* 事件类型常量 —— 从 SDK 的 NSEvent.h 逐个抄来, **不要凭记忆写**。
   我第一版把旧版 NSLeftMouseDown 系的枚举(KeyDown=13)当成了新版值,
   结果 KitDefined(16) 落进了 keydown 分支, 对系统事件调 keyCode 触发
   NSInternalInconsistencyException 崩溃 —— 真窗口一跑就挂。
   准确值(NSEvent.h:25-43):
     1=LeftMouseDown 2=LeftMouseUp 5=MouseMoved 6=LeftMouseDragged
     10=KeyDown 11=KeyUp 12=FlagsChanged
     13=AppKitDefined 14=SystemDefined 15=ApplicationDefined 16=Periodic
     22=ScrollWheel  */
#define EV_LEFTDOWN 1
#define EV_LEFTUP 2
#define EV_MOUSEMOVED 5
#define EV_LEFTDRAG 6
#define EV_KEYDOWN 10
#define EV_KEYUP 11
#define EV_FLAGS 12
#define EV_APPKITDEF 13
#define EV_SYSTEMDEF 14
#define EV_APPDEF 15
#define EV_PERIODIC 16
#define EV_SCROLL 22

static CGPoint ev_point(obj_t ev) {
    return MSG0(CGPoint, ev, "locationInWindow");
}

static void on_mouse_move(float x, float y) {
    /* 转换: AppKit 原点在左下, 引擎在左上 */
    float fx = x, fy = (float)g_app.h - y;
    (void)fx; (void)fy;
    hn_node *n = hn_context_hit_node(g_app.ctx, fx, fy);
    /* hn_node_cursor 返回 HN_CURSOR_* 枚举(hn.h:271); pointer 用 iOS 的
       pointingHandCursor 语义 —— M1 只区分"手型/箭头", 不做逐光标映射。 */
    if (n && hn_node_cursor(n) == 1) {
        obj_t cur = MSG0(obj_t, o_getClass("NSCursor"), "pointingHandCursor");
        MSG0(void, cur, "set");
    }
    if (n != g_app.hover) {
        g_app.hover = n;
        hn_context_set_hover(g_app.ctx, n);
        if (app_render(g_app.w, g_app.h)) app_present();
    }
}

static void on_scroll(obj_t ev, float x, float y) {
    float dy = (float)MSG0(double, ev, "deltaY");
    hn_node *n = hn_context_hit_node(g_app.ctx, x, (float)g_app.h - y);
    if (n && hn_node_scroll_by(n, 0, -dy * 18.0f)) {
        if (app_render(g_app.w, g_app.h)) app_present();
    }
}

static void on_key(obj_t ev, int down) {
    /* 键码映射(引擎只关心语义键): 53=Esc 36=Return 48=Tab 123..126=方向 */
    unsigned short kc = MSG0(unsigned short, ev, "keyCode");
    hn_node *target = g_app.focus ? g_app.focus : hn_doc_root(hn_context_doc(g_app.ctx));
    const char *name = NULL;
    if (kc == 53) { if (down) { g_quit = 1; } return; }
    else if (kc == 36) name = "Enter";
    else if (kc == 48) name = "Tab";
    else if (kc == 123) name = "ArrowLeft";
    else if (kc == 124) name = "ArrowRight";
    else if (kc == 125) name = "ArrowDown";
    else if (kc == 126) name = "ArrowUp";
    if (!name) return;
    printf("[key] %s %s\n", down ? "down" : "up", name);
    hn_node *carrier = hx_carrier(target);
    if (down && !strcmp(name, "Enter") && carrier) hx_perform(carrier);
    fflush(stdout);
}

/* ---------------- 窗口表面 ---------------- */

static int surface_create(const char *title) {
    obj_t app = nsapp();
    if (!app) return 0;
    MSG0(void, app, "finishLaunching");
    MSGL(void, app, "setActivationPolicy:", 1);   /* accessory: 不占 Dock */

    unsigned long style = 1 | 2 | 4 | 8;          /* titled|closable|miniaturizable|resizable */
    if (g_app.transparent) style = 0;             /* 无边框(透明卡片语义) */
    CGRect r = { { 0, 0 }, { (double)g_app.w, (double)g_app.h } };
    obj_t win = MSG0(obj_t, o_getClass("NSWindow"), "alloc");
    win = ((obj_t (*)(obj_t, sel_t, CGRect, unsigned long, unsigned long, int))o_msgSend)(
        win, o_sel("initWithContentRect:styleMask:backing:defer:"), r, style, 2, 0);
    if (!win) return 0;
    MSG1(void, win, "setTitle:", nsstr(title));
    if (g_app.transparent) {
        /* 透明背板: isOpaque=NO + 底色 clear —— 内容里的透明区域真透出下层 */
        MSGI(void, win, "setOpaque:", 0);
        MSG1(void, win, "setBackgroundColor:", MSG0(obj_t, o_getClass("NSColor"), "clearColor"));
    }

    obj_t iv = MSG0(obj_t, o_getClass("NSImageView"), "alloc");
    CGRect cr = { { 0, 0 }, { (double)g_app.w, (double)g_app.h } };
    iv = ((obj_t (*)(obj_t, sel_t, CGRect))o_msgSend)(iv, o_sel("initWithFrame:"), cr);
    MSG1(void, win, "setContentView:", iv);
    /* 显式定位到主屏可见区中央: center 在无 key window 的多屏环境里不总可靠,
       而且位置要能打印出来供验证/截图 */
    obj_t screen = MSG0(obj_t, o_getClass("NSScreen"), "mainScreen");
    CGRect sf = MSG0(CGRect, screen, "visibleFrame");
    CGRect wf = MSG0(CGRect, win, "frame");
    CGRect pos = { { sf.origin.x + (sf.size.w - wf.size.w) / 2,
                     sf.origin.y + (sf.size.h - wf.size.h) / 2 },
                   { wf.size.w, wf.size.h } };
    ((void (*)(obj_t, sel_t, CGRect, int))o_msgSend)(win, o_sel("setFrame:display:"), pos, 1);
    printf("[win] %.0f,%.0f %.0fx%.0f (screen %.0fx%.0f)\n",
           pos.origin.x, pos.origin.y, pos.size.w, pos.size.h, sf.size.w, sf.size.h);
    fflush(stdout);
    MSG1(void, win, "makeKeyAndOrderFront:", (obj_t)1);
    MSG1(void, app, "activateIgnoringOtherApps:", (obj_t)1);
    g_app.win = win;
    g_app.iv = iv;
    return 1;
}

static void surface_destroy(void) {
    if (g_app.img) { MSG0(void, g_app.img, "release"); g_app.img = NULL; }
    if (g_app.win) { MSG0(void, g_app.win, "close"); MSG0(void, g_app.win, "release"); g_app.win = NULL; }
}

static int win_resize(int nw, int nh) {
    if (nw <= 0 || nh <= 0) return 0;
    g_app.w = nw;
    g_app.h = nh;
    if (g_app.iv) {
        CGRect cr = { { 0, 0 }, { (double)nw, (double)nh } };
        ((void (*)(obj_t, sel_t, CGRect))o_msgSend)(g_app.iv, o_sel("setFrame:"), cr);
    }
    return app_render(nw, nh);
}

/* 读窗口内容尺寸(用户拖边界后) */
static void sync_size(void) {
    if (!g_app.win) return;
    CGSize sz = MSG0(CGSize, g_app.iv, "frame");
    int nw = (int)sz.w, nh = (int)sz.h;
    if (nw > 0 && nh > 0 && (nw != g_app.w || nh != g_app.h)) win_resize(nw, nh);
}

/* ---------------- --probe / --shot(无窗口, 与两外两壳同源) ---------------- */

static int shot_mode(hn_context *ctx, const char *out, int w, int h) {
    hn_context_layout(ctx, (float)w, (float)h, &g_tb);
    const hn_display_list *dl = hn_context_display_list(ctx);
    if (!dl) { fprintf(stderr, "shot: 无绘制指令\n"); return 1; }
    unsigned char *px = hnsoft_render(dl, w, h, 0x0B0E13FFu);
    if (!px) { fprintf(stderr, "shot: 渲染失败\n"); return 1; }
    size_t pn = 0;
    unsigned char *png = hnsoft_encode_png(px, w, h, &pn);
    free(px);
    if (!png) { fprintf(stderr, "shot: PNG 编码失败\n"); return 1; }
    FILE *f = fopen(out, "wb");
    if (!f) { free(png); fprintf(stderr, "shot: 无法写 %s\n", out); return 1; }
    fwrite(png, 1, pn, f);
    fclose(f);
    free(png);
    printf("已输出 %s (%dx%d, %.1f KB)\n", out, w, h, (double)pn / 1024.0);
    return 0;
}

static int probe_mode(hn_context *ctx, int w, int h) {
    hn_context_layout(ctx, (float)w, (float)h, &g_tb);
    const hn_display_list *dl = hn_context_display_list(ctx);
    if (!dl) { fprintf(stderr, "probe: 无绘制指令\n"); return 1; }
    printf("cmds=%d\n", dl->count);

    unsigned char *px = hnsoft_render(dl, w, h, 0x00000000);
    if (!px) { fprintf(stderr, "probe: 渲染失败\n"); return 1; }
    long visible = 0;
    for (int i = 0; i < w * h; i++)
        if (px[i * 4 + 3] > 8) visible++;
    printf("canvas=%dx%d visible=%.1f%%\n", w, h, (double)visible * 100.0 / (double)(w * h));
    free(px);

    struct { int x, y; } pts[] = { { w / 2, h / 2 }, { 8, 8 }, { w - 8, 8 }, { 8, h - 8 }, { w - 8, h - 8 } };
    for (size_t i = 0; i < sizeof(pts) / sizeof(pts[0]); i++) {
        const char *id = hn_context_hit_test(ctx, (float)pts[i].x, (float)pts[i].y);
        hn_node *n = hn_context_hit_node(ctx, (float)pts[i].x, (float)pts[i].y);
        printf("hit(%d,%d) -> <%s id=%s>\n", pts[i].x, pts[i].y,
               n ? hn_node_tag(n) : "?", id ? id : "(无id)");
    }
    int idn = 0, idbad = hn_context_verify_hits(ctx, &idn);
    printf("id-hits=%d bad=%d\n", idn, idbad);
    if (idbad) return 1;
    printf("probe OK\n");
    return 0;
}

/* ---------------- main(镜像 hnwin.c:669-839) ---------------- */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "用法: hnweb_macos <file.html> [W H]          开窗口\n"
                        "      hnweb_macos <file.html> --probe [W H]  无窗口自检\n"
                        "      hnweb_macos <file.html> --shot <png> [W H]  渲染位图\n");
        return 2;
    }
    const char *path = argv[1];
    int probe = (argc > 2 && !strcmp(argv[2], "--probe"));
    int shot = (argc > 2 && !strcmp(argv[2], "--shot"));
    int W = 480, H = 700;
    if (!probe && !shot) {
        if (argc > 2) W = atoi(argv[2]);
        if (argc > 3) H = atoi(argv[3]);
    } else {
        int a = shot ? 4 : 3;
        if (argc > a) W = atoi(argv[a]);
        if (argc > a + 1) H = atoi(argv[a + 1]);
    }

    size_t hlen = 0;
    char *html = read_all(path, &hlen);
    if (!html) { fprintf(stderr, "hnweb_macos: 无法读取 %s\n", path); return 1; }

    /* 路径口径统一到 HTML 所在目录(资产/link/图片相对文档) */
    const char *base = path;
    const char *slash = strrchr(path, '/');
    if (slash) base = slash + 1;
    if (slash) {
        char dir[1024];
        size_t dl = (size_t)(slash - path + 1);
        if (dl < sizeof(dir)) {
            memcpy(dir, path, dl);
            dir[dl] = 0;
            if (chdir(dir) != 0) { /* 目录不可用时保持原 cwd */ }
        }
    }
    g_html_path = base;

    hn_doc *doc = hn_parse_html(html, hlen);
    if (!doc) { fprintf(stderr, "hnweb_macos: 解析失败\n"); free(html); return 1; }
    hn_doc_autoid_hx(doc);   /* hx 载体无 id 时自动编址(另两壳同口径) */
    hn_context *ctx = hn_context_create();
    hn_context_set_doc(ctx, doc);
    hn_context_set_assets(ctx, &g_assets);
    g_tb.ctx = NULL; g_tb.measure = tb_measure; g_tb.metrics = tb_metrics;

    /* 清单: 标题/尺寸/透明 */
    hn_manifest m;
    memset(&m, 0, sizeof(m));
    hn_doc_manifest(doc, &m);
    if (m.w > 0) W = m.w;
    if (m.h > 0) H = m.h;
    if (m.transparent == 1) g_app.transparent = 1;
    const char *title = m.title ? m.title : "hn";

    /* 外链样式内联(引擎不做 I/O) */
    size_t clen = 0;
    char *css = load_css(path, html, &clen);
    if (css && clen) hn_context_add_sheet(ctx, hn_parse_css(css, clen));

    g_app.ctx = ctx;
    g_app.w = W; g_app.h = H;

    if (probe) {
        int rc = probe_mode(ctx, W, H);
        hn_context_destroy(ctx);
        free(html); free(css);
        return rc;
    }
    if (shot) {
        int rc = shot_mode(ctx, argv[3], W, H);
        hn_context_destroy(ctx);
        free(html); free(css);
        return rc;
    }

    /* ---- 开窗口 + 事件泵 ---- */
    if (!objc_load()) {
        fprintf(stderr, "hnweb_macos: 无法装载 AppKit(无 GUI 环境?)\n");
        hn_context_destroy(ctx);
        free(html); free(css);
        return 1;
    }
    if (!surface_create(title)) {
        fprintf(stderr, "hnweb_macos: 窗口创建失败\n");
        hn_context_destroy(ctx);
        free(html); free(css);
        return 1;
    }
    if (app_render(W, H)) app_present();
    run_load_actions();

    obj_t app = nsapp();
    obj_t mode = nsstr("kCFRunLoopDefaultMode");
    obj_t NSDate = o_getClass("NSDate");
    double last = now_s();
    while (!g_quit) {
        /* 16ms 节拍: 与 hnwin 的 SetTimer 同频; 无事时 select 语义上等效
           (nextEvent...untilDate 会阻塞到超时, 不忙等)。 */
        obj_t lim = ((obj_t (*)(obj_t, sel_t, double))o_msgSend)(
            NSDate, o_sel("dateWithTimeIntervalSinceNow:"), 0.016);
        obj_t ev = ((obj_t (*)(obj_t, sel_t, unsigned long, obj_t, obj_t, int))o_msgSend)(
            app, o_sel("nextEventMatchingMask:untilDate:inMode:dequeue:"), ~0UL, lim, mode, 1);
        if (ev) {
            unsigned long ty = MSG0(unsigned long, ev, "type");
            if (ty == EV_LEFTDOWN) {
                CGPoint p = ev_point(ev);
                on_click((int)p.x, (int)((double)g_app.h - p.y));
            } else if (ty == EV_MOUSEMOVED || ty == EV_LEFTDRAG) {
                CGPoint p = ev_point(ev);
                on_mouse_move((float)p.x, (float)p.y);
            } else if (ty == EV_LEFTUP) {
                /* 点击已在 down 处理(与两外两壳同口径) */
            } else if (ty == EV_FLAGS) {
                /* 修饰键变化: M1 忽略(按键事件自带 flags) */
            } else if (ty == EV_SCROLL) {
                CGPoint p = ev_point(ev);
                on_scroll(ev, (float)p.x, (float)p.y);
            } else if (ty == EV_KEYDOWN) {
                on_key(ev, 1);
            } else if (ty == EV_KEYUP) {
                on_key(ev, 0);
            }
            /* 只把我们**没消费**的用户输入交回 AppKit(窗口移动/关闭/重绘)。
               系统定义事件(NSEventTypeSystemDefined=14 / AppKitDefined=15 /
               KitDefined=16)不能 sendEvent: —— 实测会抛
               NSInternalInconsistencyException 直接崩掉宿主
               ("Invalid message sent to event ... type=KitDefined")。
               这类事件是 AppKit 自己产生的内部消息, 泵出来跳过即可。 */
            /* AppKitDefined/SystemDefined/ApplicationDefined/Periodic
               (13..16)是 AppKit 内部消息; 对它们 sendEvent: 会抛
               NSInternalInconsistencyException —— 跳过。 */
            if (ty < EV_APPKITDEF || ty > EV_PERIODIC) MSG1(void, app, "sendEvent:", ev);
        }

        /* 用户拖边界 → 重排 */
        sync_size();

        /* 帧循环: 有动画在跑就重渲染(anim_tick 返回是否需要下一帧) */
        double now = now_s();
        float dt = (float)((now - last) * 1000.0);
        last = now;
        if (hn_context_anim_tick(ctx, dt)) {
            if (app_render(g_app.w, g_app.h)) app_present();
        } else if (g_app.dirty) {
            g_app.dirty = 0;
            if (app_render(g_app.w, g_app.h)) app_present();
        }
        poll_tick();

        /* 窗口被用户关闭(红叉/⌘W) */
        if (!MSG0(int, g_app.win, "isVisible")) g_quit = 1;
    }

    surface_destroy();
    hn_context_destroy(ctx);
    free(html);
    free(css);
    return 0;
}

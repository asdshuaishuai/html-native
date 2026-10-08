/* hnweb_win.c — Windows 运行时(门面实现): Win32 窗口 + hnsoft 软件光栅
 *
 * 定位: tools/hnwin.c 的收编 —— 引擎(C99, 平台无关)之上的第一层 Windows
 * 表面物化, 也是 tools/hnweb.h 门面的平台实现(与 tools/hnweb_linux.c 同一
 * 门面 API, hnweb.h 的调用方跨平台一行不改, 换的只是实现文件):
 *   解析 → 级联 → 布局 → display list → hnsoft RGBA 位图 → DIB/UpdateLayeredWindow 上屏
 * 窗口/事件/像素搬运这一层(Win32 消息泵 ↔ X11 事件循环)与引擎管线保持
 * 同构, 因此 hnwin.c 的主体原样搬入, 只把全局单例 g_app 折进 hnweb_win
 * 结构(hnwin 不支持多窗; 门面口径允许多窗, 与 hnweb_linux.c 同一手法定岑)。
 *
 * 零编译期外部依赖: Win32 是系统自带 ABI, 直接链接(build-multiplatform.sh
 * 的 WIN_LIBS: user32/gdi32), 不需要 Linux 侧的 dlopen 手抄。--probe /
 * --shot 不创建任何窗口, 无头(CI/wine)环境照常工作。
 *
 * 路径口径: 启动即 chdir 到 HTML 所在目录 —— 资产/图片/link 全部相对文档
 * 目录解析(与浏览器一致; hnwin.c:688-705 同一口径)。
 *
 * 用法:
 *   hnweb_win <file.html> [W H]               开窗口
 *   hnweb_win <file.html> --probe [W H]       无窗口自检(管线统计 + 采样命中), 供 CI
 *   hnweb_win <file.html> --shot <png> [W H]  渲染位图落盘
 *
 * 已知取舍(M1, 继承 hnwin 的 Windows 口径):
 *   - 文本: -DHN_NO_TEXT 交叉口径不链 FreeType, 位图无字形(几何仍正确);
 *     部署平台链 FreeType 去掉宏即开启
 *   - DPI: CSS px = 物理 px(hnweb.h:1 / hnwin.c:20 同一口径), 高分屏未缩放
 *   - 透明窗口: WS_EX_LAYERED + UpdateLayeredWindow 预乘 alpha(hnwin.c:242-269
 *     的预乘路径原样保留); 窗口仍带边框 —— hnweb.h:30 的"无边框"措辞未实现
 *     (与 hnwin 同一 M1 取舍, Linux 门面同样带 WM 装饰)
 *   - hx 传输: sys:// 系统桥真身在 tools/sysbridge.c, 但本门面二进制的源集
 *     (与 hnweb-linux 目标同组)不含它 —— 管道与 hnwin 同构, 片段请求不可得
 *     (stderr 一行, 与 hnweb_linux.c:753-758 同一口径); 接桥 = 换一个函数
 *   - hn-renderer=webkit 的 WebView2 兜底属于 tools/hnwebview.c 那套 CLI
 *     运行时; 门面只打 stderr 提醒并继续引擎渲染(绝不静默切换)
 *   - Esc 关窗(对齐 Linux 门面 hnweb_linux.c:1089-1090; hnwin 本体无此键)
 *   - 单线程使用(引擎非线程安全, 与 hnwin 相同)
 */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h> /* chdir —— 与 hnwin.c:31 同一 include(mingw 映射 CRT) */

#include "hn.h"
/* ---- hnweb.h 门面声明(必须先于 HNWEB_RENDER 缺省定义) ----
 * 门面头(tools/hnweb.h)自带绘制后端切换点: -DHN_USE_CAIRO 时它把
 * HNWEB_RENDER 定义为 hncairo_render。若先在这里定义缺省宏再包含头文件,
 * 头里的 #ifndef 会看到"已定义"而直接让路 —— cairo 切换被静默击败。
 * 所以顺序: 先门面, 后缺省(与 hnweb_linux.c:53-72 相同)。 */
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

/* UTF-8 → UTF-16。Win32 宽字符 API 要求 UTF-16; 走 ANSI 版会把 UTF-8
   字节按本地代码页(中文 Windows = GBK)解读, 中文标题即乱码。
   (与 hnwin.c:48-55 逐字一致。) */
static wchar_t *utf8_to_w(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

static char *str_copy(const char *s) {
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
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
    asset_tab[asset_n].path = str_copy(path);
    if (!asset_tab[asset_n].path) { free(d); return NULL; } /* 复制失败则下次重复读盘, 不留 NULL 键 */
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
            acc[len] = 0;                                                   \
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

/* 轮询(hx-trigger="every Ns"): 每 tick 检查到期(hnwin.c:376-378 同构) */
typedef struct { char id[64]; int ms; DWORD last; } poll_ent;

/* 应用级事件队列容量(点击/滚动/按键经 hnweb_poll 交还) */
#define HNWEB_EVQ_CAP 64

/* hnwin 的 app_t 是全局单例(g_app); 门面允许多窗, 状态全部进 win 结构,
   只把进程级的窗口类注册与文本后端留作全局。 */
struct hnweb_win {
    hn_context *ctx;
    int w, h;            /* 客户区尺寸(CSS px; AdjustWindowRect 已折掉装饰) */
    int transparent;     /* 实际生效(WS_EX_LAYERED + 预乘上屏) */
    int resizable;
    int closed;          /* 关闭钮 / Esc / WM_DESTROY 置位, hnweb_run 据此返回 */
    int dirty;           /* 内容已渲染待上屏(镜像 hnwin 的 g_dirty) */
    int snapped;         /* HNWEB_SHOT 首帧抓屏只做一次 */
    HWND hwnd;
    HBITMAP dib;         /* 不透明窗口的常驻 DIB section */
    unsigned char *dib_bits;
    unsigned char *px;   /* hnsoft 输出的 RGBA 位图 */
    hn_node *hover;      /* 当前 hover 元素(避免每像素重排) */
    hn_node *focus;      /* 当前焦点 input(回车提交用) */
    /* 应用级事件队列(点击/滚动/按键经 hnweb_poll 交还) */
    hn_event_kind evq_kind[HNWEB_EVQ_CAP];
    float evq_x[HNWEB_EVQ_CAP], evq_y[HNWEB_EVQ_CAP];
    char evq_id[HNWEB_EVQ_CAP][64];
    hn_node *evq_target[HNWEB_EVQ_CAP];
    int evq_key[HNWEB_EVQ_CAP];
    int evq_head, evq_len;
    char last_id[64];    /* hnweb_poll 交出的 target_id 落点(两次 poll 之间有效) */
    poll_ent polls[16];
    int poll_n;          /* -1 = 未初始化 */
    char *html, *css;    /* 交给引擎的文档源(arena 可能引用, 存活期 = 窗口) */
    char *store_id;
};

/* 布局期文本后端(镜像 hnwin.c:196-209): 不注入则引擎走等宽估算,
   CJK 偏宽 65%。测量随绘制后端同源切换(HN_USE_CAIRO → hncairo_measure/
   metrics; 测量与绘制不同源时字形会落出行盒)。 */
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

/* RGBA(非预乘, hnsoft 输出) → BGRA, 供 DIB / UpdateLayeredWindow 使用。
   premul=1 时输出预乘 alpha(透明窗口上屏要求)。与 hnwin.c:223-233 逐字一致。 */
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

/* ---- 渲染 + 上屏(hnwin.c:212-275 的同构) ---- */

/* 布局 + 软件光栅。宽高变化或内容变化后调用。 */
static int app_render(hnweb_win *w) {
    hn_context_layout(w->ctx, (float)w->w, (float)w->h, &g_tb);
    const hn_display_list *dl = hn_context_display_list(w->ctx);
    if (!dl) return 0;
    free(w->px);
    w->px = HNWEB_RENDER(dl, w->w, w->h, 0x00000000); /* 透明底色; 透明窗口另由 strip_root_background 剥 body */
    return w->px != NULL;
}

/* 按 w/h 重建背板缓冲(等价 DeleteObject+CreateDIBSection, hnwin.c:440-451)。
   透明窗口无常驻 DIB: UpdateLayeredWindow 每次上屏现建临时位图。 */
static int dib_rebuild(hnweb_win *w) {
    if (!w->hwnd) return 0;
    if (w->transparent) return 1;
    if (w->dib) { DeleteObject(w->dib); w->dib = NULL; w->dib_bits = NULL; }
    if (w->w <= 0 || w->h <= 0) return 0;
    HDC hdc = GetDC(w->hwnd);
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w->w;
    bi.bmiHeader.biHeight = -w->h; /* 自上而下 */
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    w->dib = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, (void **)&w->dib_bits, NULL, 0);
    ReleaseDC(w->hwnd, hdc);
    if (!w->dib) { fprintf(stderr, "hnweb_win: CreateDIBSection 失败\n"); return 0; }
    return 1;
}

/* 把引擎位图送进窗口: 透明走 UpdateLayeredWindow(预乘), 不透明走常驻
   DIB + InvalidateRect(WM_PAINT 里 BitBlt)。hwnd 必须有效 —— hnwin.c:236-241
   同一论证; 门面的 hx load/poll 路径刻意只标脏, 由 frame_step 统一上屏。 */
static void present(hnweb_win *w) {
    if (!w->px || !w->hwnd) return;
    if (w->transparent) {
        /* 全窗口逐像素更新: 构造 32bpp DIB(临时)喂给 UpdateLayeredWindow
           (hnwin.c:244-269 原样) */
        HDC hdc = GetDC(w->hwnd);
        BITMAPINFO bi;
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w->w;
        bi.bmiHeader.biHeight = -w->h; /* 自上而下 */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void *bits = NULL;
        HBITMAP bmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (bmp && bits) {
            rgba_to_bgra((unsigned char *)bits, w->px, w->w * w->h, 1); /* 预乘 */
            HDC mem = CreateCompatibleDC(hdc);
            HGDIOBJ old = SelectObject(mem, bmp);
            POINT pt_src = { 0, 0 };
            SIZE sz = { w->w, w->h };
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            UpdateLayeredWindow(w->hwnd, hdc, NULL, &sz, mem, &pt_src, 0, &bf, ULW_ALPHA);
            SelectObject(mem, old);
            DeleteDC(mem);
        }
        if (bmp) DeleteObject(bmp);
        ReleaseDC(w->hwnd, hdc);
    } else {
        if (w->dib_bits)
            rgba_to_bgra(w->dib_bits, w->px, w->w * w->h, 0);
        InvalidateRect(w->hwnd, NULL, FALSE);
    }
}

/* ---- sys:// 系统桥(与 hnweb_linux.c:748-758 同一口径) ----
   Windows 真身在 tools/sysbridge.c(进程内应答), 但本门面二进制的源集
   (与 hnweb-linux 目标同组)不链它。hx-* 管道与 hnwin 保持同构, 片段请求
   不可得按 hnwin 的 !frag 分支返回 0 —— 点击/回车轨迹照常打印; 接桥 =
   把本函数换成 sysbridge.c 的 sys_fragment 并加一个源文件。 */
static char *sys_fragment(const char *url, const char *form_body, const char *store_id) {
    (void)form_body;
    (void)store_id;
    fprintf(stderr, "hnweb_win: hx 请求 %s 无传输(sys:// 桥未链入, 见 tools/sysbridge.c)\n", url);
    return NULL;
}

/* ---- hx-* 执行(htmx 语义, 镜像 hnwin.c:299-351) ----
   transport 进程内应答(sys://)由上面的 sys_fragment 槽位提供。 */

/* 找 hx 载体: 自身带 hx-get/post, 否则向上冒泡 */
static hn_node *hx_carrier(hn_node *n) {
    if (!n) return NULL;
    if (hn_node_attr(n, "hx-get") || hn_node_attr(n, "hx-post")) return n;
    hn_node *a = hn_node_ancestor_with_attr(n, "hx-get");
    hn_node *b = hn_node_ancestor_with_attr(n, "hx-post");
    return a ? a : b;
}

/* 对一个载体执行 sys:// 请求并换入目标元素; 返回 1 表示发生了 swap。
   direct=1(点击/回车)立即上屏; direct=0(load/poll 路径)先标脏,
   由帧循环统一上屏 —— 否则 DOM 更新了但画面不动(hnwin.c:344-348 语义)。 */
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

/* GetTickCount 口径的轮询计时(hnwin.c:380-401 同构; 49.7 天回绕是同一取舍) */
static void poll_tick(hnweb_win *w) {
    if (w->poll_n < 0) {
        w->poll_n = 0;
        hn_doc *doc = hn_context_doc(w->ctx);
        for (int i = 0; i < 16; i++) {
            const char *id = NULL; int ms = 0;
            if (!hn_doc_poll_at(doc, i, &id, &ms)) break;
            snprintf(w->polls[w->poll_n].id, 64, "%s", id);
            w->polls[w->poll_n].ms = ms;
            w->polls[w->poll_n].last = GetTickCount();
            w->poll_n++;
        }
    }
    DWORD now = GetTickCount();
    for (int i = 0; i < w->poll_n; i++) {
        if (now - w->polls[i].last >= (DWORD)w->polls[i].ms) {
            w->polls[i].last = now;
            hn_node *n = hn_doc_find_by_id(hn_context_doc(w->ctx), w->polls[i].id);
            if (n) hx_perform(w, n, 0);
        }
    }
}

/* ---- 事件队列(hnweb_poll 的供给端) ---- */

static void evq_push(hnweb_win *w, hn_event_kind kind, float x, float y,
                     const char *id, hn_node *target, int key) {
    if (w->evq_len >= HNWEB_EVQ_CAP) {
        static int warned = 0;
        if (!warned) { warned = 1; fprintf(stderr, "hnweb_win: 事件队列满, 丢弃后续事件\n"); }
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
    w->evq_len++;
}

/* PNG 落盘: hnweb_shot 与 HNWEB_SHOT 首帧抓屏共用 */
static int write_png(hnweb_win *w, const char *path) {
    if (!w->px) return 0;
    size_t pn = 0;
    unsigned char *png = hnsoft_encode_png(w->px, w->w, w->h, &pn);
    if (!png) { fprintf(stderr, "hnweb_win: PNG 编码失败\n"); return 0; }
    FILE *of = fopen(path, "wb");
    if (!of) { fprintf(stderr, "hnweb_win: 无法写 %s\n", path); free(png); return 0; }
    fwrite(png, 1, pn, of);
    fclose(of);
    free(png);
    return 1;
}

/* 点击: 命中测试回传最深元素 id + 焦点 + hx 派发(hnwin.c:404-422 同构)。
   命中产生的事件进队列, 由 hnweb_poll 交还。 */
static void on_click(hnweb_win *w, float x, float y) {
    const char *id = hn_context_hit_test(w->ctx, x, y);
    hn_node *n = hn_context_hit_node(w->ctx, x, y);
    const char *tag = n ? hn_node_tag(n) : NULL;
    printf("[click] (%d,%d) -> <%s id=%s>\n", (int)x, (int)y,
           tag ? tag : "?", id ? id : "(无id)");
    /* input 焦点(回车提交用) */
    w->focus = n ? hn_node_ancestor_input(n) : NULL;
    if (w->focus) hn_context_set_focus(w->ctx, w->focus);
    /* 点击坐标回传: x/y 给几何, id 给语义, 二者都来自引擎命中测试 */
    evq_push(w, HN_EV_CLICK, x, y, id, n, HN_KEY_NONE);
    /* hx 派发: 自身或祖先带 hx-get/post 即执行 */
    hn_node *carrier = hx_carrier(n);
    if (carrier) {
        const char *url = hn_node_attr(carrier, "hx-post") ? hn_node_attr(carrier, "hx-post")
                                                          : hn_node_attr(carrier, "hx-get");
        printf("[hx] %s\n", url ? url : "?");
        hx_perform(w, carrier, 1);
    }
    fflush(stdout);
}

/* Win32 虚拟键 → HN_KEY_*(镜像 hnweb_linux.c:916-934 的键符号表) */
static int vk_to_hn(WPARAM vk) {
    switch (vk) {
    case VK_RETURN:  return HN_KEY_ENTER;
    case VK_ESCAPE:  return HN_KEY_ESC;
    case VK_TAB:     return HN_KEY_TAB;
    case VK_BACK:    return HN_KEY_BACKSPACE;
    case VK_DELETE:  return HN_KEY_DELETE;
    case VK_LEFT:    return HN_KEY_LEFT;
    case VK_RIGHT:   return HN_KEY_RIGHT;
    case VK_UP:      return HN_KEY_UP;
    case VK_DOWN:    return HN_KEY_DOWN;
    case VK_HOME:    return HN_KEY_HOME;
    case VK_END:     return HN_KEY_END;
    case VK_PRIOR:   return HN_KEY_PAGEUP;
    case VK_NEXT:    return HN_KEY_PAGEDOWN;
    case VK_SPACE:   return HN_KEY_SPACE;
    default:         return HN_KEY_NONE;
    }
}

/* hn-renderer 声明探测(hnwin.c:279-297 的扫描原样): 门面不带 WebView2
   兜底, 但绝不静默切换 —— 文档显式声明 webkit 时打一行 stderr 说明实际
   用的是引擎渲染。 */
static int declares_webkit(const char *html) {
    for (const char *p = html; (p = strstr(p, "hn-renderer")) != NULL; p++) {
        const char *tag_end = strchr(p, '>');
        if (!tag_end) break;
        const char *c = strstr(p, "content");
        if (c && c < tag_end) {
            const char *q = strchr(c, '"');
            if (!q) q = strchr(c, '\'');
            if (q && q < tag_end) {
                char qc = *q++;
                const char *e2 = strchr(q, qc);
                if (e2 && e2 <= tag_end && (size_t)(e2 - q) == 6 &&
                    !_strnicmp(q, "webkit", 6))
                    return 1;
            }
        }
    }
    return 0;
}

/* ---- 窗口表面 ---- */

/* 光标: 每次 mouse move 都设(hnwin.c:487 逐次 SetCursor 同款; 系统会在
   WM_SETCURSOR 里重置为类光标, 移动中覆盖即可, 股票光标 LoadCursor 近零开销)。 */
static void cursor_apply(hnweb_win *w, int want_hand) {
    (void)w;
    SetCursor(LoadCursor(NULL, want_hand ? IDC_HAND : IDC_ARROW));
}

/* 建窗(不显示)+ 注册进程级窗口类。首帧渲染在 ShowWindow 之前完成,
   避免 hnwin "先 Show 后画" 的黑闪。返回 1 = 可渲染上屏。 */
static LRESULT CALLBACK win_proc_dispatch(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static int wndclass_register(void);
static int surface_create(hnweb_win *w, const char *title) {
    if (!wndclass_register()) return 0;
    /* 标题: manifest 里是 UTF-8, 转 UTF-16 后进宽字符 API(hnwin.c:778-780) */
    wchar_t *title_w = utf8_to_w(title && title[0] ? title : "hnweb");
    if (!title_w) title_w = utf8_to_w("hnweb");
    DWORD style = WS_OVERLAPPEDWINDOW;
    if (!w->resizable)
        style &= ~(DWORD)(WS_THICKFRAME | WS_MAXIMIZEBOX); /* 固定尺寸表面 */
    DWORD exstyle = w->transparent ? WS_EX_LAYERED : 0;
    /* client 区正好 w×h(装饰由 AdjustWindowRect 折算, hnwin.c:774-786) */
    RECT r = { 0, 0, w->w, w->h };
    AdjustWindowRect(&r, style, FALSE);
    HWND hwnd = CreateWindowExW(exstyle, L"HnWeb", title_w, style,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top,
                                NULL, NULL, GetModuleHandle(NULL), w);
    free(title_w);
    if (!hwnd) { fprintf(stderr, "hnweb_win: CreateWindowEx 失败\n"); return 0; }
    w->hwnd = hwnd;
    return 1;
}

static int wndclass_register(void) {
    static int done = 0;
    if (done) return 1;
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = win_proc_dispatch;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH); /* hnwin.c:769 同底色 */
    wc.lpszClassName = L"HnWeb";
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    if (!RegisterClassExW(&wc)) {
        fprintf(stderr, "hnweb_win: RegisterClassEx 失败\n");
        return 0;
    }
    done = 1;
    return 1;
}

/* 尺寸变了才动作 → 重 layout → 重建背板 → 重渲染上屏(WM_SIZE 语义,
   hnwin.c:434-455)。返回 1 表示实际发生了变化。 */
static int win_resize(hnweb_win *w, int nw, int nh) {
    if (nw <= 0 || nh <= 0) return 0;
    if (nw == w->w && nh == w->h) return 0;
    w->w = nw;
    w->h = nh;
    if (app_render(w) && dib_rebuild(w)) {
        present(w);
        return 1;
    }
    return 0;
}

/* 帧步进(WM_TIMER 分支 hnwin.c:518-536 的等价物): 脏上屏 + HNWEB_SHOT
   首帧抓屏 + hx 轮询 + 动画推进。返回 1 = 本次有上屏。 */
static int frame_step(hnweb_win *w, float dt_ms) {
    int presented = 0;
    /* load/poll 路径没有直接上屏, 只标了脏; 在这里统一上屏 */
    if (w->dirty) {
        w->dirty = 0;
        present(w);
        presented = 1;
    }
    /* 首帧完成后把上屏像素落一次盘(HNWEB_SHOT=<png>; 对齐 hnweb_linux 的
       同名出口, 取代 hnwin 的 HN_WIN_SNAP —— 抓的是引擎位图本身, 编码路径
       与 hnweb_shot 同源)。 */
    if (!w->snapped) {
        w->snapped = 1;
        const char *snap = getenv("HNWEB_SHOT");
        if (snap && *snap) write_png(w, snap);
    }
    /* hx 轮询(every Ns) */
    poll_tick(w);
    /* 过渡动画推进; 返回 1 表示仍在跑 —— 继续帧循环 */
    if (hn_context_anim_tick(w->ctx, dt_ms)) {
        if (app_render(w)) {
            present(w);
            presented = 1;
        }
    }
    return presented;
}

/* ---- Win32 消息 → 门面调用(等价 hnwin 的 wnd_proc 427-558) ---- */

static LRESULT CALLBACK win_proc_dispatch(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_CREATE) {
        /* lpCreateParams 携带 hnweb_win*(CreateWindowExW 的最后实参);
           先挂句柄表再起帧循环, 后续消息都能找回自己的状态 */
        SetWindowLongPtrW(hwnd, GWLP_USERDATA,
                          (LONG_PTR)((const CREATESTRUCTW *)lp)->lpCreateParams);
        SetTimer(hwnd, 1, 16, NULL); /* 动画帧循环: 16ms tick(hnwin.c:429-433) */
        return 0;
    }
    hnweb_win *w = (hnweb_win *)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (!w) return DefWindowProcW(hwnd, msg, wp, lp);
    switch (msg) {
    case WM_SIZE: {
        int cw = LOWORD(lp), ch = HIWORD(lp);
        /* 建窗期的等尺寸 WM_SIZE 由 hnweb_resize 的同尺寸守卫挡成 no-op */
        if (cw > 0 && ch > 0) hnweb_resize(w, cw, ch);
        return 0;
    }
    case WM_PAINT: {
        /* 透明窗口由 UpdateLayeredWindow 独占上屏, 系统不再发有效 WM_PAINT */
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (!w->transparent && w->dib && w->dib_bits) {
            HDC mem = CreateCompatibleDC(hdc);
            HGDIOBJ old = SelectObject(mem, w->dib);
            BitBlt(hdc, 0, 0, w->w, w->h, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        /* 必须请求 WM_MOUSELEAVE: 不调用 TrackMouseEvent 的话, 光标移出窗口
           后 hover 状态不会被清除(hnwin.c:473-481; macOS 侧用 NSTrackingArea) */
        TRACKMOUSEEVENT tme;
        memset(&tme, 0, sizeof(tme));
        tme.cbSize = sizeof(tme);
        tme.dwFlags = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        hnweb_mouse_move(w, (float)LOWORD(lp), (float)HIWORD(lp));
        return 0;
    }
    case WM_MOUSELEAVE:
        /* 光标离开窗口: 清 hover, 否则高亮不灭(镜像 hnweb_linux 的 LeaveNotify) */
        hnweb_mouse_leave(w);
        return 0;
    case WM_LBUTTONDOWN:
        hnweb_mouse_down(w, (float)LOWORD(lp), (float)HIWORD(lp), 1);
        return 0;
    case WM_MOUSEWHEEL: {
        /* lp 是**屏幕**坐标: 多屏布局下主屏左侧/上方会出现负分量,
           hnwin.c:503 的 LOWORD 截断会把负坐标变 65535(潜在缺陷), 这里
           先做符号扩展再转客户区 */
        POINT pt = { (short)LOWORD(lp), (short)HIWORD(lp) };
        ScreenToClient(hwnd, &pt);
        /* 一格 = 48px(hnwin.c:508 的整数算式原样) */
        float dy = (float)(-(short)HIWORD(wp) / WHEEL_DELTA * 48);
        hnweb_scroll(w, (float)pt.x, (float)pt.y, dy);
        return 0;
    }
    case WM_TIMER: {
        if (wp == 1) frame_step(w, 16.0f);
        return 0;
    }
    case WM_KEYDOWN: {
        int key = vk_to_hn(wp);
        hnweb_key(w, key, NULL); /* M1 不做键入编辑, utf8 位保留(与 Linux 门面一致) */
        if (key == HN_KEY_ESC)
            DestroyWindow(hwnd); /* M1 口径: Esc 退出主循环(镜像 hnweb_linux.c:1089-1090) */
        return 0;
    }
    case WM_DESTROY:
        KillTimer(hwnd, 1);
        w->closed = 1;
        w->hwnd = NULL; /* 系统侧已销毁, hnweb_close 不再 DestroyWindow */
        /* 不 PostQuitMessage: hnwin.c:552-555 是进程级单窗循环; 门面循环以
           本窗 closed 收束(hnweb_run), 多窗互不牵连, 也不会把残留 WM_QUIT
           留给下一个循环误吞 */
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

/* ================= 门面实现 ================= */

/* 开窗口并装载文档: 解析 → 清单 → context 组装 → 建窗 → 首帧
   (相当于 HNEngine.open 首开分支 + hnwin main 的 669-814 段)。
   html/css 内部复制, 调用方随后可 free。 */
int hnweb_open(const hnweb_opts *opts, const char *html, size_t html_len,
               const char *css, size_t css_len, hnweb_win **out) {
    if (out) *out = NULL;
    if (!out || !opts || !html || html_len == 0) {
        fprintf(stderr, "hnweb_win: hnweb_open 参数缺失\n");
        return 0;
    }

    tb_setup();
    hnweb_win *w = (hnweb_win *)calloc(1, sizeof(hnweb_win));
    if (!w) { fprintf(stderr, "hnweb_win: 内存不足\n"); return 0; }
    w->poll_n = -1;
    w->resizable = opts->resizable;
    w->store_id = str_copy(opts->store_id && opts->store_id[0] ? opts->store_id : "default");
    w->html = (char *)malloc(html_len + 1);
    w->css = (css && css_len) ? (char *)malloc(css_len + 1) : NULL;
    if (!w->store_id || !w->html || (css && css_len && !w->css)) {
        fprintf(stderr, "hnweb_win: 内存不足\n");
        hnweb_close(w);
        return 0;
    }
    memcpy(w->html, html, html_len);
    w->html[html_len] = 0;
    if (css && css_len) {
        memcpy(w->css, css, css_len);
        w->css[css_len] = 0;
    }
    if (declares_webkit(w->html))
        fprintf(stderr, "hnweb_win: 文档声明 hn-renderer=webkit; 门面无 WebView2 兜底, 继续引擎渲染\n");

    /* 解析 + context 组装(镜像 hnwin.c:706-719; "谁 set 谁放手", doc/sheet
       归 context, 窗口销毁时一并释放 —— hn.h:235-239) */
    hn_doc *doc = hn_parse_html(w->html, html_len);
    if (!doc) { fprintf(stderr, "hnweb_win: 解析失败\n"); hnweb_close(w); return 0; }
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
    w->ctx = ctx;

    /* 清单: 标题/尺寸/透明由 hn 编码声明, 引擎解析; opts 显式值优先 */
    hn_manifest man;
    memset(&man, 0, sizeof man);
    hn_doc_manifest(doc, &man);
    w->w = opts->w > 0 ? (int)opts->w : (man.w > 0 ? (int)man.w : 480);
    w->h = opts->h > 0 ? (int)opts->h : (man.h > 0 ? (int)man.h : 700);
    w->transparent = opts->transparent || man.transparent;
    if (w->transparent) hn_context_strip_root_background(ctx);
    const char *title = opts->title && opts->title[0] ? opts->title
                        : (man.title && man.title[0] ? man.title : "hnweb");

    /* 建窗 + 首帧双动作(镜像 hnwin.c:797-814 native 分支; 本门面不挂
       webview 兜底 —— 系统级 webview 壳是 hnwebview.c 那套 CLI 运行时) */
    if (!surface_create(w, title)) { hnweb_close(w); return 0; }
    if (!app_render(w) || !dib_rebuild(w)) {
        fprintf(stderr, "hnweb_win: 首帧渲染失败\n");
        hnweb_close(w);
        return 0;
    }
    present(w);
    ShowWindow(w->hwnd, SW_SHOW);
    UpdateWindow(w->hwnd);
    run_load_actions(w); /* hx-trigger="load" 立即执行一次(脏标记由首帧循环上屏) */

    *out = w;
    return 1;
}

/* 热更新: 整文档换入、窗口与焦点保持(HNEngine.swift:308-320 语义,
   引擎侧走 hn_context_render)。上屏由 hnweb_frame 完成。 */
int hnweb_update(hnweb_win *w, const char *html, size_t len) {
    if (!w || !w->ctx || !html || len == 0) return 0;
    hn_context_render(w->ctx, html, len);
    /* 旧 DOM 的节点指针全部失效: hover/焦点一并清除 */
    w->hover = NULL;
    w->focus = NULL;
    hn_context_set_hover(w->ctx, NULL);
    hn_context_set_focus(w->ctx, NULL);
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

/* 喂 :hover + cursor(hn_context_set_hover / hn_node_cursor) */
void hnweb_mouse_move(hnweb_win *w, float x, float y) {
    if (!w || !w->ctx) return;
    hn_node *n = hn_context_hit_node(w->ctx, x, y);
    /* hover 变化才重渲染, 否则每像素移动都重排一遍 */
    if (n != w->hover) {
        w->hover = n;
        hn_context_set_hover(w->ctx, n); /* 触发 :hover 重新匹配 */
        if (app_render(w)) present(w);
    }
    cursor_apply(w, n && hn_node_cursor(n) == 1);
}

/* 清 hover(TrackMouseEvent(TME_LEAVE) ↔ hnwin.c:490-497 WM_MOUSELEAVE) */
void hnweb_mouse_leave(hnweb_win *w) {
    if (!w || !w->ctx) return;
    if (w->hover) {
        w->hover = NULL;
        hn_context_set_hover(w->ctx, NULL);
        if (app_render(w)) present(w);
    }
}

/* 命中测试 + 焦点 + hx 派发(hnwin.c on_click); 事件经 hnweb_poll 交还 */
void hnweb_mouse_down(hnweb_win *w, float x, float y, int button) {
    if (!w || !w->ctx) return;
    if (button != 1) return; /* M1: 只处理左键(镜像 WM_LBUTTONDOWN) */
    on_click(w, x, y);
}

/* delta_y 一格 = 48px(hnwin.c:508)。内部 scrollable_at/scroll_by, 位置
   变了才 repaint(不重排)—— 与 hnwin WM_MOUSEWHEEL 同一口径。 */
void hnweb_scroll(hnweb_win *w, float x, float y, float delta_y) {
    if (!w || !w->ctx) return;
    hn_node *s = hn_context_scrollable_at(w->ctx, x, y);
    if (!s) return;
    /* 事件语义: target = 滚动容器本身, id = 它的 id(无则空) */
    evq_push(w, HN_EV_SCROLL, x, y, hn_node_attr(s, "id"), s, HN_KEY_NONE);
    if (hn_node_scroll_by(s, 0, delta_y)) {
        hn_context_repaint(w->ctx);
        free(w->px);
        w->px = HNWEB_RENDER(hn_context_display_list(w->ctx), w->w, w->h, 0x00000000);
        present(w);
    }
}

/* key 用 hn.h 的 HN_KEY_*; Enter → 表单载体提交(hnwin.c:538-550 语义) */
void hnweb_key(hnweb_win *w, int key, const char *utf8) {
    if (!w || !w->ctx) return;
    (void)utf8; /* M1 不做键入编辑(与 hnwin 一致), 仅保留参数位 */
    evq_push(w, HN_EV_KEYDOWN, 0, 0, NULL, w->focus, key);
    if (key == HN_KEY_ENTER && w->focus) {
        /* 回车提交: input 上按 Enter → 自身 → 祖先 → 最近容器第一个
           hx-post/get 载体(与 native 引擎语义一致) */
        hn_node *carrier = hn_node_form_carrier(w->focus);
        if (carrier) {
            const char *url = hn_node_attr(carrier, "hx-post") ? hn_node_attr(carrier, "hx-post")
                                                              : hn_node_attr(carrier, "hx-get");
            printf("[hx-enter] %s\n", url ? url : "?");
            hx_perform(w, carrier, 1);
            fflush(stdout);
        }
    }
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

/* 委托式主循环: GetMessage 阻塞等事件 + 16ms WM_TIMER 帧步进, 窗口关闭
   (关闭钮 / Esc / hnweb_close)即返回 —— hnwin.c:828-832 的 GetMessage
   循环同款。区别于 hnwin 的唯一一点: 不依赖 PostQuitMessage, 循环条件是
   本窗 closed, 多窗的 run 互不牵连。要与其它事件源共存就别用这个,
   改 hnweb_poll + hnweb_frame。 */
void hnweb_run(hnweb_win *w) {
    if (!w || !w->hwnd) return;
    MSG msg;
    while (!w->closed) {
        int r = (int)GetMessage(&msg, NULL, 0, 0);
        if (r <= 0) break; /* WM_QUIT / 错误: 外部的循环退出请求 */
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
}

/* 销毁窗口与引擎上下文。所有权对齐 hn.h:235-239 "谁 set 谁放手":
   交给 context 的 doc/sheet 随 hn_context_destroy 释放; html/css 源、
   位图与 GDI 背板归窗口, 也在这里释放。 */
void hnweb_close(hnweb_win *w) {
    if (!w) return;
    if (w->hwnd) {
        DestroyWindow(w->hwnd); /* 同步触发 WM_DESTROY: 清定时器/置 closed */
        w->hwnd = NULL;
    }
    if (w->dib) { DeleteObject(w->dib); w->dib = NULL; }
    w->dib_bits = NULL;
    if (w->ctx) {
        hn_context_destroy(w->ctx);
        w->ctx = NULL;
    }
    free(w->px);
    free(w->html);
    free(w->css);
    free(w->store_id);
    free(w);
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
   这两个路径不创建任何窗口 —— 无头(CI/wine)环境也能跑。 */

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

/* ---------------- main(镜像 hnweb_linux.c:1484-1609) ---------------- */

int main(int argc, char **argv) {
    /* console 输出 UTF-8(命中 id 可能是中文), 否则终端按本地代码页乱码
       (hnwin.c:816-817, 提前到入口覆盖 --probe 的输出) */
    SetConsoleOutputCP(CP_UTF8);
    if (argc < 2) {
        fprintf(stderr, "用法: hnweb_win <file.html> [W H]               开窗口\n"
                        "      hnweb_win <file.html> --probe [W H]      无窗口自检\n"
                        "      hnweb_win <file.html> --shot <png> [W H] 渲染位图落盘\n"
                        "      (尺寸缺省取清单 hn-window, 再缺省 480x700)\n");
        return 2;
    }
    const char *path = argv[1];
    int probe = (argc > 2 && !strcmp(argv[2], "--probe"));
    /* --shot 只给路径也能出图(与 hnweb_linux 同宽; hnwin 要求 --shot 后
       必须跟 W H, 这里放宽, 尺寸一律有下限保护) */
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
       顺序: 先按原 cwd 读 HTML, 再 chdir, 之后一切相对解析用 basename。
       (hnwin.c:688-705 同口径; '/' 与 '\\' 都认。) */
    size_t hlen = 0;
    char *html = read_all(path, &hlen);
    if (!html) { fprintf(stderr, "hnweb_win: 无法读取 %s\n", path); return 1; }
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
    if (!doc) { fprintf(stderr, "hnweb_win: 解析失败\n"); free(html); return 1; }

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
    opts.resizable = 1;             /* M1 常规窗口(WS_OVERLAPPEDWINDOW) */
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

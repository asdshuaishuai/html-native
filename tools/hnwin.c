/* hnwin.c — Windows 运行时 MVP: Win32 窗口 + hnsoft 软件光栅
 *
 * 定位: 引擎(C99, 平台无关)之上的第一层 Windows 表面物化。它证明
 * "display list 是平台边界" —— 这个文件是 Windows 侧的消费者:
 *   解析 → 级联 → 布局 → display list → hnsoft RGBA 位图 → 窗口像素
 *
 * M1 能力(够跑真实文档):
 *   窗口(manifest: 标题/尺寸/透明背板) · 渲染 · 点击命中测试
 *   hover 伪类 · 滚轮滚动 · resize 重排 · 过渡动画帧循环
 *
 * 路径口径: 启动即 chdir 到 HTML 所在目录 —— 资产/图片/link 全部
 * 相对文档目录解析(与浏览器一致; hnsoft 内部 fopen 也是 cwd 基准)。
 *
 * 用法:
 *   hnwin <file.html> [W H]     开窗口
 *   hnwin <file.html> --probe   无窗口自检(管线统计 + 采样点命中), 供 CI
 *
 * 已知取舍(M1):
 *   - 文本: 无 FreeType 时引擎用等宽估算, 位图不渲染字形(几何仍正确)
 *   - DPI: 按 CSS px = 物理 px, 高分屏未缩放
 *   - 透明窗口用 UpdateLayeredWindow(预乘 alpha); 不透明走 BitBlt
 *   - 点击目前只回传命中元素 id(事件派发管道是 M2)
 */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "hn.h"
#include "hnsoft.h"

/* UTF-8 → UTF-16。Win32 宽字符 API 要求 UTF-16; 走 ANSI 版会把 UTF-8
   字节按本地代码页(中文 Windows = GBK)解读, 中文标题即乱码。 */
static wchar_t *utf8_to_w(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = (wchar_t *)malloc((size_t)n * sizeof(wchar_t));
    if (!w) return NULL;
    MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

/* ---------------- 文件读取 / 资产后端(与 hncore.c 同口径) ---------------- */

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
    asset_tab[asset_n].data = d;
    asset_tab[asset_n].len = n;
    asset_n++;
    if (len) *len = n;
    return d;
}

static char *load_css(const char *html_path, const char *html, size_t *out_len) {
    /* 1) 同名 .css */
    size_t plen = strlen(html_path);
    if (plen > 5 && !strcmp(html_path + plen - 5, ".html")) {
        char *css_path = (char *)malloc(plen + 1);
        memcpy(css_path, html_path, plen - 5);
        strcpy(css_path + plen - 5, ".css");
        size_t clen = 0;
        char *css = read_all(css_path, &clen);
        free(css_path);
        if (css) { *out_len = clen; return css; }
    }
    /* 2) <link rel="stylesheet" href="..."> 内联 */
    const char *p = html;
    while ((p = strstr(p, "<link")) != NULL) {
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
                    memcpy(path, q, n);
                    path[n] = 0;
                    size_t clen = 0;
                    char *css = read_all(path, &clen);
                    free(path);
                    if (css) { *out_len = clen; return css; }
                }
            }
        }
        p = end + 1;
    }
    *out_len = 0;
    return NULL;
}

/* ---------------- 应用状态 ---------------- */

typedef struct {
    hn_context *ctx;
    int w, h;               /* client 区尺寸(CSS px) */
    int transparent;        /* manifest: 逐像素 alpha 窗口 */
    unsigned char *px;      /* hnsoft 输出的 RGBA 位图 */
    int px_n;
    HBITMAP dib;            /* 不透明窗口用的 DIB section */
    unsigned char *dib_bits;
    hn_node *hover;         /* 当前 hover 元素(避免每像素重排) */
} app_t;

static app_t g_app;
static const char *g_html_path;
static hn_node *g_focus;      /* 当前焦点 input(回车提交用) */
static int g_use_webkit;      /* hn-renderer=webkit: 走 WebView2 兜底 */
static const char *g_store_id = "default";

/* WebView2 兜底 + sys:// 系统桥(公共模块) */
int  hnwebview_open(HWND parent, const char *html, const char *css,
                    const char *store_id);
void hnwebview_resize(void);
void hnwebview_close(void);
char *sys_fragment(const char *url, const char *form_body, const char *store_id);

/* 布局期文本后端: FreeType 真实测量(不注入则引擎走等宽估算, CJK
   偏宽 65%, 窄容器内文字被错误地逐字换行 —— macOS 无此问题因为它
   注入了 CoreText 后端)。 */
static hn_text_backend g_tb;
static float tb_measure(void *ctx, const hn_font_desc *font,
                        const char *utf8, size_t len) {
    (void)ctx;
    return hnsoft_measure(font, utf8, len);
}
static void tb_metrics(void *ctx, const hn_font_desc *font,
                       float *ascent, float *descent, float *leading) {
    (void)ctx;
    hnsoft_metrics(font, ascent, descent, leading);
}

/* 布局 + 软件光栅。宽高变化或内容变化后调用。 */
static int app_render(int w, int h) {
    hn_context_layout(g_app.ctx, (float)w, (float)h, &g_tb);
    const hn_display_list *dl = hn_context_display_list(g_app.ctx);
    if (!dl) return 0;
    free(g_app.px);
    g_app.px = hnsoft_render(dl, w, h, 0x00000000);
    g_app.px_n = w * h * 4;
    return g_app.px != NULL;
}

/* RGBA(直通) → BGRA 预乘, 供 DIB / UpdateLayeredWindow 使用。
   premul=1 时输出预乘 alpha(UpdateLayeredWindow 要求)。 */
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

/* 把引擎位图送进窗口: 透明走 UpdateLayeredWindow, 不透明走 DIB+Invalidate。 */
static void app_present(HWND hwnd) {
    if (!g_app.px) return;
    if (g_app.transparent) {
        /* 全窗口逐像素更新: 构造 32bpp DIB(临时)喂给 UpdateLayeredWindow */
        HDC hdc = GetDC(hwnd);
        BITMAPINFO bi;
        memset(&bi, 0, sizeof(bi));
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = g_app.w;
        bi.bmiHeader.biHeight = -g_app.h; /* 自上而下 */
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void *bits = NULL;
        HBITMAP bmp = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
        if (bmp && bits) {
            rgba_to_bgra((unsigned char *)bits, g_app.px, g_app.w * g_app.h, 1);
            HDC mem = CreateCompatibleDC(hdc);
            HGDIOBJ old = SelectObject(mem, bmp);
            POINT ptSrc = { 0, 0 };
            SIZE sz = { g_app.w, g_app.h };
            BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };
            UpdateLayeredWindow(hwnd, hdc, NULL, &sz, mem, &ptSrc, 0, &bf, ULW_ALPHA);
            SelectObject(mem, old);
            DeleteDC(mem);
            DeleteObject(bmp);
        }
        ReleaseDC(hwnd, hdc);
    } else {
        if (g_app.dib_bits)
            rgba_to_bgra(g_app.dib_bits, g_app.px, g_app.w * g_app.h, 0);
        InvalidateRect(hwnd, NULL, FALSE);
    }
}

/* hn-renderer 声明: content=webkit 时走系统 WebView2 兜底。
   页面显式声明, 绝不静默切换 —— 与 README 的双渲染器契约一致。 */
static int wants_webkit(const char *html) {
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

/* ---- hx-* 执行(htmx 语义): 点击/回车/load/轮询共用 ----
   transport 进程内(sys:// 直接应答), 引擎不碰网络。语义与
   macOS 运行时一致: hx-get/post + hx-target + hx-swap + 表单参数。 */

/* 找 hx 载体: 自身带 hx-get/post, 否则向上冒泡 */
static hn_node *hx_carrier(hn_node *n) {
    if (!n) return NULL;
    if (hn_node_attr(n, "hx-get") || hn_node_attr(n, "hx-post")) return n;
    hn_node *a = hn_node_ancestor_with_attr(n, "hx-get");
    hn_node *b = hn_node_ancestor_with_attr(n, "hx-post");
    return a ? a : b;
}

/* 对一个载体执行 sys:// 请求并换入目标元素; 返回 1 表示发生了 swap */
static int hx_perform(HWND hwnd, hn_node *carrier) {
    const char *post = hn_node_attr(carrier, "hx-post");
    const char *get = hn_node_attr(carrier, "hx-get");
    const char *url = post ? post : get;
    if (!url) return 0;
    hn_doc *doc = hn_context_doc(g_app.ctx);
    /* 表单参数(POST 发 body / GET 拼 query) */
    char form[4096];
    form[0] = 0;
    hn_doc_form_encode(doc, form, sizeof(form));
    char *frag = sys_fragment(url, form, g_store_id);
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
    if (ok && app_render(g_app.w, g_app.h)) app_present(hwnd);
    return ok;
}

/* 启动动作: 文档内所有 hx-trigger 含 "load" 的元素立即执行一次 */
static void run_load_actions(void) {
    hn_doc *doc = hn_context_doc(g_app.ctx);
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
            int ok = hx_perform(NULL, n);
            printf("[hx-load] swap=%d\n", ok);
        }
    }
    if (!any) printf("[hx-load] 无 load 触发元素\n");
    fflush(stdout);
}

/* 轮询(hx-trigger="every Ns"): 每 tick 检查到期 */
typedef struct { char id[64]; int ms; DWORD last; } poll_ent;
static poll_ent g_polls[16];
static int g_poll_n = -1;

static void poll_tick(void) {
    if (g_poll_n < 0) {
        g_poll_n = 0;
        hn_doc *doc = hn_context_doc(g_app.ctx);
        for (int i = 0; i < 16; i++) {
            const char *id = NULL; int ms = 0;
            if (!hn_doc_poll_at(doc, i, &id, &ms)) break;
            snprintf(g_polls[g_poll_n].id, 64, "%s", id);
            g_polls[g_poll_n].ms = ms;
            g_polls[g_poll_n].last = GetTickCount();
            g_poll_n++;
        }
    }
    DWORD now = GetTickCount();
    for (int i = 0; i < g_poll_n; i++) {
        if (now - g_polls[i].last >= (DWORD)g_polls[i].ms) {
            g_polls[i].last = now;
            hn_node *n = hn_doc_find_by_id(hn_context_doc(g_app.ctx), g_polls[i].id);
            if (n) hx_perform(NULL, n);
        }
    }
}

/* 点击: 命中测试回传最深元素 id + hx 派发 + input 焦点 */
static void on_click(HWND hwnd, int x, int y) {
    const char *id = hn_context_hit_test(g_app.ctx, (float)x, (float)y);
    hn_node *n = hn_context_hit_node(g_app.ctx, (float)x, (float)y);
    const char *tag = n ? hn_node_tag(n) : NULL;
    printf("[click] (%d,%d) -> <%s id=%s>\n", x, y,
           tag ? tag : "?", id ? id : "(无id)");
    /* input 焦点(回车提交用) */
    g_focus = n ? hn_node_ancestor_input(n) : NULL;
    if (g_focus) hn_context_set_focus(g_app.ctx, g_focus);
    /* hx 派发: 自身或祖先带 hx-get/post 即执行 */
    hn_node *carrier = hx_carrier(n);
    if (carrier) {
        const char *url = hn_node_attr(carrier, "hx-post") ? hn_node_attr(carrier, "hx-post")
                                                          : hn_node_attr(carrier, "hx-get");
        printf("[hx] %s\n", url ? url : "?");
        hx_perform(hwnd, carrier);
    }
    fflush(stdout);
}

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp);
static void snap_window(HWND hwnd, const char *out);

static LRESULT CALLBACK wnd_proc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        /* 动画帧循环: 16ms tick, 有过渡动画在跑就重渲染 */
        SetTimer(hwnd, 1, 16, NULL);
        return 0;
    }
    case WM_SIZE: {
        int w = LOWORD(lp), h = HIWORD(lp);
        if (g_use_webkit) { hnwebview_resize(); return 0; }
        if (w > 0 && h > 0 && (w != g_app.w || h != g_app.h)) {
            g_app.w = w; g_app.h = h;
            if (app_render(w, h)) {
                if (g_app.dib) { DeleteObject(g_app.dib); g_app.dib = NULL; g_app.dib_bits = NULL; }
                HDC hdc = GetDC(hwnd);
                BITMAPINFO bi;
                memset(&bi, 0, sizeof(bi));
                bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
                bi.bmiHeader.biWidth = w;
                bi.bmiHeader.biHeight = -h;
                bi.bmiHeader.biPlanes = 1;
                bi.bmiHeader.biBitCount = 32;
                bi.bmiHeader.biCompression = BI_RGB;
                g_app.dib = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, (void **)&g_app.dib_bits, NULL, 0);
                ReleaseDC(hwnd, hdc);
                app_present(hwnd);
            }
        }
        return 0;
    }
    case WM_PAINT: {
        PAINTSTRUCT ps;
        HDC hdc = BeginPaint(hwnd, &ps);
        if (!g_app.transparent && g_app.dib && g_app.dib_bits) {
            HDC mem = CreateCompatibleDC(hdc);
            HGDIOBJ old = SelectObject(mem, g_app.dib);
            BitBlt(hdc, 0, 0, g_app.w, g_app.h, mem, 0, 0, SRCCOPY);
            SelectObject(mem, old);
            DeleteDC(mem);
        }
        EndPaint(hwnd, &ps);
        return 0;
    }
    case WM_MOUSEMOVE: {
        int x = LOWORD(lp), y = HIWORD(lp);
        hn_node *n = hn_context_hit_node(g_app.ctx, (float)x, (float)y);
        if (n != g_app.hover) {
            g_app.hover = n;
            hn_context_set_hover(g_app.ctx, n); /* 触发 :hover 重新匹配 */
            if (app_render(g_app.w, g_app.h)) app_present(hwnd);
        }
        SetCursor(LoadCursor(NULL, (n && hn_node_cursor(n) == 1) ? IDC_HAND : IDC_ARROW));
        return 0;
    }
    case WM_LBUTTONDOWN:
        on_click(hwnd, LOWORD(lp), HIWORD(lp));
        return 0;
    case WM_MOUSEWHEEL: {
        int x = LOWORD(lp), y = HIWORD(lp);
        POINT pt = { x, y };
        ScreenToClient(hwnd, &pt);
        hn_node *s = hn_context_scrollable_at(g_app.ctx, (float)pt.x, (float)pt.y);
        if (s) {
            float dy = (float)(-(short)HIWORD(wp) / WHEEL_DELTA * 48);
            if (hn_node_scroll_by(s, 0, dy)) {
                hn_context_repaint(g_app.ctx);
                free(g_app.px);
                g_app.px = hnsoft_render(hn_context_display_list(g_app.ctx), g_app.w, g_app.h, 0x00000000);
                app_present(hwnd);
            }
        }
        return 0;
    }
    case WM_TIMER: {
        /* 首帧完成后抓一次真实窗口像素(HN_WIN_SNAP 调试用) */
        static int snapped = 0;
        if (!snapped) {
            snapped = 1;
            const char *snap = getenv("HN_WIN_SNAP");
            if (snap && *snap) snap_window(hwnd, snap);
        }
        /* hx 轮询(every Ns); webkit 路径由注入脚本自理 */
        if (!g_use_webkit) poll_tick();
        /* 过渡动画推进; 返回 1 表示仍在跑 —— 继续帧循环 */
        if (hn_context_anim_tick(g_app.ctx, 16.0f)) {
            free(g_app.px);
            g_app.px = hnsoft_render(hn_context_display_list(g_app.ctx), g_app.w, g_app.h, 0x00000000);
            app_present(hwnd);
        }
        return 0;
    }
    case WM_KEYDOWN: {
        /* 回车提交: input 上按 Enter → 自身 → 祖先 → 最近容器第一个
           hx-post/get 载体(与 native 引擎语义一致) */
        if (wp == VK_RETURN && g_focus && !g_use_webkit) {
            hn_node *carrier = hn_node_form_carrier(g_focus);
            if (carrier) {
                const char *url = hn_node_attr(carrier, "hx-post") ? hn_node_attr(carrier, "hx-post")
                                                                  : hn_node_attr(carrier, "hx-get");
                printf("[hx-enter] %s\n", url ? url : "?");
                hx_perform(hwnd, carrier);
            }
        }
        return 0;
    }
    case WM_DESTROY:
        if (g_use_webkit) hnwebview_close();
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProc(hwnd, msg, wp, lp);
}

/* ---------------- --probe: 无窗口自检 ----------------
   验证管线在 Windows 上完整跑通, 并采样命中点。退出码 0 = 健康。 */
/* 遍历带 id 元素, 对盒中心做命中 —— 验证命中/冒泡语义自洽。
   中心点必命中自身或后代, 冒泡向上必经自身, 因此 hit==id 或后代 id;
   NULL 说明盒坐标无效, 判失败。 */
static int probe_id_hits(hn_context *ctx, hn_node *n, int *count) {
    int bad = 0;
    if (n && hn_node_tag(n)) {
        const char *id = hn_node_attr(n, "id");
        if (id && id[0]) {
            float x, y, w, h;
            hn_node_box(n, &x, &y, &w, &h);
            if (w > 0 && h > 0) {
                (*count)++;
                const char *hit = hn_context_hit_test(ctx, x + w / 2, y + h / 2);
                if (!hit) { printf("FAIL id=%s: 中心点无命中\n", id); bad++; }
                else if (strcmp(hit, id) != 0)
                    printf("note id=%s: 中心命中=%s (带 id 后代覆盖, 正常)\n", id, hit);
            }
        }
    }
    for (hn_node *c = hn_node_first_child(n); c; c = hn_node_next_sibling(c))
        bad += probe_id_hits(ctx, c, count);
    return bad;
}

/* 调试/视觉回归: HN_WIN_SNAP=<png> 时, 首帧后从窗口 DC 抓取真实显示
   像素存 PNG —— 验证的是真正贴进窗口的位图(含 BGRA 通道转换), 而非
   引擎输出。对无头环境无法截屏的 CI 尤其有用。 */
static void snap_window(HWND hwnd, const char *out) {
    HDC hdc = GetDC(hwnd);
    HDC mem = CreateCompatibleDC(hdc);
    RECT rc;
    GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) { DeleteDC(mem); ReleaseDC(hwnd, hdc); return; }
    BITMAPINFO bi;
    memset(&bi, 0, sizeof(bi));
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void *bits = NULL;
    HBITMAP bmp = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, NULL, 0);
    if (!bmp) { DeleteDC(mem); ReleaseDC(hwnd, hdc); return; }
    HGDIOBJ old = SelectObject(mem, bmp);
    BitBlt(mem, 0, 0, w, h, hdc, 0, 0, SRCCOPY);
    /* 窗口 DIB 是 BGRA, 转回 RGBA 给编码器 */
    unsigned char *rgba = (unsigned char *)malloc((size_t)w * h * 4);
    if (rgba) {
        const unsigned char *bgra = (const unsigned char *)bits;
        for (int i = 0; i < w * h; i++) {
            rgba[i * 4]     = bgra[i * 4 + 2];
            rgba[i * 4 + 1] = bgra[i * 4 + 1];
            rgba[i * 4 + 2] = bgra[i * 4];
            rgba[i * 4 + 3] = 0xFF;
        }
        size_t pn = 0;
        unsigned char *png = hnsoft_encode_png(rgba, w, h, &pn);
        if (png) {
            FILE *of = fopen(out, "wb");
            if (of) { fwrite(png, 1, pn, of); fclose(of); }
            free(png);
        }
        free(rgba);
    }
    SelectObject(mem, old);
    DeleteObject(bmp);
    DeleteDC(mem);
    ReleaseDC(hwnd, hdc);
}

/* ---------------- --shot: 渲染位图落盘(与窗口管线同源) ----------------
   不弹窗, 把 hnsoft 渲染结果编码为 PNG —— 验证"窗口会画什么"的
   确定性手段, 也适合无窗口环境出图。 */
static int shot_mode(hn_context *ctx, const char *out, int w, int h) {
    hn_context_layout(ctx, (float)w, (float)h, &g_tb);
    const hn_display_list *dl = hn_context_display_list(ctx);
    if (!dl) { fprintf(stderr, "shot: 无绘制指令\n"); return 1; }
    unsigned char *px = hnsoft_render(dl, w, h, 0x00000000);
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

    /* 采样命中: 中心 + 四角 */
    struct { int x, y; } pts[] = { {w/2,h/2}, {8,8}, {w-8,8}, {8,h-8}, {w-8,h-8} };
    for (size_t i = 0; i < sizeof(pts)/sizeof(pts[0]); i++) {
        const char *id = hn_context_hit_test(ctx, (float)pts[i].x, (float)pts[i].y);
        hn_node *n = hn_context_hit_node(ctx, (float)pts[i].x, (float)pts[i].y);
        printf("hit(%d,%d) -> <%s id=%s>\n", pts[i].x, pts[i].y,
               n ? hn_node_tag(n) : "?", id ? id : "(无id)");
    }
    /* id 命中自洽: 每个带 id 元素的盒中心必须命中自身(或带 id 后代) */
    int idn = 0, idbad = probe_id_hits(ctx, hn_doc_root(hn_context_doc(ctx)), &idn);
    printf("id-hits=%d bad=%d\n", idn, idbad);
    if (idbad) return 1;
    printf("probe OK\n");
    return 0;
}

/* ---------------- main ---------------- */

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "用法: hnwin <file.html> [W H]  开窗口\n"
                        "      hnwin <file.html> --probe  无窗口自检\n");
        return 2;
    }
    const char *path = argv[1];
    int probe = (argc > 2 && !strcmp(argv[2], "--probe"));
    int shot = (argc > 4 && !strcmp(argv[2], "--shot"));
    int W = 480, H = 700;
    if (!probe && !shot) {
        if (argc > 2) W = atoi(argv[2]);
        if (argc > 3) H = atoi(argv[3]);
    } else {
        int a = probe ? 3 : 4;
        if (argc > a) W = atoi(argv[a]);
        if (argc > a + 1) H = atoi(argv[a + 1]);
    }

    /* 路径口径统一到 HTML 所在目录(资产/link/图片都是相对文档的)。
       顺序: 先按原 cwd 读 HTML, 再 chdir, 之后一切相对解析用 basename。 */
    size_t hlen = 0;
    char *html = read_all(path, &hlen);
    if (!html) { fprintf(stderr, "hnwin: 无法读取 %s\n", path); return 1; }
    const char *base = path;
    for (const char *p = path; *p; p++)
        if (*p == '/' || *p == '\\') base = p + 1;
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            size_t dl = (size_t)(p - path) + 1;
            char *dir = (char *)malloc(dl + 1);
            memcpy(dir, path, dl);
            dir[dl] = 0;
            if (chdir(dir) != 0) { /* chdir 失败不影响绝对路径场景 */ }
            free(dir);
        }
    }
    hn_doc *doc = hn_parse_html(html, hlen);
    if (!doc) { fprintf(stderr, "hnwin: 解析失败\n"); return 1; }

    size_t clen = 0;
    char *css = load_css(base, html, &clen);

    hn_context *ctx = hn_context_create();
    hn_context_set_doc(ctx, doc);
    if (css && clen) hn_context_add_sheet(ctx, hn_parse_css(css, clen));
    hn_asset_backend ab;
    memset(&ab, 0, sizeof(ab));
    ab.load = asset_load;
    hn_context_set_assets(ctx, &ab);
    hn_context_apply_theme(ctx); /* hn-theme 设计令牌(若文档声明) */

    /* 清单: 标题/尺寸/透明背板由 hn 编码声明, 引擎解析 */
    hn_manifest man;
    memset(&man, 0, sizeof(man));
    hn_doc_manifest(doc, &man);
    if (man.w > 0 && argc <= 4) W = (int)man.w;
    if (man.h > 0 && argc <= 4) H = (int)man.h;

    g_app.ctx = ctx;
    g_app.w = W; g_app.h = H;
    g_app.transparent = man.transparent;
    g_html_path = path;

    /* store id: 文件 stem(dashboard.html → dashboard) */
    g_store_id = base;
    char stem[256];
    snprintf(stem, sizeof(stem), "%s", base);
    char *dot = strrchr(stem, '.');
    if (dot) *dot = 0;
    g_store_id = _strdup(stem);

    /* 文本后端: FreeType 真实测量(否则 CJK 按字节估算偏宽 65%, 布局失真) */
    g_tb.ctx = NULL;
    g_tb.measure = tb_measure;
    g_tb.metrics = tb_metrics;

    /* 渲染器声明: hn-renderer=webkit 显式兜底(绝不静默切换) */
    g_use_webkit = wants_webkit(html);

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

    /* ---- Win32 窗口(UNICODE 构建: 类名/标题走 UTF-16) ---- */
    const wchar_t *cls = L"HnWin";
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wnd_proc;
    wc.hInstance = GetModuleHandle(NULL);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = cls;
    wc.hCursor = LoadCursor(NULL, IDC_ARROW);
    RegisterClassExW(&wc);

    /* client 区正好 W×H */
    RECT r = { 0, 0, W, H };
    AdjustWindowRect(&r, WS_OVERLAPPEDWINDOW, FALSE);

    /* 标题: manifest 里是 UTF-8, 转 UTF-16 后进宽字符 API */
    wchar_t *title_w = utf8_to_w(man.title && man.title[0] ? man.title : "hnwin");
    if (!title_w) title_w = utf8_to_w("hnwin");

    DWORD exstyle = man.transparent ? WS_EX_LAYERED : 0;
    HWND hwnd = CreateWindowExW(exstyle, cls, title_w, WS_OVERLAPPEDWINDOW,
                                CW_USEDEFAULT, CW_USEDEFAULT,
                                r.right - r.left, r.bottom - r.top,
                                NULL, NULL, wc.hInstance, NULL);
    if (!hwnd) { fprintf(stderr, "hnwin: CreateWindow 失败\n"); return 1; }
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    /* 首帧: 渲染器双分支 —— webkit 兜底挂 WebView2, native 走 hnsoft */
    if (g_use_webkit) {
        if (!hnwebview_open(hwnd, html, css, g_store_id)) {
            fprintf(stderr, "hnwin: WebView2 兜底启动失败\n");
            return 1;
        }
    } else {
        if (!app_render(W, H)) { fprintf(stderr, "hnwin: 首帧渲染失败\n"); return 1; }
        if (!man.transparent) {
            HDC hdc = GetDC(hwnd);
            BITMAPINFO bi;
            memset(&bi, 0, sizeof(bi));
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = W;
            bi.bmiHeader.biHeight = -H;
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 32;
            bi.bmiHeader.biCompression = BI_RGB;
            g_app.dib = CreateDIBSection(hdc, &bi, DIB_RGB_COLORS, (void **)&g_app.dib_bits, NULL, 0);
            ReleaseDC(hwnd, hdc);
        }
        app_present(hwnd);
        run_load_actions(); /* hx-trigger="load" 立即执行一次 */
    }

    /* console 输出 UTF-8(命中 id 可能是中文), 否则终端按本地代码页乱码 */
    SetConsoleOutputCP(CP_UTF8);
    printf("[hnwin] 窗口已开: %s (%dx%d)%s%s\n",
           man.title && man.title[0] ? man.title : "hnwin", W, H,
           g_use_webkit ? " [webkit 兜底]" : "",
           man.transparent ? " 透明背板" : "");
    if (!g_use_webkit)
        printf("[hnwin] 文本: %s\n", hnsoft_font_loaded()
               ? "FreeType 已加载(微软雅黑)" : "无字体(等宽估算, 位图不含字形)");
    fflush(stdout);
    free(title_w);

    MSG msg;
    while (GetMessage(&msg, NULL, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }

    if (g_app.dib) DeleteObject(g_app.dib);
    free(g_app.px);
    hn_context_destroy(ctx);
    free(css); free(html);
    return 0;
}

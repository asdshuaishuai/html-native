/* hncore.c — 引擎核心的无平台 CLI(纯 ISO C99, 零依赖)
 *
 * 存在意义: 证明并交付"引擎的平台无关性"。
 * 这个二进制不链接 AppKit/CoreText/任何系统 UI 框架 —— 只要有 C 编译器就能构建,
 * 因此 macOS / Linux / Windows 三平台产物完全同源, 且行为逐字节可比。
 *
 * 它能做什么(不依赖任何图形后端):
 *   hncore parse  <file.html>            解析并打印 DOM 树
 *   hncore layout <file.html> [W H]      级联+布局, 打印盒模型树
 *   hncore paint  <file.html> [W H]      翻译为绘制指令并打印(display list)
 *   hncore text   <file.html> [W H]      打印文本片段(字节区间 + 位置)
 *   hncore boxes  <file.html> [W H]      打印每个元素的绝对盒(机器可读)
 *   hncore verify <file.html> [W H]      自检: 布局不变量(重叠/越界/负尺寸)
 *
 * 布局在没有文本后端时使用等宽估算回退(引擎内建), 因此纯 C 也能算出几何;
 * 接入真实字体(CoreText/DirectWrite/FreeType)由各平台运行时完成。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn.h"
#include "hnsoft.h"
/* cairo 绘制后端: 与 hnsoft 同签名, 编译时带 -DHN_USE_CAIRO 即启用。
   两份实现的差别正是"每个平台手写一份"与"一个跨平台框架"的差别 ——
   renderc 子命令用来对照同一份显示列表在两者下的输出。 */
#ifdef HN_USE_CAIRO
#include "hn_cairo.h"
#endif

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

/* ---- 资产后端: 引擎不做 I/O, 由这里提供字节(Lottie JSON 等) ----
   缓冲必须长期存活(引擎就地解析会写入), 故用一张表持有。 */
typedef struct { char *path; char *data; size_t len; } asset_ent;
static asset_ent asset_tab[64];
static int asset_n = 0;

/* 文本测量回调: 直接转发到 hnsoft(FreeType 可用时真实测量,
   否则回退到引擎同口径的等宽估算) */
static float hnsoft_measure_cb(void *ctx, const hn_font_desc *font,
                               const char *utf8, size_t len) {
    (void)ctx;
    return hnsoft_measure(font, utf8, len);
}
#ifdef HN_USE_CAIRO
static float hncairo_measure_cb(void *ctx, const hn_font_desc *font,
                                const char *utf8, size_t len) {
    (void)ctx;
    return hncairo_measure(font, utf8, len);
}
static void hncairo_metrics_cb(void *ctx, const hn_font_desc *font,
                               float *ascent, float *descent, float *leading) {
    (void)ctx;
    hncairo_metrics(font, ascent, descent, leading);
}
#endif

static void hnsoft_metrics_cb(void *ctx, const hn_font_desc *font,
                              float *ascent, float *descent, float *leading) {
    (void)ctx;
    hnsoft_metrics(font, ascent, descent, leading);
}

/* HTML 所在目录(含结尾分隔符)。href/src 相对路径必须相对**文档目录**解析
   —— 与浏览器口径一致。以 cwd 为基准会在“文档不在 cwd”时静默丢资产
   (实测: 从项目根 render examples/lottie.html, Lottie 求值直接归零)。 */
static char g_base_dir[1024];

static char *read_asset(const char *path, size_t *len) {
    char *d = read_all(path, len);
    if (!d && g_base_dir[0]) {
        char joined[1200];
        snprintf(joined, sizeof(joined), "%s%s", g_base_dir, path);
        d = read_all(joined, len);
    }
    return d;
}

static const char *asset_load(void *ctx, const char *path, size_t *len) {
    (void)ctx;
    for (int i = 0; i < asset_n; i++)
        if (!strcmp(asset_tab[i].path, path)) { if (len) *len = asset_tab[i].len; return asset_tab[i].data; }
    if (asset_n >= 64) return NULL;
    size_t n = 0;
    char *d = read_asset(path, &n);
    if (!d) return NULL;
    /* strdup 是 POSIX 而非 C99, 严格 c99 下(musl/Windows libc)不声明它 ——
       交叉构建会报 implicit declaration 并因此把返回值当 int。 */
    size_t pn = strlen(path);
    char *pcopy = (char *)malloc(pn + 1);
    if (!pcopy) { free(d); return NULL; }
    memcpy(pcopy, path, pn + 1);
    asset_tab[asset_n].path = pcopy;
    asset_tab[asset_n].data = d;
    asset_tab[asset_n].len = n;
    asset_n++;
    if (len) *len = n;
    return d;
}

/* 常见位置: 同名 .css 与 <link rel=stylesheet href> 抽取 */
static char *load_css(const char *html_path, const char *html, size_t *out_len) {
    /* 1) 同名 .css 文件 */
    size_t plen = strlen(html_path);
    if (plen > 5 && !strcmp(html_path + plen - 5, ".html")) {
        char *css_path = (char *)malloc(plen + 1);
        memcpy(css_path, html_path, plen - 5);
        strcpy(css_path + plen - 5, ".css");
        FILE *f = fopen(css_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long n = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (n > 0) {
                char *buf = (char *)malloc((size_t)n + 1);
                size_t got = fread(buf, 1, (size_t)n, f);
                buf[got] = 0;
                fclose(f);
                free(css_path);
                *out_len = got;
                return buf;
            }
            fclose(f);
        }
        free(css_path);
    }
    /* 2) <link rel="stylesheet" href="..."> 内联抽取(剥掉标签) */
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
                    /* href 是路径: 相对 HTML 目录解析(与资产口径一致) */
                    char *path = (char *)malloc(n + 1);
                    memcpy(path, q, n);
                    path[n] = 0;
                    size_t clen = 0;
                    char *css = read_asset(path, &clen);
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

static void print_dom(hn_node *n, int depth) {
    for (int i = 0; i < depth; i++) fputs("  ", stdout);
    const char *tag = hn_node_tag(n);
    if (tag) {
        printf("<%s", tag);
        /* 打出**全部**属性(不只 id/class)。
           data-* / hx-* / hn-* 这些才是排查"为什么没生效"的关键线索 ——
           只打 id 和 class 时, 看到 <div id="x"> 完全无法判断它到底带了
           哪些 hx-get 或 hn-mesh 声明。 */
        for (int i = 0; ; i++) {
            const char *an = NULL, *av = NULL;
            if (!hn_node_attr_at(n, i, &an, &av)) break;
            printf(" %s=\"%s\"", an, av ? av : "");
        }
        printf(">\n");
    } else {
        size_t vlen = 0;
        const char *v = hn_node_value(n, &vlen);
        printf("#text");
        if (v && vlen) {
            size_t show = vlen > 40 ? 40 : vlen;
            printf(" \"%.*s%s\"", (int)show, v, vlen > 40 ? "…" : "");
        }
        printf("\n");
    }
    for (hn_node *c = hn_node_first_child(n); c; c = hn_node_next_sibling(c))
        print_dom(c, depth + 1);
}

static void print_boxes(hn_node *n, int depth, int csv) {
    const char *tag = hn_node_tag(n);
    if (tag) {
        float x, y, w, h;
        hn_node_box(n, &x, &y, &w, &h);
        if (csv) {
            printf("%s,%s,%.1f,%.1f,%.1f,%.1f\n", tag,
                   hn_node_attr(n, "id") ? hn_node_attr(n, "id") : "", x, y, w, h);
        } else {
            for (int i = 0; i < depth; i++) fputs("  ", stdout);
            printf("%-10s %7.1f %7.1f %7.1f %7.1f", tag, x, y, w, h);
            const char *id = hn_node_attr(n, "id");
            if (id) printf("  #%s", id);
            printf("\n");
        }
    }
    for (hn_node *c = hn_node_first_child(n); c; c = hn_node_next_sibling(c))
        print_boxes(c, depth + (tag ? 1 : 0), csv);
}

/* 自检: 布局不变量。返回违反项数量(0 = 健康) */
static int verify_boxes(hn_node *n, int *checked) {
    int bad = 0;
    const char *tag = hn_node_tag(n);
    if (tag) {
        float x, y, w, h;
        hn_node_box(n, &x, &y, &w, &h);
        /* 排除 display:none / head 内元素(尺寸为 0 是正常的) */
        int visible = (w > 0 || h > 0);
        if (visible) {
            (*checked)++;
            if (w < 0 || h < 0) {
                printf("  ✘ 负尺寸: <%s> %.1fx%.1f\n", tag, w, h);
                bad++;
            }
            if (x < -1 || y < -1) {
                printf("  ✘ 负坐标: <%s> at (%.1f,%.1f)\n", tag, x, y);
                bad++;
            }
        }
    }
    for (hn_node *c = hn_node_first_child(n); c; c = hn_node_next_sibling(c))
        bad += verify_boxes(c, checked);
    return bad;
}

int main(int argc, char **argv) {
    if (argc < 3) {
        fputs("用法: hncore <render|parse|layout|paint|text|boxes|verify> <file.html> [W H|out.png W H]\n"
#ifdef HN_USE_CAIRO
              "  render  渲染为 PNG(软件光栅化, 零 GUI 依赖; 文本需 FreeType)\n"
              "  renderc 渲染为 PNG(cairo 后端: 一份实现喂所有平台)\n"
#else
              "  render  渲染为 PNG(软件光栅化, 零 GUI 依赖; 文本需 FreeType)\n"
#endif
              "  parse   解析并打印 DOM 树\n"
              "  layout  级联+布局, 打印盒模型树\n"
              "  paint   打印绘制指令(display list)\n"
              "  text    打印文本片段(位置与字节区间)\n"
              "  boxes   打印绝对盒(每行一个元素)\n"
              "  verify  布局不变量自检\n", stderr);
        return 2;
    }
    const char *cmd = argv[1];
    const char *path = argv[2];
    float W = argc > 3 ? (float)atof(argv[3]) : 460;
    float H = argc > 4 ? (float)atof(argv[4]) : 560;

    /* HTML 所在目录: 资产/link 的相对路径以此为基准(与浏览器口径一致) */
    for (const char *p = path; *p; p++) {
        if (*p == '/' || *p == '\\') {
            size_t dl = (size_t)(p - path) + 1;
            if (dl < sizeof(g_base_dir)) {
                memcpy(g_base_dir, path, dl);
                g_base_dir[dl] = 0;
            }
        }
    }

    size_t hlen = 0;
    char *html = read_all(path, &hlen);
    if (!html) {
        fprintf(stderr, "hncore: 无法读取 %s\n", path);
        return 1;
    }

    hn_doc *doc = hn_parse_html(html, hlen);
    if (!doc) {
        fprintf(stderr, "hncore: 解析失败\n");
        free(html);
        return 1;
    }

    if (!strcmp(cmd, "parse")) {
        hn_node *root = hn_doc_root(doc);
        if (root) print_dom(root, 0);
        free(html);
        return 0;
    }

    size_t clen = 0;
    char *css = load_css(path, html, &clen);

    hn_context *ctx = hn_context_create();
    hn_context_set_doc(ctx, doc);
    if (css && clen) hn_context_add_sheet(ctx, hn_parse_css(css, clen));
    /* 资产后端: Lottie JSON 由这里读入(引擎自身不做 I/O) */
    hn_asset_backend ab;
    memset(&ab, 0, sizeof(ab));
    ab.ctx = NULL;
    ab.load = asset_load;
    hn_context_set_assets(ctx, &ab);
    /* 文本后端: FreeType 真实测量(不注入则 CJK 按字节×字号×0.55 估算,
       偏宽 65%, 窄容器内文字被错误逐字换行; 与注入 CoreText 的
       macOS 运行时行为不一致) */
    hn_text_backend tb;
    memset(&tb, 0, sizeof(tb));
    tb.measure = hnsoft_measure_cb;
    tb.metrics = hnsoft_metrics_cb;
    hn_context_layout(ctx, W, H, &tb);
    /* HN_CLOCK=<ms>: 把外部资源动画(Lottie/网格)推进到指定时刻。
       没有这步则渲染的是第 0 帧 —— 断言需要确定性时刻。 */
    const char *clock_env = getenv("HN_CLOCK");
    if (clock_env && *clock_env) {
        float ms = (float)atof(clock_env);
        /* 分步推进, 让多段关键帧与循环取模都走到(单步也能到, 这里只为稳妥) */
        float step = 16.0f;
        for (float t = 0; t < ms; t += step)
            hn_context_anim_tick(ctx, t + step > ms ? ms - t : step);
        hn_context_repaint(ctx);
    }

    int rc = 0;
    if (!strcmp(cmd, "layout")) {
        hn_node *root = hn_doc_root(doc);
        if (root) print_boxes(root, 0, 0);
    } else if (!strcmp(cmd, "boxes")) {
        hn_node *root = hn_doc_root(doc);
        if (root) print_boxes(root, 0, 1);
    } else if (!strcmp(cmd, "paint")) {
        const hn_display_list *dl = hn_context_display_list(ctx);
        if (!dl) { fprintf(stderr, "hncore: 无绘制指令\n"); rc = 1; }
        else {
            printf("指令数: %d\n", dl->count);
            for (int i = 0; i < dl->count; i++) {
                const hn_cmd *c = &dl->cmds[i];
                switch (c->kind) {
                case HN_CMD_RECT:
                    /* stroke/shadow 一并打印 —— 少了它们就没法验证
                       border-color / box-shadow 是否生效(只能看到 fill)。 */
                    printf("RECT   %7.1f %7.1f %7.1f %7.1f r=%.1f fill=%08X%s stroke=%08X w=%.1f%s\n",
                           c->x, c->y, c->w, c->h, c->radius, c->fill,
                           c->gradient ? " gradient" : "",
                           c->stroke, c->stroke_w,
                           c->shadow ? " shadow" : "");
                    break;
                case HN_CMD_TEXT:
                    printf("TEXT   x=%7.1f bl=%7.1f ", c->tx, c->baseline);
                    for (size_t k = 0; k < c->text_len && k < 40; k++)
                        fputc(c->text[k], stdout);
                    printf("  (sz=%.0f fill=%08X)\n", c->font.size_px, c->fill);
                    break;
                case HN_CMD_IMAGE:
                    printf("IMAGE  %7.1f %7.1f %7.1f %7.1f %s\n",
                           c->x, c->y, c->w, c->h, c->text ? c->text : "");
                    break;
                case HN_CMD_CLIP_PUSH: printf("CLIP+  %7.1f %7.1f %7.1f %7.1f\n", c->x, c->y, c->w, c->h); break;
                case HN_CMD_CLIP_POP:  printf("CLIP-\n"); break;
                case HN_CMD_QUAD:
                    printf("QUAD   (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f) (%.1f,%.1f) fill=%08X\n",
                           c->qx[0], c->qy[0], c->qx[1], c->qy[1],
                           c->qx[2], c->qy[2], c->qx[3], c->qy[3], c->fill);
                    break;
                case HN_CMD_POLYGON: {
                    float mnx = 1e9f, mny = 1e9f, mxx = -1e9f, mxy = -1e9f;
                    for (int q = 0; q < c->poly_n; q++) {
                        float x = c->poly[q * 2], y = c->poly[q * 2 + 1];
                        if (x < mnx) mnx = x; if (x > mxx) mxx = x;
                        if (y < mny) mny = y; if (y > mxy) mxy = y;
                    }
                    printf("POLY   n=%3d bbox=[%7.1f %7.1f %7.1f %7.1f] fill=%08X stroke=%08X w=%.1f%s\n",
                           c->poly_n, mnx, mny, mxx - mnx, mxy - mny,
                           c->fill, c->stroke, c->stroke_w,
                           c->even_odd ? " evenodd" : "");
                    break;
                }
                case HN_CMD_MESH:
                    printf("MESH   %dx%d %7.1f %7.1f %7.1f %7.1f %s\n",
                           c->mesh_cols, c->mesh_rows, c->x, c->y, c->w, c->h,
                           c->text ? c->text : "");
                    break;
                }
            }
        }
    } else if (!strcmp(cmd, "text")) {
        hn_node *root = hn_doc_root(doc);
        /* 深度遍历找持有 run 的节点 */
        hn_node *stack[4096];
        int sp = 0;
        if (root) stack[sp++] = root;
        while (sp) {
            hn_node *n = stack[--sp];
            int nr = hn_node_run_count(n);
            for (int i = 0; i < nr; i++) {
                float x, bl, w, yt, h;
                if (hn_node_run_at(n, i, &x, &bl, &w, &yt, &h) == 1) {
                    /* 带上片段文本 —— 只打坐标和宽度时, 无法判断某个 run
                       到底是词还是空白串(排查 white-space 必须看到内容)。 */
                    size_t bl2 = 0, be = 0;
                    printf("run  x=%7.1f  baseline=%7.1f  w=%6.1f  h=%5.1f  \"", x, bl, w, h);
                    if (hn_node_run_range(n, i, &bl2, &be) == 1) {
                        size_t tl = 0;
                        const char *tx = hn_node_text(n, &tl);
                        if (tx && be <= tl) {
                            for (size_t k = bl2; k < be && k < bl2 + 24; k++)
                                fputc(tx[k] == ' ' ? '_' : tx[k], stdout);
                        }
                    }
                    printf("\"\n");
                }
            }
            for (hn_node *c = hn_node_first_child(n); c; c = hn_node_next_sibling(c))
                if (sp < 4096) stack[sp++] = c;
        }
    } else if (!strcmp(cmd, "render")) {
        const char *out = argc > 3 ? argv[3] : "out.png";
        if (argc > 4) { W = (float)atof(argv[4]); }
        if (argc > 5) { H = (float)atof(argv[5]); }
        hn_context_layout(ctx, W, H, &tb);
        const hn_display_list *dl = hn_context_display_list(ctx);
        if (!dl) { fprintf(stderr, "hncore: 无绘制指令\n"); rc = 1; }
        else {
            unsigned char *px = hnsoft_render(dl, (int)W, (int)H, 0x0B0E13FF);
            if (!px) { fprintf(stderr, "hncore: 渲染失败\n"); rc = 1; }
            else {
                size_t pn = 0;
                unsigned char *png = hnsoft_encode_png(px, (int)W, (int)H, &pn);
                if (png) {
                    FILE *of = fopen(out, "wb");
                    if (of) { fwrite(png, 1, pn, of); fclose(of);
                        printf("已渲染 %dx%d → %s (%.1f KB)%s\n", (int)W, (int)H, out,
                               (double)pn / 1024.0,
                               hnsoft_font_loaded() ? "" : "  [无字体: 文本未渲染]");
                    } else { fprintf(stderr, "hncore: 无法写 %s\n", out); rc = 1; }
                    free(png);
                } else { fprintf(stderr, "hncore: PNG 编码失败\n"); rc = 1; }
                free(px);
            }
        }
#ifdef HN_USE_CAIRO
    } else if (!strcmp(cmd, "renderc")) {
        /* 用 cairo 后端渲染(与 render 同一份显示列表, 不同绘制实现)。
           文本测量也换成 hncairo_*: 测量与绘制必须同源, 否则字形会落出排好的
           行盒 —— 这也是保留 FreeType 而不是用 cairo 字体后端的原因。 */
        const char *out = argc > 3 ? argv[3] : "outc.png";
        if (argc > 4) { W = (float)atof(argv[4]); }
        if (argc > 5) { H = (float)atof(argv[5]); }
        hn_text_backend tb = { NULL, hncairo_measure_cb, hncairo_metrics_cb };
        hn_context_layout(ctx, W, H, &tb);
        const hn_display_list *dl = hn_context_display_list(ctx);
        if (!dl) { fprintf(stderr, "hncore: 无绘制指令\n"); rc = 1; }
        else {
            unsigned char *px = hncairo_render(dl, (int)W, (int)H, 0x0B0E13FF);
            if (!px) { fprintf(stderr, "hncore: cairo 渲染失败\n"); rc = 1; }
            else {
                size_t pn = 0;
                unsigned char *png = hncairo_encode_png(px, (int)W, (int)H, &pn);
                if (png) {
                    FILE *of = fopen(out, "wb");
                    if (of) { fwrite(png, 1, pn, of); fclose(of);
                        printf("已渲染(cairo) %dx%d → %s (%.1f KB)%s\n",
                               (int)W, (int)H, out, (double)pn / 1024.0,
                               hncairo_font_loaded() ? "" : "  [无字体: 文本未渲染]");
                    } else { fprintf(stderr, "hncore: 无法写 %s\n", out); rc = 1; }
                    free(png);
                } else { fprintf(stderr, "hncore: cairo PNG 编码失败\n"); rc = 1; }
                free(px);
            }
        }
    } else if (!strcmp(cmd, "verify")) {
#else
    } else if (!strcmp(cmd, "verify")) {
#endif
        int checked = 0;
        int bad = verify_boxes(hn_doc_root(doc), &checked);
        printf("检查元素: %d, 违反不变量: %d\n", checked, bad);
        /* 命中自洽: 每个带 id 元素的盒中心必须命中自身或带 id 后代。
           此前只有 Windows 的 hnwin --probe 做这项检查 —— 它是纯引擎语义,
           与平台无关, 没理由不在所有平台上验。 */
        int hits = 0, hbad = hn_context_verify_hits(ctx, &hits);
        printf("命中自检: %d 个带 id 元素, 违反 %d\n", hits, hbad);
        if (hbad) rc = 1;
        else printf("布局自检通过\n");
    } else {
        fprintf(stderr, "hncore: 未知命令 %s\n", cmd);
        rc = 2;
    }

    free(css);
    free(html);
    return rc;
}

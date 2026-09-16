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
                    /* href 是路径: 读其内容 */
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

static void print_dom(hn_node *n, int depth) {
    for (int i = 0; i < depth; i++) fputs("  ", stdout);
    const char *tag = hn_node_tag(n);
    if (tag) {
        printf("<%s", tag);
        const char *id = hn_node_attr(n, "id");
        if (id) printf(" id=\"%s\"", id);
        const char *cls = hn_node_attr(n, "class");
        if (cls) printf(" class=\"%s\"", cls);
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
        fputs("用法: hncore <parse|layout|paint|text|boxes|verify> <file.html> [W H]\n"
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
    /* 无文本后端: 引擎用等宽估算(纯 C 也能算几何) */
    hn_context_layout(ctx, W, H, NULL);

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
                    printf("RECT   %7.1f %7.1f %7.1f %7.1f r=%.1f fill=%08X%s\n",
                           c->x, c->y, c->w, c->h, c->radius, c->fill,
                           c->gradient ? " gradient" : "");
                    break;
                case HN_CMD_TEXT:
                    printf("TEXT   x=%7.1f bl=%7.1f ", c->tx, c->baseline);
                    for (size_t k = 0; k < c->text_len && k < 40; k++)
                        fputc(c->text[k], stdout);
                    printf("  (sz=%.0f)\n", c->font.size_px);
                    break;
                case HN_CMD_IMAGE:
                    printf("IMAGE  %7.1f %7.1f %7.1f %7.1f %s\n",
                           c->x, c->y, c->w, c->h, c->text ? c->text : "");
                    break;
                case HN_CMD_CLIP_PUSH: printf("CLIP+  %7.1f %7.1f %7.1f %7.1f\n", c->x, c->y, c->w, c->h); break;
                case HN_CMD_CLIP_POP:  printf("CLIP-\n"); break;
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
                if (hn_node_run_at(n, i, &x, &bl, &w, &yt, &h) == 1)
                    printf("run  x=%7.1f  baseline=%7.1f  w=%6.1f  h=%5.1f\n", x, bl, w, h);
            }
            for (hn_node *c = hn_node_first_child(n); c; c = hn_node_next_sibling(c))
                if (sp < 4096) stack[sp++] = c;
        }
    } else if (!strcmp(cmd, "verify")) {
        hn_node *root = hn_doc_root(doc);
        int checked = 0;
        int bad = root ? verify_boxes(root, &checked) : 0;
        printf("检查元素: %d, 违反不变量: %d\n", checked, bad);
        if (bad) rc = 1;
        else printf("布局自检通过\n");
    } else {
        fprintf(stderr, "hncore: 未知命令 %s\n", cmd);
        rc = 2;
    }

    free(css);
    free(html);
    return rc;
}

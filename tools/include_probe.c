/* include_probe.c — <include> 零构建组件全链路探针(确定性, 退出码判定)
 *
 * 管线与生产同一条: hn_html_set_doc_path(喂文档路径) → hn_parse_html
 * (装载期文本级拼接) → hn_context 布局/绘制。断言分五层:
 *   1. 嵌套拼接: inc-main → inc-a → inc-b 三层内容全部进入 DOM
 *   2. 基准随层级走: sub/inc-c.html 引的是**同目录** inc-d.html(不是文档目录)
 *   3. 环检测: inc-b 回引 inc-main —— 根内容只出现一次, 不再展开
 *   4. 深度上限: 运行时生成 8 层链式夹具, 第 6 层进 DOM、第 7 层被占位截断
 *   5. 渲染断言: include 进来的元素有布局盒(#inc-b w/h>0)且 "inc-b-body"
 *      以 TEXT 指令出现在显示列表; 缺文件页(ghost.html)解析不失败、
 *      其余内容完好
 *
 * 夹具: examples/fixtures/(inc-main/inc-a/inc-b + sub/inc-c/sub/inc-d;
 *       ghost.html 故意不存在)。深度链的 8 个临时夹具在运行时生成,
 *       结束时删除, 不留在仓库里。
 *
 * 用法(py 薄封装编好即跑; 手工编译见 tools/include_probe.py):
 *   python3 tools/include_probe.py
 * 断言失败打印 got/want, 退出码 1; 全绿退出 0。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn.h"

static int g_pass = 0, g_fail = 0;

static void check(int cond, const char *label) {
    printf("%s %s\n", cond ? "  ✔" : "  ✘", label);
    if (cond) g_pass++; else g_fail++;
}

static void check_int(long got, long want, const char *label) {
    int ok = got == want;
    printf("%s %s (got %ld want %ld)\n", ok ? "  ✔" : "  ✘", label, got, want);
    if (ok) g_pass++; else g_fail++;
}

static void check_box(float got_w, float got_h, const char *label) {
    int ok = got_w > 0 && got_h > 0;
    printf("%s %s (got %.1fx%.1f want >0x>0)\n", ok ? "  ✔" : "  ✘", label,
           got_w, got_h);
    if (ok) g_pass++; else g_fail++;
}

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

static void write_all(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) { fprintf(stderr, "include_probe: 无法写夹具 %s\n", path); exit(1); }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

/* ---- DOM 计数(hn.h 公开遍历口径, 与 hncore print_dom 同法) ---- */

static int count_id_rec(hn_node *n, const char *id) {
    int c = 0;
    if (hn_node_tag(n)) {
        const char *v = hn_node_attr(n, "id");
        if (v && !strcmp(v, id)) c++;
    }
    for (hn_node *ch = hn_node_first_child(n); ch; ch = hn_node_next_sibling(ch))
        c += count_id_rec(ch, id);
    return c;
}

static int count_id(hn_doc *doc, const char *id) {
    return doc ? count_id_rec(hn_doc_root(doc), id) : 0;
}

static int count_tag_rec(hn_node *n, const char *tag) {
    int c = 0;
    if (hn_node_tag(n) && !strcmp(hn_node_tag(n), tag)) c++;
    for (hn_node *ch = hn_node_first_child(n); ch; ch = hn_node_next_sibling(ch))
        c += count_tag_rec(ch, tag);
    return c;
}

static int count_tag(hn_node *n, const char *tag) {
    return n ? count_tag_rec(n, tag) : 0;
}

/* 任意文本节点(含 include 拼进来的)是否含 needle */
static int text_has_rec(hn_node *n, const char *needle) {
    if (!hn_node_tag(n)) {
        size_t tl = 0;
        const char *tx = hn_node_text(n, &tl);
        if (tx && tl >= strlen(needle) && strstr(tx, needle)) return 1;
    }
    for (hn_node *ch = hn_node_first_child(n); ch; ch = hn_node_next_sibling(ch))
        if (text_has_rec(ch, needle)) return 1;
    return 0;
}

/* 显示列表里是否存在内容含 needle 的 TEXT 指令 */
static int dl_text_contains(const hn_display_list *dl, const char *needle) {
    if (!dl) return 0;
    size_t nl = strlen(needle);
    for (int i = 0; i < dl->count; i++) {
        const hn_cmd *c = &dl->cmds[i];
        if (c->kind != HN_CMD_TEXT || !c->text || c->text_len < nl) continue;
        for (size_t k = 0; k + nl <= c->text_len; k++)
            if (!memcmp(c->text + k, needle, nl)) return 1;
    }
    return 0;
}

/* ---- 深度链夹具: root → d1 → … → d7(第 7 层应被占位截断) ---- */

static void make_depth_chain(const char *fix, char root_path[], size_t cap) {
    char p[1024];
    static char rootbuf[256];
    snprintf(rootbuf, sizeof(rootbuf),
             "<div id=\"deep-root\">deep-root\n"
             "<include src=\"probe-deep-1.html\">\n</div>");
    snprintf(p, sizeof(p), "%s/probe-deep-root.html", fix);
    write_all(p, rootbuf);
    for (int i = 1; i <= 6; i++) {
        char buf[256];
        snprintf(buf, sizeof(buf),
                 "<div id=\"deep-%d\">deep-%d\n"
                 "<include src=\"probe-deep-%d.html\">\n</div>", i, i, i + 1);
        snprintf(p, sizeof(p), "%s/probe-deep-%d.html", fix, i);
        write_all(p, buf);
    }
    snprintf(p, sizeof(p), "%s/probe-deep-7.html", fix);
    write_all(p, "<div id=\"deep-7\">deep-7-end</div>");
    snprintf(root_path, cap, "%s/probe-deep-root.html", fix);
}

static void clean_depth_chain(const char *fix) {
    char p[1024];
    snprintf(p, sizeof(p), "%s/probe-deep-root.html", fix); remove(p);
    for (int i = 1; i <= 7; i++) {
        snprintf(p, sizeof(p), "%s/probe-deep-%d.html", fix, i);
        remove(p);
    }
}

int main(int argc, char **argv) {
    printf("include_probe — <include> 零构建组件(装载期文本级拼接)\n\n");

    /* 夹具目录: py 以仓库根为 cwd 运行, 缺省相对路径即可命中;
       也接受 argv[1] 显式指定(手工从别处运行用)。 */
    const char *fix = (argc > 1) ? argv[1] : "examples/fixtures";

    /* == 1. 嵌套拼接 + 缺文件容错 + 环检测(同一份根夹具) == */
    printf("== 嵌套拼接(inc-main → inc-a → inc-b) ==\n");
    char main_path[1024];
    snprintf(main_path, sizeof(main_path), "%s/inc-main.html", fix);
    size_t hlen = 0;
    char *html = read_all(main_path, &hlen);
    check(html != NULL, "夹具 inc-main.html 可读");
    if (!html) return 1;
    hn_html_set_doc_path(main_path);          /* 装载通道: 相对本文档目录解析 */
    hn_doc *doc = hn_parse_html(html, hlen);
    check(doc != NULL, "含缺文件 include(ghost.html)的文档解析不失败");
    if (!doc) return 1;
    check_int(count_id(doc, "main-h"), 1, "根文档内容进入 DOM");
    check_int(count_id(doc, "inc-a"), 1, "一级片段 inc-a 进入 DOM");
    check_int(count_id(doc, "inc-b"), 1, "二级片段 inc-b 进入 DOM");
    check(text_has_rec(hn_doc_root(doc), "inc-b-body"), "二级片段文本在 DOM");
    check_int(count_tag(hn_doc_root(doc), "include"), 0,
              "<include> 标签不残留(全部原地替换为内容)");

    printf("== 基准随包含层级走 ==\n");
    check_int(count_id(doc, "inc-c"), 1, "sub/inc-c.html 进入 DOM");
    check_int(count_id(doc, "inc-d"), 1,
              "inc-d(被 inc-c 以同目录 src 引用)进入 DOM —— 基准不是文档根");

    printf("== 缺文件容错 ==\n");
    check_int(count_id(doc, "main-tail"), 1,
              "缺文件 include 之后的兄弟内容完好");
    check_int(count_id(doc, "ghost"), 0, "缺文件贡献零节点(仅注释占位)");

    printf("== 环检测 ==\n");
    check_int(count_id(doc, "main-h"), 1,
              "inc-b 回引 inc-main: 根内容只出现一次(环被截断)");
    check_int(count_id(doc, "inc-a"), 1, "环上的 inc-a 也只出现一次");

    /* == 2. 深度上限: 8 层链, 第 7 层被占位截断 == */
    printf("== 深度上限(嵌套上限 6) ==\n");
    char deep_path[1024];
    make_depth_chain(fix, deep_path, sizeof(deep_path));
    size_t dlen = 0;
    char *dhtml = read_all(deep_path, &dlen);
    check(dhtml != NULL, "深度链夹具生成可读");
    hn_html_set_doc_path(deep_path);
    hn_doc *ddoc = dhtml ? hn_parse_html(dhtml, dlen) : NULL;
    check(ddoc != NULL, "深度链文档解析成功");
    if (ddoc) {
        check_int(count_id(ddoc, "deep-1"), 1, "第 1 层进入 DOM");
        check_int(count_id(ddoc, "deep-6"), 1, "第 6 层(上限)进入 DOM");
        check_int(count_id(ddoc, "deep-7"), 0, "第 7 层被占位截断, 不进 DOM");
        hn_doc_free(ddoc);
    }
    free(dhtml);
    clean_depth_chain(fix);

    /* == 3. 渲染断言: include 进来的元素有布局盒与 TEXT 指令 == */
    printf("== 渲染(布局盒 + 显示列表) ==\n");
    hn_context *ctx = hn_context_create();
    hn_context_set_doc(ctx, doc);            /* 所有权交给 context */
    hn_context_layout(ctx, 460, 560, NULL);  /* 无文本后端: 引擎内建估算回退 */
    hn_node *nb = hn_doc_find_by_id(doc, "inc-b");
    check(nb != NULL, "inc-b 节点可寻址");
    if (nb) {
        float x = 0, y = 0, w = 0, h = 0;
        hn_node_box(nb, &x, &y, &w, &h);
        check_box(w, h, "include 进来的元素有布局盒");
        printf("  (inc-b 盒: %.1f,%.1f %.1fx%.1f)\n", x, y, w, h);
    }
    const hn_display_list *dl = hn_context_display_list(ctx);
    check(dl != NULL && dl->count > 0, "显示列表非空");
    check(dl_text_contains(dl, "inc-b-body"), "include 内容出现 TEXT 绘制指令");
    check(dl_text_contains(dl, "inc-main-head"), "根文档内容同样在显示列表");

    hn_context_destroy(ctx);
    free(html);

    printf("\n结果: %d 通过, %d 失败\n", g_pass, g_fail);
    if (g_fail == 0) printf("全绿 — <include> 零构建组件链路健康\n");
    return g_fail ? 1 : 0;
}

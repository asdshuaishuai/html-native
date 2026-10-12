/* md_probe.c — Markdown 直染验收(纯 C99, 离屏, 零 GUI 依赖)
 *
 * 管线与生产同一条: hn_context_set_doc(触发 hn_md pass: md 源 → 普通 DOM)
 * → 级联 → 布局 → 绘制指令列表。断言分三层:
 *   结构 —— DOM 里出现预期的语义标签/属性/文本(零样式依赖, 全确定);
 *   渲染 —— 布局盒 + 显示列表: 代码块有背景矩形、表格有边框线、标题有 TEXT;
 *   回归 —— 无 hn-md 的普通文档零影响(结构不变 + 布局健康)。
 *
 * 断言口径: 只断与文本度量无关的确定性结论(结构/属性/指令形态/几何相交),
 * 不断具体像素宽度(FreeType 有无会改变排版宽度, 与 render_probe 同口径)。
 *
 * 用法(直接 cc, 无 SPM; 由 tools/md_probe.py 薄封装编译运行):
 *   bash tools/tools/md_probe.sh [已编译产物路径]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn.h"
/* hnsoft.h 不在 include/(对包外是模块私有), 但渲染断言必须用生产文本后端
   与生产光栅器同源 —— 以相对路径包含引擎树内的同一份头(与 render_probe 同法)。 */
#include "../Sources/CHtmlNative/hnsoft.h"

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

static void check_str(const char *got, const char *want, const char *label) {
    int ok = got && want && !strcmp(got, want);
    printf("%s %s (got %s want %s)\n", ok ? "  ✔" : "  ✘", label,
           got ? got : "(null)", want ? want : "(null)");
    if (ok) g_pass++; else g_fail++;
}

static void check_float(float got, float want, const char *label) {
    int ok = got > want - 0.01f && got < want + 0.01f;
    printf("%s %s (got %.2f want %.2f)\n", ok ? "  ✔" : "  ✘", label, got, want);
    if (ok) g_pass++; else g_fail++;
}

/* ---------- DOM 查询辅助 ---------- */

static hn_node *find_tag(hn_node *n, const char *tag) {
    if (!n) return NULL;
    const char *t = hn_node_tag(n);
    if (t && !strcmp(t, tag)) return n;
    for (hn_node *c = hn_node_first_child(n); c; c = hn_node_next_sibling(c)) {
        hn_node *r = find_tag(c, tag);
        if (r) return r;
    }
    return NULL;
}

static int count_tag(hn_node *n, const char *tag) {
    if (!n) return 0;
    int k = 0;
    const char *t = hn_node_tag(n);
    if (t && !strcmp(t, tag)) k++;
    for (hn_node *c = hn_node_first_child(n); c; c = hn_node_next_sibling(c))
        k += count_tag(c, tag);
    return k;
}

static hn_node *child_tag(hn_node *n, const char *tag) {
    if (!n) return NULL;
    for (hn_node *c = hn_node_first_child(n); c; c = hn_node_next_sibling(c)) {
        const char *t = hn_node_tag(c);
        if (t && !strcmp(t, tag)) return c;
    }
    return NULL;
}

static int child_elem_count(hn_node *n) {
    int k = 0;
    if (!n) return 0;
    for (hn_node *c = hn_node_first_child(n); c; c = hn_node_next_sibling(c))
        if (hn_node_tag(c)) k++;
    return k;
}

static size_t node_text(hn_node *n, char *out, size_t cap) {
    if (!n) { if (cap) out[0] = 0; return 0; }
    return hn_node_text_content(n, out, cap);
}

static int text_is(hn_node *n, const char *want) {
    char buf[256];
    node_text(n, buf, sizeof(buf));
    return !strcmp(buf, want);
}

static hn_node *find_input(hn_node *n) { return find_tag(n, "input"); }

/* 文档序遍历所有 li, 统计任务清单形态(li.task + checkbox/checked) */
static void scan_tasks(hn_node *n, int *task, int *cb, int *checked) {
    if (!n) return;
    const char *t = hn_node_tag(n);
    if (t && !strcmp(t, "li")) {
        const char *cls = hn_node_attr(n, "class");
        if (cls && strstr(cls, "task")) {
            (*task)++;
            for (hn_node *ch = hn_node_first_child(n); ch; ch = hn_node_next_sibling(ch)) {
                const char *ct = hn_node_tag(ch);
                if (ct && !strcmp(ct, "input")) {
                    (*cb)++;
                    if (hn_node_attr(ch, "checked")) (*checked)++;
                }
            }
        }
    }
    for (hn_node *c = hn_node_first_child(n); c; c = hn_node_next_sibling(c))
        scan_tasks(c, task, cb, checked);
}

/* DFS 找第一个"标签匹配且文本内容含 sub"的元素 */
static hn_node *find_tag_with_text(hn_node *n, const char *tag, const char *sub) {
    if (!n) return NULL;
    const char *t = hn_node_tag(n);
    if (t && !strcmp(t, tag)) {
        char buf[512];
        hn_node_text_content(n, buf, sizeof(buf));
        if (strstr(buf, sub)) return n;
    }
    for (hn_node *c = hn_node_first_child(n); c; c = hn_node_next_sibling(c)) {
        hn_node *r = find_tag_with_text(c, tag, sub);
        if (r) return r;
    }
    return NULL;
}

/* ---------- 装载辅助: 解析 + set_doc(触发 hn_md)+ 布局 ---------- */

static float measure_cb(void *ctx, const hn_font_desc *font,
                        const char *utf8, size_t len) {
    (void)ctx;
    return hnsoft_measure(font, utf8, len);
}
static void metrics_cb(void *ctx, const hn_font_desc *font,
                       float *ascent, float *descent, float *leading) {
    (void)ctx;
    hnsoft_metrics(font, ascent, descent, leading);
}

#define VW 720.0f
#define VH 900.0f

/* context 生命周期: *doc 交给 context(销毁时随 context 释放) */
static hn_context *load(const char *html, hn_doc **doc_out) {
    hn_doc *doc = hn_parse_html(html, strlen(html));
    if (!doc) return NULL;
    hn_context *c = hn_context_create();
    hn_context_set_doc(c, doc);            /* 这里触发 hn_md 后处理 pass */
    hn_text_backend tb = { NULL, measure_cb, metrics_cb };
    hn_context_layout(c, VW, VH, &tb);
    if (doc_out) *doc_out = doc;
    return c;
}

/* ---------- 用例一: 全量块级 + 行内子集 ---------- */

static const char *MD_HTML =
"<html><head>"
"<meta name=\"hn-theme\" content=\"dark\">"
"<meta name=\"hn-window\" content=\"720x900\">"
"</head><body>"
"<div class=\"card\"><div id=\"md-main\" hn-md>\n"
"# 周报概述\n"
"\n"
"正文里有 **加粗强调**、*斜体注记*、`inline_cc`、~~删除内容~~ 与 [参考链接](https://example.com/report)。\n"
"\n"
"## 进展明细\n"
"\n"
"### 渲染管线\n"
"\n"
"1. 第一步甲\n"
"2. 第二步乙\n"
"3. 第三步丙\n"
"\n"
"- [ ] 待办事项一\n"
"- [x] 已办事项二\n"
"- 普通条目\n"
"\n"
"> 引用一行: 把应用开发的技术成本压到最低。\n"
"\n"
"---\n"
"\n"
"| 指标 | 数值 | 说明 |\n"
"| --- | :---: | --- |\n"
"| 首帧耗时 | 8ms | 快 |\n"
"| 内存占用 | 24MB | 稳 |\n"
"\n"
"```python\n"
"print(\"hello\")\n"
"x = 42\n"
"```\n"
"</div></div></body></html>";

static void test_structure(void) {
    printf("== Markdown 直染: 结构 ==\n");
    hn_doc *doc = NULL;
    hn_context *c = load(MD_HTML, &doc);
    check(c && doc, "装载成功(hn-md 源被转换)");
    if (!c) return;
    hn_node *root = hn_doc_root(doc);

    /* 块级 */
    hn_node *h3 = find_tag(root, "h3");
    check(h3 && text_is(h3, "渲染管线"), "三级标题 → h3 且文本正确");
    hn_node *h1 = find_tag(root, "h1");
    check(h1 && text_is(h1, "周报概述"), "一级标题 → h1 且文本正确");
    hn_node *ol = find_tag(root, "ol");
    check(ol && child_elem_count(ol) == 3, "有序列表 3 项");
    hn_node *li1 = ol ? hn_node_first_child(ol) : NULL;
    check(li1 && text_is(li1, "第一步甲"), "有序列表首项文本正确");

    hn_node *pre = find_tag(root, "pre");
    hn_node *code = pre ? child_tag(pre, "code") : NULL;
    check(pre && code, "代码块 pre>code");
    check_str(code ? hn_node_attr(code, "data-lang") : NULL, "python", "代码块 data-lang=python");
    char buf[256];
    node_text(code, buf, sizeof(buf));
    check(strstr(buf, "print(\"hello\")") && strstr(buf, "x = 42"),
          "代码块内容原样保留(含换行)");

    hn_node *table = find_tag(root, "table");
    check(table != NULL, "表格 → table");
    check_int(table ? count_tag(table, "tr") : 0, 3, "表格 3 行(表头 + 2 体行)");
    hn_node *htr = table ? hn_node_first_child(table) : NULL;
    check_int(htr ? child_elem_count(htr) : 0, 3, "表头行 3 列 th");
    check(htr && hn_node_tag(hn_node_first_child(htr)) && !strcmp(hn_node_tag(hn_node_first_child(htr)), "th"),
          "表头单元格是 th");
    check_int(table ? count_tag(table, "td") : 0, 6, "体行共 6 个 td");
    hn_node *td_first = find_tag(table, "td");
    check(td_first && text_is(td_first, "首帧耗时"), "体行单元格文本正确");

    /* 任务清单: 同一 ul 内 2 个任务项 + 1 个普通项 */
    hn_node *ul = find_tag(root, "ul");
    check(ul && child_elem_count(ul) == 3, "无序列表 3 项(2 任务 + 1 普通)");
    int n_task = 0, n_cb = 0, n_checked = 0;
    scan_tasks(root, &n_task, &n_cb, &n_checked);
    check_int(n_task, 2, "任务清单 2 项(li.task)");
    check_int(n_cb, 2, "任务清单 2 个 checkbox");
    check_int(n_checked, 1, "任务清单其一 checked");

    hn_node *bq = find_tag(root, "blockquote");
    check(bq != NULL, "引用 → blockquote");
    hn_node *bqp = bq ? child_tag(bq, "p") : NULL;
    check(bqp && text_is(bqp, "引用一行: 把应用开发的技术成本压到最低。"), "引用内容成段");

    hn_node *hr = find_tag(root, "hr");
    check(hr != NULL, "分隔线 → hr");

    /* 行内 */
    hn_node *strong = find_tag(root, "strong");
    check(strong && text_is(strong, "加粗强调"), "行内 **粗** → strong");
    hn_node *em = find_tag(root, "em");
    check(em && text_is(em, "斜体注记"), "行内 *斜* → em");
    hn_node *s_el = find_tag(root, "s");
    check(s_el && text_is(s_el, "删除内容"), "行内 ~~删除~~ → s");
    hn_node *a = find_tag(root, "a");
    check_str(a ? hn_node_attr(a, "href") : NULL, "https://example.com/report", "链接 href 存属性");
    check(a && text_is(a, "参考链接"), "链接文本正确");
    /* 行内码: 段落里的 code(文档序在 pre>code 之前) */
    hn_node *p = find_tag(root, "p");
    hn_node *icc = p ? child_tag(p, "code") : NULL;
    check(icc && text_is(icc, "inline_cc"), "行内 `码` → code");

    check_int(hn_doc_validate(doc, 0), 0, "md 转换后 DOM 结构自检零问题");
    hn_context_destroy(c);
}

/* ---------- 用例二: 嵌套与转义 ---------- */

static void test_inline_nesting(void) {
    printf("== Markdown 直染: 嵌套与转义 ==\n");
    const char *SRC =
        "<html><body><div hn-md>\n"
        "段一 **粗里有`码`** 收尾。\n"
        "\n"
        "段二 \\*不是斜体\\* 与 \\`不是码\\`。\n"
        "</div></body></html>";
    hn_doc *doc = NULL;
    hn_context *c = load(SRC, &doc);
    if (!c) { check(0, "嵌套用例装载"); return; }
    hn_node *root = hn_doc_root(doc);
    hn_node *strong = find_tag(root, "strong");
    check(strong != NULL, "嵌套: strong 存在");
    hn_node *inner_code = strong ? child_tag(strong, "code") : NULL;
    check(inner_code && text_is(inner_code, "码"), "嵌套: **粗里有`码`** → strong>code");
    hn_node *em = find_tag(root, "em");
    check(em == NULL, "转义: \\*不产生 em");
    /* 转义段: 文本里应出现字面的 * 与 ` */
    hn_node *p2 = find_tag_with_text(root, "p", "不是斜体");
    int found = 0;
    if (p2) {
        char buf[256];
        node_text(p2, buf, sizeof(buf));
        found = strstr(buf, "*不是斜体*") != NULL && strstr(buf, "`不是码`") != NULL;
    }
    check(found, "转义: \\* 与 \\` 输出字面字符");
    check_int(hn_doc_validate(doc, 0), 0, "嵌套用例 DOM 自检");
    hn_context_destroy(c);
}

static void test_nested_list(void) {
    printf("== Markdown 直染: 嵌套列表 ==\n");
    const char *SRC =
        "<html><body><div hn-md>\n"
        "- 外层甲\n"
        "  - 内层一\n"
        "  - 内层二\n"
        "- 外层乙\n"
        "</div></body></html>";
    hn_doc *doc = NULL;
    hn_context *c = load(SRC, &doc);
    if (!c) { check(0, "嵌套列表装载"); return; }
    hn_node *root = hn_doc_root(doc);
    hn_node *outer = find_tag(root, "ul");
    check(outer && child_elem_count(outer) == 2, "外层列表 2 项");
    hn_node *li1 = outer ? hn_node_first_child(outer) : NULL;
    hn_node *inner = li1 ? child_tag(li1, "ul") : NULL;
    check(inner && child_elem_count(inner) == 2, "嵌套: li 内嵌 2 项子列表");
    hn_node *sub1 = inner ? hn_node_first_child(inner) : NULL;
    check(sub1 && text_is(sub1, "内层一"), "嵌套子项文本正确");
    hn_context_destroy(c);
}

static void test_multi_container(void) {
    printf("== Markdown 直染: 多容器 ==\n");
    const char *SRC =
        "<html><body>"
        "<div hn-md>\n# 标题甲\n</div>"
        "<p>中间夹一个普通段落</p>"
        "<div hn-md>\n## 标题乙\n</div>"
        "</body></html>";
    hn_doc *doc = NULL;
    hn_context *c = load(SRC, &doc);
    if (!c) { check(0, "多容器装载"); return; }
    hn_node *root = hn_doc_root(doc);
    check_int(count_tag(root, "h1"), 1, "容器一转换成 h1");
    check_int(count_tag(root, "h2"), 1, "容器二转换成 h2");
    check_int(count_tag(root, "p"), 1, "夹在中间的普通段落不受影响");
    hn_context_destroy(c);
}

/* ---------- 用例四: 渲染断言(布局盒 + 显示列表) ---------- */

static int rect_intersects(float x1, float y1, float w1, float h1,
                           float x2, float y2, float w2, float h2) {
    return x1 < x2 + w2 && x2 < x1 + w1 && y1 < y2 + h2 && y2 < y1 + h1;
}

static void test_render(void) {
    printf("== Markdown 直染: 渲染 ==\n");
    hn_doc *doc = NULL;
    hn_context *c = load(MD_HTML, &doc);
    if (!c) { check(0, "渲染用例装载"); return; }
    hn_node *root = hn_doc_root(doc);
    const hn_display_list *dl = hn_context_display_list(c);
    check(dl && dl->count > 0, "显示列表非空");

    /* 代码块: pre>code 盒内必须有带填充的 RECT(背景) */
    hn_node *pre = find_tag(root, "pre");
    hn_node *code = pre ? child_tag(pre, "code") : NULL;
    float cx, cy, cw, chh;
    hn_node_box(code, &cx, &cy, &cw, &chh);
    check(cw > 0 && chh > 0, "代码块布局盒非退化");
    int bg = 0;
    for (int i = 0; i < dl->count; i++) {
        const hn_cmd *cmd = &dl->cmds[i];
        if (cmd->kind != HN_CMD_RECT) continue;
        if ((cmd->fill & 0xFF) == 0) continue;
        if (rect_intersects(cmd->x, cmd->y, cmd->w, cmd->h, cx, cy, cw, chh)) { bg = 1; break; }
    }
    check(bg, "代码块有背景矩形指令");

    /* 表格: 单元格盒内必须有描边 RECT(边框线) */
    hn_node *td = find_tag(root, "td");
    float tx, ty, tw, th;
    hn_node_box(td, &tx, &ty, &tw, &th);
    check(tw > 0 && th > 0, "单元格布局盒非退化");
    int line = 0;
    for (int i = 0; i < dl->count; i++) {
        const hn_cmd *cmd = &dl->cmds[i];
        if (cmd->kind != HN_CMD_RECT) continue;
        if ((cmd->stroke & 0xFF) == 0 || cmd->stroke_w <= 0) continue;
        if (rect_intersects(cmd->x, cmd->y, cmd->w, cmd->h, tx, ty, tw, th)) { line = 1; break; }
    }
    check(line, "表格单元格有边框线指令");

    /* 标题: h3 盒内必须有内容为标题文本的 TEXT 指令 */
    hn_node *h3 = find_tag(root, "h3");
    float hx, hy, hw, hh;
    hn_node_box(h3, &hx, &hy, &hw, &hh);
    check(hw > 0 && hh > 0, "标题布局盒非退化");
    int htext = 0;
    const char *want = "渲染管线";
    size_t want_n = strlen(want);
    for (int i = 0; i < dl->count; i++) {
        const hn_cmd *cmd = &dl->cmds[i];
        if (cmd->kind != HN_CMD_TEXT || !cmd->text) continue;
        if (cmd->text_len != want_n || memcmp(cmd->text, want, want_n)) continue;
        if (cmd->tx >= hx - 1 && cmd->tx <= hx + hw &&
            cmd->baseline >= hy && cmd->baseline <= hy + hh) { htext = 1; break; }
    }
    check(htext, "标题有 TEXT 指令(位置落在 h3 盒内)");

    /* 任务清单 checkbox 有可见布局盒(既有控件语义参与行内流) */
    hn_node *input = find_input(root);
    float ix, iy, iw, ih;
    hn_node_box(input, &ix, &iy, &iw, &ih);
    check(iw > 0 && ih > 0, "任务清单 checkbox 有布局盒");
    check(hn_node_is_checked(input) == 0, "第一个任务项未选中");

    hn_context_destroy(c);
}

/* ---------- 用例六: 图片 ![](src) → <img> ---------- */

static void test_images(void) {
    printf("== Markdown 直染: 图片 ==\n");
    const char *SRC =
        "<html><body><div hn-md>\n"
        "![构建徽标](assets/avatar-a.png)\n"
        "\n"
        "正文引用一张空 alt 图 ![](assets/avatar-k.png) 与一段文字。\n"
        "\n"
        "畸形不吞字: ![alt](no-close 仍按字面渲染。\n"
        "</div></body></html>";
    hn_doc *doc = NULL;
    hn_context *c = load(SRC, &doc);
    if (!c) { check(0, "图片用例装载"); return; }
    hn_node *root = hn_doc_root(doc);
    check_int(count_tag(root, "img"), 2, "两张合法图片 → 2 个 img(畸形不计)");

    /* 第一张: src/alt 存属性(走既有图片管线, 零新绘制) */
    hn_node *img = find_tag(root, "img");
    check_str(img ? hn_node_attr(img, "src") : NULL, "assets/avatar-a.png", "img src 属性正确");
    check_str(img ? hn_node_attr(img, "alt") : NULL, "构建徽标", "img alt 属性正确");

    /* 第二张: 空 alt 文本 → 不设 alt 属性 */
    hn_node *img2 = find_tag_with_text(root, "p", "空 alt") ?
                     child_tag(find_tag_with_text(root, "p", "空 alt"), "img") : NULL;
    check(img2 != NULL, "行内图片生成 img(参与行内流)");
    check(img2 && hn_node_attr(img2, "alt") == NULL, "空 alt 文本不设 alt 属性");
    check_str(img2 ? hn_node_attr(img2, "src") : NULL, "assets/avatar-k.png", "行内图片 src 正确");

    /* 畸形: 未闭合括号按字面文本渲染(不吞字) */
    hn_node *lit = find_tag_with_text(root, "p", "no-close");
    check(lit != NULL, "畸形图片语法按字面保留");

    /* img 是既有语义元素: 有布局盒(图片后端缺席时走占位尺寸) */
    float ix, iy, iw, ih;
    hn_node_box(img, &ix, &iy, &iw, &ih);
    check(iw > 0 && ih > 0, "img 布局盒非退化");
    check_int(hn_doc_validate(doc, 0), 0, "图片用例 DOM 自检");
    hn_context_destroy(c);
}

/* ---------- 用例七: 链接点击导航(锚滚动 + pending link) ---------- */

static const char *LINK_HTML =
"<html><body>"
"<div id=\"view\" style=\"height: 300; overflow: scroll;\">"
"<div hn-md>\n"
"## 目录\n"
"\n"
"[外部文档](https://example.com/docs)\n"
"\n"
"[页内跳转](#target)\n"
"\n"
"占位段落甲, 撑出可滚动高度。\n"
"\n"
"占位段落乙, 撑出可滚动高度。\n"
"\n"
"占位段落丙, 撑出可滚动高度。\n"
"\n"
"占位段落丁, 撑出可滚动高度。\n"
"\n"
"占位段落戊, 撑出可滚动高度。\n"
"\n"
"占位段落己, 撑出可滚动高度。\n"
"\n"
"占位段落庚, 撑出可滚动高度。\n"
"</div>"
"<div id=\"target\" style=\"height: 60;\">TARGET-ANCHOR</div>"
"</div>"
"</body></html>";

/* 找 href 匹配的 a 元素 */
static hn_node *find_link(hn_node *n, const char *href) {
    if (!n) return NULL;
    const char *t = hn_node_tag(n);
    if (t && !strcmp(t, "a")) {
        const char *h = hn_node_attr(n, "href");
        if (h && !strcmp(h, href)) return n;
    }
    for (hn_node *c2 = hn_node_first_child(n); c2; c2 = hn_node_next_sibling(c2)) {
        hn_node *r = find_link(c2, href);
        if (r) return r;
    }
    return NULL;
}

static void test_links(void) {
    printf("== Markdown 直染: 链接点击导航 ==\n");
    hn_doc *doc = NULL;
    hn_context *c = load(LINK_HTML, &doc);
    if (!c) { check(0, "链接用例装载"); return; }
    hn_node *root = hn_doc_root(doc);

    /* 外链: 命中 → pending link 置位 → take 一次取走, 二次为空 */
    hn_node *ext = find_link(root, "https://example.com/docs");
    check(ext != NULL, "外链 a[href] 存在");
    float x, y, w, h;
    hn_node_box(ext, &x, &y, &w, &h);
    check(w > 0 && h > 0, "外链布局盒非退化");
    int r = hn_context_activate_link(c, x + w / 2, y + h / 2);
    check_int(r, HN_LINK_EXTERNAL, "命中外链 → HN_LINK_EXTERNAL");
    char link[512];
    check_int(hn_doc_take_pending_link(doc, link, sizeof(link)), 1, "take_pending_link 取到链接");
    check_str(link, "https://example.com/docs", "pending link 内容正确");
    check_int(hn_doc_take_pending_link(doc, link, sizeof(link)), 0, "二次 take 为空(一次消费即清)");

    /* 锚: 命中 → 引擎内滚动, 容器偏移变化 */
    hn_node *anc = find_link(root, "#target");
    check(anc != NULL, "锚链接 a[href=#target] 存在");
    hn_node *view = hn_doc_find_by_id(doc, "view");
    check(view != NULL, "滚动容器存在");
    float sy0;
    hn_node_scroll_get(view, NULL, &sy0);
    check_float(sy0, 0, "初始滚动偏移为 0");
    hn_node_box(anc, &x, &y, &w, &h);
    r = hn_context_activate_link(c, x + w / 2, y + h / 2);
    check_int(r, HN_LINK_ANCHOR, "命中锚链接 → HN_LINK_ANCHOR");
    float sy1;
    hn_node_scroll_get(view, NULL, &sy1);
    check(sy1 > sy0 + 1.0f, "锚滚动后容器偏移变化(向目标滚动)");
    hn_node *target = hn_doc_find_by_id(doc, "target");
    float tby, vbx, vby, vbw, vbh;
    hn_node_box(view, &vbx, &vby, &vbw, &vbh);
    hn_node_box(target, NULL, &tby, NULL, NULL);
    check(tby - sy1 <= vby + vbh + 1.0f, "目标元素滚进容器可视区");

    /* 无链接处点击: 无动作且不污染 pending */
    hn_node *p1 = find_tag_with_text(root, "p", "占位段落甲");
    hn_node_box(p1, &x, &y, &w, &h);
    r = hn_context_activate_link(c, x + w / 2, y + h / 2);
    check_int(r, HN_LINK_NONE, "普通段落点击 → HN_LINK_NONE");
    check_int(hn_doc_take_pending_link(doc, link, sizeof(link)), 0, "无链接点击不置 pending");

    /* 直调入口: 不存在的 id 返回 0 */
    check_int(hn_doc_scroll_to(doc, "no-such-id"), 0, "scroll_to 未知 id 返回 0");
    check_int(hn_doc_validate(doc, 0), 0, "链接用例 DOM 自检");
    hn_context_destroy(c);
}

/* ---------- 用例八: 围栏代码块语法高亮(最小 tokenizer 三色) ---------- */

static hn_node *find_span_cls(hn_node *n, const char *cls) {
    if (!n) return NULL;
    const char *t = hn_node_tag(n);
    if (t && !strcmp(t, "span")) {
        const char *cl = hn_node_attr(n, "class");
        if (cl && strstr(cl, cls)) return n;
    }
    for (hn_node *c2 = hn_node_first_child(n); c2; c2 = hn_node_next_sibling(c2)) {
        hn_node *r = find_span_cls(c2, cls);
        if (r) return r;
    }
    return NULL;
}

static int count_span_cls(hn_node *n, const char *cls) {
    if (!n) return 0;
    int k = 0;
    const char *t = hn_node_tag(n);
    if (t && !strcmp(t, "span")) {
        const char *cl = hn_node_attr(n, "class");
        if (cl && strstr(cl, cls)) k++;
    }
    for (hn_node *c2 = hn_node_first_child(n); c2; c2 = hn_node_next_sibling(c2))
        k += count_span_cls(c2, cls);
    return k;
}

static void test_highlight(void) {
    printf("== Markdown 直染: 代码高亮 ==\n");
    /* python: 关键字(def/return) + 字符串 + 注释 + 数字, 四类齐全 */
    const char *SRC =
        "<html><body><div hn-md>\n"
        "```python\n"
        "def render(doc):      # 注释说明\n"
        "    s = \"字符串字面量\"\n"
        "    return 42\n"
        "```\n"
        "</div></body></html>";
    const char *RAW = "def render(doc):      # 注释说明\n    s = \"字符串字面量\"\n    return 42";
    hn_doc *doc = NULL;
    hn_context *c = load(SRC, &doc);
    if (!c) { check(0, "高亮用例装载"); return; }
    hn_node *root = hn_doc_root(doc);
    hn_node *code = find_tag(root, "pre");
    code = code ? child_tag(code, "code") : NULL;
    check(code != NULL, "高亮: pre>code 存在");
    check_str(code ? hn_node_attr(code, "data-lang") : NULL, "python", "data-lang=python");

    hn_node *kk = find_span_cls(code, "tok-k");
    hn_node *ss = find_span_cls(code, "tok-s");
    hn_node *cc = find_span_cls(code, "tok-c");
    check(kk != NULL, "存在 tok-k span(关键字/数字)");
    check(ss != NULL, "存在 tok-s span(字符串)");
    check(cc != NULL, "存在 tok-c span(注释)");
    check_int(count_span_cls(code, "tok-k"), 3, "tok-k 共 3 个(def/return/42)");
    if (ss) { char b[128]; node_text(ss, b, sizeof(b)); check_str(b, "\"字符串字面量\"", "tok-s 包住完整引号串"); }
    if (cc) { char b[128]; node_text(cc, b, sizeof(b)); check_str(b, "# 注释说明", "tok-c 包住整行注释"); }

    /* 文本内容逐字节无损: 高亮只是包一层 */
    char buf[512];
    node_text(code, buf, sizeof(buf));
    check(!strcmp(buf, RAW), "高亮后文本内容逐字节无损");
    check_int(hn_doc_validate(doc, 0), 0, "高亮用例 DOM 自检");
    hn_context_destroy(c);

    /* 未知语言: 只做字符串+注释(无关键字) */
    const char *SRC2 =
        "<html><body><div hn-md>\n"
        "```brainfuck\n"
        "say \"hi\" # note\n"
        "```\n"
        "</div></body></html>";
    doc = NULL;
    c = load(SRC2, &doc);
    if (!c) { check(0, "未知 lang 用例装载"); return; }
    root = hn_doc_root(doc);
    hn_node *pre2 = find_tag(root, "pre");
    hn_node *code2 = pre2 ? child_tag(pre2, "code") : NULL;
    check(code2 != NULL, "未知 lang: pre>code 存在");
    check(find_span_cls(code2, "tok-s") != NULL, "未知 lang: 仍有字符串高亮");
    check(find_span_cls(code2, "tok-c") != NULL, "未知 lang: 仍有注释高亮");
    check_int(count_span_cls(code2, "tok-k"), 0, "未知 lang: 无关键字高亮");
    hn_context_destroy(c);

    /* 无 lang 围栏: 同未知语言口径(字符串+注释) */
    const char *SRC3 =
        "<html><body><div hn-md>\n"
        "```\n"
        "x = \"v\"  // 斜杠注释\n"
        "```\n"
        "</div></body></html>";
    doc = NULL;
    c = load(SRC3, &doc);
    if (!c) { check(0, "无 lang 用例装载"); return; }
    root = hn_doc_root(doc);
    hn_node *pre3 = find_tag(root, "pre");
    hn_node *code3 = pre3 ? child_tag(pre3, "code") : NULL;
    check(find_span_cls(code3, "tok-s") != NULL, "无 lang: 字符串高亮");
    check(find_span_cls(code3, "tok-c") != NULL, "无 lang: // 注释高亮");
    hn_context_destroy(c);
}

/* ---------- 用例四: 回归(普通 HTML 零影响) ---------- */

static void test_regression(void) {
    printf("== 回归: 无 hn-md 的普通文档 ==\n");
    const char *SRC =
        "<html><head><meta name=\"hn-theme\" content=\"dark\"></head><body>"
        "<div class=\"card\">"
        "<h1>普通标题</h1>"
        "<p>普通段落</p>"
        "<div class=\"chip\">块</div>"
        "</div>"
        "</body></html>";
    hn_doc *doc = NULL;
    hn_context *c = load(SRC, &doc);
    if (!c) { check(0, "回归用例装载"); return; }
    hn_node *root = hn_doc_root(doc);
    check_int(count_tag(root, "pre"), 0, "无 hn-md 不引入 pre");
    check_int(count_tag(root, "table"), 0, "无 hn-md 不引入 table");
    check_int(count_tag(root, "blockquote"), 0, "无 hn-md 不引入 blockquote");
    hn_node *h1 = find_tag(root, "h1");
    check(h1 && text_is(h1, "普通标题"), "既有结构原样");
    int n_div_children = 0;
    for (hn_node *ch = hn_node_first_child(find_tag(root, "div")); ch; ch = hn_node_next_sibling(ch))
        n_div_children++;
    check_int(n_div_children, 3, "卡片子元素数量不变(h1/p/div)");
    check_int(hn_doc_validate(doc, 0), 0, "DOM 自检零问题");
    const hn_display_list *dl = hn_context_display_list(c);
    check(dl && dl->count > 0, "布局与绘制正常");
    hn_context_destroy(c);
}

/* ---------- 用例五: pre/<br> 测高(回归: 多行代码块不得被算成一行) ----------
 *
 * 背景: 布局的度量模式(measuring)曾对强制换行(<br>/pre 换行)不计数,
 * white-space:pre 的代码块在 flex 容器里被按"一行高"强制定高(stretch),
 * 底部被裁/下方内容重叠。断言口径:
 *   - pre 盒高 ≥ 行数 × 行高(行高取 run 自带的行盒高, 与字体无关);
 *   - 绘制行不超出盒底(不裁);
 *   - pre 之后兄弟元素的 y ≥ pre 底(不被重叠);
 *   - <br> 两行段落同断言。
 */

/* 从元素子树收集"不同 y_top 的行数"与最大行盒高。
   递归进子元素 —— 围栏代码块高亮后, 行内文本分布在 span(tok-*)的文本
   节点上, 只看直接子文本节点会把多行数成一行。空行没有 run, 不计入 ——
   总行数断言用源码行数另行给出。 */
typedef struct { float ys[256]; int n; float lh; } line_acc;

static void count_lines_walk(hn_node *el, line_acc *acc) {
    for (hn_node *ch = hn_node_first_child(el); ch; ch = hn_node_next_sibling(ch)) {
        if (hn_node_tag(ch)) { count_lines_walk(ch, acc); continue; }
        for (int i = 0; i < hn_node_run_count(ch) && acc->n < 256; i++) {
            float x, b, w, yt, h;
            hn_node_run_at(ch, i, &x, &b, &w, &yt, &h);
            if (h > acc->lh) acc->lh = h;
            int seen = 0;
            for (int k = 0; k < acc->n; k++)
                if (acc->ys[k] == yt) { seen = 1; break; }
            if (!seen) acc->ys[acc->n++] = yt;
        }
    }
}

static int count_lines(hn_node *el, float *lh_out) {
    line_acc acc;
    acc.n = 0;
    acc.lh = 0;
    count_lines_walk(el, &acc);
    if (lh_out) *lh_out = acc.lh;
    return acc.n;
}

static void test_pre_br_height(void) {
    printf("== Markdown 直染: pre/<br> 测高 ==\n");

    /* 场景 A: md fence 代码块(4 行)+ 后续段落 */
    const char *SRC_A =
        "<html><body><div hn-md>\n"
        "```c\n"
        "int main() {\n"
        "    return 0;\n"
        "}\n"
        "\n"
        "/* end */\n"
        "```\n"
        "\n"
        "fence 之后的段落。\n"
        "</div></body></html>";
    hn_doc *doc = NULL;
    hn_context *c = load(SRC_A, &doc);
    if (!c) { check(0, "pre 测高用例装载"); return; }
    hn_node *root = hn_doc_root(doc);
    hn_node *pre = find_tag(root, "pre");
    hn_node *code = pre ? child_tag(pre, "code") : NULL;
    hn_node *txt = code ? hn_node_first_child(code) : NULL;
    if (!txt) { check(0, "pre 测高: 找到 code 文本"); hn_context_destroy(c); return; }
    float lh = 0;
    int n_drawn = count_lines(code, &lh);   /* 空行无 run → 只数到非空行 */
    check_int(n_drawn, 4, "fence 代码块绘制 4 个非空行(空行不产 run)");
    check(lh > 0, "行盒高 > 0");
    float px, py, pw, ph;
    hn_node_box(code, &px, &py, &pw, &ph);
    /* 源码 5 行(含 1 空行): 空行同样占一行盒 —— 盒高必须按 5 行算 */
    check(ph >= 5.0f * lh - 1.0f,
          "pre 盒高 ≥ 源码行数×行高(含空行, 容差 1px)");
    /* 绘制行不得超出盒底(底部被裁的形态) */
    float max_bottom = 0;
    for (int i = 0; i < hn_node_run_count(txt); i++) {
        float x, b, w, yt, h;
        hn_node_run_at(txt, i, &x, &b, &w, &yt, &h);
        if (yt + h > max_bottom) max_bottom = yt + h;
    }
    check(max_bottom <= py + ph + 1.0f, "绘制行不超出 pre 盒底");
    /* pre 之后兄弟段落: 不被重叠 */
    hn_node *after = find_tag_with_text(root, "p", "fence 之后的段落");
    float ax, ay, aw, ah;
    hn_node_box(after, &ax, &ay, &aw, &ah);
    check(ay >= py + ph - 1.0f, "pre 之后兄弟 y ≥ pre 底(不被重叠)");
    check_int(hn_doc_validate(doc, 0), 0, "pre 测高用例 DOM 自检");
    hn_context_destroy(c);

    /* 场景 B: <br> 两行段落 + 后续段落 */
    const char *SRC_B =
        "<html><body>"
        "<p>first<br>second</p>"
        "<p id=\"after\">AFTER-BR</p>"
        "</body></html>";
    doc = NULL;
    c = load(SRC_B, &doc);
    if (!c) { check(0, "br 测高用例装载"); return; }
    root = hn_doc_root(doc);
    hn_node *p1 = find_tag(root, "p");
    lh = 0;
    int nlines = count_lines(p1, &lh);
    check_int(nlines, 2, "<br> 段落绘制 2 行");
    float bx, by, bw2, bh2;
    hn_node_box(p1, &bx, &by, &bw2, &bh2);
    check(bh2 >= (float)nlines * lh - 1.0f, "<br> 段落盒高 ≥ 2×行高");
    hn_node *after2 = hn_doc_find_by_id(doc, "after");
    float ax2, ay2, aw2, ah2;
    hn_node_box(after2, &ax2, &ay2, &aw2, &ah2);
    check(ay2 >= by + bh2 - 1.0f, "<br> 段落之后兄弟不被重叠");
    hn_context_destroy(c);

    /* 场景 C: flex 容器里的 pre 代码块(stretch 强制高不得裁掉多行) */
    const char *SRC_C =
        "<html><head><style>"
        ".row { display: flex; }"
        ".card { overflow: hidden; border-radius: 10; }"
        ".chead { padding: 7 12; }"
        ".cbody { padding: 10 14; white-space: pre; font-family: monospace; font-size: 12; line-height: 1.5; }"
        "</style></head><body>"
        "<div class=\"row\"><div class=\"card\" id=\"card\">"
        "<div class=\"chead\">HEAD</div>"
        "<div class=\"cbody\" id=\"cbody\">L1\nL2\nL3\nL4</div>"
        "</div></div>"
        "</body></html>";
    doc = NULL;
    c = load(SRC_C, &doc);
    if (!c) { check(0, "flex pre 用例装载"); return; }
    hn_node *card = hn_doc_find_by_id(doc, "card");
    hn_node *body_el = hn_doc_find_by_id(doc, "cbody");
    lh = 0;
    nlines = count_lines(body_el, &lh);
    check_int(nlines, 4, "flex 内 pre 绘制 4 行");
    float fx, fy, fw, fh;
    hn_node_box(card, &fx, &fy, &fw, &fh);
    /* 卡片高至少容纳 4 行 + 代码体内边距(20)+ 代码头一行 —— 即未被
       stretch 按一行度量强制压扁 */
    check(fh >= (float)nlines * lh + 20.0f + lh - 1.0f,
          "flex 内 pre 卡片高容纳全部行(stretch 不裁底)");
    hn_context_destroy(c);
}

int main(void) {
    printf("md_probe: Markdown 直染验收(确定性断言)\n");
    test_structure();
    test_inline_nesting();
    test_nested_list();
    test_multi_container();
    test_render();
    test_pre_br_height();
    test_images();
    test_links();
    test_highlight();
    test_regression();
    printf("\nmd_probe: %d 项断言, 失败 %d\n", g_pass + g_fail, g_fail);
    return g_fail ? 1 : 0;
}

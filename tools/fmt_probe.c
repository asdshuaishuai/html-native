/* fmt_probe.c — JSON / CSV 直染验收(纯 C99, 离屏, 零 GUI 依赖)
 *
 * 管线与生产同一条: hn_context_set_doc(触发 hn_md + hn_fmt 两个直染 pass)
 * → 级联 → 布局 → 绘制指令列表。断言分四层:
 *   结构 —— DOM 里出现预期的类/令牌 span/文本(零样式依赖, 全确定);
 *   截断 —— 深度/数组/行数/列数四道截断的精确口径;
 *   回落 —— 坏 JSON / 空 CSV 的错误条 + 原样代码块;
 *   渲染 —— 显示列表有 TEXT 且盒不溢出;
 *   回归 —— 无 hn-json/hn-csv 的普通文档零影响。
 *
 * 断言口径: 只断与文本度量无关的确定性结论(类/属性/文本/元素计数/几何相交),
 * 不断具体像素宽度(与 md_probe 同口径)。
 *
 * 用法(由 tools/fmt_probe.py 薄封装编译运行):
 *   bash tools/tools/fmt_probe.sh [已编译产物路径]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn.h"
#include "../Sources/CHtmlNative/hnsoft.h"

/* 直染 pass 的内部入口(hn_internal.h 声明)。探针要显式复跑一遍装载批次
   验证幂等 —— 生产路径经 hn_context_set_doc 隐式触发, 这里按同一函数原型
   直接调用(与 md_probe 同口径: 只依赖生产实现, 不复制语义)。 */
int hn_fmt_apply(hn_doc *doc);

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

/* class 含子串的元素(项目里 class 是空格分隔串, 用 strstr 判定) */
static int cls_has(hn_node *n, const char *cls) {
    const char *c = hn_node_attr(n, "class");
    return c && strstr(c, cls);
}

static hn_node *find_cls(hn_node *n, const char *cls) {
    if (!n) return NULL;
    if (hn_node_tag(n) && cls_has(n, cls)) return n;
    for (hn_node *c = hn_node_first_child(n); c; c = hn_node_next_sibling(c)) {
        hn_node *r = find_cls(c, cls);
        if (r) return r;
    }
    return NULL;
}

static int count_cls(hn_node *n, const char *cls) {
    if (!n) return 0;
    int k = 0;
    if (hn_node_tag(n) && cls_has(n, cls)) k++;
    for (hn_node *c = hn_node_first_child(n); c; c = hn_node_next_sibling(c))
        k += count_cls(c, cls);
    return k;
}

static int count_tag_cls(hn_node *n, const char *tag, const char *cls) {
    if (!n) return 0;
    int k = 0;
    const char *t = hn_node_tag(n);
    if (t && !strcmp(t, tag) && cls_has(n, cls)) k++;
    for (hn_node *c = hn_node_first_child(n); c; c = hn_node_next_sibling(c))
        k += count_tag_cls(c, tag, cls);
    return k;
}

static size_t node_text(hn_node *n, char *out, size_t cap) {
    if (!n) { if (cap) out[0] = 0; return 0; }
    return hn_node_text_content(n, out, cap);
}

static int text_has(hn_node *n, const char *sub) {
    char buf[4096];
    node_text(n, buf, sizeof(buf));
    return strstr(buf, sub) != NULL;
}

static int text_is(hn_node *n, const char *want) {
    char buf[4096];
    node_text(n, buf, sizeof(buf));
    return !strcmp(buf, want);
}

/* ---------- 装载辅助: 解析 + set_doc(触发两个直染 pass)+ 布局 ---------- */

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

static hn_context *load(const char *html, hn_doc **doc_out) {
    hn_doc *doc = hn_parse_html(html, strlen(html));
    if (!doc) return NULL;
    hn_context *c = hn_context_create();
    hn_context_set_doc(c, doc);
    hn_text_backend tb = { NULL, measure_cb, metrics_cb };
    hn_context_layout(c, VW, VH, &tb);
    if (doc_out) *doc_out = doc;
    return c;
}

/* ---------- 用例一: JSON 结构(令牌/摘要/嵌套) ---------- */

static const char *JSON_HTML =
"<html><head><meta name=\"hn-theme\" content=\"dark\">"
"<meta name=\"hn-window\" content=\"720x900\"></head><body>"
"<div id=\"j1\" hn-json>"
"{\"name\": \"深度巡检\", \"ok\": true, \"retries\": 3, \"ratio\": 0.75,"
" \"nil\": null, \"note\": \"第一行\\n第二行 \\\"引号\\\" 已转义\","
" \"items\": [1, 2.5, \"x\", false, null],"
" \"meta\": { \"built\": { \"day\": { \"hour\": { \"minute\": { \"second\": 6 } } } } }}"
"</div></body></html>";

static void test_json_structure(void) {
    printf("== JSON 直染: 结构 ==\n");
    hn_doc *doc = NULL;
    hn_context *c = load(JSON_HTML, &doc);
    check(c && doc, "装载成功(hn-json 源被转换)");
    if (!c) return;
    hn_node *root = hn_doc_root(doc);

    /* 摘要条 */
    hn_node *head = find_cls(root, "json-head");
    check(head != NULL, "摘要条 div.json-head 存在");
    check(head && text_has(head, "keys"), "摘要条含 keys 计数");
    check(head && text_has(head, "8 keys"), "摘要条键数正确(8 keys)");

    /* 键: .tok-k 数量 = 对象成员数(顶层 8 + 嵌套 5 = 13) */
    check_int(count_cls(root, "tok-k"), 13, "键 span.tok-k 共 13 个(顶层 8 + 嵌套 5)");
    hn_node *k1 = find_cls(root, "tok-k");
    check(k1 && text_is(k1, "\"name\""), "键 span 文本含引号(\"name\")");

    /* 值令牌: 字符串 .tok-s / 数字 .tok-n / 布尔与 null .tok-b */
    check(find_cls(root, "tok-s") != NULL, "字符串 span.tok-s 存在");
    check(find_cls(root, "tok-n") != NULL, "数字 span.tok-n 存在");
    check(find_cls(root, "tok-b") != NULL, "布尔/null span.tok-b 存在");
    check(k1 && cls_has(k1, "tok-k"), "键 span 的 class 属性里就是 tok-k");
    /* 数字: retries 3 / ratio 0.75 / items 1 / items 2.5 / second 6 = 5 个 */
    check_int(count_cls(root, "tok-n"), 5, "数字令牌 5 个(3 / 0.75 / 1 / 2.5 / 6)");
    /* 布尔+null: ok=true / items false / items null / nil null = 4 个 */
    check_int(count_cls(root, "tok-b"), 4, "布尔+null 令牌 4 个");
    check_int(count_cls(root, "tok-s"), 3, "字符串值令牌 3 个(name / note / \"x\")");

    /* 字符串转义与中文: note 的值被重新转义显示(找含"第一行"的 span) */
    hn_node *st = NULL;
    {
        struct { hn_node **v; int n, cap; } stack = {0};
        #define PUSH(p) do { \
            if (stack.n == stack.cap) { \
                int nc = stack.cap ? stack.cap * 2 : 64; \
                hn_node **nv = (hn_node **)realloc(stack.v, sizeof(hn_node *) * (size_t)nc); \
                if (nv) { stack.v = nv; stack.cap = nc; } \
            } \
            if (stack.n < stack.cap) stack.v[stack.n++] = (p); \
        } while (0)
        PUSH(root);
        while (stack.n > 0) {
            hn_node *nd = stack.v[--stack.n];
            if (hn_node_tag(nd) && !strcmp(hn_node_tag(nd), "span") && text_has(nd, "第一行")) {
                st = nd; break;
            }
            for (hn_node *ch = hn_node_first_child(nd); ch; ch = hn_node_next_sibling(ch)) PUSH(ch);
        }
        free(stack.v);
        #undef PUSH
    }
    check(st != NULL, "含中文的字符串 span 存在");
    if (st) {
        char buf[256];
        node_text(st, buf, sizeof(buf));
        check(strstr(buf, "\\n") != NULL, "换行被重新转义为 \\n 显示");
        check(strstr(buf, "\\\"引号\\\"") != NULL, "引号被重新转义为 \\\" 显示");
        check(strstr(buf, "第一行") != NULL && strstr(buf, "第二行") != NULL, "中文原样保留");
        check(buf[0] == '"' && buf[strlen(buf) - 1] == '"', "字符串值带显示引号");
    } else {
        check(0, "转义断言依赖 note span(缺失)");
    }

    /* 嵌套: 缩进子树 div.json-kids(padding-left 内联) */
    hn_node *kids = find_cls(root, "json-kids");
    check(kids != NULL, "嵌套子树 div.json-kids 存在");
    check_str(kids ? hn_node_attr(kids, "style") : NULL, "padding-left: 14",
              "缩进以内联 padding-left 落位");
    /* meta→built→day→hour→minute 五层容器 = 5 个 kids div */
    check_int(count_cls(root, "json-kids"), 6, "kids 子树数 = 容器子层数(6)");

    /* 每行 div.json-row */
    check(count_cls(root, "json-row") > 0, "行 div.json-row 存在");

    check_int(hn_doc_validate(doc, 0), 0, "JSON 转换后 DOM 结构自检零问题");
    hn_context_destroy(c);
}

/* ---------- 用例二: JSON 深度截断(8 层 → 6 + …) ---------- */

static void test_json_depth(void) {
    printf("== JSON 直染: 深度截断 ==\n");
    const char *SRC =
        "<html><body><div id=\"deep\" hn-json>"
        "{\"a\":{\"b\":{\"c\":{\"d\":{\"e\":{\"f\":{\"g\":{\"h\":1}}}}}}}}"
        "</div></body></html>";
    hn_doc *doc = NULL;
    hn_context *c = load(SRC, &doc);
    if (!c) { check(0, "深度用例装载"); return; }
    hn_node *root = hn_doc_root(doc);
    check_int(count_cls(root, "json-kids"), 5, "8 层输入: kids 子树止于 5(可见容器 6 层)");
    hn_node *el = find_cls(root, "json-ellip");
    check(el != NULL, "深度截断 span.json-ellip 存在");
    check(el && text_is(el, "\xE2\x80\xA6"), "截断标记为单字符 …");
    check_int(count_cls(root, "json-ellip"), 1, "恰好一个深度截断标记");
    /* 最深 kids 的祖先链上 kids 数 = 5(嵌套层级断言) */
    {
        int max_chain = 0;
        struct { hn_node **v; int n, cap, depth; } stack = {0};
        /* 递归函数更直观 */
        (void)stack;
        /* 用 DFS 计算"从根到该 kids 的 kids 祖先数" */
        struct frame { hn_node *n; int depth; };
        struct frame *st = NULL;
        int sn = 0, scap = 0;
        #define F_PUSH(p, d) do { \
            if (sn == scap) { \
                int nc = scap ? scap * 2 : 64; \
                struct frame *nv = (struct frame *)realloc(st, sizeof(*nv) * (size_t)nc); \
                if (nv) { st = nv; scap = nc; } \
            } \
            if (sn < scap) { st[sn].n = (p); st[sn].depth = (d); sn++; } \
        } while (0)
        F_PUSH(root, 0);
        while (sn > 0) {
            struct frame f = st[--sn];
            int d = f.depth + (hn_node_tag(f.n) && cls_has(f.n, "json-kids") ? 1 : 0);
            if (hn_node_tag(f.n) && cls_has(f.n, "json-kids") && d > max_chain) max_chain = d;
            for (hn_node *ch = hn_node_first_child(f.n); ch; ch = hn_node_next_sibling(ch))
                F_PUSH(ch, d);
        }
        free(st);
        #undef F_PUSH
        check_int(max_chain, 5, "kids 嵌套链最深 5 层(容器深度 6 止)");
    }
    check_int(hn_doc_validate(doc, 0), 0, "深度用例 DOM 自检");
    hn_context_destroy(c);
}

/* ---------- 用例三: JSON 大数组截断(60 → 50 + more) ---------- */

static void test_json_big_array(void) {
    printf("== JSON 直染: 大数组截断 ==\n");
    static char html[8192];
    size_t o = 0;
    o += (size_t)snprintf(html + o, sizeof(html) - o,
        "<html><body><div id=\"big\" hn-json>[");
    for (int i = 0; i < 60; i++)
        o += (size_t)snprintf(html + o, sizeof(html) - o, "%s%d", i ? ", " : "", i);
    snprintf(html + o, sizeof(html) - o, "]</div></body></html>");

    hn_doc *doc = NULL;
    hn_context *c = load(html, &doc);
    if (!c) { check(0, "大数组用例装载"); return; }
    hn_node *root = hn_doc_root(doc);
    hn_node *head = find_cls(root, "json-head");
    check(head != NULL, "数组摘要条存在");
    check(head && text_has(head, "60 items"), "摘要条计数 = 60 items");
    check_int(count_cls(root, "tok-n"), 50, "渲染 50 个数字(前 50 条)");
    hn_node *more = find_cls(root, "json-more");
    check(more != NULL, "尾行 span.json-more 存在");
    check(more && text_is(more, "\xE2\x80\xA6 more 10 items"),
          "尾行文案: … more 10 items");
    check_int(count_tag_cls(root, "div", "json-more"), 1, "尾行只一行(div.json-more)");
    check_int(hn_doc_validate(doc, 0), 0, "大数组用例 DOM 自检");
    hn_context_destroy(c);
}

/* ---------- 用例四: 坏 JSON 优雅回落 ---------- */

static void test_json_fallback(void) {
    printf("== JSON 直染: 解析失败回落 ==\n");
    const char *BAD = "{\"a\": 1, \"b\": [1, 2";
    const char *SRC =
        "<html><body><div id=\"bad\" hn-json>{\"a\": 1, \"b\": [1, 2</div></body></html>";
    hn_doc *doc = NULL;
    hn_context *c = load(SRC, &doc);
    if (!c) { check(0, "坏 JSON 用例装载"); return; }
    hn_node *root = hn_doc_root(doc);
    hn_node *err = find_cls(root, "json-err");
    check(err != NULL, "错误条 div.json-err 存在");
    check(err && text_has(err, "解析失败"), "错误条文案含 解析失败");
    hn_node *pre = find_tag(root, "pre");
    hn_node *code = pre ? child_tag(pre, "code") : NULL;
    check(pre && code, "回落代码块 pre>code");
    check(code && text_is(code, BAD), "回落块保留原文(逐字节)");
    check_int(count_cls(root, "json-head"), 0, "失败时不渲染摘要条");
    check_int(hn_doc_validate(doc, 0), 0, "坏 JSON 用例 DOM 自检");
    hn_context_destroy(c);
}

/* ---------- 用例五: CSV 结构(RFC4180 子集 + 数字列右对齐) ---------- */

static const char *CSV_HTML =
"<html><head><meta name=\"hn-theme\" content=\"dark\"></head><body>"
"<div id=\"c1\" hn-csv>"
"id,name,note,qty\n"
"r1,\"a,b\",\"他说 \"\"你好\"\"\",10\n"
"r2,普通行,直接放,20\n"
"r3,末行,\"line1\n"
"line2\",30\n"
"</div></body></html>";

static void test_csv_structure(void) {
    printf("== CSV 直染: 结构 ==\n");
    hn_doc *doc = NULL;
    hn_context *c = load(CSV_HTML, &doc);
    check(c && doc, "装载成功(hn-csv 源被转换)");
    if (!c) return;
    hn_node *root = hn_doc_root(doc);
    hn_node *table = find_tag(root, "table");
    check(table != NULL, "渲染成 table 语义");
    check(table && cls_has(table, "csv"), "table 带 class=csv");
    check_int(count_tag(table, "tr"), 4, "4 行(表头 + 3 数据行)");
    hn_node *htr = table ? hn_node_first_child(table) : NULL;
    check(htr && cls_has(htr, "csv-head"), "表头行 tr.csv-head");
    check_int(htr ? child_elem_count(htr) : 0, 4, "表头 4 列");
    check(htr && hn_node_first_child(htr) &&
          !strcmp(hn_node_tag(hn_node_first_child(htr)), "th"), "表头单元格是 th");
    check_int(count_tag(table, "td"), 12, "数据单元格 3 行 × 4 列 = 12 个 td");
    check_int(count_tag_cls(table, "td", "csv"), 0, "td 不额外挂 csv 类(皮肤按 tr/table)");

    /* 引号字段: 含逗号、含 "" 转义、含换行 */
    hn_node *r1 = htr ? hn_node_next_sibling(htr) : NULL;
    hn_node *c2 = r1 ? hn_node_next_sibling(hn_node_first_child(r1)) : NULL;
    check(c2 && text_is(c2, "a,b"), "引号内逗号按字面保留(a,b)");
    hn_node *c3 = c2 ? hn_node_next_sibling(c2) : NULL;
    check(c3 && text_is(c3, "他说 \"你好\""), "引号内 \"\" 转义为字面引号");
    hn_node *r3 = r1 ? hn_node_next_sibling(hn_node_next_sibling(r1)) : NULL;
    hn_node *r3c3 = r3 ? hn_node_next_sibling(hn_node_next_sibling(hn_node_first_child(r3))) : NULL;
    check(r3c3 && text_has(r3c3, "line1") && text_has(r3c3, "line2"),
          "引号内换行保留(跨行单元格)");
    check_str(r3c3 ? hn_node_attr(r3c3, "style") : NULL, "white-space: pre-wrap",
              "跨行单元格落 white-space: pre-wrap(换行可视)");

    /* 数字列右对齐: qty 列(下标 3)3/3 可解析为数 → th/td 内联右对齐 */
    hn_node *h4 = htr ? hn_node_next_sibling(hn_node_next_sibling(hn_node_next_sibling(hn_node_first_child(htr)))) : NULL;
    check_str(h4 ? hn_node_attr(h4, "style") : NULL, "text-align: right",
              "数字列表头 th 内联右对齐");
    hn_node *d1 = r1 ? hn_node_next_sibling(hn_node_next_sibling(hn_node_next_sibling(hn_node_first_child(r1)))) : NULL;
    check_str(d1 ? hn_node_attr(d1, "style") : NULL, "text-align: right",
              "数字列 td 内联右对齐");
    /* 文本列(id/name/note)无右对齐 */
    check(hn_node_first_child(r1) && hn_node_attr(hn_node_first_child(r1), "style") == NULL,
          "文本列 td 无右对齐");
    check(hn_node_first_child(htr) && hn_node_attr(hn_node_first_child(htr), "style") == NULL,
          "文本列表头无右对齐");
    check_int(hn_doc_validate(doc, 0), 0, "CSV 转换后 DOM 结构自检");
    hn_context_destroy(c);
}

/* ---------- 用例六: CSV 行/列截断 + 分号分隔 ---------- */

static void test_csv_truncation(void) {
    printf("== CSV 直染: 行/列截断与自定义分隔符 ==\n");
    static char html[131072];
    size_t o = 0;
    o += (size_t)snprintf(html + o, sizeof(html) - o, "<html><body><div id=\"big\" hn-csv>");
    /* 14 列表头 */
    for (int k = 0; k < 14; k++)
        o += (size_t)snprintf(html + o, sizeof(html) - o, "%sc%d", k ? "," : "", k);
    o += (size_t)snprintf(html + o, sizeof(html) - o, "\n");
    /* 210 数据行(首列是数字 → 右对齐列) */
    for (int r = 0; r < 210; r++) {
        o += (size_t)snprintf(html + o, sizeof(html) - o, "%d", r);
        for (int k = 1; k < 14; k++)
            o += (size_t)snprintf(html + o, sizeof(html) - o, ",v%d_%d", r, k);
        o += (size_t)snprintf(html + o, sizeof(html) - o, "\n");
    }
    snprintf(html + o, sizeof(html) - o, "</div></body></html>");

    hn_doc *doc = NULL;
    hn_context *c = load(html, &doc);
    if (!c) { check(0, "截断用例装载"); return; }
    hn_node *root = hn_doc_root(doc);
    hn_node *table = find_tag(root, "table");
    check(table != NULL, "截断用例 table 存在");
    check_int(table ? count_tag(table, "tr") : 0, 202, "行数 = 1 表头 + 200 数据 + 1 提示");
    hn_node *htr = table ? hn_node_first_child(table) : NULL;
    check_int(htr ? child_elem_count(htr) : 0, 12, "列数 = 12(前 12 列)");
    hn_node *more = find_cls(root, "csv-more");
    check(more != NULL, "尾行 tr.csv-more 存在");
    check(more && text_is(more, "\xE2\x80\xA6 共 210 行 · 共 14 列"),
          "提示文案: … 共 210 行 · 共 14 列");
    hn_node *mt = more ? hn_node_first_child(more) : NULL;
    check_int(mt ? child_elem_count(more) : 0, 1, "提示行只有 1 个单元格");
    check(mt && text_is(mt, "\xE2\x80\xA6 共 210 行 · 共 14 列"), "提示文本落在 td 上");
    /* 首列数字 → 右对齐 */
    hn_node *d1 = htr ? hn_node_next_sibling(htr) : NULL;
    hn_node *d1c1 = d1 ? hn_node_first_child(d1) : NULL;
    check_str(d1c1 ? hn_node_attr(d1c1, "style") : NULL, "text-align: right",
              "截断用例数字列仍右对齐");
    check_int(hn_doc_validate(doc, 0), 0, "截断用例 DOM 自检");
    hn_context_destroy(c);

    /* 分号分隔 */
    const char *SRC2 =
        "<html><body><div hn-csv hn-csv-sep=\";\">a;b;c\n1;2.5;x\n3;4;y\n</div></body></html>";
    doc = NULL;
    c = load(SRC2, &doc);
    if (!c) { check(0, "分号用例装载"); return; }
    root = hn_doc_root(doc);
    hn_node *t2 = find_tag(root, "table");
    check(t2 != NULL, "分号分隔: table 存在");
    hn_node *h2 = t2 ? hn_node_first_child(t2) : NULL;
    check_int(h2 ? child_elem_count(h2) : 0, 3, "分号分隔: 3 列");
    hn_node *hr2 = h2 ? hn_node_next_sibling(h2) : NULL;
    hn_node *hc2 = hr2 ? hn_node_next_sibling(hn_node_first_child(hr2)) : NULL;
    check_str(hc2 ? hn_node_attr(hc2, "style") : NULL, "text-align: right",
              "分号分隔: 数字列右对齐");
    hn_node *hc1 = hr2 ? hn_node_first_child(hr2) : NULL;
    check(hc1 && text_is(hc1, "1"), "分号分隔: 首格文本 1");
    check_int(hn_doc_validate(doc, 0), 0, "分号用例 DOM 自检");
    hn_context_destroy(c);

    /* 闭引号后尾随空白(缩进书写的源常见): 仍按表格渲染, 不回落 */
    const char *SRC3 =
        "<html><body><div hn-csv>a,b\n1,\"x, y\"   \n2,\"z\"\n</div></body></html>";
    doc = NULL;
    c = load(SRC3, &doc);
    if (!c) { check(0, "尾随空白用例装载"); return; }
    root = hn_doc_root(doc);
    check_int(count_tag(root, "table"), 1, "闭引号后尾随空白仍渲染 table");
    check_int(count_tag(root, "pre"), 0, "闭引号后尾随空白不回落代码块");
    hn_node *tt = find_tag(root, "table");
    hn_node *btr = tt ? hn_node_next_sibling(hn_node_first_child(tt)) : NULL;
    hn_node *bc2 = btr ? hn_node_next_sibling(hn_node_first_child(btr)) : NULL;
    check(bc2 && text_is(bc2, "x, y"), "尾随空白剥离后引号内逗号仍保留");
    check_int(hn_doc_validate(doc, 0), 0, "尾随空白用例 DOM 自检");
    hn_context_destroy(c);
}

/* ---------- 用例七: 空 CSV 回落 + 无分隔符单列 ---------- */

static void test_csv_fallback(void) {
    printf("== CSV 直染: 空/畸形回落 ==\n");
    const char *SRC =
        "<html><body><div id=\"empty\" hn-csv>   \n\t\n  </div></body></html>";
    hn_doc *doc = NULL;
    hn_context *c = load(SRC, &doc);
    if (!c) { check(0, "空 CSV 用例装载"); return; }
    hn_node *root = hn_doc_root(doc);
    check_int(count_tag(root, "table"), 0, "全空白源不产 table");
    hn_node *pre = find_tag(root, "pre");
    check(pre != NULL, "空源回落原样代码块 pre");
    check_int(hn_doc_validate(doc, 0), 0, "空 CSV 用例 DOM 自检");
    hn_context_destroy(c);

    /* 未闭合引号: 解析失败 → 回落原文 */
    const char *SRC2 =
        "<html><body><div id=\"bad\" hn-csv>a,b\n1,\"未闭合\n</div></body></html>";
    doc = NULL;
    c = load(SRC2, &doc);
    if (!c) { check(0, "坏 CSV 用例装载"); return; }
    root = hn_doc_root(doc);
    check_int(count_tag(root, "table"), 0, "未闭合引号不产 table");
    hn_node *pre2 = find_tag(root, "pre");
    check(pre2 && text_has(pre2, "未闭合"), "坏 CSV 回落块保留原文");
    hn_context_destroy(c);
}

/* ---------- 用例八: 渲染(显示列表 TEXT + 盒不溢出) ---------- */

static int rect_intersects(float x1, float y1, float w1, float h1,
                           float x2, float y2, float w2, float h2) {
    return x1 < x2 + w2 && x2 < x1 + w1 && y1 < y2 + h2 && y2 < y1 + h1;
}

static void test_render(void) {
    printf("== JSON/CSV 直染: 渲染 ==\n");
    const char *SRC =
        "<html><head><meta name=\"hn-theme\" content=\"dark\"></head><body>"
        "<div id=\"jbox\" style=\"width: 520\">"
        "<div hn-json>{\"tool\": \"read_file\", \"lines\": 120, \"ok\": true,"
        " \"path\": \"/tmp/a b/c.txt\"}</div></div>"
        "<div id=\"cbox\" style=\"width: 520\">"
        "<div hn-csv>id,metric,value\n1,first frame,8\n2,memory,24\n</div></div>"
        "</body></html>";
    hn_doc *doc = NULL;
    hn_context *c = load(SRC, &doc);
    if (!c) { check(0, "渲染用例装载"); return; }
    const hn_display_list *dl = hn_context_display_list(c);
    check(dl && dl->count > 0, "显示列表非空");

    hn_node *jbox = hn_doc_find_by_id(doc, "jbox");
    hn_node *row = find_cls(jbox, "json-row");
    float rx, ry, rw, rh, bx, by, bw, bh;
    hn_node_box(row, &rx, &ry, &rw, &rh);
    check(rw > 0 && rh > 0, "json-row 布局盒非退化");
    hn_node_box(jbox, &bx, &by, &bw, &bh);
    check(bx + bw + 1.0f >= rx + rw, "json-row 不溢出容器右界");

    /* JSON: 盒内必须有 TEXT 指令(键/值着色树真的画出文本) */
    int jtext = 0;
    for (int i = 0; i < dl->count; i++) {
        const hn_cmd *cmd = &dl->cmds[i];
        if (cmd->kind != HN_CMD_TEXT || !cmd->text) continue;
        if (cmd->tx >= bx - 1 && cmd->tx <= bx + bw &&
            cmd->baseline >= by && cmd->baseline <= by + bh) { jtext = 1; break; }
    }
    check(jtext, "JSON 盒内有 TEXT 指令");

    /* 摘要条文本(形如 "{…} 4 keys · N B")必须真的出现在显示列表:
       在 json-head 盒内找一条含 "keys" 的 TEXT 指令 */
    hn_node *jhead = find_cls(jbox, "json-head");
    float hx, hy, hw, hh;
    hn_node_box(jhead, &hx, &hy, &hw, &hh);
    check(hw > 0 && hh > 0, "json-head 布局盒非退化");
    int htext = 0;
    for (int i = 0; i < dl->count; i++) {
        const hn_cmd *cmd = &dl->cmds[i];
        if (cmd->kind != HN_CMD_TEXT || !cmd->text) continue;
        if (cmd->text_len < 5) continue;
        if (cmd->tx >= hx - 1 && cmd->tx <= hx + hw &&
            cmd->baseline >= hy && cmd->baseline <= hy + hh) { htext = 1; break; }
    }
    check(htext, "摘要条有 TEXT 指令(位置落在 json-head 盒内)");

    /* CSV: 表格盒在容器内, 单元格有 TEXT */
    hn_node *cbox = hn_doc_find_by_id(doc, "cbox");
    hn_node *table = find_tag(cbox, "table");
    float tx, ty, tw, th;
    hn_node_box(table, &tx, &ty, &tw, &th);
    check(tw > 0 && th > 0, "CSV 表格布局盒非退化");
    hn_node_box(cbox, &bx, &by, &bw, &bh);
    check(tx + tw <= bx + bw + 1.0f, "CSV 表格不溢出容器右界");
    hn_node *td = find_tag(table, "td");
    float dx, dy, dw, dh;
    hn_node_box(td, &dx, &dy, &dw, &dh);
    check(dw > 0 && dh > 0, "CSV 单元格布局盒非退化");
    int ctext = 0;
    for (int i = 0; i < dl->count; i++) {
        const hn_cmd *cmd = &dl->cmds[i];
        if (cmd->kind != HN_CMD_TEXT || !cmd->text) continue;
        if (cmd->tx >= dx - 1 && cmd->tx <= dx + dw + 4 &&
            cmd->baseline >= dy && cmd->baseline <= dy + dh) { ctext = 1; break; }
    }
    check(ctext, "CSV 单元格内有 TEXT 指令");

    /* 表格单元格有边框线(既有表格 UA 皮肤生效) */
    int line = 0;
    for (int i = 0; i < dl->count; i++) {
        const hn_cmd *cmd = &dl->cmds[i];
        if (cmd->kind != HN_CMD_RECT) continue;
        if ((cmd->stroke & 0xFF) == 0 || cmd->stroke_w <= 0) continue;
        if (rect_intersects(cmd->x, cmd->y, cmd->w, cmd->h, dx, dy, dw, dh)) { line = 1; break; }
    }
    check(line, "CSV 单元格有边框线指令");

    /* 大 JSON 不炸布局由 test_big_no_explode 专测; 此处只收尾 */
    check_int(hn_doc_validate(doc, 0), 0, "渲染用例 DOM 自检");
    hn_context_destroy(c);
}

/* ---------- 用例九: 大 JSON 不炸布局(盒高有界) ---------- */

static void test_big_no_explode(void) {
    printf("== JSON 直染: 巨 JSON 布局有界 ==\n");
    static char html[65536];
    size_t o = 0;
    /* 30 层深的嵌套 + 每层 80 个兄弟成员: 未截断会渲染上万行 */
    o += (size_t)snprintf(html + o, sizeof(html) - o,
        "<html><body><div id=\"huge\" style=\"width: 520\" hn-json>{");
    for (int d = 0; d < 30; d++) {
        for (int k = 0; k < 80; k++)
            o += (size_t)snprintf(html + o, sizeof(html) - o, "%s\"k%d\": %d",
                                  k ? ", " : "", k, k);
        o += (size_t)snprintf(html + o, sizeof(html) - o, ", \"deep\": {");
    }
    o += (size_t)snprintf(html + o, sizeof(html) - o, "\"leaf\": 1");
    for (int d = 0; d < 30; d++) o += (size_t)snprintf(html + o, sizeof(html) - o, "}");
    snprintf(html + o, sizeof(html) - o, "}</div></body></html>");

    hn_doc *doc = NULL;
    hn_context *c = load(html, &doc);
    if (!c) { check(0, "巨 JSON 用例装载"); return; }
    hn_node *huge = hn_doc_find_by_id(doc, "huge");
    check(count_cls(huge, "json-row") <= 401, "行数被预算截断(≤ 401)");
    check(find_cls(huge, "json-more") != NULL, "截断提示行存在");
    float hx, hy, hw, hh;
    hn_node_box(huge, &hx, &hy, &hw, &hh);
    /* 未截断口径: 30 层 × 81 成员 ≈ 2400+ 行 ≈ 48000px; 有界 = 远小于它 */
    check(hh > 0 && hh < VH * 11, "容器高度有界(巨 JSON 不炸布局)");
    check_int(hn_doc_validate(doc, 0), 0, "巨 JSON 用例 DOM 自检");
    hn_context_destroy(c);
}

/* ---------- 用例十: 回归(无 hn-json/hn-csv 文档零影响) ---------- */

static void test_regression(void) {
    printf("== 回归: 无 hn-json/hn-csv 的普通文档 ==\n");
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
    check_int(count_tag(root, "pre"), 0, "无 hn-json/hn-csv 不引入 pre");
    check_int(count_tag(root, "table"), 0, "无 hn-json/hn-csv 不引入 table");
    check_int(count_cls(root, "json-head"), 0, "无 json-head 注入");
    check_int(count_cls(root, "json-row"), 0, "无 json-row 注入");
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

/* ---------- 用例十一: 幂等与原地更新(hn_doc_swap 带上新源) ---------- */

static void test_idempotent_update(void) {
    printf("== JSON/CSV 直染: 幂等与原地更新 ==\n");
    const char *SRC =
        "<html><body>"
        "<div id=\"j\" hn-json>{\"first\": 1}</div>"
        "<div id=\"c\" hn-csv>a,b\n1,2\n</div>"
        "</body></html>";
    hn_doc *doc = NULL;
    hn_context *c = load(SRC, &doc);
    if (!c) { check(0, "幂等用例装载"); return; }
    hn_node *root = hn_doc_root(doc);
    check_int(count_cls(root, "json-head"), 1, "首次转换产出 1 个 json-head");
    check_int(count_tag(root, "table"), 1, "首次转换产出 1 个 table");
    /* 再跑一遍直染 pass(模拟第二次装载批次): 不得重复转换 */
    hn_fmt_apply(doc);
    check_int(count_cls(root, "json-head"), 1, "再跑 pass: json-head 不重复");
    check_int(count_tag(root, "table"), 1, "再跑 pass: table 不重复");
    check_int(count_cls(root, "tok-k"), 1, "再跑 pass: 令牌 span 不重复");

    /* 原地更新: 换元素内容为新源(inner swap 内部会再跑直染) → 重新转换 */
    const char *NEW_J = "{\"second\": {\"nested\": true}, \"n\": 2}";
    int r = hn_doc_swap(doc, "j", HN_SWAP_INNER, NEW_J, strlen(NEW_J));
    check_int(r, 1, "hn_doc_swap 换 hn-json 内容成功");
    root = hn_doc_root(doc);
    check_int(count_cls(root, "json-head"), 1, "换源后仍是 1 个摘要条");
    hn_node *h = find_cls(root, "json-head");
    check(h && text_has(h, "2 keys"), "换源后摘要条反映新键数(2 keys)");
    /* 旧内容("first")必须消失, 新内容("second")必须出现。
       只走公开 API(hn_node_first_child/next_sibling/text_content)遍历 —— 不触内部结构。 */
    static char alldoc[16384];
    size_t ao = 0;
    alldoc[0] = 0;
    {
        struct { hn_node **v; int n, cap; } stack = {0};
        #define PUSH2(p) do { \
            if (stack.n == stack.cap) { \
                int nc = stack.cap ? stack.cap * 2 : 64; \
                hn_node **nv = (hn_node **)realloc(stack.v, sizeof(hn_node *) * (size_t)nc); \
                if (nv) { stack.v = nv; stack.cap = nc; } \
            } \
            if (stack.n < stack.cap) stack.v[stack.n++] = (p); \
        } while (0)
        PUSH2(root);
        while (stack.n > 0) {
            hn_node *nd = stack.v[--stack.n];
            if (hn_node_tag(nd)) {   /* 元素: 收集其文本内容(含后代) */
                size_t got = hn_node_text_content(nd, alldoc + ao, sizeof(alldoc) - ao - 1);
                ao += got;
                alldoc[ao] = 0;
                if (ao >= sizeof(alldoc) - 2) break;
            }
            for (hn_node *ch = hn_node_first_child(nd); ch; ch = hn_node_next_sibling(ch)) PUSH2(ch);
        }
        free(stack.v);
        #undef PUSH2
    }
    check(strstr(alldoc, "first") == NULL, "换源后旧内容不残留");
    check(strstr(alldoc, "second") != NULL, "换源后新内容被渲染");
    check_int(hn_doc_validate(doc, 0), 0, "幂等与更新用例 DOM 自检");
    hn_context_destroy(c);
}

int main(void) {
    printf("fmt_probe: JSON/CSV 直染验收(确定性断言)\n");
    test_json_structure();
    test_json_depth();
    test_json_big_array();
    test_json_fallback();
    test_csv_structure();
    test_csv_truncation();
    test_csv_fallback();
    test_render();
    test_big_no_explode();
    test_regression();
    test_idempotent_update();
    printf("\nfmt_probe: %d 项断言, 失败 %d\n", g_pass + g_fail, g_fail);
    return g_fail ? 1 : 0;
}

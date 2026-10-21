/* freeze_l2_probe.c — 冻结布局 L2 前置(文本测量缓存)探针
 *
 * 口径(freeze-design.md §三): L2 = 布局只重排脏子树, 前置是"从缓存盒
 * 恢复"路径; 文本测量缓存是它的第一步 —— measure(字体五元组+文本字节 →
 * 宽度)是纯函数, 记忆化的命中浮点与重测严格相等。本探针钉住三条:
 *   A. 逐位一致: 同一文档 + 同一布局序列(初次 / 改宽重排 / 改一处文本
 *      再重排), 缓存关(HN_MEASURE_CACHE=0)与开(缺省)的布局结果转储
 *      逐字节相同 —— 元素盒 / 文本 runs / 显示列表全部按位(hex)打印,
 *      由 freeze_l2_probe.sh 对两份转储做 cmp。
 *   B. 命中: 第二遍起 hits>0; 改一处文本后的那遍布局, 旧文本仍然命中
 *      (hits 相比上一遍净增) —— "改一处, 其余零测量成本"的 L2 主张。
 *   C. 提速(信息值, 不做硬线 —— CPU 硬线归 perf_probe): 固定文档 300
 *      遍全量重排的开关计时对比(bench 模式)。
 *
 * 用法: freeze_l2_probe [dump|bench]
 *   dump(缺省): 布局序列 → stdout 转储(两模式必须逐字节一致) + stderr
 *               统计; 缓存应开而命中为 0 → 退出码 1。
 *   bench:      300 遍重排计时, stdout 一行 "bench ms=<毫秒>"。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/time.h>
#include "hn.h"
#include "hnsoft.h"
#include "hn_internal.h"

static int g_pass = 0, g_fail = 0;

static void check(int cond, const char *label) {
    /* 断言结果走 stderr: stdout 必须是纯转储(两模式 cmp 逐字节可比) */
    fprintf(stderr, "%s %s\n", cond ? "  ✔" : "  ✘", label);
    if (cond) g_pass++; else g_fail++;
}

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

/* ---- 测试文档: 测量密集(中英混排 × 4 组字体面 + 截断块 + flex 行),
   生成而非静态字符串 —— 保证突变句可参数化且无对齐巧合。 ---- */

static const char *g_sentences[] = {
    "渲染引擎把 HTML/CSS 级联成显示列表: 解析、选择器匹配、继承、布局、"
        "绘制指令十四种, 窗口即画布, 无浏览器也无运行时依赖, 二进制几百 KB。",
    "The quick brown fox jumps over the lazy dog while measuring every word "
        "against the FreeType face cache, 0123456789 and punctuation too.",
    "中英混排 mixed text 的行内排版 inline formatting context 按词贪心换行, "
        "跨节点共享行盒并保持基线对齐, 溢出时按 line-clamp 截断加省略号。",
    "冻结指令片段: 静态子树按全量指纹缓存, 命中帧整块 memcpy 回放跳过 "
        "paint walk; 指纹含计算样式、布局盒与滚动偏移, 变了即自动失效重录。",
};
static const char *g_classes[] = { "a", "b", "c", "d" };

static char *build_doc(int n_para) {
    size_t cap = (size_t)n_para * 256 + 1024;
    char *buf = (char *)malloc(cap);
    size_t off = 0;
    int i;
    off += (size_t)snprintf(buf + off, cap - off,
        "<html><head><style>"
        "html,body{margin:0;background:#fff}"
        "p{margin:6 12}"
        ".a{font-size:15px}"
        ".b{font-size:13px;font-family:monospace}"
        ".c{font-size:16px;font-style:italic;letter-spacing:2px}"
        ".d{font-size:12px;font-weight:700}"
        ".clamp{font-size:14px;line-clamp:2;height:44px;overflow:hidden}"
        ".row{display:flex;gap:6;width:380}"
        "</style></head><body>");
    for (i = 0; i < n_para; i++) {
        const char *cls = g_classes[i % 4];
        const char *txt = g_sentences[i % 4];
        off += (size_t)snprintf(buf + off, cap - off,
            "<p class=\"%s\" id=\"p%d\">%s #%d</p>", cls, i, txt, i);
    }
    off += (size_t)snprintf(buf + off, cap - off,
        "<div class=\"clamp\" id=\"clamp\">%s%s%s</div>"
        "<div class=\"row\">"
        "<span class=\"a\">甲</span><span class=\"b\">乙</span>"
        "<span class=\"c\">丙</span><span class=\"d\">丁</span>"
        "<span class=\"a\">戊</span><span class=\"b\">己</span>"
        "</div></body></html>",
        g_sentences[0], g_sentences[1], g_sentences[2]);
    return buf;
}

static hn_node *find_id(hn_node *n, const char *id) {
    if (n->kind == HN_ELEM && n->id && !strcmp(n->id, id)) return n;
    for (n = n->first; n; n = n->next) {
        hn_node *r = find_id(n, id);
        if (r) return r;
    }
    return NULL;
}

/* ---- 转储(逐字节可比): 浮点一律按位打印, 不经十进制往返 ---- */

static void pbits(FILE *f, float v) {
    unsigned u;
    memcpy(&u, &v, 4);
    fprintf(f, " %08x", u);
}

static void dump_node(hn_node *n, FILE *f) {
    if (n->kind == HN_ELEM) {
        int i;
        fprintf(f, "E %s %s bx", n->tag ? n->tag : "?", n->id ? n->id : "-");
        pbits(f, n->bx); pbits(f, n->by); pbits(f, n->bw); pbits(f, n->bh);
        pbits(f, n->content_h);
        fprintf(f, " runs=%d", n->n_runs);
        for (i = 0; i < n->n_runs; i++) {
            const hn_run *r = &n->runs[i];
            fprintf(f, " [%zu,%zu]", r->begin, r->end);
            pbits(f, r->x); pbits(f, r->width);
            pbits(f, r->baseline); pbits(f, r->y_top); pbits(f, r->h);
        }
        fputc('\n', f);
    } else {
        fprintf(f, "T %u ", (unsigned)n->text_len);
        if (n->text_len) fwrite(n->text, 1, n->text_len, f);
        fputc('\n', f);
    }
    for (n = n->first; n; n = n->next) dump_node(n, f);
}

static void dump_dl(const hn_display_list *dl, FILE *f) {
    int i;
    for (i = 0; i < dl->count; i++) {
        const hn_cmd *m = &dl->cmds[i];
        fprintf(f, "C %d", (int)m->kind);
        pbits(f, m->x); pbits(f, m->y); pbits(f, m->w); pbits(f, m->h);
        pbits(f, m->radius); pbits(f, m->stroke_w);
        fprintf(f, " %08x %08x", m->fill, m->stroke);
        pbits(f, m->tx); pbits(f, m->baseline);
        fprintf(f, " fz"); pbits(f, m->font.size_px);
        fprintf(f, " fw=%d fi=%d ff=%d", m->font.weight, m->font.italic,
                m->font.family);
        fprintf(f, " tl=%u ", (unsigned)m->text_len);
        if (m->text && m->text_len) fwrite(m->text, 1, m->text_len, f);
        fputc('\n', f);
    }
}

int main(int argc, char **argv) {
    const char *mode = (argc > 1) ? argv[1] : "dump";
    /* 与 hn_layout_mcache_create 同一判据: HN_MEASURE_CACHE=0 = 关 */
    const char *env = getenv("HN_MEASURE_CACHE");
    int cache_on = !(env && *env && strcmp(env, "0") == 0);
    int N_PARA = 96;

    if (!strcmp(mode, "bench")) {
        /* C. 提速口径(信息值): 固定文档 300 遍全量重排(宽在 420/421 间
           交替 —— 布局输入真实变化, 文本面不变 → 开缓存应全命中)。
           用 layout_nopaint 隔离**纯布局**(L2 的目标面; 带 paint 的话
           L1 片段回放在两模式下同价, 会稀释比值)。 */
        int iters = 300, i;
        char *html = build_doc(N_PARA);
        struct timeval t0, t1;
        hn_doc *doc = hn_parse_html(html, strlen(html));
        hn_context *ctx = hn_context_create();
        hn_text_backend tb;
        long hits = 0, misses = 0, entries = 0;
        double ms;
        memset(&tb, 0, sizeof tb);
        tb.measure = measure_cb;
        tb.metrics = metrics_cb;
        if (!doc || !ctx) return 2;
        hn_context_set_doc(ctx, doc);
        hn_context_layout(ctx, 420, 640, &tb);   /* 预热(首轮不计时) */
        gettimeofday(&t0, NULL);
        for (i = 0; i < iters; i++)
            hn_context_layout_nopaint(ctx, i % 2 ? 421.0f : 420.0f, 640.0f, &tb);
        gettimeofday(&t1, NULL);
        ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
             (double)(t1.tv_usec - t0.tv_usec) / 1000.0;
        hn_layout_mcache_stats(ctx->mcache, &hits, &misses, &entries);
        printf("bench ms=%.1f iters=%d\n", ms, iters);
        fprintf(stderr, "stats hits=%ld misses=%ld entries=%ld\n",
                hits, misses, entries);
        hn_context_destroy(ctx);
        free(html);
        return 0;
    }

    /* A+B. dump 模式: 初次布局 → 改宽重排 → 改一处文本再重排 → 转储。 */
    {
        char *html = build_doc(N_PARA);
        hn_doc *doc = hn_parse_html(html, strlen(html));
        hn_context *ctx = hn_context_create();
        hn_text_backend tb;
        hn_node *mut, *root;
        long h0 = 0, m0 = 0, e0 = 0;      /* 两遍后的统计 */
        long h1 = 0, m1 = 0, e1 = 0;      /* 突变遍后的统计 */
        const char *mut_txt = "突变句: 改一处文本, 其余子树的测量应全部命中缓存"
                              " —— mutation replaces the whole paragraph text.";
        memset(&tb, 0, sizeof tb);
        tb.measure = measure_cb;
        tb.metrics = metrics_cb;
        if (!doc || !ctx) return 2;
        hn_context_set_doc(ctx, doc);
        hn_context_layout(ctx, 420, 640, &tb);
        hn_context_layout(ctx, 421, 640, &tb);
        hn_layout_mcache_stats(ctx->mcache, &h0, &m0, &e0);
        mut = find_id(hn_doc_root(doc), "p7");
        root = hn_doc_root(doc);
        check(root != NULL, "文档根存在");
        check(mut != NULL, "突变目标 p7 存在");
        check(hn_node_set_text_content(mut, mut_txt, strlen(mut_txt)) == 1,
              "set_text_content 成功");
        hn_context_layout(ctx, 420, 640, &tb);
        hn_layout_mcache_stats(ctx->mcache, &h1, &m1, &e1);
        fprintf(stderr, "stats@2 hits=%ld misses=%ld entries=%ld\n", h0, m0, e0);
        fprintf(stderr, "stats@3 hits=%ld misses=%ld entries=%ld\n", h1, m1, e1);
        if (cache_on) {
            check(e0 > 0, "缓存有条目(entries>0)");
            check(h0 > 0, "第二遍起命中(hits>0)");
            check(h1 > h0, "突变遍旧文本仍命中(hits 净增)");
            check(m1 > m0, "突变遍新文本计入 miss");
        }
        check(hn_doc_validate(doc, 0) == 0, "结构自检通过");
        dump_node(root, stdout);
        dump_dl(hn_context_display_list(ctx), stdout);
        hn_context_destroy(ctx);
        free(html);
    }

    fprintf(stderr, "通过 %d / 失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
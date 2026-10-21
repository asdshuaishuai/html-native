/* freeze_l2_probe.c — 冻结布局(L2)探针: 文本测量缓存 + 子树布局记忆
 *
 * 口径(freeze-design.md §三): L2 = 布局只重排脏子树。两层机制:
 *   - 文本测量缓存(L2 前置): measure(字体五元组+文本字节→宽度)记忆化;
 *   - 子树布局记忆(L2 本体第一刀): 子树 Merkle 键+布局输入全中 → 跳过
 *     重排(原点变只平移)。
 * 断言面:
 *   A. 逐位一致: 机制全关(HN_MEASURE_CACHE=0 HN_LAYOUT_SKIP=0)与全开
 *      (缺省)的同序列布局转储 cmp 逐字节相同 —— 盒/runs/显示列表按位。
 *   B. 测量缓存命中: 第二遍起 hits>0; 改一处文本那遍 hits 净增。
 *   C. 布局记忆命中: 同输入第二遍 hit>0 且 moved==0; 改一处文本后
 *      前方兄弟 hit、后方兄弟 moved>0(平移回放)。
 *   D. 提速(信息值, 不做硬线): 三种负载 300 遍纯布局重排的开关计时 ——
 *      static(全同输入)/stream(单点文本逐遍变更)/resize(逐遍改宽,
 *      布局记忆的最坏情形: 键全失配只付哈希)。
 * 用法: freeze_l2_probe [dump|bench]
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

static int env_off(const char *name) {
    const char *v = getenv(name);
    return v && *v && strcmp(v, "0") == 0;
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
        /* 多行内组容器(文本/块/文本混排): 同帧两个 IFC 组先后省略号截断,
           组间 runs 不得互毁(wipe-once 帧代数的审查修复锚) */
        ".mg{font-size:14px;line-clamp:1;height:22px;overflow:hidden;width:380}"
        ".blk{width:40;height:18;background:#345}"
        "</style></head><body>");
    for (i = 0; i < n_para; i++) {
        const char *cls = g_classes[i % 4];
        const char *txt = g_sentences[i % 4];
        off += (size_t)snprintf(buf + off, cap - off,
            "<p class=\"%s\" id=\"p%d\">%s #%d</p>", cls, i, txt, i);
    }
    off += (size_t)snprintf(buf + off, cap - off,
        "<div class=\"mg\">%s"
        "<div class=\"blk\"></div>"
        "%s</div>"
        "<div class=\"clamp\" id=\"clamp\">%s%s%s</div>"
        "<div class=\"row\">"
        "<span class=\"a\">甲</span><span class=\"b\">乙</span>"
        "<span class=\"c\">丙</span><span class=\"d\">丁</span>"
        "<span class=\"a\">戊</span><span class=\"b\">己</span>"
        "</div></body></html>",
        g_sentences[2], g_sentences[3],
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

/* ---- 转储: exact 模式浮点按位打印(命中路径必须逐位一致);
   approx 模式十进制打印(突变序列含平移回放 —— 与全新布局存在
   浮点结合序差 ~1ulp, 数学等价, 由 .sh 按 ≤0.01px 容差逐 token 对照) */

static int g_approx = 0;

static void pbits(FILE *f, float v) {
    if (g_approx) {
        fprintf(f, " %.6f", (double)v);
        return;
    }
    unsigned u;
    memcpy(&u, &v, 4);
    fprintf(f, " %08x", u);
}

static void pcolor(FILE *f, unsigned c) {
    if (g_approx) { fprintf(f, " %u", c); return; }
    fprintf(f, " %08x", c);
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
        pcolor(f, m->fill); pcolor(f, m->stroke);
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
    int cache_on = !env_off("HN_MEASURE_CACHE");
    int lskip_on = !env_off("HN_LAYOUT_SKIP");
    int smemo_on = !env_off("HN_STYLE_MEMO");
    int N_PARA = 96;

    if (!strcmp(mode, "bench")) {
        /* D. 三种负载(信息值): layout_nopaint 隔离纯布局(L2 目标面)。
           static: 全同输入(布局记忆全命中);
           stream: 单点文本逐遍变更(脏链重排, 后方兄弟平移回放);
           resize: 逐遍改宽(布局记忆最坏情形: 键全失配只付哈希)。 */
        int iters = 300, i;
        static const char *names[3] = { "static", "stream", "resize" };
        for (int w = 0; w < 3; w++) {
            char *html = build_doc(N_PARA);
            struct timeval t0, t1;
            hn_doc *doc = hn_parse_html(html, strlen(html));
            hn_context *ctx = hn_context_create();
            hn_text_backend tb;
            long hit = 0, moved = 0, miss = 0, bail = 0;
            double ms;
            char buf[256];
            memset(&tb, 0, sizeof tb);
            tb.measure = measure_cb;
            tb.metrics = metrics_cb;
            if (!doc || !ctx) return 2;
            hn_context_set_doc(ctx, doc);
            hn_context_layout(ctx, 420, 640, &tb);   /* 预热(首轮不计时) */
            gettimeofday(&t0, NULL);
            for (i = 0; i < iters; i++) {
                if (w == 1) {
                    hn_node *mut = find_id(hn_doc_root(doc), "p7");
                    const char *txt = (i % 2) ? "流式负载甲: 单点文本逐遍变更, "
                        "脏链重排, 后方兄弟整树平移回放。" :
                        "流式负载乙: 与甲互斥 —— 每一遍都让 p7 子树失配, "
                        "其余子树要么命中要么平移。";
                    hn_node_set_text_content(mut, txt, strlen(txt));
                    hn_context_layout_nopaint(ctx, 420, 640, &tb);
                } else {
                    hn_context_layout_nopaint(ctx,
                        (w == 2 && i % 2) ? 421.0f : 420.0f, 640.0f, &tb);
                }
            }
            gettimeofday(&t1, NULL);
            ms = (double)(t1.tv_sec - t0.tv_sec) * 1000.0 +
                 (double)(t1.tv_usec - t0.tv_usec) / 1000.0;
            hn_layout_lskip_stats(&hit, &moved, &miss, &bail);
            snprintf(buf, sizeof buf, "bench-%s ms=%.1f iters=%d "
                     "[hit=%ld moved=%ld miss=%ld bail=%ld]\n",
                     names[w], ms, iters, hit, moved, miss, bail);
            fputs(buf, stdout);
            hn_context_destroy(ctx);
            free(html);
        }
        return 0;
    }

    /* A+B+C. dump: argv[2]=exact(同输入双遍 → 命中路径逐位锚) |
       approx(突变遍 → 平移回放, 容差对照)。 */
    {
        int approx = (argc > 2 && !strcmp(argv[2], "approx"));
        char *html = build_doc(N_PARA);
        hn_doc *doc = hn_parse_html(html, strlen(html));
        hn_context *ctx = hn_context_create();
        hn_text_backend tb;
        hn_node *mut, *root;
        long h0 = 0, m0 = 0, e0 = 0;      /* 测量缓存: 同输入遍后 */
        long h1 = 0, m1 = 0, e1 = 0;      /* 测量缓存: 突变遍后 */
        long k1h = 0, k1m = 0, k1x = 0, k1b = 0;   /* 布局记忆: 同输入遍 */
        long k2h = 0, k2m = 0, k2x = 0, k2b = 0;   /* 布局记忆: 突变遍 */
        long s0h = 0, s0m = 0, s1h = 0, s1m = 0;   /* 级联记忆: 同输入遍/突变遍 */
        const char *mut_txt = "突变句: 改一处文本, 其余子树的测量应全部命中缓存"
                              " —— mutation replaces the whole paragraph text.";
        memset(&tb, 0, sizeof tb);
        tb.measure = measure_cb;
        tb.metrics = metrics_cb;
        if (!doc || !ctx) return 2;
        hn_context_set_doc(ctx, doc);
        hn_context_layout(ctx, 420, 640, &tb);
        hn_context_layout(ctx, 420, 640, &tb);          /* 同输入: 布局记忆应全中 */
        hn_layout_lskip_stats(&k1h, &k1m, &k1x, &k1b);
        hn_style_memo_stats(&s0h, &s0m);
        hn_layout_mcache_stats(ctx->mcache, &h0, &m0, &e0);
        mut = find_id(hn_doc_root(doc), "p7");
        root = hn_doc_root(doc);
        check(root != NULL, "文档根存在");
        check(mut != NULL, "突变目标 p7 存在");
        if (approx) {
            check(hn_node_set_text_content(mut, mut_txt, strlen(mut_txt)) == 1,
                  "set_text_content 成功");
            hn_context_layout(ctx, 420, 640, &tb);      /* 突变遍 */
            hn_style_memo_stats(&s1h, &s1m);
            hn_layout_lskip_stats(&k2h, &k2m, &k2x, &k2b);
            hn_layout_mcache_stats(ctx->mcache, &h1, &m1, &e1);
            fprintf(stderr, "lskip@mut  hit=%ld moved=%ld miss=%ld bail=%ld\n",
                    k2h, k2m, k2x, k2b);
            fprintf(stderr, "smemo@mut  hits=%ld miss=%ld\n", s1h, s1m);
            fprintf(stderr, "mcache@3 hits=%ld misses=%ld entries=%ld\n",
                    h1, m1, e1);
        }
        fprintf(stderr, "lskip@same hit=%ld moved=%ld miss=%ld bail=%ld\n",
                k1h, k1m, k1x, k1b);
        fprintf(stderr, "smemo@same hits=%ld miss=%ld\n", s0h, s0m);
        fprintf(stderr, "mcache@2 hits=%ld misses=%ld entries=%ld\n", h0, m0, e0);
        if (lskip_on) {
            check(k1h > 0, "同输入遍整子树命中(hit>0)");
            check(k1m == 0, "同输入遍无平移(moved==0)");
        }
        if (approx && lskip_on) {
            check(k2h > 0, "突变遍前方兄弟仍命中(hit>0)");
            check(k2m > 0, "突变遍后方兄弟平移回放(moved>0)");
        }
        if (smemo_on) {
            check(s0h > 0, "级联记忆: 同输入遍命中(hits>0)");
        }
        if (approx && smemo_on) {
            check(s1h > 0, "突变遍未变节点仍命中(hits>0)");
        }
        if (cache_on) {
            check(e0 > 0, "测量缓存有条目(entries>0)");
            check(h0 > 0, "第二遍起命中(hits>0)");
        }
        if (approx && cache_on) {
            check(h1 > h0, "突变遍旧文本仍命中(hits 净增)");
            check(m1 > m0, "突变遍新文本计入 miss");
        }
        check(hn_doc_validate(doc, 0) == 0, "结构自检通过");
        g_approx = approx;
        dump_node(root, stdout);
        dump_dl(hn_context_display_list(ctx), stdout);
        hn_context_destroy(ctx);
        free(html);
    }

    fprintf(stderr, "通过 %d / 失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
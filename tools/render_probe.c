/* render_probe.c — 离屏验收(纯 C99, 零 GUI 依赖, 零构建链)
 *
 * 管线与真窗口同一条: 解析 → 级联 → 布局 → 绘制指令 → hnsoft 位图 → PNG。
 * 位图即窗口所见 —— 光栅器就是生产用的 hnsoft, 不存在第二套"测试专用"实现。
 *
 * 用法(与其他 C 探针同口径, 直接 cc, 无 SPM; 本机 61 断言全绿验证过):
 *   cc -O2 -I Sources/CHtmlNative/include -I Sources/CHtmlNative \
 *      -I /opt/homebrew/include/freetype2 tools/render_probe.c \
 *      Sources/CHtmlNative/hn_{arena,html,css,style,layout,paint,context,theme}.c \
 *      Sources/CHtmlNative/hn_{json,lottie,mesh,png}.c \
 *      Sources/CHtmlNative/hnsoft.c -L/opt/homebrew/lib -lfreetype -lm \
 *      -o /tmp/render_probe
 *   /tmp/render_probe                           # 内置用例 + 断言(全绿退出 0)
 *   /tmp/render_probe a.html a.css out.png [W H] # 渲染任意文件(不断言)
 *
 * 历史: 本可执行曾是 Swift 版(415 项断言), c49cfbd 全量 C99 时随 Swift 层
 * 一起移除; 这里以 C99 重生 —— 引擎是唯一渲染路径, 验收也必须同源。
 * 曾以 Package.swift(SPM)提供入口, 已删 —— 构建链只有 bash + cc/zig cc。
 *
 * 断言口径: 只断与文本度量无关的确定性结论(颜色/几何/指令形态/像素采样),
 * 文字宽度因 FreeType 有无而不同, 不进入断言(引擎同口径回退, 见 hnsoft.c)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "hn.h"
/* hnsoft.h 不在 include/(对包外是模块私有), 但验收必须调用生产光栅器 ——
   以相对路径包含引擎树内的同一份头, 保持单一实现来源。 */
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

static void check_color(hn_color got, hn_color want, const char *label) {
    int ok = got == want;
    printf("%s %s (got %08X want %08X)\n", ok ? "  ✔" : "  ✘", label, got, want);
    if (ok) g_pass++; else g_fail++;
}

static void check_near(float got, float want, float tol, const char *label) {
    int ok = (float)fabs((double)(got - want)) <= tol;
    printf("%s %s (got %.1f want %.1f±%.1f)\n", ok ? "  ✔" : "  ✘", label, got, want, tol);
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

/* 文本测量后端: 与 hncore 同口径 —— 转发到 hnsoft(FreeType 可用则真实
   测量, 否则引擎同款等宽估算)。断言不依赖具体宽度。 */
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

/* 深度遍历: 命中第一个满足 pred 的节点(断言辅助, 文档规模小, 递归即可) */
typedef int (*node_pred)(hn_node *n, void *ud);
static hn_node *find_node(hn_node *n, node_pred pred, void *ud) {
    if (pred(n, ud)) return n;
    for (hn_node *c = hn_node_first_child(n); c; c = hn_node_next_sibling(c)) {
        hn_node *hit = find_node(c, pred, ud);
        if (hit) return hit;
    }
    return NULL;
}

static int pred_has_runs(hn_node *n, void *ud) {
    (void)ud;
    return hn_node_run_count(n) > 0;
}

/* ---- 内置用例一: 主文档(布局/样式/指令/命中/像素都要用) ---- */

static const char *DOC_HTML =
"<html>\n"
"<head>\n"
"<meta name=\"hn-surface\" content=\"popup\">\n"
"<meta name=\"hn-window\" content=\"360x520@100,80\">\n"
"<meta name=\"hn-title\" content=\"渲染测试\">\n"
"<style>\n"
"  .row { display: flex; gap: 10; }\n"
"  .chip { width: 40; height: 12; border-radius: 6; }\n"
"  .a { background: #ff5f57; } .b { background: #febc2e; } .c { background: #28c840; }\n"
"  #card-bg { display: none; }\n"
"</style>\n"
"</head>\n"
"<body>\n"
"  <div id=\"card\" class=\"card\">\n"
"    <div class=\"row\">\n"
"      <div id=\"chip-a\" class=\"chip a\"></div>\n"
"      <div id=\"chip-b\" class=\"chip b\"></div>\n"
"      <div id=\"chip-c\" class=\"chip c\"></div>\n"
"    </div>\n"
"    <h1 id=\"title\">你好, html-native</h1>\n"
"    <p id=\"para\">HTML 负责结构, CSS 负责样式, 全程没有浏览器参与。</p>\n"
"    <div id=\"card-bg\">不该出现</div>\n"
"    <button id=\"tap-button\">点我试试</button>\n"
"  </div>\n"
"</body>\n"
"</html>\n";

static const char *DOC_CSS =
"html, body { margin: 0; width: 100%; height: 100%; background: #0f1115; }\n"
".card { background: #1b1f27; border: 1 solid #2a3040; border-radius: 16;\n"
"        padding: 24; display: flex; flex-direction: column; gap: 12;\n"
"        height: 100%; box-sizing: border-box; }\n"
"h1 { color: #e8eaf0; font-size: 26; font-weight: 700; margin: 4 0 0 0; }\n"
"p { color: #9aa3b5; font-size: 14; margin: 0; line-height: 1.6; }\n"
"button { background: #4f7cff; color: #ffffff; padding: 10 18; border-radius: 8;\n"
"         text-align: center; font-weight: 600; }\n";

#define VW 800.0f
#define VH 600.0f

/* == 解析与清单 == */
static void test_parse(void) {
    printf("== 解析 ==\n");
    hn_doc *doc = hn_parse_html(DOC_HTML, strlen(DOC_HTML));
    check(doc != NULL, "hn_parse_html 成功");
    if (!doc) return;
    hn_node *root = hn_doc_root(doc);
    hn_node *body = hn_doc_body(doc);
    check(root != NULL, "文档根存在");
    check(body != NULL, "<body> 存在");
    check_int(hn_doc_validate(doc, 100000), 0, "结构自检(链表环/父子一致)");

    hn_node *title = hn_doc_find_by_id(doc, "title");
    check(title != NULL, "find_by_id(title) 命中");
    char buf[128] = { 0 };
    if (title) hn_node_text_content(title, buf, sizeof(buf) - 1);
    check(strstr(buf, "你好") != NULL, "元素文本内容含 CJK");

    hn_manifest mm;
    memset(&mm, 0, sizeof(mm));
    hn_doc_manifest(doc, &mm);
    check_int(mm.surface == HN_SURFACE_POPUP, 1, "清单 surface=popup");
    check_near(mm.w, 360, 0.5f, "清单 w=360");
    check_near(mm.h, 520, 0.5f, "清单 h=520");
    check(mm.title && !strcmp(mm.title, "渲染测试"), "清单 title");
    hn_doc_free(doc);
}

/* == 级联(计算样式) ==
   计算样式在级联+布局后才存在(引擎的 debug_* 读的是布局期写回的样式),
   与 hncore 同口径: 先 hn_context_layout 再查询。 */
static void test_style(void) {
    printf("== 级联 ==\n");
    hn_doc *doc = hn_parse_html(DOC_HTML, strlen(DOC_HTML));
    hn_sheet *sheet = hn_parse_css(DOC_CSS, strlen(DOC_CSS));
    hn_context *ctx = hn_context_create();
    hn_context_set_doc(ctx, doc);
    hn_context_add_sheet(ctx, sheet);
    hn_text_backend tb = { NULL, measure_cb, metrics_cb };
    hn_context_layout(ctx, VW, VH, &tb);

    hn_node *card = hn_doc_find_by_id(doc, "card");
    hn_color c = 0;
    hn_node_debug_background(card, &c);
    check_color(c, 0x1B1F27FF, "#card 背景 #1b1f27");
    hn_node_debug_border(card, &c);
    check_color(c, 0x2A3040FF, "#card 边框 #2a3040");

    struct { const char *id; hn_color want; } chips[] = {
        { "chip-a", 0xFF5F57FF }, { "chip-b", 0xFEBC2EFF }, { "chip-c", 0x28C840FF },
    };
    for (int i = 0; i < 3; i++) {
        hn_node *n = hn_doc_find_by_id(doc, chips[i].id);
        hn_color bg = 0;
        if (n) hn_node_debug_background(n, &bg);
        check_color(bg, chips[i].want, "chip 类背景(级联到 .a/.b/.c)");
    }
    hn_node *h1 = hn_doc_find_by_id(doc, "title");
    hn_node_debug_color(h1, &c);
    check_color(c, 0xE8EAF0FF, "h1 文字色 #e8eaf0");

    hn_node *none_el = hn_doc_find_by_id(doc, "card-bg");
    check(none_el != NULL && hn_node_display(none_el) == 2, "display:none 语义(display=2)");
    hn_context_destroy(ctx);
}

/* == 布局 == */
static void test_layout(void) {
    printf("== 布局 ==\n");
    hn_doc *doc = hn_parse_html(DOC_HTML, strlen(DOC_HTML));
    hn_sheet *sheet = hn_parse_css(DOC_CSS, strlen(DOC_CSS));
    hn_context *ctx = hn_context_create();
    hn_context_set_doc(ctx, doc);
    hn_context_add_sheet(ctx, sheet);
    hn_text_backend tb = { NULL, measure_cb, metrics_cb };
    hn_context_layout(ctx, VW, VH, &tb);

    float cx, cy, cw, ch;
    hn_node_box(hn_doc_find_by_id(doc, "card"), &cx, &cy, &cw, &ch);
    check_near(cw, VW, 1.0f, "card 宽=视口宽(100%)");
    check_near(ch, VH, 1.0f, "card 高=视口高(100%)");

    float ax, ay, aw, ah, bx, by, bw, bh, gx, gy, gw, gh;
    hn_node_box(hn_doc_find_by_id(doc, "chip-a"), &ax, &ay, &aw, &ah);
    hn_node_box(hn_doc_find_by_id(doc, "chip-b"), &bx, &by, &bw, &bh);
    hn_node_box(hn_doc_find_by_id(doc, "chip-c"), &gx, &gy, &gw, &gh);
    check_near(aw, 40, 0.5f, "chip 宽 40");
    check_near(ah, 12, 0.5f, "chip 高 12");
    check_near(ay, by, 0.5f, "chip 同一行(y 对齐)");
    check_near(by, gy, 0.5f, "chip 同一行(y 对齐, c)");
    check(ax < bx && bx < gx, "chip 按文档序从左到右");
    check_near(bx - (ax + aw), 10, 1.5f, "flex gap=10(a→b)");
    check_near(gx - (bx + bw), 10, 1.5f, "flex gap=10(b→c)");
    check(ax > cx + 24, "chip 在 card 内边距之内");

    float tx, ty, tw, th;
    hn_node_box(hn_doc_find_by_id(doc, "title"), &tx, &ty, &tw, &th);
    check(ty > ay + ah, "标题排在 chip 行之后(文档流)");

    /* 行内片段挂在**文本节点**上(元素自身 run_count 为 0): 找标题子树里
       第一个持有片段的节点, 区间必须落在该节点文本内 */
    hn_node *tn = hn_doc_find_by_id(doc, "title");
    hn_node *holder = NULL;
    if (tn) holder = find_node(tn, pred_has_runs, NULL);
    check(holder != NULL, "标题持有行内片段(在文本节点上)");
    if (holder) {
        int runs = hn_node_run_count(holder);
        size_t tl = 0;
        const char *txt = hn_node_text(holder, &tl);
        check(txt != NULL && tl > 0, "片段持有者是文本节点");
        for (int i = 0; i < runs; i++) {
            size_t b = 0, e = 0;
            if (hn_node_run_range(holder, i, &b, &e) == 1) {
                check(e <= tl && b < e, "run 字节区间落在文本内");
            } else {
                check(0, "run 字节区间落在文本内");
            }
        }
    }
    hn_context_destroy(ctx);
}

/* == 命中 == */
static void test_hit(void) {
    printf("== 命中 ==\n");
    hn_doc *doc = hn_parse_html(DOC_HTML, strlen(DOC_HTML));
    hn_sheet *sheet = hn_parse_css(DOC_CSS, strlen(DOC_CSS));
    hn_context *ctx = hn_context_create();
    hn_context_set_doc(ctx, doc);
    hn_context_add_sheet(ctx, sheet);
    hn_text_backend tb = { NULL, measure_cb, metrics_cb };
    hn_context_layout(ctx, VW, VH, &tb);

    float x, y, w, h;
    hn_node_box(hn_doc_find_by_id(doc, "chip-a"), &x, &y, &w, &h);
    const char *id = hn_context_hit_test(ctx, x + w / 2, y + h / 2);
    check(id && !strcmp(id, "chip-a"), "chip 中心命中自身");

    int checked = 0;
    int bad = hn_context_verify_hits(ctx, &checked);
    check_int(bad, 0, "命中自洽(带 id 元素盒中心命中自身/后代)");
    printf("  (检查元素: %d)\n", checked);
    hn_context_destroy(ctx);
}

/* == 绘制指令(display list 形态) == */
static int clip_depth = 0, clip_unbalanced = 0;
/* 文本按片段出指令(hncore paint 可见: "你好, " 与 "html-native" 各一条),
   断言改为方向相反的包含: 片段文本 ⊆ 标题全文。 */
static void scan_dl(const hn_display_list *dl, int *rects, int *texts,
                    int *neg, const char *title_text, int *title_fragments) {
    for (int i = 0; i < dl->count; i++) {
        const hn_cmd *c = &dl->cmds[i];
        if (c->kind == HN_CMD_RECT) {
            (*rects)++;
            if (c->w < 0 || c->h < 0) (*neg)++;
        } else if (c->kind == HN_CMD_TEXT) {
            (*texts)++;
            if (title_text && c->text && c->text_len >= 4) {
                char buf[128];
                size_t n = c->text_len < sizeof(buf) - 1 ? c->text_len : sizeof(buf) - 1;
                memcpy(buf, c->text, n);
                buf[n] = 0;
                if (strstr(title_text, buf)) (*title_fragments)++;
            }
        } else if (c->kind == HN_CMD_CLIP_PUSH) {
            clip_depth++;
        } else if (c->kind == HN_CMD_CLIP_POP) {
            clip_depth--;
            if (clip_depth < 0) clip_unbalanced = 1;
        }
    }
}

static void test_display_list(void) {
    printf("== 绘制指令 ==\n");
    hn_doc *doc = hn_parse_html(DOC_HTML, strlen(DOC_HTML));
    hn_sheet *sheet = hn_parse_css(DOC_CSS, strlen(DOC_CSS));
    hn_context *ctx = hn_context_create();
    hn_context_set_doc(ctx, doc);
    hn_context_add_sheet(ctx, sheet);
    hn_text_backend tb = { NULL, measure_cb, metrics_cb };
    hn_context_layout(ctx, VW, VH, &tb);

    char title_buf[128] = { 0 };
    hn_node_text_content(hn_doc_find_by_id(doc, "title"), title_buf,
                         sizeof(title_buf) - 1);

    const hn_display_list *dl = hn_context_display_list(ctx);
    check(dl != NULL && dl->count > 0, "display list 非空");

    int rects = 0, texts = 0, neg = 0, fragments = 0;
    clip_depth = 0; clip_unbalanced = 0;
    scan_dl(dl, &rects, &texts, &neg, title_buf, &fragments);
    check_int(neg, 0, "无负尺寸 RECT");
    check(rects >= 5, "RECT 覆盖(画布+card+3 chips)");
    check(texts >= 2, "TEXT 指令存在(标题/段落/按钮)");
    check_int(clip_unbalanced, 0, "CLIP 不下溢");
    check_int(clip_depth, 0, "CLIP 入栈出栈配平");
    check(fragments >= 1, "标题文本进入指令(片段 ⊆ 全文)");
    hn_context_destroy(ctx);
}

/* == 动画: @keyframes + anim_tick 推进 == */
static void test_anim(void) {
    printf("== 动画 ==\n");
    const char *html =
        "<style>\n"
        "@keyframes grow { from { opacity: 0; } to { opacity: 1; } }\n"
        "#m { width: 50; height: 50; background: #336699; animation: grow 1s linear 1; }\n"
        "</style>\n"
        "<div id=\"m\"></div>\n";
    hn_doc *doc = hn_parse_html(html, strlen(html));
    hn_sheet *sheet = hn_parse_css("", 0);
    hn_context *ctx = hn_context_create();
    hn_context_set_doc(ctx, doc);
    hn_context_add_sheet(ctx, sheet);
    hn_text_backend tb = { NULL, measure_cb, metrics_cb };
    hn_context_layout(ctx, 200, 200, &tb);

    hn_node *m = hn_doc_find_by_id(doc, "m");
    const char *name = NULL;
    float ms = 0;
    int iter = 0;
    hn_node_debug_anim(m, &name, &ms, &iter);
    check(name && !strcmp(name, "grow"), "动画名解析(grow)");
    check_near(ms, 1000, 1.0f, "动画时长 1s=1000ms");

    /* dt_ms < 0 = 仅初始化: 关键帧 from 生效 → opacity 0 */
    hn_context_anim_tick(ctx, -1);
    float op = -1;
    hn_node_debug_opacity(m, &op);
    check(op < 0.05f, "初始化后 opacity=from(0)");

    /* 推进到中点(线性缓动 → 一半), 再推过终点 → 1 */
    hn_context_anim_tick(ctx, 500);
    hn_node_debug_opacity(m, &op);
    check(op > 0.2f && op < 0.85f, "半程 opacity≈0.5");
    hn_context_anim_tick(ctx, 600);
    hn_node_debug_opacity(m, &op);
    check(op > 0.95f, "终点 opacity=to(1)");
    hn_context_destroy(ctx);
}

/* == 输入控件 == */
static void test_input(void) {
    printf("== 输入控件 ==\n");
    const char *html =
        "<style>input { width: 120; height: 16; }</style>\n"
        "<input name=\"user\" value=\"hn\">\n"
        "<input type=\"password\" name=\"pw\" value=\"s3cret\">\n"
        "<input type=\"checkbox\" name=\"remember\" checked>\n"
        "<input type=\"checkbox\" name=\"later\">\n"
        "<textarea name=\"bio\">hi</textarea>\n"
        "<button id=\"go\" disabled>提交</button>\n";
    hn_doc *doc = hn_parse_html(html, strlen(html));
    hn_sheet *sheet = hn_parse_css("", 0);
    hn_context *ctx = hn_context_create();
    hn_context_set_doc(ctx, doc);
    hn_context_add_sheet(ctx, sheet);
    hn_text_backend tb = { NULL, measure_cb, metrics_cb };
    hn_context_layout(ctx, 400, 300, &tb);

    hn_node *user = hn_doc_input_at(doc, 0);
    hn_node *pw = hn_doc_input_at(doc, 1);
    hn_node *remember = hn_doc_input_at(doc, 2);
    hn_node *later = hn_doc_input_at(doc, 3);
    hn_node *bio = hn_doc_input_at(doc, 4);
    check_int(hn_node_input_kind(user), HN_IN_TEXT, "input 缺省 = 文本框");
    check_int(hn_node_input_kind(pw), HN_IN_PASSWORD, "type=password 语义");
    check_int(hn_node_input_kind(remember), HN_IN_CHECKBOX, "type=checkbox 语义");
    check_int(hn_node_input_kind(bio), HN_IN_TEXTAREA, "textarea 语义");
    check_int(hn_node_is_checked(remember), 1, "checked 属性 = 选中");
    check_int(hn_node_is_checked(later), 0, "无 checked = 未选");
    hn_node *go = hn_doc_find_by_id(doc, "go");
    check_int(hn_node_is_disabled(go), 1, "disabled 属性生效");

    char form[256] = { 0 };
    hn_doc_form_encode(doc, form, sizeof(form) - 1);
    check(strstr(form, "user=hn") != NULL, "表单编码含 user=hn");
    check(strstr(form, "pw=s3cret") != NULL, "表单编码含密码字段(值保持原文)");
    hn_context_destroy(ctx);
}

/* == 光栅与 PNG(位图即窗口所见) == */
static void test_raster(void) {
    printf("== 光栅/PNG ==\n");
    hn_doc *doc = hn_parse_html(DOC_HTML, strlen(DOC_HTML));
    hn_sheet *sheet = hn_parse_css(DOC_CSS, strlen(DOC_CSS));
    hn_context *ctx = hn_context_create();
    hn_context_set_doc(ctx, doc);
    hn_context_add_sheet(ctx, sheet);
    hn_text_backend tb = { NULL, measure_cb, metrics_cb };
    hn_context_layout(ctx, VW, VH, &tb);
    const hn_display_list *dl = hn_context_display_list(ctx);
    check(dl != NULL, "待光栅的 display list 就绪");

    int W = (int)VW, H = (int)VH;
    unsigned char *px = hnsoft_render(dl, W, H, 0x0B0E13FF);
    check(px != NULL, "hnsoft_render 出位图");
    if (!px) { hn_context_destroy(ctx); return; }

    /* 采样点都取实心区中心(避开圆角抗锯齿与文本):
       card 底部内边距区 / chip-a 正中 */
    float bx, by, bw, bh, ax, ay, aw, ah;
    hn_node_box(hn_doc_find_by_id(doc, "card"), &bx, &by, &bw, &bh);
    hn_node_box(hn_doc_find_by_id(doc, "chip-a"), &ax, &ay, &aw, &ah);
    long card_ofs = ((long)(by + bh - 12) * W + (long)(bx + 12)) * 4;
    long chip_ofs = ((long)(ay + ah / 2) * W + (long)(ax + aw / 2)) * 4;
    check(px[card_ofs] == 0x1B && px[card_ofs + 1] == 0x1F && px[card_ofs + 2] == 0x27,
          "card 内边距区像素 = #1b1f27(RGBA8 顺序)");
    check(px[chip_ofs] == 0xFF && px[chip_ofs + 1] == 0x5F && px[chip_ofs + 2] == 0x57,
          "chip-a 中心像素 = #ff5f57(显示列表→位图一致性)");

    size_t pn = 0;
    unsigned char *png = hnsoft_encode_png(px, W, H, &pn);
    check(png != NULL && pn > 8 &&
          png[0] == 0x89 && png[1] == 'P' && png[2] == 'N' && png[3] == 'G',
          "PNG 编码(魔数合法)");
    const char *out = "/tmp/hn_render_test.png";
    FILE *f = png ? fopen(out, "wb") : NULL;
    if (f) {
        fwrite(png, 1, pn, f);
        fclose(f);
        size_t back = 0;
        char *again = read_all(out, &back);
        check(again != NULL && back == pn, "PNG 落盘并完整读回");
        free(again);
        printf("  (已写 %s, %.1f KB)\n", out, (double)pn / 1024.0);
    } else {
        check(0, "PNG 落盘并完整读回");
    }
    free(png);
    free(px);
    hn_context_destroy(ctx);
}

/* 任意文件模式: render_probe a.html [a.css] out.png [W H] —— 不断言。
   第 2 参以 .css 结尾 → 完整四参形态; 否则视为 a.html out.png [W H]。 */
static int render_file(int argc, char **argv) {
    const char *html_path = argv[1];
    const char *css_path = NULL;
    const char *out = "out.png";
    float W = 800, H = 600;
    if (argc >= 3 && strlen(argv[2]) > 4 &&
        !strcmp(argv[2] + strlen(argv[2]) - 4, ".css")) {
        css_path = argv[2];
        if (argc >= 4) out = argv[3];
        if (argc >= 5) W = (float)atof(argv[4]);
        if (argc >= 6) H = (float)atof(argv[5]);
    } else {
        if (argc >= 3) out = argv[2];
        if (argc >= 4) W = (float)atof(argv[3]);
        if (argc >= 5) H = (float)atof(argv[4]);
    }
    size_t hlen = 0, clen = 0;
    char *html = read_all(html_path, &hlen);
    if (!html) { fprintf(stderr, "render_probe: 无法读取 %s\n", html_path); return 1; }
    char *css = css_path ? read_all(css_path, &clen) : NULL;

    hn_doc *doc = hn_parse_html(html, hlen);
    hn_context *ctx = hn_context_create();
    hn_context_set_doc(ctx, doc);
    if (css && clen) hn_context_add_sheet(ctx, hn_parse_css(css, clen));
    hn_text_backend tb = { NULL, measure_cb, metrics_cb };
    hn_context_layout(ctx, W, H, &tb);
    const hn_display_list *dl = hn_context_display_list(ctx);
    int rc = 0;
    if (!dl) { fprintf(stderr, "render_probe: 无绘制指令\n"); rc = 1; }
    else {
        unsigned char *px = hnsoft_render(dl, (int)W, (int)H, 0x0B0E13FF);
        if (!px) { fprintf(stderr, "render_probe: 渲染失败\n"); rc = 1; }
        else {
            size_t pn = 0;
            unsigned char *png = hnsoft_encode_png(px, (int)W, (int)H, &pn);
            FILE *f = png ? fopen(out, "wb") : NULL;
            if (f) {
                fwrite(png, 1, pn, f);
                fclose(f);
                printf("已渲染 %dx%d → %s (%.1f KB)%s\n", (int)W, (int)H, out,
                       (double)pn / 1024.0,
                       hnsoft_font_loaded() ? "" : "  [无字体: 文本未渲染]");
            } else { fprintf(stderr, "render_probe: 无法写 %s\n", out); rc = 1; }
            free(png);
            free(px);
        }
    }
    free(css);
    free(html);
    return rc;
}

int main(int argc, char **argv) {
    /* 文件模式: 带路径参数即渲染任意文件(内置用例零参数) */
    if (argc >= 3) return render_file(argc, argv);

    printf("render_probe — C99 离屏验收(引擎: 解析→级联→布局→指令→hnsoft 位图)\n\n");
    test_parse();
    test_style();
    test_layout();
    test_hit();
    test_display_list();
    test_anim();
    test_input();
    test_raster();

    printf("\n结果: %d 通过, %d 失败\n", g_pass, g_fail);
    if (g_fail == 0) printf("全绿 — 引擎层健康\n");
    return g_fail ? 1 : 0;
}

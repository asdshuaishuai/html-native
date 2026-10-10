/* quad3d_probe.c — 3D 场景面 display list 断言(纯 C99, 零 GUI 依赖)
 *
 * 断言锚定的是 examples/threejs-lab.html 压测发现的三条 3D 边界**修复后**
 * 的正确行为 —— 修复前本探针必须是红的(逐条输出失败原因):
 *   A. preserve-3d: 父子 transform 必须复合 —— 立方体六面(父级 perspective
 *      + 各面 rotateY/X + translateZ)要产出 >=6 条 QUAD, 且四角非轴对齐
 *      (存在透视形变)。当前引擎各元素独立投影, 场景旋转不进子级。
 *   B. QUAD 渐变: linear-gradient 底色的 3D 元素, 其 QUAD 必须带渐变字段
 *      (hn_cmd 现有口径: gradient=1 + grad_from/grad_to 两端色非零)。
 *      当前 QUAD 路径只填平色 → 渐变元素投影后全透明。
 *   C. 3D 子树文字: 3D 变换元素内的文字必须以 TEXT 指令进显示列表
 *      (数量 >=1, 且片段 ⊆ 子树全文)。当前 QUAD 分支跳过全部子节点。
 *   D. 布局上下文: flex 直接子项 / position:absolute / inline-block 原子盒的
 *      3D 变换元素不得被静默丢弃 —— 各自至少产出 1 条非退化 QUAD。
 *
 * 断言全部是 display list 层面的确定性结论(指令形态/几何), 不依赖文本度量
 * (与 render_probe 同口径: 文字宽度因 FreeType 有无而不同, 不进断言)。
 *
 * 用法(与其他 C 探针同口径, 直接 cc, 无 SPM; python3 tools/quad3d_probe.py
 * 是本文件的门禁薄封装):
 *   cc -O2 -I Sources/CHtmlNative/include -I Sources/CHtmlNative \
 *      -I /opt/homebrew/include/freetype2 tools/quad3d_probe.c \
 *      Sources/CHtmlNative/hn_{arena,html,css,style,layout,paint,context,theme}.c \
 *      Sources/CHtmlNative/hn_{json,lottie,mesh,media,png}.c \
 *      Sources/CHtmlNative/hnsoft.c -L/opt/homebrew/lib -lfreetype -lm \
 *      -o /tmp/quad3d_probe
 *   /tmp/quad3d_probe          # 内置用例 + 断言(修复后全绿退出 0)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include "hn.h"
/* hnsoft.h 不在 include/(对包外是模块私有), 但测量回退必须与生产同一实现 ——
   以相对路径包含引擎树内的同一份头(与 render_probe.c 同一口径)。 */
#include "../Sources/CHtmlNative/hnsoft.h"

static int g_pass = 0, g_fail = 0;

static void check(int cond, const char *label) {
    printf("%s %s\n", cond ? "  ✔" : "  ✘", label);
    if (cond) g_pass++; else g_fail++;
}

static char g_buf[192];
static const char *mkstr(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_buf, sizeof(g_buf), fmt, ap);
    va_end(ap);
    return g_buf;
}

/* 文本测量后端: 转发到 hnsoft(FreeType 可用则真实测量, 否则引擎同款
   等宽估算)。断言不依赖具体宽度。 */
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

/* 字节级子串(haystack 里找 needle); C99 没有 memmem, 自己写(文档规模小) */
static int bytes_find(const char *hay, size_t hn, const char *needle, size_t nn) {
    if (nn == 0 || nn > hn) return 0;
    for (size_t i = 0; i + nn <= hn; i++)
        if (memcmp(hay + i, needle, nn) == 0) return 1;
    return 0;
}

/* 内嵌 HTML/CSS → 引擎管线: 解析 → 级联 → 布局 → display list。
   与真窗口同一条管线(引擎是唯一渲染路径), 探针只是把产物取下来断言。 */
static hn_context *build_ctx(const char *html, float vw, float vh) {
    hn_doc *doc = hn_parse_html(html, strlen(html));
    if (!doc) return NULL;
    hn_sheet *sheet = hn_parse_css("", 0);
    hn_context *ctx = hn_context_create();
    hn_context_set_doc(ctx, doc);          /* context 接管 doc 生命周期 */
    if (sheet) hn_context_add_sheet(ctx, sheet);
    hn_text_backend tb = { NULL, measure_cb, metrics_cb };
    hn_context_layout(ctx, vw, vh, &tb);
    return ctx;
}

/* ---- QUAD 几何判据(确定性, 不含任何度量随机性) ---- */

/* 面积下限: 投影成一条线(rotateY 90° 类, 退化面片)不算可画面。 */
#define QUAD_MIN_AREA 4.0f

/* 鞋带公式面积(绝对值, px²)。透视投影后的面片是平面四边形, 鞋带即面积。 */
static float quad_area(const hn_cmd *q) {
    float a = 0;
    for (int k = 0; k < 4; k++) {
        int j = (k + 1) & 3;
        a += q->qx[k] * q->qy[j] - q->qx[j] * q->qy[k];
    }
    return (float)fabs((double)a) * 0.5f;
}

/* 是否存在斜边(同时有 x 与 y 分量的边)。轴对齐矩形的每条边必有一个
   分量为 0; 斜边 = 矩形被旋转/透视了。容差 0.05px。 */
static int quad_has_oblique_edge(const hn_cmd *q) {
    const float eps = 0.05f;
    for (int k = 0; k < 4; k++) {
        int j = (k + 1) & 3;
        float dx = (float)fabs((double)(q->qx[j] - q->qx[k]));
        float dy = (float)fabs((double)(q->qy[j] - q->qy[k]));
        if (dx > eps && dy > eps) return 1;
    }
    return 0;
}

typedef struct {
    int total;        /* QUAD 指令总数 */
    int drawable;     /* 非退化(面积 > QUAD_MIN_AREA) */
    int oblique;      /* 含斜边(非轴对齐 = 有透视形变) */
    int grad_quads;   /* 带渐变字段且两端色非零 */
    int texts;        /* TEXT 指令总数 */
    int text_in_doc;  /* TEXT 内容 ⊆ 期望全文 */
} dl_stats;

static void scan_stats(const hn_display_list *dl, const char *doc_text,
                       dl_stats *st) {
    memset(st, 0, sizeof(*st));
    for (int i = 0; i < dl->count; i++) {
        const hn_cmd *c = &dl->cmds[i];
        if (c->kind == HN_CMD_QUAD) {
            st->total++;
            if (quad_area(c) > QUAD_MIN_AREA) st->drawable++;
            if (quad_has_oblique_edge(c)) st->oblique++;
            /* 渐变口径 = hn_cmd 现有字段(RECT 同款):
               gradient 标记 + 两端色非零(未声明是 0, 透明色同样是 0) */
            if (c->gradient && c->grad_from != 0 && c->grad_to != 0)
                st->grad_quads++;
        } else if (c->kind == HN_CMD_TEXT) {
            st->texts++;
            if (doc_text && c->text && c->text_len > 0 &&
                bytes_find(doc_text, strlen(doc_text), c->text, c->text_len))
                st->text_in_doc++;
        }
    }
}

/* 打印 QUAD 一览(失败诊断用; 全绿时也打印, 便于人审) */
static void dump_quads(const hn_display_list *dl) {
    int k = 0;
    for (int i = 0; i < dl->count; i++) {
        const hn_cmd *c = &dl->cmds[i];
        if (c->kind != HN_CMD_QUAD) continue;
        k++;
        printf("    QUAD %d: (%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f)(%.1f,%.1f)"
               " 面积=%.1f fill=%08X grad=%u from=%08X to=%08X\n",
               k, c->qx[0], c->qy[0], c->qx[1], c->qy[1],
               c->qx[2], c->qy[2], c->qx[3], c->qy[3],
               quad_area(c), c->fill, (unsigned)c->gradient,
               c->grad_from, c->grad_to);
    }
}

/* ==================================================================
 * 用例 A: preserve-3d 立方体
 * 场景带自身旋转(rotateY(-32°) rotateX(18°)) + perspective + preserve-3d,
 * 六个面各 rotateY/X(90° 步进) + translateZ。修复后: 场景旋转复合进各面
 * → 六面全部投影为非轴对齐四边形。修复前: 各面独立投影 —— 场景旋转根本
 * 不进子级, 正对/背对面保持轴对齐方块, 侧/顶/底面近乎退化。
 * ================================================================== */
static const char *CUBE_HTML =
"<style>\n"
"html, body { margin: 0; }\n"
".scene { display: block; width: 200; height: 200; perspective: 600;\n"
"         transform: rotateY(-32deg) rotateX(18deg);\n"
"         transform-style: preserve-3d; }\n"
".face { display: block; width: 120; height: 120; background: #4f7cff; }\n"
".f1 { transform: translateZ(60px); }\n"
".f2 { transform: rotateY(90deg) translateZ(60px); }\n"
".f3 { transform: rotateY(180deg) translateZ(60px); }\n"
".f4 { transform: rotateY(270deg) translateZ(60px); }\n"
".f5 { transform: rotateX(90deg) translateZ(60px); }\n"
".f6 { transform: rotateX(-90deg) translateZ(60px); }\n"
"</style>\n"
"<body><div class=\"scene\">"
"<div class=\"face f1\"></div><div class=\"face f2\"></div>"
"<div class=\"face f3\"></div><div class=\"face f4\"></div>"
"<div class=\"face f5\"></div><div class=\"face f6\"></div>"
"</div></body>";

static void test_cube(void) {
    printf("== A. preserve-3d 立方体: 六面 QUAD + 透视形变 ==\n");
    hn_context *ctx = build_ctx(CUBE_HTML, 800, 900);
    const hn_display_list *dl = ctx ? hn_context_display_list(ctx) : NULL;
    if (!dl) { check(0, "A0 display list 就绪"); return; }
    dl_stats st;
    scan_stats(dl, NULL, &st);
    dump_quads(dl);
    check(st.total >= 6,
          mkstr("A1 立方体产出 >=6 条 QUAD (got %d)", st.total));
    check(st.drawable >= 6,
          mkstr("A2 >=6 条 QUAD 非退化(面积>%gpx², 非一条线) (got %d)",
                (double)QUAD_MIN_AREA, st.drawable));
    check(st.oblique >= 6,
          mkstr("A3 >=6 条 QUAD 四角非轴对齐(含斜边=存在透视形变) (got %d)",
                st.oblique));
    hn_context_destroy(ctx);
}

/* ==================================================================
 * 用例 B: 渐变 3D 卡
 * linear-gradient 底色 + rotateY。修复后: 投影 QUAD 带渐变字段
 * (gradient=1 + grad_from/grad_to 两端色非零)。修复前: QUAD 路径只填
 * 平色, 渐变元素的 background 色是 0 → fill=0 → 投影后全透明。
 * ================================================================== */
static const char *GRAD_HTML =
"<style>\n"
"html, body { margin: 0; }\n"
".gcard { display: block; width: 160; height: 100; transform: rotateY(28deg);\n"
"         perspective: 500;\n"
"         background: linear-gradient(135deg, #ff8a3d, #4f7cff); }\n"
"</style>\n"
"<body><div class=\"gcard\"></div></body>";

static void test_gradient(void) {
    printf("== B. 渐变 3D 卡: QUAD 携带渐变字段 ==\n");
    hn_context *ctx = build_ctx(GRAD_HTML, 800, 600);
    const hn_display_list *dl = ctx ? hn_context_display_list(ctx) : NULL;
    if (!dl) { check(0, "B0 display list 就绪"); return; }
    dl_stats st;
    scan_stats(dl, NULL, &st);
    dump_quads(dl);
    check(st.total >= 1,
          mkstr("B1 3D 卡投影出 QUAD (got %d)", st.total));
    check(st.grad_quads >= 1,
          mkstr("B2 QUAD 带渐变(gradient=1 且 grad_from/grad_to 两端色非零)"
                " (got %d)", st.grad_quads));
    hn_context_destroy(ctx);
}

/* ==================================================================
 * 用例 C: 3D 卡内文字
 * 3D 变换卡带子元素(h3/p)。修复后: 子树文字以 TEXT 指令进显示列表。
 * 修复前: 自身画完 QUAD 直接跳出, 子节点一个都不走 → 文字全丢。
 * ================================================================== */
static const char *CARD_TEXT_HTML =
"<style>\n"
"html, body { margin: 0; }\n"
".tcard { display: block; width: 220; height: 120; transform: rotateY(24deg);\n"
"         perspective: 500; background: #12324a; }\n"
".tcard h3 { font-size: 20; color: #ffffff; margin: 8 0 0 0; }\n"
".tcard p { font-size: 13; color: #9aa3b5; margin: 4 0 0 0; }\n"
"</style>\n"
"<body><div class=\"tcard\" id=\"tcard\"><h3>3D 卡内标题</h3>"
"<p>副标题文本</p></div></body>";

static void test_card_text(void) {
    printf("== C. 3D 卡内文字: 子树 TEXT 指令进显示列表 ==\n");
    hn_context *ctx = build_ctx(CARD_TEXT_HTML, 800, 600);
    const hn_display_list *dl = ctx ? hn_context_display_list(ctx) : NULL;
    if (!dl) { check(0, "C0 display list 就绪"); return; }
    char full[256] = { 0 };
    hn_node *card = hn_doc_find_by_id(hn_context_doc(ctx), "tcard");
    if (card) hn_node_text_content(card, full, sizeof(full) - 1);
    dl_stats st;
    scan_stats(dl, full, &st);
    check(st.texts >= 1,
          mkstr("C1 3D 卡子树 TEXT 指令 >=1 (got %d)", st.texts));
    check(st.text_in_doc >= 1,
          mkstr("C2 至少一条 TEXT 内容 ⊆ 卡片全文「%s」(片段⊆全文, 与度量无关)"
                " (got %d)", full, st.text_in_doc));
    hn_context_destroy(ctx);
}

/* ==================================================================
 * 用例 D: 布局上下文不丢 3D 元素
 * flex 直接子项 / position:absolute 的 3D 变换元素各产出 >=1 条非退化
 * QUAD(被静默丢弃 = 一条都没有)。
 * ================================================================== */
static const char *FLEX_HTML =
"<style>\n"
"html, body { margin: 0; }\n"
".hrow { display: flex; width: 420; height: 140; }\n"
".fitem { display: block; width: 120; height: 80; transform: rotateY(26deg);\n"
"         perspective: 500; background: #2ecc71; }\n"
"</style>\n"
"<body><div class=\"hrow\"><div class=\"fitem\"></div></div></body>";

static const char *ABS_HTML =
"<style>\n"
"html, body { margin: 0; }\n"
".absbox { display: block; position: relative; width: 420; height: 160; }\n"
".ael { position: absolute; left: 24; top: 24; width: 120; height: 80;\n"
"       transform: rotateX(26deg); perspective: 500; background: #9b59ff; }\n"
"</style>\n"
"<body><div class=\"absbox\"><div class=\"ael\"></div></div></body>";

/* inline-block 原子盒: 布局期是行内流里的原子片段(hn_layout.c 的
   is_inline_level 分支), 绘制期必须照常走 paint_walk_g 的 3D 投影主路,
   不得被行内快路径(自带 run 的 inline 分支)截走 */
static const char *INLINE_BLOCK_HTML =
"<style>\n"
"html, body { margin: 0; }\n"
".host { display: block; width: 420; height: 160; }\n"
".ibel { display: inline-block; width: 120; height: 80;\n"
"        transform: rotateY(26deg); perspective: 500; background: #2ecc71; }\n"
"</style>\n"
"<body><div class=\"host\"><div class=\"ibel\"></div></div></body>";

/* tag = 断言编号前缀("D1"/"D3"); what = 布局上下文名 */
static void quad_case(const char *tag, const char *what, const char *html) {
    hn_context *ctx = build_ctx(html, 800, 600);
    const hn_display_list *dl = ctx ? hn_context_display_list(ctx) : NULL;
    if (!dl) { check(0, mkstr("%s0 display list 就绪", tag)); return; }
    dl_stats st;
    scan_stats(dl, NULL, &st);
    dump_quads(dl);
    check(st.total >= 1,
          mkstr("%s %s 3D 元素产出 >=1 条 QUAD (got %d)", tag, what, st.total));
    check(st.drawable >= 1,
          mkstr("%s-b 该 QUAD 非退化(面积>%gpx²) (got %d)",
                tag, (double)QUAD_MIN_AREA, st.drawable));
    hn_context_destroy(ctx);
}

static void test_layout_contexts(void) {
    printf("== D. 布局上下文: flex 子项 / absolute / inline-block 的 3D 元素不被丢弃 ==\n");
    quad_case("D1", "flex 直接子项", FLEX_HTML);
    quad_case("D2", "inline-block 原子盒", INLINE_BLOCK_HTML);
    quad_case("D3", "absolute", ABS_HTML);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("quad3d_probe — 3D 场景面 display list 断言"
           "(preserve-3d 复合 / QUAD 渐变 / 3D 子树文字 / 布局上下文)\n"
           "断言锚定修复后的正确行为; 修复前必须逐条红。\n\n");
    test_cube();
    test_gradient();
    test_card_text();
    test_layout_contexts();

    printf("\n结果: %d 通过, %d 失败\n", g_pass, g_fail);
    if (g_fail == 0) printf("全绿 — 3D 边界已收口\n");
    return g_fail ? 1 : 0;
}

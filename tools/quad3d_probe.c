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
 * ---- E-J(上一轮验收遗留清单, 修复前必须红)----
 *   E. 面片渐变像素: linear-gradient 的 rotateY 3D 卡经 hnsoft_render 出位图,
 *      沿渐变轴两端采样 —— 两端颜色必须显著不同, 且**逼近声明的端色**
 *      (CSS 口径: 轴端 = 渐变端色; 当前 hnsoft 渐变轴长按 2*half 计算,
 *      面片两端只落在渐变 25%/75% 处)。
 *   F. RECT 渐变轴长 = CSS 口径: 竖直 linear-gradient(180deg, 黑, 白) 的
 *      100px 高元素, 位图 y≈0 接近黑、y≈100 接近白、中点≈中灰
 *      (当前 hnsoft 的 grad_axis_t 分母是 half*2 —— 轴长两倍于 CSS 的
 *      |W·sinθ|+|H·cosθ|, 元素两端只到 25%/75% → 顶不黑白不白)。
 *   G. cairo 缝线: cairo 后端(本机有 cairo 才编译, 否则 skip)对同一渐变
 *      QUAD 的对角线(双三角拆分线)采样, 相邻采样亮度跳变不得 >2/255
 *      (当前两片三角形各自 clip 后 paint, 拆分线处异常)。
 *   H. preserve-3d 的 2D 分量: 父级 scale(2) + preserve-3d + 子级 rotateY
 *      面片 —— 子面片 QUAD 的投影尺寸应是基准(无 scale)的约 2 倍
 *      (当前 st_local_3d 只复合 rotate/translateZ, 2D scale 不进 3D 矩阵,
 *      preserve-3d 子级拿到的复合矩阵不含祖先缩放 → 比值 ≈1)。
 *   I. flat 摊平精确性: 父级大 rotateY(透视) + 子盒贴远边 —— 子盒中心经
 *      父平面投影(单应)映射后应落在投影四边形远边的对应位置
 *      (当前 install_flatten 用左上/右上/左下三点仿射拟合, 表达不了
 *      透视, 远边误差达数十 px)。
 *   J. 文字与装饰随变换: (a) rotateY 面片上近/远两端的 TEXT 字号应随透视
 *      缩放 —— 面片远端的字更小(当前摊平只有均匀 sqrt|det| 一档 → 两端
 *      同字号);
 *      (b) scale(2) 祖先下的 li 标记坐标/尺寸应翻倍(当前 paint_marker
 *      直接用 n->bx/by 减 sx/sy, 不经 tfx/tfy 也不随 scale);
 *      text-decoration 线坐标/厚应翻倍(238ec95 已把 push_deco 接进仿射,
 *      断言钉住防回退)。
 *
 * 断言全部是 display list / hnsoft 内存位图层面的确定性结论(指令形态/
 * 几何/像素采样), 不依赖文本度量(与 render_probe 同口径: 文字宽度因
 * FreeType 有无而不同, 不进断言)。像素采样直接读 hnsoft_render 的
 * 内存位图, 不经 PNG。
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
 *
 * cairo 变体(用例 G): 本机 pkg-config 找得到 cairo 时, 追加
 *   -DQUAD3D_HAVE_CAIRO $(pkg-config --cflags --libs cairo) \
 *   Sources/CHtmlNative/hn_cairo.c
 * 再编一份运行 —— G 才真正执行; 没编 cairo 时 G 打印 skip(不算失败)。
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
/* cairo 后端(用例 G): 只有 py 门禁探测到本机 cairo 并定义
   QUAD3D_HAVE_CAIRO 时才编入 hn_cairo.c 并包含这份头。 */
#ifdef QUAD3D_HAVE_CAIRO
#include "../Sources/CHtmlNative/hn_cairo.h"
#endif

static int g_pass = 0, g_fail = 0, g_skip = 0;

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

/* ==================================================================
 * 以下 E-J: 上一轮验收遗留清单(修复前必须逐条红; A-D 不得回退)
 * 像素级断言直接读 hnsoft_render 的内存位图(RGBA8), 不经 PNG。
 * ================================================================== */

/* ---- 位图采样工具 ---- */

/* 打包色亮度(Rec.601 整数近似, 0..255) */
static unsigned lum_rgb(hn_color c) {
    unsigned r = (c >> 24) & 0xFF, g = (c >> 16) & 0xFF, b = (c >> 8) & 0xFF;
    return (r * 19595u + g * 38470u + b * 7471u) >> 16;
}

/* 位图双线性参数点: 四角 0/1/2/3 的双线性插值(凸四边形内必在内部) */
static void quad_bilinear(const hn_cmd *q, float u, float v,
                          float *px, float *py) {
    float w0 = (1 - u) * (1 - v), w1 = u * (1 - v);
    float w2 = u * v, w3 = (1 - u) * v;
    *px = w0 * q->qx[0] + w1 * q->qx[1] + w2 * q->qx[2] + w3 * q->qx[3];
    *py = w0 * q->qy[0] + w1 * q->qy[1] + w2 * q->qy[2] + w3 * q->qy[3];
}

/* 内存位图取亮度(坐标四舍五入到像素; 越界钳制) */
static unsigned lum_px(const unsigned char *bm, int w, int h, float x, float y) {
    int ix = (int)(x + 0.5f), iy = (int)(y + 0.5f);
    if (ix < 0) ix = 0;
    if (ix > w - 1) ix = w - 1;
    if (iy < 0) iy = 0;
    if (iy > h - 1) iy = h - 1;
    const unsigned char *p = bm + ((size_t)iy * w + ix) * 4;
    return ((unsigned)p[0] * 19595u + (unsigned)p[1] * 38470u
            + (unsigned)p[2] * 7471u) >> 16;
}

/* 深度优先找第一个指定标签的元素节点(公开 API 只有 tag 访问器,
   非元素节点 tag 返回 NULL, 恰好天然跳过文本节点) */
static hn_node *find_tag(hn_node *n, const char *tag) {
    const char *t = hn_node_tag(n);
    if (t && !strcmp(t, tag)) return n;
    for (hn_node *c = hn_node_first_child(n); c; c = hn_node_next_sibling(c)) {
        hn_node *hit = find_tag(c, tag);
        if (hit) return hit;
    }
    return NULL;
}

/* 按平色找第一条 QUAD / RECT(用独占底色给目标元素打标, 避免按序数猜) */
static const hn_cmd *find_quad_by_fill(const hn_display_list *dl, hn_color fill) {
    for (int i = 0; i < dl->count; i++) {
        const hn_cmd *c = &dl->cmds[i];
        if (c->kind == HN_CMD_QUAD && c->fill == fill) return c;
    }
    return NULL;
}
static const hn_cmd *find_rect_by_fill(const hn_display_list *dl, hn_color fill) {
    for (int i = 0; i < dl->count; i++) {
        const hn_cmd *c = &dl->cmds[i];
        if (c->kind == HN_CMD_RECT && c->fill == fill) return c;
    }
    return NULL;
}
/* 该平色的最大 TEXT 字号(J 的透视字号断言; 字号在指令上, 与度量无关) */
static float max_text_size_by_fill(const hn_display_list *dl, hn_color fill) {
    float best = 0;
    for (int i = 0; i < dl->count; i++) {
        const hn_cmd *c = &dl->cmds[i];
        if (c->kind == HN_CMD_TEXT && c->fill == fill && c->font.size_px > best)
            best = c->font.size_px;
    }
    return best;
}

/* QUAD 顶点 0→1 边长(H 的投影尺寸口径: 上边 = 元素盒宽的投影) */
static float quad_edge01(const hn_cmd *q) {
    float dx = q->qx[1] - q->qx[0], dy = q->qy[1] - q->qy[0];
    return (float)sqrt((double)dx * dx + (double)dy * dy);
}

/* ---- 8x8 线性方程组(列主元消元)—— 单应求解的内核 ---- */
static int solve8(double a[8][9], double x[8]) {
    for (int col = 0; col < 8; col++) {
        int piv = col;
        for (int r = col + 1; r < 8; r++)
            if (fabs(a[r][col]) > fabs(a[piv][col])) piv = r;
        if (fabs(a[piv][col]) < 1e-12) return 0;
        if (piv != col)
            for (int k = 0; k < 9; k++) {
                double t = a[col][k]; a[col][k] = a[piv][k]; a[piv][k] = t;
            }
        for (int r = 0; r < 8; r++) {
            if (r == col) continue;
            double f = a[r][col] / a[col][col];
            for (int k = col; k < 9; k++) a[r][k] -= f * a[col][k];
        }
    }
    for (int i = 0; i < 8; i++) x[i] = a[i][8] / a[i][i];
    return 1;
}

/* 元素盒四角 → 投影四边形四角的单应 H(3x3, h[8]=1)。
   透视投影把元素平面映到屏幕是仿射表达不了的双线性射影 —— 四对对应点
   唯一确定单应, 这才是"平面上任一点该落在哪"的精确口径。 */
static int homography_box_to_quad(float bx, float by, float bw, float bh,
                                  const float qx[4], const float qy[4],
                                  double h[9]) {
    static const float fx[4] = { 0, 1, 1, 0 }, fy[4] = { 0, 0, 1, 1 };
    double a[8][9];
    memset(a, 0, sizeof(a));
    for (int k = 0; k < 4; k++) {
        double x = bx + fx[k] * bw, y = by + fy[k] * bh;
        double u = qx[k], v = qy[k];
        double r1[9] = { x, y, 1, 0, 0, 0, -u * x, -u * y, u };
        double r2[9] = { 0, 0, 0, x, y, 1, -v * x, -v * y, v };
        for (int j = 0; j < 9; j++) { a[2 * k][j] = r1[j]; a[2 * k + 1][j] = r2[j]; }
    }
    double s[8];
    if (!solve8(a, s)) return 0;
    for (int i = 0; i < 8; i++) h[i] = s[i];
    h[8] = 1.0;
    return 1;
}

static void homography_apply(const double h[9], double x, double y,
                             double *pu, double *pv) {
    double w = h[6] * x + h[7] * y + h[8];
    *pu = (h[0] * x + h[1] * y + h[2]) / w;
    *pv = (h[3] * x + h[4] * y + h[5]) / w;
}

/* ==================================================================
 * 用例 E: 面片渐变像素(hnsoft 位图)
 * rotateY 3D 卡 + linear-gradient(90deg, 深灰, 浅灰) → 渲染位图沿渐变轴
 * 两端采样: E1 两端显著不同; E2 两端逼近声明端色(CSS: 轴端=端色;
 * 当前 grad_axis_t 轴长按 2*half, 两端只落在渐变 25%/75% → E2 红)。
 * ================================================================== */
static const char *EGRAD_HTML =
"<style>\n"
"html, body { margin: 0; }\n"
".gq { display: block; width: 160; height: 100; transform: rotateY(35deg);\n"
"      perspective: 500;\n"
"      background: linear-gradient(90deg, #141414, #ececec); }\n"
"</style>\n"
"<body><div class=\"gq\"></div></body>";

static void test_grad_pixels(void) {
    printf("== E. 面片渐变像素: hnsoft 位图沿渐变轴两端采样 ==\n");
    hn_context *ctx = build_ctx(EGRAD_HTML, 800, 600);
    const hn_display_list *dl = ctx ? hn_context_display_list(ctx) : NULL;
    if (!dl) { check(0, "E0 display list 就绪"); return; }
    const hn_cmd *q = NULL;
    for (int i = 0; i < dl->count; i++) {
        const hn_cmd *c = &dl->cmds[i];
        if (c->kind == HN_CMD_QUAD && c->gradient &&
            c->grad_from != 0 && c->grad_to != 0) { q = c; break; }
    }
    if (!q) { check(0, "E0 渐变 QUAD 存在"); hn_context_destroy(ctx); return; }
    unsigned char *bm = hnsoft_render(dl, 800, 600, 0x0B0E13FF);
    if (!bm) { check(0, "E0 hnsoft 位图就绪"); hn_context_destroy(ctx); return; }
    float x1, y1, x2, y2;
    quad_bilinear(q, 0.12f, 0.5f, &x1, &y1);   /* 渐变轴近端(左) */
    quad_bilinear(q, 0.88f, 0.5f, &x2, &y2);   /* 渐变轴远端(右) */
    unsigned l1 = lum_px(bm, 800, 600, x1, y1);
    unsigned l2 = lum_px(bm, 800, 600, x2, y2);
    unsigned w1 = lum_rgb(q->grad_from), w2 = lum_rgb(q->grad_to);
    printf("    近端(%.0f,%.0f)亮度=%u  远端(%.0f,%.0f)亮度=%u  声明端色亮度 %u/%u\n",
           (double)x1, (double)y1, l1, (double)x2, (double)y2, l2, w1, w2);
    check(l1 > l2 + 60 || l2 > l1 + 60,
          mkstr("E1 渐变轴两端颜色显著不同(|Δ亮度|>60) (got %u vs %u)", l1, l2));
    check((int)abs((int)l1 - (int)w1) <= 40 && (int)abs((int)l2 - (int)w2) <= 40,
          mkstr("E2 两端逼近声明端色(CSS 轴端口径 ±40) (got %u/%u want %u/%u)",
                l1, l2, w1, w2));
    free(bm);
    hn_context_destroy(ctx);
}

/* ==================================================================
 * 用例 F: RECT 渐变轴长 = CSS 口径
 * 竖直 linear-gradient(180deg, 黑, 白) 的 100px 高元素: 位图 y≈0 接近黑、
 * y≈100 接近白、中点≈中灰。CSS 轴长 = |W·sinθ|+|H·cosθ| = 100;
 * 当前 hnsoft 的 grad_axis_t 分母是 half*2=200 → 两端只到 25%/75%。
 * ================================================================== */
static const char *VFADE_HTML =
"<style>\n"
"html, body { margin: 0; }\n"
".vfade { display: block; width: 120; height: 100;\n"
"         background: linear-gradient(180deg, #000000, #ffffff); }\n"
"</style>\n"
"<body><div class=\"vfade\" id=\"vf\"></div></body>";

static void test_rect_axis(void) {
    printf("== F. RECT 渐变轴长 = CSS 口径(180deg: 顶黑/底白/中灰) ==\n");
    hn_context *ctx = build_ctx(VFADE_HTML, 400, 300);
    const hn_display_list *dl = ctx ? hn_context_display_list(ctx) : NULL;
    hn_node *vf = ctx ? hn_doc_find_by_id(hn_context_doc(ctx), "vf") : NULL;
    if (!dl || !vf) { check(0, "F0 display list / 元素就绪"); hn_context_destroy(ctx); return; }
    float bx, by, bw, bh;
    hn_node_box(vf, &bx, &by, &bw, &bh);
    unsigned char *bm = hnsoft_render(dl, 400, 300, 0x0B0E13FF);
    if (!bm) { check(0, "F0 hnsoft 位图就绪"); hn_context_destroy(ctx); return; }
    float cx = bx + bw * 0.5f;
    unsigned top = lum_px(bm, 400, 300, cx, by + 2);
    unsigned mid = lum_px(bm, 400, 300, cx, by + bh * 0.5f);
    unsigned bot = lum_px(bm, 400, 300, cx, by + bh - 2);
    printf("    盒(%.0f,%.0f %0.fx%.0f) 亮度: 顶=%u 中=%u 底=%u\n",
           (double)bx, (double)by, (double)bw, (double)bh, top, mid, bot);
    check(top <= 45, mkstr("F1 y≈0 接近黑(≤45) (got %u)", top));
    check(bot >= 210, mkstr("F2 y≈100 接近白(≥210) (got %u)", bot));
    check(mid >= 105 && mid <= 150, mkstr("F3 中点≈中灰(105..150) (got %u)", mid));
    free(bm);
    hn_context_destroy(ctx);
}

/* ==================================================================
 * 用例 G: cairo 渐变面片的双三角拆分线
 * 同一渐变 QUAD 经 hncairo_render 出位图, 沿对角线 0→2(两片三角形的
 * 拆分线)按 ~1px 步进采样: 相邻采样亮度跳变不得 >2/255(底层渐变本身
 * 平缓, 斜率贡献 ≈1/px; 拆分缝会呈离散跳变)。本机无 cairo → skip。
 * ================================================================== */
static void test_cairo_seam(void) {
    printf("== G. cairo 渐变面片: 双三角拆分线(对角线)无异常跳变 ==\n");
#ifndef QUAD3D_HAVE_CAIRO
    printf("  ○ G skip: 本探针未编入 cairo 后端(门禁探测本机无 cairo) —— 不参与判定\n");
    g_skip++;
#else
    hn_context *ctx = build_ctx(EGRAD_HTML, 800, 600);
    const hn_display_list *dl = ctx ? hn_context_display_list(ctx) : NULL;
    if (!dl) { check(0, "G0 display list 就绪"); return; }
    const hn_cmd *q = NULL;
    for (int i = 0; i < dl->count; i++) {
        const hn_cmd *c = &dl->cmds[i];
        if (c->kind == HN_CMD_QUAD && c->gradient &&
            c->grad_from != 0 && c->grad_to != 0) { q = c; break; }
    }
    if (!q) { check(0, "G0 渐变 QUAD 存在"); hn_context_destroy(ctx); return; }
    unsigned char *bm = hncairo_render(dl, 800, 600, 0x0B0E13FF);
    if (!bm) { check(0, "G0 cairo 位图就绪"); hn_context_destroy(ctx); return; }
    /* 对角线长度决定采样数(≈1px 步进), 钳到 [64, 512] */
    float ddx = q->qx[2] - q->qx[0], ddy = q->qy[2] - q->qy[0];
    float dlen = (float)sqrt((double)ddx * ddx + (double)ddy * ddy);
    int n = (int)dlen;
    if (n < 64) n = 64;
    if (n > 512) n = 512;
    float maxjump = 0, at_t = 0;
    int prev = -1;
    for (int i = 0; i < n; i++) {
        float t = 0.06f + 0.88f * (float)i / (float)(n - 1);
        float sx = q->qx[0] + ddx * t;
        float sy = q->qy[0] + ddy * t;
        int l = (int)lum_px(bm, 800, 600, sx, sy);
        if (prev >= 0) {
            float jump = (float)abs(l - prev);
            if (jump > maxjump) { maxjump = jump; at_t = t; }
        }
        prev = l;
    }
    printf("    对角线 %d 采样(≈1px 步进), 最大相邻跳变=%.0f/255 (t=%.2f 处)\n",
           n, (double)maxjump, (double)at_t);
    check(maxjump <= 2.0f,
          mkstr("G1 对角线相邻采样无 >2/255 异常跳变 (max %.0f @t=%.2f)",
                (double)maxjump, (double)at_t));
    free(bm);
    hn_context_destroy(ctx);
#endif
}

/* ==================================================================
 * 用例 H: preserve-3d 的 2D 分量(scale 不进 3D 矩阵)
 * 基准: preserve-3d 父级 + rotateY(30°) 子面片; 变体: 父级再叠 scale(2)。
 * CSS 里 scale(2) 进入 3D 复合矩阵, 子面片投影尺寸 = 基准的 2 倍;
 * 当前 st_local_3d 只复合 rotate/translateZ, 2D scale 只活在 2D 绘制
 * 状态里 → 子级拿到的复合矩阵不含缩放, 比值 ≈1。
 * ================================================================== */
static const char *H3D_BASE_HTML =
"<style>\n"
"html, body { margin: 0; }\n"
".h3d { display: block; width: 400; height: 300;\n"
"       transform-style: preserve-3d; }\n"
".hface { display: block; width: 120; height: 80; transform: rotateY(30deg);\n"
"         background: #e74c3c; }\n"
"</style>\n"
"<body><div class=\"h3d\"><div class=\"hface\"></div></div></body>";

static const char *H3D_SCALED_HTML =
"<style>\n"
"html, body { margin: 0; }\n"
".h3d { display: block; width: 400; height: 300; transform: scale(2);\n"
"       transform-style: preserve-3d; }\n"
".hface { display: block; width: 120; height: 80; transform: rotateY(30deg);\n"
"         background: #e74c3c; }\n"
"</style>\n"
"<body><div class=\"h3d\"><div class=\"hface\"></div></div></body>";

static float measure_face_edge(const char *html) {
    hn_context *ctx = build_ctx(html, 800, 900);
    const hn_display_list *dl = ctx ? hn_context_display_list(ctx) : NULL;
    float edge = -1;
    if (dl) {
        const hn_cmd *q = find_quad_by_fill(dl, 0xE74C3CFF);
        if (q) {
            dump_quads(dl);
            edge = quad_edge01(q);
        }
    }
    hn_context_destroy(ctx);
    return edge;
}

static void test_p3d_scale(void) {
    printf("== H. preserve-3d 的 2D 分量: scale(2) 父级下面片投影 ≈2 倍 ==\n");
    float base = measure_face_edge(H3D_BASE_HTML);
    float scaled = measure_face_edge(H3D_SCALED_HTML);
    if (base <= 0 || scaled <= 0) {
        check(0, "H0 两个文档都投影出面片 QUAD");
        return;
    }
    float ratio = scaled / base;
    printf("    基准上边=%.2f  scale(2) 上边=%.2f  比值=%.3f\n",
           (double)base, (double)scaled, (double)ratio);
    check((double)fabs((double)(base - 120.0f * 0.8660254f)) <= 3.0,
          mkstr("H1 基准面片上边 ≈120·cos30°=103.9 (got %.2f)", (double)base));
    check(ratio >= 1.75f && ratio <= 2.25f,
          mkstr("H2 scale(2)+preserve-3d 下面片投影 ≈2 倍基准(1.75..2.25) (got %.3f)",
                (double)ratio));
}

/* ==================================================================
 * 用例 I: flat 摊平精确性(三点仿射表达不了透视)
 * 父卡 rotateY(50°)+perspective(500), 子盒贴远边(left:240/300)。
 * 修复后: 子盒中心经父平面投影(单应)映射, 落在投影四边形远边的对应
 * 位置; 当前 install_flatten 用左上/右上/左下三点仿射拟合 —— 远边第 4 角
 * 都对不上, 子盒中心偏差达数十 px。
 * ================================================================== */
static const char *FLAT_HTML =
"<style>\n"
"html, body { margin: 0; }\n"
".fcard { display: block; position: relative; width: 300; height: 200;\n"
"         transform: rotateY(50deg); perspective: 500; background: #223344; }\n"
".fsub { display: block; position: absolute; left: 240; top: 70; width: 36;\n"
"        height: 24; background: #ff5522; }\n"
"</style>\n"
"<body><div class=\"fcard\" id=\"fc\"><div class=\"fsub\" id=\"fs\"></div></div></body>";

static void test_flatten_accuracy(void) {
    printf("== I. flat 摊平精确性: 远边子盒中心 = 单应映射位置 ==\n");
    hn_context *ctx = build_ctx(FLAT_HTML, 800, 600);
    const hn_display_list *dl = ctx ? hn_context_display_list(ctx) : NULL;
    hn_node *card = ctx ? hn_doc_find_by_id(hn_context_doc(ctx), "fc") : NULL;
    hn_node *sub = ctx ? hn_doc_find_by_id(hn_context_doc(ctx), "fs") : NULL;
    if (!dl || !card || !sub) {
        check(0, "I0 display list / 元素就绪");
        hn_context_destroy(ctx);
        return;
    }
    const hn_cmd *q = find_quad_by_fill(dl, 0x223344FF);
    const hn_cmd *r = find_rect_by_fill(dl, 0xFF5522FF);
    if (!q || !r) { check(0, "I0 父 QUAD + 子 RECT 就绪(独占底色打标)"); hn_context_destroy(ctx); return; }
    float cbx, cby, cbw, cbh, sbx, sby, sbw, sbh;
    hn_node_box(card, &cbx, &cby, &cbw, &cbh);
    hn_node_box(sub, &sbx, &sby, &sbw, &sbh);
    double h[9];
    if (!homography_box_to_quad(cbx, cby, cbw, cbh, q->qx, q->qy, h)) {
        check(0, "I0 单应可解(四角非退化)");
        hn_context_destroy(ctx);
        return;
    }
    double eu, ev;
    homography_apply(h, sbx + sbw * 0.5, sby + sbh * 0.5, &eu, &ev);
    float au = r->x + r->w * 0.5f, av = r->y + r->h * 0.5f;
    float dist = (float)sqrt((double)(au - eu) * (au - eu) + (double)(av - ev) * (av - ev));
    printf("    子盒中心: 实际(%.1f,%.1f) 单应期望(%.1f,%.1f) 偏差=%.1fpx\n",
           (double)au, (double)av, eu, ev, (double)dist);
    check(dist <= 8.0f,
          mkstr("I1 子盒中心落在父面片单应映射位置 ±8px (got %.1fpx)", (double)dist));
    hn_context_destroy(ctx);
}

/* ==================================================================
 * 用例 J: 文字与装饰随变换
 * (a) rotateY(45°)+perspective 面片上近/远两端各放一段文字 —— 字号应随
 *     透视缩放: 远端字更小(期望近 ≈20·1.18、远 ≈20·0.87, 比值 ≈0.73;
 *     当前摊平只有均匀 sqrt|det| 一档 → 两端同字号)。
 *     (注: translateZ 平移面的**整面**字号缩放 install_flatten 已能给对
 *     —— 面上 k 恒定, 均匀缩放恰好精确; 真正缺的是同一张**转动面**内
 *     近远端的字号差。)
 * (b) scale(2) 祖先下的 li 标记: 坐标与尺寸都应翻倍(经 tfx/tfy);
 *     当前 paint_marker 直接用 n->bx/by 减 sx/sy → 原尺寸原坐标。
 *     text-decoration 线(坐标/厚随仿射)钉住防回退。
 * ================================================================== */
static const char *J3D_TEXT_HTML =
"<style>\n"
"html, body { margin: 0; }\n"
".rcard { display: block; position: relative; width: 300; height: 160;\n"
"         transform: rotateY(45deg); perspective: 500; background: #223344; }\n"
".tn { display: block; position: absolute; left: 10; top: 60; color: #ff2020;\n"
"      font-size: 20; }\n"
".tf { display: block; position: absolute; left: 230; top: 60; color: #20ff40;\n"
"      font-size: 20; }\n"
"</style>\n"
"<body><div class=\"rcard\"><span class=\"tn\">近字</span>"
"<span class=\"tf\">远字</span></div></body>";

static const char *J_SCALE2_HTML =
"<style>\n"
"html, body { margin: 0; }\n"
".s2host { display: block; width: 400; height: 300; transform: scale(2); }\n"
".s2host li { color: #ff2200; font-size: 20; }\n"
".dk { display: block; text-decoration: underline; color: #2080ff; font-size: 20; }\n"
"</style>\n"
"<body><div class=\"s2host\" id=\"s2h\">\n"
"<ul><li>列表项</li></ul>\n"
"<span class=\"dk\">下划线文本</span>\n"
"</div></body>";

static void test_text_deco_scale(void) {
    printf("== J. 文字与装饰: 透视字号 / scale(2) 下标记与装饰线 ==\n");
    /* --- (a) 转动面片内文字的透视字号(近大远小) --- */
    hn_context *ctx = build_ctx(J3D_TEXT_HTML, 800, 600);
    const hn_display_list *dl = ctx ? hn_context_display_list(ctx) : NULL;
    if (!dl) { check(0, "J0 display list 就绪"); return; }
    float sn = max_text_size_by_fill(dl, 0xFF2020FF);   /* 近端 #ff2020 */
    float sf = max_text_size_by_fill(dl, 0x20FF40FF);   /* 远端 #20ff40 */
    printf("    TEXT 字号: 面片近端=%.2f 远端=%.2f (期望比值 ≈0.73, 且各不相同)\n",
           (double)sn, (double)sf);
    check(sn > 0 && sf > 0, "J0 近/远端文字各产出 TEXT 指令");
    check(sn > sf * 1.1f,
          mkstr("J1 面片远端字号随透视变小(近端 > 远端×1.1) (got 近 %.2f vs 远 %.2f)",
                (double)sn, (double)sf));
    hn_context_destroy(ctx);

    /* --- (b) scale(2) 祖先下的 li 标记 / text-decoration --- */
    ctx = build_ctx(J_SCALE2_HTML, 900, 800);
    dl = ctx ? hn_context_display_list(ctx) : NULL;
    hn_node *host = ctx ? hn_doc_find_by_id(hn_context_doc(ctx), "s2h") : NULL;
    /* li 用标签搜索(空白文本节点会让"第一个孩子"链不可靠) */
    hn_node *li = host ? find_tag(host, "li") : NULL;
    if (!dl || !host || !li) {
        check(0, "J3 display list / host / li 就绪");
        hn_context_destroy(ctx);
        return;
    }
    float hbx, hby, hbw, hbh, lbx, lby, lbw, lbh;
    hn_node_box(host, &hbx, &hby, &hbw, &hbh);
    hn_node_box(li, &lbx, &lby, &lbw, &lbh);
    float ox = hbx + hbw * 0.5f, oy = hby + hbh * 0.5f;   /* scale(2) 原点 = 盒中心 */
    const hn_cmd *mk = find_rect_by_fill(dl, 0xFF2200FF); /* li 标记(独占色) */
    if (!mk) { check(0, "J3 li 标记 RECT 就绪"); hn_context_destroy(ctx); return; }
    /* 期望: 标记局部中心 (bx-11.5, by+11.5) 经 scale(2) 映射; 尺寸翻倍 */
    float lcx = lbx - 11.5f, lcy = lby + 20.0f * 0.45f + 2.5f;
    float ecx = (lcx - ox) * 2.0f + ox, ecy = (lcy - oy) * 2.0f + oy;
    float acx = mk->x + mk->w * 0.5f, acy = mk->y + mk->h * 0.5f;
    float mdist = (float)sqrt((double)(acx - ecx) * (acx - ecx)
                            + (double)(acy - ecy) * (acy - ecy));
    printf("    li 标记: 实际 %.0fx%.0f @(%.1f,%.1f)  期望 10x10 @(%.1f,%.1f)\n",
           (double)mk->w, (double)mk->h, (double)acx, (double)acy, (double)ecx, (double)ecy);
    check((double)fabs((double)(mk->w - 10.0f)) <= 0.6f,
          mkstr("J3 scale(2) 下 li 标记尺寸翻倍(≈10) (got %.1f)", (double)mk->w));
    check(mdist <= 3.0f,
          mkstr("J4 scale(2) 下 li 标记坐标随变换翻倍(±3px) (got %.1fpx)", (double)mdist));
    /* text-decoration 线: 期望坐标 = 文字基线(已变换) + 字号·0.14·2, 厚 1.5·2。
       基线哨兵用 -inf: scale(2) 以盒中心为原点, 内容坐标可以为负。 */
    const hn_cmd *deco = find_rect_by_fill(dl, 0x2080FFFF);
    float tb = -1e30f;
    for (int i = 0; i < dl->count; i++) {
        const hn_cmd *c = &dl->cmds[i];
        if (c->kind == HN_CMD_TEXT && c->fill == 0x2080FFFF) { tb = c->baseline; break; }
    }
    if (deco && tb > -1e29f) {
        float edecoy = tb + 20.0f * 0.14f * 2.0f;
        printf("    装饰线: y=%.1f 厚=%.1f  期望 y=%.1f 厚=3.0\n",
               (double)deco->y, (double)deco->h, (double)edecoy);
        check((double)fabs((double)(deco->y - edecoy)) <= 3.0,
              mkstr("J5 装饰线坐标随 scale(2) 翻倍(±3px) (got %.1f want %.1f)",
                    (double)deco->y, (double)edecoy));
        check((double)fabs((double)(deco->h - 3.0f)) <= 0.6f,
              mkstr("J6 装饰线厚随 scale(2) 翻倍(≈3) (got %.1f)", (double)deco->h));
    } else {
        check(0, "J5/J6 text-decoration 线 RECT + 同色 TEXT 就绪");
    }
    hn_context_destroy(ctx);
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    printf("quad3d_probe — 3D 场景面 display list + 位图断言"
           "(preserve-3d 复合 / QUAD 渐变 / 3D 子树文字 / 布局上下文 /\n"
           "渐变像素 / cairo 缝线 / 2D 分量进 3D 矩阵 / 摊平单应 / 文字装饰随变换)\n"
           "A-D 锚定修复后的正确行为(不得回退); E-J 是上轮验收遗留清单,"
           "修复前必须逐条红。\n\n");
    test_cube();
    test_gradient();
    test_card_text();
    test_layout_contexts();
    test_grad_pixels();
    test_rect_axis();
    test_cairo_seam();
    test_p3d_scale();
    test_flatten_accuracy();
    test_text_deco_scale();

    printf("\n结果: %d 通过, %d 失败", g_pass, g_fail);
    if (g_skip > 0) printf(" (+%d skip)", g_skip);
    printf("\n");
    if (g_fail == 0 && g_skip == 0) printf("全绿 — 3D 边界已收口\n");
    else if (g_fail == 0) printf("绿(含 skip) — 通过项全部成立\n");
    return g_fail ? 1 : 0;
}

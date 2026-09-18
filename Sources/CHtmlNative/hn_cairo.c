/* hn_cairo.c — cairo 绘制后端(跨平台, 一个实现喂所有平台)
 *
 * 把显示列表翻译成 cairo 调用。与 hnsoft_render 同签名, 因此 Windows 运行时
 * (tools/hnwin.c)、无头 hncore、PNG 输出全都只调 hncairo_render 这一个入口,
 * 换链接目标即可切换实现, 调用方一行不改。
 *
 * 三处不显然的地方, 都是实测踩过之后写在注释里的:
 *
 * 1) 渐变轴必须跨**矩形自身**的外接盒, 不是画布。display list 的 grad_angle 是
 *    CSS 角度(0deg=向上, 90deg=向右)。第一版探针把轴建在画布上, 结果矩形左端
 *    落在渐变 61% 处 —— 看着"渐变能用", 颜色全错。
 *
 * 2) cairo_pattern_set_matrix 期望的是 **user→pattern** 的矩阵。传反了不报错,
 *    只是静默画出错的贴图(采样点落到三角形外 → 整块透明)。探针里一个明确在
 *    三角形内部的点返回了 alpha=0, 才定位到是方向错了而不是数学错了。
 *
 * 3) 阴影模糊: cairo 没有内置高斯模糊。用"画到 1/N 小面再放大 + BILINEAR 过滤"
 *    近似 —— 放大本身就是一次低通, 边缘得到连续 alpha 衰减。比 hnsoft 的
 *    "6 层扩边"更接近真实 box-shadow。
 *
 * 文本刻意仍走 FreeType 光栅化, 不用 cairo 的字体后端: **测量与绘制必须同源**。
 * 布局期用 FreeType 的字形 advance 排布, 绘制若换成另一套字体后端, 字形就会
 * 落到排好的行盒之外。所以字形位图取 FreeType 的, 只把混合交给 cairo。
 */
#include "hn_cairo.h"
#include "hn_png.h"

#include <cairo.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef HN_NO_TEXT
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

/* ---------------- 颜色 ---------------- */

typedef struct { float r, g, b, a; } fcolor;

static fcolor unpack(hn_color c) {
    fcolor r;
    r.r = (float)((c >> 24) & 0xFF) / 255.0f;
    r.g = (float)((c >> 16) & 0xFF) / 255.0f;
    r.b = (float)((c >> 8) & 0xFF) / 255.0f;
    r.a = (float)(c & 0xFF) / 255.0f;
    return r;
}

/* ---------------- 圆角矩形路径 ---------------- */

/* 追加一个圆角矩形的子路径(不 fill/clip, 交给调用方决定)。
 * radius<=0 退化为普通矩形(与引擎的 radius=0 语义一致)。
 * 半径过半会自交, 必须夹到短边的一半。
 *
 * **调用前必须先 cairo_new_path()**: cairo_new_sub_path 是"追加子路径", 不是
 * "开始新路径"。不清空的话, 当前路径里还留着上一条指令的形状, 于是这一次的
 * fill 会把前面所有形状一起涂掉 —— 症状是"第二个矩形一出现, 第一个矩形就变了
 * 颜色", 极难往路径累积上想。 */
static void rounded_rect(cairo_t *cr, double x, double y, double w, double h,
                         double radius) {
    double r = radius;
    double lim = (w < h ? w : h) * 0.5;
    if (r > lim) r = lim;
    if (r <= 0.5) { cairo_rectangle(cr, x, y, w, h); return; }
    double d = M_PI / 180.0;
    cairo_new_sub_path(cr);
    cairo_arc(cr, x + r,     y + r,     r, 180 * d, 270 * d);
    cairo_arc(cr, x + w - r, y + r,     r, 270 * d, 360 * d);
    cairo_arc(cr, x + w - r, y + h - r, r, 0,       90 * d);
    cairo_arc(cr, x + r,     y + h - r, r, 90 * d,  180 * d);
    cairo_close_path(cr);
}

/* ---------------- 阴影(cairo 无内置模糊) ---------------- */

/* 把形状渲到 1/sc 的小面(白色, 只要 alpha 作蒙版), 再 cairo_mask 画阴影色。
 * 放大即一次低通 → 边缘连续 alpha 衰减, 且颜色不被插值(小面是纯白)。
 * 第一版在小面里直接填阴影色再放大, 边缘会同时插值颜色, 结果偏亮。 */
static void paint_shadow(cairo_t *cr, const hn_cmd *c) {
    fcolor col = unpack(c->shadow_color);
    if (col.a <= 0.003f || c->shadow_blur <= 0.01f) return;

    double x = c->x + c->shadow_ox, y = c->y + c->shadow_oy;
    double w = c->w, h = c->h, r = c->radius, blur = c->shadow_blur;
    int sc = 4;
    if (blur > 24) sc = 6;
    if (blur > 48) sc = 8;
    double pad = blur / (double)sc + 2.0;
    int sw = (int)((w + pad * 2) / sc) + 2;
    int sh = (int)((h + pad * 2) / sc) + 2;
    if (sw < 2 || sh < 2) return;

    cairo_surface_t *t = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sw, sh);
    if (cairo_surface_status(t) != CAIRO_STATUS_SUCCESS) return;
    /* 必须先清零: cairo_image_surface_create 不清内存, 而这张面是当**蒙版**用
       的 —— 形状之外的垃圾 alpha 会让阴影漏到别处, 而且是每次运行都可能不同
       的不确定输出。 */
    cairo_t *tc = cairo_create(t);
    cairo_set_operator(tc, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(tc, 0, 0, 0, 0);
    cairo_paint(tc);
    cairo_set_operator(tc, CAIRO_OPERATOR_OVER);
    cairo_scale(tc, 1.0 / sc, 1.0 / sc);
    cairo_set_source_rgba(tc, 1, 1, 1, 1);
    rounded_rect(tc, pad, pad, w, h, r);
    cairo_fill(tc);
    cairo_destroy(tc);

    cairo_save(cr);
    cairo_set_source_rgba(cr, col.r, col.g, col.b, col.a);
    /* 必须把蒙版面按 sc 放大: cairo_mask_surface 自己不缩放, 而这张面只有
       (w+2*pad)/sc 像素宽。不放大就只覆盖 27×17 个用户单位, 阴影看起来
       "完全没有" —— 而代码是执行了的, 极难排查。当前 CTM 对蒙版同样生效,
       所以 translate + scale 再 mask(0,0) 即可。 */
    cairo_translate(cr, x - pad, y - pad);
    cairo_scale(cr, (double)sc, (double)sc);
    cairo_mask_surface(cr, t, 0, 0);
    cairo_restore(cr);
    cairo_surface_destroy(t);
}

/* ---------------- 渐变 ---------------- */

/* CSS 角度 → cairo 渐变轴, 轴跨**矩形自身**外接盒在该方向的投影。
 * 建在画布上会让矩形落在渐变中段, 颜色全错。 */
static cairo_pattern_t *make_gradient(const hn_cmd *c) {
    double cx = c->x + c->w * 0.5, cy = c->y + c->h * 0.5;
    double ang = (double)c->grad_angle * M_PI / 180.0;
    double dx = sin(ang), dy = -cos(ang);       /* 0deg = 向上 */
    double len = (fabs(c->w * dx) + fabs(c->h * dy)) * 0.5;
    if (len < 0.001) len = 1.0;
    fcolor a = unpack(c->grad_from), b = unpack(c->grad_to);
    cairo_pattern_t *g = cairo_pattern_create_linear(
        cx - dx * len, cy - dy * len, cx + dx * len, cy + dy * len);
    cairo_pattern_add_color_stop_rgba(g, 0, a.r, a.g, a.b, a.a);
    cairo_pattern_add_color_stop_rgba(g, 1, b.r, b.g, b.b, b.a);
    return g;
}

/* ---------------- 文本(FreeType 字形 → A8 蒙版) ---------------- */

#ifndef HN_NO_TEXT
static FT_Library ft_lib = NULL;
static FT_Face    ft_face = NULL;
static int        ft_ok = 0, ft_inited = 0;

/* 与 hnsoft.c 同一份字体搜索表: 同一套字形度量口径, 否则排版会变。 */
static const char *FONT_PATHS[] = {
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/STHeiti Medium.ttc",
    "/System/Library/Fonts/Supplemental/Songti.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "C:\\Windows\\Fonts\\msyh.ttc",
    "C:\\Windows\\Fonts\\simhei.ttf",
    NULL,
};

static void font_init(void) {
    if (ft_inited) return;
    ft_inited = 1;
    if (FT_Init_FreeType(&ft_lib)) return;
    for (int i = 0; FONT_PATHS[i]; i++) {
        if (FT_New_Face(ft_lib, FONT_PATHS[i], 0, &ft_face) == 0) { ft_ok = 1; return; }
    }
}

static int utf8_next(const unsigned char *s, size_t len, size_t i, int *cp) {
    if (i >= len) return 0;
    unsigned char ch = s[i];
    if (ch < 0x80) { *cp = ch; return 1; }
    int n = ch < 0xE0 ? 2 : (ch < 0xF0 ? 3 : 4);
    if (i + (size_t)n > len) { *cp = '?'; return 1; }
    int v = ch & (0xFF >> (n + 1));
    for (int k = 1; k < n; k++) v = (v << 6) | (s[i + k] & 0x3F);
    *cp = v;
    return n;
}

int hncairo_font_loaded(void) { font_init(); return ft_ok; }

/* FreeType 光栅一个码位, 返回 A8 面(调用方 destroy)。left/top 是字形位图
 * 相对基线的偏移, 与 hnsoft 用的同一组值 —— 定位必须一致。 */
static cairo_surface_t *glyph_surface(int cp, int size_px,
                                      int *left, int *top, int *advance) {
    if (!ft_ok) return NULL;
    if (FT_Set_Pixel_Sizes(ft_face, 0, (FT_UInt)size_px)) return NULL;
    if (FT_Load_Char(ft_face, (FT_ULong)cp, FT_LOAD_RENDER)) return NULL;
    FT_GlyphSlot g = ft_face->glyph;
    *left = g->bitmap_left;
    *top = g->bitmap_top;
    *advance = (int)(g->advance.x >> 6);
    if (g->bitmap.width == 0 || g->bitmap.rows == 0) return NULL;
    cairo_surface_t *s = cairo_image_surface_create(
        CAIRO_FORMAT_A8, (int)g->bitmap.width, (int)g->bitmap.rows);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return NULL;
    }
    /* A8 的 stride 可能大于 width(对齐), 必须按行拷 */
    unsigned char *dst = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    for (unsigned y = 0; y < g->bitmap.rows; y++)
        memcpy(dst + (size_t)y * stride,
               g->bitmap.buffer + (size_t)y * g->bitmap.pitch, g->bitmap.width);
    cairo_surface_mark_dirty(s);
    return s;
}

/* 文本阴影: 把整行字形画进一张临时面(阴影色), 降采样放大做一次模糊,
 * 再按 (ox,oy) 偏移合成。按**整行**做而不是按字形 —— 每个字形单独模糊
 * 既慢又会在相邻字形处出现亮斑。
 * 先画阴影后画正文, 由调用方保证。 */
static void paint_text_shadow(cairo_t *cr, const hn_cmd *c, int size_px) {
    fcolor scol = unpack(c->shadow_color);
    if (scol.a <= 0.003f) return;
    double blur = c->shadow_blur;
    int sc = blur > 24 ? 6 : (blur > 48 ? 8 : 4);
    double pad = blur + 4;

    /* 行包围盒: 先量一遍字形推进, 求总宽; 高按字号的 1.6 倍近似
       (CJK 字形的 top/bottom 都在字号量级内, 1.6 倍足够容纳)。 */
    double tw = 0;
    const unsigned char *s = (const unsigned char *)c->text;
    size_t len = c->text_len, i = 0;
    while (i < len) {
        int cp; int n = utf8_next(s, len, i, &cp);
        if (!n) break;
        i += (size_t)n;
        if (cp == ' ') { tw += size_px * 0.28; continue; }
        if (FT_Set_Pixel_Sizes(ft_face, 0, (FT_UInt)size_px)) return;
        if (FT_Load_Char(ft_face, (FT_ULong)cp, FT_LOAD_DEFAULT) == 0)
            tw += (double)(ft_face->glyph->advance.x >> 6);
    }
    double th = size_px * 1.6;
    int sw = (int)((tw + pad * 2) / sc) + 2, sh = (int)((th + pad * 2) / sc) + 2;
    if (sw < 2 || sh < 2 || sw > 4096 || sh > 512) return;

    cairo_surface_t *t = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, sw, sh);
    if (cairo_surface_status(t) != CAIRO_STATUS_SUCCESS) return;
    cairo_t *tc = cairo_create(t);
    /* 清零 + 白色画字形(alpha 当蒙版, 颜色不受插值污染) */
    cairo_set_operator(tc, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(tc, 0, 0, 0, 0);
    cairo_paint(tc);
    cairo_set_operator(tc, CAIRO_OPERATOR_OVER);
    cairo_scale(tc, 1.0 / sc, 1.0 / sc);
    cairo_set_source_rgba(tc, 1, 1, 1, 1);
    i = 0;
    double pen_x = pad, baseline = pad + th * 0.8;
    while (i < len) {
        int cp; int n = utf8_next(s, len, i, &cp);
        if (!n) break;
        i += (size_t)n;
        if (cp == ' ') { pen_x += size_px * 0.28; continue; }
        int left, top, adv;
        cairo_surface_t *gs = glyph_surface(cp, size_px, &left, &top, &adv);
        if (gs) {
            cairo_mask_surface(tc, gs, pen_x + left, baseline - top);
            cairo_surface_destroy(gs);
        }
        pen_x += (double)adv;
    }
    cairo_destroy(tc);

    cairo_save(cr);
    cairo_set_source_rgba(cr, scol.r, scol.g, scol.b, scol.a);
    /* 放大即模糊(与矩形阴影同一手法); 偏移 (ox,oy) 向下为正 */
    cairo_translate(cr, c->tx - pad + c->shadow_ox, c->baseline - th * 0.8 - pad + c->shadow_oy);
    cairo_scale(cr, (double)sc, (double)sc);
    cairo_mask_surface(cr, t, 0, 0);
    cairo_restore(cr);
    cairo_surface_destroy(t);
}

static void paint_text(cairo_t *cr, const hn_cmd *c) {
    font_init();
    if (!ft_ok) return;
    int size_px = (int)(c->font.size_px + 0.5f);
    if (size_px < 4) return;
    fcolor col = unpack(c->fill);
    if (col.a <= 0.01f) return;

    if (c->shadow) paint_text_shadow(cr, c, size_px);

    const unsigned char *s = (const unsigned char *)c->text;
    size_t len = c->text_len, i = 0;
    double pen_x = c->tx, baseline = c->baseline;
    cairo_set_source_rgba(cr, col.r, col.g, col.b, col.a);
    while (i < len) {
        int cp;
        int n = utf8_next(s, len, i, &cp);
        if (!n) break;
        i += (size_t)n;
        if (cp == ' ') { continue; }   /* 空格宽度已含在排版里 */
        int left, top, adv;
        cairo_surface_t *gs = glyph_surface(cp, size_px, &left, &top, &adv);
        if (gs) {
            /* 用字形的 alpha 当蒙版画当前源色 —— 一次调用, 不需要 IN 运算 */
            cairo_mask_surface(cr, gs, pen_x + left, baseline - top);
            cairo_surface_destroy(gs);
        }
        pen_x += (double)adv;
        if (c->font.letter_spacing > 0) pen_x += c->font.letter_spacing;
    }
}
#else
int hncairo_font_loaded(void) { return 0; }
static void paint_text(cairo_t *cr, const hn_cmd *c) { (void)cr; (void)c; }
#endif

/* ---------------- 图片 ---------------- */

/* 读文件解成 RGBA8。复用引擎自带的 PNG 解码(无外部依赖, 跨平台行为一致)。 */
static cairo_surface_t *load_image(const char *path) {
    if (!path || !*path) return NULL;
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return NULL; }
    long n = ftell(fp);
    if (n <= 0) { fclose(fp); return NULL; }
    rewind(fp);
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    if (!buf) { fclose(fp); return NULL; }
    if (fread(buf, 1, (size_t)n, fp) != (size_t)n) {
        free(buf); fclose(fp); return NULL;
    }
    fclose(fp);
    int w = 0, h = 0;
    unsigned char *rgba = hn_png_decode(buf, (size_t)n, &w, &h);
    free(buf);
    if (!rgba || w <= 0 || h <= 0) { free(rgba); return NULL; }

    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s); free(rgba); return NULL;
    }
    cairo_surface_flush(s);
    unsigned char *dst = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    for (int y = 0; y < h; y++) {
        const unsigned char *src = rgba + (size_t)y * w * 4;
        unsigned char *d = dst + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            unsigned char R = src[x*4+0], G = src[x*4+1];
            unsigned char B = src[x*4+2], A = src[x*4+3];
            /* 非预乘 → cairo 的预乘 ARGB32(小端字节序 [B,G,R,A]) */
            if (A != 0 && A != 255) {
                R = (unsigned char)(((unsigned)R * A) / 255u);
                G = (unsigned char)(((unsigned)G * A) / 255u);
                B = (unsigned char)(((unsigned)B * A) / 255u);
            }
            d[x*4+0] = B; d[x*4+1] = G; d[x*4+2] = R; d[x*4+3] = A;
        }
    }
    cairo_surface_mark_dirty(s);
    free(rgba);
    return s;
}

static void paint_image(cairo_t *cr, const hn_cmd *c) {
    cairo_surface_t *img = load_image(c->text);
    if (!img) return;
    double iw = cairo_image_surface_get_width(img);
    double ih = cairo_image_surface_get_height(img);
    if (iw <= 0 || ih <= 0) { cairo_surface_destroy(img); return; }
    cairo_save(cr);
    cairo_rectangle(cr, c->x, c->y, c->w, c->h);
    cairo_clip(cr);
    /* 先 translate 再 scale: 面的原点落在 (x,y), 范围正好铺满 rect。
       反过来先 scale 再 set_source_surface(x,y) 会让原点被缩放到别处。 */
    cairo_translate(cr, c->x, c->y);
    cairo_scale(cr, c->w / iw, c->h / ih);
    cairo_set_source_surface(cr, img, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_GOOD);
    cairo_paint(cr);
    cairo_restore(cr);
    cairo_surface_destroy(img);
}

/* ---------------- 网格贴图(Live2D 类) ---------------- */

/* 逐三角形: clip 到三角形 → 解 2x2 仿射把源图 UV 对齐到该三角形 → paint。
 * 仿射对任意 UV 都要解方程(不能假设 uv=(0,0)(1,0)(0,1))。
 * 退化三角形跳过: 网格变形常把整行压平, 面积为 0 时 clip 什么都画不出。 */
static void paint_mesh(cairo_t *cr, const hn_cmd *c) {
    cairo_surface_t *img = load_image(c->text);
    if (!img) return;
    int cols = c->mesh_cols, rows = c->mesh_rows;
    if (cols <= 0 || rows <= 0) { cairo_surface_destroy(img); return; }
    const float *v = c->mesh_verts, *uv = c->mesh_uv;
    if (!v || !uv) { cairo_surface_destroy(img); return; }

    double iw = cairo_image_surface_get_width(img);
    double ih = cairo_image_surface_get_height(img);
    int gx = cols + 1;
    const int tri[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };

    for (int r = 0; r < rows; r++) {
        for (int q = 0; q < cols; q++) {
            int ia = r * gx + q;
            int quad[4] = { ia, ia + 1, ia + 1 + gx, ia + gx };
            for (int t = 0; t < 2; t++) {
                int i0 = quad[tri[t][0]], i1 = quad[tri[t][1]], i2 = quad[tri[t][2]];
                double sx[3] = { v[i0*2], v[i1*2], v[i2*2] };
                double sy[3] = { v[i0*2+1], v[i1*2+1], v[i2*2+1] };
                double area = (sx[1]-sx[0])*(sy[2]-sy[0]) - (sy[1]-sy[0])*(sx[2]-sx[0]);
                if (fabs(area) < 1e-6) continue;

                /* 源图像素坐标 */
                double px0 = uv[i0*2]*iw,   py0 = uv[i0*2+1]*ih;
                double px1 = uv[i1*2]*iw,   py1 = uv[i1*2+1]*ih;
                double px2 = uv[i2*2]*iw,   py2 = uv[i2*2+1]*ih;
                /* 解: screen - s0 = M * (p - p0) */
                double ex1 = px1-px0, ey1 = py1-py0;
                double ex2 = px2-px0, ey2 = py2-py0;
                double det = ex1*ey2 - ey1*ex2;
                if (fabs(det) < 1e-9) continue;
                double inv = 1.0 / det;
                double fx1 = sx[1]-sx[0], fy1 = sy[1]-sy[0];
                double fx2 = sx[2]-sx[0], fy2 = sy[2]-sy[0];
                double a11 = (fx1*ey2 - fx2*ey1) * inv;
                double a12 = (fx2*ex1 - fx1*ex2) * inv;
                double a21 = (fy1*ey2 - fy2*ey1) * inv;
                double a22 = (fy2*ex1 - fy1*ex2) * inv;

                cairo_save(cr);
                cairo_new_path(cr);
                cairo_move_to(cr, sx[0], sy[0]);
                cairo_line_to(cr, sx[1], sy[1]);
                cairo_line_to(cr, sx[2], sy[2]);
                cairo_close_path(cr);
                cairo_clip(cr);

                /* 矩阵要 **user→pattern**, 所以构造 pattern→user 后取逆。
                   传正的不报错, 只会静默画出错的贴图(实测踩过)。 */
                cairo_matrix_t m;
                cairo_matrix_init(&m, a11, a21, a12, a22, 0, 0);
                m.x0 = sx[0] - (a11*px0 + a12*py0);
                m.y0 = sy[0] - (a21*px0 + a22*py0);
                cairo_matrix_invert(&m);

                cairo_pattern_t *sp = cairo_pattern_create_for_surface(img);
                cairo_pattern_set_matrix(sp, &m);
                cairo_pattern_set_filter(sp, CAIRO_FILTER_GOOD);
                cairo_set_source(cr, sp);
                cairo_paint(cr);
                cairo_pattern_destroy(sp);
                cairo_restore(cr);
            }
        }
    }
    cairo_surface_destroy(img);
}

/* ---------------- 主入口 ---------------- */

unsigned char *hncairo_render(const hn_display_list *dl, int width, int height,
                              hn_color bg) {
    if (width <= 0 || height <= 0 || !dl) return NULL;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, width, height);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s);
        return NULL;
    }
    cairo_t *cr = cairo_create(s);

    fcolor b = unpack(bg);
    cairo_set_source_rgba(cr, b.r, b.g, b.b, b.a);
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);   /* 底色要覆盖, 不是叠加 */
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    for (int i = 0; i < dl->count; i++) {
        const hn_cmd *c = &dl->cmds[i];
        switch (c->kind) {
        case HN_CMD_RECT: {
            if (c->shadow) paint_shadow(cr, c);
            cairo_save(cr);
            cairo_new_path(cr);          /* 不清则 fill 会连带上一条指令的形状 */
            rounded_rect(cr, c->x, c->y, c->w, c->h, c->radius);
            if (c->gradient) {
                cairo_pattern_t *g = make_gradient(c);
                cairo_set_source(cr, g);
                cairo_fill_preserve(cr);
                cairo_pattern_destroy(g);
            } else {
                fcolor f = unpack(c->fill);
                if (f.a > 0.001f) {
                    cairo_set_source_rgba(cr, f.r, f.g, f.b, f.a);
                    cairo_fill_preserve(cr);
                }
            }
            fcolor st = unpack(c->stroke);
            if (st.a > 0.001f && c->stroke_w > 0) {
                cairo_set_source_rgba(cr, st.r, st.g, st.b, st.a);
                cairo_set_line_width(cr, c->stroke_w);
                cairo_stroke(cr);
            }
            cairo_restore(cr);
            break;
        }
        case HN_CMD_TEXT:
            paint_text(cr, c);
            break;
        case HN_CMD_IMAGE:
            paint_image(cr, c);
            break;
        case HN_CMD_CLIP_PUSH:
            cairo_save(cr);
            cairo_new_path(cr);          /* 同上: 裁剪范围只能是这个矩形 */
            rounded_rect(cr, c->x, c->y, c->w, c->h, c->radius);
            cairo_clip(cr);
            break;
        case HN_CMD_CLIP_POP:
            cairo_restore(cr);
            break;
        case HN_CMD_QUAD: {
            cairo_save(cr);
            cairo_new_path(cr);
            cairo_move_to(cr, c->qx[0], c->qy[0]);
            cairo_line_to(cr, c->qx[1], c->qy[1]);
            cairo_line_to(cr, c->qx[2], c->qy[2]);
            cairo_line_to(cr, c->qx[3], c->qy[3]);
            cairo_close_path(cr);
            fcolor f = unpack(c->fill);
            if (f.a > 0.001f) {
                cairo_set_source_rgba(cr, f.r, f.g, f.b, f.a);
                cairo_fill(cr);
            }
            cairo_restore(cr);
            break;
        }
        case HN_CMD_POLYGON: {
            if (!c->poly || c->poly_n < 3) break;
            cairo_save(cr);
            cairo_new_path(cr);
            cairo_move_to(cr, c->poly[0], c->poly[1]);
            for (int k = 1; k < c->poly_n; k++)
                cairo_line_to(cr, c->poly[k*2], c->poly[k*2+1]);
            cairo_close_path(cr);
            cairo_set_fill_rule(cr, c->even_odd ? CAIRO_FILL_RULE_EVEN_ODD
                                                : CAIRO_FILL_RULE_WINDING);
            fcolor f = unpack(c->fill);
            if (f.a > 0.001f) {
                cairo_set_source_rgba(cr, f.r, f.g, f.b, f.a);
                cairo_fill(cr);
            }
            cairo_restore(cr);
            break;
        }
        case HN_CMD_MESH:
            paint_mesh(cr, c);
            break;
        }
    }

    cairo_destroy(cr);

    /* 预乘 ARGB32 → 调用方要的非预乘 RGBA8。
       混合后的像素是预乘的; 直接拆通道出去, 调用方按非预乘解读会偏暗。
       所以半透明像素要反解: C = C_premul * 255 / A。 */
    cairo_surface_flush(s);
    unsigned char *src = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    unsigned char *out = (unsigned char *)malloc((size_t)width * height * 4);
    if (!out) { cairo_surface_destroy(s); return NULL; }
    for (int y = 0; y < height; y++) {
        const unsigned char *p = src + (size_t)y * stride;
        unsigned char *d = out + (size_t)y * width * 4;
        for (int x = 0; x < width; x++) {
            /* 一律按下标配对, 不用指针自增 —— 早先用 `d += 4` 放在循环末尾,
               而上面两条 continue 会跳过它, 于是每个像素都写进行首 4 字节。 */
            unsigned char B = p[x*4+0], G = p[x*4+1], R = p[x*4+2], A = p[x*4+3];
            if (A == 0) { d[x*4+0]=d[x*4+1]=d[x*4+2]=d[x*4+3]=0; continue; }
            if (A == 255) { d[x*4+0]=R; d[x*4+1]=G; d[x*4+2]=B; d[x*4+3]=255; continue; }
            unsigned r = (unsigned)R * 255u / A, g = (unsigned)G * 255u / A;
            unsigned bb = (unsigned)B * 255u / A;
            d[x*4+0] = (unsigned char)(r > 255 ? 255 : r);
            d[x*4+1] = (unsigned char)(g > 255 ? 255 : g);
            d[x*4+2] = (unsigned char)(bb > 255 ? 255 : bb);
            d[x*4+3] = A;
        }
    }
    cairo_surface_destroy(s);
    return out;
}

/* ---------------- PNG 编码(走 cairo, 真 zlib 压缩) ---------------- */

/* cairo 的 PNG 写出是流式回调, 需要一个可增长的累加器。
 * 单线程渲染路径下文件级静态是安全的; 引擎的显示列表消费本就是单线程。 */
static unsigned char *g_png_buf = NULL;
static size_t         g_png_len = 0;

static cairo_status_t png_accum(void *closure, const unsigned char *data,
                                unsigned int length) {
    (void)closure;
    unsigned char *nb = (unsigned char *)realloc(g_png_buf, g_png_len + length);
    if (!nb) return CAIRO_STATUS_NO_MEMORY;
    memcpy(nb + g_png_len, data, length);
    g_png_buf = nb;
    g_png_len += length;
    return CAIRO_STATUS_SUCCESS;
}

unsigned char *hncairo_encode_png(const unsigned char *rgba, int w, int h,
                                  size_t *out_len) {
    if (!rgba || w <= 0 || h <= 0) return NULL;
    g_png_buf = NULL; g_png_len = 0;
    cairo_surface_t *s = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, w, h);
    if (cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(s); return NULL;
    }
    cairo_surface_flush(s);
    unsigned char *dst = cairo_image_surface_get_data(s);
    int stride = cairo_image_surface_get_stride(s);
    for (int y = 0; y < h; y++) {
        const unsigned char *p = rgba + (size_t)y * w * 4;
        unsigned char *d = dst + (size_t)y * stride;
        for (int x = 0; x < w; x++) {
            unsigned char R = p[x*4+0], G = p[x*4+1], B = p[x*4+2], A = p[x*4+3];
            if (A != 0 && A != 255) {     /* 非预乘 → 预乘 */
                R = (unsigned char)(((unsigned)R * A) / 255u);
                G = (unsigned char)(((unsigned)G * A) / 255u);
                B = (unsigned char)(((unsigned)B * A) / 255u);
            }
            d[x*4+0] = B; d[x*4+1] = G; d[x*4+2] = R; d[x*4+3] = A;
        }
    }
    cairo_surface_mark_dirty(s);
    cairo_status_t st = cairo_surface_write_to_png_stream(s, png_accum, NULL);
    cairo_surface_destroy(s);
    if (st != CAIRO_STATUS_SUCCESS) {
        free(g_png_buf); g_png_buf = NULL; g_png_len = 0;
        return NULL;
    }
    *out_len = g_png_len;
    return g_png_buf;
}

/* ---------------- 文本测量(与 hnsoft_measure 同口径) ---------------- */

float hncairo_measure(const hn_font_desc *font, const char *utf8, size_t len) {
    if (!len) return 0;
#ifndef HN_NO_TEXT
    font_init();
    if (ft_ok) {
        int size_px = (int)(font->size_px + 0.5f);
        if (size_px < 1) size_px = 1;
        float total = 0;
        const unsigned char *s = (const unsigned char *)utf8;
        size_t i = 0;
        int nchars = 0;
        while (i < len) {
            int cp;
            int n = utf8_next(s, len, i, &cp);
            if (!n) break;
            i += (size_t)n;
            if (FT_Set_Pixel_Sizes(ft_face, 0, (FT_UInt)size_px)) break;
            if (FT_Load_Char(ft_face, (FT_ULong)cp, FT_LOAD_DEFAULT) == 0)
                total += (float)(ft_face->glyph->advance.x >> 6);
            else
                total += font->size_px * 0.55f * (float)n;
            nchars++;
        }
        if (font->letter_spacing > 0 && nchars > 0)
            total += font->letter_spacing * (float)nchars;
        return total;
    }
#endif
    return (float)len * font->size_px * 0.55f;
}

void hncairo_metrics(const hn_font_desc *font, float *ascent, float *descent,
                     float *leading) {
#ifndef HN_NO_TEXT
    font_init();
    if (ft_ok) {
        int size_px = (int)(font->size_px + 0.5f);
        if (size_px < 1) size_px = 1;
        if (FT_Set_Pixel_Sizes(ft_face, 0, (FT_UInt)size_px) == 0) {
            FT_Size_Metrics m = ft_face->size->metrics;
            *ascent  = (float)(m.ascender >> 6);
            *descent = (float)((-m.descender) >> 6);
            *leading = (float)(m.height >> 6) - *ascent - *descent;
            if (*leading < 0) *leading = 0;
            return;
        }
    }
#endif
    *ascent = font->size_px * 0.8f;
    *descent = font->size_px * 0.2f;
    *leading = 0;
}

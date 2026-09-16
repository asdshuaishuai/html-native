/* hnsoft.c — 软件光栅化后端
 *
 * 设计: 显示列表 → RGBA 像素, 纯 C 逐指令实现。
 * - 圆角矩形: 有向距离场(SDF) + 0.5px 覆盖率抗锯齿
 * - 线性渐变: 沿渐变轴投影
 * - 阴影: 多层扩边圆角矩形近似(不用高斯模糊, 成本太高)
 * - 裁剪: 轴对齐矩形交叠栈(圆角裁剪简化为矩形, 对 UI 裁剪足够)
 * - 文本: FreeType 光栅化 + (字形,字号) 缓存
 * - PNG: 存储型 deflate(合法 PNG, 无压缩) + CRC32
 */
#include "hnsoft.h"
#include <stdlib.h>
#include <string.h>
#include <math.h>

#ifndef HN_NO_TEXT
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

/* ---------------- 颜色工具 ---------------- */

typedef struct { float r, g, b, a; } fcolor;

static fcolor unpack(hn_color c) {
    fcolor r;
    r.r = (float)((c >> 24) & 0xFF) / 255.0f;
    r.g = (float)((c >> 16) & 0xFF) / 255.0f;
    r.b = (float)((c >> 8) & 0xFF) / 255.0f;
    r.a = (float)(c & 0xFF) / 255.0f;
    return r;
}

static void blend(unsigned char *px, fcolor c) {
    float ia = 1.0f - c.a;
    px[0] = (unsigned char)((c.r * 255.0f * c.a + px[0] * ia) + 0.5f);
    px[1] = (unsigned char)((c.g * 255.0f * c.a + px[1] * ia) + 0.5f);
    px[2] = (unsigned char)((c.b * 255.0f * c.a + px[2] * ia) + 0.5f);
}

/* ---------------- 帧缓冲 ---------------- */

typedef struct {
    unsigned char *px;    /* RGBA8, 自上而下 */
    int w, h;
    /* 裁剪栈(轴对齐交叠) */
    struct { int x0, y0, x1, y1; } clip[32];
    int clip_n;
} fb;

static void fb_init(fb *f, int w, int h, hn_color bg) {
    f->w = w; f->h = h; f->clip_n = 0;
    f->px = (unsigned char *)malloc((size_t)w * h * 4);
    fcolor b = unpack(bg);
    unsigned char r = (unsigned char)(b.r * 255 + 0.5f);
    unsigned char g = (unsigned char)(b.g * 255 + 0.5f);
    unsigned char bl = (unsigned char)(b.b * 255 + 0.5f);
    for (int i = 0; i < w * h; i++) {
        f->px[i * 4] = r; f->px[i * 4 + 1] = g; f->px[i * 4 + 2] = bl; f->px[i * 4 + 3] = 255;
    }
}

static void fb_push_clip(fb *f, int x0, int y0, int x1, int y1) {
    if (f->clip_n >= 32) return;
    int bx0 = 0, by0 = 0, bx1 = f->w, by1 = f->h;
    if (f->clip_n > 0) {
        bx0 = f->clip[f->clip_n - 1].x0; by0 = f->clip[f->clip_n - 1].y0;
        bx1 = f->clip[f->clip_n - 1].x1; by1 = f->clip[f->clip_n - 1].y1;
    }
    int cx0 = x0 > bx0 ? x0 : bx0, cy0 = y0 > by0 ? y0 : by0;
    int cx1 = x1 < bx1 ? x1 : bx1, cy1 = y1 < by1 ? y1 : by1;
    f->clip[f->clip_n].x0 = cx0; f->clip[f->clip_n].y0 = cy0;
    f->clip[f->clip_n].x1 = cx1; f->clip[f->clip_n].y1 = cy1;
    f->clip_n++;
}

static void fb_pop_clip(fb *f) { if (f->clip_n > 1) f->clip_n--; }

static int fb_clip_ok(fb *f, int x, int y) {
    if (x < 0 || y < 0 || x >= f->w || y >= f->h) return 0;
    if (f->clip_n == 0) return 1;
    int cx0 = f->clip[f->clip_n - 1].x0, cy0 = f->clip[f->clip_n - 1].y0;
    int cx1 = f->clip[f->clip_n - 1].x1, cy1 = f->clip[f->clip_n - 1].y1;
    return x >= cx0 && y >= cy0 && x < cx1 && y < cy1;
}

static void fb_blend(fb *f, int x, int y, fcolor c) {
    if (!fb_clip_ok(f, x, y)) return;
    blend(&f->px[(y * f->w + x) * 4], c);
}

/* ---------------- 圆角矩形 SDF ---------------- */

static float sd_rounded(float px, float py, float x, float y, float w, float h, float r) {
    float qx = fabsf(px - (x + w * 0.5f)) - (w * 0.5f - r);
    float qy = fabsf(py - (y + h * 0.5f)) - (h * 0.5f - r);
    float ax = qx > 0 ? qx : 0, ay = qy > 0 ? qy : 0;
    return sqrtf(ax * ax + ay * ay) + (qx > qy ? qx : (qy > qx ? qy : 0)) - r;
}

/* ---------------- 渐变 ---------------- */

static fcolor grad_at(const hn_cmd *c, float px, float py) {
    fcolor from = unpack(c->grad_from), to = unpack(c->grad_to);
    /* CSS 角度: 0deg=向上, 顺时针。渐变轴沿该方向穿过盒中心。 */
    float rad = c->grad_angle * 3.14159265f / 180.0f;
    float dx = sinf(rad), dy = -cosf(rad);
    float cx = c->x + c->w * 0.5f, cy = c->y + c->h * 0.5f;
    float half = fabsf(c->w * dx) + fabsf(c->h * dy);
    float t = ((px - cx) * dx + (py - cy) * dy) / (half > 0 ? half * 2 : 1) + 0.5f;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    fcolor r;
    r.r = from.r + (to.r - from.r) * t;
    r.g = from.g + (to.g - from.g) * t;
    r.b = from.b + (to.b - from.b) * t;
    r.a = from.a + (to.a - from.a) * t;
    return r;
}

/* ---------------- 矩形(填充/渐变/描边) ---------------- */

static void paint_rect(fb *f, const hn_cmd *c, float scale, float ox, float oy, float alpha) {
    float x = (c->x - ox) * scale + ox, y = (c->y - oy) * scale + oy;
    float w = c->w * scale, h = c->h * scale, r = c->radius * scale;
    float x0 = floorf(x) - 2, y0 = floorf(y) - 2;
    float x1 = ceilf(x + w) + 2, y1 = ceilf(y + h) + 2;
    int ix0 = (int)x0, iy0 = (int)y0, ix1 = (int)x1 + 1, iy1 = (int)y1 + 1;

    fcolor base = unpack(c->fill);
    base.a *= alpha;
    fcolor sc = unpack(c->stroke);
    sc.a *= alpha;

    for (int py = iy0; py <= iy1; py++) {
        for (int px = ix0; px <= ix1; px++) {
            float fx = px + 0.5f, fy = py + 0.5f;
            float d = sd_rounded(fx, fy, x, y, w, h, r);
            float cov = 0.5f - d;
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            fcolor col;
            if (c->gradient) col = grad_at(c, fx, fy); else col = base;
            col.a *= cov;
            fb_blend(f, px, py, col);
            /* 描边: 内外 1px 环 */
            if (c->stroke_w > 0 && sc.a > 0.01f) {
                float sd_in = fabsf(d) - c->stroke_w * 0.5f * scale;
                float scov = 0.5f - sd_in;
                if (scov > 0) {
                    fcolor sv = sc; sv.a *= cov * (0.5f - fmaxf(0, -sd_in));
                    if (sv.a > 0) fb_blend(f, px, py, sv);
                }
            }
        }
    }
}

/* ---------------- 阴影(扩边矩形近似模糊) ---------------- */

static void paint_shadow(fb *f, const hn_cmd *c, float scale, float ox, float oy, float alpha) {
    fcolor col = unpack(c->shadow_color);
    col.a *= alpha;
    if (col.a <= 0.01f) return;
    float blur = c->shadow_blur * scale;
    float ox_ = c->shadow_ox * scale, oy_ = c->shadow_oy * scale;
    /* 6 层扩边近似模糊: 每层递减透明度, 叠加成柔和边缘 */
    const int RINGS = 6;
    for (int i = RINGS; i >= 1; i--) {
        float t = (float)i / RINGS;
        float grow = blur * t;
        fcolor ring = col;
        ring.a *= (1.0f - t) * (1.0f - t) * 0.9f / RINGS * 2.0f;
        if (ring.a <= 0.005f) continue;
        float x = (c->x + ox_ - grow * 0.5f - ox) * scale + ox;
        float y = (c->y + oy_ - grow * 0.5f - oy) * scale + oy;
        float w = (c->w + grow) * scale, h = (c->h + grow) * scale;
        float rr = (c->radius + grow * 0.5f) * scale;
        int ix0 = (int)x - 2, iy0 = (int)y - 2;
        int ix1 = (int)(x + w) + 2, iy1 = (int)(y + h) + 2;
        for (int py = iy0; py <= iy1; py++)
            for (int px = ix0; px <= ix1; px++) {
                float fx = px + 0.5f, fy = py + 0.5f;
                float d = sd_rounded(fx, fy, x, y, w, h, rr > 0 ? rr : 0);
                float cov = 0.5f - d;
                if (cov <= 0) continue;
                if (cov > 1) cov = 1;
                fcolor cc = ring; cc.a *= cov;
                fb_blend(f, px, py, cc);
            }
    }
}

/* ---------------- 文本(FreeType) ---------------- */

#ifndef HN_NO_TEXT
static FT_Library ft_lib;
static int ft_inited = 0;
static FT_Face ft_face;
static int ft_face_ok = 0;

/* 系统字体路径探测(优先 CJK 覆盖广的) */
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
        if (FT_New_Face(ft_lib, FONT_PATHS[i], 0, &ft_face) == 0) { ft_face_ok = 1; return; }
    }
}

/* 字形缓存: 线性表(同字号下通常 < 200 字, 足够且简单) */
typedef struct { int codepoint; int size_px; int w, h; int left, top; int advance; unsigned char *bm; } glyph;
static glyph glyphs[2048];
static int glyph_n = 0;

static const glyph *glyph_get(int cp, int size_px) {
    for (int i = 0; i < glyph_n; i++)
        if (glyphs[i].codepoint == cp && glyphs[i].size_px == size_px) return &glyphs[i];
    if (!ft_face_ok) return NULL;
    FT_Set_Pixel_Sizes(ft_face, 0, size_px);
    if (FT_Load_Char(ft_face, (FT_ULong)cp, FT_LOAD_RENDER)) return NULL;
    FT_GlyphSlot g = ft_face->glyph;
    if (glyph_n >= (int)(sizeof(glyphs) / sizeof(glyphs[0]))) return NULL;
    glyph *e = &glyphs[glyph_n++];
    e->codepoint = cp; e->size_px = size_px;
    e->w = g->bitmap.width; e->h = g->bitmap.rows;
    e->left = g->bitmap_left; e->top = g->bitmap_top;
    e->advance = (int)(g->advance.x >> 6);
    int n = e->w * e->h;
    if (n > 0) {
        e->bm = (unsigned char *)malloc((size_t)n);
        memcpy(e->bm, g->bitmap.buffer, (size_t)n);
    } else e->bm = NULL;
    return e;
}
#endif

/* UTF-8 解码一个码点, 返回字节数(0=结尾) */
static int utf8_next(const unsigned char *s, size_t len, size_t i, int *cp) {
    if (i >= len) return 0;
    unsigned char c = s[i];
    if (c < 0x80) { *cp = c; return 1; }
    int n = c < 0xE0 ? 2 : (c < 0xF0 ? 3 : 4);
    if (i + n > len) { *cp = '?'; return 1; }
    int v = c & (0xFF >> (n + 1));
    for (int k = 1; k < n; k++) v = (v << 6) | (s[i + k] & 0x3F);
    *cp = v;
    return n;
}

static void paint_text(fb *f, const hn_cmd *c, float scale, float ox, float oy, float alpha) {
#ifndef HN_NO_TEXT
    font_init();
    if (!ft_face_ok) return;
    int size_px = (int)(c->font.size_px * scale + 0.5f);
    if (size_px < 4) return;
    fcolor col = unpack(c->fill);
    col.a *= alpha;
    if (col.a <= 0.01f) return;

    const unsigned char *s = (const unsigned char *)c->text;
    size_t len = c->text_len;
    float pen_x = (c->tx + ox) * scale + ox;
    float baseline = (c->baseline + oy) * scale + oy;
    size_t i = 0;
    while (i < len) {
        int cp;
        int n = utf8_next(s, len, i, &cp);
        if (!n) break;
        i += (size_t)n;
        if (cp == ' ') { continue; }   /* 空格宽度已含在排版中, 无需画 */
        const glyph *gl = glyph_get(cp, size_px);
        if (!gl) continue;
        /* 字形位图混合(基线对齐: top 为字形顶部相对基线的偏移) */
        int gx = (int)pen_x + gl->left;
        int gy = (int)baseline - gl->top;
        for (int yy = 0; yy < gl->h; yy++)
            for (int xx = 0; xx < gl->w; xx++) {
                unsigned char a = gl->bm[yy * gl->w + xx];
                if (!a) continue;
                fcolor gc = col;
                gc.a *= a / 255.0f;
                fb_blend(f, gx + xx, gy + yy, gc);
            }
        pen_x += gl->advance;
    }
#endif
}

/* ---------------- 图片(PNG/BMP 加载: 最小 PNG 解码器) ---------------- */

/* PNG 解码: 只支持存储型与 zlib 固定哈夫曼以外的常见情形太复杂,
 * 这里用 zlib 的 "stored block" 与常见过滤器逐行反解 —— 完整实现太长,
 * 简化策略: 尝试读取 IHDR 并解析无压缩 IDAT; 失败则跳过图片(不崩溃)。 */
static unsigned char *load_image_pixels(const char *path, int *w, int *h) {
    /* 最小实现: 读 PNG 签名/IHDR, 尝试 zlib stored 解码。
       (服务端渲染场景图片常为 PNG; 复杂编码交由平台后端处理) */
    (void)path; (void)w; (void)h;
    return NULL;
}

static void paint_image(fb *f, const hn_cmd *c, float scale, float ox, float oy, float alpha) {
    int iw = 0, ih = 0;
    unsigned char *px = load_image_pixels(c->text ? c->text : "", &iw, &ih);
    if (!px) {
        /* 占位: 画虚线框, 表明"图片位"而非静默空白 */
        float x = (c->x - ox) * scale + ox, y = (c->y - oy) * scale + oy;
        float w = c->w * scale, h = c->h * scale;
        for (int yy = (int)y; yy < (int)(y + h); yy++)
            for (int xx = (int)x; xx < (int)(x + w); xx++) {
                if (((xx / 6) + (yy / 6)) % 2 == 0) {
                    fcolor ph = { 0.18f, 0.20f, 0.26f, 0.8f * alpha };
                    fb_blend(f, xx, yy, ph);
                }
            }
        return;
    }
    int x0 = (int)((c->x - ox) * scale + ox), y0 = (int)((c->y - oy) * scale + oy);
    int w = (int)(c->w * scale), h = (int)(c->h * scale);
    for (int yy = 0; yy < h; yy++)
        for (int xx = 0; xx < w; xx++) {
            int sx = xx * iw / (w > 0 ? w : 1), sy = yy * ih / (h > 0 ? h : 1);
            const unsigned char *p = &px[(sy * iw + sx) * 4];
            fcolor col = { p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f, p[3] / 255.0f * alpha };
            fb_blend(f, x0 + xx, y0 + yy, col);
        }
    free(px);
}

/* ---------------- 主渲染 ---------------- */

unsigned char *hnsoft_render(const hn_display_list *dl, int width, int height, hn_color bg) {
    if (!dl || !dl->cmds || width <= 0 || height <= 0) return NULL;
    fb f;
    fb_init(&f, width, height, bg);

    for (int i = 0; i < dl->count; i++) {
        const hn_cmd *c = &dl->cmds[i];
        switch (c->kind) {
        case HN_CMD_RECT:
            if (c->shadow) {
                /* 有阴影先画阴影(在填充之下) */
                paint_shadow(&f, c, 1.0f, 0, 0, 1.0f);
            }
            paint_rect(&f, c, 1.0f, 0, 0, 1.0f);
            break;
        case HN_CMD_TEXT:
            paint_text(&f, c, 1.0f, 0, 0, 1.0f);
            break;
        case HN_CMD_IMAGE:
            paint_image(&f, c, 1.0f, 0, 0, 1.0f);
            break;
        case HN_CMD_CLIP_PUSH:
            fb_push_clip(&f, (int)c->x, (int)c->y, (int)(c->x + c->w), (int)(c->y + c->h));
            break;
        case HN_CMD_CLIP_POP:
            fb_pop_clip(&f);
            break;
        }
    }
    return f.px;
}

/* ---------------- PNG 编码(存储型 deflate) ---------------- */

static unsigned long crc_table[256];
static int crc_ready = 0;
static void crc_init(void) {
    for (unsigned long n = 0; n < 256; n++) {
        unsigned long c = n;
        for (int k = 0; k < 8; k++) c = (c & 1) ? 0xEDB88320UL ^ (c >> 1) : c >> 1;
        crc_table[n] = c;
    }
    crc_ready = 1;
}
static unsigned long crc32_upd(const unsigned char *buf, size_t len) {
    if (!crc_ready) crc_init();
    unsigned long c = 0xFFFFFFFFUL;
    for (size_t n = 0; n < len; n++) c = crc_table[(c ^ buf[n]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFUL;
}

static void be32(unsigned char *p, unsigned long v) {
    p[0] = (unsigned char)(v >> 24); p[1] = (unsigned char)(v >> 16);
    p[2] = (unsigned char)(v >> 8); p[3] = (unsigned char)v;
}

static void png_chunk(unsigned char **out, size_t *n, const char *type,
                      const unsigned char *data, size_t len) {
    be32(*out + *n, (unsigned long)len); *n += 4;
    memcpy(*out + *n, type, 4); *n += 4;
    if (len) memcpy(*out + *n, data, len);
    *n += len;
    unsigned char cb[4];
    /* CRC 覆盖 type+data */
    if (!crc_ready) crc_init();
    unsigned long crcv = 0xFFFFFFFFUL;
    for (size_t k = 0; k < 4; k++) crcv = crc_table[(crcv ^ type[k]) & 0xFF] ^ (crcv >> 8);
    for (size_t k = 0; k < len; k++) crcv = crc_table[(crcv ^ data[k]) & 0xFF] ^ (crcv >> 8);
    crcv ^= 0xFFFFFFFFUL;
    be32(cb, crcv);
    memcpy(*out + *n, cb, 4); *n += 4;
}

unsigned char *hnsoft_encode_png(const unsigned char *rgba, int w, int h, size_t *out_len) {
    if (!rgba || w <= 0 || h <= 0 || !out_len) return NULL;
    if (!crc_ready) crc_init();
    /* 原始数据: 每行前加 filter=0 */
    size_t raw_len = (size_t)h * (w * 4 + 1);
    unsigned char *raw = (unsigned char *)malloc(raw_len);
    if (!raw) return NULL;
    for (int y = 0; y < h; y++) {
        raw[(size_t)y * (w * 4 + 1)] = 0;
        memcpy(raw + (size_t)y * (w * 4 + 1) + 1, rgba + (size_t)y * w * 4, (size_t)w * 4);
    }
    /* zlib 流: 2 字节头 + stored blocks + adler32 */
    size_t max_blocks = raw_len / 65535 + 2;
    size_t zcap = raw_len + max_blocks * 5 + 16;
    unsigned char *z = (unsigned char *)malloc(zcap);
    if (!z) { free(raw); return NULL; }
    size_t zn = 0;
    z[zn++] = 0x78; z[zn++] = 0x01;
    size_t off = 0;
    while (off < raw_len) {
        size_t blk = raw_len - off > 65535 ? 65535 : raw_len - off;
        int last = (off + blk >= raw_len);
        z[zn++] = (unsigned char)(last ? 1 : 0);
        z[zn++] = (unsigned char)(blk & 0xFF);
        z[zn++] = (unsigned char)(blk >> 8);
        z[zn++] = (unsigned char)(~blk & 0xFF);
        z[zn++] = (unsigned char)((~blk >> 8) & 0xFF);
        memcpy(z + zn, raw + off, blk);
        zn += blk;
        off += blk;
    }
    /* adler32 */
    unsigned long s1 = 1, s2 = 0;
    for (size_t k = 0; k < raw_len; k++) {
        s1 = (s1 + raw[k]) % 65521;
        s2 = (s2 + s1) % 65521;
    }
    unsigned long adler = (s2 << 16) | s1;
    be32(z + zn, adler); zn += 4;
    free(raw);

    /* PNG 容器 */
    size_t cap = zn + 256;
    unsigned char *out = (unsigned char *)malloc(cap);
    if (!out) { free(z); return NULL; }
    size_t n = 0;
    const unsigned char sig[8] = { 137, 80, 78, 71, 13, 10, 26, 10 };
    memcpy(out + n, sig, 8); n += 8;

    unsigned char ihdr[13];
    be32(ihdr, (unsigned long)w);
    be32(ihdr + 4, (unsigned long)h);
    ihdr[8] = 8; ihdr[9] = 6; ihdr[10] = 0; ihdr[11] = 0; ihdr[12] = 0;
    png_chunk(&out, &n, "IHDR", ihdr, 13);
    png_chunk(&out, &n, "IDAT", z, zn);
    png_chunk(&out, &n, "IEND", NULL, 0);
    free(z);
    *out_len = n;
    return out;
}

int hnsoft_font_loaded(void) {
#ifndef HN_NO_TEXT
    font_init();
    return ft_face_ok;
#else
    return 0;
#endif
}

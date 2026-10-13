/* hn_gpu.c — GPU 渲染后端引擎侧: 显示列表 → hnp_gpu_host 原语(翻译器)
 *
 * 分层铁律的 GPU 版(契约见 platform/hn_platform.h 尾部): 平台出**原语**
 * (设备/纹理/四边形/裁剪/读回), 本文件出**画法** —— 逐指令把显示列表翻译
 * 成四边形批, 覆盖不了的指令在 hn_gpu_available 里如实报告, 呼出方(rt)
 * 整帧回退 hnsoft(逐帧、静默)。
 *
 * 宿主槽: 与 hn_media_host 同款全局槽(壳经 hn_rt_set_gpu_host 注入;
 * 引擎不认识任何平台 —— 不引用 hnp_gpu_host() 函数本身, 因此 hncore 等
 * 不链平台文件的产物编译本文件也无未定义符号)。
 *
 * ── 与 hnsoft 的像素等价性(逐条口径) ──
 * 1. 预乘换算链: hnsoft 在**非预乘**空间做 source-over 后写回非预乘 RGBA;
 *    GPU 帧缓冲是**预乘**域。两者数学等价: hnsoft blend 的
 *    "先转预乘 → 相加 → 除回非预乘" 与预乘域直接 over 在无限精度下同值,
 *    差异只剩 8bit 舍入(±1/通道)。输出约定:
 *      - hn_gpu_render_scaled 返回**预乘 BGRA**(Metal readback 原样, 与
 *        rt_raster 现有 "RGBA→预乘 BGRA" 的产物同口径, blit 直用);
 *      - hn_rt_shot 需要**非预乘 RGBA**(PNG 口径): 拿到预乘 BGRA 后
 *        反预乘(BGRA→RGBA + rgb×255/a)。底色不透明(缺省渲染底)时整幅
 *        alpha=255, 预乘⇄非预乘往返无损; 半透明底(透明窗口)有 ±1 舍入。
 * 2. 边缘抗锯齿: hnsoft 用 SDF 0.5px 覆盖率 —— **整数坐标**盒在像素中心
 *    (k+0.5)处 d=∓0.5, cov 恰为 1/0, 与 GPU 四边形的硬边逐位一致。
 *    因此 RECT/CLIP 一律取整(round)到物理像素; 非整坐标盒的边缘一像素行
 *    内有 hnsoft 半覆盖 vs GPU 硬边的差别(探针采样取整盒, 真窗观感可忽略)。
 * 3. 渐变: 烘焙 256×1 LUT 纹理(参数 t → 预乘色), t 的解析式**逐式抄**
 *    hnsoft 的 grad_t_eval/grad_axis_t(CSS 盒+角度 / 显式 linear / radial
 *    双圆大根全同式)。linear 的 t 在屏幕空间仿射 → 四边形 uv 线性插值
 *    = 精确; radial 的 t 非仿射 → 盒面 24×24(QUAD 8×8)细分近似(hnsoft
 *    逐像素精确; 中心附近偏差最大, 真窗观感平滑, 探针容差按 ±40 设计)。
 *    LUT 线性采样与 hnsoft 的分段线性插值在 256 步下偏差 ≤ ~1/255。
 * 4. 文本: FreeType 字形 1:1 光栅 —— 字号取整、(码点,字号) 缓存键、
 *    bitmap_left/top、advance>>6、空格不落笔也不进笔(hnsoft paint_text
 *    的原样语义, 包括 letter_spacing 不参与绘制)、坐标 (int) 截断 ——
 *    全部抄 hnsoft.c 同款参数; 字形覆盖率四通道同值入纹理(预乘白),
 *    纹理×顶点色(预乘) = 覆盖率正确作用在 rgb 与 a 上。纹理线性采样在
 *    整数四边形上像素中心精确命中文素中心 = 与最近采样逐位一致。
 * 5. 图片: hn_png_decode 同源解码; 1:1 尺寸绘制时 GPU 像素中心精确命中
 *    源图文素(与 hnsoft 的最近采样一致); 缩放绘制 GPU 线性 vs hnsoft
 *    最近, 邻文素级差异(探针取 1:1, 真窗观感线性更平滑)。
 * 6. 不支持→回退的细目(hn_gpu_available, 契约"呼出方整帧回退"):
 *    POLYGON / MESH / CLEAR_RECT(位集未声明)、RECT 带阴影或圆角>0.5px
 *    (四边形原语没有 SDF 圆角与模糊)、TEXT 带阴影、圆角 CLIP(>0.5px,
 *    scissor 是轴对齐)、HN_NO_TEXT 构建的 TEXT、图片解码失败(hnsoft 会
 *    画占位虚线框 —— 回退保语义)。
 */
/* strdup 在 POSIX 而非 ISO C —— 与 hn_rt.c 同款 feature-test(交叉构建
   (musl)的 -std=c99 下不定义它就拿不到声明)。 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#include "hn_internal.h"      /* 原型(hn_gpu_set_host/available/render_scaled) */
#include "hn_platform.h"      /* hnp_gpu_host 原语表(契约) */
#include "hn_png.h"           /* 图片解码(与 hnsoft 同源) */

#ifndef HN_NO_TEXT
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

/* ---------------- 宿主槽(与 hn_media_host 同款模式) ---------------- */

static const hnp_gpu_host *g_gpu;   /* 壳注入(hn_rt_set_gpu_host → 本文件) */
static int g_gpu_inited;            /* init() 已试过一次(结果缓存) */
static int g_gpu_ok;                /* init 成功 */
static unsigned g_frame_clock;      /* LRU 触达计数(纹理缓存逐出序) */

static void caches_reset(void);     /* 全部纹理缓存清空(定义在纹理段) */

void hn_gpu_set_host(const void *host) {
    const hnp_gpu_host *h = (const hnp_gpu_host *)host;
    if (h == g_gpu) return;
    /* 换宿主: 旧设备的全部句柄作废 → 引擎缓存一并清空(纹理句柄不再有效) */
    if (g_gpu && g_gpu_ok && g_gpu->shutdown) g_gpu->shutdown();
    g_gpu = h;
    g_gpu_inited = 0;
    g_gpu_ok = 0;
    caches_reset();
}

static int host_ready(void) {
    if (!g_gpu) return 0;
    if (!g_gpu_inited) {
        g_gpu_inited = 1;
        g_gpu_ok = g_gpu->init ? g_gpu->init() : 0;
        /* 契约"init 失败 supported 视为 0": g_gpu 是 const 表(不可写),
           以 host_ready()==0 表达同一语义 —— 呼出方只认本函数的布尔。 */
    }
    return g_gpu_ok;
}

/* ---------------- 颜色/渐变(逐式抄 hnsoft.c, 保证 t 与色一致) ---------------- */

typedef struct { float r, g, b, a; } fcolor;

static fcolor unpack(hn_color c) {
    fcolor r;
    r.r = (float)((c >> 24) & 0xFF) / 255.0f;
    r.g = (float)((c >> 16) & 0xFF) / 255.0f;
    r.b = (float)((c >> 8) & 0xFF) / 255.0f;
    r.a = (float)(c & 0xFF) / 255.0f;
    return r;
}

static float grad_axis_t(const hn_cmd *c, float px, float py, float scale) {
    float rad = c->grad_angle * 3.14159265f / 180.0f;
    float dx = sinf(rad), dy = -cosf(rad);
    float cx = (c->x + c->w * 0.5f) * scale, cy = (c->y + c->h * 0.5f) * scale;
    float half = (fabsf(c->w * dx) + fabsf(c->h * dy)) * scale;
    float t = ((px - cx) * dx + (py - cy) * dy) / (half > 0 ? half : 1) + 0.5f;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    return t;
}

static float grad_t_eval(const hn_cmd *c, float px, float py, float scale) {
    if (c->grad_kind == 1) {
        float ax0 = c->gax0 * scale, ay0 = c->gay0 * scale;
        float dx = (c->gax1 - c->gax0) * scale, dy = (c->gay1 - c->gay0) * scale;
        float len2 = dx * dx + dy * dy;
        float t = len2 > 1e-6f
                ? ((px - ax0) * dx + (py - ay0) * dy) / len2 : 0.0f;
        if (t < 0) t = 0; else if (t > 1) t = 1;
        return t;
    }
    if (c->grad_kind == 2) {
        float cx0 = c->gcx0 * scale, cy0 = c->gcy0 * scale;
        float dx = (c->gcx1 - c->gcx0) * scale, dy = (c->gcy1 - c->gcy0) * scale;
        float dr = (c->gr1 - c->gr0) * scale;
        float r0 = c->gr0 * scale;
        float bx = px - cx0, by = py - cy0;
        float a = dx * dx + dy * dy - dr * dr;
        float b = bx * dx + by * dy + r0 * dr;
        float cq = bx * bx + by * by - r0 * r0;
        float t;
        if (fabsf(a) < 1e-6f)
            t = fabsf(b) < 1e-6f ? 1.0f : cq / (2.0f * b);
        else {
            float disc = b * b - a * cq;
            if (disc < 0) t = 1.0f;
            else t = (b + (a > 0 ? sqrtf(disc) : -sqrtf(disc))) / a;
        }
        if (t < 0) t = 0; else if (t > 1) t = 1;
        return t;
    }
    return grad_axis_t(c, px, py, scale);
}

/* 多 stop 分段线性插值(与 hnsoft grad_color_at 逐式一致) */
static fcolor grad_color_at(const hn_cmd *c, float t) {
    fcolor a, b;
    if (c->gn_stops >= 2) {
        const hn_grad_stop *s = c->gstops;
        if (t <= s[0].t) return unpack(s[0].c);
        for (int i = 1; i < c->gn_stops; i++) {
            if (t <= s[i].t) {
                float span = s[i].t - s[i - 1].t;
                float f = span > 1e-6f ? (t - s[i - 1].t) / span : 0.0f;
                a = unpack(s[i - 1].c);
                b = unpack(s[i].c);
                fcolor r;
                r.r = a.r + (b.r - a.r) * f;
                r.g = a.g + (b.g - a.g) * f;
                r.b = a.b + (b.b - a.b) * f;
                r.a = a.a + (b.a - a.a) * f;
                return r;
            }
        }
        return unpack(s[c->gn_stops - 1].c);
    }
    a = unpack(c->grad_from);
    b = unpack(c->grad_to);
    fcolor r;
    r.r = a.r + (b.r - a.r) * t;
    r.g = a.g + (b.g - a.g) * t;
    r.b = a.b + (b.b - a.b) * t;
    r.a = a.a + (b.a - a.a) * t;
    return r;
}

/* QUAD 角点渐变参数(hnsoft quad_grad_corner_t 同式: CSS 盒+角度模型,
   采样点 = 投影前元素盒角点 × scale) */
static float quad_grad_corner_t(const hn_cmd *c, int k, float scale) {
    static const float fx[4] = { 0, 1, 1, 0 }, fy[4] = { 0, 0, 1, 1 };
    return grad_axis_t(c, (c->x + fx[k] * c->w) * scale,
                       (c->y + fy[k] * c->h) * scale, scale);
}

/* ---------------- UTF-8(与 hnsoft 同一解码器口径) ---------------- */

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

/* ---------------- 纹理缓存(图片路径 / 媒体帧指针 / 渐变 LUT / 字形) ---------------- */

/* 图片缓存: 按路径(hnsoft 每帧重解; GPU 侧缓存纹理 —— 上传一次反复采样,
   这正是 GPU 路径的性能来源之一)。负结果也缓存(与引擎 GIF 缓存同一纪律,
   避免 availableness 在两次询问间翻转)。上限 64 张, LRU 逐出。 */
typedef struct { char *path; hnp_gpu_tex tex; int ok; unsigned used; } img_ent;
static img_ent g_imgs[64];
static int g_img_n;

/* 媒体帧缓存: 按位图指针(引擎会话双缓冲的两块缓冲交替)。内容每帧可能
   变化 → 命中即整幅 tex_update(replaceRegion 上传 ≈ hnsoft 每帧 blend 的
   同量级带宽)。上限 16, LRU 逐出。 */
typedef struct { const unsigned char *ptr; int w, h, stride; hnp_gpu_tex tex; unsigned used; } bmp_ent;
static bmp_ent g_bmps[16];
static int g_bmp_n;

/* 渐变 LUT 缓存: 键 = 渐变参数快照(含 stops 深拷贝 —— cmd 的 stops 在
   显示列表生命周期后失效)。上限 64, LRU 逐出。 */
typedef struct {
    hn_color from, to;
    unsigned char kind, has_stops;
    float angle;
    float p[10];                 /* linear: gax0..gay1; radial: gcx0..gr1 */
    int n_stops;
    hn_grad_stop stops[16];
    hnp_gpu_tex tex;
    unsigned used;
} lut_ent;
static lut_ent g_luts[64];
static int g_lut_n;

/* 字形缓存: (码点, 物理字号) —— hnsoft 同键(hnsoft 忽略粗斜体, 这里
   逐位对齐它的观感)。上限 4096; 满后新字形触发整帧回退(与 hnsoft
   缓存满丢弃同级的诚实降级)。 */
typedef struct {
    int cp, size_px;
    hnp_gpu_tex tex;      /* 覆盖率纹理(四通道同值; 无位图的字形 = 0) */
    int w, h, left, topv, adv;
} glyph_ent;
static glyph_ent g_glyphs[4096];
static int g_glyph_n;

/* 全部缓存清空(换宿主: 句柄已随 shutdown 作废) */
void caches_reset(void) {
    for (int i = 0; i < g_img_n; i++) {
        if (g_imgs[i].tex && g_gpu) g_gpu->tex_free(g_imgs[i].tex);
        free(g_imgs[i].path);
    }
    g_img_n = 0;
    for (int i = 0; i < g_bmp_n; i++)
        if (g_bmps[i].tex && g_gpu) g_gpu->tex_free(g_bmps[i].tex);
    g_bmp_n = 0;
    for (int i = 0; i < g_lut_n; i++)
        if (g_luts[i].tex && g_gpu) g_gpu->tex_free(g_luts[i].tex);
    g_lut_n = 0;
    g_glyph_n = 0;   /* 字形纹理句柄一并作废 */
}

/* ---------------- 图片解码 + 缓存(hnsoft load_image_pixels 同源) ---------------- */

static unsigned char *load_png_pixels(const char *path, int *w, int *h) {
    *w = *h = 0;
    if (!path || !*path) return NULL;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return NULL; }
    int iw = 0, ih = 0;
    unsigned char *px = hn_png_decode(buf, (size_t)n, &iw, &ih);
    free(buf);
    if (!px) return NULL;
    *w = iw;
    *h = ih;
    return px;
}

static img_ent *img_lookup(const char *path) {
    for (int i = 0; i < g_img_n; i++)
        if (!strcmp(g_imgs[i].path, path)) { g_imgs[i].used = ++g_frame_clock; return &g_imgs[i]; }
    int ok = 0, iw = 0, ih = 0;
    unsigned char *px = load_png_pixels(path, &iw, &ih);
    hnp_gpu_tex tex = 0;
    if (px) {
        tex = g_gpu->tex_create(iw, ih, px);
        free(px);
        ok = tex != 0;
    }
    /* 表满: LRU 逐出一张(句柄 free 后槽位复用) */
    if (g_img_n >= (int)(sizeof(g_imgs) / sizeof(g_imgs[0]))) {
        int old = 0;
        for (int i = 1; i < g_img_n; i++)
            if (g_imgs[i].used < g_imgs[old].used) old = i;
        if (g_imgs[old].tex) g_gpu->tex_free(g_imgs[old].tex);
        free(g_imgs[old].path);
        g_imgs[old] = g_imgs[g_img_n - 1];
        g_img_n--;
    }
    img_ent *e = &g_imgs[g_img_n++];
    e->path = strdup(path ? path : "");
    e->tex = tex;
    e->ok = ok;
    e->used = ++g_frame_clock;
    return e;
}

/* ---------------- 媒体帧纹理(按指针缓存, 命中即整幅更新) ---------------- */

static hnp_gpu_tex bmp_tex(const hn_cmd *c) {
    const unsigned char *ptr = c->bitmap;
    for (int i = 0; i < g_bmp_n; i++) {
        if (g_bmps[i].ptr == ptr && g_bmps[i].w == c->bitmap_w &&
            g_bmps[i].h == c->bitmap_h && g_bmps[i].stride == c->bitmap_stride) {
            /* 内容可能已变(媒体推新帧) → 整幅重传(stride 折成紧排) */
            int w = c->bitmap_w, h = c->bitmap_h;
            unsigned char *packed = (unsigned char *)malloc((size_t)w * h * 4);
            if (packed) {
                for (int y = 0; y < h; y++)
                    memcpy(packed + (size_t)y * w * 4,
                           ptr + (size_t)y * c->bitmap_stride, (size_t)w * 4);
                g_gpu->tex_update(g_bmps[i].tex, packed);
                free(packed);
            }
            g_bmps[i].used = ++g_frame_clock;
            return g_bmps[i].tex;
        }
    }
    int w = c->bitmap_w, h = c->bitmap_h;
    if (!ptr || w <= 0 || h <= 0 || c->bitmap_stride <= 0) return 0;
    unsigned char *packed = (unsigned char *)malloc((size_t)w * h * 4);
    if (!packed) return 0;
    for (int y = 0; y < h; y++)
        memcpy(packed + (size_t)y * w * 4,
               ptr + (size_t)y * c->bitmap_stride, (size_t)w * 4);
    hnp_gpu_tex tex = g_gpu->tex_create(w, h, packed);
    free(packed);
    if (!tex) return 0;
    if (g_bmp_n >= (int)(sizeof(g_bmps) / sizeof(g_bmps[0]))) {
        int old = 0;
        for (int i = 1; i < g_bmp_n; i++)
            if (g_bmps[i].used < g_bmps[old].used) old = i;
        g_gpu->tex_free(g_bmps[old].tex);
        g_bmps[old] = g_bmps[g_bmp_n - 1];
        g_bmp_n--;
    }
    g_bmps[g_bmp_n].ptr = ptr;
    g_bmps[g_bmp_n].w = w;
    g_bmps[g_bmp_n].h = h;
    g_bmps[g_bmp_n].stride = c->bitmap_stride;
    g_bmps[g_bmp_n].tex = tex;
    g_bmps[g_bmp_n].used = ++g_frame_clock;
    g_bmp_n++;
    return tex;
}

/* ---------------- 渐变 LUT(烘焙 + 缓存) ---------------- */

static int lut_key_eq(const lut_ent *e, const hn_cmd *c) {
    if (e->from != c->grad_from || e->to != c->grad_to) return 0;
    if (e->kind != c->grad_kind || e->angle != c->grad_angle) return 0;
    float p[10] = { c->gax0, c->gay0, c->gax1, c->gay1,
                    c->gcx0, c->gcy0, c->gr0, c->gcx1, c->gcy1, c->gr1 };
    if (memcmp(e->p, p, sizeof(p)) != 0) return 0;
    int hs = c->gn_stops >= 2;
    if (e->has_stops != (unsigned char)hs) return 0;
    if (hs) {
        if (e->n_stops != c->gn_stops) return 0;
        if (memcmp(e->stops, c->gstops,
                   sizeof(hn_grad_stop) * (size_t)c->gn_stops) != 0) return 0;
    }
    return 1;
}

static hnp_gpu_tex grad_lut(const hn_cmd *c) {
    for (int i = 0; i < g_lut_n; i++)
        if (lut_key_eq(&g_luts[i], c)) { g_luts[i].used = ++g_frame_clock; return g_luts[i].tex; }

    unsigned char rgba[256 * 4];
    for (int i = 0; i < 256; i++) {
        fcolor col = grad_color_at(c, ((float)i + 0.5f) / 256.0f);
        /* 非预乘 → 预乘(LUT 在预乘域被采样, 与纹理混合口径一致) */
        rgba[i * 4 + 0] = (unsigned char)(col.r * col.a * 255.0f + 0.5f);
        rgba[i * 4 + 1] = (unsigned char)(col.g * col.a * 255.0f + 0.5f);
        rgba[i * 4 + 2] = (unsigned char)(col.b * col.a * 255.0f + 0.5f);
        rgba[i * 4 + 3] = (unsigned char)(col.a * 255.0f + 0.5f);
    }
    hnp_gpu_tex tex = g_gpu->tex_create(256, 1, rgba);
    if (!tex) return 0;

    if (g_lut_n >= (int)(sizeof(g_luts) / sizeof(g_luts[0]))) {
        int old = 0;
        for (int i = 1; i < g_lut_n; i++)
            if (g_luts[i].used < g_luts[old].used) old = i;
        g_gpu->tex_free(g_luts[old].tex);
        g_luts[old] = g_luts[g_lut_n - 1];
        g_lut_n--;
    }
    lut_ent *e = &g_luts[g_lut_n++];
    e->from = c->grad_from;
    e->to = c->grad_to;
    e->kind = c->grad_kind;
    e->has_stops = (unsigned char)(c->gn_stops >= 2);
    e->angle = c->grad_angle;
    float p[10] = { c->gax0, c->gay0, c->gax1, c->gay1,
                    c->gcx0, c->gcy0, c->gr0, c->gcx1, c->gcy1, c->gr1 };
    memcpy(e->p, p, sizeof(p));
    e->n_stops = c->gn_stops >= 2 ? c->gn_stops : 0;
    if (e->n_stops > 0) memcpy(e->stops, c->gstops, sizeof(hn_grad_stop) * (size_t)e->n_stops);
    e->tex = tex;
    e->used = ++g_frame_clock;
    return tex;
}

/* ---------------- 字形 → 纹理(FreeType; 参数逐项抄 hnsoft.c) ---------------- */

#ifndef HN_NO_TEXT
static FT_Library ft_lib;
static int ft_inited = 0;
static FT_Face ft_face;
static int ft_face_ok = 0;

/* 系统字体路径探测(与 hnsoft FONT_PATHS 同一份 —— 同一字体文件才出同一
   字形位图; 首个命中的 face 0 也一致) */
static const char *FONT_PATHS[] = {
    "/System/Library/Fonts/PingFang.ttc",
    "/System/Library/Fonts/Hiragino Sans GB.ttc",
    "/System/Library/Fonts/STHeiti Medium.ttc",
    "/System/Library/Fonts/Supplemental/Songti.ttc",
    "/usr/share/fonts/opentype/noto/NotoSansCJK-Regular.ttc",
    "/usr/share/fonts/truetype/wqy/wqy-microhei.ttc",
    "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    NULL,
};

static void gpu_font_init(void) {
    if (ft_inited) return;
    ft_inited = 1;
    if (FT_Init_FreeType(&ft_lib)) return;
    for (int i = 0; FONT_PATHS[i]; i++) {
        if (FT_New_Face(ft_lib, FONT_PATHS[i], 0, &ft_face) == 0) { ft_face_ok = 1; return; }
    }
}

static const glyph_ent *glyph_get(int cp, int size_px) {
    for (int i = 0; i < g_glyph_n; i++)
        if (g_glyphs[i].cp == cp && g_glyphs[i].size_px == size_px) return &g_glyphs[i];
    gpu_font_init();
    if (!ft_face_ok) return NULL;
    if (g_glyph_n >= (int)(sizeof(g_glyphs) / sizeof(g_glyphs[0]))) return NULL;  /* 满 → 整帧回退 */
    FT_Set_Pixel_Sizes(ft_face, 0, size_px);
    if (FT_Load_Char(ft_face, (FT_ULong)cp, FT_LOAD_RENDER)) return NULL;
    FT_GlyphSlot g = ft_face->glyph;
    glyph_ent *e = &g_glyphs[g_glyph_n++];
    e->cp = cp;
    e->size_px = size_px;
    e->w = (int)g->bitmap.width;
    e->h = (int)g->bitmap.rows;
    e->left = g->bitmap_left;
    e->topv = g->bitmap_top;
    e->adv = (int)(g->advance.x >> 6);
    e->tex = 0;
    if (e->w > 0 && e->h > 0) {
        /* 非预乘白 × 覆盖率 alpha(宿主上传时统一预乘 → 纹理即预乘白,
           (cov,cov,cov,cov) —— 覆盖率同时作用在 rgb 与 a)。若这里直接
           存 cov 到 rgb, 宿主再乘一次就是双重预乘(实测边缘像素深一倍)。 */
        unsigned char *rgba = (unsigned char *)malloc((size_t)e->w * e->h * 4);
        if (!rgba) return NULL;
        for (int i = 0; i < e->w * e->h; i++) {
            unsigned char cov = g->bitmap.buffer[i];
            rgba[i * 4 + 0] = 255;
            rgba[i * 4 + 1] = 255;
            rgba[i * 4 + 2] = 255;
            rgba[i * 4 + 3] = cov;
        }
        e->tex = g_gpu->tex_create(e->w, e->h, rgba);
        free(rgba);
        if (!e->tex) return NULL;   /* 纹理资源紧张 → 整帧回退 */
    }
    return e;
}
#endif /* !HN_NO_TEXT */

/* ---------------- 四边形批(同色同纹理的连续 quad 合成一次 draw_quads) ---------------- */

static float *g_qv;             /* 顶点数组(4 顶点 × 4 float/quad) */
static int g_qv_cap, g_qv_n;    /* 容量按 quad 计 */
static hnp_gpu_tex g_q_tex;
static unsigned g_q_rgba;
static int g_q_any;

static void quads_flush(void) {
    if (!g_q_any) return;
    g_gpu->draw_quads(g_qv, g_qv_n, g_q_tex, g_q_rgba);
    g_q_any = 0;
    g_qv_n = 0;
}

/* 一个四边形(顶点序按契约: x0y0,x1y0,x2y1,x3y1; uv 随顶点) */
static void quad_emit(hnp_gpu_tex tex, unsigned rgba,
                      float x0, float y0, float x1, float y1,
                      float x2, float y2, float x3, float y3,
                      float u0, float v0, float u1, float v1,
                      float u2, float v2, float u3, float v3) {
    if (tex != g_q_tex || rgba != g_q_rgba) quads_flush();
    g_q_tex = tex;
    g_q_rgba = rgba;
    if (g_qv_n + 1 > g_qv_cap) {
        int nc = g_qv_cap ? g_qv_cap * 2 : 1024;
        float *nv = (float *)realloc(g_qv, (size_t)nc * 16 * sizeof(float));
        if (!nv) return;   /* 分配失败: 丢弃该 quad(上界外的防御) */
        g_qv = nv;
        g_qv_cap = nc;
    }
    float *v = g_qv + (size_t)g_qv_n * 16;
    v[0] = x0; v[1] = y0; v[2] = u0; v[3] = v0;
    v[4] = x1; v[5] = y1; v[6] = u1; v[7] = v1;
    v[8] = x2; v[9] = y2; v[10] = u2; v[11] = v2;
    v[12] = x3; v[13] = y3; v[14] = u3; v[15] = v3;
    g_qv_n++;
    g_q_any = 1;
}

/* 矩形四顶点(**周界序** TL→TR→BR→BL, 见 hn_platform.h draw_quads 注 ——
   周界序下 GPU 端 (0,1,2)+(0,2,3) 拆分的两三角形绕序一致, 免剔除;
   uv 随顶点: (u0,v0)→(u0+du,v0)→(u0+du,v0+dv)→(u0,v0+dv)) */
static void quad_rect(hnp_gpu_tex tex, unsigned rgba,
                      float x, float y, float w, float h,
                      float u0, float v0, float du, float dv) {
    quad_emit(tex, rgba, x, y, x + w, y, x + w, y + h, x, y + h,
              u0, v0, u0 + du, v0, u0 + du, v0 + dv, u0, v0 + dv);
}

/* ---------------- 指令翻译 ---------------- */

/* 一条 RECT(hnsoft paint_rect 口径: 先填充/渐变后描边; 取整口径见头注 2) */
static void emit_rect(const hn_cmd *c, float scale) {
    int ix = (int)floorf(c->x * scale + 0.5f);
    int iy = (int)floorf(c->y * scale + 0.5f);
    int iw = (int)floorf(c->w * scale + 0.5f);
    int ih = (int)floorf(c->h * scale + 0.5f);
    if (iw <= 0 || ih <= 0) return;

    int grad = (c->gradient || c->grad_kind) != 0;
    if (grad) {
        /* 渐变(hnsoft: 渐变声明时无论 fill 是否可见都画) */
        hnp_gpu_tex lut = grad_lut(c);
        if (!lut) { quads_flush(); return; }   /* LUT 资源紧张: 该盒缺笔(容差内) */
        if (c->grad_kind == 2) {
            /* radial: t 非屏幕仿射 → 盒面 24×24 细分, 每顶点按 grad_t_eval
               求 t(近似精度与近似范围见头注 3) */
            const int N = 24;
            for (int gy = 0; gy < N; gy++) {
                for (int gx = 0; gx < N; gx++) {
                    float px0 = (float)ix + (float)iw * gx / N;
                    float px1 = (float)ix + (float)iw * (gx + 1) / N;
                    float py0 = (float)iy + (float)ih * gy / N;
                    float py1 = (float)iy + (float)ih * (gy + 1) / N;
                    float t00 = grad_t_eval(c, px0, py0, scale);
                    float t10 = grad_t_eval(c, px1, py0, scale);
                    float t01 = grad_t_eval(c, px0, py1, scale);
                    float t11 = grad_t_eval(c, px1, py1, scale);
                    /* 周界序: TL→TR→BR→BL(uv 随顶点) */
                    quad_emit(lut, 0xFFFFFFFFu, px0, py0, px1, py0, px1, py1, px0, py1,
                              t00, 0.5f, t10, 0.5f, t11, 0.5f, t01, 0.5f);
                }
            }
        } else {
            /* linear(CSS 盒+角度或显式轴): t 屏幕仿射 → 角点精确 uv 一次成盒
               (周界序 TL→TR→BR→BL, 见 quad_rect 注) */
            float xs[4] = { (float)ix, (float)(ix + iw), (float)(ix + iw), (float)ix };
            float ys[4] = { (float)iy, (float)iy, (float)(iy + ih), (float)(iy + ih) };
            float t4[4];
            t4[0] = grad_t_eval(c, (float)ix, (float)iy, scale);
            t4[1] = grad_t_eval(c, (float)(ix + iw), (float)iy, scale);
            t4[2] = grad_t_eval(c, (float)(ix + iw), (float)(iy + ih), scale);
            t4[3] = grad_t_eval(c, (float)ix, (float)(iy + ih), scale);
            quad_emit(lut, 0xFFFFFFFFu, xs[0], ys[0], xs[1], ys[1], xs[2], ys[2], xs[3], ys[3],
                      t4[0], 0.5f, t4[1], 0.5f, t4[2], 0.5f, t4[3], 0.5f);
        }
    } else if ((c->fill & 0xFFu) > 0) {
        quad_rect(0, c->fill, (float)ix, (float)iy, (float)iw, (float)ih, 0, 0, 0, 0);
    }

    /* 描边(hnsoft 描边模型的**逐像素精确**复刻): hnsoft 的 stroke 是
       sd_in = |d| - hw ∈ (-0.5, 0.5) 的环(外侧半平台 0.5, 内侧线性下降),
       像素中心 |d| 恒为半整数 → 每条边至多一条中心线 t = ⌊hw⌋+0.5 被画:
         hw ∈ (k, k+1) → 因子 f = 0.5 (hw ≤ t) / 0.5-(hw-t) (hw > t);
         hw 落整数(偶数宽)→ 两邻中心恰在 ±0.5 边界 → **一个像素都不画**
       (hnsoft 实测口径: border:2px/4px 不可见, 这就是参照语义)。
       角部: r=0 的 SDF 角弧在整数中心上只剩 ≤5% alpha 残点(≤13/通道,
       容差内 —— 不复刻)。四段环: 上/下两行通宽(角元归行), 左/右两列
       去角元 —— 与 hnsoft"每像素画一次"一致, 不双重混合。 */
    fcolor sc = unpack(c->stroke);
    if (c->stroke_w > 0 && sc.a > 0.01f && (c->stroke & 0xFFu) != 0) {
        float hw = c->stroke_w * scale * 0.5f;
        float t = floorf(hw) + 0.5f;             /* 唯一候选中心线(半整数) */
        float sd_in = t - hw;
        if (sd_in > -0.5f && sd_in < 0.5f && 2.0f * t < (float)iw && 2.0f * t < (float)ih) {
            float f = (sd_in >= 0) ? 0.5f : 0.5f + sd_in;
            unsigned rgba_ring = (c->stroke & 0xFFFFFF00u)
                               | (unsigned)((float)(c->stroke & 0xFFu) * f);
            float x0 = (float)ix, y0 = (float)iy, x1 = (float)(ix + iw), y1 = (float)(iy + ih);
            float cx0 = x0 + t, cx1 = x1 - t, cy0 = y0 + t, cy1 = y1 - t;
            /* 上/下行(通宽: 含两端角元 —— 半开区间 [cx0, cx1+0.5) 恰含中心
               cx0 与 cx1, 不含界外) */
            quad_rect(0, rgba_ring, cx0, cy0 - 0.5f, (cx1 + 0.5f) - cx0, 1.0f, 0, 0, 0, 0);
            quad_rect(0, rgba_ring, cx0, cy1 - 0.5f, (cx1 + 0.5f) - cx0, 1.0f, 0, 0, 0, 0);
            /* 左/右列(去角元: 角元已归行 —— y ∈ [cy0+0.5, cy1-0.5) 只含
               中心 cy0+1 .. cy1-1) */
            quad_rect(0, rgba_ring, cx0 - 0.5f, cy0 + 0.5f, 1.0f,
                      (cy1 - 0.5f) - (cy0 + 0.5f), 0, 0, 0, 0);
            quad_rect(0, rgba_ring, cx1 - 0.5f, cy0 + 0.5f, 1.0f,
                      (cy1 - 0.5f) - (cy0 + 0.5f), 0, 0, 0, 0);
        }
    }
}

/* 一条 QUAD(hnsoft paint_quad 口径: 任意四顶点; 渐变 = CSS 盒+角度模型的
   角点参数 → LUT uv 双线性插值(hnsoft 是逐三角形重心插值, 对角线附近有
   二阶小差); 门禁与 hnsoft 同式) */
static void emit_quad_cmd(const hn_cmd *c, float scale) {
    fcolor col = unpack(c->fill);
    int grad = (c->gradient || c->grad_kind) != 0;
    if (grad) {
        fcolor gfrom = unpack(c->grad_from), gto = unpack(c->grad_to);
        if (col.a <= 0.01f && gfrom.a <= 0.01f && gto.a <= 0.01f) return;
        hnp_gpu_tex lut = grad_lut(c);
        if (!lut) return;
        float t4[4];
        for (int k = 0; k < 4; k++) t4[k] = quad_grad_corner_t(c, k, scale);
        quad_emit(lut, 0xFFFFFFFFu,
                  c->qx[0] * scale, c->qy[0] * scale,
                  c->qx[1] * scale, c->qy[1] * scale,
                  c->qx[2] * scale, c->qy[2] * scale,
                  c->qx[3] * scale, c->qy[3] * scale,
                  t4[0], 0.5f, t4[1], 0.5f, t4[2], 0.5f, t4[3], 0.5f);
    } else {
        if (col.a <= 0.01f) return;
        quad_emit(0, c->fill,
                  c->qx[0] * scale, c->qy[0] * scale,
                  c->qx[1] * scale, c->qy[1] * scale,
                  c->qx[2] * scale, c->qy[2] * scale,
                  c->qx[3] * scale, c->qy[3] * scale,
                  0, 0, 0, 0, 0, 0, 0, 0);
    }
}

/* 一条 TEXT(hnsoft paint_text 逐项对齐: 空格不落笔不进笔、letter_spacing
   不参与绘制、坐标 (int) 截断、(码点,物理字号) 缓存键) */
static void emit_text(const hn_cmd *c, float scale) {
#ifndef HN_NO_TEXT
    int size_px = (int)(c->font.size_px * scale + 0.5f);
    if (size_px < 4) return;
    fcolor col = unpack(c->fill);
    if (col.a <= 0.01f) return;

    const unsigned char *s = (const unsigned char *)c->text;
    size_t len = c->text_len;
    float pen_x = c->tx * scale;
    float baseline = c->baseline * scale;
    size_t i = 0;
    while (i < len) {
        int cp;
        int n = utf8_next(s, len, i, &cp);
        if (!n) break;
        i += (size_t)n;
        if (cp == ' ') { continue; }   /* hnsoft 同款: 空格不落笔, 笔不进 */
        const glyph_ent *gl = glyph_get(cp, size_px);
        if (!gl) continue;             /* hnsoft 同款: 缺字形跳过该字, 笔不进 */
        if (gl->tex && gl->w > 0 && gl->h > 0) {
            int gx = (int)pen_x + gl->left;
            int gy = (int)baseline - gl->topv;
            quad_rect(gl->tex, c->fill, (float)gx, (float)gy,
                      (float)gl->w, (float)gl->h, 0, 0, 1, 1);
        }
        pen_x += (float)gl->adv;
    }
#else
    (void)c; (void)scale;
#endif
}

/* ---------------- 可用性(hn_gpu_available, 契约"呼出方整帧回退") ---------------- */

int hn_gpu_available(const hn_display_list *dl) {
    if (!dl || !host_ready()) return 0;
    for (int i = 0; i < dl->count; i++) {
        const hn_cmd *c = &dl->cmds[i];
        if ((unsigned)c->kind > 10) return 0;
        if (!(g_gpu->caps.supported_cmds & (1u << c->kind))) return 0;
        switch (c->kind) {
        case HN_CMD_RECT:
            if (c->shadow) return 0;                    /* box-shadow: 四边形原语无模糊 */
            if (c->radius > 0.5f) return 0;             /* 圆角: 无 SDF(v1 回退, 头注 6) */
            if ((c->gradient || c->grad_kind) && c->gn_stops > 16)
                return 0;                               /* LUT 缓存键容量界(诚实降级) */
            break;
        case HN_CMD_QUAD:
            if ((c->gradient || c->grad_kind) && c->gn_stops > 16) return 0;
            break;
        case HN_CMD_TEXT:
#ifdef HN_NO_TEXT
            return 0;                                   /* 构建期无 FreeType */
#else
            if (c->shadow) return 0;                    /* 文本阴影同上 */
#endif
            break;
        case HN_CMD_IMAGE:
            if (!img_lookup(c->text ? c->text : "")->ok) return 0;   /* hnsoft 占位框保真 */
            break;
        case HN_CMD_CLIP_PUSH:
            if (c->radius > 0.5f) return 0;             /* scissor 轴对齐, 圆角裁不精确 */
            break;
        case HN_CMD_POLYGON:
        case HN_CMD_MESH:
        case HN_CMD_CLEAR_RECT:
            return 0;                                   /* 位集未声明(防御性重复) */
        default:
            break;                                      /* QUAD/BITMAP/CLIP_POP 直通 */
        }
    }
    return 1;
}

/* ---------------- 主翻译循环 ---------------- */

int hn_gpu_render_scaled(const hn_display_list *dl, int w, int h, hn_color bg,
                         float scale, unsigned char **out_bgra) {
    if (!out_bgra || !dl || w <= 0 || h <= 0) return 0;
    float rs = (scale >= 0.5f) ? scale : 1.0f;
    if (!host_ready()) return 0;
    if (!g_gpu->begin(w, h, bg)) return 0;

    g_q_any = 0;
    g_qv_n = 0;
    g_q_tex = 0;
    g_q_rgba = 0;

    int ok = 1;
    for (int i = 0; i < dl->count && ok; i++) {
        const hn_cmd *c = &dl->cmds[i];
        switch (c->kind) {
        case HN_CMD_RECT:
            emit_rect(c, rs);
            break;
        case HN_CMD_QUAD:
            emit_quad_cmd(c, rs);
            break;
        case HN_CMD_TEXT:
            emit_text(c, rs);
            break;
        case HN_CMD_IMAGE: {
            img_ent *e = img_lookup(c->text ? c->text : "");
            int ix = (int)floorf(c->x * rs + 0.5f);
            int iy = (int)floorf(c->y * rs + 0.5f);
            int iw = (int)floorf(c->w * rs + 0.5f);
            int ih = (int)floorf(c->h * rs + 0.5f);
            if (e->ok && e->tex && iw > 0 && ih > 0)
                quad_rect(e->tex, 0xFFFFFFFFu, (float)ix, (float)iy,
                          (float)iw, (float)ih, 0, 0, 1, 1);
            break;
        }
        case HN_CMD_BITMAP: {
            hnp_gpu_tex t = bmp_tex(c);
            int ix = (int)floorf(c->x * rs + 0.5f);
            int iy = (int)floorf(c->y * rs + 0.5f);
            int iw = (int)floorf(c->w * rs + 0.5f);
            int ih = (int)floorf(c->h * rs + 0.5f);
            if (t && iw > 0 && ih > 0)
                quad_rect(t, 0xFFFFFFFFu, (float)ix, (float)iy,
                          (float)iw, (float)ih, 0, 0, 1, 1);
            break;
        }
        case HN_CMD_CLIP_PUSH:
            quads_flush();   /* 裁剪状态切换前冲批 —— 批内 quad 必须吃当条的 scissor */
            g_gpu->clip_push((int)floorf(c->x * rs + 0.5f), (int)floorf(c->y * rs + 0.5f),
                             (int)floorf(c->w * rs + 0.5f), (int)floorf(c->h * rs + 0.5f));
            break;
        case HN_CMD_CLIP_POP:
            quads_flush();
            g_gpu->clip_pop();
            break;
        case HN_CMD_POLYGON:
        case HN_CMD_MESH:
        case HN_CMD_CLEAR_RECT:
            ok = 0;   /* 可用性检查已挡; 防御性兜底(直呼 render 的误用) */
            break;
        }
    }
    quads_flush();

    unsigned char *buf = (unsigned char *)malloc((size_t)w * h * 4);
    if (!ok || !buf || !g_gpu->end_readback(buf, w, h)) {
        /* 帧被放弃: end_readback 未调时宿主在下次 begin 兜底回收(契约允许
           呼出方逐帧放弃) —— 调用方静默回退 hnsoft */
        free(buf);
        return 0;
    }
    *out_bgra = buf;
    return 1;
}

/* hnsoft.c — 软件光栅化后端
 *
 * 设计: 显示列表 → RGBA 像素, 纯 C 逐指令实现。
 * - 圆角矩形: 有向距离场(SDF) + 0.5px 覆盖率抗锯齿
 * - 线性渐变: 沿渐变轴投影
 * - 阴影: 多层扩边圆角矩形近似(不用高斯模糊, 成本太高)
 * - 裁剪: 轴对齐矩形交叠栈; radius>0 时按圆角矩形 SDF 精确判内
 *   (clip-path: circle 借圆角方盒表达, 角部不能漏)
 * - 文本: FreeType 光栅化 + (字形,字号) 缓存
 * - PNG: 存储型 deflate(合法 PNG, 无压缩) + CRC32
 */
/* 图片解码缓存要 stat() 取 (mtime,size) 做失效键 —— -std=c99 严格模式下
   POSIX 声明默认隐藏, 必须在一切 include 之前显式打开(只影响本 TU)。 */
#if !defined(_WIN32) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "hnsoft.h"
#include "hn_png.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/stat.h>

#ifndef HN_NO_TEXT
#include <ft2build.h>
#include FT_FREETYPE_H
#endif

/* ---------------- 颜色工具 ---------------- */

typedef struct { float r, g, b, a; } fcolor;

/* 图像像素解码(定义在文件后部): 返回 malloc 的 RGBA8, 调用方 free */
static unsigned char *load_image_pixels(const char *path, int *w, int *h);
/* 带 (path, mtime, size) 解码缓存的版本: 返回缓存持有的**借用指针**
   (调用方不得 free), 失败返回 NULL。渲染每帧的 IMAGE/MESH 指令都走它 ——
   此前每帧重跑 fopen + inflate + unfilter, profile 显示占静态图片页
   引擎算力的 ~40%(hn_png inf_data/unfilter 是 Top 叶子)。 */
static const unsigned char *image_pixels_cached(const char *path, int *w, int *h);

static fcolor unpack(hn_color c) {
    fcolor r;
    r.r = (float)((c >> 24) & 0xFF) / 255.0f;
    r.g = (float)((c >> 16) & 0xFF) / 255.0f;
    r.b = (float)((c >> 8) & 0xFF) / 255.0f;
    r.a = (float)(c & 0xFF) / 255.0f;
    return r;
}

/* source-over 合成(**非预乘** RGBA 空间 —— 与 hn_color 语义一致, 也是调用方
   拿到缓冲后直接期待的布局)。
   之前这个函数只写 RGB 不写 alpha, 于是光栅器里 alpha 恒为 255 ——
   半透明色画到透明像素上, 颜色被折半(变得很深)而 alpha 仍是 255, 出来是一块
   深色而不是"淡色叠加"。透明背板之所以必须修这里, 就是因为整条 alpha 链
   在最后一环断了。 */
static void blend(unsigned char *px, fcolor c) {
    float sa = c.a;
    if (sa <= 0.0f) return;
    float da = px[3] / 255.0f;
    if (sa >= 0.999f && da >= 0.999f) {          /* 快路: 双不透明 */
        px[0] = (unsigned char)(c.r * 255.0f + 0.5f);
        px[1] = (unsigned char)(c.g * 255.0f + 0.5f);
        px[2] = (unsigned char)(c.b * 255.0f + 0.5f);
        px[3] = 255;
        return;
    }
    float oa = sa + da * (1.0f - sa);            /* 出 alpha */
    if (oa <= 0.0005f) { px[0]=px[1]=px[2]=px[3]=0; return; }
    /* 非预乘: 先把两侧转预乘再相加, 最后除回去 */
    float sr = c.r * sa, sg = c.g * sa, sb = c.b * sa;
    float dr = (px[0] / 255.0f) * da, dg = (px[1] / 255.0f) * da, db = (px[2] / 255.0f) * da;
    float orr = (sr + dr * (1.0f - sa)) / oa;
    float og = (sg + dg * (1.0f - sa)) / oa;
    float ob = (sb + db * (1.0f - sa)) / oa;
    px[0] = (unsigned char)(orr * 255.0f + 0.5f);
    px[1] = (unsigned char)(og * 255.0f + 0.5f);
    px[2] = (unsigned char)(ob * 255.0f + 0.5f);
    px[3] = (unsigned char)(oa * 255.0f + 0.5f);
}

/* ---------------- 帧缓冲 ---------------- */

typedef struct {
    unsigned char *px;    /* RGBA8, 自上而下 */
    int w, h;
    hn_color bg;          /* 渲染底色(HN_CMD_CLEAR_RECT 就地回填它 ——
                             "局部清空 = 露出画布底"而非全图重置) */
    /* 裁剪栈(轴对齐交叠 + 圆角半径; radius>0 时按 SDF 精确判内 ——
       clip-path: circle 借 CLIP_PUSH 的圆角方盒表达, 帧缓冲必须认半径,
       否则方盒四角漏出, 圆形头像/圆形按钮裁不动)。 */
    struct { float x0, y0, x1, y1, r; } clip[32];
    int clip_n;
} fb;

/* 圆角矩形 SDF(定义在下方"圆角矩形 SDF"一节; 裁剪判定先用) */
static float sd_rounded(float px, float py, float x, float y, float w, float h, float r);

static void fb_init(fb *f, int w, int h, hn_color bg) {
    f->w = w; f->h = h; f->clip_n = 0;
    f->bg = bg;
    f->px = (unsigned char *)malloc((size_t)w * h * 4);
    fcolor b = unpack(bg);
    unsigned char r = (unsigned char)(b.r * 255 + 0.5f);
    unsigned char g = (unsigned char)(b.g * 255 + 0.5f);
    unsigned char bl = (unsigned char)(b.b * 255 + 0.5f);
    /* alpha 取**底色自己的** alpha: 之前这里硬编码 255, 于是调用方传全透明
       底色(hnwin.c 就是 hnsoft_render(dl,w,h,0x00000000))时整张画布变成
       不透明黑 —— 透明背板直接失效, 而且症状是"声明了透明却是一块黑",
       完全不会怀疑到光栅器。 */
    unsigned char ba = (unsigned char)(b.a * 255 + 0.5f);
    for (int i = 0; i < w * h; i++) {
        f->px[i * 4] = r; f->px[i * 4 + 1] = g; f->px[i * 4 + 2] = bl; f->px[i * 4 + 3] = ba;
    }
}

static void fb_push_clip(fb *f, float x0, float y0, float x1, float y1, float r) {
    if (f->clip_n >= 32) return;
    float bx0 = 0, by0 = 0, bx1 = (float)f->w, by1 = (float)f->h;
    if (f->clip_n > 0) {
        bx0 = f->clip[f->clip_n - 1].x0; by0 = f->clip[f->clip_n - 1].y0;
        bx1 = f->clip[f->clip_n - 1].x1; by1 = f->clip[f->clip_n - 1].y1;
    }
    f->clip[f->clip_n].x0 = x0 > bx0 ? x0 : bx0;
    f->clip[f->clip_n].y0 = y0 > by0 ? y0 : by0;
    f->clip[f->clip_n].x1 = x1 < bx1 ? x1 : bx1;
    f->clip[f->clip_n].y1 = y1 < by1 ? y1 : by1;
    f->clip[f->clip_n].r = r > 0 ? r : 0;
    f->clip_n++;
}

/* 此前是 `if (clip_n > 1)` —— 第一层裁剪永远弹不掉: 文档里第一个 overflow
   容器(或 clip-path)POP 之后, 后续所有绘制仍被裁在它的盒内, 表现为
   "第一个可滚动区块之后的兄弟内容整块消失"。栈平衡由引擎保证, 归零合法。 */
static void fb_pop_clip(fb *f) { if (f->clip_n > 0) f->clip_n--; }

static int fb_clip_ok(fb *f, int x, int y) {
    if (x < 0 || y < 0 || x >= f->w || y >= f->h) return 0;
    if (f->clip_n == 0) return 1;
    float cx0 = f->clip[f->clip_n - 1].x0, cy0 = f->clip[f->clip_n - 1].y0;
    float cx1 = f->clip[f->clip_n - 1].x1, cy1 = f->clip[f->clip_n - 1].y1;
    float cr  = f->clip[f->clip_n - 1].r;
    float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
    if (fx < cx0 || fy < cy0 || fx >= cx1 || fy >= cy1) return 0;
    if (cr > 0) {
        float d = sd_rounded(fx, fy, cx0, cy0, cx1 - cx0, cy1 - cy0, cr);
        if (d > 0) return 0;
    }
    return 1;
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
    /* 最后一项必须用 fmaxf(qx, qy)。原先是
           (qx > qy ? qx : (qy > qx ? qy : 0))
       它在 qx == qy 时落到 0 —— 而正方形矩形的中心正好 qx == qy, 于是
       "内部距离"整项丢失, d 塌成 0, 覆盖率 cov = 0.5。
       后果: 正方形正中心那个像素的不透明度只有一半(半透明色变成四分之一,
       不透明色变成 50% alpha)。45° 对角线上每一处都受影响。
       用 fmaxf 才是圆角盒 SDF 的标准写法。 */
    return sqrtf(ax * ax + ay * ay) + fmaxf(qx, qy) - r;
}

/* ---------------- 渐变 ---------------- */

/* CSS 渐变参数: 点在渐变轴上的位置(0 = from 端, 1 = to 端)。
   CSS 角度: 0deg=向上, 顺时针; 轴沿该方向穿过盒中心。
   轴长口径 = CSS 渐变线长 |W·sinθ|+|H·cosθ|, 端点在中心 ± 轴长/2 ——
   盒沿轴向的极端点恰好落在渐变 0/1(此前多除一个 2, 两端只到 25%/75%)。
   与 cairo 后端 make_gradient 的轴端点定义逐式一致(两后端同口径)。
   RECT 采样屏幕点, QUAD 采样**投影前的元素盒**角点 —— 同一条公式。
   scale = 绘制缩放(HiDPI): 盒中心/轴长随位图坐标同乘, 采到的点(px,py)
   也已在物理像素域 —— 分子分母同倍率, 参数 t 与 1x 完全一致(scale=1.0
   时乘 1.0 逐位不变, 既有探针的像素锚不动)。 */
static float grad_axis_t(const hn_cmd *c, float px, float py, float scale) {
    float rad = c->grad_angle * 3.14159265f / 180.0f;
    float dx = sinf(rad), dy = -cosf(rad);
    float cx = (c->x + c->w * 0.5f) * scale, cy = (c->y + c->h * 0.5f) * scale;
    float half = (fabsf(c->w * dx) + fabsf(c->h * dy)) * scale;
    float t = ((px - cx) * dx + (py - cy) * dy) / (half > 0 ? half : 1) + 0.5f;
    if (t < 0) t = 0; else if (t > 1) t = 1;
    return t;
}

/* 显式渐变(canvas 2D 口径)的参数 t: grad_kind=1 线性 = 点在绝对轴
   (gax0,gay0)→(gax1,gay1) 上的投影; grad_kind=2 radial = CSS
   createRadialGradient 双圆口径 —— 解 |P-(C0+t·ΔC)| = r0+t·Δr 取大根
   (|ΔC|≈|Δr| 时退化为线性, disc<0 = 锥外区按末 stop)。t 钳到 [0,1]。
   与 hn_paint(ledger_grad_t)/hn_cairo(pattern 语义)同口径。
   scale = 绘制缩放: 轴/圆端点先乘 scale 进物理像素域, 与采样点同域 ——
   分子分母同倍率, t 不变(scale=1.0 逐位不变)。 */
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
            else {
                /* 大根: a>0 取 (b+√)/a, a<0 取 (b-√)/a(除负翻转) */
                t = (b + (a > 0 ? sqrtf(disc) : -sqrtf(disc))) / a;
            }
        }
        if (t < 0) t = 0; else if (t > 1) t = 1;
        return t;
    }
    return grad_axis_t(c, px, py, scale);      /* CSS 盒+角度模型(旧指令) */
}

/* 多 stop 分段线性插值(n>=2 用 stops 数组; 否则 from→to 两端 ——
   与 hn_paint 的 ledger_grad_color 同一口径)。 */
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

/* 单点取渐变色: 参数 t → 颜色(RECT/POLYGON 逐像素路径共用) */
static fcolor grad_at(const hn_cmd *c, float px, float py, float scale) {
    return grad_color_at(c, grad_t_eval(c, px, py, scale));
}

/* 面片渐变: 渐变画在**投影前的元素平面**上(cmd 的 x/y/w/h 装的正是投影前
   的元素盒, 见 hn_paint), 随面片一起被投影 —— 等效 CSS 的"渐变先画在
   元素上, 再整体 3D 变换"。角点 k 的渐变参数用 RECT 同一条 grad_axis_t
   在局部盒坐标求出(角度/轴长口径与 RECT 逐式一致)。
   scale: 采样点(元素盒角点)与盒中心/轴长同乘 —— 归一化参数 t 是
   scale 不变量(HiDPI 下 3D 面片渐变与 1x 逐位同值)。 */
static float quad_grad_corner_t(const hn_cmd *c, int k, float scale) {
    static const float fx[4] = { 0, 1, 1, 0 }, fy[4] = { 0, 0, 1, 1 };
    return grad_axis_t(c, (c->x + fx[k] * c->w) * scale, (c->y + fy[k] * c->h) * scale, scale);
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
    int grad = (c->gradient || c->grad_kind) != 0;
    int do_stroke = (c->stroke_w > 0 && sc.a > 0.01f);

    /* 行级剪裁外提(与 blend_rgba 同一构造): 接受域用与 fb_clip_ok **同一个**
       浮点谓词单调走查, 圆角/病态剪裁退回逐像素原路 → 逐位等价。
       profile: fb_clip_ok 是交互页 Top 叶子之一 —— 全页矩形每像素都重跑
       4 次浮点比较 + 裁剪栈读。 */
    int fast = 1, xlo = 0, xhi = f->w - 1;
    float cy0 = 0, cy1 = 0;
    if (f->clip_n > 0) {
        const float cx0 = f->clip[f->clip_n - 1].x0, cy0c = f->clip[f->clip_n - 1].y0;
        const float cx1 = f->clip[f->clip_n - 1].x1, cy1c = f->clip[f->clip_n - 1].y1;
        const float cr = f->clip[f->clip_n - 1].r;
        if (!(cx0 == cx0) || !(cy0c == cy0c) || !(cx1 == cx1) || !(cy1c == cy1c) || cr > 0) {
            fast = 0;
        } else {
            cy0 = cy0c;
            cy1 = cy1c;
            while (xlo <= xhi && !((float)xlo + 0.5f >= cx0)) xlo++;
            while (xhi >= xlo && !((float)xhi + 0.5f < cx1)) xhi--;
        }
    }
    /* 实心快路(r==0 且描边不画): 逐式复算 sd_rounded 的 qx/qy(同一表达式
       → 同一浮点值), 两方向都 ≤ -0.5 时 ax=ay=0、d = fmaxf(qx,qy) ≤ -0.5
       → cov = 0.5-d ≥ 1 → 钳位后恒为 1.0f, col.a *= 1.0f 逐位不变 ——
       免每像素 sqrtf。渐变仍逐像素取色。 */
    const int solid = (r == 0.0f && !do_stroke);

    for (int py = iy0; py <= iy1; py++) {
        if (py < 0 || py >= f->h) continue;   /* fb_clip_ok 的帧界, 快路必须保住 */
        if (fast && f->clip_n > 0) {
            const float fy = (float)py + 0.5f;
            if (!(fy >= cy0) || !(fy < cy1)) continue;
        }
        int px_a = ix0, px_b = ix1;
        if (fast) {
            if (px_a < xlo) px_a = xlo;
            if (px_b > xhi) px_b = xhi;
        }
        for (int px = px_a; px <= px_b; px++) {
            float fx = px + 0.5f, fy = py + 0.5f;
            fcolor col;
            float cov;
            float d = 0.0f;
            int solid_pix = 0;
            if (solid) {
                /* 与 sd_rounded 同式: 保证分支后果与通用路径逐位一致 */
                float qx = fabsf(fx - (x + w * 0.5f)) - (w * 0.5f - r);
                float qy = fabsf(fy - (y + h * 0.5f)) - (h * 0.5f - r);
                if (qx <= -0.5f && qy <= -0.5f) solid_pix = 1;
            }
            if (solid_pix) {
                cov = 1.0f;
                if (grad) col = grad_at(c, fx, fy, scale); else col = base;
            } else {
                d = sd_rounded(fx, fy, x, y, w, h, r);
                cov = 0.5f - d;
                if (cov <= 0) continue;
                if (cov > 1) cov = 1;
                if (grad) col = grad_at(c, fx, fy, scale); else col = base;
            }
            col.a *= cov;
            fb_blend(f, px, py, col);
            /* 描边: 内外 1px 环(solid 时 do_stroke 恒假, d 即通用路径的 d) */
            if (do_stroke) {
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

/* ---------------- 四边形(3D 投影面片) ---------------- */

/* 扫描线填充: 对每行求与四边形各边的交点, 取最小/最大 x 之间填充。
   边缘按 0.5px 覆盖率抗锯齿(与圆角矩形一致的策略)。
   渐变: 四边形拆两条三角形(对角线 0-2), 每条按 MESH 仿射纹理映射同款
   的重心权重插值渐变参数 t(颜色 = from→to 按 t 插值)。一般透视四边形
   不是仿射像, 两三角形在对角线处连续但有折角 —— 与 MESH 网格同一既定
   简化口径, 换来的是渐变轴随面片一起被投影(见 quad_grad_corner_t)。 */
static void paint_quad(fb *f, const hn_cmd *c, float alpha, float scale) {
    fcolor col = unpack(c->fill);
    col.a *= alpha;
    fcolor gfrom, gto;
    int grad = (c->gradient || c->grad_kind) != 0;
    if (grad) {
        /* 渐变元素的 background 解析为 0(渐变即底色) → fill 常是全透明,
           门禁看两端色而不是 fill(否则渐变面片被当成透明整块跳过) */
        gfrom = unpack(c->grad_from);
        gto = unpack(c->grad_to);
        gfrom.a *= alpha; gto.a *= alpha;
        if (col.a <= 0.01f && gfrom.a <= 0.01f && gto.a <= 0.01f) return;
    } else if (col.a <= 0.01f) {
        return;
    }
    /* 角点渐变参数 + 两条三角形的重心分母(MESH 同款公式), 退化三角形跳过。
       scale(HiDPI): 投影后的角点在 paint 时乘 scale 进物理像素域 ——
       3D 投影在引擎侧按逻辑 px 做, 均匀缩放与投影可交换, 语义是整幅放大。 */
    static const int TRI[2][3] = { { 0, 1, 2 }, { 0, 2, 3 } };
    float QX[4], QY[4];
    for (int i = 0; i < 4; i++) { QX[i] = c->qx[i] * scale; QY[i] = c->qy[i] * scale; }
    float t4[4] = { 0, 0, 0, 0 };
    float den[2] = { 0, 0 };
    if (grad) {
        for (int k = 0; k < 4; k++) t4[k] = quad_grad_corner_t(c, k, scale);
        for (int t = 0; t < 2; t++) {
            int a = TRI[t][0], b = TRI[t][1], d = TRI[t][2];
            den[t] = (QY[b] - QY[d]) * (QX[a] - QX[d])
                   + (QX[d] - QX[b]) * (QY[a] - QY[d]);
        }
    }
    /* 包围盒 */
    float minx = QX[0], maxx = QX[0], miny = QY[0], maxy = QY[0];
    for (int i = 1; i < 4; i++) {
        if (QX[i] < minx) minx = QX[i];
        if (QX[i] > maxx) maxx = QX[i];
        if (QY[i] < miny) miny = QY[i];
        if (QY[i] > maxy) maxy = QY[i];
    }
    int y0 = (int)floorf(miny) - 1, y1 = (int)ceilf(maxy) + 1;
    for (int py = y0; py <= y1; py++) {
        float fy = py + 0.5f;
        float xs[8];
        int nx = 0;
        for (int e = 0; e < 4; e++) {
            float ax = QX[e], ay = QY[e];
            float bx = QX[(e + 1) & 3], by = QY[(e + 1) & 3];
            if ((ay <= fy && by > fy) || (by <= fy && ay > fy)) {
                float t = (fy - ay) / (by - ay);
                if (nx < 8) xs[nx++] = ax + t * (bx - ax);
            }
        }
        if (nx < 2) continue;
        /* 排序取最小/最大(四边形凸, 最多 2 个交点, 这里通用处理) */
        for (int a2 = 1; a2 < nx; a2++) {
            float k = xs[a2]; int b2 = a2 - 1;
            while (b2 >= 0 && xs[b2] > k) { xs[b2 + 1] = xs[b2]; b2--; }
            xs[b2 + 1] = k;
        }
        float lx = xs[0], rx = xs[nx - 1];
        int x0 = (int)floorf(lx), x1 = (int)ceilf(rx);
        for (int px = x0; px <= x1; px++) {
            float fx = px + 0.5f;
            float cov = 0;
            if (fx >= lx + 0.5f && fx <= rx - 0.5f) cov = 1.0f;
            else if (fx > lx - 0.5f && fx < rx + 0.5f) cov = 0.5f;   /* 边缘半覆盖 */
            if (cov <= 0) continue;
            fcolor cc;
            if (grad) {
                /* 含该像素的三角形内按重心权重插值 t(权重公式与 paint_mesh
                   逐式一致); 对角线两侧插值连续, 取先命中的一条 */
                float t = t4[0];
                for (int ti = 0; ti < 2; ti++) {
                    if (fabsf(den[ti]) < 1e-6f) continue;
                    int a = TRI[ti][0], b = TRI[ti][1], d = TRI[ti][2];
                    float x0c = QX[a], y0c = QY[a];
                    float x1c = QX[b], y1c = QY[b];
                    float x2c = QX[d], y2c = QY[d];
                    float w0 = ((y1c - y2c) * (fx - x2c) + (x2c - x1c) * (fy - y2c)) / den[ti];
                    float w1 = ((y2c - y0c) * (fx - x2c) + (x0c - x2c) * (fy - y2c)) / den[ti];
                    float w2 = 1.0f - w0 - w1;
                    if (w0 < -0.001f || w1 < -0.001f || w2 < -0.001f) continue;
                    t = w0 * t4[a] + w1 * t4[b] + w2 * t4[d];
                    break;
                }
                if (t < 0) t = 0; else if (t > 1) t = 1;
                /* 颜色统一走 stops 插值器(多 stop QUAD 免费获得; 无 stops
                   时即 from→to 两端, 与旧口径逐位一致); 组透明度在此补上 */
                cc = grad_color_at(c, t);
                cc.a *= alpha;
            } else {
                cc = col;
            }
            cc.a *= cov;
            fb_blend(f, px, py, cc);
        }
    }
}

/* ---------------- 多边形填充(扫描线 + 覆盖抗锯齿) ---------------- */

typedef struct { float x; int wind; } xhit;

static void paint_polygon(fb *f, const hn_cmd *c, float scale, float ox, float oy, float alpha) {
    if (!c->poly || c->poly_n < 3) return;
    int n = c->poly_n;
    fcolor fillc = unpack(c->fill);
    fillc.a *= alpha;
    fcolor strokec = unpack(c->stroke);
    strokec.a *= alpha;
    /* 显式渐变(canvas gpath): 逐像素按 grad_t_eval/stops 插值取色 ——
       fill() 遇渐变不再回落纯色。门禁仍看首 stop 色(cmd.fill)。 */
    int grad = c->grad_kind != 0;
    int do_fill = fillc.a > 0.004f;
    int do_stroke = c->stroke_w > 0.05f && strokec.a > 0.004f;
    if (!do_fill && !do_stroke) return;

    /* 顶点/交点缓冲: 常规多边形(≤256 顶点)走栈上定长, 免掉稳态帧循环里
       每帧 3 次 malloc/free(账本动画页每帧重画同一条路径); 超长路径
       (canvas 大型 path)仍走堆, 行为不变。 */
    enum { POLY_STACK_N = 256 };
    float px_stack[POLY_STACK_N], py_stack[POLY_STACK_N];
    xhit hits_stack[POLY_STACK_N + 2];
    float *px = n <= POLY_STACK_N ? px_stack : (float *)malloc(sizeof(float) * (size_t)n);
    float *py = n <= POLY_STACK_N ? py_stack : (float *)malloc(sizeof(float) * (size_t)n);
    if (!px || !py) {
        if (px != px_stack) free(px);
        if (py != py_stack) free(py);
        return;
    }
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i < n; i++) {
        float x = (c->poly[i * 2]     - ox) * scale + ox;
        float y = (c->poly[i * 2 + 1] - oy) * scale + oy;
        px[i] = x; py[i] = y;
        if (x < minx) minx = x; if (x > maxx) maxx = x;
        if (y < miny) miny = y; if (y > maxy) maxy = y;
    }
    float hw = do_stroke ? c->stroke_w * scale * 0.5f : 0;

    if (do_fill) {
        int y0 = (int)floorf(miny) - 1, y1 = (int)ceilf(maxy) + 1;
        xhit *hits = n <= POLY_STACK_N ? hits_stack
                                       : (xhit *)malloc(sizeof(xhit) * (size_t)(n + 2));
        if (!hits) {
            if (px != px_stack) free(px);
            if (py != py_stack) free(py);
            return;
        }
        for (int s = y0; s <= y1; s++) {
            float fy = (float)s + 0.5f;
            int nh = 0;
            for (int e = 0; e < n; e++) {
                int e2 = (e + 1) % n;
                float ay = py[e], by = py[e2];
                if (fabsf(by - ay) < 1e-6f) continue;
                if ((ay <= fy && by > fy) || (by <= fy && ay > fy)) {
                    float t = (fy - ay) / (by - ay);
                    hits[nh].x = px[e] + t * (px[e2] - px[e]);
                    hits[nh].wind = (by > ay) ? 1 : -1;
                    nh++;
                }
            }
            if (nh < 2) continue;
            for (int a2 = 1; a2 < nh; a2++) {
                xhit k = hits[a2]; int b2 = a2 - 1;
                while (b2 >= 0 && hits[b2].x > k.x) { hits[b2 + 1] = hits[b2]; b2--; }
                hits[b2 + 1] = k;
            }
            for (int k = 0; k < nh - 1; k++) {
                int inside;
                if (c->even_odd) {
                    inside = (k & 1);
                } else {
                    int wsum = 0;
                    for (int q = 0; q <= k; q++) wsum += hits[q].wind;
                    inside = (wsum != 0);
                }
                if (!inside) continue;
                float lx = hits[k].x, rx = hits[k + 1].x;
                if (rx <= lx) continue;
                for (int xx = (int)floorf(lx); xx <= (int)ceilf(rx); xx++) {
                    float fx = (float)xx + 0.5f;
                    float cov = 0;
                    if (fx >= lx + 0.5f && fx <= rx - 0.5f) cov = 1.0f;
                    else if (fx > lx - 0.5f && fx < rx + 0.5f) cov = 0.5f;
                    if (cov <= 0) continue;
                    fcolor cc = grad ? grad_at(c, fx, fy, scale) : fillc;
                    cc.a *= cov;
                    fb_blend(f, xx, s, cc);
                }
            }
        }
        if (hits != hits_stack) free(hits);
    }

    /* 描边: 每条边按宽度展开成四边形带(闭合路径) */
    if (do_stroke) {
        int sy0lim = (int)floorf(miny - hw) - 1, sy1lim = (int)ceilf(maxy + hw) + 1;
        for (int e = 0; e < n; e++) {
            int e2 = (e + 1) % n;
            float ax = px[e], ay = py[e], bx = px[e2], by = py[e2];
            float ex = bx - ax, ey = by - ay;
            float elen = sqrtf(ex * ex + ey * ey);
            if (elen < 1e-5f) continue;
            float nx = -ey / elen * hw, ny = ex / elen * hw;
            float qx[4] = { ax + nx, bx + nx, bx - nx, ax - nx };
            float qy[4] = { ay + ny, by + ny, by - ny, ay - ny };
            float qminy = qy[0], qmaxy = qy[0];
            for (int i = 1; i < 4; i++) {
                if (qy[i] < qminy) qminy = qy[i];
                if (qy[i] > qmaxy) qmaxy = qy[i];
            }
            int sy0 = (int)floorf(qminy) - 1, sy1 = (int)ceilf(qmaxy) + 1;
            if (sy0 < sy0lim) sy0 = sy0lim;
            if (sy1 > sy1lim) sy1 = sy1lim;
            for (int s = sy0; s <= sy1; s++) {
                float fy = (float)s + 0.5f;
                float xs[8]; int nx2 = 0;
                for (int i = 0; i < 4; i++) {
                    float p1y = qy[i], p2y = qy[(i + 1) & 3];
                    if ((p1y <= fy && p2y > fy) || (p2y <= fy && p1y > fy)) {
                        float t = (fy - p1y) / (p2y - p1y);
                        if (nx2 < 8) xs[nx2++] = qx[i] + t * (qx[(i + 1) & 3] - qx[i]);
                    }
                }
                if (nx2 < 2) continue;
                float lx = xs[0], rx = xs[0];
                for (int i = 1; i < nx2; i++) {
                    if (xs[i] < lx) lx = xs[i];
                    if (xs[i] > rx) rx = xs[i];
                }
                for (int xx = (int)floorf(lx); xx <= (int)ceilf(rx); xx++) {
                    float fx = (float)xx + 0.5f;
                    float cov = 0;
                    if (fx >= lx + 0.5f && fx <= rx - 0.5f) cov = 1.0f;
                    else if (fx > lx - 0.5f && fx < rx + 0.5f) cov = 0.5f;
                    if (cov <= 0) continue;
                    fcolor cc = strokec; cc.a *= cov;
                    fb_blend(f, xx, s, cc);
                }
            }
        }
    }
    if (px != px_stack) free(px);
    if (py != py_stack) free(py);
}

/* ---------------- 网格变形贴图(重心坐标反查 UV) ---------------- */

static void paint_mesh(fb *f, const hn_cmd *c, float alpha, float scale) {
    if (!c->mesh_verts || !c->mesh_uv || c->mesh_cols <= 0 || c->mesh_rows <= 0) return;
    int iw = 0, ih = 0;
    /* 借用指针(解码缓存持有), 不得 free —— 此前每帧 malloc/解码/free,
       30fps 下等于每秒把同一张 PNG 解 30 遍 */
    const unsigned char *ipx = image_pixels_cached(c->text ? c->text : "", &iw, &ih);
    float ga = (float)(c->fill & 0xFFu) / 255.0f * alpha;
    if (ga <= 0.004f) return;
    /* scale(HiDPI): 网格顶点是屏幕坐标, paint 时乘 scale 进物理像素域;
       UV 是归一化采样坐标, 与缩放无关 —— 源图照常采样, 只是落点更密。 */
    if (!ipx) {
        /* 无图像解码器: 画网格线示意, 让"网格变形"这层可见(而非静默空白) */
        int cols = c->mesh_cols, rows = c->mesh_rows;
        fcolor col = { 0.35f, 0.55f, 0.95f, 0.55f * ga };
        for (int r = 0; r <= rows; r++)
            for (int cc = 0; cc < cols; cc++) {
                int i0 = r * (cols + 1) + cc, i1 = i0 + 1;
                float x0 = c->mesh_verts[i0 * 2] * scale, y0 = c->mesh_verts[i0 * 2 + 1] * scale;
                float x1 = c->mesh_verts[i1 * 2] * scale, y1 = c->mesh_verts[i1 * 2 + 1] * scale;
                int ns = (int)(fabsf(x1 - x0) + fabsf(y1 - y0)) + 1;
                if (ns > 400) ns = 400;
                for (int s = 0; s <= ns; s++) {
                    float t = (float)s / (float)ns;
                    fb_blend(f, (int)(x0 + (x1 - x0) * t), (int)(y0 + (y1 - y0) * t), col);
                }
            }
        for (int cc = 0; cc <= cols; cc++)
            for (int r = 0; r < rows; r++) {
                int i0 = r * (cols + 1) + cc, i1 = i0 + cols + 1;
                float x0 = c->mesh_verts[i0 * 2] * scale, y0 = c->mesh_verts[i0 * 2 + 1] * scale;
                float x1 = c->mesh_verts[i1 * 2] * scale, y1 = c->mesh_verts[i1 * 2 + 1] * scale;
                int ns = (int)(fabsf(x1 - x0) + fabsf(y1 - y0)) + 1;
                if (ns > 400) ns = 400;
                for (int s = 0; s <= ns; s++) {
                    float t = (float)s / (float)ns;
                    fb_blend(f, (int)(x0 + (x1 - x0) * t), (int)(y0 + (y1 - y0) * t), col);
                }
            }
        return;
    }
    int cols = c->mesh_cols, rows = c->mesh_rows;
    for (int r = 0; r < rows; r++) {
        for (int cc = 0; cc < cols; cc++) {
            int i00 = r * (cols + 1) + cc;
            int i10 = i00 + 1, i01 = i00 + cols + 1, i11 = i01 + 1;
            int tri[2][3] = { { i00, i10, i01 }, { i10, i11, i01 } };
            for (int t = 0; t < 2; t++) {
                int a = tri[t][0], b = tri[t][1], d = tri[t][2];
                float x0 = c->mesh_verts[a * 2] * scale, y0 = c->mesh_verts[a * 2 + 1] * scale;
                float x1 = c->mesh_verts[b * 2] * scale, y1 = c->mesh_verts[b * 2 + 1] * scale;
                float x2 = c->mesh_verts[d * 2] * scale, y2 = c->mesh_verts[d * 2 + 1] * scale;
                float u0 = c->mesh_uv[a * 2], v0 = c->mesh_uv[a * 2 + 1];
                float u1 = c->mesh_uv[b * 2], v1 = c->mesh_uv[b * 2 + 1];
                float u2 = c->mesh_uv[d * 2], v2 = c->mesh_uv[d * 2 + 1];
                float den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
                if (fabsf(den) < 1e-6f) continue;      /* 折叠的退化三角形 */
                float minx = x0, maxx = x0, miny = y0, maxy = y0;
                if (x1 < minx) minx = x1; if (x1 > maxx) maxx = x1;
                if (x2 < minx) minx = x2; if (x2 > maxx) maxx = x2;
                if (y1 < miny) miny = y1; if (y1 > maxy) maxy = y1;
                if (y2 < miny) miny = y2; if (y2 > maxy) maxy = y2;
                for (int yy = (int)floorf(miny); yy <= (int)ceilf(maxy); yy++) {
                    float fy = (float)yy + 0.5f;
                    for (int xx = (int)floorf(minx); xx <= (int)ceilf(maxx); xx++) {
                        float fx = (float)xx + 0.5f;
                        float w0 = ((y1 - y2) * (fx - x2) + (x2 - x1) * (fy - y2)) / den;
                        float w1 = ((y2 - y0) * (fx - x2) + (x0 - x2) * (fy - y2)) / den;
                        float w2 = 1.0f - w0 - w1;
                        if (w0 < -0.02f || w1 < -0.02f || w2 < -0.02f) continue;
                        float u = w0 * u0 + w1 * u1 + w2 * u2;
                        float v = w0 * v0 + w1 * v1 + w2 * v2;
                        int sx = (int)(u * (float)iw), sy = (int)(v * (float)ih);
                        if (sx < 0) sx = 0; else if (sx >= iw) sx = iw - 1;
                        if (sy < 0) sy = 0; else if (sy >= ih) sy = ih - 1;
                        const unsigned char *p = &ipx[(sy * iw + sx) * 4];
                        fcolor col = { p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f,
                                       p[3] / 255.0f * ga };
                        fb_blend(f, xx, yy, col);
                    }
                }
            }
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

/* ---------------- 布局期文本测量(hn_text_backend 实现) ---------------- */

float hnsoft_measure(const hn_font_desc *font, const char *utf8, size_t len) {
    if (!len) return 0;
#ifndef HN_NO_TEXT
    font_init();
    if (ft_face_ok) {
        int size_px = (int)(font->size_px + 0.5f);
        if (size_px < 1) size_px = 1;
        float total = 0;
        const unsigned char *s = (const unsigned char *)utf8;
        size_t i = 0;
        int nchars = 0;
        while (i < len) {
            int cp;
            int n = utf8_next(s, len, i, &cp);
            const glyph *g = glyph_get(cp, size_px);
            /* 缺字形时按字节数回退(与引擎无后端口径一致) */
            total += g ? (float)g->advance : font->size_px * 0.55f * (float)n;
            nchars++;
            i += (size_t)n;
        }
        if (font->letter_spacing > 0 && nchars > 0)
            total += font->letter_spacing * (float)nchars;
        return total;
    }
#endif
    /* 回退: 与引擎无文本后端口径一致(字节数×字号×0.55) */
    return (float)len * font->size_px * 0.55f;
}

void hnsoft_metrics(const hn_font_desc *font, float *ascent, float *descent,
                    float *leading) {
#ifndef HN_NO_TEXT
    font_init();
    if (ft_face_ok) {
        int size_px = (int)(font->size_px + 0.5f);
        if (size_px < 1) size_px = 1;
        FT_Set_Pixel_Sizes(ft_face, 0, (FT_UInt)size_px);
        FT_Size_Metrics m = ft_face->size->metrics;
        *ascent = (float)m.ascender / 64.0f;
        *descent = -(float)m.descender / 64.0f;
        *leading = (float)(m.height - m.ascender + m.descender) / 64.0f;
        return;
    }
#endif
    *ascent = font->size_px * 0.8f;
    *descent = font->size_px * 0.2f;
    *leading = font->size_px * 0.45f;
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

    /* text-shadow: 阴影遍(偏移 + 阴影色)。此实现**不带模糊** —— hnsoft 是
       零依赖兜底, 高斯模糊的近似(降采样放大)需要一整块临时帧缓冲, 对纯软件
       路径不划算; cairo/CoreGraphics 后端有真模糊。要模糊请用那两个后端。 */
    if (c->shadow) {
        fcolor sc2 = unpack(c->shadow_color);
        sc2.a *= alpha;
        if (sc2.a > 0.01f) {
            const unsigned char *ss = (const unsigned char *)c->text;
            size_t slen = c->text_len, si = 0;
            float spen = (c->tx + ox + c->shadow_ox) * scale + ox;
            float sbase = (c->baseline + oy + c->shadow_oy) * scale + oy;
            while (si < slen) {
                int cp;
                int n2 = utf8_next(ss, slen, si, &cp);
                if (!n2) break;
                si += (size_t)n2;
                if (cp == ' ') { spen += 0; continue; }
                const glyph *g2 = glyph_get(cp, size_px);
                if (!g2 || !g2->bm) continue;
                int gx2 = (int)spen + g2->left, gy2 = (int)sbase - g2->top;
                for (int yy2 = 0; yy2 < g2->h; yy2++)
                    for (int xx2 = 0; xx2 < g2->w; xx2++) {
                        unsigned char a2 = g2->bm[yy2 * g2->w + xx2];
                        if (!a2) continue;
                        fcolor gc2 = sc2;
                        gc2.a = sc2.a * (a2 / 255.0f);
                        fb_blend(f, gx2 + xx2, gy2 + yy2, gc2);
                    }
                spen += (float)g2->advance;
            }
        }
    }

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
/* 读文件并解 PNG。返回 malloc 的 RGBA8(调用方 free), 失败返回 NULL。 */
static unsigned char *load_image_pixels(const char *path, int *w, int *h) {
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

/* ---------------- 图片解码缓存((path, mtime, size) 键, LRU 字节预算) ------
 * 动画帧循环里 IMAGE/MESH 指令每帧都来要同一张图 —— 此前每帧
 * fopen + inflate + unfilter 全量重跑(macOS sample 实测: pet.html 引擎
 * 叶子样本里 hn_png 解码三件套占 ~40%)。解码是文件的纯函数, 缓存不动像素。
 *
 * 失效键: 路径 + st_mtime(秒) + st_size —— 文件被替换后下一帧重新解码。
 * 有界: 解码字节预算 HN_IMG_CACHE_BUDGET(48MB 软上限, 超了 LRU 逐出;
 * 单条超预算的大图保留 —— 总占用仍有界 = 预算 + 最大单条) +
 * 条目上限 HN_IMG_CACHE_MAX_ENT。解码失败也入表(负缓存, 免得缺图页
 * 每帧重跑 fopen; stat 变化即重试)。 */
typedef struct {
    char *path;
    long long mtime, fsize;   /* 失效键; stat 不可用/文件缺失时为哨兵值 */
    unsigned char *px;        /* RGBA8 解码结果(缓存持有) */
    int w, h;
    size_t bytes;             /* px 字节数(预算口径; 失败条目为 0) */
    unsigned long long last_use;
} img_cache_ent;

#define HN_IMG_CACHE_BUDGET  (48u << 20)   /* 48MB 解码像素软上限 */
#define HN_IMG_CACHE_MAX_ENT 32
#define HN_IMG_STAT_MISS_MTIME (-1)        /* 文件缺失/stat 失败的哨兵 */

static img_cache_ent g_imgc[HN_IMG_CACHE_MAX_ENT];
static int g_imgc_n = 0;
static size_t g_imgc_bytes = 0;
static unsigned long long g_imgc_clock = 0;
/* 命中/未命中计数(HN_IMG_CACHE_STATS=1 时逐帧打印, 供 perf_probe 断言) */
static long g_img_hit = 0, g_img_miss = 0, g_img_evict = 0;

static void img_stat_line(void) {
    static int stats_on = -1;
    if (stats_on < 0) stats_on = getenv("HN_IMG_CACHE_STATS") ? 1 : 0;
    if (!stats_on) return;
    fprintf(stderr, "hn_img_cache: hit=%ld miss=%ld evict=%ld entries=%d bytes=%zu\n",
            g_img_hit, g_img_miss, g_img_evict, g_imgc_n, g_imgc_bytes);
}

static void img_key_of(const char *path, long long *mtime, long long *fsize) {
    *mtime = HN_IMG_STAT_MISS_MTIME;
    *fsize = -1;
#if defined(_WIN32)
    struct _stat st;
    if (path && *path && _stat(path, &st) == 0) {
        *mtime = (long long)st.st_mtime;
        *fsize = (long long)st.st_size;
    }
#else
    struct stat st;
    if (path && *path && stat(path, &st) == 0) {
        *mtime = (long long)st.st_mtime;
        *fsize = (long long)st.st_size;
    }
#endif
}

/* 超预算/超表时逐出最久未用, 腾到装得下 need 为止(全空则止 —— 软上限) */
static void img_cache_evict(size_t need) {
    while (g_imgc_n > 0 &&
           (g_imgc_bytes + need > HN_IMG_CACHE_BUDGET || g_imgc_n >= HN_IMG_CACHE_MAX_ENT)) {
        int victim = 0;
        for (int i = 1; i < g_imgc_n; i++)
            if (g_imgc[i].last_use < g_imgc[victim].last_use) victim = i;
        g_imgc_bytes -= g_imgc[victim].bytes;
        free(g_imgc[victim].px);
        free(g_imgc[victim].path);
        g_imgc[victim] = g_imgc[g_imgc_n - 1];
        g_imgc_n--;
        g_img_evict++;
    }
}

static const unsigned char *image_pixels_cached(const char *path, int *w, int *h) {
    *w = *h = 0;
    if (!path || !*path) return NULL;
    long long mtime, fsize;
    img_key_of(path, &mtime, &fsize);
    for (int i = 0; i < g_imgc_n; i++) {
        if (g_imgc[i].mtime == mtime && g_imgc[i].fsize == fsize &&
            g_imgc[i].path && !strcmp(g_imgc[i].path, path)) {
            g_imgc[i].last_use = ++g_imgc_clock;
            g_img_hit++;
            img_stat_line();
            if (!g_imgc[i].px) return NULL;   /* 负缓存: 解码失败的路径 */
            *w = g_imgc[i].w;
            *h = g_imgc[i].h;
            return g_imgc[i].px;
        }
    }
    /* 未命中: 解码(失败也入表, 但 stat 失败的缺失文件用哨兵键可被
       "文件出现"自动击穿 —— 键不同自然走到这里重新解码) */
    g_img_miss++;
    int iw = 0, ih = 0;
    unsigned char *px = load_image_pixels(path, &iw, &ih);
    size_t bytes = px ? (size_t)iw * ih * 4 : 0;
    img_cache_evict(bytes);
    g_imgc[g_imgc_n].path = (char *)malloc(strlen(path) + 1);
    if (g_imgc[g_imgc_n].path) strcpy(g_imgc[g_imgc_n].path, path);
    g_imgc[g_imgc_n].mtime = mtime;
    g_imgc[g_imgc_n].fsize = fsize;
    g_imgc[g_imgc_n].px = px;
    g_imgc[g_imgc_n].w = iw;
    g_imgc[g_imgc_n].h = ih;
    g_imgc[g_imgc_n].bytes = bytes;
    g_imgc[g_imgc_n].last_use = ++g_imgc_clock;
    g_imgc_n++;
    g_imgc_bytes += bytes;
    img_stat_line();
    if (!px) return NULL;
    *w = iw;
    *h = ih;
    return px;
}

/* RGBA8(非预乘) → 画布拉伸混合。IMAGE(解码文件)与 BITMAP(媒体当前帧)
   共用同一条像素路径 —— 两者的差别只在"像素从哪来"(stride 分开给:
   文件 = iw*4 紧排, 媒体帧 = 引擎会话的 stride)。

   扫描线快路(profile: fb_blend/fb_clip_ok 是三页共同的 Top 叶子 ——
   全幅 blit 每像素都重跑一遍浮点剪裁判定): 把 fb_clip_ok 的接受域
   **一次性**外提成整数 x 区间 + 行级 y 判定, 圆角裁剪与 NaN 坐标退回
   逐像素原路。等价性靠构造保证: 区间端点用与 fb_clip_ok **同一个**
   浮点谓词单调走查(x+0.5f >= cx0 / x+0.5f < cx1), 不引入任何新的
   舍入路径 → 接受集逐位相同。不透明快路(源 alpha=255、组 alpha≥0.999、
   目底 alpha=255 → blend 走双不透明直写)已对 0..255 全值验证
   round(v/255*255+0.5)==v 逐位成立, 直拷贝与 blend 输出一致。 */
static void blend_rgba(fb *f, const unsigned char *px, int stride, int iw, int ih,
                       int x0, int y0, int w, int h, float alpha) {
    if (w <= 0 || h <= 0) return;
    int fast = 1, xlo = 0, xhi = f->w - 1;
    float cy0 = 0, cy1 = 0;
    if (f->clip_n > 0) {
        const float cx0 = f->clip[f->clip_n - 1].x0, cy0c = f->clip[f->clip_n - 1].y0;
        const float cx1 = f->clip[f->clip_n - 1].x1, cy1c = f->clip[f->clip_n - 1].y1;
        const float cr = f->clip[f->clip_n - 1].r;
        /* NaN/非有限剪裁坐标: fb_clip_ok 的比较全为假反而"放行", 语义病态,
           不赌快路的区间推导 —— 退回逐像素原路逐位复刻 */
        if (!(cx0 == cx0) || !(cy0c == cy0c) || !(cx1 == cx1) || !(cy1c == cy1c) || cr > 0) {
            fast = 0;
        } else {
            cy0 = cy0c;
            cy1 = cy1c;
            /* 接受列 ⇔ x+0.5f >= cx0 且 x+0.5f < cx1(与 fb_clip_ok 同式);
               谓词沿 x 单调, 从两端向内走查即得精确接受区间 */
            while (xlo <= xhi && !((float)xlo + 0.5f >= cx0)) xlo++;
            while (xhi >= xlo && !((float)xhi + 0.5f < cx1)) xhi--;
        }
    }
    const int opaque = (alpha >= 0.999f);
    const int wdiv = w, hdiv = h;   /* 上面已挡 0; 除法防 0 的旧卫语句等价收敛 */
    for (int yy = 0; yy < h; yy++) {
        const int dy = y0 + yy;
        if (dy < 0 || dy >= f->h) continue;
        if (fast && f->clip_n > 0) {
            const float fy = (float)dy + 0.5f;
            if (!(fy >= cy0) || !(fy < cy1)) continue;
        }
        const int sy = yy * ih / hdiv;
        const unsigned char *srow = px + (size_t)sy * stride;
        unsigned char *drow = f->px + (size_t)dy * f->w * 4;
        if (fast) {
            int a = xlo - x0;
            int b = xhi - x0;
            if (a < 0) a = 0;
            if (b > w - 1) b = w - 1;
            for (int xx = a; xx <= b; xx++) {
                const unsigned char *p = srow + (size_t)(xx * iw / wdiv) * 4;
                unsigned char *d = drow + (size_t)(x0 + xx) * 4;
                if (opaque && p[3] == 255 && d[3] == 255) {
                    /* 双不透明直写: blend 快路的输出对 0..255 全值逐位等于源 */
                    d[0] = p[0];
                    d[1] = p[1];
                    d[2] = p[2];
                } else {
                    fcolor col = { p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f,
                                   p[3] / 255.0f * alpha };
                    blend(d, col);
                }
            }
        } else {
            /* 原路(圆角/病态剪裁): 逐像素 fb_clip_ok + blend, 逐位不变 */
            for (int xx = 0; xx < w; xx++) {
                int sx = xx * iw / wdiv;
                const unsigned char *p = srow + (size_t)sx * 4;
                fcolor col = { p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f,
                               p[3] / 255.0f * alpha };
                fb_blend(f, x0 + xx, dy, col);
            }
        }
    }
}

static void paint_image(fb *f, const hn_cmd *c, float scale, float ox, float oy, float alpha) {
    int iw = 0, ih = 0;
    /* 借用指针(解码缓存持有), 不得 free —— 同 paint_mesh */
    const unsigned char *px = image_pixels_cached(c->text ? c->text : "", &iw, &ih);
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
    blend_rgba(f, px, iw * 4, iw, ih, x0, y0, w, h, alpha);
}

/* BITMAP: 媒体当前帧(RGBA8 非预乘, 引擎持有缓存)。位图为空时忽略 ——
   正常情况下引擎只在有帧时才发 BITMAP, 这里只是防御。 */
static void paint_bitmap(fb *f, const hn_cmd *c, float scale, float ox, float oy, float alpha) {
    if (!c->bitmap || c->bitmap_w <= 0 || c->bitmap_h <= 0 || c->bitmap_stride <= 0) return;
    int x0 = (int)((c->x - ox) * scale + ox), y0 = (int)((c->y - oy) * scale + oy);
    int w = (int)(c->w * scale), h = (int)(c->h * scale);
    blend_rgba(f, c->bitmap, c->bitmap_stride, c->bitmap_w, c->bitmap_h,
               x0, y0, w, h, alpha);
}

/* 局部清空(canvas clearRect 的矩形): 矩形内**就地回填渲染底色**
 * (SOURCE 语义, 不是 OVER —— 透明画布清出真透明)。裁剪栈照常生效:
 * 清空范围被钳在画布元素盒内。此前 clearRect 退化为全图清账, 局部
 * 清空会连矩形外的内容一起抹掉 —— 这里的就地回填才是正确口径。
 * scale(HiDPI): 矩形坐标随位图同乘。 */
static void paint_clear_rect(fb *f, const hn_cmd *c, float scale) {
    int x0 = (int)floorf(c->x * scale), y0 = (int)floorf(c->y * scale);
    int x1 = (int)ceilf((c->x + c->w) * scale), y1 = (int)ceilf((c->y + c->h) * scale);
    fcolor b = unpack(f->bg);
    unsigned char br = (unsigned char)(b.r * 255.0f + 0.5f);
    unsigned char bg = (unsigned char)(b.g * 255.0f + 0.5f);
    unsigned char bb = (unsigned char)(b.b * 255.0f + 0.5f);
    unsigned char ba = (unsigned char)(b.a * 255.0f + 0.5f);
    for (int y = y0; y < y1; y++)
        for (int x = x0; x < x1; x++) {
            if (!fb_clip_ok(f, x, y)) continue;
            unsigned char *p = &f->px[(y * f->w + x) * 4];
            p[0] = br; p[1] = bg; p[2] = bb; p[3] = ba;
        }
}

/* ---------------- 主渲染 ---------------- */

unsigned char *hnsoft_render_scaled(const hn_display_list *dl, int width, int height,
                                    hn_color bg, float scale) {
    /* cmds == NULL 且 count == 0 是**合法的空显示列表**(文档没有任何可见
       指令, 如只有空白 canvas 的页面): 仍要产出底色位图 —— 否则"首帧为空"
       的文档会被运行时判成渲染失败, <script> 驱动的页面(canvas 2D 账本)
       根本活不到脚本执行。 */
    if (!dl || width <= 0 || height <= 0) return NULL;
    /* scale 缺省/非法按 1.0(hnsoft_render 的既有口径, 逐位一致) */
    float rs = (scale >= 0.5f) ? scale : 1.0f;
    fb f;
    fb_init(&f, width, height, bg);

    for (int i = 0; i < dl->count; i++) {
        const hn_cmd *c = &dl->cmds[i];
        switch (c->kind) {
        case HN_CMD_RECT:
            if (c->shadow) {
                /* 有阴影先画阴影(在填充之下) */
                paint_shadow(&f, c, rs, 0, 0, 1.0f);
            }
            paint_rect(&f, c, rs, 0, 0, 1.0f);
            break;
        case HN_CMD_TEXT:
            /* 文本: 字号 ×rs 后走 FreeType —— 真 2x 字形光栅, 不是拉伸 */
            paint_text(&f, c, rs, 0, 0, 1.0f);
            break;
        case HN_CMD_IMAGE:
            paint_image(&f, c, rs, 0, 0, 1.0f);
            break;
        case HN_CMD_BITMAP:
            paint_bitmap(&f, c, rs, 0, 0, 1.0f);
            break;
        case HN_CMD_CLEAR_RECT:
            paint_clear_rect(&f, c, rs);
            break;
        case HN_CMD_CLIP_PUSH:
            fb_push_clip(&f, c->x * rs, c->y * rs,
                         (c->x + c->w) * rs, (c->y + c->h) * rs, c->radius * rs);
            break;
        case HN_CMD_QUAD:
            paint_quad(&f, c, 1.0f, rs);
            break;
        case HN_CMD_POLYGON:
            paint_polygon(&f, c, rs, 0, 0, 1.0f);
            break;
        case HN_CMD_MESH:
            paint_mesh(&f, c, 1.0f, rs);
            break;
        case HN_CMD_CLIP_POP:
            fb_pop_clip(&f);
            break;
        }
    }
    return f.px;
}

unsigned char *hnsoft_render(const hn_display_list *dl, int width, int height, hn_color bg) {
    return hnsoft_render_scaled(dl, width, height, bg, 1.0f);
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

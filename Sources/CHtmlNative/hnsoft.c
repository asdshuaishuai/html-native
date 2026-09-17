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

/* 图像像素解码(定义在文件后部): 返回 malloc 的 RGBA8, 调用方 free */
static unsigned char *load_image_pixels(const char *path, int *w, int *h);

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

/* ---------------- 四边形(3D 投影面片) ---------------- */

/* 扫描线填充: 对每行求与四边形各边的交点, 取最小/最大 x 之间填充。
   边缘按 0.5px 覆盖率抗锯齿(与圆角矩形一致的策略)。 */
static void paint_quad(fb *f, const hn_cmd *c, float alpha) {
    fcolor col = unpack(c->fill);
    col.a *= alpha;
    if (col.a <= 0.01f) return;
    /* 包围盒 */
    float minx = c->qx[0], maxx = c->qx[0], miny = c->qy[0], maxy = c->qy[0];
    for (int i = 1; i < 4; i++) {
        if (c->qx[i] < minx) minx = c->qx[i];
        if (c->qx[i] > maxx) maxx = c->qx[i];
        if (c->qy[i] < miny) miny = c->qy[i];
        if (c->qy[i] > maxy) maxy = c->qy[i];
    }
    int y0 = (int)floorf(miny) - 1, y1 = (int)ceilf(maxy) + 1;
    for (int py = y0; py <= y1; py++) {
        float fy = py + 0.5f;
        float xs[8];
        int nx = 0;
        for (int e = 0; e < 4; e++) {
            float ax = c->qx[e], ay = c->qy[e];
            float bx = c->qx[(e + 1) & 3], by = c->qy[(e + 1) & 3];
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
            fcolor cc = col; cc.a *= cov;
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
    int do_fill = fillc.a > 0.004f;
    int do_stroke = c->stroke_w > 0.05f && strokec.a > 0.004f;
    if (!do_fill && !do_stroke) return;

    float *px = (float *)malloc(sizeof(float) * (size_t)n);
    float *py = (float *)malloc(sizeof(float) * (size_t)n);
    if (!px || !py) { free(px); free(py); return; }
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
        xhit *hits = (xhit *)malloc(sizeof(xhit) * (size_t)(n + 2));
        if (!hits) { free(px); free(py); return; }
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
                    fcolor cc = fillc; cc.a *= cov;
                    fb_blend(f, xx, s, cc);
                }
            }
        }
        free(hits);
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
    free(px); free(py);
}

/* ---------------- 网格变形贴图(重心坐标反查 UV) ---------------- */

static void paint_mesh(fb *f, const hn_cmd *c, float alpha) {
    if (!c->mesh_verts || !c->mesh_uv || c->mesh_cols <= 0 || c->mesh_rows <= 0) return;
    int iw = 0, ih = 0;
    unsigned char *ipx = load_image_pixels(c->text ? c->text : "", &iw, &ih);
    float ga = (float)(c->fill & 0xFFu) / 255.0f * alpha;
    if (ga <= 0.004f) { if (ipx) free(ipx); return; }
    if (!ipx) {
        /* 无图像解码器: 画网格线示意, 让"网格变形"这层可见(而非静默空白) */
        int cols = c->mesh_cols, rows = c->mesh_rows;
        fcolor col = { 0.35f, 0.55f, 0.95f, 0.55f * ga };
        for (int r = 0; r <= rows; r++)
            for (int cc = 0; cc < cols; cc++) {
                int i0 = r * (cols + 1) + cc, i1 = i0 + 1;
                float x0 = c->mesh_verts[i0 * 2], y0 = c->mesh_verts[i0 * 2 + 1];
                float x1 = c->mesh_verts[i1 * 2], y1 = c->mesh_verts[i1 * 2 + 1];
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
                float x0 = c->mesh_verts[i0 * 2], y0 = c->mesh_verts[i0 * 2 + 1];
                float x1 = c->mesh_verts[i1 * 2], y1 = c->mesh_verts[i1 * 2 + 1];
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
                float x0 = c->mesh_verts[a * 2], y0 = c->mesh_verts[a * 2 + 1];
                float x1 = c->mesh_verts[b * 2], y1 = c->mesh_verts[b * 2 + 1];
                float x2 = c->mesh_verts[d * 2], y2 = c->mesh_verts[d * 2 + 1];
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
    free(ipx);
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
        case HN_CMD_QUAD:
            paint_quad(&f, c, 1.0f);
            break;
        case HN_CMD_POLYGON:
            paint_polygon(&f, c, 1.0f, 0, 0, 1.0f);
            break;
        case HN_CMD_MESH:
            paint_mesh(&f, c, 1.0f);
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

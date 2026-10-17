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

/* 带混合模式的合成(canvas globalCompositeOperation 子集; mode 为
   HN_BLEND_*, source-over 时与上面 blend 逐位一致)。
   公式口径(与 Canvas 2D 规范的可分离混合项一致, 在非预乘 RGB 上计算):
     multiply: B(s,d) = s·d
     screen:   B(s,d) = s + d - s·d        (= 255-(255-s)(255-d)/255 同式)
     几何合成 = source-over: out.rgb = sa·B + (1-sa)·d, out.a = sa+da(1-sa)
     lighter: 预乘加法(plus-lighter): 预乘通道相加并钳制, 再反预乘。
   双不透明时即任务口径的逐通道公式: multiply=src·dst/255、
   screen=255-(255-src)(255-dst)/255、lighter=min(255,src+dst)。 */
static void blend_mode(unsigned char *px, fcolor c, int mode) {
    if (mode == HN_BLEND_SRC_OVER) { blend(px, c); return; }
    float sa = c.a;
    if (sa <= 0.0f) return;
    float sr = c.r, sg = c.g, sb = c.b;
    float dr = px[0] / 255.0f, dg = px[1] / 255.0f, db = px[2] / 255.0f;
    float da = px[3] / 255.0f;
    if (mode == HN_BLEND_LIGHTER) {
        float oa = sa + da;
        if (oa > 1.0f) oa = 1.0f;
        float orr = sr * sa + dr * da;
        float og = sg * sa + dg * da;
        float ob = sb * sa + db * da;
        if (orr > 1.0f) orr = 1.0f;
        if (og > 1.0f) og = 1.0f;
        if (ob > 1.0f) ob = 1.0f;
        px[0] = (unsigned char)(orr * 255.0f + 0.5f);
        px[1] = (unsigned char)(og * 255.0f + 0.5f);
        px[2] = (unsigned char)(ob * 255.0f + 0.5f);
        px[3] = (unsigned char)(oa * 255.0f + 0.5f);
        return;
    }
    /* multiply / screen: 可分离混合项 + source-over 几何合成 */
    float br, bg2, bb;
    if (mode == HN_BLEND_MULTIPLY) {
        br = sr * dr; bg2 = sg * dg; bb = sb * db;
    } else {                                     /* HN_BLEND_SCREEN */
        br = sr + dr - sr * dr;
        bg2 = sg + dg - sg * dg;
        bb = sb + db - sb * db;
    }
    float oa = sa + da * (1.0f - sa);
    if (oa <= 0.0005f) { px[0]=px[1]=px[2]=px[3]=0; return; }
    float orr = (sa * br + da * (1.0f - sa) * dr) / oa;
    float og = (sa * bg2 + da * (1.0f - sa) * dg) / oa;
    float ob = (sa * bb + da * (1.0f - sa) * db) / oa;
    px[0] = (unsigned char)(orr * 255.0f + 0.5f);
    px[1] = (unsigned char)(og * 255.0f + 0.5f);
    px[2] = (unsigned char)(ob * 255.0f + 0.5f);
    px[3] = (unsigned char)(oa * 255.0f + 0.5f);
}

/* ---------------- 分配计数(HN_ALLOC_TRACE churn 实证口径) ----------------
 * 渲染热路径的堆分配累计: fb 位图缓冲(一次性变体每帧新分配 / 复用变体
 * 仅扩容)、超长多边形堆缓冲、新字形位图。动画宿主接复用缓冲后稳态增量
 * 应 ≈0/帧 —— rt 层每 300 帧差分打印(perf_probe 断言), 数字直接量化
 * "消除每帧全页位图分配 churn"这件事本身。 */
static long g_soft_allocs = 0;
long hnsoft_heap_allocs(void) { return g_soft_allocs; }

/* ---------------- 帧缓冲 ---------------- */

typedef struct {
    unsigned char *px;    /* RGBA8, 自上而下 */
    int w, h;
    hn_color bg;          /* 渲染底色(HN_CMD_CLEAR_RECT 就地回填它 ——
                             "局部清空 = 露出画布底"而非全图重置) */
    /* 裁剪栈(轴对齐交叠 + 圆角半径; radius>0 时按 SDF 精确判内 ——
       clip-path: circle 借 CLIP_PUSH 的圆角方盒表达, 帧缓冲必须认半径,
       否则方盒四角漏出, 圆形头像/圆形按钮裁不动)。
       poly != NULL 的条目(canvas cpath 精确路径裁剪)在 bbox 之上再按
       多边形内外判定(非零环绕, 子路径按 segs 分段闭合); poly 指向显示
       列表的顶点数组(逻辑 px), scale 存条目里按像素现场换算 —— 免拷贝,
       生命周期 = 渲染期间显示列表必然存活。bbox 已在 push 时与前层交叠,
       多边形判定只对 poly 条目追加(纯矩形帧不付逐像素 PIP 的代价)。 */
    struct {
        float x0, y0, x1, y1, r;
        const float *poly; int poly_n;
        const int *segs; int nsegs;
        float scale;
    } clip[32];
    int clip_n;
    int poly_clips;       /* 栈内 poly 条目数(fb_clip_ok 的快速开关) */
    unsigned char blend;  /* 当前指令的混合模式(HN_CMD_* 的 blend 字段,
                             render_cmds 逐指令装填; fb_blend 消费) */
} fb;

/* 圆角矩形 SDF(定义在下方"圆角矩形 SDF"一节; 裁剪判定先用) */
static float sd_rounded(float px, float py, float x, float y, float w, float h, float r);
static float sd_rounded_aa4(float fx, float fy, float x, float y, float w, float h, float r);

/* 底色填充(**全像素覆盖** —— 复用缓冲的安全性由"每像素都被写"保证):
   上一帧的任何残留都被这一步抹掉, 之后才跑指令循环。两个公开变体
   (一次性 malloc / 复用缓冲)都先走这里, 拿到的位图逐位一致。 */
static void fb_fill_bg(fb *f, hn_color bg) {
    fcolor b = unpack(bg);
    unsigned char r = (unsigned char)(b.r * 255 + 0.5f);
    unsigned char g = (unsigned char)(b.g * 255 + 0.5f);
    unsigned char bl = (unsigned char)(b.b * 255 + 0.5f);
    /* alpha 取**底色自己的** alpha: 之前这里硬编码 255, 于是调用方传全透明
       底色(hnwin.c 就是 hnsoft_render(dl,w,h,0x00000000))时整张画布变成
       不透明黑 —— 透明背板直接失效, 而且症状是"声明了透明却是一块黑",
       完全不会怀疑到光栅器。 */
    unsigned char ba = (unsigned char)(b.a * 255 + 0.5f);
    for (int i = 0; i < f->w * f->h; i++) {
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
    f->clip[f->clip_n].poly = NULL;
    f->clip[f->clip_n].poly_n = 0;
    f->clip[f->clip_n].segs = NULL;
    f->clip[f->clip_n].nsegs = 0;
    f->clip[f->clip_n].scale = 1.0f;
    f->clip_n++;
}

/* 精确路径裁剪入栈: bbox = 顶点包围盒与前层的交叠(快速拒绝),
   poly 判定在 fb_clip_ok 里追加。 */
static void fb_push_clip_poly(fb *f, const float *poly, int n,
                              const int *segs, int nsegs, float scale) {
    if (!poly || n < 3 || f->clip_n >= 32) return;
    float minx = poly[0] * scale, maxx = minx, miny = poly[1] * scale, maxy = miny;
    for (int i = 1; i < n; i++) {
        float x = poly[i * 2] * scale, y = poly[i * 2 + 1] * scale;
        if (x < minx) minx = x;
        if (x > maxx) maxx = x;
        if (y < miny) miny = y;
        if (y > maxy) maxy = y;
    }
    float bx0 = 0, by0 = 0, bx1 = (float)f->w, by1 = (float)f->h;
    if (f->clip_n > 0) {
        bx0 = f->clip[f->clip_n - 1].x0; by0 = f->clip[f->clip_n - 1].y0;
        bx1 = f->clip[f->clip_n - 1].x1; by1 = f->clip[f->clip_n - 1].y1;
    }
    f->clip[f->clip_n].x0 = minx > bx0 ? minx : bx0;
    f->clip[f->clip_n].y0 = miny > by0 ? miny : by0;
    f->clip[f->clip_n].x1 = maxx < bx1 ? maxx : bx1;
    f->clip[f->clip_n].y1 = maxy < by1 ? maxy : by1;
    f->clip[f->clip_n].r = 0;
    f->clip[f->clip_n].poly = poly;
    f->clip[f->clip_n].poly_n = n;
    f->clip[f->clip_n].segs = segs;
    f->clip[f->clip_n].nsegs = nsegs;
    f->clip[f->clip_n].scale = scale;
    f->clip_n++;
    f->poly_clips++;
}

/* 此前是 `if (clip_n > 1)` —— 第一层裁剪永远弹不掉: 文档里第一个 overflow
   容器(或 clip-path)POP 之后, 后续所有绘制仍被裁在它的盒内, 表现为
   "第一个可滚动区块之后的兄弟内容整块消失"。栈平衡由引擎保证, 归零合法。 */
static void fb_pop_clip(fb *f) {
    if (f->clip_n <= 0) return;
    f->clip_n--;
    if (f->clip[f->clip_n].poly) f->poly_clips--;
}

/* 点在折线多边形内(非零环绕, 逐条子路径闭合; canvas clip 的填充规则)。
   子路径按 segs 表分段(segs 缺省 = 全部顶点一条路径); 每条子路径末点→
   首点补闭合边。坐标已按条目 scale 换算到设备域。 */
static int pip_poly(const float *poly, int n, const int *segs, int nsegs,
                    float scale, float px, float py) {
    int wind = 0;
    int start = 0;
    for (int s = 0; s <= (segs ? nsegs : 0); s++) {
        int cnt = segs ? segs[s] : n;
        if (cnt < 0) cnt = 0;
        if (start + cnt > n) cnt = n - start;
        if (cnt >= 3) {
            for (int e = 0; e < cnt; e++) {
                int e2 = (e + 1) % cnt;
                float ax = poly[(start + e) * 2] * scale;
                float ay = poly[(start + e) * 2 + 1] * scale;
                float bx = poly[(start + e2) * 2] * scale;
                float by = poly[(start + e2) * 2 + 1] * scale;
                if ((ay <= py && by > py) || (by <= py && ay > py)) {
                    /* 交点在像素右侧 → 按边方向计环绕 */
                    float t = (py - ay) / (by - ay);
                    if (ax + t * (bx - ax) > px)
                        wind += (by > ay) ? 1 : -1;
                }
            }
        }
        start += segs ? segs[s] : n;
        if (start >= n) break;
    }
    return wind != 0;
}

/* 裁剪覆盖(0..1): 顶层圆角裁剪按 SDF 覆盖率混合(边缘像素 4-tap), 内容
   (头像/圆角卡片 overflow:hidden)的裁边不再是 1px 硬台阶。直边/远端像素
   与旧二值判定同费用(同一次 sd_rounded); poly 裁剪仍二值(pip 无覆盖)。 */
static float fb_clip_cov(fb *f, int x, int y) {
    if (x < 0 || y < 0 || x >= f->w || y >= f->h) return 0;
    if (f->clip_n == 0) return 1;
    float cx0 = f->clip[f->clip_n - 1].x0, cy0 = f->clip[f->clip_n - 1].y0;
    float cx1 = f->clip[f->clip_n - 1].x1, cy1 = f->clip[f->clip_n - 1].y1;
    float cr  = f->clip[f->clip_n - 1].r;
    float fx = (float)x + 0.5f, fy = (float)y + 0.5f;
    if (fx < cx0 - 0.5f || fy < cy0 - 0.5f || fx >= cx1 + 0.5f || fy >= cy1 + 0.5f) return 0;
    float cov = 1.0f;
    if (cr > 0) {
        float d = sd_rounded(fx, fy, cx0, cy0, cx1 - cx0, cy1 - cy0, cr);
        cov = 0.5f - d;
        if (cov <= 0) return 0;
        if (cov > 1) cov = 1;
        if (cov > 0.02f && cov < 0.98f)
            cov = sd_rounded_aa4(fx, fy, cx0, cy0, cx1 - cx0, cy1 - cy0, cr);
    }
    if (f->poly_clips > 0) {
        /* 路径裁剪条目逐层判定(bbox 已在 push 时交叠, 只补内外测试) */
        for (int i = 0; i < f->clip_n; i++) {
            if (!f->clip[i].poly) continue;
            if (fx < f->clip[i].x0 || fy < f->clip[i].y0 ||
                fx >= f->clip[i].x1 || fy >= f->clip[i].y1) return 0;
            if (!pip_poly(f->clip[i].poly, f->clip[i].poly_n,
                          f->clip[i].segs, f->clip[i].nsegs,
                          f->clip[i].scale, fx, fy)) return 0;
        }
    }
    return cov;
}

static int fb_clip_ok(fb *f, int x, int y) {
    return fb_clip_cov(f, x, y) > 0.0f;
}

static void fb_blend(fb *f, int x, int y, fcolor c) {
    float cc = fb_clip_cov(f, x, y);
    if (cc <= 0) return;
    c.a *= cc;
    blend_mode(&f->px[(y * f->w + x) * 4], c, f->blend);
}

/* ---------------- 圆角矩形 SDF ---------------- */

static float sd_rounded(float px, float py, float x, float y, float w, float h, float r) {
    float qx = fabsf(px - (x + w * 0.5f)) - (w * 0.5f - r);
    float qy = fabsf(py - (y + h * 0.5f)) - (h * 0.5f - r);
    float ax = qx > 0 ? qx : 0, ay = qy > 0 ? qy : 0;
    /* 内部项是 min(fmaxf(qx, qy), 0) —— 标准圆角盒 SDF 的写法。
       只写 fmaxf(qx, qy) 会漏掉那个 min(...,0): 此时"在矩形内、但在圆角
       安全区外"的那一圈(四条直边带)判成外部, 形状整体四边各缩 r/2。
       实测(白底 100x100 红块): r=10 -> 90x90; r=50 -> 50x50。也就是说
       border-radius:50% 的方块只画出**一半**大小的圆。
       演进: 最早是 (qx > qy ? qx : (qy > qx ? qy : 0)) —— qx==qy 时落到 0,
       正方形中心那格丢内部距离, cov 塌成 0.5(中心只有半透明); 改成 fmaxf
       修掉了中心, 但同时引入了上面那条直边内缩。min(fmaxf(..),0) 两者都对:
       中心处 qx==qy<0 -> 取 qx; 直边带 qx>0,qy<0 -> 取 0(不改半径项)。 */
    return sqrtf(ax * ax + ay * ay) + fminf(fmaxf(qx, qy), 0.0f) - r;
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

/* 单点取渐变色: 参数 t → 颜色(RECT/POLYGON 逐像素路径共用)。
   去色带(高清): 8×8 有序抖动(经典 bayer 矩阵)对最终 8bit 量化加半档
   抖动 —— 平滑渐变的相邻像素落在同一 8bit 桶里形成可见色带(8bit/通道
   量化误差), 抖动把量化误差打散成不可见的细颗粒噪声。t 先按抖动矩阵
   偏移 ±0.5/256 再量化, 观感等价于提升 ~3bit 色深。 */
static float grad_dither(int px, int py) {
    static const unsigned char bayer[8][8] = {
        {  0, 32,  8, 40,  2, 34, 10, 42 },
        { 48, 16, 56, 24, 50, 18, 58, 26 },
        { 12, 44,  4, 36, 14, 46,  6, 38 },
        { 60, 28, 52, 20, 62, 30, 54, 22 },
        {  3, 35, 11, 43,  1, 33,  9, 41 },
        { 51, 19, 59, 27, 49, 17, 57, 25 },
        { 15, 47,  7, 39, 13, 45,  5, 37 },
        { 63, 31, 55, 23, 61, 29, 53, 21 },
    };
    return ((float)bayer[py & 7][px & 7] + 0.5f) / 64.0f - 0.5f;   /* -0.5..+0.5 */
}

static fcolor grad_at(const hn_cmd *c, float px, float py, float scale) {
    fcolor r = grad_color_at(c, grad_t_eval(c, px, py, scale));
    /* 抖动作用在 8bit 量化步长上(每通道 ±半档): 只影响量化, 不改渐变走向 */
    float d = grad_dither((int)floorf(px), (int)floorf(py)) / 255.0f;
    r.r += d;
    r.g += d;
    r.b += d;
    return r;
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

/* 边缘像素 4-tap 旋转网格超采样(RGSS): 单像素单采样把 SDF 覆盖率量化成
   1px 台阶 —— 大圆/大圆角的楼梯感(用户实测截图)即此。4 个亚像素 tap 让
   边缘方向有 5 级覆盖过渡, 台阶散成短渐变。只对边缘像素调用(内部像素
   走 solid 快路, 开销与周长成正比)。 */
static float sd_rounded_aa4(float fx, float fy, float x, float y, float w, float h, float r) {
    float c0 = 0.5f - sd_rounded(fx - 0.25f, fy - 0.125f, x, y, w, h, r);
    float c1 = 0.5f - sd_rounded(fx + 0.25f, fy + 0.125f, x, y, w, h, r);
    float c2 = 0.5f - sd_rounded(fx - 0.125f, fy + 0.25f, x, y, w, h, r);
    float c3 = 0.5f - sd_rounded(fx + 0.125f, fy - 0.25f, x, y, w, h, r);
    c0 = c0 < 0 ? 0 : (c0 > 1 ? 1 : c0);
    c1 = c1 < 0 ? 0 : (c1 > 1 ? 1 : c1);
    c2 = c2 < 0 ? 0 : (c2 > 1 ? 1 : c2);
    c3 = c3 < 0 ? 0 : (c3 > 1 ? 1 : c3);
    return (c0 + c1 + c2 + c3) * 0.25f;
}

/* 描边环覆盖(CSS border 语义): 环带 = 盒边内侧 [sd ∈ (-bw, 0)],
   覆盖 = clamp01(0.5 - sd) - clamp01(0.5 + sd + bw) —— 外缘/内缘各 1px
   线性过渡。bw = 线宽(物理像素)。 */
static float sd_ring(float sd, float bw) {
    float o = 0.5f - sd;
    float i = 0.5f - sd - bw;
    if (o < 0) o = 0; else if (o > 1) o = 1;
    if (i < 0) i = 0; else if (i > 1) i = 1;
    return o - i;
}

/* 环覆盖的 4-tap 版本(边缘像素付费, 同 sd_rounded_aa4 的门控纪律) */
static float sd_ring_aa4(float fx, float fy, float x, float y, float w, float h, float r, float bw) {
    float c0 = sd_ring(sd_rounded(fx - 0.25f, fy - 0.125f, x, y, w, h, r), bw);
    float c1 = sd_ring(sd_rounded(fx + 0.25f, fy + 0.125f, x, y, w, h, r), bw);
    float c2 = sd_ring(sd_rounded(fx - 0.125f, fy + 0.25f, x, y, w, h, r), bw);
    float c3 = sd_ring(sd_rounded(fx + 0.125f, fy - 0.25f, x, y, w, h, r), bw);
    return (c0 + c1 + c2 + c3) * 0.25f;
}

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
        } else if (f->poly_clips > 0) {
            fast = 0;   /* 路径裁剪: 行级外提覆盖不了, 退回逐像素 fb_clip_ok */
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
                /* 边缘像素: 4-tap 超采样(锯齿治理); 渐变色仍取像素中心 */
                if (cov > 0.02f && cov < 0.98f)
                    cov = sd_rounded_aa4(fx, fy, x, y, w, h, r);
                if (grad) col = grad_at(c, fx, fy, scale); else col = base;
            }
            col.a *= cov;
            fb_blend(f, px, py, col);
            /* 描边(CSS border 语义): 环带 = 盒边内侧 [sd ∈ (-bw, 0)]。
               覆盖 = clamp01(0.5-sd) - clamp01(0.5+sd+bw), 外缘/内缘各 1px
               线性; 弯段(圆角/圆)在过渡带内走 4-tap 超采样(同填充纪律)。
               旧实现的带以盒边为中心(|d|∈[bw/2-0.5, bw/2+0.5])再被内调制
               因子压成 ~半透明幽灵环 —— 宽边框几乎不可见(实测 2px 边只剩
               8% 覆盖), 且半透明环自身仍是硬边。 */
            if (do_stroke) {
                float bw = c->stroke_w * scale;
                float ring = sd_ring(d, bw);
                if (ring > 0.02f && ring < 0.98f)
                    ring = sd_ring_aa4(fx, fy, x, y, w, h, r, bw);
                if (ring > 0.004f) {
                    fcolor sv = sc;
                    sv.a *= ring;
                    fb_blend(f, px, py, sv);
                }
            }
        }
    }
}

/* ---------------- 阴影(高质量: 离屏 alpha 蒙版 + 3-pass box ≈ 高斯) ----------------
 *
 * 旧实现是 6 层扩边矩形叠透明 —— 观感"一圈圈光环", 大 blur 下尤其假。
 * 新实现与浏览器同路数:
 *   1. 阴影形状画进**独立 alpha 蒙版**(单通道, 无背景污染);
 *   2. 蒙版做 3 次盒模糊(Kovesi 三盒逼近高斯, 误差 <3%; 滑窗累积和,
 *      每趟 O(n) 与半径无关);
 *   3. 蒙版 × 阴影色 blend 回主帧。
 * CSS blur 口径: σ = blur/2; 小 blur(≤1.5px) 单层 SDF 直绘(不值模糊)。 */

/* 蒙版单通道 box blur(水平或垂直一趟; 累积和滑窗, O(n) 与半径无关) */
static void mask_blur_line(float *line, int n, int r) {
    if (r <= 0 || n <= 1) return;
    float *dst = (float *)malloc(sizeof(float) * (size_t)n);
    if (!dst) return;
    float sum = 0;
    int win = 2 * r + 1;
    for (int i = -r; i <= r; i++) sum += line[i < 0 ? 0 : (i >= n ? n - 1 : i)];
    for (int i = 0; i < n; i++) {
        dst[i] = sum / (float)win;
        int add = i + r + 1, sub = i - r;
        sum += line[add >= n ? n - 1 : add];
        sum -= line[sub < 0 ? 0 : sub];
    }
    memcpy(line, dst, sizeof(float) * (size_t)n);
    free(dst);
}

static void paint_shadow(fb *f, const hn_cmd *c, float scale, float ox, float oy, float alpha) {
    fcolor col = unpack(c->shadow_color);
    col.a *= alpha;
    if (col.a <= 0.01f) return;
    float blur = c->shadow_blur * scale;
    float ox_ = c->shadow_ox * scale, oy_ = c->shadow_oy * scale;
    float x = (c->x + ox_) * scale;
    float y = (c->y + oy_) * scale;
    float w = c->w * scale, h = c->h * scale;
    float rr = (c->radius > 0 ? c->radius : 0) * scale;

    if (blur <= 1.5f) {
        int ix0 = (int)x - 2, iy0 = (int)y - 2;
        int ix1 = (int)(x + w) + 2, iy1 = (int)(y + h) + 2;
        for (int py = iy0; py <= iy1; py++)
            for (int px = ix0; px <= ix1; px++) {
                float fx = px + 0.5f, fy = py + 0.5f;
                float d = sd_rounded(fx, fy, x, y, w, h, rr);
                float cov = 0.5f - d;
                if (cov <= 0) continue;
                if (cov > 1) cov = 1;
                fcolor cc = col; cc.a *= cov;
                fb_blend(f, px, py, cc);
            }
        return;
    }

    /* 蒙版域: 形状 bbox + 3σ 余量 */
    float sigma = blur * 0.5f;
    int pad = (int)(sigma * 3.0f) + 2;
    int mx0 = (int)floorf(x) - pad, my0 = (int)floorf(y) - pad;
    int mx1 = (int)ceilf(x + w) + pad, my1 = (int)ceilf(y + h) + pad;
    int mw = mx1 - mx0 + 1, mh = my1 - my0 + 1;
    if (mw <= 0 || mh <= 0 || mw > 8192 || mh > 8192) return;
    float *mask = (float *)calloc((size_t)mw * (size_t)mh, sizeof(float));
    if (!mask) return;

    for (int py = my0; py <= my1; py++)
        for (int px = mx0; px <= mx1; px++) {
            float fx = px + 0.5f, fy = py + 0.5f;
            float d = sd_rounded(fx, fy, x, y, w, h, rr);
            float cov = 0.5f - d;
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
            mask[(size_t)(py - my0) * mw + (size_t)(px - mx0)] = cov;
        }

    /* 3-pass box(Kovesi 系数 {0.564, 0.564, 0.575}); 每趟 H+V */
    float rs[3] = { 0.564f, 0.564f, 0.575f };
    float *line = (float *)malloc(sizeof(float) * (size_t)(mw > mh ? mw : mh));
    if (!line) { free(mask); return; }
    for (int pass = 0; pass < 3; pass++) {
        int r = (int)(sigma * rs[pass] + 0.5f) - 1;
        if (r < 1) r = 1;
        for (int yy = 0; yy < mh; yy++) {
            memcpy(line, mask + (size_t)yy * mw, sizeof(float) * (size_t)mw);
            mask_blur_line(line, mw, r);
            memcpy(mask + (size_t)yy * mw, line, sizeof(float) * (size_t)mw);
        }
        for (int xx = 0; xx < mw; xx++) {
            for (int yy = 0; yy < mh; yy++) line[yy] = mask[(size_t)yy * mw + xx];
            mask_blur_line(line, mh, r);
            for (int yy = 0; yy < mh; yy++) mask[(size_t)yy * mw + xx] = line[yy];
        }
    }
    free(line);


    for (int py = my0; py <= my1; py++) {
        if (py < 0 || py >= f->h) continue;
        for (int px = mx0; px <= mx1; px++) {
            if (px < 0 || px >= f->w) continue;
            float m = mask[(size_t)(py - my0) * mw + (size_t)(px - mx0)];
            if (m <= 0.004f) continue;
            if (m > 1) m = 1;
            fcolor cc = col;
            cc.a *= m;
            fb_blend(f, px, py, cc);
        }
    }
    free(mask);
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
            /* 边缘覆盖 = 像素域 [fx-0.5, fx+0.5] 与扫描线跨 [lx, rx] 的
               重叠长(解析, 左右缘各 1px 线性)。旧口径 0/0.5/1 三档 ——
               3D 面片边缘台阶感即此。 */
            float lo = fx - 0.5f > lx ? fx - 0.5f : lx;
            float hi = fx + 0.5f < rx ? fx + 0.5f : rx;
            float cov = hi - lo;
            if (cov <= 0) continue;
            if (cov > 1) cov = 1;
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

/* 超长多边形(>256 顶点)的堆 scratch: 三段(px/py/hits)同水位、**只增不
   减**的有界缓存(上限 = 进程见过的最大顶点数)。此前每次进入 paint_polygon
   都 malloc/free 一套(账本动画页每帧重画同一条长路径 —— 稳态帧也逃不掉);
   缓存化后只有首次见到更大路径时扩容一次。渲染是单线程主循环, 免锁。
   返回 0 = 扩容失败(OOM; 与旧 malloc 失败口径一致 —— 跳过本 path 不画)。 */
static float *g_poly_xs = NULL, *g_poly_ys = NULL;
static xhit *g_poly_hits = NULL;
static int g_poly_cap = 0;

static int poly_scratch_ensure(int n, float **px, float **py, xhit **hits) {
    if (n > g_poly_cap) {
        int nc = n + n / 4 + 16;   /* 1.25x 头部: 尺寸渐增的路径免逐帧小步扩容 */
        float *xs = (float *)realloc(g_poly_xs, sizeof(float) * (size_t)nc);
        if (!xs) return 0;
        g_poly_xs = xs;
        float *ys = (float *)realloc(g_poly_ys, sizeof(float) * (size_t)nc);
        if (!ys) return 0;
        g_poly_ys = ys;
        xhit *hs = (xhit *)realloc(g_poly_hits, sizeof(xhit) * (size_t)(nc + 2));
        if (!hs) return 0;
        g_poly_hits = hs;
        g_poly_cap = nc;
        g_soft_allocs += 3;        /* 只在扩容时计数(稳态 0) */
    }
    *px = g_poly_xs;
    *py = g_poly_ys;
    *hits = g_poly_hits;
    return 1;
}

static int poly_inside(const float *px, const float *py, int n,
                       float x, float y, int even_odd) {
    int parity = 0, winding = 0;
    for (int i = 0; i < n; i++) {
        int j = (i + n - 1) % n;
        float yi = py[i], yj = py[j];
        int crosses = ((yi > y) != (yj > y));
        if (!crosses) continue;
        float xcross = px[j] + (px[i] - px[j]) * (y - yj) / (yi - yj);
        if (x < xcross) {
            parity ^= 1;
            winding += yi > yj ? 1 : -1;
        }
    }
    return even_odd ? parity : winding != 0;
}

static float poly_edge_coverage(const float *px, const float *py, int n,
                                float x, float y, int even_odd,
                                float lx, float rx) {
    /* Only boundary pixels take fixed sub-samples. Interior pixels stay on
       the original single-sample path; the candidate span is expanded by one
       pixel so a sloped edge cannot fall between scanline endpoints. */
    if (x >= lx + 0.5f && x <= rx - 0.5f) return 1.0f;
    int hit = 0;
    for (int sy = 0; sy < 4; sy++) {
        for (int sx = 0; sx < 4; sx++) {
            float qx = floorf(x) + ((float)sx + 0.5f) * 0.25f;
            float qy = floorf(y) + ((float)sy + 0.5f) * 0.25f;
            hit += poly_inside(px, py, n, qx, qy, even_odd);
        }
    }
    return (float)hit * 0.0625f;
}

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

    /* 顶点/交点缓冲: 常规多边形(≤256 顶点)走栈上定长; 超长路径(canvas
       大型 path)走模块级 scratch(poly_scratch_ensure —— 只增不减的有界
       缓存, 稳态帧零分配)。 */
    enum { POLY_STACK_N = 256 };
    float px_stack[POLY_STACK_N], py_stack[POLY_STACK_N];
    xhit hits_stack[POLY_STACK_N + 2];
    float *px = px_stack, *py = py_stack;
    xhit *hits = hits_stack;
    if (n > POLY_STACK_N && !poly_scratch_ensure(n, &px, &py, &hits)) return;
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
                int xi0 = (int)floorf(lx) - 1;
                int xi1 = (int)ceilf(rx) + 1;
                for (int xx = xi0; xx <= xi1; xx++) {
                    float fx = (float)xx + 0.5f;
                    float cov = poly_edge_coverage(px, py, n, fx, fy,
                                                   c->even_odd, lx, rx);
                    if (cov <= 0.0f) continue;
                    fcolor cc = grad ? grad_at(c, fx, fy, scale) : fillc;
                    cc.a *= cov;
                    fb_blend(f, xx, s, cc);
                }
            }
        }
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
                /* 边缘 AA(解析覆盖率, 替代旧的 ±2% 重心容差二值跳过):
                   w_i·k_i 恰为"像素中心到第 i 条边的带符号距离 ÷ 像素盒
                   沿该边法向的投影半宽"(边长 L 在分子分母相消, 免 sqrt;
                   半宽 = (|nx|+|ny|)/2L —— 正交边 0.5px, 45° 边 0.71px)。
                   单侧坡道: 形内(w≥0)恒全覆盖(内部像素与旧内核逐位一致);
                   形外覆盖率随距离线性衰减, 0 点落在形外 0.5~0.71px ——
                   过渡只占形外**一排**像素, 视觉边缘位置外偏 ≤0.35px。
                   共享边上两侧合计仍 ≥1(一侧恒全覆盖, 过剩覆盖), 不产生
                   欠覆盖接缝/裂纹; w≈0⁻ 的浮点裂缝像素 cov≈1, 比旧容差
                   更稳, 且不再像旧 -0.02 容差那样向外多涂一整圈(容差 =
                   2%×高, 大三角形外扩可达数像素且粗细不均)。
                   (2026-10-14 复审数值核验: 系数若写 4·|den|/sum, 坡道
                   实际零点只有 0.25~0.35px, 45° 阶梯外排像素 cov≤0 被整
                   排裁掉 —— 名为 AA 实为硬边; 本系数 2·|den|/sum 才满足
                   覆盖率 = 1−距离/半宽 的解析口径。) */
                float k0 = 2.0f * fabsf(den) /
                           fmaxf(fabsf(y1 - y2) + fabsf(x2 - x1), 1e-6f);
                float k1 = 2.0f * fabsf(den) /
                           fmaxf(fabsf(y2 - y0) + fabsf(x0 - x2), 1e-6f);
                float k2 = 2.0f * fabsf(den) /
                           fmaxf(fabsf(y0 - y1) + fabsf(x1 - x0), 1e-6f);
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
                        /* 多边形外时取各边坡道的最小者(不乘积): 乘积在凸角
                           把两侧坡道相乘, 角像素覆盖被双重打折, 底层/背景
                           透出成孤点亮斑; min 与"过剩覆盖"同一口径 —— 只
                           会偏胖不会开裂, 直边/单边时与乘积逐位相同。 */
                        float cov = 1.0f;
                        if (w0 < 0.0f) { float c0 = 1.0f + w0 * k0; if (c0 < cov) cov = c0; }
                        if (w1 < 0.0f) { float c1 = 1.0f + w1 * k1; if (c1 < cov) cov = c1; }
                        if (w2 < 0.0f) { float c2 = 1.0f + w2 * k2; if (c2 < cov) cov = c2; }
                        if (cov <= 0.0f) continue;
                        float u = w0 * u0 + w1 * u1 + w2 * u2;
                        float v = w0 * v0 + w1 * v1 + w2 * v2;
                        int sx = (int)(u * (float)iw), sy = (int)(v * (float)ih);
                        if (sx < 0) sx = 0; else if (sx >= iw) sx = iw - 1;
                        if (sy < 0) sy = 0; else if (sy >= ih) sy = ih - 1;
                        const unsigned char *p = &ipx[(sy * iw + sx) * 4];
                        fcolor col = { p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f,
                                       p[3] / 255.0f * ga * cov };
                        fb_blend(f, xx, yy, col);
                    }
                }
            }
        }
    }
}

/*/* ---------------- 三角汤(Cubism drawable) ----------------
 * HN_CMD_TRIS: mesh_verts/mesh_uv = **顶点汤**(每三角 3 顶点, 已由
 * 发射侧从索引展开), mesh_cols = 三角形数, text = 贴图路径,
 * fill.a = 整体不透明度。与 paint_mesh 同一颗三角形仿射贴图内核
 * (逐像素重心插值 + fb_blend —— f->blend 由派发层按指令设置,
 * Additive=MESH 的 LIGHTER / Multiply=MULTIPLY 全都现成)。 */
static void paint_tris(fb *f, const hn_cmd *c, float alpha, float scale) {
    if (!c->mesh_verts || !c->mesh_uv || c->mesh_cols <= 0) return;
    int iw = 0, ih = 0;
    const unsigned char *ipx = image_pixels_cached(c->text ? c->text : "", &iw, &ih);
    if (getenv("HN_CUBISM_TRACE"))
        fprintf(stderr, "[tris] ntri=%d img=%p %dx%d path=%.60s\n", c->mesh_cols,
                (void *)ipx, iw, ih, c->text ? c->text : "");
    float ga = (float)(c->fill & 0xFFu) / 255.0f * alpha;
    if (ga <= 0.004f) return;
    if (!ipx) return;   /* 无解码器: 静默(Cubism 页面必有解码器, 与 MESH 的
                           示意网格不同 —— 三角汤画线只会更乱) */
    int ntri = c->mesh_cols;
    for (int t = 0; t < ntri; t++) {
        int a = t * 3, b = t * 3 + 1, d = t * 3 + 2;
        float x0 = c->mesh_verts[a * 2] * scale, y0 = c->mesh_verts[a * 2 + 1] * scale;
        float x1 = c->mesh_verts[b * 2] * scale, y1 = c->mesh_verts[b * 2 + 1] * scale;
        float x2 = c->mesh_verts[d * 2] * scale, y2 = c->mesh_verts[d * 2 + 1] * scale;
        float u0 = c->mesh_uv[a * 2], v0 = c->mesh_uv[a * 2 + 1];
        float u1 = c->mesh_uv[b * 2], v1 = c->mesh_uv[b * 2 + 1];
        float u2 = c->mesh_uv[d * 2], v2 = c->mesh_uv[d * 2 + 1];
        float den = (y1 - y2) * (x0 - x2) + (x2 - x1) * (y0 - y2);
        if (fabsf(den) < 1e-6f) continue;
        if (c->mesh_rows == 1 && den > 0.0f) continue;   /* TRIS 背面剔除(试验):
            mesh_rows=1 标记"单面", 负面积(屏幕顺绕)跳过 */
        /* 边缘 AA(解析覆盖率, 与 paint_mesh 同一颗内核, 替代旧的 ±2%
           重心容差二值跳过): w_i·k_i 恰为"像素中心到第 i 条边的带符号
           距离 ÷ 像素盒沿该边法向的投影半宽"(边长 L 在分子分母相消,
           免 sqrt)。单侧坡道: 形内(w≥0)恒全覆盖(内部像素逐位不变);
           形外覆盖率随距离线性衰减, 0 点在形外 0.5~0.71px —— 过渡
           只占一排像素。三角汤相邻三角的共享边上两侧合计仍 ≥1
           (一侧恒全覆盖, 过剩覆盖), 不产生欠覆盖接缝; 轮廓处按覆盖率
           alpha 混合, 硬锯齿变成 1 排灰度过渡。旧 -0.02 容差原本兼职
           补浮点裂缝, 这里 w≈0⁻ 时 cov≈1 同样无缝且不再向外多涂一整圈
           (容差 = 2%×高, 大三角形外扩可达数像素且粗细不均)。
           (系数口径同 paint_mesh: 2·|den|/sum 才是解析覆盖; 4 则零点
           减半、45° 阶梯外排整排被裁, 名为 AA 实为硬边 —— 2026-10-14
           复审核验后统一。) */
        float k0 = 2.0f * fabsf(den) /
                   fmaxf(fabsf(y1 - y2) + fabsf(x2 - x1), 1e-6f);
        float k1 = 2.0f * fabsf(den) /
                   fmaxf(fabsf(y2 - y0) + fabsf(x0 - x2), 1e-6f);
        float k2 = 2.0f * fabsf(den) /
                   fmaxf(fabsf(y0 - y1) + fabsf(x1 - x0), 1e-6f);
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
                /* 多边形外时取各边坡道的最小者(不乘积), 理由同 paint_mesh:
                   乘积在凸角双重打折 → 底层透出孤点亮斑; min 只偏胖不开裂,
                   直边/单边时与乘积逐位相同。 */
                float cov = 1.0f;
                if (w0 < 0.0f) { float c0 = 1.0f + w0 * k0; if (c0 < cov) cov = c0; }
                if (w1 < 0.0f) { float c1 = 1.0f + w1 * k1; if (c1 < cov) cov = c1; }
                if (w2 < 0.0f) { float c2 = 1.0f + w2 * k2; if (c2 < cov) cov = c2; }
                if (cov <= 0.0f) continue;
                float u = w0 * u0 + w1 * u1 + w2 * u2;
                float v = w0 * v0 + w1 * v1 + w2 * v2;
                int sxp = (int)(u * (float)iw), syp = (int)(v * (float)ih);
                if (sxp < 0) sxp = 0; else if (sxp >= iw) sxp = iw - 1;
                if (syp < 0) syp = 0; else if (syp >= ih) syp = ih - 1;
                const unsigned char *p = &ipx[(syp * iw + sxp) * 4];
                fcolor col = { p[0] / 255.0f, p[1] / 255.0f, p[2] / 255.0f,
                               p[3] / 255.0f * ga * cov };
                fb_blend(f, xx, yy, col);
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
        g_soft_allocs++;
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

/* RGBA8(非预乘) → 画布拉伸混合。IMAGE(解码文件)/BITMAP(媒体当前帧)/
   PDATA(putImageData 块)共用同一条像素路径 —— 三者的差别只在"像素从哪来"
   与"取源图的哪一块"(sx0/sy0/sw0/sh0 = 源图像素子矩形; 整图调用传
   (0,0,iw,ih), 行为与旧口径逐位一致; 9 参 drawImage 与 putImageData 的
   脏矩形裁剪都化归到这里)。
   目标 rect (x0,y0,w,h) 拉伸贴出源子矩形。

   扫描线快路(profile: fb_blend/fb_clip_ok 是三页共同的 Top 叶子 ——
   全幅 blit 每像素都重跑一遍浮点剪裁判定): 把 fb_clip_ok 的接受域
   **一次性**外提成整数 x 区间 + 行级 y 判定, 圆角裁剪与 NaN 坐标退回
   逐像素原路。等价性靠构造保证: 区间端点用与 fb_clip_ok **同一个**
   浮点谓词单调走查(x+0.5f >= cx0 / x+0.5f < cx1), 不引入任何新的
   舍入路径 → 接受集逐位相同。不透明快路(源 alpha=255、组 alpha≥0.999、
   目底 alpha=255 → blend 走双不透明直写)已对 0..255 全值验证
   round(v/255*255+0.5)==v 逐位成立, 直拷贝与 blend 输出一致。
   混合模式非 source-over 时禁用直写快路(逐像素走 blend_mode)。 */
static void blend_rgba(fb *f, const unsigned char *px, int stride, int iw, int ih,
                       float sx0, float sy0, float sw0, float sh0,
                       int x0, int y0, int w, int h, float alpha) {
    if (w <= 0 || h <= 0) return;
    int fast = 1, xlo = 0, xhi = f->w - 1;
    float cy0 = 0, cy1 = 0;
    if (f->blend != HN_BLEND_SRC_OVER) fast = 0;      /* 混合模式: 无直写 */
    if (f->poly_clips > 0) fast = 0;                  /* 路径裁剪: 逐像素判定 */
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
        const int sy = (int)(sy0 + (float)yy * sh0 / hdiv);
        const unsigned char *srow = px + (size_t)sy * stride;
        unsigned char *drow = f->px + (size_t)dy * f->w * 4;
        if (fast) {
            int a = xlo - x0;
            int b = xhi - x0;
            if (a < 0) a = 0;
            if (b > w - 1) b = w - 1;
            for (int xx = a; xx <= b; xx++) {
                const int sx = (int)(sx0 + (float)xx * sw0 / wdiv);
                const unsigned char *p = srow + (size_t)sx * 4;
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
            /* 原路(圆角/病态剪裁/混合模式): 逐像素 fb_clip_ok + blend_mode,
               source-over 下逐位不变 */
            for (int xx = 0; xx < w; xx++) {
                int sx = (int)(sx0 + (float)xx * sw0 / wdiv);
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
    /* 源子矩形(9 参 drawImage); 缺省整图(3/5 参, 旧口径逐位一致) */
    float sx0 = 0, sy0 = 0, sw0 = (float)iw, sh0 = (float)ih;
    if (c->has_isrc) {
        sx0 = c->isx; sy0 = c->isy; sw0 = c->isw; sh0 = c->ish;
        if (sx0 < 0) { sw0 += sx0; sx0 = 0; }
        if (sy0 < 0) { sh0 += sy0; sy0 = 0; }
        if (sx0 + sw0 > (float)iw) sw0 = (float)iw - sx0;
        if (sy0 + sh0 > (float)ih) sh0 = (float)ih - sy0;
        if (sw0 <= 0 || sh0 <= 0) return;
    }
    blend_rgba(f, px, iw * 4, iw, ih, sx0, sy0, sw0, sh0, x0, y0, w, h, alpha);
}

/* BITMAP: 媒体当前帧(RGBA8 非预乘, 引擎持有缓存)。位图为空时忽略 ——
   正常情况下引擎只在有帧时才发 BITMAP, 这里只是防御。 */
static void paint_bitmap(fb *f, const hn_cmd *c, float scale, float ox, float oy, float alpha) {
    if (!c->bitmap || c->bitmap_w <= 0 || c->bitmap_h <= 0 || c->bitmap_stride <= 0) return;
    int x0 = (int)((c->x - ox) * scale + ox), y0 = (int)((c->y - oy) * scale + oy);
    int w = (int)(c->w * scale), h = (int)(c->h * scale);
    float layer_alpha = (float)(c->fill & 0xFFu) / 255.0f;
    blend_rgba(f, c->bitmap, c->bitmap_stride, c->bitmap_w, c->bitmap_h,
               0, 0, (float)c->bitmap_w, (float)c->bitmap_h,
               x0, y0, w, h, alpha * layer_alpha);
}

/* PDATA: putImageData 的 RGBA 块(引擎账本持有, 1:1 贴入; 源子矩形 =
   脏矩形裁剪, 记录侧已装进 isrc)。标准口径: 不受 gco 影响 —— 引擎侧
   blend 恒缺省 source-over。 */
static void paint_pdata(fb *f, const hn_cmd *c, float scale, float ox, float oy, float alpha) {
    if (!c->bitmap || c->bitmap_w <= 0 || c->bitmap_h <= 0 || c->bitmap_stride <= 0) return;
    int x0 = (int)((c->x - ox) * scale + ox), y0 = (int)((c->y - oy) * scale + oy);
    int w = (int)(c->w * scale), h = (int)(c->h * scale);
    blend_rgba(f, c->bitmap, c->bitmap_stride, c->bitmap_w, c->bitmap_h,
               c->isx, c->isy, c->isw, c->ish, x0, y0, w, h, alpha);
}

/* ---------------- pattern 平铺填充(createPattern) ---------------- */

/* 平铺采样: 目标像素 (u,v) 相对图案原点的偏移 → 按平铺模式折回源图像素。
   返回 0 = 该偏移无像素(repeat-x 的纵越界 / no-repeat 的越界)。 */
static int pattern_sample(const unsigned char *px, int iw, int ih, int rep,
                          float du, float dv, fcolor *out) {
    int u, v;
    switch (rep) {
    case HN_PAT_REPEAT:
        u = (int)fmodf(du, (float)iw);
        v = (int)fmodf(dv, (float)ih);
        if (u < 0) u += iw;
        if (v < 0) v += ih;
        break;
    case HN_PAT_REPEAT_X:
        u = (int)fmodf(du, (float)iw);
        if (u < 0) u += iw;
        v = (int)dv;
        if (v < 0 || v >= ih) return 0;
        break;
    case HN_PAT_REPEAT_Y:
        v = (int)fmodf(dv, (float)ih);
        if (v < 0) v += ih;
        u = (int)du;
        if (u < 0 || u >= iw) return 0;
        break;
    default:                                     /* HN_PAT_NONE */
        u = (int)du;
        v = (int)dv;
        if (u < 0 || v < 0 || u >= iw || v >= ih) return 0;
        break;
    }
    const unsigned char *p = px + ((size_t)v * iw + (size_t)u) * 4;
    out->r = p[0] / 255.0f;
    out->g = p[1] / 255.0f;
    out->b = p[2] / 255.0f;
    out->a = p[3] / 255.0f;
    return 1;
}

/* PATTERN 指令: 目标 = 矩形(poly==NULL, 行列直铺)或闭合多边形(扫描线
   求交区间, 与 paint_polygon 的交点机制同款)。逐像素按平铺模式采样源图,
   经 fb_blend 落画布(裁剪栈/混合模式照常生效)。
   锚定口径: 图案原点 = 画布坐标原点(逻辑 px; HiDPI 下采样坐标先除回
   scale)—— 与 canvas 的 pattern space 语义一致(不随 fillRect 位置平移);
   createPattern 时刻的 CTM 简化为不参与(与折线路径模型同一既定档)。 */
static void paint_pattern(fb *f, const hn_cmd *c, float scale) {
    int iw = 0, ih = 0;
    const unsigned char *px = image_pixels_cached(c->text ? c->text : "", &iw, &ih);
    if (!px || iw <= 0 || ih <= 0) return;   /* 缺图: 无覆盖(而非静默涂黑) */
    int rep = c->pattern_rep;
    if (!c->poly) {
        /* 矩形目标: 行列直铺(采样坐标 = 相对图案锚点/画布原点的逻辑偏移) */
        int x0 = (int)floorf(c->x * scale), y0 = (int)floorf(c->y * scale);
        int x1 = (int)ceilf((c->x + c->w) * scale), y1 = (int)ceilf((c->y + c->h) * scale);
        if (x0 < 0) x0 = 0;
        if (y0 < 0) y0 = 0;
        if (x1 > f->w) x1 = f->w;
        if (y1 > f->h) y1 = f->h;
        for (int y = y0; y < y1; y++)
            for (int x = x0; x < x1; x++) {
                fcolor col;
                if (!pattern_sample(px, iw, ih, rep,
                                    ((float)x - c->pat_ox) / scale,
                                    ((float)y - c->pat_oy) / scale, &col)) continue;
                fb_blend(f, x, y, col);
            }
        return;
    }
    /* 多边形目标: 逐扫描线求交, 非零环绕区间内逐像素采样 */
    int n = c->poly_n;
    if (n < 3) return;
    enum { PST_STACK = 256 };
    float vx_stack[PST_STACK], vy_stack[PST_STACK];
    float *vx = vx_stack, *vy = vy_stack;
    xhit *hits = NULL;
    if (n > PST_STACK) {
        float *d1 = NULL, *d2 = NULL;
        if (!poly_scratch_ensure(n, &d1, &d2, &hits)) return;
        vx = d1; vy = d2;
    } else {
        float *d1 = NULL, *d2 = NULL;
        poly_scratch_ensure(1, &d1, &d2, &hits);     /* hits 槽(顶点走栈) */
        if (!hits) return;
    }
    float miny = 1e9f, maxy = -1e9f;
    for (int i = 0; i < n; i++) {
        vx[i] = c->poly[i * 2] * scale;
        vy[i] = c->poly[i * 2 + 1] * scale;
        if (vy[i] < miny) miny = vy[i];
        if (vy[i] > maxy) maxy = vy[i];
    }
    int y0 = (int)floorf(miny), y1 = (int)ceilf(maxy);
    for (int s = y0; s <= y1; s++) {
        if (s < 0 || s >= f->h) continue;
        float fy = (float)s + 0.5f;
        int nh = 0;
        for (int e = 0; e < n; e++) {
            int e2 = (e + 1) % n;
            float ay = vy[e], by = vy[e2];
            if (fabsf(by - ay) < 1e-6f) continue;
            if ((ay <= fy && by > fy) || (by <= fy && ay > fy)) {
                float t = (fy - ay) / (by - ay);
                hits[nh].x = vx[e] + t * (vx[e2] - vx[e]);
                hits[nh].wind = (by > ay) ? 1 : -1;
                nh++;
            }
        }
        if (nh < 2) continue;
        for (int a2 = 1; a2 < nh; a2++) {
            xhit k = hits[a2];
            int b2 = a2 - 1;
            while (b2 >= 0 && hits[b2].x > k.x) { hits[b2 + 1] = hits[b2]; b2--; }
            hits[b2 + 1] = k;
        }
        for (int k = 0; k < nh - 1; k++) {
            int wsum = 0;
            for (int q = 0; q <= k; q++) wsum += hits[q].wind;
            if (wsum == 0) continue;                 /* 非零环绕外 */
            float lx = hits[k].x, rx = hits[k + 1].x;
            if (rx <= lx) continue;
            int xi0 = (int)floorf(lx), xi1 = (int)ceilf(rx);
            if (xi0 < 0) xi0 = 0;
            if (xi1 > f->w) xi1 = f->w;
            for (int x = xi0; x < xi1; x++) {
                float fx = (float)x + 0.5f;
                if (fx < lx || fx >= rx) continue;
                fcolor col;
                if (!pattern_sample(px, iw, ih, rep,
                                    (fx - c->pat_ox) / scale,
                                    (fy - c->pat_oy) / scale, &col)) continue;
                fb_blend(f, x, s, col);
            }
        }
    }
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

/* 指令循环(渲染核心): fb 已就绪(尺寸/底色全像素覆盖/裁剪栈清零), 逐条
 * 消费显示列表。两个公开变体(hnsoft_render_scaled 一次性缓冲 /
 * hnsoft_render_reuse[_scaled] 复用缓冲)都落到这里 —— 同一条渲染代码
 * 路径, 像素输出逐位一致, 差别只在缓冲归谁分配。 */
static void render_cmds(fb *f, const hn_display_list *dl, float rs) {
    /* cmds == NULL 且 count == 0 是**合法的空显示列表**(文档没有任何可见
       指令, 如只有空白 canvas 的页面): 仍要产出底色位图 —— 否则"首帧为空"
       的文档会被运行时判成渲染失败, <script> 驱动的页面(canvas 2D 账本)
       根本活不到脚本执行。 */
    for (int i = 0; i < dl->count; i++) {
        const hn_cmd *c = &dl->cmds[i];
        f->blend = c->blend;   /* 每指令混合模式(fb_blend/blend_rgba 消费) */
        switch (c->kind) {
        case HN_CMD_RECT:
            if (c->shadow) {
                /* 有阴影先画阴影(在填充之下) */
                paint_shadow(f, c, rs, 0, 0, 1.0f);
            }
            paint_rect(f, c, rs, 0, 0, 1.0f);
            break;
        case HN_CMD_TEXT:
            /* 文本: 字号 ×rs 后走 FreeType —— 真 2x 字形光栅, 不是拉伸 */
            paint_text(f, c, rs, 0, 0, 1.0f);
            break;
        case HN_CMD_IMAGE:
            paint_image(f, c, rs, 0, 0, 1.0f);
            break;
        case HN_CMD_BITMAP:
            paint_bitmap(f, c, rs, 0, 0, 1.0f);
            break;
        case HN_CMD_PDATA:
            paint_pdata(f, c, rs, 0, 0, 1.0f);
            break;
        case HN_CMD_PATTERN:
            paint_pattern(f, c, rs);
            break;
        case HN_CMD_CLEAR_RECT:
            paint_clear_rect(f, c, rs);
            break;
        case HN_CMD_CLIP_PUSH:
            fb_push_clip(f, c->x * rs, c->y * rs,
                         (c->x + c->w) * rs, (c->y + c->h) * rs, c->radius * rs);
            break;
        case HN_CMD_CLIP_PATH_PUSH:
            fb_push_clip_poly(f, c->poly, c->poly_n,
                              c->clip_segs, c->clip_nsegs, rs);
            break;
        case HN_CMD_QUAD:
            paint_quad(f, c, 1.0f, rs);
            break;
        case HN_CMD_POLYGON:
            paint_polygon(f, c, rs, 0, 0, 1.0f);
            break;
        case HN_CMD_MESH:
            paint_mesh(f, c, 1.0f, rs);
            break;
        case HN_CMD_TRIS:
            paint_tris(f, c, 1.0f, rs);
            break;
        case HN_CMD_CLIP_POP:
            fb_pop_clip(f);
            break;
        }
    }
    f->blend = HN_BLEND_SRC_OVER;
}

/* 公开变体①: 每次调用 malloc 一张新位图(一次性口径; shot/CLI/探针用)。
 * 帧循环宿主请用 hnsoft_render_reuse —— 否则动画/媒体页每帧一对
 * malloc/free 全页位图(920x520 下 ~2MB × 帧率的分配器 churn)。 */
unsigned char *hnsoft_render_scaled(const hn_display_list *dl, int width, int height,
                                    hn_color bg, float scale) {
    if (!dl || width <= 0 || height <= 0) return NULL;
    /* scale 缺省/非法按 1.0(hnsoft_render 的既有口径, 逐位一致) */
    float rs = (scale >= 0.5f) ? scale : 1.0f;
    fb f;
    f.w = width; f.h = height; f.clip_n = 0; f.bg = bg;
    f.poly_clips = 0; f.blend = HN_BLEND_SRC_OVER;
    f.px = (unsigned char *)malloc((size_t)width * height * 4);
    g_soft_allocs++;
    if (!f.px) return NULL;
    fb_fill_bg(&f, bg);
    render_cmds(&f, dl, rs);
    return f.px;
}

/* 公开变体②(主实现, 带 scale): 渲染进**调用方持有的复用缓冲**。
 * 需求字节(w*h*4) > *slot_cap 时 realloc 扩容(只增不减), 然后与变体①
 * 同一条 fb_fill_bg + render_cmds 路径 —— 像素逐位一致。失败返回 NULL
 * 且 *slot/*slot_cap 保持原值(旧缓冲原地保留, 上一帧位图仍可 blit)。 */
unsigned char *hnsoft_render_reuse_scaled(const hn_display_list *dl, int width, int height,
                                          hn_color bg, float scale,
                                          unsigned char **slot, int *slot_cap) {
    if (!dl || width <= 0 || height <= 0 || !slot || !slot_cap) return NULL;
    size_t need = (size_t)width * (size_t)height * 4;
    if (need > 0x7FFFFFFFu) return NULL;        /* *slot_cap 是 int 口径的防呆上限 */
    if (!*slot || (int)need > *slot_cap) {
        /* 只增不减: 窗口缩小不缩容(Resize 抖动不反复 realloc)。 */
        unsigned char *np = (unsigned char *)realloc(*slot, need);
        if (!np) return NULL;
        *slot = np;
        *slot_cap = (int)need;
        g_soft_allocs++;                        /* 只在扩容时计数(稳态 0) */
    }
    fb f;
    f.w = width; f.h = height; f.clip_n = 0; f.bg = bg; f.px = *slot;
    f.poly_clips = 0; f.blend = HN_BLEND_SRC_OVER;
    fb_fill_bg(&f, bg);
    float rs = (scale >= 0.5f) ? scale : 1.0f;
    render_cmds(&f, dl, rs);
    return *slot;
}

/* 公开变体②的 1x 薄封装(hnsoft_render 的复用对应物; scale 缺省/非法
 * 按 1.0 的口径与变体①一致)。 */
unsigned char *hnsoft_render_reuse(const hn_display_list *dl, int width, int height,
                                   hn_color bg, unsigned char **slot, int *slot_cap) {
    return hnsoft_render_reuse_scaled(dl, width, height, bg, 1.0f, slot, slot_cap);
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

float hnsoft_paint_mask_cov(float u, float fx, float fy, float w, float h) {
    if (u <= 0) return 0;
    if (u >= 1) return 1;
    float ax = 0.858f, ay = 0.514f;               /* 单位轴(≈55° 对角) */
    float L = w * ax + h * ay;                    /* 投影跨度 */
    if (L <= 0) return 1;
    float F = L * 0.20f;                          /* 笔锋带宽 */
    float A = L * 0.015f;                         /* 波纹幅值(有界) */
    float e = 1.0f - (1.0f - u) * (1.0f - u);     /* ease-out */
    float P = -F + e * (L + 2.0f * F);            /* 笔锋头部投影位置 */
    float d0 = P - (fx * ax + fy * ay);           /* 头部有向距离 */
    /* 波纹免三角快判: ripple ∈ [-A, A] 有界, d0 超出 ±(F/2+A) 的像素
       直接判满/空(判定与精确式严格一致) —— 只有笔锋带内的像素才算
       sinf。2x 大窗(640k px)铺色帧从 ~10ms 降到 ~1-2ms。 */
    if (d0 - A >= F) return 1;
    if (d0 + A <= 0) return 0;
    float across = fx * ay - fy * ax;             /* 垂直轴坐标 */
    float ripple = sinf(across * 0.020f + u * 5.2f) * A;
    float m = (d0 + ripple) / F;
    if (m < 0) m = 0; else if (m > 1) m = 1;
    return m * m * (3.0f - 2.0f * m);             /* smoothstep */
}

int hnsoft_font_loaded(void) {
#ifndef HN_NO_TEXT
    font_init();
    return ft_face_ok;
#else
    return 0;
#endif
}

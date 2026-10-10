/* hn_paint.c — DOM → 绘制指令列表(文档序深度优先, 父先子后)
 * 滚动: overflow 容器把子树绘制偏移 (scroll_x, scroll_y) 并用
 * CLIP_PUSH/CLIP_POP 裁剪到自身盒内; 偏移沿树下累积。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn_internal.h"

static hn_color mul_alpha(hn_color col, float a) {
    unsigned al = (unsigned)(col & 0xFFu);
    al = (unsigned)((float)al * a);
    if (al > 255) al = 255;
    return (col & 0xFFFFFF00u) | al;
}

/* ---- filter(brightness/contrast/saturate) ----
   绘制期对 fill 颜色做调色 —— 只调 RGB, alpha 不动。三档连乘已并入
   单乘数(解析期), 顺序 saturate → contrast → brightness; 单函数声明下
   与 CSS 精确一致, 多函数链是近似(CSS 严格按序逐像素复合)。 */
static hn_color fx_color(hn_color col, const hn_style *st) {
    float br = st->filter_br, ct = st->filter_ct, sat = st->filter_sat;
    if (br == 1.0f && ct == 1.0f && sat == 1.0f) return col;
    float r = (float)((col >> 24) & 0xFF) / 255.0f;
    float g = (float)((col >> 16) & 0xFF) / 255.0f;
    float b = (float)((col >> 8) & 0xFF) / 255.0f;
    if (sat != 1.0f) {
        /* Rec.709 亮度: 0 = 灰度, 2 = 过饱和(允许越界, 末端钳制) */
        float lum = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        r = lum + (r - lum) * sat;
        g = lum + (g - lum) * sat;
        b = lum + (b - lum) * sat;
    }
    if (ct != 1.0f) {
        r = (r - 0.5f) * ct + 0.5f;
        g = (g - 0.5f) * ct + 0.5f;
        b = (b - 0.5f) * ct + 0.5f;
    }
    if (br != 1.0f) {
        r *= br; g *= br; b *= br;
    }
    int ir = (int)(r * 255.0f + 0.5f), ig = (int)(g * 255.0f + 0.5f), ib = (int)(b * 255.0f + 0.5f);
    if (ir < 0) ir = 0; if (ir > 255) ir = 255;
    if (ig < 0) ig = 0; if (ig > 255) ig = 255;
    if (ib < 0) ib = 0; if (ib > 255) ib = 255;
    return ((hn_color)ir << 24) | ((hn_color)ig << 16) | ((hn_color)ib << 8) | (col & 0xFFu);
}

/* 样式色 → 指令 fill: 先过 filter 调色, 再乘组透明度。
   凡是从样式取色(background / color / border / grad 等)的出口都走这里。 */
static hn_color fill_color(const hn_style *st, hn_color col, float alpha) {
    return mul_alpha(fx_color(col, st), alpha);
}

/* 引擎缺省强调色(:focus 环 / 选中态填充)。与主题 --accent 同族,
   作者/主题可用 :focus 或 :checked 规则覆盖观感。 */
#define HN_ACCENT 0x3D74FFFFu

/* 动画 4 分量(rgba 0..1) → 打包色 */
static hn_color anim_color(const float v[4]) {
    int r = (int)(v[0] * 255.0f + 0.5f), g = (int)(v[1] * 255.0f + 0.5f);
    int b = (int)(v[2] * 255.0f + 0.5f), a = (int)(v[3] * 255.0f + 0.5f);
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    if (a < 0) a = 0; if (a > 255) a = 255;
    return ((hn_color)r << 24) | ((hn_color)g << 16) | ((hn_color)b << 8) | (hn_color)a;
}

/* 绘制期变换状态: 位移已并入 sx/sy; scale 需要几何换算。
   缩放原点取元素盒中心 —— 与 CSS transform-origin: center 一致。
   ---- 3D 层级(preserve-3d) ----
   m3d: 祖先链累积的复合 3D 矩阵(列向量口径, 仿射部分, 不含透视);
        flat 元素把它重置为单位阵(子级 3D 从零开始), preserve-3d 元素
        把"自己叠完的"传下去。persp_d/pcx/pcy: 最近的 perspective 声明
        (视距 + 声明者盒中心 = 消失点), 未声明时 0 = 正交。
   ---- flat 摊平仿射 ----
   axx..aty: 2D 仿射(axx axy / ayx ayy 为线性部分, atx aty 为平移),
   把"被投影祖先"的平面坐标映到屏幕 —— 父卡转动后子内容随面片摊平
   (与 CSS 扁平化语义一致)。缺省 = 单位阵, 此时 tfx/tfy 与旧口径逐位一致。
   fhx/fhy/fhw: 摊平的**射影分母**(w = fhx·x + fhy·y + fhw, 缺省 0/0/1 =
   仿射)。透视投影是射影映射, 仿射 6 自由度表达不了第 4 角 —— 装入完整
   单应后 tfx/tfy 按 u = 分子/w 求值(与旧口径逐位一致: w 恒为 1 时除法
   精确)。fhx/fhy 非零即"射影摊平"生效, 文字/盒尺寸按锚点处局部线尺度
   取档(近大远小)。 */
typedef struct {
    int   depth;
    int   nodes;
    float scale;   /* 累积等比缩放(默认 1) */
    float scale_x, scale_y;  /* 累积**非等比**分量(默认 1) */
    float text_scale;        /* 文字等比缩放(默认 1): 2D 链累积 × 摊平仿射的
                                均匀等效(sqrt|det|) —— 字号随面片一起缩放 */
    float ox, oy;  /* 缩放原点(绝对坐标) */
    float m3d[4][4];      /* 祖先链复合 3D 矩阵(缺省单位阵) */
    float persp_d;        /* 最近的 perspective 视距(px; 0 = 无/正交) */
    float persp_cx, persp_cy;   /* 视距声明者盒中心(消失点) */
    float axx, axy, atx;  /* 摊平仿射: u = axx*x + axy*y + atx */
    float ayx, ayy, aty;  /*             v = ayx*x + ayy*y + aty */
    float fhx, fhy, fhw;  /* 摊平射影分母: w = fhx*x + fhy*y + fhw
                             (缺省 0/0/1 = 仿射; install_flatten 装单应) */
} paint_guard;

/* ---- 4x4 矩阵(preserve-3d 层级栈的最小算术) ----
   行主序存储, 列向量口径: p' = M·p。所有运算按固定书写顺序展开
   (先 i 后 j, 乘积按 0..3 累加), 不依赖 FMA —— 跨架构确定性由
   构建门禁的 -ffp-contract=off 兜底(见 tools/build-multiplatform.sh)。 */

static void mat_identity(float m[4][4]) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            m[i][j] = (i == j) ? 1.0f : 0.0f;
}

static int mat_is_identity(const float m[4][4]) {
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++) {
            float want = (i == j) ? 1.0f : 0.0f;
            if (m[i][j] != want) return 0;
        }
    return 1;
}

/* o = a·b(先作用 b, 再作用 a)。o 不得与 a/b 同址(经 r 中转)。 */
static void mat_mul(float o[4][4], const float a[4][4], const float b[4][4]) {
    float r[4][4];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            r[i][j] = a[i][0] * b[0][j] + a[i][1] * b[1][j]
                    + a[i][2] * b[2][j] + a[i][3] * b[3][j];
    for (int i = 0; i < 4; i++)
        for (int j = 0; j < 4; j++)
            o[i][j] = r[i][j];
}

/* 左乘平移阵(o = T·m): 平移在**变换后**坐标系里生效(先 m 后 T)。 */
static void mat_translate(float o[4][4], const float m[4][4],
                          float tx, float ty, float tz) {
    float t[4][4];
    mat_identity(t);
    t[0][3] = tx; t[1][3] = ty; t[2][3] = tz;
    mat_mul(o, t, m);
}

/* 左乘旋转阵。axis: 0=X 1=Y 2=Z, 角度 rad。行、列排布与既有逐角点
   旋转公式一致(y' = y·cos − z·sin 等), 只是改为矩阵形式可复合。 */
static void mat_rotate(float o[4][4], const float m[4][4], int axis, float rad) {
    float r[4][4], c = cosf(rad), s = sinf(rad);
    mat_identity(r);
    if (axis == 0) {
        r[1][1] = c; r[1][2] = -s;
        r[2][1] = s; r[2][2] = c;
    } else if (axis == 1) {
        r[0][0] = c;  r[0][2] = s;
        r[2][0] = -s; r[2][2] = c;
    } else {
        r[0][0] = c; r[0][1] = -s;
        r[1][0] = s; r[1][1] = c;
    }
    mat_mul(o, r, m);
}

/* 仿射阵作用一个点: (x,y,0,1) → (u,v,z,w)。z 供透视除用;
   本引擎的 3D 矩阵只由平移/旋转构成(透视在投影处单独除), w 恒为 1。 */
static void mat_point(const float m[4][4], float x, float y,
                      float *pu, float *pv, float *pz) {
    *pu = m[0][0] * x + m[0][1] * y + m[0][3];
    *pv = m[1][0] * x + m[1][1] * y + m[1][3];
    *pz = m[2][0] * x + m[2][1] * y + m[2][3];
}

/* ---- 3D 变换 ---- */

/* 元素的局部 3D 矩阵(不含父级):
   L = T(origin) · Rz · Ry · Rx · R3d · T_z(translateZ) · T(-origin)
   origin 为**绝对坐标**(bx+偏移) —— 与既有逐角点口径一致(先减 origin
   旋转再加回); 旋转顺序与旧实现相同(rotate3d 的 Rodrigues 结果先作用,
   再 X → Y → Z; translateZ 为纯深度位移, 先于全部旋转 —— 即 CSS 里
   把 translateZ 写在函数列末尾的语义)。origin 缺省 = 盒中心。
   返回 1 = 元素带 3D 变换(rotate/rotateX/Y/Z/rotate3d/translateZ 任一非零)。 */
static int st_local_3d(const hn_style *st, float bx, float by,
                       float bw, float bh, float m[4][4]) {
    if (st->rotate_x == 0 && st->rotate_y == 0 && st->rotate == 0
        && !st->has_r3d && st->translate_z == 0) return 0;
    const float D2R = 3.14159265358979f / 180.0f;
    /* transform-origin: 缺省盒中心; % 在此处按盒尺寸解出 */
    float cx = bx + (st->has_origin
                     ? (st->origin_pct_x ? st->origin_x * bw : st->origin_x)
                     : bw * 0.5f);
    float cy = by + (st->has_origin
                     ? (st->origin_pct_y ? st->origin_y * bh : st->origin_y)
                     : bh * 0.5f);
    mat_identity(m);
    mat_translate(m, m, -cx, -cy, 0);                       /* T(-origin) */
    if (st->translate_z != 0) mat_translate(m, m, 0, 0, st->translate_z);
    if (st->has_r3d) {
        /* rotate3d(x,y,z,angle): 绕任意轴旋转(Rodrigues 公式), 轴已归一化。
           R = (1-c)·A⊗A + c·I + s·[A]× —— 与既有逐角点实现同一组系数。 */
        float c3 = cosf(st->r3d_deg * D2R), s3 = sinf(st->r3d_deg * D2R);
        float ax = st->r3d_x, ay = st->r3d_y, az = st->r3d_z;
        float t3 = 1.0f - c3;
        float r[4][4];
        mat_identity(r);
        r[0][0] = t3*ax*ax + c3;      r[0][1] = t3*ax*ay - s3*az; r[0][2] = t3*ax*az + s3*ay;
        r[1][0] = t3*ax*ay + s3*az;   r[1][1] = t3*ay*ay + c3;    r[1][2] = t3*ay*az - s3*ax;
        r[2][0] = t3*ax*az - s3*ay;   r[2][1] = t3*ay*az + s3*ax; r[2][2] = t3*az*az + c3;
        mat_mul(m, r, m);
    }
    if (st->rotate_x != 0) mat_rotate(m, m, 0, st->rotate_x * D2R);
    if (st->rotate_y != 0) mat_rotate(m, m, 1, st->rotate_y * D2R);
    if (st->rotate != 0)   mat_rotate(m, m, 2, st->rotate   * D2R);
    mat_translate(m, m, cx, cy, 0);                         /* T(origin) */
    return 1;
}

/* 元素的 2D 缩放, 3D 阵形式(preserve-3d 子级复合用):
   S = T(origin)·diag(sx, sy, 1)·T(-origin)。因子与 2D 绘制状态同源
   (scale·scale_x / scale·scale_y), 原点与 2D 路径同款(盒中心 /
   transform-origin)。返回 0 = 元素无 2D 缩放(m 为单位阵, 调用方不必乘)。
   CSS 里 2D 函数就是 3D 矩阵的退化形式 —— transform-style: preserve-3d
   时它必须进复合矩阵, 否则 scale(2) 祖先下的子面片投影尺寸不变。 */
static int st_scale_2d(const hn_style *st, float ox, float oy, float m[4][4]) {
    mat_identity(m);
    int active = (st->scale != 1.0f && st->scale > 0.01f)
              || st->scale_x != 1.0f || st->scale_y != 1.0f;
    if (!active) return 0;
    float sx = st->scale_x, sy = st->scale_y;
    if (st->scale != 1.0f && st->scale > 0.01f) { sx *= st->scale; sy *= st->scale; }
    m[0][0] = sx; m[1][1] = sy;
    m[0][3] = ox - sx * ox;
    m[1][3] = oy - sy * oy;
    return 1;
}

/* 复合矩阵 + 透视 → 屏幕四角。视距 persp ≤ 0 为正交(不除);
   透视时 w = 1 − z/d, 收缩系数 k = d/(d−z) 与既有单元素口径逐式相同
   (含 [0.05,20] 钳制 —— 面片越过视平面时压到近处, 不爆坐标)。 */
static void project_corners(const float m[4][4], float bx, float by,
                            float bw, float bh,
                            float persp, float sx, float sy,
                            float pcx, float pcy,
                            float ox[4], float oy[4]) {
    float lx[4] = { 0, bw, bw, 0 };
    float ly[4] = { 0, 0, bh, bh };
    float dist = persp > 0 ? persp : 0;      /* 0 = 正交投影 */
    for (int i = 0; i < 4; i++) {
        float u, v, z;
        mat_point(m, bx + lx[i], by + ly[i], &u, &v, &z);
        float px = u, py = v;
        if (dist > 0.01f) {
            /* 透视: 离观察者越远(z 越小)缩放越小, 缩放朝消失点(pcx,pcy)
               收缩 —— 父级 perspective 时即声明者盒中心 */
            float k = dist / (dist - z);
            if (k < 0.05f) k = 0.05f;
            if (k > 20.0f) k = 20.0f;
            px = pcx + (px - pcx) * k;
            py = pcy + (py - pcy) * k;
        }
        ox[i] = px - sx;
        oy[i] = py - sy;
    }
}

/* project_3d — 矩阵口径的投影入口: 局部矩阵叠上继承矩阵后投影四角。
   parent = 祖先链复合矩阵(flat 祖先链上恒为单位阵, 行为与旧单元素口径
   一致); out_total 恒被写出(= parent·L, preserve-3d 时传给子级),
   无自身 3D 且 parent 为单位阵时早退返回 0(矩形快路径, 行为不变)。 */
static int project_3d(const hn_style *st, const float parent[4][4],
                      float bx, float by, float bw, float bh,
                      float persp, float sx, float sy,
                      float pcx, float pcy,
                      float ox[4], float oy[4],
                      float out_total[4][4]) {
    float local[4][4];
    if (!st_local_3d(st, bx, by, bw, bh, local)) {
        /* 无自身 3D: 继承矩阵也是单位阵 → 矩形快路径(单元素行为不变);
           否则内容坐在 preserve-3d 祖先的面片上, 随复合矩阵投影。 */
        if (mat_is_identity(parent)) {
            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++)
                    out_total[i][j] = parent[i][j];
            return 0;
        }
        for (int i = 0; i < 4; i++)
            for (int j = 0; j < 4; j++)
                out_total[i][j] = parent[i][j];
        project_corners(out_total, bx, by, bw, bh, persp, sx, sy, pcx, pcy, ox, oy);
        return 1;
    }
    mat_mul(out_total, parent, local);
    project_corners(out_total, bx, by, bw, bh, persp, sx, sy, pcx, pcy, ox, oy);
    return 1;
}

/* 绝对坐标 → 设备坐标(先过摊平单应/仿射, 再绕原点缩放, 最后减滚动偏移)。
   摊平缺省为单位阵 + 分母 1 —— 此时与旧口径逐位一致((x-ox)*S + ox - sx)。
   射影摊平(install_flatten 装了单应)按 u = 分子/w 求值; w≈0(点越过视平面)
   钳到 1e-6 防爆。x/y 各自用自己的缩放分量 —— scale(2,1) 不能被当成等比 2。 */
static inline float tfx(const paint_guard *g, float sx, float x, float y) {
    float w = g->fhx * x + g->fhy * y + g->fhw;
    if (w > -1e-6f && w < 1e-6f) w = w < 0.0f ? -1e-6f : 1e-6f;
    float u = (g->axx * x + g->axy * y + g->atx) / w;
    return (u - g->ox) * g->scale * g->scale_x + g->ox - sx;
}
static inline float tfy(const paint_guard *g, float sy, float x, float y) {
    float w = g->fhx * x + g->fhy * y + g->fhw;
    if (w > -1e-6f && w < 1e-6f) w = w < 0.0f ? -1e-6f : 1e-6f;
    float v = (g->ayx * x + g->ayy * y + g->aty) / w;
    return (v - g->oy) * g->scale * g->scale_y + g->oy - sy;
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

/* 射影摊平在点 (x,y) 处沿盒 x 边 / y 边的局部线尺度(雅可比列向量的模)。
   透视下面片内近大远小 —— 均匀一档(sqrt|det|)给不出位置相关的尺度,
   文字字号 / 盒尺寸以**锚点处**的局部尺度计:
   ∂u/∂x = (axx·w − U·fhx)/w² 等(U/V 为分子多项式在点处的值)。 */
static void flatten_axes_at(const paint_guard *g, float x, float y,
                            float *kx, float *ky) {
    float w = g->fhx * x + g->fhy * y + g->fhw;
    if (w > -1e-6f && w < 1e-6f) w = w < 0.0f ? -1e-6f : 1e-6f;
    float u = g->axx * x + g->axy * y + g->atx;
    float v = g->ayx * x + g->ayy * y + g->aty;
    float iw = 1.0f / (w * w);
    float dux = (g->axx * w - u * g->fhx) * iw;
    float dvx = (g->ayx * w - v * g->fhx) * iw;
    float duy = (g->axy * w - u * g->fhy) * iw;
    float dvy = (g->ayy * w - v * g->fhy) * iw;
    *kx = sqrtf(dux * dux + dvx * dvx);
    *ky = sqrtf(duy * duy + dvy * dvy);
}

/* 把"父面片的投影四边形"装进绘制状态(flat 扁平化):
   盒四角 ↔ 投影四边形四角解**单应**(透视投影是射影映射, 仿射 6 自由度
   表达不了第 4 角 —— 三点仿射拟合在远边误差达数十 px), 子树坐标经
   tfx/tfy 按 u = 分子/w 落到面片对应位置。退化(四角共线等不可解)时
   回退左上/右上/左下三点仿射(旧口径), 无射影分母。
   面片的尺度已并入单应, 子树的累积 scale 归位(不叠加父的透视缩放)。 */
static void install_flatten(paint_guard *g, float bx, float by, float bw, float bh,
                            const float qx[4], const float qy[4]) {
    if (bw < 0.0001f || bh < 0.0001f) return;
    float e1x = (qx[1] - qx[0]) / bw, e1y = (qy[1] - qy[0]) / bw;
    float e2x = (qx[3] - qx[0]) / bh, e2y = (qy[3] - qy[0]) / bh;
    double h[9];
    if (homography_box_to_quad(bx, by, bw, bh, qx, qy, h)) {
        g->axx = (float)h[0]; g->axy = (float)h[1]; g->atx = (float)h[2];
        g->ayx = (float)h[3]; g->ayy = (float)h[4]; g->aty = (float)h[5];
        g->fhx = (float)h[6]; g->fhy = (float)h[7]; g->fhw = 1.0f;
    } else {
        g->axx = e1x; g->axy = e2x; g->atx = qx[0] - e1x * bx - e2x * by;
        g->ayx = e1y; g->ayy = e2y; g->aty = qy[0] - e1y * bx - e2y * by;
        g->fhx = 0.0f; g->fhy = 0.0f; g->fhw = 1.0f;
    }
    g->scale = 1.0f; g->scale_x = 1.0f; g->scale_y = 1.0f;
    g->ox = 0.0f; g->oy = 0.0f;
    /* 子树文字随面片缩放: 缺省档 = 仿射的均匀等效 sqrt|det|(det = 两棱叉积,
       即投影四边形与元素盒的面积比)。射影摊平(fhx/fhy 非零)时 paint_runs
       会以**锚点处局部线尺度**逐 run 覆盖 —— 同一张转动面片内近大远小。
       **简化口径(如实写明)**: 锚点位置/字号档是对的 —— 字形本身仍按轴对齐
       位图绘制, 不做逐像素透视畸变(一般透视四边形需要逐字形纹理映射,
       对 UI 卡片不值得; preserve-3d 更深层文字本就按摊平仿射近似, 同一量级)。 */
    float det = e1x * e2y - e2x * e1y;
    g->text_scale = sqrtf(fabsf(det));
}

static void push_cmd(hn_context *c, hn_cmd *cmd);

/* 跨文件入口: Lottie / 网格生成器把自己的指令并入同一条显示列表 */
void hn_paint_push(hn_context *c, const hn_cmd *cmd) {
    push_cmd(c, (hn_cmd *)cmd);
}
hn_arena *hn_context_tmp(hn_context *c) { return c->tmp; }

/* 该元素要播放的 Lottie 文件:
     hn-lottie="a.json"            显式路径
     <img src="a.json" hn-lottie>  布尔属性(值为空串), 路径取 src
   仅 "false"/"0" 视为显式关闭。 */
static const char *hn_attr_lottie_src(hn_node *n) {
    const char *v = hn_node_attr(n, "hn-lottie");
    if (!v) return NULL;
    if (!strcmp(v, "false") || !strcmp(v, "0")) return NULL;
    if (!*v || !strcmp(v, "true") || !strcmp(v, "1")) {
        const char *src = hn_node_attr(n, "src");
        return (src && *src) ? src : NULL;
    }
    return v;
}

/* 有效圆角: % 值按盒短边解析(border-radius:50% 即圆) */
static float eff_radius(const hn_style *st, float w, float h) {
    if (!st->radius_pct || st->radius <= 0) return st->radius;
    float m = w < h ? w : h;
    return m * st->radius / 100.0f;
}

/* 文本装饰线: underline / line-through / overline(随文本基线定位)。
   x/baseline 由调用方先过摊平仿射(与正文 TEXT 同一口径), ts 为文字
   等比缩放 —— 偏移与线厚跟着缩, 否则 3D 面片上的下划线会脱离文字。 */
static void push_deco(hn_context *c, const hn_style *st, float x, float baseline,
                      float w, float alpha, float sx, float sy, float ts) {
    if (!st->text_deco || w <= 0) return;
    float ys[3]; int ny = 0;
    if (st->text_deco & 1) ys[ny++] = baseline + st->font_size * 0.14f * ts;
    if (st->text_deco & 2) ys[ny++] = baseline - st->font_size * 0.30f * ts;
    if (st->text_deco & 4) ys[ny++] = baseline - st->font_size * 0.92f * ts;
    hn_color col = fill_color(st, st->color, alpha);
    for (int i = 0; i < ny; i++) {
        hn_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.kind = HN_CMD_RECT;
        cmd.x = x - sx;
        cmd.y = ys[i] - sy;
        cmd.w = w;
        cmd.h = (st->font_size > 14 ? 1.5f : 1.0f) * ts;
        cmd.fill = col;
        push_cmd(c, &cmd);
    }
}

/* 列表标记: ul → 实心圆/方/空心圆, ol → 十进制序号(右对齐于内容左缘)。
   坐标一律经 tfx/tfy(摊平单应 + 累积缩放), 尺寸/字号随 text_scale ——
   此前直用 n->bx/by 减 sx/sy, 不经变换也不随缩放: scale(2) 祖先下
   标记坐标/尺寸都不变(与已变换的正文脱节)。 */
static void paint_marker(hn_context *c, hn_node *n, const hn_style *st,
                         float alpha, float sx, float sy, const paint_guard *g) {
    hn_color col = fill_color(st, st->color, alpha);
    int ordered = !strcmp(n->parent->tag, "ol");
    if (ordered) {
        int idx = 1;
        for (hn_node *s = n->prev; s; s = s->prev)
            if (s->kind == HN_ELEM && s->tag && !strcmp(s->tag, "li")) idx++;
        char *num = hn_arena_alloc(c->tmp, 12);   /* 存活至下次 layout, 与指令列表同生命周期 */
        if (!num) return;
        snprintf(num, 12, "%d.", idx);
        hn_font_desc fd = { st->font_size, st->font_weight, st->font_italic, st->letter_spacing, st->font_family };
        float w = 0;
        if (c->tb_valid && c->tb.measure) w = c->tb.measure(c->tb.ctx, &fd, num, strlen(num));
        float px = n->bx - 7 - w, py = n->by + st->font_size * 1.02f;
        hn_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.kind = HN_CMD_TEXT;
        cmd.text = num;
        cmd.text_len = strlen(num);
        cmd.tx = tfx(g, sx, px, py);
        cmd.baseline = tfy(g, sy, px, py);
        cmd.font.size_px = st->font_size * g->text_scale;
        cmd.font.weight = st->font_weight;
        cmd.font.italic = st->font_italic;
        cmd.font.letter_spacing = st->letter_spacing;
        cmd.fill = col;
        push_cmd(c, &cmd);
        return;
    }
    /* 无序标记: 0=disc(默认) 2=square 3=circle(空心)。尺寸随 text_scale
       (2D 链缩放或摊平面片的均匀档), 锚点(盒左缘外 14px, 字号 0.45 处)
       经 tfx/tfy 变换。 */
    float msize = 5.0f * g->text_scale;
    float px = n->bx - 14, py = n->by + st->font_size * 0.45f;
    hn_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = HN_CMD_RECT;
    cmd.w = cmd.h = msize;
    cmd.x = tfx(g, sx, px, py);
    cmd.y = tfy(g, sy, px, py);
    if (st->list_style == 3) {          /* 空心圆 */
        cmd.radius = msize * 0.5f;
        cmd.stroke = col;
        cmd.stroke_w = 1;
    } else if (st->list_style == 2) {   /* 方块 */
        cmd.fill = col;
    } else {                            /* 实心圆 */
        cmd.radius = msize * 0.5f;
        cmd.fill = col;
    }
    push_cmd(c, &cmd);
}

/* :focus 缺省视觉: 聚焦控件外扩 2px 的强调色描边(与浏览器默认 outline
   同位)。主题/作者的 :focus 规则(border-color 等)照常生效, 环是叠加的
   默认指示 —— 主题里 input:focus 已改边框色, 二者并存不冲突。 */
static void paint_focus_ring(hn_context *c, hn_node *n, const hn_style *st,
                             float alpha, float sx, float sy, const paint_guard *g) {
    float w = n->bw * g->scale * g->scale_x, h = n->bh * g->scale * g->scale_y;
    if (w <= 0 || h <= 0) return;
    hn_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = HN_CMD_RECT;
    cmd.x = tfx(g, sx, n->bx, n->by) - 2;
    cmd.y = tfy(g, sy, n->bx, n->by) - 2;
    cmd.w = w + 4;
    cmd.h = h + 4;
    cmd.radius = eff_radius(st, n->bw, n->bh) * g->scale + 2;
    cmd.stroke = mul_alpha(HN_ACCENT, alpha);
    cmd.stroke_w = 2;
    push_cmd(c, &cmd);
}

/* 两点间的粗线段(四边形填充): 复选框对勾由两段构成。
   RECT 无法旋转, 任意方向的线段必须走 QUAD(纯色填充)。 */
static void push_quad_seg(hn_context *c, float x0, float y0, float x1, float y1,
                          float half, hn_color col) {
    float dx = x1 - x0, dy = y1 - y0;
    float len = sqrtf(dx * dx + dy * dy);
    if (len < 0.01f) return;
    float nx = -dy / len * half, ny = dx / len * half;
    hn_cmd q;
    memset(&q, 0, sizeof(q));
    q.kind = HN_CMD_QUAD;
    q.qx[0] = x0 + nx; q.qy[0] = y0 + ny;
    q.qx[1] = x1 + nx; q.qy[1] = y1 + ny;
    q.qx[2] = x1 - nx; q.qy[2] = y1 - ny;
    q.qx[3] = x0 - nx; q.qy[3] = y0 - ny;
    q.fill = col;
    push_cmd(c, &q);
}

/* 某一边的有效边框(宽/色): border_w4/c4 的 0 = 继承统一声明,
   宽 -1 = 显式关闭该边, 色 1 = 继承(与 hn_style 的解析口径一致)。 */
static void border_side_spec(const hn_style *st, int side,
                             float *bw, hn_color *bc) {
    *bw = st->border_w4[side] ? (st->border_w4[side] > 0 ? st->border_w4[side] : 0)
                              : st->border_w;
    *bc = st->border_c4[side] ? (st->border_c4[side] > 1 ? st->border_c4[side]
                                                         : st->border_color)
                              : st->border_color;
}

/* 3D 面片的边框: 外四角沿相邻两条边方向各内缩边宽得内四角, 每条边发一条
   梯形 QUAD。此前 3D 元素的边框仍按未变换矩形画(RECT 表达不了投影),
   面片一转边框就落在面片之外。内缩是"沿边走 w"的口径: 直角处带宽恰为
   w, 一般四边形锐角处略窄(sin θ 因子); 圆角半径不参与(梯形边为直线)。
   最短边钳制内缩量, 防细长面片上两侧内缩相遇把内四角翻转。 */
static void push_quad_border(hn_context *c, const hn_style *st,
                             const float qx[4], const float qy[4], float alpha) {
    float ws[4], ix[4], iy[4];
    hn_color cs[4];
    float emin = 1e30f;
    for (int k = 0; k < 4; k++) {
        int j = (k + 1) & 3;
        float dx = qx[j] - qx[k], dy = qy[j] - qy[k];
        float len = sqrtf(dx * dx + dy * dy);
        if (len < emin) emin = len;
    }
    float wcap = emin * 0.45f;
    for (int s = 0; s < 4; s++) {
        border_side_spec(st, s, &ws[s], &cs[s]);
        if (ws[s] > wcap) ws[s] = wcap;
    }
    for (int k = 0; k < 4; k++) {
        int p = (k + 3) & 3, nx = (k + 1) & 3;
        float ux = qx[nx] - qx[k], uy = qy[nx] - qy[k];
        float vx = qx[p] - qx[k], vy = qy[p] - qy[k];
        float ul = sqrtf(ux * ux + uy * uy), vl = sqrtf(vx * vx + vy * vy);
        if (ul < 0.0001f || vl < 0.0001f) { ix[k] = qx[k]; iy[k] = qy[k]; continue; }
        ix[k] = qx[k] + ux / ul * ws[k] + vx / vl * ws[p];
        iy[k] = qy[k] + uy / ul * ws[k] + vy / vl * ws[p];
    }
    for (int s = 0; s < 4; s++) {
        int j = (s + 1) & 3;
        if (ws[s] <= 0 || (cs[s] & 0xFFu) == 0) continue;
        hn_cmd e;
        memset(&e, 0, sizeof(e));
        e.kind = HN_CMD_QUAD;
        e.qx[0] = qx[s]; e.qy[0] = qy[s];
        e.qx[1] = qx[j]; e.qy[1] = qy[j];
        e.qx[2] = ix[j]; e.qy[2] = iy[j];
        e.qx[3] = ix[s]; e.qy[3] = iy[s];
        e.fill = fill_color(st, cs[s], alpha);
        push_cmd(c, &e);
    }
}

/* checkbox / radio 的控件绘制: 方框/圆圈 + 选中标记。
   观感链: 作者/主题样式优先(background/border/color), 缺省用引擎观感
   (选中 = 强调色填充 + 白色对勾 / 圆点; 未选中 = 灰描边空心)。 */
static void paint_check(hn_context *c, hn_node *n, const hn_style *st,
                        float alpha, float sx, float sy, const paint_guard *g) {
    float w = n->bw * g->scale * g->scale_x, h = n->bh * g->scale * g->scale_y;
    if (w <= 0 || h <= 0) return;
    int radio = (hn_node_input_kind(n) == HN_IN_RADIO);
    int checked = hn_node_is_checked(n);
    /* 盒子取 bw/bh 的较小者作正方形边长, 居中于控件盒 */
    float s = w < h ? w : h;
    float cx = tfx(g, sx, n->bx, n->by) + w * 0.5f;
    float cy = tfy(g, sy, n->bx, n->by) + h * 0.5f;
    float bx = cx - s * 0.5f, by = cy - s * 0.5f;
    hn_color track = (st->border_color & 0xFFu) ? st->border_color : 0x8A8A8AFF;
    float sw = st->border_w > 0 ? st->border_w : 1.5f;

    hn_cmd box;
    memset(&box, 0, sizeof(box));
    box.kind = HN_CMD_RECT;
    box.x = bx; box.y = by; box.w = s; box.h = s;
    box.radius = radio ? s * 0.5f : eff_radius(st, s, s);
    if (box.radius <= 0) box.radius = s * 0.18f;   /* 复选框缺省小圆角 */
    if ((st->background & 0xFFu)) box.fill = fill_color(st, st->background, alpha);
    if (checked) {
        /* 选中: 缺省强调色填充(作者声明了 background 则尊重作者),
           radio 保持描边 + 内点 */
        if (!radio && !(box.fill & 0xFFu)) box.fill = mul_alpha(HN_ACCENT, alpha);
        box.stroke = (st->border_w > 0 && (st->border_color & 0xFFu))
            ? fill_color(st, st->border_color, alpha) : 0;
        box.stroke_w = st->border_w;
    } else {
        box.stroke = fill_color(st, track, alpha);
        box.stroke_w = sw;
    }
    push_cmd(c, &box);

    if (!checked) return;
    if (radio) {
        /* 内点: 圆点半径约 1/5 边长 */
        float d = s * 0.42f;
        hn_cmd dot;
        memset(&dot, 0, sizeof(dot));
        dot.kind = HN_CMD_RECT;
        dot.x = cx - d * 0.5f; dot.y = cy - d * 0.5f;
        dot.w = dot.h = d;
        dot.radius = d * 0.5f;
        dot.fill = fill_color(st, (st->color & 0xFFu) ? st->color : HN_ACCENT, alpha);
        push_cmd(c, &dot);
        return;
    }
    /* 白色对勾: 两段线段(短升 + 长降), 线宽随尺寸 */
    hn_color mark = fill_color(st, (st->color & 0xFFu) ? st->color : 0xFFFFFFFFu, alpha);
    float lw = s * 0.13f;
    if (lw < 1.5f) lw = 1.5f;
    push_quad_seg(c, bx + s * 0.24f, by + s * 0.52f, bx + s * 0.42f, by + s * 0.70f,
                  lw * 0.5f, mark);
    push_quad_seg(c, bx + s * 0.42f, by + s * 0.70f, bx + s * 0.78f, by + s * 0.32f,
                  lw * 0.5f, mark);
}

/* 绘制某节点的全部行内片段(文本节点用父样式, 元素用自身样式) */
static void paint_runs(hn_context *c, hn_node *n, const hn_style *st,
                       float alpha, float sx, float sy, const paint_guard *g) {
    for (int i = 0; i < n->n_runs; i++) {
        hn_run *r = &n->runs[i];
        if (r->end <= r->begin) continue;
        /* 字号随等比缩放: 2D 链累积为均匀一档; 射影摊平(单应已装入)取
           **锚点处局部线尺度** —— 同一张转动面片内近大远小(透视字号),
           均匀一档给不出近远端差。锚点已走单应; 字形本身按轴对齐位图
           绘制, 不做透视畸变(简化口径见 install_flatten 注)。 */
        float ts = g->text_scale;
        if (g->fhx != 0.0f || g->fhy != 0.0f) {
            float kx, ky;
            flatten_axes_at(g, r->x, r->baseline, &kx, &ky);
            ts = sqrtf(kx * ky);
        }
        hn_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.kind = HN_CMD_TEXT;
        cmd.text = n->text + r->begin;
        cmd.text_len = r->end - r->begin;
        cmd.tx = tfx(g, sx, r->x, r->baseline);
        cmd.baseline = tfy(g, sy, r->x, r->baseline);
        cmd.font.size_px = st->font_size * ts;
        cmd.font.weight = st->font_weight;
        cmd.font.italic = st->font_italic;
        cmd.font.letter_spacing = st->letter_spacing;
        cmd.fill = fill_color(st, st->color, alpha);
        /* text-shadow: 借用 RECT 的阴影字段(结构里是共享的)。
           绘制端按"先画阴影偏移版、再画正文"两遍处理。 */
        if (st->text_shadow) {
            cmd.shadow = 1;
            cmd.shadow_color = fill_color(st, st->text_shadow_color, alpha);
            cmd.shadow_blur = st->text_shadow_blur;
            cmd.shadow_ox = st->text_shadow_ox;
            cmd.shadow_oy = st->text_shadow_oy;
        }
        push_cmd(c, &cmd);
        /* 装饰线与正文同一单应(先变换再传 0 偏移), 宽/厚随锚点处字号档 */
        push_deco(c, st, tfx(g, sx, r->x, r->baseline), tfy(g, sy, r->x, r->baseline),
                  r->width * ts, alpha, 0.0f, 0.0f, ts);
    }
}

static void push_cmd(hn_context *c, hn_cmd *cmd) {
    if (c->n_cmds == c->cap_cmds) {
        int nc = c->cap_cmds ? c->cap_cmds * 2 : 64;
        hn_cmd *nv = realloc(c->cmds, sizeof(hn_cmd) * (size_t)nc);
        if (!nv) abort();
        c->cmds = nv;
        c->cap_cmds = nc;
    }
    c->cmds[c->n_cmds++] = *cmd;
}

/* ---------------- <canvas>: 2D 账本重放 ----------------
 * 绘制 canvas 元素 = 自身盒背景 → CLIP_PUSH 元素盒 → 逐条把账本 op 翻译成
 * 显示列表指令 → CLIP_POP。账本 op 的坐标是画布属性坐标系(width/height
 * 属性, JS 侧 CTM 已在记录前应用), 这里按 盒/属性 的比例映射到元素盒 ——
 * 属性尺寸 = 盒尺寸时(最常见)是 1:1 平移。账本属于元素生命周期
 * (hn_canvas.c), 样式重算/重放不影响它; clearRect = 清账本。 */

/* 折线描边: 每段展开成垂直四边形(QUAD)。开放折线不闭合 —— 引擎的
   POLYGON 描边总是闭合路径, 用 QUAD 逐段展开才能表达 canvas 的开放路径。 */
static void canvas_stroke_polyline(hn_context *c, const float *pts, int n_pts,
                                   hn_color col, float hw, float ox, float oy,
                                   float kx, float ky, float alpha) {
    if (hw <= 0.05f || (col & 0xFFu) == 0) return;
    hn_color fc = mul_alpha(col, alpha);
    for (int i = 0; i + 1 < n_pts; i++) {
        float x1 = ox + pts[i * 2] * kx,         y1 = oy + pts[i * 2 + 1] * ky;
        float x2 = ox + pts[(i + 1) * 2] * kx,   y2 = oy + pts[(i + 1) * 2 + 1] * ky;
        float ex = x2 - x1, ey = y2 - y1;
        float len = sqrtf(ex * ex + ey * ey);
        if (len < 1e-5f) continue;
        float nx = -ey / len * hw, ny = ex / len * hw;
        hn_cmd q;
        memset(&q, 0, sizeof(q));
        q.kind = HN_CMD_QUAD;
        q.qx[0] = x1 + nx; q.qy[0] = y1 + ny;
        q.qx[1] = x2 + nx; q.qy[1] = y2 + ny;
        q.qx[2] = x2 - nx; q.qy[2] = y2 - ny;
        q.qx[3] = x1 - nx; q.qy[3] = y1 - ny;
        q.fill = fc;
        push_cmd(c, &q);
    }
}

/* 圆头端点/圆滑交点: 顶点处补一枚 16 边形圆片(廉价矢量渲染器的标准做法) */
static void canvas_cap_dot(hn_context *c, float cx, float cy, float r,
                           hn_color col, float alpha) {
    if (r <= 0.05f) return;
    const int SEG = 16;
    float *poly = (float *)hn_arena_alloc(c->tmp, sizeof(float) * SEG * 2);
    if (!poly) return;
    for (int i = 0; i < SEG; i++) {
        float a = (float)i * 6.2831853f / SEG;
        poly[i * 2] = cx + cosf(a) * r;
        poly[i * 2 + 1] = cy + sinf(a) * r;
    }
    hn_cmd p;
    memset(&p, 0, sizeof(p));
    p.kind = HN_CMD_POLYGON;
    p.poly = poly;
    p.poly_n = SEG;
    p.fill = mul_alpha(col, alpha);
    push_cmd(c, &p);
}

static void paint_canvas(hn_context *c, hn_node *n, const hn_style *st,
                         float alpha, float sx, float sy, const paint_guard *g) {
    /* 元素盒背景(canvas 是替换元素: 盒背景仍按样式画, 账本内容叠在其上) */
    int has_fill = (st->background & 0xFFu) || st->has_gradient;
    int has_border = st->border_w > 0 && (st->border_color & 0xFFu);
    float box_w = n->bw * g->scale * g->scale_x;
    float box_h = n->bh * g->scale * g->scale_y;
    if (has_fill || has_border) {
        hn_cmd bg;
        memset(&bg, 0, sizeof(bg));
        bg.kind = HN_CMD_RECT;
        bg.x = tfx(g, sx, n->bx, n->by);
        bg.y = tfy(g, sy, n->bx, n->by);
        bg.w = box_w;
        bg.h = box_h;
        bg.radius = eff_radius(st, bg.w, bg.h);
        bg.fill = fill_color(st, st->background, alpha);
        bg.stroke = fill_color(st, st->border_color, alpha);
        bg.stroke_w = st->border_w;
        if (st->has_gradient) {
            bg.gradient = 1;
            bg.grad_from = fill_color(st, st->grad_from, alpha);
            bg.grad_to = fill_color(st, st->grad_to, alpha);
            bg.grad_angle = st->grad_angle;
        }
        push_cmd(c, &bg);
    }

    const char *id = hn_node_attr(n, "id");
    hn_canvas_ledger *l = id ? hn_canvas_ledger_find(c, id) : NULL;
    if (!l || l->n_ops == 0) return;   /* 无账本 = 空白画布(与浏览器一致) */

    /* 画布属性坐标 → 元素盒: 属性 width/height 定义坐标空间, 缺省 300x150 */
    const char *aw = hn_node_attr(n, "width");
    const char *ah = hn_node_attr(n, "height");
    float cw = aw && *aw ? (float)atof(aw) : 300.0f;
    float ch = ah && *ah ? (float)atof(ah) : 150.0f;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    float kx = box_w / cw, ky = box_h / ch;
    float ox = tfx(g, sx, n->bx, n->by);
    float oy = tfy(g, sy, n->bx, n->by);
    /* 线宽/字号等"长度"按两轴均档(非等比缩放时各向误差最小) */
    float ks = (kx + ky) * 0.5f;

    hn_cmd cp;
    memset(&cp, 0, sizeof(cp));
    cp.kind = HN_CMD_CLIP_PUSH;
    cp.x = ox;
    cp.y = oy;
    cp.w = box_w;
    cp.h = box_h;
    cp.radius = eff_radius(st, box_w, box_h);
    push_cmd(c, &cp);

    for (int i = 0; i < l->n_ops; i++) {
        const hn_canvas_op *o = &l->ops[i];
        hn_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        switch (o->kind) {
        case HN_CV_FILL_RECT:
            cmd.kind = HN_CMD_RECT;
            cmd.x = ox + o->x * kx;
            cmd.y = oy + o->y * ky;
            cmd.w = o->w * kx;
            cmd.h = o->h * ky;
            cmd.fill = mul_alpha(o->color, alpha);
            if ((cmd.fill & 0xFFu) == 0 || cmd.w <= 0 || cmd.h <= 0) continue;
            push_cmd(c, &cmd);
            break;
        case HN_CV_STROKE_PATH: {
            float hw = o->stroke_w * ks * 0.5f;
            canvas_stroke_polyline(c, o->pts, o->n_pts, o->color, hw,
                                   ox, oy, kx, ky, alpha);
            if (o->cap_round) {
                for (int p2 = 0; p2 < o->n_pts; p2++)
                    canvas_cap_dot(c, ox + o->pts[p2 * 2] * kx,
                                   oy + o->pts[p2 * 2 + 1] * ky, hw,
                                   o->color, alpha);
            }
            break;
        }
        case HN_CV_FILL_PATH: {
            if (o->n_pts < 3 || (o->color & 0xFFu) == 0) break;
            float *poly = (float *)hn_arena_alloc(c->tmp, sizeof(float) * (size_t)o->n_pts * 2);
            if (!poly) break;
            for (int p2 = 0; p2 < o->n_pts; p2++) {
                poly[p2 * 2] = ox + o->pts[p2 * 2] * kx;
                poly[p2 * 2 + 1] = oy + o->pts[p2 * 2 + 1] * ky;
            }
            cmd.kind = HN_CMD_POLYGON;
            cmd.poly = poly;
            cmd.poly_n = o->n_pts;
            cmd.fill = mul_alpha(o->color, alpha);
            push_cmd(c, &cmd);
            break;
        }
        case HN_CV_GRAD_RECT: {
            cmd.kind = HN_CMD_RECT;
            cmd.x = ox + o->x * kx;
            cmd.y = oy + o->y * ky;
            cmd.w = o->w * kx;
            cmd.h = o->h * ky;
            cmd.fill = mul_alpha(o->color, alpha);
            cmd.gradient = 1;
            cmd.grad_from = mul_alpha(o->color, alpha);
            cmd.grad_to = mul_alpha(o->color2, alpha);
            /* canvas 渐变轴 (x0,y0)→(x1,y1) → 引擎的 CSS 角度口径
               (0deg=向上, 方向 = (sinθ, -cosθ)): θ = atan2(dx, -dy) */
            float gdx = (o->gx1 - o->gx0) * kx, gdy = (o->gy1 - o->gy0) * ky;
            cmd.grad_angle = atan2f(gdx, -gdy) * (180.0f / 3.14159265f);
            push_cmd(c, &cmd);
            break;
        }
        case HN_CV_CLIP_PUSH:
            cmd.kind = HN_CMD_CLIP_PUSH;
            cmd.x = ox + o->x * kx;
            cmd.y = oy + o->y * ky;
            cmd.w = o->w * kx;
            cmd.h = o->h * ky;
            push_cmd(c, &cmd);
            break;
        case HN_CV_CLIP_POP:
            cmd.kind = HN_CMD_CLIP_POP;
            push_cmd(c, &cmd);
            break;
        case HN_CV_TEXT: {
            if ((o->color & 0xFFu) == 0 || !o->str) break;
            /* 文本串复制进 tmp 区(生命周期 = 显示列表) */
            size_t sl = strlen(o->str);
            char *ts = (char *)hn_arena_alloc(c->tmp, sl + 1);
            if (!ts) break;
            memcpy(ts, o->str, sl + 1);
            cmd.kind = HN_CMD_TEXT;
            cmd.text = ts;
            cmd.text_len = sl;
            cmd.tx = ox + o->x * kx;
            cmd.baseline = oy + o->y * ky;
            cmd.font.size_px = o->font_px * ky;
            cmd.font.weight = st->font_weight;
            cmd.font.italic = st->font_italic;
            cmd.font.letter_spacing = st->letter_spacing;
            cmd.font.family = st->font_family;
            cmd.fill = mul_alpha(o->color, alpha);
            push_cmd(c, &cmd);
            break;
        }
        case HN_CV_IMAGE: {
            if (!o->str || !o->str[0]) break;
            size_t sl = strlen(o->str);
            char *ts = (char *)hn_arena_alloc(c->tmp, sl + 1);
            if (!ts) break;
            memcpy(ts, o->str, sl + 1);
            cmd.kind = HN_CMD_IMAGE;
            cmd.text = ts;
            cmd.text_len = sl;
            cmd.x = ox + o->x * kx;
            cmd.y = oy + o->y * ky;
            cmd.w = o->w * kx;
            cmd.h = o->h * ky;
            push_cmd(c, &cmd);
            break;
        }
        }
    }

    hn_cmd cpo;
    memset(&cpo, 0, sizeof(cpo));
    cpo.kind = HN_CMD_CLIP_POP;
    push_cmd(c, &cpo);
}

/* 防御性保护: DOM 结构异常(纵向超深嵌套 / 横向兄弟链成环)时只截断绘制,
   绝不让渲染器崩溃或死循环。正常文档远达不到这些上限。
   两个维度都要防: 只看深度防不了兄弟链成环(那是同层无限循环)。 */
#define HN_PAINT_MAX_DEPTH 256
#define HN_PAINT_MAX_NODES 200000

static void paint_walk_g(hn_context *c, hn_node *n, const hn_style *pst,
                         float alpha, float sx, float sy, paint_guard *g);

static void paint_walk(hn_context *c, hn_node *n, const hn_style *pst,
                       float alpha, float sx, float sy) {
    paint_guard g;
    memset(&g, 0, sizeof(g));
    g.scale = 1.0f; g.scale_x = 1.0f; g.scale_y = 1.0f;   /* 累积缩放缺省 1 */
    g.text_scale = 1.0f;       /* 文字等比缩放缺省 1(3D 摊平时改写, 见下) */
    mat_identity(g.m3d);       /* 3D 复合矩阵缺省单位阵(正交, 无祖先变换) */
    g.axx = 1.0f; g.ayy = 1.0f;   /* 摊平仿射缺省单位阵 */
    g.fhx = 0.0f; g.fhy = 0.0f; g.fhw = 1.0f;   /* 射影分母缺省 1(仿射) */
    paint_walk_g(c, n, pst, alpha, sx, sy, &g);
}

/* 是否存在需要排序的定位子节点(避免常规路径付出排序成本) */
static int has_positioned_children(hn_node *n) {
    for (hn_node *ch = n->first; ch; ch = ch->next)
        if (ch->kind == HN_ELEM && ch->style.position != HN_POS_STATIC) return 1;
    return 0;
}

/* 排序键: positioned 元素按 z-index 升序排在非 positioned 之后 */
static int zkey(const hn_node *ch) {
    if (ch->kind != HN_ELEM || ch->style.position == HN_POS_STATIC) return 0;
    /* z-index<=0 的定位元素仍应在流内容之上(与 CSS 一致: 定位元素形成层),
       这里映射到 1, 真正的正 z-index 归一到 2..N */
    return ch->style.z_index > 0 ? ch->style.z_index + 1 : 1;
}

/* 按 z 序绘制子节点(插入排序 + 稳定: 同键保持文档顺序) */
static void paint_children_sorted(hn_context *c, hn_node *n, const hn_style *st,
                                  float alpha, float sx, float sy, paint_guard *g) {
    hn_node *list[256];
    int cnt = 0;
    for (hn_node *ch = n->first; ch && cnt < 256; ch = ch->next) list[cnt++] = ch;
    /* 稳定插入排序(键 0 = 非定位, 保持原位; 仅定位元素按 z 上浮) */
    for (int i = 1; i < cnt; i++) {
        hn_node *key = list[i];
        int kk = zkey(key);
        if (kk == 0) continue;                 /* 非定位元素不移动 */
        int j = i - 1;
        while (j >= 0 && zkey(list[j]) > kk) { list[j + 1] = list[j]; j--; }
        list[j + 1] = key;
    }
    for (int i = 0; i < cnt; i++) {
        if (g->nodes > HN_PAINT_MAX_NODES || g->depth > HN_PAINT_MAX_DEPTH) break;
        g->depth++;
        paint_walk_g(c, list[i], st, alpha, sx, sy, g);
        g->depth--;
    }
}

static void paint_walk_g(hn_context *c, hn_node *n, const hn_style *pst,
                         float alpha, float sx, float sy, paint_guard *g) {
    if (alpha <= 0.001f) return;
    if (g->depth > HN_PAINT_MAX_DEPTH) return;      /* 纵向异常 */
    if (++g->nodes > HN_PAINT_MAX_NODES) return;    /* 横向环/规模异常 */
    int clipped = 0;   /* 见下方 goto 处的说明: 必须在此初始化 */
    int cp_clip = 0;   /* clip-path 裁剪是否已入栈(所有提前 return 都要补 POP) */

    if (n->kind == HN_TEXT) {
        paint_runs(c, n, pst, alpha, sx, sy, g);
        return;
    }

    if (n->style.display == HN_DISP_NONE) return;
    const hn_style *st = &n->style;

    /* 动画变换: 位移平移整棵子树(精确); 缩放换以元素盒中心为原点(几何换算)。
       两者都由动画插值写入样式字段, 这里只读消费。 */
    float saved_sx = sx, saved_sy = sy;
    float saved_scale = g->scale, saved_ox = g->ox, saved_oy = g->oy;
    float saved_scale_x = g->scale_x, saved_scale_y = g->scale_y;
    float saved_text_scale = g->text_scale;
    /* 3D 层级状态: 出口恢复(兄弟节点不受本元素影响) */
    float saved_m3d[4][4], saved_axx = g->axx, saved_axy = g->axy, saved_atx = g->atx;
    float saved_ayx = g->ayx, saved_ayy = g->ayy, saved_aty = g->aty;
    float saved_fhx = g->fhx, saved_fhy = g->fhy, saved_fhw = g->fhw;
    float saved_pd = g->persp_d, saved_pcx = g->persp_cx, saved_pcy = g->persp_cy;
    memcpy(saved_m3d, g->m3d, sizeof(saved_m3d));
    /* 透视视距: 元素自身的 perspective, 否则取**最近的**祖先声明
       (绘制状态沿树携带, 不再局限于直接父级 —— 与 CSS"用最近的
       perspective"一致)。投影中心(消失点)跟着视距的**声明者**走:
       CSS perspective-origin 缺省 50% 50% —— 自己声明就是自己盒中心,
       祖先声明就是声明者的盒中心。 */
    float persp = st->perspective;
    float pcx = n->bx + n->bw * 0.5f, pcy = n->by + n->bh * 0.5f;
    if (persp <= 0 && g->persp_d > 0) {
        persp = g->persp_d;
        pcx = g->persp_cx; pcy = g->persp_cy;
    }
    if (st->translate_x != 0) sx -= st->translate_x;
    if (st->translate_y != 0) sy -= st->translate_y;
    /* ---- 缩放 + 缩放原点 ----
       transform-origin 之前完全没实现: 所有 rotate/scale 都以**盒中心**为原点。
       想绕左上角转(铰链/表盘)只能靠 translate 硬凑 —— 凑出来的角度还不对。
       origin 的 % 在此处才能解出(要盒尺寸), 所以解析期只存分数。
       注意 origin 只在**有缩放或旋转**时才需要, 但设了也要生效(即使只有
       rotate —— 旋转同样围绕它)。 */
    float ox_want = 0.0f, oy_want = 0.0f;
    int want_origin = (st->scale != 1.0f && st->scale > 0.01f)
                   || st->scale_x != 1.0f || st->scale_y != 1.0f;
    if (st->has_origin && want_origin) {
        ox_want = st->origin_pct_x ? st->origin_x * n->bw : st->origin_x;
        oy_want = st->origin_pct_y ? st->origin_y * n->bh : st->origin_y;
        g->ox = n->bx + ox_want;
        g->oy = n->by + oy_want;
    } else if (want_origin) {
        /* 缺省 = 盒中心(50% 50%), 与 CSS 一致 */
        g->ox = n->bx + n->bw * 0.5f;
        g->oy = n->by + n->bh * 0.5f;
    }
    if (st->scale != 1.0f && st->scale > 0.01f) {
        g->scale = saved_scale * st->scale;
        g->text_scale = saved_text_scale * st->scale;   /* 字号跟着等比缩放 */
    }
    if (st->scale_x != 1.0f) g->scale_x = saved_scale_x * st->scale_x;
    if (st->scale_y != 1.0f) g->scale_y = saved_scale_y * st->scale_y;

    /* ---- clip-path(circle/inset 简化版) ----
       在自身背景/标记/子树绘制之前入栈, 元素全部产出都被裁剪(与 CSS 一致)。
       circle 借用 CLIP_PUSH 的圆角矩形: 边长 = 2r 的方盒 + radius = r,
       圆角后端(cairo/CoreGraphics)的路径即圆; hnsoft 帧缓冲按 SDF 精确裁剪。
       退化(半径 0 / inset 掏空)时零面积裁剪 = 整体不可见, 与 CSS 一致。 */
    if (st->clip_shape) {
        hn_cmd cp;
        memset(&cp, 0, sizeof(cp));
        cp.kind = HN_CMD_CLIP_PUSH;
        if (st->clip_shape == 1) {
            float ccx = n->bx + n->bw * 0.5f, ccy = n->by + n->bh * 0.5f;
            float r;
            if (st->clip_pct & 16) {
                /* 百分比半径: CSS 相对 sqrt(w²+h²)/√2 */
                r = st->clip_a * 0.01f
                  * sqrtf(n->bw * n->bw + n->bh * n->bh) / 1.4142136f;
            } else if (st->clip_a > 0) {
                r = st->clip_a;
            } else {
                r = (n->bw < n->bh ? n->bw : n->bh) * 0.5f;   /* closest-side */
            }
            if (r < 0) r = 0;
            cp.x = ccx - r - sx;
            cp.y = ccy - r - sy;
            cp.w = cp.h = r * 2.0f;
            cp.radius = r;
        } else {
            float t = st->clip_a, rr = st->clip_b, bb = st->clip_c, ll = st->clip_d;
            /* 百分比: top/bottom 相对盒高, left/right 相对盒宽(CSS 口径) */
            if (st->clip_pct & 1) t *= n->bh * 0.01f;
            if (st->clip_pct & 2) rr *= n->bw * 0.01f;
            if (st->clip_pct & 4) bb *= n->bh * 0.01f;
            if (st->clip_pct & 8) ll *= n->bw * 0.01f;
            float rad = st->clip_round;
            if (st->clip_pct & 32) rad *= (n->bw < n->bh ? n->bw : n->bh) * 0.01f;
            if (rad < 0) rad = 0;
            cp.x = n->bx + ll - sx;
            cp.y = n->by + t - sy;
            cp.w = n->bw - ll - rr;
            cp.h = n->bh - t - bb;
            if (cp.w < 0) cp.w = 0;
            if (cp.h < 0) cp.h = 0;
            cp.radius = rad;
        }
        cp_clip = 1;
        push_cmd(c, &cp);
    }

    /* 列表标记: li 且父为 ul/ol(背景之上、内容之左) */
    if (n->tag && !strcmp(n->tag, "li") && n->parent && n->parent->tag
        && st->list_style != 1
        && (!strcmp(n->parent->tag, "ul") || !strcmp(n->parent->tag, "ol"))) {
        paint_marker(c, n, st, alpha, sx, sy, g);
    }

    /* Lottie 矢量动画: 声明了 hn-lottie 的元素整体由动画接管(覆盖盒背景与子节点)。
       解析结果按路径缓存, 时钟取自节点的 ext_clock(由动画 tick 推进)。 */
    if (n->tag) {
        const char *lsrc = hn_attr_lottie_src(n);
        if (lsrc) {
            struct hn_lottie *l = hn_context_lottie(c, lsrc);
            if (l) {
                float bw = n->bw * g->scale * g->scale_x, bh = n->bh * g->scale * g->scale_y;
                float bx = tfx(g, sx, n->bx, n->by), by = tfy(g, sy, n->bx, n->by);
                /* 元素盒背景仍要画(承载底色/圆角), 动画画在其上 */
                int lf = (st->background & 0xFFu) || st->has_gradient;
                if (lf) {
                    hn_cmd bg;
                    memset(&bg, 0, sizeof(bg));
                    bg.kind = HN_CMD_RECT;
                    bg.x = bx; bg.y = by; bg.w = bw; bg.h = bh;
                    bg.radius = eff_radius(st, bw, bh);
                    bg.fill = fill_color(st, st->background, alpha);
                    if (st->has_gradient) {
                        bg.gradient = 1;
                        bg.grad_from = fill_color(st, st->grad_from, alpha);
                        bg.grad_to = fill_color(st, st->grad_to, alpha);
                        bg.grad_angle = st->grad_angle;
                    }
                    push_cmd(c, &bg);
                }
                float speed = 1.0f;
                const char *sp = hn_node_attr(n, "hn-lottie-speed");
                if (sp && *sp) { speed = (float)atof(sp); if (speed <= 0.01f) speed = 1.0f; }
                const char *fit = hn_node_attr(n, "hn-lottie-fit");
                hn_lottie_emit(c, l, n->ext_clock * speed, bw, bh, bx, by,
                               alpha * st->opacity, fit);
                goto clip_done;   /* clip-path 已入栈, 出口统一补 POP */
            }
            /* 解析失败: 落到常规 img 分支(显示占位), 让问题可见而非静默空白 */
        }
    }

    /* 网格变形贴图(Live2D 类效果原语): hn-mesh="cols x rows" 或脚本写入顶点 */
    if (n->tag && (hn_node_attr(n, "hn-mesh") || n->mesh_verts)) {
        const char *msrc = hn_node_attr(n, "src");
        if (msrc && *msrc) {
            hn_mesh_emit(c, n, msrc, n->bw * g->scale * g->scale_x, n->bh * g->scale * g->scale_y,
                         tfx(g, sx, n->bx, n->by), tfy(g, sy, n->bx, n->by), alpha);
            goto clip_done;
        }
    }

    /* 媒体元素(<video>/<audio>): 每次 repaint 对会话轮询一次当前帧(契约 §5)。
       有帧 → HN_CMD_BITMAP(几何 = 元素盒, 像素源 = 会话双缓冲缓存);
       无帧(LOADING/无宿主/open 失败/纯音频) → 深底占位 RECT, 探针可见
       而非静默空白。audio 永不发 BITMAP。 */
    if (n->tag && (!strcmp(n->tag, "video") || !strcmp(n->tag, "audio"))) {
        if (n->tag[0] == 'v') {
            const unsigned char *bm = NULL;
            int bmw = 0, bmh = 0, bstride = 0;
            double bpts = 0;
            if (hn_media_poll_frame(c, n, &bm, &bmw, &bmh, &bstride, &bpts)) {
                hn_cmd cmd;
                memset(&cmd, 0, sizeof(cmd));
                cmd.kind = HN_CMD_BITMAP;
                cmd.x = tfx(g, sx, n->bx, n->by); cmd.y = tfy(g, sy, n->bx, n->by);
                cmd.w = n->bw * g->scale * g->scale_x;
                cmd.h = n->bh * g->scale * g->scale_y;
                cmd.radius = eff_radius(st, cmd.w, cmd.h);
                cmd.bitmap = bm;
                cmd.bitmap_stride = bstride;
                cmd.bitmap_w = bmw;
                cmd.bitmap_h = bmh;
                cmd.bitmap_pts = bpts;
                push_cmd(c, &cmd);
            } else {
                hn_cmd cmd;
                memset(&cmd, 0, sizeof(cmd));
                cmd.kind = HN_CMD_RECT;
                cmd.x = tfx(g, sx, n->bx, n->by); cmd.y = tfy(g, sy, n->bx, n->by);
                cmd.w = n->bw * g->scale * g->scale_x;
                cmd.h = n->bh * g->scale * g->scale_y;
                cmd.radius = eff_radius(st, cmd.w, cmd.h);
                cmd.fill = fill_color(st, 0x14171EFFu, alpha);   /* 深底 */
                cmd.stroke = fill_color(st, 0x3A4150FFu, alpha); /* 边框 */
                cmd.stroke_w = 1.0f;
                push_cmd(c, &cmd);
            }
        }
        goto clip_done;
    }

    /* 图片元素 */
    if (n->tag && !strcmp(n->tag, "img")) {
        const char *src = hn_node_attr(n, "src");
        if (src) {
            hn_cmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.kind = HN_CMD_IMAGE;
            cmd.x = tfx(g, sx, n->bx, n->by); cmd.y = tfy(g, sy, n->bx, n->by);
            cmd.w = n->bw * g->scale * g->scale_x; cmd.h = n->bh * g->scale * g->scale_y;
            cmd.radius = eff_radius(st, cmd.w, cmd.h);
            cmd.text = src;
            cmd.text_len = strlen(src);
            push_cmd(c, &cmd);
        }
        goto clip_done;
    }

    /* <canvas>: 2D 账本重放(替换元素 — 不画子节点, fallback 内容不显示) */
    if (n->tag && !strcmp(n->tag, "canvas")) {
        paint_canvas(c, n, st, alpha, sx, sy, g);
        goto clip_done;
    }

    /* 输入控件: 盒(背景/边框/圆角) + 值文本 + 插入符 */
    if (hn_node_is_input(n)) {
        int ik = hn_node_input_kind(n);
        if (ik == HN_IN_CHECKBOX || ik == HN_IN_RADIO) {
            /* 复选/单选: 控件本体绘制(方框/圆圈 + 选中标记) */
            paint_check(c, n, st, alpha, sx, sy, g);
            if (c->focus_node == n)
                paint_focus_ring(c, n, st, alpha, sx, sy, g);
            goto clip_done;
        }
        int f = (st->background & 0xFFu) || st->has_gradient;
        int b = st->border_w > 0 && (st->border_color & 0xFFu);
        if (f || b) {
            hn_cmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.kind = HN_CMD_RECT;
            cmd.x = tfx(g, sx, n->bx, n->by); cmd.y = tfy(g, sy, n->bx, n->by); cmd.w = n->bw * g->scale * g->scale_x; cmd.h = n->bh * g->scale * g->scale_y;
            cmd.radius = eff_radius(st, cmd.w, cmd.h);
            cmd.fill = fill_color(st, st->background, alpha);
            cmd.stroke = fill_color(st, st->border_color, alpha);
            cmd.stroke_w = st->border_w;
            if (st->has_shadow) {
                cmd.shadow = 1;
                cmd.shadow_color = fill_color(st, st->sh_color, alpha);
                cmd.shadow_blur = st->sh_blur;
                cmd.shadow_ox = st->sh_ox;
                cmd.shadow_oy = st->sh_oy;
            }
            push_cmd(c, &cmd);
        }
        /* :focus 缺省焦点环(主题/作者 :focus 规则叠加其上) */
        if (c->focus_node == n)
            paint_focus_ring(c, n, st, alpha, sx, sy, g);
        size_t vlen = 0;
        const char *val = hn_node_value(n, &vlen);
        int is_ph = (!val || vlen == 0);
        if (is_ph) {
            const char *ph = hn_node_attr(n, "placeholder");
            if (ph && *ph) { val = ph; vlen = strlen(ph); }
        }
        int is_pw = (ik == HN_IN_PASSWORD);
        /* 值文本用 run(由 layout_input 产出); placeholder 无 run 时现场测量绘制 */
        if (!is_ph && is_pw && vlen > 0) {
            /* 密码掩码: 每个码点替换为 •(值本身保持原文, 表单提交不受影响)。
               run 的几何按原文测得, 只借用首 run 的起点/基线。 */
            size_t cps = 0;
            for (size_t k = 0; k < vlen; k++)
                if (((unsigned char)val[k] & 0xC0) != 0x80) cps++;
            char *dots = cps ? hn_arena_alloc(c->tmp, cps * 3 + 1) : NULL;
            if (dots) {
                for (size_t k = 0; k < cps; k++) memcpy(dots + k * 3, "\xE2\x80\xA2", 3);
                dots[cps * 3] = 0;
                float x0 = n->n_runs > 0 ? n->runs[0].x : n->bx + st->padding[3];
                float bl = n->n_runs > 0 ? n->runs[0].baseline : 0;
                if (n->n_runs == 0) {
                    float a = st->font_size * 0.8f, d = st->font_size * 0.2f, l = 0;
                    if (c->tb_valid && c->tb.metrics) {
                        hn_font_desc fd = { st->font_size, st->font_weight, st->font_italic, st->letter_spacing, st->font_family };
                        c->tb.metrics(c->tb.ctx, &fd, &a, &d, &l);
                    }
                    bl = n->by + st->padding[0]
                       + (st->font_size * st->line_height - (a + d)) * 0.5f + a;
                }
                hn_cmd cmd;
                memset(&cmd, 0, sizeof(cmd));
                cmd.kind = HN_CMD_TEXT;
                cmd.text = dots;
                cmd.text_len = cps * 3;
                cmd.tx = tfx(g, sx, x0, bl);
                cmd.baseline = tfy(g, sy, x0, bl);
                cmd.font.size_px = st->font_size;
                cmd.font.weight = st->font_weight;
                cmd.font.italic = st->font_italic;
                cmd.font.letter_spacing = st->letter_spacing;
                cmd.fill = fill_color(st, st->color, alpha);
                push_cmd(c, &cmd);
            }
        } else if (!is_ph && n->n_runs > 0) {
            for (int i = 0; i < n->n_runs; i++) {
                hn_run *r = &n->runs[i];
                if (r->end <= r->begin || (size_t)r->end > vlen) continue;
                hn_cmd cmd;
                memset(&cmd, 0, sizeof(cmd));
                cmd.kind = HN_CMD_TEXT;
                cmd.text = val + r->begin;
                cmd.text_len = r->end - r->begin;
                cmd.tx = tfx(g, sx, r->x, r->baseline);
                cmd.baseline = tfy(g, sy, r->x, r->baseline);
                /* 与 paint_runs 同口径: 字号随等比缩放(含摊平仿射的均匀等效) */
                cmd.font.size_px = st->font_size * g->text_scale;
                cmd.font.weight = st->font_weight;
                cmd.font.italic = st->font_italic;
                cmd.font.letter_spacing = st->letter_spacing;
                cmd.fill = fill_color(st, st->color, alpha);
                push_cmd(c, &cmd);
            }
        } else if (is_ph && val && vlen) {
            hn_cmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.kind = HN_CMD_TEXT;
            cmd.text = val;
            cmd.text_len = vlen;
            float a = st->font_size * 0.8f, d = st->font_size * 0.2f, l = 0;
            if (c->tb_valid && c->tb.metrics) {
                hn_font_desc fd = { st->font_size, st->font_weight, st->font_italic, st->letter_spacing, st->font_family };
                c->tb.metrics(c->tb.ctx, &fd, &a, &d, &l);
            }
            cmd.tx = n->bx + st->padding[3] - sx;
            cmd.baseline = n->by + st->padding[0] + (st->font_size * st->line_height - (a + d)) * 0.5f + a - sy;
            cmd.font.size_px = st->font_size;
            cmd.font.weight = st->font_weight;
            cmd.font.italic = st->font_italic;
            cmd.font.letter_spacing = st->letter_spacing;
            hn_color dim = st->color;
            cmd.fill = fill_color(st, dim, alpha * 0.45f);
            push_cmd(c, &cmd);
        }
        /* 插入符: 聚焦且有值状态时(x/y 取 caret 所在 run 的行位置) */
        if (c->focus_node == n && c->caret_on) {
            float cx = n->bx + st->padding[3] - sx;
            float cy = n->by + st->padding[0] - sy + 1;
            if (is_pw) {
                /* 掩码态的 caret: 按 caret 前的码点数 × 掩码符步进定位
                   (run 的几何是原文的, 直接用会落在错误位置) */
                size_t pre = 0;
                for (size_t k = 0; k < (size_t)n->caret && k < vlen; k++)
                    if (((unsigned char)val[k] & 0xC0) != 0x80) pre++;
                float step = st->font_size * 0.55f + (st->letter_spacing > 0 ? st->letter_spacing : 0);
                if (c->tb_valid && c->tb.measure) {
                    hn_font_desc fd = { st->font_size, st->font_weight, st->font_italic, st->letter_spacing, st->font_family };
                    step = c->tb.measure(c->tb.ctx, &fd, "\xE2\x80\xA2", 3)
                         + (st->letter_spacing > 0 ? st->letter_spacing : 0);
                }
                float x0 = n->n_runs > 0 ? n->runs[0].x : n->bx + st->padding[3];
                cx = x0 + step * (float)pre - sx;
                if (n->n_runs > 0) cy = n->runs[0].y_top - sy + 1;
            } else if (n->n_runs > 0) {
                /* 找到 caret 所在 run, 累加其前段宽度; 多行值跟随所在行盒 */
                for (int i = 0; i < n->n_runs; i++) {
                    hn_run *r = &n->runs[i];
                    if ((int)r->begin <= n->caret && (int)r->end >= n->caret) {
                        float frac = (r->end > r->begin)
                            ? (float)(n->caret - (int)r->begin) / (float)(r->end - r->begin) : 0;
                        cx = r->x + r->width * frac - sx;
                        if (r->h > 0) cy = r->y_top - sy + 1;
                        break;
                    }
                }
            }
            hn_cmd cc;
            memset(&cc, 0, sizeof(cc));
            cc.kind = HN_CMD_RECT;
            cc.x = cx;
            cc.y = cy;
            cc.w = 1.5f;
            cc.h = st->font_size * 1.15f;
            cc.fill = fill_color(st, st->color, alpha);
            push_cmd(c, &cc);
        }
        goto clip_done;
    }

    /* 行内元素: 先画自身盒(背景/边框), 再画自己的文本片段 */
    if (st->display == HN_DISP_INLINE && n->n_runs > 0) {
        int f = (st->background & 0xFFu) || st->has_gradient;
        int b = st->border_w > 0 && (st->border_color & 0xFFu);
        if (f || b) {
            hn_cmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.kind = HN_CMD_RECT;
            cmd.x = tfx(g, sx, n->bx, n->by); cmd.y = tfy(g, sy, n->bx, n->by); cmd.w = n->bw * g->scale * g->scale_x; cmd.h = n->bh * g->scale * g->scale_y;
            cmd.radius = st->radius;
            cmd.fill = fill_color(st, st->background, alpha);
            cmd.stroke = fill_color(st, st->border_color, alpha);
            cmd.stroke_w = st->border_w;
            push_cmd(c, &cmd);
        }
        paint_runs(c, n, st, alpha, sx, sy, g);
        goto clip_done;
    }

    /* ---- 3D 投影(矩阵口径) ----
       自身带 3D 变换, 或处于 preserve-3d 祖先的复合矩阵中(继承矩阵非单位)
       时, 本盒按投影四边形绘制(矩形无法表达透视形变); 否则矩形快路径。
       out_total = 继承矩阵·局部矩阵, preserve-3d 时原样传给子级。
       投影要赶在逐侧边框之前算 —— 3D 面片的边框不能按未变换矩形画。 */
    float qx[4], qy[4];
    float mtotal[4][4];
    int proj3d = project_3d(st, g->m3d, n->bx, n->by, n->bw, n->bh,
                            persp, sx, sy, pcx, pcy, qx, qy, mtotal);

    /* 逐侧边框: 若任一侧有独立声明, 用四条矩形绘制(替代统一边框)。
       3D 面片不走这里 —— 未变换矩形会画到面片之外, 边框由
       push_quad_border 按投影四边形出四条梯形(见下)。 */
    int per_side = st->border_w4[0] || st->border_w4[1] || st->border_w4[2] || st->border_w4[3]
                   || st->border_c4[0] || st->border_c4[1] || st->border_c4[2] || st->border_c4[3];
    if (per_side && !proj3d) {
        /* 背景仍按盒绘制(不含边框), 然后四边分别填充 */
        int f = (st->background & 0xFFu) || st->has_gradient;
        if (f) {
            hn_cmd bg;
            memset(&bg, 0, sizeof(bg));
            bg.kind = HN_CMD_RECT;
            bg.x = tfx(g, sx, n->bx, n->by); bg.y = tfy(g, sy, n->bx, n->by);
            bg.w = n->bw * g->scale * g->scale_x; bg.h = n->bh * g->scale * g->scale_y;
            bg.radius = eff_radius(st, bg.w, bg.h);
            bg.fill = fill_color(st, st->background, alpha);
            if (st->has_gradient) {
                bg.gradient = 1;
                bg.grad_from = fill_color(st, st->grad_from, alpha);
                bg.grad_to = fill_color(st, st->grad_to, alpha);
                bg.grad_angle = st->grad_angle;
            }
            push_cmd(c, &bg);
        }
        for (int side = 0; side < 4; side++) {
            /* -1 表示显式 0(不画); 0 表示继承统一 border_w(口径见 border_side_spec) */
            float bw;
            hn_color bc;
            border_side_spec(st, side, &bw, &bc);
            if (bw <= 0 || (bc & 0xFFu) == 0) continue;
            hn_cmd e;
            memset(&e, 0, sizeof(e));
            e.kind = HN_CMD_RECT;
            float bx = n->bx, by = n->by, bwid = n->bw, bhei = n->bh;
            float t = bw;
            if (side == 0)      { bwid = n->bw; bhei = t; }
            else if (side == 2) { by = n->by + n->bh - t; bhei = t; }
            else if (side == 3) { bhei = n->bh; bwid = t; }
            else                { bx = n->bx + n->bw - t; bhei = n->bh; bwid = t; }
            e.x = tfx(g, sx, bx, by); e.y = tfy(g, sy, bx, by);
            e.w = bwid * g->scale; e.h = bhei * g->scale;
            e.fill = fill_color(st, bc, alpha);
            push_cmd(c, &e);
        }
        /* 内容由子节点绘制(下方继续) */
    }
    int has_fill = (st->background & 0xFFu) || st->has_gradient;
    int has_border = st->border_w > 0 && (st->border_color & 0xFFu);
    if (proj3d) {
        if (has_fill) {
            hn_cmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            /* 投影四边形填充。渐变字段与 RECT 同款 —— 之前只填平色,
               linear-gradient 底色的 3D 元素(background 解析为 0)投影后全透明。
               x/y/w/h 另装**投影前的元素盒**(减滚动): 渲染端据此解渐变轴,
               轴随面片一起被投影(等效 CSS: 渐变画在元素平面上再 3D 变换)。 */
            cmd.kind = HN_CMD_QUAD;
            for (int k = 0; k < 4; k++) { cmd.qx[k] = qx[k]; cmd.qy[k] = qy[k]; }
            cmd.x = n->bx - sx; cmd.y = n->by - sy;
            cmd.w = n->bw; cmd.h = n->bh;
            cmd.fill = fill_color(st, st->background, alpha);
            if (st->has_gradient) {
                cmd.gradient = 1;
                cmd.grad_from = fill_color(st, st->grad_from, alpha);
                cmd.grad_to = fill_color(st, st->grad_to, alpha);
                cmd.grad_angle = st->grad_angle;
            }
            push_cmd(c, &cmd);
            /* 不再跳过子节点: 子树照常递归, 文字随摊平仿射落到面片上。 */
        }
        if (has_border || per_side)
            push_quad_border(c, st, qx, qy, alpha);   /* 四条梯形(逐侧宽/色) */
    } else if (has_fill || has_border) {
        hn_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.kind = HN_CMD_RECT;
        cmd.x = tfx(g, sx, n->bx, n->by); cmd.y = tfy(g, sy, n->bx, n->by);
        /* 射影摊平: 盒尺寸随**锚点处局部线尺度**(近大远小) —— 只换锚点
           不换尺寸的盒在面片远边会偏出单应位置数十 px。2D 缩放链
           (fhx/fhy = 0)不进此档, 尺寸口径不变。 */
        float kw = 1.0f, kh = 1.0f;
        if (g->fhx != 0.0f || g->fhy != 0.0f)
            flatten_axes_at(g, n->bx, n->by, &kw, &kh);
        cmd.w = n->bw * g->scale * g->scale_x * kw;
        cmd.h = n->bh * g->scale * g->scale_y * kh;
        cmd.radius = eff_radius(st, cmd.w, cmd.h);
        cmd.fill = fill_color(st, st->background, alpha);
        cmd.stroke = fill_color(st, st->border_color, alpha);
        cmd.stroke_w = st->border_w;
        if (st->has_gradient) {
            cmd.gradient = 1;
            cmd.grad_from = fill_color(st, st->grad_from, alpha);
            cmd.grad_to = fill_color(st, st->grad_to, alpha);
            cmd.grad_angle = st->grad_angle;
        }
        if (st->has_shadow) {
            cmd.shadow = 1;
            cmd.shadow_color = fill_color(st, st->sh_color, alpha);
            cmd.shadow_blur = st->sh_blur;
            cmd.shadow_ox = st->sh_ox;
            cmd.shadow_oy = st->sh_oy;
        }
        push_cmd(c, &cmd);
    }

    /* ---- 子树的 3D 状态(preserve-3d 复合 / flat 扁平化) ----
       自身声明 perspective 则成为子树的**新视距声明者**(消失点=自身盒中心);
       preserve-3d: 子级继承"自己叠完的"复合矩阵继续叠;
       flat(缺省): 子级 3D 从零开始(矩阵复位), 但父面片的投影四边形以
       2D 仿射并进绘制状态 —— 子随父卡转动后被摊平, 与 CSS 扁平化语义一致。 */
    if (st->perspective > 0) {
        g->persp_d = st->perspective;
        g->persp_cx = n->bx + n->bw * 0.5f;
        g->persp_cy = n->by + n->bh * 0.5f;
    }
    if (st->transform_style == 1) {
        /* preserve-3d: 子级继承"自己叠完的"复合矩阵。2D 缩放分量也要并进
           3D 链(右乘: 元素自身变换先于子级局部矩阵生效) —— 此前只复合
           rotate/translateZ, scale(2) 祖先下的子面片投影尺寸不变(比值 ≈1)。
           无 2D 缩放时逐位拷贝(既有场景矩阵不变)。 */
        float s2[4][4];
        if (st_scale_2d(st, g->ox, g->oy, s2))
            mat_mul(g->m3d, mtotal, s2);
        else
            for (int i = 0; i < 4; i++)
                for (int j = 0; j < 4; j++) g->m3d[i][j] = mtotal[i][j];
    } else {
        mat_identity(g->m3d);
        if (proj3d) install_flatten(g, n->bx, n->by, n->bw, n->bh, qx, qy);
    }

    /* overflow 容器: 裁剪 + 子树滚动偏移。
       clipped 在函数开头就初始化(0), 任何分支都不会读到未初始化值。 */
    clipped = st->overflow != 0;
    if (clipped) {
        hn_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.kind = HN_CMD_CLIP_PUSH;
        cmd.x = n->bx - sx; cmd.y = n->by - sy;
        cmd.w = n->bw; cmd.h = n->bh;
        cmd.radius = eff_radius(st, cmd.w, cmd.h);
        push_cmd(c, &cmd);
    }
    float csx = sx + n->scroll_x, csy = sy + n->scroll_y;

    float child_alpha = alpha * st->opacity;
    /* 子节点绘制顺序: z-index 非零的定位元素排在后面(覆盖常规流内容)。
       稳定排序: 同 z-index 保持文档顺序。 */
    if (has_positioned_children(n)) {
        paint_children_sorted(c, n, st, child_alpha, csx, csy, g);
    } else {
        for (hn_node *ch = n->first; ch; ch = ch->next) {
            if (g->nodes > HN_PAINT_MAX_NODES || g->depth > HN_PAINT_MAX_DEPTH) break;
            g->depth++;
            paint_walk_g(c, ch, st, child_alpha, csx, csy, g);
            g->depth--;
        }
    }
    /* 容器自身的行内片段(目前仅 text-overflow 的省略号):
       在子节点之后绘制, 使其覆盖在被截断的文本之上 */
    if (n->n_runs > 0 && st->display != HN_DISP_INLINE && !hn_node_is_input(n)) {
        paint_runs(c, n, st, alpha, sx, sy, g);
    }

    /* 恢复本层之前的变换状态(兄弟节点不受影响) */
    sx = saved_sx; sy = saved_sy;
    g->scale = saved_scale; g->scale_x = saved_scale_x; g->scale_y = saved_scale_y;
    g->text_scale = saved_text_scale;
    g->ox = saved_ox; g->oy = saved_oy;
    memcpy(g->m3d, saved_m3d, sizeof(saved_m3d));
    g->persp_d = saved_pd; g->persp_cx = saved_pcx; g->persp_cy = saved_pcy;
    g->axx = saved_axx; g->axy = saved_axy; g->atx = saved_atx;
    g->ayx = saved_ayx; g->ayy = saved_ayy; g->aty = saved_aty;
    g->fhx = saved_fhx; g->fhy = saved_fhy; g->fhw = saved_fhw;

    if (clipped) {
        hn_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.kind = HN_CMD_CLIP_POP;
        push_cmd(c, &cmd);

        /* 滚动指示条(仅已滚动时出现) */
        float max_s = n->content_h - n->bh;
        if (max_s > 0.001f && n->scroll_y > 0.001f) {
            float track_h = n->bh - 4;
            float thumb_h = n->bh * n->bh / n->content_h;
            if (thumb_h < 24) thumb_h = 24;
            if (thumb_h > track_h) thumb_h = track_h;
            float ty = n->by - sy + 2 + (n->scroll_y / max_s) * (track_h - thumb_h);
            hn_cmd sb;
            memset(&sb, 0, sizeof(sb));
            sb.kind = HN_CMD_RECT;
            sb.x = n->bx - sx + n->bw - 5;
            sb.y = ty;
            sb.w = 3;
            sb.h = thumb_h;
            sb.radius = 1.5f;
            sb.fill = 0xFFFFFF55;
            push_cmd(c, &sb);
        }
    }

clip_done:
    /* clip-path 裁剪出栈(与入栈配对; 入栈点在自身背景绘制之前) */
    if (cp_clip) {
        hn_cmd cp;
        memset(&cp, 0, sizeof(cp));
        cp.kind = HN_CMD_CLIP_POP;
        push_cmd(c, &cp);
    }
}

void hn_paint_root(hn_context *c) {
    c->n_cmds = 0;
    if (c->doc) paint_walk(c, c->doc->root, NULL, 1.0f, 0, 0);
}

/* 立即清除根链底色(不打开开关)。见 hn_context_strip_root_background。
   html 与 body 都要清: 只清一个时另一个仍会铺满整屏。
   文档根是解析器合成的包裹节点, body 往往在更深一层 —— 必须真正遍历。 */
void hn_strip_root_background_now(hn_context *c) {
    if (!c || !c->doc || !c->doc->root) return;
    hn_node *stack[8];
    int sp = 0;
    stack[sp++] = c->doc->root;
    int guard = 0;
    while (sp > 0 && guard++ < 64) {
        hn_node *n = stack[--sp];
        if (n->kind == HN_ELEM && n->tag &&
            (!strcmp(n->tag, "html") || !strcmp(n->tag, "body"))) {
            n->style.background = 0;
            n->style.has_gradient = 0;
        }
        for (hn_node *ch = n->first; ch && sp < 8; ch = ch->next)
            if (ch->kind == HN_ELEM) stack[sp++] = ch;
    }
}

/* 透明背板: 把根链(html / body)的实色背景改为透明。
   声明 hn-transparent 时由运行时调用。
   注意这是**持续生效的开关**: 真正的清理发生在每次样式计算之后
   (见 hn_style_compute_all 的调用点), 这里只是打开开关并立刻清一次,
   让调用方无需额外重布局。只改一次的话, 下一次 layout 从级联重算样式
   就会把底色装回来 —— 表现为"首次显示透明, 一 resize 就变回不透明"。 */
void hn_context_strip_root_background(hn_context *c) {
    if (!c) return;
    c->transparent = 1;
    hn_strip_root_background_now(c);
}

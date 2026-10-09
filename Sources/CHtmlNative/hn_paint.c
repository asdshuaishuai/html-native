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
   缩放原点取元素盒中心 —— 与 CSS transform-origin: center 一致。 */
typedef struct {
    int   depth;
    int   nodes;
    float scale;   /* 累积等比缩放(默认 1) */
    float scale_x, scale_y;  /* 累积**非等比**分量(默认 1) */
    float ox, oy;  /* 缩放原点(绝对坐标) */
} paint_guard;

/* ---- 3D 变换 ---- */

/* 把盒子四角经 3D 旋转 + 透视投影得到屏幕坐标。
   旋转原点 = transform-origin(缺省 50% 50% 即盒中心)。
   透视投影中心 = (pcx, pcy) —— 与旋转原点分开传: CSS 里 perspective
   声明在父级时, 消失点是**父盒中心**(perspective-origin 缺省 50% 50%),
   不是子元素的 transform-origin。此前二者共用一个点, 子元素偏离父中心时
   透视方向错误(往自己的 origin 收缩而不是往父盒中心消失)。
   返回 1 表示存在实际 3D 倾斜(需要按四边形绘制), 0 表示无需变换。 */
static int project_3d(const hn_style *st, float bx, float by, float bw, float bh,
                      float persp, float sx, float sy,
                      float pcx, float pcy,
                      float ox[4], float oy[4]) {
    /* 只有"完全没有 3D 变换"时才走矩形快路径。translateZ 与 rotate3d 同样
       会破坏轴对齐性(透视缩放 / 任意轴旋转), 漏掉它们就等于声明了没反应。 */
    if (st->rotate_x == 0 && st->rotate_y == 0 && st->rotate == 0
        && !st->has_r3d && st->translate_z == 0) return 0;
    const float D2R = 3.14159265358979f / 180.0f;
    /* transform-origin: 之前这里写死盒中心, 声明了 origin 也无效 ——
       绕左上角转(铰链/表盘)只能靠 translate 硬凑, 而且凑出来的角度是错的。 */
    float cx = bx + (st->has_origin
                     ? (st->origin_pct_x ? st->origin_x * bw : st->origin_x)
                     : bw * 0.5f);
    float cy = by + (st->has_origin
                     ? (st->origin_pct_y ? st->origin_y * bh : st->origin_y)
                     : bh * 0.5f);
    float rx = st->rotate_x * D2R, ry = st->rotate_y * D2R, rz = st->rotate * D2R;
    float cxr = cosf(rx), sxr = sinf(rx);
    float cyr = cosf(ry), syr = sinf(ry);
    float czr = cosf(rz), szr = sinf(rz);
    /* rotate3d(x,y,z,angle): 绕任意轴旋转(Rodrigues 公式)。先于 X/Y/Z 轴旋转
       施加 —— 与 CSS 的函数复合顺序一致(先写的先作用)。
       轴已在解析期归一化, 这里只做 sin/cos。 */
    float c3 = cosf(st->r3d_deg * D2R), s3 = sinf(st->r3d_deg * D2R);
    float ax = st->r3d_x, ay = st->r3d_y, az = st->r3d_z;
    float t3 = 1.0f - c3;
    /*  Rodrigues 的 3x3(行主序):
        R = t3*A⊗A + c3*I + s3*[A]×  */
    float r00 = t3*ax*ax + c3,     r01 = t3*ax*ay - s3*az, r02 = t3*ax*az + s3*ay;
    float r10 = t3*ax*ay + s3*az,  r11 = t3*ay*ay + c3,    r12 = t3*ay*az - s3*ax;
    float r20 = t3*ax*az - s3*ay,  r21 = t3*ay*az + s3*ax, r22 = t3*az*az + c3;
    float dist = persp > 0 ? persp : 0;      /* 0 = 正交投影 */
    /* 四角取**盒左上为基准**, 再减 origin 得到相对原点的局部坐标。
       早先这里存的是"相对盒中心"的坐标, 而 origin 支持加进来之后
       cx/cy 可能不是盒中心 —— 于是盒子自身相对原点的偏移被丢掉,
       表现为"绕左上角转"实际绕的是"变换后图形的中心"。
       origin 缺省(盒中心)时两者等价, 所以只有显式声明 origin 才暴露。 */
    float lx[4] = { 0, bw, bw, 0 };
    float ly[4] = { 0, 0, bh, bh };
    int tilted = 0;
    for (int i = 0; i < 4; i++) {
        float x = bx + lx[i] - cx, y = by + ly[i] - cy;
        /* translateZ: 沿 Z 的深度位移。>0 靠近观察者, 透视下图形变大 ——
           这就是"声明了 translateZ 却毫无变化"缺的那一步。 */
        float z = st->translate_z;
        /* 任意轴旋转先施加 */
        if (st->has_r3d) {
            float nx = r00*x + r01*y + r02*z;
            float ny = r10*x + r11*y + r12*z;
            float nz = r20*x + r21*y + r22*z;
            x = nx; y = ny; z = nz;
        }
        /* 绕 X */
        float y1 = y * cxr - z * sxr, z1 = y * sxr + z * cxr;
        /* 绕 Y */
        float x2 = x * cyr + z1 * syr, z2 = -x * syr + z1 * cyr;
        /* 绕 Z */
        float x3 = x2 * czr - y1 * szr, y3 = x2 * szr + y1 * czr;
        float px = cx + x3, py = cy + y3;
        if (dist > 0.01f) {
            /* 透视: 离观察者越远(z2 越小)缩放越小;
               缩放朝透视中心(pcx,pcy)收缩 —— 父级 perspective 时即父盒中心 */
            float k = dist / (dist - z2);
            if (k < 0.05f) k = 0.05f;
            if (k > 20.0f) k = 20.0f;
            px = pcx + (px - pcx) * k;
            py = pcy + (py - pcy) * k;
        }
        if (fabsf(z2) > 0.01f) tilted = 1;
        ox[i] = px - sx;
        oy[i] = py - sy;
    }
    /* 判定: 绕 X/Y 旋转或绕 Z 旋转(非 0 角度)都会让矩形不再轴对齐,
       必须按四边形绘制。只有"完全没有旋转变换"时才走矩形快路径
       (translate/scale 不改变轴对齐性, 仍可用矩形 + 宽高缩放)。 */
    /* translateZ/rotate3d 也会让矩形不再轴对齐(透视缩放或任意轴旋转),
       所以它们同样必须走四边形路径。 */
    if (st->rotate_x == 0 && st->rotate_y == 0 && fabsf(st->rotate) < 0.01f
        && !st->has_r3d && st->translate_z == 0) return 0;
    (void)tilted;
    return 1;
}

/* 绝对坐标 → 设备坐标(先绕原点缩放, 再减滚动偏移)。
   x/y 各自用**自己的**缩放分量 —— 之前只有一个 scale, 于是 scale(2,1)
   被当成等比 2 处理(图形纵向也被拉高一倍)。 */
static inline float tfx(const paint_guard *g, float sx, float x) {
    return (x - g->ox) * g->scale * g->scale_x + g->ox - sx;
}
static inline float tfy(const paint_guard *g, float sy, float y) {
    return (y - g->oy) * g->scale * g->scale_y + g->oy - sy;
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

/* 文本装饰线: underline / line-through / overline(随文本基线定位) */
static void push_deco(hn_context *c, const hn_style *st, float x, float baseline,
                      float w, float alpha, float sx, float sy) {
    if (!st->text_deco || w <= 0) return;
    float ys[3]; int ny = 0;
    if (st->text_deco & 1) ys[ny++] = baseline + st->font_size * 0.14f;
    if (st->text_deco & 2) ys[ny++] = baseline - st->font_size * 0.30f;
    if (st->text_deco & 4) ys[ny++] = baseline - st->font_size * 0.92f;
    hn_color col = fill_color(st, st->color, alpha);
    for (int i = 0; i < ny; i++) {
        hn_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.kind = HN_CMD_RECT;
        cmd.x = x - sx;
        cmd.y = ys[i] - sy;
        cmd.w = w;
        cmd.h = st->font_size > 14 ? 1.5f : 1.0f;
        cmd.fill = col;
        push_cmd(c, &cmd);
    }
}

/* 列表标记: ul → 实心圆/方/空心圆, ol → 十进制序号(右对齐于内容左缘) */
static void paint_marker(hn_context *c, hn_node *n, const hn_style *st,
                         float alpha, float sx, float sy) {
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
        hn_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.kind = HN_CMD_TEXT;
        cmd.text = num;
        cmd.text_len = strlen(num);
        cmd.tx = n->bx - 7 - w - sx;
        cmd.baseline = n->by + st->font_size * 1.02f - sy;
        cmd.font.size_px = st->font_size;
        cmd.font.weight = st->font_weight;
        cmd.font.italic = st->font_italic;
        cmd.font.letter_spacing = st->letter_spacing;
        cmd.fill = col;
        push_cmd(c, &cmd);
        return;
    }
    /* 无序标记: 0=disc(默认) 2=square 3=circle(空心) */
    hn_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = HN_CMD_RECT;
    cmd.w = cmd.h = st->list_style == 2 ? 5 : 5;
    cmd.x = n->bx - 14 - sx;
    cmd.y = n->by + st->font_size * 0.45f - sy;
    if (st->list_style == 3) {          /* 空心圆 */
        cmd.radius = 2.5f;
        cmd.stroke = col;
        cmd.stroke_w = 1;
    } else if (st->list_style == 2) {   /* 方块 */
        cmd.fill = col;
    } else {                            /* 实心圆 */
        cmd.radius = 2.5f;
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
    cmd.x = tfx(g, sx, n->bx) - 2;
    cmd.y = tfy(g, sy, n->by) - 2;
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
    float cx = tfx(g, sx, n->bx) + w * 0.5f;
    float cy = tfy(g, sy, n->by) + h * 0.5f;
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
        hn_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.kind = HN_CMD_TEXT;
        cmd.text = n->text + r->begin;
        cmd.text_len = r->end - r->begin;
        cmd.tx = tfx(g, sx, r->x);
        cmd.baseline = tfy(g, sy, r->baseline);
        if (g->scale != 1.0f) cmd.font.size_px = st->font_size * g->scale;
        cmd.font.size_px = st->font_size;
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
        push_deco(c, st, r->x, r->baseline, r->width, alpha, sx, sy);
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

/* 防御性保护: DOM 结构异常(纵向超深嵌套 / 横向兄弟链成环)时只截断绘制,
   绝不让渲染器崩溃或死循环。正常文档远达不到这些上限。
   两个维度都要防: 只看深度防不了兄弟链成环(那是同层无限循环)。 */
#define HN_PAINT_MAX_DEPTH 256
#define HN_PAINT_MAX_NODES 200000

static void paint_walk_g(hn_context *c, hn_node *n, const hn_style *pst,
                         float alpha, float sx, float sy, paint_guard *g);

static void paint_walk(hn_context *c, hn_node *n, const hn_style *pst,
                       float alpha, float sx, float sy) {
    paint_guard g = { 0, 0, 1.0f, 1.0f, 1.0f, 0, 0 };
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
    /* 透视视距: 元素自身的 perspective, 否则取父级的(与 CSS 一致)。
       投影中心(消失点)跟着视距的**声明者**走: CSS perspective-origin
       缺省 50% 50% —— 自己声明就是自己盒中心, 父级声明就是父盒中心。 */
    float persp = st->perspective;
    float pcx = n->bx + n->bw * 0.5f, pcy = n->by + n->bh * 0.5f;
    if (persp <= 0 && pst && pst->perspective > 0) {
        persp = pst->perspective;
        if (n->parent) {
            pcx = n->parent->bx + n->parent->bw * 0.5f;
            pcy = n->parent->by + n->parent->bh * 0.5f;
        }
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
    if (st->scale != 1.0f && st->scale > 0.01f) g->scale = saved_scale * st->scale;
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
        paint_marker(c, n, st, alpha, sx, sy);
    }

    /* Lottie 矢量动画: 声明了 hn-lottie 的元素整体由动画接管(覆盖盒背景与子节点)。
       解析结果按路径缓存, 时钟取自节点的 ext_clock(由动画 tick 推进)。 */
    if (n->tag) {
        const char *lsrc = hn_attr_lottie_src(n);
        if (lsrc) {
            struct hn_lottie *l = hn_context_lottie(c, lsrc);
            if (l) {
                float bw = n->bw * g->scale * g->scale_x, bh = n->bh * g->scale * g->scale_y;
                float bx = tfx(g, sx, n->bx), by = tfy(g, sy, n->by);
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
                         tfx(g, sx, n->bx), tfy(g, sy, n->by), alpha);
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
                cmd.x = tfx(g, sx, n->bx); cmd.y = tfy(g, sy, n->by);
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
                cmd.x = tfx(g, sx, n->bx); cmd.y = tfy(g, sy, n->by);
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
            cmd.x = tfx(g, sx, n->bx); cmd.y = tfy(g, sy, n->by);
            cmd.w = n->bw * g->scale * g->scale_x; cmd.h = n->bh * g->scale * g->scale_y;
            cmd.radius = eff_radius(st, cmd.w, cmd.h);
            cmd.text = src;
            cmd.text_len = strlen(src);
            push_cmd(c, &cmd);
        }
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
            cmd.x = tfx(g, sx, n->bx); cmd.y = tfy(g, sy, n->by); cmd.w = n->bw * g->scale * g->scale_x; cmd.h = n->bh * g->scale * g->scale_y;
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
                cmd.tx = tfx(g, sx, x0);
                cmd.baseline = tfy(g, sy, bl);
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
                cmd.tx = tfx(g, sx, r->x);
                cmd.baseline = tfy(g, sy, r->baseline);
                if (g->scale != 1.0f) cmd.font.size_px = st->font_size * g->scale;
                cmd.font.size_px = st->font_size;
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
            cmd.x = tfx(g, sx, n->bx); cmd.y = tfy(g, sy, n->by); cmd.w = n->bw * g->scale * g->scale_x; cmd.h = n->bh * g->scale * g->scale_y;
            cmd.radius = st->radius;
            cmd.fill = fill_color(st, st->background, alpha);
            cmd.stroke = fill_color(st, st->border_color, alpha);
            cmd.stroke_w = st->border_w;
            push_cmd(c, &cmd);
        }
        paint_runs(c, n, st, alpha, sx, sy, g);
        goto clip_done;
    }

    /* 逐侧边框: 若任一侧有独立声明, 用四条矩形绘制(替代统一边框) */
    int per_side = st->border_w4[0] || st->border_w4[1] || st->border_w4[2] || st->border_w4[3]
                   || st->border_c4[0] || st->border_c4[1] || st->border_c4[2] || st->border_c4[3];
    if (per_side) {
        /* 背景仍按盒绘制(不含边框), 然后四边分别填充 */
        int f = (st->background & 0xFFu) || st->has_gradient;
        if (f) {
            hn_cmd bg;
            memset(&bg, 0, sizeof(bg));
            bg.kind = HN_CMD_RECT;
            bg.x = tfx(g, sx, n->bx); bg.y = tfy(g, sy, n->by);
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
            /* -1 表示显式 0(不画); 0 表示继承统一 border_w */
            float bw = st->border_w4[side] ? (st->border_w4[side] > 0 ? st->border_w4[side] : 0)
                                           : st->border_w;
            hn_color bc = st->border_c4[side] ? (st->border_c4[side] > 1 ? st->border_c4[side]
                                                                            : st->border_color)
                                              : st->border_color;
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
            e.x = tfx(g, sx, bx); e.y = tfy(g, sy, by);
            e.w = bwid * g->scale; e.h = bhei * g->scale;
            e.fill = fill_color(st, bc, alpha);
            push_cmd(c, &e);
        }
        /* 内容由子节点绘制(下方继续) */
    }
    int has_fill = (st->background & 0xFFu) || st->has_gradient;
    int has_border = st->border_w > 0 && (st->border_color & 0xFFu);
    if (has_fill || has_border) {
        hn_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        /* 3D 变换: 有倾斜时按投影后的四边形填充(矩形无法表达透视形变) */
        float qx[4], qy[4];
        if (project_3d(st, n->bx, n->by, n->bw, n->bh, persp, sx, sy, pcx, pcy, qx, qy)) {
            hn_cmd q;
            memset(&q, 0, sizeof(q));
            q.kind = HN_CMD_QUAD;
            for (int k = 0; k < 4; k++) { q.qx[k] = qx[k]; q.qy[k] = qy[k]; }
            q.fill = fill_color(st, st->background, alpha);
            push_cmd(c, &q);
            /* 四边形目前只填充; 子节点(文本)仍按平面绘制(简化),
               对 UI 场景(卡片翻转/3D 倾斜)足够。 */
            goto draw_children;
        }
        cmd.kind = HN_CMD_RECT;
        cmd.x = tfx(g, sx, n->bx); cmd.y = tfy(g, sy, n->by); cmd.w = n->bw * g->scale * g->scale_x; cmd.h = n->bh * g->scale * g->scale_y;
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

    /* overflow 容器: 裁剪 + 子树滚动偏移。
       clipped 在函数开头就初始化: 上面的 3D 分支会 `goto draw_children`
       跳过本声明, 在 C 里那等于**未初始化**(不是 0)。后果是 3D 变换的
       overflow 容器可能在没 push 过 CLIP_PUSH 的情况下走到末尾的
       `if (clipped)` 发一个 CLIP_POP —— 后端裁剪栈弹出空栈,
       CoreGraphics 的 restoreGState 与 hnsoft 的 clip 栈都会错位。 */
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
draw_children:
    /* 容器自身的行内片段(目前仅 text-overflow 的省略号):
       在子节点之后绘制, 使其覆盖在被截断的文本之上 */
    if (n->n_runs > 0 && st->display != HN_DISP_INLINE && !hn_node_is_input(n)) {
        paint_runs(c, n, st, alpha, sx, sy, g);
    }

    /* 恢复本层之前的变换状态(兄弟节点不受影响) */
    sx = saved_sx; sy = saved_sy;
    g->scale = saved_scale; g->scale_x = saved_scale_x; g->scale_y = saved_scale_y;
    g->ox = saved_ox; g->oy = saved_oy;

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

/* hn_lottie.c — Lottie(bodymovin)矢量动画求值 → 绘制指令
 *
 * 分层: 引擎负责"矢量图形 + 时间轴", 产物仍是平台无关的绘制指令
 * (POLYGON / IMAGE / RECT / CLIP_PUSH), 因此各渲染后端(hnsoft、cairo、
 * GPU)同等支持 —— 不需要为每个平台实现一遍播放器。
 * JSON 从哪来由运行时的资产后端注入(引擎自身不做磁盘/网络 I/O)。
 *
 * 支持子集(覆盖图标/loading/表情/插画类动画的绝大多数):
 *   图层: 形状层(ty=4) / 纯色层(ty=1) / 图片层(ty=2)
 *   形状: 组(gr, 含嵌套与自身变换) / 矩形(rc, 含圆角) / 椭圆(el) / 路径(sh)
 *   画刷: 填充(fl) / 描边(st) / **渐变填充(gf) / 渐变描边(gs)** ——
 *         线性多 stop 在形状局部空间切成色带; 径向退化为中位色
 *   动画: 锚点/位置/缩放/旋转/不透明度关键帧 + 路径变形关键帧(sh.ks.k)
 *   mask: 图层 masksProperties 的**单条加法蒙版** —— 取路径外接矩形做
 *         CLIP_PUSH 裁剪(矩形蒙版精确, 曲线蒙版是外接盒近似)
 * 明确不支持(跳过, 不报错): 预合成 / 文本层 / 表达式 / 特效 / trim path
 *   之外的高级 trim 语义 / 轨道遮罩(td) / merge paths / 时间重映射 /
 *   径向渐变的形状化 / 蒙版的反相·相减·相交模式。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn_internal.h"
#include "hn_json.h"
#include "hn_png.h"          /* hn_inflate: ZIP 条目的裸 deflate 解压 */

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef HN_MIN
#define HN_MIN(a, b) ((a) < (b) ? (a) : (b))
#endif

#define LOTTIE_SEG        8      /* 贝塞尔 → 折线的每段细分数 */
#define LOTTIE_MAX_PATHS  512    /* 单组内路径数上限(防异常文件) */
#define LOTTIE_MAX_DEPTH  12     /* 组嵌套深度上限 */
#define LOTTIE_MAX_VERTS  4096   /* 单条路径顶点上限 */

/* ---------------- 可动画属性 ---------------- */

typedef struct {
    float t;
    float v[4];
    unsigned char hold;      /* 保持到下一帧(不插值) */
} lkf;

typedef struct {
    float v[4];
    int   ncomp;
    lkf  *kf;
    int   n_kf;
} lprop;

static void lprop_at(const lprop *p, float t, float out[4]) {
    out[0] = p->v[0]; out[1] = p->v[1]; out[2] = p->v[2]; out[3] = p->v[3];
    if (p->n_kf <= 0) return;
    if (t <= p->kf[0].t) { memcpy(out, p->kf[0].v, sizeof(float) * 4); return; }
    if (t >= p->kf[p->n_kf - 1].t) {
        memcpy(out, p->kf[p->n_kf - 1].v, sizeof(float) * 4);
        return;
    }
    for (int i = 0; i < p->n_kf - 1; i++) {
        const lkf *a = &p->kf[i], *b = &p->kf[i + 1];
        if (t < a->t || t > b->t) continue;
        float span = b->t - a->t;
        float f = span > 0.0001f ? (t - a->t) / span : 0;
        if (a->hold) f = 0;
        for (int k = 0; k < 4; k++)
            out[k] = a->v[k] + (b->v[k] - a->v[k]) * f;
        return;
    }
}

static float lprop_at1(const lprop *p, float t, float dflt) {
    if (p->ncomp == 0 && p->n_kf == 0) return dflt;
    float o[4];
    lprop_at(p, t, o);
    return o[0];
}

/* Lottie 颜色是归一化 [r,g,b(,a)] */
static hn_color lot_color(const float v[4], float alpha) {
    float av = (v[3] > 0.0001f ? v[3] : 1.0f) * alpha;
    int r = (int)(v[0] * 255.0f + 0.5f), g = (int)(v[1] * 255.0f + 0.5f);
    int b = (int)(v[2] * 255.0f + 0.5f), a = (int)(av * 255.0f + 0.5f);
    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;
    if (a < 0) a = 0; else if (a > 255) a = 255;
    return ((hn_color)r << 24) | ((hn_color)g << 16) | ((hn_color)b << 8) | (hn_color)a;
}

/* 不透明度: 不同导出工具用 0..1 或 0..100 */
static float norm_op(float o) { return o > 1.0001f ? o / 100.0f : o; }

/* ---------------- 形状定义 ---------------- */

enum { L_GROUP, L_RECT, L_ELLIPSE, L_PATH, L_FILL, L_STROKE, L_TRANSFORM, L_TRIM, L_REPEATER,
       L_FILL_GRAD, L_STROKE_GRAD };

typedef struct { float t; float *data; int n; } lpkf;   /* 路径关键帧 */

#define LOTTIE_MAX_GSTOPS 16    /* 渐变 stop 上限 */
#define LOTTIE_GRAD_BANDS 24    /* 线性渐变→色带数(填充) */

typedef struct lshape {
    int kind;
    lprop pos, size, round;           /* 几何 */
    lprop color, opacity, width;      /* 画刷 */
    int   fill_rule;                  /* 1=nonzero 2=evenodd */
    float *verts, *tan_in, *tan_out;  /* 静态路径 */
    int    n_verts, closed;
    lpkf  *pkf; int n_pkf;
    /* trim path: 沿路径按百分比取一段。line-draw / 进度环 / 加载动画的核心。
       之前完全不支持 —— 但更糟的是**静态 sh 路径本身就不显示**(见 parse_item),
       所以带 trim 的描边动画整条什么都不画。 */
    lprop trim_start, trim_end, trim_offset;
    unsigned char has_trim;
    /* repeater: 把组内路径按 tr 的位移重复 c 次。网格/光栅/粒子常用。 */
    lprop rep_pos;
    lprop rep_count_prop;   /* c 为动画属性时的载体 */
    float rep_count;
    unsigned char has_rep;
    /* 渐变画刷(gf 渐变填充 / gs 渐变描边) —— 多 stop 线性渐变。
       引擎的 POLYGON 指令没有渐变通道, 因此这里在**形状局部空间**把几何
       按渐变轴切成色带(每条色带一个插值色), 再走既有变换/发射路径 ——
       cairo 与 hnsoft 两个后端零改动即得到渐变观感(带宽 1px 级以下时
       相邻色带的衔接不可见)。
       字段: s/e = 渐变轴两端(局部坐标, 可动画); gsp/gsc = stop 位置与颜色。
       径向(t=2)无对应的几何切分方式(等值线是圆环, 单轮廓多边形表达不了),
       退化为中位色 —— 见文件头"不支持"一节。 */
    lprop gp0, gp1;
    lprop gsp[LOTTIE_MAX_GSTOPS], gsc[LOTTIE_MAX_GSTOPS];
    int    n_gstops;
    int    gtype;                     /* 1=线性 2=径向 */
    unsigned char has_grad;
    lprop anchor, tpos, scale, rot;   /* 变换 */
    struct lshape *kids; int n_kids;  /* 组 */
} lshape;

typedef struct {
    int   kind;                       /* 0=形状 1=纯色 2=图片 */
    float start_ms, end_ms;
    float w, h;
    const char *image_src;
    hn_color solid_color;
    lprop anchor, pos, scale, rot, opacity;
    lshape *shapes; int n_shapes;
    /* mask 最小版(masksProperties): 第一条 mode="a" 且 inv=false 的蒙版,
       在绘制该层内容前用 CLIP_PUSH 裁剪。路径是**图层局部坐标**的贝塞尔
       顶点(与形状层的 sh 同一几何编码), 展开为折线后取外接矩形做矩形
       裁剪 —— 对矩形/圆角矩形蒙版(卡片/图片裁切)精确, 对任意曲线蒙版
       是外接盒近似(会把少许边缘内容带进来)。反相/相减/相交等模式不做,
       多条蒙版只取第一条 —— 见文件头"不支持"一节。 */
    float *mask_v, *mask_i, *mask_o;
    int    n_mask_v;
    unsigned char mask_closed;
    lpkf  *mask_pkf; int n_mask_pkf;
    unsigned char has_mask;
} llayer;

struct hn_lottie {
    hn_arena *arena;                  /* 结构体自身的内存 */
    float w, h;                       /* 画布尺寸 */
    float duration_ms;
    llayer *layers; int n_layers;
    void  *json_arena;
    char  *json_buf;                  /* 就地解析需要可写缓冲 */
};

void hn_lottie_free(struct hn_lottie *l) {
    if (!l) return;
    if (l->arena) hn_arena_destroy(l->arena);
    if (l->json_arena) hn_json_free(l->json_arena);
    free(l->json_buf);
    free(l);
}

float hn_lottie_duration_ms(struct hn_lottie *l) { return l ? l->duration_ms : 0; }

void hn_lottie_size(struct hn_lottie *l, float *w, float *h) {
    if (w) *w = l ? l->w : 0;
    if (h) *h = l ? l->h : 0;
}

/* ---------------- JSON → 结构 ---------------- */

static float *flatten_num(hn_arena *a, const hn_json *v, int *n_out) {
    *n_out = 0;
    if (!v || v->kind != HN_JSON_ARR || v->count <= 0) return NULL;
    int n = v->count;
    float *r = (float *)hn_arena_alloc(a, sizeof(float) * (size_t)n);
    for (int i = 0; i < n; i++) r[i] = (float)hn_json_num(hn_json_at(v, i), 0);
    *n_out = n;
    return r;
}

/* 展平 [[x,y],...] 或 [x,y,...] 为 [x,y,...]。*n_pts = 点数(不是元素数)。 */
static float *flatten_pts(hn_arena *a, const hn_json *v, int *n_pts) {
    *n_pts = 0;
    if (!v || v->kind != HN_JSON_ARR || v->count <= 0) return NULL;
    hn_json *f0 = hn_json_at(v, 0);
    int n = v->count;
    if (f0 && f0->kind == HN_JSON_ARR) {
        float *r = (float *)hn_arena_alloc(a, sizeof(float) * (size_t)(n * 2));
        for (int i = 0; i < n; i++) {
            hn_json *e = hn_json_at(v, i);
            r[i * 2]     = (float)hn_json_num(hn_json_at(e, 0), 0);
            r[i * 2 + 1] = (float)hn_json_num(hn_json_at(e, 1), 0);
        }
        *n_pts = n;
        return r;
    }
    if (f0 && (f0->kind == HN_JSON_NUM || f0->kind == HN_JSON_BOOL)) {
        int m = 0;
        float *r = flatten_num(a, v, &m);
        *n_pts = m / 2;
        return r;
    }
    return NULL;
}

/* 一个 JSON 值 → 可动画属性的值部分。
   两种编码都要吃下:
     [100,100]       平铺分量(位置/缩放/颜色)
     [[1,2],[3,4]]   点数组(路径顶点)
   颜色常是 3 分量 [r,g,b] —— 早期实现把"点数"误当"元素数", 只取了 2 个分量,
   导致蓝色分量恒为 0(颜色全部偏黄)。这里按元素类型显式分流。 */
static void prop_value(hn_arena *a, const hn_json *v, lprop *out) {
    if (!v) return;
    if (v->kind == HN_JSON_NUM || v->kind == HN_JSON_BOOL) {
        out->v[0] = (float)hn_json_num(v, 0);
        out->ncomp = 1;
        return;
    }
    if (v->kind != HN_JSON_ARR || v->count <= 0) return;
    hn_json *f0 = hn_json_at(v, 0);
    if (f0 && f0->kind == HN_JSON_ARR) {
        int np = 0;
        float *p = flatten_pts(a, v, &np);
        if (!p) return;
        int comp = np * 2;
        if (comp > 4) comp = 4;
        for (int i = 0; i < comp; i++) out->v[i] = p[i];
        out->ncomp = comp;
        return;
    }
    int n = 0;
    float *f = flatten_num(a, v, &n);
    if (!f) return;
    if (n > 4) n = 4;
    for (int i = 0; i < n; i++) out->v[i] = f[i];
    out->ncomp = n;
}

static void parse_prop(hn_arena *a, const hn_json *v, lprop *out) {
    memset(out, 0, sizeof(*out));
    if (!v) return;
    /* 直接是值(非 {a,k} 包装) */
    if (v->kind == HN_JSON_NUM || v->kind == HN_JSON_BOOL ||
        (v->kind == HN_JSON_ARR && hn_json_at(v, 0) &&
         hn_json_at(v, 0)->kind != HN_JSON_OBJ)) {
        prop_value(a, v, out);
        return;
    }
    if (v->kind != HN_JSON_OBJ) return;
    const hn_json *k = hn_json_obj_get(v, "k");
    if (!k) return;
    /* k 是值(静态) */
    if (k->kind != HN_JSON_ARR) { prop_value(a, k, out); return; }
    hn_json *f0 = hn_json_at(k, 0);
    if (!f0) return;
    if (f0->kind != HN_JSON_OBJ) { prop_value(a, k, out); return; }
    /* k 是关键帧数组 */
    int cnt = k->count;
    out->kf = (lkf *)hn_arena_alloc(a, sizeof(lkf) * (size_t)cnt);
    memset(out->kf, 0, sizeof(lkf) * (size_t)cnt);
    for (int i = 0; i < cnt; i++) {
        hn_json *e = hn_json_at(k, i);
        lkf *kf = &out->kf[i];
        kf->t = (float)hn_json_num(hn_json_obj_get(e, "t"), 0);
        hn_json *h = hn_json_obj_get(e, "h");
        if (h && hn_json_bool(h, 0)) kf->hold = 1;
        lprop tmp;
        memset(&tmp, 0, sizeof(tmp));
        prop_value(a, hn_json_obj_get(e, "s"), &tmp);
        memcpy(kf->v, tmp.v, sizeof(kf->v));
        if (out->ncomp == 0) out->ncomp = tmp.ncomp;
    }
    out->n_kf = cnt;
}

/* 关键帧时间归一: Lottie 的 kf.t 单位是**帧**, 引擎内部统一用毫秒。
   早期实现直接拿帧号当毫秒, 于是 1.5s 的动画被当成 90ms 播完 ——
   表现为"动画只剩最后一帧"(时间轴一次性越过所有关键帧)。 */
static void norm_prop_time(lprop *p, float mpf) {
    if (p->n_kf <= 0 || !p->kf) return;
    for (int i = 0; i < p->n_kf; i++) p->kf[i].t *= mpf;
}

static void norm_shape_time(lshape *s, float mpf) {
    norm_prop_time(&s->pos, mpf);
    norm_prop_time(&s->size, mpf);
    norm_prop_time(&s->round, mpf);
    norm_prop_time(&s->color, mpf);
    norm_prop_time(&s->opacity, mpf);
    norm_prop_time(&s->width, mpf);
    norm_prop_time(&s->anchor, mpf);
    norm_prop_time(&s->tpos, mpf);
    norm_prop_time(&s->scale, mpf);
    norm_prop_time(&s->rot, mpf);
    norm_prop_time(&s->gp0, mpf);
    norm_prop_time(&s->gp1, mpf);
    for (int i = 0; i < s->n_gstops; i++) {
        norm_prop_time(&s->gsp[i], mpf);
        norm_prop_time(&s->gsc[i], mpf);
    }
    for (int i = 0; i < s->n_pkf; i++) s->pkf[i].t *= mpf;
    for (int i = 0; i < s->n_kids; i++) norm_shape_time(&s->kids[i], mpf);
}

/* 路径几何: {c:closed, v:[[x,y]], i:[[x,y]], o:[[x,y]]} */
static void parse_path_geom(hn_arena *a, const hn_json *ks, lshape *sh) {
    if (!ks || ks->kind != HN_JSON_OBJ) return;
    sh->closed = hn_json_bool(hn_json_obj_get(ks, "c"), 0);
    int nv = 0, ni = 0, no = 0;
    sh->verts   = flatten_pts(a, hn_json_obj_get(ks, "v"), &nv);
    sh->tan_in  = flatten_pts(a, hn_json_obj_get(ks, "i"), &ni);
    sh->tan_out = flatten_pts(a, hn_json_obj_get(ks, "o"), &no);
    sh->n_verts = nv;
    if (sh->n_verts > LOTTIE_MAX_VERTS) sh->n_verts = LOTTIE_MAX_VERTS;
}

static void parse_transform(hn_arena *a, const hn_json *tr, lshape *sh) {
    if (!tr) return;
    parse_prop(a, hn_json_obj_get(tr, "a"), &sh->anchor);
    parse_prop(a, hn_json_obj_get(tr, "p"), &sh->tpos);
    parse_prop(a, hn_json_obj_get(tr, "s"), &sh->scale);
    parse_prop(a, hn_json_obj_get(tr, "r"), &sh->rot);
    parse_prop(a, hn_json_obj_get(tr, "o"), &sh->opacity);
}

static int parse_items(hn_arena *a, const hn_json *arr, lshape **out);

/* 在 assets 数组里按 id 找预合成定义。
   真实导出文件几乎必带预合成 —— 不支持的话整条内容什么都不显示。 */
static const hn_json *find_asset(const hn_json *root, const char *refId) {
    if (!root || !refId) return NULL;
    const hn_json *assets = hn_json_obj_get(root, "assets");
    if (!assets || assets->kind != HN_JSON_ARR) return NULL;
    for (int i = 0; i < assets->count; i++) {
        const hn_json *as = hn_json_at(assets, i);
        if (!as || as->kind != HN_JSON_OBJ) continue;
        const char *id = hn_json_str(hn_json_obj_get(as, "id"), NULL);
        if (id && !strcmp(id, refId)) return as;
    }
    return NULL;
}

/* 预合成(ty=0)展平: 每个内层图层 → 一个 L_GROUP, 变换取自该图层的 ks,
   子项就是它的 shapes。预合成自身的图层变换随后通过 emit_group 既有的
   组合链与内层变换自然叠加 —— 不需要另写一套图层递归。 */
static int parse_precomp(hn_arena *a, const hn_json *comp_layers, float fr, float ip,
                         lshape **out) {
    *out = NULL;
    if (!comp_layers || comp_layers->kind != HN_JSON_ARR || comp_layers->count <= 0) return 0;
    int cap = comp_layers->count;
    lshape *list = (lshape *)hn_arena_alloc(a, sizeof(lshape) * (size_t)cap);
    memset(list, 0, sizeof(lshape) * (size_t)cap);
    int n = 0;
    for (int i = 0; i < cap; i++) {
        const hn_json *lj = hn_json_at(comp_layers, i);
        if (!lj || lj->kind != HN_JSON_OBJ) continue;
        int ty = (int)hn_json_num(hn_json_obj_get(lj, "ty"), 4);
        if (ty == 1 || ty == 2) continue;      /* 纯色/图片层在组里无对应物 */
        lshape *g = &list[n];
        memset(g, 0, sizeof(*g));
        g->kind = L_GROUP;
        const hn_json *ks = hn_json_obj_get(lj, "ks");
        if (!ks) ks = lj;
        parse_prop(a, hn_json_obj_get(ks, "a"), &g->anchor);
        parse_prop(a, hn_json_obj_get(ks, "p"), &g->tpos);
        parse_prop(a, hn_json_obj_get(ks, "s"), &g->scale);
        parse_prop(a, hn_json_obj_get(ks, "r"), &g->rot);
        parse_prop(a, hn_json_obj_get(ks, "o"), &g->opacity);
        g->n_kids = parse_items(a, hn_json_obj_get(lj, "shapes"), &g->kids);
        float mpf = 1000.0f / fr;
        norm_prop_time(&g->anchor, mpf); norm_prop_time(&g->tpos, mpf);
        norm_prop_time(&g->scale, mpf);  norm_prop_time(&g->rot, mpf);
        norm_prop_time(&g->opacity, mpf);
        for (int q = 0; q < g->n_kids; q++) norm_shape_time(&g->kids[q], mpf);
        n++;
    }
    *out = list;
    return n;
}

static void parse_item(hn_arena *a, const hn_json *it, lshape *sh) {
    memset(sh, 0, sizeof(*sh));
    sh->kind = -1;
    const char *ty = hn_json_str(hn_json_obj_get(it, "ty"), "");
    if (!strcmp(ty, "gr")) {
        sh->kind = L_GROUP;
        parse_transform(a, hn_json_obj_get(it, "tr"), sh);
        sh->n_kids = parse_items(a, hn_json_obj_get(it, "it"), &sh->kids);
    } else if (!strcmp(ty, "rc")) {
        sh->kind = L_RECT;
        parse_prop(a, hn_json_obj_get(it, "p"), &sh->pos);
        parse_prop(a, hn_json_obj_get(it, "s"), &sh->size);
        parse_prop(a, hn_json_obj_get(it, "r"), &sh->round);
    } else if (!strcmp(ty, "el")) {
        sh->kind = L_ELLIPSE;
        parse_prop(a, hn_json_obj_get(it, "p"), &sh->pos);
        parse_prop(a, hn_json_obj_get(it, "s"), &sh->size);
    } else if (!strcmp(ty, "sh")) {
        sh->kind = L_PATH;
        const hn_json *ks = hn_json_obj_get(it, "ks");
        if (ks && ks->kind == HN_JSON_OBJ) {
            const hn_json *k = hn_json_obj_get(ks, "k");
            hn_json *f0 = (k && k->kind == HN_JSON_ARR) ? hn_json_at(k, 0) : NULL;
            if (f0 && f0->kind == HN_JSON_OBJ) {
                /* 路径变形关键帧 */
                int cnt = k->count;
                sh->pkf = (lpkf *)hn_arena_alloc(a, sizeof(lpkf) * (size_t)cnt);
                memset(sh->pkf, 0, sizeof(lpkf) * (size_t)cnt);
                for (int i = 0; i < cnt; i++) {
                    hn_json *e = hn_json_at(k, i);
                    sh->pkf[i].t = (float)hn_json_num(hn_json_obj_get(e, "t"), 0);
                    hn_json *s0 = hn_json_at(hn_json_obj_get(e, "s"), 0);
                    lshape tmp;
                    memset(&tmp, 0, sizeof(tmp));
                    if (s0) parse_path_geom(a, s0, &tmp);
                    int n = tmp.n_verts;
                    sh->pkf[i].n = n;
                    sh->pkf[i].data = (float *)hn_arena_alloc(a, sizeof(float) * (size_t)(n * 6 + 8));
                    if (n > 0) {
                        if (tmp.verts)   memcpy(sh->pkf[i].data,         tmp.verts,   sizeof(float) * (size_t)(n * 2));
                        if (tmp.tan_in)  memcpy(sh->pkf[i].data + n * 2, tmp.tan_in,  sizeof(float) * (size_t)(n * 2));
                        if (tmp.tan_out) memcpy(sh->pkf[i].data + n * 4, tmp.tan_out, sizeof(float) * (size_t)(n * 2));
                    }
                    if (i == 0) { sh->closed = tmp.closed; sh->n_verts = n; }
                }
                sh->n_pkf = cnt;
            } else {
                /* 非动画路径: 几何在 **ks.k** 里, 不是 ks 自身。
                   ks 是 {"a":0,"k":{c,v,i,o}} 这样的包装层, 直接拿 ks 去读
                   v/i/o 全是 NULL → n_verts=0 → path_points 直接 return,
                   整条路径什么都不画。这是**静态 sh 路径完全不显示**的原因
                   (Lottie 内容里绝大多数 sh 都是静态路径 + 图层变换带动)。
                   变形关键帧那条分支取的是 s[0] 也就是真正的路径对象, 所以
                   只有它会工作 —— 两处口径不一致正是这个 bug 的来源。 */
                if (k && k->kind == HN_JSON_OBJ) parse_path_geom(a, k, sh);
            }
        }
    } else if (!strcmp(ty, "fl")) {
        sh->kind = L_FILL;
        parse_prop(a, hn_json_obj_get(it, "c"), &sh->color);
        parse_prop(a, hn_json_obj_get(it, "o"), &sh->opacity);
        sh->fill_rule = ((int)hn_json_num(hn_json_obj_get(it, "r"), 1) == 2) ? 2 : 1;
    } else if (!strcmp(ty, "st")) {
        sh->kind = L_STROKE;
        parse_prop(a, hn_json_obj_get(it, "c"), &sh->color);
        parse_prop(a, hn_json_obj_get(it, "o"), &sh->opacity);
        parse_prop(a, hn_json_obj_get(it, "w"), &sh->width);
    } else if (!strcmp(ty, "gf") || !strcmp(ty, "gs")) {
        /* 渐变填充/描边: 多 stop 线性渐变。s/e 是**形状局部坐标**的轴两端
           (可动画), g.k 是 stop 数组 [{t:位置, s:[r,g,b,a]}, ...]。
           t=2(径向)不做形状化, 取 stop 中位色退化为纯色填充/描边。 */
        sh->kind = (!strcmp(ty, "gf")) ? L_FILL_GRAD : L_STROKE_GRAD;
        parse_prop(a, hn_json_obj_get(it, "s"), &sh->gp0);
        parse_prop(a, hn_json_obj_get(it, "e"), &sh->gp1);
        parse_prop(a, hn_json_obj_get(it, "o"), &sh->opacity);
        parse_prop(a, hn_json_obj_get(it, "w"), &sh->width);
        sh->gtype = ((int)hn_json_num(hn_json_obj_get(it, "t"), 1) == 2) ? 2 : 1;
        /* g.k 有两种真实编码, 都要吃下:
             (a) 新式: [{t:pos, s:[r,g,b(,a)]}, ...]
             (b) 旧式(bodymovin 至今仍大量导出): 扁平数字数组
                 [pos, r, g, b, pos, r, g, b, ...] —— g.p 是 stop 数。
           只认其中一种会让另一类文件的渐变整个消失(静默空白)。 */
        const hn_json *g = hn_json_obj_get(it, "g");
        const hn_json *k = g ? hn_json_obj_get(g, "k") : NULL;
        if (k && k->kind == HN_JSON_OBJ) k = hn_json_obj_get(k, "k");   /* {a,k} 包装 */
        if (k && k->kind == HN_JSON_ARR && k->count > 0) {
            hn_json *e0 = hn_json_at(k, 0);
            if (e0 && e0->kind == HN_JSON_OBJ) {
                int ns = k->count;
                if (ns > LOTTIE_MAX_GSTOPS) ns = LOTTIE_MAX_GSTOPS;
                int n = 0;
                for (int i = 0; i < ns; i++) {
                    const hn_json *st_e = hn_json_at(k, i);
                    if (!st_e || st_e->kind != HN_JSON_OBJ) continue;
                    parse_prop(a, hn_json_obj_get(st_e, "t"), &sh->gsp[n]);
                    parse_prop(a, hn_json_obj_get(st_e, "s"), &sh->gsc[n]);
                    n++;
                }
                sh->n_gstops = n;
            } else if (e0 && (e0->kind == HN_JSON_NUM || e0->kind == HN_JSON_BOOL)) {
                int total = k->count / 4;
                if (total > LOTTIE_MAX_GSTOPS) total = LOTTIE_MAX_GSTOPS;
                int n = 0;
                for (int i = 0; i < total; i++) {
                    const hn_json *pe = hn_json_at(k, i * 4);
                    lprop pv;
                    memset(&pv, 0, sizeof(pv));
                    pv.v[0] = (float)hn_json_num(pe, 0);
                    pv.ncomp = 1;
                    sh->gsp[n] = pv;
                    memset(&sh->gsc[n], 0, sizeof(lprop));
                    for (int c = 0; c < 3; c++)
                        sh->gsc[n].v[c] = (float)hn_json_num(hn_json_at(k, i * 4 + 1 + c), 0);
                    sh->gsc[n].v[3] = 1.0f;
                    sh->gsc[n].ncomp = 4;
                    n++;
                }
                sh->n_gstops = n;
            }
        }
        if (sh->n_gstops > 0) sh->has_grad = 1;
    } else if (!strcmp(ty, "tr")) {
        sh->kind = L_TRANSFORM;
        parse_transform(a, it, sh);
    } else if (!strcmp(ty, "rp")) {
        /* repeater: c=次数 o=起始副本偏移 m=1 正常 2 反向。
           tr 给出每个副本的增量变换(常见的是位移)。 */
        sh->kind = L_REPEATER;
        sh->has_rep = 1;
        /* c 既可能是静态数字("c":3), 也可能是动画属性({"a":0,"k":3})。
           两种都得认 —— 否则 bodymovin 导出的带动画副本数的文件会退化成 1 份。 */
        const hn_json *cc = hn_json_obj_get(it, "c");
        if (cc && cc->kind == HN_JSON_OBJ) {
            parse_prop(a, cc, &sh->rep_count_prop);
            sh->rep_count = 1.0f;
        } else {
            sh->rep_count = (float)hn_json_num(cc, 1);
        }
        const hn_json *tr = hn_json_obj_get(it, "tr");
        if (tr) parse_prop(a, hn_json_obj_get(tr, "p"), &sh->rep_pos);
    } else if (!strcmp(ty, "tm")) {
        /* trim path: s/e/o 都是 0..100 的百分比属性。整个组的绘制范围
           由它裁剪 —— 所以单独存下来, 不并入 L_TRANSFORM。 */
        sh->kind = L_TRIM;
        parse_prop(a, hn_json_obj_get(it, "s"), &sh->trim_start);
        parse_prop(a, hn_json_obj_get(it, "e"), &sh->trim_end);
        parse_prop(a, hn_json_obj_get(it, "o"), &sh->trim_offset);
        sh->has_trim = 1;
    }
}

static int parse_items(hn_arena *a, const hn_json *arr, lshape **out) {
    *out = NULL;
    if (!arr || arr->kind != HN_JSON_ARR || arr->count <= 0) return 0;
    int cap = arr->count;
    lshape *list = (lshape *)hn_arena_alloc(a, sizeof(lshape) * (size_t)cap);
    int n = 0;
    for (int i = 0; i < cap; i++) {
        hn_json *it = hn_json_at(arr, i);
        if (!it || it->kind != HN_JSON_OBJ) continue;
        parse_item(a, it, &list[n]);
        if (list[n].kind >= 0) n++;
    }
    *out = list;
    return n;
}

/* ---------------- dotLottie(.lottie)ZIP 包 ----------------
 *
 * .lottie 是 ZIP 容器(dotLottie 规范 v1: manifest.json 声明元信息,
 * animations/<id>.json 是各动画本体)。这里做**最小 ZIP 读取**:
 * 不落盘、不建临时文件、零外部依赖 —— 解压复用 hn_png.c 的 hn_inflate
 * (PNG IDAT 与 ZIP method=8 都是 RFC 1951 裸 deflate, 同一份实现)。
 *
 * 结构(APPNOTE.TXT):
 *   [本地文件头 PK\x03\x04 + 数据] ... [中央目录 PK\x01\x02 ...] [EOCD PK\x05\x06]
 * 读取顺序与写入相反: 从尾部扫 EOCD → 拿中央目录位置/条目数 → 遍历中央
 * 目录拿各条目的名字/压缩方法/尺寸/本地头偏移 → 按需从本地头取数据。
 * 不做的(从简且与规范无冲突): ZIP64(单动画 JSON 远小于 4GB)、加密、
 * 分卷、数据描述符依赖(中央目录里的尺寸是权威值)。
 */

#define HN_ZIP_EOCD_SIG  0x06054b50u
#define HN_ZIP_CDIR_SIG  0x02014b50u
#define HN_ZIP_LFH_SIG   0x04034b50u

static unsigned rd_le16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static unsigned rd_le32(const unsigned char *p) {
    return p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}

/* ZIP 中央目录条目(只留取动画需要的字段) */
typedef struct {
    const char *name;        /* 指向包内字节(不 NUL 结尾, 用 name_len) */
    unsigned    name_len;
    unsigned    method;      /* 0=存储 8=deflate */
    unsigned    comp_size;
    unsigned    lfh_off;     /* 本地文件头在包内的偏移 */
} zip_entry;

typedef struct {
    const unsigned char *buf;
    size_t               n;
    zip_entry           *ents;      /* malloc 数组, hn_zip_free 释放 */
    int                  n_ents;
} zip_file;

/* 释放中央目录条目表。zip_file 本体是调用方的栈变量, 这里只 dispose 它
   持有的堆数组(不能连 z 一起 free —— 那是栈指针) */
static void hn_zip_free(zip_file *z) {
    if (!z) return;
    free(z->ents);
    z->ents = NULL;
    z->n_ents = 0;
}

/* EOCD 定位: 从尾部倒扫签名(注释区最长 65535, 扫 22+65535 字节足够) */
static int hn_zip_open(const unsigned char *buf, size_t n, zip_file *z) {
    memset(z, 0, sizeof(*z));
    if (n < 22) return 0;
    size_t scan_max = n - 22;
    size_t scan_floor = scan_max > 65557 ? scan_max - 65557 : 0;
    size_t e = scan_max + 1;
    while (e-- > scan_floor) {
        if (rd_le32(buf + e) == HN_ZIP_EOCD_SIG) break;
    }
    if (e < scan_floor || rd_le32(buf + e) != HN_ZIP_EOCD_SIG) return 0;
    int total = (int)rd_le16(buf + e + 10);         /* 全部条目数 */
    size_t cd_off = rd_le32(buf + e + 16);
    size_t cd_size = rd_le32(buf + e + 12);
    if (cd_off + (size_t)cd_size > n || total <= 0) return 0;
    z->buf = buf; z->n = n;
    z->ents = (zip_entry *)calloc((size_t)total, sizeof(zip_entry));
    if (!z->ents) return 0;
    size_t p = cd_off, end = cd_off + cd_size;
    for (int i = 0; i < total && p + 46 <= end; i++) {
        if (rd_le32(buf + p) != HN_ZIP_CDIR_SIG) break;
        zip_entry *e2 = &z->ents[z->n_ents++];
        e2->method    = rd_le16(buf + p + 10);
        e2->comp_size = rd_le32(buf + p + 20);
        e2->lfh_off   = rd_le32(buf + p + 42);
        e2->name_len  = rd_le16(buf + p + 28);
        e2->name      = (const char *)buf + p + 46;
        p += 46 + e2->name_len + rd_le16(buf + p + 30) + rd_le16(buf + p + 32);
    }
    return z->n_ents > 0;
}

static int ent_is(const zip_entry *e, const char *name) {
    size_t l = strlen(name);
    return e->name_len == l && !memcmp(e->name, name, l);
}

/* 提取条目内容(malloc, 调用方 free)。失败返回 NULL。 */
static unsigned char *hn_zip_extract(const zip_file *z, const zip_entry *e,
                                     size_t *out_len) {
    *out_len = 0;
    if (e->lfh_off + 30 > z->n) return NULL;
    const unsigned char *lfh = z->buf + e->lfh_off;
    if (rd_le32(lfh) != HN_ZIP_LFH_SIG) return NULL;
    /* 数据区 = 本地头 30 + 名字 + 额外字段(本地头的 extra 可能与中央
       目录不同, 以本地头自己的字段为准) */
    size_t name_len = rd_le16(lfh + 26), extra_len = rd_le16(lfh + 28);
    size_t data_off = e->lfh_off + 30 + name_len + extra_len;
    if (data_off + e->comp_size > z->n) return NULL;
    const unsigned char *data = z->buf + data_off;
    if (e->method == 0) {                           /* 存储 */
        unsigned char *out = (unsigned char *)malloc(e->comp_size + 1);
        if (!out) return NULL;
        memcpy(out, data, e->comp_size);
        out[e->comp_size] = 0;
        *out_len = e->comp_size;
        return out;
    }
    if (e->method != 8) return NULL;                /* 其它方法不支持 */
    unsigned char *out = hn_inflate(data, e->comp_size, out_len);
    if (out) {                                      /* 补 NUL 便于直接当字符串解析 */
        unsigned char *n2 = (unsigned char *)realloc(out, *out_len + 1);
        if (!n2) { free(out); *out_len = 0; return NULL; }
        out = n2;
        out[*out_len] = 0;
    }
    return out;
}

/* 选出要播的动画 JSON:
   1) 有 manifest.json → 解析, 取 animations[0].id → "animations/<id>.json"
      (dotLottie 规范: manifest.animations[] 的次序就是播放次序, 首个为
      缺省动画; hn-lottie-index 多选一暂不做 —— 最简口径是恒取首个)。
   2) 无 manifest(或解析失败) → 回退为第一个 animations/ 目录下的 .json 条目。 */
static int hn_zip_pick_animation(const zip_file *z, int *idx_out) {
    *idx_out = -1;
    int fallback = -1;
    for (int i = 0; i < z->n_ents; i++) {
        const zip_entry *e = &z->ents[i];
        if (ent_is(e, "manifest.json")) {
            size_t mn = 0;
            unsigned char *mb = hn_zip_extract(z, e, &mn);
            if (mb) {
                void *ja = NULL;
                hn_json *root = hn_json_parse((char *)mb, mn, &ja);
                const hn_json *anims = root ? hn_json_obj_get(root, "animations") : NULL;
                if (anims && anims->kind == HN_JSON_ARR && anims->count > 0) {
                    const hn_json *a0 = hn_json_at(anims, 0);
                    const char *id = hn_json_str(hn_json_obj_get(a0, "id"), NULL);
                    /* 目录名固定 "animations/"(规范 §Structure); id 即文件名 */
                    if (id && *id) {
                        char want[160];
                        snprintf(want, sizeof(want), "animations/%s.json", id);
                        for (int k = 0; k < z->n_ents; k++)
                            if (ent_is(&z->ents[k], want)) { *idx_out = k; break; }
                    }
                }
                hn_json_free(ja);
                free(mb);
                if (*idx_out >= 0) return 1;
            }
        }
        /* 最短合法名 = "animations/"(11) + 文件名(≥1) + ".json"(5) = 17;
           前后缀判据在这里, 长度只做下限保护 */
        if (fallback < 0 && e->name_len >= 17 &&
            !memcmp(e->name, "animations/", 11) &&
            !memcmp(e->name + e->name_len - 5, ".json", 5))
            fallback = i;
    }
    if (fallback >= 0) { *idx_out = fallback; return 1; }
    return 0;
}

/* .lottie 字节 → 动画 JSON 文本(malloc, 调用方 free)。非 ZIP 返回 NULL。 */
static char *hn_dotlottie_json(const unsigned char *buf, size_t n, size_t *json_len) {
    zip_file z;
    if (!hn_zip_open(buf, n, &z)) return NULL;
    char *json = NULL;
    int idx = -1;
    if (hn_zip_pick_animation(&z, &idx) && idx >= 0)
        json = (char *)hn_zip_extract(&z, &z.ents[idx], json_len);
    hn_zip_free(&z);
    return json;
}

/* ---------------- 加载 ---------------- */

struct hn_lottie *hn_lottie_load(hn_context *c, const char *path) {
    if (!c || !path) return NULL;
    const hn_asset_backend *ab = hn_context_assets(c);
    if (!ab || !ab->load) return NULL;
    size_t len = 0;
    const char *bytes = ab->load(ab->ctx, path, &len);
    if (!bytes || len == 0) return NULL;

    /* dotLottie(.lottie): ZIP 容器 → 解出动画 JSON 后走同一 JSON 管线。
       识别靠**文件头 PK 魔数**(规范并允许任意扩展名, 魔数比后缀更可靠);
       失败(坏包/无动画)返回 NULL, 调用方落到占位路径让问题可见。 */
    char *buf = NULL;
    char *zjson = NULL;
    void *ja = NULL;
    hn_json *root = NULL;
    if (len >= 4 && !memcmp(bytes, "PK\x03\x04", 4)) {
        size_t zjn = 0;
        zjson = hn_dotlottie_json((const unsigned char *)bytes, len, &zjn);
        if (!zjson) return NULL;
        root = hn_json_parse(zjson, zjn, &ja);
        if (!root) { free(zjson); return NULL; }
    } else {
        buf = (char *)malloc(len + 1);      /* 就地解析会覆盖原字节 */
        if (!buf) return NULL;
        memcpy(buf, bytes, len);
        buf[len] = 0;
        root = hn_json_parse(buf, len, &ja);
        if (!root) { free(buf); return NULL; }
    }

    struct hn_lottie *l = (struct hn_lottie *)calloc(1, sizeof(*l));
    if (!l) { hn_json_free(ja); free(zjson); free(buf); return NULL; }
    l->json_arena = ja;
    l->json_buf = zjson ? zjson : buf;   /* 两者只有一个非 NULL, free() 统一释放 */
    l->w = (float)hn_json_num(hn_json_obj_get(root, "w"), 0);
    l->h = (float)hn_json_num(hn_json_obj_get(root, "h"), 0);
    float fr = (float)hn_json_num(hn_json_obj_get(root, "fr"), 60);
    if (fr < 1) fr = 60;
    float ip = (float)hn_json_num(hn_json_obj_get(root, "ip"), 0);
    float op = (float)hn_json_num(hn_json_obj_get(root, "op"), 0);
    if (op > ip) l->duration_ms = (op - ip) * 1000.0f / fr;

    l->arena = hn_arena_create();
    hn_arena *a = l->arena;
    const hn_json *layers = hn_json_obj_get(root, "layers");
    if (layers && layers->kind == HN_JSON_ARR && layers->count > 0) {
        int cnt = layers->count;
        l->layers = (llayer *)hn_arena_alloc(a, sizeof(llayer) * (size_t)cnt);
        memset(l->layers, 0, sizeof(llayer) * (size_t)cnt);
        int n = 0;
        for (int i = 0; i < cnt; i++) {
            hn_json *lj = hn_json_at(layers, i);
            if (!lj || lj->kind != HN_JSON_OBJ) continue;
            llayer *ly = &l->layers[n];
            int ty = (int)hn_json_num(hn_json_obj_get(lj, "ty"), 4);
            ly->kind = (ty == 2) ? 2 : (ty == 1 ? 1 : 0);   /* 0/4 都走形状层路径 */
            ly->start_ms = ((float)hn_json_num(hn_json_obj_get(lj, "ip"), ip) - ip) * 1000.0f / fr;
            ly->end_ms   = ((float)hn_json_num(hn_json_obj_get(lj, "op"), op) - ip) * 1000.0f / fr;
            ly->w = (float)hn_json_num(hn_json_obj_get(lj, "w"), l->w);
            ly->h = (float)hn_json_num(hn_json_obj_get(lj, "h"), l->h);
            if (ly->kind == 2) ly->image_src = hn_json_str(hn_json_obj_get(lj, "refId"), NULL);
            if (ly->kind == 1) {
                int m = 0;
                float *sc = flatten_num(a, hn_json_obj_get(lj, "sc"), &m);
                if (sc && m >= 3) ly->solid_color = lot_color(sc, 1.0f);
            }
            /* 图层变换: Lottie 把 a/p/s/r/o 统一放在 "ks" 下(不是图层顶层)。
               早期实现直接在图层上找, 于是位置恒为 (0,0)、缩放恒为 100%
               —— 表现为所有形状堆在画布左上角(几何正确但整体错位)。 */
            const hn_json *ks = hn_json_obj_get(lj, "ks");
            if (!ks) ks = lj;                  /* 少数导出省略 ks 层 */
            parse_prop(a, hn_json_obj_get(ks, "a"), &ly->anchor);
            parse_prop(a, hn_json_obj_get(ks, "p"), &ly->pos);
            parse_prop(a, hn_json_obj_get(ks, "s"), &ly->scale);
            parse_prop(a, hn_json_obj_get(ks, "r"), &ly->rot);
            parse_prop(a, hn_json_obj_get(ks, "o"), &ly->opacity);
            if (ty == 0) {
                /* 预合成: 展开 asset 的内层图层 */
                const char *ref = hn_json_str(hn_json_obj_get(lj, "refId"), NULL);
                const hn_json *as = find_asset(root, ref);
                ly->n_shapes = as
                    ? parse_precomp(a, hn_json_obj_get(as, "layers"), fr, ip, &ly->shapes)
                    : 0;
            } else {
                ly->n_shapes = parse_items(a, hn_json_obj_get(lj, "shapes"), &ly->shapes);
            }
            /* mask 最小版: 取第一条 mode="a"(加法)且 inv=false 的蒙版路径。
               其余模式(相减/相交/反相)与后续蒙版跳过 —— 绘制期用该路径的
               外接矩形做 CLIP_PUSH(矩形蒙版精确, 曲线蒙版是外接盒近似)。 */
            const hn_json *masks = hn_json_obj_get(lj, "masksProperties");
            if (masks && masks->kind == HN_JSON_ARR) {
                for (int q = 0; q < masks->count && !ly->has_mask; q++) {
                    const hn_json *m = hn_json_at(masks, q);
                    if (!m || m->kind != HN_JSON_OBJ) continue;
                    const char *mode = hn_json_str(hn_json_obj_get(m, "mode"), "a");
                    if (strcmp(mode, "a") != 0) continue;
                    if (hn_json_bool(hn_json_obj_get(m, "inv"), 0)) continue;
                    const hn_json *pt = hn_json_obj_get(m, "pt");
                    if (!pt || pt->kind != HN_JSON_OBJ) continue;
                    const hn_json *k = hn_json_obj_get(pt, "k");
                    if (!k) continue;
                    hn_json *f0 = (k->kind == HN_JSON_ARR) ? hn_json_at(k, 0) : NULL;
                    if (f0 && f0->kind == HN_JSON_OBJ) {
                        /* 蒙版路径变形关键帧: 与 sh 同一几何编码 */
                        int cnt = k->count;
                        ly->mask_pkf = (lpkf *)hn_arena_alloc(a, sizeof(lpkf) * (size_t)cnt);
                        memset(ly->mask_pkf, 0, sizeof(lpkf) * (size_t)cnt);
                        for (int j = 0; j < cnt; j++) {
                            hn_json *e = hn_json_at(k, j);
                            ly->mask_pkf[j].t = (float)hn_json_num(hn_json_obj_get(e, "t"), 0);
                            hn_json *s0 = hn_json_at(hn_json_obj_get(e, "s"), 0);
                            lshape tmp;
                            memset(&tmp, 0, sizeof(tmp));
                            if (s0) parse_path_geom(a, s0, &tmp);
                            int nv = tmp.n_verts;
                            ly->mask_pkf[j].n = nv;
                            ly->mask_pkf[j].data = (float *)hn_arena_alloc(a, sizeof(float) * (size_t)(nv * 6 + 8));
                            if (nv > 0 && ly->mask_pkf[j].data) {
                                if (tmp.verts)   memcpy(ly->mask_pkf[j].data,         tmp.verts,   sizeof(float) * (size_t)(nv * 2));
                                if (tmp.tan_in)  memcpy(ly->mask_pkf[j].data + nv * 2, tmp.tan_in,  sizeof(float) * (size_t)(nv * 2));
                                if (tmp.tan_out) memcpy(ly->mask_pkf[j].data + nv * 4, tmp.tan_out, sizeof(float) * (size_t)(nv * 2));
                            }
                            if (j == 0) { ly->mask_closed = (unsigned char)tmp.closed; ly->n_mask_v = nv; }
                        }
                        ly->n_mask_pkf = cnt;
                        ly->has_mask = 1;
                    } else if (k->kind == HN_JSON_OBJ) {
                        lshape tmp;
                        memset(&tmp, 0, sizeof(tmp));
                        parse_path_geom(a, k, &tmp);
                        ly->mask_v = tmp.verts;
                        ly->mask_i = tmp.tan_in;
                        ly->mask_o = tmp.tan_out;
                        ly->n_mask_v = tmp.n_verts;
                        ly->mask_closed = (unsigned char)tmp.closed;
                        if (ly->n_mask_v > 0) ly->has_mask = 1;
                    }
                }
            }
            /* 关键帧时间: 帧 → 毫秒(整层归一) */
            float mpf = 1000.0f / fr;
            norm_prop_time(&ly->anchor, mpf);
            norm_prop_time(&ly->pos, mpf);
            norm_prop_time(&ly->scale, mpf);
            norm_prop_time(&ly->rot, mpf);
            norm_prop_time(&ly->opacity, mpf);
            for (int q = 0; q < ly->n_mask_pkf; q++) ly->mask_pkf[q].t *= mpf;
            for (int q = 0; q < ly->n_shapes; q++) norm_shape_time(&ly->shapes[q], mpf);
            n++;
        }
        l->n_layers = n;
    }
    if (l->n_layers == 0 || l->duration_ms <= 0) {
        hn_lottie_free(l);
        return NULL;
    }
    return l;
}

/* ---------------- 几何求值 ---------------- */

typedef struct { float *v; int n, cap; hn_arena *a; } vbuf;

static void vb_push(vbuf *b, float x, float y) {
    if (b->n >= LOTTIE_MAX_VERTS) return;
    if (b->n == b->cap) {
        int nc = b->cap ? b->cap * 2 : 64;
        float *nv = (float *)hn_arena_alloc(b->a, sizeof(float) * (size_t)(nc * 2));
        if (b->v && b->n) memcpy(nv, b->v, sizeof(float) * (size_t)(b->n * 2));
        b->v = nv; b->cap = nc;
    }
    b->v[b->n * 2] = x; b->v[b->n * 2 + 1] = y;
    b->n++;
}

typedef struct { float a, b, c, d, e, f; } lmat;

/* 行向量约定: p' = p · M。lm_mul(m, o) = "**先** m, **后** o"(即 m·o)。
   父子链的合成顺序必须是"先子后父": 子节点坐标先经自身变换, 再经父级
   变换到上层空间。写成 lm_mul(parent, child) 会把父变换套在内层,
   表现为"旋转的子节点绕着错误的原点飞出去"。 */
static lmat lm_mul(lmat m, lmat o) {
    lmat r;
    r.a = m.a * o.a + m.b * o.c;
    r.b = m.a * o.b + m.b * o.d;
    r.c = m.c * o.a + m.d * o.c;
    r.d = m.c * o.b + m.d * o.d;
    r.e = m.e * o.a + m.f * o.c + o.e;
    r.f = m.e * o.b + m.f * o.d + o.f;
    return r;
}

static void lm_apply(lmat m, float x, float y, float *ox, float *oy) {
    *ox = m.a * x + m.c * y + m.e;
    *oy = m.b * x + m.d * y + m.f;
}

/* 锚点/位置/缩放/旋转 → 仿射矩阵 */
static lmat lm_from(const lprop *anchor, const lprop *pos, const lprop *scale,
                    const lprop *rot, float t) {
    float av[4] = { 0, 0, 0, 0 }, pv[4] = { 0, 0, 0, 0 }, rv[4] = { 0, 0, 0, 0 };
    float sv[4] = { 100, 100, 100, 100 };
    lprop_at(anchor, t, av);
    lprop_at(pos, t, pv);
    lprop_at(rot, t, rv);
    if (scale->ncomp || scale->n_kf) lprop_at(scale, t, sv);
    float sxx = sv[0] / 100.0f, syy = sv[1] / 100.0f;
    float ang = (float)(rv[0] * M_PI / 180.0);
    float cs = cosf(ang), sn = sinf(ang);
    lmat m;
    m.a = cs * sxx;   m.b = sn * sxx;
    m.c = -sn * syy;  m.d = cs * syy;
    float ax = av[0] * sxx, ay = av[1] * syy;
    m.e = pv[0] - (cs * ax - sn * ay);
    m.f = pv[1] - (sn * ax + cs * ay);
    return m;
}

/* 路径(含变形关键帧)展开为折线 */
static void path_points(hn_arena *a, const lshape *sh, float t, vbuf *out) {
    const float *V = NULL, *I = NULL, *O = NULL;
    int nv = sh->n_verts;
    if (sh->n_pkf > 0) {
        int i = 0;
        for (; i < sh->n_pkf - 1; i++)
            if (t >= sh->pkf[i].t && t <= sh->pkf[i + 1].t) break;
        if (i >= sh->n_pkf - 1) i = sh->n_pkf - 2;
        if (i < 0) i = 0;
        const lpkf *A = &sh->pkf[i], *B = &sh->pkf[i + 1];
        int n = HN_MIN(A->n, B->n);
        if (n <= 0) return;
        float span = B->t - A->t;
        float f = span > 0.0001f ? (t - A->t) / span : 0;
        if (f < 0) f = 0; else if (f > 1) f = 1;
        float *buf = (float *)hn_arena_alloc(a, sizeof(float) * (size_t)(n * 6));
        for (int k = 0; k < n * 6; k++) buf[k] = A->data[k] + (B->data[k] - A->data[k]) * f;
        V = buf; I = buf + n * 2; O = buf + n * 4;
        nv = n;
    } else {
        V = sh->verts; I = sh->tan_in; O = sh->tan_out;
    }
    if (!V || nv < 2) return;
    int last = sh->closed ? nv : nv - 1;
    for (int i = 0; i < last; i++) {
        int nx = (i + 1) % nv;
        float x0 = V[i * 2], y0 = V[i * 2 + 1];
        float x1 = V[nx * 2], y1 = V[nx * 2 + 1];
        float c1x = x0 + (O ? O[i * 2] : 0),   c1y = y0 + (O ? O[i * 2 + 1] : 0);
        float c2x = x1 + (I ? I[nx * 2] : 0),  c2y = y1 + (I ? I[nx * 2 + 1] : 0);
        if (fabsf(c1x - x0) < 0.01f && fabsf(c1y - y0) < 0.01f &&
            fabsf(c2x - x1) < 0.01f && fabsf(c2y - y1) < 0.01f) {
            vb_push(out, x0, y0);                 /* 直线段只推起点 */
            continue;
        }
        for (int s = 0; s < LOTTIE_SEG; s++) {
            float u = (float)s / (float)LOTTIE_SEG, mu = 1.0f - u;
            vb_push(out, mu * mu * mu * x0 + 3 * mu * mu * u * c1x +
                         3 * mu * u * u * c2x + u * u * u * x1,
                        mu * mu * mu * y0 + 3 * mu * mu * u * c1y +
                         3 * mu * u * u * c2y + u * u * u * y1);
        }
    }
    /* 开路径必须补上最后一个顶点: 上面的直线段优化只推**段起点**, 闭合路径
       靠首尾相接自然补齐, 开路径不会 —— 于是一条两点的直线只产出 1 个点,
       而 emit_paint 要求 n>=2, 整条线(以及它的描边)就被静默丢掉。
       这正是"开路径 + 描边什么都不显示"的原因。 */
    if (!sh->closed && nv > 0)
        vb_push(out, V[(nv - 1) * 2], V[(nv - 1) * 2 + 1]);
}

static void prim_points(const lshape *sh, float t, vbuf *out) {
    if (sh->kind == L_RECT) {
        float pv[4], sv[4];
        lprop_at(&sh->pos, t, pv);
        lprop_at(&sh->size, t, sv);
        float w = sv[0], h = sv[1];
        if (w <= 0 || h <= 0) return;
        float x = pv[0] - w * 0.5f, y = pv[1] - h * 0.5f;
        float r = lprop_at1(&sh->round, t, 0);
        if (r <= 0.01f) {
            vb_push(out, x, y);
            vb_push(out, x + w, y);
            vb_push(out, x + w, y + h);
            vb_push(out, x, y + h);
            return;
        }
        if (r > w * 0.5f) r = w * 0.5f;
        if (r > h * 0.5f) r = h * 0.5f;
        const float q = (float)(M_PI * 0.5);
        float ccx[4] = { x + w - r, x + r, x + r, x + w - r };
        float ccy[4] = { y + h - r, y + h - r, y + r, y + r };
        float a0[4]  = { 0, q, q * 2, q * 3 };
        for (int k = 0; k < 4; k++)
            for (int s = 0; s <= 6; s++) {
                float ang = a0[k] + q * (float)s / 6.0f;
                vb_push(out, ccx[k] + cosf(ang) * r, ccy[k] + sinf(ang) * r);
            }
        return;
    }
    if (sh->kind == L_ELLIPSE) {
        float pv[4], sv[4];
        lprop_at(&sh->pos, t, pv);
        lprop_at(&sh->size, t, sv);
        float rx = sv[0] * 0.5f, ry = sv[1] * 0.5f;
        if (rx <= 0 || ry <= 0) return;
        const int seg = 32;
        for (int i = 0; i < seg; i++) {
            float ang = 2.0f * (float)M_PI * (float)i / (float)seg;
            vb_push(out, pv[0] + cosf(ang) * rx, pv[1] + sinf(ang) * ry);
        }
    }
}

/* ---------------- 发射 ---------------- */

typedef struct { vbuf paths[LOTTIE_MAX_PATHS]; int n_paths; hn_arena *a; } pathbag;

static vbuf *bag_new(pathbag *b) {
    if (b->n_paths >= LOTTIE_MAX_PATHS) return NULL;
    vbuf *v = &b->paths[b->n_paths++];
    if (v->a) { v->n = 0; return v; }       /* 复用缓冲(容量保留) */
    memset(v, 0, sizeof(*v));
    v->a = b->a;
    return v;
}

/* 适配参数: 画布 → 元素盒。fill 用独立轴缩放, 其余等比。 */
typedef struct { float kx, ky, ox, oy; } lfit;

static lfit fit_of(const char *mode, float cw, float ch, float bw, float bh) {
    lfit f = { 1, 1, 0, 0 };
    if (cw <= 0 || ch <= 0 || bw <= 0 || bh <= 0) {
        f.kx = f.ky = 1; f.ox = f.oy = 0;
        return f;
    }
    if (mode && !strcmp(mode, "fill")) {
        f.kx = bw / cw; f.ky = bh / ch;
        f.ox = f.oy = 0;
        return f;
    }
    float kx = bw / cw, ky = bh / ch;
    float k = kx < ky ? kx : ky;                    /* contain */
    if (mode && !strcmp(mode, "cover")) k = kx > ky ? kx : ky;
    if (mode && !strcmp(mode, "none"))  k = 1;
    f.kx = f.ky = k;
    /* 居中: 在画布坐标系里偏移(除以 k 把盒偏移换算回画布空间) */
    f.ox = cw * 0.5f - bw / (2.0f * k);
    f.oy = ch * 0.5f - bh / (2.0f * k);
    return f;
}

/* 画刷: 把 bag 中 [from, n_paths) 的路径按 paint 发射 */
/* 0..100 的百分比标量(Lottie 的 trim s/e/o 都是这个量纲) */
static float norm_pct(float v) { return v; }

/* trim path 的采样区间(顶点计数近似, 见 emit_paint 内的说明)。
   返回 0 = 区间为空(不画), 否则 b0/b1 出参为保留的顶点下标区间 [b0, b1)。 */
static int trim_range(const vbuf *v, float tr_s, float tr_e, float tr_o,
                      int *b0, int *b1) {
    int n = v->n;
    *b0 = 0; *b1 = n;
    if (!(tr_s > 0.0f || tr_e < 100.0f)) return n >= 2;
    double total = 0;
    for (int q = 1; q < n; q++) {
        double dx = (double)v->v[q * 2] - (double)v->v[(q - 1) * 2];
        double dy = (double)v->v[q * 2 + 1] - (double)v->v[(q - 1) * 2 + 1];
        total += sqrt(dx * dx + dy * dy);
    }
    (void)total;
    double a0 = fmod(tr_s + tr_o, 100.0), a1 = fmod(tr_e + tr_o, 100.0);
    if (a0 < 0) a0 += 100.0;
    if (a1 < 0) a1 += 100.0;
    if (a1 <= a0) a1 += 100.0;              /* 跨 0 的情况: 绕一圈 */
    int i0 = (int)(a0 / 100.0 * (n - 1) + 0.5);
    int i1 = (int)(a1 / 100.0 * (n - 1) + 0.5);
    if (i0 < 0) i0 = 0;
    if (i1 > n - 1) i1 = n - 1;
    if (i1 <= i0) return 0;                 /* 区间为空: 不画 */
    *b0 = i0; *b1 = i1 + 1;
    return *b1 - *b0 >= 2;
}

/* 渐变 stop 求值: u ∈ [0,1] → 归一化 RGBA(线性插值)。
   Lottie 的 stop 位置一般是 0..1; 少数导出用 0..100 —— 有点越界就整体 /100。
   无 stop 返回 0(调用方退化)。 */
static int grad_color_at(const lshape *paint, float t, float u, float out[4]) {
    int ns = paint->n_gstops;
    if (ns <= 0) return 0;
    float pos[LOTTIE_MAX_GSTOPS];
    float col[LOTTIE_MAX_GSTOPS][4];
    float scale01 = 1.0f;
    for (int i = 0; i < ns; i++) {
        float pv[4], cv[4];
        lprop_at(&paint->gsp[i], t, pv);
        lprop_at(&paint->gsc[i], t, cv);
        pos[i] = pv[0];
        if (pos[i] > 1.5f) scale01 = 100.0f;
        col[i][0] = cv[0]; col[i][1] = cv[1]; col[i][2] = cv[2];
        col[i][3] = (cv[3] > 0.0001f ? cv[3] : 1.0f);
    }
    for (int i = 0; i < ns; i++) pos[i] /= scale01;
    /* stop 顺序未必有序: 按位置插入排序(数量 <= 16, 开销可忽略) */
    for (int i = 1; i < ns; i++) {
        float kp = pos[i], kc[4];
        memcpy(kc, col[i], sizeof(kc));
        int j = i - 1;
        while (j >= 0 && pos[j] > kp) {
            pos[j + 1] = pos[j];
            memcpy(col[j + 1], col[j], sizeof(kc));
            j--;
        }
        pos[j + 1] = kp;
        memcpy(col[j + 1], kc, sizeof(kc));
    }
    if (u <= pos[0]) { memcpy(out, col[0], sizeof(float) * 4); return 1; }
    if (u >= pos[ns - 1]) { memcpy(out, col[ns - 1], sizeof(float) * 4); return 1; }
    for (int i = 0; i < ns - 1; i++) {
        if (u < pos[i] || u > pos[i + 1]) continue;
        float span = pos[i + 1] - pos[i];
        float f = span > 0.0001f ? (u - pos[i]) / span : 0.0f;
        for (int k = 0; k < 4; k++) out[k] = col[i][k] + (col[i + 1][k] - col[i][k]) * f;
        return 1;
    }
    memcpy(out, col[ns - 1], sizeof(float) * 4);
    return 1;
}

/* 半平面裁剪(Sutherland–Hodgman): 保留 n·p <= d 一侧。in/out 为 x,y 交替。
   返回输出顶点数(可能为 0 = 整体在带外)。 */
static int clip_half(const float *in, int n, float nx, float ny, float d,
                     float *out) {
    int m = 0;
    for (int i = 0; i < n; i++) {
        const float *A = &in[i * 2];
        const float *B = &in[((i + 1) % n) * 2];
        float da = nx * A[0] + ny * A[1] - d;
        float db = nx * B[0] + ny * B[1] - d;
        if (da <= 0.0f && m < LOTTIE_MAX_VERTS) {
            out[m * 2] = A[0]; out[m * 2 + 1] = A[1]; m++;
        }
        if (((da < 0.0f && db > 0.0f) || (da > 0.0f && db < 0.0f)) &&
            m < LOTTIE_MAX_VERTS) {
            float f = da / (da - db);
            out[m * 2]     = A[0] + (B[0] - A[0]) * f;
            out[m * 2 + 1] = A[1] + (B[1] - A[1]) * f;
            m++;
        }
    }
    return m;
}

/* 渐变画刷(gf 填充 / gs 描边)。
   原理: 引擎的 POLYGON 指令没有渐变通道, 而所有后端都支持"任意多边形 + 填充色",
   因此在**形状局部空间**把几何按渐变轴切成 N 条色带, 每条色带取轴中点处的
   插值色 —— 后端零改动即得渐变观感(带数 32 时相邻色差 <= 3%, 目视连续)。
   gf: 用两条平行半平面把路径多边形裁成条带再发射(填充);
   gs: 把折线按段展开成小四边形(带端点外扩以防拐角漏缝), 逐段取色 ——
       拐角处是斜接近似而非圆角(CSS/Lottie 的 round join 不做)。 */
static void emit_grad(hn_context *c, hn_arena *a, pathbag *bag, const lshape *paint,
                      lmat m, float alpha, float sx, float sy, lfit f,
                      int from, float t, float tr_s, float tr_e, float tr_o,
                      int rep_c, float rpx, float rpy) {
    float ov[4];
    lprop_at(&paint->opacity, t, ov);
    float pop = (paint->opacity.ncomp || paint->opacity.n_kf) ? norm_op(ov[0]) : 1.0f;
    float aa = alpha * pop;
    if (aa <= 0.003f) return;
    if (paint->n_gstops <= 0) return;
    int is_stroke = (paint->kind == L_STROKE_GRAD);
    float sw = is_stroke ? lprop_at1(&paint->width, t, 0) : 0;
    if (is_stroke && sw <= 0.05f) return;
    /* 渐变轴(局部坐标)。径向(t=2)没有对应的几何切分方式: 等值线是圆环,
       单轮廓多边形表达不了 —— 退化为中位色(如实写明, 不硬做)。 */
    float av[4], ev[4];
    lprop_at(&paint->gp0, t, av);
    lprop_at(&paint->gp1, t, ev);
    float ax = av[0], ay = av[1], ex = ev[0], ey = ev[1];
    float dx = ex - ax, dy = ey - ay;
    float len2 = dx * dx + dy * dy;
    int solid_fallback = (paint->gtype == 2) || (len2 < 0.0001f);
    if (solid_fallback) {
        float mid[4];
        grad_color_at(paint, t, 0.5f, mid);
        hn_color col = lot_color(mid, aa);
        for (int rep = 0; rep < (rep_c > 0 ? rep_c : 1); rep++) {
            float rdx = rpx * (float)rep, rdy = rpy * (float)rep;
            for (int i = from; i < bag->n_paths; i++) {
                vbuf *v = &bag->paths[i];
                int b0, b1;
                if (!trim_range(v, tr_s, tr_e, tr_o, &b0, &b1)) continue;
                int cn = b1 - b0;
                float *out = (float *)hn_arena_alloc(a, sizeof(float) * (size_t)(cn * 2));
                if (!out) continue;
                for (int q = 0; q < cn; q++) {
                    float px, py;
                    lm_apply(m, v->v[(b0 + q) * 2], v->v[(b0 + q) * 2 + 1], &px, &py);
                    out[q * 2]     = (px - f.ox) * f.kx + sx + rdx;
                    out[q * 2 + 1] = (py - f.oy) * f.ky + sy + rdy;
                }
                hn_cmd cmd;
                memset(&cmd, 0, sizeof(cmd));
                cmd.kind = HN_CMD_POLYGON;
                cmd.poly = out;
                cmd.poly_n = cn;
                cmd.even_odd = (paint->fill_rule == 2);
                if (is_stroke) { cmd.stroke = col; cmd.stroke_w = sw * f.kx; }
                else cmd.fill = col;
                hn_paint_push(c, &cmd);
            }
        }
        return;
    }
    const int NB = LOTTIE_GRAD_BANDS;
    float *tmp1 = (float *)hn_arena_alloc(a, sizeof(float) * (size_t)(LOTTIE_MAX_VERTS * 2));
    float *tmp2 = (float *)hn_arena_alloc(a, sizeof(float) * (size_t)(LOTTIE_MAX_VERTS * 2));
    if (!tmp1 || !tmp2) return;
    float px0 = dx * ax + dy * ay;              /* d·p0 */
    for (int rep = 0; rep < (rep_c > 0 ? rep_c : 1); rep++) {
        float rdx = rpx * (float)rep, rdy = rpy * (float)rep;
        for (int i = from; i < bag->n_paths; i++) {
            vbuf *v = &bag->paths[i];
            int b0, b1;
            if (!trim_range(v, tr_s, tr_e, tr_o, &b0, &b1)) continue;
            int cn = b1 - b0;
            if (cn < 2) continue;
            if (is_stroke) {
                /* 描边: 逐段四边形(端点外扩 w/2, 让相邻四边形在拐角重叠)。
                   每段再细分 sub 份: 渐变沿路径推进时才有连续观感 ——
                   一条两点直线只有一段, 不细分就整条一个中位色(实测缺陷)。
                   sub 与色带数挂钩: 段少时细分多, 段多时每段一份即可。 */
                float hw = sw * 0.5f;
                int nseg = cn - 1;
                int sub = nseg > 0 ? NB / nseg : 1;
                if (sub < 1) sub = 1;
                if (sub > 32) sub = 32;
                for (int q = 0; q + 1 < cn; q++) {
                    float x0 = v->v[(b0 + q) * 2], y0 = v->v[(b0 + q) * 2 + 1];
                    float x1 = v->v[(b0 + q + 1) * 2], y1 = v->v[(b0 + q + 1) * 2 + 1];
                    float sdx = x1 - x0, sdy = y1 - y0;
                    float sl = sqrtf(sdx * sdx + sdy * sdy);
                    if (sl < 1e-5f) continue;
                    float ux = sdx / sl, uy = sdy / sl;
                    float nx = -uy * hw, ny = ux * hw;
                    for (int k = 0; k < sub; k++) {
                        float k0 = (float)k / (float)sub, k1 = (float)(k + 1) / (float)sub;
                        float ax = x0 + sdx * k0, ay = y0 + sdy * k0;
                        float bx = x0 + sdx * k1, by = y0 + sdy * k1;
                        float qx[4] = { ax - ux * hw + nx, bx + ux * hw + nx,
                                        bx + ux * hw - nx, ax - ux * hw - nx };
                        float qy[4] = { ay - uy * hw + ny, by + uy * hw + ny,
                                        by + uy * hw - ny, ay - uy * hw - ny };
                        float mid[4];
                        float uu = ((dx * (ax + bx) * 0.5f + dy * (ay + by) * 0.5f) - px0) / len2;
                        if (!grad_color_at(paint, t, uu, mid)) continue;
                        float *out = (float *)hn_arena_alloc(a, sizeof(float) * 8);
                        if (!out) continue;
                        for (int m2 = 0; m2 < 4; m2++) {
                            float px, py;
                            lm_apply(m, qx[m2], qy[m2], &px, &py);
                            out[m2 * 2]     = (px - f.ox) * f.kx + sx + rdx;
                            out[m2 * 2 + 1] = (py - f.oy) * f.ky + sy + rdy;
                        }
                        hn_cmd cmd;
                        memset(&cmd, 0, sizeof(cmd));
                        cmd.kind = HN_CMD_POLYGON;
                        cmd.poly = out;
                        cmd.poly_n = 4;
                        cmd.fill = lot_color(mid, aa);
                        hn_paint_push(c, &cmd);
                    }
                }
                continue;
            }
            /* 填充: 逐色带把多边形裁出来 */
            for (int k = 0; k < NB; k++) {
                float t0 = (float)k / (float)NB, t1 = (float)(k + 1) / (float)NB;
                int n1 = clip_half(&v->v[b0 * 2], cn, -dx, -dy, -(px0 + t0 * len2), tmp1);
                if (n1 < 3) continue;
                int n2 = clip_half(tmp1, n1, dx, dy, px0 + t1 * len2, tmp2);
                if (n2 < 3) continue;
                float mid[4];
                if (!grad_color_at(paint, t, (t0 + t1) * 0.5f, mid)) continue;
                float *out = (float *)hn_arena_alloc(a, sizeof(float) * (size_t)(n2 * 2));
                if (!out) continue;
                for (int q = 0; q < n2; q++) {
                    float px, py;
                    lm_apply(m, tmp2[q * 2], tmp2[q * 2 + 1], &px, &py);
                    out[q * 2]     = (px - f.ox) * f.kx + sx + rdx;
                    out[q * 2 + 1] = (py - f.oy) * f.ky + sy + rdy;
                }
                hn_cmd cmd;
                memset(&cmd, 0, sizeof(cmd));
                cmd.kind = HN_CMD_POLYGON;
                cmd.poly = out;
                cmd.poly_n = n2;
                cmd.even_odd = (paint->fill_rule == 2);
                cmd.fill = lot_color(mid, aa);
                hn_paint_push(c, &cmd);
            }
        }
    }
}

static void emit_paint(hn_context *c, hn_arena *a, pathbag *bag, const lshape *paint,
                       lmat m, float alpha, float sx, float sy, lfit f,
                       int from, float t, float tr_s, float tr_e, float tr_o,
                       int rep_c, float rpx, float rpy) {
    float ov[4];
    lprop_at(&paint->opacity, t, ov);
    float pop = (paint->opacity.ncomp || paint->opacity.n_kf) ? norm_op(ov[0]) : 1.0f;
    float aa = alpha * pop;
    if (aa <= 0.003f) return;
    float cv[4];
    lprop_at(&paint->color, t, cv);
    int is_stroke = (paint->kind == L_STROKE);
    float sw = is_stroke ? lprop_at1(&paint->width, t, 0) : 0;
    hn_color col = lot_color(cv, aa);
    /* repeater: 同一组路径按 tr 的位移重复 c 次。
       实现走"增量位移"而不是逐个复合 tr 的完整变换 —— 覆盖绝大多数
       (rp 的 tr 常见就是纯位移); 带缩放的 repeater 会用位移近似。 */
    for (int rep = 0; rep < (rep_c > 0 ? rep_c : 1); rep++) {
    float rep_dx = rpx * (float)rep, rep_dy = rpy * (float)rep;
    for (int i = from; i < bag->n_paths; i++) {
        vbuf *v = &bag->paths[i];
        if (v->n < 2) continue;
        int n = v->n;
        /* trim path: 按累计弧长比例取 [s, e) 区间(offset 平移起点)。
           实现走"顶点计数近似"而不是精确弧长参数化 —— 对 Lottie 常见的
           均匀细分折线两者差异在 1% 量级, 而精确弧长要为每条路径建前缀和,
           收益不成比例。 */
        int b0 = 0, b1 = n;
        if (!trim_range(v, tr_s, tr_e, tr_o, &b0, &b1)) continue;
        int cn = b1 - b0;
        if (cn < 2) continue;
        float *out = (float *)hn_arena_alloc(a, sizeof(float) * (size_t)(cn * 2));
        for (int q = 0; q < cn; q++) {
            float px, py;
            lm_apply(m, v->v[(b0 + q) * 2], v->v[(b0 + q) * 2 + 1], &px, &py);
            out[q * 2]     = (px - f.ox) * f.kx + sx + rep_dx;
            out[q * 2 + 1] = (py - f.oy) * f.ky + sy + rep_dy;
        }
        n = cn;
        hn_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.kind = HN_CMD_POLYGON;
        cmd.poly = out;
        cmd.poly_n = n;
        cmd.even_odd = (paint->fill_rule == 2);
        if (is_stroke) {
            cmd.stroke = col;
            cmd.stroke_w = sw * f.kx;
            if (cmd.stroke_w <= 0.05f) continue;
        } else {
            cmd.fill = col;
        }
        hn_paint_push(c, &cmd);
    }
    }
}

static void emit_group(hn_context *c, hn_arena *a, const lshape *items, int n,
                       float t, lmat parent, float alpha, float sx, float sy,
                       lfit f, pathbag *bag, int depth) {
    if (depth > LOTTIE_MAX_DEPTH || n <= 0) return;
    lmat gm = parent;
    float galpha = alpha;
    for (int i = 0; i < n; i++) {
        if (items[i].kind != L_TRANSFORM) continue;
        lmat tm = lm_from(&items[i].anchor, &items[i].tpos, &items[i].scale,
                          &items[i].rot, t);
        /* 合成顺序: 本组变换先作用, 再经父级 —— 即 lm_mul(本组, 累积)。
           写成 lm_mul(累积, 本组) 会把父变换夹在内层, 表现为
           "带旋转的子组绕错误原点飞出去"。 */
        gm = lm_mul(tm, gm);
        float ov[4];
        lprop_at(&items[i].opacity, t, ov);
        if (items[i].opacity.ncomp || items[i].opacity.n_kf) galpha *= norm_op(ov[0]);
        break;
    }
    int from = bag->n_paths;
    for (int i = 0; i < n; i++) {
        const lshape *s = &items[i];
        if (s->kind == L_RECT || s->kind == L_ELLIPSE) {
            vbuf *v = bag_new(bag);
            if (v) prim_points(s, t, v);
        } else if (s->kind == L_PATH) {
            vbuf *v = bag_new(bag);
            if (v) path_points(a, s, t, v);
        } else if (s->kind == L_GROUP) {
            /* **组自身的变换必须先作用**。预合成展开(parse_precomp)把内层
               图层的 ks(a/p/s/r/o)存进 L_GROUP 自身字段 —— 这里若只传父矩阵,
               内层图层的位置/锚点/缩放就整体丢失: 内容全部按局部坐标堆在
               画布原点附近, 表现为"contain 装盒后画到盒外/顶角"(实测
               500x690 的 emoji 装进 340x470 只剩原点附近一团)。
               常规组(gr)的变换是子项里的 L_TRANSFORM(下方统一处理),
               自身字段为零 → lm_from 恒等 → 两种来源在此自然汇合。
               合成顺序与 L_TRANSFORM 一致: 先本组, 后父级。 */
            lmat sm = lm_from(&s->anchor, &s->tpos, &s->scale, &s->rot, t);
            lmat cm = lm_mul(sm, gm);
            float galpha2 = galpha;
            float gov[4];
            lprop_at(&s->opacity, t, gov);
            if (s->opacity.ncomp || s->opacity.n_kf) galpha2 *= norm_op(gov[0]);
            emit_group(c, a, s->kids, s->n_kids, t, cm, galpha2, sx, sy, f, bag,
                       depth + 1);
        }
    }
    /* trim path: 组内的 tm 项作用于**本组全部路径**。多个 tm 取第一个
       (Lottie 实际也只允许一个)。 */
    float tr_s = 0.0f, tr_e = 100.0f, tr_o = 0.0f;
    for (int i = 0; i < n; i++) {
        if (items[i].kind != L_TRIM) continue;
        float sv[4];
        lprop_at(&items[i].trim_start, t, sv); tr_s = norm_pct(sv[0]);
        lprop_at(&items[i].trim_end,   t, sv); tr_e = norm_pct(sv[0]);
        lprop_at(&items[i].trim_offset,t, sv); tr_o = norm_pct(sv[0]);
        break;
    }
    float rep_c = 1.0f; float rpx = 0, rpy = 0; unsigned char has_rep = 0;
    for (int i = 0; i < n; i++) {
        if (items[i].kind != L_REPEATER) continue;
        has_rep = 1;
        if (items[i].rep_count_prop.n_kf || items[i].rep_count_prop.ncomp) {
            float cv2[4]; lprop_at(&items[i].rep_count_prop, t, cv2);
            rep_c = cv2[0] > 1 ? cv2[0] : 1;
        } else {
            rep_c = items[i].rep_count > 1 ? items[i].rep_count : 1;
        }
        float pv[4];
        lprop_at(&items[i].rep_pos, t, pv);
        rpx = pv[0]; rpy = pv[1];
        break;
    }
    for (int i = 0; i < n; i++) {
        const lshape *s = &items[i];
        if (s->kind == L_FILL || s->kind == L_STROKE)
            emit_paint(c, a, bag, s, gm, galpha, sx, sy, f, from, t,
                       tr_s, tr_e, tr_o, has_rep ? (int)rep_c : 1, rpx, rpy);
        else if (s->kind == L_FILL_GRAD || s->kind == L_STROKE_GRAD)
            emit_grad(c, a, bag, s, gm, galpha, sx, sy, f, from, t,
                      tr_s, tr_e, tr_o, has_rep ? (int)rep_c : 1, rpx, rpy);
    }
    bag->n_paths = from;
}

/* mask 最小版的裁剪矩形: 蒙版路径在**图层局部坐标**(与形状层几何同一空间,
   图层 ks 变换随后统一作用)。路径先展平为折线, 取外接矩形, 四角过图层
   矩阵取轴对齐 bbox —— 旋转图层下矩形蒙版依然贴着内容。返回 0 = 蒙版
   退化(无点/零面积) → 调用方不应裁剪(视作无蒙版, 保持内容可见而非全隐)。 */
static int mask_clip_rect(hn_arena *a, const llayer *ly, float t, lmat m,
                          lfit f, float sx, float sy,
                          float *cx, float *cy, float *cw, float *ch) {
    if (!ly->has_mask) return 0;
    vbuf bp;
    memset(&bp, 0, sizeof(bp));
    bp.a = a;
    if (ly->n_mask_pkf > 0) {
        int i = 0;
        for (; i < ly->n_mask_pkf - 1; i++)
            if (t >= ly->mask_pkf[i].t && t <= ly->mask_pkf[i + 1].t) break;
        if (i >= ly->n_mask_pkf - 1) i = ly->n_mask_pkf - 2;
        if (i < 0) i = 0;
        const lpkf *A = &ly->mask_pkf[i], *B = &ly->mask_pkf[i + 1];
        int n = HN_MIN(A->n, B->n);
        if (n <= 0) return 0;
        float span = B->t - A->t;
        float ff = span > 0.0001f ? (t - A->t) / span : 0;
        if (ff < 0) ff = 0; else if (ff > 1) ff = 1;
        float *buf = (float *)hn_arena_alloc(a, sizeof(float) * (size_t)(n * 6));
        if (!buf) return 0;
        for (int k = 0; k < n * 6; k++) buf[k] = A->data[k] + (B->data[k] - A->data[k]) * ff;
        lshape tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.verts = buf; tmp.tan_in = buf + n * 2; tmp.tan_out = buf + n * 4;
        tmp.n_verts = n; tmp.closed = ly->mask_closed;
        path_points(a, &tmp, t, &bp);
    } else if (ly->n_mask_v > 0) {
        lshape tmp;
        memset(&tmp, 0, sizeof(tmp));
        tmp.verts = ly->mask_v; tmp.tan_in = ly->mask_i; tmp.tan_out = ly->mask_o;
        tmp.n_verts = ly->n_mask_v; tmp.closed = ly->mask_closed;
        path_points(a, &tmp, t, &bp);
    }
    if (bp.n < 2) return 0;
    float mnx = 1e9f, mny = 1e9f, mxx = -1e9f, mxy = -1e9f;
    for (int i = 0; i < bp.n; i++) {
        float px, py;
        lm_apply(m, bp.v[i * 2], bp.v[i * 2 + 1], &px, &py);
        float gx = (px - f.ox) * f.kx + sx;
        float gy = (py - f.oy) * f.ky + sy;
        if (gx < mnx) mnx = gx;
        if (gx > mxx) mxx = gx;
        if (gy < mny) mny = gy;
        if (gy > mxy) mxy = gy;
    }
    if (mxx - mnx < 0.01f || mxy - mny < 0.01f) return 0;
    *cx = mnx; *cy = mny; *cw = mxx - mnx; *ch = mxy - mny;
    return 1;
}

/* ---------------- 对外绘制入口 ---------------- */

/* 按时间求值并发射指令。sx/sy 为元素盒绝对原点的滚动补偿,
   bw/bh 为元素盒尺寸, clock_ms 为动画时钟。 */
void hn_lottie_emit(hn_context *c, struct hn_lottie *l, float clock_ms,
                    float bw, float bh, float sx, float sy, float alpha,
                    const char *fit) {
    if (!c || !l || l->n_layers <= 0) return;
    float dur = l->duration_ms > 0 ? l->duration_ms : 1.0f;
    float t = clock_ms;
    /* 循环取模。用 >= 而非 >: t 恰好等于时长时应折回 0(Lottie 的语义是
       播放 [ip, op) 区间), 否则会在循环边界多停一帧、产生可见的顿挫。 */
    if (t >= dur) t = fmodf(t, dur);
    lfit f = fit_of(fit, l->w, l->h, bw, bh);
    hn_arena *a = hn_context_tmp(c);
    pathbag bag;
    memset(&bag, 0, sizeof(bag));
    bag.a = a;
    for (int i = l->n_layers - 1; i >= 0; i--) {   /* Lottie 图层序: 后声明者在下 */
        llayer *ly = &l->layers[i];
        if (t < ly->start_ms - 0.5f || t > ly->end_ms + 0.5f) continue;
        float ov[4];
        lprop_at(&ly->opacity, t, ov);
        float lo = (ly->opacity.ncomp || ly->opacity.n_kf) ? norm_op(ov[0]) : 1.0f;
        float la = alpha * lo;
        if (la <= 0.003f) continue;
        lmat m = lm_from(&ly->anchor, &ly->pos, &ly->scale, &ly->rot, t);
        if (ly->kind == 2) {                              /* 图片层 */
            if (!ly->image_src) continue;
            float x0, y0, x1, y1;
            lm_apply(m, 0, 0, &x0, &y0);
            lm_apply(m, ly->w, ly->h, &x1, &y1);
            hn_cmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.kind = HN_CMD_IMAGE;
            cmd.x = (x0 - f.ox) * f.kx + sx;
            cmd.y = (y0 - f.oy) * f.ky + sy;
            cmd.w = (x1 - x0) * f.kx;
            cmd.h = (y1 - y0) * f.ky;
            cmd.text = ly->image_src;
            cmd.text_len = strlen(ly->image_src);
            cmd.fill = 0xFFFFFF00u | (unsigned)(la * 255.0f);   /* 承载图层不透明度 */
            hn_paint_push(c, &cmd);
            continue;
        }
        if (ly->kind == 1) {                              /* 纯色层 */
            float x0, y0, x1, y1;
            lm_apply(m, 0, 0, &x0, &y0);
            lm_apply(m, ly->w, ly->h, &x1, &y1);
            hn_cmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.kind = HN_CMD_RECT;
            cmd.x = (x0 - f.ox) * f.kx + sx;
            cmd.y = (y0 - f.oy) * f.ky + sy;
            cmd.w = (x1 - x0) * f.kx;
            cmd.h = (y1 - y0) * f.ky;
            cmd.fill = (ly->solid_color & 0xFFFFFF00u) | (unsigned)(la * 255.0f);
            hn_paint_push(c, &cmd);
            continue;
        }
        if (!ly->shapes || ly->n_shapes <= 0) continue;
        /* mask 最小版: 内容发射前裁到蒙版外接矩形(CLIP_PUSH/POP 配对)。
           矩形蒙版精确; 曲线蒙版把少许边缘内容带进来 —— 如实写明。 */
        float mcx = 0, mcy = 0, mcw = 0, mch = 0;
        int mask_on = mask_clip_rect(a, ly, t, m, f, sx, sy, &mcx, &mcy, &mcw, &mch);
        if (mask_on) {
            hn_cmd cp;
            memset(&cp, 0, sizeof(cp));
            cp.kind = HN_CMD_CLIP_PUSH;
            cp.x = mcx; cp.y = mcy; cp.w = mcw; cp.h = mch;
            hn_paint_push(c, &cp);
        }
        bag.n_paths = 0;
        emit_group(c, a, ly->shapes, ly->n_shapes, t, m, la, sx, sy, f, &bag, 0);
        if (mask_on) {
            hn_cmd cp;
            memset(&cp, 0, sizeof(cp));
            cp.kind = HN_CMD_CLIP_POP;
            hn_paint_push(c, &cp);
        }
    }
}

/* ================= Live2D 运行时包(moc3 边界的诚实接入) =================
 *
 * Live2D 的 .moc3 是专有二进制(需要 Cubism SDK 商业授权, 既有契约: 不解),
 * 但它的**运行时包**里所有外围资源都是公开 JSON:
 *   model3.json  FileReferences: Moc/Textures/Physics/DisplayInfo/Motions 组
 *   cdi3.json    DisplayInfo: 参数清单(Id + 日文显示名)
 *   physics3.json  物理设置(Meta 计数; v1 只解析计数, 不做仿真)
 *   motion3.json   动作曲线(v2.4: Curves[].Id + Segments)
 * Neb 的做法: 解析这些公开数据, 把 motion3 曲线**采样后喂给自有 rig**
 * (hn_mesh 的形变通道) —— 立绘动起来靠的是官方动作数据, 网格变形是
 * 引擎自己的原语。moc3 网格变形渲染不做, 也如实声明不做。
 *
 * motion3 求值器(Live2D Cubism 规范 Motion 文档 v2.4):
 *   Curves[i].Segments = [x0,y0, (type, 点...)*]
 *   段类型: 0=线性(1 点) / 1=贝塞尔(3 点: c1,c2,终点) /
 *           2=阶梯(1 点: 保持前值到终点才跳变) / 3=反阶梯(1 点: 立即跳到新值)
 *   贝塞尔按参数 u 求 x(u)=t 的反解(x 单调, AreBeziersRestricted 保证),
 *   再取 y(u) —— 二分 28 次, 确定性闭式迭代(无 libm 依赖)。
 *   Meta.Loop=true 时采样时间对 Duration 取模回绕。
 */

typedef struct {
    char  *id;                 /* 曲线参数 Id(ParamAngleX 等) */
    int    n_pts;              /* 端点数(含首点) */
    float *px, *py;            /* 端点(x=秒, y=值) */
    unsigned char *seg_type;   /* 段 i 连接端点 i→i+1, 类型 0/1/2/3 */
    float *bx, *by;            /* 贝塞尔段控制点(每段 2 个, 非贝塞尔段为 0) */
} l2_curve;

typedef struct {
    char    *file;             /* 相对 model3.json 的路径(诊断用) */
    float    duration;         /* Meta.Duration(秒) */
    int      loop;             /* Meta.Loop */
    l2_curve *curves;
    int      n_curves;
} l2_motion;

typedef struct l2_group {
    char *name;                /* Motions 组名(Idle/Tap/...) */
    int   base, count;         /* motions[] 里的区间 [base, base+count) */
    struct l2_group *next;
} l2_group;

struct hn_live2d {
    char *model3_path;         /* 缓存键(规范化路径) */
    char *json_buf;            /* model3.json 的解析缓冲 —— hn_json 是就地解析,
                                  字符串值指向缓冲内, 必须与结构体同生命周期 */
    /* FileReferences 各字段的"已声明/已解析"事实(诚实面板的数据源) */
    int   has_moc;             /* Moc 已声明 —— moc3 本体不解析(专有) */
    int   textures;            /* 贴图数 */
    char *texture_paths[8];
    int   has_physics;
    int   physics_settings, physics_inputs, physics_outputs, physics_vertices;
    int   has_display_info;
    int   cdi3_params;         /* cdi3.json 参数条数 */
    char **cdi3_id, **cdi3_name;   /* 参数 Id → 显示名(前 64 条, 探针用) */
    int   cdi3_stored;
    l2_motion *motions;
    int   n_motions;
    l2_group *groups;
    int   n_groups;
};

#define L2_CDI3_MAX 64

static char *l2_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

/* 目录拼接: dir 是 model3.json 所在目录(含尾 '/' 或为空), rel 是包内相对路径 */
static char *l2_join(const char *dir, const char *rel) {
    if (!dir) dir = "";
    size_t dn = strlen(dir), rn = strlen(rel);
    char *p = (char *)malloc(dn + rn + 1);
    if (!p) return NULL;
    memcpy(p, dir, dn);
    memcpy(p + dn, rel, rn + 1);
    return p;
}

/* 取路径的目录前缀(含尾 '/'; 无 '/' 返回空串堆块) */
static char *l2_dirname(const char *path) {
    const char *slash = strrchr(path, '/');
    size_t n = slash ? (size_t)(slash - path + 1) : 0;
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, path, n);
    p[n] = 0;
    return p;
}

static void l2_free_motion(l2_motion *mo) {
    if (!mo) return;
    free(mo->file);
    for (int i = 0; i < mo->n_curves; i++) {
        free(mo->curves[i].id);
        free(mo->curves[i].px);
        free(mo->curves[i].py);
        free(mo->curves[i].seg_type);
        free(mo->curves[i].bx);
        free(mo->curves[i].by);
    }
    free(mo->curves);
}

void hn_live2d_free(struct hn_live2d *m) {
    if (!m) return;
    free(m->model3_path);
    free(m->json_buf);
    for (int i = 0; i < m->textures && i < 8; i++) free(m->texture_paths[i]);
    for (int i = 0; i < m->cdi3_stored; i++) { free(m->cdi3_id[i]); free(m->cdi3_name[i]); }
    free(m->cdi3_id);
    free(m->cdi3_name);
    for (int i = 0; i < m->n_motions; i++) l2_free_motion(&m->motions[i]);
    free(m->motions);
    for (l2_group *g = m->groups; g; ) {
        l2_group *nx = g->next;
        free(g->name);
        free(g);
        g = nx;
    }
    free(m);
}

/* 解析一条 motion3 曲线的 Segments 平面数组 */
static int l2_parse_segments(hn_arena *a_unused, const hn_json *segs, l2_curve *cv) {
    (void)a_unused;
    if (!segs || segs->kind != HN_JSON_ARR || segs->count < 2) return 0;
    int cap = segs->count / 2 + 2;
    cv->px = (float *)calloc((size_t)cap, sizeof(float));
    cv->py = (float *)calloc((size_t)cap, sizeof(float));
    cv->seg_type = (unsigned char *)calloc((size_t)cap, sizeof(unsigned char));
    cv->bx = (float *)calloc((size_t)cap * 2, sizeof(float));
    cv->by = (float *)calloc((size_t)cap * 2, sizeof(float));
    if (!cv->px || !cv->py || !cv->seg_type || !cv->bx || !cv->by) return 0;
    cv->px[0] = (float)hn_json_num(hn_json_at(segs, 0), 0);
    cv->py[0] = (float)hn_json_num(hn_json_at(segs, 1), 0);
    int npts = 1, si = 2;
    while (si < segs->count) {
        int type = (int)hn_json_num(hn_json_at(segs, si), -1);
        int need = (type == 1) ? 7 : 3;          /* 段标记 + 点(贝塞尔 3 点) */
        if (si + need > segs->count) break;      /* 截断段: 丢弃(容错不崩) */
        cv->px[npts] = (float)hn_json_num(hn_json_at(segs, si + need - 2), 0);
        cv->py[npts] = (float)hn_json_num(hn_json_at(segs, si + need - 1), 0);
        cv->seg_type[npts - 1] = (unsigned char)type;
        if (type == 1) {
            cv->bx[(npts - 1) * 2]     = (float)hn_json_num(hn_json_at(segs, si + 1), 0);
            cv->by[(npts - 1) * 2]     = (float)hn_json_num(hn_json_at(segs, si + 2), 0);
            cv->bx[(npts - 1) * 2 + 1] = (float)hn_json_num(hn_json_at(segs, si + 3), 0);
            cv->by[(npts - 1) * 2 + 1] = (float)hn_json_num(hn_json_at(segs, si + 4), 0);
        }
        npts++;
        si += need;
    }
    cv->n_pts = npts;
    return npts >= 2;
}

/* 加载并解析一个 motion3.json(失败返回 0, mo 保持可安全释放的状态) */
static int l2_load_motion(hn_context *c, const char *dir, const char *rel,
                          l2_motion *mo) {
    const hn_asset_backend *ab = hn_context_assets(c);
    if (!ab || !ab->load) return 0;
    char *path = l2_join(dir, rel);
    if (!path) return 0;
    size_t len = 0;
    const char *bytes = ab->load(ab->ctx, path, &len);
    mo->file = l2_strdup(rel);
    if (!bytes || len == 0) { free(path); return 0; }
    char *buf = (char *)malloc(len + 1);
    if (!buf) { free(path); return 0; }
    memcpy(buf, bytes, len);
    buf[len] = 0;
    void *ja = NULL;
    hn_json *root = hn_json_parse(buf, len, &ja);
    free(path);
    if (!root) { free(buf); return 0; }
    const hn_json *meta = hn_json_obj_get(root, "Meta");
    mo->duration = (float)hn_json_num(hn_json_obj_get(meta, "Duration"), 0);
    mo->loop = hn_json_bool(hn_json_obj_get(meta, "Loop"), 1);
    const hn_json *curves = hn_json_obj_get(root, "Curves");
    int n = (curves && curves->kind == HN_JSON_ARR) ? curves->count : 0;
    mo->curves = (l2_curve *)calloc((size_t)(n > 0 ? n : 1), sizeof(l2_curve));
    if (!mo->curves) { hn_json_free(ja); free(buf); return 0; }
    int ok = 0;
    for (int i = 0; i < n; i++) {
        hn_json *cj = hn_json_at(curves, i);
        const char *id = hn_json_str(hn_json_obj_get(cj, "Id"), NULL);
        if (!id) continue;
        l2_curve *cv = &mo->curves[mo->n_curves];
        memset(cv, 0, sizeof(*cv));
        cv->id = l2_strdup(id);
        if (!cv->id) continue;
        if (!l2_parse_segments(NULL, hn_json_obj_get(cj, "Segments"), cv)) {
            free(cv->id);
            continue;
        }
        mo->n_curves++;
        ok = 1;
    }
    hn_json_free(ja);
    free(buf);
    return ok && mo->duration > 0;
}

struct hn_live2d *hn_live2d_load(hn_context *c, const char *model3_path) {
    if (!c || !model3_path || !*model3_path) return NULL;
    const hn_asset_backend *ab = hn_context_assets(c);
    if (!ab || !ab->load) return NULL;
    size_t len = 0;
    const char *bytes = ab->load(ab->ctx, model3_path, &len);
    if (!bytes || len == 0) return NULL;
    char *buf = (char *)malloc(len + 1);
    if (!buf) return NULL;
    memcpy(buf, bytes, len);
    buf[len] = 0;
    void *ja = NULL;
    hn_json *root = hn_json_parse(buf, len, &ja);
    if (!root) { free(buf); return NULL; }

    struct hn_live2d *m = (struct hn_live2d *)calloc(1, sizeof(*m));

    if (!m) { hn_json_free(ja); free(buf); return NULL; }
    m->json_buf = buf;         /* 就地解析: 字符串指向 buf 内, 随结构体释放 */
    m->model3_path = l2_strdup(model3_path);
    m->cdi3_id = (char **)calloc(L2_CDI3_MAX, sizeof(char *));
    m->cdi3_name = (char **)calloc(L2_CDI3_MAX, sizeof(char *));
    if (!m->model3_path || !m->cdi3_id || !m->cdi3_name) {
        hn_live2d_free(m);
        hn_json_free(ja);
        return NULL;
    }
    char *dir = l2_dirname(model3_path);

    const hn_json *fr = hn_json_obj_get(root, "FileReferences");
    const hn_json *moc = hn_json_obj_get(fr, "Moc");
    if (moc && moc->kind == HN_JSON_STR && moc->str && *moc->str)
        m->has_moc = 1;                    /* 只记声明; moc3 本体不解析 */
    const hn_json *tex = hn_json_obj_get(fr, "Textures");
    if (tex && tex->kind == HN_JSON_ARR)
        for (int i = 0; i < tex->count && m->textures < 8; i++) {
            const char *tp = hn_json_str(hn_json_at(tex, i), NULL);
            if (!tp) continue;
            m->texture_paths[m->textures++] = l2_join(dir, tp);
        }
    const hn_json *phy = hn_json_obj_get(fr, "Physics");
    const char *phy_rel = hn_json_str(phy, NULL);
    if (phy_rel && *phy_rel) {
        char *pp = l2_join(dir, phy_rel);
        size_t pl = 0;
        const char *pb = pp ? ab->load(ab->ctx, pp, &pl) : NULL;
        if (pb && pl > 0) {
            char *pbuf = (char *)malloc(pl + 1);
            if (pbuf) {
                memcpy(pbuf, pb, pl);
                pbuf[pl] = 0;
                void *pja = NULL;
                hn_json *proot = hn_json_parse(pbuf, pl, &pja);
                if (proot) {
                    const hn_json *pm = hn_json_obj_get(proot, "Meta");
                    /* physics3 v1 只解析计数(渲染不用物理 —— 如实口径) */
                    m->physics_settings = (int)hn_json_num(hn_json_obj_get(pm, "PhysicsSettingCount"), 0);
                    m->physics_inputs   = (int)hn_json_num(hn_json_obj_get(pm, "TotalInputCount"), 0);
                    m->physics_outputs  = (int)hn_json_num(hn_json_obj_get(pm, "TotalOutputCount"), 0);
                    m->physics_vertices = (int)hn_json_num(hn_json_obj_get(pm, "VertexCount"), 0);
                    m->has_physics = 1;
                    hn_json_free(pja);
                }
                free(pbuf);
            }
        }
        free(pp);
    }
    const hn_json *di = hn_json_obj_get(fr, "DisplayInfo");
    const char *di_rel = hn_json_str(di, NULL);
    if (di_rel && *di_rel) {
        char *dp = l2_join(dir, di_rel);
        size_t dl = 0;
        const char *db = dp ? ab->load(ab->ctx, dp, &dl) : NULL;
        if (db && dl > 0) {
            char *dbuf = (char *)malloc(dl + 1);
            if (dbuf) {
                memcpy(dbuf, db, dl);
                dbuf[dl] = 0;
                void *dja = NULL;
                hn_json *droot = hn_json_parse(dbuf, dl, &dja);
                if (droot) {
                    const hn_json *ps = hn_json_obj_get(droot, "Parameters");
                    if (ps && ps->kind == HN_JSON_ARR) {
                        m->cdi3_params = ps->count;
                        for (int i = 0; i < ps->count && m->cdi3_stored < L2_CDI3_MAX; i++) {
                            hn_json *e = hn_json_at(ps, i);
                            const char *pid = hn_json_str(hn_json_obj_get(e, "Id"), NULL);
                            const char *nm = hn_json_str(hn_json_obj_get(e, "Name"), NULL);
                            if (!pid) continue;
                            m->cdi3_id[m->cdi3_stored] = l2_strdup(pid);
                            m->cdi3_name[m->cdi3_stored] = l2_strdup(nm);
                            if (m->cdi3_id[m->cdi3_stored]) m->cdi3_stored++;
                        }
                        m->has_display_info = 1;
                    }
                    hn_json_free(dja);
                }
                free(dbuf);
            }
        }
        free(dp);
    }
    /* Motions 组: 每组一个文件数组, 逐个解析 motion3 */
    const hn_json *motions = hn_json_obj_get(fr, "Motions");
    if (motions && motions->kind == HN_JSON_OBJ) {
        m->motions = (l2_motion *)calloc((size_t)(motions->count > 0 ? motions->count : 1),
                                         sizeof(l2_motion));
        if (m->motions) {
            int cap = 0;
            for (int g = 0; g < motions->count; g++) {
                /* OBJ 的子节点走 items[](hn_json_at 只认 ARR —— 与
                   hn_json_obj_get 同一访问路径) */
                hn_json *ge = (g >= 0 && g < motions->count) ? motions->items[g] : NULL;
                if (!ge || ge->kind != HN_JSON_ARR || !ge->key) continue;
                l2_group *grp = (l2_group *)calloc(1, sizeof(l2_group));
                if (!grp) continue;
                grp->name = l2_strdup(ge->key);
                grp->base = m->n_motions;
                for (int i = 0; i < ge->count; i++) {
                    hn_json *fe = hn_json_at(ge, i);
                    const char *rel = hn_json_str(hn_json_obj_get(fe, "File"), NULL);
                    if (!rel) continue;
                    if (cap <= m->n_motions) {
                        int nc = cap ? cap * 2 : motions->count;
                        l2_motion *nm2 = (l2_motion *)realloc(m->motions,
                                                              sizeof(l2_motion) * (size_t)nc);
                        if (!nm2) break;
                        m->motions = nm2;
                        cap = nc;
                    }
                    l2_motion *mo = &m->motions[m->n_motions];
                    memset(mo, 0, sizeof(*mo));
                    if (l2_load_motion(c, dir, rel, mo)) m->n_motions++;
                    else l2_free_motion(mo);
                }
                grp->count = m->n_motions - grp->base;
                if (grp->count > 0) {
                    grp->next = m->groups;
                    m->groups = grp;       /* 头插: 顺序与 JSON 逆序, 无碍(按键查) */
                    m->n_groups++;
                } else {
                    free(grp->name);
                    free(grp);
                }
            }
        }
    }
    hn_json_free(ja);
    free(dir);
    if (m->n_motions == 0) { hn_live2d_free(m); return NULL; }
    return m;
}

/* ---------------- motion3 求值 ---------------- */

/* 三次贝塞尔 B(u): 端点 p0,p3 与控制点 p1,p2(标量分量) */
static float l2_bez(float p0, float p1, float p2, float p3, float u) {
    float mu = 1.0f - u;
    return mu * mu * mu * p0 + 3.0f * mu * mu * u * p1 +
           3.0f * mu * u * u * p2 + u * u * u * p3;
}

/* 曲线在 t 秒的值。段语义(规范):
   线性=两端点插值; 阶梯=保持段起点值(到终点才跳);
   反阶梯=进入段即取终点值; 贝塞尔=反解 x(u)=t 后取 y(u)。 */
static int l2_curve_at(const l2_curve *cv, float t, float *out) {
    if (!cv || cv->n_pts < 2) return 0;
    if (t <= cv->px[0]) { *out = cv->py[0]; return 1; }
    int last = cv->n_pts - 1;
    if (t >= cv->px[last]) { *out = cv->py[last]; return 1; }
    for (int i = 0; i < last; i++) {
        /* 半开区间 [px[i], px[i+1]): 恰在关键帧时刻取后一段 —— 阶梯段
           的跳变发生在终点时刻(新值生效), 与 Cubism 关键帧语义一致;
           连续段(线性/贝塞尔)在共享端点两侧同值, 不受影响。 */
        if (t < cv->px[i] || t >= cv->px[i + 1]) continue;
        float x0 = cv->px[i], x1 = cv->px[i + 1];
        switch (cv->seg_type[i]) {
        case 2:                                    /* 阶梯: 前值保持 */
            *out = cv->py[i];
            return 1;
        case 3:                                    /* 反阶梯: 立即跳到新值 */
            *out = cv->py[i + 1];
            return 1;
        case 1: {                                  /* 贝塞尔: x 单调反解 */
            float c1x = cv->bx[i * 2],     c1y = cv->by[i * 2];
            float c2x = cv->bx[i * 2 + 1], c2y = cv->by[i * 2 + 1];
            float lo = 0.0f, hi = 1.0f, u = 0.5f;
            for (int k = 0; k < 28; k++) {         /* 28 次二分: 1e-8 级, 确定性 */
                u = (lo + hi) * 0.5f;
                float xu = l2_bez(x0, c1x, c2x, x1, u);
                if (xu < t) lo = u; else hi = u;
            }
            *out = l2_bez(cv->py[i], c1y, c2y, cv->py[i + 1], u);
            return 1;
        }
        default: {                                 /* 0 = 线性 */
            float f = x1 - x0 > 0.000001f ? (t - x0) / (x1 - x0) : 0.0f;
            *out = cv->py[i] + (cv->py[i + 1] - cv->py[i]) * f;
            return 1;
        }
        }
    }
    *out = cv->py[last];
    return 1;
}

int hn_live2d_motion_sample(const struct hn_live2d *m, int motion_idx,
                            const char *param, float t_sec, float *out) {
    if (!m || motion_idx < 0 || motion_idx >= m->n_motions || !param || !out)
        return 0;
    const l2_motion *mo = &m->motions[motion_idx];
    float t = t_sec;
    if (mo->loop && mo->duration > 0) {            /* Loop=true: 对时长取模回绕 */
        t = fmodf(t, mo->duration);
        if (t < 0) t += mo->duration;
    } else if (mo->duration > 0 && t > mo->duration) {
        t = mo->duration;                          /* 不循环: 钳到末帧 */
    }
    for (int i = 0; i < mo->n_curves; i++)
        if (!strcmp(mo->curves[i].id, param))
            return l2_curve_at(&mo->curves[i], t, out);
    return 0;
}

/* ---------------- 包信息(诚实面板/探针断言面) ---------------- */

void hn_live2d_get_info(const struct hn_live2d *m, hn_live2d_info *info) {
    if (!info) return;
    memset(info, 0, sizeof(*info));
    if (!m) return;
    info->has_moc           = m->has_moc;
    info->textures          = m->textures;
    info->has_physics       = m->has_physics;
    info->physics_settings  = m->physics_settings;
    info->physics_inputs    = m->physics_inputs;
    info->physics_outputs   = m->physics_outputs;
    info->physics_vertices  = m->physics_vertices;
    info->has_display_info  = m->has_display_info;
    info->cdi3_params       = m->cdi3_params;
    info->motion_groups     = m->n_groups;
    info->motions           = m->n_motions;
    for (int i = 0; i < m->n_motions; i++) {
        info->curves_total += m->motions[i].n_curves;
        if (m->motions[i].duration > info->duration_max)
            info->duration_max = m->motions[i].duration;
    }
}

int hn_live2d_motion_count(const struct hn_live2d *m) { return m ? m->n_motions : 0; }

const char *hn_live2d_cdi3_name(const struct hn_live2d *m, const char *param_id) {
    if (!m || !param_id) return NULL;
    for (int i = 0; i < m->cdi3_stored; i++)
        if (m->cdi3_id[i] && !strcmp(m->cdi3_id[i], param_id))
            return m->cdi3_name[i];
    return NULL;
}

/* 按组名 + 组内序号选 motion; 组缺省时回退第一个 motion(缺省动画口径) */
int hn_live2d_motion_pick(const struct hn_live2d *m, const char *group, int index) {
    if (!m || m->n_motions <= 0) return -1;
    if (group && *group) {
        /* 组已声明却未命中 → -1(声明驱动: 宁可不驱动也不静默换动作) */
        for (const l2_group *g = m->groups; g; g = g->next)
            if (!strcmp(g->name, group)) {
                if (index >= 0 && index < g->count) return g->base + index;
                return -1;
            }
        return -1;
    }
    return 0;                                      /* 未声明组 → 第一个 motion */
}

const char *hn_live2d_motion_file(const struct hn_live2d *m, int motion_idx) {
    if (!m || motion_idx < 0 || motion_idx >= m->n_motions) return NULL;
    return m->motions[motion_idx].file;
}

float hn_live2d_motion_duration(const struct hn_live2d *m, int motion_idx) {
    if (!m || motion_idx < 0 || motion_idx >= m->n_motions) return 0;
    return m->motions[motion_idx].duration;
}

/* ---------------- 进程级模型缓存 ----------------
 * 实时窗口每帧都要采样 motion(rig 驱动), 每次 emit 重解 ~90KB JSON 不可
 * 接受; hn_context.c 的按路径缓存不归本文件管(纪律: 不碰), 因此在文件内
 * 做固定 4 槽 LRU。语义与 Lottie/GIF 缓存一致: 键 = 路径, 不追踪文件热
 * 更新; 探针可用 hn_live2d_cache_clear() 复位。 */
typedef struct {
    char *path;
    struct hn_live2d *model;
    unsigned last_use;
} l2_cache_slot;

static l2_cache_slot g_l2_cache[4];
static unsigned g_l2_clock;

struct hn_live2d *hn_live2d_acquire(hn_context *c, const char *model3_path) {
    if (!model3_path || !*model3_path) return NULL;
    g_l2_clock++;
    int free_slot = -1, lru_slot = 0;
    for (int i = 0; i < 4; i++) {
        if (g_l2_cache[i].model && g_l2_cache[i].path &&
            !strcmp(g_l2_cache[i].path, model3_path)) {
            g_l2_cache[i].last_use = g_l2_clock;
            return g_l2_cache[i].model;
        }
        if (!g_l2_cache[i].model && free_slot < 0) free_slot = i;
        else if (g_l2_cache[i].last_use < g_l2_cache[lru_slot].last_use) lru_slot = i;
    }
    struct hn_live2d *m = hn_live2d_load(c, model3_path);
    if (!m) return NULL;
    int slot = free_slot >= 0 ? free_slot : lru_slot;
    free(g_l2_cache[slot].path);
    hn_live2d_free(g_l2_cache[slot].model);
    g_l2_cache[slot].path = l2_strdup(model3_path);
    g_l2_cache[slot].model = m;
    g_l2_cache[slot].last_use = g_l2_clock;
    return m;
}

void hn_live2d_cache_clear(void) {
    for (int i = 0; i < 4; i++) {
        free(g_l2_cache[i].path);
        hn_live2d_free(g_l2_cache[i].model);
        g_l2_cache[i].path = NULL;
        g_l2_cache[i].model = NULL;
        g_l2_cache[i].last_use = 0;
    }
}

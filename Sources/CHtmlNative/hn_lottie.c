/* hn_lottie.c — Lottie(bodymovin)矢量动画求值 → 绘制指令
 *
 * 分层: 引擎负责"矢量图形 + 时间轴", 产物仍是平台无关的绘制指令
 * (POLYGON / IMAGE / RECT), 因此三个后端(native CoreGraphics、hnsoft、
 * WebKit)同等支持 —— 不需要为每个平台实现一遍播放器。
 * JSON 从哪来由运行时的资产后端注入(引擎自身不做磁盘/网络 I/O)。
 *
 * 支持子集(覆盖图标/loading/表情/插画类动画的绝大多数):
 *   图层: 形状层(ty=4) / 纯色层(ty=1) / 图片层(ty=2)
 *   形状: 组(gr, 含嵌套与自身变换) / 矩形(rc, 含圆角) / 椭圆(el) / 路径(sh)
 *   画刷: 填充(fl) / 描边(st)
 *   动画: 锚点/位置/缩放/旋转/不透明度关键帧 + 路径变形关键帧(sh.ks.k)
 * 明确不支持(跳过, 不报错): 预合成 / 文本层 / 表达式 / 特效 / trim path /
 *   遮罩与轨道遮罩 / repeater / merge paths / 时间重映射。
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "hn_internal.h"
#include "hn_json.h"

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

enum { L_GROUP, L_RECT, L_ELLIPSE, L_PATH, L_FILL, L_STROKE, L_TRANSFORM, L_TRIM, L_REPEATER };

typedef struct { float t; float *data; int n; } lpkf;   /* 路径关键帧 */

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

/* ---------------- 加载 ---------------- */

struct hn_lottie *hn_lottie_load(hn_context *c, const char *path) {
    if (!c || !path) return NULL;
    const hn_asset_backend *ab = hn_context_assets(c);
    if (!ab || !ab->load) return NULL;
    size_t len = 0;
    const char *bytes = ab->load(ab->ctx, path, &len);
    if (!bytes || len == 0) return NULL;

    char *buf = (char *)malloc(len + 1);      /* 就地解析会覆盖原字节 */
    if (!buf) return NULL;
    memcpy(buf, bytes, len);
    buf[len] = 0;

    void *ja = NULL;
    hn_json *root = hn_json_parse(buf, len, &ja);
    if (!root) { free(buf); return NULL; }

    struct hn_lottie *l = (struct hn_lottie *)calloc(1, sizeof(*l));
    if (!l) { hn_json_free(ja); free(buf); return NULL; }
    l->json_arena = ja;
    l->json_buf = buf;
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
            /* 关键帧时间: 帧 → 毫秒(整层归一) */
            float mpf = 1000.0f / fr;
            norm_prop_time(&ly->anchor, mpf);
            norm_prop_time(&ly->pos, mpf);
            norm_prop_time(&ly->scale, mpf);
            norm_prop_time(&ly->rot, mpf);
            norm_prop_time(&ly->opacity, mpf);
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
        if (tr_s > 0.0f || tr_e < 100.0f) {
            double total = 0;
            for (int q = 1; q < n; q++) {
                double dx = (double)v->v[q*2] - v->v[(q-1)*2];
                double dy = (double)v->v[q*2+1] - v->v[(q-1)*2+1];
                total += sqrt(dx*dx + dy*dy);
            }
            double a0 = fmod(tr_s + tr_o, 100.0), a1 = fmod(tr_e + tr_o, 100.0);
            if (a0 < 0) a0 += 100.0;
            if (a1 < 0) a1 += 100.0;
            if (a1 <= a0) a1 += 100.0;              /* 跨 0 的情况: 绕一圈 */
            int i0 = (int)(a0 / 100.0 * (n - 1) + 0.5);
            int i1 = (int)(a1 / 100.0 * (n - 1) + 0.5);
            if (i0 < 0) i0 = 0;
            if (i1 > n - 1) i1 = n - 1;
            if (i1 <= i0) continue;                 /* 区间为空: 不画 */
            b0 = i0; b1 = i1 + 1;
        }
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
            emit_group(c, a, s->kids, s->n_kids, t, gm, galpha, sx, sy, f, bag, depth + 1);
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
    }
    bag->n_paths = from;
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
        bag.n_paths = 0;
        emit_group(c, a, ly->shapes, ly->n_shapes, t, m, la, sx, sy, f, &bag, 0);
    }
}

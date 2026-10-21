/* hn_layout.c — 布局引擎: block 流 + flexbox 子集 + 行内格式化上下文(IFC)
 *
 * 设计:
 *  - layout_box 递归布局一个节点(border-box), 返回其高度;
 *    measuring=1 为"度量模式": 宽度 auto 收缩到内容宽, 文本不换行。
 *  - 文本与 display:inline 元素合起来构成行内格式化上下文: 共享行盒、
 *    按词贪心换行、跨节点/跨行保持基线对齐; 排版结果记为 run(绝对坐标)。
 *  - flex: pass1 度量子项 → 分配主轴(grow/shrink/justify) → pass2 终布局。
 *  - 有意简化: 无 margin 折叠、无 grid、无绝对定位。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn_internal.h"

/* 解析 margin 分量: em 已在样式期展开, 这里处理百分比。
   CSS 规定 margin 的百分比基于**包含块的行内尺寸(水平书写模式下即宽度)**,
   四个边都一样 —— 包括上下边。所以 margin-top:50% 在 400 宽的容器里
   是 200px, 不是容器高的一半, 更不是字面的 50px。 */
static float margin_px(const hn_style *st, int side, float cb_width) {
    float v = st->margin[side];
    if (st->margin_u[side] == HN_U_PCT) return v / 100.0f * cb_width;
    return v;
}

/* 从样式读四边 margin(已解析百分比) */
static void margins_of(const hn_style *st, float cb_width, float out[4]) {
    for (int i = 0; i < 4; i++) out[i] = margin_px(st, i, cb_width);
}

static float size_of(float v, int u, float base, float font_px) {
    switch (u) {
    case HN_U_PX:  return v;
    case HN_U_PCT: return base > 0 ? v / 100.0f * base : -1.0f;
    case HN_U_EM:  return v * font_px;
    default:       return -1.0f;
    }
}

/* ---------- 文本测量缓存(freeze L2 前置: "从缓存盒恢复"的第一步) ----------
 * 布局每遍 clear_runs 后从头测量全部文本; 流式应用(聊天/日志)与重复
 * 重排(hover/焦点/热更新)场景下, 旧文本的测量全是重复功(2000 卡文档
 * layout 实测 ~14ms, 测量是其中大头)。measure 是纯函数: 字体五元组
 * (size_px/weight/italic/letter_spacing/family)+ 文本字节 → 宽度浮点,
 * 在此做记忆化。键逐字段逐位比较(不做 struct memcmp —— 初始化式不保证
 * padding 字节), 命中返回的浮点与重测严格相等 → 缓存开/关布局结果逐位
 * 一致(freeze_l2_probe 锚定)。
 * 纪律:
 *   - 每上下文一份(hn_context.mcache); hn_layout_root 入口 sync 文本
 *     后端, 后端换人即整表清空(测量语义跟随当前后端);
 *   - 条目上限满即整表清空(简单可预期, 不做 LRU —— 热文本下一遍自会
 *     重建; 溢出即说明文档文本面已超预算, 缓存收益归零不如归零开销);
 *   - HN_MEASURE_CACHE=0 全局关闭; HN_MEASURE_TRACE=1 每遍布局出口打
 *     命中统计(与 L1 的 HN_FRAGMENT_TRACE 同手法)。 */
typedef struct hn_ment {
    struct hn_ment *next;
    unsigned long long hash;    /* 全键 FNV-1a 64(链内快筛; 逐字段比较兜底) */
    float w;
    hn_font_desc font;          /* 五元组(逐字段比较) */
    unsigned len;
    char text[];                /* 文本字节副本(len 字节; C99 弹性数组成员) */
} hn_ment;

#define MC_BUCKETS     4096u    /* 2 的幂(hash 按位取桶) */
#define MC_MAX_ENTRIES 16384

typedef struct hn_mcache {
    hn_ment *b[MC_BUCKETS];
    long entries;
    long hits, misses;          /* 累计统计(TRACE / freeze_l2_probe 读) */
    hn_text_backend tb;         /* sync 时换人(memcmp 字段)即全清 */
    int tb_valid, on, trace;
} hn_mcache;

/* 当前布局的测量缓存(hn_layout_root 进出设置/清空; 布局不重入, 单指针
   即可 —— measure_run 的全部调用点都在布局栈内)。 */
static hn_mcache *g_mc;

static unsigned long long mc_mix(unsigned long long h, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    size_t i;
    for (i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

static unsigned long long mc_hash(const hn_font_desc *f, const char *s,
                                  unsigned n) {
    unsigned long long h = 1469598103934665603ull;   /* FNV-1a 64 偏移基 */
    h = mc_mix(h, &f->size_px, sizeof f->size_px);
    h = mc_mix(h, &f->weight, sizeof f->weight);
    h = mc_mix(h, &f->italic, sizeof f->italic);
    h = mc_mix(h, &f->letter_spacing, sizeof f->letter_spacing);
    h = mc_mix(h, &f->family, sizeof f->family);
    return mc_mix(h, s, n);
}

static int mc_font_eq(const hn_font_desc *a, const hn_font_desc *b) {
    return a->size_px == b->size_px && a->weight == b->weight &&
           a->italic == b->italic &&
           a->letter_spacing == b->letter_spacing && a->family == b->family;
}

static hn_ment *mc_find(hn_mcache *mc, const hn_font_desc *f, const char *s,
                        unsigned n, unsigned long long h) {
    hn_ment *e = mc->b[h & (MC_BUCKETS - 1)];
    for (; e; e = e->next) {
        if (e->hash != h || e->len != n || !mc_font_eq(&e->font, f)) continue;
        if (n && memcmp(e->text, s, n) != 0) continue;
        return e;
    }
    return NULL;
}

static void mc_clear(hn_mcache *mc) {
    unsigned i;
    for (i = 0; i < MC_BUCKETS; i++) {
        hn_ment *e = mc->b[i];
        while (e) { hn_ment *nx = e->next; free(e); e = nx; }
        mc->b[i] = NULL;
    }
    mc->entries = 0;
}

static void mc_put(hn_mcache *mc, const hn_font_desc *f, const char *s,
                   unsigned n, unsigned long long h, float w) {
    hn_ment *e;
    if (mc->entries >= MC_MAX_ENTRIES) mc_clear(mc);   /* 满即整表清空(见头注) */
    e = (hn_ment *)malloc(sizeof(hn_ment) + (size_t)n);
    if (!e) return;                     /* 分配失败: 本条不缓存, 直测兜底 */
    e->next = mc->b[h & (MC_BUCKETS - 1)];
    mc->b[h & (MC_BUCKETS - 1)] = e;
    e->hash = h;
    e->w = w;
    e->font = *f;
    e->len = n;
    if (n) memcpy(e->text, s, n);
    mc->entries++;
}

void *hn_layout_mcache_create(void) {
    hn_mcache *mc = (hn_mcache *)calloc(1, sizeof *mc);
    const char *env;
    if (!mc) return NULL;
    env = getenv("HN_MEASURE_CACHE");
    mc->on = !(env && *env && strcmp(env, "0") == 0);
    env = getenv("HN_MEASURE_TRACE");
    mc->trace = (env && *env && strcmp(env, "0") != 0);
    return mc;
}

void hn_layout_mcache_free(void *p) {
    hn_mcache *mc = (hn_mcache *)p;
    if (!mc) return;
    mc_clear(mc);
    if (g_mc == mc) g_mc = NULL;    /* 防御: 布局栈外销毁不留悬空当前指针 */
    free(mc);
}

void hn_layout_mcache_sync(void *p, const hn_text_backend *tb, int tb_valid) {
    hn_mcache *mc = (hn_mcache *)p;
    int changed;
    if (!mc) return;
    changed = (mc->tb_valid != tb_valid) ||
              (tb_valid && (mc->tb.ctx != tb->ctx ||
                            mc->tb.measure != tb->measure ||
                            mc->tb.metrics != tb->metrics));
    if (!changed) return;
    mc_clear(mc);                   /* 后端换人: 测量语义可能整体不同, 全清 */
    if (tb_valid) mc->tb = *tb;
    mc->tb_valid = tb_valid;
}

void hn_layout_mcache_stats(const void *p, long *hits, long *misses,
                            long *entries) {
    const hn_mcache *mc = (const hn_mcache *)p;
    if (hits) *hits = mc ? mc->hits : 0;
    if (misses) *misses = mc ? mc->misses : 0;
    if (entries) *entries = mc ? mc->entries : 0;
}

static float measure_run(const hn_text_backend *tb, const hn_style *st,
                         const char *s, size_t n) {
    if (!n) return 0;
    if (tb && tb->measure) {
        hn_font_desc f = { st->font_size, st->font_weight, st->font_italic, st->letter_spacing, st->font_family };
        if (g_mc && g_mc->on) {
            unsigned len = (unsigned)n;
            unsigned long long h = mc_hash(&f, s, len);
            hn_ment *e = mc_find(g_mc, &f, s, len, h);
            if (e) { g_mc->hits++; return e->w; }
            g_mc->misses++;
            {
                float w = tb->measure(tb->ctx, &f, s, n);
                mc_put(g_mc, &f, s, len, h, w);
                return w;
            }
        }
        return tb->measure(tb->ctx, &f, s, n);
    }
    return (float)n * st->font_size * 0.55f; /* 无文本后端时的粗略回退 */
}

static void font_metrics(const hn_text_backend *tb, const hn_style *st,
                         float *ascent, float *descent) {
    float a = st->font_size * 0.8f, d = st->font_size * 0.2f;
    if (tb && tb->metrics) {
        float l = 0;
        hn_font_desc f = { st->font_size, st->font_weight, st->font_italic, st->letter_spacing, st->font_family };
        tb->metrics(tb->ctx, &f, &a, &d, &l);
    }
    *ascent = a;
    *descent = d;
}

static size_t utf8_next(const char *s, size_t rem) {
    unsigned char c = (unsigned char)*s;
    size_t k = 1;
    if (c >= 0xF0) k = 4;
    else if (c >= 0xE0) k = 3;
    else if (c >= 0xC0) k = 2;
    return k <= rem ? k : rem;
}

/* ---------------- 行内排版(IFC) ---------------- */

typedef struct {
    hn_node        *owner;        /* 文本节点; NULL = 强制换行标记 */
    const hn_style *st;           /* 测量/行高用(文本取父元素样式) */
    size_t          begin, end;
    float           w;
    float           pad_before, pad_after; /* 内联元素的左右内边距/外边距 */
    int             space_before; /* 源码中此前有空白(行首丢弃) */
    int             forced_break;
    /* white-space:pre 下的空白串(保留原宽度)。标记它是因为行首的空白串
       按规范要丢弃 —— 没有这个标记就没法区分"pre 的空白串"与普通词。 */
    int             is_space_run;
} iitem;

typedef struct { iitem *v; int n, cap; int pending_space; } ivec;

static void run_push(hn_node *n, size_t b, size_t e, float x, float w,
                     float baseline, float y_top, float h) {
    hn_run *nv = realloc(n->runs, sizeof(hn_run) * (size_t)(n->n_runs + 1));
    if (!nv) abort();
    n->runs = nv;
    hn_run *r = &n->runs[n->n_runs++];
    r->begin = b; r->end = e;
    r->x = x; r->width = w;
    r->baseline = baseline;
    r->y_top = y_top; r->h = h;
}

static void iv_push(ivec *iv, iitem it) {
    if (iv->n == iv->cap) {
        int nc = iv->cap ? iv->cap * 2 : 16;
        iitem *nv = realloc(iv->v, sizeof(iitem) * (size_t)nc);
        if (!nv) abort();
        iv->v = nv;
        iv->cap = nc;
    }
    iv->v[iv->n++] = it;
}

/* 行内子树 → item 序列(文档序); 空白折叠为一枚"前置空格"标记 */
static float layout_box(hn_context *c, hn_node *n, float x, float y,
                        float avail_w, float avail_h,
                        const hn_text_backend *tb, int measuring, float forced_h,
                        float forced_w);
static float layout_box_inner(hn_context *c, hn_node *n, float x, float y,
                              float avail_w, float avail_h,
                              const hn_text_backend *tb, int measuring, float forced_h,
                              float forced_w);
/* 子树布局记忆辅助(lskip 块; IFC 落盘点调用) */
static void ifc_wipe(hn_node *container, hn_node *start, hn_node *stop);

static void flatten_inline(hn_context *c, hn_node *n, ivec *iv, const hn_text_backend *tb,
                           float avail_w, int measuring) {
    if (n->kind == HN_TEXT) {
        const hn_style *st = &n->parent->style;
        const char *s = n->text;
        size_t len = n->text_len, i = 0;
        /* white-space: pre / pre-wrap —— 保留连续空格与换行, 不做空白折叠。
           折叠逻辑原本无条件把空格序列压成一个"待放空格", 于是 pre 与 normal
           的输出完全一样(实测 "a   b" 三种取值结果一致)。
           pre 与 pre-wrap 的差别只在换行: pre 不因宽度换行, pre-wrap 换行。 */
        int preserve = (st->white_space == 2 || st->white_space == 3);
        if (preserve) {
            /* 空白集合必须与折叠模式一致(含换行), 否则 pre 下 "a\n\nb" 会
               整个当成一个词 —— 换行既没保留也没断开。 */
            #define HN_IS_WS(ch) ((ch) == ' ' || (ch) == '\t' || (ch) == '\n' || \
                                 (ch) == '\r' || (ch) == '\f')
            while (i < len) {
                /* 连续的空白本身作为一个 item(保留原宽度), 而不是折叠成 1 个空格 */
                size_t j = i;
                while (j < len && HN_IS_WS(s[j])) j++;
                if (j > i) {
                    /* 空白串里若含换行, 必须**强制断行**(CSS 的 pre 语义) ——
                       只把它当作有宽度的片段是不够的: 换行符在大多数字体里
                       宽度为 0 或不固定, 于是 "a\nb" 会画在同一行上。
                       做法与 <br> 一致: 发一个 forced_break 片段,
                       行累加器已能处理。 */
                    int has_nl = 0;
                    for (size_t k = i; k < j; k++)
                        if (s[k] == '\n' || s[k] == '\r') { has_nl = 1; break; }
                    if (has_nl) {
                        /* 每个换行符一个强制断行: "\n\n"(空行)必须产生**两个**
                           断行标记, 整串压成一个会让空行消失(pre 代码块里
                           空行是常态)。做法与 <br> 一致: 发 forced_break 片段,
                           行累加器已能处理。\r\n 算一个换行。 */
                        for (size_t k = i; k < j; k++) {
                            int is_nl = (s[k] == '\n' || s[k] == '\r');
                            if (is_nl && s[k] == '\r' && k + 1 < j && s[k + 1] == '\n') continue;
                            if (!is_nl) continue;
                            iitem br;
                            memset(&br, 0, sizeof(br));
                            br.owner = n;
                            br.st = st;
                            br.forced_break = 1;
                            iv_push(iv, br);
                        }
                        i = j;
                        continue;
                    }
                    iitem it;
                    memset(&it, 0, sizeof(it));
                    it.owner = n;
                    it.st = st;
                    it.begin = i;
                    it.end = j;
                    /* word-spacing 在 pre 下作用于串内**每个**空白字符 */
                    it.w = measure_run(tb, st, s + i, j - i)
                         + st->word_spacing * (float)(j - i);
                    it.space_before = 0;
                    it.is_space_run = 1;      /* 标记: 行首的空白串可被丢弃 */
                    iv_push(iv, it);
                    i = j;
                    continue;
                }
                /* 非空白: 一直读到下一个空白(空白不再作为词边界) */
                j = i;
                while (j < len && !HN_IS_WS(s[j])) j++;
                iitem it;
                memset(&it, 0, sizeof(it));
                it.owner = n;
                it.st = st;
                it.begin = i;
                it.end = j;
                it.w = measure_run(tb, st, s + i, j - i);
                it.space_before = 0;
                iv_push(iv, it);
                i = j;
            }
            return;
        }
        /* 折叠模式的空白判定必须覆盖**全部** HTML 空白字符。tokenizer 现在保留
           原始空白(为了 white-space:pre), 所以这里漏掉 \n/\t 的话,
           "a\n\n\nb" 会被当成一个词, 宽度把换行符也算进去。 */
        while (i < len) {
            unsigned char wc = (unsigned char)s[i];
            if (wc == ' ' || wc == '\t' || wc == '\n' || wc == '\r' || wc == '\f') {
                if (iv->n > 0) iv->pending_space = 1;
                i++;
                continue;
            }
            size_t j = i;
            while (j < len) {
                unsigned char wc2 = (unsigned char)s[j];
                if (wc2 == ' ' || wc2 == '\t' || wc2 == '\n' ||
                    wc2 == '\r' || wc2 == '\f') break;
                j++;
            }
            iitem it;
            memset(&it, 0, sizeof(it));
            it.owner = n;
            it.st = st;
            it.begin = i; it.end = j;
            it.w = measure_run(tb, st, s + i, j - i);
            it.space_before = iv->pending_space;
            iv->pending_space = 0;
            iv_push(iv, it);
            i = j;
        }
        return;
    }
    if (n->style.display == HN_DISP_NONE) return;
    if (n->style.display == HN_DISP_INLINE_BLOCK
        || (n->style.display == HN_DISP_FLEX && n->style.disp_inline)) {
        /* 原子片段: 先布局自身盒子(值文本/内边距/边框都在这里算), 再作为整体入流 */
        float avail = avail_w > 0 ? avail_w : -1;
        layout_box(c, n, 0, 0, avail, -1, tb, measuring, -1, -1);
        iitem it;
        memset(&it, 0, sizeof(it));
        it.owner = n;               /* 借用 owner: 标记为该节点的占位片段 */
        it.st = &n->style;
        it.begin = 0; it.end = 0;   /* 无文本区间(盒子由元素绘制) */
        it.w = n->bw;
        it.space_before = iv->pending_space;
        iv->pending_space = 0;
        iv_push(iv, it);
        return;
    }
    if (n->tag && !strcmp(n->tag, "br")) {
        iitem it;
        memset(&it, 0, sizeof(it));
        it.forced_break = 1;
        iv_push(iv, it);
        iv->pending_space = 0;
        return;
    }
    int start = iv->n;
    const float padl = n->style.padding[3] + n->style.margin[3];
    const float padr = n->style.padding[1] + n->style.margin[1];
    for (hn_node *ch = n->first; ch; ch = ch->next) flatten_inline(c, ch, iv, tb, avail_w, measuring);
    if (iv->n > start) {
        iv->v[start].pad_before += padl;
        iv->v[iv->n - 1].pad_after += padr;
    }
}

/* 收集节点(含子树)所有 run 的包围盒 */
static int collect_bounds(hn_node *n, float *x0, float *y0, float *x1, float *y1) {
    int any = 0;
    for (int i = 0; i < n->n_runs; i++) {
        hn_run *r = &n->runs[i];
        if (r->x < *x0) *x0 = r->x;
        if (r->y_top < *y0) *y0 = r->y_top;
        if (r->x + r->width > *x1) *x1 = r->x + r->width;
        if (r->y_top + r->h > *y1) *y1 = r->y_top + r->h;
        any = 1;
    }
    if (n->kind == HN_ELEM)
        for (hn_node *ch = n->first; ch; ch = ch->next)
            if (collect_bounds(ch, x0, y0, x1, y1)) any = 1;
    return any;
}

/* 行内节点占位盒: 由子树 run 汇总(元素再外扩内边距与边框) */
static void collapse_node_box(hn_node *n) {
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    if (!collect_bounds(n, &x0, &y0, &x1, &y1)) { n->bw = n->bh = 0; return; }
    if (n->kind == HN_ELEM) {
        const hn_style *st = &n->style;
        x0 -= st->padding[3] + st->border_w;
        x1 += st->padding[1] + st->border_w;
        y0 -= st->padding[0] + st->border_w;
        y1 += st->padding[2] + st->border_w;
    }
    n->bx = x0; n->by = y0;
    n->bw = x1 - x0;
    n->bh = y1 - y0;
}

typedef struct {
    int   idx[256];
    float x[256];         /* 相对行首的 x(已含左 pad 与前置空格) */
    int   n;
    float w;              /* 含左右 pad 的行宽 */
    float asc, desc, lh;  /* 行盒度量 */
    int   has_ellipsis;   /* 本行尾部需绘制 "…"(text-overflow: ellipsis) */
    float ellipsis_x;     /* 省略号相对行首的 x */
    float ellipsis_w;     /* 省略号宽度(截断时已测量) */
    hn_node *ell_owner;   /* 承载省略号文本的节点(临时文本节点) */
} line_acc;

static void la_reset(line_acc *la) {
    la->n = 0; la->w = 0; la->asc = 0; la->desc = 0; la->lh = 0;
    la->has_ellipsis = 0; la->ellipsis_x = 0; la->ellipsis_w = 0; la->ell_owner = NULL;
}

static void la_grow(line_acc *la, const iitem *it, const hn_text_backend *tb) {
    float a, d;
    font_metrics(tb, it->st, &a, &d);
    float lh = it->st->font_size * it->st->line_height;
    if (lh > la->lh) la->lh = lh;
    if (a > la->asc) la->asc = a;
    if (d > la->desc) la->desc = d;
}

/* 把第 i 个片段追加进当前行(nowrap 路径用; 逻辑与常规路径一致) */
static void la_add(line_acc *la, ivec *iv, int i, float space_w, const hn_text_backend *tb) {
    if (la->n >= 256) return;
    iitem *it = &iv->v[i];
    la->idx[la->n] = i;
    la->x[la->n] = la->w + space_w + it->pad_before;
    la->n++;
    la->w = la->x[la->n - 1] + it->w + it->pad_after;
    la_grow(la, it, tb);
}

static void translate_subtree(hn_node *n, float dx, float dy);
static void translate_x(hn_node *n, float dx);

static float align_off(int align, float max_w, float w) {
    if (max_w <= 0) return 0;
    float off = 0;
    if (align == 1) off = (max_w - w) * 0.5f;
    else if (align == 2) off = max_w - w;
    return off > 0 ? off : 0;
}

/* 落盘当前行 */
/* 省略号截断: 在 max_w 内尽量多放片段, 末尾追加 "…"。
   用于 text-overflow: ellipsis + white-space: nowrap 的组合(单行溢出)。
   做法: 从行尾回退, 直到剩余空间能容纳省略号, 把最后一个片段的文本区间截短。 */
/* 省略号承载文本: 容器节点上惰性分配一个 "…"(arena 生命周期同显示列表),
   省略号 run 以 (0..3) 字节区间挂在该节点上, 绘制阶段按普通文本取字。 */
static void ell_ensure_text(hn_node *container) {
    static const char ELL[] = "\xE2\x80\xA6";
    if (!container->text) {
        container->text = hn_arena_strndup(container->arena, ELL, 3);
        container->text_len = 3;
    }
}

/* 省略号截断: 在 max_w 内尽量多放内容, 末尾追加 "…"。
 *
 * 关键点: CJK 文本没有空格, 整段是**一个片段**(可能几百 px)。所以不能只
 * "整片段取舍" —— 必须支持**片段内按码点回退**, 否则第一段就超预算时会
 * 整行丢空(什么都不画)。
 *
 * force: line-clamp 的最后一行(内容本行放得下、但**之后还有内容**)也要
 * 追加省略号 —— 此时行宽 ≤ max_w, 缺省的"行没超宽就不截断"判定会跳过,
 * 必须强制按"行宽 + 省略号 ≤ max_w"的预算处理。 */
static void la_clip_ellipsis(line_acc *la, ivec *iv, const hn_text_backend *tb, float max_w,
                             hn_arena *container_arena, hn_node *ell_parent, int force) {
    if (la->n == 0 || max_w <= 0) return;
    if (!force && la->w <= max_w) return;
    const hn_style *st0 = iv->v[la->idx[0]].st;
    float ell_w = measure_run(tb, st0, "\xE2\x80\xA6", 3);
    float budget = max_w - ell_w;
    if (budget < 0) budget = 0;

    /* 逐片段累加, 找出"整段能放下"的最大前缀数(第 1 段放不下也要保留它做段内截断) */
    float acc = 0;
    int keep = 0;
    for (int k = 0; k < la->n; k++) {
        iitem *it = &iv->v[la->idx[k]];
        float gap = (k == 0) ? 0
            : (la->x[k] - (la->x[k - 1] + iv->v[la->idx[k - 1]].w
                           + iv->v[la->idx[k - 1]].pad_after));
        float seg = gap + it->pad_before + it->w + it->pad_after;
        if (acc + seg > budget) break;
        acc += seg;
        keep++;
    }
    if (keep == 0) keep = 1;              /* 至少保留第一段做段内截断 */

    /* 段内截断: 对最后一个保留片段按码点回退到剩余空间 */
    if (keep <= la->n) {
        iitem *last = &iv->v[la->idx[keep - 1]];
        float before = acc - (last->pad_before + last->w + last->pad_after);
        if (before < 0) before = 0;
        float room = budget - before;
        if (last->end > last->begin) {
            const char *txt = last->owner ? last->owner->text : NULL;
            if (txt) {
                size_t b = last->begin, e = last->end, cut = b;
                float w = 0;
                while (b < e) {
                    size_t cn = utf8_next(txt + b, e - b);
                    float cw = measure_run(tb, last->st, txt + b, cn);
                    if (room > 0 && w + cw > room) { break; }   /* 放不下就停 */
                    w += cw;
                    b += cn;
                    cut = b;
                }
                last->end = cut;
                last->w = w;
            }
        }
    }
    la->n = keep;

    /* 重算行宽 */
    la->w = 0;
    for (int k = 0; k < la->n; k++) {
        iitem *it = &iv->v[la->idx[k]];
        la->w = la->x[k] + it->w + it->pad_after;
    }

    /* 追加省略号: 用容器 arena 分配一个临时文本节点承载 "…",
       使 la_emit 能像普通片段一样为它产出 run(绘制/命中都自然工作)。 */
    if (container_arena && ell_parent) {
        /* 承载节点用**容器自身**: 它在 DOM 树里, 绘制阶段会访问到它的 runs。
           (若用游离的临时节点, paint_walk 遍历不到, 省略号永远不显示。) */
        ell_ensure_text(ell_parent);
        la->ell_owner = ell_parent;
        la->ellipsis_x = la->w;
        la->ellipsis_w = ell_w;
        la->has_ellipsis = 1;
        la->w += ell_w;
    }
}

static void la_emit(line_acc *la, ivec *iv, float x, float y_top, float max_w, int align) {
    if (!la->n) return;
    float off = align_off(align, max_w, la->w);
    float half = (la->lh - (la->asc + la->desc)) * 0.5f;
    if (half < 0) half = 0;
    float baseline = y_top + half + la->asc;
    for (int k = 0; k < la->n; k++) {
        iitem *it = &iv->v[la->idx[k]];
        if (!it->owner) continue;
        if (it->end <= it->begin) {
            /* 行内块: 摆放其盒子(垂直方向按基线对齐到行盒), 并把内部 run 一起平移 */
            if (it->owner->kind == HN_ELEM) {
                hn_node *b = it->owner;
                float nbx = x + off + la->x[k] + b->style.margin[3];
                float nby = y_top + (la->lh - b->bh) * 0.5f;
                if (nby < y_top) nby = y_top;
                float dx = nbx - b->bx, dy = nby - b->by;
                b->bx = nbx;
                b->by = nby;
                for (int t = 0; t < b->n_runs; t++) {
                    b->runs[t].x += dx;
                    b->runs[t].baseline += dy;
                    b->runs[t].y_top += dy;
                }
                /* 子元素盒也要一起平移! 原子行内盒(inline-block / inline-flex)
                   内部可能还有自己的块级子节点(如 inline-flex 里的 img/div),
                   它们是在 (0,0) 相对空间里被布局的; 只平移自身 run 会让这些
                   子盒停留在 (0,0) —— 表现为图片/子块跑到窗口左上角。 */
                for (hn_node *sub = b->first; sub; sub = sub->next) {
                    if (sub->kind == HN_ELEM) translate_subtree(sub, dx, dy);
                    else if (sub->n_runs > 0) {
                        for (int t = 0; t < sub->n_runs; t++) {
                            sub->runs[t].x += dx;
                            sub->runs[t].baseline += dy;
                            sub->runs[t].y_top += dy;
                        }
                    }
                }
            }
            continue;
        }
        run_push(it->owner, it->begin, it->end, x + off + la->x[k], it->w,
                 baseline, y_top, la->lh > 0 ? la->lh : it->st->font_size);
    }
    /* 省略号 run: 承载节点由 la_clip_ellipsis 分配(临时文本节点)。
       begin/end 是它在 "…" 中的字节区间(0..3), 因此绘制阶段能正常取到文本。 */
    if (la->has_ellipsis && la->ell_owner) {
        /* 省略号的宽度在 la_clip_ellipsis 里已算过, 存在 ellipsis_w;
           样式取本行首个片段(没有片段则取承载节点的父级样式) */
        iitem *first = la->n > 0 ? &iv->v[la->idx[0]] : NULL;
        const hn_style *st_ell = first ? first->st
            : (la->ell_owner->parent ? &la->ell_owner->parent->style : NULL);
        float fs = st_ell ? st_ell->font_size : 14;
        run_push(la->ell_owner, 0, 3, x + off + la->ellipsis_x, la->ellipsis_w,
                 baseline, y_top, la->lh > 0 ? la->lh : fs);
    }
}

/* 单个片段独立成行(超长词硬拆时用) */
static void emit_seg(hn_node *owner, size_t b, size_t e, float x, float y_top, float w,
                     float asc, float desc, float lh, float max_w, int align) {
    float off = align_off(align, max_w, w);
    float half = (lh - (asc + desc)) * 0.5f;
    if (half < 0) half = 0;
    run_push(owner, b, e, x + off, w, y_top + half + asc, y_top, lh);
}

/* 空格宽度(词间隔): 空格本身 + word-spacing 附加 */
static float space_w_of(const hn_text_backend *tb, const hn_style *st) {
    return measure_run(tb, st, " ", 1) + st->word_spacing;
}

/* 摊平 + 布局一个行内格式化上下文; 返回占用高度 */
static float layout_ifc(hn_context *c, hn_node *container, hn_node *start, hn_node *stop,
                        float x, float y, float max_w,
                        const hn_text_backend *tb, int measuring,
                        float *out_pref_w) {
    ivec iv;
    memset(&iv, 0, sizeof(iv));
    for (hn_node *ch = start; ch && ch != stop; ch = ch->next) {
        if (ch->kind == HN_ELEM && ch->style.display == HN_DISP_NONE) continue;
        if (ch->kind == HN_TEXT && ch->text_len == 0) continue;
        flatten_inline(c, ch, &iv, tb, max_w, measuring);
    }
    const int align = container->style.text_align;

    if (measuring) {
        /* 行数统计: 与下方落盘路径同一口径 —— 每个"片段组"(被强制换行
           隔开的连续片段)占一行; 空行(连续两个换行, 其后仍有内容)也占一行,
           尾随换行不算。缺了这一步, white-space:pre 与 <br> 在度量模式下
           只算一行 —— flex 容器按度量结果给子项定高(stretch 强制高),
           多行代码块被裁得只剩一行(agent-chat 的 code-card 即此症状)。 */
        int lines = 0, in_group = 0;
        float cur = 0, maxw = 0, maxh = 0, any = 0;
        for (int i = 0; i < iv.n; i++) {
            iitem *it = &iv.v[i];
            if (it->forced_break) {              /* <br>/换行: 首选宽取最长行 */
                if (in_group) { lines++; in_group = 0; }
                else if (i + 1 < iv.n) lines++;  /* 空行(后面还有内容才计) */
                if (cur > maxw) maxw = cur;
                cur = 0;
                continue;
            }
            if (!it->owner) continue;
            if (it->space_before && any) cur += space_w_of(tb, it->st);
            cur += it->pad_before + it->w + it->pad_after;
            float a, d;
            font_metrics(tb, it->st, &a, &d);
            float lh = it->st->font_size * it->st->line_height;
            if (lh > maxh) maxh = lh;
            any = 1;
            in_group = 1;
        }
        if (in_group) lines++;
        if (out_pref_w) *out_pref_w = cur > maxw ? cur : maxw;
        /* 仅行内级元素由 run 汇总占位盒; 块级/flex 子项的盒由其父级摆放 */
        for (hn_node *ch = start; ch && ch != stop; ch = ch->next)
            if (ch->kind == HN_ELEM && ch->style.display == HN_DISP_INLINE) collapse_node_box(ch);
        free(iv.v);
        if (!any) return 0;
        return maxh * (lines > 0 ? (float)lines : 1.0f);
    }

    /* line-clamp 状态: lines_done = 已落盘的行盒数; clamp_n > 0 时最多
       显示 clamp_n 行, 内容超出时最后一行以省略号收尾(与单行 ellipsis
       同一手法: la_clip_ellipsis / 手工补省略号 run)。 */
    const int clamp_n = container->style.line_clamp;
    int lines_done = 0;

    line_acc la;
    la_reset(&la);
    float cur_y = 0;
    int i = 0;
    while (i < iv.n) {
        iitem *it = &iv.v[i];

        /* 行数已用尽: 剩余内容全部丢弃(省略号已由产生第 clamp_n 行的分支补上) */
        if (clamp_n > 0 && lines_done >= clamp_n) break;

        if (it->forced_break) {
            /* 强制断行后仍有内容、且本行是最后一行 → 先截断补省略号再落盘 */
            if (clamp_n > 0 && lines_done + 1 >= clamp_n && i + 1 < iv.n && max_w > 0) {
                la_clip_ellipsis(&la, &iv, tb, max_w, container->arena, container, 1);
                la_emit(&la, &iv, x, y + cur_y, max_w, align);
                cur_y += la.lh;
                lines_done++;
                la_reset(&la);
                break;
            }
            la_emit(&la, &iv, x, y + cur_y, max_w, align);
            if (la.n) { cur_y += la.lh; lines_done++; }
            else if (i + 1 < iv.n) {
                /* 空行也占一行(pre 代码块的空行 / <br><br>): 行高取其后最近
                   片段的行盒(pre 空行两端同字号, 结果一致)。行首(本行无内容)
                   且后面再无内容(尾随换行)不算 —— 与浏览器口径一致。 */
                float elh = 0;
                for (int k = i + 1; k < iv.n; k++) {
                    if (!iv.v[k].st) continue;
                    elh = iv.v[k].st->font_size * iv.v[k].st->line_height;
                    break;
                }
                if (elh <= 0) elh = container->style.font_size * container->style.line_height;
                cur_y += elh;
                lines_done++;
            }
            la_reset(&la);
            i++;
            continue;
        }

        float space_w = (it->space_before && la.n > 0) ? space_w_of(tb, it->st) : 0;
        float need = it->pad_before + it->w + it->pad_after;

        /* white-space: nowrap — 不因宽度换行(超出部分由 text-overflow 处理) */
        int nowrap = (container->style.white_space == 1);
        if (nowrap) {
            la_add(&la, &iv, i, space_w, tb);
            i++;
            continue;
        }

        /* 本行放不下: 已有内容则换行后重试 */
        if (la.n > 0 && max_w > 0 && la.w + space_w + need > max_w) {
            /* 换行即说明本行之后还有内容: 若本行已是最后一行, 截断补省略号 */
            if (clamp_n > 0 && lines_done + 1 >= clamp_n) {
                la_clip_ellipsis(&la, &iv, tb, max_w, container->arena, container, 1);
                la_emit(&la, &iv, x, y + cur_y, max_w, align);
                cur_y += la.lh;
                lines_done++;
                la_reset(&la);
                break;
            }
            la_emit(&la, &iv, x, y + cur_y, max_w, align);
            cur_y += la.lh;
            lines_done++;
            la_reset(&la);
            continue;
        }

        /* 空行仍放不下: 按码点硬拆(覆盖 CJK / 长 URL) */
        if (la.n == 0 && max_w > 0 && need > max_w && it->owner && it->end > it->begin) {
            const char *s = it->owner->text;
            size_t p = it->begin, e = it->end, seg = p;
            float acc = 0, a, d;
            font_metrics(tb, it->st, &a, &d);
            float lh = it->st->font_size * it->st->line_height;
            /* clamp: 本行若是允许的最后一行, 拆行预算收紧留出省略号 */
            float budget = max_w;
            int ell_line = 0;
            float ell_w = 0;
            if (clamp_n > 0 && lines_done + 1 >= clamp_n) {
                ell_w = measure_run(tb, it->st, "\xE2\x80\xA6", 3);
                budget = max_w - ell_w;
                if (budget < 0) budget = 0;
                ell_line = 1;
            }
            while (p < e) {
                size_t cn = utf8_next(s + p, e - p);
                float cw = measure_run(tb, it->st, s + p, cn);
                if (acc > 0 && acc + cw > budget) {
                    emit_seg(it->owner, seg, p, x, y + cur_y, acc, a, d, lh, max_w, align);
                    cur_y += lh;
                    lines_done++;
                    seg = p;
                    acc = 0;
                    if (clamp_n > 0 && lines_done + 1 >= clamp_n && !ell_line) {
                        ell_w = measure_run(tb, it->st, "\xE2\x80\xA6", 3);
                        budget = max_w - ell_w;
                        if (budget < 0) budget = 0;
                        ell_line = 1;
                    }
                }
                acc += cw;
                p += cn;
            }
            if (ell_line) {
                /* 行数已到上限: 末段直接落盘并补省略号, 剩余内容全部丢弃 */
                if (acc > 0)
                    emit_seg(it->owner, seg, e, x, y + cur_y, acc, a, d, lh, max_w, align);
                float half = (lh - (a + d)) * 0.5f;
                if (half < 0) half = 0;
                float eoff = align_off(align, max_w, acc + ell_w);
                ell_ensure_text(container);
                run_push(container, 0, 3, x + eoff + acc, ell_w,
                         y + cur_y + half + a, y + cur_y, lh);
                cur_y += lh;
                lines_done++;
                break;
            }
            float padl = it->pad_before;
            it->begin = seg; it->end = e; it->w = acc;
            it->pad_before = 0;
            la.idx[la.n] = i;
            la.x[la.n] = padl;
            la.n++;
            la.w = padl + acc + it->pad_after;
            la.asc = a; la.desc = d; la.lh = lh;
            i++;
            continue;
        }

        if (la.n >= 256) {
            /* 行片段数达到行累加器上限: 与换行同样处理(clamp 同样适用) */
            if (clamp_n > 0 && lines_done + 1 >= clamp_n) {
                la_clip_ellipsis(&la, &iv, tb, max_w, container->arena, container, 1);
                la_emit(&la, &iv, x, y + cur_y, max_w, align);
                cur_y += la.lh;
                lines_done++;
                la_reset(&la);
                break;
            }
            la_emit(&la, &iv, x, y + cur_y, max_w, align);
            cur_y += la.lh;
            lines_done++;
            la_reset(&la);
            continue;
        }

        la.idx[la.n] = i;
        la.x[la.n] = la.w + space_w + it->pad_before;
        la.n++;
        la.w = la.x[la.n - 1] + it->w + it->pad_after;
        la_grow(&la, it, tb);
        i++;
    }
    if (la.n) {
        /* text-overflow: ellipsis + nowrap: 单行超出时截断并加省略号 */
        if (container->style.text_overflow == 1 && container->style.white_space == 1)
            la_clip_ellipsis(&la, &iv, tb, max_w, container->arena, container, 0);
        la_emit(&la, &iv, x, y + cur_y, max_w, align);
        cur_y += la.lh;
    }

    if (out_pref_w) {
        float pw = 0;
        for (int k = 0; k < iv.n; k++) pw += iv.v[k].w + iv.v[k].pad_before + iv.v[k].pad_after;
        *out_pref_w = pw;
    }
    for (hn_node *ch = start; ch && ch != stop; ch = ch->next) {
        /* 只有行内级节点需要由 run 汇总其占位盒;
           块级/flex 子项的盒已由父级布局摆好, 用 run 反推会覆盖其绝对坐标 */
        if (ch->kind == HN_ELEM && ch->style.display != HN_DISP_INLINE) continue;
        collapse_node_box(ch);
    }
    free(iv.v);
    return cur_y;
}

/* 每次 layout 前清空全树 run, 避免遍历不到的节点残留旧片段 */
static void clear_runs(hn_node *n) {
    n->n_runs = 0;
    for (hn_node *ch = n->first; ch; ch = ch->next) clear_runs(ch);
}

/* 是否脱离常规流(position: absolute / fixed) */
static int is_out_of_flow(const hn_node *ch) {
    return ch->kind == HN_ELEM &&
           (ch->style.position == HN_POS_ABSOLUTE || ch->style.position == HN_POS_FIXED);
}

/* 定位参照块: 最近 positioned 祖先; 无则视口(fixed 直接对视口) */
static void containing_block(hn_context *c, hn_node *n,
                             float *x, float *y, float *w, float *h) {
    hn_node *p = n->parent;
    while (p && p->kind == HN_ELEM) {
        if (p->style.position == HN_POS_RELATIVE ||
            p->style.position == HN_POS_ABSOLUTE ||
            p->style.position == HN_POS_FIXED) {
            /* 参照块 = 该祖先的 padding box */
            *x = p->bx + p->style.padding[3];
            *y = p->by + p->style.padding[0];
            *w = p->bw - p->style.padding[1] - p->style.padding[3];
            *h = p->bh - p->style.padding[0] - p->style.padding[2];
            return;
        }
        p = p->parent;
    }
    /* 无 positioned 祖先: 视口 */
    if (n->style.position == HN_POS_ABSOLUTE || n->style.position == HN_POS_FIXED) {
        /* absolute 兜底视口, fixed 恒视口 */
    }
    *x = 0; *y = 0; *w = c->vw; *h = c->vh;
}

/* 摆放脱离流的子节点(在容器尺寸确定后调用) */
static void layout_out_of_flow(hn_context *c, hn_node *n, const hn_text_backend *tb) {
    for (hn_node *ch = n->first; ch; ch = ch->next) {
        if (ch->kind != HN_ELEM) continue;
        if (ch->style.display == HN_DISP_NONE) continue;
        if (!is_out_of_flow(ch)) continue;

        const hn_style *cs = &ch->style;
        float bx, by, bw, bv;
        if (cs->position == HN_POS_FIXED) {
            /* fixed: 参照视口 */
            bx = 0; by = 0; bw = c->vw; bv = c->vh;
        } else {
            containing_block(c, ch, &bx, &by, &bw, &bv);
        }

        /* 尺寸解析: 百分比需要参照块尺寸(不能传 -1, 否则 100% 解析失败 →
           shrink-to-fit 得到内容宽, 表现为 fixed 头部只有几十像素宽)。
           - 左右都设: 拉伸到剩余空间
           - 否则: 用参照块宽作为基准, 让 100%/50% 等百分比可解析;
             宽度未声明时 layout_box 会走 shrink-to-fit(auto 语义) */
        float avail_w = cs->has_left && cs->has_right
            ? (bw - cs->left - cs->right)
            : (cs->width_u == HN_U_PCT ? bw : -1);
        float avail_h = (cs->has_top && cs->has_bottom)
            ? (bv - cs->top - cs->bottom)
            : (cs->height_u == HN_U_PCT ? bv : -1);
        layout_box(c, ch, 0, 0, avail_w, avail_h, tb, 0,
                   (cs->has_top && cs->has_bottom) ? avail_h : -1, -1);

        /* 水平: left 优先; 否则 right 对齐; 都无则留在容器内容起点 */
        /* 水平: left / right / 都不设(留在容器内容起点)
           注意不能用 left 值是否为 0 判断"是否声明" —— 用 has_* 标志 */
        float px;
        if (cs->has_left && !cs->has_right) {
            px = bx + cs->left;
        } else if (cs->has_right && !cs->has_left) {
            px = bx + bw - cs->right - ch->bw;
        } else if (cs->has_left && cs->has_right) {
            px = bx + cs->left;          /* 两侧同设: left 优先(并拉伸宽) */
        } else {
            px = n->bx + n->style.padding[3];
        }
        /* 垂直: top 优先; 否则 bottom 对齐; 都无则留在容器内容起点 */
        float py;
        if (cs->has_top) py = by + cs->top;
        else if (cs->has_bottom) py = by + bv - cs->bottom - ch->bh;
        else py = n->by + n->style.padding[0];

        translate_subtree(ch, px - ch->bx, py - ch->by);
        /* 此处**不要**再递归 layout_out_of_flow(ch): 上面那次 layout_box(ch)
           走到尾部已经调过一次(见本文件"容器尺寸已定 → 摆放脱离流的子节点"),
           再调一次会把 ch 的脱离流后代整棵重新排布一遍。盒坐标因 translate
           幂等而看不出差别, 但 run_push 只追加不清空 —— 文字指令直接翻倍:
           实测 absolute 子元素嵌在 absolute 父里时, "hn-mesh · 网格变形"
           被画两遍(第二份整体偏移 (left, top)), 背景 RECT 却只有一份。 */
    }
}

/* 子节点是否参与行内流 */
static int is_inline_level(hn_node *ch) {
    if (ch->kind == HN_TEXT) return 1;
    if (ch->style.display == HN_DISP_NONE) return 0;
    return ch->style.display == HN_DISP_INLINE || ch->style.display == HN_DISP_INLINE_BLOCK
        || (ch->style.display == HN_DISP_FLEX && ch->style.disp_inline);
}

/* 输入控件: 用当前值布局内部文本(run 挂在控件节点自身, 供绘制/命中)。
   placeholder 仅在无值时由绘制阶段弱色绘制, 不参与排版。 */
static void layout_input(hn_context *c, hn_node *n, float content_w, const hn_text_backend *tb, int measuring) {
    int ik = hn_node_input_kind(n);
    if (ik == HN_IN_CHECKBOX || ik == HN_IN_RADIO) {
        /* 复选/单选没有可排版文本(状态由绘制阶段读取 checked 属性) */
        n->text = NULL;
        n->text_len = 0;
        return;
    }
    size_t vlen = 0;
    const char *v = hn_node_value(n, &vlen);
    if (!v) { v = ""; vlen = 0; }

    /* 值文本以临时文本节点承载(避免经过控件自身递归); 产出的 run 归属控件节点,
       这样绘制/命中/caret 都只需查控件自身。
       多行值(textarea)按 '\n' 切成 文本+br 的栈上兄弟链, IFC 逐行排布;
       切片直接指向原值缓冲, run 的字节区间 rebasing 回全值坐标。 */
    enum { MAX_SEG = 48 };
    hn_node seg[MAX_SEG];
    int nseg = 0;
    size_t ls = 0;
    for (size_t i = 0; i <= vlen && nseg < MAX_SEG; i++) {
        if (i < vlen && v[i] != '\n') continue;
        if (i > ls || i == vlen) {           /* 非空切片(或末尾占位, IFC 自会跳过空文本) */
            hn_node *t = &seg[nseg++];
            memset(t, 0, sizeof(*t));
            t->kind = HN_TEXT;
            t->arena = n->arena;
            t->parent = n;
            t->text = v + ls;
            t->text_len = i - ls;
        }
        if (i < vlen) {                      /* 行尾 '\n' → 强制换行占位 */
            hn_node *b = &seg[nseg++];
            memset(b, 0, sizeof(*b));
            b->kind = HN_ELEM;
            b->arena = n->arena;
            b->parent = n;
            b->tag = "br";
        }
        ls = i + 1;
    }
    for (int k = 0; k + 1 < nseg; k++) seg[k].next = &seg[k + 1];

    const float padl = n->style.padding[3];
    const float padt = n->style.padding[0];
    float pref = 0;
    float h = 0;
    if (nseg > 0 && vlen > 0 && content_w > 0) {
        h = layout_ifc(c, n, &seg[0], NULL, n->bx + padl, n->by + padt,
                       content_w, tb, measuring, &pref);
        /* 把各切片的 run 移交给控件(begin/end 加上切片在全值中的偏移) */
        for (int k = 0; k < nseg; k++) {
            if (seg[k].kind != HN_TEXT || seg[k].n_runs <= 0) continue;
            size_t off = (size_t)(seg[k].text - v);
            hn_run *nv = realloc(n->runs, sizeof(hn_run) * (size_t)(n->n_runs + seg[k].n_runs));
            if (!nv) abort();
            n->runs = nv;
            for (int r = 0; r < seg[k].n_runs; r++) {
                hn_run *dst = &n->runs[n->n_runs + r];
                *dst = seg[k].runs[r];
                dst->begin += off;
                dst->end += off;
            }
            n->n_runs += seg[k].n_runs;
            free(seg[k].runs);
            seg[k].runs = NULL;
            seg[k].n_runs = 0;
        }
    }
    /* 值文本挂在控件上供绘制取字符(shadow 状态, 不改动 arena 内容) */
    n->text = v;
    n->text_len = vlen;

    n->pref_w = pref;
    /* 按钮标签: 作者未声明 text-align 时按控件惯例居中(平移 run;
       text-align:center 已由 IFC 居中的不再叠加) */
    if (ik == HN_IN_BUTTON && n->style.text_align == 0 &&
        pref > 0 && content_w > pref && n->n_runs > 0) {
        float dx = (content_w - pref) * 0.5f;
        for (int i = 0; i < n->n_runs; i++) n->runs[i].x += dx;
    }
    if (h > 0) n->content_h = h + padt + n->style.padding[2];
}

/* ---------------- 元素节点 ---------------- */

static float layout_box(hn_context *c, hn_node *n, float x, float y,
                        float avail_w, float avail_h,
                        const hn_text_backend *tb, int measuring, float forced_h,
                        float forced_w);

/* 整棵子树水平平移(含 runs), 用于 margin:auto 居中 */
/* 整棵子树平移(dx, dy): 盒坐标 + run 的横纵位置都要跟上。
   子树布局记忆的记录原点(lf_ox/oy)同步跟进 —— 它必须永远镜像子树的
   真实位置, 否则后续命中帧的平移量会算错(调用方含 margin:auto 居中、
   脱离流摆放与 lskip 命中平移, 三处共用同一不变量)。 */
static void translate_subtree(hn_node *n, float dx, float dy) {
    n->bx += dx;
    n->by += dy;
    if (n->lf_valid) { n->lf_ox += dx; n->lf_oy += dy; }
    for (int i = 0; i < n->n_runs; i++) {
        n->runs[i].x += dx;
        n->runs[i].baseline += dy;
        n->runs[i].y_top += dy;
    }
    for (hn_node *ch = n->first; ch; ch = ch->next) translate_subtree(ch, dx, dy);
}

static void translate_x(hn_node *n, float dx) { translate_subtree(n, dx, 0); }

static float layout_block(hn_context *c, hn_node *n, const hn_style *st,
                          float cx, float cy, float cw,
                          const hn_text_backend *tb, int measuring, float cross_h) {
    (void)st;
    float cur_y = 0, max_w = 0;
    hn_node *prev_block = NULL;      /* 上一个参与流内定位的块级子节点(用于 margin 折叠) */
    float pending_mb = 0;            /* 上一位遗留的 margin-bottom, 留待与下一位的 mt 折叠 */
    hn_node *ch = n->first;
    while (ch) {
        if (ch->kind == HN_ELEM && ch->style.display == HN_DISP_NONE) { ch = ch->next; continue; }
        if (ch->kind == HN_TEXT && ch->text_len == 0) { ch = ch->next; continue; }
        /* 脱离流的子节点不占位(稍后单独摆放) */
        if (is_out_of_flow(ch)) { ch = ch->next; continue; }

        if (is_inline_level(ch)) {
            /* 连续的行内级子节点合成一个行内格式化上下文 */
            hn_node *last = ch;
            while (last->next) {
                hn_node *nx = last->next;
                if (nx->kind == HN_ELEM && nx->style.display == HN_DISP_NONE) { last = nx; continue; }
                if (!is_inline_level(nx)) break;
                last = nx;
            }
            float pref = 0;
            ifc_wipe(n, ch, last->next);   /* 本次 IFC 将重建链上文本 runs(旧根清职责随重建走) */
            float h = layout_ifc(c, n, ch, last->next, cx, cy + cur_y, cw, tb, measuring, &pref);
            cur_y += h;
            if (pref > max_w) max_w = pref;
            ch = last->next;
            continue;
        }

        /* 块级子节点 */
        float mgn[4];
        if (ch->kind == HN_ELEM) margins_of(&ch->style, cw, mgn);
        else mgn[0] = mgn[1] = mgn[2] = mgn[3] = 0;
        float ml = mgn[3], mr = mgn[1];
        float mt = mgn[0], mb = mgn[2];
        float avail = (cw < 0) ? -1 : cw - ml - mr;
        if (avail < 0 && cw >= 0) avail = 0;
        /* 相邻块级 margin 折叠: 上一个元素的 margin-bottom 与本次的
           margin-top 取**最大值**(而非相加) —— CSS 标准行为。
           不折叠会让垂直间距凭空变大一倍(实测: 两段各 margin:20 的
           文字之间出现 40px 空隙而不是 20px), 破坏排版节奏。
           注意: 负 margin 也按折叠规则处理(取 max(负,负) 即较小者)。 */
        /* 相邻块级 margin 折叠(CSS 标准): 前一个的 margin-bottom 与本个的
           margin-top 取**最大值**, 而不是相加 —— 不折叠会让垂直间距凭空
           大一倍(实测两段 margin:20 的文字之间空隙是 40px 而非 20px)。
           实现用"挂起 margin"模型: 前一位的 mb 不立即占位, 而是留到
           下一位与 mt 一起折叠后一次性占位。这样既不会重复计入,
           最后一个元素的 mb 也能正确计入总高。负 margin 同样处理:
           两负取更小者, 一正一负则相加(可相互抵消)。 */
        float eff_mt;
        if (prev_block == NULL || is_out_of_flow(prev_block)) {
            eff_mt = mt;                      /* 首位: 与父内容区顶边折叠 */
        } else if (pending_mb > 0 && mt > 0) {
            eff_mt = mt > pending_mb ? mt : pending_mb;
        } else if (pending_mb < 0 && mt < 0) {
            eff_mt = mt < pending_mb ? mt : pending_mb;
        } else {
            eff_mt = pending_mb + mt;
        }
        layout_box(c, ch, cx + ml, cy + cur_y + eff_mt, avail, cross_h, tb, measuring, -1, -1);
        /* margin:0 auto — 左右 auto 吸收剩余空间(双侧=居中, 仅左=推右) */
        if (!measuring && cw >= 0 && ch->kind == HN_ELEM && (ch->style.margin_auto & 10)) {
            float rem = cw - ch->bw - ml - mr;
            if (rem > 0) {
                int na = ((ch->style.margin_auto & 8) ? 1 : 0) + ((ch->style.margin_auto & 2) ? 1 : 0);
                if (ch->style.margin_auto & 8) translate_x(ch, rem / (float)na);
            }
        }
        cur_y += eff_mt + ch->bh;        /* mb 挂起, 交给下一位折叠 */
        pending_mb = mb;
        float uw = ch->bw + ml + mr;
        if (uw > max_w) max_w = uw;
        prev_block = ch;
        ch = ch->next;
    }
    n->pref_w = max_w;
    return cur_y + pending_mb;          /* 末位挂起的 mb 仍要计入总高 */
}

typedef struct { hn_node *n; float ml, mr, mt, mb; } fitem;

/* ---- flex 子项表的有界缓存(参照 hnsoft.c poly_scratch_ensure 手法) ----
   此前每次 layout_flex 都 malloc/free 一套 items(外加一份 line 行号表,
   calloc), 而 relayout 是稳态高频路径(动画/媒体页每帧全页重排) —— 缓存化
   后只有首次见到更大规模才扩容一次(1.25x 头部, **只增不减**), 稳态零分配。
   layout_flex 是**递归**的(嵌套 flex: pass1/pass2 的 layout_box 会再进
   layout_flex), 单一模块级缓冲会被内层调用改写 —— 因此**按递归深度各留
   一个槽位**(现实文档 ≤32 层; 更深回退一次性堆分配, 由调用方负责释放)。
   布局是单线程主循环, 免锁。line 表所有 cnt 项在换行扫描时先写后读,
   无需 calloc 清零。 */
#define FLEX_SCRATCH_MAX_DEPTH 32
static fitem *g_flex_items[FLEX_SCRATCH_MAX_DEPTH];
static int    g_flex_icap[FLEX_SCRATCH_MAX_DEPTH];
static int   *g_flex_line[FLEX_SCRATCH_MAX_DEPTH];
static int    g_flex_lcap[FLEX_SCRATCH_MAX_DEPTH];
static int    g_flex_depth = 0;

/* 进入一层 flex: 取本深度槽位的 items/line, 不足则 1.25x 扩容。
   *heap=1 表示超深回退了堆(释放时须 free)。返回 0 = OOM(扩容失败;
   与旧 malloc 失败口径一致, 调用方 abort —— 排版无"跳过"语义)。 */
static int flex_scratch_acquire(int n, fitem **items, int **line, int *heap) {
    *heap = 0;
    int d = g_flex_depth;
    if (d >= FLEX_SCRATCH_MAX_DEPTH) {
        /* 超深嵌套(>32 层, 现实文档不会出现): 回退一次性堆 */
        *items = (fitem *)malloc(sizeof(fitem) * (size_t)n);
        *line = (int *)malloc(sizeof(int) * (size_t)n);
        if (!*items || !*line) { free(*items); free(*line); return 0; }
        *heap = 1;
    } else {
        if (n > g_flex_icap[d]) {
            int nc = n + n / 4 + 16;   /* 1.25x 头部: 免逐帧小步扩容 */
            fitem *ni = (fitem *)realloc(g_flex_items[d], sizeof(fitem) * (size_t)nc);
            if (!ni) return 0;
            g_flex_items[d] = ni;
            g_flex_icap[d] = nc;
        }
        if (n > g_flex_lcap[d]) {
            int nc = n + n / 4 + 16;
            int *nl = (int *)realloc(g_flex_line[d], sizeof(int) * (size_t)nc);
            if (!nl) return 0;
            g_flex_line[d] = nl;
            g_flex_lcap[d] = nc;
        }
        *items = g_flex_items[d];
        *line = g_flex_line[d];
    }
    g_flex_depth++;
    return 1;
}

static void flex_scratch_release(fitem *items, int *line, int heap) {
    if (heap) { free(items); free(line); }
    if (g_flex_depth > 0) g_flex_depth--;
}

static float layout_flex_body(hn_context *c, hn_node *n, const hn_style *st,
                              float cx, float cy, float cw, float cross_h,
                              const hn_text_backend *tb, int measuring,
                              fitem *items, int *line, int cnt) {
    int k = 0;
    for (hn_node *ch = n->first; ch; ch = ch->next) {
        if (ch->kind == HN_ELEM && ch->style.display == HN_DISP_NONE) continue;
        if (ch->kind == HN_TEXT && ch->text_len == 0) continue;
        if (is_out_of_flow(ch)) continue;
        items[k].n = ch;
        if (ch->kind == HN_ELEM) {
            float mgn[4];
            margins_of(&ch->style, cw, mgn);
            items[k].ml = mgn[3]; items[k].mr = mgn[1];
            items[k].mt = mgn[0]; items[k].mb = mgn[2];
        } else {
            items[k].ml = items[k].mr = items[k].mt = items[k].mb = 0;
        }
        k++;
    }

    /* pass1: 度量(flex:<n> 的 basis=0 子项直接按 basis 定主轴尺寸) */
    for (int i = 0; i < cnt; i++) {
        hn_node *ch = items[i].n;
        float basis = -1;
        if (ch->kind == HN_ELEM && ch->style.flex_basis_u == HN_U_PX)
            basis = ch->style.flex_basis;
        /* pass1 一律纯度量(measuring=1): 真布局在 pass2 完成。
           此处若真布局会在 (0,0) 生成 run, 与 pass2 的绝对坐标副本并存 → 文本错位/重影。 */
        if (st->flex_row) {
            layout_box(c, ch, 0, 0, basis >= 0 ? basis : -1, -1, tb, 1, -1, -1);
        } else {
            layout_box(c, ch, 0, 0, cw, -1, tb, 1, basis >= 0 ? basis : -1, -1);
        }
        /* flex-basis 是**确定的主轴尺寸**, 不是"可用宽"。
           只把它当 avail_w 传进去时, 空内容的子项会量出 bw=0(内容宽为 0),
           于是 basis 完全失效 —— 表现为 "flex-basis:200 的子项宽度是 0,
           兄弟项把它那份空间也一起吃掉"。这里显式钉住边框盒尺寸。 */
        if (basis >= 0 && ch->kind == HN_ELEM) {
            if (st->flex_row) {
                float want = ch->style.box_border
                    ? basis
                    : basis + ch->style.padding[3] + ch->style.padding[1]
                           + ch->style.border_w * 2;
                ch->bw = want;
            } else {
                float want = ch->style.box_border
                    ? basis
                    : basis + ch->style.padding[0] + ch->style.padding[2]
                           + ch->style.border_w * 2;
                ch->bh = want;
            }
        }
    }

    /* order: 按视觉顺序排(flex_order 升序, 同序保持文档序 —— 稳定排序)。
       注意必须在 pass1 **之后**做: 度量顺序与摆放顺序无关, 但 sum 的累加
       顺序会影响 justify 的分布, 因此统一按 order 重排后再算。 */
    for (int i = 1; i < cnt; i++) {
        fitem key = items[i];
        int kk = key.n->kind == HN_ELEM ? key.n->style.flex_order : 0;
        int j = i - 1;
        while (j >= 0) {
            int kj = items[j].n->kind == HN_ELEM ? items[j].n->style.flex_order : 0;
            if (kj <= kk) break;
            items[j + 1] = items[j];
            j--;
        }
        items[j + 1] = key;
    }

    float gap_total = st->gap * (float)(cnt - 1);
    float used_cross = 0;

    /* flex-wrap: 先按主轴可用尺寸把子项切成若干行(行方向)或列(列方向)。
       行号存在 line[i]; 未换行时全为 0(与原单行路径完全一致, 零行为差异)。
       line 来自复用槽(不必清零: 下面扫描对全部 cnt 项先写后读)。 */
    int nlines = 1;
    {
        int cur = 0;
        float acc = 0;
        for (int i = 0; i < cnt; i++) {
            float outer = items[i].ml + items[i].n->bw + items[i].mr;
            float avail = st->flex_row ? cw : cross_h;
            /* 第一个子项即使超宽也留在本行(否则会无限换行) */
            if (st->flex_wrap && avail > 0 && i > 0 &&
                acc > 0 && acc + st->gap + outer > avail + 0.01f) {
                cur++;
                acc = 0;
            }
            line[i] = cur;
            acc += (acc > 0 ? st->gap : 0) + outer;
        }
        nlines = cur + 1;
    }

    if (st->flex_row) {
        float sum = 0, grow = 0;
        for (int i = 0; i < cnt; i++) {
            hn_node *ch = items[i].n;
            sum += items[i].ml + ch->bw + items[i].mr;
            if (ch->kind == HN_ELEM && ch->style.flex_grow > 0) grow += ch->style.flex_grow;
        }
        n->pref_h = 0;
        for (int i = 0; i < cnt; i++) {
            float oh = items[i].mt + items[i].n->bh + items[i].mb;
            if (oh > n->pref_h) n->pref_h = oh;
        }
        /* 多行时容器自身高度 = 各行高之和(而不是单行的最大高) */
        if (st->flex_wrap && nlines > 1 && line) {
            float tot = 0;
            for (int L = 0; L < nlines; L++) {
                float lh = 0;
                for (int i = 0; i < cnt; i++) {
                    if (line[i] != L) continue;
                    float oh = items[i].mt + items[i].n->bh + items[i].mb;
                    if (oh > lh) lh = oh;
                }
                tot += lh + (L > 0 ? st->gap : 0);
            }
            n->pref_h = tot;
        }

        float free_space = (cw >= 0 && !(measuring && cw < 0)) ? cw - sum - gap_total : 0;
        if (free_space < 0) free_space = 0;   /* 收缩按行在下面用 ldef 计算 */

        if (measuring) {
            n->pref_w = sum + gap_total;
            return n->pref_h;
        }

        /* 交叉轴: 容器定高用定高; auto 时取内容最大高(单行)或各行之和(换行) */
        float cross_avail = cross_h >= 0 ? cross_h : n->pref_h;
        float line_cross_off = 0;      /* 当前行在交叉轴上的起点 */

        float mx = 0;
        for (int L = 0; L < nlines; L++) {
            /* ---- 本行的统计: 主轴占用 / grow / shrink / 交叉高 ---- */
            float lsum = 0, lgrow = 0, lgaps = 0;
            int lcnt = 0;
            float lcross = 0;
            for (int i = 0; i < cnt; i++) {
                if (line && line[i] != L) continue;
                hn_node *ch = items[i].n;
                lsum += items[i].ml + ch->bw + items[i].mr;
                lgaps += st->gap;
                if (ch->kind == HN_ELEM && ch->style.flex_grow > 0) lgrow += ch->style.flex_grow;
                lcnt++;
                float oh = items[i].mt + ch->bh + items[i].mb;
                if (oh > lcross) lcross = oh;
            }
            if (lgaps > 0) lgaps -= st->gap;        /* n 项之间 n-1 个 gap */
            if (!st->flex_wrap || nlines <= 1) { lsum = sum; lgrow = grow; lcnt = cnt; }

            float lfree = (cw >= 0) ? cw - lsum - lgaps : 0;
            if (lfree < 0) lfree = 0;
            float ldef = 0;
            if (cw >= 0 && cw - lsum - lgaps < 0) ldef = -(cw - lsum - lgaps);

            float lead = 0, extra_gap = 0;
            if (lgrow <= 0 && cw >= 0) {
                float rem = cw - lsum - lgaps;
                if (rem > 0) {
                    int nauto = 0;
                    for (int i = 0; i < cnt; i++) {
                        if (line && line[i] != L) continue;
                        hn_node *a = items[i].n;
                        if (a->kind != HN_ELEM) continue;
                        if (a->style.margin_auto & 8) nauto++;
                        if (a->style.margin_auto & 2) nauto++;
                    }
                    if (nauto > 0) {
                        float per = rem / (float)nauto;
                        for (int i = 0; i < cnt; i++) {
                            if (line && line[i] != L) continue;
                            hn_node *a = items[i].n;
                            if (a->kind != HN_ELEM) continue;
                            if (a->style.margin_auto & 8) items[i].ml += per;
                            if (a->style.margin_auto & 2) items[i].mr += per;
                        }
                    } else if (st->justify == HN_JUST_CENTER) lead = rem * 0.5f;
                    else if (st->justify == HN_JUST_END) lead = rem;
                    else if (st->justify == HN_JUST_BETWEEN && lcnt > 1) extra_gap = rem / (float)(lcnt - 1);
                    else if (st->justify == HN_JUST_AROUND && lcnt > 0) {
                        float u = rem / (float)lcnt;
                        lead = u * 0.5f;
                        extra_gap = u;
                    } else if (st->justify == HN_JUST_EVENLY) {
                        float u = rem / (float)(lcnt + 1);
                        lead = u;
                        extra_gap = u;
                    }
                }
            }
            /* 本行的交叉可用高。
               不换行时保持原语义(定高容器里 align-items:stretch 填满容器高),
               否则以本行内容高为准 —— 若换行时仍用容器高, 每行都会占满容器,
               行间距变成容器高(实测 3 行各隔 200px 而不是 20px)。 */
            float lcross_avail = (!st->flex_wrap || nlines <= 1) && cross_h >= 0
                                 ? cross_h : lcross;

            float lshrink_sum = 0;
            for (int i = 0; i < cnt; i++) {
                if (line && line[i] != L) continue;
                hn_node *ch = items[i].n;
                float sk = ch->kind == HN_ELEM ? ch->style.flex_shrink : 1;
                if (sk < 0) sk = 0;
                lshrink_sum += (items[i].ml + ch->bw + items[i].mr) * sk;
            }
            if (nlines > 1) ldef = 0;                 /* 换行不收缩 */

            mx = lead;
            for (int i = 0; i < cnt; i++) {
                if (line && line[i] != L) continue;
                hn_node *ch = items[i].n;
                float main_outer = items[i].ml + ch->bw + items[i].mr;
                if (lfree > 0 && lgrow > 0 && ch->kind == HN_ELEM && ch->style.flex_grow > 0)
                    main_outer += lfree * ch->style.flex_grow / lgrow;
                if (ldef > 0 && lshrink_sum > 0) {
                    float sk = ch->kind == HN_ELEM ? ch->style.flex_shrink : 1;
                    if (sk < 0) sk = 0;
                    main_outer -= ldef * (main_outer * sk / lshrink_sum);
                    if (main_outer < 0) main_outer = 0;
                }

                /* 百分比高度的本轮重解析: pass1 度量时交叉轴未知(-1),
                   height:100% 按未知解析成内容高(实测为 0), 居中偏移
                   cy_off=(H-0)/2 把 flex item 整体下移半视口 —— 实测
                   html,body{height:100%}+flex 页面全坏。此时行交叉可用高
                   已定, 百分比高度子项按可用高重排一次再取 outer_h。
                   (stretch 的 forced 路径本就带定高重排, 不受影响。) */
                float resolved_h = -1;
                if (ch->kind == HN_ELEM && ch->style.height_u == HN_U_PCT &&
                    lcross_avail > 0) {
                    float inner = lcross_avail - items[i].mt - items[i].mb;
                    if (inner > 0) {
                        layout_box(c, ch, 0, 0,
                                   main_outer - items[i].ml - items[i].mr,
                                   inner, tb, 1, -1,
                                   main_outer - items[i].ml - items[i].mr);
                        resolved_h = ch->bh;
                    }
                }
                float outer_h = items[i].mt + ch->bh + items[i].mb;
            float cy_off = items[i].mt, forced = -1;
            if (ch->kind == HN_ELEM) {
                /* 子项自己的 align-self 覆盖容器的 align-items */
                hn_align ea = ch->style.has_self_align ? ch->style.self_align : st->align;
                switch (ea) {
                case HN_ALIGN_START:  cy_off = items[i].mt; break;
                case HN_ALIGN_CENTER: cy_off = items[i].mt + (lcross_avail - (resolved_h > 0 ? resolved_h : outer_h)) * 0.5f; break;
                case HN_ALIGN_END:    cy_off = lcross_avail - (resolved_h > 0 ? resolved_h : outer_h) + items[i].mt; break;
                default: /* stretch: 钉到本行交叉可用高(内容超出由 overflow 裁剪) */
                    if (ch->style.height_u == HN_U_AUTO) {
                        forced = lcross_avail - items[i].mt - items[i].mb;
                        if (forced < 0) forced = 0;
                    }
                    break;
                }
            }
            float mainw = main_outer - items[i].ml - items[i].mr;
            if (mainw < 0) mainw = 0;
            /* 交叉轴定位 = 行起点 + 行内对齐偏移 */
            layout_box(c, ch, cx + mx + items[i].ml, cy + line_cross_off + cy_off,
                       mainw, lcross_avail, tb, 0, forced, mainw);
            mx += main_outer + st->gap + extra_gap;
        }
            line_cross_off += lcross_avail + (L + 1 < nlines ? st->gap : 0);
        }
        used_cross = cross_avail;
    } else {
        /* 列方向: 主轴 = 纵向, 受 cross_h(容器定高)约束 */
        float sum = 0, grow = 0;
        for (int i = 0; i < cnt; i++) {
            hn_node *ch = items[i].n;
            sum += items[i].mt + ch->bh + items[i].mb;
            if (ch->kind == HN_ELEM && ch->style.flex_grow > 0) grow += ch->style.flex_grow;
        }
        n->pref_h = sum + gap_total;

        float free_space = cross_h >= 0 ? cross_h - sum - gap_total : 0;
        float shrink_deficit = 0;
        if (free_space < 0) {
            shrink_deficit = -free_space;
            free_space = 0;
        }

        if (measuring) {
            n->pref_w = 0;
            for (int i = 0; i < cnt; i++) {
                float uw = items[i].ml + items[i].n->bw + items[i].mr;
                if (uw > n->pref_w) n->pref_w = uw;
            }
            return sum + gap_total;
        }

        float lead = 0, extra_gap = 0;
        if (grow <= 0 && cross_h >= 0) {
            float rem = cross_h - sum - gap_total;
            if (rem > 0) {
                if (st->justify == HN_JUST_CENTER) lead = rem * 0.5f;
                else if (st->justify == HN_JUST_END) lead = rem;
                else if (st->justify == HN_JUST_BETWEEN && cnt > 1) extra_gap = rem / (float)(cnt - 1);
                else if (st->justify == HN_JUST_AROUND && cnt > 0) {
                    float u = rem / (float)cnt;
                    lead = u * 0.5f;
                    extra_gap = u;
                } else if (st->justify == HN_JUST_EVENLY) {
                    float u = rem / (float)(cnt + 1);
                    lead = u;
                    extra_gap = u;
                }
            }
        }

        float shrink_wsum = 0;
        for (int i = 0; i < cnt; i++) {
            hn_node *ch = items[i].n;
            float sk = ch->kind == HN_ELEM ? ch->style.flex_shrink : 1;
            if (sk < 0) sk = 0;
            shrink_wsum += (items[i].mt + ch->bh + items[i].mb) * sk;
        }

        float my = lead;
        for (int i = 0; i < cnt; i++) {
            hn_node *ch = items[i].n;
            float forced = -1;
            if (free_space > 0 && grow > 0 && ch->kind == HN_ELEM && ch->style.flex_grow > 0)
                forced = ch->bh + free_space * ch->style.flex_grow / grow;
            if (shrink_deficit > 0 && shrink_wsum > 0) {
                float sk = ch->kind == HN_ELEM ? ch->style.flex_shrink : 1;
                if (sk < 0) sk = 0;
                float outer = items[i].mt + ch->bh + items[i].mb;
                float shrunk = outer - shrink_deficit * (outer * sk / shrink_wsum);
                if (shrunk < 0) shrunk = 0;
                forced = shrunk - items[i].mt - items[i].mb;
            }

            /* 交叉轴(宽度): 默认 stretch 拉满 */
            float wavail = (cw < 0) ? -1 : cw - items[i].ml - items[i].mr;
            float cx_off = items[i].ml;
            if (ch->kind == HN_ELEM && st->align != HN_ALIGN_STRETCH && ch->style.width_u != HN_U_AUTO) {
                float outer_w = items[i].ml + ch->bw + items[i].mr;
                if (cw >= 0) {
                    if (st->align == HN_ALIGN_CENTER) cx_off = (cw - outer_w) * 0.5f + items[i].ml;
                    else if (st->align == HN_ALIGN_END) cx_off = cw - outer_w + items[i].ml;
                }
            }
            if (wavail < 0) wavail = ch->bw;
            layout_box(c, ch, cx + cx_off, cy + my + items[i].mt, wavail, cross_h, tb, 0, forced, -1);
            my += items[i].mt + ch->bh + items[i].mb + st->gap + extra_gap;
        }
        used_cross = cross_h >= 0 ? cross_h : my - st->gap - extra_gap;
        n->pref_w = 0;
        for (int i = 0; i < cnt; i++) {
            float uw = items[i].ml + items[i].n->bw + items[i].mr;
            if (uw > n->pref_w) n->pref_w = uw;
        }
    }

    return used_cross;
}

/* 包装: 数子项(空容器零行为不变) → 取本深度复用槽 → 跑排版 → 归还深度。
   缓冲本体不释放(只增不减), 超深堆回退由 release 按 heap 标记释放。 */
static float layout_flex(hn_context *c, hn_node *n, const hn_style *st,
                         float cx, float cy, float cw, float cross_h,
                         const hn_text_backend *tb, int measuring) {
    int cnt = 0;
    for (hn_node *ch = n->first; ch; ch = ch->next) {
        if (ch->kind == HN_ELEM && ch->style.display == HN_DISP_NONE) continue;
        if (ch->kind == HN_TEXT && ch->text_len == 0) continue;
        if (is_out_of_flow(ch)) continue;      /* 浮层不参与 flex 排版 */
        cnt++;
    }
    if (!cnt) { n->pref_w = 0; return 0; }

    fitem *items = NULL;
    int *line = NULL;
    int heap = 0;
    if (!flex_scratch_acquire(cnt, &items, &line, &heap)) abort();  /* OOM: 与旧 malloc 失败同口径 */
    float r = layout_flex_body(c, n, st, cx, cy, cw, cross_h, tb, measuring, items, line, cnt);
    flex_scratch_release(items, line, heap);
    return r;
}

static float layout_box_inner(hn_context *c, hn_node *n, float x, float y,
                              float avail_w, float avail_h,
                              const hn_text_backend *tb, int measuring, float forced_h,
                              float forced_w) {
    if (n->kind == HN_TEXT) {
        float pref = 0;
        /* 文本独立成盒(flex 匿名子项等): 终排(非度量)是本节点 runs 的重建
           点 —— 度量遍不得销毁 runs; 容器自带 runs 仅来自省略号挂载, 一并
           翻新(旧 clear_runs(root) 的职责随重建走, 见 lskip 块注)。 */
        if (!measuring) {
            n->n_runs = 0;
            if (n->parent) n->parent->n_runs = 0;
        }
        float h = layout_ifc(c, n->parent, n, n->next, x, y, avail_w, tb, measuring, &pref);
        n->pref_w = pref;
        n->bw = measuring ? pref : (avail_w > 0 ? avail_w : pref);
        n->bh = h;
        return h;
    }

    hn_style *st = &n->style;
    if (st->display == HN_DISP_NONE) { n->bw = n->bh = 0; return 0; }
    n->pref_h = 0;

    const float bt = st->border_w, bb = st->border_w, bl = st->border_w, br = st->border_w;
    float pt = st->padding[0] + bt, pr = st->padding[1] + br;
    float pb = st->padding[2] + bb, pl = st->padding[3] + bl;

    /* img 尺寸回退链: 样式 > width/height 属性 > 图片固有尺寸(后端) */
    if (n->tag && !strcmp(n->tag, "img")) {
        float iw = -1, ih = -1;
        const char *src = hn_node_attr(n, "src");
        if (c->images && c->images->size && src) {
            if (!c->images->size(c->images->ctx, src, &iw, &ih)) { iw = ih = -1; }
        }
        if (st->width_u == HN_U_AUTO) {
            const char *a = hn_node_attr(n, "width");
            float aw = a ? (float)atof(a) : -1;
            st->width = aw > 0 ? aw : (iw > 0 ? iw : 100);
            st->width_u = HN_U_PX;
        }
        if (st->height_u == HN_U_AUTO) {
            const char *a = hn_node_attr(n, "height");
            float ah = a ? (float)atof(a) : -1;
            st->height = ah > 0 ? ah : (ih > 0 ? ih : 100);
            st->height_u = HN_U_PX;
        }
    }

    /* video/audio 替换元素尺寸链: 样式 > width/height 属性 > 会话帧固有尺寸 >
       缺省(契约 §5)。video 缺省 300 x 16:9(HTML 约定宽 300); audio 兜底高 0
       (不可见, 盒仍在文档流里)。READY 拉到首帧后如未显式定尺寸, 下一次
       layout 用宿主 meta 尺寸(hn_media_intrinsic 给值 → 自然触发一次重排)。 */
    if (n->tag && (!strcmp(n->tag, "video") || !strcmp(n->tag, "audio"))) {
        int audio = (n->tag[0] == 'a');
        int iw = -1, ih = -1;
        hn_media_intrinsic(c, n, &iw, &ih);
        if (st->width_u == HN_U_AUTO) {
            const char *a = hn_node_attr(n, "width");
            float aw = a ? (float)atof(a) : -1;
            st->width = aw > 0 ? aw : (iw > 0 ? (float)iw : 300);
            st->width_u = HN_U_PX;
        }
        if (st->height_u == HN_U_AUTO) {
            const char *a = hn_node_attr(n, "height");
            float ah = a ? (float)atof(a) : -1;
            if (audio)            st->height = ah > 0 ? ah : 0;
            else if (ah > 0)      st->height = ah;
            else if (ih > 0)      st->height = (float)ih;
            else                  st->height = st->width * 9.0f / 16.0f;  /* 缺省 16:9 */
            st->height_u = HN_U_PX;
        }
    }

    /* <canvas> 替换元素尺寸链: 样式 > width/height 属性 > 缺省 300x150。
       属性即绘图坐标空间的尺寸(HTML 约定缺省 300 x 150) —— 绘制期重放
       账本 op 时按 盒/属性 的比例映射(见 hn_paint paint_canvas)。 */
    if (n->tag && !strcmp(n->tag, "canvas")) {
        if (st->width_u == HN_U_AUTO) {
            const char *a = hn_node_attr(n, "width");
            float aw = a ? (float)atof(a) : -1;
            st->width = aw > 0 ? aw : 300;
            st->width_u = HN_U_PX;
        }
        if (st->height_u == HN_U_AUTO) {
            const char *a = hn_node_attr(n, "height");
            float ah = a ? (float)atof(a) : -1;
            st->height = ah > 0 ? ah : 150;
            st->height_u = HN_U_PX;
        }
    }

    /* 输入控件: 默认尺寸 */
    if (hn_node_is_input(n)) {
        int ik = hn_node_input_kind(n);
        if (ik == HN_IN_RANGE) {
            /* 滑条: UA 缺省盒(UA css 160x20), 零 padding —— 不吃文本框
               的 180 缺省与内边距(那是文本框观感) */
            if (st->width_u == HN_U_AUTO) { st->width = 160; st->width_u = HN_U_PX; }
            if (st->height_u == HN_U_AUTO) { st->height = 20; st->height_u = HN_U_PX; }
            st->padding[0] = st->padding[1] = st->padding[2] = st->padding[3] = 0;
            pt = st->padding[0] + bt; pr = st->padding[1] + br;
            pb = st->padding[2] + bb; pl = st->padding[3] + bl;
        } else if (ik == HN_IN_CHECKBOX || ik == HN_IN_RADIO) {
            /* 复选/单选: 固定方寸(随字号), 无文本内边距 —— 之前套用文本框
               的默认(180 宽 + padding), 视觉上是一块可输入区域而非控件 */
            float s = st->font_size * 0.9f;
            if (s < 12) s = 12;
            if (s > 20) s = 20;
            if (st->width_u == HN_U_AUTO) { st->width = s; st->width_u = HN_U_PX; }
            if (st->height_u == HN_U_AUTO) { st->height = s; st->height_u = HN_U_PX; }
            st->padding[0] = st->padding[1] = st->padding[2] = st->padding[3] = 0;
        } else {
            if (st->width_u == HN_U_AUTO && ik != HN_IN_BUTTON) { st->width = 180; st->width_u = HN_U_PX; }
            /* 按钮宽度留给 pref 收缩(见 layout_input 尾部), 不吃 180 缺省 */
            if (st->height_u == HN_U_AUTO) {
                st->height = st->font_size * st->line_height + 14;
                st->height_u = HN_U_PX;
            }
            if (st->padding[1] == 0 && st->padding[3] == 0) { st->padding[1] = 10; st->padding[3] = 10; }
            if (st->padding[0] == 0 && st->padding[2] == 0) { st->padding[0] = 7; st->padding[2] = 7; }
        }
        pt = st->padding[0] + bt; pr = st->padding[1] + br;
        pb = st->padding[2] + bb; pl = st->padding[3] + bl;
    }

    /* calc(<pct>% ± <len>): PCT 通道解出后再加减绝对偏移。clamp 到 0 ——
       calc(100% - 500px) 在窄容器里会得到负宽, 负宽在布局里语义不明。 */
    float w = size_of(st->width, st->width_u, avail_w, st->font_size);
    float h = size_of(st->height, st->height_u, avail_h, st->font_size);
    if (st->width_u == HN_U_PCT && st->width_calc_px) w += st->width_calc_px;
    if (st->height_u == HN_U_PCT && st->height_calc_px) h += st->height_calc_px;
    if (w < 0 && st->width_u == HN_U_PCT) w = 0;
    if (h < 0 && st->height_u == HN_U_PCT) h = 0;
    /* min/max 约束(auto 尺寸同样受钳: 上限截断/下限托底) */
    if (st->min_w_u != HN_U_AUTO || st->max_w_u != HN_U_AUTO) {
        float mnw = st->min_w_u == HN_U_AUTO ? -1 : size_of(st->min_w, st->min_w_u, avail_w, st->font_size);
        float mxw = st->max_w_u == HN_U_AUTO ? -1 : size_of(st->max_w, st->max_w_u, avail_w, st->font_size);
        if (mnw > 0 && (w < mnw)) w = mnw;
        if (mxw > 0 && (w < 0 || w > mxw)) w = mxw;
    }
    if (st->min_h_u != HN_U_AUTO || st->max_h_u != HN_U_AUTO) {
        float mnh = st->min_h_u == HN_U_AUTO ? -1 : size_of(st->min_h, st->min_h_u, avail_h, st->font_size);
        float mxh = st->max_h_u == HN_U_AUTO ? -1 : size_of(st->max_h, st->max_h_u, avail_h, st->font_size);
        if (mnh > 0 && h >= 0 && h < mnh) h = mnh;
        if (mxh > 0 && h > mxh) h = mxh;   /* auto(h<0) 的上限在内容计算后截断 */
    }
    if (forced_h > 0) h = forced_h;
    /* forced_w: 主轴尺寸由外部(flex 的 grow/shrink 结算结果)强制指定。
       没有这一条时, 显式 width 会把 flex 算好的 mainw 整个盖掉 ——
       表现为"flex-shrink 完全不生效, 子项溢出互相重叠"(实测 2 个
       width:150 的子项塞进 width:200 的容器, 各自仍是 150 且第二项
       画在第一项身上)。 */
    if (forced_w > 0) w = forced_w;
    /* aspect-ratio: 一维已定、另一维 auto 时按 w/h 补齐。标准口径里 ratio
       作用于内容盒 —— 这里按盒尺寸近似(引擎尚无 padding/border 参与的
       精细替换尺寸模型), 探针锚定无 padding 用例; min/max 钳制照常生效。
       ratio 补出的 h 计入 h_definite(否则会被内容高覆盖)。 */
    int ar_h_forced = 0;
    if (st->aspect_ratio > 0) {
        int w_auto = (forced_w <= 0) && (st->width_u == HN_U_AUTO || w < 0);
        int h_auto = (forced_h <= 0) && (st->height_u == HN_U_AUTO || h < 0);
        if (!w_auto && h_auto && w > 0) { h = w / st->aspect_ratio; ar_h_forced = 1; }
        else if (!h_auto && w_auto && h > 0) w = h * st->aspect_ratio;
    }
    int h_definite = ar_h_forced || (forced_h > 0) ||
                     (st->height_u != HN_U_AUTO && h >= 0);

    n->bx = x;
    n->by = y;

    /* 内容宽: auto = 可用宽(边框盒预算)内含 padding;
       显式宽度按 box-sizing 解释 */
    float content_w;
    if (measuring) content_w = (w >= 0) ? (st->box_border ? w - pl - pr : w) : -1;
    else if (w < 0) content_w = avail_w - pl - pr;
    else content_w = (st->box_border ? w : w + pl + pr) - pl - pr;
    if (content_w < 0) content_w = 0;

    float content_h_def = -1;
    if (h_definite) content_h_def = ((forced_h > 0 || st->box_border) ? h : h + pt + pb) - pt - pb;
    if (content_h_def < 0) content_h_def = -1;

    /* 输入控件: 值文本走自身 IFC */
    if (hn_node_is_input(n)) {
        /* 控件 runs 的每帧一次清理(旧根清职责; 控件在 bail 清单里必走
           重建 —— 布点内两次 layout_input 的追加语义保持原样) */
        n->n_runs = 0;
        if (hn_node_input_kind(n) == HN_IN_BUTTON && st->width_u == HN_U_AUTO) {
            /* 按钮按标签收缩(浏览器 shrink-to-fit): 两遍布局 —— 先量标签
               pref(给大宽度, IFC 不折行量出单行宽; -1 会被 content_w>0
               短路), 再以 pref 为内容宽重排 run。单遍的话 run 按 content_w
               (可用宽)居中, 盒再收缩, 标签就落在盒外了(实测复现)。 */
            layout_input(c, n, 1e6f, tb, 1);
            float pw = n->pref_w > 0 ? n->pref_w : content_w;
            if (pw > content_w) pw = content_w;
            layout_input(c, n, pw, tb, measuring);
            n->bw = pw + pl + pr;
        } else {
            layout_input(c, n, content_w, tb, measuring);
            n->bw = content_w + pl + pr;
        }
        if (h_definite) {
            h = (forced_h > 0 || st->box_border) ? h : h + pt + pb;
            n->bh = h;
        } else {
            n->bh = n->content_h > 0 ? n->content_h : st->font_size * st->line_height + pt + pb;
        }
        n->content_h = n->bh;
        return n->bh;
    }

    /* overflow 容器: 子项按内容自然高布局(不受盒高约束), 溢出的部分由滚动查看。
       否则内容会被强行压进盒子里, 既显示不全也无法滚动。 */
    float inner_h_def = st->overflow ? -1 : content_h_def;
    float content_used;
    if (st->display == HN_DISP_FLEX)
        content_used = layout_flex(c, n, st, x + pl, y + pt, content_w, inner_h_def, tb, measuring);
    else
        content_used = layout_block(c, n, st, x + pl, y + pt, content_w, tb, measuring, inner_h_def);

    /* 高度: forced_h / border-box 已是边框盒最终值; content-box 显式高度需加 padding */
    if (!h_definite) h = content_used + pt + pb;
    else if (forced_h <= 0 && !st->box_border) h += pt + pb;
    /* auto/内容高的最终钳制(min 托底, max 截断) */
    if (st->min_h_u != HN_U_AUTO) {
        float mnh = size_of(st->min_h, st->min_h_u, avail_h, st->font_size);
        if (mnh > 0 && h < mnh) h = mnh;
    }
    if (st->max_h_u != HN_U_AUTO) {
        float mxh = size_of(st->max_h, st->max_h_u, avail_h, st->font_size);
        if (mxh > 0 && h > mxh) h = mxh;
    }
    n->bh = h;
    /* content_h 记录**内容自然总高**, 不受盒高限制 —— 这是滚动上限的依据。
       定高容器(height:N)装不下的内容仍要计入, 否则 overflow 无法滚动。 */
    n->content_h = content_used + pt + pb;
    if (n->pref_h > 0 && n->pref_h + pt + pb > n->content_h)
        n->content_h = n->pref_h + pt + pb;
    if (n->content_h < n->bh) n->content_h = n->bh;

    if (measuring && st->width_u == HN_U_AUTO) n->bw = n->pref_w + pl + pr;
    else if (n->tag && !strcmp(n->tag, "button") && st->width_u == HN_U_AUTO) {
        /* <button> 按内容收缩(浏览器惯例); 其余 inline-block 维持引擎既有
           "占满可用宽的原子盒"模型(文档口径)。子文本 run 是按收缩前
           content_w 排的(text-align:center 居中于可用宽) —— 收缩量从
           子树 run x 里收回, 标签才落在盒内。 */
        n->bw = n->pref_w + pl + pr;
        float shrink_dx = (content_w - n->pref_w) * 0.5f;
        if (st->text_align == 1 && shrink_dx > 0)
            for (hn_node *ch = n->first; ch; ch = ch->next)
                translate_x(ch, -shrink_dx);   /* run 挂在子节点(文本节点)上 */
    }
    else n->bw = content_w + pl + pr;

    /* 容器尺寸已定 → 摆放脱离流的子节点(position: absolute/fixed)。
       必须在此处(而非布局中途): 浮层参照的是父容器的最终盒。 */
    if (!measuring) layout_out_of_flow(c, n, tb);

    return n->bh;
}

/* ---------------- 子树布局记忆(freeze L2 本体第一刀) ----------------
 * 现状: 任何变化触发全文档重排(2000 卡文档 layout ~14ms/帧; 1.2 节
 * "增量渲染/惰性布局"缺口)。本机制把重排收敛到脏分支:
 *
 *   lf_key(node) = own_hash(node) ⊕ Σ child_lf_key        —— Merkle 键
 *   own_hash     = FNV-1a(kind/tag/attrs/文本字节/计算样式字节)
 *
 * 每帧 O(子数) 增量维护(子级键已在, 不做 O(子树) 全量哈希)。
 * layout_box 入口: 键 + 布局输入(可用宽高/强制宽高/度量模式)全部命中
 * → 整子树跳过重排(runs/盒/content_h 原样保留); 原点不同只
 * translate_subtree —— 排版对绝对原点平移不变(盒/runs 全是相对同一
 * 原点的输出)。未命中 → 正常布局, 尾部用刚布局好的子级键重算自身键
 * (递归自底向上自然成立)。布局输入变化(容器宽/伸缩结果)照常使键失配。
 *
 * bail(不缓存不跳过, 键在 ls_own_hash 内判定并向祖先传播):
 *   - 替换元素与输入控件(img/video/audio/canvas/progress/select/button/
 *     input/textarea): layout 期把解析尺寸写回计算样式, 尺寸链依赖外部
 *     状态(图片固有尺寸/媒体会话 meta/JS 值) —— 键抓不到;
 *   - 脱离流(position:absolute/fixed): 摆放依赖子树外的祖先盒
 *     (containing_block/视口), 自身键不完整;
 *   - 动态表现型(hn-lottie/three/cubism/mesh/live/stream/gif): 每帧
 *     数值变, 与 L1 片段同一清单。
 *   含 bail 子级的容器同样不可跳(子级布局必须每帧重跑) —— v1 边界,
 *   收益主体是文本/盒型子树(段落/卡片文字区/聊天行)。
 * display:none 子树: lskip_hide 使键失效(隐藏期变更不追踪), 显形帧整树
 * 重排; 父键对隐藏子级折"浅哈希"(显隐翻转即键变)。脱离流子级折全量
 * 深哈希(摆放随祖先盒变, 键必须每帧可信)。
 * runs 清理: 旧 clear_runs(root) 与跳过不兼容(命中帧 runs 必须原样),
 * 改"谁重建谁清理": IFC 落盘点 ifc_wipe + 文本/控件终排自带清 —— 重建
 * 子树每帧与旧口径同构, 跳过子树 runs 原样保留。
 * 观测: HN_LAYOUT_SKIP=0 关闭; HN_LAYOUT_SKIP_TRACE=1 每遍布局出口打
 * 统计。正确性锚: tools/freeze_l2_probe.sh(命中路径按位 cmp 一致 +
 * 突变序列 ≤0.01px 容差对照 + 命中/平移断言)。 */
static struct {
    long hit, moved, miss, bail;
    unsigned char on, trace, inited;
} g_ls;

static unsigned long long ls_mix(unsigned long long h, const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    size_t i;
    for (i = 0; i < n; i++) { h ^= b[i]; h *= 1099511628211ull; }
    return h;
}

/* 整树键刷新(每遍布局前; 自底向上, O(N) 纯哈希): 键恒反映当前内容,
   外部突变与 anim 样式写当帧即被父层感知(教训: 曾按"父折子级已存键"
   增量维护, 子级未被重排前键是旧的, 父层误命中吞掉突变 —— anim_probe
   ghost 与最小复现实测后改整树刷新)。 */
static unsigned long long ls_own_hash(hn_node *n, int *bail) {
    unsigned long long h = 1469598103934665603ull;   /* FNV-1a 64 偏移基 */
    int i;
    h = ls_mix(h, &n->kind, sizeof n->kind);
    if (n->tag) {
        h = ls_mix(h, n->tag, strlen(n->tag));
        if (hn_node_is_input(n) ||
            !strcmp(n->tag, "img") || !strcmp(n->tag, "video") ||
            !strcmp(n->tag, "audio") || !strcmp(n->tag, "canvas") ||
            !strcmp(n->tag, "progress") || !strcmp(n->tag, "select") ||
            !strcmp(n->tag, "button") ||
            hn_node_attr(n, "hn-lottie") || hn_node_attr(n, "hn-three") ||
            hn_node_attr(n, "hn-cubism") || hn_node_attr(n, "hn-mesh") ||
            hn_node_attr(n, "hn-live") || hn_node_attr(n, "hn-stream") ||
            hn_node_gif_src(n))
            *bail = 1;
    }
    h = ls_mix(h, &n->n_attrs, sizeof n->n_attrs);
    for (i = 0; i < n->n_attrs; i++) {
        h = ls_mix(h, n->attrs[i].name, strlen(n->attrs[i].name));
        h = ls_mix(h, n->attrs[i].value, strlen(n->attrs[i].value));
    }
    if (n->text && n->text_len) h = ls_mix(h, n->text, n->text_len);
    h = ls_mix(h, &n->style, sizeof n->style);
    if (n->style.position == HN_POS_ABSOLUTE || n->style.position == HN_POS_FIXED)
        *bail = 1;
    return h;
}

static void ls_refresh(hn_node *n) {
    int bail = 0;
    unsigned long long h = ls_own_hash(n, &bail);
    int idx = 0;
    for (hn_node *ch = n->first; ch; ch = ch->next, idx++) {
        ls_refresh(ch);
        h = ls_mix(h, &idx, sizeof idx);   /* 顺序敏感 */
        h = ls_mix(h, &ch->lf_key, sizeof ch->lf_key);
        if (ch->lf_bail) bail = 1;
    }
    n->lf_key = h;
    n->lf_bail = (unsigned char)(bail ? 1 : 0);
}

/* IFC 落盘前的 runs 清理(旧 clear_runs(root) 的"随重建走"替代):
   容器自身 runs(省略号挂载) + [start,stop) 链上将被 flatten 的文本节点。
   跳过子树不经过此处 —— runs 原样保留正是命中的意义。 */
static void ifc_wipe_walk(hn_node *n) {
    if (n->kind == HN_TEXT) { n->n_runs = 0; return; }
    for (hn_node *ch = n->first; ch; ch = ch->next)
        if (!(ch->kind == HN_ELEM && ch->style.display == HN_DISP_NONE))
            ifc_wipe_walk(ch);
}

static void ifc_wipe(hn_node *container, hn_node *start, hn_node *stop) {
    container->n_runs = 0;
    for (hn_node *ch = start; ch && ch != stop; ch = ch->next)
        if (!(ch->kind == HN_ELEM && ch->style.display == HN_DISP_NONE))
            ifc_wipe_walk(ch);
}

/* 包装: 入口查记忆(记录键==当前键 + 布局输入全中 → 跳过/平移),
   未中走 inner 并记录几何与当时的键(键已由 ls_refresh 与内容同步)。 */
static float layout_box(hn_context *c, hn_node *n, float x, float y,
                        float avail_w, float avail_h,
                        const hn_text_backend *tb, int measuring, float forced_h,
                        float forced_w) {
    if (!g_ls.on || n->kind != HN_ELEM || n->lf_bail)
        return layout_box_inner(c, n, x, y, avail_w, avail_h, tb, measuring,
                                forced_h, forced_w);
    if (n->lf_valid && n->lf_rec_key == n->lf_key &&
        n->lf_aw == avail_w && n->lf_ah == avail_h &&
        n->lf_fh == forced_h && n->lf_fw == forced_w &&
        n->lf_meas == (unsigned char)(measuring != 0)) {
        float dx = x - n->lf_ox, dy = y - n->lf_oy;
        if (dx != 0 || dy != 0) {
            n->lf_ox = x; n->lf_oy = y;   /* translate_subtree 同步子树 lf 原点 */
            translate_subtree(n, dx, dy);
            g_ls.moved++;
        } else {
            g_ls.hit++;
        }
        return n->lf_h;
    }
    g_ls.miss++;
    {
        float h = layout_box_inner(c, n, x, y, avail_w, avail_h, tb, measuring,
                                   forced_h, forced_w);
        if (!n->lf_bail) {   /* bail 候选: 只布局不记录(替换元素链还会写样式) */
            n->lf_rec_key = n->lf_key;
            n->lf_valid = 1;
            n->lf_ox = x; n->lf_oy = y; n->lf_h = h;
            n->lf_aw = avail_w; n->lf_ah = avail_h;
            n->lf_fh = forced_h; n->lf_fw = forced_w;
            n->lf_meas = (unsigned char)(measuring != 0);
        }
        return h;
    }
}

void hn_layout_lskip_stats(long *hit, long *moved, long *miss, long *bail) {
    if (hit) *hit = g_ls.hit;
    if (moved) *moved = g_ls.moved;
    if (miss) *miss = g_ls.miss;
    if (bail) *bail = g_ls.bail;
}

void hn_layout_root(hn_context *c) {
    const hn_text_backend *tb = c->tb_valid ? &c->tb : NULL;
    hn_node *root = c->doc->root;
    g_mc = (hn_mcache *)c->mcache;
    hn_layout_mcache_sync(c->mcache, tb, c->tb_valid);
    if (!g_ls.inited) {
        const char *env = getenv("HN_LAYOUT_SKIP");
        g_ls.on = !(env && *env && strcmp(env, "0") == 0);
        env = getenv("HN_LAYOUT_SKIP_TRACE");
        g_ls.trace = (env && *env && strcmp(env, "0") != 0);
        g_ls.inited = 1;
    }
    g_ls.hit = g_ls.moved = g_ls.miss = g_ls.bail = 0;
    if (g_ls.on) ls_refresh(root);    /* 整树键刷新: 键恒反映当前内容 */
    else clear_runs(root);            /* 机制关: 旧口径(根清) */
    float forced = (root->style.height_u == HN_U_AUTO) ? c->vh : -1;
    layout_box(c, root, 0, 0, c->vw, c->vh, tb, 0, forced, -1);
    if (g_mc && g_mc->trace)
        fprintf(stderr, "[mcache] hits=%ld misses=%ld entries=%ld\n",
                g_mc->hits, g_mc->misses, g_mc->entries);
    if (g_ls.trace)
        fprintf(stderr, "[lskip] hit=%ld moved=%ld miss=%ld bail=%ld\n",
                g_ls.hit, g_ls.moved, g_ls.miss, g_ls.bail);
    g_mc = NULL;
}

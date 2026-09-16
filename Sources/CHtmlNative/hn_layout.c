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

static float size_of(float v, int u, float base, float font_px) {
    switch (u) {
    case HN_U_PX:  return v;
    case HN_U_PCT: return base > 0 ? v / 100.0f * base : -1.0f;
    case HN_U_EM:  return v * font_px;
    default:       return -1.0f;
    }
}

static float measure_run(const hn_text_backend *tb, const hn_style *st,
                         const char *s, size_t n) {
    if (!n) return 0;
    if (tb && tb->measure) {
        hn_font_desc f = { st->font_size, st->font_weight, st->font_italic, st->letter_spacing, st->font_family };
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
                        const hn_text_backend *tb, int measuring, float forced_h);

static void flatten_inline(hn_context *c, hn_node *n, ivec *iv, const hn_text_backend *tb,
                           float avail_w, int measuring) {
    if (n->kind == HN_TEXT) {
        const hn_style *st = &n->parent->style;
        const char *s = n->text;
        size_t len = n->text_len, i = 0;
        while (i < len) {
            if (s[i] == ' ') { if (iv->n > 0) iv->pending_space = 1; i++; continue; }
            size_t j = i;
            while (j < len && s[j] != ' ') j++;
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
        layout_box(c, n, 0, 0, avail, -1, tb, measuring, -1);
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
} line_acc;

static void la_reset(line_acc *la) {
    la->n = 0; la->w = 0; la->asc = 0; la->desc = 0; la->lh = 0;
}

static void la_grow(line_acc *la, const iitem *it, const hn_text_backend *tb) {
    float a, d;
    font_metrics(tb, it->st, &a, &d);
    float lh = it->st->font_size * it->st->line_height;
    if (lh > la->lh) la->lh = lh;
    if (a > la->asc) la->asc = a;
    if (d > la->desc) la->desc = d;
}

static float align_off(int align, float max_w, float w) {
    if (max_w <= 0) return 0;
    float off = 0;
    if (align == 1) off = (max_w - w) * 0.5f;
    else if (align == 2) off = max_w - w;
    return off > 0 ? off : 0;
}

/* 落盘当前行 */
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
            }
            continue;
        }
        run_push(it->owner, it->begin, it->end, x + off + la->x[k], it->w,
                 baseline, y_top, la->lh > 0 ? la->lh : it->st->font_size);
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
        float cur = 0, maxw = 0, maxh = 0, any = 0;
        for (int i = 0; i < iv.n; i++) {
            iitem *it = &iv.v[i];
            if (it->forced_break) {              /* <br>/换行: 首选宽取最长行 */
                if (cur > maxw) maxw = cur;
                cur = 0;
                continue;
            }
            if (!it->owner) continue;
            if (it->space_before && any) cur += measure_run(tb, it->st, " ", 1);
            cur += it->pad_before + it->w + it->pad_after;
            float a, d;
            font_metrics(tb, it->st, &a, &d);
            float lh = it->st->font_size * it->st->line_height;
            if (lh > maxh) maxh = lh;
            any = 1;
        }
        if (out_pref_w) *out_pref_w = cur > maxw ? cur : maxw;
        /* 仅行内级元素由 run 汇总占位盒; 块级/flex 子项的盒由其父级摆放 */
        for (hn_node *ch = start; ch && ch != stop; ch = ch->next)
            if (ch->kind == HN_ELEM && ch->style.display == HN_DISP_INLINE) collapse_node_box(ch);
        free(iv.v);
        return any ? maxh : 0;
    }

    line_acc la;
    la_reset(&la);
    float cur_y = 0;
    int i = 0;
    while (i < iv.n) {
        iitem *it = &iv.v[i];

        if (it->forced_break) {
            la_emit(&la, &iv, x, y + cur_y, max_w, align);
            if (la.n) cur_y += la.lh;
            la_reset(&la);
            i++;
            continue;
        }

        float space_w = (it->space_before && la.n > 0) ? measure_run(tb, it->st, " ", 1) : 0;
        float need = it->pad_before + it->w + it->pad_after;

        /* 本行放不下: 已有内容则换行后重试 */
        if (la.n > 0 && max_w > 0 && la.w + space_w + need > max_w) {
            la_emit(&la, &iv, x, y + cur_y, max_w, align);
            cur_y += la.lh;
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
            while (p < e) {
                size_t cn = utf8_next(s + p, e - p);
                float cw = measure_run(tb, it->st, s + p, cn);
                if (acc > 0 && acc + cw > max_w) {
                    emit_seg(it->owner, seg, p, x, y + cur_y, acc, a, d, lh, max_w, align);
                    cur_y += lh;
                    seg = p;
                    acc = 0;
                }
                acc += cw;
                p += cn;
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
            la_emit(&la, &iv, x, y + cur_y, max_w, align);
            cur_y += la.lh;
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
    if (h > 0) n->content_h = h + padt + n->style.padding[2];
}

/* ---------------- 元素节点 ---------------- */

static float layout_box(hn_context *c, hn_node *n, float x, float y,
                        float avail_w, float avail_h,
                        const hn_text_backend *tb, int measuring, float forced_h);

/* 整棵子树水平平移(含 runs), 用于 margin:auto 居中 */
static void translate_subtree(hn_node *n, float dx) {
    n->bx += dx;
    for (int i = 0; i < n->n_runs; i++) n->runs[i].x += dx;
    for (hn_node *ch = n->first; ch; ch = ch->next) translate_subtree(ch, dx);
}

static float layout_block(hn_context *c, hn_node *n, const hn_style *st,
                          float cx, float cy, float cw,
                          const hn_text_backend *tb, int measuring, float cross_h) {
    (void)st;
    float cur_y = 0, max_w = 0;
    hn_node *ch = n->first;
    while (ch) {
        if (ch->kind == HN_ELEM && ch->style.display == HN_DISP_NONE) { ch = ch->next; continue; }
        if (ch->kind == HN_TEXT && ch->text_len == 0) { ch = ch->next; continue; }

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
            float h = layout_ifc(c, n, ch, last->next, cx, cy + cur_y, cw, tb, measuring, &pref);
            cur_y += h;
            if (pref > max_w) max_w = pref;
            ch = last->next;
            continue;
        }

        /* 块级子节点 */
        float ml = ch->style.margin[3], mr = ch->style.margin[1];
        float mt = ch->style.margin[0], mb = ch->style.margin[2];
        float avail = (cw < 0) ? -1 : cw - ml - mr;
        if (avail < 0 && cw >= 0) avail = 0;
        layout_box(c, ch, cx + ml, cy + cur_y + mt, avail, cross_h, tb, measuring, -1);
        /* margin:0 auto — 左右 auto 吸收剩余空间(双侧=居中, 仅左=推右) */
        if (!measuring && cw >= 0 && ch->kind == HN_ELEM && (ch->style.margin_auto & 10)) {
            float rem = cw - ch->bw - ml - mr;
            if (rem > 0) {
                int na = ((ch->style.margin_auto & 8) ? 1 : 0) + ((ch->style.margin_auto & 2) ? 1 : 0);
                if (ch->style.margin_auto & 8) translate_subtree(ch, rem / (float)na);
            }
        }
        cur_y += mt + ch->bh + mb;
        float uw = ch->bw + ml + mr;
        if (uw > max_w) max_w = uw;
        ch = ch->next;
    }
    n->pref_w = max_w;
    return cur_y;
}

typedef struct { hn_node *n; float ml, mr, mt, mb; } fitem;

static float layout_flex(hn_context *c, hn_node *n, const hn_style *st,
                         float cx, float cy, float cw, float cross_h,
                         const hn_text_backend *tb, int measuring) {
    int cnt = 0;
    for (hn_node *ch = n->first; ch; ch = ch->next) {
        if (ch->kind == HN_ELEM && ch->style.display == HN_DISP_NONE) continue;
        if (ch->kind == HN_TEXT && ch->text_len == 0) continue;
        cnt++;
    }
    if (!cnt) { n->pref_w = 0; return 0; }

    fitem *items = malloc(sizeof(fitem) * (size_t)cnt);
    if (!items) abort();
    int k = 0;
    for (hn_node *ch = n->first; ch; ch = ch->next) {
        if (ch->kind == HN_ELEM && ch->style.display == HN_DISP_NONE) continue;
        if (ch->kind == HN_TEXT && ch->text_len == 0) continue;
        items[k].n = ch;
        if (ch->kind == HN_ELEM) {
            items[k].ml = ch->style.margin[3]; items[k].mr = ch->style.margin[1];
            items[k].mt = ch->style.margin[0]; items[k].mb = ch->style.margin[2];
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
            layout_box(c, ch, 0, 0, basis >= 0 ? basis : -1, -1, tb, 1, -1);
        } else {
            layout_box(c, ch, 0, 0, cw, -1, tb, 1, basis >= 0 ? basis : -1);
        }
    }

    float gap_total = st->gap * (float)(cnt - 1);
    float used_cross = 0;

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

        float free_space = (cw >= 0 && !(measuring && cw < 0)) ? cw - sum - gap_total : 0;
        float shrink_deficit = 0;
        if (free_space < 0) {
            shrink_deficit = -free_space;
            free_space = 0;
        }

        if (measuring) {
            n->pref_w = sum + gap_total;
            free(items);
            return n->pref_h;
        }

        float lead = 0, extra_gap = 0;
        if (grow <= 0 && cw >= 0) {
            float rem = cw - sum - gap_total;
            if (rem > 0) {
                /* auto 主轴外边距优先吸收剩余空间(标准: 存在 auto margin 时忽略 justify) */
                int nauto = 0;
                for (int i = 0; i < cnt; i++) {
                    hn_node *a = items[i].n;
                    if (a->kind != HN_ELEM) continue;
                    if (a->style.margin_auto & 8) nauto++;
                    if (a->style.margin_auto & 2) nauto++;
                }
                if (nauto > 0) {
                    float per = rem / (float)nauto;
                    for (int i = 0; i < cnt; i++) {
                        hn_node *a = items[i].n;
                        if (a->kind != HN_ELEM) continue;
                        if (a->style.margin_auto & 8) items[i].ml += per;
                        if (a->style.margin_auto & 2) items[i].mr += per;
                    }
                } else if (st->justify == HN_JUST_CENTER) lead = rem * 0.5f;
                else if (st->justify == HN_JUST_END) lead = rem;
                else if (st->justify == HN_JUST_BETWEEN && cnt > 1) extra_gap = rem / (float)(cnt - 1);
            }
        }

        /* 交叉轴可用高: 容器定高用定高; auto 才取内容最大高 */
        float cross_avail = cross_h >= 0 ? cross_h : 0;
        if (cross_h < 0) {
            for (int i = 0; i < cnt; i++)
                if (items[i].mt + items[i].n->bh + items[i].mb > cross_avail)
                    cross_avail = items[i].mt + items[i].n->bh + items[i].mb;
        }

        float shrink_wsum = 0;
        for (int i = 0; i < cnt; i++) {
            hn_node *ch = items[i].n;
            float sk = ch->kind == HN_ELEM ? ch->style.flex_shrink : 1;
            if (sk < 0) sk = 0;
            shrink_wsum += (items[i].ml + ch->bw + items[i].mr) * sk;
        }

        float mx = lead;
        for (int i = 0; i < cnt; i++) {
            hn_node *ch = items[i].n;
            float main_outer = items[i].ml + ch->bw + items[i].mr;
            if (free_space > 0 && grow > 0 && ch->kind == HN_ELEM && ch->style.flex_grow > 0)
                main_outer += free_space * ch->style.flex_grow / grow;
            if (shrink_deficit > 0 && shrink_wsum > 0) {
                float sk = ch->kind == HN_ELEM ? ch->style.flex_shrink : 1;
                if (sk < 0) sk = 0;
                main_outer -= shrink_deficit * (main_outer * sk / shrink_wsum);
                if (main_outer < 0) main_outer = 0;
            }

            float outer_h = items[i].mt + ch->bh + items[i].mb;
            float cy_off = items[i].mt, forced = -1;
            if (ch->kind == HN_ELEM) {
                switch (st->align) {
                case HN_ALIGN_START:  cy_off = items[i].mt; break;
                case HN_ALIGN_CENTER: cy_off = items[i].mt + (cross_avail - outer_h) * 0.5f; break;
                case HN_ALIGN_END:    cy_off = cross_avail - outer_h + items[i].mt; break;
                default: /* stretch: 钉到交叉轴可用高(内容超出由 overflow 裁剪) */
                    if (ch->style.height_u == HN_U_AUTO) {
                        forced = cross_avail - items[i].mt - items[i].mb;
                        if (forced < 0) forced = 0;
                    }
                    break;
                }
            }
            float mainw = main_outer - items[i].ml - items[i].mr;
            if (mainw < 0) mainw = 0;
            layout_box(c, ch, cx + mx + items[i].ml, cy + cy_off, mainw, cross_avail, tb, 0, forced);
            mx += main_outer + st->gap + extra_gap;
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
            free(items);
            return sum + gap_total;
        }

        float lead = 0, extra_gap = 0;
        if (grow <= 0 && cross_h >= 0) {
            float rem = cross_h - sum - gap_total;
            if (rem > 0) {
                if (st->justify == HN_JUST_CENTER) lead = rem * 0.5f;
                else if (st->justify == HN_JUST_END) lead = rem;
                else if (st->justify == HN_JUST_BETWEEN && cnt > 1) extra_gap = rem / (float)(cnt - 1);
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
            layout_box(c, ch, cx + cx_off, cy + my + items[i].mt, wavail, cross_h, tb, 0, forced);
            my += items[i].mt + ch->bh + items[i].mb + st->gap + extra_gap;
        }
        used_cross = cross_h >= 0 ? cross_h : my - st->gap - extra_gap;
        n->pref_w = 0;
        for (int i = 0; i < cnt; i++) {
            float uw = items[i].ml + items[i].n->bw + items[i].mr;
            if (uw > n->pref_w) n->pref_w = uw;
        }
    }

    free(items);
    return used_cross;
}

static float layout_box(hn_context *c, hn_node *n, float x, float y,
                        float avail_w, float avail_h,
                        const hn_text_backend *tb, int measuring, float forced_h) {
    if (n->kind == HN_TEXT) {
        float pref = 0;
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

    /* 输入控件: 默认尺寸 */
    if (hn_node_is_input(n)) {
        if (st->width_u == HN_U_AUTO) { st->width = 180; st->width_u = HN_U_PX; }
        if (st->height_u == HN_U_AUTO) {
            st->height = st->font_size * st->line_height + 14;
            st->height_u = HN_U_PX;
        }
        if (st->padding[1] == 0 && st->padding[3] == 0) { st->padding[1] = 10; st->padding[3] = 10; }
        if (st->padding[0] == 0 && st->padding[2] == 0) { st->padding[0] = 7; st->padding[2] = 7; }
        pt = st->padding[0] + bt; pr = st->padding[1] + br;
        pb = st->padding[2] + bb; pl = st->padding[3] + bl;
    }

    float w = size_of(st->width, st->width_u, avail_w, st->font_size);
    float h = size_of(st->height, st->height_u, avail_h, st->font_size);
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
    int h_definite = (forced_h > 0) || (st->height_u != HN_U_AUTO && h >= 0);

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
        layout_input(c, n, content_w, tb, measuring);
        n->bw = content_w + pl + pr;
        if (h_definite) {
            h = (forced_h > 0 || st->box_border) ? h : h + pt + pb;
            n->bh = h;
        } else {
            n->bh = n->content_h > 0 ? n->content_h : st->font_size * st->line_height + pt + pb;
        }
        n->content_h = n->bh;
        return n->bh;
    }

    float content_used;
    if (st->display == HN_DISP_FLEX)
        content_used = layout_flex(c, n, st, x + pl, y + pt, content_w, content_h_def, tb, measuring);
    else
        content_used = layout_block(c, n, st, x + pl, y + pt, content_w, tb, measuring, content_h_def);

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
    n->content_h = content_used + pt + pb;
    if (n->pref_h > 0 && n->pref_h + pt + pb > n->content_h)
        n->content_h = n->pref_h + pt + pb;

    if (measuring && st->width_u == HN_U_AUTO) n->bw = n->pref_w + pl + pr;
    else n->bw = content_w + pl + pr;

    return n->bh;
}

void hn_layout_root(hn_context *c, const hn_text_backend *tb) {
    hn_node *root = c->doc->root;
    clear_runs(root);
    float forced = (root->style.height_u == HN_U_AUTO) ? c->vh : -1;
    layout_box(c, root, 0, 0, c->vw, c->vh, tb, 0, forced);
}

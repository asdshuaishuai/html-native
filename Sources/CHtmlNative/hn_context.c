/* hn_context.c — 上下文: 组合文档/样式表, 布局编排, 命中测试, 热更新(set_text/swap/render), 清单解析 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn_internal.h"

const char *hn_ua_css(void); /* hn_style.c */
static int anim_walk(hn_node *n, float dt_ms);

hn_context *hn_context_create(void) {
    hn_context *c = calloc(1, sizeof(hn_context));
    if (!c) abort();
    c->tmp = hn_arena_create();
    /* UA 样式表永远排在最前(最低优先级) */
    const char *ua = hn_ua_css();
    hn_sheet *sh = hn_parse_css(ua, strlen(ua));
    hn_context_add_sheet(c, sh);
    return c;
}

void hn_context_destroy(hn_context *c) {
    if (!c) return;
    for (int i = 0; i < c->n_sheets; i++) hn_sheet_free(c->sheets[i]);
    free(c->sheets);
    hn_doc_free(c->doc);
    hn_arena_destroy(c->tmp);
    free(c->cmds);
    free(c);
}

void hn_context_set_doc(hn_context *c, hn_doc *doc) {
    c->doc = doc;
    /* 主题是渲染语义: 声明了 hn-theme 就装载基座(所有平台一致) */
    hn_context_apply_theme(c);
}

hn_doc *hn_context_doc(hn_context *c) { return c->doc; }

void hn_context_add_sheet(hn_context *c, hn_sheet *sheet) {
    if (c->n_sheets == c->cap_sheets) {
        int nc = c->cap_sheets ? c->cap_sheets * 2 : 8;
        hn_sheet **ns = realloc(c->sheets, sizeof(hn_sheet *) * (size_t)nc);
        if (!ns) abort();
        c->sheets = ns;
        c->cap_sheets = nc;
    }
    c->sheets[c->n_sheets++] = sheet;
}

void hn_context_layout(hn_context *c, float width, float height,
                       const hn_text_backend *backend) {
    if (!c->doc) return;
    c->vw = width;
    c->vh = height;
    c->tb = backend;
    hn_arena_destroy(c->tmp);
    c->tmp = hn_arena_create();
    c->n_cmds = 0;
    hn_style_compute_all(c);
    hn_layout_root(c, backend);
    hn_paint_root(c);
    c->dl.cmds = c->cmds;
    c->dl.count = c->n_cmds;
}

void hn_context_repaint(hn_context *c) {
    if (!c->doc) return;
    hn_paint_root(c);
    c->dl.cmds = c->cmds;
    c->dl.count = c->n_cmds;
}

void hn_context_set_images(hn_context *c, const hn_image_backend *backend) {
    c->images = backend;
}

/* 推进过渡动画并写回样式; 返回 1 表示仍有动画在跑(dt_ms<0 → 仅初始化) */
int hn_context_anim_tick(hn_context *c, float dt_ms) {
    if (!c->doc || !c->doc->root) return 0;
    return anim_walk(c->doc->root, dt_ms);
}

void hn_context_set_hover(hn_context *c, hn_node *n) { c->hover_node = n; }
void hn_context_set_focus(hn_context *c, hn_node *n) { c->focus_node = n; }
void hn_context_set_caret_visible(hn_context *c, int on) { c->caret_on = on; }
void hn_context_set_active(hn_context *c, hn_node *n) { c->active_node = n; }

/* ---------------- 输入控件 ---------------- */

int hn_node_is_input(hn_node *n) {
    if (!n || n->kind != HN_ELEM || !n->tag) return 0;
    return !strcmp(n->tag, "input") || !strcmp(n->tag, "textarea");
}

const char *hn_node_value(hn_node *n, size_t *len_out) {
    if (len_out) *len_out = 0;
    if (!hn_node_is_input(n)) return NULL;
    if (n->value) {
        if (len_out) *len_out = n->value_len;
        return n->value;
    }
    const char *a = hn_node_attr(n, "value");
    if (a) {
        if (len_out) *len_out = strlen(a);
        return a;
    }
    if (len_out) *len_out = 0;
    return "";
}

/* 保证值缓冲区有容量(在节点所属文档的 arena 上增长) */
static void value_ensure(hn_node *n, size_t need) {
    if (n->value_cap >= need + 1) return;
    size_t nc = n->value_cap ? n->value_cap * 2 : 64;
    while (nc < need + 1) nc *= 2;
    char *nv = hn_arena_alloc(n->arena, nc);
    if (n->value && n->value_len) memcpy(nv, n->value, n->value_len);
    nv[n->value_len] = 0;
    n->value = nv;
    n->value_cap = nc;
}

int hn_node_set_value(hn_node *n, const char *utf8, size_t len) {
    if (!hn_node_is_input(n) || !n->arena) return 0;
    value_ensure(n, len);
    if (len) memcpy(n->value, utf8, len);
    n->value[len] = 0;
    n->value_len = len;
    n->caret = (int)len;
    return 1;
}

int hn_node_caret(hn_node *n) {
    return (n && hn_node_is_input(n)) ? n->caret : 0;
}

void hn_node_set_caret(hn_node *n, int byte_off) {
    if (!hn_node_is_input(n)) return;
    if (byte_off < 0) byte_off = 0;
    if ((size_t)byte_off > n->value_len) byte_off = (int)n->value_len;
    n->caret = byte_off;
}

/* 收集 name=value 并 URL 编码(application/x-www-form-urlencoded) */
static void form_walk(hn_node *n, char *out, size_t cap, size_t *used, int *first) {
    if (n->kind == HN_ELEM) {
        if (hn_node_is_input(n)) {
            const char *name = hn_node_attr(n, "name");
            if (name) {
                size_t vlen = 0;
                const char *v = hn_node_value(n, &vlen);
                if (*used < cap) {
                    if (!*first) *used += (size_t)snprintf(out + *used, cap - *used, "&");
                }
                *first = 0;
                for (const char *p = name; *p && *used + 4 < cap; p++) {
                    int c = (unsigned char)*p;
                    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                        out[(*used)++] = (char)c;
                    } else {
                        static const char *HX = "0123456789ABCDEF";
                        out[(*used)++] = '%';
                        out[(*used)++] = HX[(c >> 4) & 0xF];
                        out[(*used)++] = HX[c & 0xF];
                    }
                }
                if (*used + 1 < cap) out[(*used)++] = '=';
                for (size_t i = 0; i < vlen && *used + 4 < cap; i++) {
                    int c = (unsigned char)v[i];
                    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                        out[(*used)++] = (char)c;
                    } else {
                        static const char *HX = "0123456789ABCDEF";
                        out[(*used)++] = '%';
                        out[(*used)++] = HX[(c >> 4) & 0xF];
                        out[(*used)++] = HX[c & 0xF];
                    }
                }
            }
        }
        for (hn_node *ch = n->first; ch; ch = ch->next) form_walk(ch, out, cap, used, first);
    }
}

size_t hn_doc_form_encode(hn_doc *doc, char *out, size_t cap) {
    size_t used = 0;
    int first = 1;
    if (doc && doc->root) form_walk(doc->root, out, cap - 1, &used, &first);
    out[used] = 0;
    return used;
}

hn_node *hn_node_first_child(hn_node *n) { return n ? n->first : NULL; }
hn_node *hn_node_next_sibling(hn_node *n) { return n ? n->next : NULL; }

static int has_attr_walk(hn_node *n, const char *name) {
    if (n->kind == HN_ELEM && hn_node_attr(n, name)) return 1;
    for (hn_node *ch = n->first; ch; ch = ch->next)
        if (has_attr_walk(ch, name)) return 1;
    return 0;
}

int hn_doc_has_attr(hn_doc *doc, const char *name) {
    return (doc && doc->root) ? has_attr_walk(doc->root, name) : 0;
}

hn_node *hn_doc_input_at(hn_doc *doc, int idx) {
    if (!doc || !doc->root || idx < 0) return NULL;
    hn_node *stack[512];
    hn_node *found = NULL;
    int sp = 0, seen = 0;
    stack[sp++] = doc->root;
    while (sp > 0 && !found) {
        hn_node *n = stack[--sp];
        if (hn_node_is_input(n)) {
            if (seen == idx) { found = n; break; }
            seen++;
        }
        hn_node *kids[512];
        int kn = 0;
        for (hn_node *ch = n->first; ch && kn < 512; ch = ch->next) kids[kn++] = ch;
        for (int i = kn - 1; i >= 0 && sp < 512; i--) stack[sp++] = kids[i];
    }
    return found;
}

void hn_node_box(hn_node *n, float *x, float *y, float *w, float *h) {
    if (x) *x = n ? n->bx : 0;
    if (y) *y = n ? n->by : 0;
    if (w) *w = n ? n->bw : 0;
    if (h) *h = n ? n->bh : 0;
}

void hn_node_debug_background(hn_node *n, hn_color *out) {
    if (out) *out = n ? n->style.background : 0;
}

void hn_node_debug_border(hn_node *n, hn_color *out) {
    if (out) *out = n ? n->style.border_color : 0;
}

void hn_node_debug_color(hn_node *n, hn_color *out) {
    if (out) *out = n ? n->style.color : 0;
}

int hn_node_display(hn_node *n) {
    return n ? (int)n->style.display : 2;
}

int hn_node_run_count(hn_node *n) {
    return n ? n->n_runs : 0;
}

int hn_node_run_at(hn_node *n, int i, float *x, float *baseline, float *w,
                   float *y_top, float *h) {
    if (!n || i < 0 || i >= n->n_runs) return 0;
    hn_run *r = &n->runs[i];
    if (x) *x = r->x;
    if (baseline) *baseline = r->baseline;
    if (w) *w = r->width;
    if (y_top) *y_top = r->y_top;
    if (h) *h = r->h;
    return 1;
}

int hn_node_cursor(hn_node *n) {
    return (n && n->kind == HN_ELEM) ? n->style.cursor : 0;
}

const hn_display_list *hn_context_display_list(const hn_context *c) {
    if (!c || !c->doc) return NULL;
    return &((hn_context *)c)->dl;
}

/* ---------------- 命中测试 ---------------- */

static hn_node *hit_walk(hn_node *n, float x, float y) {
    if (n->kind == HN_ELEM && n->style.display == HN_DISP_NONE) return NULL;
    if (x < n->bx || y < n->by || x >= n->bx + n->bw || y >= n->by + n->bh) return NULL;
    for (hn_node *ch = n->last; ch; ch = ch->prev) {
        hn_node *r = hit_walk(ch, x + n->scroll_x, y + n->scroll_y);
        if (r) return r;
    }
    return n->kind == HN_ELEM ? n : NULL; /* 文本节点向上冒泡到父元素 */
}

hn_node *hn_context_hit_node(hn_context *c, float x, float y) {
    if (!c->doc) return NULL;
    return hit_walk(c->doc->root, x, y);
}

/* ---------------- 过渡动画 ----------------
 *
 * 引擎持有"当前值"(n->anim), 样式计算得到"目标值"。每次 tick 时:
 *   目标变化 → 记录 from/to 并重置进度; 否则推进进度。
 * 绘制前把插值结果覆盖到样式字段, 绘制器无需感知动画。
 */

static void rgba_of(hn_color c, float out[4]) {
    out[0] = (float)((c >> 24) & 0xFF) / 255.0f;
    out[1] = (float)((c >> 16) & 0xFF) / 255.0f;
    out[2] = (float)((c >> 8) & 0xFF) / 255.0f;
    out[3] = (float)(c & 0xFF) / 255.0f;
}

static hn_color color_of(const float v[4]) {
    int r = (int)(v[0] * 255.0f + 0.5f), g = (int)(v[1] * 255.0f + 0.5f);
    int b = (int)(v[2] * 255.0f + 0.5f), a = (int)(v[3] * 255.0f + 0.5f);
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    if (a < 0) a = 0; if (a > 255) a = 255;
    return ((hn_color)r << 24) | ((hn_color)g << 16) | ((hn_color)b << 8) | (hn_color)a;
}

static void lerp4(float out[4], const float a[4], const float b[4], float t) {
    for (int i = 0; i < 4; i++) out[i] = a[i] + (b[i] - a[i]) * t;
}

/* 遍历树: 对齐动画目标, 必要时推进进度, 把当前值写回样式。
 * dt_ms < 0 表示"只初始化不推进"(首帧)。返回 1 表示仍有动画在跑。 */
static int anim_walk(hn_node *n, float dt_ms) {
    int active = 0;
    if (n->kind == HN_ELEM) {
        hn_style *st = &n->style;
        hn_anim *a = &n->anim;

        /* 样式计算刚写入的字段就是"目标值"; 但绘制前我们会把插值结果写回同一字段。
           为区分二者: 每次写回后记录 last_bg/last_fg; 下次比较时若与记录一致,
           说明该字段是插值产物, 真正的目标值仍是 a->*_to。 */
        float bg_now[4], fg_now[4];
        rgba_of(st->background, bg_now);
        rgba_of(st->color, fg_now);

        if (!a->inited) {
            a->inited = 1;
            a->ms = st->transition_ms;
            a->o = a->o_to = st->opacity;
            memcpy(a->bg, bg_now, sizeof(bg_now));
            memcpy(a->fg, fg_now, sizeof(fg_now));
            memcpy(a->bg_to, bg_now, sizeof(bg_now));
            memcpy(a->fg_to, fg_now, sizeof(fg_now));
            memcpy(a->bg_from, bg_now, sizeof(bg_now));
            memcpy(a->fg_from, fg_now, sizeof(fg_now));
            a->o_from = a->o_to;
            a->active = 0;
            goto children;
        }

        {
            /* 采用哪个值作为目标:
               - 若上次 tick 写回过(dirty_written) 且当前样式字段与我们写回的一致
                 → 字段是插值产物, 目标取 a->*_to
               - 否则字段就是样式计算刚写入的目标 */
            int style_is_ours = a->dirty_written &&
                memcmp(bg_now, a->bg, sizeof(bg_now)) == 0 &&
                memcmp(fg_now, a->fg, sizeof(fg_now)) == 0;
            float bg_target[4], fg_target[4];
            if (style_is_ours) {
                memcpy(bg_target, a->bg_to, sizeof(bg_target));
                memcpy(fg_target, a->fg_to, sizeof(fg_target));
            } else {
                memcpy(bg_target, bg_now, sizeof(bg_target));
                memcpy(fg_target, fg_now, sizeof(fg_target));
                a->dirty_written = 0;   /* 新目标来自样式计算 */
            }

            int changed = 0;
            for (int i = 0; i < 4; i++) {
                if (fabsf(bg_target[i] - a->bg_to[i]) > 0.004f) { a->bg_to[i] = bg_target[i]; changed = 1; }
                if (fabsf(fg_target[i] - a->fg_to[i]) > 0.004f) { a->fg_to[i] = fg_target[i]; changed = 1; }
            }
            if (st->transition_ms != a->ms) { a->ms = st->transition_ms; changed = 1; }

            if (changed) {
                memcpy(a->bg_from, a->bg, sizeof(a->bg));
                memcpy(a->fg_from, a->fg, sizeof(a->fg));
                a->o_from = a->o;
                a->t = 0;
                a->active = (a->ms > 0);
            }

            if (a->active && a->ms > 0) {
                if (dt_ms > 0) a->t += dt_ms / a->ms;
                if (a->t >= 1.0f) { a->t = 1.0f; a->active = 0; }
                float e = a->t * a->t * (3.0f - 2.0f * a->t); /* smoothstep */
                lerp4(a->bg, a->bg_from, a->bg_to, e);
                lerp4(a->fg, a->fg_from, a->fg_to, e);
                a->o = a->o_from + (a->o_to - a->o_from) * e;
            } else {
                memcpy(a->bg, a->bg_to, sizeof(a->bg));
                memcpy(a->fg, a->fg_to, sizeof(a->fg));
                a->o = a->o_to;
            }

            st->background = color_of(a->bg);
            st->color = color_of(a->fg);
            st->opacity = a->o;
            a->dirty_written = 1;
        }
        if (a->active) active = 1;
    }
children:
    for (hn_node *ch = n->first; ch; ch = ch->next)
        if (anim_walk(ch, dt_ms)) active = 1;
    return active;
}

/* 推进过渡动画并写回样式; 返回 1 表示仍有动画在跑(dt_ms<0 → 仅初始化) */
/* ---------------- 滚动 ---------------- */

/* 找点位下最深的 overflow 容器 */
static hn_node *scroll_walk(hn_node *n, float x, float y) {
    if (n->kind == HN_ELEM && n->style.display == HN_DISP_NONE) return NULL;
    if (x < n->bx || y < n->by || x >= n->bx + n->bw || y >= n->by + n->bh) return NULL;
    hn_node *found = NULL;
    for (hn_node *ch = n->last; ch; ch = ch->prev) {
        hn_node *r = scroll_walk(ch, x + n->scroll_x, y + n->scroll_y);
        if (r) { found = r; break; }
    }
    if (found) return found;
    return (n->kind == HN_ELEM && n->style.overflow) ? n : NULL;
}

hn_node *hn_context_scrollable_at(hn_context *c, float x, float y) {
    if (!c->doc) return NULL;
    return scroll_walk(c->doc->root, x, y);
}

int hn_node_scroll_by(hn_node *n, float dx, float dy) {
    if (!n || n->kind != HN_ELEM || !n->style.overflow) return 0;
    float max_y = n->content_h - n->bh;
    if (max_y < 0) max_y = 0;
    float max_x = 0; /* v1 仅纵向滚动 */
    float ny = n->scroll_y + dy;
    if (ny < 0) ny = 0;
    if (ny > max_y) ny = max_y;
    (void)dx; (void)max_x;
    if (ny == n->scroll_y) return 0;
    n->scroll_y = ny;
    return 1;
}

const char *hn_context_hit_test(hn_context *c, float x, float y) {
    hn_node *n = hn_context_hit_node(c, x, y);
    /* 无 id 的元素继续向上找最近有 id 的祖先(冒泡语义) */
    while (n) {
        if (n->id) return n->id;
        n = n->parent;
    }
    return NULL;
}

/* ---------------- 属性 / 查找 ---------------- */

const char *hn_node_tag(hn_node *n) {
    if (!n || n->kind != HN_ELEM) return NULL;
    return n->tag;
}

const char *hn_node_attr(hn_node *n, const char *name) {
    if (!n) return NULL;
    for (int i = 0; i < n->n_attrs; i++)
        if (!strcmp(n->attrs[i].name, name)) return n->attrs[i].value;
    return NULL;
}

hn_node *hn_node_ancestor_with_attr(hn_node *n, const char *name) {
    for (; n; n = n->parent)
        if (n->kind == HN_ELEM && hn_node_attr(n, name)) return n;
    return NULL;
}

/* root 子树内(文档序, 跳过 exclude 所在分支)第一个携带指定属性的元素 */
static hn_node *subtree_first_with_attr(hn_node *root, hn_node *exclude, const char *name) {
    if (root != exclude && root->kind == HN_ELEM && hn_node_attr(root, name)) return root;
    for (hn_node *ch = root->first; ch; ch = ch->next) {
        if (ch == exclude) continue;
        hn_node *r = subtree_first_with_attr(ch, exclude, name);
        if (r) return r;
    }
    return NULL;
}

hn_node *hn_node_form_carrier(hn_node *n) {
    if (!n || n->kind != HN_ELEM) return NULL;
    if (hn_node_attr(n, "hx-post") || hn_node_attr(n, "hx-get")) return n;
    int depth = 0;
    for (hn_node *p = n->parent; p && depth < 6; p = p->parent, depth++) {
        if (p->kind == HN_ELEM && (hn_node_attr(p, "hx-post") || hn_node_attr(p, "hx-get")))
            return p;
        hn_node *f = subtree_first_with_attr(p, n, "hx-post");
        if (!f) f = subtree_first_with_attr(p, n, "hx-get");
        if (f) return f;
    }
    return NULL;
}

static hn_node *find_by_id_rec(hn_node *n, const char *id) {
    if (n->kind == HN_ELEM) {
        const char *vid = hn_node_attr(n, "id");
        if (vid && !strcmp(vid, id)) return n;
        for (hn_node *ch = n->first; ch; ch = ch->next) {
            hn_node *r = find_by_id_rec(ch, id);
            if (r) return r;
        }
    }
    return NULL;
}

int hn_doc_autoid_hx(hn_doc *doc) {
    if (!doc || !doc->root) return 0;
    int assigned = 0, counter = 0;
    hn_node *stack[256];
    int sp = 0;
    stack[sp++] = doc->root;
    while (sp) {
        hn_node *n = stack[--sp];
        if (n->kind != HN_ELEM) continue;
        int has_hx = hn_node_attr(n, "hx-get") || hn_node_attr(n, "hx-post");
        if (has_hx && !hn_node_attr(n, "id") && n->arena) {
            char buf[32];
            snprintf(buf, sizeof(buf), "hx-auto-%d", ++counter);
            size_t vl = strlen(buf) + 1;
            char *v = hn_arena_alloc(n->arena, vl);
            if (v) {
                memcpy(v, buf, vl);
                hn_attr *na = hn_arena_alloc(n->arena, sizeof(hn_attr) * (size_t)(n->n_attrs + 1));
                if (na) {
                    for (int i = 0; i < n->n_attrs; i++) na[i] = n->attrs[i];
                    na[n->n_attrs].name = "id";
                    na[n->n_attrs].value = v;
                    n->attrs = na;
                    n->n_attrs++;
                    assigned++;
                }
            }
        }
        for (hn_node *ch = n->last; ch; ch = ch->prev)
            if (sp < 256) stack[sp++] = ch;
    }
    return assigned;
}

hn_node *hn_doc_root(hn_doc *doc) {
    return (doc && doc->root) ? doc->root : NULL;
}

hn_node *hn_doc_body(hn_doc *doc) {
    if (!doc || !doc->root) return NULL;
    for (hn_node *ch = doc->root->first; ch; ch = ch->next)
        if (ch->kind == HN_ELEM && ch->tag && !strcmp(ch->tag, "body")) return ch;
    return doc->root;
}

hn_node *hn_doc_find_by_id(hn_doc *doc, const char *id) {
    if (!doc || !doc->root || !id) return NULL;
    return find_by_id_rec(doc->root, id);
}

const char *hn_doc_find_trigger_on_load(hn_doc *doc) {
    const char *id = NULL;
    hn_doc_load_at(doc, 0, &id);
    return id;
}

/* 枚举 hx-trigger 含 "load" 的元素(页面启动自动请求用; 全量, 非仅首个) */
int hn_doc_load_at(hn_doc *doc, int idx, const char **out_id) {
    if (out_id) *out_id = NULL;
    if (!doc) return 0;
    hn_node *stack[256];
    int sp = 0, found = 0;
    stack[sp++] = doc->root;
    while (sp) {
        hn_node *n = stack[--sp];
        if (n->kind != HN_ELEM) continue;
        const char *trig = hn_node_attr(n, "hx-trigger");
        if (trig && strstr(trig, "load")) {
            /* token 级匹配, 排除 "autoload" 之类的误命中 */
            const char *p = trig;
            int hit = 0;
            while (*p && !hit) {
                while (*p == ' ' || *p == ',') p++;
                const char *st = p;
                while (*p && *p != ' ' && *p != ',') p++;
                if ((size_t)(p - st) == 4 && !strncmp(st, "load", 4)) hit = 1;
            }
            if (hit) {
                if (found == idx) {
                    const char *id = hn_node_attr(n, "id");
                    if (id && out_id) *out_id = id;
                    return 1;
                }
                found++;
            }
        }
        /* 子节点逆序入栈保持文档序 */
        for (hn_node *ch = n->last; ch; ch = ch->prev)
            if (sp < 256) stack[sp++] = ch;
    }
    return 0;
}

/* ---------------- 应用清单 ---------------- */

void hn_doc_manifest(const hn_doc *doc, hn_manifest *out) {
    out->surface = HN_SURFACE_WINDOW;
    out->w = out->h = 0;
    out->x = out->y = 0;
    out->title = NULL;
    out->theme = NULL;
    if (!doc || !doc->root) return;

    hn_node *stack[256];
    int sp = 0;
    stack[sp++] = doc->root;
    while (sp > 0) {
        hn_node *n = stack[--sp];
        if (n->kind == HN_ELEM && n->tag && !strcmp(n->tag, "meta")) {
            const char *name = hn_node_attr(n, "name");
            const char *content = hn_node_attr(n, "content");
            if (name && content) {
                if (!strcmp(name, "hn-theme")) {
                    out->theme = content;
                } else if (!strcmp(name, "hn-surface")) {
                    if (!strncmp(content, "popup", 5)) out->surface = HN_SURFACE_POPUP;
                    else if (!strncmp(content, "layer", 5)) out->surface = HN_SURFACE_LAYER;
                    else out->surface = HN_SURFACE_WINDOW;
                } else if (!strcmp(name, "hn-window")) {
                    float w = 0, h = 0, x = 0, y = 0;
                    if (sscanf(content, "%fx%f@%f,%f", &w, &h, &x, &y) >= 2) {
                        out->w = w; out->h = h; out->x = x; out->y = y;
                    } else {
                        int nw = 0, nh = 0;
                        if (sscanf(content, "%dx%d", &nw, &nh) == 2) {
                            out->w = (float)nw; out->h = (float)nh;
                        } else if (sscanf(content, "%fx%f", &w, &h) == 2) {
                            out->w = w; out->h = h;
                        }
                    }
                } else if (!strcmp(name, "hn-title")) {
                    out->title = content;
                }
            }
        }
        hn_node *kids[256];
        int kn = 0;
        for (hn_node *ch = n->first; ch && kn < 256; ch = ch->next) kids[kn++] = ch;
        for (int i = kn - 1; i >= 0 && sp < 256; i--) stack[sp++] = kids[i];
    }
}

/* 解析 hx-trigger 中的 "every N[ms|s]" → 毫秒; 无则 0 */
static int parse_every(const char *trig) {
    if (!trig) return 0;
    /* 在整串里找 "every", 其后的数值 token 即间隔(可带 ms/s 后缀) */
    const char *hit = NULL;
    for (const char *p = trig; *p; p++) {
        if (!strncmp(p, "every", 5) &&
            (p == trig || p[-1] == ' ' || p[-1] == ',') &&
            (p[5] == 0 || p[5] == ' ' || p[5] == '\t')) { hit = p + 5; break; }
    }
    if (!hit) return 0;
    while (*hit == ' ' || *hit == '\t') hit++;
    if (!*hit) return 0;
    const char *st = hit;
    while (*hit && *hit != ' ' && *hit != ',' && *hit != '\t') hit++;
    char buf[24];
    size_t bl = (size_t)(hit - st);
    if (bl == 0 || bl >= sizeof(buf)) return 0;
    memcpy(buf, st, bl);
    buf[bl] = 0;
    double v = strtod(buf, NULL);
    if (v <= 0) return 0;
    int ms = strstr(buf, "ms") ? (int)v : (int)(v * 1000.0);
    if (ms < 50) ms = 50;             /* 下限: 避免忙轮询 */
    if (ms > 3600000) ms = 3600000;   /* 上限: 1 小时 */
    return ms;
}

int hn_doc_poll_at(hn_doc *doc, int idx, const char **out_id, int *out_ms) {
    if (!doc || !doc->root || idx < 0) return 0;
    hn_node *stack[512];
    int sp = 0, seen = 0;
    stack[sp++] = doc->root;
    while (sp > 0) {
        hn_node *n = stack[--sp];
        if (n->kind == HN_ELEM) {
            const char *trig = hn_node_attr(n, "hx-trigger");
            const char *id = hn_node_attr(n, "id");
            int ms = parse_every(trig);
            if (ms > 0 && id) {
                if (seen == idx) {
                    if (out_id) *out_id = id;
                    if (out_ms) *out_ms = ms;
                    return 1;
                }
                seen++;
            }
        }
        hn_node *kids[512];
        int kn = 0;
        for (hn_node *ch = n->first; ch && kn < 512; ch = ch->next) kids[kn++] = ch;
        for (int i = kn - 1; i >= 0 && sp < 512; i--) stack[sp++] = kids[i];
    }
    return 0;
}

/* ---------------- 热更新 ---------------- */

int hn_doc_set_text(hn_doc *doc, const char *element_id, const char *utf8_text) {
    if (!doc || !doc->root) return 0;
    hn_node *n = find_by_id_rec(doc->root, element_id);
    if (!n) return 0;
    n->first = n->last = NULL;
    hn_node *t = hn_arena_alloc(doc->arena, sizeof(hn_node));
    memset(t, 0, sizeof(hn_node));
    t->kind = HN_TEXT;
    t->parent = n;
    t->text = hn_arena_strndup(doc->arena, utf8_text ? utf8_text : "", utf8_text ? strlen(utf8_text) : 0);
    t->text_len = t->text ? strlen(t->text) : 0;
    n->first = n->last = t;
    return 1;
}

/* 片段子节点从临时 root 摘下, 链入目标 */
static void splice_children(hn_doc *doc, hn_node *frag, hn_node *target, hn_swap_mode mode) {
    (void)doc;
    hn_node *ff = frag->first, *fl = frag->last;
    if (!ff) {
        if (mode == HN_SWAP_INNER) target->first = target->last = NULL;
        return;
    }
    switch (mode) {
    case HN_SWAP_INNER:
        target->first = target->last = NULL;
        for (hn_node *ch = ff; ch; ch = ch->next) ch->parent = target;
        target->first = ff;
        target->last = fl;
        break;
    case HN_SWAP_APPEND: {
        for (hn_node *ch = ff; ch; ch = ch->next) ch->parent = target;
        if (target->last) {
            target->last->next = ff;
            ff->prev = target->last;
            target->last = fl;
        } else {
            target->first = ff;
            target->last = fl;
        }
        break;
    }
    case HN_SWAP_PREPEND: {
        for (hn_node *ch = ff; ch; ch = ch->next) ch->parent = target;
        if (target->first) {
            target->first->prev = fl;
            fl->next = target->first;
            target->first = ff;
        } else {
            target->first = ff;
            target->last = fl;
        }
        break;
    }    case HN_SWAP_OUTER: {
        hn_node *p = target->parent;
        if (!p) break; /* 根不可替换 */
        hn_node *before = target->prev, *after = target->next;
        if (before) before->next = after; else p->first = after;
        if (after) after->prev = before; else p->last = before;
        for (hn_node *ch = ff; ch; ch = ch->next) ch->parent = p;
        if (before) { before->next = ff; ff->prev = before; }
        else p->first = ff;
        if (after) { after->prev = fl; fl->next = after; }
        else p->last = fl;
        break;
    }
    }
}

int hn_doc_swap(hn_doc *doc, const char *target_id, hn_swap_mode mode,
                const char *html_src, size_t len) {
    if (!doc || !doc->root || !target_id) return 0;
    hn_node *target = find_by_id_rec(doc->root, target_id);
    if (!target) return 0;
    hn_node *frag = hn_arena_alloc(doc->arena, sizeof(hn_node));
    memset(frag, 0, sizeof(hn_node));
    frag->kind = HN_ELEM;
    frag->tag = "fragment";
    if (html_src && len) hn_parse_into(doc, frag, html_src, len);
    splice_children(doc, frag, target, mode);
    return 1;
}

/* 热渲染: 用新 HTML 整体替换文档内容(保留样式表) — agent 直接推送 UI 的原语 */
void hn_context_render(hn_context *c, const char *html_src, size_t len) {
    if (!c->doc) return;
    hn_node *root = c->doc->root;
    root->first = root->last = NULL;
    if (html_src && len) {
        hn_node *frag = hn_arena_alloc(c->doc->arena, sizeof(hn_node));
        memset(frag, 0, sizeof(hn_node));
        frag->kind = HN_ELEM;
        frag->tag = "fragment";
        hn_parse_into(c->doc, frag, html_src, len);
        for (hn_node *ch = frag->first; ch; ch = ch->next) ch->parent = root;
        root->first = frag->first;
        root->last = frag->last;
    }
    /* 热更新可能改写了 hn-theme: 重新装载主题基座 */
    hn_context_apply_theme(c);
}

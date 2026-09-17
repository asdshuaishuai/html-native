/* hn_context.c — 上下文: 组合文档/样式表, 布局编排, 命中测试, 热更新(set_text/swap/render), 清单解析 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn_internal.h"

const char *hn_ua_css(void); /* hn_style.c */
static int anim_walk(hn_context *c, hn_node *n, float dt_ms);

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
    hn_context_lottie_clear(c);
    free(c->lot);
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
    if (backend) { c->tb = *backend; c->tb_valid = 1; } else { c->tb_valid = 0; }
    hn_arena_destroy(c->tmp);
    c->tmp = hn_arena_create();
    c->n_cmds = 0;
    hn_style_compute_all(c);
    /* 透明背板: 样式重算会把底色从级联装回来, 必须在这之后重新剥离。
       这是"持久开关"语义的关键一步(见 hn_context_strip_root_background)。 */
    if (c->transparent) hn_strip_root_background_now(c);
    hn_layout_root(c);
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

void hn_context_set_assets(hn_context *c, const hn_asset_backend *backend) {
    c->assets = backend;
    hn_context_lottie_clear(c);     /* 后端换了, 旧解析结果可能不再可得 */
}

const hn_asset_backend *hn_context_assets(hn_context *c) { return c ? c->assets : NULL; }

/* ---- Lottie 缓存(按路径) ---- */

struct hn_lottie *hn_context_lottie(hn_context *c, const char *path) {
    if (!c || !path || !*path) return NULL;
    for (int i = 0; i < c->n_lot; i++)
        if (c->lot[i].path && !strcmp(c->lot[i].path, path)) return c->lot[i].lottie;
    struct hn_lottie *l = hn_lottie_load(c, path);
    if (!l) {
        /* 缓存失败结果(负缓存), 避免每帧重复尝试读文件 */
        if (c->n_lot == c->cap_lot) {
            int nc = c->cap_lot ? c->cap_lot * 2 : 8;
            void *nv = realloc(c->lot, sizeof(*c->lot) * (size_t)nc);
            if (!nv) return NULL;
            c->lot = nv; c->cap_lot = nc;
        }
        c->lot[c->n_lot].path = strdup(path);
        c->lot[c->n_lot].lottie = NULL;
        c->n_lot++;
        return NULL;
    }
    if (c->n_lot == c->cap_lot) {
        int nc = c->cap_lot ? c->cap_lot * 2 : 8;
        void *nv = realloc(c->lot, sizeof(*c->lot) * (size_t)nc);
        if (!nv) { hn_lottie_free(l); return NULL; }
        c->lot = nv; c->cap_lot = nc;
    }
    c->lot[c->n_lot].path = strdup(path);
    c->lot[c->n_lot].lottie = l;
    c->n_lot++;
    return l;
}

void hn_context_lottie_clear(hn_context *c) {
    if (!c || !c->lot) return;
    for (int i = 0; i < c->n_lot; i++) {
        free(c->lot[i].path);
        if (c->lot[i].lottie) hn_lottie_free(c->lot[i].lottie);
    }
    c->n_lot = 0;
}

/* ---- 脚本驱动网格 ---- */

int hn_node_set_mesh_verts(hn_node *n, const float *verts, int cols, int rows) {
    if (!n || n->kind != HN_ELEM || cols <= 0 || rows <= 0) return 0;
    if (cols > 128) cols = 128;
    if (rows > 128) rows = 128;
    int nv = (cols + 1) * (rows + 1);
    float *p = (float *)hn_arena_alloc(n->arena, sizeof(float) * (size_t)(nv * 2));
    if (!p) return 0;
    if (verts) memcpy(p, verts, sizeof(float) * (size_t)(nv * 2));
    else {
        /* 未给顶点则用均匀网格 */
        for (int r = 0; r <= rows; r++)
            for (int cc = 0; cc <= cols; cc++) {
                int i = r * (cols + 1) + cc;
                p[i * 2]     = (float)cc / (float)cols * n->bw;
                p[i * 2 + 1] = (float)r / (float)rows * n->bh;
            }
    }
    n->mesh_verts = p;
    n->mesh_cols = cols;
    n->mesh_rows = rows;
    return 1;
}

int hn_node_mesh_info(hn_node *n, int *cols, int *rows) {
    if (!n || n->kind != HN_ELEM) return 0;
    if (n->mesh_verts) {
        if (cols) *cols = n->mesh_cols;
        if (rows) *rows = n->mesh_rows;
        return 1;
    }
    const char *g = hn_node_attr(n, "hn-mesh");
    if (!g || !*g || !strcmp(g, "false") || !strcmp(g, "0")) return 0;
    if (cols) *cols = 0;
    if (rows) *rows = 0;
    return 1;
}

/* 推进过渡动画并写回样式; 返回 1 表示仍有动画在跑(dt_ms<0 → 仅初始化) */
int hn_context_anim_tick(hn_context *c, float dt_ms) {
    if (!c->doc || !c->doc->root) return 0;
    return anim_walk(c, c->doc->root, dt_ms);
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

/* ---------------- DOM 变更(push_attr / splice 复用) ---------------- */

int hn_node_set_attr(hn_node *n, const char *name, const char *value) {
    if (!n || n->kind != HN_ELEM || !name || !n->arena) return 0;
    /* 已存在则原地改值(值在 arena 上重新分配, 旧值不回收) */
    for (int i = 0; i < n->n_attrs; i++) {
        if (!strcmp(n->attrs[i].name, name)) {
            n->attrs[i].value = hn_arena_strndup(n->arena, value ? value : "",
                                                 value ? strlen(value) : 0);
            return 1;
        }
    }
    hn_attr *na = hn_arena_alloc(n->arena, sizeof(hn_attr) * (size_t)(n->n_attrs + 1));
    if (!na) return 0;
    for (int i = 0; i < n->n_attrs; i++) na[i] = n->attrs[i];
    na[n->n_attrs].name = hn_arena_strndup(n->arena, name, strlen(name));
    na[n->n_attrs].value = hn_arena_strndup(n->arena, value ? value : "",
                                            value ? strlen(value) : 0);
    n->attrs = na;
    n->n_attrs++;
    return 1;
}

static size_t text_rec(hn_node *n, char *out, size_t cap, size_t used) {
    for (hn_node *ch = n->first; ch; ch = ch->next) {
        if (ch->kind == HN_TEXT && ch->text && ch->text_len) {
            size_t room = cap > used + 1 ? cap - used - 1 : 0;
            size_t take = ch->text_len < room ? ch->text_len : room;
            if (take) memcpy(out + used, ch->text, take);
            used += take;
        } else if (ch->kind == HN_ELEM) {
            used = text_rec(ch, out, cap, used);
        }
    }
    return used;
}

size_t hn_node_text_content(hn_node *n, char *out, size_t cap) {
    if (!n || !out || cap == 0) return 0;
    size_t used = text_rec(n, out, cap, 0);
    out[used] = 0;
    return used;
}

int hn_node_set_text_content(hn_node *n, const char *utf8, size_t len) {
    if (!n || n->kind != HN_ELEM || !n->arena) return 0;
    n->first = n->last = NULL;
    if (!utf8 || !len) return 1;
    hn_node *t = hn_arena_alloc(n->arena, sizeof(hn_node));
    if (!t) return 0;
    memset(t, 0, sizeof(hn_node));
    t->kind = HN_TEXT;
    t->arena = n->arena;
    t->parent = n;
    t->text = hn_arena_strndup(n->arena, utf8, len);
    t->text_len = len;
    n->first = n->last = t;
    return 1;
}

hn_node *hn_node_append_element(hn_node *parent, const char *tag) {
    if (!parent || !tag || !parent->arena) return NULL;
    hn_node *el = hn_arena_alloc(parent->arena, sizeof(hn_node));
    if (!el) return NULL;
    memset(el, 0, sizeof(hn_node));
    el->kind = HN_ELEM;
    el->arena = parent->arena;
    el->tag = hn_arena_strndup(parent->arena, tag, strlen(tag));
    el->anim.fresh = 1;
    el->parent = parent;
    if (parent->last) { parent->last->next = el; el->prev = parent->last; parent->last = el; }
    else { parent->first = parent->last = el; }
    return el;
}

int hn_node_remove_child(hn_node *parent, hn_node *child) {
    if (!parent || !child) return 0;
    for (hn_node *ch = parent->first; ch; ch = ch->next) {
        if (ch != child) continue;
        if (ch->prev) ch->prev->next = ch->next; else parent->first = ch->next;
        if (ch->next) ch->next->prev = ch->prev; else parent->last = ch->prev;
        ch->next = NULL; ch->prev = NULL; ch->parent = NULL;
        return 1;
    }
    return 0;
}

/* 解析 HTML 片段并插入(供脚本的 innerHTML/append 语义) */
int hn_node_insert_html(hn_node *parent, const char *html, size_t len, hn_swap_mode mode) {
    if (!parent || !html || !parent->arena) return 0;
    hn_node *frag = hn_arena_alloc(parent->arena, sizeof(hn_node));
    if (!frag) return 0;
    memset(frag, 0, sizeof(hn_node));
    frag->kind = HN_ELEM;
    frag->arena = parent->arena;
    frag->tag = "fragment";
    /* 注意: hn_parse_into 会通过 doc 登记内联样式表(<style>), 不能传 NULL。
       片段里的 <style> 也应生效, 因此借用宿主文档的 arena 建一个临时 doc 视图。 */
    hn_doc tmpdoc;
    memset(&tmpdoc, 0, sizeof(tmpdoc));
    tmpdoc.arena = parent->arena;
    tmpdoc.root = frag;
    hn_parse_into(&tmpdoc, frag, html, len);
    hn_node *ff = frag->first, *fl = frag->last;
    if (!ff) {
        if (mode == HN_SWAP_INNER) parent->first = parent->last = NULL;
        return 1;
    }
    ff->prev = NULL;
    fl->next = NULL;
    if (mode == HN_SWAP_INNER) {
        parent->first = parent->last = NULL;
        for (hn_node *c = ff; c; c = c->next) c->parent = parent;
        parent->first = ff; parent->last = fl;
    } else if (mode == HN_SWAP_PREPEND) {
        for (hn_node *c = ff; c; c = c->next) c->parent = parent;
        if (parent->first) { parent->first->prev = fl; fl->next = parent->first; parent->first = ff; }
        else { parent->first = ff; parent->last = fl; }
    } else { /* APPEND */
        for (hn_node *c = ff; c; c = c->next) c->parent = parent;
        if (parent->last) { parent->last->next = ff; ff->prev = parent->last; parent->last = fl; }
        else { parent->first = ff; parent->last = fl; }
    }
    return 1;
}

void hn_node_debug_anim(hn_node *n, const char **name, float *ms, int *iter) {
    if (name) *name = n ? n->style.kf_name : NULL;
    if (ms) *ms = n ? n->style.kf_ms : 0;
    if (iter) *iter = n ? n->style.kf_iter : 0;
}

void hn_node_debug_rotate(hn_node *n, float *deg) {
    if (deg) *deg = n ? n->style.rotate : 0;
}

void hn_node_debug_opacity(hn_node *n, float *out) {
    if (out) *out = n ? n->style.opacity : 0;
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

int hn_node_run_range(hn_node *n, int i, size_t *begin, size_t *end) {
    if (!n || !n->runs || i < 0 || i >= n->n_runs) return 0;
    if (begin) *begin = n->runs[i].begin;
    if (end) *end = n->runs[i].end;
    return 1;
}

const char *hn_node_text(hn_node *n, size_t *len_out) {
    if (!n || n->kind != HN_TEXT) { if (len_out) *len_out = 0; return NULL; }
    if (len_out) *len_out = n->text_len;
    return n->text;
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
/* ---------- @keyframes ---------- */

hn_keyframes *hn_sheet_find_kf(hn_sheet *sh, const char *name) {
    if (!sh || !name) return NULL;
    for (int i = 0; i < sh->n_kfs; i++)
        if (sh->kfs[i].name && !strcmp(sh->kfs[i].name, name)) return &sh->kfs[i];
    return NULL;
}

/* 从所有已加载样式表里按名字找动画定义 */
static const hn_keyframes *find_kf(hn_context *c, const char *name) {
    if (!c || !name) return NULL;
    for (int i = 0; i < c->n_sheets; i++) {
        hn_keyframes *kf = hn_sheet_find_kf(c->sheets[i], name);
        if (kf) return kf;
    }
    if (c->doc)
        for (int i = 0; i < c->doc->n_inline; i++) {
            hn_keyframes *kf = hn_sheet_find_kf(c->doc->inline_sheets[i], name);
            if (kf) return kf;
        }
    return NULL;
}

/* 在关键帧时间轴 t(0..1) 处取样: 找到相邻两帧并插值, 结果写入 style。
   只处理可动画的几何与颜色属性(与 transition 支持的范围一致)。 */
static void kf_sample(const hn_keyframes *kf, float t, hn_style *st) {
    if (!kf || kf->n_stops == 0) return;
    /* 找 t 落在的区间 */
    int a = 0, b = kf->n_stops - 1;
    for (int i = 0; i < kf->n_stops - 1; i++) {
        if (t >= kf->stops[i].at && t <= kf->stops[i + 1].at) { a = i; b = i + 1; break; }
    }
    const hn_kf_stop *sa = &kf->stops[a], *sb = &kf->stops[b];
    float span = sb->at - sa->at;
    float f = span > 0.0001f ? (t - sa->at) / span : 0;
    if (f < 0) f = 0; else if (f > 1) f = 1;
    /* 两个端点都要看: 某属性可能只在其中一帧声明(另一帧用元素当前值 = 不插值) */
    for (int side = 0; side < 2; side++) {
        const hn_kf_stop *stp = side == 0 ? sa : sb;
        float k = side == 0 ? (1 - f) : f;
        if (k <= 0.0001f) continue;
        for (int d = 0; d < stp->n_decls; d++) {
            const char *nm = stp->decls[d].name;
            const char *val = stp->decls[d].value;
            float num = (float)strtod(val, NULL);
            if (!strcmp(nm, "opacity")) st->opacity += (num - st->opacity) * k;
            else if (!strcmp(nm, "translate-x") || !strcmp(nm, "translate")) st->translate_x += (num - st->translate_x) * k;
            else if (!strcmp(nm, "translate-y")) st->translate_y += (num - st->translate_y) * k;
            else if (!strcmp(nm, "scale")) st->scale = 1.0f + (num - 1.0f) * k;
            else if (!strcmp(nm, "rotate") || !strcmp(nm, "rotate-z")) st->rotate = num * k;
            else if (!strcmp(nm, "rotate-x")) st->rotate_x = num * k;
            else if (!strcmp(nm, "rotate-y")) st->rotate_y = num * k;
            else if (!strcmp(nm, "background") || !strcmp(nm, "background-color")) {
                hn_color col = 0;
                if (hn_color_parse(val, strlen(val), &col)) {
                    float c4[4], cur4[4];
                    rgba_of(col, c4); rgba_of(st->background, cur4);
                    for (int q = 0; q < 4; q++) cur4[q] += (c4[q] - cur4[q]) * k;
                    st->background = color_of(cur4);
                    st->has_gradient = 0;
                }
            } else if (!strcmp(nm, "color")) {
                hn_color col = 0;
                if (hn_color_parse(val, strlen(val), &col)) {
                    float c4[4], cur4[4];
                    rgba_of(col, c4); rgba_of(st->color, cur4);
                    for (int q = 0; q < 4; q++) cur4[q] += (c4[q] - cur4[q]) * k;
                    st->color = color_of(cur4);
                }
            }
        }
    }
}

/* 推进 @keyframes 动画: 返回 1 表示仍在播放 */
static int kf_tick(hn_node *n, const hn_keyframes *kf, float ms, int iter, int dir,
                   int /* fill */, float *clock_io, int *done_io, hn_style *st) {
    if (!kf || ms <= 0) return 0;
    float dur = ms;
    *clock_io += 0;               /* 时钟由调用方按 dt 累加 */
    float total = *clock_io / dur;          /* 已播放周期数(可为小数) */
    float t;
    if (iter < 0) {                          /* 无限循环 */
        t = total - floorf(total);
    } else {
        if (total >= (float)iter) { *done_io = 1; t = 1.0f; }
        else t = total - floorf(total);
    }
    /* 方向: alternate 时偶数轮正向、奇数轮反向 */
    int cyc = (int)floorf(total);
    int rev = 0;
    if (dir == 1) rev = 1;
    else if (dir == 2) rev = (cyc & 1);
    else if (dir == 3) rev = !(cyc & 1);
    if (rev) t = 1.0f - t;
    kf_sample(kf, t, st);
    return iter < 0 || !*done_io;
}

/* 关键帧动画的时钟推进(每帧调用; dt 由 anim_tick 传入) */
static float g_last_dt = 0;

/* ---------- 缓动 ---------- */

/* 三次贝塞尔求值: 先由 x 反解参数 t(牛顿迭代), 再算 y —— 与 CSS 同法 */
static float bezier_axis(float t, float a1, float a2) {
    float c = 3.0f * a1, b = 3.0f * (a2 - a1) - c, a = 1.0f - c - b;
    return ((a * t + b) * t + c) * t;
}
static float bezier_solve(float x, float x1, float x2) {
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    float t = x;                       /* 初值 */
    for (int i = 0; i < 8; i++) {      /* 牛顿迭代: 收敛快且确定性(无随机) */
        float fx = bezier_axis(t, x1, x2) - x;
        if (fabsf(fx) < 1e-5f) break;
        float c = 3.0f * x1, b = 3.0f * (x2 - x1) - c, a = 1.0f - c - b;
        float d = (3.0f * a * t + 2.0f * b) * t + c;
        if (fabsf(d) < 1e-6f) break;
        t -= fx / d;
        if (t < 0) t = 0; else if (t > 1) t = 1;
    }
    return t;
}
static float ease_apply(int ease, const float cb[4], float t0) {
    if (t0 <= 0) return 0;
    if (t0 >= 1) return 1;
    switch (ease) {
    case HN_EASE_LINEAR: return t0;
    case HN_EASE_IN:     return t0 * t0;
    case HN_EASE_OUT:    return 1.0f - (1.0f - t0) * (1.0f - t0);
    case HN_EASE_IN_OUT: return t0 < 0.5f ? 2.0f * t0 * t0
                                           : 1.0f - 2.0f * (1.0f - t0) * (1.0f - t0);
    case HN_EASE_CSS:    return bezier_axis(bezier_solve(t0, 0.25f, 0.25f), 0.1f, 1.0f);
    case HN_EASE_CUBIC:  return bezier_axis(bezier_solve(t0, cb[0], cb[2]), cb[1], cb[3]);
    default:             return t0 * t0 * (3.0f - 2.0f * t0);   /* smoothstep */
    }
}

/* 入场动画的起始状态: 返回 from 值(位移/缩放/透明度) */
static void enter_from(int preset, float *tx, float *ty, float *sc, float *op) {
    *tx = 0; *ty = 0; *sc = 1.0f; *op = 1.0f;
    switch (preset) {
    case HN_ENTER_UP:    *ty = 10.0f; *op = 0.0f; break;
    case HN_ENTER_DOWN:  *ty = -10.0f; *op = 0.0f; break;
    case HN_ENTER_LEFT:  *tx = -12.0f; *op = 0.0f; break;
    case HN_ENTER_FADE:  *op = 0.0f; break;
    case HN_ENTER_SCALE: *sc = 0.94f; *op = 0.0f; break;
    default: break;
    }
}

static int anim_walk(hn_context *c, hn_node *n, float dt_ms) {
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

        /* 外部资源动画时钟(Lottie / 网格变形): 与 CSS 动画共用帧循环, 但独立累加。
           CSS 动画有"播完"概念, Lottie 需要持续时钟(自身循环取模)。
           有声明就把节点标记为活跃, 帧驱动才会持续调用 repaint。 */
        if (dt_ms > 0 &&
            (hn_node_attr(n, "hn-lottie") || hn_node_attr(n, "hn-mesh") || n->mesh_verts)) {
            n->ext_clock += dt_ms;
            active = 1;
        }

        /* @keyframes 动画: 声明了 animation-name 且能在样式表里找到定义。
           每帧按时间轴采样, 直接写入样式字段(几何由绘制阶段消费)。
           与入场预设互斥: 有关键帧动画就不再播入场。 */
        if (st->kf_name && st->kf_ms > 0) {
            if (!a->kf) {
                a->kf = find_kf(c, st->kf_name);
                a->kf_clock = 0;
                a->kf_done = 0;
            }
            if (a->kf && !a->kf_done) {
                if (dt_ms > 0) a->kf_clock += dt_ms;
                int still = kf_tick(n, a->kf, st->kf_ms, st->kf_iter, st->kf_dir,
                                    st->kf_fill, &a->kf_clock, &a->kf_done, st);
                st->opacity = st->opacity;   /* kf_sample 已就地写入 */
                a->o = st->opacity;
                a->tx = st->translate_x; a->ty = st->translate_y;
                a->sc = st->scale;       a->rot = st->rotate;
                a->dirty_written = 1;
                if (still) active = 1;
                goto children;
            }
        }

        /* 入场动画: 元素首次出现且声明了 animation 时, 从预设起始态过渡到目标态。
           用独立标志跟踪, 使其不被随后的样式重算打断(每次 relayout 都会把
           st->opacity 等重置为级联结果, 若无独立状态入场会瞬间跳到终态)。
           这让新插入的内容(消息/列表项/卡片)平滑浮现, 而不是硬闪出现。 */
        if (a->entering) {
            if (dt_ms > 0) a->t += dt_ms / a->ms;
            if (a->t >= 1.0f) { a->t = 1.0f; a->entering = 0; a->active = 0; }
            float e2 = ease_apply(a->ease, a->cb, a->t);
            a->o  = a->o_from  + (a->o_to  - a->o_from)  * e2;
            a->tx = a->tx_from + (a->tx_to - a->tx_from) * e2;
            a->ty = a->ty_from + (a->ty_to - a->ty_from) * e2;
            a->sc = a->sc_from + (a->sc_to - a->sc_from) * e2;
            memcpy(a->bg, a->bg_to, sizeof(a->bg));
            memcpy(a->fg, a->fg_to, sizeof(a->fg));
            st->background = color_of(a->bg);
            st->color = color_of(a->fg);
            st->opacity = a->o;
            st->translate_x = a->tx;
            st->translate_y = a->ty;
            st->scale = a->sc;
            a->dirty_written = 1;
            active = 1;
            goto children;
        }
        if (a->fresh && st->anim_enter != HN_ENTER_NONE && st->enter_ms > 0) {
            a->fresh = 0;
            a->inited = 1;
            a->entering = 1;
            float fx, fy, fsc, fop;
            enter_from(st->anim_enter, &fx, &fy, &fsc, &fop);
            a->o = fop;  a->o_from = fop;  a->o_to = st->opacity;
            a->tx = fx;  a->tx_from = fx;  a->tx_to = st->translate_x;
            a->ty = fy;  a->ty_from = fy;  a->ty_to = st->translate_y;
            a->sc = fsc; a->sc_from = fsc; a->sc_to = st->scale;
            a->ms = st->enter_ms;
            a->ease = st->anim_ease;
            memcpy(a->cb, st->cb, sizeof(a->cb));
            rgba_of(st->background, a->bg);
            rgba_of(st->color, a->fg);
            memcpy(a->bg_from, a->bg, sizeof(a->bg));
            memcpy(a->fg_from, a->fg, sizeof(a->fg));
            memcpy(a->bg_to, a->bg, sizeof(a->bg));
            memcpy(a->fg_to, a->fg, sizeof(a->fg));
            a->t = 0;
            a->active = 1;
            a->dirty_written = 0;
            goto children;
        }
        if (a->fresh) a->fresh = 0;   /* 无入场声明: 直接以终态出现 */

        if (!a->inited) {
            a->inited = 1;
            a->ms = st->transition_ms;
            a->tx = a->tx_from = a->tx_to = st->translate_x;
            a->ty = a->ty_from = a->ty_to = st->translate_y;
            a->sc = a->sc_from = a->sc_to = st->scale;
            a->ease = st->anim_ease;
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

            /* 几何目标: 与颜色同样要区分"样式写入的目标"和"上次写回的插值" */
            float tx_target, ty_target, sc_target, o_target;
            if (style_is_ours) {
                tx_target = a->tx_to; ty_target = a->ty_to;
                sc_target = a->sc_to; o_target = a->o_to;
            } else {
                tx_target = st->translate_x; ty_target = st->translate_y;
                sc_target = st->scale;       o_target = st->opacity;
            }

            int changed = 0;
            for (int i = 0; i < 4; i++) {
                if (fabsf(bg_target[i] - a->bg_to[i]) > 0.004f) { a->bg_to[i] = bg_target[i]; changed = 1; }
                if (fabsf(fg_target[i] - a->fg_to[i]) > 0.004f) { a->fg_to[i] = fg_target[i]; changed = 1; }
            }
            if (fabsf(tx_target - a->tx_to) > 0.02f) { a->tx_to = tx_target; changed = 1; }
            if (fabsf(ty_target - a->ty_to) > 0.02f) { a->ty_to = ty_target; changed = 1; }
            if (fabsf(sc_target - a->sc_to) > 0.0005f) { a->sc_to = sc_target; changed = 1; }
            if (fabsf(o_target - a->o_to) > 0.004f) { a->o_to = o_target; changed = 1; }
            if (st->transition_ms != a->ms) { a->ms = st->transition_ms; changed = 1; }
            if (st->anim_ease != a->ease) { a->ease = st->anim_ease; changed = 1; }
            memcpy(a->cb, st->cb, sizeof(a->cb));

            if (changed) {
                memcpy(a->bg_from, a->bg, sizeof(a->bg));
                memcpy(a->fg_from, a->fg, sizeof(a->fg));
                a->o_from = a->o;
                a->tx_from = a->tx;
                a->ty_from = a->ty;
                a->sc_from = a->sc;
                a->t = 0;
                a->active = (a->ms > 0);
            }

            if (a->active && a->ms > 0) {
                if (dt_ms > 0) a->t += dt_ms / a->ms;
                if (a->t >= 1.0f) { a->t = 1.0f; a->active = 0; }
                float e = ease_apply(a->ease, a->cb, a->t);
                lerp4(a->bg, a->bg_from, a->bg_to, e);
                lerp4(a->fg, a->fg_from, a->fg_to, e);
                a->o  = a->o_from  + (a->o_to  - a->o_from)  * e;
                a->tx = a->tx_from + (a->tx_to - a->tx_from) * e;
                a->ty = a->ty_from + (a->ty_to - a->ty_from) * e;
                a->sc = a->sc_from + (a->sc_to - a->sc_from) * e;
            } else {
                memcpy(a->bg, a->bg_to, sizeof(a->bg));
                memcpy(a->fg, a->fg_to, sizeof(a->fg));
                a->o = a->o_to;
                a->tx = a->tx_to; a->ty = a->ty_to; a->sc = a->sc_to;
            }

            st->background = color_of(a->bg);
            st->color = color_of(a->fg);
            st->opacity = a->o;
            /* 几何插值写回样式字段: 绘制阶段按此偏移/缩放(read-only 消费) */
            st->translate_x = a->tx;
            st->translate_y = a->ty;
            st->scale = a->sc;
            a->dirty_written = 1;
        }
        if (a->active) active = 1;
    }
children:
    for (hn_node *ch = n->first; ch; ch = ch->next)
        if (anim_walk(c, ch, dt_ms)) active = 1;
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

/* ---------------- 事件 ---------------- */

const char *hn_event_name(hn_event_kind k) {
    switch (k) {
    case HN_EV_CLICK:      return "click";
    case HN_EV_DBLCLICK:   return "dblclick";
    case HN_EV_MOUSEDOWN:  return "mousedown";
    case HN_EV_MOUSEUP:    return "mouseup";
    case HN_EV_MOUSEMOVE:  return "mousemove";
    case HN_EV_MOUSEENTER: return "mouseenter";
    case HN_EV_MOUSELEAVE: return "mouseleave";
    case HN_EV_KEYDOWN:    return "keydown";
    case HN_EV_KEYUP:      return "keyup";
    case HN_EV_FOCUS:      return "focus";
    case HN_EV_BLUR:       return "blur";
    case HN_EV_INPUT:      return "input";
    case HN_EV_CHANGE:     return "change";
    case HN_EV_SUBMIT:     return "submit";
    case HN_EV_SCROLL:     return "scroll";
    case HN_EV_HOVER:      return "hover";
    default:               return "unknown";
    }
}

/* 冒泡路径只含"有 id 的元素": 运行时与 JS 都以 id 寻址。
   这样路径长度小(通常 2-4 层), 派发成本低。 */
hn_node *hn_event_path_at(hn_node *target, int idx) {
    if (!target || idx < 0) return NULL;
    int seen = 0;
    for (hn_node *n = target; n; n = n->parent) {
        if (n->kind != HN_ELEM) continue;
        if (!hn_node_attr(n, "id")) continue;
        if (seen == idx) return n;
        seen++;
    }
    return NULL;
}

int hn_event_path_len(hn_node *target) {
    if (!target) return 0;
    int seen = 0;
    for (hn_node *n = target; n; n = n->parent)
        if (n->kind == HN_ELEM && hn_node_attr(n, "id")) seen++;
    return seen;
}

hn_node *hn_node_ancestor_input(hn_node *n) {
    for (; n; n = n->parent)
        if (hn_node_is_input(n)) return n;
    return NULL;
}

void hn_node_scroll_get(hn_node *n, float *x, float *y) {
    if (x) *x = n ? n->scroll_x : 0;
    if (y) *y = n ? n->scroll_y : 0;
}

void hn_node_scroll_range(hn_node *n, float *max_x, float *max_y) {
    float mx = 0, my = 0;
    if (n && n->kind == HN_ELEM && n->style.overflow) {
        my = n->content_h - n->bh;
        if (my < 0) my = 0;
    }
    if (max_x) *max_x = mx;
    if (max_y) *max_y = my;
}

/* 可增长的遍历栈 —— 固定数组在 DOM 变大后会静默丢节点(遍历不完整),
   表现为"某些元素查不到/流式容器丢失"。这里按需扩容, 不设上限。 */
typedef struct { hn_node **v; int n, cap; } nstack;

static void ns_init(nstack *st, int cap) {
    st->v = malloc(sizeof(hn_node *) * (size_t)cap);
    st->n = 0; st->cap = st->v ? cap : 0;
}
static void ns_push(nstack *st, hn_node *n) {
    if (st->n == st->cap) {
        int nc = st->cap ? st->cap * 2 : 512;
        hn_node **nv = realloc(st->v, sizeof(hn_node *) * (size_t)nc);
        if (!nv) return;              /* 内存耗尽: 放弃该节点(不崩溃) */
        st->v = nv; st->cap = nc;
    }
    st->v[st->n++] = n;
}
static int  ns_pop(nstack *st, hn_node **out) {
    if (st->n == 0) return 0;
    *out = st->v[--st->n];
    return 1;
}
static void ns_free(nstack *st) { free(st->v); st->v = NULL; st->n = st->cap = 0; }

hn_node *hn_doc_find_attr(hn_doc *doc, const char *attr, int idx) {
    if (!doc || !doc->root || !attr) return NULL;
    nstack st;
    ns_init(&st, 512);
    if (!st.v) return NULL;
    ns_push(&st, doc->root);
    hn_node *found = NULL;
    hn_node *n;
    int seen = 0;
    while (ns_pop(&st, &n)) {
        if (n->kind == HN_ELEM && hn_node_attr(n, attr)) {
            if (seen == idx) { found = n; break; }
            seen++;
        }
        for (hn_node *ch = n->last; ch; ch = ch->prev) ns_push(&st, ch);
    }
    ns_free(&st);
    return found;
}

/* 环形缓冲: 流式视图(日志/对话/agent 轨迹)只需保留最近 N 条。
   移除的是最旧的若干个元素子节点, 因此内容可以无限追加而内存与视觉都收敛。 */
int hn_node_trim_children(hn_node *n, int keep_last) {
    if (!n || keep_last <= 0) return 0;
    /* 数元素子节点 */
    int total = 0;
    for (hn_node *ch = n->first; ch; ch = ch->next)
        if (ch->kind == HN_ELEM) total++;
    int drop = total - keep_last;
    if (drop <= 0) return 0;

    int removed = 0;
    hn_node *ch = n->first;
    while (ch && removed < drop) {
        hn_node *nx = ch->next;
        if (ch->kind == HN_ELEM) {
            /* 摘链: 双向链表两侧都要接好 */
            if (ch->prev) ch->prev->next = ch->next; else n->first = ch->next;
            if (ch->next) ch->next->prev = ch->prev; else n->last = ch->prev;
            /* 关键: 被摘节点自身指针必须清空 —— 否则旧 next/prev 仍指向链上,
               任何沿旧指针的遍历(如 paint_walk)都可能绕回已摘节点形成环 → 栈溢出。 */
            ch->next = NULL;
            ch->prev = NULL;
            ch->parent = NULL;
            removed++;
        }
        ch = nx;
    }
    /* 一致性兜底: 链表首尾若仍指向已摘节点则修正 */
    if (n->first && n->first->prev) n->first->prev = NULL;
    if (n->last && n->last->next) n->last->next = NULL;
    return removed;
}

/* 结构自检: 显式 visited 表 + 预算上限, 任何环都会被检出而不是死循环 */
int hn_doc_validate(hn_doc *doc, int node_budget) {
    if (!doc || !doc->root) return 1;
    if (node_budget <= 0) node_budget = 100000;
    int issues = 0, visited = 0;
    nstack st;
    ns_init(&st, 512);
    if (!st.v) return 1;
    ns_push(&st, doc->root);
    hn_node *n;
    while (ns_pop(&st, &n)) {
        if (++visited > node_budget) { issues++; break; }   /* 超预算: 疑似环 */
        /* 自引用 */
        if (n->next == n || n->prev == n || n->first == n || n->last == n) issues++;
        /* 父子一致性 */
        for (hn_node *ch = n->first; ch; ch = ch->next) {
            if (ch->parent != n) issues++;
            if (ch == n) { issues++; break; }               /* 自己是自己的子节点 */
            ns_push(&st, ch);
        }
        /* 双向链一致性: first->prev 应为 NULL, last->next 应为 NULL */
        if (n->first && n->first->prev) issues++;
        if (n->last && n->last->next) issues++;
    }
    ns_free(&st);
    return issues;
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

int hn_node_attr_at(hn_node *n, int idx, const char **name, const char **value) {
    if (!n || n->kind != HN_ELEM || idx < 0 || idx >= n->n_attrs) return 0;
    if (name) *name = n->attrs[idx].name;
    if (value) *value = n->attrs[idx].value;
    return 1;
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
    static int counter = 0;   /* 单调递增: 新分配不与既有 id 冲突, 避免重复命名 */
    int assigned = 0;
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
            /* 若该 id 已被占用(热更新残留), 继续递增 */
            while (find_by_id_rec(doc->root, buf)) {
                snprintf(buf, sizeof(buf), "hx-auto-%d", ++counter);
            }
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
    if (!doc || !doc->root) return 0;
    nstack st;
    ns_init(&st, 256);
    if (!st.v) return 0;
    ns_push(&st, doc->root);
    hn_node *n;
    int found = 0;
    while (ns_pop(&st, &n)) {
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
                    ns_free(&st);
                    return 1;
                }
                found++;
            }
        }
        /* 子节点逆序入栈保持文档序 */
        for (hn_node *ch = n->last; ch; ch = ch->prev) ns_push(&st, ch);
    }
    ns_free(&st);
    return 0;
}

/* ---------------- 应用清单 ---------------- */

void hn_doc_manifest(const hn_doc *doc, hn_manifest *out) {
    out->surface = HN_SURFACE_WINDOW;
    out->w = out->h = 0;
    out->x = out->y = 0;
    out->title = NULL;
    out->theme = NULL;
    out->transparent = 0;
    out->shadow = 1;        /* 默认带投影 */
    out->draggable = 1;     /* 默认可拖动 */
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
                } else if (!strcmp(name, "hn-transparent")) {
                    out->transparent = strcmp(content, "false") && strcmp(content, "0");
                } else if (!strcmp(name, "hn-shadow")) {
                    out->shadow = strcmp(content, "false") && strcmp(content, "0");
                } else if (!strcmp(name, "hn-draggable")) {
                    out->draggable = strcmp(content, "false") && strcmp(content, "0");
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
    nstack st;
    ns_init(&st, 512);
    if (!st.v) return 0;
    ns_push(&st, doc->root);
    hn_node *n;
    int seen = 0;
    while (ns_pop(&st, &n)) {
        if (n->kind == HN_ELEM) {
            const char *trig = hn_node_attr(n, "hx-trigger");
            const char *id = hn_node_attr(n, "id");
            int ms = parse_every(trig);
            if (ms > 0 && id) {
                if (seen == idx) {
                    if (out_id) *out_id = id;
                    if (out_ms) *out_ms = ms;
                    ns_free(&st);
                    return 1;
                }
                seen++;
            }
        }
        for (hn_node *ch = n->last; ch; ch = ch->prev) ns_push(&st, ch);
    }
    ns_free(&st);
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
    /* 切断片段首尾的外向指针: 片段是被挂接的独立链, 若尾节点的 next 还指着
       别的节点, 挂到目标链上会形成环 → 遍历(paint_walk)无限递归栈溢出。 */
    ff->prev = NULL;
    fl->next = NULL;
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

/* arena 压缩: 全量重渲染时回收旧 DOM 内存。
 * 调用方须持有完整的 HTML(将重新解析)。用于流式应用的长期内存控制。 */
void hn_context_compact(hn_context *c, const char *html, size_t len) {
    if (!c || !c->doc) return;
    hn_context_lottie_clear(c);          /* 旧文档的路径键已随 arena 释放 */
    hn_arena_destroy(c->doc->arena);
    hn_doc_free(c->doc);
    c->doc = hn_parse_html(html, len);
    if (c->doc) {
        hn_context_set_doc(c, c->doc);
        hn_context_apply_theme(c);
    }
}

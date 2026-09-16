/* hn_style.c — 样式计算: 匹配 → 级联 → 继承 → computed style
 * 级联顺序: UA 样式表 < 文档内联 <style> < 运行时追加样式表 < 元素 style 属性,
 * 同优先级内按 (specificity, 表序, 规则序) 排序, 后者优先。
 * font-size 先于其它属性应用(处理 em 单位依赖)。
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "hn_internal.h"
#include "hn_port.h"

static const char *UA_CSS =
    "head, title, base, link, meta, style, script, noscript { display: none; }\n"
    "b, strong, i, em, span, a, code, small, u, s, sub, sup, label, br, em { display: inline; }\n"
    "input, button, textarea, img, select { display: inline-block; }\n"
    "b, strong { font-weight: 700; } i, em { font-style: italic; }\n"
    "h1 { font-size: 32; font-weight: 700; margin: 21 0 7; }\n"
    "h2 { font-size: 24; font-weight: 700; margin: 20 0 6; }\n"
    "h3 { font-size: 20; font-weight: 700; margin: 18 0 5; }\n"
    "h4, h5, h6 { font-size: 17; font-weight: 700; margin: 16 0 4; }\n"
    "p { margin: 0 0 12; }\n"
    "body { margin: 8; }\n"
    "button { text-align: center; }\n"
    "ul, ol { margin: 8 0; padding: 4 0 4 26; }\n"
    "li { display: block; margin: 2 0; }\n"
    "a { color: #0969da; text-decoration: underline; }\n"
    "u { text-decoration: underline; }\n"
    "s, strike, del { text-decoration: line-through; }\n"
    "hr { display: block; border: 1 solid #c8c8c8; height: 0; margin: 10 0; }\n"
    "code, pre, kbd, samp, tt { font-family: monospace; }\n";

const char *hn_ua_css(void) { return UA_CSS; }

void hn_style_default(hn_style *st) {
    memset(st, 0, sizeof(*st));
    st->min_w = st->max_w = st->min_h = st->max_h = -1;
    st->display = HN_DISP_BLOCK;
    st->flex_row = 1;
    st->justify = HN_JUST_START;
    st->align = HN_ALIGN_STRETCH;
    st->color = 0x000000FF;
    st->font_size = 16;
    st->font_weight = 400;
    st->line_height = 1.45f;
    st->opacity = 1;
    st->width_u = HN_U_AUTO;
    st->height_u = HN_U_AUTO;
    st->flex_basis = -1;
    st->flex_basis_u = HN_U_AUTO;
    st->flex_shrink = 1;
    st->scale = 1.0f;
}

void hn_style_inherit(hn_style *dst, const hn_style *p) {
    dst->color = p->color;
    dst->list_style = p->list_style;
    dst->font_family = p->font_family;
    dst->font_size = p->font_size;
    dst->font_weight = p->font_weight;
    dst->font_italic = p->font_italic;
    dst->letter_spacing = p->letter_spacing;
    dst->text_align = p->text_align;
    dst->line_height = p->line_height;
    dst->opacity = p->opacity;
}

/* ---------- 值解析工具 ---------- */

typedef struct { const char *s; size_t n; } sv;

/* 游标式 token 迭代: 从 v 中切出下一个非空白 token 到 out, v 前进 */
static int next_tok(sv *v, sv *out) {
    while (v->n && (*v->s == ' ' || *v->s == '\t')) { v->s++; v->n--; }
    if (!v->n) return 0;
    out->s = v->s;
    while (v->n && *v->s != ' ' && *v->s != '\t') { v->s++; v->n--; }
    out->n = (size_t)(v->s - out->s);
    return 1;
}

static int sv_eq(sv t, const char *lit) {
    return t.n == strlen(lit) && !strncmp(t.s, lit, t.n);
}

static int sv_num(sv t, float *out) {
    char buf[40];
    if (t.n == 0 || t.n >= sizeof(buf)) return 0;
    memcpy(buf, t.s, t.n);
    buf[t.n] = 0;
    char *e = NULL;
    float v = strtof(buf, &e);
    if (e == buf) return 0;
    *out = v;
    return 1;
}

/* 视口/根字号(每次 compute 全量刷新; vw/vh/rem 即时解析为 px) */
static float g_vw = 0, g_vh = 0, g_root_font = 16;

/* 数值 + 可选单位后缀; 无后缀视为 px */
static int sv_len(sv t, float font_px, float *out, int *unit) {
    char buf[40];
    if (t.n == 0 || t.n >= sizeof(buf)) return 0;
    memcpy(buf, t.s, t.n);
    buf[t.n] = 0;
    char *e = NULL;
    float v = strtof(buf, &e);
    if (e == buf) return 0;
    int u = HN_U_PX;
    if (!strncmp(e, "px", 2) || !strncmp(e, "pt", 2)) { if (e[1] == 't') v = v * 96.0f / 72.0f; }
    else if (!strncmp(e, "vw", 2)) v = v * g_vw / 100.0f;
    else if (!strncmp(e, "vh", 2)) v = v * g_vh / 100.0f;
    else if (!strncmp(e, "rem", 3)) v = v * g_root_font;
    else if (*e == '%') u = HN_U_PCT;
    else if (!strncmp(e, "em", 2)) u = HN_U_EM;
    *out = v;
    *unit = u;
    return 1;
}

static int sv_color(sv t, hn_color *out) {
    char buf[64];
    if (t.n == 0 || t.n >= sizeof(buf)) return 0;
    memcpy(buf, t.s, t.n);
    buf[t.n] = 0;
    return hn_color_parse(buf, strlen(buf), out);
}

/* 整条声明值按一个颜色解析(hsl(120, 50%, 50%) 这类含空格的函数式) */
static int sv_color_full(sv t, hn_color *out) { return sv_color(t, out); }

/* ---------- 声明应用 ---------- */

/* 缓动名/cubic-bezier() → 枚举 + 参数(与 CSS 命名对齐) */
static unsigned char parse_ease(sv t, float cb[4]) {
    char buf[64];
    if (t.n == 0 || t.n >= sizeof(buf)) return HN_EASE_SMOOTH;
    memcpy(buf, t.s, t.n); buf[t.n] = 0;
    if (!strcmp(buf, "linear")) return HN_EASE_LINEAR;
    if (!strcmp(buf, "ease")) return HN_EASE_CSS;
    if (!strcmp(buf, "ease-in")) return HN_EASE_IN;
    if (!strcmp(buf, "ease-out")) return HN_EASE_OUT;
    if (!strcmp(buf, "ease-in-out")) return HN_EASE_IN_OUT;
    if (!strncmp(buf, "cubic-bezier(", 13)) {
        const char *p = buf + 13;
        for (int i = 0; i < 4; i++) {
            while (*p == ' ' || *p == ',') p++;
            cb[i] = (float)strtod(p, (char **)&p);
        }
        return HN_EASE_CUBIC;
    }
    return HN_EASE_SMOOTH;
}

/* 上一次 apply_len4 的 auto 位图(TRBL), 仅 margin 消费 */
static unsigned char g_len4_auto;

static void apply_len4(sv v, float font_px, float out[4]) {
    float vals[4];
    int units[4], autos[4], n = 0;
    sv tok;
    while (n < 4 && next_tok(&v, &tok)) {
        autos[n] = sv_eq(tok, "auto");
        if (autos[n]) { vals[n] = 0; units[n] = HN_U_PX; }
        else if (!sv_len(tok, font_px, &vals[n], &units[n])) return;
        n++;
    }
    if (!n) return;
    /* TRBL 展开 */
    if (n == 1) {
        for (int i = 0; i < 4; i++) { out[i] = vals[0]; autos[i] = autos[0]; }
    } else if (n == 2) {
        out[0] = out[2] = vals[0]; out[1] = out[3] = vals[1];
        autos[0] = autos[2] = autos[0]; autos[1] = autos[3] = autos[1];
    } else if (n == 3) {
        out[0] = vals[0]; out[1] = out[3] = vals[1]; out[2] = vals[2];
        int a1 = autos[1]; autos[0] = autos[0]; autos[1] = autos[3] = a1; autos[2] = autos[2];
    } else {
        out[0] = vals[0]; out[1] = vals[1]; out[2] = vals[2]; out[3] = vals[3];
    }
    /* em 单位按当前字号折算 */
    for (int i = 0; i < 4; i++)
        if (units[i] == HN_U_EM) out[i] *= font_px;
    /* 调用方通过全局暂存读取 auto 位(margin 专用) */
    g_len4_auto = (autos[0] ? 1 : 0) | (autos[1] ? 2 : 0) | (autos[2] ? 4 : 0) | (autos[3] ? 8 : 0);
}

/* 大小写不敏感子串查找 */
static const char *find_ci_str(const char *hay, const char *needle) {
    size_t m = strlen(needle);
    size_t n = strlen(hay);
    if (n < m) return NULL;
    for (size_t i = 0; i + m <= n; i++)
        if (!strncasecmp(hay + i, needle, m)) return hay + i;
    return NULL;
}

/* 解析 linear-gradient(...) 内部: [angle,] color [, color...]
 * 成功返回 1 并填充 from/to/angle */
static int parse_gradient(const char *v, hn_style *st) {
    const char *lg = find_ci_str(v, "linear-gradient(");
    if (!lg) return 0;
    const char *p = lg + 16;
    const char *end = v + strlen(v);
    int depth = 1;
    const char *e = p;
    while (e < end && depth > 0) {
        if (*e == '(') depth++;
        else if (*e == ')') { depth--; if (!depth) break; }
        e++;
    }
    /* 顶层逗号分段 */
    hn_color cols[8];
    int ncol = 0;
    float angle = 180; /* 缺省 to bottom */
    depth = 0;
    const char *seg = p;
    char buf[96];
    for (const char *i = p; i <= e; i++) {
        if (i < e && *i == '(') depth++;
        else if (i < e && *i == ')') depth--;
        if ((i == e || (*i == ',' && depth == 0))) {
            size_t l = (size_t)(i - seg);
            if (l && l < sizeof(buf)) {
                memcpy(buf, seg, l);
                buf[l] = 0;
                char *t = buf;
                while (*t == ' ' || *t == '\t') t++;
                char *te = t + strlen(t);
                while (te > t && (te[-1] == ' ' || te[-1] == '\t')) te--;
                *te = 0;
                size_t tl = te - t;
                if (tl > 3 && !strncasecmp(t + tl - 3, "deg", 3)) {
                    t[tl - 3] = 0;
                    angle = strtof(t, NULL);
                } else {
                    /* 去掉色标后缀 " N%" */
                    char *sp = strchr(t, ' ');
                    if (sp) *sp = 0;
                    hn_color col;
                    if (hn_color_parse(t, strlen(t), &col) && ncol < 8) cols[ncol++] = col;
                }
            }
            seg = i + 1;
        }
    }
    if (ncol < 2) return 0;
    st->has_gradient = 1;
    st->grad_from = cols[0];
    st->grad_to = cols[ncol - 1];
    st->grad_angle = angle;
    return 1;
}

static void apply_decl(hn_style *st, const char *name, const char *value) {
    sv v = { value, strlen(value) };
    sv t;
    float f;
    hn_color col;

    if (!strcmp(name, "font-size")) {
        int u;
        if (sv_len(v, st->font_size, &f, &u)) {
            float base = st->font_size; /* 尚未覆盖时即父级字号 */
            if (u == HN_U_PX || u == HN_U_AUTO) st->font_size = f;
            else if (u == HN_U_EM) st->font_size = base * f;
            else if (u == HN_U_PCT) st->font_size = base * f / 100.0f;
            if (st->font_size < 1) st->font_size = 1;
        }
    } else if (!strcmp(name, "display")) {
        if (!next_tok(&v, &t)) return;
        if (sv_eq(t, "none")) st->display = HN_DISP_NONE;
        else if (sv_eq(t, "inline-flex")) { st->display = HN_DISP_FLEX; st->disp_inline = 1; }
        else if (sv_eq(t, "flex")) { st->display = HN_DISP_FLEX; st->disp_inline = 0; }
        else if (sv_eq(t, "inline")) st->display = HN_DISP_INLINE;
        else if (sv_eq(t, "inline-block")) st->display = HN_DISP_INLINE_BLOCK;
        else st->display = HN_DISP_BLOCK;
    } else if (!strcmp(name, "flex-direction")) {
        if (!next_tok(&v, &t)) return;
        st->flex_row = sv_eq(t, "column") || sv_eq(t, "column-reverse") ? 0 : 1;
    } else if (!strcmp(name, "justify-content")) {
        if (!next_tok(&v, &t)) return;
        if (sv_eq(t, "center")) st->justify = HN_JUST_CENTER;
        else if (sv_eq(t, "flex-end") || sv_eq(t, "end") || sv_eq(t, "right")) st->justify = HN_JUST_END;
        else if (sv_eq(t, "space-between")) st->justify = HN_JUST_BETWEEN;
        else st->justify = HN_JUST_START;
    } else if (!strcmp(name, "align-items") || !strcmp(name, "align-self")) {
        if (!next_tok(&v, &t)) return;
        if (sv_eq(t, "center")) st->align = HN_ALIGN_CENTER;
        else if (sv_eq(t, "flex-start") || sv_eq(t, "start")) st->align = HN_ALIGN_START;
        else if (sv_eq(t, "flex-end") || sv_eq(t, "end")) st->align = HN_ALIGN_END;
        else st->align = HN_ALIGN_STRETCH;
    } else if (!strcmp(name, "flex-grow")) {
        if (next_tok(&v, &t) && sv_num(t, &f) && f > 0) st->flex_grow = f;
    } else if (!strcmp(name, "flex-shrink")) {
        if (next_tok(&v, &t) && sv_num(t, &f) && f >= 0) st->flex_shrink = f;
    } else if (!strcmp(name, "flex")) {
        /* flex: <number> ⇒ grow=n, basis=0(等分); flex:none ⇒ 不伸缩 */
        if (next_tok(&v, &t)) {
            if (sv_eq(t, "none")) st->flex_grow = 0;
            else if (sv_num(t, &f)) {
                st->flex_grow = f > 0 ? f : 0;
                st->flex_basis = 0;
                st->flex_basis_u = HN_U_PX;
            }
        }
    } else if (!strcmp(name, "flex-basis")) {
        int u;
        if (next_tok(&v, &t) && sv_len(t, st->font_size, &f, &u)) {
            st->flex_basis = f;
            st->flex_basis_u = (unsigned char)u;
        }
    } else if (!strcmp(name, "gap") || !strcmp(name, "row-gap") || !strcmp(name, "column-gap")) {
        int u;
        if (next_tok(&v, &t) && sv_len(t, st->font_size, &f, &u)) st->gap = f;
    } else if (!strcmp(name, "width")) {
        int u;
        if (next_tok(&v, &t) && sv_len(t, st->font_size, &f, &u)) { st->width = f; st->width_u = (unsigned char)u; }
    } else if (!strcmp(name, "height")) {
        int u;
        if (next_tok(&v, &t) && sv_len(t, st->font_size, &f, &u) && st->height_u == HN_U_AUTO) {
            st->height = f;
            st->height_u = (unsigned char)u;
        }
    } else if (!strcmp(name, "margin") || !strncmp(name, "margin-", 7)) {
        if (strlen(name) == 6) {
            apply_len4(v, st->font_size, st->margin);
            st->margin_auto = g_len4_auto;
        }
        else {
            int idx = name[7] == 't' ? 0 : name[7] == 'r' ? 1 : name[7] == 'b' ? 2 : 3;
            int u;
            sv tok0;
            if (next_tok(&v, &tok0) && sv_eq(tok0, "auto")) {
                st->margin[idx] = 0;
                st->margin_auto |= (unsigned char)(1 << idx);
            } else if (next_tok(&v, &t) && sv_len(t, st->font_size, &f, &u))
                st->margin[idx] = (u == HN_U_EM) ? f * st->font_size : f;
        }
    } else if (!strcmp(name, "padding") || !strncmp(name, "padding-", 8)) {
        if (strlen(name) == 7) apply_len4(v, st->font_size, st->padding);
        else {
            int idx = name[8] == 't' ? 0 : name[8] == 'r' ? 1 : name[8] == 'b' ? 2 : 3;
            int u;
            if (next_tok(&v, &t) && sv_len(t, st->font_size, &f, &u)) st->padding[idx] = (u == HN_U_EM) ? f * st->font_size : f;
        }
    } else if (!strcmp(name, "border")) {
        /* border: <width> [solid|...] [color] */
        while (next_tok(&v, &t)) {
            int u;
            float w;
            if (t.n >= 4 && !strncmp(t.s, "url(", 4)) continue;
            if (sv_len(t, st->font_size, &w, &u)) st->border_w = (u == HN_U_EM) ? w * st->font_size : w;
            else if (sv_color(t, &col)) st->border_color = col;
        }
    } else if (!strcmp(name, "border-width")) {
        int u;
        if (next_tok(&v, &t) && sv_len(t, st->font_size, &f, &u)) st->border_w = (u == HN_U_EM) ? f * st->font_size : f;
    } else if (!strcmp(name, "border-color")) {
        sv whole = { value, strlen(value) };
        if (!sv_color_full(whole, &col)) { if (next_tok(&v, &t)) sv_color(t, &col); }
        st->border_color = col;
    } else if (!strcmp(name, "border-radius")) {
        int u;
        if (next_tok(&v, &t) && sv_len(t, st->font_size, &f, &u)) {
            st->radius = (u == HN_U_EM) ? f * st->font_size : f;
            st->radius_pct = (u == HN_U_PCT);
        }
    } else if (!strcmp(name, "background") || !strcmp(name, "background-color")) {
        /* linear-gradient(...) → 渐变; 否则跳过 url() 取颜色 */
        if (parse_gradient(value, st)) return;
        sv whole = { value, strlen(value) };
        if (!sv_color_full(whole, &col)) {
            while (next_tok(&v, &t)) {
                if (t.n >= 4 && !strncmp(t.s, "url(", 4)) continue;
                if (sv_color(t, &col)) break;
            }
        }
        st->background = col;
        st->has_gradient = 0;   /* 纯色声明(含内联 style)覆盖此前的渐变 */
    } else if (!strcmp(name, "box-shadow")) {
        /* box-shadow: [inset] ox oy blur [spread] color */
        float nums[4];
        int nn = 0;
        hn_color scol = 0x00000059;
        while (next_tok(&v, &t)) {
            if (sv_eq(t, "none")) { st->has_shadow = 0; return; }
            if (sv_eq(t, "inset")) continue;
            if (nn < 4 && sv_num(t, &f)) { nums[nn++] = f; continue; }
            if (sv_color(t, &scol)) break;
        }
        if (nn >= 2) {
            st->has_shadow = 1;
            st->sh_ox = nums[0];
            st->sh_oy = nums[1];
            st->sh_blur = nn >= 3 ? nums[2] : 8;
            st->sh_color = scol;
        }
    } else if (!strcmp(name, "overflow") || !strcmp(name, "overflow-y")) {
        if (!next_tok(&v, &t)) return;
        st->overflow = sv_eq(t, "visible") ? 0 : 1;
    } else if (!strcmp(name, "transition")) {
        /* 取第一个时间值作为时长(如 "0.2s" / "200ms"); 多属性简化为统一时长 */
        while (next_tok(&v, &t)) {
            char buf[24];
            if (t.n == 0 || t.n >= sizeof(buf)) continue;
            memcpy(buf, t.s, t.n);
            buf[t.n] = 0;
            double val = strtod(buf, NULL);
            if (strstr(buf, "ms")) st->transition_ms = (float)val;
            else if (strchr(buf, 's')) st->transition_ms = (float)(val * 1000.0);
            if (st->transition_ms > 0) break;
        }
    } else if (!strcmp(name, "translate")) {
        /* translate: x y  (现代 CSS 独立属性; 也接受 translateY(x) 函数式写法) */
        int nth = 0;   /* 按出现次序取分量, 不能按"值是否为 0"判断(0 12 会误判) */
        while (next_tok(&v, &t)) {
            char buf[48];
            if (t.n == 0 || t.n >= sizeof(buf)) continue;
            memcpy(buf, t.s, t.n); buf[t.n] = 0;
            if (!strncmp(buf, "translateY(", 11) || !strncmp(buf, "translatey(", 11)) {
                st->translate_y = (float)strtod(buf + 11, NULL);
            } else if (!strncmp(buf, "translateX(", 11) || !strncmp(buf, "translatex(", 11)) {
                st->translate_x = (float)strtod(buf + 11, NULL);
            } else {
                int u; float fx;
                if (sv_len(t, st->font_size, &fx, &u)) {
                    if (nth == 0) st->translate_x = fx; else st->translate_y = fx;
                    nth++;
                }
            }
        }
    } else if (!strcmp(name, "scale")) {
        int u; float fx;
        if (next_tok(&v, &t) && sv_len(t, st->font_size, &fx, &u)) st->scale = fx;
    } else if (!strcmp(name, "transition-timing-function")) {
        if (!next_tok(&v, &t)) return;
        st->anim_ease = parse_ease(t, st->cb);
    } else if (!strcmp(name, "animation") || !strcmp(name, "hn-anim")) {
        /* animation: <预设名> <时长> <缓动>  (预设: up/down/left/fade/scale) */
        while (next_tok(&v, &t)) {
            char buf[48];
            if (t.n == 0 || t.n >= sizeof(buf)) continue;
            memcpy(buf, t.s, t.n); buf[t.n] = 0;
            if (!strcmp(buf, "up")) st->anim_enter = HN_ENTER_UP;
            else if (!strcmp(buf, "down")) st->anim_enter = HN_ENTER_DOWN;
            else if (!strcmp(buf, "left")) st->anim_enter = HN_ENTER_LEFT;
            else if (!strcmp(buf, "fade")) st->anim_enter = HN_ENTER_FADE;
            else if (!strcmp(buf, "scale")) st->anim_enter = HN_ENTER_SCALE;
            else if (!strcmp(buf, "none")) st->anim_enter = HN_ENTER_NONE;
            else {
                double val = strtod(buf, NULL);
                if (val > 0) {
                    if (strstr(buf, "ms")) st->enter_ms = (float)val;
                    else if (strchr(buf, 's')) st->enter_ms = (float)(val * 1000.0);
                    else st->anim_ease = parse_ease(t, st->cb);
                }
            }
        }
        if (st->anim_enter != HN_ENTER_NONE && st->enter_ms <= 0) st->enter_ms = 260;
    } else if (!strcmp(name, "cursor")) {
        if (!next_tok(&v, &t)) return;
        st->cursor = sv_eq(t, "pointer") ? 1 : 0;
    } else if (!strcmp(name, "text-decoration")) {
        st->text_deco = 0;
        while (next_tok(&v, &t)) {
            if (sv_eq(t, "underline")) st->text_deco |= 1;
            else if (sv_eq(t, "line-through")) st->text_deco |= 2;
            else if (sv_eq(t, "overline")) st->text_deco |= 4;
            else if (sv_eq(t, "none")) { st->text_deco = 0; break; }
        }
    } else if (!strcmp(name, "font-family")) {
        st->font_family = 0;
        while (next_tok(&v, &t)) {
            if (t.n >= 4 && (!strncasecmp(t.s, "mono", 4) || !strncasecmp(t.s, "menlo", 5)
                             || !strncasecmp(t.s, "courier", 7) || !strncasecmp(t.s, "consolas", 8)))
                st->font_family = 1;
            else if (t.n >= 5 && (!strncasecmp(t.s, "serif", 5) || !strncasecmp(t.s, "georgia", 7)
                                  || !strncasecmp(t.s, "times", 5) || !strncasecmp(t.s, "songti", 6)))
                st->font_family = 2;   /* sans/system/-apple-system/helvetica/pingfang → 0 */
        }
    } else if (!strcmp(name, "min-width")) {
        int u; if (next_tok(&v, &t) && sv_len(t, st->font_size, &f, &u)) {
            if (u != HN_U_PCT) { st->min_w = f; st->min_w_u = HN_U_PX; }
            else { st->min_w = f; st->min_w_u = HN_U_PCT; }
        }
    } else if (!strcmp(name, "max-width")) {
        int u; if (next_tok(&v, &t) && sv_len(t, st->font_size, &f, &u)) {
            if (u != HN_U_PCT) { st->max_w = f; st->max_w_u = HN_U_PX; }
            else { st->max_w = f; st->max_w_u = HN_U_PCT; }
        }
    } else if (!strcmp(name, "min-height")) {
        int u; if (next_tok(&v, &t) && sv_len(t, st->font_size, &f, &u)) {
            if (u != HN_U_PCT) { st->min_h = f; st->min_h_u = HN_U_PX; }
            else { st->min_h = f; st->min_h_u = HN_U_PCT; }
        }
    } else if (!strcmp(name, "max-height")) {
        int u; if (next_tok(&v, &t) && sv_len(t, st->font_size, &f, &u)) {
            if (u != HN_U_PCT) { st->max_h = f; st->max_h_u = HN_U_PX; }
            else { st->max_h = f; st->max_h_u = HN_U_PCT; }
        }
    } else if (!strcmp(name, "list-style") || !strcmp(name, "list-style-type")) {
        if (!next_tok(&v, &t)) return;
        if (sv_eq(t, "none")) st->list_style = 1;
        else if (sv_eq(t, "square")) st->list_style = 2;
        else if (sv_eq(t, "circle")) st->list_style = 3;
        else st->list_style = 0;   /* disc / decimal 等按父级 ul/ol 自动 */
    } else if (!strcmp(name, "color")) {
        sv whole = { value, strlen(value) };
        if (!sv_color_full(whole, &col)) { if (next_tok(&v, &t)) sv_color(t, &col); }
        st->color = col;
    } else if (!strcmp(name, "font-weight")) {
        if (!next_tok(&v, &t)) return;
        if (sv_eq(t, "bold") || sv_eq(t, "bolder")) st->font_weight = 700;
        else if (sv_eq(t, "normal")) st->font_weight = 400;
        else if (sv_eq(t, "lighter")) st->font_weight = 300;
        else if (sv_num(t, &f)) st->font_weight = (int)f;
    } else if (!strcmp(name, "font-style")) {
        if (!next_tok(&v, &t)) return;
        st->font_italic = (sv_eq(t, "italic") || sv_eq(t, "oblique")) ? 1 : 0;
    } else if (!strcmp(name, "text-align")) {
        if (!next_tok(&v, &t)) return;
        if (sv_eq(t, "center") || sv_eq(t, "middle")) st->text_align = 1;
        else if (sv_eq(t, "right") || sv_eq(t, "end")) st->text_align = 2;
        else st->text_align = 0;
    } else if (!strcmp(name, "line-height")) {
        if (!next_tok(&v, &t)) return;
        if (sv_eq(t, "normal")) st->line_height = 1.45f;
        else if (sv_num(t, &f)) {
            /* 纯数字 = 倍数; 带 px = 像素值换算 */
            st->line_height = (t.n > 2 && !strncmp(t.s + t.n - 2, "px", 2))
                              ? f / (st->font_size > 0 ? st->font_size : 16) : f;
            if (st->line_height < 0.5f) st->line_height = 0.5f;
        }
    } else if (!strcmp(name, "letter-spacing")) {
        if (!next_tok(&v, &t)) return;
        if (sv_eq(t, "normal")) st->letter_spacing = 0;
        else { int u; if (sv_len(t, st->font_size, &f, &u)) st->letter_spacing = (u == HN_U_EM) ? f * st->font_size : f; }
    } else if (!strcmp(name, "box-sizing")) {
        if (!next_tok(&v, &t)) return;
        st->box_border = sv_eq(t, "border-box") ? 1 : 0;
    } else if (!strcmp(name, "opacity")) {
        if (next_tok(&v, &t) && sv_num(t, &f)) st->opacity = f < 0 ? 0 : (f > 1 ? 1 : f);
    }
    /* 其余属性(font-family/overflow/cursor/...) 有意忽略 */
}

/* ---------- 匹配 ---------- */

static const char *get_attr(hn_node *n, const char *name) {
    for (int i = 0; i < n->n_attrs; i++)
        if (!strcmp(n->attrs[i].name, name)) return n->attrs[i].value;
    return NULL;
}

static int has_class(hn_node *n, const char *cls) {
    const char *cv = get_attr(n, "class");
    if (!cv) return 0;
    size_t ln = strlen(cls);
    const char *p = cv;
    while (*p) {
        while (*p == ' ') p++;
        const char *st = p;
        while (*p && *p != ' ') p++;
        if ((size_t)(p - st) == ln && !strncmp(st, cls, ln)) return 1;
    }
    return 0;
}

static int compound_matches(const hn_compound *cp, hn_node *n, hn_context *c) {
    if (n->kind != HN_ELEM) return 0;
    if (cp->tag && (!n->tag || strcmp(cp->tag, n->tag))) return 0;
    if (cp->id && (!n->id || strcmp(cp->id, n->id))) return 0;
    for (int i = 0; i < cp->n_cls; i++)
        if (!has_class(n, cp->cls[i])) return 0;
    /* 伪类状态匹配 */
    if (cp->pseudo == 1 && (!c || n != c->hover_node)) return 0;
    if (cp->pseudo == 2 && (!c || n != c->active_node)) return 0;
    if (cp->pseudo == 3 && (!c || n != c->focus_node)) return 0;
    /* :nth-child / :first-child / :last-child — 父元素下按元素序(1 起)匹配 */
    if (cp->nth) {
        if (!n->parent) return 0;
        if (cp->nth == -3) {   /* first-child: 前面无元素兄弟 */
            for (hn_node *s = n->prev; s; s = s->prev)
                if (s->kind == HN_ELEM) return 0;
            return 1;
        }
        if (cp->nth == -4) {   /* last-child: 后面无元素兄弟 */
            for (hn_node *s = n->next; s; s = s->next)
                if (s->kind == HN_ELEM) return 0;
            return 1;
        }
        int idx = 0, found = 0;
        for (hn_node *s = n->parent->first; s; s = s->next) {
            if (s->kind != HN_ELEM) continue;
            idx++;
            if (s == n) { found = 1; break; }
        }
        if (!found) return 0;
        if (cp->nth == -1) { if ((idx & 1) == 0) return 0; }
        else if (cp->nth == -2) { if (idx & 1) return 0; }
        else if (idx != cp->nth) return 0;
    }
    return 1;
}

/* 最近的元素兄弟(方向: prev=-1 向前 / +1 向后), 无则 NULL */
static hn_node *elem_sibling(hn_node *n, int dir) {
    for (hn_node *s = dir < 0 ? n->prev : n->next; s; s = dir < 0 ? s->prev : s->next)
        if (s->kind == HN_ELEM) return s;
    return NULL;
}

static int selector_matches(const hn_selector *sel, hn_node *n, hn_context *c) {
    if (sel->n_parts == 0) return 0;
    if (!compound_matches(&sel->parts[sel->n_parts - 1], n, c)) return 0;
    hn_node *cur = n;
    for (int i = sel->n_parts - 2; i >= 0; i--) {
        int comb = sel->parts[i + 1].comb;   /* part[i+1] 左侧的组合器 */
        if (comb == 1) {                     /* '>' 子代: 仅直接父 */
            hn_node *p = cur->parent;
            if (!p || !compound_matches(&sel->parts[i], p, c)) return 0;
            cur = p;
        } else if (comb == 2) {              /* '+' 相邻: 紧邻前元素兄弟 */
            hn_node *s = elem_sibling(cur, -1);
            if (!s || !compound_matches(&sel->parts[i], s, c)) return 0;
            cur = s;
        } else if (comb == 3) {              /* '~' 通用: 任一前元素兄弟 */
            hn_node *s = elem_sibling(cur, -1), *hit = NULL;
            while (s && !hit) {
                if (compound_matches(&sel->parts[i], s, c)) hit = s;
                else s = elem_sibling(s, -1);
            }
            if (!hit) return 0;
            cur = hit;
        } else {                             /* 后代 */
            hn_node *anc = cur->parent;
            while (anc && !compound_matches(&sel->parts[i], anc, c)) anc = anc->parent;
            if (!anc) return 0;
            cur = anc;
        }
    }
    return 1;
}

/* ---------- 级联计算 ---------- */

typedef struct { hn_rule *r; int sheet; } match_t;

static int match_before(match_t a, match_t b) {
    if (a.r->spec != b.r->spec) return a.r->spec < b.r->spec;
    if (a.sheet != b.sheet) return a.sheet < b.sheet;
    return a.r->order < b.r->order;
}

/* ---------- CSS 自定义属性 ---------- */

static void var_push(hn_arena *ar, hn_var **vars, int *n, const char *name, const char *value) {
    /* 同名覆盖(后者胜) */
    for (int i = 0; i < *n; i++)
        if (!strcmp((*vars)[i].name, name)) { (*vars)[i].value = value; return; }
    hn_var *nv = hn_arena_alloc(ar, sizeof(hn_var) * (size_t)(*n + 1));
    if (*n) memcpy(nv, *vars, sizeof(hn_var) * (size_t)*n);
    *vars = nv;
    nv[*n].name = name;
    nv[*n].value = value;
    (*n)++;
}

/* 代换 value 中的 var(--x[, fallback]); depth 限制递归 */
static char *resolve_vars(hn_arena *ar, const char *value, hn_var *vars, int n_vars, int depth) {
    if (depth > 4 || !strstr(value, "var(")) return (char *)value;
    size_t len = strlen(value);
    char *out = hn_arena_alloc(ar, len * 3 + 8);
    size_t o = 0;
    const char *p = value;
    while (*p) {
        const char *hit = strstr(p, "var(");
        if (!hit) { memcpy(out + o, p, strlen(p)); o += strlen(p); break; }
        memcpy(out + o, p, (size_t)(hit - p)); o += (size_t)(hit - p);
        const char *q = hit + 4;
        const char *name = q;
        int nd = 0;
        const char *end = NULL;
        int inner = 0;
        while (*q) {
            if (*q == '(') inner++;
            else if (*q == ')') { if (!inner) { end = q; break; } inner--; }
            else if (*q == ',' && !inner && !nd) nd = 1;
            q++;
        }
        if (!end) { out[o++] = 'v'; out[o++] = 'a'; out[o++] = 'r'; out[o++] = '('; p = hit + 4; continue; }
        size_t name_len = 0;
        const char *sep = NULL;
        for (const char *i = name; i < end; i++)
            if (*i == ',' && !sep) { sep = i; break; }
        name_len = sep ? (size_t)(sep - name) : (size_t)(end - name);
        char nb[96];
        if (name_len == 0 || name_len >= sizeof(nb)) { p = end + 1; continue; }
        memcpy(nb, name, name_len); nb[name_len] = 0;

        const char *sub = NULL;
        for (int i = n_vars - 1; i >= 0; i--)
            if (!strcmp(vars[i].name, nb)) { sub = vars[i].value; break; }
        if (!sub && sep) {
            /* 回退值: sep+1 .. end */
            size_t fl = (size_t)(end - sep - 1);
            char *fb = hn_arena_alloc(ar, fl + 1);
            memcpy(fb, sep + 1, fl); fb[fl] = 0;
            char *t = fb;
            while (*t == ' ') t++;
            char *te = t + strlen(t);
            while (te > t && te[-1] == ' ') te--;
            *te = 0;
            sub = t;
        }
        if (sub) {
            char *rs = resolve_vars(ar, sub, vars, n_vars, depth + 1);
            size_t rl = strlen(rs);
            memcpy(out + o, rs, rl); o += rl;
        }
        p = end + 1;
    }
    out[o] = 0;
    return out;
}

static void apply_decls(hn_style *st, hn_decl *ds, int nd, int phase) {
    for (int i = 0; i < nd; i++) {
        int is_fs = !strcmp(ds[i].name, "font-size");
        if ((phase == 0) != is_fs) continue;
        apply_decl(st, ds[i].name, ds[i].value);
    }
}

static void compute_rec(hn_context *c, hn_node *n, hn_node *parent) {
    if (n->kind != HN_ELEM) return;

    /* id 缓存 */
    n->id = get_attr(n, "id");

    hn_style st;
    hn_style_default(&st);
    if (parent) hn_style_inherit(&st, &parent->style);

    /* 收集匹配规则: 顺序 = context sheets(UA 在前) + 文档内联 <style> */
    match_t ms[512];
    int nm = 0;
    hn_sheet *sheets[64];
    int nsheets = 0;
    for (int i = 0; i < c->n_sheets && nsheets < 64; i++) sheets[nsheets++] = c->sheets[i];
    if (c->doc)
        for (int i = 0; i < c->doc->n_inline && nsheets < 64; i++) sheets[nsheets++] = c->doc->inline_sheets[i];

    for (int si = 0; si < nsheets; si++) {
        hn_sheet *sh = sheets[si];
        for (int ri = 0; ri < sh->n_rules; ri++) {
            hn_rule *r = &sh->rules[ri];
            if (r->media_min_w > 0 && c->vw < r->media_min_w) continue;
            if (r->media_max_w > 0 && c->vw > r->media_max_w) continue;
            if (!selector_matches(&r->sel, n, c)) continue;
            match_t m = { r, si };
            int idx = nm;
            while (idx > 0 && match_before(m, ms[idx - 1])) { ms[idx] = ms[idx - 1]; idx--; }
            if (nm < 512) ms[idx] = m, nm++; else ms[idx] = m;
        }
    }

    /* 变量表: 继承父级, 叠加匹配规则与行内 style 里的 --decls */
    hn_var *vars = NULL;
    int nvars = 0;
    if (parent && parent->style.n_vars && c->tmp) {
        vars = hn_arena_alloc(c->tmp, sizeof(hn_var) * (size_t)parent->style.n_vars);
        memcpy(vars, parent->style.vars, sizeof(hn_var) * (size_t)parent->style.n_vars);
        nvars = parent->style.n_vars;
    }
    if (c->tmp) {
        for (int i = 0; i < nm; i++)
            for (int d = 0; d < ms[i].r->n_decls; d++) {
                const char *nm_ = ms[i].r->decls[d].name;
                if (nm_ && nm_[0] == '-' && nm_[1] == '-')
                    var_push(c->tmp, &vars, &nvars, nm_, ms[i].r->decls[d].value);
            }
        const char *inline_style = get_attr(n, "style");
        if (inline_style) {
            hn_decl *ds;
            int nd;
            hn_parse_inline_decls(c->tmp, inline_style, strlen(inline_style), &ds, &nd);
            for (int d = 0; d < nd; d++)
                if (ds[d].name[0] == '-' && ds[d].name[1] == '-')
                    var_push(c->tmp, &vars, &nvars, ds[d].name, ds[d].value);
        }
    }
    st.vars = vars;
    st.n_vars = nvars;

    for (int phase = 0; phase < 2; phase++)
        for (int i = 0; i < nm; i++)
            for (int d = 0; d < ms[i].r->n_decls; d++) {
                hn_decl *dl = &ms[i].r->decls[d];
                if (dl->name[0] == '-' && dl->name[1] == '-') continue;
                const char *rv = c->tmp ? resolve_vars(c->tmp, dl->value, vars, nvars, 0) : dl->value;
                hn_decl one = { dl->name, rv };
                apply_decls(&st, &one, 1, phase);
            }

    /* 元素内联 style 属性优先级最高 */
    const char *inline_style = get_attr(n, "style");
    if (inline_style && c->tmp) {
        hn_decl *ds;
        int nd;
        hn_parse_inline_decls(c->tmp, inline_style, strlen(inline_style), &ds, &nd);
        for (int d = 0; d < nd; d++) {
            if (ds[d].name[0] == '-' && ds[d].name[1] == '-') continue;
            const char *rv = resolve_vars(c->tmp, ds[d].value, vars, nvars, 0);
            hn_decl one = { ds[d].name, rv };
            apply_decls(&st, &one, 1, 0);
            apply_decls(&st, &one, 1, 1);
        }
    }

    n->style = st;
    if (!parent) g_root_font = st.font_size;   /* rem 基准 = 根元素字号 */
    for (hn_node *ch = n->first; ch; ch = ch->next) compute_rec(c, ch, n);
}

void hn_style_compute_all(hn_context *c) {
    if (!c->doc) return;
    g_vw = c->vw;
    g_vh = c->vh;
    compute_rec(c, c->doc->root, NULL);
}

/* ---------- 内联 style 属性声明解析 ---------- */

void hn_parse_inline_decls(hn_arena *ar, const char *src, size_t len, hn_decl **out, int *n_out) {
    *out = NULL;
    *n_out = 0;
    int cap = 0;
    const char *p = src, *end = src + len;
    while (p < end) {
        while (p < end && (*p == ' ' || *p == ';')) p++;
        if (p >= end) break;
        const char *colon = memchr(p, ':', (size_t)(end - p));
        const char *semi = memchr(p, ';', (size_t)(end - p));
        const char *stop = semi ? semi : end;
        if (!colon || colon > stop) { p = semi ? semi + 1 : end; continue; }
        const char *ns = p, *ne = colon;
        while (ne > ns && ne[-1] == ' ') ne--;
        const char *vs = colon + 1, *ve = stop;
        while (vs < ve && *vs == ' ') vs++;
        while (ve > vs && ve[-1] == ' ') ve--;
        if (ne > ns && ve > vs) {
            if (*n_out == cap) {
                int nc = cap ? cap * 2 : 4;
                hn_decl *nd = hn_arena_alloc(ar, sizeof(hn_decl) * (size_t)nc);
                if (*out) memcpy(nd, *out, sizeof(hn_decl) * (size_t)*n_out);
                *out = nd;
                cap = nc;
            }
            (*out)[*n_out].name = hn_arena_strndup(ar, ns, (size_t)(ne - ns));
            (*out)[*n_out].value = hn_arena_strndup(ar, vs, (size_t)(ve - vs));
            (*n_out)++;
        }
        p = semi ? semi + 1 : end;
    }
}

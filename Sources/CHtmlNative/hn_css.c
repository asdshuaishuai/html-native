/* hn_css.c — CSS 子集解析器 → 规则表
 * 选择器: tag / .class / #id / * 的复合, 仅后代组合(空格; '>' 按后代宽松处理),
 * 选择器组展开为独立规则(各自 specificity), 伪类 :hover/:active/:focus/:nth-child。
 * 声明以字符串保存, 具体语义在样式计算阶段解释。
 * @media (min/max-width) 解析为规则的约束条件, 其余 at-rule 跳过。
 */
#include <ctype.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include "hn_internal.h"
#include "hn_port.h"

typedef struct { const char *p, *end; hn_arena *ar; int order; } cps;

static const char *strcasestr_local(const char *hay, const char *needle) {
    size_t m = strlen(needle);
    size_t n = strlen(hay);
    if (n < m) return NULL;
    for (size_t i = 0; i + m <= n; i++)
        if (!strncasecmp(hay + i, needle, m)) return hay + i;
    return NULL;
}

/* 带长度的整数解析(支持前导 +/-), 无数字返回 0 */
static int atoi_n(const char *s, size_t n) {
    int sign = 1, v = 0, any = 0;
    size_t i = 0;
    if (i < n && (s[i] == '+' || s[i] == '-')) { if (s[i] == '-') sign = -1; i++; }
    for (; i < n; i++) {
        if (!isdigit((unsigned char)s[i])) break;
        v = v * 10 + (s[i] - '0');
        any = 1;
    }
    return any ? sign * v : 0;
}

static void skip_ws(cps *s) {    for (;;) {
        while (s->p < s->end && isspace((unsigned char)*s->p)) s->p++;
        if ((size_t)(s->end - s->p) >= 2 && s->p[0] == '/' && s->p[1] == '*') {
            const char *q = s->p + 2;
            while (q + 1 < s->end && !(q[0] == '*' && q[1] == '/')) q++;
            s->p = (q + 1 < s->end) ? q + 2 : s->end;
            continue;
        }
        break;
    }
}

/* 标签名统一小写(HTML 标签不敏感); class/id 走 read_ident_cased(大小写敏感) */
static char *read_ident(cps *s) {
    const char *st = s->p;
    if (s->p >= s->end) return NULL;
    int c = (unsigned char)*s->p;
    if (!(isalpha(c) || c == '-' || c == '_' || c >= 0x80)) return NULL;
    s->p++;
    while (s->p < s->end) {
        c = (unsigned char)*s->p;
        if (isalnum(c) || c == '-' || c == '_' || c >= 0x80) s->p++;
        else break;
    }
    char *out = hn_arena_alloc(s->ar, (size_t)(s->p - st) + 1);
    char *ret = out;
    for (const char *i = st; i < s->p; i++) *out++ = (char)tolower((unsigned char)*i);
    *out = 0;
    return ret;
}

/* 保留大小写的标识符读取: CSS 中 class 与 id 是**大小写敏感**的,
   不能统一小写(那会让 #absR / .myClass 匹配失败)。 */
static char *read_ident_cased(cps *s) {
    const char *st = s->p;
    if (s->p >= s->end) return NULL;
    int c = (unsigned char)*s->p;
    if (!(isalpha(c) || c == '-' || c == '_' || c >= 0x80)) return NULL;
    s->p++;
    while (s->p < s->end) {
        c = (unsigned char)*s->p;
        if (isalnum(c) || c == '-' || c == '_' || c >= 0x80) s->p++;
        else break;
    }
    return hn_arena_strndup(s->ar, st, (size_t)(s->p - st));
}

static char *read_value(cps *s) {
    const char *st = s->p;
    while (s->p < s->end && *s->p != ';' && *s->p != '}') s->p++;
    const char *e = s->p;
    while (e > st && isspace((unsigned char)e[-1])) e--;
    while (st < e && isspace((unsigned char)*st)) st++;
    /* 去 !important */
    if ((size_t)(e - st) >= 10 && !strncasecmp(e - 10, "!important", 10)) {
        e -= 10;
        while (e > st && isspace((unsigned char)e[-1])) e--;
    }
    return hn_arena_strndup(s->ar, st, (size_t)(e - st));
}

static int read_compound(cps *s, hn_compound *cp) {
    memset(cp, 0, sizeof(*cp));
    int any = 0;
    for (;;) {
        if (s->p < s->end && *s->p == '*') { s->p++; any = 1; continue; }
        if (s->p < s->end && (isalpha((unsigned char)*s->p) || *s->p == '_' || (unsigned char)*s->p >= 0x80)) {
            char *t = read_ident(s);
            if (t && !cp->tag) cp->tag = t;
            any = 1;
            continue;
        }
        if (s->p < s->end && *s->p == '.') {
            s->p++;
            char *c = read_ident_cased(s);     /* class 大小写敏感 */
            if (c && cp->n_cls < 8) cp->cls[cp->n_cls++] = c;
            any = 1;
            continue;
        }
        if (s->p < s->end && *s->p == '#') {
            s->p++;
            char *i = read_ident_cased(s);     /* id 大小写敏感 */
            if (i && !cp->id) cp->id = i;
            any = 1;
            continue;
        }
        /* 伪类: :hover / :active / :focus / :nth-child(odd|even|N|n 公式) */
        if (s->p + 1 < s->end && *s->p == ':') {
            const char *save = s->p;
            s->p++;
            char *pse = read_ident(s);
            if (pse) {
                if (!strcmp(pse, "hover")) cp->pseudo = 1;
                else if (!strcmp(pse, "active")) cp->pseudo = 2;
                else if (!strcmp(pse, "focus") || !strcmp(pse, "focus-visible")) cp->pseudo = 3;
                else if (!strcmp(pse, "nth-child")) {
                    /* 括号参数: odd / even / 2n / 2n+1 / 整数 */
                    int v = 0;
                    if (s->p < s->end && *s->p == '(') {
                        s->p++;
                        const char *b = s->p;
                        while (s->p < s->end && *s->p != ')') s->p++;
                        size_t ln = (size_t)(s->p - b);
                        if (s->p < s->end) s->p++;   /* 吃掉 ')' */
                        if (ln == 3 && !strncasecmp(b, "odd", 3)) v = -1;
                        else if (ln == 4 && !strncasecmp(b, "even", 4)) v = -2;
                        else {
                            /* 含 'n' 的公式: 2n→even, 2n+1→odd, 其余按首整数 */
                            const char *np = memchr(b, 'n', ln);
                            int base = atoi_n(b, ln);
                            if (np) v = (base % 2 == 0) ? -2 : -1;
                            else v = base;
                        }
                    }
                    cp->nth = v;
                }
                else if (!strcmp(pse, "first-child")) cp->nth = -3;
                else if (!strcmp(pse, "last-child")) cp->nth = -4;
                any = 1;
                continue;
            }
            s->p = save;
        }
        break;
    }
    return any;
}

static void parse_rules(cps *s, hn_sheet *sh, float min_w, float max_w);

static void emit_rule(hn_sheet *sh, hn_compound *parts, int n_parts,
                      hn_decl *ds, int nd, int *order, float min_w, float max_w) {
    hn_rule *r = hn_arena_alloc(sh->arena, sizeof(hn_rule));
    memset(r, 0, sizeof(*r));
    r->sel.parts = hn_arena_alloc(sh->arena, sizeof(hn_compound) * (size_t)n_parts);
    memcpy(r->sel.parts, parts, sizeof(hn_compound) * (size_t)n_parts);
    r->sel.n_parts = n_parts;
    r->decls = hn_arena_alloc(sh->arena, sizeof(hn_decl) * (size_t)nd);
    memcpy(r->decls, ds, sizeof(hn_decl) * (size_t)nd);
    r->n_decls = nd;
    int spec = 0;
    for (int i = 0; i < n_parts; i++) {
        /* 伪类与 :nth-child 按类级特异性计(标准行为) */
        spec += (parts[i].id ? 256 : 0)
              + (parts[i].n_cls + (parts[i].nth ? 1 : 0) + (parts[i].pseudo ? 1 : 0)) * 16
              + (parts[i].tag ? 1 : 0);
    }
    r->spec = spec > 0xFFFF ? 0xFFFF : spec;
    r->order = (*order)++;
    r->media_min_w = min_w;
    r->media_max_w = max_w;

    hn_rule *rs = hn_arena_alloc(sh->arena, sizeof(hn_rule) * (size_t)(sh->n_rules + 1));
    if (sh->n_rules) memcpy(rs, sh->rules, sizeof(hn_rule) * (size_t)sh->n_rules);
    sh->rules = rs;
    rs[sh->n_rules++] = *r;
}

static void parse_rules(cps *s, hn_sheet *sh, float min_w, float max_w) {
    for (;;) {
        skip_ws(s);
        if (s->p >= s->end) break;
        if (*s->p == '@') {
            /* @media: 解析 min-width/max-width 条件后递归解析内部规则 */
            if (!strncasecmp(s->p, "@media", 6)) {
                const char *brace = memchr(s->p, '{', (size_t)(s->end - s->p));
                const char *semi = memchr(s->p, ';', (size_t)(s->end - s->p));
                if (brace && (!semi || brace < semi)) {
                    char cond[128];
                    size_t cl = (size_t)(brace - s->p - 6);
                    if (cl >= sizeof(cond)) cl = sizeof(cond) - 1;
                    memcpy(cond, s->p + 6, cl);
                    cond[cl] = 0;
                    float nmin = min_w, nmax = max_w;
                    /* 形如 (min-width: 600px) — 在整个条件串里找 */
                    const char *mw = strcasestr_local(cond, "min-width");
                    if (mw) nmin = strtof(strchr(mw, ':') + 1, NULL);
                    mw = strcasestr_local(cond, "max-width");
                    if (mw) {
                        float v = strtof(strchr(mw, ':') + 1, NULL);
                        if (max_w < 0 || v < max_w) nmax = v;
                    }
                    s->p = brace + 1;
                    parse_rules(s, sh, nmin, nmax);
                    if (s->p < s->end && *s->p == '}') s->p++;
                    continue;
                }
            }
            /* 其余 at-rule 整体跳过 */
            const char *brace = memchr(s->p, '{', (size_t)(s->end - s->p));
            const char *semi = memchr(s->p, ';', (size_t)(s->end - s->p));
            if (brace && (!semi || brace < semi)) {
                int depth = 0;
                const char *i = brace;
                while (i < s->end) {
                    if (*i == '{') depth++;
                    else if (*i == '}' && --depth == 0) { i++; break; }
                    i++;
                }
                s->p = i;
            } else {
                s->p = semi ? semi + 1 : s->end;
            }
            continue;
        }

        /* 选择器组: sels[i][..], ns[i] = 复合段数 */
        hn_compound sels[24][12];
        int ns[24], nsel = 0, ok = 1;
        int pend_comb = 0;   /* 作用在下一个复合左侧的组合器 */
        memset(ns, 0, sizeof(ns));
        for (;;) {
            skip_ws(s);
            if (s->p >= s->end) { ok = 0; break; }
            if (*s->p == '{') break;
            if (nsel >= 24 || ns[nsel] >= 12 || !read_compound(s, &sels[nsel][ns[nsel]])) { ok = 0; break; }
            sels[nsel][ns[nsel]].comb = pend_comb;
            ns[nsel]++;
            skip_ws(s);
            if (s->p >= s->end) { ok = 0; break; }
            if (*s->p == '>') { s->p++; pend_comb = 1; continue; }
            if (*s->p == '+') { s->p++; pend_comb = 2; continue; }
            if (*s->p == '~') { s->p++; pend_comb = 3; continue; }
            if (*s->p == ',') { s->p++; nsel++; pend_comb = 0; continue; }
            if (*s->p == '{') break;
            pend_comb = 0;   /* 后代组合 */
            continue;
        }
        if (!ok || s->p >= s->end || *s->p != '{') {
            const char *q = memchr(s->p, '}', (size_t)(s->end - s->p));
            s->p = q ? q + 1 : s->end;
            continue;
        }
        s->p++; /* '{' */

        hn_decl ds[64];
        int nd = 0;
        for (;;) {
            skip_ws(s);
            if (s->p >= s->end) break;
            if (*s->p == '}') { s->p++; break; }
            char *prop = read_ident(s);
            skip_ws(s);
            if (!prop || s->p >= s->end || *s->p != ':') {
                const char *q = memchr(s->p, ';', (size_t)(s->end - s->p));
                const char *qb = memchr(s->p, '}', (size_t)(s->end - s->p));
                if (qb && (!q || qb < q)) s->p = qb;
                else if (q) s->p = q + 1;
                else s->p = s->end;
                continue;
            }
            s->p++; /* ':' */
            char *val = read_value(s);
            if (s->p < s->end && *s->p == ';') s->p++;
            if (val && *val && nd < 64) {
                ds[nd].name = prop;
                ds[nd].value = val;
                nd++;
            }
        }
        for (int i = 0; i <= nsel; i++)
            if (ns[i]) emit_rule(sh, sels[i], ns[i], ds, nd, &s->order, min_w, max_w);
    }
}

hn_sheet *hn_parse_css(const char *src, size_t len) {
    hn_sheet *sh = calloc(1, sizeof(hn_sheet));
    if (!sh) abort();
    sh->arena = hn_arena_create();
    if (src && len) {
        cps s = { src, src + len, sh->arena, 0 };
        parse_rules(&s, sh, -1.0f, -1.0f);
    }
    return sh;
}

void hn_sheet_free(hn_sheet *sh) {
    if (!sh) return;
    hn_arena_destroy(sh->arena);
    free(sh);
}

/* ---------------- 颜色 ---------------- */

static int hex_v(int c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static hn_color pack_rgba(int r, int g, int b, int a) {
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    if (a < 0) a = 0; if (a > 255) a = 255;
    return ((hn_color)r << 24) | ((hn_color)g << 16) | ((hn_color)b << 8) | (hn_color)a;
}

/* hsl(h, s%, l% [, a]) — s/l 支持前导点小数; h 支持(deg/rad 后缀忽略) */
static int hsl_parse(const char *s, size_t n, hn_color *out, int with_alpha) {
    float comp[4] = {0, 0, 0, 1};
    int ci = 0;
    size_t i = 0;
    while (i < n && ci < 4) {
        while (i < n && (s[i] == ' ' || s[i] == ',')) i++;
        size_t st = i;
        while (i < n && s[i] != ',' && s[i] != ' ' && s[i] != ')') i++;
        if (i > st) {
            char buf[24];
            size_t l = i - st < sizeof(buf) - 1 ? i - st : sizeof(buf) - 1;
            memcpy(buf, s + st, l);
            buf[l] = 0;
            comp[ci++] = strtof(buf, NULL);
        } else break;
    }
    int need = with_alpha ? 4 : 3;
    if (ci < 3) return 0;
    (void)need;
    float h = comp[0], sat = comp[1] / 100.0f, lig = comp[2] / 100.0f;
    if (sat < 0) sat = 0; if (sat > 1) sat = 1;
    if (lig < 0) lig = 0; if (lig > 1) lig = 1;
    float c = (1 - fabsf(2 * lig - 1)) * sat;
    float hp = fmodf(fmodf(h, 360.0f) + 360.0f, 360.0f) / 60.0f;
    float x = c * (1 - fabsf(fmodf(hp, 2.0f) - 1));
    float r = 0, g = 0, b = 0;
    if (hp < 1)      { r = c; g = x; }
    else if (hp < 2) { r = x; g = c; }
    else if (hp < 3) { g = c; b = x; }
    else if (hp < 4) { g = x; b = c; }
    else if (hp < 5) { r = x; b = c; }
    else             { r = c; b = x; }
    float m = lig - c / 2;
    int a = comp[3] <= 1.0f ? (int)(comp[3] * 255) : (int)comp[3];
    *out = pack_rgba((int)((r + m) * 255.0f + 0.5f),
                     (int)((g + m) * 255.0f + 0.5f),
                     (int)((b + m) * 255.0f + 0.5f), a);
    return 1;
}

int hn_color_parse(const char *s, size_t n, hn_color *out) {
    if (!s || !n) return 0;
    while (n && *s == ' ') { s++; n--; }
    while (n && s[n - 1] == ' ') n--;
    if (!n) return 0;

    if (*s == '#') {
        s++; n--;
        if (n != 3 && n != 4 && n != 6 && n != 8) return 0;
        int v[8];
        for (size_t i = 0; i < n; i++) { v[i] = hex_v(s[i]); if (v[i] < 0) return 0; }
        if (n == 3) *out = pack_rgba(v[0] * 17, v[1] * 17, v[2] * 17, 255);
        else if (n == 4) *out = pack_rgba(v[0] * 17, v[1] * 17, v[2] * 17, v[3] * 17);
        else if (n == 6) *out = pack_rgba(v[0]*16+v[1], v[2]*16+v[3], v[4]*16+v[5], 255);
        else *out = pack_rgba(v[0]*16+v[1], v[2]*16+v[3], v[4]*16+v[5], v[6]*16+v[7]);
        return 1;
    }
    if (n >= 5 && !strncasecmp(s, "rgba(", 5)) s += 5, n -= 5;
    else if (n >= 4 && !strncasecmp(s, "rgb(", 4)) s += 4, n -= 4;
    else if (n >= 5 && !strncasecmp(s, "hsla(", 5)) return hsl_parse(s + 5, n - 5, out, 1);
    else if (n >= 4 && !strncasecmp(s, "hsl(", 4)) return hsl_parse(s + 4, n - 4, out, 0);
    else if (n >= 11 && !strncasecmp(s, "transparent", 11)) { *out = 0; return 1; }
    else {
        static struct { const char *n; hn_color c; } N[] = {
            {"black", 0x000000FF}, {"white", 0xFFFFFFFF}, {"red", 0xFF0000FF},
            {"green", 0x008000FF}, {"blue", 0x0000FFFF},  {"yellow", 0xFFFF00FF},
            {"orange", 0xFFA500FF},{"purple", 0x800080FF},{"pink", 0xFFC0CBFF},
            {"gray", 0x808080FF},  {"grey", 0x808080FF},   {"silver", 0xC0C0C0FF},
            {"gold", 0xFFD700FF},   {"cyan", 0x00FFFFFF},  {"aqua", 0x00FFFFFF},
            {"magenta", 0xFF00FFFF},{"fuchsia", 0xFF00FFFF},{"lime", 0x00FF00FF},
            {"navy", 0x000080FF},   {"teal", 0x008080FF},  {"olive", 0x808000FF},
            {"maroon", 0x800000FF}, {"coral", 0xFF7F50FF}, {"salmon", 0xFA8072FF},
            {"tomato", 0xFF6347FF}, {"crimson", 0xDC143CFF},{"indigo", 0x4B0082FF},
            {"violet", 0xEE82EEFF}, {"brown", 0xA52A2AFF},
            {"darkgray", 0xA9A9A9FF},{"lightgray", 0xD3D3D3FF},
            {"darkblue", 0x00008BFF},{"lightblue", 0xADD8E6FF},
            {"darkred", 0x8B0000FF},{"lightred", 0xFFB3B3FF},
        };
        for (size_t i = 0; i < sizeof(N) / sizeof(N[0]); i++)
            if (n == strlen(N[i].n) && !strncasecmp(s, N[i].n, n)) { *out = N[i].c; return 1; }
        return 0;
    }
    /* rgb(a)( r, g, b [, a] ) */
    float comp[4] = {0, 0, 0, 1};
    int ci = 0;
    size_t i = 0;
    while (i < n && ci < 4) {
        while (i < n && (s[i] == ' ' || s[i] == ',' || s[i] == '\t')) i++;
        size_t st = i;
        while (i < n && s[i] != ',' && s[i] != ' ' && s[i] != ')') i++;
        if (i > st) {
            char buf[24];
            size_t l = i - st < sizeof(buf) - 1 ? i - st : sizeof(buf) - 1;
            memcpy(buf, s + st, l);
            buf[l] = 0;
            comp[ci++] = strtof(buf, NULL);
        } else break;
    }
    if (ci < 3) return 0;
    int a = comp[3] <= 1.0f ? (int)(comp[3] * 255) : (int)comp[3];
    *out = pack_rgba((int)comp[0], (int)comp[1], (int)comp[2], a);
    return 1;
}

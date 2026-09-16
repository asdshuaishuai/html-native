/* hn_html.c — HTML 子集解析器 → DOM
 * 支持: 元素/属性(void 元素, 自闭合), 文本实体解码 + 空白折叠,
 * <style>/<script> 原文收集(style 内容直接解析为样式表), 注释/doctype 跳过。
 * 有意不做的: 错误恢复语义完全对齐浏览器规范(足够解析良构文档)。
 */
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include "hn_internal.h"

static int is_ws(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static void append_child(hn_node *parent, hn_node *child) {
    child->parent = parent;
    child->next = child->prev = NULL;
    if (parent->last) {
        parent->last->next = child;
        child->prev = parent->last;
        parent->last = child;
    } else {
        parent->first = parent->last = child;
    }
}

static hn_node *new_node(hn_arena *ar, hn_node_kind k) {
    hn_node *n = hn_arena_alloc(ar, sizeof(hn_node));
    memset(n, 0, sizeof(hn_node));
    n->kind = k;
    n->arena = ar;
    return n;
}

static char *lower_dup(hn_arena *ar, const char *s, size_t n) {
    char *p = hn_arena_alloc(ar, n + 1);
    for (size_t i = 0; i < n; i++) p[i] = (char)tolower((unsigned char)s[i]);
    p[n] = 0;
    return p;
}

static int is_void_tag(const char *t) {
    static const char *v[] = { "area","base","br","col","embed","hr","img","input",
                               "link","meta","param","source","track","wbr", NULL };
    for (int i = 0; v[i]; i++) if (!strcmp(v[i], t)) return 1;
    return 0;
}

/* 大小写不敏感查找子串 */
static const char *find_ci(const char *hay, size_t n, const char *needle) {
    size_t m = strlen(needle);
    if (n < m) return NULL;
    for (size_t i = 0; i + m <= n; i++)
        if (!strncasecmp(hay + i, needle, m)) return hay + i;
    return NULL;
}

/* 码点 → utf8(至多 4 字节), 返回字节数 */
static size_t cp_utf8(unsigned cp, char *out) {
    size_t o = 0;
    if (cp < 0x80) out[o++] = (char)cp;
    else if (cp < 0x800) {
        out[o++] = (char)(0xC0 | (cp >> 6));
        out[o++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out[o++] = (char)(0xE0 | (cp >> 12));
        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[o++] = (char)(0x80 | (cp & 0x3F));
    } else {
        out[o++] = (char)(0xF0 | (cp >> 18));
        out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[o++] = (char)(0x80 | (cp & 0x3F));
    }
    return o;
}

/* 实体: 成功时写出(至多 4 字节 utf8), 返回消耗的源长度; 失败返回 0 */
static size_t entity_out(const char *s, size_t n, char *out, size_t *out_n) {
    struct { const char *name; const char *rep; } E[] = {
        { "&amp;",  "&" },      { "&lt;",  "<" },   { "&gt;",  ">" },
        { "&quot;", "\"" },     { "&apos;", "'" },  { "&#39;", "'" },
        { "&nbsp;", "\xC2\xA0" }, { "&copy;", "\xC2\xA9" }, { "&mdash;", "\xE2\x80\x94" },
    };
    for (size_t i = 0; i < sizeof(E) / sizeof(E[0]); i++) {
        size_t l = strlen(E[i].name);
        if (n >= l && !strncmp(s, E[i].name, l)) {
            size_t rl = strlen(E[i].rep);
            memcpy(out, E[i].rep, rl);
            *out_n = rl;
            return l;
        }
    }
    if (n >= 4 && s[1] == '#' && (s[2] == 'x' || s[2] == 'X')) { /* 十六进制实体 &#x2014; */
        size_t i = 3;
        long v = 0;
        while (i < n && isxdigit((unsigned char)s[i])) {
            char c = s[i];
            v = v * 16 + (c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
            i++;
        }
        if (i > 3 && i < n && s[i] == ';' && v > 0 && v < 0x110000) {
            *out_n = cp_utf8((unsigned)v, out);
            return i + 1;
        }
    }
    if (n >= 3 && s[1] == '#') { /* 十进制数字实体 &#123; */
        size_t i = 2;
        long v = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; }
        if (i > 2 && i < n && s[i] == ';' && v > 0 && v < 0x110000) {
            *out_n = cp_utf8((unsigned)v, out);
            return i + 1;
        }
    }
    return 0;
}

/* 文本入树: 实体解码 + 空白折叠(nbsp 不折叠) */
static void push_text(hn_arena *ar, hn_node *cur, const char *s, size_t n) {
    char *buf = hn_arena_alloc(ar, n + 8);
    size_t o = 0;
    int pend = 0; /* 挂起的空格 */
    for (size_t i = 0; i < n; ) {
        if (s[i] == '&') {
            char rep[4]; size_t rl = 0;
            size_t used = entity_out(s + i, n - i, rep, &rl);
            if (used) {
                for (size_t k = 0; k < rl; k++) {
                    unsigned char b = (unsigned char)rep[k];
                    if (b == ' ') { pend = 1; }
                    else { if (pend) { buf[o++] = ' '; pend = 0; } buf[o++] = (char)b; }
                }
                i += used;
                continue;
            }
        }
        int c = (unsigned char)s[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f') { pend = 1; i++; continue; }
        if (pend) { buf[o++] = ' '; pend = 0; }
        buf[o++] = s[i++];
    }
    if (!o) return;
    hn_node *t = new_node(ar, HN_TEXT);
    t->text = buf;
    t->text_len = o;
    append_child(cur, t);
}

/* 解析后整理: 混排场景下的边界裁剪(前后是元素或端点时裁掉首尾空格) */
static void normalize_texts(hn_node *n) {
    for (hn_node *c = n->first; c; c = c->next) {
        if (c->kind == HN_TEXT && c->text_len) {
            const char *s = c->text; size_t len = c->text_len;
            if (!c->prev || c->prev->kind == HN_ELEM)
                while (len && *s == ' ') { s++; len--; }
            if (!c->next || c->next->kind == HN_ELEM)
                while (len && s[len - 1] == ' ') len--;
            c->text = s;
            c->text_len = len;
        } else if (c->kind == HN_ELEM) {
            normalize_texts(c);
        }
    }
}

static void push_attr(hn_arena *ar, hn_node *el, const char *name, const char *value) {
    hn_attr *na = hn_arena_alloc(ar, sizeof(hn_attr) * (size_t)(el->n_attrs + 1));
    if (el->n_attrs) memcpy(na, el->attrs, sizeof(hn_attr) * (size_t)el->n_attrs);
    el->attrs = na;
    na[el->n_attrs].name = name;
    na[el->n_attrs].value = value;
    el->n_attrs++;
}

void hn_parse_into(hn_doc *doc, hn_node *root, const char *src, size_t len) {
    hn_arena *ar = doc->arena;
    const char *p = src, *end = src + len;
    if (len >= 3 && !memcmp(src, "\xEF\xBB\xBF", 3)) p += 3;
    hn_node *cur = root;

    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        if (!lt) { push_text(ar, cur, p, (size_t)(end - p)); break; }
        if (lt > p) push_text(ar, cur, p, (size_t)(lt - p));

        if ((size_t)(end - lt) >= 4 && !memcmp(lt, "<!--", 4)) {
            const char *cl = find_ci(lt + 4, (size_t)(end - lt - 4), "-->");
            p = cl ? cl + 3 : end;
            continue;
        }
        if (lt[1] == '!' || lt[1] == '?') { /* doctype / 处理指令 */
            const char *g = memchr(lt, '>', (size_t)(end - lt));
            p = g ? g + 1 : end;
            continue;
        }
        if (lt[1] == '/') { /* 结束标签: 宽松向上匹配 */
            const char *g = memchr(lt, '>', (size_t)(end - lt));
            if (!g) break;
            const char *np = lt + 2;
            size_t nl = 0;
            while (np + nl < g && !is_ws((unsigned char)np[nl])) nl++;
            if (nl) {
                char *name = lower_dup(ar, np, nl);
                hn_node *a = cur;
                while (a && a != root && (!a->tag || strcmp(a->tag, name))) a = a->parent;
                if (a && a != root) cur = a->parent;
            }
            p = g + 1;
            continue;
        }
        if (isalpha((unsigned char)lt[1])) { /* 开始标签 */
            const char *np = lt + 1;
            size_t nl = 0;
            while (np + nl < end && (isalnum((unsigned char)np[nl]) || np[nl] == '-' || np[nl] == '_')) nl++;
            char *tag = lower_dup(ar, np, nl);
            hn_node *el = new_node(ar, HN_ELEM);
            el->tag = tag;
            append_child(cur, el);

            const char *a = np + nl;
            int self_close = 0;
            while (a < end) {
                while (a < end && is_ws((unsigned char)*a)) a++;
                if (a >= end) break;
                if (*a == '>') { a++; break; }
                if (*a == '/') { self_close = 1; a++; continue; }
                const char *an = a;
                size_t anl = 0;
                while (a < end && !is_ws((unsigned char)*a) && *a != '=' && *a != '>' && *a != '/') { a++; anl++; }
                if (!anl) { a++; continue; }
                char *aname = lower_dup(ar, an, anl);
                const char *val = NULL;
                const char *val_end = NULL;
                const char *q = a;
                while (q < end && is_ws((unsigned char)*q)) q++;
                if (q < end && *q == '=') {
                    q++;
                    while (q < end && is_ws((unsigned char)*q)) q++;
                    if (q < end && (*q == '"' || *q == '\'')) {
                        char qc = *q++;
                        val = q;
                        while (q < end && *q != qc) q++;
                        val_end = q;
                        a = (q < end) ? q + 1 : end;
                    } else {
                        val = q;
                        while (q < end && !is_ws((unsigned char)*q) && *q != '>') q++;
                        val_end = q;
                        a = q;
                    }
                    /* 属性值实体解码并复制进 arena(闭合引号不计入) */
                    if (val) {
                        char dec[512];
                        size_t o = 0;
                        const char *i = val;
                        size_t rem = (size_t)(val_end - val);
                        while (rem && o + 4 < sizeof(dec)) {
                            if (*i == '&') {
                                char rep[4]; size_t rl = 0;
                                size_t used = entity_out(i, rem, rep, &rl);
                                if (used) { memcpy(dec + o, rep, rl); o += rl; i += used; rem -= used; continue; }
                            }
                            dec[o++] = *i++; rem--;
                        }
                        push_attr(ar, el, aname, hn_arena_strndup(ar, dec, o));
                        continue;
                    }
                }
                push_attr(ar, el, aname, "");
            }

            int voidel = is_void_tag(tag);
            if (!voidel && !self_close && (!strcmp(tag, "style") || !strcmp(tag, "script"))) {
                const char *close = find_ci(a, (size_t)(end - a), strcmp(tag, "style") ? "</script" : "</style");
                const char *gt = close ? memchr(close, '>', (size_t)(end - close)) : NULL;
                const char *stop = gt ? gt : end;
                if (!strcmp(tag, "style") && stop > a) {
                    hn_sheet *sh = hn_parse_css(a, (size_t)(stop - a));
                    if (sh) {
                        if (doc->n_inline == doc->cap_inline) {
                            int nc = doc->cap_inline ? doc->cap_inline * 2 : 4;
                            struct hn_sheet **ns = hn_arena_alloc(ar, sizeof(hn_sheet *) * (size_t)nc);
                            if (doc->inline_sheets) memcpy(ns, doc->inline_sheets, sizeof(hn_sheet *) * (size_t)doc->n_inline);
                            doc->inline_sheets = ns;
                            doc->cap_inline = nc;
                        }
                        doc->inline_sheets[doc->n_inline++] = sh;
                    }
                }
                p = gt ? gt + 1 : end;
                continue;
            }
            if (!voidel && !self_close) cur = el;
            p = a;
            continue;
        }
        /* 孤立 '<' 视为文本 */
        push_text(ar, cur, "<", 1);
        p = lt + 1;
    }
    normalize_texts(root);
}

hn_doc *hn_parse_html(const char *src, size_t len) {
    hn_doc *doc = calloc(1, sizeof(hn_doc));
    if (!doc) abort();
    doc->arena = hn_arena_create();
    doc->root = new_node(doc->arena, HN_ELEM);
    doc->root->tag = "html";
    if (src && len) hn_parse_into(doc, doc->root, src, len);
    return doc;
}

void hn_doc_free(hn_doc *doc) {
    if (!doc) return;
    for (int i = 0; i < doc->n_inline; i++) hn_sheet_free(doc->inline_sheets[i]);
    hn_arena_destroy(doc->arena);
    free(doc);
}

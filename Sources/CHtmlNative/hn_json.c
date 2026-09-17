/* hn_json.c — 极简 JSON 解析器(递归下降, 就地解析)
 *
 * 关键设计: 每个节点独立分配(地址稳定), 容器的子节点用**指针数组**。
 * 绝不能假设"子节点在节点池中连续" —— 嵌套容器的子节点会占据池中的
 * 位置, 于是父容器会错把内层子节点当成自己的元素。这个 bug 曾在
 * Lottie 解析中表现为"形状层解析不出任何几何"。
 *
 * 字符串转义就地解码(解码后不会比原文更长, 可安全覆盖原字节)。
 */
#include "hn_json.h"
#include <stdlib.h>
#include <string.h>

/* 句柄布局: [长度][节点指针...]。长度前置, 释放时精确遍历。 */
typedef struct {
    int       n;
    hn_json **nodes;
} jhandle;

typedef struct {
    char     *p, *end;
    hn_json **nodes;
    int       n, cap;
    int       failed;
} jp;

static hn_json *jalloc(jp *j) {
    if (j->n == j->cap) {
        int nc = j->cap ? j->cap * 2 : 64;
        hn_json **nv = (hn_json **)realloc(j->nodes, sizeof(hn_json *) * (size_t)nc);
        if (!nv) { j->failed = 1; return NULL; }
        j->nodes = nv;
        j->cap = nc;
    }
    hn_json *n = (hn_json *)calloc(1, sizeof(hn_json));
    if (!n) { j->failed = 1; return NULL; }
    j->nodes[j->n++] = n;
    return n;
}

static void jfree_all(jp *j) {
    for (int i = 0; i < j->n; i++) {
        free(j->nodes[i]->items);
        free(j->nodes[i]);
    }
    free(j->nodes);
}

static void jskip(jp *j) {
    for (;;) {
        while (j->p < j->end && (unsigned char)*j->p <= ' ') j->p++;
        /* 容忍 // 行注释: 部分 Lottie 导出/手写文件会带 */
        if (j->p + 1 < j->end && j->p[0] == '/' && j->p[1] == '/') {
            while (j->p < j->end && *j->p != '\n') j->p++;
            continue;
        }
        break;
    }
}

static int jhex(char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

/* 解析字符串(指针已指向开引号之后), 就地解码转义 */
static char *jstr(jp *j) {
    char *hdr = j->p;
    char *w = j->p;
    while (j->p < j->end && *j->p != '"') {
        if (*j->p == '\\' && j->p + 1 < j->end) {
            j->p++;
            char e = *j->p++;
            switch (e) {
            case 'n': *w++ = '\n'; break;
            case 't': *w++ = '\t'; break;
            case 'r': *w++ = '\r'; break;
            case 'b': *w++ = '\b'; break;
            case 'f': *w++ = '\f'; break;
            case '/': *w++ = '/'; break;
            case '\\': *w++ = '\\'; break;
            case '"': *w++ = '"'; break;
            case 'u': {
                if (j->p + 4 > j->end) { j->failed = 1; return hdr; }
                int u = 0;
                for (int i = 0; i < 4; i++) {
                    int h = jhex(j->p[i]);
                    if (h < 0) { j->failed = 1; return hdr; }
                    u = u * 16 + h;
                }
                j->p += 4;
                unsigned cp = (unsigned)u;
                /* 代理对合并(emoji 等 BMP 外字符) */
                if (cp >= 0xD800u && cp <= 0xDBFFu && j->p + 6 <= j->end &&
                    j->p[0] == '\\' && j->p[1] == 'u') {
                    int lo = 0, ok = 1;
                    for (int i = 0; i < 4; i++) {
                        int h = jhex(j->p[2 + i]);
                        if (h < 0) { ok = 0; break; }
                        lo = lo * 16 + h;
                    }
                    if (ok && lo >= 0xDC00 && lo <= 0xDFFF) {
                        cp = 0x10000u + ((cp - 0xD800u) << 10) + ((unsigned)lo - 0xDC00u);
                        j->p += 6;
                    }
                }
                if (cp < 0x80u) {
                    *w++ = (char)cp;
                } else if (cp < 0x800u) {
                    *w++ = (char)(0xC0u | (cp >> 6));
                    *w++ = (char)(0x80u | (cp & 0x3Fu));
                } else if (cp < 0x10000u) {
                    *w++ = (char)(0xE0u | (cp >> 12));
                    *w++ = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                    *w++ = (char)(0x80u | (cp & 0x3Fu));
                } else {
                    *w++ = (char)(0xF0u | (cp >> 18));
                    *w++ = (char)(0x80u | ((cp >> 12) & 0x3Fu));
                    *w++ = (char)(0x80u | ((cp >> 6) & 0x3Fu));
                    *w++ = (char)(0x80u | (cp & 0x3Fu));
                }
                break;
            }
            default: *w++ = e; break;
            }
        } else {
            *w++ = *j->p++;
        }
    }
    if (j->p >= j->end) { j->failed = 1; return hdr; }
    j->p++;                       /* 跳过闭引号 */
    *w = 0;                       /* 就地截断 */
    return hdr;
}

static hn_json *jvalue(jp *j);

static int jpush(jp *j, hn_json *c, hn_json *v) {
    hn_json **nv = (hn_json **)realloc(c->items, sizeof(hn_json *) * (size_t)(c->count + 1));
    if (!nv) { j->failed = 1; return 0; }
    c->items = nv;
    c->items[c->count++] = v;
    return 1;
}

static hn_json *jarray(jp *j) {
    hn_json *a = jalloc(j);
    if (!a) return NULL;
    a->kind = HN_JSON_ARR;
    j->p++;                                   /* '[' */
    jskip(j);
    if (j->p < j->end && *j->p == ']') { j->p++; return a; }
    for (;;) {
        jskip(j);
        hn_json *e = jvalue(j);
        if (!e) { j->failed = 1; return a; }
        if (!jpush(j, a, e)) return a;
        jskip(j);
        if (j->p < j->end && *j->p == ',') { j->p++; continue; }
        if (j->p < j->end && *j->p == ']') { j->p++; break; }
        j->failed = 1;
        break;
    }
    return a;
}

static hn_json *jobject(jp *j) {
    hn_json *o = jalloc(j);
    if (!o) return NULL;
    o->kind = HN_JSON_OBJ;
    j->p++;                                   /* '{' */
    jskip(j);
    if (j->p < j->end && *j->p == '}') { j->p++; return o; }
    for (;;) {
        jskip(j);
        if (j->p >= j->end || *j->p != '"') { j->failed = 1; break; }
        j->p++;
        char *k = jstr(j);
        if (j->failed) break;
        jskip(j);
        if (j->p >= j->end || *j->p != ':') { j->failed = 1; break; }
        j->p++;
        hn_json *v = jvalue(j);
        if (!v) { j->failed = 1; break; }
        v->key = k;
        if (!jpush(j, o, v)) break;
        jskip(j);
        if (j->p < j->end && *j->p == ',') { j->p++; continue; }
        if (j->p < j->end && *j->p == '}') { j->p++; break; }
        j->failed = 1;
        break;
    }
    return o;
}

static hn_json *jvalue(jp *j) {
    jskip(j);
    if (j->p >= j->end) { j->failed = 1; return NULL; }
    char ch = *j->p;
    if (ch == '{') return jobject(j);
    if (ch == '[') return jarray(j);
    if (ch == '"') {
        j->p++;
        hn_json *s = jalloc(j);
        if (!s) return NULL;
        s->kind = HN_JSON_STR;
        s->str = jstr(j);
        return s;
    }
    if (j->end - j->p >= 4 && !strncmp(j->p, "true", 4)) {
        j->p += 4;
        hn_json *b = jalloc(j);
        if (!b) return NULL;
        b->kind = HN_JSON_BOOL; b->boolean = 1; b->num = 1;
        return b;
    }
    if (j->end - j->p >= 5 && !strncmp(j->p, "false", 5)) {
        j->p += 5;
        hn_json *b = jalloc(j);
        if (!b) return NULL;
        b->kind = HN_JSON_BOOL; b->boolean = 0; b->num = 0;
        return b;
    }
    if (j->end - j->p >= 4 && !strncmp(j->p, "null", 4)) {
        j->p += 4;
        hn_json *n = jalloc(j);
        if (!n) return NULL;
        n->kind = HN_JSON_NULL;
        return n;
    }
    /* 数字: strtod 需要 \0 结尾, 就地临时截断后恢复 */
    {
        char *st = j->p;
        char *q = j->p;
        if (*q == '-' || *q == '+') q++;
        while (q < j->end && ((*q >= '0' && *q <= '9') || *q == '.' ||
                              *q == 'e' || *q == 'E' || *q == '+' || *q == '-')) q++;
        if (q == st) { j->failed = 1; return NULL; }
        char save = *q;
        *q = 0;
        double d = strtod(st, NULL);
        *q = save;
        j->p = q;
        hn_json *n = jalloc(j);
        if (!n) return NULL;
        n->kind = HN_JSON_NUM;
        n->num = d;
        return n;
    }
}

hn_json *hn_json_parse(char *buf, size_t len, void **arena_out) {
    if (arena_out) *arena_out = NULL;
    if (!buf || len == 0) return NULL;
    jp j;
    memset(&j, 0, sizeof(j));
    j.p = buf;
    j.end = buf + len;
    hn_json *root = jvalue(&j);
    if (j.failed || !root) { jfree_all(&j); return NULL; }
    /* 句柄: [长度][节点...]。长度前置, 释放时精确遍历。 */
    jhandle *h = (jhandle *)malloc(sizeof(jhandle) + sizeof(hn_json *) * (size_t)(j.n + 1));
    if (!h) { jfree_all(&j); return NULL; }
    h->n = j.n;
    h->nodes = (hn_json **)((char *)h + sizeof(jhandle));
    for (int i = 0; i < j.n; i++) h->nodes[i] = j.nodes[i];
    free(j.nodes);                     /* 只释放指针表, 节点本身归 h 管 */
    if (arena_out) *arena_out = h;
    return root;
}

void hn_json_free(void *arena) {
    if (!arena) return;
    jhandle *h = (jhandle *)arena;
    for (int i = 0; i < h->n; i++) {
        free(h->nodes[i]->items);
        free(h->nodes[i]);
    }
    free(h);
}

/* ---------------- 便捷访问 ---------------- */

hn_json *hn_json_obj_get(const hn_json *obj, const char *key) {
    if (!obj || obj->kind != HN_JSON_OBJ || !key) return NULL;
    for (int i = 0; i < obj->count; i++)
        if (obj->items[i]->key && !strcmp(obj->items[i]->key, key))
            return obj->items[i];
    return NULL;
}

double hn_json_num(const hn_json *v, double dflt) {
    if (!v) return dflt;
    if (v->kind == HN_JSON_NUM) return v->num;
    if (v->kind == HN_JSON_BOOL) return v->boolean ? 1 : 0;
    return dflt;
}

const char *hn_json_str(const hn_json *v, const char *dflt) {
    if (v && v->kind == HN_JSON_STR && v->str) return v->str;
    return dflt;
}

int hn_json_bool(const hn_json *v, int dflt) {
    if (!v) return dflt;
    if (v->kind == HN_JSON_BOOL) return v->boolean;
    if (v->kind == HN_JSON_NUM) return v->num != 0;
    return dflt;
}

hn_json *hn_json_at(const hn_json *arr, int i) {
    if (!arr || arr->kind != HN_JSON_ARR || i < 0 || i >= arr->count) return NULL;
    return arr->items[i];
}

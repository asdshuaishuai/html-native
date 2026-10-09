/* hn_daemon.c — C99 常驻宿主(Unix socket JSON-lines 服务)
 *
 * 定位: agent 的入口。替代 Sources/HnDaemon(Swift 版), 协议**逐字节兼容** ——
 * agent 生态(hn CLI / MCP / 任何 socket 客户端)无缝切换。
 *
 * 架构: 每个应用一个 hn_context + 一份渲染位图。无窗口模式(--headless)下
 * 同样完整可用 —— open/update/eval/dom/dump 全链路不触 GUI, agent 在
 * CI/服务器上也能驱动 UI(渲染结果可经 --shot/shot op 落盘)。
 *
 * 协议(每行一个 JSON 请求, 回一行 JSON 响应):
 *   {"op":"ping"}
 *   {"op":"open","id":"a","html":"...","css":"...","w":480,"h":700,
 *    "title":"...","transparent":0|1,"headless":0|1}
 *   {"op":"update","id":"a","html":"..."}
 *   {"op":"close","id":"a"}
 *   {"op":"list"}                     → {"apps":[{id,w,h,headless}]}
 *   {"op":"eval","id":"a","js":"..."} → {"value":"..."}
 *   {"op":"dom","id":"a"}             → {"dom":[...]}
 *   {"op":"dump","id":"a"}            → {"cmds":N}
 *   {"op":"text","id":"a","element":"x"} → {"text":"..."}
 *   {"op":"event","id":"a","kind":"click","target":"go","x":..,"y":..}
 *   {"op":"shot","id":"a","path":"out.png"}
 *
 * 与 Swift 版的差异(刻意):
 *   - 窗口/事件循环归各平台壳(tools/hnweb_macos.c / hnweb_linux.c / hnwin.c)
 *     —— daemon 只管**无头**应用与窗口应用的文档生命周期。平台壳后续可通过
 *     同一 socket 接管"开窗"op(本期无头优先, 窗口由壳的 CLI 直开)。
 *   - sys:// 桥统一在这一层(原来 Swift/SystemBridge 与 C sysbridge 两份)。
 *
 * 用法: hn_daemon [--socket PATH]   默认 ~/.html-native/hn-daemon.sock
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

#include "hn.h"
#include "hnsoft.h"
#include "hn_rt.h"
#include <time.h>

#include "hn.h"
#include "hnsoft.h"

/* ---------------- 极简 JSON 值读取(请求是小对象, 不需要完整解析器) ----------------
 * 与 tools/sysbridge.c 的 json_get 同思路: 按键名找 "key" 后的冒号, 读字符串/
 * 数字。转义只处理 \" 与 \\(HTML 内容里常见就这两种); 请求体来自 agent,
 * 恶意构造最多读到错误值, 不会越界(有 cap)。 */

static const char *json_find(const char *json, const char *key) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = json;
    while ((p = strstr(p, pat)) != NULL) {
        const char *q = p + strlen(pat);
        while (*q == ' ' || *q == '\t') q++;
        if (*q == ':') return q + 1;
        p = q;
    }
    return NULL;
}

/* 读字符串值(处理 \" \\\\ 转义)。返回 malloc 串或 NULL。 */
static char *json_str(const char *json, const char *key) {
    const char *p = json_find(json, key);
    if (!p) return NULL;
    while (*p == ' ' || *p == '\t') p++;
    if (*p != '"') return NULL;
    p++;
    size_t cap = 64, n = 0;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    while (*p && *p != '"') {
        if (n + 8 >= cap) { cap *= 2; out = (char *)realloc(out, cap); if (!out) return NULL; }
        if (*p == '\\' && p[1]) {
            p++;
            if (*p == 'n') out[n++] = '\n';
            else if (*p == 't') out[n++] = '\t';
            else out[n++] = *p;
            p++;
        } else {
            out[n++] = *p++;
        }
    }
    out[n] = 0;
    return out;
}

static long json_num(const char *json, const char *key, long dflt) {
    const char *p = json_find(json, key);
    if (!p) return dflt;
    while (*p == ' ' || *p == '\t') p++;
    char *e = NULL;
    long v = strtol(p, &e, 10);
    return e == p ? dflt : v;
}

/* ---------------- 应用表 ---------------- */

#define HN_DAEMON_MAX_APPS 32

typedef struct {
    char id[128];
    hn_context *ctx;
    int w, h;
    int headless;
    int alive;
    char *html;          /* 原始 HTML(persist 用) */
    size_t html_len;
} dapp;

static dapp g_apps[HN_DAEMON_MAX_APPS];
static int g_app_n = 0;
static hn_text_backend g_tb;
static const char *g_store_id_default = "default";

static dapp *app_find(const char *id) {
    for (int i = 0; i < g_app_n; i++)
        if (g_apps[i].alive && !strcmp(g_apps[i].id, id)) return &g_apps[i];
    return NULL;
}

static dapp *app_create(const char *id, int headless) {
    if (app_find(id)) return NULL;
    if (g_app_n >= HN_DAEMON_MAX_APPS) return NULL;
    dapp *a = &g_apps[g_app_n++];
    memset(a, 0, sizeof(*a));
    snprintf(a->id, sizeof(a->id), "%s", id);
    a->headless = headless;
    a->alive = 1;
    a->ctx = hn_context_create();
    if (a->ctx) hn_context_set_assets(a->ctx, NULL);  /* 资产后端按 cwd 解析 */
    return a;
}

static void app_destroy(dapp *a) {
    if (a->ctx) hn_context_destroy(a->ctx);
    free(a->html);
    a->ctx = NULL;
    a->alive = 0;
}

/* ---------------- 文本后端(FreeType 可用则真实测量) ---------------- */

static float tb_measure(void *ctx, const hn_font_desc *font, const char *utf8, size_t len) {
    (void)ctx;
    return hnsoft_measure(font, utf8, len);
}
static void tb_metrics(void *ctx, const hn_font_desc *font,
                       float *ascent, float *descent, float *leading) {
    (void)ctx;
    hnsoft_metrics(font, ascent, descent, leading);
}

/* ---------------- sys:// 桥(统一层, 与 hnweb_macos.c 同一份 KV 格式) ---------------- */

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    *len = got;
    return buf;
}

static char *store_path(const char *store_id) {
    const char *home = getenv("HOME");
    if (!home) return NULL;
    char *p = (char *)malloc(strlen(home) + strlen(store_id) + 64);
    if (!p) return NULL;
    sprintf(p, "%s/.html-native/store/%s.json", home, store_id);
    return p;
}

static int store_get(const char *file, const char *key, char *out, size_t cap) {
    size_t n = 0;
    char *d = read_file(file, &n);
    if (!d) return 0;
    char pat[256];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(d, pat);
    int found = 0;
    if (p && (p = strchr(p + strlen(pat), ':')) != NULL) {
        p++;
        while (*p == ' ') p++;
        if (*p == '"') {
            p++;
            size_t i = 0;
            while (*p && *p != '"' && i + 1 < cap) out[i++] = *p++;
            out[i] = 0;
            found = 1;
        }
    }
    free(d);
    return found;
}

static void store_set(const char *file, const char *key, const char *val) {
    char *obj = NULL;
    size_t obj_n = 0, cap = 4096;
    obj = (char *)malloc(cap);
    if (!obj) return;
    obj[0] = 0;
    int have_key = 0;
    size_t n = 0;
    char *d = read_file(file, &n);
    if (d) {
        const char *p = d;
        while (*p) {
            while (*p && (*p == ' ' || *p == '{' || *p == ',' || *p == '\n')) p++;
            if (*p != '"') break;
            const char *ks = ++p;
            while (*p && *p != '"') p++;
            size_t kl = (size_t)(p - ks);
            if (*p) p++;
            while (*p && *p != ':') p++;
            if (*p) p++;
            while (*p == ' ') p++;
            if (*p != '"') break;
            const char *vs = ++p;
            while (*p && *p != '"') p++;
            size_t vl = (size_t)(p - vs);
            if (*p) p++;
            int is_t = (kl == strlen(key) && !strncmp(ks, key, kl));
            const char *wv = is_t ? val : vs;
            size_t wl = is_t ? strlen(val) : vl;
            while (obj_n + kl + wl + 8 >= cap) { cap *= 2; obj = (char *)realloc(obj, cap); }
            if (!obj) { free(d); return; }
            if (obj_n) obj[obj_n++] = ',';
            obj_n += (size_t)snprintf(obj + obj_n, cap - obj_n, "\"%.*s\":\"%.*s\"",
                                      (int)kl, ks, (int)wl, wv);
            if (is_t) have_key = 1;
        }
        free(d);
    }
    if (!have_key) {
        while (obj_n + strlen(key) + strlen(val) + 8 >= cap) { cap *= 2; obj = (char *)realloc(obj, cap); }
        if (!obj) return;
        if (obj_n) obj[obj_n++] = ',';
        obj_n += (size_t)snprintf(obj + obj_n, cap - obj_n, "\"%s\":\"%s\"", key, val);
    }
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", file);
    char *sl = strrchr(dir, '/');
    if (sl) { *sl = 0; mkdir(dir, 0755); }
    FILE *f = fopen(file, "wb");
    if (f) { fprintf(f, "{%s}", obj); fclose(f); }
    free(obj);
}

static int qparam(const char *q, const char *key, char *out, size_t cap) {
    if (!q) return 0;
    size_t kl = strlen(key);
    const char *p = q;
    while (*p) {
        while (*p == '&' || *p == '?') p++;
        if (!strncmp(p, key, kl) && p[kl] == '=') {
            const char *v = p + kl + 1;
            size_t i = 0;
            while (*v && *v != '&' && i + 1 < cap) out[i++] = *v++;
            out[i] = 0;
            return 1;
        }
        while (*p && *p != '&') p++;
    }
    return 0;
}

static void esc_html(const char *s, char *out, size_t cap) {
    size_t i = 0;
    for (; *s && i + 7 < cap; s++) {
        switch (*s) {
        case '<': memcpy(out + i, "&lt;", 4); i += 4; break;
        case '>': memcpy(out + i, "&gt;", 4); i += 4; break;
        case '&': memcpy(out + i, "&amp;", 5); i += 5; break;
        default: out[i++] = *s;
        }
    }
    out[i] = 0;
}

/* sys:// → HTML 片段。与 hnweb_macos.c / hnwin.c 同一份语义(store KV), 这里是
   统一层 —— 三个壳的 sys_fragment 可以逐步收敛到这一份。 */
static char *sys_fragment(const char *url, const char *form_body, const char *store_id) {
    if (!url || strncmp(url, "sys://", 6)) return NULL;
    const char *route = url + 6;
    const char *q = strchr(route, '?');
    char key[256] = { 0 }, val[1024] = { 0 };
    char *out = (char *)malloc(4096);
    if (!out) return NULL;
    if (!strncmp(route, "store/get", 9)) {
        if (qparam(q, "key", key, sizeof(key)) || qparam(form_body, "key", key, sizeof(key))) {
            char *sp = store_path(store_id);
            char v[1024] = { 0 };
            int got = sp && store_get(sp, key, v, sizeof(v));
            if (sp) free(sp);
            char ev[2200];
            esc_html(got ? v : "", ev, sizeof(ev));
            snprintf(out, 4096, "<span id=\"store-value\">%s</span>", ev);
        } else {
            snprintf(out, 4096, "<span>missing key</span>");
        }
    } else if (!strncmp(route, "store/set", 9)) {
        int hk = qparam(q, "key", key, sizeof(key)) || qparam(form_body, "key", key, sizeof(key));
        int hv = qparam(q, "value", val, sizeof(val)) || qparam(form_body, "value", val, sizeof(val));
        if (hk && hv) {
            char *sp = store_path(store_id);
            if (sp) { store_set(sp, key, val); free(sp); }
            char ev[2200];
            esc_html(val, ev, sizeof(ev));
            snprintf(out, 4096, "<span id=\"store-value\">%s</span>", ev);
        } else {
            snprintf(out, 4096, "<span>missing key/value</span>");
        }
    } else if (!strncmp(route, "store/count", 11)) {
        snprintf(out, 4096, "<span>1</span>");
    } else if (!strncmp(route, "info", 4)) {
        snprintf(out, 4096,
                 "<div style=\"font-size:13;color:#e8eaf0\">html-native daemon (C99)</div>"
                 "<div style=\"font-size:11;color:#6b7386\">no webview, no swift, one engine</div>");
    } else {
        free(out);
        return NULL;
    }
    return out;
}

/* ---------------- hx-* 执行(与三壳同一套 htmx 语义) ---------------- */

static hn_node *hx_carrier(hn_node *n) {
    if (!n) return NULL;
    if (hn_node_attr(n, "hx-get") || hn_node_attr(n, "hx-post")) return n;
    hn_node *a = hn_node_ancestor_with_attr(n, "hx-get");
    hn_node *b = hn_node_ancestor_with_attr(n, "hx-post");
    return a ? a : b;
}

static int hx_perform(dapp *a, hn_node *carrier) {
    const char *post = hn_node_attr(carrier, "hx-post");
    const char *get = hn_node_attr(carrier, "hx-get");
    const char *url = post ? post : get;
    if (!url) return 0;
    hn_doc *doc = hn_context_doc(a->ctx);
    char form[4096];
    form[0] = 0;
    hn_doc_form_encode(doc, form, sizeof(form));
    char *frag = sys_fragment(url, form, g_store_id_default);
    if (!frag) return 0;
    const char *tid = hn_node_attr(carrier, "hx-target");
    if (!tid || !tid[0] || !strcmp(tid, "this")) tid = hn_node_attr(carrier, "id");
    if (!tid || !tid[0]) { free(frag); return 0; }
    const char *sm = hn_node_attr(carrier, "hx-swap");
    hn_swap_mode m = HN_SWAP_INNER;
    if (sm) {
        if (!strcmp(sm, "outerHTML")) m = HN_SWAP_OUTER;
        else if (!strcmp(sm, "append") || !strcmp(sm, "beforeend")) m = HN_SWAP_APPEND;
        else if (!strcmp(sm, "prepend") || !strcmp(sm, "afterbegin")) m = HN_SWAP_PREPEND;
    }
    int ok = hn_doc_swap(doc, tid, m, frag, strlen(frag));
    free(frag);
    if (ok) hn_context_layout(a->ctx, (float)a->w, (float)a->h, &g_tb);
    return ok;
}

/* ---------------- op 处理 ---------------- */

/* 回一个 JSON(转义 value 字段里的引号/反斜杠) */
static void resp_json(char *out, size_t cap, int ok, const char *body) {
    snprintf(out, cap, "%s%s%s}", ok ? "{\"ok\":true" : "{\"ok\":false",
             body && body[0] ? "," : "", body ? body : "");
}

static char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    *len = got;
    return buf;
}

static void handle_line(const char *line, char *out, size_t cap) {
    char op[64] = { 0 };
    const char *opp = json_find(line, "op");
    if (opp) {
        while (*opp == ' ' || *opp == '"') opp++;
        size_t i = 0;
        while (opp[i] && opp[i] != '"' && i + 1 < sizeof(op)) { op[i] = opp[i]; i++; }
        op[i] = 0;
    }
    if (!op[0]) { resp_json(out, cap, 0, "\"error\":\"no op\""); return; }

    if (!strcmp(op, "ping")) {
        resp_json(out, cap, 1, "\"engine\":\"html-native\",\"impl\":\"c99\",\"surfaces\":[\"headless\",\"window\"]");
        return;
    }

    if (!strcmp(op, "open")) {
        char *id = json_str(line, "id");
        char *html = json_str(line, "html");
        char *css = json_str(line, "css");
        char *title = json_str(line, "title");
        long w = json_num(line, "w", 480);
        long h = json_num(line, "h", 700);
        long headless = json_num(line, "headless", 1);
        if (!id || !html) {
            free(id); free(html); free(css); free(title);
            resp_json(out, cap, 0, "\"error\":\"id/html required\"");
            return;
        }
        dapp *a = app_create(id, (int)headless);
        if (!a) {
            free(id); free(html); free(css); free(title);
            resp_json(out, cap, 0, "\"error\":\"table full or dup id\"");
            return;
        }
        a->w = (int)w; a->h = (int)h;
        a->html = strdup(html);
        a->html_len = strlen(html);
        hn_doc *doc = hn_parse_html(html, strlen(html));
        if (!doc) {
            app_destroy(a);
            free(id); free(html); free(css); free(title);
            resp_json(out, cap, 0, "\"error\":\"parse failed\"");
            return;
        }
        hn_doc_autoid_hx(doc);
        hn_context_set_doc(a->ctx, doc);
        hn_manifest m;
        memset(&m, 0, sizeof(m));
        hn_doc_manifest(doc, &m);
        if (m.w > 0) a->w = m.w;
        if (m.h > 0) a->h = m.h;
        hn_context_layout(a->ctx, (float)a->w, (float)a->h, &g_tb);
        char body[256];
        snprintf(body, sizeof(body), "\"id\":\"%s\",\"w\":%d,\"h\":%d,\"headless\":%s",
                 a->id, a->w, a->h, a->headless ? "true" : "false");
        resp_json(out, cap, 1, body);
        free(id); free(html); free(css); free(title);
        return;
    }

    if (!strcmp(op, "list")) {
        char body[2048];
        size_t n = 0;
        n += (size_t)snprintf(body + n, cap - n, "\"apps\":[");
        int first = 1;
        for (int i = 0; i < g_app_n; i++) {
            if (!g_apps[i].alive) continue;
            n += (size_t)snprintf(body + n, cap - n, "%s{\"id\":\"%s\",\"w\":%d,\"h\":%d}",
                                  first ? "" : ",", g_apps[i].id, g_apps[i].w, g_apps[i].h);
            first = 0;
        }
        n += (size_t)snprintf(body + n, cap - n, "]");
        resp_json(out, cap, 1, body);
        return;
    }

    /* 需要 id 的 op 在这里统一解析一次 */
    char *id = json_str(line, "id");
    dapp *a = id ? app_find(id) : NULL;

    if (!strcmp(op, "update")) {
        if (!a) { free(id); resp_json(out, cap, 0, "\"error\":\"app not found\""); return; }
        char *html = json_str(line, "html");
        if (!html) { free(id); resp_json(out, cap, 0, "\"error\":\"html required\""); return; }
        hn_doc *nd = hn_parse_html(html, strlen(html));
        if (!nd) { free(html); free(id); resp_json(out, cap, 0, "\"error\":\"parse failed\""); return; }
        hn_doc_autoid_hx(nd);
        hn_context_set_doc(a->ctx, nd);
        hn_context_layout(a->ctx, (float)a->w, (float)a->h, &g_tb);
        free(a->html);
        a->html = strdup(html);
        a->html_len = strlen(html);
        free(html);
        free(id);
        resp_json(out, cap, 1, "");
        return;
    }
    if (!strcmp(op, "close")) {
        if (!a) { free(id); resp_json(out, cap, 0, "\"error\":\"app not found\""); return; }
        app_destroy(a);
        free(id);
        resp_json(out, cap, 1, "");
        return;
    }
    if (!a) { free(id); resp_json(out, cap, 0, "\"error\":\"app not found\""); return; }

    if (!strcmp(op, "dump")) {
        hn_context_layout(a->ctx, (float)a->w, (float)a->h, &g_tb);
        const hn_display_list *dl = hn_context_display_list(a->ctx);
        char body[128];
        snprintf(body, sizeof(body), "\"cmds\":%d", dl ? dl->count : 0);
        resp_json(out, cap, 1, body);
        return;
    }
    if (!strcmp(op, "text")) {
        char *el = json_str(line, "element");
        free(id);
        hn_doc *doc = hn_context_doc(a->ctx);
        hn_node *n = el ? hn_doc_find_by_id(doc, el) : NULL;
        char body[4128];
        if (n) {
            char txt[4096];
            size_t tn = hn_node_text_content(n, txt, sizeof(txt));
            txt[tn] = 0;
            char ev[4200];
            esc_html(txt, ev, sizeof(ev));
            snprintf(body, sizeof(body), "\"text\":\"%s\"", ev);
        } else {
            snprintf(body, sizeof(body), "\"text\":\"\"");
        }
        free(el);
        resp_json(out, cap, 1, body);
        return;
    }
    if (!strcmp(op, "event")) {
        free(id);
        char *kind = json_str(line, "kind");
        char *target = json_str(line, "target");
        hn_doc *doc = hn_context_doc(a->ctx);
        hn_node *n = target ? hn_doc_find_by_id(doc, target) : hn_doc_root(doc);
        int hit = 0;
        if (n) {
            hn_node *carrier = hx_carrier(n);
            if (carrier && kind && !strcmp(kind, "click")) hit = hx_perform(a, carrier);
            else hit = n != NULL;
        }
        char body[128];
        snprintf(body, sizeof(body), "\"consumed\":%s", hit ? "true" : "false");
        free(kind); free(target);
        resp_json(out, cap, 1, body);
        return;
    }
    if (!strcmp(op, "shot")) {
        free(id);
        char *p = json_str(line, "path");
        hn_context_layout(a->ctx, (float)a->w, (float)a->h, &g_tb);
        const hn_display_list *dl = hn_context_display_list(a->ctx);
        int rc = 1;
        if (dl) {
            unsigned char *px = hnsoft_render(dl, a->w, a->h, 0x0B0E13FFu);
            if (px) {
                size_t pn = 0;
                unsigned char *png = hnsoft_encode_png(px, a->w, a->h, &pn);
                if (png) {
                    FILE *f = p ? fopen(p, "wb") : NULL;
                    if (f) { fwrite(png, 1, pn, f); fclose(f); rc = 0; }
                    free(png);
                }
                free(px);
            }
        }
        free(p);
        resp_json(out, cap, rc == 0, rc == 0 ? "" : "\"error\":\"shot failed\"");
        return;
    }
    if (!strcmp(op, "anim")) {
        if (!a) { resp_json(out, cap, 0, "\"error\":\"app not found\""); return; }
        hn_context_layout(a->ctx, (float)a->w, (float)a->h, &g_tb);
        const hn_display_list *dl = hn_context_display_list(a->ctx);
        int anim_count = 0;
        if (dl) {
            /* 遍历显示列表, 统计有过渡/动画标记的指令 */
            for (int i = 0; i < dl->count; i++) {
                if (dl->cmds[i].kind == 1 /* RECT */ && dl->cmds[i].gradient) anim_count++;
            }
        }
        /* 引擎的动画时钟在 tick — 告知调用方动画系统在跑 */
        snprintf(out, cap, "{\"ok\":true,\"anim_active\":%s,\"cmds\":%d}",
                 hn_context_anim_tick(a->ctx, 16.0f) ? "true" : "false", dl ? dl->count : 0);
        (void)anim_count;
        return;
    }
    if (!strcmp(op, "applets")) {
        /* 轻应用槽位枚举(简化: 返回当前所有 headless 应用的 id) */
        char body[2048];
        size_t n = 0;
        n += (size_t)snprintf(body + n, cap - n, "\"apps\":[");
        int first = 1;
        for (int i = 0; i < g_app_n; i++) {
            if (!g_apps[i].alive) continue;
            n += (size_t)snprintf(body + n, cap - n, "%s{\"id\":\"%s\"}",
                                  first ? "" : ",", g_apps[i].id);
            first = 0;
        }
        n += (size_t)snprintf(body + n, cap - n, "]");
        resp_json(out, cap, 1, body);
        return;
    }
    if (!strcmp(op, "persist")) {
        if (!a) { resp_json(out, cap, 0, "\"error\":\"app not found\""); return; }
        char *p = json_str(line, "path");
        char path_buf[1024];
        if (!p) {
            snprintf(path_buf, sizeof(path_buf), "%s/.html-native/apps/%s.hnapp",
                     getenv("HOME") ? getenv("HOME") : ".", a->id);
            p = path_buf;
        }
        /* 持久化: 把当前 HTML + CSS 写进 JSON 胶囊 */
        hn_doc *doc = hn_context_doc(a->ctx);
        FILE *f = fopen(p, "wb");
        if (!f) { resp_json(out, cap, 0, "\"error\":\"cannot write\""); return; }
        /* 用 --shot 生成 PNG + JSON 元数据打包(简化为 HTML+meta) */
        const hn_display_list *dl = hn_context_display_list(a->ctx);
        fprintf(f, "{\"format\":\"hnapp\",\"version\":1,\"id\":\"%s\",\"w\":%d,\"h\":%d,\"html\":\"",
                a->id, a->w, a->h);
        {   /* JSON 字符串转义: 只需处理 \ 和 " 和控制字符 */
            size_t ci;
            for (ci = 0; ci < a->html_len; ci++) {
                unsigned char ch = (unsigned char)a->html[ci];
                if (ch == '"' || ch == 0x5C) fputc(0x5C, f);  /* " or \ */
                if (ch == '\n') fprintf(f, "\\n");
                else if (ch == '\t') fprintf(f, "\\t");
                else if (ch != '\r') fputc(ch, f);
            }
        }
        fprintf(f, "\"}");
        fclose(f);
        char body[512];
        snprintf(body, sizeof(body), "\"path\":\"%s\"", p);
        resp_json(out, cap, 1, body);
        return;
    }
    if (!strcmp(op, "restore")) {
        char *p = json_str(line, "path");
        if (!p) { resp_json(out, cap, 0, "\"error\":\"path required\""); return; }
        size_t n = 0;
        char *d = read_all(p, &n);
        free(p);
        if (!d) { resp_json(out, cap, 0, "\"error\":\"file not found\""); return; }
        /* 解析胶囊 JSON: 提取 id/html */
        char *rid = json_str(d, "id");
        char *rhtml = json_str(d, "html");
        free(d);
        if (!rid || !rhtml) { free(rid); free(rhtml); resp_json(out, cap, 0, "\"error\":\"bad capsule\""); return; }
        dapp *ra = app_create(rid, 1);
        if (ra) {
            ra->w = 480; ra->h = 700;
            hn_doc *nd = hn_parse_html(rhtml, strlen(rhtml));
            if (nd) {
                hn_doc_autoid_hx(nd);
                hn_context_set_doc(ra->ctx, nd);
                hn_context_layout(ra->ctx, (float)ra->w, (float)ra->h, &g_tb);
            }
        }
        free(rid); free(rhtml);
        resp_json(out, cap, ra != NULL, ra ? "" : "\"error\":\"restore failed\"");
        return;
    }
    if (!strcmp(op, "eval"))
        if (!strcmp(op, "eval")) {
        char *js = json_str(line, "js");
        if (!js) { resp_json(out, cap, 0, "\"error\":\"js required\""); return; }
        hn_doc *doc = hn_context_doc(a->ctx);
        if (!doc) fprintf(stderr, "[eval] doc=NULL\n");
        char *result = hn_rt_eval(js, id, doc);
        fprintf(stderr, "[eval] result=%s\n", result ? result : "(null)");
        free(js);
        if (result) {
            char body[4096];
            snprintf(body, sizeof(body), "\"value\":\"%s\"", result);
            free(result);
            resp_json(out, cap, 1, body);
        } else {
            resp_json(out, cap, 1, "\"value\":null");
        }
        /* eval 里的 hnSetText/hnSetValue 改了 DOM, 布局重排后 text_content
           才能反映 —— 不重排的话 text op 读到的是旧值。 */
        hn_context_layout(a->ctx, (float)a->w, (float)a->h, &g_tb);
        return;
    }

    snprintf(out, cap, "{\"ok\":false,\"error\":\"unknown op: %s\"}", op);
}

/* ---------------- Unix socket 服务 ---------------- */

static int g_listen = -1;
static volatile sig_atomic_t g_run = 1;
static void on_term(int sig) { (void)sig; g_run = 0; }

static int serve_line(int fd, const char *line, char *resp, size_t cap) {
    handle_line(line, resp, cap);
    size_t n = strlen(resp);
    size_t off = 0;
    while (off < n) {
        ssize_t w = write(fd, resp + off, n - off);
        if (w <= 0) return -1;
        off += (size_t)w;
    }
    return (ssize_t)write(fd, "\n", 1) == 1 ? 0 : -1;
}

static void serve(int cfd) {
    char buf[1 << 20];
    size_t n = 0;
    char resp[1 << 19];
    for (;;) {
        ssize_t r = read(cfd, buf + n, sizeof(buf) - n - 1);
        if (r <= 0) break;
        n += (size_t)r;
        buf[n] = 0;
        size_t start = 0;
        for (size_t i = 0; i < n; i++) {
            if (buf[i] == '\n') {
                buf[i] = 0;
                if (i > start) { if (serve_line(cfd, buf + start, resp, sizeof(resp)) != 0) { close(cfd); return; } }
                start = i + 1;
            }
        }
        if (start > 0) { memmove(buf, buf + start, n - start); n -= start; }
        if (n >= sizeof(buf) - 1) n = 0;   /* 单行超限: 丢弃防失控 */
    }
    close(cfd);
}

int main(int argc, char **argv) {
    const char *home = getenv("HOME");
    char sock[1024];
    if (argc > 2 && !strcmp(argv[1], "--socket")) {
        snprintf(sock, sizeof(sock), "%s", argv[2]);
    } else {
        snprintf(sock, sizeof(sock), "%s/.html-native/hn-daemon.sock", home ? home : ".");
    }
    signal(SIGINT, on_term);
    signal(SIGTERM, on_term);
    signal(SIGPIPE, SIG_IGN);

    g_tb.ctx = NULL; g_tb.measure = tb_measure; g_tb.metrics = tb_metrics;

    g_listen = socket(AF_UNIX, SOCK_STREAM, 0);
    if (g_listen < 0) { perror("socket"); return 1; }
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, sock, sizeof(addr.sun_path) - 1);
    unlink(sock);
    {
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s", sock);
        char *sl = strrchr(dir, '/');
        if (sl) { *sl = 0; mkdir(dir, 0755); }
    }
    if (bind(g_listen, (struct sockaddr *)&addr, sizeof(addr)) != 0) { perror("bind"); return 1; }
    if (listen(g_listen, 16) != 0) { perror("listen"); return 1; }
    fprintf(stderr, "hn_daemon(C99): listening %s\n", sock);

    while (g_run) {
        int cfd = accept(g_listen, NULL, NULL);
        if (cfd < 0) { if (g_run) continue; break; }
        serve(cfd);
    }
    close(g_listen);
    unlink(sock);
    for (int i = 0; i < g_app_n; i++)
        if (g_apps[i].alive) app_destroy(&g_apps[i]);
    return 0;
}

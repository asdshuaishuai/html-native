/* hn_pack.c — `hn pack` 实现(见 hn_pack.h 契约)
 *
 * 纯 C99 + hn_http(URL 拉取; dlopen libcurl, 失败时 URL 输入如实报错)
 * + hn_zip(胶囊 v2 写出)。HTML 扫描为**静态文本扫描**(不执行 JS、不做
 * DOM 树重构) —— 与"边界宣言: 不承诺任意网页"一致, 体检报告即机检结果。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <errno.h>
#include <time.h>
#ifdef _WIN32
#include <direct.h>   /* _mkdir */
#endif

#include "hn_pack.h"
#include "hn_zip.h"
#include "hn_http.h"  /* rt/ (运行时网络层; pack 经它拉 URL) */

/* ---- 小工具 ---- */

static char *pk_read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (n > 0 && fread(buf, 1, (size_t)n, f) != (size_t)n) {
        fclose(f); free(buf); return NULL;
    }
    fclose(f);
    buf[n] = 0;
    if (len) *len = (size_t)n;
    return buf;
}

static int pk_write_file(const char *path, const void *data, size_t n) {
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    if (n && fwrite(data, 1, n, f) != n) { fclose(f); return 0; }
    fclose(f);
    return 1;
}

static int pk_mkdir(const char *path) {
#ifdef _WIN32
    return _mkdir(path) == 0 || errno == EEXIST;
#else
    return mkdir(path, 0755) == 0;
#endif
}

static int pk_is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFDIR);
}

static char *pk_strdup(const char *s) {
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

/* URL 是否 http(s) */
static int pk_is_url(const char *s) {
    return !strncasecmp(s, "http://", 7) || !strncasecmp(s, "https://", 8);
}

/* 从 URL/路径取 basename(去 query/fragment) */
static void pk_basename(const char *ref, char *out, size_t cap) {
    const char *q = ref;
    const char *end = q + strlen(q);
    const char *cut = end;
    for (const char *p = q; p < end; p++)
        if (*p == '?' || *p == '#') { cut = p; break; }
    const char *slash = NULL;
    for (const char *p = q; p < cut; p++) if (*p == '/') slash = p;
    const char *name = slash ? slash + 1 : q;
    size_t n = (size_t)(cut - name);
    if (n == 0) { snprintf(out, cap, "index.html"); return; }
    if (n >= cap) n = cap - 1;
    memcpy(out, name, n);
    out[n] = 0;
}

/* URL 的"目录前缀"(用于相对引用解析): 最后一个 '/' 之前(含) */
static void pk_dir_of(const char *url_or_path, char *out, size_t cap) {
    snprintf(out, cap, "%s", url_or_path);
    char *sl = strrchr(out, '/');
    if (sl) sl[1] = 0;
    else out[0] = 0;
}

/* 相对引用 → 绝对(对 URL: 逐段解析 ../ 与 ./; 对文件: 目录拼接)。
   简化口径: 不处理协议相对(//host)与绝对路径(/x)以外的 URL 特例 ——
   /x 对 URL 输入按主机根拼接(记 base_host), 对文件输入按盘根(罕见, 原样)。 */
static void pk_resolve(const char *base_dir_or_url, const char *base_host,
                       const char *ref, char *out, size_t cap) {
    if (pk_is_url(ref)) { snprintf(out, cap, "%s", ref); return; }
    if (ref[0] == '/') {
        if (base_host[0]) snprintf(out, cap, "%s%s", base_host, ref);
        else snprintf(out, cap, "%s", ref);
        return;
    }
    /* 拼 base, 然后消化 ../ 与 ./ */
    char joined[2048];
    snprintf(joined, sizeof(joined), "%s%s", base_dir_or_url, ref);
    if (!pk_is_url(joined)) { snprintf(out, cap, "%s", joined); return; }
    /* URL: 手写归一 — authority 段(//host[:port])免疫 ../ 弹出;
       只归一其后的路径段(否则 host 被并进路径, 产出 http:///host/... ) */
    const char *proto = strstr(joined, "://");
    if (!proto) { snprintf(out, cap, "%s", joined); return; }
    const char *rest = proto + 3;
    const char *path0 = strchr(rest, '/');
    size_t alen = path0 ? (size_t)(path0 - rest) : strlen(rest);
    char head[512], authority[512];
    size_t hl = (size_t)(rest - joined);
    if (hl >= sizeof(head)) hl = sizeof(head) - 1;
    memcpy(head, joined, hl); head[hl] = 0;
    if (alen >= sizeof(authority)) alen = sizeof(authority) - 1;
    memcpy(authority, rest, alen); authority[alen] = 0;
    char norm[2048];
    size_t on = 0;
    char *parts[256];
    int np = 0;
    char buf[2048];
    snprintf(buf, sizeof(buf), "%s", path0 ? path0 + 1 : "");
    char *tok = strtok(buf, "/");
    while (tok) {
        if (!strcmp(tok, ".")) { /* skip */ }
        else if (!strcmp(tok, "..")) { if (np > 0) np--; }
        else { parts[np++] = tok; }
        tok = strtok(NULL, "/");
    }
    norm[0] = 0;
    for (int i = 0; i < np; i++) {
        size_t l = strlen(parts[i]);
        if (on + l + 2 >= sizeof(norm)) break;
        norm[on++] = '/';
        memcpy(norm + on, parts[i], l + 1);
        on += l;
    }
    if (on == 0) norm[on++] = '/';
    norm[on] = 0;
    snprintf(out, cap, "%s%s%s", head, authority, norm);   /* head 已含 // */
}

/* 拉取(URL)或读文件(本地) */
static unsigned char *pk_load(const char *url_or_path, size_t *len, long *status,
                              hn_pack_result *res) {
    if (pk_is_url(url_or_path)) {
        char current[2048];
        snprintf(current, sizeof(current), "%s", url_or_path);
        for (int hop = 0; hop < 5; hop++) {
            hn_http_response resp;
            memset(&resp, 0, sizeof(resp));
            hn_http_request_opts o;
            memset(&o, 0, sizeof(o));
            o.url = current;
            o.method = "GET";
            if (!hn_http_request(&o, &resp)) {
                if (status) *status = -1;
                if (res) snprintf(res->report, sizeof(res->report),
                                  "URL 拉取失败: %s (%s)", current, hn_http_last_error());
                return NULL;
            }
            if (resp.status >= 300 && resp.status < 400 && resp.headers) {
                /* Location 头(手找, 大小写不敏感) */
                const char *loc = NULL;
                for (const char *p = resp.headers; *p; ) {
                    const char *eol = strchr(p, '\n');
                    if (!strncasecmp(p, "location:", 9)) { loc = p + 9; break; }
                    if (!eol) break;
                    p = eol + 1;
                }
                if (loc) {
                    while (*loc == ' ') loc++;
                    char next[2048];
                    size_t nl = 0;
                    while (loc[nl] && loc[nl] != '\r' && loc[nl] != '\n' &&
                           nl < sizeof(next) - 1) { next[nl] = loc[nl]; nl++; }
                    next[nl] = 0;
                    char base[1024];
                    pk_dir_of(current, base, sizeof(base));
                    char full[2048];
                    if (pk_is_url(next)) snprintf(full, sizeof(full), "%s", next);
                    else if (next[0] == '/') {
                        /* 绝对路径: 拼 proto://host */
                        const char *pp2 = strstr(current, "://");
                        size_t hl = pp2 ? (size_t)(pp2 + 1 - current) : 0;
                        const char *slash = pp2 ? strchr(pp2 + 3, '/') : NULL;
                        if (slash) hl = (size_t)(slash - current);
                        snprintf(full, sizeof(full), "%.*s%s", (int)hl, current, next);
                    } else snprintf(full, sizeof(full), "%s%s", base, next);
                    hn_http_response_free(&resp);
                    snprintf(current, sizeof(current), "%s", full);
                    continue;
                }
            }
            if (status) *status = resp.status;
            if (resp.status >= 200 && resp.status < 300) {
                if (len) *len = resp.body_len;
                return resp.body;   /* 所有权转移 */
            }
            if (res) snprintf(res->report, sizeof(res->report),
                              "HTTP %ld: %s", resp.status, current);
            hn_http_response_free(&resp);
            if (status) *status = resp.status;
            return NULL;
        }
        if (status) *status = -1;
        return NULL;
    }
    if (status) *status = 200;
    return (unsigned char *)pk_read_file(url_or_path, len);
}

/* ---- 资产表 ---- */
typedef struct {
    char *ref;      /* 原引用(文档里出现的字面) */
    char *mapped;   /* 改写后的相对路径("assets/a001-x.png") */
    char *abs;      /* 解析后的绝对 URL/路径 */
} pk_asset;

typedef struct {
    pk_asset a[256];
    int n;
} pk_assets;

static const char *pk_assets_map(pk_assets *as, const char *ref,
                                 const char *base_dir, const char *base_host,
                                 const char *project_root, int *n_written,
                                 hn_pack_result *res) {
    /* data:/空/锚点: 原样 */
    if (!ref[0] || !strncasecmp(ref, "data:", 5) || ref[0] == '#')
        return ref;
    for (int i = 0; i < as->n; i++)
        if (!strcmp(as->a[i].ref, ref)) return as->a[i].mapped;
    if (as->n >= 256) return ref;
    pk_asset *e = &as->a[as->n];
    char abs[2048];
    pk_resolve(base_dir, base_host, ref, abs, sizeof(abs));
    char name[256];
    pk_basename(ref, name, sizeof(name));
    if (!name[0] || !strchr(name, '.')) snprintf(name, sizeof(name), "a%03d.bin", as->n);
    char mapped[300];
    snprintf(mapped, sizeof(mapped), "assets/a%03d-%s", as->n, name);
    /* 拉取/拷贝到 assets_dir/mapped */
    size_t len = 0;
    long st = 0;
    unsigned char *data = pk_load(abs, &len, &st, res);
    char full[1024];
    snprintf(full, sizeof(full), "%s/%s", project_root, mapped);   /* mapped 已含 assets/ 前缀 */
    if (data && pk_write_file(full, data, len)) {
        (*n_written)++;
        e->ref = pk_strdup(ref);
        e->mapped = pk_strdup(mapped);
        e->abs = pk_strdup(abs);
        as->n++;
        free(data);
        return e->mapped;
    }
    if (data) free(data);
    /* 拉取失败: 引用原样保留(报告里可见) */
    return ref;
}

/* ---- 兼容性体检 ---- */
typedef struct { const char *name; } pk_unsupported_tag;
static const pk_unsupported_tag UNSUP_TAGS[] = {
    { "iframe" }, { "frame" }, { "frameset" }, { "object" }, { "embed" },
    { "applet" }, { "svg" }, { "math" }, { "base" },
};
/* 被忽略的 CSS 特性(子集口径; 字面扫描, 大小写不敏感) */
typedef struct { const char *needle; const char *label; } pk_ignored_css;
static const pk_ignored_css IGN_CSS[] = {
    { "display:grid", "display:grid" },
    { "display: grid", "display: grid" },
    { "float:", "float" },
    { "position:sticky", "position:sticky" },
    { "position: sticky", "position:sticky" },
    { "backdrop-filter", "backdrop-filter" },
    { "writing-mode", "writing-mode" },
    { "columns:", "multi-column" },
    { "conic-gradient", "conic-gradient" },
    { "position:fixed", "position:fixed(canvas 外固定层)" },
};

static void pk_audit(const char *html, size_t n, char *report, size_t cap,
                     int *n_unsup, int *n_ign, int *n_scripts) {
    size_t used = 0;
    int unsup = 0, ign = 0, scripts = 0;
    #define PK_PUT(...) do { \
        if (used < cap - 1) used += (size_t)snprintf(report + used, cap - used, __VA_ARGS__); } while (0)
    PK_PUT("兼容性体检(引擎子集口径):\n");
    /* 元素扫描: 静态找 '<name' */
    for (size_t i = 0; i + 1 < n; i++) {
        if (html[i] != '<' || !isalpha((unsigned char)html[i + 1])) continue;
        char tag[32];
        size_t t = 0;
        size_t j = i + 1;
        while (j < n && (isalnum((unsigned char)html[j]) || html[j] == '-') &&
               t < sizeof(tag) - 1)
            tag[t++] = (char)tolower((unsigned char)html[j++]);
        tag[t] = 0;
        if (!strcmp(tag, "script")) scripts++;
        for (size_t u = 0; u < sizeof(UNSUP_TAGS) / sizeof(UNSUP_TAGS[0]); u++) {
            if (!strcmp(tag, UNSUP_TAGS[u].name)) {
                unsup++;
                PK_PUT("  ✗ 不支持元素 <%s>\n", tag);
            }
        }
    }
    /* CSS 特性扫描(<style> 与 style 属性一起被字面覆盖 —— 保守多报可接受) */
    for (size_t u = 0; u < sizeof(IGN_CSS) / sizeof(IGN_CSS[0]); u++) {
        size_t nl = strlen(IGN_CSS[u].needle);
        int count = 0;
        /* 大小写不敏感计数(简单双循环; 文档量级小, 可接受) */
        for (size_t k = 0; k + nl <= n; k++) {
            size_t kk = k;
            int ok = 1;
            for (size_t m2 = 0; m2 < nl; m2++, kk++) {
                if (tolower((unsigned char)html[kk]) !=
                    tolower((unsigned char)IGN_CSS[u].needle[m2])) { ok = 0; break; }
            }
            if (ok) count++;
        }
        if (count) {
            ign += count;
            PK_PUT("  ✗ 被忽略 CSS: %s ×%d\n", IGN_CSS[u].label, count);
        }
    }
    if (scripts) PK_PUT("  · <script> ×%d (wasm3 可用; QuickJS 视构建)\n", scripts);
    if (!unsup && !ign) PK_PUT("  ✓ 未发现子集外特性\n");
    *n_unsup = unsup;
    *n_ign = ign;
    (void)scripts;
    #undef PK_PUT
}

/* ---- HTML 改写(资产改写 + meta 注入) ---- */

/* 在 <head> 内注入 meta 行(head 结束前; 无 head 就插到 <body> 前或最前) */
static char *pk_inject_metas(const char *html, const char *metas) {
    size_t n = strlen(html);
    if (strstr(html, "</head>")) {
        /* 找 </head> */
        const char *close = strstr(html, "</head>");
        if (close) {
            size_t at = (size_t)(close - html);
            size_t mlen = strlen(metas);
            char *out = (char *)malloc(n + mlen + 2);
            if (!out) return NULL;
            memcpy(out, html, at);
            out[at] = 0;
            strcat(out, metas);
            strcat(out, "\n");
            strcat(out, html + at);
            return out;
        }
    }
    /* 回退: 整体前插 */
    size_t mlen = strlen(metas);
    char *out = (char *)malloc(n + mlen + 2);
    if (!out) return NULL;
    strcpy(out, metas);
    strcat(out, "\n");
    strcat(out, html);
    return out;
}

/* 对每个资产引用做字面替换(引用在文档里以 src/href="ref" 形态出现;
   双引号/单引号各试一遍) */
static char *pk_rewrite_refs(const char *html, pk_assets *as) {
    size_t cap = strlen(html) * 2 + 4096;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t used = 0;
    const char *p = html;
    while (*p) {
        int replaced = 0;
        for (int i = 0; i < as->n; i++) {
            const char *ref = as->a[i].ref;
            size_t rl = strlen(ref);
            if (strncmp(p, ref, rl) == 0) {
                /* 字面替换(子集页面的 src/href 引用形态单一, 误替率可接受) */
                size_t ml = strlen(as->a[i].mapped);
                if (used + ml + 1 >= cap) {
                    cap *= 2;
                    char *np = (char *)realloc(out, cap);
                    if (!np) { free(out); return NULL; }
                    out = np;
                }
                memcpy(out + used, as->a[i].mapped, ml);
                used += ml;
                p += rl;
                replaced = 1;
                break;
            }
        }
        if (!replaced) out[used++] = *p++;
    }
    out[used] = 0;
    return out;
}

int hn_pack_run(const hn_pack_opts *o, hn_pack_result *res) {
    memset(res, 0, sizeof(*res));
    if (!o || !o->input) { snprintf(res->report, sizeof(res->report), "缺输入"); return 1; }

    /* 1. 载入源 */
    size_t html_len = 0;
    long st = 0;
    char base_dir[1024] = "", base_host[512] = "";
    char source_loc[2048];
    snprintf(source_loc, sizeof(source_loc), "%s", o->input);
    if (pk_is_url(o->input)) {
        /* base_host = proto://host (绝对路径引用的解析基) */
        const char *pp = strstr(o->input, "://") + 3;
        size_t k2 = 0;
        while (pp[k2] && pp[k2] != '/' && k2 < sizeof(base_host) - 1) k2++;
        snprintf(base_host, sizeof(base_host), "%.*s%.*s",
                 (int)(pp - o->input), o->input, (int)k2, pp);
        pk_dir_of(o->input, base_dir, sizeof(base_dir));
    } else {
        pk_dir_of(o->input, base_dir, sizeof(base_dir));
        if (pk_is_dir(o->input)) {
            /* 目录: 取 index.html */
            snprintf(source_loc, sizeof(source_loc), "%s/index.html", o->input);
            pk_dir_of(source_loc, base_dir, sizeof(base_dir));
        }
    }
    unsigned char *html_u = pk_load(source_loc, &html_len, &st, res);
    if (!html_u) {
        snprintf(res->report, sizeof(res->report),
                 "载入失败: %s (status=%ld)", source_loc, st);
        return 2;
    }
    const char *html = (const char *)html_u;

    /* 2. id/title 推导 */
    char idbuf[128];
    if (o->id && o->id[0]) snprintf(idbuf, sizeof(idbuf), "%s", o->id);
    else if (pk_is_url(o->input) && base_host[0]) {
        /* host 去端口/点 → id */
        const char *hp = strstr(o->input, "://");
        hp = hp ? hp + 3 : o->input;
        size_t k = 0;
        while (hp[k] && hp[k] != '/' && hp[k] != ':' && k < sizeof(idbuf) - 1) {
            char ch = hp[k];
            idbuf[k] = (ch == '.') ? '-' : ch;
            k++;
        }
        idbuf[k] = 0;
    } else {
        char name[128];
        pk_basename(source_loc, name, sizeof(name));
        char *dot = strrchr(name, '.');
        if (dot) *dot = 0;
        snprintf(idbuf, sizeof(idbuf), "%s", name[0] ? name : "app");
    }

    /* 3. 兼容性体检 */
    int n_scripts = 0;
    pk_audit(html, html_len, res->report, sizeof(res->report),
             &res->n_unsupported_tags, &res->n_ignored_css, &n_scripts);

    /* 4. 资产收集目录 */
    const char *out_dir = o->out_dir && o->out_dir[0] ? o->out_dir : "";
    char dirbuf[1024];
    if (!out_dir[0]) snprintf(dirbuf, sizeof(dirbuf), "%s-project", idbuf);
    else snprintf(dirbuf, sizeof(dirbuf), "%s", out_dir);
    res->project_dir = pk_strdup(dirbuf);
    char assets_dir[1100];
    snprintf(assets_dir, sizeof(assets_dir), "%s/assets", dirbuf);
    if (!o->no_assets) {
        pk_mkdir(dirbuf);
        pk_mkdir(assets_dir);
    }

    /* 5. 资产收集 + 改写 */
    pk_assets as;
    memset(&as, 0, sizeof(as));
    int n_written = 0;
    if (!o->no_assets) {
        /* 扫 src=/href= 引用(双引号优先, 单引号同式; v1 只收带扩展名的) */
        const char *p = html;
        while (*p) {
            const char *attr = NULL;
            if (!strncasecmp(p, "src", 3) && (p[3] == '=' || p[3] == ' ')) attr = p + 3;
            else if (!strncasecmp(p, "href", 4) && (p[4] == '=' || p[4] == ' ')) attr = p + 4;
            if (attr) {
                while (*attr == ' ') attr++;
                if (*attr == '=') {
                    attr++;
                    while (*attr == ' ') attr++;
                    char q = *attr;
                    if (q == '"' || q == '\'') {
                        attr++;
                        const char *e = attr;
                        while (*e && *e != q) e++;
                        char ref[1024];
                        size_t rl = (size_t)(e - attr);
                        if (rl >= sizeof(ref)) rl = sizeof(ref) - 1;
                        memcpy(ref, attr, rl); ref[rl] = 0;
                        if (!pk_is_url(ref) || pk_is_url(o->input)) {
                            /* 相对引用, 或 URL 页内的同源绝对引用 */
                            const char *mapped = pk_assets_map(
                                &as, ref, base_dir, base_host, dirbuf, &n_written, res);
                            (void)mapped;
                        }
                        p = e;
                    }
                }
            }
            p++;
        }
    }
    res->n_assets = as.n;
    char *rewritten = NULL;
    if (!o->no_assets && as.n > 0) {
        rewritten = pk_rewrite_refs(html, &as);
        res->n_rewritten = as.n;
        html = rewritten ? rewritten : html;
    }

    /* 6. meta 注入 */
    char metas[1024];
    {
        const char *title = o->title && o->title[0] ? o->title : idbuf;
        snprintf(metas, sizeof(metas),
                 "<meta name=\"hn-window\" content=\"%dx%d\">"
                 "<meta name=\"hn-title\" content=\"%s\">",
                 o->w > 0 ? o->w : 480, o->h > 0 ? o->h : 700, title);
    }
    char *final_html = pk_inject_metas(html, metas);

    /* 7. 写项目目录 */
    {
        char pj[1024], path[1100];
        snprintf(path, sizeof(path), "%s/project.json", dirbuf);
        snprintf(pj, sizeof(pj),
                 "{\n  \"name\": \"%s\",\n  \"entry\": \"index.html\"\n}\n", idbuf);
        pk_write_file(path, pj, strlen(pj));
        snprintf(path, sizeof(path), "%s/index.html", dirbuf);
        pk_write_file(path, final_html, strlen(final_html));
        snprintf(path, sizeof(path), "%s/audit.txt", dirbuf);
        pk_write_file(path, res->report, strlen(res->report));
    }

    /* 8. 胶囊(可选) */
    if (o->capsule && o->capsule[0]) {
        hn_zip_member m[96];
        int nm = 0;
        char manifest[512];
        snprintf(manifest, sizeof(manifest),
                 "{\"format\":\"hnapp\",\"version\":2,\"id\":\"%s\",\"w\":%d,"
                 "\"h\":%d,\"entry\":\"index.html\"}", idbuf,
                 o->w > 0 ? o->w : 480, o->h > 0 ? o->h : 700);
        m[nm].name = "manifest.json";
        m[nm].data = (const unsigned char *)manifest;
        m[nm].n = strlen(manifest);
        nm++;
        m[nm].name = "index.html";
        m[nm].data = (const unsigned char *)final_html;
        m[nm].n = strlen(final_html);
        nm++;
        for (int i = 0; i < as.n && nm < 95; i++) {
            char zpath[320];
            snprintf(zpath, sizeof(zpath), "%s", as.a[i].mapped);
            size_t len = 0;
            char abs[2048];
            snprintf(abs, sizeof(abs), "%s/%s", dirbuf, as.a[i].mapped);   /* 项目根(mapped 已含 assets/) */
            unsigned char *data = pk_load(abs, &len, &st, res);
            if (!data) continue;
            char *namez = (char *)malloc(strlen(zpath) + 1);
            strcpy(namez, zpath);
            m[nm].name = namez;
            m[nm].data = data;
            m[nm].n = len;
            nm++;
        }
        FILE *f = fopen(o->capsule, "wb");
        if (f) {
            if (hn_zip_write(f, m, nm) == 0) {
                res->capsule_path = pk_strdup(o->capsule);
            }
            fclose(f);
        }
        /* m 里 data 指针多数指向已释放/栈数据已写出 ✓(hn_zip_write 同步写) */
    }

    free(final_html);
    if (rewritten) free(rewritten);
    free(html_u);
    return 0;
}

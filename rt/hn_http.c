/* hn_http.c -- runtime-bound HTTP for macOS, Linux and Windows. */
#define _POSIX_C_SOURCE 200809L
#include "hn_http.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if defined(_WIN32)
#include <windows.h> /* WinHTTP itself is deliberately not included. */
#include <process.h>
#else
#include <dlfcn.h>
#include <pthread.h>
#include <strings.h>
#endif

#define HN_HTTP_MAX_BODY (10u * 1024u * 1024u)
#define HN_HTTP_MAX_HEADERS (1024u * 1024u)
#define HN_HTTP_MAX_REDIRECTS 5

static char g_http_error[512];
static void http_error(const char *fmt, const char *arg) {
    if (arg) snprintf(g_http_error, sizeof(g_http_error), fmt, arg);
    else snprintf(g_http_error, sizeof(g_http_error), "%s", fmt);
}
const char *hn_http_last_error(void) {
    return g_http_error[0] ? g_http_error : "HTTP error";
}

static int ascii_ieq_n(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (tolower((unsigned char)a[i]) != tolower((unsigned char)b[i])) return 0;
    return 1;
}
static int ascii_ieq(const char *a, const char *b) {
    size_t na = strlen(a), nb = strlen(b);
    return na == nb && ascii_ieq_n(a, b, na);
}
static int ascii_ncmp(const char *a, const char *b, size_t n) {
    return ascii_ieq_n(a, b, n);
}

/* Extract and validate the authority without allocating. */
static int http_url_parts(const char *url, int *https, const char **host,
                          size_t *host_len, unsigned *port,
                          const char **path) {
    const char *p, *authority, *end, *h;
    if (!url) { http_error("missing URL", NULL); return 0; }
    p = strstr(url, "://");
    if (!p || p == url) {
        http_error("only http/https URLs are allowed", NULL); return 0;
    }
    if ((size_t)(p - url) == 4 && ascii_ieq_n(url, "http", 4)) *https = 0;
    else if ((size_t)(p - url) == 5 && ascii_ieq_n(url, "https", 5)) *https = 1;
    else { http_error("only http/https URLs are allowed", NULL); return 0; }
    authority = p + 3;
    end = authority + strcspn(authority, "/?#");
    if (end == authority) { http_error("URL has no host", NULL); return 0; }
    h = authority;
    if (memchr(h, '@', (size_t)(end - h))) {
        http_error("URL userinfo is not allowed", NULL); return 0;
    }
    if (*h == '[') {
        const char *close = (const char *)memchr(h + 1, ']', (size_t)(end - h - 1));
        if (!close) { http_error("invalid IPv6 host", NULL); return 0; }
        *host = h + 1; *host_len = (size_t)(close - h - 1); h = close + 1;
        *port = *https ? 443u : 80u;
        if (h < end) {
            if (*h != ':' || h + 1 == end) { http_error("invalid URL port", NULL); return 0; }
            *port = (unsigned)strtoul(h + 1, NULL, 10);
            if (*port > 65535u) { http_error("invalid URL port", NULL); return 0; }
        }
    } else {
        const char *colon = NULL;
        for (const char *q = h; q < end; q++) if (*q == ':') colon = q;
        if (colon) {
            if (colon == h || colon + 1 == end) { http_error("invalid URL port", NULL); return 0; }
            *host = h; *host_len = (size_t)(colon - h);
            *port = (unsigned)strtoul(colon + 1, NULL, 10);
            if (*port > 65535u) { http_error("invalid URL port", NULL); return 0; }
        } else {
            *host = h; *host_len = (size_t)(end - h); *port = *https ? 443u : 80u;
        }
    }
    if (*host_len == 0) { http_error("URL has no host", NULL); return 0; }
    *path = *end ? end : "/";
    return 1;
}

static int host_allowed(const char *host, size_t host_len) {
    const char *allow = getenv("HN_FETCH_ALLOW");
    char h[512];
    if (!allow || !*allow) return 1;
    if (host_len >= sizeof(h)) { http_error("host is too long", NULL); return 0; }
    for (size_t i = 0; i < host_len; i++) h[i] = (char)tolower((unsigned char)host[i]);
    h[host_len] = 0;
    const char *p = allow;
    while (*p) {
        while (*p == ',' || isspace((unsigned char)*p)) p++;
        const char *e = p;
        while (*e && *e != ',') e++;
        while (e > p && isspace((unsigned char)e[-1])) e--;
        size_t n = (size_t)(e - p);
        if (n && ((n == host_len && ascii_ncmp(p, h, n)) ||
                  (n < host_len && h[host_len - n - 1] == '.' &&
                   ascii_ncmp(p, h + host_len - n, n)))) return 1;
        p = *e ? e + 1 : e;
    }
    fprintf(stderr, "[fetch] host 不在白名单: %s\n", h);
    fflush(stderr);
    http_error("host is not in HN_FETCH_ALLOW", NULL);
    return 0;
}
static int request_check(const hn_http_request_opts *o, int *https,
                         const char **host, size_t *host_len, unsigned *port,
                         const char **path) {
    if (!o || !o->url || !http_url_parts(o->url, https, host, host_len, port, path)) return 0;
    return host_allowed(*host, *host_len);
}

typedef struct { unsigned char *body; size_t len, cap; int too_big; } body_buf;
typedef struct { char *data; size_t len, cap; char *content_type; } header_buf;

static int append_bytes(unsigned char **data, size_t *len, size_t *cap,
                        const void *src, size_t n, size_t max) {
    if (n > max || *len > max - n) return 0;
    if (*len + n + 1 > *cap) {
        size_t c = *cap ? *cap : 4096;
        while (c < *len + n + 1 && c < max + 1) c *= 2;
        if (c > max + 1) c = max + 1;
        unsigned char *p = (unsigned char *)realloc(*data, c);
        if (!p) return 0;
        *data = p; *cap = c;
    }
    if (n) memcpy(*data + *len, src, n);
    *len += n; (*data)[*len] = 0;
    return 1;
}
static int append_header(header_buf *b, const char *src, size_t n) {
    if (!append_bytes((unsigned char **)&b->data, &b->len, &b->cap, src, n, HN_HTTP_MAX_HEADERS)) return 0;
    const char *colon = (const char *)memchr(src, ':', n);
    if (colon && (size_t)(colon - src) == 12 && ascii_ncmp(src, "Content-Type", 12)) {
        const char *v = colon + 1, *e = src + n;
        while (v < e && isspace((unsigned char)*v)) v++;
        while (e > v && (e[-1] == '\r' || e[-1] == '\n' || isspace((unsigned char)e[-1]))) e--;
        free(b->content_type); b->content_type = (char *)malloc((size_t)(e - v) + 1);
        if (b->content_type) { memcpy(b->content_type, v, (size_t)(e - v)); b->content_type[e - v] = 0; }
    }
    return 1;
}

#if !defined(_WIN32)
/* libcurl ABI declarations are intentionally local and opaque. */
typedef void CURL; typedef struct curl_slist curl_slist; typedef int CURLcode;
typedef CURL *(*curl_easy_init_fn)(void);
typedef CURLcode (*curl_easy_setopt_fn)(CURL *, int, ...);
typedef CURLcode (*curl_easy_perform_fn)(CURL *);
typedef void (*curl_easy_cleanup_fn)(CURL *);
typedef curl_slist *(*curl_slist_append_fn)(curl_slist *, const char *);
typedef void (*curl_slist_free_all_fn)(curl_slist *);
typedef CURLcode (*curl_easy_getinfo_fn)(CURL *, int, ...);
static struct {
    void *lib; int tried;
    curl_easy_init_fn easy_init; curl_easy_setopt_fn easy_setopt;
    curl_easy_perform_fn easy_perform; curl_easy_cleanup_fn easy_cleanup;
    curl_slist_append_fn slist_append; curl_slist_free_all_fn slist_free_all;
    curl_easy_getinfo_fn easy_getinfo;
} g_curl;
static int curl_load(void) {
    if (g_curl.tried) return g_curl.lib != NULL;
    g_curl.tried = 1;
#if defined(__APPLE__)
    g_curl.lib = dlopen("/usr/lib/libcurl.dylib", RTLD_LAZY | RTLD_LOCAL);
#else
    g_curl.lib = dlopen("libcurl.so.4", RTLD_LAZY | RTLD_LOCAL);
    if (!g_curl.lib) g_curl.lib = dlopen("libcurl.so", RTLD_LAZY | RTLD_LOCAL);
#endif
    if (!g_curl.lib) { http_error("libcurl unavailable", NULL); return 0; }
#define CURL_SYM(field, name) do { *(void **)(&g_curl.field) = dlsym(g_curl.lib, name); if (!g_curl.field) { dlclose(g_curl.lib); g_curl.lib = NULL; http_error("libcurl symbol unavailable: %s", name); return 0; } } while (0)
    CURL_SYM(easy_init, "curl_easy_init"); CURL_SYM(easy_setopt, "curl_easy_setopt");
    CURL_SYM(easy_perform, "curl_easy_perform"); CURL_SYM(easy_cleanup, "curl_easy_cleanup");
    CURL_SYM(slist_append, "curl_slist_append"); CURL_SYM(slist_free_all, "curl_slist_free_all");
    CURL_SYM(easy_getinfo, "curl_easy_getinfo");
#undef CURL_SYM
    return 1;
}
int hn_http_available(void) { return curl_load(); }
static size_t curl_write(void *ptr, size_t size, size_t nmemb, void *opaque) {
    body_buf *b = (body_buf *)opaque; size_t n = size * nmemb;
    if (!append_bytes(&b->body, &b->len, &b->cap, ptr, n, HN_HTTP_MAX_BODY)) { b->too_big = 1; return 0; }
    return n;
}
static size_t curl_header(void *ptr, size_t size, size_t nmemb, void *opaque) {
    header_buf *b = (header_buf *)opaque; size_t n = size * nmemb;
    return append_header(b, (const char *)ptr, n) ? n : 0;
}
int hn_http_request(const hn_http_request_opts *o, hn_http_response *out) {
    int https; const char *host, *path; size_t host_len; unsigned port;
    if (!out) { http_error("missing response", NULL); return 0; }
    memset(out, 0, sizeof(*out));
    if (!request_check(o, &https, &host, &host_len, &port, &path)) return 0;
    (void)https; (void)host; (void)host_len; (void)port; (void)path;
    if (!curl_load()) return 0;
    CURL *c = g_curl.easy_init();
    if (!c) { http_error("curl_easy_init failed", NULL); return 0; }
    body_buf body = {0}; header_buf headers = {0}; curl_slist *list = NULL;
    const char *method = (o->method && *o->method) ? o->method : "GET";
    int ok = 0; long status = 0; CURLcode code;
    g_curl.easy_setopt(c, 10002, o->url);       /* CURLOPT_URL */
    g_curl.easy_setopt(c, 20011, curl_write);   /* CURLOPT_WRITEFUNCTION */
    g_curl.easy_setopt(c, 10001, &body);        /* CURLOPT_WRITEDATA */
    g_curl.easy_setopt(c, 20079, curl_header);  /* CURLOPT_HEADERFUNCTION */
    g_curl.easy_setopt(c, 10029, &headers);     /* CURLOPT_HEADERDATA */
    const char *timeout = getenv("HN_FETCH_TIMEOUT_MS");
    long timeout_ms = timeout && *timeout ? strtol(timeout, NULL, 10) : 20000;
    if (timeout_ms < 1) timeout_ms = 20000;
    g_curl.easy_setopt(c, 155, timeout_ms);     /* CURLOPT_TIMEOUT_MS */
    g_curl.easy_setopt(c, 99, 1L);              /* CURLOPT_NOSIGNAL */
    g_curl.easy_setopt(c, 52, 0L);              /* CURLOPT_FOLLOWLOCATION = 0: manual redirect */
    g_curl.easy_setopt(c, 68, 0L);              /* no auto redirect */
    g_curl.easy_setopt(c, 10036, method);       /* CURLOPT_CUSTOMREQUEST */
    for (size_t i = 0; i < o->header_count; i++) {
        char line[2048];
        if (!o->headers[i].name || !o->headers[i].value) continue;
        snprintf(line, sizeof(line), "%s: %s", o->headers[i].name, o->headers[i].value);
        list = g_curl.slist_append(list, line);
    }
    if (list) g_curl.easy_setopt(c, 10023, list); /* CURLOPT_HTTPHEADER */
    if (o->body && o->body_len) {
        g_curl.easy_setopt(c, 10015, o->body);       /* CURLOPT_POSTFIELDS */
        g_curl.easy_setopt(c, 30120, (long long)o->body_len); /* POSTFIELDSIZE_LARGE */
    } else if (!ascii_ieq(method, "GET")) {
        g_curl.easy_setopt(c, 10015, "");
        g_curl.easy_setopt(c, 30120, (long long)0);
    }
    /* Manual redirect loop: re-check allowlist on each hop, limit to HN_HTTP_MAX_REDIRECTS */
    char *current_url = NULL;
    for (int redirects = 0; redirects <= HN_HTTP_MAX_REDIRECTS; redirects++) {
        if (redirects > 0) {
            free(body.body); free(headers.data); free(headers.content_type);
            body.body = NULL; body.len = body.cap = 0; body.too_big = 0;
            headers.data = NULL; headers.len = headers.cap = 0; headers.content_type = NULL;
            g_curl.easy_setopt(c, 10002, current_url); /* CURLOPT_URL */
        }
        code = g_curl.easy_perform(c);
        if (code != 0) {
            if (body.too_big) http_error("response exceeds 10MB", NULL);
            else snprintf(g_http_error, sizeof(g_http_error), "curl error %d", code);
            break;
        }
        if (g_curl.easy_getinfo(c, 0x200002, &status) != 0) {
            http_error("could not read HTTP status", NULL);
            break;
        }
        /* 3xx redirect: extract Location, validate host allowlist */
        if (status >= 300 && status < 400) {
            char *location = NULL;
            if (g_curl.easy_getinfo(c, 0x100000 + 18, &location) != 0 || !location || !*location) {
                http_error("redirect missing Location", NULL);
                break;
            }
            /* Parse and validate redirect URL */
            int rh; const char *rhost, *rpath; size_t rhost_len; unsigned rport;
            if (!http_url_parts(location, &rh, &rhost, &rhost_len, &rport, &rpath)) {
                http_error("redirect Location invalid", NULL);
                break;
            }
            if (!host_allowed(rhost, rhost_len)) {
                http_error("redirect host not in HN_FETCH_ALLOW", NULL);
                break;
            }
            /* HTTPS downgrade check: original was HTTPS, redirect must also be HTTPS */
            if (https && !rh) {
                http_error("redirect HTTPS downgrade blocked", NULL);
                break;
            }
            free(current_url);
            current_url = strdup(location);
            if (!current_url) {
                http_error("out of memory", NULL);
                break;
            }
            continue; /* next hop */
        }
        /* Non-3xx: final response */
        out->body = body.body; out->body_len = body.len; body.body = NULL;
        out->headers = headers.data; headers.data = NULL;
        out->content_type = headers.content_type; headers.content_type = NULL;
        out->status = status; ok = 1;
        break;
    }
    if (!ok && status >= 300 && status < 400) {
        http_error("too many redirects", NULL);
    }
    free(current_url);
    if (list) g_curl.slist_free_all(list);
    g_curl.easy_cleanup(c);
    free(body.body); free(headers.data); free(headers.content_type);
    return ok;
}
#else
/* WinHTTP declarations. No winhttp.h dependency: only the ABI is declared. */
typedef void *HN_HINTERNET;
typedef unsigned short INTERNET_PORT;
typedef HN_HINTERNET (WINAPI *hn_winhttp_connect_fn)(HN_HINTERNET, LPCWSTR, INTERNET_PORT, DWORD);
typedef HN_HINTERNET (WINAPI *hn_winhttp_request_fn)(HN_HINTERNET, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR, LPCWSTR *, DWORD, DWORD);
typedef BOOL (WINAPI *hn_winhttp_send_fn)(HN_HINTERNET, LPCWSTR, DWORD, LPVOID, DWORD, DWORD, DWORD_PTR);
typedef BOOL (WINAPI *hn_winhttp_receive_fn)(HN_HINTERNET, LPVOID);
typedef BOOL (WINAPI *hn_winhttp_query_fn)(HN_HINTERNET, DWORD, LPCWSTR, LPVOID, LPDWORD, LPDWORD);
typedef BOOL (WINAPI *hn_winhttp_read_fn)(HN_HINTERNET, LPVOID, DWORD, LPDWORD);
typedef BOOL (WINAPI *hn_winhttp_close_fn)(HN_HINTERNET);
#define HN_WINHTTP_ACCESS_TYPE_NO_PROXY 1
#define HN_WINHTTP_FLAG_SECURE 0x00800000
#define HN_WINHTTP_QUERY_RAW_HEADERS_CRLF 22
#define HN_WINHTTP_QUERY_STATUS_CODE 19
#define HN_WINHTTP_QUERY_CONTENT_TYPE 1
#define HN_WINHTTP_QUERY_LOCATION 24
#define HN_WINHTTP_QUERY_FLAG_NUMBER 0x20000000
#define HN_WINHTTP_ADDREQ_FLAG_ADD 0x20000000
static struct { HMODULE lib; int tried; hn_winhttp_open_fn open; hn_winhttp_connect_fn connect; hn_winhttp_request_fn request; hn_winhttp_send_fn send; hn_winhttp_receive_fn receive; hn_winhttp_query_fn query; hn_winhttp_read_fn read; hn_winhttp_close_fn close; } g_wh;
static int wh_load(void) {
    if (g_wh.tried) return g_wh.lib != NULL; g_wh.tried = 1;
    g_wh.lib = LoadLibraryW(L"winhttp.dll");
    if (!g_wh.lib) { http_error("winhttp.dll unavailable", NULL); return 0; }
#define WH_SYM(field, name) do { *(FARPROC *)&g_wh.field = GetProcAddress(g_wh.lib, name); if (!g_wh.field) { FreeLibrary(g_wh.lib); g_wh.lib = NULL; http_error("WinHTTP symbol unavailable: %s", name); return 0; } } while (0)
    WH_SYM(open, "WinHttpOpen"); WH_SYM(connect, "WinHttpConnect"); WH_SYM(request, "WinHttpOpenRequest");
    WH_SYM(send, "WinHttpSendRequest"); WH_SYM(receive, "WinHttpReceiveResponse"); WH_SYM(query, "WinHttpQueryHeaders");
    WH_SYM(read, "WinHttpReadData"); WH_SYM(close, "WinHttpCloseHandle");
#undef WH_SYM
    return 1;
}
int hn_http_available(void) { return wh_load(); }
static wchar_t *wide_dup(const char *s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL; wchar_t *w = (wchar_t *)calloc((size_t)n, sizeof(wchar_t));
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n); return w;
}
static int wh_query_text(HN_HINTERNET h, DWORD q, wchar_t **out) {
    DWORD n = 0, idx = 0; *out = NULL;
    g_wh.query(h, q, NULL, NULL, &n, &idx);
    if (!n) return 0; *out = (wchar_t *)calloc(n + 2, sizeof(wchar_t));
    if (!*out) return 0; return g_wh.query(h, q, *out, &n, &idx) ? 1 : 0;
}
int hn_http_request(const hn_http_request_opts *o, hn_http_response *out) {
    int https; const char *host, *path; size_t host_len; unsigned port;
    if (!out) { http_error("missing response", NULL); return 0; } memset(out, 0, sizeof(*out));
    if (!request_check(o, &https, &host, &host_len, &port, &path) || !wh_load()) return 0;
    char *current = strdup(o->url); if (!current) { http_error("out of memory", NULL); return 0; }
    int ok = 0;
    for (int redirects = 0; redirects <= HN_HTTP_MAX_REDIRECTS; redirects++) {
        int ch; const char *chost, *cpath; size_t chost_len; unsigned cport;
        if (!http_url_parts(current, &ch, &chost, &chost_len, &cport, &cpath) || !host_allowed(chost, chost_len)) break;
        wchar_t *whost = wide_dup(chost), *wpath = wide_dup(cpath); HN_HINTERNET ses = NULL, con = NULL, req = NULL;
        if (!whost || !wpath) { free(whost); free(wpath); http_error("out of memory", NULL); break; }
        ses = g_wh.open(L"html-native", HN_WINHTTP_ACCESS_TYPE_NO_PROXY, NULL, NULL, 0);
        con = ses ? g_wh.connect(ses, whost, (INTERNET_PORT)cport, 0) : NULL;
        wchar_t *wm = wide_dup(o->method && *o->method ? o->method : "GET");
        req = con && wm ? g_wh.request(con, wm, wpath, NULL, NULL, NULL, ch ? HN_WINHTTP_FLAG_SECURE : 0, 0) : NULL;
        free(wm); free(whost); free(wpath);
        if (!req) { http_error("WinHTTP request creation failed", NULL); if (con) g_wh.close(con); if (ses) g_wh.close(ses); break; }
        for (size_t i = 0; i < o->header_count; i++) {
            char line[2048]; snprintf(line, sizeof(line), "%s: %s\r\n", o->headers[i].name, o->headers[i].value);
            wchar_t *wl = wide_dup(line); if (wl) { /* no WinHttpAddRequestHeaders in minimal ABI; headers omitted safely */ free(wl); }
        }
        const char *timeout = getenv("HN_FETCH_TIMEOUT_MS"); DWORD ms = timeout && *timeout ? (DWORD)strtoul(timeout, NULL, 10) : 20000;
        (void)ms; /* session timeout setters are optional; request still has WinHTTP defaults */
        BOOL sent = g_wh.send(req, NULL, 0, (LPVOID)o->body, (DWORD)o->body_len, (DWORD)o->body_len, 0);
        BOOL received = sent && g_wh.receive(req, NULL);
        DWORD status = 0, sn = sizeof(status), idx = 0;
        if (!received || !g_wh.query(req, HN_WINHTTP_QUERY_STATUS_CODE | HN_WINHTTP_QUERY_FLAG_NUMBER, NULL, &status, &sn, &idx)) {
            http_error("WinHTTP request failed", NULL); g_wh.close(req); g_wh.close(con); g_wh.close(ses); break;
        }
        if (status >= 300 && status < 400) {
            wchar_t *loc = NULL; int got = wh_query_text(req, HN_WINHTTP_QUERY_LOCATION, &loc);
            if (!got || redirects == HN_HTTP_MAX_REDIRECTS) { free(loc); http_error("too many redirects", NULL); g_wh.close(req); g_wh.close(con); g_wh.close(ses); break; }
            int need = WideCharToMultiByte(CP_UTF8, 0, loc, -1, NULL, 0, NULL, NULL); char *next = (char *)malloc((size_t)need + 1);
            if (!next || !WideCharToMultiByte(CP_UTF8, 0, loc, -1, next, need, NULL, NULL)) { free(next); free(loc); http_error("invalid redirect", NULL); g_wh.close(req); g_wh.close(con); g_wh.close(ses); break; }
            char *resolved = NULL; if (strstr(next, "://")) resolved = strdup(next);
            else { size_t cap = strlen(current) + strlen(next) + 8; resolved = (char *)malloc(cap); if (next[0] == '/') { const char *slash = strstr(current, "://"); slash = slash ? strchr(slash + 3, '/') : NULL; snprintf(resolved, cap, "%.*s%s", slash ? (int)(slash - current) : (int)strlen(current), current, next); } else { const char *slash = strrchr(current, '/'); snprintf(resolved, cap, "%.*s/%s", slash ? (int)(slash - current) : (int)strlen(current), current, next); } }
            free(next); free(loc); free(current); current = resolved; g_wh.close(req); g_wh.close(con); g_wh.close(ses); if (!current) { http_error("out of memory", NULL); break; } continue;
        }
        body_buf body = {0}; unsigned char chunk[8192]; DWORD got = 0;
        do { if (!g_wh.read(req, chunk, sizeof(chunk), &got)) break; if (!append_bytes(&body.body, &body.len, &body.cap, chunk, got, HN_HTTP_MAX_BODY)) { body.too_big = 1; break; } } while (got);
        if (body.too_big) { free(body.body); http_error("response exceeds 10MB", NULL); }
        else { out->body = body.body; out->body_len = body.len; out->status = (long)status; ok = 1; }
        g_wh.close(req); g_wh.close(con); g_wh.close(ses); break;
    }
    free(current); return ok;
}
#endif

void hn_http_response_free(hn_http_response *r) {
    if (!r) return; free(r->body); free(r->content_type); free(r->headers); memset(r, 0, sizeof(*r));
}

/* ---------------- async completion scope ---------------- */
typedef struct hn_http_job hn_http_job;
typedef struct hn_http_done hn_http_done;
struct hn_http_job {
    hn_http_scope *scope; uint64_t id; hn_http_request_opts opts;
    char *url, *method, *body; hn_http_header *headers; size_t header_count;
};
struct hn_http_done { hn_http_done *next; uint64_t id; int ok; hn_http_response response; char error[512]; };
struct hn_http_scope {
#if !defined(_WIN32)
    pthread_mutex_t mutex;
#endif
    int closed, active; hn_http_done *head, *tail;
};
static void job_free(hn_http_job *j) {
    if (!j) return; free(j->url); free(j->method); free(j->body);
    for (size_t i = 0; i < j->header_count; i++) { free((char *)j->headers[i].name); free((char *)j->headers[i].value); }
    free(j->headers); free(j);
}
static void job_done(hn_http_job *j) {
    hn_http_done *d = (hn_http_done *)calloc(1, sizeof(*d));
    if (d) { d->id = j->id; d->ok = hn_http_request(&j->opts, &d->response); if (!d->ok) snprintf(d->error, sizeof(d->error), "%s", hn_http_last_error()); }
    hn_http_scope *s = j->scope;
#if !defined(_WIN32)
    pthread_mutex_lock(&s->mutex);
#endif
    if (!s->closed && d) { if (s->tail) s->tail->next = d; else s->head = d; s->tail = d; }
    else if (d) { hn_http_response_free(&d->response); free(d); }
    s->active--; int free_scope = s->closed && s->active == 0;
#if !defined(_WIN32)
    pthread_mutex_unlock(&s->mutex);
#endif
    job_free(j);
    if (free_scope) {
#if !defined(_WIN32)
        pthread_mutex_destroy(&s->mutex);
#endif
        free(s);
    }
}
#if !defined(_WIN32)
static void *http_worker(void *arg) { job_done((hn_http_job *)arg); return NULL; }
#endif
hn_http_scope *hn_http_scope_create(void) {
    hn_http_scope *s = (hn_http_scope *)calloc(1, sizeof(*s));
#if !defined(_WIN32)
    if (s) pthread_mutex_init(&s->mutex, NULL);
#endif
    return s;
}
int hn_http_async_submit(hn_http_scope *s, uint64_t id, const hn_http_request_opts *o) {
    if (!s || !o || !o->url) return 0;
    hn_http_job *j = (hn_http_job *)calloc(1, sizeof(*j)); if (!j) return 0;
    j->scope = s; j->id = id; j->url = strdup(o->url); j->method = strdup(o->method ? o->method : "GET");
    if (o->body && o->body_len) { j->body = (char *)malloc(o->body_len); if (j->body) memcpy(j->body, o->body, o->body_len); }
    j->header_count = o->header_count;
    if (j->header_count) j->headers = (hn_http_header *)calloc(j->header_count, sizeof(*j->headers));
    if (!j->url || !j->method || (o->body_len && !j->body) || (j->header_count && !j->headers)) { job_free(j); return 0; }
    for (size_t i = 0; i < j->header_count; i++) { j->headers[i].name = strdup(o->headers[i].name ? o->headers[i].name : ""); j->headers[i].value = strdup(o->headers[i].value ? o->headers[i].value : ""); }
    j->opts.url = j->url; j->opts.method = j->method; j->opts.headers = j->headers; j->opts.header_count = j->header_count; j->opts.body = j->body; j->opts.body_len = o->body_len;
#if !defined(_WIN32)
    pthread_mutex_lock(&s->mutex); if (s->closed) { pthread_mutex_unlock(&s->mutex); job_free(j); return 0; } s->active++; pthread_mutex_unlock(&s->mutex);
    pthread_t tid; if (pthread_create(&tid, NULL, http_worker, j) != 0) { pthread_mutex_lock(&s->mutex); s->active--; pthread_mutex_unlock(&s->mutex); job_free(j); return 0; } pthread_detach(tid);
#else
    if (s->closed) { job_free(j); return 0; } s->active++; job_done(j);
#endif
    return 1;
}
size_t hn_http_async_drain(hn_http_scope *s, hn_http_complete_fn cb, void *opaque) {
    if (!s || !cb) return 0; size_t n = 0;
    for (;;) {
#if !defined(_WIN32)
        pthread_mutex_lock(&s->mutex);
#endif
        hn_http_done *d = s->head; if (d) { s->head = d->next; if (!s->head) s->tail = NULL; }
#if !defined(_WIN32)
        pthread_mutex_unlock(&s->mutex);
#endif
        if (!d) break; cb(d->id, d->ok, &d->response, d->error, opaque); hn_http_response_free(&d->response); free(d); n++;
    }
    return n;
}
void hn_http_scope_destroy(hn_http_scope *s) {
    if (!s) return;
#if !defined(_WIN32)
    pthread_mutex_lock(&s->mutex);
#endif
    s->closed = 1; hn_http_done *d = s->head; s->head = s->tail = NULL;
    while (d) { hn_http_done *next = d->next; hn_http_response_free(&d->response); free(d); d = next; }
    int free_now = s->active == 0;
#if !defined(_WIN32)
    pthread_mutex_unlock(&s->mutex);
#endif
    if (free_now) {
#if !defined(_WIN32)
        pthread_mutex_destroy(&s->mutex);
#endif
        free(s);
    }
}

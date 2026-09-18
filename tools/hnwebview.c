/* hnwebview.c — Windows WebView2 兜底渲染器
 *
 * 对应 macOS 的 HNWebKitHost.swift: 页面显式声明
 * `<meta name="hn-renderer" content="webkit">` 时走系统 WebView2,
 * 用于引擎暂不支持的构造(grid / absolute / canvas / svg / video)。
 * **绝不静默切换** —— 与 native 路径共用窗口生命周期。
 *
 * 四条注入(与 macOS 兜底同语义):
 *   1. CSS 归一化: 引擎接受免单位数值(padding:16=16px), 标准 CSS 会忽略,
 *      兜底前补 px, 保证两条路径长相一致
 *   2. 字体栈: 中文微软雅黑(WebView2 默认 Times → 中文 fallback 宋体)
 *   3. htmx-lite JS: hx-get/post、hx-target、hx-swap、hx-trigger(load/every)、
 *      表单参数、回车提交 —— 与 native 语义一致
 *   4. sys:// 桥: 页面 postMessage 请求 → 本机应答(CPU/内存/磁盘/电池/
 *      uptime/host + 本地 KV), 零网络
 *
 * WebView2 通过 WebView2Loader.dll 动态加载(纯 C, 无需 import lib)。
 */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#define CINTERFACE
#include <windows.h>
#include <objbase.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>

#include "WebView2.h"

/* sys:// 应答(公共模块 sysbridge.c, native/webkit 共用) */
char *sys_fragment(const char *url, const char *form_body, const char *store_id);

/* ================= 1. CSS 归一化(移植 HNCSSNormalizer) ================= */

static int is_len_prop(const char *name) {
    static const char *props[] = {
        "margin", "margin-top", "margin-right", "margin-bottom", "margin-left",
        "padding", "padding-top", "padding-right", "padding-bottom", "padding-left",
        "width", "height", "min-width", "max-width", "min-height", "max-height",
        "top", "right", "bottom", "left", "gap", "row-gap", "column-gap",
        "border-width", "border-radius", "font-size", "letter-spacing",
        "flex-basis", "border", "box-shadow", "text-indent", "outline-width",
        NULL
    };
    for (int i = 0; props[i]; i++)
        if (!strcmp(props[i], name)) return 1;
    return 0;
}

static int ends_with_ci(const char *s, size_t len, const char *suf) {
    size_t n = strlen(suf);
    if (len < n) return 0;
    for (size_t i = 0; i < n; i++) {
        char a = s[len - n + i];
        if (a >= 'A' && a <= 'Z') a += 32;
        if (a != suf[i]) return 0;
    }
    return 1;
}

/* 单 token 补单位: 纯数字 → +px; 关键字/带括号/带#/已有单位 → 原样 */
static void normalize_token(char *tok, size_t *len) {
    size_t n = *len;
    if (n == 0) return;
    static const char *keywords[] = {
        "auto", "inherit", "initial", "none", "normal",
        "solid", "dashed", "dotted", "transparent", "currentcolor", NULL
    };
    for (int i = 0; keywords[i]; i++) {
        size_t kl = strlen(keywords[i]);
        if (n == kl && !_strnicmp(tok, keywords[i], n)) return;
    }
    if (memchr(tok, '(', n) || tok[0] == '#') return;
    static const char *units[] = {
        "px", "em", "rem", "vw", "vh", "pt", "%", "s", "ms", "deg", "fr",
        "ch", "vmin", "vmax", NULL
    };
    for (int i = 0; units[i]; i++)
        if (ends_with_ci(tok, n, units[i])) return;
    /* 尾部标点(逗号/斜杠)剥离后判断是否纯数字 */
    size_t core = n;
    size_t sn = 0;
    while (core > 0 && (tok[core - 1] == ',' || tok[core - 1] == '/')) { core--; sn++; }
    if (core == 0) return;
    int is_num = 1;
    for (size_t i = 0; i < core; i++) {
        char c = tok[i];
        if (!((c >= '0' && c <= '9') || c == '.' || c == '-' || c == '+')) { is_num = 0; break; }
    }
    if (!is_num) return;
    /* core + "px" + suffix */
    memmove(tok + core + 2, tok + core, sn + 1);
    memcpy(tok + core, "px", 2);
    *len = core + 2 + sn;
}

static void normalize_value(char *v) {
    size_t len = strlen(v);
    /* 逐 token(括号外空格/逗号分隔), 原地改写 */
    char out[4096];
    size_t oi = 0;
    char tok[512];
    size_t ti = 0;
    int paren = 0;
    for (size_t i = 0; i < len && oi < sizeof(out) - 600; i++) {
        char c = v[i];
        if (c == '(') paren++;
        else if (c == ')') paren = (paren > 0 ? paren - 1 : 0);
        int is_sep = (c == ' ' || c == '\t' || c == ',') && paren == 0;
        if (is_sep) {
            size_t tl = ti;
            normalize_token(tok, &tl);
            if (oi + tl + 1 < sizeof(out)) { memcpy(out + oi, tok, tl); oi += tl; out[oi++] = c; }
            ti = 0;
        } else if (ti < sizeof(tok) - 8) {
            tok[ti++] = c;
        }
    }
    if (ti > 0) {
        size_t tl = ti;
        normalize_token(tok, &tl);
        if (oi + tl < sizeof(out)) { memcpy(out + oi, tok, tl); oi += tl; }
    }
    out[oi] = 0;
    strcpy(v, out);
}

/* 整体归一化: 只在声明块内改写; 注释/字符串不动 */
static char *normalize_css(const char *css) {
    size_t cap = strlen(css) * 2 + 64;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t oi = 0;
    char decl[2048];
    size_t di = 0;
    int depth = 0, in_comment = 0;
    char quote = 0;
    const char *p = css;
    while (*p) {
        char c = *p;
        if (in_comment) {
            if (c == '*' && p[1] == '/') { in_comment = 0; p += 2; continue; }
            if (oi + 1 < cap) out[oi++] = c;
            p++; continue;
        }
        if (quote) {
            if (di + 1 < sizeof(decl)) decl[di++] = c;
            if (c == quote) quote = 0;
            p++; continue;
        }
        if (c == '/' && p[1] == '*') {
            if (depth > 0 && di > 0) { /* 落盘 */ }
            if (di > 0) {
                if (oi + di < cap) { memcpy(out + oi, decl, di); oi += di; }
                di = 0;
            }
            in_comment = 1; p += 2; continue;
        }
        if (c == '"' || c == '\'') { quote = c; if (di + 1 < sizeof(decl)) decl[di++] = c; p++; continue; }
        if (c == '{') {
            if (oi + di + 1 < cap) { memcpy(out + oi, decl, di); oi += di; out[oi++] = '{'; }
            di = 0; depth++; p++; continue;
        }
        if (c == '}') {
            if (di > 0) {
                decl[di] = 0;
                /* 落盘前改写 */
                const char *colon = strchr(decl, ':');
                if (colon && depth > 0) {
                    char name[256];
                    size_t nl = (size_t)(colon - decl);
                    if (nl < sizeof(name) - 1) {
                        memcpy(name, decl, nl);
                        name[nl] = 0;
                        /* 去空白 + 小写 */
                        char nm[256];
                        size_t k = 0;
                        for (size_t j = 0; name[j] && k < sizeof(nm) - 1; j++)
                            if (name[j] != ' ' && name[j] != '\t' && name[j] != '\n')
                                nm[k++] = (name[j] >= 'A' && name[j] <= 'Z') ? name[j] + 32 : name[j];
                        nm[k] = 0;
                        if (is_len_prop(nm)) {
                            char val[2048];
                            strncpy(val, colon + 1, sizeof(val) - 1);
                            val[sizeof(val) - 1] = 0;
                            normalize_value(val);
                            if (oi + nl + 2 + strlen(val) < cap) {
                                oi += (size_t)sprintf(out + oi, "%.*s:%s", (int)nl, decl, val);
                                di = 0;
                                if (oi + 1 < cap) out[oi++] = '}';
                                depth = depth > 0 ? depth - 1 : 0;
                                p++; continue;
                            }
                        }
                    }
                }
                if (oi + di < cap) { memcpy(out + oi, decl, di); oi += di; }
                di = 0;
            }
            if (oi + 1 < cap) out[oi++] = '}';
            depth = depth > 0 ? depth - 1 : 0;
            p++; continue;
        }
        if (c == ';') {
            if (di > 0) {
                decl[di] = 0;
                const char *colon = strchr(decl, ':');
                if (colon && depth > 0) {
                    char name[256];
                    size_t nl = (size_t)(colon - decl);
                    if (nl < sizeof(name) - 1) {
                        memcpy(name, decl, nl);
                        name[nl] = 0;
                        char nm[256];
                        size_t k = 0;
                        for (size_t j = 0; name[j] && k < sizeof(nm) - 1; j++)
                            if (name[j] != ' ' && name[j] != '\t' && name[j] != '\n')
                                nm[k++] = (name[j] >= 'A' && name[j] <= 'Z') ? name[j] + 32 : name[j];
                        nm[k] = 0;
                        if (is_len_prop(nm)) {
                            char val[2048];
                            strncpy(val, colon + 1, sizeof(val) - 1);
                            val[sizeof(val) - 1] = 0;
                            normalize_value(val);
                            if (oi + nl + 2 + strlen(val) < cap) {
                                oi += (size_t)sprintf(out + oi, "%.*s:%s", (int)nl, decl, val);
                                di = 0;
                                if (oi + 1 < cap) out[oi++] = ';';
                                p++; continue;
                            }
                        }
                    }
                }
                if (oi + di < cap) { memcpy(out + oi, decl, di); oi += di; }
                di = 0;
            }
            if (oi + 1 < cap) out[oi++] = ';';
            p++; continue;
        }
        if (di + 1 < sizeof(decl)) decl[di++] = c;
        p++;
    }
    if (di > 0 && oi + di < cap) { memcpy(out + oi, decl, di); oi += di; }
    out[oi] = 0;
    return out;
}

/* ================= 2. 注入(字体栈 + htmx-lite + 归一化 CSS) ================= */

/* 与 native 对齐的中文字体栈(WebView2 默认 Times → 中文 fallback 宋体);
   只设字体, 不碰盒模型(WebView2 自带 UA 样式已足够) */
static const char *FONT_STACK_CSS =
"html { font-family: -apple-system, \"Segoe UI\", \"Microsoft YaHei\","
" \"PingFang SC\", \"Hiragino Sans GB\", \"Heiti SC\", sans-serif; }\n"
"code, pre, kbd, samp, tt { font-family: \"Cascadia Code\", Consolas,"
" \"Courier New\", monospace; }\n";

/* htmx-lite: 与 macOS 兜底同语义; sys:// 请求走 window.chrome.webview
   postMessage(原生应答后回注), 页面零依赖 */
static const char *HTMX_LITE_JS =
"(function () {\n"
"  var pending = {};\n"
"  var seq = 0;\n"
"  window.__hnResolve = function (id, html) {\n"
"    var p = pending[id];\n"
"    if (!p) return;\n"
"    delete pending[id];\n"
"    p(html);\n"
"  };\n"
"  window.chrome.webview.addEventListener('message', function (ev) {\n"
"    var d = typeof ev.data === 'string' ? JSON.parse(ev.data) : ev.data;\n"
"    window.__hnResolve(d.id, d.html);\n"
"  });\n"
"  function hnRequest(url, method, body) {\n"
"    return new Promise(function (resolve, reject) {\n"
"      var id = 'r' + (++seq);\n"
"      pending[id] = resolve;\n"
"      try {\n"
"        window.chrome.webview.postMessage(JSON.stringify({ id: id, url: url, body: body || '', method: method || 'GET' }));\n"
"      } catch (e) { delete pending[id]; reject(e); }\n"
"      setTimeout(function () { if (pending[id]) { delete pending[id]; reject(new Error('timeout')); } }, 8000);\n"
"    });\n"
"  }\n"
"  function formData() {\n"
"    var out = [];\n"
"    document.querySelectorAll('input,textarea').forEach(function (el) {\n"
"      var n = el.getAttribute('name') || el.id;\n"
"      if (!n) return;\n"
"      out.push(encodeURIComponent(n) + '=' + encodeURIComponent(el.value || ''));\n"
"    });\n"
"    return out.join('&');\n"
"  }\n"
"  function intervalOf(trig) {\n"
"    var i = trig.indexOf('every');\n"
"    if (i < 0) return 0;\n"
"    var rest = trig.substring(i + 5).trim();\n"
"    var num = '';\n"
"    for (var k = 0; k < rest.length; k++) {\n"
"      var c = rest.charAt(k);\n"
"      if (c >= '0' && c <= '9') num += c; else break;\n"
"    }\n"
"    if (!num) return 0;\n"
"    var n = parseInt(num, 10);\n"
"    var after = rest.substring(num.length).trim();\n"
"    return after.indexOf('ms') === 0 ? n : n * 1000;\n"
"  }\n"
"  function triggerOf(el) { return (el.getAttribute('hx-trigger') || 'click').toLowerCase(); }\n"
"  function request(el) {\n"
"    var get = el.getAttribute('hx-get'), post = el.getAttribute('hx-post');\n"
"    var url = post || get;\n"
"    if (!url) return;\n"
"    var target = el.getAttribute('hx-target') || 'this';\n"
"    var tEl = (target === 'this') ? el : document.getElementById(target);\n"
"    if (!tEl) return;\n"
"    var swap = (el.getAttribute('hx-swap') || 'innerHTML').toLowerCase();\n"
"    var body = null;\n"
"    if (post) body = formData();\n"
"    else { var q = formData(); if (q) url += (url.indexOf('?') >= 0 ? '&' : '?') + q; }\n"
"    hnRequest(url, post ? 'POST' : 'GET', body).then(function (html) {\n"
"      if (swap === 'outerhtml') tEl.outerHTML = html;\n"
"      else if (swap === 'append' || swap === 'beforeend') tEl.insertAdjacentHTML('beforeend', html);\n"
"      else if (swap === 'prepend' || swap === 'afterbegin') tEl.insertAdjacentHTML('afterbegin', html);\n"
"      else tEl.innerHTML = html;\n"
"    }).catch(function (e) { tEl.textContent = '(请求失败: ' + url + ')'; });\n"
"  }\n"
"  function start() {\n"
"    document.querySelectorAll('[hx-trigger]').forEach(function (el) {\n"
"      var trig = triggerOf(el);\n"
"      if (trig.indexOf('load') >= 0) request(el);\n"
"      var ms = intervalOf(trig);\n"
"      if (ms > 0) setInterval(function () { request(el); }, ms);\n"
"    });\n"
"    document.addEventListener('click', function (ev) {\n"
"      var el = ev.target && ev.target.closest ? ev.target.closest('[hx-get],[hx-post]') : null;\n"
"      if (!el) return;\n"
"      ev.preventDefault();\n"
"      request(el);\n"
"    });\n"
"    document.addEventListener('keydown', function (ev) {\n"
"      if (ev.key !== 'Enter') return;\n"
"      var el = ev.target;\n"
"      if (!el || el.tagName !== 'INPUT') return;\n"
"      var carrier = el.closest('[hx-post],[hx-get]');\n"
"      if (!carrier) {\n"
"        var p = el.parentElement;\n"
"        for (var i = 0; i < 5 && p && !carrier; i++, p = p.parentElement) carrier = p.querySelector('[hx-post],[hx-get]');\n"
"      }\n"
"      if (carrier) { ev.preventDefault(); request(carrier); }\n"
"    });\n"
"  }\n"
"  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start);\n"
"  else start();\n"
"})();\n";

/* 注入: charset + 字体栈 + 归一化 CSS + htmx-lite, 插到 </head> 前 */
static char *inject_document(const char *html, const char *css) {
    char *norm = css ? normalize_css(css) : NULL;
    const char *css_use = norm ? norm : (css ? css : "");

    size_t cap = strlen(html) + strlen(css_use) + strlen(FONT_STACK_CSS)
               + strlen(HTMX_LITE_JS) + 512;
    char *out = (char *)malloc(cap);
    if (!out) { free(norm); return NULL; }

    char injection[8192];
    int has_charset = (strstr(html, "charset=") != NULL);
    snprintf(injection, sizeof(injection),
        "%s"
        "<style>%s</style>\n"
        "<style>%s</style>\n"
        "<script>%s</script>\n",
        has_charset ? "" : "<meta charset=\"utf-8\">\n",
        FONT_STACK_CSS, css_use, HTMX_LITE_JS);

    /* 插到 </head>(大小写不敏感)前, 否则 <body 前, 否则最前 */
    const char *head_end = NULL;
    for (const char *q = html; *q; q++) {
        if ((q[0] == '<') &&
            (q[1] == '/' || q[1] == '/') &&
            !_strnicmp(q + 1, "/head", 5)) { head_end = q; break; }
    }
    if (head_end) {
        size_t hl = (size_t)(head_end - html);
        snprintf(out, cap, "%.*s%s%s", (int)hl, html, injection, head_end);
    } else {
        const char *body = NULL;
        for (const char *q = html; *q; q++) {
            if (q[0] == '<' && !_strnicmp(q + 1, "body", 4)) { body = q; break; }
        }
        if (body) {
            size_t bl = (size_t)(body - html);
            snprintf(out, cap, "%.*s%s%s", (int)bl, html, injection, body);
        } else {
            snprintf(out, cap, "%s%s", injection, html);
        }
    }
    free(norm);
    return out;
}

/* ================= 3. WebView2 COM 宿主(动态加载, 纯 C) ================= */

static HMODULE g_wv2_mod;
static HRESULT (WINAPI *g_create_env)(PCWSTR, PCWSTR,
    ICoreWebView2EnvironmentOptions *,
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);
static ICoreWebView2Controller *g_controller;
static ICoreWebView2 *g_webview;
static HWND g_parent;
static char *g_pending_html;   /* 环境就绪前到达的文档 */
static const char *g_store_id = "default";

/* ---- IUnknown 共用实现(回调均为进程内静态单例, 不计数) ---- */
static HRESULT STDMETHODCALLTYPE cb_qi(IUnknown *This, REFIID riid, void **ppv) {
    (void)This;
    if (!ppv) return E_POINTER;
    if (IsEqualIID(riid, &IID_IUnknown)) { *ppv = This; return S_OK; }
    *ppv = NULL;
    return E_NOINTERFACE;
}
static ULONG STDMETHODCALLTYPE cb_addref(IUnknown *This) { (void)This; return 1; }
static ULONG STDMETHODCALLTYPE cb_release(IUnknown *This) { (void)This; return 1; }

/* ---- 环境创建完成 → 创建 Controller ---- */
static HRESULT STDMETHODCALLTYPE env_completed_Invoke(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This,
    HRESULT err, ICoreWebView2Environment *env);

static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandlerVtbl g_env_vtbl = {
    cb_qi, cb_addref, cb_release, env_completed_Invoke
};
static ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler g_env_handler = {
    &g_env_vtbl
};

/* ---- Controller 创建完成 → 挂父窗口 + 导航 ---- */
static HRESULT STDMETHODCALLTYPE ctrl_completed_Invoke(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This,
    HRESULT err, ICoreWebView2Controller *ctrl);

static ICoreWebView2CreateCoreWebView2ControllerCompletedHandlerVtbl g_ctrl_vtbl = {
    cb_qi, cb_addref, cb_release, ctrl_completed_Invoke
};
static ICoreWebView2CreateCoreWebView2ControllerCompletedHandler g_ctrl_handler = {
    &g_ctrl_vtbl
};

/* ---- WebMessage 到达(sys:// 请求) ---- */
static HRESULT STDMETHODCALLTYPE msg_received_Invoke(
    ICoreWebView2WebMessageReceivedEventHandler *This,
    ICoreWebView2 *sender, ICoreWebView2WebMessageReceivedEventArgs *args);

static ICoreWebView2WebMessageReceivedEventHandlerVtbl g_msg_vtbl = {
    cb_qi, cb_addref, cb_release, msg_received_Invoke
};
static ICoreWebView2WebMessageReceivedEventHandler g_msg_handler = {
    &g_msg_vtbl
};

/* ---- 迷你 JSON: 从 "key":"value" 取值 ---- */
static void json_str(const char *json, const char *key, char *out, size_t cap) {
    char pat[256];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    if (!p) { out[0] = 0; return; }
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '"' && i < cap - 1) {
        if (*p == '\\' && p[1]) {
            p++;
            switch (*p) {
            case 'n': out[i++] = '\n'; break;
            case 't': out[i++] = '\t'; break;
            case 'r': out[i++] = '\r'; break;
            default: out[i++] = *p; break;
            }
            p++;
        } else out[i++] = *p++;
    }
    out[i] = 0;
}

/* 构造应答 JSON: {"id":"...","html":"..."}(html 转义) */
static char *make_reply_json(const char *id, const char *html) {
    size_t cap = strlen(id) + strlen(html) * 2 + 64;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t i = (size_t)sprintf(out, "{\"id\":\"%s\",\"html\":\"", id);
    for (const char *p = html; *p && i < cap - 4; p++) {
        if (*p == '"' || *p == '\\') { out[i++] = '\\'; out[i++] = *p; }
        else if (*p == '\n') { out[i++] = '\\'; out[i++] = 'n'; }
        else if (*p == '\r') { out[i++] = '\\'; out[i++] = 'r'; }
        else out[i++] = *p;
    }
    out[i] = 0;
    strcat(out, "\"}");
    return out;
}

/* ---- 消息处理: sys:// 应答 ---- */
static HRESULT STDMETHODCALLTYPE msg_received_Invoke(
    ICoreWebView2WebMessageReceivedEventHandler *This,
    ICoreWebView2 *sender, ICoreWebView2WebMessageReceivedEventArgs *args) {
    (void)This; (void)sender;
    if (!args || !g_webview) return S_OK;
    LPWSTR pw = NULL;
    if (FAILED(args->lpVtbl->TryGetWebMessageAsString(args, &pw)) || !pw) return S_OK;
    char json[8192];
    WideCharToMultiByte(CP_UTF8, 0, pw, -1, json, sizeof(json), NULL, NULL);
    CoTaskMemFree(pw);

    char id[64], url[1024], body[1024];
    json_str(json, "id", id, sizeof(id));
    json_str(json, "url", url, sizeof(url));
    json_str(json, "body", body, sizeof(body));

    char *frag = sys_fragment(url, body, g_store_id);
    const char *html = frag ? frag : "<div class=\"sysv\">(不支持的 sys:// 路由)</div>";
    char *reply = make_reply_json(id, html);
    if (reply) {
        wchar_t *wr = (wchar_t *)malloc((strlen(reply) + 1) * sizeof(wchar_t));
        if (wr) {
            MultiByteToWideChar(CP_UTF8, 0, reply, -1, wr, (int)(strlen(reply) + 1));
            g_webview->lpVtbl->PostWebMessageAsJson(g_webview, wr);
            free(wr);
        }
        free(reply);
    }
    free(frag);
    return S_OK;
}

/* ---- Controller 就绪: 挂窗口 + 导航 ---- */
static HRESULT STDMETHODCALLTYPE ctrl_completed_Invoke(
    ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *This,
    HRESULT err, ICoreWebView2Controller *ctrl) {
    (void)This;
    if (FAILED(err) || !ctrl) return err;
    g_controller = ctrl;
    RECT rc;
    GetClientRect(g_parent, &rc);
    ctrl->lpVtbl->put_Bounds(ctrl, rc);
    ctrl->lpVtbl->put_ParentWindow(ctrl, g_parent);
    if (SUCCEEDED(ctrl->lpVtbl->get_CoreWebView2(ctrl, &g_webview)) && g_webview) {
        g_webview->lpVtbl->add_WebMessageReceived(g_webview, &g_msg_handler, NULL);
        if (g_pending_html) {
            wchar_t *w = (wchar_t *)malloc((strlen(g_pending_html) + 1) * sizeof(wchar_t));
            if (w) {
                MultiByteToWideChar(CP_UTF8, 0, g_pending_html, -1, w, (int)(strlen(g_pending_html) + 1));
                g_webview->lpVtbl->NavigateToString(g_webview, w);
                free(w);
            }
        }
    }
    printf("[hnwebview] WebView2 已就绪\n");
    fflush(stdout);
    return S_OK;
}

/* ---- 环境就绪 → 创建 Controller ---- */
static HRESULT STDMETHODCALLTYPE env_completed_Invoke(
    ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *This,
    HRESULT err, ICoreWebView2Environment *env) {
    (void)This;
    if (FAILED(err) || !env) { fprintf(stderr, "hnwebview: 环境创建失败\n"); return err; }
    return env->lpVtbl->CreateCoreWebView2Controller(env, g_parent, &g_ctrl_handler);
}

/* ================= 4. 公开接口 ================= */

/* 打开 WebView2 兜底: parent 是宿主窗口(client 区即浏览器区域)。
   返回 1 表示异步创建已启动。 */
int hnwebview_open(HWND parent, const char *html, const char *css,
                   const char *store_id) {
    if (!g_wv2_mod) {
        g_wv2_mod = LoadLibraryW(L"WebView2Loader.dll");
        if (!g_wv2_mod) {
            fprintf(stderr, "hnwebview: 未找到 WebView2Loader.dll(随可执行分发)\n");
            return 0;
        }
        g_create_env = (void *)GetProcAddress(g_wv2_mod, "CreateCoreWebView2EnvironmentWithOptions");
        if (!g_create_env) {
            fprintf(stderr, "hnwebview: Loader 缺导出 CreateCoreWebView2EnvironmentWithOptions\n");
            return 0;
        }
    }
    g_parent = parent;
    g_store_id = store_id ? store_id : "default";

    char *injected = inject_document(html, css);
    free(g_pending_html);
    g_pending_html = injected;

    /* 用户数据目录: 隔离每应用缓存(~/.html-native/webview/<id>) */
    wchar_t home[MAX_PATH], uddir[MAX_PATH + 128];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) wcscpy(home, L"C:\\");
    _snwprintf(uddir, MAX_PATH + 128, L"%s\\.html-native\\webview\\%S", home, g_store_id);
    wchar_t *wstore = (wchar_t *)malloc((strlen(g_store_id) + 1) * sizeof(wchar_t));
    MultiByteToWideChar(CP_UTF8, 0, g_store_id, -1, wstore, (int)(strlen(g_store_id) + 1));

    HRESULT hr = g_create_env(NULL, uddir, NULL, &g_env_handler);
    free(wstore);
    if (FAILED(hr)) { fprintf(stderr, "hnwebview: CreateCoreWebView2Environment 失败\n"); return 0; }
    return 1;
}

void hnwebview_resize(void) {
    if (!g_controller || !g_parent) return;
    RECT rc;
    GetClientRect(g_parent, &rc);
    g_controller->lpVtbl->put_Bounds(g_controller, rc);
}

void hnwebview_close(void) {
    if (g_controller) { g_controller->lpVtbl->Close(g_controller); g_controller = NULL; }
    g_webview = NULL;
    free(g_pending_html);
    g_pending_html = NULL;
}

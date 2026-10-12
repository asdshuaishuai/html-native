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
 *    "title":"...","transparent":0|1,"headless":0|1,"path":"a.html"}
 *   {"op":"open",...,"window":1,"path":"a.html"}   → 桌面弹原生窗(见下)
 *   {"op":"update","id":"a","html":"...","path":"a.html"}
 *       headless 应用: hn_rt_render 整文档热替换;
 *       window 应用:   新内容(html + 可选 css)原子写回 spawn 的源文件,
 *                      子进程 hn_app 的 mtime watch 自行重载 —— 热推送
 *                      对真窗口生效。回 {"ok":true,"window":true,"reloaded":true}
 *   {"op":"eval"/"shot"/"dump"/"text"} 对 window 应用回
 *       ok:false,"error":"unsupported for window apps (headless only)"
 *       (文档在子进程里, daemon 无句柄 —— headless 是渲染/截图管线,
 *        window 是给人看的桌面窗, 分工见下)
 *   {"op":"close","id":"a"}           → headless 销毁; window 应用 SIGTERM+waitpid
 *   {"op":"list"}                     → {"apps":[{id,kind:"headless",w,h} |
 *                                                {id,kind:"window",pid,w,h}]}
 *   {"op":"eval","id":"a","js":"..."} → {"value":"..."}
 *   {"op":"dom","id":"a"}             → {"dom":[...]}
 *   {"op":"dump","id":"a"}            → {"cmds":N}
 *   {"op":"text","id":"a","element":"x"} → {"text":"..."}
 *   {"op":"event","id":"a","kind":"click","target":"go","x":..,"y":..}
 *   {"op":"shot","id":"a","path":"out.png"}
 *
 * **窗口即 UI**(daemon 接管开窗): open 带 "window":1 时 daemon 不建文档
 * (不占 headless 槽位), 而是 posix_spawn 平台壳子进程
 * `hnapp-macos-arm64 <path> [W H]` —— 每窗一进程, 关窗即退。回包
 * `{"ok":true,"id":..,"pid":N,"window":true,"w":..,"h":..}`; 非 macOS
 * 本机或平台壳二进制缺失 → 回退 headless 并标注 "fallback":"headless"。
 * 文件路径口径与 hn_app 相同: 相对 daemon cwd 传给子进程, hn_app 自会
 * 读文件后 chdir 文档目录解析资产。daemon 退出(SIGINT/SIGTERM/SIGHUP)
 * 对存活窗口子进程统一 SIGTERM 收口, 不留孤儿窗口、不留 stale socket。
 *
 * 装载路径: open/update 走 hn_rt(与 app/hn_app.c 完全同款)——
 * <link rel=stylesheet> 内联、--css 附加表、文档 <script> 执行、hx load
 * 动作、资产后端一条龙。上送 "path" 时装载基准切到**文档目录**
 * (<link>/<include>/资产相对文档位置解析, 装载完即回), 不送则按 daemon
 * cwd —— 与 hn_app 的 chdir 口径一致, 两条路径出图逐字节相同。
 *
 * 与 Swift 版的差异(刻意):
 *   - 窗口/事件循环归各平台壳(tools/hnweb_macos.c / hnweb_linux.c / hnwin.c)
 *     —— daemon 只管**无头**应用的文档生命周期; 开窗 = spawn 平台壳子进程
 *     (open window:1), 窗口的事件循环仍在壳里, daemon 只记 pid/生命周期。
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
#include <errno.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>
#include <sys/wait.h>

extern char **environ;

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
    hn_rt *rt;           /* hn_rt 装载路径(与 hn_app 同款): link 内联/css/script */
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
    return a;
}

/* 引擎上下文(hn_rt 持有; dump/shot/event 等只读消费) */
static hn_context *app_ctx(dapp *a) {
    return a && a->rt ? hn_rt_context(a->rt) : NULL;
}

static void app_destroy(dapp *a) {
    if (a->rt) hn_rt_close(a->rt);
    free(a->html);
    a->rt = NULL;
    a->alive = 0;
}

/* ---------------- 窗口应用表(spawn 平台壳子进程, 每窗一进程) ----------------
 *
 * "窗口即 UI": open window:1 不占上面的 headless 槽位, 这里只记
 * 子进程 pid、请求尺寸与 spawn 的源文件 path —— 文档/事件循环全在
 * 平台壳(hn_app)里, daemon 只管生命周期与热推送: list 列出、
 * close 杀掉、update 把新内容原子写回 path(子进程 mtime watch 自行
 * 重载, 见 app/hn_app.c)、退出统一收口。
 */

#define HN_DAEMON_MAX_WINS 64

typedef struct {
    char id[128];
    pid_t pid;
    int w, h;
    int alive;
    char path[1024];     /* spawn 时上送的源文件: update 热推送原子写回这里 */
} dwin;

static dwin g_wins[HN_DAEMON_MAX_WINS];
static int g_win_n = 0;
static const char *g_argv0 = NULL;   /* 用于定位同目录的平台壳 */

static dwin *win_find(const char *id) {
    for (int i = 0; i < g_win_n; i++)
        if (g_wins[i].alive && !strcmp(g_wins[i].id, id)) return &g_wins[i];
    return NULL;
}

/* 收割已退出的子进程(用户 Esc 手关窗后 pid 会变僵尸, list/close 前顺手收) */
static void win_reap(void) {
    for (int i = 0; i < g_win_n; i++) {
        if (!g_wins[i].alive) continue;
        int st = 0;
        pid_t r = waitpid(g_wins[i].pid, &st, WNOHANG);
        if (r == g_wins[i].pid || (r < 0 && errno == ECHILD)) g_wins[i].alive = 0;
    }
}

/* SIGTERM + 限时回收; 500ms 顽抗则 SIGKILL。返回前 alive 已落账 */
static void win_kill(dwin *w) {
    if (kill(w->pid, SIGTERM) == 0) {
        for (int i = 0; i < 50; i++) {
            int st = 0;
            if (waitpid(w->pid, &st, WNOHANG) == w->pid) { w->alive = 0; return; }
            struct timespec ts = { 0, 10 * 1000 * 1000 };
            nanosleep(&ts, NULL);
        }
        kill(w->pid, SIGKILL);
        while (waitpid(w->pid, NULL, 0) < 0 && errno == EINTR) {}
    }
    w->alive = 0;
}

/* 平台壳路径: 仅 macOS 本机接管开窗, 其余平台一律回退 headless。
 * 候选顺序: daemon 可执行文件同目录(dist/ 自包含) → 相对 cwd 的 dist/。 */
static int win_shell_path(char *out, size_t cap) {
#if !defined(__APPLE__)
    (void)out; (void)cap;
    return 0;
#else
    const char *name = "hnapp-macos-arm64";
    if (g_argv0 && g_argv0[0]) {
        char dir[1024];
        snprintf(dir, sizeof(dir), "%s", g_argv0);
        char *sl = strrchr(dir, '/');
        if (sl) {
            *sl = 0;
            int n = snprintf(out, cap, "%s/%s", dir, name);
            if (n > 0 && (size_t)n < cap && access(out, X_OK) == 0) return 1;
        }
    }
    int n = snprintf(out, cap, "dist/%s", name);
    if (n > 0 && (size_t)n < cap && access(out, X_OK) == 0) return 1;
    return 0;
#endif
}

/* posix_spawn 平台壳: hnapp <path> [W H](与手工 CLI 直开完全同参) */
static pid_t win_spawn(const char *shell, const char *path, int w, int h) {
    char ws[16], hs[16];
    snprintf(ws, sizeof(ws), "%d", w);
    snprintf(hs, sizeof(hs), "%d", h);
    char *cargv[] = { (char *)shell, (char *)path, ws, hs, NULL };
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
    pid_t pid = -1;
    int rc = posix_spawn(&pid, shell, NULL, &attr, cargv, environ);
    posix_spawnattr_destroy(&attr);
    return rc == 0 ? pid : -1;
}

/* 原子覆写: tmp 写**同目录** + rename(同目录才在同一文件系统, rename 原子)。
 * 子进程 hn_app 的 mtime watch 随时可能 stat+read 这个 path —— fopen "wb"
 * 先截断再写会让它读到半截 HTML; rename 语义下 path 要么指旧文件要么指
 * 新文件, 子进程永远读到完整内容(tmp+rename 的"稳定 inode"口径)。 */
static int win_atomic_write(const char *path, const char *data, size_t len) {
    char tmp[1152];
    snprintf(tmp, sizeof(tmp), "%s.hnu-%d.tmp", path, (int)getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) return 0;
    size_t n = fwrite(data, 1, len, f);
    int werr = ferror(f);
    fclose(f);
    if (werr || n != len || rename(tmp, path) != 0) {
        unlink(tmp);
        return 0;
    }
    return 1;
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
    hn_doc *doc = hn_context_doc(app_ctx(a));
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
    if (ok) hn_context_layout(app_ctx(a), (float)a->w, (float)a->h, &g_tb);
    return ok;
}

/* 装载基准: <link>/<include>/资产相对**文档目录**解析(与 app/hn_app.c 的
   chdir 同款)。daemon 是多应用宿主, 只在装载期间切入文档目录, 装载完
   即回 —— shot 等路径仍按 daemon 自身 cwd 解析。无 path(纯内存 HTML)时
   按 daemon cwd 兜底, 协议向后兼容。 */
typedef struct { char prev[1024]; int saved; } doc_dir_guard;

static void doc_dir_enter(doc_dir_guard *g, const char *path) {
    g->saved = 0;
    g->prev[0] = 0;
    if (!path || !path[0]) return;
    const char *slash = strrchr(path, '/');
    if (!slash) return;                     /* 裸文件名: cwd 已是文档目录 */
    char cwdbuf[1024];
    if (!getcwd(cwdbuf, sizeof(cwdbuf))) return;
    size_t dl = (size_t)(slash - path + 1);  /* 含尾斜杠 */
    char dir[1024];
    if (dl >= sizeof(dir)) return;
    memcpy(dir, path, dl);
    dir[dl] = 0;
    if (chdir(dir) != 0) return;
    snprintf(g->prev, sizeof(g->prev), "%s", cwdbuf);
    g->saved = 1;
}

static void doc_dir_leave(doc_dir_guard *g) {
    if (g->saved) chdir(g->prev);
    g->saved = 0;
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
        char *spath = json_str(line, "path");   /* 源文件路径(CLI 上送) */
        long w = json_num(line, "w", 480);
        long h = json_num(line, "h", 700);
        long headless = json_num(line, "headless", 1);
        long window = json_num(line, "window", 0);
        if (!id || !html) {
            free(id); free(html); free(css); free(title); free(spath);
            resp_json(out, cap, 0, "\"error\":\"id/html required\"");
            return;
        }
        /* ---- window:1 → 不建文档, spawn 平台壳子进程开真窗 ----
           每窗一进程, 不占 headless 槽位; 非 macOS 本机或平台壳二进制缺失
           → 落到下面原 headless 路径并在响应标注 fallback。 */
        int fallback_headless = 0;
        if (window) {
            if (!spath || !spath[0]) {
                free(id); free(html); free(css); free(title); free(spath);
                resp_json(out, cap, 0, "\"error\":\"window open needs path\"");
                return;
            }
            char shell[1024];
            int shelled = win_shell_path(shell, sizeof(shell));
            if (shelled) win_reap();
            if (shelled && g_win_n < HN_DAEMON_MAX_WINS &&
                !win_find(id) && !app_find(id)) {
                pid_t pid = win_spawn(shell, spath, (int)w, (int)h);
                if (pid > 0) {
                    dwin *wv = &g_wins[g_win_n++];
                    memset(wv, 0, sizeof(*wv));
                    snprintf(wv->id, sizeof(wv->id), "%s", id);
                    wv->pid = pid;
                    wv->w = (int)w; wv->h = (int)h;
                    wv->alive = 1;
                    snprintf(wv->path, sizeof(wv->path), "%s", spath);
                    char body[192];
                    snprintf(body, sizeof(body),
                             "\"id\":\"%s\",\"pid\":%d,\"window\":true,\"w\":%d,\"h\":%d",
                             id, (int)pid, (int)w, (int)h);
                    resp_json(out, cap, 1, body);
                    free(id); free(html); free(css); free(title); free(spath);
                    return;
                }
            }
            fallback_headless = 1;   /* 壳缺失/spawn 失败/表满或撞 id */
        }
        dapp *a = app_create(id, (int)headless);
        if (!a) {
            free(id); free(html); free(css); free(title); free(spath);
            resp_json(out, cap, 0, "\"error\":\"table full or dup id\"");
            return;
        }
        a->w = (int)w; a->h = (int)h;
        a->html = strdup(html);
        a->html_len = strlen(html);
        /* 装载走 hn_rt(与 app/hn_app.c 同款): <link rel=stylesheet> 内联、
           --css 附加样式表、文档 <script> 执行、hx load 动作一条龙 ——
           daemon 与桌面应用两条路径自此同一装载语义。装载期间切入文档
           目录(link/include/资产按文档位置解析), 完成即回。 */
        hn_rt_desc d;
        memset(&d, 0, sizeof(d));
        d.id = a->id;
        d.html = html;
        d.html_len = strlen(html);
        d.css = css;
        d.css_len = css ? strlen(css) : 0;
        d.w = a->w;
        d.h = a->h;
        doc_dir_guard g;
        doc_dir_enter(&g, spath);
        a->rt = hn_rt_open(&d);
        doc_dir_leave(&g);
        if (!a->rt) {
            app_destroy(a);
            free(id); free(html); free(css); free(title); free(spath);
            resp_json(out, cap, 0, "\"error\":\"parse failed\"");
            return;
        }
        hn_manifest m;
        memset(&m, 0, sizeof(m));
        hn_doc_manifest(hn_rt_doc(a->rt), &m);
        if (m.w > 0) a->w = m.w;
        if (m.h > 0) a->h = m.h;
        char body[256];
        if (fallback_headless)
            snprintf(body, sizeof(body), "\"id\":\"%s\",\"w\":%d,\"h\":%d,\"headless\":%s,\"fallback\":\"headless\"",
                     a->id, a->w, a->h, a->headless ? "true" : "false");
        else
            snprintf(body, sizeof(body), "\"id\":\"%s\",\"w\":%d,\"h\":%d,\"headless\":%s",
                     a->id, a->w, a->h, a->headless ? "true" : "false");
        resp_json(out, cap, 1, body);
        free(id); free(html); free(css); free(title); free(spath);
        return;
    }

    if (!strcmp(op, "list")) {
        /* 注: 界宽一律用 sizeof(body)-n —— 曾误写 cap-n(out 的容量),
           FORTIFY 下必触发 __chk_fail(开个应用 hn list 就把 daemon 打崩) */
        char body[2048];
        size_t n = 0;
        n += (size_t)snprintf(body + n, sizeof(body) - n, "\"apps\":[");
        int first = 1;
        win_reap();   /* 先收僵尸, list 只列活着的 */
        for (int i = 0; i < g_app_n; i++) {
            if (!g_apps[i].alive) continue;
            n += (size_t)snprintf(body + n, sizeof(body) - n,
                                  "%s{\"id\":\"%s\",\"kind\":\"headless\",\"w\":%d,\"h\":%d}",
                                  first ? "" : ",", g_apps[i].id, g_apps[i].w, g_apps[i].h);
            first = 0;
        }
        for (int i = 0; i < g_win_n; i++) {
            if (!g_wins[i].alive) continue;
            n += (size_t)snprintf(body + n, sizeof(body) - n,
                                  "%s{\"id\":\"%s\",\"kind\":\"window\",\"pid\":%d,\"w\":%d,\"h\":%d}",
                                  first ? "" : ",", g_wins[i].id,
                                  (int)g_wins[i].pid, g_wins[i].w, g_wins[i].h);
            first = 0;
        }
        n += (size_t)snprintf(body + n, sizeof(body) - n, "]");
        resp_json(out, cap, 1, body);
        return;
    }

    /* 需要 id 的 op 在这里统一解析一次 */
    char *id = json_str(line, "id");
    dapp *a = id ? app_find(id) : NULL;

    /* window 应用只伺候 open/update(热推送)/close/list —— eval/shot/
       dump/text 需要 daemon 内的文档句柄, 而窗口应用的文档在子进程
       hn_app 里。错误信息写明分工: headless 是 agent 的渲染/截图管线,
       window 是给人看的桌面窗。 */
    win_reap();   /* 已手关的窗口不再挡道(落回 app not found 口径) */
    if (id) {
        static const char *const headless_only[] = { "eval", "shot", "dump", "text" };
        for (int i = 0; i < 4; i++) {
            if (!strcmp(op, headless_only[i]) && win_find(id)) {
                free(id);
                resp_json(out, cap, 0,
                          "\"error\":\"unsupported for window apps (headless only)\"");
                return;
            }
        }
    }

    if (!strcmp(op, "update")) {
        /* ---- window 应用: 新内容原子写回 spawn 的源文件 ----
           daemon 不持有窗口应用的文档(在子进程 hn_app 里), 热推送 = 把
           html(以及可选 css) 原子覆写 wv->path, 子进程 mtime watch 检测到
           变化自行整文档重载(窗口表面/进程不动)。css 无独立通道(文件即
           整文档), 以 <style> 前缀并入。(僵尸窗口已在上面的统一 reap 收掉) */
        dwin *wv = id ? win_find(id) : NULL;
        if (wv) {
            char *whtml = json_str(line, "html");
            char *wcss = json_str(line, "css");
            if (!whtml || !wv->path[0]) {
                free(whtml); free(wcss); free(id);
                resp_json(out, cap, 0, "\"error\":\"html required\"");
                return;
            }
            int ok;
            if (wcss && wcss[0]) {
                size_t need = strlen(whtml) + strlen(wcss) + 32;
                char *full = (char *)malloc(need);
                if (full) {
                    snprintf(full, need, "<style>%s</style>\n%s", wcss, whtml);
                    ok = win_atomic_write(wv->path, full, strlen(full));
                    free(full);
                } else ok = 0;
            } else {
                ok = win_atomic_write(wv->path, whtml, strlen(whtml));
            }
            free(whtml); free(wcss); free(id);
            if (ok) resp_json(out, cap, 1, "\"window\":true,\"reloaded\":true");
            else resp_json(out, cap, 0, "\"error\":\"write failed\"");
            return;
        }
        if (!a) { free(id); resp_json(out, cap, 0, "\"error\":\"app not found\""); return; }
        char *html = json_str(line, "html");
        char *spath = json_str(line, "path");
        if (!html) { free(id); free(spath); resp_json(out, cap, 0, "\"error\":\"html required\""); return; }
        /* 热更新同走 hn_rt: 整文档替换(样式表与 link 内联保留), 文档
           <script> 重跑、hx load 动作重放 —— 与 hn_app 的 render 语义一致。
           装载基准同 open: 有 path 则切入文档目录, 完成即回 */
        doc_dir_guard g;
        doc_dir_enter(&g, spath);
        int ok = hn_rt_render(a->rt, html, strlen(html));
        doc_dir_leave(&g);
        if (!ok) {
            free(html); free(spath); free(id);
            resp_json(out, cap, 0, "\"error\":\"parse failed\"");
            return;
        }
        free(a->html);
        a->html = strdup(html);
        a->html_len = strlen(html);
        free(html);
        free(spath);
        free(id);
        resp_json(out, cap, 1, "");
        return;
    }
    if (!strcmp(op, "close")) {
        dwin *wv = id ? win_find(id) : NULL;
        if (wv) {
            win_kill(wv);   /* SIGTERM + waitpid 回收, 不留僵尸/孤儿窗 */
            free(id);
            resp_json(out, cap, 1, "\"window\":true");
            return;
        }
        if (!a) { free(id); resp_json(out, cap, 0, "\"error\":\"app not found\""); return; }
        app_destroy(a);
        free(id);
        resp_json(out, cap, 1, "");
        return;
    }
    if (!a) { free(id); resp_json(out, cap, 0, "\"error\":\"app not found\""); return; }

    if (!strcmp(op, "dump")) {
        hn_context_layout(app_ctx(a), (float)a->w, (float)a->h, &g_tb);
        const hn_display_list *dl = hn_context_display_list(app_ctx(a));
        char body[128];
        snprintf(body, sizeof(body), "\"cmds\":%d", dl ? dl->count : 0);
        resp_json(out, cap, 1, body);
        return;
    }
    if (!strcmp(op, "text")) {
        char *el = json_str(line, "element");
        free(id);
        hn_doc *doc = hn_context_doc(app_ctx(a));
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
        hn_doc *doc = hn_context_doc(app_ctx(a));
        hn_node *n = target ? hn_doc_find_by_id(doc, target) : hn_doc_root(doc);
        int hit = 0;
        if (n) {
            hn_node *carrier = hx_carrier(n);
            if (carrier && kind && !strcmp(kind, "click")) hit = hx_perform(a, carrier);
            else hit = n != NULL;
        }
        /* 链接导航(md 直染的 a[href]): 事件带 x/y 时做命中激活 ——
           #锚引擎内滚动; 外部链接取回 pending 回传 agent(headless 无桌面,
           由调用方决定如何打开; 真窗口壳路径由 hn_rt_button 直接系统打开) */
        char linkurl[2048] = { 0 };
        if (kind && !strcmp(kind, "click")) {
            /* x/y 是数字字段(w/h 同款), 走 json_num; 双双出现才做命中激活 */
            char xs[32], ys[32];
            long has_x = json_num(line, "x", -2147483647L), has_y = json_num(line, "y", -2147483647L);
            snprintf(xs, sizeof xs, "%ld", has_x);
            snprintf(ys, sizeof ys, "%ld", has_y);
            if (has_x != -2147483647L && has_y != -2147483647L) {
                float ex = strtof(xs, NULL), ey = strtof(ys, NULL);
                int lk = hn_context_activate_link(app_ctx(a), ex, ey);
                if (lk == HN_LINK_ANCHOR) { hit = 1; hn_context_repaint(app_ctx(a)); }
                else if (lk == HN_LINK_EXTERNAL) { hn_doc_take_pending_link(doc, linkurl, sizeof linkurl); hit = 1; }
            }
        }
        char body[2200];
        if (linkurl[0])
            snprintf(body, sizeof(body), "\"consumed\":true,\"link\":\"%s\"", linkurl);
        else
            snprintf(body, sizeof(body), "\"consumed\":%s", hit ? "true" : "false");
        free(kind); free(target);
        resp_json(out, cap, 1, body);
        return;
    }
    if (!strcmp(op, "shot")) {
        free(id);
        char *p = json_str(line, "path");
        hn_context_layout(app_ctx(a), (float)a->w, (float)a->h, &g_tb);
        /* anim op 推进的动画时钟在 relayout 里被级联重算冲掉 —— 零步进
           重采样再重建显示列表, shot 取到的才是推进到的那一帧
           (与 tools/hncore.c render 路径同一修法)。 */
        hn_context_anim_tick(app_ctx(a), 0);
        hn_context_repaint(app_ctx(a));
        const hn_display_list *dl = hn_context_display_list(app_ctx(a));
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
        hn_context_layout(app_ctx(a), (float)a->w, (float)a->h, &g_tb);
        const hn_display_list *dl = hn_context_display_list(app_ctx(a));
        int anim_count = 0;
        if (dl) {
            /* 遍历显示列表, 统计有过渡/动画标记的指令 */
            for (int i = 0; i < dl->count; i++) {
                if (dl->cmds[i].kind == 1 /* RECT */ && dl->cmds[i].gradient) anim_count++;
            }
        }
        /* 引擎的动画时钟在 tick — 告知调用方动画系统在跑 */
        snprintf(out, cap, "{\"ok\":true,\"anim_active\":%s,\"cmds\":%d}",
                 hn_context_anim_tick(app_ctx(a), 16.0f) ? "true" : "false", dl ? dl->count : 0);
        (void)anim_count;
        return;
    }
    if (!strcmp(op, "applets")) {
        /* 轻应用槽位枚举(简化: 返回当前所有 headless 应用的 id) */
        char body[2048];
        size_t n = 0;
        n += (size_t)snprintf(body + n, sizeof(body) - n, "\"apps\":[");
        int first = 1;
        for (int i = 0; i < g_app_n; i++) {
            if (!g_apps[i].alive) continue;
            n += (size_t)snprintf(body + n, sizeof(body) - n, "%s{\"id\":\"%s\"}",
                                  first ? "" : ",", g_apps[i].id);
            first = 0;
        }
        n += (size_t)snprintf(body + n, sizeof(body) - n, "]");
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
        hn_doc *doc = hn_context_doc(app_ctx(a));
        FILE *f = fopen(p, "wb");
        if (!f) { resp_json(out, cap, 0, "\"error\":\"cannot write\""); return; }
        /* 用 --shot 生成 PNG + JSON 元数据打包(简化为 HTML+meta) */
        const hn_display_list *dl = hn_context_display_list(app_ctx(a));
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
            /* 恢复同走 hn_rt 装载路径(link 内联/script 执行与 open 一致) */
            hn_rt_desc d;
            memset(&d, 0, sizeof(d));
            d.id = ra->id;
            d.html = rhtml;
            d.html_len = strlen(rhtml);
            d.w = ra->w;
            d.h = ra->h;
            ra->rt = hn_rt_open(&d);
            if (!ra->rt) { ra->alive = 0; }
        }
        free(rid); free(rhtml);
        resp_json(out, cap, ra != NULL, ra ? "" : "\"error\":\"restore failed\"");
        return;
    }
    if (!strcmp(op, "eval"))
        if (!strcmp(op, "eval")) {
        char *js = json_str(line, "js");
        if (!js) { resp_json(out, cap, 0, "\"error\":\"js required\""); return; }
        hn_doc *doc = hn_context_doc(app_ctx(a));
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
        hn_context_layout(app_ctx(a), (float)a->w, (float)a->h, &g_tb);
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
    g_argv0 = argc > 0 ? argv[0] : NULL;
    const char *home = getenv("HOME");
    char sock[1024];
    if (argc > 2 && !strcmp(argv[1], "--socket")) {
        snprintf(sock, sizeof(sock), "%s", argv[2]);
    } else {
        snprintf(sock, sizeof(sock), "%s/.html-native/hn-daemon.sock", home ? home : ".");
    }
    /* 收尾信号: INT/TERM/HUP 同一 handler。用 sigaction 且**不带 SA_RESTART**
       —— 阻塞中的 accept 被 EINTR 打断, 主循环立刻看到 g_run=0 退出收口。
       (signal() 的 BSD 语义带 SA_RESTART, 以前 SIGHUP 无 handler 更是直接
       默认终止, socket 文件留在盘上成 stale。) */
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = on_term;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
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
    fcntl(g_listen, F_SETFD, FD_CLOEXEC);   /* spawn 的窗口子进程不继承监听 fd */
    fprintf(stderr, "hn_daemon(C99): listening %s\n", sock);

    while (g_run) {
        int cfd = accept(g_listen, NULL, NULL);
        if (cfd < 0) { if (g_run) continue; break; }
        serve(cfd);
    }
    close(g_listen);
    unlink(sock);
    /* 窗口应用 = 子进程: 统一 SIGTERM 收口, 不留孤儿窗口 */
    for (int i = 0; i < g_win_n; i++)
        if (g_wins[i].alive) win_kill(&g_wins[i]);
    for (int i = 0; i < g_app_n; i++)
        if (g_apps[i].alive) app_destroy(&g_apps[i]);
    return 0;
}

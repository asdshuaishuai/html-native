/* hn_rt.c — 公共运行时层实现(单源, 三平台共享)
 *
 * 全部逻辑曾散落在 hnweb_macos.c / hnweb_linux.c / hnwin.c / hn_daemon.c
 * 各自的一份(实测重复: read_all×3, hx_perform×3, sys_fragment×2, store×2)。
 * 这里收敛为一份; 平台壳只留 hn_platform.h 的实现 + ~150 行主循环。
 *
 * 依赖: 引擎(Sources/CHtmlNative) + hnsoft(软件光栅)。
 * 不依赖任何平台 API —— 平台无关是这一层的存在理由。
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <dlfcn.h>
#include <math.h>   /* NAN(hnWasmCall 失败返回值, 经 quickjs.h 的 JS_NAN) */
/* statvfs 的声明: Linux/musl 头文件不自依赖, 必须显式包含 statvfs.h,
   否则 hnapp 的 musl 交叉报 incomplete type。macOS 侧走既有传递包含路径
   即可(实测 zig 精简 Darwin 头显式包含 mount.h 反而 u_int 裸奔报错)。 */
#ifndef __APPLE__
#include <sys/statvfs.h>
#endif

#include "hn_rt.h"
#ifdef HN_HAVE_QUICKJS
#include <quickjs.h>
#endif

/* ---------------- 结构 ---------------- */

struct hn_rt {
    hn_context *ctx;
    int w, h;
    int transparent;
    unsigned char *bgra;        /* 预乘 BGRA(平台 blit 的直接输入) */
    hn_node *hover;
    hn_node *focus;
    char id[128];              /* store 隔离键 */
    void *js_rt;               /* JSRuntime *(QuickJS; lazy init) */
    void *js_ctx;              /* JSContext * */
    /* 轮询表(hx-trigger="every Ns") */
    struct { hn_node *node; double due, sec; } polls[16];
    int poll_n;
    int poll_built;
#ifdef HN_HAVE_QUICKJS
    /* JS 时钟(虚拟单调毫秒): 每帧 += dt_ms。真窗壳喂真实间隔 ⇒ 与单调钟
       一致; 探针喂固定 16 ⇒ 定时器语义可确定性断言(不受真实睡眠抖动影响)。 */
    double clock_ms;
    /* 定时器表(setTimeout/setInterval 共用; 上限 64) */
    struct {
        int id;              /* 句柄(>0; 0 = 空槽) */
        double due;          /* 到期时刻(clock_ms 域的绝对值) */
        double interval_ms;  /* >0 = 周期定时器; 0 = 单次 */
        JSValue func;        /* 回调(持有引用; hn_rt_close 统一释放) */
        JSValue args;        /* 附加参数(JS Array; JS_UNDEFINED = 无) */
    } timers[64];
    int timer_n;             /* 占用槽位数(0 = 表空) */
    int timer_seq;           /* 句柄发号器 */
    /* requestAnimationFrame 队列: 下一帧开头按注册序执行一次 */
    struct { int id; JSValue cb; } raf_q[64];
    int raf_n;
    int raf_seq;
#endif
};

/* ---------------- 文件读取 / 资产后端 ---------------- */

typedef struct { char *path; char *data; size_t len; } asset_ent;
static asset_ent g_assets[64];
static int g_asset_n;

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

static const char *asset_load(void *ctx, const char *path, size_t *len) {
    (void)ctx;
    for (int i = 0; i < g_asset_n; i++)
        if (!strcmp(g_assets[i].path, path)) { *len = g_assets[i].len; return g_assets[i].data; }
    size_t n = 0;
    char *d = read_all(path, &n);
    if (!d) return NULL;
    if (g_asset_n < 64) {
        g_assets[g_asset_n].path = strdup(path);
        g_assets[g_asset_n].data = d;
        g_assets[g_asset_n].len = n;
        g_asset_n++;
    }
    *len = n;
    return d;
}

static const hn_asset_backend g_assets_be = { NULL, asset_load };

/* 媒体宿主槽(仿 g_assets_be): 壳经 hn_rt_set_media 注入, hn_rt_open 时
   装进每个 context。NULL = 媒体禁用(引擎整体降级: 元素照常解析只是不播)。 */
static const hn_media_host *g_media_be = NULL;

void hn_rt_set_media(const hn_media_host *host) { g_media_be = host; }

/* <link rel=stylesheet> 内联(引擎不做 I/O; 相对 cwd —— 调用方先 chdir 文档目录) */
static char *load_css_links(const char *html, size_t *out_len) {
    const char *p = html;
    char *out = NULL;
    size_t cap = 0, used = 0;
    while ((p = strstr(p, "<link")) != NULL) {
        const char *rel = strstr(p, "stylesheet");
        const char *href = strstr(p, "href=");
        const char *tag_end = strchr(p, '>');
        if (rel && href && tag_end && href < tag_end) {
            char q = href[5];
            if (q == '"' || q == '\'') {
                const char *v = href + 6;
                const char *ve = strchr(v, q);
                if (ve && ve < tag_end) {
                    size_t vl = (size_t)(ve - v);
                    char path[1024];
                    if (vl + 1 < sizeof(path)) {
                        memcpy(path, v, vl);
                        path[vl] = 0;
                        size_t cl = 0;
                        char *css = read_all(path, &cl);
                        if (css) {
                            size_t need = used + cl + 2;
                            if (need > cap) { cap = need * 2 + 256; out = (char *)realloc(out, cap); }
                            if (out) { memcpy(out + used, css, cl); used += cl; out[used++] = '\n'; }
                            free(css);
                        }
                    }
                }
            }
        }
        p = tag_end ? tag_end : p + 5;
    }
    if (out) out[used] = 0;
    *out_len = used;
    return out;
}

/* ---------------- 文本后端(FreeType; 无则引擎等宽估算) ---------------- */

static float tb_measure(void *ctx, const hn_font_desc *font, const char *utf8, size_t len) {
    (void)ctx;
    return hnsoft_measure(font, utf8, len);
}
static void tb_metrics(void *ctx, const hn_font_desc *font,
                       float *ascent, float *descent, float *leading) {
    (void)ctx;
    hnsoft_metrics(font, ascent, descent, leading);
}
static hn_text_backend g_tb = { NULL, tb_measure, tb_metrics };

/* ---------------- sys:// 桥(单源) ---------------- */

static char *store_path(const char *store_id) {
    const char *home = getenv("HOME");
    if (!home) return NULL;
    char *p = (char *)malloc(strlen(home) + strlen(store_id) + 64);
    if (!p) return NULL;
    sprintf(p, "%s/.html-native/store/%s.json", home, store_id);
    return p;
}

int hn_rt_store_get(const char *store_id, const char *key, char *out, size_t cap) {
    char *sp = store_path(store_id);
    if (!sp) return 0;
    size_t n = 0;
    char *d = read_all(sp, &n);
    free(sp);
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

int hn_rt_store_set(const char *store_id, const char *key, const char *val) {
    char *sp = store_path(store_id);
    if (!sp) return 0;
    char *obj = NULL;
    size_t obj_n = 0, cap = 4096;
    obj = (char *)malloc(cap);
    if (!obj) { free(sp); return 0; }
    obj[0] = 0;
    int have_key = 0;
    size_t n = 0;
    char *d = read_all(sp, &n);
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
            if (!obj) { free(d); free(sp); return 0; }
            if (obj_n) obj[obj_n++] = ',';
            obj_n += (size_t)snprintf(obj + obj_n, cap - obj_n, "\"%.*s\":\"%.*s\"",
                                      (int)kl, ks, (int)wl, wv);
            if (is_t) have_key = 1;
        }
        free(d);
    }
    if (!have_key) {
        while (obj_n + strlen(key) + strlen(val) + 8 >= cap) { cap *= 2; obj = (char *)realloc(obj, cap); }
        if (!obj) { free(sp); return 0; }
        if (obj_n) obj[obj_n++] = ',';
        obj_n += (size_t)snprintf(obj + obj_n, cap - obj_n, "\"%s\":\"%s\"", key, val);
    }
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", sp);
    char *sl = strrchr(dir, '/');
    if (sl) { *sl = 0; mkdir(dir, 0755); }
    FILE *f = fopen(sp, "wb");
    int ok = 0;
    if (f) { fprintf(f, "{%s}", obj); fclose(f); ok = 1; }
    free(obj);
    free(sp);
    return ok;
}

static int qparam(const char *q, const char *key, char *out, size_t cap);
static void esc_html(const char *s, char *out, size_t cap);
static double sys_cpu_load(void);
static long sys_mem_used_mb(void);
static long sys_mem_total_mb(void);
static long sys_disk_free_gb(void);
static double sys_uptime_s(void);
static void sys_host(char *, size_t);
static int clip_set(const char *);
static int clip_get(char *, size_t);
static int sys_open_url(const char *);
/* 文档内联 <script> 执行(QuickJS 有实现; 无 JS 引擎时是 no-op stub) */
static void run_doc_scripts(hn_rt *rt);
/* JS 定时器/RAF 宿主绑定与帧驱动(QuickJS 有实现; 无 JS 构建为 no-op stub) */
static void js_scope_bind_rt(hn_rt *rt);
static void js_scope_unbind_rt(hn_rt *rt);
static int js_timers_drain(hn_rt *rt, int *repaint);

char *hn_rt_sys_fragment(const char *url, const char *form_body, const char *store_id) {
    if (!url || strncmp(url, "sys://", 6)) return NULL;
    const char *route = url + 6;
    const char *q = strchr(route, '?');
    char key[256] = { 0 }, val[1024] = { 0 };
    char *out = (char *)malloc(4096);
    if (!out) return NULL;
    if (!strncmp(route, "store/get", 9)) {
        if (qparam(q, "key", key, sizeof(key)) || qparam(form_body, "key", key, sizeof(key))) {
            char v[1024] = { 0 };
            int got = hn_rt_store_get(store_id, key, v, sizeof(v));
            char ev[2200];
            esc_html(got ? v : "", ev, sizeof(ev));
            snprintf(out, 4096, "<span id=\"store-value\">%s</span>", ev);
        } else snprintf(out, 4096, "<span>missing key</span>");
    } else if (!strncmp(route, "store/set", 9)) {
        int hk = qparam(q, "key", key, sizeof(key)) || qparam(form_body, "key", key, sizeof(key));
        int hv = qparam(q, "value", val, sizeof(val)) || qparam(form_body, "value", val, sizeof(val));
        if (hk && hv) {
            hn_rt_store_set(store_id, key, val);
            char ev[2200];
            esc_html(val, ev, sizeof(ev));
            snprintf(out, 4096, "<span id=\"store-value\">%s</span>", ev);
        } else snprintf(out, 4096, "<span>missing key/value</span>");
    } else if (!strncmp(route, "store/count", 11)) {
        snprintf(out, 4096, "<span>1</span>");
    } else if (!strncmp(route, "cpu", 3)) {
        double load = sys_cpu_load();
        snprintf(out, 4096,
                 "<div style=\"font-size:20;font-weight:700;color:#7fdbca\">%.0f%%</div>"
                 "<div style=\"font-size:11;color:#6b7386\">CPU load average</div>", load < 0 ? 0 : load);
    } else if (!strncmp(route, "memory", 6) || !strncmp(route, "mem", 3)) {
        long used = sys_mem_used_mb(), total = sys_mem_total_mb();
        int pct = total > 0 ? (int)(used * 100 / total) : 0;
        snprintf(out, 4096,
                 "<div style=\"font-size:20;font-weight:700;color:#7fdbca\">%ld MB</div>"
                 "<div style=\"font-size:11;color:#6b7386\">%ld MB total (%d%%)</div>",
                 used, total, pct);
    } else if (!strncmp(route, "disk", 4)) {
        snprintf(out, 4096,
                 "<div style=\"font-size:20;font-weight:700;color:#7fdbca\">%ld GB</div>"
                 "<div style=\"font-size:11;color:#6b7386\">disk free</div>", sys_disk_free_gb());
    } else if (!strncmp(route, "uptime", 6)) {
        snprintf(out, 4096,
                 "<div style=\"font-size:20;font-weight:700;color:#7fdbca\">%.0f s</div>"
                 "<div style=\"font-size:11;color:#6b7386\">uptime</div>", sys_uptime_s());
    } else if (!strncmp(route, "host", 4)) {
        char host[256];
        sys_host(host, sizeof(host));
        snprintf(out, 4096,
                 "<div style=\"font-size:16;font-weight:600;color:#e8eaf0\">%s</div>", host);
    } else if (!strncmp(route, "clipboard/get", 13)) {
        char text[4096] = { 0 };
        clip_get(text, sizeof(text));
        char ev[4200];
        esc_html(text, ev, sizeof(ev));
        snprintf(out, 4096, "<div id=\"clip\">%s</div>", ev);
    } else if (!strncmp(route, "clipboard/set", 13)) {
        char v[4096] = { 0 };
        int ok = (qparam(q, "value", v, sizeof(v)) || qparam(form_body, "value", v, sizeof(v))) && clip_set(v);
        snprintf(out, 4096, ok ? "<span>copied</span>" : "<span>clip unavailable</span>");
    } else if (!strncmp(route, "open", 4)) {
        char target[2048] = { 0 };
        if (qparam(q, "url", target, sizeof(target)) || qparam(form_body, "url", target, sizeof(target)))
            sys_open_url(target);
        snprintf(out, 4096, "<span></span>");
    } else if (!strncmp(route, "notify", 6)) {
        char text[2048] = { 0 };
        qparam(q, "text", text, sizeof(text));
        fprintf(stderr, "[notify] %s\n", text);
        snprintf(out, 4096, "<span></span>");
    } else {
        free(out);
        return NULL;
    }
    return out;
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

/* ---------------- sys:// 系统桥补齐(POSIX 可移植) ----------------
 * 缺口: agent 的智能 UI 需要"打通系统层级数据" —— CPU/内存/磁盘/电池/
 * uptime/host/剪贴板/通知/打开。这些此前在 Swift SystemBridge 里, 全层
 * 删除后要在 C 侧重建。macOS 侧走 POSIX sysctl + AppKit NSPasteboard/
 * NSWorkspace(dlopen); Linux 侧走 /proc + xclip。两平台同一语义。 */

#ifdef __APPLE__
#include <sys/types.h>
#include <sys/statvfs.h>

/* sysctlbyname 不用 sys/sysctl.h(header 在 _POSIX_C_SOURCE 下拉入 BSD 类型 u_int
   而 -std=c99 + _POSIX_C_SOURCE 200809L 不定义它 → 编译失败)。 */
extern int sysctlbyname(const char *, void *, size_t *, const void *, size_t);
#endif
#include <time.h>

static unsigned long g_boot_hint;   /* uptime 用(进程启动时的 monotonic) */

/* 剪贴板/openURL 的 ObjC 调用: 每次经 dlsym(不依赖链接了哪个 hnp_*.c)。
   Apple 平台 AppKit 已由 hnp_init 装载; 非 Apple 直接返回 0。 */
#ifdef __APPLE__
#include <dlfcn.h>
static void *hn_objc_msgSend;   /* lazy: 首次调用时 dlsym */
static void *hn_objc_sel(const char *name) {
    return dlsym((void *)0, name);
}
static const char *hn_cls(const char *name) { return name; }  /* 占位: objc_getClass 由 dlsym */
#endif

static double sys_uptime_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec;
}

static double sys_cpu_load(void) {
#ifdef __APPLE__
    long loads[3];
    if (sysctlbyname("vm.loadavg", loads, &(size_t){sizeof(loads)}, NULL, 0) != 0) return -1;
    return (double)loads[2] / 256.0 * 100.0;
#else
    FILE *f = fopen("/proc/loadavg", "r");
    if (!f) return -1;
    double l;
    int r = fscanf(f, "%lf", &l);
    fclose(f);
    return r == 1 ? l * 100.0 : -1.0;
#endif
}

static long sys_mem_used_mb(void) {
#ifdef __APPLE__
    uint64_t total = 0, free_pgs = 0, inactive = 0;
    size_t sz = sizeof(total);
    sysctlbyname("hw.memsize", &total, &sz, NULL, 0);
    sz = sizeof(free_pgs);
    sysctlbyname("vm.page_free_count", &free_pgs, &sz, NULL, 0);
    sz = sizeof(inactive);
    sysctlbyname("vm.page_inactive_count", &inactive, &sz, NULL, 0);
    long pg = 4096;
    sz = sizeof(pg);
    sysctlbyname("vm.pagesize", &pg, &sz, NULL, 0);
    return (long)((total - (free_pgs + inactive) * (uint64_t)pg) / 1048576);
#else
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    long total = 0, avail = 0; char line[256];
    while (fgets(line, sizeof(line), f)) {
        if (!strncmp(line, "MemTotal:", 9)) sscanf(line + 9, "%ld", &total);
        else if (!strncmp(line, "MemAvailable:", 13)) sscanf(line + 13, "%ld", &avail);
    }
    fclose(f);
    return total > 0 ? (total - avail) / 1024 : -1;
#endif
}

static long sys_mem_total_mb(void) {
#ifdef __APPLE__
    uint64_t v = 0; size_t sz = sizeof(v);
    sysctlbyname("hw.memsize", &v, &sz, NULL, 0);
    return (long)(v / 1048576);
#else
    FILE *f = fopen("/proc/meminfo", "r");
    if (!f) return -1;
    long v = 0; char line[256];
    while (fgets(line, sizeof(line), f))
        if (!strncmp(line, "MemTotal:", 9)) { sscanf(line + 9, "%ld", &v); break; }
    fclose(f);
    return v / 1024;
#endif
}

static long sys_disk_free_gb(void) {
    struct statvfs st;
    if (statvfs("/", &st) != 0) return -1;
    return (long)((double)st.f_bavail * st.f_frsize / 1073741824.0);
}

static void sys_host(char *out, size_t cap) {
#ifdef __APPLE__
    char buf[256]; size_t sz = sizeof(buf) - 1;
    if (sysctlbyname("kern.hostname", buf, &sz, NULL, 0) == 0) { buf[sz] = 0; snprintf(out, cap, "%s", buf); return; }
#else
    FILE *f = fopen("/etc/hostname", "r");
    if (f) {
        if (fgets(out, (int)cap, f)) {
            char *nl = strchr(out, '\n');
            if (nl) *nl = 0;
            fclose(f);
            return;
        }
        fclose(f);
    }
#endif
    snprintf(out, cap, "unknown");
}

/* 剪贴板/打开 URL: macOS 走 AppKit(dlsym); Linux 下一阶段(xclip/xdg-open)。 */
#ifdef __APPLE__
#include <dlfcn.h>
static int clip_set(const char *text) {
    void *(*gc)(const char *) = (void *(*)(const char *))dlsym((void *)0, "objc_getClass");
    void *(*sl)(const char *) = (void *(*)(const char *))dlsym((void *)0, "sel_registerName");
    void *(*send)(void*, void*, ...) = (void *(*)(void*, void*, ...))dlsym((void *)0, "objc_msgSend");
    if (!gc || !sl || !send) return 0;
    void *pb = send(gc("NSPasteboard"), sl("generalPasteboard"));
    if (!pb) return 0;
    ((void (*)(void*,void*))send)(pb, sl("clearContents"));
    void *nscls = gc("NSString");
    void *str = ((void*(*)(void*,void*,const char*))send)(nscls, sl("stringWithUTF8String:"), text);
    void *type_str = ((void*(*)(void*,void*,const char*))send)(nscls, sl("stringWithUTF8String:"), "public.utf8-plain-text");
    ((void*(*)(void*,void*,void*,void*))send)(pb, sl("setString:forType:"), str, type_str);
    return 1;
}
static int clip_get(char *out, size_t cap) {
    void *(*gc)(const char *) = (void *(*)(const char *))dlsym((void *)0, "objc_getClass");
    void *(*sl)(const char *) = (void *(*)(const char *))dlsym((void *)0, "sel_registerName");
    void *(*send)(void*, void*, ...) = (void *(*)(void*, void*, ...))dlsym((void *)0, "objc_msgSend");
    if (!gc || !sl || !send) return 0;
    void *pb = send(gc("NSPasteboard"), sl("generalPasteboard"));
    if (!pb) return 0;
    void *type = ((void*(*)(void*,void*,const char*))send)(gc("NSString"), sl("stringWithUTF8String:"), "public.utf8-plain-text");
    void *str = ((void*(*)(void*,void*,void*))send)(pb, sl("stringForType:"), type);
    if (!str) return 0;
    const char *c = ((const char *(*)(void*,void*))send)(str, sl("UTF8String"));
    if (!c) return 0;
    snprintf(out, cap, "%s", c);
    return 1;
}
static int sys_open_url(const char *url) {
    void *(*gc)(const char *) = (void *(*)(const char *))dlsym((void *)0, "objc_getClass");
    void *(*sl)(const char *) = (void *(*)(const char *))dlsym((void *)0, "sel_registerName");
    void *(*send)(void*, void*, ...) = (void *(*)(void*, void*, ...))dlsym((void *)0, "objc_msgSend");
    if (!gc || !sl || !send) return 0;
    void *ws = send(gc("NSWorkspace"), sl("sharedWorkspace"));
    if (!ws) return 0;
    void *str = ((void*(*)(void*,void*,const char*))send)(gc("NSString"), sl("stringWithUTF8String:"), url);
    ((void (*)(void*,void*,void*))send)(ws, sl("openURL:"), str);
    return 1;
}
#else
static int clip_set(const char *text) { (void)text; return 0; }
static int clip_get(char *out, size_t cap) { (void)out; (void)cap; return 0; }
static int sys_open_url(const char *url) { (void)url; return 0; }
#endif


static hn_node *hx_carrier(hn_node *n) {
    if (!n) return NULL;
    if (hn_node_attr(n, "hx-get") || hn_node_attr(n, "hx-post")) return n;
    hn_node *a = hn_node_ancestor_with_attr(n, "hx-get");
    hn_node *b = hn_node_ancestor_with_attr(n, "hx-post");
    return a ? a : b;
}

static int hx_perform(hn_rt *rt, hn_node *carrier) {
    const char *post = hn_node_attr(carrier, "hx-post");
    const char *get = hn_node_attr(carrier, "hx-get");
    const char *url = post ? post : get;
    if (!url) return 0;
    hn_doc *doc = hn_context_doc(rt->ctx);
    char form[4096];
    form[0] = 0;
    hn_doc_form_encode(doc, form, sizeof(form));
    char *frag = hn_rt_sys_fragment(url, form, rt->id);
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
    if (ok) hn_context_layout(rt->ctx, (float)rt->w, (float)rt->h, &g_tb);
    return ok;
}

/* 启动动作: hx-trigger 含 "load" 的元素立即执行一次 */
static void run_load_actions(hn_rt *rt) {
    hn_doc *doc = hn_context_doc(rt->ctx);
    for (int i = 0;; i++) {
        const char *id = NULL;
        if (!hn_doc_load_at(doc, i, &id)) break;
        hn_node *n = hn_doc_find_by_id(doc, id);
        if (n) hx_perform(rt, n);
    }
    rt->poll_built = 0;   /* 内容变了, 轮询表重建 */
}

/* ---------------- 渲染 ---------------- */

static int rt_render(hn_rt *rt) {
    hn_context_layout(rt->ctx, (float)rt->w, (float)rt->h, &g_tb);
    /* relayout 会把动画采样值从级联重算冲掉(每帧 tick 后紧跟 render,
       kf_clock 不变)—— 零步进重采样一次再建显示列表, 过渡/关键帧的
       中间态才真正落到像素(与 tools/hncore.c render 路径同一修法)。 */
    hn_context_anim_tick(rt->ctx, 0);
    hn_context_repaint(rt->ctx);
    const hn_display_list *dl = hn_context_display_list(rt->ctx);
    if (!dl) return 0;
    hn_color bg = rt->transparent ? 0x00000000u : 0x0B0E13FFu;
    unsigned char *rgba = hnsoft_render(dl, rt->w, rt->h, bg);
    if (!rgba) return 0;
    int n = rt->w * rt->h;
    free(rt->bgra);
    rt->bgra = (unsigned char *)malloc((size_t)n * 4);
    if (!rt->bgra) { free(rgba); return 0; }
    /* RGBA 非预乘 → BGRA 预乘(三平台 blit 的统一约定) */
    for (int i = 0; i < n; i++) {
        unsigned char r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
        if (a != 255) { r = (unsigned char)((r * a) / 255); g = (unsigned char)((g * a) / 255); b = (unsigned char)((b * a) / 255); }
        rt->bgra[i * 4] = b; rt->bgra[i * 4 + 1] = g; rt->bgra[i * 4 + 2] = r; rt->bgra[i * 4 + 3] = a;
    }
    free(rgba);
    return 1;
}

/* ---------------- 公开 API ---------------- */

/* doc → ctx 注册表: hn_rt_eval 的既有签名只拿得到 doc(桥内寻址元素),
   而媒体桥需要 context(宿主 + 会话表)。hn_rt 创建/换文档时登记,
   eval 时反查。表满/未登记(如 daemon 自建 context)时媒体桥静默 no-op
   —— 与"宿主未注入"同一降级口径(契约 §6)。 */
static struct { hn_doc *doc; hn_context *ctx; } g_doc_ctx[64];
static int g_doc_ctx_n = 0;

static void doc_ctx_bind(hn_doc *doc, hn_context *ctx) {
    if (!doc || !ctx) return;
    for (int i = 0; i < g_doc_ctx_n; i++)
        if (g_doc_ctx[i].doc == doc) { g_doc_ctx[i].ctx = ctx; return; }
    if (g_doc_ctx_n < 64) {
        g_doc_ctx[g_doc_ctx_n].doc = doc;
        g_doc_ctx[g_doc_ctx_n].ctx = ctx;
        g_doc_ctx_n++;
    }
}

static void doc_ctx_unbind_ctx(hn_context *ctx) {
    for (int i = 0; i < g_doc_ctx_n; i++) {
        if (g_doc_ctx[i].ctx == ctx) {
            g_doc_ctx[i] = g_doc_ctx[g_doc_ctx_n - 1];
            g_doc_ctx_n--;
            i--;
        }
    }
}

static void doc_ctx_unbind_doc(hn_doc *doc) {
    for (int i = 0; i < g_doc_ctx_n; i++) {
        if (g_doc_ctx[i].doc == doc) {
            g_doc_ctx[i] = g_doc_ctx[g_doc_ctx_n - 1];
            g_doc_ctx_n--;
            i--;
        }
    }
}

static hn_context *doc_ctx_find(hn_doc *doc) {
    if (!doc) return NULL;
    for (int i = 0; i < g_doc_ctx_n; i++)
        if (g_doc_ctx[i].doc == doc) return g_doc_ctx[i].ctx;
    return NULL;
}

hn_rt *hn_rt_open(const hn_rt_desc *d) {
    if (!d || !d->html) return NULL;
    hn_rt *rt = (hn_rt *)calloc(1, sizeof(*rt));
    if (!rt) return NULL;
    snprintf(rt->id, sizeof(rt->id), "%s", d->id ? d->id : "default");
    rt->w = d->w > 0 ? d->w : 480;
    rt->h = d->h > 0 ? d->h : 700;

    hn_doc *doc = hn_parse_html(d->html, d->html_len);
    if (!doc) { free(rt); return NULL; }
    hn_doc_autoid_hx(doc);
    rt->ctx = hn_context_create();
    hn_context_set_doc(rt->ctx, doc);
    hn_context_set_assets(rt->ctx, &g_assets_be);
    if (g_media_be) hn_context_set_media(rt->ctx, g_media_be);

    hn_manifest m;
    memset(&m, 0, sizeof(m));
    hn_doc_manifest(doc, &m);
    if (m.w > 0) rt->w = m.w;
    if (m.h > 0) rt->h = m.h;
    rt->transparent = (m.transparent == 1);

    if (d->css && d->css_len) hn_context_add_sheet(rt->ctx, hn_parse_css(d->css, d->css_len));
    doc_ctx_bind(doc, rt->ctx);
    {
        /* 文档里的 <link rel=stylesheet> 内联(相对 cwd) */
        size_t cl = 0;
        char *css = load_css_links(d->html, &cl);
        if (css && cl) hn_context_add_sheet(rt->ctx, hn_parse_css(css, cl));
        free(css);
    }
    if (!rt_render(rt)) { hn_rt_close(rt); return NULL; }
    /* 定时器/RAF 的宿主绑定先于文档脚本: 装载期 setTimeout 就要把回调
       记到本 rt 的表上(桥函数只有 JSContext*, 经 opaque 里的 rt 反查) */
    js_scope_bind_rt(rt);
    run_doc_scripts(rt);   /* 文档内联 <script>: canvas 2D 等纯 JS 页面的驱动 */
    run_load_actions(rt);
    rt_render(rt);
    return rt;
}

/* ---------------- JS 运行时(QuickJS, lazy init) ----------------
 * QuickJS 是构建期可选项: 构建脚本探测到本机安装才定义 HN_HAVE_QUICKJS。
 * 交叉目标(如 musl)没有对应架构的 quickjs 库可链, 不定义 —— 整段编译
 * 出局, hn_rt_eval 按既有"Error: ..."口径如实报告未启用(不静默)。 */
#ifdef HN_HAVE_QUICKJS

/* 桥函数经 opaque 取用的环境(契约 §6: 从 hn_doc* 扩为 {doc, ctx})。
   实例挂在 js_scopes 槽位上(生命周期同 JS 上下文), opaque 存其指针。
   rt = 定时器/RAF 的宿主(hn_rt_open 时绑定; setTimeout 经它落到 rt 的表)。 */
typedef struct { hn_doc *doc; hn_context *ctx; hn_rt *rt; } js_bridge;

static void js_rt_free(hn_rt *rt) {
    if (rt->js_ctx) { JS_FreeContext((JSContext *)rt->js_ctx); rt->js_ctx = NULL; }
    if (rt->js_rt) { JS_RunGC((JSRuntime *)rt->js_rt); JS_FreeRuntime((JSRuntime *)rt->js_rt); rt->js_rt = NULL; }
}

static void js_rt_init(hn_rt *rt) {
    if (rt->js_ctx) return;
    rt->js_rt = JS_NewRuntime();
    if (!rt->js_rt) return;
    rt->js_ctx = JS_NewContext((JSRuntime *)rt->js_rt);
}

/* hn_set_text(id, text): JS 桥 → 引擎文本更新(与 hn_doc_set_text 同口径) */
static JSValue js_hn_set_text(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_UNDEFINED;
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    const char *id = JS_ToCString(ctx, argv[0]);
    const char *text = JS_ToCString(ctx, argv[1]);
    if (b && b->doc && id && text) hn_doc_set_text(b->doc, id, text);
    if (id) JS_FreeCString(ctx, id);
    if (text) JS_FreeCString(ctx, text);
    return JS_UNDEFINED;
}

/* hn_set_value(id, value): input 的 value 更新 */
static JSValue js_hn_set_value(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_UNDEFINED;
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    const char *id = JS_ToCString(ctx, argv[0]);
    const char *val = JS_ToCString(ctx, argv[1]);
    if (b && b->doc && id && val) {
        hn_node *n = hn_doc_find_by_id(b->doc, id);
        if (n) hn_node_set_value(n, val, strlen(val));
    }
    if (id) JS_FreeCString(ctx, id);
    if (val) JS_FreeCString(ctx, val);
    return JS_UNDEFINED;
}

/* hnMeshParam(id, name, value): 数字人参数桥 —— 引擎侧 hn_node_set_mesh_param
   把形变参数写到节点上, 下一次网格求值生效。name ∈ look_x/look_y/mouth/
   body_x/body_y(值 -1..1, 引擎侧钳位; 未知名静默忽略)。与 hnSetText 同一
   纪律: 找不到 id → 返回 false, 不抛异常。 */
static JSValue js_hn_mesh_param(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 3 || !JS_IsString(argv[1])) return JS_FALSE;
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    const char *id = JS_ToCString(ctx, argv[0]);
    const char *name = JS_ToCString(ctx, argv[1]);
    double v = 0;
    JS_ToFloat64(ctx, &v, argv[2]);
    hn_node *n = (b && b->doc && id && name) ? hn_doc_find_by_id(b->doc, id) : NULL;
    if (n) hn_node_set_mesh_param(n, name, (float)v);
    if (id) JS_FreeCString(ctx, id);
    if (name) JS_FreeCString(ctx, name);
    return JS_NewBool(ctx, n != NULL);
}

/* ---- 媒体桥(hnMedia*, 契约 §6): 宿主未注入/目标不存在 → 静默 no-op 或
   缺省值, 不抛异常(与 hnSetText 一致) ---- */

/* 桥内寻址: opaque {doc, ctx} → 媒体会话可用的 context(doc 反查表兜底) */
static hn_context *js_media_ctx(JSContext *ctx) {
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    if (!b) return NULL;
    if (b->ctx) return b->ctx;
    return doc_ctx_find(b->doc);
}

static JSValue js_hn_media_play(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_FALSE;
    hn_context *c = js_media_ctx(ctx);
    const char *id = JS_ToCString(ctx, argv[0]);
    hn_node *n = (c && id) ? hn_doc_find_by_id(hn_context_doc(c), id) : NULL;
    int ok = (c && n) ? hn_media_play(c, n) : 0;
    if (id) JS_FreeCString(ctx, id);
    return JS_NewBool(ctx, ok);
}

static JSValue js_hn_media_pause(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_UNDEFINED;
    hn_context *c = js_media_ctx(ctx);
    const char *id = JS_ToCString(ctx, argv[0]);
    hn_node *n = (c && id) ? hn_doc_find_by_id(hn_context_doc(c), id) : NULL;
    if (c && n) hn_media_pause(c, n);
    if (id) JS_FreeCString(ctx, id);
    return JS_UNDEFINED;
}

static JSValue js_hn_media_seek(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_FALSE;
    hn_context *c = js_media_ctx(ctx);
    const char *id = JS_ToCString(ctx, argv[0]);
    double sec = 0;
    JS_ToFloat64(ctx, &sec, argv[1]);
    hn_node *n = (c && id) ? hn_doc_find_by_id(hn_context_doc(c), id) : NULL;
    int ok = (c && n) ? hn_media_seek(c, n, sec) : 0;
    if (id) JS_FreeCString(ctx, id);
    return JS_NewBool(ctx, ok);
}

static JSValue js_hn_media_time(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NewFloat64(ctx, 0);
    hn_context *c = js_media_ctx(ctx);
    const char *id = JS_ToCString(ctx, argv[0]);
    hn_node *n = (c && id) ? hn_doc_find_by_id(hn_context_doc(c), id) : NULL;
    double t = (c && n) ? hn_media_time(c, n) : 0;
    if (id) JS_FreeCString(ctx, id);
    return JS_NewFloat64(ctx, t);
}

static JSValue js_hn_media_duration(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NewFloat64(ctx, -1);
    hn_context *c = js_media_ctx(ctx);
    const char *id = JS_ToCString(ctx, argv[0]);
    hn_node *n = (c && id) ? hn_doc_find_by_id(hn_context_doc(c), id) : NULL;
    double d = (c && n) ? hn_media_duration(c, n) : -1;
    if (id) JS_FreeCString(ctx, id);
    return JS_NewFloat64(ctx, d);
}

/* hnMediaVolume(id, v): 音量 + 静音联动(契约表: set_volume + set_muted)。
   v>0 = 解除静音, v<=0 = 静音 —— 音量归零与静音同义。 */
static JSValue js_hn_media_volume(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_UNDEFINED;
    hn_context *c = js_media_ctx(ctx);
    const char *id = JS_ToCString(ctx, argv[0]);
    double v = 0;
    JS_ToFloat64(ctx, &v, argv[1]);
    hn_node *n = (c && id) ? hn_doc_find_by_id(hn_context_doc(c), id) : NULL;
    if (c && n) {
        hn_media_set_volume(c, n, (float)v);
        hn_media_set_muted(c, n, v <= 0.0);
    }
    if (id) JS_FreeCString(ctx, id);
    return JS_UNDEFINED;
}

/* ---- WASM 桥(hnWasm*, 契约 §6/§7): 模块表在引擎侧(hn_wasm.c),
   id = 表下标+1。失败返回 -1 / NaN, 不抛异常(与 hnMedia* 一致) ---- */

/* hnWasmLoad(bytesOrPath) → id(≥1; 失败 -1)。bytes = ArrayBuffer/Uint8Array;
   字符串按资产路径经资产后端读(引擎不做 I/O, 复用 hn_rt 的资产表)。
   hn_wasm_load 内部复制字节, JS 侧缓冲生命周期无关。 */
static JSValue js_hn_wasm_load(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NewInt32(ctx, -1);
    const unsigned char *bytes = NULL;
    size_t n = 0;
    if (JS_IsString(argv[0])) {
        const char *path = JS_ToCString(ctx, argv[0]);
        if (path) {
            bytes = (const unsigned char *)asset_load(NULL, path, &n);
            JS_FreeCString(ctx, path);
        }
    } else {
        /* TypedArray(Uint8Array)→ 取后备 ArrayBuffer 的字节区间;
           本身就是 ArrayBuffer 时直接取。都不是 → 清掉残留异常, 返回 -1。 */
        size_t off = 0, view_len = 0, bpe = 0;
        JSValue tab = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &view_len, &bpe);
        if (!JS_IsException(tab)) {
            size_t total = 0;
            uint8_t *buf = JS_GetArrayBuffer(ctx, &total, tab);
            if (buf && view_len <= total && off <= total - view_len) {
                bytes = buf + off;
                n = view_len;
            }
            JS_FreeValue(ctx, tab);
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx));   /* 桥不抛异常 */
            bytes = JS_GetArrayBuffer(ctx, &n, argv[0]);
        }
    }
    if (!bytes) {
        JS_FreeValue(ctx, JS_GetException(ctx));       /* JS_GetArrayBuffer 的失败残留 */
        return JS_NewInt32(ctx, -1);
    }
    return JS_NewInt32(ctx, hn_wasm_install(bytes, n));
}

/* hnWasmCall(id, fn, ...args) → i32 结果; 失败 NaN(契约 §6: args 按 Number→i32) */
static JSValue js_hn_wasm_call(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 2 || argc - 2 > 64) return JS_NAN;
    int32_t id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    const char *fn = JS_ToCString(ctx, argv[1]);
    hn_wasm *w = fn ? hn_wasm_by_id(id) : NULL;
    int32_t vals[64];
    int na = 0;
    for (int i = 2; i < argc; i++, na++)
        JS_ToInt32(ctx, &vals[na], argv[i]);
    int32_t out = 0;
    int ok = (w && fn) ? hn_wasm_call(w, fn, vals, na, &out) : 0;
    if (fn) JS_FreeCString(ctx, fn);
    return ok ? JS_NewInt32(ctx, out) : JS_NAN;
}

/* ---- <canvas> 2D 桥(hnCanvas2D) ----
 * glue(见下方 CANVAS_GLUE_JS)在 JS 层维护状态机(fillStyle/save/restore
 * 栈/当前路径/CTM/渐变对象 —— 与 canvaskit htmlcanvas/canvas2dcontext.js 的
 * JS 层设计同构), 每个绘制调用在 JS 里解析成"已解析的原语 op"(坐标已经
 * CTM 变换、颜色已 RGBA), JSON 序列化后调本桥 → 引擎 hn_canvas_record
 * 落账(协议见 Sources/CHtmlNative/hn_canvas.c 头注)。
 * 返回记录后的账本 op 数(失败 -1; clear 返回清掉的条数)。 */
static JSValue js_hn_canvas_2d(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    if (argc < 1 || !JS_IsString(argv[0])) return JS_NewInt32(ctx, -1);
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    hn_context *c = b ? (b->ctx ? b->ctx : doc_ctx_find(b->doc)) : NULL;
    if (!c) return JS_NewInt32(ctx, -1);
    size_t len = 0;
    const char *json = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!json) return JS_NewInt32(ctx, -1);
    int n = hn_canvas_record(c, json, len);
    JS_FreeCString(ctx, json);
    return JS_NewInt32(ctx, n);
}

/* __hnMeasureText(deviceFontSizePx, text) → 宽度 px(canvas measureText 的
 * 测量缝)。走 hn_canvas_measure —— 引擎 context 的文本后端(与布局排版
 * 同一测量入口); 无后端构建 = 字节数×字号×0.55 估算, 与引擎同口径。
 * 字号按 glue 现值传入(canvas font 只解析字号, 缺省 400/无衬线)。 */
static JSValue js_hn_measure_text(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    hn_context *c = b ? (b->ctx ? b->ctx : doc_ctx_find(b->doc)) : NULL;
    double px = 10.0;
    if (argc > 0) JS_ToFloat64(ctx, &px, argv[0]);
    float w = 0;
    if (argc > 1 && JS_IsString(argv[1])) {
        size_t len = 0;
        const char *s = JS_ToCStringLen(ctx, &len, argv[1]);
        if (s) w = hn_canvas_measure(c, (float)px, s, len);
        if (s) JS_FreeCString(ctx, s);
    }
    return JS_NewFloat64(ctx, w);
}

/* ---------------- <canvas> 2D glue(JS 层状态机) ----------------
 * 标准 2D context 的方法面全部在此实现, 汇到 hnCanvas2D:
 *   fillRect/strokeRect/beginPath/moveTo/lineTo/arc/ellipse/quadraticCurveTo/
 *   bezierCurveTo/arcTo/closePath/fill/stroke/clip/save/restore/translate/
 *   scale/rotate/setTransform/transform/resetTransform/fillText/strokeText/
 *   measureText/createLinearGradient/createRadialGradient/clearRect/
 *   drawImage/setLineDash/getLineDash
 *   + fillStyle/strokeStyle/lineWidth/lineCap/font/lineDashOffset/textAlign/
 *     textBaseline/shadowColor/shadowBlur/shadowOffsetX/shadowOffsetY 属性。
 * 简化口径(注释即契约):
 *   - arc/ellipse 以 Math.cos/sin 折线化, 15° 一段(与账本 op 的折线模型
 *     一致); 二次曲线固定 16 段、三次曲线固定 24 段(恒定段数, 段内直线
 *     近似 —— 与折线模型同档, 不做自适应细分);
 *   - arcTo 用切点/圆心闭式解; 退化输入(半径非正/共线/折返)退化为
 *     lineTo 到拐点 —— 不崩溃是第一契约;
 *   - clearRect 记录 clear_rect op(绘制期矩形内回填渲染底色); 覆盖整块
 *     画布的清空由引擎识别为清账("清账重录"动画循环保持 op 有界);
 *   - clip() 取当前路径包围盒(矩形裁剪);
 *   - 渐变: createLinearGradient/createRadialGradient 全部 stop 按扁平数组
 *     下送(引擎分段线性插值); radial 为 CSS createRadialGradient 双圆口径;
 *     fill()/stroke() 遇渐变下送 gpath/带渐变 spath(不再回落纯色);
 *   - setLineDash 在 stroke 落账前按折线累计弧长切段(每段一个 spath;
 *     单次 stroke 上限 512 段, 到顶截断防失控路径灌爆账本);
 *   - textAlign(left/center/right)按 measureText 宽度修正锚点; textBaseline
 *     (top/middle/alphabetic)按 0.8/0.3×字号 的比例修正基线(与引擎无
 *     后端回退口径一致);
 *   - measureText 走 __hnMeasureText 原生桥(引擎文本后端同源测量;
 *     无后端构建 = 字节数×字号×0.55);
 *   - shadow 仅随 fillRect 系(frect/grect)下送(路径填充/描边暂不带影);
 *     引擎阴影按 blur>0 生效, blur=0 的硬阴影不支持;
 *   - 线宽/字号随 CTM 按sqrt(|det|) 均匀近似(折线模型的既定简化);
 *   - 文字 y 缺省 = 基线(textBaseline 缺省 alphabetic)。
 * 入口: hnGet2D(id) 直接返回 context; 宿主没有 document 时补一个仅支持
 * canvas 的最小 getElementById(形如标准用法的替代入口)。 */
static const char *CANVAS_GLUE_JS =
"(function(g){"
"  if (typeof g.hnGet2D === 'function') return;"
"  function clamp255(v){v=+v;return v<0?0:(v>255?255:v);}"
"  var NAMED={black:[0,0,0],silver:[192,192,192],gray:[128,128,128],grey:[128,128,128],"
"    white:[255,255,255],red:[255,0,0],green:[0,128,0],blue:[0,0,255],yellow:[255,255,0],"
"    orange:[255,165,0],purple:[128,0,128],fuchsia:[255,0,255],cyan:[0,255,255],"
"    magenta:[255,0,255],lime:[0,255,0],maroon:[128,0,0],navy:[0,0,128],olive:[128,128,0],"
"    teal:[0,128,128],aqua:[0,255,255],gold:[255,215,0],pink:[255,192,203],"
"    transparent:[0,0,0,0]};"
"  function parseColor(v){"
"    var c=[0,0,0,255];"
"    if (v && v.__hngrad) return c;"
"    if (typeof v!=='string') return c;"
"    var s=v.trim();"
"    if (s.charAt(0)==='#'){"
"      var h=s.slice(1);"
"      if (h.length===3||h.length===4) h=h[0]+h[0]+h[1]+h[1]+h[2]+h[2]+(h.length===4?h[3]+h[3]:'');"
"      if (h.length>=6){"
"        c[0]=parseInt(h.slice(0,2),16)||0;"
"        c[1]=parseInt(h.slice(2,4),16)||0;"
"        c[2]=parseInt(h.slice(4,6),16)||0;"
"        if (h.length>=8) c[3]=parseInt(h.slice(6,8),16)||0;"
"      }"
"      return c;"
"    }"
"    if (s.slice(0,4)==='rgba'&&s.charAt(4)==='('){"
"      var p=s.slice(5,s.lastIndexOf(')')).split(',');"
"      c[0]=clamp255(parseFloat(p[0]));c[1]=clamp255(parseFloat(p[1]));c[2]=clamp255(parseFloat(p[2]));"
"      c[3]=Math.round((p.length>3?parseFloat(p[3]):1)*255);"
"      return c;"
"    }"
"    if (s.slice(0,4)==='rgb('&&s.charAt(3)==='('){"
"      var q=s.slice(4,s.lastIndexOf(')')).split(',');"
"      c[0]=clamp255(parseFloat(q[0]));c[1]=clamp255(parseFloat(q[1]));c[2]=clamp255(parseFloat(q[2]));"
"      return c;"
"    }"
"    var n=NAMED[s.toLowerCase()];"
"    if (n) return [n[0],n[1],n[2], n.length>3?n[3]:255];"
"    return c;"
"  }"
"  function rgba(c){"
"    return ((clamp255(Math.round(c[0]))<<24)|(clamp255(Math.round(c[1]))<<16)|"
"            (clamp255(Math.round(c[2]))<<8)|clamp255(Math.round(c[3])))>>>0;"
"  }"
"  var cache={};"
"  function hnGet2D(id){"
"    id=String(id);"
"    if (cache[id]) return cache[id];"
"    var z=make2D(id);"
"    cache[id]=z;"
"    return z;"
"  }"
"  g.hnGet2D=hnGet2D;"
"  if (!g.document) {"
"    g.document={getElementById:function(id){"
"      return {getContext:function(kind){return (kind==='2d'||kind==='2D')?hnGet2D(id):null;}};"
"    }};"
"  }"
"  function make2D(cvId){"
"    function send(o){o.cv=cvId;try{hnCanvas2D(JSON.stringify(o));}catch(e){}}"
"    function stopsOf(gd){return gd.stops.slice().sort(function(p,q){return p.t-q.t;});}"
"    function solidOf(style){"
"      if (style && style.__hngrad){"
"        var s=stopsOf(style);"
"        var a=s.length?s[0].c:[0,0,0,255];"
"        var b=s.length?s[s.length-1].c:a;"
"        return [(a[0]+b[0])/2,(a[1]+b[1])/2,(a[2]+b[2])/2,(a[3]+b[3])/2];"
"      }"
"      return parseColor(style);"
"    }"
"    var st={fill:[0,0,0,255],stroke:[0,0,0,255],fillSrc:'#000000',strokeSrc:'#000000',"
"            lw:1,cap:'butt',fontPx:10,fontSrc:'10px sans-serif',m:[1,0,0,1,0,0],clips:0,"
"            dash:[],dashOff:0,align:'left',base:'alphabetic',"
"            shSrc:'rgba(0,0,0,0)',shC:[0,0,0,0],shX:0,shY:0,shB:0};"
"    var stack=[],subs=[],cur=null;"
"    function xf(x,y){var m=st.m;return [m[0]*x+m[2]*y+m[4], m[1]*x+m[3]*y+m[5]];}"
"    function mul(n){var m=st.m;"
"      st.m=[m[0]*n[0]+m[2]*n[1], m[1]*n[0]+m[3]*n[1],"
"            m[0]*n[2]+m[2]*n[3], m[1]*n[2]+m[3]*n[3],"
"            m[0]*n[4]+m[2]*n[5]+m[4], m[1]*n[4]+m[3]*n[5]+m[5]];}"
"    function detScale(){var m=st.m;return Math.sqrt(Math.abs(m[0]*m[3]-m[1]*m[2]))||1;}"
"    function ensure(){if(!cur){cur=[];subs.push({p:cur,closed:false});}}"
"    function cloneState(s){return {fill:s.fill.slice(),stroke:s.stroke.slice(),"
"      fillSrc:s.fillSrc,strokeSrc:s.strokeSrc,lw:s.lw,cap:s.cap,fontPx:s.fontPx,"
"      fontSrc:s.fontSrc,m:s.m.slice(),clips:s.clips,dash:s.dash.slice(),dashOff:s.dashOff,"
"      align:s.align,base:s.base,shSrc:s.shSrc,shC:s.shC.slice(),shX:s.shX,shY:s.shY,shB:s.shB};}"
"    /* 渐变 op 公共字段: 轴/圆坐标经 CTM 变换(渐变坐标属 user space),"
"       全部 stop 按 [t,rgba,...] 扁平数组下送(引擎分段线性插值)。 */"
"    function gradOp(gd){"
"      var ds=detScale(),s=stopsOf(gd);"
"      if (!s.length) s=[{t:0,c:[0,0,0,255]},{t:1,c:[0,0,0,255]}];"
"      var flat=[];"
"      for (var i=0;i<s.length;i++) flat.push(s[i].t,rgba(s[i].c));"
"      var o={radial:gd.radial?1:0,c0:rgba(s[0].c),c1:rgba(s[s.length-1].c),stops:flat};"
"      if (gd.radial){"
"        var q0=xf(gd.x0,gd.y0),q1=xf(gd.x1,gd.y1);"
"        o.x0=q0[0];o.y0=q0[1];o.x1=q1[0];o.y1=q1[1];"
"        o.r0=(gd.r0||0)*ds;o.r1=(gd.r1||0)*ds;"
"      } else {"
"        var p0=xf(gd.x0,gd.y0),p1=xf(gd.x1,gd.y1);"
"        o.x0=p0[0];o.y0=p0[1];o.x1=p1[0];o.y1=p1[1];"
"      }"
"      return o;"
"    }"
"    /* 文本测量: __hnMeasureText 原生桥(设备字号, 串) → 宽 px;"
"       桥缺失/异常时按 字符数×字号×0.55 估算(与引擎无后端口径一致)。 */"
"    function measureW(s){"
"      var w=0;"
"      try{w=__hnMeasureText(st.fontPx*detScale(),s);}catch(e){}"
"      return (typeof w==='number'&&isFinite(w)&&w>0)?w:String(s).length*st.fontPx*0.55;"
"    }"
"    function textOp(s,x,y,style){"
"      var q=xf(x,y);"
"      var px=st.fontPx*detScale();"
"      if (st.align==='center') q[0]-=measureW(String(s))/2;"
"      else if (st.align==='right') q[0]-=measureW(String(s));"
"      if (st.base==='top') q[1]+=px*0.8;"
"      else if (st.base==='middle') q[1]+=px*0.3;"
"      send({op:'text',s:String(s),x:q[0],y:q[1],px:px,col:rgba(solidOf(style))});"
"    }"
"    function rectPts(x,y,w,h){return [x,y, x+w,y, x+w,y+h, x,y+h];}"
"    function shadowOn(o){"
"      if (st.shC[3]>0&&st.shB>0){o.shc=rgba(st.shC);o.shx=st.shX;o.shy=st.shY;o.shb=st.shB;}"
"      return o;"
"    }"
"    function fillRectImpl(x,y,w,h){"
"      var m=st.m,grad=(st.fill===null),rotated=(m[1]!==0||m[2]!==0),o,gg;"
"      if (rotated){"
"        var p=rectPts(x,y,w,h),t=[];"
"        for (var i=0;i<8;i+=2){var q=xf(p[i],p[i+1]);t.push(q[0],q[1]);}"
"        if (grad){o={op:'gpath',pts:t};gg=gradOp(st.fillSrc);"
"          for (var k in gg)o[k]=gg[k];}"
"        else o={op:'fpath',pts:t,col:rgba(solidOf(st.fillSrc))};"
"        send(shadowOn(o));"
"        return;"
"      }"
"      var q0=xf(x,y),tw=m[0]*w,th=m[3]*h;"
"      if (tw<0){q0[0]+=tw;tw=-tw;}"
"      if (th<0){q0[1]+=th;th=-th;}"
"      if (grad){o={op:'grect',x:q0[0],y:q0[1],w:tw,h:th};"
"        gg=gradOp(st.fillSrc);for (var k2 in gg)o[k2]=gg[k2];}"
"      else o={op:'frect',x:q0[0],y:q0[1],w:tw,h:th,col:rgba(st.fill)};"
"      send(shadowOn(o));"
"    }"
"    var api={};"
"    Object.defineProperty(api,'fillStyle',{"
"      get:function(){return st.fillSrc;},"
"      set:function(v){st.fillSrc=v;st.fill=(v&&v.__hngrad)?null:parseColor(v);}});"
"    Object.defineProperty(api,'strokeStyle',{"
"      get:function(){return st.strokeSrc;},"
"      set:function(v){st.strokeSrc=v;st.stroke=(v&&v.__hngrad)?null:parseColor(v);}});"
"    Object.defineProperty(api,'lineWidth',{"
"      get:function(){return st.lw;},set:function(v){st.lw=(+v)>0?+v:1;}});"
"    Object.defineProperty(api,'lineCap',{"
"      get:function(){return st.cap;},set:function(v){st.cap=String(v);}});"
"    Object.defineProperty(api,'font',{"
"      get:function(){return st.fontSrc;},"
"      set:function(v){st.fontSrc=String(v);"
"        var m=/([0-9.]+)\\s*px/.exec(st.fontSrc);"
"        if (m) st.fontPx=parseFloat(m[1]);}});"
"    Object.defineProperty(api,'lineDashOffset',{"
"      get:function(){return st.dashOff;},set:function(v){st.dashOff=+v||0;}});"
"    Object.defineProperty(api,'textAlign',{"
"      get:function(){return st.align;},set:function(v){st.align=String(v);}});"
"    Object.defineProperty(api,'textBaseline',{"
"      get:function(){return st.base;},set:function(v){st.base=String(v);}});"
"    Object.defineProperty(api,'shadowColor',{"
"      get:function(){return st.shSrc;},"
"      set:function(v){st.shSrc=String(v);st.shC=parseColor(v);}});"
"    Object.defineProperty(api,'shadowBlur',{"
"      get:function(){return st.shB;},"
"      set:function(v){var b=+v;st.shB=(b>0)?b:0;}});"
"    Object.defineProperty(api,'shadowOffsetX',{"
"      get:function(){return st.shX;},set:function(v){st.shX=+v||0;}});"
"    Object.defineProperty(api,'shadowOffsetY',{"
"      get:function(){return st.shY;},set:function(v){st.shY=+v||0;}});"
"    api.save=function(){stack.push(cloneState(st));};"
"    api.restore=function(){"
"      if (!stack.length) return;"
"      var s=stack.pop();"
"      while (st.clips>s.clips){send({op:'cpop'});st.clips--;}"
"      st.fill=s.fill;st.stroke=s.stroke;st.fillSrc=s.fillSrc;st.strokeSrc=s.strokeSrc;"
"      st.lw=s.lw;st.cap=s.cap;st.fontPx=s.fontPx;st.fontSrc=s.fontSrc;st.m=s.m;"
"      st.dash=s.dash;st.dashOff=s.dashOff;st.align=s.align;st.base=s.base;"
"      st.shSrc=s.shSrc;st.shC=s.shC;st.shX=s.shX;st.shY=s.shY;st.shB=s.shB;"
"    };"
"    api.translate=function(x,y){mul([1,0,0,1,x,y]);};"
"    api.scale=function(x,y){mul([x,0,0,y,0,0]);};"
"    api.rotate=function(a){var c=Math.cos(a),s=Math.sin(a);mul([c,s,-s,c,0,0]);};"
"    api.transform=function(a,b,c,d,e,f){mul([a,b,c,d,e,f]);};"
"    api.setTransform=function(a,b,c,d,e,f){st.m=[a,b,c,d,e,f];};"
"    api.resetTransform=function(){st.m=[1,0,0,1,0,0];};"
"    api.beginPath=function(){subs=[];cur=null;};"
"    api.closePath=function(){"
"      if (cur&&cur.length>=4){cur.push(cur[0],cur[1]);subs[subs.length-1].closed=true;}"
"    };"
"    api.moveTo=function(x,y){cur=[x,y];subs.push({p:cur,closed:false});};"
"    api.lineTo=function(x,y){ensure();cur.push(x,y);};"
"    /* 曲线族: 全部在落账前折线化(固定段数, 口径见文件头注释)。 */"
"    function arcPts(x,y,rx,ry,rot,a0,a1,ccw){"
"      var TAU=Math.PI*2,sweep=a1-a0;"
"      if (ccw){while(sweep>0)sweep-=TAU;if(sweep===0)sweep=-TAU;}"
"      else{while(sweep<0)sweep+=TAU;if(sweep===0)return;}"
"      var n=Math.max(2,Math.ceil(Math.abs(sweep)/(Math.PI/12)));"
"      var co=Math.cos(rot||0),si=Math.sin(rot||0);"
"      function pt(a){var ex=rx*Math.cos(a),ey=ry*Math.sin(a);"
"        return [x+ex*co-ey*si, y+ex*si+ey*co];}"
"      var s0=pt(a0);"
"      if (cur&&cur.length>=2)cur.push(s0[0],s0[1]);"
"      else{cur=[s0[0],s0[1]];subs.push({p:cur,closed:false});}"
"      for (var i=1;i<=n;i++){var q2=pt(a0+sweep*i/n);cur.push(q2[0],q2[1]);}"
"    }"
"    api.arc=function(x,y,r,a0,a1,ccw){"
"      if (!(r>0)) return;"
"      arcPts(x,y,r,r,0,a0,a1,ccw);"
"    };"
"    api.ellipse=function(x,y,rx,ry,rot,a0,a1,ccw){"
"      if (!(rx>0&&ry>0)) return;"
"      arcPts(x,y,rx,ry,+rot||0,a0,a1,ccw);"
"    };"
"    api.quadraticCurveTo=function(cpx,cpy,x,y){"
"      ensure();var l=cur.length,p0x=cur[l-2],p0y=cur[l-1],n=16;"
"      for (var i=1;i<=n;i++){var t=i/n,u=1-t;"
"        cur.push(u*u*p0x+2*u*t*cpx+t*t*x, u*u*p0y+2*u*t*cpy+t*t*y);}"
"    };"
"    api.bezierCurveTo=function(c1x,c1y,c2x,c2y,x,y){"
"      ensure();var l=cur.length,p0x=cur[l-2],p0y=cur[l-1],n=24;"
"      for (var i=1;i<=n;i++){var t=i/n,u=1-t;"
"        cur.push(u*u*u*p0x+3*u*u*t*c1x+3*u*t*t*c2x+t*t*t*x,"
"                 u*u*u*p0y+3*u*u*t*c1y+3*u*t*t*c2y+t*t*t*y);}"
"    };"
"    /* arcTo: 与两条切线相切的圆弧(切点/圆心闭式解); 退化输入(半径非正/"
"       共线/折返)退化为 lineTo 到拐点。 */"
"    api.arcTo=function(x1,y1,x2,y2,r){"
"      ensure();var l=cur.length,p0x=cur[l-2],p0y=cur[l-1];"
"      var v1x=p0x-x1,v1y=p0y-y1,v2x=x2-x1,v2y=y2-y1;"
"      var l1=Math.sqrt(v1x*v1x+v1y*v1y),l2=Math.sqrt(v2x*v2x+v2y*v2y);"
"      if(!(r>0)||l1<1e-9||l2<1e-9){cur.push(x1,y1);return;}"
"      v1x/=l1;v1y/=l1;v2x/=l2;v2y/=l2;"
"      var cosA=v1x*v2x+v1y*v2y;"
"      if(cosA<=-0.99999||cosA>=0.99999){cur.push(x1,y1);return;}"
"      var sinA=Math.sqrt(1-cosA*cosA);"
"      var tLen=r*sinA/(1+cosA);"
"      var t1x=x1+v1x*tLen,t1y=y1+v1y*tLen,t2x=x1+v2x*tLen,t2y=y1+v2y*tLen;"
"      var bx=v1x+v2x,by=v1y+v2y,bl=Math.sqrt(bx*bx+by*by);"
"      if(bl<1e-9){cur.push(x1,y1);return;}"
"      var cd=r/Math.sqrt((1-cosA)/2);"
"      var cx=x1+bx/bl*cd,cy=y1+by/bl*cd;"
"      var a0=Math.atan2(t1y-cy,t1x-cx),a1=Math.atan2(t2y-cy,t2x-cx);"
"      arcPts(cx,cy,r,r,0,a0,a1,(v1x*v2y-v1y*v2x)<0);"
"    };"
"    api.fill=function(){"
"      var grad=(st.fill===null);"
"      for (var i=0;i<subs.length;i++){"
"        var p=subs[i].p;"
"        if (p.length<6) continue;"
"        if (!grad) send({op:'fpath',pts:p.slice(),col:rgba(st.fill)});"
"        else{var o={op:'gpath',pts:p.slice()};"
"          var gg=gradOp(st.fillSrc);for (var k in gg)o[k]=gg[k];send(o);}"
"      }"
"    };"
"    /* setLineDash: stroke 落账前按折线累计弧长切段(每段一个 spath;"
"       单次 stroke 上限 512 段, 到顶截断)。 */"
"    function dashSegs(p){"
"      var pat=st.dash,off=+st.dashOff||0;"
"      if(!pat||!pat.length)return [p];"
"      var L=0,i;for(i=0;i<pat.length;i++){pat[i]=+pat[i];if(!(pat[i]>0))pat[i]=0;L+=pat[i];}"
"      if(!(L>0))return [p];"
"      var out=[],run=null,q=off%L;if(q<0)q+=L;"
"      var idx=0,guard=0;"
"      while(q>=pat[idx]&&guard++<4096){q-=pat[idx];idx=(idx+1)%pat.length;}"
"      var on=(idx%2===0);"
"      if(on)run=[p[0],p[1]];"
"      var n=p.length/2;"
"      for(i=1;i<n;i++){"
"        var x0=p[(i-1)*2],y0=p[(i-1)*2+1],x1=p[i*2],y1=p[i*2+1];"
"        var dx=x1-x0,dy=y1-y0,seg=Math.sqrt(dx*dx+dy*dy);"
"        if(seg<1e-9)continue;"
"        var t=0;"
"        while(t<seg-1e-9){"
"          var step=pat[idx]-q;if(step>seg-t)step=seg-t;"
"          t+=step;q+=step;"
"          var bx2=x0+dx*(t/seg),by2=y0+dy*(t/seg);"
"          if(on)run.push(bx2,by2);"
"          if(q>=pat[idx]-1e-9){"
"            if(on&&run&&run.length>=4)out.push(run);"
"            q=0;idx=(idx+1)%pat.length;on=(idx%2===0);"
"            run=on?[bx2,by2]:null;"
"          }"
"        }"
"      }"
"      if(on&&run&&run.length>=4)out.push(run);"
"      return out;"
"    }"
"    api.stroke=function(){"
"      var grad=(st.stroke===null);"
"      var col=grad?0:rgba(solidOf(st.strokeSrc));"
"      var w=st.lw*detScale(),cap=(st.cap==='round');"
"      for (var i=0;i<subs.length;i++){"
"        var p=subs[i].p;"
"        if (p.length<4) continue;"
"        var segs=(st.dash&&st.dash.length)?dashSegs(p):[p];"
"        var total=0;"
"        for (var d=0;d<segs.length&&total<512;d++,total++){"
"          var o={op:'spath',pts:segs[d],col:col,w:w,cap:cap};"
"          if (grad){var gg=gradOp(st.strokeSrc);for (var k in gg)o[k]=gg[k];}"
"          send(o);"
"        }"
"      }"
"    };"
"    api.clip=function(){"
"      var minx=1e9,miny=1e9,maxx=-1e9,maxy=-1e9,any=false;"
"      for (var i=0;i<subs.length;i++)"
"        for (var j=0;j+1<subs[i].p.length;j+=2){"
"          var q=xf(subs[i].p[j],subs[i].p[j+1]);"
"          if (q[0]<minx)minx=q[0];if(q[0]>maxx)maxx=q[0];"
"          if (q[1]<miny)miny=q[1];if(q[1]>maxy)maxy=q[1];"
"          any=true;"
"        }"
"      if (!any) return;"
"      send({op:'clip',x:minx,y:miny,w:maxx-minx,h:maxy-miny});"
"      st.clips++;"
"    };"
"    api.fillRect=function(x,y,w,h){fillRectImpl(x,y,w,h);};"
"    api.strokeRect=function(x,y,w,h){"
"      var p=rectPts(x,y,w,h),t=[];"
"      for (var i=0;i<8;i+=2){var q=xf(p[i],p[i+1]);t.push(q[0],q[1]);}"
"      t.push(t[0],t[1]);"
"      send({op:'spath',pts:t,col:rgba(solidOf(st.strokeSrc)),w:st.lw*detScale(),"
"           cap:(st.cap==='round')});"
"    };"
"    api.fillText=function(s,x,y){textOp(s,x,y,st.fillSrc);};"
"    api.strokeText=function(s,x,y){textOp(s,x,y,st.strokeSrc);};"
"    api.measureText=function(s){return {width:measureW(String(s))};};"
"    api.createLinearGradient=function(x0,y0,x1,y1){"
"      return {__hngrad:true,x0:+x0||0,y0:+y0||0,x1:+x1||0,y1:+y1||0,stops:[],"
"        addColorStop:function(t,color){this.stops.push({t:+t||0,c:parseColor(color)});}};"
"    };"
"    api.createRadialGradient=function(x0,y0,r0,x1,y1,r1){"
"      return {__hngrad:true,radial:true,x0:+x0||0,y0:+y0||0,r0:+r0||0,"
"              x1:+x1||0,y1:+y1||0,r1:+r1||0,stops:[],"
"        addColorStop:function(t,color){this.stops.push({t:+t||0,c:parseColor(color)});}};"
"    };"
"    api.setLineDash=function(a){"
"      if(!a||!a.length){st.dash=[];return;}"
"      var d=[];for(var i=0;i<a.length;i++)d.push(+a[i]||0);"
"      st.dash=d;"
"    };"
"    api.getLineDash=function(){return st.dash.slice();};"
"    api.clearRect=function(x,y,w,h){"
"      if (w===undefined){send({op:'clear'});return;}"
"      var m=st.m,q=xf(x,y),tw=m[0]*w,th=m[3]*h;"
"      if (tw<0){q[0]+=tw;tw=-tw;}"
"      if (th<0){q[1]+=th;th=-th;}"
"      send({op:'clear_rect',x:q[0],y:q[1],w:tw,h:th});"
"    };"
"    api.drawImage=function(img,x,y,w,h){"
"      var src=(typeof img==='string')?img:(img&&img.src?img.src:'');"
"      if (!src) return;"
"      var q=xf(x,y),m=st.m;"
"      var dw=(w===undefined)?100:w,dh=(h===undefined)?100:h;"
"      send({op:'image',src:src,x:q[0],y:q[1],w:dw*m[0],h:dh*m[3]});"
"    };"
"    return api;"
"  }"
"})(globalThis);";

/* JS 运行时表(按 scope_key 隔离; daemon 的不同应用用不同 key)。
   bridge 槽随上下文持有(opaque 指向它), 每次 eval 按 doc/ctx 刷新;
   glue_done/timer_glue_done = 各 glue 已装入(每 scope 只装一次)。 */
#define JS_MAX_SCOPES 32
static struct { char key[128]; JSRuntime *rt; JSContext *ctx; js_bridge bridge;
                int glue_done, timer_glue_done; } js_scopes[JS_MAX_SCOPES];
static int js_scope_n = 0;

static JSContext *js_scope_get(const char *key) {
    for (int i = 0; i < js_scope_n; i++)
        if (!strcmp(js_scopes[i].key, key)) return js_scopes[i].ctx;
    if (js_scope_n >= JS_MAX_SCOPES) return NULL;
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) return NULL;
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { JS_FreeRuntime(rt); return NULL; }
    snprintf(js_scopes[js_scope_n].key, sizeof(js_scopes[0].key), "%s", key);
    js_scopes[js_scope_n].rt = rt;
    js_scopes[js_scope_n].ctx = ctx;
    memset(&js_scopes[js_scope_n].bridge, 0, sizeof(js_bridge));
    js_scopes[js_scope_n].glue_done = 0;
    js_scopes[js_scope_n].timer_glue_done = 0;
    JS_SetContextOpaque(ctx, &js_scopes[js_scope_n].bridge);
    js_scope_n++;
    return ctx;
}

/* 只查找不创建(hn_rt_frame 驱动定时器用 —— 帧路径不允许副作用建表) */
static JSContext *js_scope_find(const char *key) {
    for (int i = 0; i < js_scope_n; i++)
        if (!strcmp(js_scopes[i].key, key)) return js_scopes[i].ctx;
    return NULL;
}

/* rt ↔ scope 绑定: 桥函数(setTimeout 等)只有 JSContext*, 经 opaque 的
   js_bridge.rt 反查到挂定时器表的 rt。hn_rt_open 绑定 / hn_rt_close 解绑;
   同 key 的旧 rt 被新 rt 覆盖(后者接管此后注册的定时器 —— 每窗一进程,
   同 key 双活不是真实场景)。 */
static void js_scope_bind_rt(hn_rt *rt) {
    if (!rt) return;
    JSContext *ctx = js_scope_get(rt->id);   /* 惰性建 scope(与文档脚本同一上下文) */
    if (!ctx) return;
    for (int i = 0; i < js_scope_n; i++)
        if (js_scopes[i].ctx == ctx) { js_scopes[i].bridge.rt = rt; return; }
}

static void js_scope_unbind_rt(hn_rt *rt) {
    if (!rt) return;
    for (int i = 0; i < js_scope_n; i++)
        if (js_scopes[i].bridge.rt == rt) js_scopes[i].bridge.rt = NULL;
}

/* 释放 rt 定时器/RAF 表持有的全部 JSValue 引用(hn_rt_close 调; 必须在
   JS 上下文消亡前 —— 上下文属 js_scopes 全局表, 生命周期长于任何 rt)。 */
static void js_rt_release_timers(hn_rt *rt) {
    JSContext *ctx = js_scope_find(rt->id);
    if (ctx) {
        for (int i = 0; i < 64; i++) {
            if (!rt->timers[i].id) continue;
            JS_FreeValue(ctx, rt->timers[i].func);
            if (!JS_IsUndefined(rt->timers[i].args)) JS_FreeValue(ctx, rt->timers[i].args);
            rt->timers[i].id = 0;
        }
        for (int i = 0; i < rt->raf_n; i++) JS_FreeValue(ctx, rt->raf_q[i].cb);
        /* 顺手 GC: 回调闭包可能成环(自引用), 引用清零后让环可回收 */
        for (int i = 0; i < js_scope_n; i++)
            if (!strcmp(js_scopes[i].key, rt->id)) JS_RunGC(js_scopes[i].rt);
    }
    rt->timer_n = 0;
    rt->raf_n = 0;
}

/* ---------------- JS 定时器与 RAF(帧驱动) ----------------
 * QuickJS 没有后台事件线程 —— 标准定时语义由 hn_rt_frame 每帧驱动:
 *   setTimeout/setInterval: 到期即调(hn_rt_frame 开头, 每帧一次扫描);
 *   requestAnimationFrame: 下一帧开头执行一次(注册序, 不做时间对齐)。
 * 时钟是 rt 的虚拟单调毫秒(clock_ms += dt): 真窗壳喂真实间隔 ⇒ 与单调钟
 * 一致; 探针喂固定 16 ⇒ "3 帧×16ms 不触发, 第 4 帧触发"可确定性断言。
 * 纪律(帧驱动定时器的通用约束):
 *   - 单帧最多 8 个回调(RAF+到期定时器合计), 超出顺延下一帧 —— 回调
 *     再狂也不饿死主循环;
 *   - 单次定时器先摘槽再调用(回调里 clearTimeout 自身安全), 调用前
 *     DupValue(回调里 clearInterval 自身安全);
 *   - 回调异常吞掉打 stderr(与 hn_rt_eval 同口径), 不向上传播;
 *   - 跑过回调即按"可能改了画面"处理: rt_render + repaint 置位, 桥改动
 *     (hnSetText/hnCanvas2D)经既有管线落像素。
 * C 原生走 __hn* 内部名, 标准签名由 TIMER_GLUE_JS 转接(可变参收敛为
 * 参数数组)。无宿主 rt 的 eval(daemon 裸 scope)注册定时器返回 0 ——
 * 定时器是帧驱动的, 帧属于 rt, 没有宿主就没有时钟。 */

/* __hnTimer(fn, ms, argsArray, periodic) → 句柄(>0; 表满/无效 0) */
static JSValue js_hn_timer(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    hn_rt *rt = b ? b->rt : NULL;
    if (!rt || argc < 2 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
    int slot = -1;
    for (int i = 0; i < 64; i++)
        if (!rt->timers[i].id) { slot = i; break; }
    if (slot < 0) return JS_NewInt32(ctx, 0);          /* 表满: 句柄 0 */
    double ms = 0;
    JS_ToFloat64(ctx, &ms, argv[1]);
    if (!(ms >= 0)) ms = 0;                            /* NaN/负数 → 0(浏览器同义) */
    int periodic = 0;
    if (argc > 3) { int32_t p = 0; JS_ToInt32(ctx, &p, argv[3]); periodic = p != 0; }
    int id = ++rt->timer_seq;
    rt->timers[slot].id = id;
    rt->timers[slot].due = rt->clock_ms + ms;
    rt->timers[slot].interval_ms = periodic ? ms : 0.0;
    rt->timers[slot].func = JS_DupValue(ctx, argv[0]);
    rt->timers[slot].args = (argc > 2 && JS_IsArray(ctx, argv[2]))
                          ? JS_DupValue(ctx, argv[2]) : JS_UNDEFINED;
    rt->timer_n++;
    return JS_NewInt32(ctx, id);
}

/* __hnTimerClear(id): clearTimeout/clearInterval 共用(同一张表, 浏览器
   也允许互换) */
static JSValue js_hn_timer_clear(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    hn_rt *rt = b ? b->rt : NULL;
    if (!rt || argc < 1) return JS_UNDEFINED;
    int32_t id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    if (id <= 0) return JS_UNDEFINED;                  /* 失效句柄: 静默 */
    for (int i = 0; i < 64; i++) {
        if (rt->timers[i].id != id) continue;
        JS_FreeValue(ctx, rt->timers[i].func);
        if (!JS_IsUndefined(rt->timers[i].args)) JS_FreeValue(ctx, rt->timers[i].args);
        rt->timers[i].func = JS_UNDEFINED;
        rt->timers[i].args = JS_UNDEFINED;
        rt->timers[i].id = 0;
        rt->timer_n--;
        break;
    }
    return JS_UNDEFINED;
}

/* __hnRAF(cb) → 句柄; __hnRAFCancel(id)。挂进下一帧队列(本帧回调里
   再挂的落到队尾 = 下下帧, 天然防重入) */
static JSValue js_hn_raf(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    hn_rt *rt = b ? b->rt : NULL;
    if (!rt || argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
    if (rt->raf_n >= 64) return JS_NewInt32(ctx, 0);   /* 队满: 丢弃 */
    int id = ++rt->raf_seq;
    rt->raf_q[rt->raf_n].id = id;
    rt->raf_q[rt->raf_n].cb = JS_DupValue(ctx, argv[0]);
    rt->raf_n++;
    return JS_NewInt32(ctx, id);
}

static JSValue js_hn_raf_cancel(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    hn_rt *rt = b ? b->rt : NULL;
    if (!rt || argc < 1) return JS_UNDEFINED;
    int32_t id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    if (id <= 0) return JS_UNDEFINED;
    for (int i = 0; i < rt->raf_n; i++) {
        if (rt->raf_q[i].id != id) continue;
        JS_FreeValue(ctx, rt->raf_q[i].cb);
        rt->raf_q[i].cb = JS_UNDEFINED;
        memmove(&rt->raf_q[i], &rt->raf_q[i + 1],
                (size_t)(rt->raf_n - i - 1) * sizeof(rt->raf_q[0]));   /* 保注册序 */
        rt->raf_n--;
        break;
    }
    return JS_UNDEFINED;
}

/* 统一回调调用: RAF 带时间戳(clock_ms, 仿 DOMHighResTimeStamp);
   定时器展开参数数组(上限 8 个, 多余丢弃 —— 最小版可变参)。异常吞掉
   打 stderr 不崩(与 hn_rt_eval 同一口径)。func/args 由调用方 Dup 过,
   生命周期覆盖本次调用。 */
static void js_invoke(JSContext *ctx, JSValue func, JSValue args,
                      int with_ts, double ts) {
    JSValue argv[8];
    int na = 0;
    if (with_ts) {
        argv[na++] = JS_NewFloat64(ctx, ts);
    } else if (!JS_IsUndefined(args)) {
        JSValue jlen = JS_GetPropertyStr(ctx, args, "length");
        int32_t len = 0;
        JS_ToInt32(ctx, &len, jlen);
        JS_FreeValue(ctx, jlen);
        if (len > 8) len = 8;
        for (; na < len; na++)
            argv[na] = JS_GetPropertyUint32(ctx, args, (uint32_t)na);
    }
    JSValue r = JS_Call(ctx, func, JS_UNDEFINED, na, argv);
    for (int i = 0; i < na; i++) JS_FreeValue(ctx, argv[i]);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, e);
        fprintf(stderr, "[timer] %s\n", msg ? msg : "exception");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r);
}

/* hn_rt_frame 的回调驱动: RAF 队头 → 到期定时器, 合计 ≤8 个; 跑过至少
   一个就 rt_render + repaint 置位。返回是否跑过回调。 */
static int js_timers_drain(hn_rt *rt, int *repaint) {
    if (rt->timer_n == 0 && rt->raf_n == 0) return 0;
    JSContext *ctx = js_scope_find(rt->id);
    if (!ctx) return 0;
    /* 桥环境对准当前文档(hot reload 换 doc 后回调不悬空; 与 hn_rt_eval
       每次的刷新同一份语义)。rt 槽不动 —— 归属关系不经帧改写。 */
    hn_doc *doc = hn_context_doc(rt->ctx);
    for (int i = 0; i < js_scope_n; i++) {
        if (js_scopes[i].ctx == ctx) {
            js_scopes[i].bridge.doc = doc;
            js_scopes[i].bridge.ctx = rt->ctx;
            break;
        }
    }
    int budget = 8, ran = 0;

    /* RAF: 队头快照(注册序; 预算外留队, 下一帧优先)。先摘队再调用 ——
       回调里挂新 RAF 追加到队尾, 不会被本帧重入。 */
    int n = rt->raf_n < budget ? rt->raf_n : budget;
    JSValue cbs[8];
    for (int i = 0; i < n; i++) cbs[i] = JS_DupValue(ctx, rt->raf_q[i].cb);
    if (n < rt->raf_n)
        memmove(&rt->raf_q[0], &rt->raf_q[n],
                (size_t)(rt->raf_n - n) * sizeof(rt->raf_q[0]));
    rt->raf_n -= n;
    for (int i = 0; i < n; i++) {
        js_invoke(ctx, cbs[i], JS_UNDEFINED, 1, rt->clock_ms);
        JS_FreeValue(ctx, cbs[i]);
        budget--;
        ran = 1;
    }

    /* 到期定时器: 单趟扫描(各限一次)。单次触发先摘槽再调用; 周期触发
       due 顺延 interval(错过的帧不追发 —— 折叠为一次, 与浏览器掉帧语义
       一致)。回调里 clearTimeout/clearInterval 自身都安全(func 已 Dup,
       单次槽已摘)。 */
    double now = rt->clock_ms;
    for (int i = 0; i < 64 && budget > 0; i++) {
        if (!rt->timers[i].id || rt->timers[i].due > now) continue;
        JSValue func = JS_DupValue(ctx, rt->timers[i].func);
        JSValue args = JS_UNDEFINED;
        if (!JS_IsUndefined(rt->timers[i].args)) args = JS_DupValue(ctx, rt->timers[i].args);
        int once = (rt->timers[i].interval_ms <= 0);
        if (once) {
            JS_FreeValue(ctx, rt->timers[i].func);
            if (!JS_IsUndefined(rt->timers[i].args)) JS_FreeValue(ctx, rt->timers[i].args);
            rt->timers[i].func = JS_UNDEFINED;
            rt->timers[i].args = JS_UNDEFINED;
            rt->timers[i].id = 0;
            rt->timer_n--;
        } else {
            rt->timers[i].due = now + rt->timers[i].interval_ms;
        }
        js_invoke(ctx, func, args, 0, 0);
        JS_FreeValue(ctx, func);
        if (!JS_IsUndefined(args)) JS_FreeValue(ctx, args);
        budget--;
        ran = 1;
    }

    if (ran) { rt_render(rt); if (repaint) *repaint = 1; }
    return ran;
}

/* 定时器/RAF 标准签名 glue: __hn* 内部桥 → 浏览器同款全局函数。
   幂等(typeof 检查), 与 canvas glue 同一装载纪律(每 scope 一次)。 */
static const char *TIMER_GLUE_JS =
"(function(g){"
"  if (typeof g.setTimeout === 'function') return;"
"  g.setTimeout=function(fn,ms){"
"    if (typeof fn!=='function') return 0;"
"    return __hnTimer(fn,+ms||0,[].slice.call(arguments,2),0);};"
"  g.setInterval=function(fn,ms){"
"    if (typeof fn!=='function') return 0;"
"    return __hnTimer(fn,+ms||0,[].slice.call(arguments,2),1);};"
"  g.clearTimeout=function(id){__hnTimerClear(+id||0);};"
"  g.clearInterval=function(id){__hnTimerClear(+id||0);};"
"  g.requestAnimationFrame=function(cb){"
"    if (typeof cb!=='function') return 0;"
"    return __hnRAF(cb);};"
"  g.cancelAnimationFrame=function(h){__hnRAFCancel(+h||0);};"
"})(globalThis);";

char *hn_rt_eval(const char *js, const char *scope_key, hn_doc *doc) {
    JSContext *ctx = js_scope_get(scope_key ? scope_key : "default");
    if (!ctx) return NULL;
    /* 桥环境: opaque = {doc, ctx}(hnSetText/hnSetValue 读 .doc; hnMedia*/
    /* 读 .ctx —— ctx 由 doc 反查表得到, 未登记的 context 媒体桥降级 no-op) */
    int glue_loaded = 0;
    for (int i = 0; i < js_scope_n; i++) {
        if (js_scopes[i].ctx == ctx) {
            js_scopes[i].bridge.doc = doc;
            js_scopes[i].bridge.ctx = doc ? doc_ctx_find(doc) : NULL;
            glue_loaded = js_scopes[i].glue_done;
            break;
        }
    }

    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "hnSetText",
        JS_NewCFunction(ctx, js_hn_set_text, "hnSetText", 2));
    JS_SetPropertyStr(ctx, global, "hnSetValue",
        JS_NewCFunction(ctx, js_hn_set_value, "hnSetValue", 2));
    JS_SetPropertyStr(ctx, global, "hnMeshParam",
        JS_NewCFunction(ctx, js_hn_mesh_param, "hnMeshParam", 3));
    JS_SetPropertyStr(ctx, global, "hnMediaPlay",
        JS_NewCFunction(ctx, js_hn_media_play, "hnMediaPlay", 1));
    JS_SetPropertyStr(ctx, global, "hnMediaPause",
        JS_NewCFunction(ctx, js_hn_media_pause, "hnMediaPause", 1));
    JS_SetPropertyStr(ctx, global, "hnMediaSeek",
        JS_NewCFunction(ctx, js_hn_media_seek, "hnMediaSeek", 2));
    JS_SetPropertyStr(ctx, global, "hnMediaTime",
        JS_NewCFunction(ctx, js_hn_media_time, "hnMediaTime", 1));
    JS_SetPropertyStr(ctx, global, "hnMediaDuration",
        JS_NewCFunction(ctx, js_hn_media_duration, "hnMediaDuration", 1));
    JS_SetPropertyStr(ctx, global, "hnMediaVolume",
        JS_NewCFunction(ctx, js_hn_media_volume, "hnMediaVolume", 2));
    JS_SetPropertyStr(ctx, global, "hnWasmLoad",
        JS_NewCFunction(ctx, js_hn_wasm_load, "hnWasmLoad", 1));
    JS_SetPropertyStr(ctx, global, "hnWasmCall",
        JS_NewCFunction(ctx, js_hn_wasm_call, "hnWasmCall", 2));
    JS_SetPropertyStr(ctx, global, "hnCanvas2D",
        JS_NewCFunction(ctx, js_hn_canvas_2d, "hnCanvas2D", 1));
    JS_SetPropertyStr(ctx, global, "hnMeasureText",
        JS_NewCFunction(ctx, js_hn_measure_text, "hnMeasureText", 2));
    /* 定时器/RAF 内部桥(__hn*; 标准签名由 TIMER_GLUE_JS 转接) */
    JS_SetPropertyStr(ctx, global, "__hnTimer",
        JS_NewCFunction(ctx, js_hn_timer, "__hnTimer", 4));
    JS_SetPropertyStr(ctx, global, "__hnTimerClear",
        JS_NewCFunction(ctx, js_hn_timer_clear, "__hnTimerClear", 1));
    JS_SetPropertyStr(ctx, global, "__hnRAF",
        JS_NewCFunction(ctx, js_hn_raf, "__hnRAF", 1));
    JS_SetPropertyStr(ctx, global, "__hnRAFCancel",
        JS_NewCFunction(ctx, js_hn_raf_cancel, "__hnRAFCancel", 1));
    JS_FreeValue(ctx, global);

    /* canvas 2D glue(hnGet2D + 状态机): 每 scope 只装一次(幂等定义,
       重复 eval 也安全, 这里省掉重复解析成本) */
    if (!glue_loaded) {
        JSValue gr = JS_Eval(ctx, CANVAS_GLUE_JS, strlen(CANVAS_GLUE_JS),
                             "<canvas-glue>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(gr)) {
            JSValue e = JS_GetException(ctx);   /* glue 坏了不拖垮 eval: 吞异常 */
            JS_FreeValue(ctx, e);
        }
        JS_FreeValue(ctx, gr);
        for (int i = 0; i < js_scope_n; i++)
            if (js_scopes[i].ctx == ctx) { js_scopes[i].glue_done = 1; break; }
    }

    /* 定时器/RAF glue(setTimeout/setInterval/requestAnimationFrame 标准
       签名): 同样每 scope 一次 */
    int timer_glue_loaded = 0;
    for (int i = 0; i < js_scope_n; i++)
        if (js_scopes[i].ctx == ctx) { timer_glue_loaded = js_scopes[i].timer_glue_done; break; }
    if (!timer_glue_loaded) {
        JSValue gr = JS_Eval(ctx, TIMER_GLUE_JS, strlen(TIMER_GLUE_JS),
                             "<timer-glue>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(gr)) {
            JSValue e = JS_GetException(ctx);
            JS_FreeValue(ctx, e);
        }
        JS_FreeValue(ctx, gr);
        for (int i = 0; i < js_scope_n; i++)
            if (js_scopes[i].ctx == ctx) { js_scopes[i].timer_glue_done = 1; break; }
    }

    size_t len = strlen(js);
    JSValue r = JS_Eval(ctx, js, len, "<eval>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, e);
        char *out = (char *)malloc(strlen(msg ? msg : "exception") + 16);
        if (out) sprintf(out, "Error: %s", msg ? msg : "?");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, e);
        return out;
    }
    const char *str = JS_ToCString(ctx, r);
    char *out = str ? strdup(str) : NULL;
    JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, r);
    return out;
}

/* ---------------- 文档内联 <script> 自动执行 ----------------
 * 整文档装载(hn_rt_open / hn_rt_render 热替换)后按文档序执行 <script>
 * 的文本 —— 与浏览器"解析即执行"对齐, 让 canvas 2D 这类纯 JS 驱动的
 * 页面无需外部 eval 驱动即可离线渲染(--shot / headless 全链路可用)。
 * hx 片段交换不重跑既有脚本(片段是数据, 不是新文档)。
 * 只用 hn.h 的公开遍历 API(first_child/next_sibling/tag/text —— rt 层
 * 不接触引擎内部结构); 脚本指针先快照后执行: eval 内的 DOM 改写不会
 * 移动 arena 内存, 指针稳定。 */
static void run_doc_scripts(hn_rt *rt) {
    hn_doc *doc = hn_context_doc(rt->ctx);
    if (!doc) return;
    hn_node *root = hn_doc_root(doc);
    if (!root) return;
    const char *texts[32];
    size_t lens[32];
    hn_node *stack[128];
    int sp = 0, n = 0;
    stack[sp++] = root;
    while (sp > 0 && n < 32) {
        hn_node *cur = stack[--sp];
        const char *tag = hn_node_tag(cur);
        if (tag && !strcmp(tag, "script")) {
            for (hn_node *ch = hn_node_first_child(cur); ch && n < 32;
                 ch = hn_node_next_sibling(ch)) {
                size_t len = 0;
                const char *t = hn_node_text(ch, &len);
                if (t && len) { texts[n] = t; lens[n] = len; n++; }
            }
        }
        /* 子节点逆序入栈 → 出栈为文档序(先序遍历) */
        hn_node *kids[64];
        int nk = 0;
        for (hn_node *ch = hn_node_first_child(cur); ch && nk < 64;
             ch = hn_node_next_sibling(ch))
            kids[nk++] = ch;
        for (int k = nk - 1; k >= 0 && sp < 128; k--)
            stack[sp++] = kids[k];
    }
    for (int i = 0; i < n; i++) {
        char *r = hn_rt_eval(texts[i], rt->id, doc);
        if (r && !strncmp(r, "Error:", 6))
            fprintf(stderr, "[script] %s\n", r);   /* 脚本异常可见而非静默 */
        free(r);
    }
}

#else
/* 无 QuickJS 构建: 定时器/RAF 全套 no-op(hn_rt_open/close/frame 的调用点
   无需知道差别 —— 表恒空, 帧节拍不含 JS 信号)。js_rt_free 空实现。 */
static void js_rt_free(hn_rt *rt) { (void)rt; }
static void js_rt_release_timers(hn_rt *rt) { (void)rt; }
static void js_scope_bind_rt(hn_rt *rt) { (void)rt; }
static void js_scope_unbind_rt(hn_rt *rt) { (void)rt; }
static int js_timers_drain(hn_rt *rt, int *repaint) { (void)rt; (void)repaint; return 0; }

/* 文档内联 <script> 执行同样依赖 QuickJS: 无 JS 引擎时不跑(eval 口径
   会如实报告"QuickJS 未编入本产物", 这里保持同一降级纪律)。 */
static void run_doc_scripts(hn_rt *rt) { (void)rt; }

char *hn_rt_eval(const char *js, const char *scope_key, hn_doc *doc) {
    (void)js; (void)scope_key; (void)doc;
    static const char msg[] = "QuickJS 未编入本产物(构建时未定义 HN_HAVE_QUICKJS)";
    char *out = (char *)malloc(sizeof(msg) + 8);
    if (out) sprintf(out, "Error: %s", msg);
    return out;
}
#endif

void hn_rt_close(hn_rt *rt) {
    if (!rt) return;
    js_rt_release_timers(rt);   /* 定时器/RAF 持有的 JSValue 先还(上下文仍活着) */
    js_scope_unbind_rt(rt);     /* 桥反查表里不悬空 —— 宿主没了定时器即失效 */
    js_rt_free(rt);
    if (rt->ctx) {
        doc_ctx_unbind_ctx(rt->ctx);   /* 悬空 ctx 不留在反查表里 */
        hn_context_destroy(rt->ctx);
    }
    free(rt->bgra);
    free(rt);
}

int hn_rt_render(hn_rt *rt, const char *html, size_t len) {
    if (!rt || !html) return 0;
    hn_doc *doc = hn_parse_html(html, len);
    if (!doc) return 0;
    hn_doc_autoid_hx(doc);
    doc_ctx_unbind_doc(hn_context_doc(rt->ctx));   /* 旧文档的登记一并撤掉 */
    hn_context_set_doc(rt->ctx, doc);
    doc_ctx_bind(doc, rt->ctx);        /* 新文档重新登记(eval 桥按 doc 反查) */
    rt->poll_built = 0;
    int ok = rt_render(rt);
    if (ok) { run_doc_scripts(rt); run_load_actions(rt); rt_render(rt); }
    return ok;
}

int hn_rt_reflow(hn_rt *rt) { return rt ? rt_render(rt) : 0; }

const unsigned char *hn_rt_pixels(hn_rt *rt) { return rt ? rt->bgra : NULL; }
int hn_rt_width(hn_rt *rt) { return rt ? rt->w : 0; }
int hn_rt_height(hn_rt *rt) { return rt ? rt->h : 0; }

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec / 1e9;
}

int hn_rt_frame(hn_rt *rt, float dt_ms, int *repaint) {
    if (!rt) return -1;
    if (repaint) *repaint = 0;
#ifdef HN_HAVE_QUICKJS
    rt->clock_ms += dt_ms > 0 ? (double)dt_ms : 0;   /* 虚拟单调钟: 随帧步进 */
#endif
    /* JS 定时器与 RAF 先行(帧开头语义): 到期回调、画布/文本改动在此
       渲染并置 repaint。 */
    js_timers_drain(rt, repaint);

    int anim = hn_context_anim_tick(rt->ctx, dt_ms);
    if (anim) { rt_render(rt); if (repaint) *repaint = 1; }

    /* 轮询表(懒建: 内容变化后重建) */
    hn_doc *doc = hn_context_doc(rt->ctx);
    if (!rt->poll_built) {
        rt->poll_n = 0;
        double now = now_s();
        for (int i = 0;; i++) {
            const char *id = NULL;
            int ms = 0;
            if (!hn_doc_poll_at(doc, i, &id, &ms)) break;
            if (rt->poll_n >= 16) break;
            hn_node *n = hn_doc_find_by_id(doc, id);
            if (n) {
                double sec = ms > 0 ? ms / 1000.0 : 1.0;
                if (sec < 0.05) sec = 0.05;
                rt->polls[rt->poll_n].node = n;
                rt->polls[rt->poll_n].due = now + sec;
                rt->polls[rt->poll_n].sec = sec;
                rt->poll_n++;
            }
        }
        rt->poll_built = 1;
    }
    double now = now_s();
    double next = -1;
    for (int i = 0; i < rt->poll_n; i++) {
        if (now >= rt->polls[i].due) {
            if (hx_perform(rt, rt->polls[i].node)) { rt_render(rt); if (repaint) *repaint = 1; }
            rt->polls[i].due = now + rt->polls[i].sec;
        }
        double wait = rt->polls[i].due - now;
        if (next < 0 || wait < next) next = wait;
    }

    /* ---- 唤醒节拍合成(取各信号的最小者; -1 = 无事可睡) ----
       RAF 待决        → 16 (活画面: 60fps 帧循环)
       定时器表非空    → min(16, 最近到期-现在) (到期即先跑回调)
       动画在跑        → 16 (过渡/keyframes/Lottie/媒体)
       hx 轮询期限     → 距最近到期的毫秒数
       全无            → -1 (宿主按自己的空闲节奏低频醒来) */
    int beat = -1;
#ifdef HN_HAVE_QUICKJS
    if (rt->raf_n > 0) beat = 16;
    if (rt->timer_n > 0) {
        double dmin = -1;
        for (int i = 0; i < 64; i++) {
            if (!rt->timers[i].id) continue;
            double d = rt->timers[i].due - rt->clock_ms;
            if (d < 0) d = 0;                        /* 预算耗尽没跑成的: 立刻再约 */
            if (dmin < 0 || d < dmin) dmin = d;
        }
        if (dmin >= 0) {
            int tb = (int)dmin;
            if (tb < 1) tb = 1;
            if (tb > 16) tb = 16;
            if (beat < 0 || tb < beat) beat = tb;
        }
    }
#endif
    if (anim && (beat < 0 || 16 < beat)) beat = 16;
    if (next > 0) {
        int pb = (int)(next * 1000.0);
        if (beat < 0 || pb < beat) beat = pb;
    }
    return beat;
}

int hn_rt_pointer_move(hn_rt *rt, float x, float y) {
    if (!rt) return 0;
    /* 喂坐标(在 hover 无变化的早退之前 —— 指针在同一元素内滑动时
       hover 不变, 但网格 hn-mesh-look 的指向每一步都要跟上)。 */
    hn_context_set_pointer(rt->ctx, x, y);
    hn_node *n = hn_context_hit_node(rt->ctx, x, y);
    if (n == rt->hover) return 0;
    rt->hover = n;
    hn_context_set_hover(rt->ctx, n);
    return rt_render(rt);
}

int hn_rt_button(hn_rt *rt, int down, float x, float y, int button) {
    if (!rt || !down || button != 0) return 0;
    const char *id = hn_context_hit_test(rt->ctx, x, y);
    hn_node *n = hn_context_hit_node(rt->ctx, x, y);
    printf("[click] (%.0f,%.0f) -> <%s id=%s>\n", x, y,
           n ? hn_node_tag(n) : "?", id ? id : "(none)");
    rt->focus = n ? hn_node_ancestor_input(n) : NULL;
    if (rt->focus) hn_context_set_focus(rt->ctx, rt->focus);
    hn_node *carrier = hx_carrier(n);
    if (carrier) {
        const char *url = hn_node_attr(carrier, "hx-post") ? hn_node_attr(carrier, "hx-post")
                                                          : hn_node_attr(carrier, "hx-get");
        printf("[hx] %s\n", url ? url : "?");
        fflush(stdout);
        if (hx_perform(rt, carrier)) return rt_render(rt);
    }
    /* 链接导航(md 直染的 a[href]): #锚引擎内滚进可视区;
       外部链接交系统默认方式打开(sys://open 同一出口), 不在窗内导航 */
    int lk = hn_context_activate_link(rt->ctx, x, y);
    if (lk == HN_LINK_ANCHOR) return rt_render(rt);
    if (lk == HN_LINK_EXTERNAL) {
        char url[2048] = { 0 };
        hn_doc_take_pending_link(hn_context_doc(rt->ctx), url, sizeof url);
        if (url[0]) sys_open_url(url);
        return rt_render(rt);
    }
    return 0;
}

int hn_rt_scroll(hn_rt *rt, float dx, float dy, float x, float y) {
    (void)dx;
    if (!rt) return 0;
    hn_node *n = hn_context_hit_node(rt->ctx, x, y);
    if (n && hn_node_scroll_by(n, 0, -dy * 18.0f)) return rt_render(rt);
    return 0;
}

int hn_rt_key(hn_rt *rt, int down, int key, const char *utf8) {
    if (!rt || !down) return 0;
    hn_node *target = rt->focus ? rt->focus : hn_doc_root(hn_context_doc(rt->ctx));
    if (key == 2 /*RETURN*/) {
        hn_node *carrier = hx_carrier(target);
        if (carrier && hx_perform(rt, carrier)) return rt_render(rt);
        return 0;
    }
    if (key == 3 /*BACKSPACE*/ && rt->focus) {
        /* 删除末字符 */
        char val[4096];
        size_t vl = 0;
        hn_node_set_value(rt->focus, "", 0);
        /* 读当前值 → 截末字符 → 写回 */
        const char *old = NULL;
        hn_node *n = rt->focus;
        if (hn_node_attr(n, "value")) old = hn_node_attr(n, "value");
        if (old) {
            size_t ol = strlen(old);
            if (ol > 0) {
                /* UTF-8: 回退到字符边界(跳过 continuation byte 10xxxxxx) */
                size_t cut = ol - 1;
                while (cut > 0 && (old[cut] & 0xC0) == 0x80) cut--;
                hn_node_set_value(n, old, cut);
            }
        }
        return rt_render(rt);
    }
    /* 可打印字符 → 追加到焦点 input 的 value */
    if (utf8 && *utf8 && rt->focus) {
        char old_val[4096] = { 0 };
        const char *old = hn_node_attr(rt->focus, "value");
        if (old) snprintf(old_val, sizeof(old_val), "%s", old);
        size_t ol = strlen(old_val);
        size_t al = strlen(utf8);
        if (ol + al < sizeof(old_val)) {
            memcpy(old_val + ol, utf8, al);
            old_val[ol + al] = 0;
            hn_node_set_value(rt->focus, old_val, ol + al);
        }
        return rt_render(rt);
    }
    return 0;
}

char *hn_rt_dom(hn_rt *rt, int max_depth) {
    if (!rt) return NULL;
    hn_doc *doc = hn_context_doc(rt->ctx);
    hn_node *root = hn_doc_root(doc);
    if (!root) return NULL;
    size_t cap = 8192, n = 0;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    /* 深度优先缩进树 */
    typedef struct { hn_node *n; int d; } st_t;
    st_t stack[256];
    int sp = 0;
    stack[sp].n = root; stack[sp].d = 0; sp++;
    while (sp > 0) {
        st_t cur = stack[--sp];
        if (hn_node_tag(cur.n)) {
            const char *tag = hn_node_tag(cur.n);
            const char *id = hn_node_attr(cur.n, "id");
            const char *cls = hn_node_attr(cur.n, "class");
            if (cur.d <= max_depth) {
                while (n + 256 >= cap) { cap *= 2; out = (char *)realloc(out, cap); if (!out) return NULL; }
                n += (size_t)snprintf(out + n, cap - n, "%*s<%s%s%s%s>\n",
                                      cur.d * 2, "", tag ? tag : "?",
                                      id ? " id=" : "", id ? id : "",
                                      cls ? " class=" : "");
                if (cls) {
                    /* class 值可能含空格, 追加 */
                    size_t l = strlen(out);
                    n = l + (size_t)snprintf(out + l, cap - l, "\"%s\"", cls);
                }
            }
            for (hn_node *c = hn_node_first_child(cur.n); c; c = hn_node_next_sibling(c))
                if (sp < 256) { stack[sp].n = c; stack[sp].d = cur.d + 1; sp++; }
        }
    }
    out[n] = 0;
    return out;
}

char *hn_rt_text(hn_rt *rt, const char *element_id) {
    if (!rt || !element_id) return NULL;
    hn_doc *doc = hn_context_doc(rt->ctx);
    hn_node *n = hn_doc_find_by_id(doc, element_id);
    if (!n) return NULL;
    char *out = (char *)malloc(4096);
    if (!out) return NULL;
    size_t tn = hn_node_text_content(n, out, 4096);
    out[tn < 4096 ? tn : 4095] = 0;
    return out;
}

hn_doc *hn_rt_doc(hn_rt *rt) { return rt ? hn_context_doc(rt->ctx) : NULL; }

/* 文档清单访问器: 平台壳在开窗时读一次(透明/无边框/层级/跨空间/穿透),
   不重新解析 —— 与 hn_rt_open 内部同一份 hn_doc_manifest 语义。 */
void hn_rt_manifest(const hn_rt *rt, hn_manifest *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    hn_doc_manifest(rt ? hn_context_doc(rt->ctx) : NULL, out);
}

hn_context *hn_rt_context(hn_rt *rt) { return rt ? rt->ctx : NULL; }

int hn_rt_cmd_count(hn_rt *rt) {
    if (!rt) return 0;
    const hn_display_list *dl = hn_context_display_list(rt->ctx);
    return dl ? dl->count : 0;
}

int hn_rt_synthetic(hn_rt *rt, const char *kind, const char *target) {
    if (!rt || !kind) return 0;
    hn_doc *doc = hn_context_doc(rt->ctx);
    hn_node *n = target ? hn_doc_find_by_id(doc, target) : hn_doc_root(doc);
    if (!n) return 0;
    if (!strcmp(kind, "click")) {
        hn_node *carrier = hx_carrier(n);
        if (carrier && hx_perform(rt, carrier)) { rt_render(rt); return 1; }
    }
    return 0;
}

int hn_rt_shot(hn_rt *rt, const char *path) {
    if (!rt || !path) return 0;
    rt_render(rt);
    const hn_display_list *dl = hn_context_display_list(rt->ctx);
    if (!dl) return 0;
    unsigned char *rgba = hnsoft_render(dl, rt->w, rt->h, rt->transparent ? 0x00000000u : 0x0B0E13FFu);
    if (!rgba) return 0;
    size_t pn = 0;
    unsigned char *png = hnsoft_encode_png(rgba, rt->w, rt->h, &pn);
    free(rgba);
    if (!png) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) { free(png); return 0; }
    fwrite(png, 1, pn, f);
    fclose(f);
    free(png);
    return 1;
}

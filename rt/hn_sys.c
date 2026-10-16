/* hn_sys.c — shared local sys:// routes and platform capabilities.
 * Hosts supply their own store namespace through callbacks; no engine/GUI host
 * dependency is needed by this translation unit. */
#define _POSIX_C_SOURCE 200809L
#include "hn_sys.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <sys/statvfs.h>
#include <time.h>
#include <dlfcn.h>
#ifndef __APPLE__
#include <glob.h>
#endif

static int qparam(const char *q, const char *key, char *out, size_t cap);
static void esc_html(const char *s, char *out, size_t cap);
static double sys_cpu_load(void);
static long sys_mem_used_mb(void);
static long sys_mem_total_mb(void);
static long sys_disk_free_gb(void);
static double sys_uptime_s(void);
static void sys_host(char *, size_t);
static int sys_battery_pct(void);

/* Match the complete route, leaving query handling to qparam. */
static int route_is(const char *route, const char *name) {
    size_t n = strlen(name);
    return !strncmp(route, name, n) && (!route[n] || route[n] == '?');
}

char *hn_sys_request(const hn_sys_host *host, const char *url, const char *form_body) {
    if (!url) return NULL;
    if (strncmp(url, "sys://", 6)) {
        fprintf(stderr, "[hx] fetch 不支持: \"%s\" — C99 引擎零网络, 仅 "
                "sys:// 本地桥; 外部资源走 sys://open 交系统打开\n", url);
        fflush(stderr);
        return NULL;
    }
    char *frag = hn_sys_fragment(host, url, form_body);
    if (!frag) {
        fprintf(stderr, "[hx] 未知 sys:// 路由: %s\n", url);
        fflush(stderr);
    }
    return frag;
}

char *hn_sys_fragment(const hn_sys_host *host, const char *url, const char *form_body) {
    if (!url || strncmp(url, "sys://", 6)) return NULL;
    const char *route = url + 6;
    const char *q = strchr(route, '?');
    char key[256] = { 0 }, val[1024] = { 0 };
    char *out = (char *)malloc(4096);
    if (!out) return NULL;
    if (route_is(route, "store/get")) {
        if (qparam(q, "key", key, sizeof(key)) || qparam(form_body, "key", key, sizeof(key))) {
            char v[1024] = { 0 };
            int got = (host && host->store_get ? host->store_get(host->ctx, key, v, sizeof(v)) : 0);
            char ev[2200];
            esc_html(got ? v : "", ev, sizeof(ev));
            snprintf(out, 4096, "<span id=\"store-value\">%s</span>", ev);
        } else snprintf(out, 4096, "<span>missing key</span>");
    } else if (route_is(route, "store/set")) {
        int hk = qparam(q, "key", key, sizeof(key)) || qparam(form_body, "key", key, sizeof(key));
        int hv = qparam(q, "value", val, sizeof(val)) || qparam(form_body, "value", val, sizeof(val));
        if (hk && hv) {
            if (host && host->store_set) host->store_set(host->ctx, key, val);
            char ev[2200];
            esc_html(val, ev, sizeof(ev));
            snprintf(out, 4096, "<span id=\"store-value\">%s</span>", ev);
        } else snprintf(out, 4096, "<span>missing key/value</span>");
    } else if (route_is(route, "store/count")) {
        snprintf(out, 4096, "<span>1</span>");
    } else if (route_is(route, "cpu")) {
        double load = sys_cpu_load();
        snprintf(out, 4096,
                 "<div style=\"font-size:20;font-weight:700;color:#7fdbca\">%.0f%%</div>"
                 "<div style=\"font-size:11;color:#6b7386\">CPU load average</div>", load < 0 ? 0 : load);
    } else if (route_is(route, "memory") || route_is(route, "mem")) {
        long used = sys_mem_used_mb(), total = sys_mem_total_mb();
        int pct = total > 0 ? (int)(used * 100 / total) : 0;
        snprintf(out, 4096,
                 "<div style=\"font-size:20;font-weight:700;color:#7fdbca\">%ld MB</div>"
                 "<div style=\"font-size:11;color:#6b7386\">%ld MB total (%d%%)</div>",
                 used, total, pct);
    } else if (route_is(route, "disk")) {
        snprintf(out, 4096,
                 "<div style=\"font-size:20;font-weight:700;color:#7fdbca\">%ld GB</div>"
                 "<div style=\"font-size:11;color:#6b7386\">disk free</div>", sys_disk_free_gb());
    } else if (route_is(route, "uptime")) {
        snprintf(out, 4096,
                 "<div style=\"font-size:20;font-weight:700;color:#7fdbca\">%.0f s</div>"
                 "<div style=\"font-size:11;color:#6b7386\">uptime</div>", sys_uptime_s());
    } else if (route_is(route, "host")) {
        char hostname[256], escaped[1600];
        sys_host(hostname, sizeof(hostname));
        esc_html(hostname, escaped, sizeof(escaped));
        snprintf(out, 4096,
                 "<div style=\"font-size:16;font-weight:600;color:#e8eaf0\">%s</div>", escaped);
    } else if (route_is(route, "battery") || route_is(route, "power")) {
        int pct = sys_battery_pct();
        if (pct < 0) snprintf(out, 4096, "<span>battery unavailable</span>");
        else snprintf(out, 4096, "<div>%d%%</div><div>battery</div>", pct);
    } else if (route_is(route, "info")) {
        /* Compose the same individual routes: additions cannot drift between
           the aggregate card and the route consumed by each host. */
        static const char *routes[] = { "host", "cpu", "memory", "disk", "battery", "uptime" };
        size_t used = 0;
        out[0] = 0;
        for (size_t i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
            char item_url[32];
            snprintf(item_url, sizeof(item_url), "sys://%s", routes[i]);
            char *item = hn_sys_fragment(host, item_url, NULL);
            if (!item) { free(out); return NULL; }
            size_t n = strlen(item);
            if (used + n < 4096) { memcpy(out + used, item, n + 1); used += n; }
            free(item);
        }
    } else if (route_is(route, "clipboard/get")) {
        char text[4096] = { 0 };
        if (host && host->clip_get) host->clip_get(host->ctx, text, sizeof(text));
        else hn_sys_clip_get(text, sizeof(text));
        char ev[4200];
        esc_html(text, ev, sizeof(ev));
        snprintf(out, 4096, "<div id=\"clip\">%s</div>", ev);
    } else if (route_is(route, "clipboard/set")) {
        char v[4096] = { 0 };
        int ok = (qparam(q, "value", v, sizeof(v)) || qparam(form_body, "value", v, sizeof(v))) &&
                 (host && host->clip_set ? host->clip_set(host->ctx, v) : hn_sys_clip_set(v));
        snprintf(out, 4096, ok ? "<span>copied</span>" : "<span>clip unavailable</span>");
    } else if (route_is(route, "open")) {
        char target[2048] = { 0 };
        if (qparam(q, "url", target, sizeof(target)) || qparam(form_body, "url", target, sizeof(target))) {
            if (host && host->open_url) host->open_url(host->ctx, target);
            else hn_sys_open_url(target);
        }
        snprintf(out, 4096, "<span></span>");
    } else if (route_is(route, "notify")) {
        char text[2048] = { 0 };
        qparam(q, "text", text, sizeof(text));
        if (host && host->notify) host->notify(host->ctx, text);
        else fprintf(stderr, "[notify] %s\n", text);
        snprintf(out, 4096, "<span></span>");
    } else {
        free(out);
        return NULL;
    }
    return out;
}

static int qparam(const char *q, const char *key, char *out, size_t cap) {
    if (!q || !key || !out || cap == 0) return 0;
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
    if (!out || cap == 0) return;
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
 * NSWorkspace(dlopen); Linux 统计走 /proc，剪贴板/open 保留不可用回退。 */

#ifdef __APPLE__
#include <sys/types.h>
#include <sys/statvfs.h>

/* sysctlbyname 不用 sys/sysctl.h(header 在 _POSIX_C_SOURCE 下拉入 BSD 类型 u_int
   而 -std=c99 + _POSIX_C_SOURCE 200809L 不定义它 → 编译失败)。 */
extern int sysctlbyname(const char *, void *, size_t *, const void *, size_t);
#endif
#include <time.h>

/* 剪贴板/openURL 的 ObjC 调用: 每次经 dlsym(不依赖链接了哪个 hnp_*.c)。
   Apple 平台 AppKit 由 hn_objc_bind 在消费点按需装载(见其注释 —— 宿主
   侧 hnp_init 的装载覆盖不到 daemon/--shot 链路); 非 Apple 直接返回 0。

   ⚠️ 2026-10 修复(本轮): 这里原写 `dlsym((void *)0, "objc_msgSend")` ——
   **RTLD_DEFAULT 在 macOS 是 (void *)-2 而非 NULL**(dlfcn.h), 传 0 得到
   的是"找不到符号"的 NULL, 于是三个函数全部走 `if (!gc || !sl || !send)`
   提前返回: ⌘C/⌘V 与 sys://clipboard 在 macOS 上**从来没工作过**, 只是
   静默失败(界面显示 "clip unavailable")。同一 bug 在 hn_sys_open_url 上
   (链接点击/notify 外链同样没打开过)。已全部改为 RTLD_DEFAULT。

   有界 autorelease 池: 本文件在池外调用 stringWithUTF8String: 等
   convenient 构造器会泄漏(与 hnp_macos.c 事故②同一根因)。这里按
   hnp_macos.c 的 pool_push/pool_pop 同款手法套一层 —— 对象只在调用
   期间存活, drain 后立即回收。 */
#ifdef __APPLE__
#include <dlfcn.h>
/* -std=c99 + _POSIX_C_SOURCE 下 Darwin 的 dlfcn.h 不暴露 RTLD_DEFAULT
   (__DARWIN_C_LEVEL 停在 POSIX 档, RTLD_DEFAULT 在 FULL 档里) ——
   这正是原作者写 (void *)0 的原因, 而 macOS 上 dlsym(0, ...) **不搜全局
   (拿到 NULL)**, 于是整块静默失效。这里按 Darwin 真值补定义
   (dyld 源码/机器码实测 = (void *)-2); 其余平台的 RTLD_DEFAULT 由
   dlfcn.h 正常给出(glibc = (void *)0, 语义是全局符号表)。 */
#ifndef RTLD_DEFAULT
#if defined(__APPLE__)
#define RTLD_DEFAULT ((void *)-2)
#else
#define RTLD_DEFAULT ((void *)0)
#endif
#endif
typedef void *(*hn_dlsym_msgSend_t)(void *, void *, ...);
typedef struct { void *send, *gc, *sl; } hn_objc_rt;
static int hn_objc_bind(hn_objc_rt *rt) {
    /* AppKit 不一定已装载进本进程 —— 宿主各有差异: 真窗壳在 hnp_macos.c
       的 hnp_init 里 dlopen; daemon 不链任何平台层(app 侧 hnp_init 的调用
       点 app/hn_app.c:429 与它无关); hn_app 的 --shot 分支在 hnp_init 之前
       就 return(2026-10-15 复审实测: daemon 与 --shot 两条链路 clipboard
       路由都换入降级占位 "clip unavailable")。在消费点自行装载, sys://
       clipboard/open 从此不依赖"宿主记得装 AppKit"。重复 dlopen 已装载的
       framework 是引用计数 no-op, 对真窗壳零代价; 首次失败(异常系统)不
       重试, 路由落回既有占位, 其余路径零影响。 */
    static int appkit_tried = 0;
    if (!appkit_tried) {
        dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_NOW);
        appkit_tried = 1;
    }
    rt->gc = dlsym(RTLD_DEFAULT, "objc_getClass");
    rt->sl = dlsym(RTLD_DEFAULT, "sel_registerName");
    rt->send = dlsym(RTLD_DEFAULT, "objc_msgSend");
    return rt->gc && rt->sl && rt->send;
}
/* 有界 autorelease 池(与 hnp_macos.c 的 pool_push/pool_pop 同款):
   NSObject 的 alloc/init 与 drain 都经 objc_msgSend 调用。 */
static void *hn_pool_enter(const hn_objc_rt *rt) {
    void *(*send)(void *, void *, ...) = (void *(*)(void *, void *, ...))rt->send;
    void *(*gc)(const char *) = (void *(*)(const char *))rt->gc;
    void *(*sel)(const char *) = (void *(*)(const char *))rt->sl;
    void *cls = gc("NSAutoreleasePool");
    if (!cls) return NULL;
    void *o = ((void *(*)(void *, void *))send)(cls, sel("alloc"));
    if (!o) return NULL;
    return ((void *(*)(void *, void *))send)(o, sel("init"));
}
static void hn_pool_exit(const hn_objc_rt *rt, void *pool) {
    if (!pool) return;
    void *(*send)(void *, void *, ...) = (void *(*)(void *, void *, ...))rt->send;
    void *(*sel)(const char *) = (void *(*)(const char *))rt->sl;
    ((void (*)(void *, void *))send)(pool, sel("drain"));
}
#endif

/* -1 means no battery or unavailable data. */
static int sys_battery_pct(void) {
#ifdef __APPLE__
    static void *io;
    static int tried;
    if (!tried) {
        io = dlopen("/System/Library/Frameworks/IOKit.framework/IOKit", RTLD_NOW);
        tried = 1;
    }
    if (!io) return -1;
    void *(*info_copy)(void) = (void *(*)(void))dlsym(io, "IOPSCopyPowerSourcesInfo");
    void *(*list_copy)(const void *) = (void *(*)(const void *))dlsym(io, "IOPSCopyPowerSourcesList");
    const void *(*description)(const void *, const void *) =
        (const void *(*)(const void *, const void *))dlsym(io, "IOPSGetPowerSourceDescription");
    long (*count)(const void *) = (long (*)(const void *))dlsym(io, "CFArrayGetCount");
    const void *(*at)(const void *, long) = (const void *(*)(const void *, long))dlsym(io, "CFArrayGetValueAtIndex");
    const void *(*get)(const void *, const void *) =
        (const void *(*)(const void *, const void *))dlsym(io, "CFDictionaryGetValue");
    void *(*str)(void *, const char *, unsigned int) =
        (void *(*)(void *, const char *, unsigned int))dlsym(io, "CFStringCreateWithCString");
    unsigned char (*number)(const void *, int, void *) =
        (unsigned char (*)(const void *, int, void *))dlsym(io, "CFNumberGetValue");
    void (*release)(const void *) = (void (*)(const void *))dlsym(io, "CFRelease");
    if (!info_copy || !list_copy || !description || !count || !at || !get || !str || !number || !release)
        return -1;
    void *info = info_copy(), *list = info ? list_copy(info) : NULL;
    void *current_key = str(NULL, "Current Capacity", 0x08000100);
    void *max_key = str(NULL, "Max Capacity", 0x08000100);
    int pct = -1;
    if (list && current_key && max_key) {
        for (long i = 0; i < count(list); i++) {
            const void *desc = description(info, at(list, i));
            const void *cur = desc ? get(desc, current_key) : NULL;
            const void *max = desc ? get(desc, max_key) : NULL;
            int c = 0, m = 0;
            /* kCFNumberIntType = 9 */
            if (cur && max && number(cur, 9, &c) && number(max, 9, &m) && m > 0 && c >= 0) {
                pct = (int)((double)c * 100 / m);
                if (pct > 100) pct = 100;
                break;
            }
        }
    }
    if (current_key) release(current_key);
    if (max_key) release(max_key);
    if (list) release(list);
    if (info) release(info);
    return pct;
#else
    glob_t paths = {0};
    if (glob("/sys/class/power_supply/*/capacity", 0, NULL, &paths) != 0) {
        globfree(&paths);
        return -1;
    }
    int pct = -1;
    for (size_t i = 0; i < paths.gl_pathc; i++) {
        FILE *f = fopen(paths.gl_pathv[i], "r");
        int value;
        if (!f) continue;
        int ok = fscanf(f, "%d", &value) == 1;
        fclose(f);
        if (ok && value >= 0 && value <= 100) { pct = value; break; }
    }
    globfree(&paths);
    return pct;
#endif
}

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
int hn_sys_clip_set(const char *text) {
    hn_objc_rt rt;
    if (!hn_objc_bind(&rt)) return 0;
    void *pool = hn_pool_enter(&rt);
    void *(*send)(void *, void *, ...) = (void *(*)(void *, void *, ...))rt.send;
    void *(*gc)(const char *) = (void *(*)(const char *))rt.gc;
    void *(*sl)(const char *) = (void *(*)(const char *))rt.sl;
    int ok = 0;
    void *pb = ((void *(*)(void *, void *))send)(gc("NSPasteboard"), sl("generalPasteboard"));
    if (pb) {
        ((void (*)(void *, void *))send)(pb, sl("clearContents"));
        void *nscls = gc("NSString");
        void *str = ((void *(*)(void *, void *, const char *))send)(
            nscls, sl("stringWithUTF8String:"), text);
        void *type = ((void *(*)(void *, void *, const char *))send)(
            nscls, sl("stringWithUTF8String:"), "public.utf8-plain-text");
        if (str && type)
            ok = ((void *(*)(void *, void *, void *, void *))send)(
                pb, sl("setString:forType:"), str, type) ? 1 : 0;
    }
    hn_pool_exit(&rt, pool);
    return ok;
}
int hn_sys_clip_get(char *out, size_t cap) {
    hn_objc_rt rt;
    if (!hn_objc_bind(&rt) || !out || cap == 0) return 0;
    out[0] = 0;
    void *pool = hn_pool_enter(&rt);
    void *(*send)(void *, void *, ...) = (void *(*)(void *, void *, ...))rt.send;
    void *(*gc)(const char *) = (void *(*)(const char *))rt.gc;
    void *(*sl)(const char *) = (void *(*)(const char *))rt.sl;
    int ok = 0;
    void *pb = ((void *(*)(void *, void *))send)(gc("NSPasteboard"), sl("generalPasteboard"));
    if (pb) {
        void *type = ((void *(*)(void *, void *, const char *))send)(
            gc("NSString"), sl("stringWithUTF8String:"), "public.utf8-plain-text");
        void *str = type ? ((void *(*)(void *, void *, void *))send)(
            pb, sl("stringForType:"), type) : NULL;
        const char *c = str ? ((const char *(*)(void *, void *))send)(
            str, sl("UTF8String")) : NULL;
        if (c) { snprintf(out, cap, "%s", c); ok = 1; }
    }
    hn_pool_exit(&rt, pool);
    return ok;
}
int hn_sys_open_url(const char *url) {
    hn_objc_rt rt;
    if (!hn_objc_bind(&rt)) return 0;
    void *pool = hn_pool_enter(&rt);
    void *(*send)(void *, void *, ...) = (void *(*)(void *, void *, ...))rt.send;
    void *(*gc)(const char *) = (void *(*)(const char *))rt.gc;
    void *(*sl)(const char *) = (void *(*)(const char *))rt.sl;
    int ok = 0;
    void *ws = ((void *(*)(void *, void *))send)(gc("NSWorkspace"), sl("sharedWorkspace"));
    if (ws) {
        void *str = ((void *(*)(void *, void *, const char *))send)(
            gc("NSString"), sl("stringWithUTF8String:"), url);
        if (str) {
            ((void (*)(void *, void *, void *))send)(ws, sl("openURL:"), str);
            ok = 1;
        }
    }
    hn_pool_exit(&rt, pool);
    return ok;
}
#else
int hn_sys_clip_set(const char *text) { (void)text; return 0; }
int hn_sys_clip_get(char *out, size_t cap) { (void)out; (void)cap; return 0; }
int hn_sys_open_url(const char *url) { (void)url; return 0; }
#endif


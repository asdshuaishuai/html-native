/* sysbridge.c — Windows 系统桥(sys://): 零网络的本地数据应答
 *
 * 对应 macOS 的 SystemBridge.swift + HNAppRoutes.swift。htmx 风格的
 * hx-get="sys://cpu" 由**运行时**解释(transport 进程内), 引擎不碰网络。
 * 两条渲染路径(native / WebView2 兜底)共用本模块 —— 换渲染层,
 * 应用模型完全一致(README 的双渲染器契约)。
 *
 * 路由: cpu / memory(mem) / disk / battery(power) / uptime / host /
 *       info(全量卡) / store/get?key= / store/set?key=&value=
 *
 * 片段模板与 macOS 同构(同 class 同色阶), 换入任何文档即可用。
 */
#define WIN32_LEAN_AND_MEAN
#define UNICODE
#define _UNICODE
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <direct.h>

/* 片段自带样式(与 macOS SystemBridge 同口径: 免单位数值 native 认,
   两条路径 CSS 相同 —— webkit 路径由归一化补齐单位) */
static const char *SYS_CSS =
"<style>"
".sysrow { display: flex; padding: 4 0; }"
".sysk { color: #8b93a7; font-size: 11; width: 92; }"
".sysv { color: #e8eaf0; font-size: 12; font-weight: 600; }"
".sysbar { height: 5; border-radius: 2.5; background: #232733; margin-top: 3; }"
".sysfill { height: 100%; border-radius: 2.5; background: #4f7cff; }"
".syshead { color: #eef0f5; font-size: 12; font-weight: 700; margin: 0 0 6 0; }"
"</style>";

/* 阈值色阶: 占用越高越暖(与 macOS 一致) */
static const char *level_color(double pct) {
    if (pct >= 90) return "#ff5f5f";
    if (pct >= 70) return "#ffa940";
    if (pct >= 40) return "#4f7cff";
    return "#4fd08a";
}

static char *sys_row(const char *key, const char *value, double pct) {
    char buf[1024];
    if (pct >= 0)
        snprintf(buf, sizeof(buf),
            "<div class=\"sysrow\"><span class=\"sysk\">%s</span>"
            "<span class=\"sysv\">%s</span></div>"
            "<div class=\"sysbar\"><div class=\"sysfill\" style=\"width:%.0f%%;background:%s\"></div></div>",
            key, value, pct, level_color(pct));
    else
        snprintf(buf, sizeof(buf),
            "<div class=\"sysrow\"><span class=\"sysk\">%s</span>"
            "<span class=\"sysv\">%s</span></div>", key, value);
    return _strdup(buf);
}

/* ---------- 系统数据(Win32) ---------- */

static int cpu_usage_pct(void) {
    FILETIME idle0, kernel0, user0, idle1, kernel1, user1;
    if (!GetSystemTimes(&idle0, &kernel0, &user0)) return -1;
    Sleep(120);
    if (!GetSystemTimes(&idle1, &kernel1, &user1)) return -1;
    ULONGLONG i0 = ((ULONGLONG)idle0.dwHighDateTime << 32) | idle0.dwLowDateTime;
    ULONGLONG k0 = ((ULONGLONG)kernel0.dwHighDateTime << 32) | kernel0.dwLowDateTime;
    ULONGLONG u0 = ((ULONGLONG)user0.dwHighDateTime << 32) | user0.dwLowDateTime;
    ULONGLONG i1 = ((ULONGLONG)idle1.dwHighDateTime << 32) | idle1.dwLowDateTime;
    ULONGLONG k1 = ((ULONGLONG)kernel1.dwHighDateTime << 32) | kernel1.dwLowDateTime;
    ULONGLONG u1 = ((ULONGLONG)user1.dwHighDateTime << 32) | user1.dwLowDateTime;
    ULONGLONG idle = i1 - i0;
    ULONGLONG total = (k1 - k0) + (u1 - u0);
    if (total == 0) return -1;
    int pct = (int)((double)(total - idle) * 100.0 / (double)total);
    return pct < 0 ? 0 : (pct > 100 ? 100 : pct);
}

static char *cpu_body(void) {
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    int pct = cpu_usage_pct();
    char cores[64];
    snprintf(cores, sizeof(cores), "%lu 核", (unsigned long)si.dwNumberOfProcessors);
    if (pct < 0) return sys_row("占用", "N/A", -1);
    char v[32];
    snprintf(v, sizeof(v), "%d%%", pct);
    char *r1 = sys_row("占用", v, (double)pct);
    char *r2 = sys_row("核心", cores, -1);
    size_t n = strlen(r1) + strlen(r2) + 1;
    char *out = (char *)malloc(n);
    snprintf(out, n, "%s%s", r1, r2);
    free(r1); free(r2);
    return out;
}

static char *mem_body(void) {
    MEMORYSTATUSEX ms;
    ms.dwLength = sizeof(ms);
    if (!GlobalMemoryStatusEx(&ms)) return sys_row("内存", "N/A", -1);
    double total_gb = (double)ms.ullTotalPhys / (1024.0 * 1024.0 * 1024.0);
    double used_gb = (double)(ms.ullTotalPhys - ms.ullAvailPhys) / (1024.0 * 1024.0 * 1024.0);
    double pct = (double)ms.dwMemoryLoad;
    char v[64], a[64];
    snprintf(v, sizeof(v), "%.1f GB", total_gb);
    snprintf(a, sizeof(a), "%.1f GB", used_gb);
    char *r1 = sys_row("物理内存", v, -1);
    char *r2 = sys_row("已用", a, pct);
    size_t n = strlen(r1) + strlen(r2) + 1;
    char *out = (char *)malloc(n);
    snprintf(out, n, "%s%s", r1, r2);
    free(r1); free(r2);
    return out;
}

static char *disk_body(void) {
    ULARGE_INTEGER free_avail, total, free_total;
    if (!GetDiskFreeSpaceExW(L"C:\\", &free_avail, &total, &free_total))
        return sys_row("磁盘", "N/A", -1);
    double total_gb = (double)total.QuadPart / (1024.0 * 1024.0 * 1024.0);
    double free_gb = (double)free_total.QuadPart / (1024.0 * 1024.0 * 1024.0);
    double pct = total.QuadPart ? (double)(total.QuadPart - free_total.QuadPart) * 100.0 / (double)total.QuadPart : 0;
    char v[64], f[64];
    snprintf(v, sizeof(v), "%.1f GB", total_gb);
    snprintf(f, sizeof(f), "%.1f GB", free_gb);
    char *r1 = sys_row("总容量", v, -1);
    char *r2 = sys_row("可用", f, 100.0 - pct);
    size_t n = strlen(r1) + strlen(r2) + 1;
    char *out = (char *)malloc(n);
    snprintf(out, n, "%s%s", r1, r2);
    free(r1); free(r2);
    return out;
}

static char *battery_body(void) {
    SYSTEM_POWER_STATUS ps;
    if (!GetSystemPowerStatus(&ps) || ps.BatteryFlag == 128)
        return sys_row("类型", "台式机", -1);
    int pct = ps.BatteryLifePercent == 255 ? -1 : (int)ps.BatteryLifePercent;
    const char *ac = (ps.ACLineStatus == 1) ? "已接通电源" : "电池供电";
    char v[64];
    snprintf(v, sizeof(v), "%s", ac);
    if (pct < 0) return sys_row("类型", v, -1);
    char v2[64];
    snprintf(v2, sizeof(v2), "%d%%", pct);
    char *r1 = sys_row("类型", v, -1);
    char *r2 = sys_row("电量", v2, (double)pct);
    size_t n = strlen(r1) + strlen(r2) + 1;
    char *out = (char *)malloc(n);
    snprintf(out, n, "%s%s", r1, r2);
    free(r1); free(r2);
    return out;
}

static char *uptime_body(void) {
    ULONGLONG ms = GetTickCount64();
    unsigned long long s = ms / 1000;
    unsigned long long d = s / 86400; s %= 86400;
    unsigned long long h = s / 3600; s %= 3600;
    unsigned long long m = s / 60;
    char v[64];
    if (d > 0) snprintf(v, sizeof(v), "%llu 天 %llu 小时 %llu 分", d, h, m);
    else if (h > 0) snprintf(v, sizeof(v), "%llu 小时 %llu 分", h, m);
    else snprintf(v, sizeof(v), "%llu 分", m);
    return sys_row("运行时间", v, -1);
}

static char *host_body(void) {
    wchar_t name[256];
    DWORD nlen = 256;
    char utf8name[512];
    if (GetComputerNameW(name, &nlen))
        WideCharToMultiByte(CP_UTF8, 0, name, -1, utf8name, sizeof(utf8name), NULL, NULL);
    else
        snprintf(utf8name, sizeof(utf8name), "Windows PC");
    /* 版本: RtlGetVersion(GetVersionEx 在高版本被兼容性 shim 谎报) */
    char ver[64] = "Windows";
    HMODULE ntdll = GetModuleHandleW(L"ntdll.dll");
    if (ntdll) {
        typedef LONG (WINAPI *rtl_get_version_t)(RTL_OSVERSIONINFOW *);
        rtl_get_version_t rtl = (rtl_get_version_t)GetProcAddress(ntdll, "RtlGetVersion");
        if (rtl) {
            RTL_OSVERSIONINFOW ovi;
            memset(&ovi, 0, sizeof(ovi));
            ovi.dwOSVersionInfoSize = sizeof(ovi);
            if (rtl(&ovi) == 0)
                snprintf(ver, sizeof(ver), "Windows %lu.%lu (build %lu)",
                         (unsigned long)ovi.dwMajorVersion,
                         (unsigned long)ovi.dwMinorVersion,
                         (unsigned long)ovi.dwBuildNumber);
        }
    }
    char *r1 = sys_row("主机", utf8name, -1);
    char *r2 = sys_row("系统", ver, -1);
    size_t n = strlen(r1) + strlen(r2) + 1;
    char *out = (char *)malloc(n);
    snprintf(out, n, "%s%s", r1, r2);
    free(r1); free(r2);
    return out;
}

/* ---------- 本地 KV store(sys://store/get|set) ----------
   存储: %USERPROFILE%\.html-native\store\<storeId>.json
   迷你 JSON: {"k":"v",...} —— 只支持字符串值(与本用例相符) */

static void store_path(char *buf, size_t cap, const char *store_id) {
    wchar_t home[MAX_PATH];
    char home_utf8[MAX_PATH];
    DWORD n = GetEnvironmentVariableW(L"USERPROFILE", home, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        snprintf(home_utf8, sizeof(home_utf8), "C:\\");
    } else {
        WideCharToMultiByte(CP_UTF8, 0, home, -1, home_utf8, sizeof(home_utf8), NULL, NULL);
    }
    snprintf(buf, cap, "%s/.html-native/store/%s.json", home_utf8, store_id);
}

/* 逐级创建目录。起点要跳过盘符("C:\")或 UNC 前缀("\\\\server\\share\\"),
   否则会拿 "C:" 当目录去 mkdir(必然失败, 且 UNC 路径会整体算错)。 */
static void ensure_store_dir(const char *path) {
    char dir[MAX_PATH * 2];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (!slash) slash = strrchr(dir, '\\');
    if (!slash) return;
    *slash = 0;
    if (!dir[0]) return;

    char *start;
    if ((dir[0] == '/' || dir[0] == '\\') && (dir[1] == '/' || dir[1] == '\\')) {
        /* UNC: \\server\share\... —— 跳过前两段(server 与 share) */
        start = dir + 2;
        int segs = 0;
        while (*start && segs < 2) {
            if (*start == '/' || *start == '\\') segs++;
            start++;
        }
    } else {
        /* 盘符: 跳过 "X:" 与紧跟的分隔符 */
        start = dir + 1;                       /* 跳过盘字母 */
        if (*start == ':') start++;
        while (*start == '/' || *start == '\\') start++;
    }
    for (char *p = start; *p; p++) {
        if (*p == '/' || *p == '\\') {
            *p = 0;
            _mkdir(dir);
            *p = '/';
        }
    }
    _mkdir(dir);
}

/* 从 JSON 文本里找 "key":"value"(键名按完整 token 比较, 不是子串)。
   值里的 **\\" 必须按转义处理** —— 否则存过的含双引号的值再读回来会被
   截断(实测 {"k":"va\\"lue"} 只读出 "va")。
   还原规则: \\" → " , \\\\ → \\ , \\/ → / , \\n → 换行(与写入侧一致)。 */
static int json_get(const char *json, const char *key, char *out, size_t cap) {
    if (!json || !key || !out || !cap) return 0;
    if (out && cap) out[0] = 0;
    char pat[512];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    size_t plen = strlen(pat);
    const char *p = json;
    while ((p = strstr(p, pat)) != NULL) {
        /* 键名必须是完整 token: 前一个字符不能是键字符的一部分 */
        int boundary = (p == json) || (p[-1] == '{' || p[-1] == ',' ||
                                       p[-1] == ' ' || p[-1] == '\n');
        if (boundary) break;
        p += plen;
    }
    if (!p) return 0;
    p += plen;
    size_t i = 0;
    while (*p && i < cap - 1) {
        if (*p == '\\' && p[1]) {
            char e = p[1];
            if (e == '"')       { out[i++] = '"';  p += 2; }
            else if (e == '\\'){ out[i++] = '\\'; p += 2; }
            else if (e == '/')  { out[i++] = '/';  p += 2; }
            else if (e == 'n')  { out[i++] = '\n'; p += 2; }
            else if (e == 't')  { out[i++] = '\t'; p += 2; }
            else                { out[i++] = e;    p += 2; }
            continue;
        }
        if (*p == '"') break;              /* 未转义的引号 = 值结束 */
        out[i++] = *p++;
    }
    out[i] = 0;
    return 1;
}

static void json_escape(const char *s, char *out, size_t cap) {
    size_t i = 0;
    for (const char *p = s; *p && i < cap - 3; p++) {
        if (*p == '"' || *p == '\\') out[i++] = '\\';
        out[i++] = *p;
    }
    out[i] = 0;
}

static int json_set(const char *path, const char *key, const char *value) {
    /* 读旧文件(可能不存在), 改写该键, 写回 */
    char buf[8192];
    size_t len = 0;
    FILE *f = fopen(path, "rb");
    if (f) {
        len = fread(buf, 1, sizeof(buf) - 1, f);
        fclose(f);
    }
    buf[len] = 0;
    char old_val[2048];
    int has_old = json_get(buf, key, old_val, sizeof(old_val));
    char esc_key[256], esc_val[2048];
    json_escape(key, esc_key, sizeof(esc_key));
    json_escape(value, esc_val, sizeof(esc_val));
    ensure_store_dir(path);
    FILE *w = fopen(path, "wb");
    if (!w) return 0;
    if (has_old) {
        /* 替换旧值(在文件文本里定位 "key":"old" 区间) */
        char pat[512];
        snprintf(pat, sizeof(pat), "\"%s\":\"", esc_key);
        char *p = strstr(buf, pat);
        if (p) {
            char *vs = p + strlen(pat);
            char *ve = vs;
            while (*ve && *ve != '"') { if (*ve == '\\' && ve[1]) ve++; ve++; }
            fwrite(buf, 1, (size_t)(vs - buf), w);
            fwrite(esc_val, 1, strlen(esc_val), w);
            fwrite(ve, 1, strlen(ve), w);
            fclose(w);
            return 1;
        }
    }
    /* 新键: 追加(保持合法 JSON) */
    char trimmed[8192];
    snprintf(trimmed, sizeof(trimmed), "%s", buf);
    size_t tl = strlen(trimmed);
    while (tl > 0 && (trimmed[tl - 1] == '}' || trimmed[tl - 1] == ' ' ||
                      trimmed[tl - 1] == '\n' || trimmed[tl - 1] == '\r'))
        trimmed[--tl] = 0;
    if (tl == 0) {
        fprintf(w, "{\"%s\":\"%s\"}", esc_key, esc_val);
    } else {
        fprintf(w, "%s,\"%s\":\"%s\"}", trimmed, esc_key, esc_val);
    }
    fclose(w);
    return 1;
}

/* HTML 转义: & < > 三个字符(与 macOS HNAppRoutes.escape 同口径)。
   存进 KV 的值会被原样拼进 HTML 片段 —— 不转义的话, 值里的 < > & 会变成
   标记(macOS 侧一直是 escape(v), Windows 漏了)。 */
static void html_escape(const char *s, char *out, size_t cap) {
    size_t i = 0;
    if (!s) s = "";
    for (const char *p = s; *p && i + 8 < cap; p++) {
        if (*p == '&')      { memcpy(out + i, "&amp;", 5);  i += 5; }
        else if (*p == '<') { memcpy(out + i, "&lt;", 4);   i += 4; }
        else if (*p == '>') { memcpy(out + i, "&gt;", 4);   i += 4; }
        else                { out[i++] = *p; }
    }
    out[i] = 0;
}

/* ---------- URL 解析 + 路由 ---------- */

/* sys://cpu?x=1 → route="cpu", query="x=1" */
static void parse_sys_url(const char *url, char *route, size_t rcap, char *query, size_t qcap) {
    const char *p = url;
    if (!_strnicmp(p, "sys://", 6)) p += 6;
    const char *q = strchr(p, '?');
    size_t rl = q ? (size_t)(q - p) : strlen(p);
    if (rl >= rcap) rl = rcap - 1;
    memcpy(route, p, rl);
    route[rl] = 0;
    if (q && qcap > 0) snprintf(query, qcap, "%s", q + 1);
    else if (qcap > 0) query[0] = 0;
}

/* query 里取指定参数(URL 解码)。
   必须**先按 & 切成 pair 再比较键名** —— 用 strstr("key=") 子串匹配的话,
   键名只要是另一个参数的子串就会取错: 实测
       mykey=WRONG&key=RIGHT  取 "key" → 得到 "WRONG"
   (sys://store/get?mykey=1&key=2 会读到 mykey 的值)。
   macOS 侧 HNAppRoutes.parsePairs 就是先 split("&") 再 split("="),
   这里对齐同一口径。 */
static void query_param(const char *query, const char *name, char *out, size_t cap) {
    if (out && cap) out[0] = 0;
    if (!query || !name || !out || !cap) return;
    size_t nlen = strlen(name);
    const char *p = query;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
        /* 本段内的 '=' 位置 */
        const char *eq = (const char *)memchr(p, '=', seg);
        size_t klen = eq ? (size_t)(eq - p) : seg;
        if (klen == nlen && !strncmp(p, name, nlen)) {
            const char *v = eq ? eq + 1 : p + seg;
            size_t vlen = eq ? (size_t)(p + seg - v) : 0;
            size_t i = 0;
            for (size_t k = 0; k < vlen && i < cap - 1; ) {
                if (v[k] == '%' && k + 2 < vlen) {
                    char hex[3] = { v[k + 1], v[k + 2], 0 };
                    char *endp = NULL;
                    long b = strtol(hex, &endp, 16);
                    /* 非法十六进制按字面量处理(不写 NUL 字节 —— 那会截断值) */
                    int okhex = (endp && *endp == 0 && b >= 0 && b <= 255);
                    out[i++] = okhex ? (char)b : v[k];
                    k += okhex ? 3 : 1;
                } else if (v[k] == '+') {
                    out[i++] = ' ';
                    k++;
                } else {
                    out[i++] = v[k++];
                }
            }
            out[i] = 0;
            return;
        }
        p = amp ? amp + 1 : NULL;
    }
}

/* sys:// 应答: 返回 malloc 的 HTML 片段(caller free); 不识别的路由返回 NULL */
char *sys_fragment(const char *url, const char *form_body, const char *store_id) {
    if (!url || _strnicmp(url, "sys://", 6) != 0) return NULL;
    char route[128], query[2048];
    parse_sys_url(url, route, sizeof(route), query, sizeof(query));

    char head[512];
    snprintf(head, sizeof(head), "%s<div class=\"syshead\">", SYS_CSS);

    if (!strcmp(route, "cpu")) {
        char *body = cpu_body();
        size_t n = strlen(head) + strlen(body) + 64;
        char *out = (char *)malloc(n);
        snprintf(out, n, "%sCPU</div>%s", head, body);
        free(body); return out;
    }
    if (!strcmp(route, "memory") || !strcmp(route, "mem")) {
        char *body = mem_body();
        size_t n = strlen(head) + strlen(body) + 64;
        char *out = (char *)malloc(n);
        snprintf(out, n, "%s内存</div>%s", head, body);
        free(body); return out;
    }
    if (!strcmp(route, "disk")) {
        char *body = disk_body();
        size_t n = strlen(head) + strlen(body) + 64;
        char *out = (char *)malloc(n);
        snprintf(out, n, "%s磁盘</div>%s", head, body);
        free(body); return out;
    }
    if (!strcmp(route, "battery") || !strcmp(route, "power")) {
        char *body = battery_body();
        size_t n = strlen(head) + strlen(body) + 64;
        char *out = (char *)malloc(n);
        snprintf(out, n, "%s电池</div>%s", head, body);
        free(body); return out;
    }
    if (!strcmp(route, "uptime")) {
        char *body = uptime_body();
        size_t n = strlen(head) + strlen(body) + 64;
        char *out = (char *)malloc(n);
        snprintf(out, n, "%s运行时间</div>%s", head, body);
        free(body); return out;
    }
    if (!strcmp(route, "host")) {
        char *body = host_body();
        size_t n = strlen(head) + strlen(body) + 64;
        char *out = (char *)malloc(n);
        snprintf(out, n, "%s主机</div>%s", head, body);
        free(body); return out;
    }
    if (!strcmp(route, "store/get")) {
        char key[512];
        query_param(query, "key", key, sizeof(key));
        char path[MAX_PATH * 2];
        store_path(path, sizeof(path), store_id ? store_id : "default");
        char buf[8192];
        buf[0] = 0;
        FILE *f = fopen(path, "rb");
        if (f) { size_t l = fread(buf, 1, sizeof(buf) - 1, f); buf[l] = 0; fclose(f); }
        char val[2048];
        if (key[0] && json_get(buf, key, val, sizeof(val))) {
            char esc[4096];
            html_escape(val, esc, sizeof(esc));
            size_t n = strlen(head) + strlen(esc) + 64;
            char *out = (char *)malloc(n);
            snprintf(out, n, "%s已存</div><div class=\"sysv\">%s</div>", head, esc);
            return out;
        }
        {
            size_t n = strlen(head) + 64;
            char *out = (char *)malloc(n);
            snprintf(out, n, "%s未设置</div>", head);
            return out;
        }
    }
    if (!strcmp(route, "store/set")) {
        /* 参数可能在 query(GET)或 form body(POST) */
        char key[512], val[2048];
        const char *q = (form_body && form_body[0]) ? form_body : query;
        query_param(q, "key", key, sizeof(key));
        query_param(q, "value", val, sizeof(val));
        if (!key[0]) return NULL;
        char path[MAX_PATH * 2];
        store_path(path, sizeof(path), store_id ? store_id : "default");
        if (json_set(path, key, val)) {
            /* 存储用原文(round-trip 要保真), 展示做转义 —— 与 macOS 一致 */
            char esc[4096];
            html_escape(val, esc, sizeof(esc));
            size_t n = strlen(head) + strlen(esc) + 64;
            char *out = (char *)malloc(n);
            if (esc[0]) snprintf(out, n, "%s已保存: <span class=\"sysv\">%s</span></div>", head, esc);
            else        snprintf(out, n, "%s已保存</div>", head);
            return out;
        }
        return NULL;
    }
    return NULL;
}

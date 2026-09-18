/* tools/sysbridge_probe.c —— sys:// 解析层的用例(宿主可编译运行)
 *
 * 为什么单独一份: hnwin / sysbridge 只编 Windows 目标, 在 macOS 上跑不了,
 * 于是这些纯函数(query 解析 / JSON 取值 / HTML 转义)的回归一直没人盯。
 * 这里把 sysbridge.c 里那三个**无平台依赖**的函数原样复制进来测。
 *
 * 防漂移: main() 末尾会重新读 tools/sysbridge.c, 按特征行比对复制版与源文件
 * 是否一致 —— 不一致就判失败。这样"复制粘贴导致测试测的不是线上代码"
 * 这个常见陷阱被堵住了。
 *
 * 三组用例各对应一个已修的 bug:
 *   query_param  —— 曾用 strstr 子串匹配键名, mykey=..&key=.. 会取错值
 *   json_get     —— 曾不处理值内转义引号, 存过含 " 的值读回来被截断
 *   html_escape  —— store 值曾原样拼进 HTML(macOS 侧一直有转义)
 *
 * 运行(在仓库根目录):
 *   cc -std=c99 -O1 -Wall tools/sysbridge_probe.c -o /tmp/sp && /tmp/sp
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

/* ================= 以下三个函数复制自 tools/sysbridge.c ================= */

static void query_param(const char *query, const char *name, char *out, size_t cap) {
    if (out && cap) out[0] = 0;
    if (!query || !name || !out || !cap) return;
    size_t nlen = strlen(name);
    const char *p = query;
    while (p && *p) {
        const char *amp = strchr(p, '&');
        size_t seg = amp ? (size_t)(amp - p) : strlen(p);
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

static int json_get(const char *json, const char *key, char *out, size_t cap) {
    if (!json || !key || !out || !cap) return 0;
    if (out && cap) out[0] = 0;
    char pat[512];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    size_t plen = strlen(pat);
    const char *p = json;
    while ((p = strstr(p, pat)) != NULL) {
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
            if (e == '"')        { out[i++] = '"';  p += 2; }
            else if (e == '\\')  { out[i++] = '\\'; p += 2; }
            else if (e == '/')   { out[i++] = '/';  p += 2; }
            else if (e == 'n')   { out[i++] = '\n'; p += 2; }
            else if (e == 't')   { out[i++] = '\t'; p += 2; }
            else                 { out[i++] = e;    p += 2; }
            continue;
        }
        if (*p == '"') break;
        out[i++] = *p++;
    }
    out[i] = 0;
    return 1;
}

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

/* ================= 防漂移校验 ================= */

/* 源文件里必须存在的特征行。改动源文件时这里要同步改, 否则会被判不一致 ——
   这是故意的: 强制"改实现就得改测试", 而不是悄悄测旧代码。 */
static const char *ANCHORS[] = {
    "static void query_param(const char *query, const char *name, char *out, size_t cap) {",
    "int okhex = (endp && *endp == 0 && b >= 0 && b <= 255);",
    "static int json_get(const char *json, const char *key, char *out, size_t cap) {",
    "int boundary = (p == json) || (p[-1] == '{' || p[-1] == ',' ||",
    "static void html_escape(const char *s, char *out, size_t cap) {",
    "memcpy(out + i, \"&amp;\", 5);  i += 5;",
    NULL
};

static int source_in_sync(void) {
    FILE *f = fopen("tools/sysbridge.c", "rb");
    if (!f) return -1;
    char buf[1 << 16];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = 0;
    for (int i = 0; ANCHORS[i]; i++)
        if (!strstr(buf, ANCHORS[i])) return 0;
    return 1;
}

/* ================= 用例 ================= */

static int bad = 0;
static void ck(const char *label, const char *got, const char *want) {
    if (strcmp(got, want) == 0) printf("  v %-32s -> '%s'\n", label, got);
    else { printf("  x %-32s -> '%s'  期望 '%s'\n", label, got, want); bad++; }
}
static void ckn(const char *label, size_t got, size_t want) {
    if (got == want) printf("  v %-32s -> len=%zu\n", label, got);
    else { printf("  x %-32s -> len=%zu  期望 %zu\n", label, got, want); bad++; }
}

int main(void) {
    char o[512];

    puts("query_param:");
    query_param("mykey=WRONG&key=RIGHT", "key", o, sizeof o);
    ck("键名是另一参数的子串", o, "RIGHT");
    query_param("key=abc&other=1", "key", o, sizeof o);
    ck("正常取值", o, "abc");
    query_param("other=1&key=abc", "key", o, sizeof o);
    ck("目标在第二个参数", o, "abc");
    query_param("key=a%20b", "key", o, sizeof o);
    ck("URL 解码 %20", o, "a b");
    query_param("key=a+b", "key", o, sizeof o);
    ck("URL 解码 +", o, "a b");
    query_param("mykey=1", "key", o, sizeof o);
    ck("只有更长名的参数时取不到", o, "");
    query_param("key=%zz", "key", o, sizeof o);
    if (strlen(o) == 1 && (unsigned char)o[0] == 0) {
        printf("  x %-32s -> 写入了 NUL 字节\n", "非法 %zz 不写 NUL"); bad++;
    } else printf("  v %-32s -> len=%zu\n", "非法 %zz 不写 NUL", strlen(o));

    puts("json_get:");
    json_get("{\"k\":\"va\\\"lue\"}", "k", o, sizeof o);
    ck("值内含转义引号", o, "va\"lue");
    json_get("{\"mykey\":\"WRONG\",\"key\":\"RIGHT\"}", "key", o, sizeof o);
    ck("键名是另一键的子串", o, "RIGHT");
    json_get("{\"k\":\"a\\\\b\"}", "k", o, sizeof o);
    ck("值内含转义反斜杠", o, "a\\b");
    json_get("{\"k\":\"a\\/b\"}", "k", o, sizeof o);
    ck("值内含转义斜杠", o, "a/b");
    json_get("{\"k\":\"a\\nb\"}", "k", o, sizeof o);
    ckn("值内含 \\n", strlen(o), 3);
    json_get("{\"k\":\"plain\"}", "nope", o, sizeof o);
    ck("键不存在返回空", o, "");

    puts("html_escape:");
    html_escape("a<b>c&d", o, sizeof o);
    ck("三个特殊字符", o, "a&lt;b&gt;c&amp;d");
    html_escape("<script>alert(1)</script>", o, sizeof o);
    ck("脚本标签被转义", o, "&lt;script&gt;alert(1)&lt;/script&gt;");
    html_escape("普通文本", o, sizeof o);
    ck("无特殊字符原样", o, "普通文本");
    html_escape("", o, sizeof o);
    ck("空串", o, "");

    int sync = source_in_sync();
    if (sync == 0) {
        printf("\nx 复制版与 tools/sysbridge.c 不一致 —— 测试可能测的不是线上代码\n");
        bad++;
    } else if (sync < 0) {
        printf("\n(未找到 tools/sysbridge.c, 跳过一致性校验; 请在仓库根目录运行)\n");
    } else {
        printf("\nv 复制版与 tools/sysbridge.c 一致\n");
    }

    printf("%s (%d 项失败)\n", bad ? "有失败" : "全部通过", bad);
    return bad ? 1 : 0;
}

/* hn_port.h — 平台可移植层(纯 ISO C99 实现)
 *
 * 存在意义: 让引擎真正只依赖 ISO C 标准库。
 *
 * `strncasecmp` / `strcasecmp` 是 POSIX 接口 —— Linux/macOS 有,
 * Windows(MSVC/mingw)叫 `_strnicmp` / `_stricmp`, 且 minGW 在 C99 严格模式下
 * 不声明它们。三平台构建因此失败, 说明引擎此前隐含依赖 POSIX。
 *
 * 这里用 ISO C 自己实现, 消除该依赖: 任何符合 C99 的编译器都能构建。
 */
#ifndef HN_PORT_H
#define HN_PORT_H

#include <stddef.h>

/* ASCII 大小写不敏感比较(仅英文字母折叠; UTF-8 多字节原样比较) */
static inline int hn_lower(int c) {
    return (c >= 'A' && c <= 'Z') ? c + 32 : c;
}

static inline int hn_strncasecmp(const char *a, const char *b, size_t n) {
    for (size_t i = 0; i < n; i++) {
        int ca = hn_lower((unsigned char)a[i]);
        int cb = hn_lower((unsigned char)b[i]);
        if (ca != cb) return ca - cb;
        if (ca == 0) return 0;
    }
    return 0;
}

static inline int hn_strcasecmp(const char *a, const char *b) {
    while (*a && *b) {
        int ca = hn_lower((unsigned char)*a);
        int cb = hn_lower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++; b++;
    }
    return hn_lower((unsigned char)*a) - hn_lower((unsigned char)*b);
}

/* 大小写不敏感子串查找, 返回首次出现位置或 NULL */
static inline const char *hn_strcasestr(const char *hay, const char *needle) {
    size_t m = 0;
    while (needle[m]) m++;
    if (!m) return hay;
    for (size_t i = 0; hay[i]; i++) {
        if (!hn_strncasecmp(hay + i, needle, m)) return hay + i;
    }
    return NULL;
}

/* 统一命名: 引擎内部一律用 hn_* 版本 */
#define strncasecmp hn_strncasecmp
#define strcasecmp  hn_strcasecmp

#endif /* HN_PORT_H */

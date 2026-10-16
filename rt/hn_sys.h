/* hn_sys.h — host-independent sys:// routing. */
#ifndef HN_SYS_H
#define HN_SYS_H
#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void *ctx;
    int (*store_get)(void *ctx, const char *key, char *out, size_t cap);
    int (*store_set)(void *ctx, const char *key, const char *value);
    /* Optional host overrides. NULL preserves the platform implementation. */
    int (*clip_set)(void *ctx, const char *text);
    int (*clip_get)(void *ctx, char *out, size_t cap);
    int (*open_url)(void *ctx, const char *url);
    int (*notify)(void *ctx, const char *text);} hn_sys_host;

/* malloc fragment, caller frees; NULL for unsupported URL/unknown route.
 * Query fields take precedence over form fields, with existing raw encoding.
 * Store callbacks retain each host's storage backend and namespace. */
char *hn_sys_fragment(const hn_sys_host *host, const char *url, const char *form_body);
/* Same routing, with flushed hx diagnostics on unsupported/unknown URLs. */
char *hn_sys_request(const hn_sys_host *host, const char *url, const char *form_body);

/* Shared with runtime editing/link handling; unsupported platforms return 0. */
int hn_sys_clip_set(const char *text);
int hn_sys_clip_get(char *out, size_t cap);
int hn_sys_open_url(const char *url);
#ifdef __cplusplus
}
#endif
#endif

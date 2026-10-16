/* hn_http.h -- optional runtime HTTP transport (no compile-time dependency). */
#ifndef HN_HTTP_H
#define HN_HTTP_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;
    const char *value;
} hn_http_header;

typedef struct {
    const char *url;
    const char *method;
    const hn_http_header *headers;
    size_t header_count;
    const void *body;
    size_t body_len;
} hn_http_request_opts;

typedef struct {
    unsigned char *body;
    size_t body_len;
    long status;
    char *content_type;
    char *headers;
} hn_http_response;

int hn_http_available(void);
int hn_http_request(const hn_http_request_opts *opts, hn_http_response *out);
void hn_http_response_free(hn_http_response *response);
const char *hn_http_last_error(void);

typedef struct hn_http_scope hn_http_scope;
typedef void (*hn_http_complete_fn)(uint64_t id, int ok,
                                    const hn_http_response *response,
                                    const char *error, void *opaque);

hn_http_scope *hn_http_scope_create(void);
int hn_http_async_submit(hn_http_scope *scope, uint64_t id,
                         const hn_http_request_opts *opts);
size_t hn_http_async_drain(hn_http_scope *scope, hn_http_complete_fn callback,
                           void *opaque);
void hn_http_scope_destroy(hn_http_scope *scope);

#ifdef __cplusplus
}
#endif
#endif

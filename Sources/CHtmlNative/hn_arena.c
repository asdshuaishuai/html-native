/* hn_arena.c — bump 分配器: 文档/样式表生命周期内零碎片释放 */
#include <stdlib.h>
#include <string.h>
#include "hn_internal.h"

typedef struct hn_block {
    struct hn_block *next;
    size_t used, cap;
    char data[] __attribute__((aligned(16)));
} hn_block;

struct hn_arena { hn_block *head; };

static hn_block *block_new(size_t cap) {
    hn_block *b = malloc(sizeof(hn_block) + cap);
    if (!b) abort();
    b->next = NULL; b->used = 0; b->cap = cap;
    return b;
}

hn_arena *hn_arena_create(void) {
    hn_arena *a = calloc(1, sizeof(hn_arena));
    if (!a) abort();
    return a;
}

void *hn_arena_alloc(hn_arena *a, size_t n) {
    n = (n + 15) & ~(size_t)15;
    size_t want = n > 65536 ? n : 65536;
    if (!a->head || a->head->cap - a->head->used < n) {
        hn_block *b = block_new(want);
        b->next = a->head; a->head = b;
    }
    void *p = a->head->data + a->head->used;
    a->head->used += n;
    return p;
}

char *hn_arena_strndup(hn_arena *a, const char *s, size_t n) {
    char *p = hn_arena_alloc(a, n + 1);
    memcpy(p, s, n);
    p[n] = 0;
    return p;
}

void hn_arena_destroy(hn_arena *a) {
    hn_block *b = a->head;
    while (b) { hn_block *nx = b->next; free(b); b = nx; }
    free(a);
}

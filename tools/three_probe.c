/* three_probe.c — P1-3/P1-8 display-list lifetime regression.
 * Run via tools/three_probe.sh (ASan/UBSan, no disk assets or GPU).
 * A cold miss returns the DOM attribute, so cold-only pressure cannot catch
 * the historic UAF. Warm three entries, hit all three in the next paint walk,
 * then emit five more nodes before consuming ANY queued TRIS command.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn_internal.h"

#define NODES 8
static int failures, checks;
static void check(int ok, const char *what) {
    checks++;
    if (!ok) { failures++; fprintf(stderr, "FAIL %s\n", what); }
}

static const unsigned char png[] = {
    0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0,0,0,0x0d,
    0x49,0x48,0x44,0x52,0,0,0,1,0,0,0,1,8,6,0,0,0,0x1f,0x15,0xc4,0x89,
    0,0,0,0x0d,0x49,0x44,0x41,0x54,0x78,0x9c,0x63,0xf8,0xdf,0xe0,
    0xf0,0x1f,0,7,0,2,0xbf,0x2b,0xd7,0xc7,0xe2,
    0,0,0,0,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82
};
static const char *const geos[NODES] = {
    "box", "plane", "sphere", "", "unknown", "plane", "sphere", "box"
};
static const int triangles[NODES] = {12, 2, 960, 12, 12, 2, 960, 12};
typedef struct {
    char paths[NODES][160];
    int calls[NODES], bad_calls;
    hn_asset_backend backend;
    hn_context *ctx;
    hn_node *nodes[NODES];
} fixture;

static const char *load(void *opaque, const char *path, size_t *len) {
    fixture *f = opaque;
    for (int i = 0; i < NODES; i++) {
        if (!strcmp(path, f->paths[i])) {
            f->calls[i]++;
            *len = sizeof(png);
            return (const char *)png;
        }
    }
    f->bad_calls++;
    if (!strcmp(path, "bad.png")) {
        *len = 12;
        return "not a PNG!!!";
    }
    if (!strcmp(path, "short.png")) {
        *len = 3;
        return "PNG";
    }
    *len = 0;
    return NULL;
}

static void fixture_init(fixture *f, int tag) {
    memset(f, 0, sizeof(*f));
    char html[8192];
    size_t used = (size_t)snprintf(html, sizeof(html),
        "<html><head><style>html,body{margin:0}"
        "div{width:80px;height:80px}</style></head><body>");
    for (int i = 0; i < NODES; i++) {
        /* Equal-length first paths expose allocator reuse/texture substitution;
         * a longer final path also exercises variable string lengths. */
        snprintf(f->paths[i], sizeof(f->paths[i]), "memory-%d-%d%s.png", tag, i,
                 i == NODES - 1 ? "-a-longer-texture-path" : "");
        used += (size_t)snprintf(html + used, sizeof(html) - used,
            "<div id='n%d' hn-three hn-three-geo='%s' hn-three-tex='%s' %s "
            "style='display:%s'></div>", i, geos[i], f->paths[i],
            i == 5 ? "hn-three-camera-orbit hn-three-azimuth='180'" : "",
            i < 3 ? "block" : "none");
    }
    snprintf(html + used, sizeof(html) - used, "</body></html>");
    f->ctx = hn_context_create();
    hn_context_set_doc(f->ctx, hn_parse_html(html, strlen(html)));
    f->backend.ctx = f;
    f->backend.load = load;
    hn_context_set_assets(f->ctx, &f->backend);
    for (int i = 0; i < NODES; i++) {
        char id[16];
        snprintf(id, sizeof(id), "n%d", i);
        f->nodes[i] = hn_doc_find_by_id(hn_context_doc(f->ctx), id);
        if (!f->nodes[i]) { fprintf(stderr, "missing fixture node\n"); exit(2); }
    }
}

/* Deliberately dereference strings only AFTER the complete display list has
 * been emitted. Under ASan, removing the tmp copy fails here on a warm hit. */
static void consume(fixture *f, int expected) {
    const hn_display_list *dl = hn_context_display_list(f->ctx);
    int n = 0, polygons = 0, bitmaps = 0;
    for (int i = 0; dl && i < dl->count; i++) {
        const hn_cmd *cmd = &dl->cmds[i];
        if (cmd->kind == HN_CMD_POLYGON) polygons++;
        if (cmd->kind == HN_CMD_BITMAP) bitmaps++;
        if (cmd->kind != HN_CMD_TRIS) continue;
        if (n >= expected) { check(0, "unexpected TRIS"); continue; }
        if (!cmd->text) {
            check(0, "TRIS preserves its texture path after eviction (non-NULL text)");
        } else {
            check(!strcmp(cmd->text, f->paths[n]), "TRIS preserves its texture path after eviction");
        }
        check(cmd->text_len == strlen(f->paths[n]), "TRIS text_len matches path");
        check(cmd->text != hn_node_attr(f->nodes[n], "hn-three-tex"), "TRIS owns a display-list path copy on cold miss too");
        check(cmd->mesh_cols == triangles[n] && cmd->mesh_rows == 1, "all geometry variants emit their full TRIS soup");
        check(cmd->mesh_verts && cmd->mesh_uv, "TRIS vertex and UV arrays exist");
        if (cmd->mesh_verts && cmd->mesh_uv) {
            int valid = 1;
            for (int k = 0; k < cmd->mesh_cols * 6; k++) {
                if (!isfinite(cmd->mesh_verts[k]) || !isfinite(cmd->mesh_uv[k]) ||
                    cmd->mesh_uv[k] < 0 || cmd->mesh_uv[k] > 1) valid = 0;
            }
            check(valid, "all TRIS vertices finite and UVs in range");
        }
        n++;
    }
    check(n == expected, "expected TRIS count in one display list");
    check(polygons == 0 && bitmaps == 0, "valid in-memory textures take TRIS, not fallback/backend");
}

static void warm_and_evict(fixture *f) {
    hn_context_layout(f->ctx, 160, 800, NULL);
    consume(f, 3);
    for (int i = 0; i < NODES; i++)
        check(f->calls[i] == (i < 3 ? 1 : 0), "warm only first three assets");
    /* All hits, no decode/load. Also covers lightweight repaint lifecycle. */
    hn_context_repaint_light(f->ctx);
    consume(f, 3);
    for (int i = 0; i < 3; i++) check(f->calls[i] == 1, "warm repaint proves positive cache hit");
    for (int i = 3; i < NODES; i++)
        hn_node_set_attr(f->nodes[i], "style", "display:block");
    hn_context_layout(f->ctx, 160, 800, NULL);
    consume(f, NODES);
    for (int i = 0; i < NODES; i++)
        check(f->calls[i] == 1, "pressure frame hits old entries before five misses evict them");
    /* FIFO traversal of >4 nodes must reload the first three after eviction. */
    hn_context_repaint_light(f->ctx);
    consume(f, NODES);
    for (int i = 0; i < 3; i++) check(f->calls[i] == 2, "next frame proves warmed entries were actually evicted");
    hn_context_repaint(f->ctx);
    consume(f, NODES);
    hn_context_layout(f->ctx, 160, 800, NULL);
    consume(f, NODES);
    check(f->bad_calls == 0, "only declared memory assets requested");
}

static void fallback(fixture *f, const char *path, int with_backend) {
    char html[256];
    snprintf(html, sizeof(html),
             "<div hn-three hn-three-tex='%s' style='width:80px;height:80px'></div>", path);
    hn_context *c = hn_context_create();
    hn_context_set_doc(c, hn_parse_html(html, strlen(html)));
    if (with_backend) hn_context_set_assets(c, &f->backend);
    int before = f->bad_calls;
    for (int frame = 0; frame < 2; frame++) {
        hn_context_layout(c, 160, 160, NULL);
        const hn_display_list *dl = hn_context_display_list(c);
        int tris = 0, polys = 0;
        for (int i = 0; i < dl->count; i++) {
            tris += dl->cmds[i].kind == HN_CMD_TRIS;
            polys += dl->cmds[i].kind == HN_CMD_POLYGON;
        }
        check(tris == 0 && polys == 12, "invalid/unavailable texture renders POLYGON on both frames");
    }
    check(f->bad_calls - before == (with_backend ? 1 : 0), "negative texture gate caches failure");
    hn_context_destroy(c);
}

int main(void) {
#ifdef _WIN32
    puts("three_probe: _WIN32 loader-guard host simulation (not a Windows executable)");
#else
    puts("three_probe: native loader, memory assets, deferred display-list consumption");
#endif
    fixture a, b;
    fixture_init(&a, 1);
    fixture_init(&b, 2); /* Keep both docs live: cache keys cannot alias. */
    warm_and_evict(&a);
    warm_and_evict(&b);
    consume(&a, NODES); /* B evicted A's cache, A's queued commands remain valid. */
    hn_context_destroy(b.ctx);
    consume(&a, NODES); /* Independent context teardown must also be harmless. */
    hn_context_repaint_light(a.ctx);
    consume(&a, NODES);
    fallback(&a, "missing.png", 1);
    fallback(&a, "short.png", 1);
    fallback(&a, "bad.png", 1);
    fallback(&a, "no-backend.png", 0);
    consume(&a, NODES); /* Negative cache entries may evict positive entries. */
    hn_context_destroy(a.ctx);
    printf("three_probe: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}

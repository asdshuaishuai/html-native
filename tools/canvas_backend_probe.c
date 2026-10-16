/* Canvas backend regression: real vtables + hn_paint integration, no GUI/JS. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn_internal.h"
#include "hn_canvas_backend.h"
#include "hnsoft.h"

static int failures;
static void check(int ok, const char *label) {
    printf("%s %s\n", ok ? "PASS" : "FAIL", label);
    failures += !ok;
}
static const unsigned char *frame(hn_canvas_backend *b) {
    const unsigned char *px = NULL;
    int w, h, stride;
    check(b->vt->frame(b, &px, &w, &h, &stride) && w == 64 && h == 48 && stride == 256,
          "backend frame is straight RGBA8");
    return px;
}
static hn_canvas_op rect(unsigned col) {
    hn_canvas_op op = {0};
    op.kind = HN_CV_FILL_RECT; op.w = 64; op.h = 48; op.color = col;
    return op;
}
static void direct(hn_canvas_backend *b) {
    check(b != NULL, "requested backend factory available");
    if (!b) return;
    hn_canvas_op op = rect(0xFF000080u);
    unsigned char first[64 * 48 * 4];
    b->vt->clear(b); b->vt->op(b, &op);
    const unsigned char *px = frame(b);
    if (!px) return;
    memcpy(first, px, sizeof first);
    for (int i = 0; i < 10; i++) { b->vt->clear(b); b->vt->op(b, &op); }
    px = frame(b);
    check(px && !memcmp(first, px, sizeof first), "translucent replay does not accumulate");
    b->vt->clear(b); px = frame(b);
    int empty = 1;
    for (size_t i = 0; px && i < sizeof first; i++) if (px[i]) empty = 0;
    check(px && empty, "clear after translucent fill is fully transparent");
    op.kind = HN_CV_CLIP_PUSH; op.x = 16; op.y = 12; op.w = 8; op.h = 8;
    b->vt->op(b, &op);
    op = rect(0x0000FFFFu); b->vt->op(b, &op);
    px = frame(b);
    check(px && px[(14 * 64 + 18) * 4 + 3] == 255 && px[(14 * 64 + 28) * 4 + 3] == 0,
          "offset rectangular clip uses width and height");
    b->vt->clear(b); op = rect(0xFF0000FFu); b->vt->op(b, &op);
    px = frame(b);
    check(px && px[(2 * 64 + 2) * 4] == 255 && px[(40 * 64 + 50) * 4 + 3] == 255,
          "new frame resets outstanding clip/save state");
    b->vt->clear(b);
    op = rect(0x112233FFu); b->vt->op(b, &op);
    op.kind = HN_CV_CLEAR_RECT; op.x = 16; op.y = 12; op.w = 16; op.h = 12;
    b->vt->op(b, &op);
    px = frame(b);
    check(px && px[(18 * 64 + 20) * 4 + 3] == 0 &&
          px[(2 * 64 + 2) * 4 + 3] == 255,
          "局部 clearRect 仅擦除矩形内透明度");
    b->vt->dispose(b);
}
static hn_context *context(void) {
    const char *html = "<html><head><style>body{margin:0}canvas{display:block}</style></head>"
                       "<body><canvas id='cv' width='64' height='48'></canvas></body></html>";
    hn_context *c = hn_context_create();
    hn_context_set_doc(c, hn_parse_html(html, strlen(html)));
    hn_context_layout(c, 64, 48, NULL);
    return c;
}
static void rec(hn_context *c, const char *json) {
    check(hn_canvas_record(c, json, strlen(json)) >= 0, "record canvas operation");
}
static const hn_cmd *bitmap(hn_context *c) {
    const hn_display_list *dl = hn_context_display_list(c);
    for (int i = 0; i < dl->count; i++) if (dl->cmds[i].kind == HN_CMD_BITMAP) return &dl->cmds[i];
    return NULL;
}
static unsigned char *png;
static size_t png_len;
static const char *load(void *ctx, const char *path, size_t *len) {
    (void)ctx; (void)path; *len = png_len; return png_len ? (const char *)png : NULL;
}
static void pipeline(int selected, int cg) {
    unsigned char tile[8 * 8 * 4];
    for (int y = 0; y < 8; y++) for (int x = 0; x < 8; x++) {
        unsigned char *p = tile + (y * 8 + x) * 4;
        p[0] = y < 4 ? 255 : 0; p[1] = 0; p[2] = y < 4 ? 0 : 255; p[3] = 255;
    }
    png = hnsoft_encode_png(tile, 8, 8, &png_len);
    check(png && png_len > 0, "test PNG encodes");
    hn_context *c = context();
    hn_asset_backend assets = {NULL, load}; hn_context_set_assets(c, &assets);
    rec(c, "{\"cv\":\"cv\",\"op\":\"frect\",\"x\":2,\"y\":2,\"w\":8,\"h\":8,\"col\":4278190208}");
    hn_context_repaint(c);
    const hn_cmd *cmd = bitmap(c);
    check((cmd != NULL) == selected, "hn_paint selects only the explicitly requested backend");
    unsigned char first[64 * 48 * 4];
    if (cmd) memcpy(first, cmd->bitmap, sizeof first);
    for (int i = 0; i < 8; i++) hn_context_repaint(c);
    cmd = bitmap(c);
    check(!selected || (cmd && !memcmp(first, cmd->bitmap, sizeof first)), "hn_paint repeated replay is stable");
    rec(c, "{\"cv\":\"cv\",\"op\":\"clear_rect\",\"x\":0,\"y\":0,\"w\":64,\"h\":48}");
    rec(c, "{\"cv\":\"cv\",\"op\":\"frect\",\"x\":30,\"y\":20,\"w\":8,\"h\":8,\"col\":65535}");
    hn_context_repaint(c); cmd = bitmap(c);
    check(!selected || (cmd && cmd->bitmap[(4 * 64 + 4) * 4 + 3] == 0 &&
          cmd->bitmap[(22 * 64 + 32) * 4 + 2] == 255), "clearRect and moved redraw leave no trail");
    hn_canvas_clear(c, "cv"); hn_context_repaint(c);
    check(!bitmap(c), "empty ledger emits no stale cached bitmap");
    if (cg) {
        rec(c, "{\"cv\":\"cv\",\"op\":\"image\",\"src\":\"tile\",\"x\":12,\"y\":8,\"w\":8,\"h\":8}");
        hn_context_repaint(c); cmd = bitmap(c);
        check(cmd && cmd->bitmap[(9 * 64 + 13) * 4] == 255 &&
              cmd->bitmap[(14 * 64 + 13) * 4 + 2] == 255 && cmd->bitmap[(9 * 64 + 25) * 4 + 3] == 0,
              "CG image draws upright at nonzero destination (real asset loading)");
        rec(c, "{\"cv\":\"cv\",\"op\":\"clear_rect\",\"x\":0,\"y\":0,\"w\":64,\"h\":48}");
        rec(c, "{\"cv\":\"cv\",\"op\":\"frect\",\"x\":0,\"y\":0,\"w\":64,\"h\":48,\"col\":255}");
        rec(c, "{\"cv\":\"cv\",\"op\":\"pdata\",\"x\":20,\"y\":10,\"w\":2,\"h\":2,\"px\":[255,0,0,0,0,255,0,255,0,0,255,255,255,255,0,255]}");
        hn_context_repaint(c); cmd = bitmap(c);
        check(cmd && cmd->bitmap[(10 * 64 + 20) * 4 + 3] == 0 &&
              cmd->bitmap[(10 * 64 + 21) * 4 + 1] == 255 &&
              cmd->bitmap[(10 * 64 + 21) * 4 + 3] == 255,
              "CG putImageData replaces pixels including alpha=0");
        rec(c, "{\"cv\":\"cv\",\"op\":\"clear_rect\",\"x\":0,\"y\":0,\"w\":64,\"h\":48}");
        rec(c, "{\"cv\":\"cv\",\"op\":\"prect\",\"src\":\"tile\",\"rep\":\"repeat\",\"x\":0,\"y\":0,\"w\":16,\"h\":8}");
        hn_context_repaint(c); cmd = bitmap(c);
        check(cmd && cmd->bitmap[(1 * 64 + 1) * 4] == 255 &&
              cmd->bitmap[(1 * 64 + 9) * 4] == 255,
              "CG pattern repeat tiles across the destination");
        rec(c, "{\"cv\":\"cv\",\"op\":\"text\",\"s\":\"Hi\",\"x\":2,\"y\":40,\"px\":18,\"col\":4294967295}");
        hn_context_repaint(c); cmd = bitmap(c);
        int ink = 0;
        if (cmd) for (int y = 20; y < 42; y++) for (int x = 0; x < 25; x++)
            ink += cmd->bitmap[(y * 64 + x) * 4 + 3] != 0;
        check(ink > 10, "CG CoreText branch emits glyph pixels");
        free(png);
        png = NULL; png_len = 0;
    }
    hn_context_destroy(c);
}
int main(int argc, char **argv) {
    int is_ity = argc > 1 && !strcmp(argv[1], "ity");
    int is_cg = argc > 1 && (!strcmp(argv[1], "cg") || !strcmp(argv[1], "system"));
    printf("canvas_backend_probe backend=%s\n", argc > 1 ? argv[1] : "default");
    if (is_ity) direct(hn_canvas_backend_ity_create(64, 48));
#ifdef HN_HAVE_CANVAS_CG
    if (is_cg) direct(hn_canvas_backend_cg_create(64, 48));
#else
    is_cg = 0;
#endif
    pipeline(is_ity || is_cg, is_cg);
    printf("canvas_backend_probe: %d failures\n", failures);
    return failures ? 1 : 0;
}

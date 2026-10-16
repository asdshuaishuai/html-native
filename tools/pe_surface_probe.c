/* pe_surface_probe.c — hn_pe 双缓冲 surface 尺寸失败回归
 *
 * 背景(ISSUES.md 附: 存量简化): "CG 后端 hn_pe surface 尺寸失败残留旧尺寸
 * (debug 构建触发断言)"。旧缺陷: PX_SurfaceCreate 失败时槽的 w/h 仍保留
 * 旧值 —— 下一帧同尺寸请求直接复用已 Free 的缓冲(UAF, debug 断言/ASan)。
 * 修复(fef4c63): 失败路径先清零 w/h 并 memset 槽, Free 由 w!=0 守卫。
 *
 * 手法: PE 池是 32MB 静态缓冲(PE_MP_SIZE) —— 超大元素(4000x3500 ≈ 56MB)
 * 的 PX_SurfaceCreate 必然失败, 失败注入确定性成立。序列:
 *   正常画(200x200) → 超大画(创建失败) → 正常画(200x200)
 * 修复前第三步会踩已 Free 的缓冲(ASan 报); 修复后三步全净。
 *
 * 用法: bash tools/pe_surface_probe.sh(需 vendor/painterengine 在场)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn.h"
#include "hn_internal.h"

static int g_pass, g_fail;
static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "  ✔" : "  ✘", what);
    if (ok) g_pass++; else g_fail++;
}

/* 1x1 合法 PNG(three_probe 同款字节) */
static const unsigned char png[] = {
    0x89,0x50,0x4e,0x47,0x0d,0x0a,0x1a,0x0a,0,0,0,0x0d,
    0x49,0x48,0x44,0x52,0,0,0,1,0,0,0,1,8,6,0,0,0,0x1f,0x15,0xc4,0x89,
    0,0,0,0x0d,0x49,0x44,0x41,0x54,0x78,0x9c,0x63,0xf8,0xdf,0xe0,
    0xf0,0x1f,0,7,0,2,0xbf,0x2b,0xd7,0xc7,0xe2,
    0,0,0,0,0x49,0x45,0x4e,0x44,0xae,0x42,0x60,0x82
};

static const char *load(void *opaque, const char *path, size_t *len) {
    (void)opaque; (void)path;
    *len = sizeof(png);
    return (const char *)png;
}

static hn_context *build(void) {
    const char *html =
        "<html><head><style>html,body{margin:0}</style></head><body>"
        "<div id='pe' hn-live='char.png' style='width:200px;height:200px'></div>"
        "</body></html>";
    hn_context *c = hn_context_create();
    hn_context_set_doc(c, hn_parse_html(html, strlen(html)));
    static hn_asset_backend be;
    memset(&be, 0, sizeof(be));
    be.ctx = NULL;
    be.load = load;
    hn_context_set_assets(c, &be);
    return c;
}

/* viewport 越大元素盒越大(width:200px 固定? 不 —— 直接改元素 style 宽高,
 * 重设文档会换 context; 简化: 两份文档两段断言共用同一 PE 池与双槽) */
static void paint_at(hn_context *c, int w, int h, int *bitmaps) {
    hn_node *n = hn_doc_find_by_id(hn_context_doc(c), "pe");
    char style[64];
    snprintf(style, sizeof(style), "width:%dpx;height:%dpx", w, h);
    hn_node_set_attr(n, "style", style);
    hn_context_layout(c, 4000, 4000, NULL);
    hn_context_repaint(c);
    const hn_display_list *dl = hn_context_display_list(c);
    *bitmaps = 0;
    for (int i = 0; dl && i < dl->count; i++)
        if (dl->cmds[i].kind == HN_CMD_BITMAP) (*bitmaps)++;
}

int main(void) {
    printf("pe_surface_probe — hn_pe 双缓冲 surface 尺寸失败回归\n");

    /* 阶段一: 正常尺寸入槽(槽0), 双缓冲交替 */
    hn_context *c = build();
    int bm = 0;
    paint_at(c, 200, 200, &bm);
    check(bm == 1, "正常尺寸 hn-live 出 BITMAP");
    paint_at(c, 200, 200, &bm);
    check(bm == 1, "同尺寸第二帧(另一槽)出 BITMAP");

    /* 阶段二: 超大(4000x3500*4 ≈ 56MB > 32MB 池) → PX_SurfaceCreate 失败
       旧缺陷在此把槽 w/h 留在旧值 + 缓冲已 Free; 修复后零化并返回 */
    paint_at(c, 4000, 3500, &bm);
    check(bm == 0, "超大 surface 创建失败 → 本帧无 BITMAP(如实降级)");

    /* 阶段三: 回到正常尺寸 —— 修复前: 槽 w/h 残留旧值, 复用已 Free 缓冲
       (ASan 報 UAF); 修复后: 重新创建, 干净出图 */
    paint_at(c, 200, 200, &bm);
    check(bm == 1, "失败后回正常尺寸干净重建(无残留复用)");
    paint_at(c, 200, 200, &bm);
    check(bm == 1, "失败后再交替槽也正常");

    hn_context_destroy(c);
    printf("结果: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

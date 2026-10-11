/* stream_probe.c — hn-stream 自动跟随验收(纯 C99, 离屏, 零 GUI 依赖)
 *
 * 契约(README「流式视图」节): 声明 hn-stream 的 overflow 容器 ——
 *   跟随: 内容增长后布局, 滚动偏移 == 最大可滚动(贴底);
 *   让出: 用户上翻(离底 > 48px)后再增长内容, 偏移不被强改;
 *   恢复: 滚回底部附近(≤ 48px)后增长内容, 重新贴底。
 * 全程用公开 API 驱动: hn_context_layout / hn_doc_swap(追加内容, 保持
 * 容器节点身份)/ hn_node_scroll_by(模拟滚轮)/ hn_node_scroll_get /
 * hn_node_scroll_range。
 *
 * 用法(由 tools/stream_probe.py 薄封装编译运行):
 *   python3 tools/stream_probe.py [二进制路径]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn.h"

static int g_pass = 0, g_fail = 0;

static void check(int cond, const char *label) {
    printf("%s %s\n", cond ? "  ✔" : "  ✘", label);
    if (cond) g_pass++; else g_fail++;
}

static int near_f(float got, float want, float tol) {
    return got >= want - tol && got <= want + tol;
}

static void check_f(float got, float want, const char *label) {
    int ok = near_f(got, want, 0.5f);
    printf("%s %s (got %.1f want %.1f)\n", ok ? "  ✔" : "  ✘", label, got, want);
    if (ok) g_pass++; else g_fail++;
}

/* ---------- 装载/查询辅助 ---------- */

#define VW 480.0f
#define VH 700.0f

static float measure_cb(void *ctx, const hn_font_desc *font,
                        const char *utf8, size_t len) {
    (void)ctx; (void)font; (void)utf8; (void)len;
    return 8.0f;   /* 等宽假度量: 内容高由显式 height 决定, 与字体无关 */
}
static void metrics_cb(void *ctx, const hn_font_desc *font,
                       float *ascent, float *descent, float *leading) {
    (void)ctx; (void)font;
    if (ascent) *ascent = 12.8f;
    if (descent) *descent = 3.2f;
    if (leading) *leading = 0;
}

static hn_context *g_c = NULL;

static void load_doc(void) {
    static const char *HTML =
        "<html><head><style>"
        "#stream { overflow: hidden; height: 220; border: 1 solid #444; }"
        ".pad { height: 120; }"
        "</style></head><body>"
        "<div id=\"stream\" hn-stream>"
        "<div class=\"pad\">block-1</div>"
        "</div>"
        "</body></html>";
    if (g_c) hn_context_destroy(g_c);
    hn_doc *doc = hn_parse_html(HTML, strlen(HTML));
    g_c = hn_context_create();
    hn_context_set_doc(g_c, doc);
}

static hn_node *stream_node(void) {
    return hn_doc_find_by_id(hn_context_doc(g_c), "stream");
}

/* 内容增长: 向 #stream 追加 n 个 120 高块(swap 保持容器节点身份) */
static void grow(int n) {
    char frag[512];
    size_t off = 0;
    for (int i = 0; i < n && off < sizeof(frag) - 40; i++)
        off += (size_t)snprintf(frag + off, sizeof(frag) - off,
                                "<div class=\"pad\">more-%d</div>", i);
    hn_doc *doc = hn_context_doc(g_c);
    int ok = hn_doc_swap(doc, "stream", HN_SWAP_APPEND, frag, strlen(frag));
    if (!ok) { printf("  ✘ swap 追加失败\n"); g_fail++; }
    hn_context_layout(g_c, VW, VH, NULL);
}

static float scroll_y_of(hn_node *n) {
    float sx, sy;
    hn_node_scroll_get(n, &sx, &sy);
    return sy;
}

int main(void) {
    printf("stream_probe: hn-stream 自动跟随验收\n");

    /* ---- 1. 首次布局: 内容矮于容器, 无可滚, 偏移 0 ---- */
    load_doc();
    hn_context_layout(g_c, VW, VH, NULL);
    hn_node *n = stream_node();
    check(n != NULL, "装载成功(hn-stream 容器存在)");
    check_f(scroll_y_of(n), 0.0f, "首屏内容不足: 偏移 0");

    /* ---- 2. 内容增长 → 跟随(偏移 == 最大可滚动, 贴底) ---- */
    grow(4);   /* 内容 120*5=600, 容器 220 → max=380 */
    n = stream_node();
    float mx, my;
    hn_node_scroll_range(n, &mx, &my);
    check_f(my, 380.0f, "追加后最大可滚动 = 380");
    check_f(scroll_y_of(n), my, "跟随: 偏移 == 最大可滚动(贴底)");

    /* ---- 3. 模拟上翻(离底 > 48px)→ 再增长, 偏移不被强改 ---- */
    int moved = hn_node_scroll_by(n, 0, -300.0f);   /* 380-300=80, 离底 300 */
    check(moved == 1, "scroll_by 上翻生效");
    check_f(scroll_y_of(n), 80.0f, "上翻后偏移 80");
    grow(2);   /* 内容 840 → max=620 */
    n = stream_node();
    check_f(scroll_y_of(n), 80.0f, "让出: 再增长偏移仍 80(不被强改)");
    hn_node_scroll_range(n, &mx, &my);
    check(near_f(my - scroll_y_of(n), 540.0f, 0.5f), "让出状态离底 > 48px");

    /* ---- 4. 回到底部附近(≤ 48px)→ 恢复跟随 ---- */
    hn_node_scroll_by(n, 0, +10000.0f);   /* 钳到底: 离底 0 → 恢复 */
    check_f(scroll_y_of(n), 620.0f, "滚回底部: 偏移钳到最大 620");
    grow(1);   /* 内容 960 → max=740 */
    n = stream_node();
    check_f(scroll_y_of(n), 740.0f, "恢复跟随: 再增长重新贴底");

    /* ---- 5. 边界: 上翻不离底(≤ 48px)不算让出 ---- */
    hn_node_scroll_by(n, 0, -30.0f);   /* 离底 30 ≤ 48 */
    grow(1);   /* 内容 1080 → max=860 */
    n = stream_node();
    check_f(scroll_y_of(n), 860.0f, "离底 ≤ 48px: 仍跟随贴底");

    /* ---- 6. 文档替换(节点重建)回默认跟随 ---- */
    load_doc();   /* 整文档重装 */
    hn_context_layout(g_c, VW, VH, NULL);
    n = stream_node();
    grow(4);
    n = stream_node();
    hn_node_scroll_range(n, &mx, &my);
    check_f(scroll_y_of(n), my, "文档替换后默认跟随(贴底)");

    hn_context_destroy(g_c);
    g_c = NULL;
    printf("\nstream_probe: %d 项断言, 失败 %d\n", g_pass + g_fail, g_fail);
    return g_fail ? 1 : 0;
}

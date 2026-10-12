/* stream_probe.c — hn-stream 自动跟随验收(纯 C99, 离屏, 零 GUI 依赖)
 *
 * 契约(README「流式视图」节): 声明 hn-stream 的 overflow 容器 ——
 *   跟随: 内容增长后布局 + 帧驱动, 滚动偏移收敛到最大可滚动(贴底);
 *   让出: 用户上翻(离底 > 48px)后再增长内容, 偏移不被强改;
 *   恢复: 滚回底部附近(≤ 48px)后增长内容, 重新贴底。
 *   滑窗: hn-stream-loop="N" 只保留最近 N 个直接子元素, 更早的从 DOM
 *         摘除(内容可无限追加而内存与视觉收敛); 无该属性则不收敛。
 *   平滑: 帧驱动下跟随是指数逼近 —— 首帧在途(偏移推进但未到底),
 *         足够帧数后收敛贴底; 让出期间不动画。
 * 全程用公开 API 驱动: hn_context_layout / hn_doc_swap(追加内容, 保持
 * 容器节点身份)/ hn_node_scroll_by(模拟滚轮)/ hn_node_scroll_get /
 * hn_node_scroll_range / hn_context_anim_tick(帧时钟)。
 *
 * 用法(由 tools/stream_probe.py 薄封装编译运行):
 *   bash tools/tools/stream_probe.sh [二进制路径]
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

static void load_html(const char *HTML) {
    if (g_c) hn_context_destroy(g_c);
    hn_doc *doc = hn_parse_html(HTML, strlen(HTML));
    g_c = hn_context_create();
    hn_context_set_doc(g_c, doc);
}

static void load_doc(void) {
    load_html(
        "<html><head><style>"
        "#stream { overflow: hidden; height: 220; border: 1 solid #444; }"
        ".pad { height: 120; }"
        "</style></head><body>"
        "<div id=\"stream\" hn-stream>"
        "<div class=\"pad\">block-1</div>"
        "</div>"
        "</body></html>");
}

/* 滑窗容器: hn-stream-loop="3", 初始 3 条 mark-a/b/c */
static void load_doc_loop(void) {
    load_html(
        "<html><head><style>"
        "#loop { overflow: hidden; height: 220; }"
        ".pad { height: 120; }"
        "</style></head><body>"
        "<div id=\"loop\" hn-stream hn-stream-loop=\"3\">"
        "<div class=\"pad\">mark-a</div>"
        "<div class=\"pad\">mark-b</div>"
        "<div class=\"pad\">mark-c</div>"
        "</div>"
        "</body></html>");
}

static hn_node *stream_node(void) {
    return hn_doc_find_by_id(hn_context_doc(g_c), "stream");
}
static hn_node *loop_node(void) {
    return hn_doc_find_by_id(hn_context_doc(g_c), "loop");
}

/* 直接子元素计数(公开遍历 API) */
static int count_children(hn_node *n) {
    int k = 0;
    for (hn_node *ch = hn_node_first_child(n); ch; ch = hn_node_next_sibling(ch))
        k++;
    return k;
}

/* 显示列表里是否存在含 needle 的 TEXT 指令(文本按字节界内搜索) */
static int dl_has_text(const char *needle) {
    const hn_display_list *dl = hn_context_display_list(g_c);
    if (!dl) return 0;
    size_t nl = strlen(needle);
    for (int i = 0; i < dl->count; i++) {
        const hn_cmd *cmd = &dl->cmds[i];
        if (cmd->kind != HN_CMD_TEXT || !cmd->text || cmd->text_len < nl)
            continue;
        for (size_t o = 0; o + nl <= cmd->text_len; o++)
            if (memcmp(cmd->text + o, needle, nl) == 0) return 1;
    }
    return 0;
}

/* 内容增长(不驱动帧时钟): 向容器 swap 追加一个 120 高块(swap 保持容器
   节点身份), 只做布局。平滑追上由布局武装、anim_tick 逐帧推进 —— 本
   函数返回后偏移仍在途中, 供"在途/收敛"断言观察。 */
static void grow_raw_id(const char *id, const char *mark) {
    char frag[128];
    snprintf(frag, sizeof frag, "<div class=\"pad\">%s</div>", mark);
    int ok = hn_doc_swap(hn_context_doc(g_c), id, HN_SWAP_APPEND,
                         frag, strlen(frag));
    if (!ok) { printf("  ✘ swap 追加失败(%s)\n", mark); g_fail++; }
    hn_context_layout(g_c, VW, VH, NULL);
}

/* 帧时钟驱动到收敛: 16ms 帧喂 anim_tick 直至无在途动画(上限兜底)。
   平滑追上以指数逼近收敛, 收敛后 anim_tick 返回 0。 */
static void settle(void) {
    for (int i = 0; i < 600; i++)
        if (!hn_context_anim_tick(g_c, 16.0f)) return;
}

/* 内容增长: 追加 n 个 120 高块并驱动帧时钟到贴底终态(旧行为口径:
   "增长 → 布局 → 偏移 == 最大"在本探针里等价于 swap+布局+收敛) */
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
    settle();
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

    /* ---- 7. hn-stream-loop="3": 环形缓冲滑窗 ---- */
    load_doc_loop();
    hn_context_layout(g_c, VW, VH, NULL);
    settle();   /* 帧驱动口径(与真实窗口一致) */
    n = loop_node();
    check(n != NULL, "装载成功(hn-stream-loop 容器存在)");
    check(count_children(n) == 3, "初始 3 条全部保留");
    grow_raw_id("loop", "mark-d");
    grow_raw_id("loop", "mark-e");
    grow_raw_id("loop", "mark-f");
    n = loop_node();
    check(count_children(n) == 3, "塞到 6 条后仍保留 3 条(环形窗口)");
    settle();
    check(!dl_has_text("mark-a") && !dl_has_text("mark-b") &&
          !dl_has_text("mark-c"), "最旧 3 条(mark-a/b/c)已从显示列表摘除");
    check(dl_has_text("mark-d") && dl_has_text("mark-e") &&
          dl_has_text("mark-f"), "最新 3 条(mark-d/e/f)仍在");
    grow_raw_id("loop", "mark-g");
    n = loop_node();
    check(count_children(n) == 3, "再增 1 条仍 3 条(窗口滑动)");
    settle();
    check(!dl_has_text("mark-d"), "最旧一条(mark-d)被挤出窗口");
    check(dl_has_text("mark-e") && dl_has_text("mark-f") &&
          dl_has_text("mark-g"), "窗口内容 = mark-e/f/g(滑动正确)");

    /* ---- 8. 无 hn-stream-loop: 子元素数不收敛(回归) ---- */
    load_doc();
    hn_context_layout(g_c, VW, VH, NULL);
    grow(4);
    grow(3);
    n = stream_node();
    check(count_children(n) == 8, "无 loop 属性: 追加 7 条后 8 条全保留(不收敛)");

    /* ---- 9. 平滑追上: 帧驱动指数逼近 ---- */
    load_doc();
    hn_context_layout(g_c, VW, VH, NULL);
    hn_context_anim_tick(g_c, 16.0f);   /* 建立帧驱动口径(live_frame) */
    grow_raw_id("stream", "raw-1"); grow_raw_id("stream", "raw-2");
    grow_raw_id("stream", "raw-3"); grow_raw_id("stream", "raw-4");
    n = stream_node();
    hn_node_scroll_range(n, &mx, &my);
    check_f(my, 380.0f, "内容增长后最大可滚动 = 380(未驱动)");
    float y0 = scroll_y_of(n);
    check(y0 < my, "武装动画: 偏移未瞬移到最大(在途起点)");
    int act = hn_context_anim_tick(g_c, 16.0f);
    float y1 = scroll_y_of(n);
    check(act == 1, "在途: anim_tick 报告活跃(帧循环不熄火)");
    check(y1 > y0 && y1 < my, "第一帧: 偏移推进但未到底(指数逼近在途)");
    settle();
    n = stream_node();
    check_f(scroll_y_of(n), my, "足够帧数后收敛贴底(偏移 == 最大)");
    check(hn_context_anim_tick(g_c, 16.0f) == 0, "收敛后 anim_tick 报告空闲");

    /* 让出后不动画: 用户接管期间武装被抑制, 帧驱动也不强改偏移 */
    moved = hn_node_scroll_by(n, 0, -300.0f);
    check(moved == 1, "平滑段: 上翻让出生效");
    float y_pre = scroll_y_of(n);
    grow_raw_id("stream", "raw-5"); grow_raw_id("stream", "raw-6");
    n = stream_node();
    hn_context_anim_tick(g_c, 16.0f);
    check_f(scroll_y_of(n), y_pre, "让出后第一帧不动画(偏移不动)");
    settle();
    n = stream_node();
    check_f(scroll_y_of(n), y_pre, "让出后持续帧驱动仍不动画(偏移不被强改)");

    hn_context_destroy(g_c);
    g_c = NULL;
    printf("\nstream_probe: %d 项断言, 失败 %d\n", g_pass + g_fail, g_fail);
    return g_fail ? 1 : 0;
}

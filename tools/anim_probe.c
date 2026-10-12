/* anim_probe.c — 动画/交互/流畅性验收(纯 C99, 离屏, 确定性时钟驱动)
 *
 * 三组语义(与本轮"动画、交互与流畅性收口"一一对应):
 *   A. 过渡推进: 改属性 → layout → anim_tick 逐帧 —— 中间帧插值值
 *      ≠ 两端, 足够帧后收敛终值; anim_tick 返回值随在途/收敛翻转。
 *   B. 帧节拍(hn_rt_frame 返回值契约, app/hn_app.c 的帧节拍接线依据):
 *      动画活跃 → 16; 无动画无轮询 → -1(空闲, 宿主按自己的节奏低频醒);
 *      hx-trigger="every 2s" → ≈2000(容差)。
 *   C. 平滑锚滚动: 实时帧(anim_tick 已驱动过, live_frame 口径)下点击
 *      #锚 → 武装动画, 偏移逐帧推进、收敛到"滚进可视区"的终态; 离屏
 *      单帧(无 anim_tick)直接落位; 用户滚动(hn_node_scroll_by)立即
 *      中断动画。
 * 全程公开 API: hn_node_set_attr / hn_context_layout / hn_context_anim_tick /
 * hn_context_display_list / hn_rt_frame / hn_context_activate_link /
 * hn_node_scroll_by / hn_node_scroll_get / hn_node_scroll_range。
 *
 * 用法(由 tools/anim_probe.py 薄封装编译运行):
 *   bash tools/tools/anim_probe.sh [二进制路径]
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn.h"
#include "hn_rt.h"

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

/* ---------- 引擎级装载/查询辅助(与 stream_probe 同一口径) ---------- */

#define VW 480.0f
#define VH 700.0f

static hn_context *g_c = NULL;

static float measure_cb(void *ctx, const hn_font_desc *font,
                        const char *utf8, size_t len) {
    (void)ctx; (void)font; (void)utf8; (void)len;
    return 8.0f;   /* 等宽假度量: 高度由显式 height 决定, 与字体无关 */
}
static void metrics_cb(void *ctx, const hn_font_desc *font,
                       float *ascent, float *descent, float *leading) {
    (void)ctx; (void)font;
    if (ascent) *ascent = 12.8f;
    if (descent) *descent = 3.2f;
    if (leading) *leading = 0;
}

static void load_html(const char *HTML) {
    if (g_c) hn_context_destroy(g_c);
    hn_doc *doc = hn_parse_html(HTML, strlen(HTML));
    g_c = hn_context_create();
    hn_context_set_doc(g_c, doc);
}

static void layout(void) { hn_context_layout(g_c, VW, VH, NULL); }

static hn_node *by_id(const char *id) {
    return hn_doc_find_by_id(hn_context_doc(g_c), id);
}

static float scroll_y_of(hn_node *n) {
    float sx, sy;
    hn_node_scroll_get(n, &sx, &sy);
    return sy;
}

/* 帧时钟驱动到收敛: 16ms 一帧喂 anim_tick 直至无在途动画(上限兜底) */
static void settle(void) {
    for (int i = 0; i < 600; i++)
        if (!hn_context_anim_tick(g_c, 16.0f)) return;
}

/* 显示列表里第一个"大矩形"的填充色(过渡卡是唯一显式着色的大块) */
static unsigned rect_fill_big(void) {
    const hn_display_list *dl = hn_context_display_list(g_c);
    if (!dl) return 0;
    for (int i = 0; i < dl->count; i++) {
        const hn_cmd *cmd = &dl->cmds[i];
        if (cmd->kind == HN_CMD_RECT && cmd->w > 100 && cmd->h > 60 &&
            (cmd->fill >> 24) != 0)   /* 有实际着色(跳过全透明底) */
            return cmd->fill;
    }
    return 0;
}

/* ---------- A. 过渡推进 ---------- */

static void test_transition(void) {
    printf("== A. 过渡推进(改属性 → layout → anim_tick 逐帧) ==\n");
    load_html(
        "<html><head><style>"
        "body { margin: 0; }"
        "#card { width: 300; height: 150; margin: 20; "
        "        background-color: #102030; transition: background-color .3s; }"
        "</style></head><body>"
        "<div id=\"card\"></div>"
        "</body></html>");
    layout();
    hn_node *card = by_id("card");
    check(card != NULL, "装载成功(过渡卡存在)");

    /* 首帧: 初始化动画状态(不产生动画), 记录起点色 */
    hn_context_anim_tick(g_c, 16.0f);
    hn_context_repaint(g_c);
    unsigned c0 = rect_fill_big();
    check(((c0 >> 24) & 0xFF) == 0x10, "起点色 = #102030(未过渡)");

    /* 改属性 → 重布局 → 推进一帧 */
    hn_node_set_attr(card, "style",
                     "width: 300; height: 150; margin: 20;"
                     "background-color: #f08070; transition: background-color .3s;");
    layout();
    int act = hn_context_anim_tick(g_c, 16.0f);
    hn_context_repaint(g_c);
    unsigned c1 = rect_fill_big();
    int r1 = (int)((c1 >> 24) & 0xFF);
    check(act == 1, "在途: anim_tick 报告活跃(帧循环不熄火)");
    check(r1 > 0x10 && r1 < 0xf0,
          "中间帧插值值 ≠ 两端(红通道在 16..240 之间)");
    check(r1 > 0x10 && r1 < 0x60, "第一帧只走了一小步(16ms / 300ms)");

    /* 足够帧后收敛终值 */
    settle();
    hn_context_repaint(g_c);
    unsigned c2 = rect_fill_big();
    int ok_end = ((c2 >> 24) & 0xFF) == 0xf0 && ((c2 >> 16) & 0xFF) == 0x80;
    check(ok_end, "收敛终值 = #f08070(逐字节)");
    check(hn_context_anim_tick(g_c, 16.0f) == 0, "收敛后 anim_tick 报告空闲");
}

/* ---------- B. 帧节拍(hn_rt_frame 返回值契约) ---------- */

static void test_frame_pacing(void) {
    printf("== B. 帧节拍(hn_rt_frame 返回值: 16 / -1 / ≈轮询期限) ==\n");

    /* B1. 动画活跃(keyframes 无限循环) → 16, 且报告 repaint */
    {
        const char *HTML =
            "<html><head><style>"
            "@keyframes pulse { from { opacity: 0.4; } to { opacity: 1; } }"
            "#a { animation: pulse 0.6s linear infinite; }"
            "</style></head><body><div id=\"a\">x</div></body></html>";
        hn_rt_desc d = { "anim-probe", HTML, strlen(HTML), NULL, 0, 400, 300 };
        hn_rt *rt = hn_rt_open(&d);
        check(rt != NULL, "B1: hn_rt_open(动画文档)");
        if (rt) {
            int rp = 0;
            int b = hn_rt_frame(rt, 16.0f, &rp);
            check(b == 16, "B1: 动画活跃 → 返回 16");
            check(rp == 1, "B1: 动画帧报告 repaint");
            b = hn_rt_frame(rt, 16.0f, &rp);
            check(b == 16, "B1: 第二帧仍 16(无限循环不熄火)");
            hn_rt_close(rt);
        }
    }

    /* B2. 无动画无轮询 → -1(空闲), 不报告 repaint */
    {
        const char *HTML =
            "<html><body><div id=\"s\">static</div></body></html>";
        hn_rt_desc d = { "anim-probe", HTML, strlen(HTML), NULL, 0, 400, 300 };
        hn_rt *rt = hn_rt_open(&d);
        check(rt != NULL, "B2: hn_rt_open(静态文档)");
        if (rt) {
            int rp = -1;
            int b = hn_rt_frame(rt, 16.0f, &rp);
            check(b == -1, "B2: 无动画无轮询 → 返回 -1(空闲)");
            check(rp == 0, "B2: 空闲帧不报告 repaint");
            b = hn_rt_frame(rt, 16.0f, &rp);
            check(b == -1, "B2: 持续空闲(稳定 -1)");
            hn_rt_close(rt);
        }
    }

    /* B3. hx-trigger="every 2s" → ≈2000(容差 ±150ms; 首帧建表后
       返回距最近到期时刻的毫秒数) */
    {
        const char *HTML =
            "<html><body>"
            "<div id=\"p\" hx-get=\"sys://store/count\" hx-trigger=\"every 2s\">0</div>"
            "</body></html>";
        hn_rt_desc d = { "anim-probe", HTML, strlen(HTML), NULL, 0, 400, 300 };
        hn_rt *rt = hn_rt_open(&d);
        check(rt != NULL, "B3: hn_rt_open(every 2s 文档)");
        if (rt) {
            int rp = -1;
            int b = hn_rt_frame(rt, 16.0f, &rp);
            check(b > 1850 && b <= 2000, "B3: hx 轮询文档 → ≈2000(容差内)");
            hn_rt_close(rt);
        }
    }
}

/* ---------- C. 平滑锚滚动 ---------- */

/* 三节长内容 + 顶部目录(#链接): 容器 300 高(无边框, 盒高即可视高),
   内容 100+120*3=460 → 最大可滚 160; sec3 的终态 = 底对齐钳到 160。 */
static const char *ANCHOR_HTML =
    "<html><head><style>"
    "body { margin: 0; }"
    "a { display: block; height: 20; }"
    "#view { overflow: hidden; height: 300; }"
    ".intro { height: 100; }"
    ".sec  { height: 120; }"
    "</style></head><body>"
    "<a href=\"#sec3\" id=\"go3\">go-sec3</a>"
    "<a href=\"#sec1\" id=\"go1\">go-sec1</a>"
    "<div id=\"view\">"
    "<div class=\"intro\">intro</div>"
    "<div class=\"sec\" id=\"sec1\">sec-1</div>"
    "<div class=\"sec\" id=\"sec2\">sec-2</div>"
    "<div class=\"sec\" id=\"sec3\">sec-3</div>"
    "</div>"
    "</body></html>";

static void click_anchor(const char *link_id) {
    hn_node *a = by_id(link_id);
    float x, y, w, h;
    hn_node_box(a, &x, &y, &w, &h);
    int r = hn_context_activate_link(g_c, x + w / 2, y + h / 2);
    if (r != HN_LINK_ANCHOR) { printf("  ✘ 点击 %s 未识别为页内锚\n", link_id); g_fail++; }
}

static void test_anchor_scroll(void) {
    printf("== C. 平滑锚滚动(实时帧逐帧逼近 / 离屏直接落位 / 用户中断) ==\n");

    /* C1. 实时帧口径: 武装 → 逐帧推进 → 收敛"滚进可视区"终态 */
    load_html(ANCHOR_HTML);
    layout();
    hn_context_anim_tick(g_c, 16.0f);   /* 建立帧驱动口径(live_frame) */
    hn_node *view = by_id("view");
    check(view != NULL, "C1: 装载成功(滚动容器存在)");
    float mx, my;
    hn_node_scroll_range(view, &mx, &my);
    check_f(my, 160.0f, "C1: 最大可滚动 = 160");
    click_anchor("go3");
    check_f(scroll_y_of(view), 0.0f, "C1: 武装当帧不瞬跳(偏移仍 0)");
    int act = hn_context_anim_tick(g_c, 16.0f);
    float y1 = scroll_y_of(view);
    check(act == 1, "C1: 在途: anim_tick 报告活跃(帧循环不熄火)");
    check(y1 > 0.0f && y1 < 160.0f, "C1: 第一帧偏移推进但未到位(指数逼近在途)");
    settle();
    check_f(scroll_y_of(view), 160.0f, "C1: 收敛到底对齐钳界终态(160)");
    hn_node *sec3 = by_id("sec3");
    float bx, by3, bw, bh3;
    hn_node_box(view, &bx, &by3, &bw, &bh3);
    float tx, ty3, tw, th3;
    hn_node_box(sec3, &tx, &ty3, &tw, &th3);
    /* 容器坐标下的可视窗口 = [scroll, scroll + bh](盒是内容坐标, 绘制/
       点击时才叠加偏移); 留 2px 松量 —— 底对齐终态撞上最大可滚钳界时,
       锚底边可停在窗底下方 ≤1px(与浏览器"滚不过头"一致)。 */
    float rel_top = (ty3 - by3) - scroll_y_of(view);
    float rel_bot = (ty3 + th3 - by3) - scroll_y_of(view);
    check(rel_top >= -2.0f && rel_bot <= bh3 + 2.0f,
          "C1: 锚元素已滚进容器可视区");
    check(hn_context_anim_tick(g_c, 16.0f) == 0, "C1: 收敛后 anim_tick 报告空闲");

    /* C2. 用户滚动立即中断: 武装后 scroll_by → 动画不再拉走偏移 */
    click_anchor("go1");                /* sec1 顶对齐终态 = 100 → 武装 */
    float yu = scroll_y_of(view);
    hn_node_scroll_by(view, 0, -30.0f); /* 用户上滚 30(接管; 160-30=130) */
    float yu2 = scroll_y_of(view);
    check(near_f(yu2 - yu, -30.0f, 0.5f), "C2: 用户滚动生效");
    hn_context_anim_tick(g_c, 16.0f);
    check_f(scroll_y_of(view), yu2, "C2: 用户接管后动画不再推进(中断生效)");
    settle();
    check_f(scroll_y_of(view), yu2, "C2: 持续帧驱动仍停在用户位置");

    /* C3. 离屏单帧口径(无 anim_tick): 直接落位 —— 截图管线不变 */
    load_html(ANCHOR_HTML);
    layout();                           /* 不 tick: live_frame 保持 0 */
    hn_node *view2 = by_id("view");
    click_anchor("go3");
    check_f(scroll_y_of(view2), 160.0f, "C3: 离屏单帧直接落位(=160, 无动画)");

    /* C4. 公开语义回归: hn_doc_scroll_to 未知 id 返回 0 */
    check(hn_doc_scroll_to(hn_context_doc(g_c), "no-such-id") == 0,
          "C4: hn_doc_scroll_to 未知 id 返回 0(语义不变)");
    check(hn_doc_scroll_to(hn_context_doc(g_c), "sec2") == 1,
          "C4: hn_doc_scroll_to 目标存在返回 1(语义不变)");
}

int main(void) {
    printf("anim_probe: 动画/交互/流畅性验收\n");
    test_transition();
    test_frame_pacing();
    test_anchor_scroll();
    printf("\nanim_probe: %d 项断言, 失败 %d\n", g_pass + g_fail, g_fail);
    return g_fail ? 1 : 0;
}

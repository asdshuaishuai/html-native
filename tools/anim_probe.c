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
#include "hn_internal.h"

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

/* D. Smart-UX 出现/消失默认动效(H5 第四轮):
 *   - 运行时 appendChild 的新节点: 首帧后半透明+下偏, 若干帧后收敛全亮
 *     (默认 fade+rise; 显式 animation 声明的节点不受影响);
 *   - JS remove 的子树: ghost 在原位淡出(显示列表里仍见其文本但 alpha
 *     递减), 生命周期后从显示列表消失;
 *   - 首帧文档内容不播默认入场(open 即终态, 不拖慢首屏)。 */
static void test_smartux_enter_exit(void) {
    printf("== Smart-UX 出现/消失默认动效 ==\n");
    const char *html =
        "<html><head><style>html,body{margin:0}</style></head><body>"
        "<div id='feed'></div><div id='static'>首屏静态</div></body></html>";
    hn_rt_desc d = { "smartux", html, strlen(html), NULL, 0, 360, 300 };
    hn_rt *rt = hn_rt_open(&d);
    check(rt != NULL, "hn_rt_open");
    if (!rt) return;
    hn_context *c = hn_rt_context(rt);
    /* 运行时插入(不经 JS, 直接引擎 append — 同 hn_node_append_element 口径) */
    hn_node *feed = hn_doc_find_by_id(hn_rt_doc(rt), "feed");
    hn_node *msg = hn_node_append_element(feed, "div");
    hn_node_set_attr(msg, "id", "msg");
    hn_node_set_text_content(msg, "消息", 6);
    /* 推 1 帧: 插入后 anim tick 应进入 entering(alpha 从 0 起) */
    hn_context_layout(c, 360, 300, NULL);
    hn_context_anim_tick(c, 40);
    hn_context_anim_tick(c, 0);   /* rt_render 同序: 零步进重采样把入场中间态写入样式 */
    hn_context_repaint(c);
    float o1 = msg->style.opacity;
    check(o1 < 0.9f, "动态插入首帧: 默认入场使 opacity 从 0 起");
    /* 推进到完成 */
    for (int i = 0; i < 20; i++) {
        hn_context_anim_tick(c, 40);
        hn_context_repaint(c);
    }
    float o2 = msg->style.opacity;
    check(near_f(o2, 1.0f, 0.02f), "入场收敛: opacity → 1");
    /* ghost: remove 后重绘, 显示列表仍含其文本(淡出中), alpha < 终态 */
    hn_node *st = hn_doc_find_by_id(hn_rt_doc(rt), "static");
    hn_node_set_text_content(st, "首屏静态GONE", 16);
    hn_context_layout(c, 360, 300, NULL);
    hn_context_repaint(c);
    hn_context_push_exit_ghost(c, st);
    hn_node_remove_child(st->parent, st);
    hn_context_repaint(c);
    const hn_display_list *dl = hn_context_display_list(c);
    int ghost_text = 0;
    for (int i = 0; dl && i < dl->count; i++)
        if (dl->cmds[i].kind == HN_CMD_TEXT && dl->cmds[i].text &&
            strstr(dl->cmds[i].text, "GONE"))
            ghost_text = 1;
    check(ghost_text, "ghost: 移除的子树仍在显示列表(t=0 原位)");
    /* 推进半程: 仍在列表但 alpha 已衰减 */
    hn_context_anim_tick(c, 80);
    hn_context_repaint(c);
    dl = hn_context_display_list(c);
    float ga = 1.0f;
    int ghost_mid = 0;
    for (int i = 0; dl && i < dl->count; i++)
        if (dl->cmds[i].kind == HN_CMD_TEXT && dl->cmds[i].text &&
            strstr(dl->cmds[i].text, "GONE")) {
            ghost_mid = 1;
            ga = (float)((dl->cmds[i].fill & 0xFFu)) / 255.0f;
        }
    check(ghost_mid && ga < 0.99f, "ghost: 半程 alpha < 1(淡出进行)");
    /* 推进 300ms: ghost 出列, 显示列表不再含其文本 */
    for (int i = 0; i < 8; i++) {
        hn_context_anim_tick(c, 40);
        hn_context_repaint(c);
    }
    dl = hn_context_display_list(c);
    int ghost_gone = 1;
    for (int i = 0; dl && i < dl->count; i++)
        if (dl->cmds[i].kind == HN_CMD_TEXT && dl->cmds[i].text &&
            strstr(dl->cmds[i].text, "GONE"))
            ghost_gone = 0;
    check(ghost_gone, "ghost 生命周期结束 → 从显示列表消失");
    hn_rt_close(rt);
}

/* == E. 桌面画板呈现(画刷铺色遮罩) ==
   纯函数端点/单调/羽化带 + rt 集成(不透明窗 = 画纸混合, 透明窗 = alpha 缩放)。
   确定性锚: apply 只动被喂的位图, --shot/--probe 路径不经过(见 hn_rt.h)。 */
static void test_paint_reveal(void) {
    printf("== E. 桌面画板呈现(画刷铺色遮罩) ==\n");
    const float W = 400, H = 300;
    /* 端点: u=0 全 0, u=1 全 1 */
    int bad = 0;
    for (int y = 0; y < 30; y++)
        for (int x = 0; x < 40; x++) {
            float fx = (x + 0.5f) * W / 40, fy = (y + 0.5f) * H / 30;
            if (hn_rt_paint_mask_cov(0.0f, fx, fy, W, H) != 0.0f) bad++;
            if (hn_rt_paint_mask_cov(1.0f, fx, fy, W, H) != 1.0f) bad++;
        }
    check(bad == 0, "遮罩端点: u=0 恒 0 / u=1 恒 1");
    /* 单调: 采样点随 u 不降 */
    bad = 0;
    for (int y = 0; y < 15; y++)
        for (int x = 0; x < 20; x++) {
            float fx = (x + 0.5f) * W / 20, fy = (y + 0.5f) * H / 15;
            float prev = -1;
            for (int k = 0; k <= 10; k++) {
                float m = hn_rt_paint_mask_cov(k / 10.0f, fx, fy, W, H);
                if (m < prev - 1e-6f) bad++;
                prev = m;
            }
        }
    check(bad == 0, "遮罩对 u 单调不降(画只往前铺, 不回卷)");
    /* u=0.5 三态并存: 已铺区(=1)/未铺区(=0)/羽化带(0<m<1) */
    int full = 0, none = 0, mid = 0, total = 0;
    for (int y = 0; y < 100; y++)
        for (int x = 0; x < 133; x++) {
            float fx = (x + 0.5f) * W / 133, fy = (y + 0.5f) * H / 100;
            float m = hn_rt_paint_mask_cov(0.5f, fx, fy, W, H);
            total++;
            if (m >= 1) full++;
            else if (m <= 0) none++;
            else mid++;
        }
    check(full > 0 && none > 0, "u=0.5 铺色进行中: 已铺/未铺两区并存");
    check(mid * 100 >= total * 2 && mid * 100 <= total * 45,
          "羽化带面积有界(2%~45%: 软笔锋, 不是硬擦线也不是全图虚化)");
    /* rt 集成: 不透明窗 = 未铺区域回落画纸底色 */
    const char *html =
        "<html><head><meta charset=\"utf-8\"><meta name=\"hn-transparent\" content=\"1\"></head>"
        "<body style=\"margin:0;background:transparent\">"
        "<div style=\"position:absolute;left:0;top:0;width:200;height:200;"
        "background:#ffffff\"></div></body></html>";
    hn_rt_desc d = { "anim-probe", html, strlen(html), NULL, 0, 120, 120 };
    hn_rt *rt = hn_rt_open(&d);
    check(rt != NULL, "画刷集成: rt 打开(透明窗)");
    if (rt) {
        /* 负断言: 不透明窗(哪怕无边框)不 eligible —— 否则先出画板再画内容 */
        const char *opaque =
            "<html><head><meta charset=\"utf-8\"><meta name=\"hn-frameless\" content=\"1\"></head>"
            "<body style=\"margin:0;background:#204080\"></body></html>";
        hn_rt_desc od = { "anim-probe-opaque", opaque, strlen(opaque), NULL, 0, 80, 80 };
        hn_rt *ort = hn_rt_open(&od);
        if (ort) {
            hn_rt_paint_reveal_begin(ort, 1000);
            check(hn_rt_paint_reveal_active(ort, 1100) == 0,
                  "不透明窗不做画刷(会先出画板, 用户裁决)");
            hn_rt_close(ort);
        }
        /* 负断言: 有标题栏窗口(无 frameless/transparent)不 eligible */
        const char *titled =
            "<html><head><meta charset=\"utf-8\"></head>"
            "<body style=\"margin:0;background:#204080\"></body></html>";
        hn_rt_desc td = { "anim-probe-titled", titled, strlen(titled), NULL, 0, 80, 80 };
        hn_rt *trt = hn_rt_open(&td);
        if (trt) {
            hn_rt_paint_reveal_begin(trt, 1000);
            check(hn_rt_paint_reveal_active(trt, 1100) == 0,
                  "有标题栏窗口不做画刷(平台淡入口径, 用户裁决)");
            hn_rt_close(trt);
        }
        hn_rt_paint_reveal_begin(rt, 1000);
        check(hn_rt_paint_reveal_active(rt, 1000) == 1, "begin 后在途");
        hn_rt_paint_reveal_apply(rt, 1000 + 120);   /* u=0.25 */
        const unsigned char *px = hn_rt_pixels(rt);
        check(px != NULL, "位图在");
        if (px) {
            int bw = hn_rt_bitmap_width(rt), bh = hn_rt_bitmap_height(rt);
            /* 透明窗口径: 未铺区域 alpha=0(透出桌面), 已铺区域 = 内容本色。
               右下角 = 轴投影最远, u=0.25 覆盖恰 0 → 全透明; 左下角在笔锋
               带内(小窗羽化占比高)→ 中间 alpha; 轴近端 = 白块不透明。 */
            int i_far = ((size_t)(bh - 2) * bw + (bw - 2)) * 4;
            check(px[i_far + 3] == 0,
                  "u=0.25 投影最远角 alpha=0(直接透出桌面, 不是画板)");
            int i_near = ((size_t)(bh - 2) * bw + 1) * 4;
            int a = px[i_near + 3];
            check(a > 0 && a < 255, "u=0.25 笔锋带外缘 = 中间 alpha(软边铺色)");
            int i_paint = ((size_t)2 * bw + 2) * 4;
            check(px[i_paint] == 0xFF && px[i_paint + 3] == 0xFF,
                  "u=0.25 轴近端 = 内容已铺(白块不透明)");
        }
        /* u≥1 收口: active 落 0 */
        check(hn_rt_paint_reveal_active(rt, 1000 + 481) == 0, "480ms 后收口");
        /* 干净帧守卫(遮罩原地叠加回归锚): 不重光栅连拍两次 apply, 输出
           必须与一次 apply 逐位一致 —— 守卫内部重出干净帧, 遮罩不叠加 */
        hn_rt_reflow(rt);
        hn_rt_paint_reveal_begin(rt, 2000);
        hn_rt_paint_reveal_apply(rt, 2000 + 120);
        unsigned char *snap1 = NULL;
        {
            int bw = hn_rt_bitmap_width(rt), bh = hn_rt_bitmap_height(rt);
            size_t n = (size_t)bw * bh * 4;
            snap1 = malloc(n);
            memcpy(snap1, hn_rt_pixels(rt), n);
            hn_rt_paint_reveal_apply(rt, 2000 + 121);   /* 同帧二次 apply */
            check(memcmp(snap1, hn_rt_pixels(rt), n) == 0,
                  "同帧二次 apply 逐位一致(raster_gen 守卫防叠加)");
        }
        /* 收口后重光栅 = 全干净帧(无遮罩残留) */
        hn_rt_reflow(rt);
        {
            int bw = hn_rt_bitmap_width(rt), bh = hn_rt_bitmap_height(rt);
            const unsigned char *p2 = hn_rt_pixels(rt);
            int i_far = ((size_t)(bh - 2) * bw + (bw - 2)) * 4;
            check(p2[i_far + 3] == 255, "收口后重光栅 = 全内容(无遮罩残留)");
        }
        free(snap1);
        hn_rt_close(rt);
    }
}

/* hn-stagger 错档(anime.js stagger 语义采纳): 容器声明 hn-stagger="每项ms",
   子项入场按"前序同批入场兄弟数"错档 —— 负进度起步(与 animation-delay
   同机制)。断言: 中途透明度严格按序递减(o0>o1>o2)、全部收敛到 1、
   无 hn-stagger 容器同时入场(零回归)、stagger=0 等价无错档。 */
static void test_stagger(void) {
    static const char *html =
        "<html><head><style>html,body{margin:0}</style></head><body>"
        "<div id=\"feed\" hn-stagger=\"80\"></div>"
        "<div id=\"plain\"></div>"
        "<div id=\"zero\" hn-stagger=\"0\"></div>"
        "</body></html>";
    hn_rt_desc d = { "stagger", html, strlen(html), NULL, 0, 360, 300 };
    hn_rt *rt = hn_rt_open(&d);
    check(rt != NULL, "hn_rt_open(stagger 页)");
    if (!rt) return;
    hn_context *c = hn_rt_context(rt);
    hn_node *feed = hn_doc_find_by_id(hn_rt_doc(rt), "feed");
    hn_node *plain = hn_doc_find_by_id(hn_rt_doc(rt), "plain");
    hn_node *zero = hn_doc_find_by_id(hn_rt_doc(rt), "zero");
    hn_node *items[3] = {0};
    for (int i = 0; i < 3; i++) {
        items[i] = hn_node_append_element(feed, "div");
        char idbuf[8]; snprintf(idbuf, sizeof(idbuf), "f%d", i);
        hn_node_set_attr(items[i], "id", idbuf);
        hn_node_append_element(plain, "div");
        hn_node_append_element(zero, "div");
    }
    hn_context_layout(c, 360, 300, NULL);
    hn_context_anim_tick(c, 0);          /* 初始化(错档负进度) */
    hn_context_anim_tick(c, 160.0f);     /* 推进 160ms: 三项处于错档中段 */
    float o0 = items[0]->style.opacity;
    float o1 = items[1]->style.opacity;
    float o2 = items[2]->style.opacity;
    fprintf(stderr, "    DBG o0=%.3f o1=%.3f o2=%.3f\n", o0, o1, o2);
    check(o0 > 0.05f, "错档首项已开始浮现");
    check(o0 > o1 && o1 > o2, "错档序: o0 > o1 > o2(透明度按序递减)");
    for (int i = 0; i < 12; i++) hn_context_anim_tick(c, 200.0f);
    o0 = items[0]->style.opacity;
    o1 = items[1]->style.opacity;
    o2 = items[2]->style.opacity;
    check(near_f(o0, 1.0f, 0.02f) && near_f(o1, 1.0f, 0.02f) && near_f(o2, 1.0f, 0.02f),
          "错档收敛: 全部 opacity → 1");
    /* plain(无 hn-stagger): 同批插入同时入场 —— 零回归对照 */
    hn_node *pn = hn_node_append_element(plain, "div");
    hn_node *pq = hn_node_append_element(plain, "div");
    hn_context_layout(c, 360, 300, NULL);
    hn_context_anim_tick(c, 0);
    hn_context_anim_tick(c, 100.0f);
    float po1 = pn->style.opacity, po2 = pq->style.opacity;
    check(near_f(po1, po2, 0.001f), "无 hn-stagger: 同批同时入场(零回归)");
    /* stagger=0: 等价无错档 */
    hn_node *z1 = hn_node_append_element(zero, "div");
    hn_node *z2 = hn_node_append_element(zero, "div");
    hn_context_layout(c, 360, 300, NULL);
    hn_context_anim_tick(c, 0);
    hn_context_anim_tick(c, 100.0f);
    float pz1 = z1->style.opacity, pz2 = z2->style.opacity;
    check(near_f(pz1, pz2, 0.001f), "stagger=0: 无错档(等价同时入场)");
    hn_rt_close(rt);
}

int main(void) {
    printf("anim_probe: 动画/交互/流畅性验收\n");
    test_transition();
    test_stagger();
    test_frame_pacing();
    test_anchor_scroll();
    test_smartux_enter_exit();
    test_paint_reveal();
    printf("\nanim_probe: %d 项断言, 失败 %d\n", g_pass + g_fail, g_fail);
    return g_fail ? 1 : 0;
}

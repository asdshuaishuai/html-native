/* canvas_probe.c — <canvas> 2D 账本全链路探针(确定性, 退出码判定)
 *
 * 管线与生产同一条: hn_rt_open(解析→布局→脚本自动执行→绘制) →
 * QuickJS glue(hnGet2D 状态机) → hnCanvas2D 落账 → hn_paint 重放 →
 * hnsoft 位图。断言分四层:
 *   1. 显示列表形态: op 翻译正确(CTM 后的 RECT/折线 POLYGON/渐变/TEXT/QUAD)
 *   2. 引擎内省: hn_canvas_count / overflow / C 直录路径
 *   3. 账本纪律: 重排版不丢账、4096 上限溢出标志、clear 归零
 *   4. 像素采样: hnsoft 位图中心像素 = 所填颜色(BGRA 预乘)
 *
 * 用法(py 薄封装编好即跑; 手工编译见 tools/canvas_probe.py):
 *   python3 tools/canvas_probe.py
 * 断言失败打印 got/want, 退出码 1; 全绿退出 0。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "hn.h"
#include "hn_rt.h"
/* hnsoft.h 是模块私有头: 验收必须调用生产光栅器, 以相对路径保持单一来源 */
#include "../Sources/CHtmlNative/hnsoft.h"

static int g_pass = 0, g_fail = 0;

static void check(int cond, const char *label) {
    printf("%s %s\n", cond ? "  ✔" : "  ✘", label);
    if (cond) g_pass++; else g_fail++;
}

static void check_int(long got, long want, const char *label) {
    int ok = got == want;
    printf("%s %s (got %ld want %ld)\n", ok ? "  ✔" : "  ✘", label, got, want);
    if (ok) g_pass++; else g_fail++;
}

static void check_color(hn_color got, hn_color want, const char *label) {
    int ok = got == want;
    printf("%s %s (got %08X want %08X)\n", ok ? "  ✔" : "  ✘", label, got, want);
    if (ok) g_pass++; else g_fail++;
}

static void check_near(float got, float want, float tol, const char *label) {
    int ok = (float)fabs((double)(got - want)) <= tol;
    printf("%s %s (got %.2f want %.2f±%.2f)\n", ok ? "  ✔" : "  ✘", label, got, want, tol);
    if (ok) g_pass++; else g_fail++;
}

/* ---- 探针文档: 六块 200x120 画布纵向排开(body margin 0, display:block)
 *  各画布盒子(绝对坐标, 由布局保证):
 *   cv  (0,0)    CTM + fillRect       cv2 (0,120) 折线路径 fill
 *   cv3 (0,240)  渐变 fillRect        cv4 (0,360) save/restore + stroke 折线
 *   cv5 (0,480)  fillText             cv6 (0,600) 留空(C 直录路径用) */
static const char *DOC_HTML =
"<html><head><style>body{margin:0}canvas{display:block}</style></head>"
"<body>"
"<canvas id=\"cv\" width=\"200\" height=\"120\"></canvas>"
"<canvas id=\"cv2\" width=\"200\" height=\"120\"></canvas>"
"<canvas id=\"cv3\" width=\"200\" height=\"120\"></canvas>"
"<canvas id=\"cv4\" width=\"200\" height=\"120\"></canvas>"
"<canvas id=\"cv5\" width=\"200\" height=\"120\"></canvas>"
"<canvas id=\"cv6\" width=\"200\" height=\"120\"></canvas>"
"<script>\n"
"var ctx = hnGet2D('cv');\n"
"ctx.translate(10, 5);\n"
"ctx.fillStyle = '#3366ff';\n"
"ctx.fillRect(0, 0, 50, 30);\n"
"\n"
"var c2 = hnGet2D('cv2');\n"
"c2.beginPath();\n"
"c2.moveTo(10, 10);\n"
"c2.lineTo(80, 10);\n"
"c2.arc(50, 50, 20, 0, Math.PI * 2, false);\n"
"c2.closePath();\n"
"c2.fillStyle = '#ff8800';\n"
"c2.fill();\n"
"\n"
"var c3 = hnGet2D('cv3');\n"
"var g = c3.createLinearGradient(0, 0, 100, 0);\n"
"g.addColorStop(0, '#ff0000');\n"
"g.addColorStop(1, '#0000ff');\n"
"c3.fillStyle = g;\n"
"c3.fillRect(10, 10, 100, 60);\n"
"\n"
"var c4 = hnGet2D('cv4');\n"
"c4.fillStyle = '#00ff00';\n"
"c4.save();\n"
"c4.fillStyle = '#ff0000';\n"
"c4.restore();\n"
"c4.fillRect(0, 0, 30, 30);\n"
"c4.strokeStyle = '#ffff00';\n"
"c4.lineWidth = 4;\n"
"c4.beginPath();\n"
"c4.moveTo(10, 60);\n"
"c4.lineTo(100, 60);\n"
"c4.stroke();\n"
"\n"
"var c5 = hnGet2D('cv5');\n"
"c5.fillStyle = '#ffffff';\n"
"c5.font = '14px sans-serif';\n"
"c5.fillText('CPU', 10, 40);\n"
"</script>"
"</body></html>";

/* ---- 显示列表区域扫描: 按 CLIP_PUSH(画布盒) 找到区域边界 ---- */
typedef struct { int begin, end; } region;

static region find_clip_region(const hn_display_list *dl, float x, float y) {
    region r = { -1, -1 };
    for (int i = 0; i < dl->count; i++) {
        const hn_cmd *c = &dl->cmds[i];
        if (c->kind == HN_CMD_CLIP_PUSH && c->x == x && c->y == y) { r.begin = i; break; }
    }
    if (r.begin < 0) return r;
    int depth = 0;
    for (int i = r.begin; i < dl->count; i++) {
        if (dl->cmds[i].kind == HN_CMD_CLIP_PUSH) depth++;
        else if (dl->cmds[i].kind == HN_CMD_CLIP_POP) {
            depth--;
            if (depth == 0) { r.end = i; break; }
        }
    }
    return r;
}

int main(void) {
    printf("canvas_probe — <canvas> 2D 账本全链路(脚本自动执行→落账→重放→位图)\n\n");

    hn_rt_desc d = { "canvas-probe", DOC_HTML, strlen(DOC_HTML), NULL, 0, 640, 620 };
    hn_rt *rt = hn_rt_open(&d);
    check(rt != NULL, "hn_rt_open 成功(文档 + 内联 <script> 自动执行)");
    if (!rt) return 1;
    hn_context *c = hn_rt_context(rt);
    check(c != NULL, "hn_rt_context 就绪");
    const hn_display_list *dl = hn_context_display_list(c);
    check(dl != NULL && dl->count > 0, "display list 非空");
    if (!dl) { hn_rt_close(rt); return 1; }

    /* == 1. CTM: translate(10,5) 后 fillRect(0,0,50,30) → 实际 (10,5,50,30) ==
       (画布盒在 (0,0), 元素映射 1:1 → 显示列表坐标即账本坐标) */
    printf("== CTM 与 fill_rect ==\n");
    region cv = find_clip_region(dl, 0, 0);
    check(cv.begin >= 0, "cv 区域有 CLIP_PUSH(账本存在才入裁剪)");
    int found = 0;
    hn_color fill = 0;
    float rx = 0, ry = 0, rw = 0, rh = 0;
    for (int i = cv.begin; i <= cv.end && !found; i++) {
        const hn_cmd *cmd = &dl->cmds[i];
        if (cmd->kind == HN_CMD_RECT && cmd->w == 50 && cmd->h == 30) {
            found = 1;
            rx = cmd->x; ry = cmd->y; rw = cmd->w; rh = cmd->h; fill = cmd->fill;
        }
    }
    check(found, "fill_rect 翻译为 RECT 指令");
    check_near(rx, 10, 0.01f, "CTM 平移 x: fillRect(0,0) → x=10");
    check_near(ry, 5, 0.01f, "CTM 平移 y: fillRect(0,0) → y=5");
    check_near(rw, 50, 0.01f, "宽 50");
    check_near(rh, 30, 0.01f, "高 30");
    check_color(fill, 0x3366FFFF, "填充色 #3366ff(RGBA 打包)");

    /* == 2. 折线路径: moveTo/lineTo/arc(15° 折线化)/closePath → fill ==
       点数: moveTo+lineTo 2 点 + arc 起点 1 点 + 24 段折线(全圆 360°/15°)
       + closePath 重复起点 = 28 点。首点 = 画布 (10,10), arc 起点 = (70,50)。 */
    printf("== 折线路径(moveTo/lineTo/arc → fill) ==\n");
    region cv2 = find_clip_region(dl, 0, 120);
    check(cv2.begin >= 0, "cv2 区域有 CLIP_PUSH");
    int polys = 0;
    float p0x = 0, p0y = 0, pax = 0, pay = 0;
    int pn = 0;
    for (int i = cv2.begin; i <= cv2.end; i++) {
        const hn_cmd *cmd = &dl->cmds[i];
        if (cmd->kind == HN_CMD_POLYGON) {
            polys++;
            if (polys == 1) {
                pn = cmd->poly_n;
                if (cmd->poly && cmd->poly_n >= 5) {
                    p0x = cmd->poly[0]; p0y = cmd->poly[1];
                    pax = cmd->poly[4]; pay = cmd->poly[5];
                }
            }
        }
    }
    check_int(polys, 1, "fill 产出 1 条闭合多边形");
    check_int(pn, 28, "点数 = 2(moveTo/lineTo)+1(arc 起点)+24(全圆 15° 折线)+1(close)");
    check_near(p0x, 10, 0.01f, "多边形首点 x = moveTo x");
    check_near(p0y, 130, 0.01f, "多边形首点 y = 画布 10 + 盒 120");
    check_near(pax, 70, 0.01f, "arc 折线起点 x = 圆心 50 + 半径 20");
    check_near(pay, 170, 0.01f, "arc 折线起点 y = 圆心 50 + 盒 120");

    /* == 3. 渐变 fillRect → grect 指令两端色非零 == */
    printf("== 线性渐变 ==\n");
    region cv3 = find_clip_region(dl, 0, 240);
    check(cv3.begin >= 0, "cv3 区域有 CLIP_PUSH");
    int grads = 0;
    hn_color g0 = 0, g1 = 0;
    for (int i = cv3.begin; i <= cv3.end; i++) {
        const hn_cmd *cmd = &dl->cmds[i];
        if (cmd->kind == HN_CMD_RECT && cmd->gradient) {
            grads++;
            g0 = cmd->grad_from;
            g1 = cmd->grad_to;
        }
    }
    check_int(grads, 1, "渐变 fillRect 产出渐变 RECT");
    check((g0 & 0xFFFFFF00u) != 0, "渐变 from 色非零");
    check((g1 & 0xFFFFFF00u) != 0, "渐变 to 色非零");
    check_color(g0, 0xFF0000FF, "渐变 from = #ff0000");
    check_color(g1, 0x0000FFFF, "渐变 to = #0000ff");

    /* == 4. save/restore: restore 后 fillStyle 回退到 #00ff00 ==
       另验 stroke 折线: (10,60)→(100,60) 宽 4 → 1 条 QUAD, y ≈ 420±2 */
    printf("== save/restore 与 stroke 折线 ==\n");
    region cv4 = find_clip_region(dl, 0, 360);
    check(cv4.begin >= 0, "cv4 区域有 CLIP_PUSH");
    hn_color restored = 0;
    int quads = 0;
    float qy_sum = 0;
    for (int i = cv4.begin; i <= cv4.end; i++) {
        const hn_cmd *cmd = &dl->cmds[i];
        if (cmd->kind == HN_CMD_RECT && cmd->w == 30 && cmd->h == 30) restored = cmd->fill;
        if (cmd->kind == HN_CMD_QUAD) {
            quads++;
            qy_sum = (cmd->qy[0] + cmd->qy[1] + cmd->qy[2] + cmd->qy[3]) / 4.0f;
        }
    }
    check_color(restored, 0x00FF00FF, "restore 后 fillStyle 回退(save 内的 #ff0000 被弹出)");
    check(quads >= 1, "stroke 折线产出 QUAD(逐段展开)");
    check_near(qy_sum, 420, 2.1f, "折线 y = 画布 60 + 盒 360(±半宽 2)");

    /* == 5. fillText → TEXT 指令(基线 y = 画布 y) == */
    printf("== fillText ==\n");
    region cv5 = find_clip_region(dl, 0, 480);
    check(cv5.begin >= 0, "cv5 区域有 CLIP_PUSH");
    int texts = 0;
    float tx = 0, bl = 0, fpx = 0;
    char tbuf[64] = { 0 };
    for (int i = cv5.begin; i <= cv5.end; i++) {
        const hn_cmd *cmd = &dl->cmds[i];
        if (cmd->kind == HN_CMD_TEXT && cmd->text) {
            texts++;
            size_t n = cmd->text_len < sizeof(tbuf) - 1 ? cmd->text_len : sizeof(tbuf) - 1;
            memcpy(tbuf, cmd->text, n);
            tbuf[n] = 0;
            tx = cmd->tx; bl = cmd->baseline; fpx = cmd->font.size_px;
        }
    }
    check(texts >= 1, "TEXT 指令存在");
    check(strstr(tbuf, "CPU") != NULL, "文本内容含 CPU");
    check_near(tx, 10, 0.01f, "文字 x = 10");
    check_near(bl, 520, 0.01f, "基线 y = 画布 40 + 盒 480");
    check_near(fpx, 14, 0.01f, "字号 = 14px(font 解析)");

    /* == 6. CLIP 配平 == */
    printf("== 裁剪配平 ==\n");
    int pushes = 0, pops = 0, unbalanced = 0;
    for (int i = 0; i < dl->count; i++) {
        if (dl->cmds[i].kind == HN_CMD_CLIP_PUSH) pushes++;
        if (dl->cmds[i].kind == HN_CMD_CLIP_POP) {
            pops++;
            if (pops > pushes) unbalanced = 1;
        }
    }
    check_int(unbalanced, 0, "CLIP 不下溢");
    check_int(pushes, 5, "五块有账本的画布各一次 CLIP_PUSH(cv6 留空 → 无裁剪)");

    /* == 7. 引擎内省: 账本 op 数与溢出标志 == */
    printf("== 账本内省 ==\n");
    check_int(hn_canvas_count(c, "cv"), 1, "cv 账本 1 op(fillRect)");
    check_int(hn_canvas_count(c, "cv2"), 1, "cv2 账本 1 op(fill)");
    check_int(hn_canvas_count(c, "cv4"), 2, "cv4 账本 2 op(fillRect + stroke)");
    check_int(hn_canvas_count(c, "nope"), -1, "未知画布 count = -1");
    check_int(hn_canvas_overflow(c, "cv"), 0, "cv 未溢出");

    /* == 8. C 直录路径 + 重排版不丢账 ==
       hn_canvas_record(引擎公开 API)不经 JS: cv6 记一条 frect → reflow →
       指令出现; 且 cv 的脚本 op 在重排版后仍在(账本属于元素生命周期)。 */
    printf("== C 直录 + 重排版持久 ==\n");
    int rec = hn_canvas_record(c,
        "{\"cv\":\"cv6\",\"op\":\"frect\",\"x\":5,\"y\":4,\"w\":40,\"h\":10,\"col\":4278255615}",
        strlen("{\"cv\":\"cv6\",\"op\":\"frect\",\"x\":5,\"y\":4,\"w\":40,\"h\":10,\"col\":4278255615}"));
    check(rec == 1, "hn_canvas_record 直录成功(op 数 = 1)");
    check(hn_rt_reflow(rt) == 1, "重排版");
    dl = hn_context_display_list(c);
    region cv6 = find_clip_region(dl, 0, 600);
    check(cv6.begin >= 0, "直录后 cv6 区域出现 CLIP_PUSH");
    found = 0;
    hn_color rec_col = 0;
    float r6x = 0, r6y = 0;
    for (int i = cv6.begin; i <= cv6.end && !found; i++) {
        const hn_cmd *cmd = &dl->cmds[i];
        if (cmd->kind == HN_CMD_RECT && cmd->w == 40 && cmd->h == 10) {
            found = 1;
            r6x = cmd->x; r6y = cmd->y; rec_col = cmd->fill;
        }
    }
    check(found, "C 直录 frect 翻译为 RECT");
    check_near(r6x, 5, 0.01f, "直录 x = 5");
    check_near(r6y, 604, 0.01f, "直录 y = 4 + 盒 600");
    check_color(rec_col, 0xFF00FFFF, "直录色 0xFF00FFFF");
    region cv_again = find_clip_region(dl, 0, 0);
    check(cv_again.begin >= 0, "重排版后 cv 脚本账本仍在(不随样式重算丢失)");

    /* == 9. 账本上限: 4096 op, 满则丢弃并置溢出标志; clear 归零 == */
    printf("== 账本上限与 clear ==\n");
    char opbuf[128];
    for (int i = 0; i < 4200; i++) {
        int n = snprintf(opbuf, sizeof(opbuf),
                         "{\"cv\":\"cv7\",\"op\":\"frect\",\"x\":%d,\"y\":0,\"w\":1,\"h\":1,\"col\":255}", i);
        hn_canvas_record(c, opbuf, (size_t)n);
    }
    check_int(hn_canvas_count(c, "cv7"), 4096, "账本上限 4096(多出的被丢弃)");
    check_int(hn_canvas_overflow(c, "cv7"), 1, "溢出标志置位");
    check_int(hn_canvas_clear(c, "cv7"), 4096, "clear 返回清掉的条数");
    check_int(hn_canvas_count(c, "cv7"), 0, "clear 后账本为空");
    check_int(hn_canvas_overflow(c, "cv7"), 0, "clear 重置溢出标志");

    /* == 10. 原生桥直调(hnCanvas2D 返回 op 数; 探针经 eval 验协议) == */
    printf("== hnCanvas2D 桥协议 ==\n");
    char *r = hn_rt_eval("hnCanvas2D('{\"cv\":\"cvx\",\"op\":\"cpop\"}')", "canvas-probe-x", hn_rt_doc(rt));
    check(r != NULL && strcmp(r, "1") == 0, "hnCanvas2D 返回记录后 op 数(1)");
    free(r);
    r = hn_rt_eval("hnCanvas2D('{\"cv\":\"cvx\",\"op\":\"clear\"}')", "canvas-probe-x", hn_rt_doc(rt));
    check(r != NULL && strcmp(r, "1") == 0, "clear op 返回清掉的条数");
    free(r);
    r = hn_rt_eval("hnCanvas2D('not-json')", "canvas-probe-x", hn_rt_doc(rt));
    check(r != NULL && strcmp(r, "-1") == 0, "坏 JSON 返回 -1");
    free(r);

    /* == 11. 像素采样: hnsoft 内存位图 = 所填颜色(BGRA 预乘) == */
    printf("== 像素采样 ==\n");
    const unsigned char *px = hn_rt_pixels(rt);
    check(px != NULL, "位图就绪(预乘 BGRA)");
    if (px) {
        int W = hn_rt_width(rt);
        long o1 = ((long)20 * W + 35) * 4;      /* cv fill_rect 中心 (35,20) */
        check(px[o1] == 0xFF && px[o1 + 1] == 0x66 && px[o1 + 2] == 0x33,
              "fill_rect 中心像素 = #3366ff(BGRA 顺序)");
        long o2 = ((long)375 * W + 15) * 4;     /* cv4 save/restore 矩形中心 (15,375) */
        check(px[o2] == 0x00 && px[o2 + 1] == 0xFF && px[o2 + 2] == 0x00,
              "restore 后矩形像素 = #00ff00(与显示列表一致)");
        long o3 = ((long)280 * W + 20) * 4;     /* cv3 渐变近 from 端 (20,280) */
        check(px[o3 + 2] > 200 && px[o3] < 60,
              "渐变 from 端偏红(R>200, B<60)");
        long o4 = ((long)280 * W + 95) * 4;     /* cv3 渐变近 to 端 (95,280) */
        check(px[o4] > 200 && px[o4 + 2] < 60,
              "渐变 to 端偏蓝(B>200, R<60)");
    }

    hn_rt_close(rt);

    printf("\n结果: %d 通过, %d 失败\n", g_pass, g_fail);
    if (g_fail == 0) printf("全绿 — canvas 2D 账本链路健康\n");
    return g_fail ? 1 : 0;
}

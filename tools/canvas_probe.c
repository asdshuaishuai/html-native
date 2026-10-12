/* canvas_probe.c — <canvas> 2D 账本全链路探针(确定性, 退出码判定)
 *
 * 管线与生产同一条: hn_rt_open(解析→布局→脚本自动执行→绘制) →
 * QuickJS glue(hnGet2D 状态机) → hnCanvas2D 落账 → hn_paint 重放 →
 * hnsoft 位图。断言分五层:
 *   1. 显示列表形态: op 翻译正确(CTM 后的 RECT/折线 POLYGON/渐变/TEXT/QUAD)
 *   2. 引擎内省: hn_canvas_count / overflow / C 直录路径
 *   3. 账本纪律: 重排版不丢账、4096 上限溢出标志、clear 归零
 *   4. 像素采样: hnsoft 位图中心像素 = 所填颜色(BGRA 预乘)
 *   5. 补全特性: radial/多 stop 渐变、fill() 带渐变、clearRect 局部、
 *      曲线折线化、measureText、lineDash、textAlign/Baseline
 *
 * 用法(py 薄封装编好即跑; 手工编译见 tools/canvas_probe.py):
 *   bash tools/tools/canvas_probe.sh
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

/* ---- 探针文档: 十二块 200x120 画布纵向排开(body margin 0, display:block)
 *  各画布盒子(绝对坐标, 由布局保证):
 *   cv  (0,0)     CTM + fillRect          cv2  (0,120)  折线路径 fill
 *   cv3 (0,240)   渐变 fillRect           cv4  (0,360)  save/restore + stroke 折线
 *   cv5 (0,480)   fillText                cv6  (0,600)  留空(C 直录路径用)
 *   cv7 (0,720)   bezier/quadratic 曲线   cv8  (0,840)  radial 渐变
 *   cv9 (0,960)   多 stop 线性渐变        cv10 (0,1080) fill() 带渐变(gpath)
 *   cv11(0,1200)  clearRect 局部清空      cv12 (0,1320) lineDash + textAlign */
static const char *DOC_HTML =
"<html><head><style>body{margin:0}canvas{display:block}</style></head>"
"<body>"
"<canvas id=\"cv\" width=\"200\" height=\"120\"></canvas>"
"<canvas id=\"cv2\" width=\"200\" height=\"120\"></canvas>"
"<canvas id=\"cv3\" width=\"200\" height=\"120\"></canvas>"
"<canvas id=\"cv4\" width=\"200\" height=\"120\"></canvas>"
"<canvas id=\"cv5\" width=\"200\" height=\"120\"></canvas>"
"<canvas id=\"cv6\" width=\"200\" height=\"120\"></canvas>"
"<canvas id=\"cv7\" width=\"200\" height=\"120\"></canvas>"
"<canvas id=\"cv8\" width=\"200\" height=\"120\"></canvas>"
"<canvas id=\"cv9\" width=\"200\" height=\"120\"></canvas>"
"<canvas id=\"cv10\" width=\"200\" height=\"120\"></canvas>"
"<canvas id=\"cv11\" width=\"200\" height=\"120\"></canvas>"
"<canvas id=\"cv12\" width=\"200\" height=\"120\"></canvas>"
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
"\n"
"var c7 = hnGet2D('cv7');\n"
"c7.beginPath();\n"
"c7.moveTo(20, 20);\n"
"c7.bezierCurveTo(80, 0, 120, 100, 180, 60);\n"
"c7.quadraticCurveTo(100, 140, 20, 100);\n"
"c7.closePath();\n"
"c7.fillStyle = '#ff00aa';\n"
"c7.fill();\n"
"\n"
"var c8 = hnGet2D('cv8');\n"
"var rg = c8.createRadialGradient(60, 40, 0, 60, 40, 50);\n"
"rg.addColorStop(0, '#ff2040');\n"
"rg.addColorStop(1, '#2040ff');\n"
"c8.fillStyle = rg;\n"
"c8.fillRect(10, 10, 100, 60);\n"
"\n"
"var c9 = hnGet2D('cv9');\n"
"var lg = c9.createLinearGradient(0, 10, 0, 70);\n"
"lg.addColorStop(0, '#ff0000');\n"
"lg.addColorStop(0.5, '#00ff00');\n"
"lg.addColorStop(1, '#0000ff');\n"
"c9.fillStyle = lg;\n"
"c9.fillRect(10, 10, 100, 60);\n"
"\n"
"var c10 = hnGet2D('cv10');\n"
"var pg = c10.createLinearGradient(10, 0, 110, 0);\n"
"pg.addColorStop(0, '#ff0000');\n"
"pg.addColorStop(1, '#0000ff');\n"
"c10.fillStyle = pg;\n"
"c10.beginPath();\n"
"c10.moveTo(60, 10);\n"
"c10.lineTo(110, 100);\n"
"c10.lineTo(10, 100);\n"
"c10.closePath();\n"
"c10.fill();\n"
"\n"
"var c11 = hnGet2D('cv11');\n"
"c11.fillStyle = '#00aa44';\n"
"c11.fillRect(0, 0, 200, 120);\n"
"c11.clearRect(60, 40, 80, 40);\n"
"\n"
"var c12 = hnGet2D('cv12');\n"
"c12.setLineDash([10, 6]);\n"
"c12.strokeStyle = '#00ccff';\n"
"c12.lineWidth = 3;\n"
"c12.beginPath();\n"
"c12.moveTo(10, 40);\n"
"c12.lineTo(190, 40);\n"
"c12.stroke();\n"
"c12.setLineDash([]);\n"
"c12.fillStyle = '#ffffff';\n"
"c12.font = '20px sans-serif';\n"
"c12.textAlign = 'center';\n"
"c12.fillText('MM', 100, 90);\n"
"c12.textAlign = 'left';\n"
"c12.textBaseline = 'top';\n"
"c12.fillText('T', 150, 100);\n"
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

/* BGRA 预乘像素取样(不透明色即 RGB) */
static const unsigned char *g_px = NULL;
static int g_w = 0;

static int pix_r(int x, int y) { return g_px[((long)y * g_w + x) * 4 + 2]; }
static int pix_g(int x, int y) { return g_px[((long)y * g_w + x) * 4 + 1]; }
static int pix_b(int x, int y) { return g_px[((long)y * g_w + x) * 4]; }

int main(void) {
    printf("canvas_probe — <canvas> 2D 账本全链路(脚本自动执行→落账→重放→位图)\n\n");

    hn_rt_desc d = { "canvas-probe", DOC_HTML, strlen(DOC_HTML), NULL, 0, 640, 1440 };
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

    /* == 3. 渐变 fillRect → 显式渐变 RECT(轴端绝对坐标 + stops 数组) == */
    printf("== 线性渐变(显式轴 + stops) ==\n");
    region cv3 = find_clip_region(dl, 0, 240);
    check(cv3.begin >= 0, "cv3 区域有 CLIP_PUSH");
    int grads = 0;
    hn_color g0 = 0, g1 = 0;
    float ax0 = 0, ay0 = 0, ax1 = 0, ay1 = 0;
    int gkind = 0, gstops = 0;
    for (int i = cv3.begin; i <= cv3.end; i++) {
        const hn_cmd *cmd = &dl->cmds[i];
        if (cmd->kind == HN_CMD_RECT && (cmd->gradient || cmd->grad_kind)) {
            grads++;
            g0 = cmd->grad_from;
            g1 = cmd->grad_to;
            gkind = cmd->grad_kind;
            gstops = cmd->gn_stops;
            ax0 = cmd->gax0; ay0 = cmd->gay0;
            ax1 = cmd->gax1; ay1 = cmd->gay1;
        }
    }
    check_int(grads, 1, "渐变 fillRect 产出渐变 RECT");
    check((g0 & 0xFFFFFF00u) != 0, "渐变 from 色非零");
    check((g1 & 0xFFFFFF00u) != 0, "渐变 to 色非零");
    check_color(g0, 0xFF0000FF, "渐变 from = #ff0000");
    check_color(g1, 0x0000FFFF, "渐变 to = #0000ff");
    check_int(gkind, 1, "grad_kind = 1(显式线性轴, 非 CSS 盒模型)");
    check_int(gstops, 2, "stops 数 = 2(两端各一)");
    /* 渐变轴独立于填充矩形: 轴端 = createLinearGradient 的坐标经 1:1 映射 */
    check_near(ax0, 0, 0.01f, "渐变轴起点 x = 0(与矩形位置无关)");
    check_near(ax1, 100, 0.01f, "渐变轴终点 x = 100(轴长 = 线段长)");

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

    /* == 5. fillText → TEXT 指令(基线 y = 画布 y; alphabetic 缺省不平移) == */
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

    /* == 6. 曲线族: bezier(24 段)+ quadratic(16 段)折线化 == */
    printf("== 曲线族(bezier/quadratic 折线化) ==\n");
    region cv7 = find_clip_region(dl, 0, 720);
    check(cv7.begin >= 0, "cv7 区域有 CLIP_PUSH");
    polys = 0;
    pn = 0;
    float bminx = 1e9f, bmaxx = -1e9f, bminy = 1e9f, bmaxy = -1e9f;
    for (int i = cv7.begin; i <= cv7.end; i++) {
        const hn_cmd *cmd = &dl->cmds[i];
        if (cmd->kind == HN_CMD_POLYGON) {
            polys++;
            pn = cmd->poly_n;
            for (int k = 0; k < cmd->poly_n; k++) {
                float vx = cmd->poly[k * 2], vy = cmd->poly[k * 2 + 1];
                if (vx < bminx) bminx = vx;
                if (vx > bmaxx) bmaxx = vx;
                if (vy < bminy) bminy = vy;
                if (vy > bmaxy) bmaxy = vy;
            }
        }
    }
    check_int(polys, 1, "曲线 fill 产出 1 条闭合多边形");
    /* 点数 = moveTo 1 + 三次 24 段 + 二次 16 段 + closePath 1 = 42 */
    check_int(pn, 42, "点数 = 1(moveTo)+24(bezier 24 段)+16(quadratic 16 段)+1(close)");
    /* 包围盒(扣掉画布盒偏移 720): 不越过控制凸包(20..180 / 曲线 y 5..141) */
    check(bminx >= 18.0f && bmaxx <= 182.0f, "包围盒 x 在控制凸包内(20..180 ±2)");
    check(bminy - 720.0f >= 5.0f && bmaxy - 720.0f <= 141.0f,
          "包围盒 y 合理(曲线不越过控制凸包)");

    /* == 7. CLIP 配平 == */
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
    check_int(pushes, 11, "十一块有账本的画布各一次 CLIP_PUSH(cv6 留空 → 无裁剪)");

    /* == 8. 引擎内省: 账本 op 数与溢出标志 == */
    printf("== 账本内省 ==\n");
    check_int(hn_canvas_count(c, "cv"), 1, "cv 账本 1 op(fillRect)");
    check_int(hn_canvas_count(c, "cv2"), 1, "cv2 账本 1 op(fill)");
    check_int(hn_canvas_count(c, "cv4"), 2, "cv4 账本 2 op(fillRect + stroke)");
    check_int(hn_canvas_count(c, "cv11"), 2, "cv11 账本 2 op(fillRect + clear_rect)");
    check_int(hn_canvas_count(c, "nope"), -1, "未知画布 count = -1");
    check_int(hn_canvas_overflow(c, "cv"), 0, "cv 未溢出");

    /* == 9. C 直录路径 + 重排版不丢账 ==
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

    /* == 10. 账本上限: 4096 op, 满则丢弃并置溢出标志; clear 归零 == */
    printf("== 账本上限与 clear ==\n");
    char opbuf[128];
    for (int i = 0; i < 4200; i++) {
        int n = snprintf(opbuf, sizeof(opbuf),
                         "{\"cv\":\"cvovf\",\"op\":\"frect\",\"x\":%d,\"y\":0,\"w\":1,\"h\":1,\"col\":255}", i);
        hn_canvas_record(c, opbuf, (size_t)n);
    }
    check_int(hn_canvas_count(c, "cvovf"), 4096, "账本上限 4096(多出的被丢弃)");
    check_int(hn_canvas_overflow(c, "cvovf"), 1, "溢出标志置位");
    check_int(hn_canvas_clear(c, "cvovf"), 4096, "clear 返回清掉的条数");
    check_int(hn_canvas_count(c, "cvovf"), 0, "clear 后账本为空");
    check_int(hn_canvas_overflow(c, "cvovf"), 0, "clear 重置溢出标志");

    /* == 11. 原生桥直调(hnCanvas2D 返回 op 数; 探针经 eval 验协议) ==
       另验 clear_rect 覆盖整画布 → 退化为清账(cv5 的 1 条 text op 被清空)。 */
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
    r = hn_rt_eval("hnGet2D('cv5').clearRect(0,0,200,120)", "canvas-probe-x", hn_rt_doc(rt));
    check(r != NULL, "全画布 clearRect 调用成功");
    free(r);
    check_int(hn_canvas_count(c, "cv5"), 0, "全画布 clearRect = 清账(op 有界, 动画循环口径)");

    /* == 12. measureText(原生桥: 宽 >0 且与字号正相关) == */
    printf("== measureText ==\n");
    r = hn_rt_eval("hnGet2D('mx').font='14px sans-serif'; hnGet2D('mx').measureText('ABC').width",
                   "canvas-probe-x", hn_rt_doc(rt));
    check(r != NULL, "measureText 返回宽度");
    float w14 = r ? (float)atof(r) : 0;
    free(r);
    r = hn_rt_eval("hnGet2D('mx').font='28px sans-serif'; hnGet2D('mx').measureText('ABC').width",
                   "canvas-probe-x", hn_rt_doc(rt));
    float w28 = r ? (float)atof(r) : 0;
    free(r);
    check(w14 > 0, "14px 宽度 > 0");
    check(w28 > w14 * 1.4f, "28px 宽度与字号正相关(> 14px 宽 × 1.4)");

    /* == 13. 像素采样: hnsoft 内存位图 = 所填颜色(BGRA 预乘) == */
    printf("== 像素采样 ==\n");
    g_px = hn_rt_pixels(rt);
    check(g_px != NULL, "位图就绪(预乘 BGRA)");
    if (g_px) {
        g_w = hn_rt_width(rt);
        check(pix_b(35, 20) == 0xFF && pix_g(35, 20) == 0x66 && pix_r(35, 20) == 0x33,
              "fill_rect 中心像素 = #3366ff(BGRA 顺序)");
        check(pix_b(15, 375) == 0x00 && pix_g(15, 375) == 0xFF && pix_r(15, 375) == 0x00,
              "restore 后矩形像素 = #00ff00(与显示列表一致)");
        check(pix_r(20, 280) > 200 && pix_b(20, 280) < 60,
              "渐变 from 端偏红(R>200, B<60)");
        check(pix_b(95, 280) > 200 && pix_r(95, 280) < 60,
              "渐变 to 端偏蓝(B>200, R<60)");

        /* -- radial 渐变(cv8 @840): 中心=内色, 边缘=外色, 中环≈插值 -- */
        printf("== radial 渐变像素 ==\n");
        check(pix_r(60, 880) > 200 && pix_b(60, 880) < 120,
              "radial 中心 = 内色(偏红)");
        check(pix_b(105, 880) > 200 && pix_r(105, 880) < 120,
              "radial 外缘 = 外色(偏蓝)");
        {
            int pr = pix_r(85, 880), pb = pix_b(85, 880);
            check(pr > 100 && pr < 210 && pb > 100 && pb < 210,
                  "radial 中环 = 两色插值(R/B 均在中段)");
        }

        /* -- 多 stop 线性(cv9 @960): 三 stop 中点≈中间色 -- */
        printf("== 多 stop 线性渐变像素 ==\n");
        check(pix_r(60, 975) > 180, "多 stop 顶部偏红");
        check(pix_g(60, 1000) > 200 && pix_r(60, 1000) < 60 && pix_b(60, 1000) < 60,
              "多 stop 中点 = 纯中间色(#00ff00)");
        check(pix_b(60, 1025) > 180, "多 stop 底部偏蓝");

        /* -- fill() 带渐变(cv10 @1080): 不再回落纯色 -- */
        printf("== fill() 带渐变像素 ==\n");
        {
            int lr = pix_r(30, 1150), lb = pix_b(30, 1150);
            int rr = pix_r(90, 1150), rb = pix_b(90, 1150);
            check(lr > 150 && lb < 110, "gpath 左端偏红(R>150)");
            check(rb > 150 && rr < 110, "gpath 右端偏蓝(B>150)");
            check(lr > rr, "gpath 红通道沿轴单调递减(非纯色回落)");
        }

        /* -- clearRect 局部(cv11 @1200): 外保留、内清空 -- */
        printf("== clearRect 局部像素 ==\n");
        check(pix_g(20, 1215) > 140 && pix_r(20, 1215) < 60,
              "清空矩形外 = 保留的绿色填充");
        check(pix_r(100, 1260) < 40 && pix_g(100, 1260) < 40 && pix_b(100, 1260) < 40,
              "清空矩形内 = 渲染底色(深色, 内容被清)");

        /* -- lineDash(cv12 @1320): 虚线开段着色、闭段露出底色 --
           线从 x=10 起, 模 [10,6]: 开段 10..20, 闭段 20..26(画布坐标) */
        printf("== lineDash / textAlign 像素 ==\n");
        check(pix_g(15, 1360) > 140 && pix_b(15, 1360) > 180 && pix_r(15, 1360) < 80,
              "虚线开段着色(#00ccff)");
        check(pix_r(23, 1360) < 40 && pix_g(23, 1360) < 40,
              "虚线闭段露出渲染底色");
    }

    /* == 14. 显示列表: lineDash 切段与 textAlign 锚点 == */
    printf("== lineDash 切段与 textAlign/textBaseline ==\n");
    region cv12 = find_clip_region(dl, 0, 1320);
    check(cv12.begin >= 0, "cv12 区域有 CLIP_PUSH");
    int spaths = 0;
    texts = 0;
    float mmx = 0, tbx = 0, tbl = 0;
    char t12[2][8] = { { 0 }, { 0 } };
    for (int i = cv12.begin; i <= cv12.end; i++) {
        const hn_cmd *cmd = &dl->cmds[i];
        if (cmd->kind == HN_CMD_QUAD) spaths++;   /* stroke 折线逐段 QUAD */
        if (cmd->kind == HN_CMD_TEXT && cmd->text && cmd->text_len < 8) {
            memcpy(t12[texts], cmd->text, cmd->text_len);
            if (texts == 0) mmx = cmd->tx;
            else { tbx = cmd->tx; tbl = cmd->baseline; }
            texts++;
        }
    }
    /* 180px 线 / [10,6] 模 = 12 个虚线段 → 12 条 spath → 12 组 QUAD */
    check(spaths >= 12, "虚线切段产出 12 条描边(180px / 16px 模)");
    check(texts >= 2, "两条 TEXT 指令(MM 与 T)");
    check(mmx > 40 && mmx < 100, "textAlign=center 锚点左移(100 - 宽/2)");
    check_near(tbx, 150, 0.01f, "textAlign=left 锚点不平移");
    check_near(tbl, 1436, 3.0f, "textBaseline=top 基线 = 100+0.8×20 + 盒 1320");

    hn_rt_close(rt);

    printf("\n结果: %d 通过, %d 失败\n", g_pass, g_fail);
    if (g_fail == 0) printf("全绿 — canvas 2D 账本链路健康\n");
    return g_fail ? 1 : 0;
}

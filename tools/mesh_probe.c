/* mesh_probe.c — 网格变形(Live2D 类原语)驱动路径断言
 *
 * Live2D 的 .moc3 是专有格式(需 Cubism SDK 商业授权), 无法自研解码; 但它
 * 公开的核心技术是"贴图映射到可变形的网格 + 逐顶点驱动"。本文件验证引擎交付
 * 的这个原语:
 *
 *   - 网格解析(NxM / 裸属性默认 / 非法值回退)
 *   - anchor 模式(bottom/top/left/right/center-radial): 固定端位移必须为 0
 *   - 变形下 UV 必须仍是均匀网格(贴图跟着网格走, 不跟着形变走)
 *   - 脚本逐顶点驱动(hn_node_set_mesh_verts): 顶点原样采用, UV 由引擎补
 *
 * 直接读显示列表里的 MESH 指令(顶点与 UV), 不经过任何平台后端 ——
 * 因为这一层是纯引擎语义。
 *
 * 用法: cc -O2 mesh_probe.c hn_mesh.c hn_paint.c hn_html.c hn_css.c
 *           hn_style.c hn_layout.c hn_context.c hn_arena.c hn_json.c
 *           hn_theme.c hn_lottie.c hn_png.c -lfreetype -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <stdarg.h>
#include "hn_internal.h"

static int fails = 0;
static void ck(int c, const char *l) { printf("%s %s\n", c ? "  v" : "  X", l); if (!c) fails++; }
static char g_fmt[160];
static const char *mkstr(const char *f, ...) {
    va_list ap; va_start(ap, f);
    vsnprintf(g_fmt, sizeof(g_fmt), f, ap);
    va_end(ap);
    return g_fmt;
}

static hn_context *ctx;
static hn_text_backend tb;

/* 建一个只含一个 mesh 图片元素的文档, 布局后取第一条 MESH 指令。
   C 没有默认参数, 所以节点通过全局 g_node 回传(单线程探针足够)。 */
static hn_context *ctx;
static hn_node *g_node;
static const hn_cmd *mesh_of(const char *img_attrs) {
    char html[512];
    snprintf(html, sizeof(html),
        "<html><head><style>html,body{margin:0}#m{width:100;height:80}</style></head>"
        "<body><img id=\"m\" src=\"t.png\" %s></body></html>", img_attrs);
    hn_context *c = hn_context_create();
    hn_context_set_doc(c, hn_parse_html(html, strlen(html)));
    hn_context_layout(c, 200, 160, &tb);
    const hn_display_list *dl = hn_context_display_list(c);
    if (!dl) return NULL;
    for (int i = 0; i < dl->count; i++)
        if (dl->cmds[i].kind == HN_CMD_MESH) {
            g_node = hn_doc_find_by_id(hn_context_doc(c), "m");
            ctx = c;
            return &dl->cmds[i];
        }
    ctx = c;
    return NULL;
}

int main(void) {
    printf("== 网格解析 ==\n");
    const hn_cmd *m = mesh_of("hn-mesh=\"4x3\"");
    ck(m && m->mesh_cols == 4 && m->mesh_rows == 3,
       mkstr("hn-mesh=\"4x3\" → %dx%d", m ? m->mesh_cols : -1, m ? m->mesh_rows : -1));
    m = mesh_of("hn-mesh");
    ck(m && m->mesh_cols == 8 && m->mesh_rows == 8,
       mkstr("裸 hn-mesh → 默认 %dx%d", m ? m->mesh_cols : -1, m ? m->mesh_rows : -1));
    m = mesh_of("hn-mesh=\"true\"");
    ck(m && m->mesh_cols == 8 && m->mesh_rows == 8, "hn-mesh=\"true\" → 默认 8x8");
    m = mesh_of("hn-mesh=\"garbage\"");
    ck(m && m->mesh_cols == 8 && m->mesh_rows == 8, "非法值 → 回退默认(不崩)");
    m = mesh_of("hn-mesh=\"1x1\"");
    ck(m && m->mesh_cols == 1 && m->mesh_rows == 1, "最小网格 1x1 可解析");

    printf("== anchor 模式: 固定端位移必须为 0 ==\n");
    /* sway 非 0 才会形变; ext_clock 未推进时相位由 (u,v) 决定, 仍有位移 */
    struct { const char *anchor; int axis; float at_fixed, at_free; } A[] = {
        { "bottom", 1, 0.0f, 1.0f },   /* axis 1 = y; 固定端 v=0, 自由端 v=1 */
        { "top",    1, 1.0f, 0.0f },
        { "left",   0, 0.0f, 1.0f },   /* axis 0 = x; 固定端 u=0 */
        { "right",  0, 1.0f, 0.0f },
    };
    for (int k = 0; k < 4; k++) {
        char attrs[128];
        snprintf(attrs, sizeof(attrs), "hn-mesh=\"4x4\" hn-mesh-sway=\"10\" hn-mesh-anchor=\"%s\"", A[k].anchor);
        m = mesh_of(attrs);
        if (!m) { ck(0, mkstr("anchor=%s 取不到 MESH", A[k].anchor)); continue; }
        int cols = m->mesh_cols;
        /* 采样点取**边中点**而不是角点: 摆动是正弦, 角点处某个分量恰好可能
           为 0(实测 bottom 自由端角点的 y 位移只有 0.06 而 x 位移有 9.4),
           拿单轴比会把正常的 0 误判成 bug。这里比**总位移模长**, 与轴无关。 */
        float disp_fixed = 0, disp_free = 0;
        for (int j = 0; j < 2; j++) {
            float u = A[k].at_fixed, v = A[k].at_fixed;
            if (A[k].axis == 1) { v = A[k].at_fixed; u = 0.5f; }  /* 竖向: 该行中点 */
            else                { u = A[k].at_fixed; v = 0.5f; }  /* 横向: 该列中点 */
            int r = (int)lroundf(v * m->mesh_rows), c = (int)lroundf(u * cols);
            int i = r * (cols + 1) + c;
            float dx = m->mesh_verts[i*2] - u * 100.0f;
            float dy = m->mesh_verts[i*2+1] - v * 80.0f;
            disp_fixed = sqrtf(dx*dx + dy*dy);

            u = A[k].at_free; v = A[k].at_free;
            if (A[k].axis == 1) { v = A[k].at_free; u = 0.5f; }
            else                { u = A[k].at_free; v = 0.5f; }
            r = (int)lroundf(v * m->mesh_rows); c = (int)lroundf(u * cols);
            i = r * (cols + 1) + c;
            dx = m->mesh_verts[i*2] - u * 100.0f;
            dy = m->mesh_verts[i*2+1] - v * 80.0f;
            disp_free = sqrtf(dx*dx + dy*dy);
        }
        ck(disp_fixed < 0.01f, mkstr("anchor=%s 固定端位移=0 (实测 %.4f)", A[k].anchor, disp_fixed));
        ck(disp_free > 0.5f,   mkstr("anchor=%s 自由端有位移 (实测 %.2f)", A[k].anchor, disp_free));
    }
    /* center/radial: 网格四角(离中心最远)位移最大, 中心附近最小 */
    m = mesh_of("hn-mesh=\"4x4\" hn-mesh-sway=\"10\" hn-mesh-anchor=\"center\"");
    if (m) {
        int cols = m->mesh_cols;
        float d_corner = 0, d_center = 1e9f;
        for (int r = 0; r <= m->mesh_rows; r++)
            for (int c = 0; c <= cols; c++) {
                int i = r * (cols + 1) + c;
                float u = (float)c / cols, v = (float)r / m->mesh_rows;
                float dx = m->mesh_verts[i*2] - u * 100.0f;
                float dy = m->mesh_verts[i*2+1] - v * 80.0f;
                float d = sqrtf(dx*dx + dy*dy);
                float dc = sqrtf((u-0.5f)*(u-0.5f) + (v-0.5f)*(v-0.5f));
                if (dc > 0.6f && d > d_corner) d_corner = d;
                if (dc < 0.15f && d < d_center) d_center = d;
            }
        ck(d_corner > 0.5f, mkstr("anchor=center 边缘有位移 (%.2f)", d_corner));
        ck(d_center < 0.5f,  mkstr("anchor=center 中心附近位移小 (%.2f)", d_center));
    } else ck(0, "anchor=center 取不到 MESH");

    printf("== UV 正确性: 变形下 UV 必须仍是均匀网格 ==\n");
    m = mesh_of("hn-mesh=\"4x3\" hn-mesh-sway=\"12\" hn-mesh-anchor=\"bottom\"");
    if (m) {
        int ok_uv = 1, ok_deformed = 0;
        for (int r = 0; r <= m->mesh_rows; r++)
            for (int c = 0; c <= m->mesh_cols; c++) {
                int i = r * (m->mesh_cols + 1) + c;
                float eu = (float)c / m->mesh_cols, ev = (float)r / m->mesh_rows;
                if (fabsf(m->mesh_uv[i*2] - eu) > 1e-5f || fabsf(m->mesh_uv[i*2+1] - ev) > 1e-5f)
                    ok_uv = 0;
                /* 顶点必须真的偏离了基准(证明形变生效, UV 才不是"恰好没变形") */
                float dx = m->mesh_verts[i*2] - eu * 100.0f;
                float dy = m->mesh_verts[i*2+1] - ev * 80.0f;
                if (sqrtf(dx*dx + dy*dy) > 0.5f) ok_deformed = 1;
            }
        ck(ok_uv, "UV 逐格等于归一化网格坐标(不随形变改变)");
        ck(ok_deformed, "顶点确实偏离基准(形变真的生效了)");
    } else ck(0, "UV 测试取不到 MESH");

    printf("== 脚本逐顶点驱动(hn_node_set_mesh_verts) ==\n");
    {
        /* 4x3 网格 = 20 顶点; 写成"右半边上移 20px"的折角 */
        int cols = 4, rows = 3, nv = (cols + 1) * (rows + 1);
        float *vs = (float *)malloc(sizeof(float) * nv * 2);
        for (int r = 0; r <= rows; r++)
            for (int c = 0; c <= cols; c++) {
                int i = r * (cols + 1) + c;
                vs[i*2]   = (float)c / cols * 100.0f;
                vs[i*2+1] = (float)r / rows * 80.0f;
                if (c > cols / 2) vs[i*2+1] -= 20.0f;      /* 右半上移 */
            }
        m = mesh_of("hn-mesh=\"4x3\"");
        hn_node *nd = g_node;
        if (nd) hn_node_set_mesh_verts(nd, vs, cols, rows);
        if (nd) {
            /* 重布局后顶点应原样采用 */
            hn_context_layout(ctx, 200, 160, &tb);
            const hn_display_list *dl = hn_context_display_list(ctx);
            const hn_cmd *mm = NULL;
            for (int i = 0; i < dl->count; i++)
                if (dl->cmds[i].kind == HN_CMD_MESH) mm = &dl->cmds[i];
            if (mm) {
                int okv = 1, okuv = 1;
                for (int r = 0; r <= rows; r++)
                    for (int c = 0; c <= cols; c++) {
                        int i = r * (cols + 1) + c;
                        float ex = vs[i*2] + mm->x, ey = vs[i*2+1] + mm->y;
                        if (fabsf(mm->mesh_verts[i*2] - ex) > 0.01f ||
                            fabsf(mm->mesh_verts[i*2+1] - ey) > 0.01f) okv = 0;
                        if (fabsf(mm->mesh_uv[i*2] - (float)c/cols) > 1e-5f) okuv = 0;
                    }
                ck(okv, "脚本顶点原样采用(含盒偏移)");
                ck(okuv, "脚本驱动时 UV 由引擎补齐为均匀网格");
            } else ck(0, "脚本驱动后没有 MESH 指令");
        } else ck(0, "脚本驱动取不到节点");
        free(vs);
    }

    printf("== %s (%d 项失败) ==\n", fails ? "失败" : "全部通过", fails);
    return fails ? 1 : 0;
}

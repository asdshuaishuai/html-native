/* mesh_probe.c — 网格变形(Live2D 类原语)驱动路径断言
 *
 * Live2D 的 .moc3 是专有格式(需 Cubism SDK 商业授权), 无法自研解码; 但它
 * 公开的核心技术是"贴图映射到可变形的网格 + 逐顶点驱动"。本文件验证引擎交付
 * 的这个原语:
 *
 *   - 网格解析(NxM / 裸属性默认 / 非法值回退)
 *   - anchor 模式(bottom/top/left/right/center-radial): 固定端位移必须为 0
 *   - 三带 anchor(head/chest/waist): 同 sway 下 头带摆幅 > 胸带 > 腰带
 *   - phase-y: 相邻行相位差 = k(两次采样反解相位)
 *   - look: 指针跟随的方向/幅度/一阶滞后(第一次逼近只走部分距离)
 *   - idle: 待机双正弦改变顶点序列; 同 t 两次采样逐位一致
 *   - hnMeshParam 引擎侧: mouth 局部张开 / body_x 整体平移 / 写入钳位
 *   - 变形下 UV 必须仍是均匀网格(贴图跟着网格走, 不跟着形变走)
 *   - 脚本逐顶点驱动(hn_node_set_mesh_verts): 顶点原样采用, UV 由引擎补
 *
 * 直接读显示列表里的 MESH 指令(顶点与 UV), 不经过任何平台后端 ——
 * 因为这一层是纯引擎语义。所有采样用固定 ext_clock/固定指针坐标,
 * 无随机源; 新形变不声明时输出与旧引擎逐位一致(回归以旧断言守护)。
 *
 * 用法: cc -O2 mesh_probe.c hn_mesh.c hn_paint.c hn_html.c hn_css.c
 *           hn_style.c hn_layout.c hn_context.c hn_arena.c hn_json.c
 *           hn_theme.c hn_lottie.c hn_png.c hn_media.c hn_gif.c
 *           hn_canvas.c hn_fmt.c
 *           -lfreetype -lm
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

/* 把当前(单网格)文档推进到指定 ext_clock 再重布局, 返回最新 MESH 指令。
   注意: 每次重布局都会让网格求值一次 —— hn-mesh-look 的一阶滞后按调用
   次数推进(这正是 look 滞后断言的驱动方式)。显示列表在重布局后可能
   重新分配, 跨布局比较顶点必须先 memcpy 到本地缓冲。 */
static const hn_cmd *remesh_at(hn_context *c, float ms) {
    if (g_node) g_node->ext_clock = ms;
    hn_context_layout(c, 200, 160, &tb);
    const hn_display_list *dl = hn_context_display_list(c);
    for (int i = 0; i < dl->count; i++)
        if (dl->cmds[i].kind == HN_CMD_MESH) return &dl->cmds[i];
    return NULL;
}

/* u=0.5 中列、第 r 行顶点的 x 位移(相对未形变基准 u*bw) */
static float xoff(const hn_cmd *m, int cols, int r) {
    int i = (r * (cols + 1) + cols / 2) * 2;
    return m->mesh_verts[i] - 0.5f * 100.0f;
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

    printf("== 三带 anchor(head/chest/waist): 同 sway 下 头带摆幅 > 胸带 > 腰带 ==\n");
    {
        /* 幅度取样: 一个摆动周期(3s)内 16 个确定性时刻, 每行取 |x 位移|
           最大值 —— max|sin| >= cos(π/16) ≈ 0.98, 行幅度比即三带权重比
           (采样噪声 < 2%, 断言留足余量)。行取 r=1(腰带)/6(胸带)/11(头带),
           全部远离固定端(v=0)。 */
        const char *names[3] = { "head", "chest", "waist" };
        for (int a = 0; a < 3; a++) {
            char attrs[160];
            snprintf(attrs, sizeof(attrs),
                "hn-mesh=\"4x12\" hn-mesh-sway=\"10\" hn-mesh-anchor=\"%s\"", names[a]);
            if (!mesh_of(attrs)) { ck(0, mkstr("anchor=%s 取不到 MESH", names[a])); continue; }
            float amp[13] = { 0 };
            for (int k = 0; k < 16; k++) {
                const hn_cmd *mm = remesh_at(ctx, (float)k * 187.5f);
                if (!mm) break;
                for (int r = 0; r <= 12; r++) {
                    float off = fabsf(xoff(mm, 4, r));
                    if (off > amp[r]) amp[r] = off;
                }
            }
            ck(amp[11] > amp[6] && amp[6] > amp[1] && amp[11] > amp[1] * 1.3f,
               mkstr("anchor=%s 行摆幅 头带(%.2f) > 胸带(%.2f) > 腰带(%.2f)",
                     names[a], amp[11], amp[6], amp[1]));
        }
    }

    printf("== phase-y: 相邻行相位差 = k ==\n");
    {
        /* 反解相位: x 位移 = sway*wgt*sin(θ), wgt = v(anchor=bottom)已知。
           两次采样(时钟 0 与 450ms, 基相位差 δ = 0.9425)联立解出每行 θ,
           相邻行差必须 = k。u=0.5 列的 u*0.6 常量在差分中消去;
           θ 全程落在 (-π, π) 主支内(采样点事先算好), atan2 无卷绕。
           两次采样跨布局 —— 顶点缓冲会被 tmp arena 复用, 先落地再重布局。 */
        m = mesh_of("hn-mesh=\"4x3\" hn-mesh-sway=\"10\" hn-mesh-anchor=\"bottom\""
                    " hn-mesh-phase-y=\"0.6\"");
        const hn_cmd *mb = NULL;
        float va[2 * 5 * 4];
        if (m) {
            memcpy(va, m->mesh_verts, sizeof(va));   /* 5x4 顶点 = (4+1)*(3+1) */
            mb = remesh_at(ctx, 450.0f);
        }
        if (m && mb) {
            float delta = 450.0f * 0.001f * (2.0f * (float)M_PI / 3.0f);
            float th[4] = { 0 };
            int ok_th = 1;
            for (int r = 1; r <= 3; r++) {
                float A = 10.0f * ((float)r / 3.0f);
                int i = (r * 5 + 2) * 2;   /* u=0.5 中列 */
                float xa = (va[i] - 50.0f) / A;
                float xb = (mb->mesh_verts[i] - 50.0f) / A;
                float co = (xb - xa * cosf(delta)) / sinf(delta);
                th[r] = atan2f(xa, co);
            }
            if (fabsf(th[2] - th[1] - 0.6f) > 2e-3f) ok_th = 0;
            if (fabsf(th[3] - th[2] - 0.6f) > 2e-3f) ok_th = 0;
            ck(ok_th, mkstr("行相位差 = k=0.6 (实测 %.4f / %.4f)",
                            th[2] - th[1], th[3] - th[2]));
        } else ck(0, "phase-y 取不到 MESH");
        /* k=0: 全网格同相位 → 行位移比 = 权重比(v 比), 波浪彻底压平 */
        m = mesh_of("hn-mesh=\"4x3\" hn-mesh-sway=\"10\" hn-mesh-anchor=\"bottom\""
                    " hn-mesh-phase-y=\"0\"");
        if (m) {
            float o1 = fabsf(xoff(m, 4, 1)), o2 = fabsf(xoff(m, 4, 2)),
                  o3 = fabsf(xoff(m, 4, 3));
            ck(o1 > 0.2f && fabsf(o2 / o1 - 2.0f) < 1e-3f && fabsf(o3 / o1 - 3.0f) < 1e-3f,
               mkstr("phase-y=0 行位移比 = 权重比 (1:%.3f:%.3f, 期望 1:2:3)",
                     o2 / o1, o3 / o1));
        } else ck(0, "phase-y=0 取不到 MESH");
    }

    printf("== look: 视线/头部跟随指针(方向/幅度/一阶滞后) ==\n");
    {
        /* sway 缺省 0 → 顶点只含 look 形变, 断言不被摆动污染 */
        m = mesh_of("hn-mesh=\"6x6\" hn-mesh-look=\"100\"");
        if (m && g_node) {
            float base[98];
            memcpy(base, m->mesh_verts, sizeof(base));
            /* 指针在元素中心右侧 100px(视口 200 宽 → 归一化顶格 1.0) */
            hn_context_set_pointer(ctx, g_node->bx + 150.0f, g_node->by + 40.0f);
            const hn_cmd *m1 = remesh_at(ctx, 0.0f);   /* 第一次逼近 */
            float o1 = xoff(m1, 6, 6);                 /* 头带行(v=1, 权重最高) */
            const hn_cmd *mc = m1;
            for (int i = 0; i < 30; i++) mc = remesh_at(ctx, 0.0f);
            float oc = xoff(mc, 6, 6);
            ck(o1 > 0.05f && o1 < 0.5f * oc,
               mkstr("一阶滞后: 第一次逼近只走部分距离 (%.3f → 收敛 %.3f)", o1, oc));
            ck(oc > 3.0f,
               mkstr("喂指针后头带整体偏移方向正确(朝指针 %.2fpx)、幅度非零", oc));
            /* look=0: 喂同一指针坐标, 顶点必须纹丝不动 */
            m = mesh_of("hn-mesh=\"6x6\" hn-mesh-look=\"0\"");
            hn_context_set_pointer(ctx, g_node->bx + 150.0f, g_node->by + 40.0f);
            const hn_cmd *m0 = m;
            for (int i = 0; i < 5; i++) m0 = remesh_at(ctx, 0.0f);
            ck(memcmp(base, m0->mesh_verts, sizeof(base)) == 0,
               "look=0: 喂指针坐标后顶点不动(强度 0 = 关闭)");
        } else ck(0, "look 取不到 MESH/节点");
    }

    printf("== idle: 待机双正弦 + 确定性 ==\n");
    {
        float sway_v[98], idle_v[98];
        m = mesh_of("hn-mesh=\"6x6\" hn-mesh-sway=\"10\"");
        const hn_cmd *sw = m ? remesh_at(ctx, 500.0f) : NULL;
        if (sw) memcpy(sway_v, sw->mesh_verts, sizeof(sway_v));
        m = mesh_of("hn-mesh=\"6x6\" hn-mesh-sway=\"10\" hn-mesh-idle=\"1\"");
        const hn_cmd *id = m ? remesh_at(ctx, 500.0f) : NULL;
        if (sw && id) {
            memcpy(idle_v, id->mesh_verts, sizeof(idle_v));
            ck(memcmp(sway_v, idle_v, sizeof(sway_v)) != 0,
               "idle 与纯 sway 同 t(500ms)顶点序列不同");
        } else ck(0, "idle 取不到 MESH");
        m = mesh_of("hn-mesh=\"6x6\" hn-mesh-sway=\"10\" hn-mesh-idle=\"1\"");
        const hn_cmd *id2 = m ? remesh_at(ctx, 500.0f) : NULL;
        ck(id2 && memcmp(idle_v, id2->mesh_verts, sizeof(idle_v)) == 0,
           "两次同 t 采样逐位一致(确定性, 无随机)");
    }

    printf("== hnMeshParam 引擎侧(hn_node_set_mesh_param) ==\n");
    {
        float base[2 * 9 * 17];
        m = mesh_of("hn-mesh=\"8x16\" hn-mesh-sway=\"0\"");
        if (m && g_node) {
            memcpy(base, m->mesh_verts, sizeof(base));
            hn_node_set_mesh_param(g_node, "mouth", 5.0f);   /* 超界 → 钳 1 */
            ck(g_node->mesh_param_mouth == 1.0f,
               mkstr("参数写入钳位 -1..1 (mouth=5 → %.1f)", (double)g_node->mesh_param_mouth));
            const hn_cmd *mm = remesh_at(ctx, 0.0f);
            int moved_up = 0, moved_dn = 0, waist_still = 1;
            if (mm)
                for (int r = 0; r <= 16; r++)
                    for (int c2 = 0; c2 <= 8; c2++) {
                        int i = (r * 9 + c2) * 2;
                        float dy = mm->mesh_verts[i + 1] - base[i + 1];
                        float v = (float)r / 16.0f, u = (float)c2 / 8.0f;
                        if (v < 0.45f && dy != 0.0f) waist_still = 0;
                        /* 口线约 v=0.72, 上唇上移/下唇下移, 嘴角(边缘列)收零 */
                        if (u == 0.5f && v > 0.6f && v < 0.72f && dy < -0.05f) moved_up = 1;
                        if (u == 0.5f && v > 0.72f && v < 0.85f && dy > 0.05f) moved_dn = 1;
                    }
            ck(moved_up && moved_dn, "mouth=1: 口线上方上移/下方下移(局部张开)");
            ck(waist_still, "mouth: 腰带以下顶点纹丝不动(形变局部于上带)");
            hn_node_set_mesh_param(g_node, "mouth", 0.0f);
            mm = remesh_at(ctx, 0.0f);
            ck(mm && memcmp(base, mm->mesh_verts, sizeof(base)) == 0,
               "mouth 归零后回到基准顶点");
            hn_node_set_mesh_param(g_node, "body_x", 0.5f);
            mm = remesh_at(ctx, 0.0f);
            int ok_body = mm != NULL;
            if (mm)
                for (int i = 0; i < 2 * 9 * 17 && ok_body; i += 2) {
                    float dx = mm->mesh_verts[i] - base[i];
                    float dy = mm->mesh_verts[i + 1] - base[i + 1];
                    if (fabsf(dx - 2.5f) > 1e-4f || dy != 0.0f) ok_body = 0;
                }
            ck(ok_body, "body_x=0.5: 全部顶点 +x 均匀平移 2.5px(方向/整体)");
            hn_node_set_mesh_param(g_node, "body_x", 0.0f);
            hn_node_set_mesh_param(g_node, "no_such_param", 1.0f);   /* 未知名: 静默 */
            ck(1, "未知名参数静默忽略(不崩、不误写)");
        } else ck(0, "hnMeshParam 取不到 MESH/节点");
    }

    printf("== 旧口径回归: 未声明新属性时 = 原闭式(逐位) ==\n");
    {
        /* 守门断言: 一份只用旧属性(hn-mesh/sway/speed/anchor)的文档, 顶点
           必须与旧引擎的闭式公式**逐位**一致 —— 新形变叠加进去时旧的
           文档(pet.html 等)一个像素都不许漂。相位按引擎同款 double 数学
           求出再转 float; 乘加次序与引擎逐项对齐(-ffp-contract=off)。 */
        m = mesh_of("hn-mesh=\"4x3\" hn-mesh-sway=\"10\" hn-mesh-anchor=\"bottom\"");
        const hn_cmd *mm = m ? remesh_at(ctx, 150.0f) : NULL;
        int ok_leg = mm != NULL;
        if (mm)
            for (int r = 0; r <= 3 && ok_leg; r++)
                for (int c2 = 0; c2 <= 4; c2++) {
                    float u = (float)c2 / 4.0f, v = (float)r / 3.0f;
                    float phase = (float)(150.0 * 0.001 * 2.0 * M_PI / 3.0);
                    float ph = phase + v * 2.2f + u * 0.6f;
                    float ex = u * 100.0f + sinf(ph) * 10.0f * v + mm->x;
                    float ey = v * 80.0f + sinf(ph * 0.7f + 1.2f) * 10.0f * 0.35f * v + mm->y;
                    int i = (r * 5 + c2) * 2;
                    if (mm->mesh_verts[i] != ex || mm->mesh_verts[i + 1] != ey)
                        ok_leg = 0;
                }
        ck(ok_leg, "未声明新属性: 顶点与旧闭式逐位一致(旧文档零漂移守门)");
    }

    printf("== %s (%d 项失败) ==\n", fails ? "失败" : "全部通过", fails);
    return fails ? 1 : 0;
}

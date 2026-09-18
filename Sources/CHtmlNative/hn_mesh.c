/* hn_mesh.c — 网格变形贴图(Live2D 类效果的原语)
 *
 * Live2D 的 .moc3 是专有格式, 解码需要其 Cubism SDK(商业授权), 无法自研。
 * 但它公开的核心技术很清晰: 把一张贴图映射到可变形的网格上, 逐顶点驱动
 * 即可实现呼吸/摆动/口型/眨眼等"活起来"的效果。本文件实现这个原语:
 *
 *   <img src="char.png" hn-mesh="12x10" hn-mesh-sway="8"
 *        hn-mesh-speed="1.4" hn-mesh-anchor="bottom">
 *
 * 顶点 = 均匀网格 + 正弦形变(按 anchor 端固定, 向另一端渐强)。
 * 更复杂的装配(骨骼/物理/口型同步)由脚本逐帧调用
 * hn_node_set_mesh_verts() 写入顶点 —— 引擎只做"网格 + 贴图"的合成,
 * rig 逻辑属于应用层, 这与 Live2D 把 SDK 与建模工具分层的做法一致。
 *
 * 输出是平台无关的 HN_CMD_MESH 指令: 各后端用三角形仿射贴图实现
 * (native 走 CGContext, hnsoft 走逐像素重心插值)。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn_internal.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* 解析 "NxM"(如 "12x10"); 失败写默认值 */
static void parse_grid(const char *s, int *cols, int *rows) {
    *cols = 8; *rows = 8;
    if (!s || !*s) return;
    int c = 0, r = 0;
    if (sscanf(s, "%dx%d", &c, &r) == 2 && c > 0 && r > 0) {
        if (c > 128) c = 128;      /* 上限: 128x128 = 16641 顶点, 足够细腻 */
        if (r > 128) r = 128;
        *cols = c; *rows = r;
    }
}

/* 顶点网格在元素盒内的基础位置(未形变): 归一化 0..1 铺满 */
static void base_verts(hn_arena *a, int cols, int rows, float bw, float bh,
                       const char *anchor, float sway, float phase,
                       float *out_verts, float *out_uv) {
    /* anchor 决定"固定端": bottom/top/left/right/center。
       固定端形变幅度为 0, 另一端最大 —— 模拟根部固定、末端摆动的物理直觉。 */
    int fixed_bottom = 1, fixed_top = 0, fixed_left = 0, fixed_right = 0, radial = 0;
    if (anchor) {
        if (!strcmp(anchor, "top"))    { fixed_bottom = 0; fixed_top = 1; }
        else if (!strcmp(anchor, "left"))  { fixed_bottom = 0; fixed_left = 1; }
        else if (!strcmp(anchor, "right")) { fixed_bottom = 0; fixed_right = 1; }
        else if (!strcmp(anchor, "center")){ fixed_bottom = 0; radial = 1; }
    }
    for (int r = 0; r <= rows; r++) {
        for (int c = 0; c <= cols; c++) {
            int i = r * (cols + 1) + c;
            float u = (float)c / (float)cols;
            float v = (float)r / (float)rows;
            float x = u * bw, y = v * bh;
            /* 形变权重(0 = 固定端) */
            float wgt = 1.0f;
            if (radial) {
                float dx = u - 0.5f, dy = v - 0.5f;
                float d = sqrtf(dx * dx + dy * dy) * 2.0f;
                wgt = d > 1.0f ? 1.0f : d;
            } else if (fixed_bottom) {
                /* wgt 必须 = 0 在**固定端**。v 向下增大, 底部是 v=0,
                   所以底部固定 → wgt = v。之前写成 1-v, 于是底部(固定端)
                   拿到最大摆幅、顶部反而纹丝不动 —— 与"根在底部、梢在顶部"
                   的物理直觉正好相反, 表现为"草/角色从顶上开始摇"。
                   横向那对(left=u / right=1-u)是对的, 参照它们即可看出
                   竖向这一对是写反了。 */
                wgt = v;
            } else if (fixed_top) {
                wgt = 1.0f - v;
            } else if (fixed_left) {
                wgt = u;
            } else if (fixed_right) {
                wgt = 1.0f - u;
            }
            if (sway != 0.0f) {
                /* 摆动: 相位沿竖直方向递进, 形成波浪(而非整体平移) */
                float ph = phase + v * 2.2f + u * 0.6f;
                x += sinf(ph) * sway * wgt;
                y += sinf(ph * 0.7f + 1.2f) * sway * 0.35f * wgt;
            }
            out_verts[i * 2]     = x;
            out_verts[i * 2 + 1] = y;
            out_uv[i * 2]     = u;
            out_uv[i * 2 + 1] = v;
        }
    }
    (void)a;
}

/* 发射 MESH 指令。src 为源图路径, bw/bh 为元素盒尺寸, sx/sy 为滚动补偿。 */
void hn_mesh_emit(hn_context *c, hn_node *n, const char *src,
                  float bw, float bh, float sx, float sy, float alpha) {
    if (!c || !n || !src || !*src) return;
    if (bw <= 0.5f || bh <= 0.5f) return;

    int cols = 0, rows = 0;
    float *verts = NULL, *uv = NULL;
    hn_arena *a = c->tmp;

    if (n->mesh_verts && n->mesh_cols > 0 && n->mesh_rows > 0) {
        /* 脚本驱动的网格(应用层自己的 rig): 顶点已是元素盒局部坐标 */
        cols = n->mesh_cols; rows = n->mesh_rows;
    } else {
        const char *g = hn_node_attr(n, "hn-mesh");
        parse_grid(g && *g && strcmp(g, "true") && strcmp(g, "1") ? g : NULL, &cols, &rows);
    }

    int nv = (cols + 1) * (rows + 1);
    verts = (float *)hn_arena_alloc(a, sizeof(float) * (size_t)(nv * 2));
    uv    = (float *)hn_arena_alloc(a, sizeof(float) * (size_t)(nv * 2));
    if (!verts || !uv) return;

    if (n->mesh_verts) {
        memcpy(verts, n->mesh_verts, sizeof(float) * (size_t)(nv * 2));
        for (int r = 0; r <= rows; r++)
            for (int cc = 0; cc <= cols; cc++) {
                int i = r * (cols + 1) + cc;
                uv[i * 2]     = (float)cc / (float)cols;
                uv[i * 2 + 1] = (float)r / (float)rows;
            }
    } else {
        float sway = 0, speed = 1.0f;
        const char *sv = hn_node_attr(n, "hn-mesh-sway");
        if (sv && *sv) sway = (float)atof(sv);
        const char *sp = hn_node_attr(n, "hn-mesh-speed");
        if (sp && *sp) { speed = (float)atof(sp); if (speed <= 0.01f) speed = 1.0f; }
        const char *anchor = hn_node_attr(n, "hn-mesh-anchor");
        /* 时钟 = 节点外部动画时钟(ms) → 弧度; 周期约 3s */
        float phase = (float)(n->ext_clock * 0.001 * speed * 2.0 * M_PI / 3.0);
        base_verts(a, cols, rows, bw, bh, anchor, sway, phase, verts, uv);
    }

    /* 平移到元素盒位置(顶点为盒局部坐标) + 滚动补偿 */
    for (int i = 0; i < nv; i++) {
        verts[i * 2]     += sx;
        verts[i * 2 + 1] += sy;
    }

    hn_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = HN_CMD_MESH;
    cmd.mesh_verts = verts;
    cmd.mesh_uv = uv;
    cmd.mesh_cols = cols;
    cmd.mesh_rows = rows;
    cmd.text = src;
    cmd.text_len = strlen(src);
    cmd.fill = 0xFFFFFF00u | (unsigned)(alpha * 255.0f);   /* 承载整体不透明度 */
    cmd.x = sx; cmd.y = sy; cmd.w = bw; cmd.h = bh;        /* 包围盒(后端剔除用) */
    hn_paint_push(c, &cmd);
}

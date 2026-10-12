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
 *
 * 数字人观感细化(与 sway 叠加; 全部是确定性闭式函数, 无随机):
 *   hn-mesh-breath="频率"  整体 y 向微缩放呼吸(缩放中心 = anchor 端);
 *   hn-mesh-anchor="head|chest|waist"
 *                          三带 anchor: 网格按 y 分腰带/胸带/头带, 摆幅
 *                          头带最大、腰带最小, 带间 smoothstep 平滑过渡;
 *   hn-mesh-phase-y="k"    行相位 = k*row(缺省 v*2.2), 波浪沿身传播;
 *   hn-mesh-look="0-100"   视线/头部跟随指针: 指针坐标由运行时喂入
 *                          (hn_context_set_pointer), 求值时取"指针相对
 *                          元素中心"的方向, 节点上一阶滞后平滑后作为
 *                          剪切+平移幅度(头带权重最高);
 *   hn-mesh-idle="1"       待机双正弦 sin(t*0.9)+0.5*sin(t*2.3) 叠加;
 *   JS 桥 hnMeshParam(id, name, v) 写节点的形变参数(hn_node_set_mesh_param),
 *   求值时叠加 look_x/look_y(偏移)/mouth(上带局部张开)/body_x/body_y(整体摆)。
 *
 * 更复杂的装配(骨骼/物理/口型同步)由脚本逐帧调用
 * hn_node_set_mesh_verts() 写入顶点 —— 引擎只做"网格 + 贴图"的合成,
 * rig 逻辑属于应用层, 这与 Live2D 把 SDK 与建模工具分层的做法一致。
 *
 * 输出是平台无关的 HN_CMD_MESH 指令: 各后端用三角形仿射贴图实现
 * (native 走 CGContext, hnsoft 走逐像素重心插值)。
 *
 * 确定性: 一切形变都是 (u, v, ext_clock, 指针坐标, 参数) 的纯函数 +
 * 节点上的标量滞后状态; 同一状态序列喂同一文档必得同一顶点序列
 * (-ffp-contract=off, 无 libm 随机源 —— 过渡曲线用多项式 smoothstep)。
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

/* smoothstep: [a,b] 内 C¹ 连续过渡(区间外取端值)。三带 anchor 的带间
   幅度过渡与口型的张开边缘都用它 —— 多项式闭式解, 不经 libm 超越函数
   (避免跨平台 libm 实现差异触碰确定性口径)。 */
static float sstep(float a, float b, float x) {
    float t = (x - a) / (b - a);
    if (t < 0.0f) t = 0.0f;
    else if (t > 1.0f) t = 1.0f;
    return t * t * (3.0f - 2.0f * t);
}

/* 三带 anchor(head/chest/waist)的摆幅权重曲线。
 * 网格按 y 分三带: 腰带(v < 0.40)/胸带(0.40..0.72)/头带(> 0.72, v=1 为
 * 自由端 —— 与 anchor=bottom 的"v=0 固定端"同一取向)。摆幅恒有
 * 头带 > 胸带 > 腰带, 带间用 smoothstep 过渡(避免硬边)。
 * 三档的差异 = 腰带的参与下限, 即人物"扎根"的位置:
 *   waist —— 下身几乎钉住(站姿摆动, 根在腰, 全身从腰往上波浪);
 *   chest —— 下身小幅参与(重心在胸, 髋部微动);
 *   head  —— 全身都动但幅度抬平(头为主角的张望/跟随, 下身仍最弱)。 */
static float band_wgt(int band, float v) {
    float lw = band == 1 ? 0.06f : (band == 2 ? 0.30f : 0.55f);
    float lc = band == 1 ? 0.55f : (band == 2 ? 0.62f : 0.78f);
    float lh = 1.0f;
    float w = lw + (lc - lw) * sstep(0.28f, 0.52f, v);   /* 腰带 → 胸带 */
    return w + (lh - w) * sstep(0.60f, 0.84f, v);        /* 胸带 → 头带 */
}

/* 一次求值的全部驱动量(emit 解析属性/指针/参数后装配, base_verts 消费) */
typedef struct {
    float sway;        /* 摆幅 px(原有) */
    float phase;       /* 基相位(弧度) = ext_clock*0.001*speed*2π/3 */
    float phase_y;     /* 行相位系数; <0 = 未声明(默认 v*2.2) */
    int   band;        /* 三带 anchor: 0=无 1=waist 2=chest 3=head */
    float breath;      /* 呼吸频率倍率, 0=关 */
    int   idle;        /* 待机双正弦 */
    /* 指针跟随: 幅度上限(px, 已按 look 强度缩放)与平滑后的方向(-1..1) */
    float look_amp_x, look_amp_y, look_x, look_y;
    /* JS 参数桥(hnNodeSetMeshParam 写入, 已钳 -1..1) */
    float p_look_x, p_look_y, p_mouth, p_body_x, p_body_y;
} mesh_drive;

/* 顶点网格在元素盒内的基础位置 + 全部程序化形变。
   输出顶点为元素盒局部坐标(调用方再加盒偏移/滚动补偿), UV 恒为均匀网格。 */
static void base_verts(hn_arena *a, int cols, int rows, float bw, float bh,
                       const char *anchor, const mesh_drive *d,
                       float *out_verts, float *out_uv) {
    /* anchor 决定"固定端": bottom/top/left/right/center-radial, 以及
       三带档(head/chest/waist —— 覆盖权重曲线, 取向同 bottom)。 */
    int fixed_bottom = 1, fixed_top = 0, fixed_left = 0, fixed_right = 0, radial = 0;
    if (anchor) {
        if (!strcmp(anchor, "top"))    { fixed_bottom = 0; fixed_top = 1; }
        else if (!strcmp(anchor, "left"))  { fixed_bottom = 0; fixed_left = 1; }
        else if (!strcmp(anchor, "right")) { fixed_bottom = 0; fixed_right = 1; }
        else if (!strcmp(anchor, "center")){ fixed_bottom = 0; radial = 1; }
    }
    int band = d->band;   /* 1..3 时覆盖下面的权重逻辑 */
    /* 呼吸缩放中心 = anchor 端(三带档取名义带的中线) */
    float breath_y0 = 0.5f * bh;
    if (band == 1)                 breath_y0 = 0.20f * bh;
    else if (band == 2)            breath_y0 = 0.56f * bh;
    else if (band == 3)            breath_y0 = 0.86f * bh;
    else if (fixed_bottom)         breath_y0 = 0.0f;
    else if (fixed_top)            breath_y0 = bh;
    /* 呼吸缩放因子: 频率只是相位倍率, 幅度固定 1.8% 盒高(微缩放,
       与 CSS 的 pet-bob 那种整体位移不同 —— 顶点真的被拉开/合拢)。 */
    float br_s = 1.0f;
    if (d->breath > 0.0f)
        br_s = 1.0f + 0.018f * sinf(d->phase * d->breath);
    /* 待机双正弦(任务口径 sin(t*0.9)+0.5*sin(t*2.3), t = 基相位 ——
       与 sway 同一时间基准, speed 同时缩放两者)。幅度小: sway 的 22%
       (无 sway 时 1.6px 底值), 只为"待机也活着", 不与主摆抢戏。 */
    float idle_ax = 0.0f, idle_ay = 0.0f;
    if (d->idle) {
        float amp = d->sway > 0.0f ? 0.22f * d->sway : 1.6f;
        float t = d->phase;
        idle_ax = amp * (sinf(t * 0.9f) + 0.5f * sinf(t * 2.3f));
        idle_ay = amp * 0.35f * (sinf(t * 1.7f + 1.1f) + 0.5f * sinf(t * 0.6f + 2.9f));
    }
    /* 口型张开幅度: 3.5% 盒高(嘴线 ±0.10v、±0.18u 内的局部张开) */
    float mouth_px = d->p_mouth * 0.035f * bh;
    /* 整体摆(JS 桥): 5% 盒尺寸上限的均匀平移 */
    float body_px = d->p_body_x * 0.05f * bw;
    float body_py = d->p_body_y * 0.05f * bh;
    for (int r = 0; r <= rows; r++) {
        for (int c = 0; c <= cols; c++) {
            int i = r * (cols + 1) + c;
            float u = (float)c / (float)cols;
            float v = (float)r / (float)rows;
            float x = u * bw, y = v * bh;
            /* 形变权重(0 = 固定端) */
            float wgt = 1.0f;
            if (band) {
                wgt = band_wgt(band, v);
            } else if (radial) {
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
            if (d->sway != 0.0f) {
                /* 摆动: 相位沿竖直方向递进, 形成波浪(而非整体平移)。
                   phase-y 声明时行相位 = k*row(波浪沿身的传播速率直接
                   可调); 缺省保持 v*2.2(与旧引擎逐位一致)。 */
                float row_ph = d->phase_y >= 0.0f ? d->phase_y * (float)r
                                                  : v * 2.2f;
                float ph = d->phase + row_ph + u * 0.6f;
                x += sinf(ph) * d->sway * wgt;
                y += sinf(ph * 0.7f + 1.2f) * d->sway * 0.35f * wgt;
            }
            if (d->idle) {
                x += idle_ax * wgt;
                y += idle_ay * wgt;
            }
            if (br_s != 1.0f)
                y = breath_y0 + (y - breath_y0) * br_s;   /* 呼吸: 绕 anchor 端微缩放 */
            if (d->look_amp_x != 0.0f || d->look_amp_y != 0.0f ||
                d->p_look_x != 0.0f || d->p_look_y != 0.0f) {
                /* 视线/头部跟随: 平移成分全场都有(0.35 底), 头带权重最高
                   —— 位移随高度递增即"剪切+平移", 看起来是朝指针侧头。
                   竖直分量弱一些(点头幅度 < 转头幅度)。指针 look 与 JS
                   桥 look_x/look_y 走同一权重曲线, 直接相加。 */
                float lw = 0.35f + 0.65f * wgt;
                x += d->look_amp_x * d->look_x * lw + d->p_look_x * 0.05f * bw * lw;
                y += d->look_amp_y * d->look_y * lw + d->p_look_y * 0.025f * bh * lw;
            }
            if (mouth_px != 0.0f) {
                /* 口型: 上带(头带内, 口线约 v=0.72)局部张开 —— 口线上方
                   顶点上移、下方下移(张开), 水平向嘴角收零。核取
                   (1-t²)³(Wyvill, C² 连续), 紧支集: 带外顶点纹丝不动。 */
                float dv = (v - 0.72f) / 0.10f;
                float du = (u - 0.5f) / 0.18f;
                float kv = 1.0f - dv * dv, ku = 1.0f - du * du;
                if (kv > 0.0f && ku > 0.0f) {
                    float open = mouth_px * kv * kv * kv * ku * ku * ku;
                    y += dv > 0.0f ? open : -open;
                }
            }
            x += body_px;
            y += body_py;
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
        const char *sv = hn_node_attr(n, "hn-mesh-sway");
        const char *sp = hn_node_attr(n, "hn-mesh-speed");
        const char *anchor = hn_node_attr(n, "hn-mesh-anchor");
        mesh_drive d;
        memset(&d, 0, sizeof(d));
        d.phase_y = -1.0f;   /* 哨兵: 未声明 hn-mesh-phase-y → 走 v*2.2 旧口径 */
        if (sv && *sv) d.sway = (float)atof(sv);
        float speed = 1.0f;
        if (sp && *sp) { speed = (float)atof(sp); if (speed <= 0.01f) speed = 1.0f; }
        /* 时钟 = 节点外部动画时钟(ms) → 弧度; 周期约 3s */
        d.phase = (float)(n->ext_clock * 0.001 * speed * 2.0 * M_PI / 3.0);
        /* 三带 anchor 档 */
        if (anchor) {
            if (!strcmp(anchor, "waist")) d.band = 1;
            else if (!strcmp(anchor, "chest")) d.band = 2;
            else if (!strcmp(anchor, "head")) d.band = 3;
        }
        /* 行相位系数(缺省 -1 = 走 v*2.2 旧口径) */
        const char *py = hn_node_attr(n, "hn-mesh-phase-y");
        if (py && *py) d.phase_y = (float)atof(py);
        /* 呼吸频率(倍率; 缺省 1) */
        const char *br = hn_node_attr(n, "hn-mesh-breath");
        if (br) d.breath = *br ? (float)atof(br) : 1.0f;
        /* 待机(布尔: 存在且非 "0"/"false") */
        const char *id = hn_node_attr(n, "hn-mesh-idle");
        if (id && *id && strcmp(id, "0") && strcmp(id, "false")) d.idle = 1;
        /* 视线跟随强度 0..100: 幅度上限按强度缩放(100 = 头带 6% 盒宽) */
        const char *lk = hn_node_attr(n, "hn-mesh-look");
        if (lk && *lk) {
            float li = (float)atof(lk);
            if (li < 0.0f) li = 0.0f;
            else if (li > 100.0f) li = 100.0f;
            d.look_amp_x = li * 0.0006f * bw;    /* li/100 * 6% bw */
            d.look_amp_y = li * 0.00025f * bh;   /* li/100 * 2.5% bh */
            /* 平滑滞后: 节点上的当前 look 向目标(指针方向)逼近固定系数
               0.2/次求值 —— 一阶滞后, 约 20 次求值收敛(60fps 下 ~0.3s)。
               确定性: 系数是字面常量, 状态只在节点上, 同一求值序列必得
               同一轨迹。未收到指针事件(has_pointer=0)时目标 = 回中。 */
            float tx = 0.0f, ty = 0.0f;
            if (c->has_pointer) {
                float cx = sx + bw * 0.5f, cy = sy + bh * 0.5f;
                float rx = c->vw > 1.0f ? c->vw * 0.5f : 200.0f;
                float ry = c->vh > 1.0f ? c->vh * 0.5f : 200.0f;
                tx = (c->pointer_x - cx) / rx;
                ty = (c->pointer_y - cy) / ry;
                if (tx < -1.0f) tx = -1.0f; else if (tx > 1.0f) tx = 1.0f;
                if (ty < -1.0f) ty = -1.0f; else if (ty > 1.0f) ty = 1.0f;
            }
            n->mesh_look_x += (tx - n->mesh_look_x) * 0.2f;
            n->mesh_look_y += (ty - n->mesh_look_y) * 0.2f;
            d.look_x = n->mesh_look_x;
            d.look_y = n->mesh_look_y;
        }
        /* JS 参数桥: 节点字段直读(写入处已钳 -1..1) */
        d.p_look_x = n->mesh_param_look_x;
        d.p_look_y = n->mesh_param_look_y;
        d.p_mouth  = n->mesh_param_mouth;
        d.p_body_x = n->mesh_param_body_x;
        d.p_body_y = n->mesh_param_body_y;
        base_verts(a, cols, rows, bw, bh, anchor, &d, verts, uv);
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

/* 数字人参数桥引擎侧(hnMeshParam 的落点): 参数存节点字段, 下一次网格
   求值生效。未知名静默忽略(桥不抛异常); 值统一钳 -1..1。 */
void hn_node_set_mesh_param(hn_node *n, const char *name, float value) {
    if (!n || n->kind != HN_ELEM || !name || !*name) return;
    if (value < -1.0f) value = -1.0f;
    else if (value > 1.0f) value = 1.0f;
    if (!strcmp(name, "look_x"))       n->mesh_param_look_x = value;
    else if (!strcmp(name, "look_y"))  n->mesh_param_look_y = value;
    else if (!strcmp(name, "mouth"))   n->mesh_param_mouth = value;
    else if (!strcmp(name, "body_x"))  n->mesh_param_body_x = value;
    else if (!strcmp(name, "body_y"))  n->mesh_param_body_y = value;
}

/* hn_three.c — three.js 子集垂直切片: 场景图 + 透视相机 + 旋转立方体
 * ============================================================
 * docs/threejs-support.md 的"下一个最小可演示切片"。本引擎无 WebGL
 * (该文档口径), three.js 类 3D 的现实路径是**CPU 侧几何 + 引擎既有
 * 光栅**。本模块做前三件事, 像素交给既有光栅:
 *
 *   1. Scene 场景图: t3_node 父子树, 局部 TRS → mat4 逐级复合
 *      (Scene → pivot Group → Mesh(BoxGeometry), 与 three.js 同构)
 *   2. PerspectiveCamera: fov/aspect/near/far + lookAt 固定视点
 *      (引擎仍无运行时相机控制器 —— 相机是本模块的常量, 文档口径不变)
 *   3. 软件三角光栅: 顶点汤(世界→视→裁剪→NDC→屏幕) → **HN_CMD_POLYGON
 *      3 顶点三角形** → 复用 paint_polygon 的既有扫描内核(与 Lottie
 *      矢量填充同一颗; 不新写扫描线)。
 *      为什么不走 HN_CMD_TRIS: TRIS 契约是"逐像素 UV 贴图三角汤"
 *      (hnsoft.c paint_tris: text 缺图 → 静默跳过), 无贴图的逐面平直
 *      着色色它表达不了; POLYGON 的纯色填充正是本切片要的通道。
 *
 * 逐面明暗(Lambert: ambient + max(0,n·L))在**本模块 CPU**算好进
 * fill 颜色 —— 引擎光栅仍无光照模型, 文档"无光照"口径不变。
 *
 * 确定性: 立方体自转角 = 节点 ext_clock(HN_CLOCK / --advance 取样,
 * 与 hn-mesh/Lottie 同一环境动画通道, 见 hn_context.c anim_walk)。
 *
 * 常驻开销: 零 —— 无全局状态、无堆分配(顶点汤进引擎临时区, 生命周期
 * = 本次显示列表); 未声明 hn-three 的页面不进任何新代码路径。
 *
 * 已知边界(诚实清单):
 *   - 近平面按"整三角剔除"处理(任一顶点 clip.w ≤ 0 丢弃), 未做剖分;
 *     demo 相机距离下永不触发, 语义边界如实声明
 *   - 遮挡 = 画家算法(视空间质心 z 远→近发射), 面片级; 凸体(立方体)
 *     配合背面剔除精确, 凹体/互穿面片不保证逐像素正确(与 CSS 3D 同限)
 *   - 一个 hn-three 元素 = 一个固定场景(立方体); 加载 glTF/自定义
 *     几何不在本切片
 */
#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "hn_internal.h"

#define T3_PI 3.14159265358979f

/* ---- mat4(行主序, 右乘列向量: clip = M · v) ---- */

typedef float t3_mat4[16];

static void t3_mat_identity(t3_mat4 m) {
    memset(m, 0, sizeof(t3_mat4));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void t3_mat_mul(t3_mat4 out, const t3_mat4 a, const t3_mat4 b) {
    t3_mat4 r;
    for (int row = 0; row < 4; row++)
        for (int col = 0; col < 4; col++) {
            float s = 0;
            for (int k = 0; k < 4; k++)
                s += a[row * 4 + k] * b[k * 4 + col];
            r[row * 4 + col] = s;
        }
    memcpy(out, r, sizeof(t3_mat4));
}

/* 平移/旋转(局部 TRS 用; 缩放并入盒子尺寸, 切片不需要 scale 节点) */
static void t3_mat_translate(t3_mat4 m, float x, float y, float z) {
    t3_mat_identity(m);
    m[12] = x; m[13] = y; m[14] = z;
}

static void t3_mat_rot_x(t3_mat4 m, float rad) {
    t3_mat_identity(m);
    float c = cosf(rad), s = sinf(rad);
    m[5] = c;  m[6] = s;
    m[9] = -s; m[10] = c;
}

static void t3_mat_rot_y(t3_mat4 m, float rad) {
    t3_mat_identity(m);
    float c = cosf(rad), s = sinf(rad);
    m[0] = c;  m[2] = -s;
    m[8] = s;  m[10] = c;
}

/* 透视投影(GL 口径, fov 为竖直角): 与 three.js PerspectiveCamera 一致 */
static void t3_mat_perspective(t3_mat4 m, float fov_rad, float aspect,
                               float near_z, float far_z) {
    memset(m, 0, sizeof(t3_mat4));
    float f = 1.0f / tanf(fov_rad * 0.5f);
    m[0] = f / aspect;
    m[5] = f;
    m[10] = (far_z + near_z) / (near_z - far_z);
    m[11] = -1.0f;
    m[14] = (2.0f * far_z * near_z) / (near_z - far_z);
}

/* lookAt(右手系): 视图矩阵(z 轴 = eye−center 归一) */
static void t3_mat_lookat(t3_mat4 m, float ex, float ey, float ez,
                          float cx, float cy, float cz) {
    float zx = ex - cx, zy = ey - cy, zz = ez - cz;
    float zl = sqrtf(zx * zx + zy * zy + zz * zz);
    if (zl < 1e-6f) { t3_mat_identity(m); return; }
    zx /= zl; zy /= zl; zz /= zl;
    /* up = (0,1,0): x = up × z, y = z × x */
    float xx = 1.0f * zz - 0.0f * zy;      /* up×z = ( 1*zz, 0, -1*zx ) */
    float xy = 0.0f * zx - 0.0f * zz;
    float xz = 0.0f * zy - 1.0f * zx;
    float xl = sqrtf(xx * xx + xy * xy + xz * xz);
    if (xl < 1e-6f) { t3_mat_identity(m); return; }
    xx /= xl; xy /= xl; xz /= xl;
    float yx = zy * xz - zz * xy;
    float yy = zz * xx - zx * xz;
    float yz = zx * xy - zy * xx;
    m[0] = xx; m[4] = xy; m[8]  = xz;
    m[1] = yx; m[5] = yy; m[9]  = yz;
    m[2] = zx; m[6] = zy; m[10] = zz;
    m[12] = -(xx * ex + xy * ey + xz * ez);
    m[13] = -(yx * ex + yy * ey + yz * ez);
    m[14] = -(zx * ex + zy * ey + zz * ez);
    m[3] = m[7] = m[11] = 0.0f;
    m[15] = 1.0f;
}

static void t3_mat_point(const t3_mat4 m, float x, float y, float z, float out[4]) {
    out[0] = m[0] * x + m[4] * y + m[8]  * z + m[12];
    out[1] = m[1] * x + m[5] * y + m[9]  * z + m[13];
    out[2] = m[2] * x + m[6] * y + m[10] * z + m[14];
    out[3] = m[3] * x + m[7] * y + m[11] * z + m[15];
}

/* ---- BoxGeometry: 8 顶点 12 三角, 逐面法线与基色 ----
   面序: +x -x +y -y +z -z(three.js BoxGeometry 材质组序) */

typedef struct { float x, y, z; } t3_vec3;

static const t3_vec3 T3_CUBE_V[8] = {
    { -0.575f, -0.575f,  0.575f }, {  0.575f, -0.575f,  0.575f },
    {  0.575f,  0.575f,  0.575f }, { -0.575f,  0.575f,  0.575f },
    { -0.575f, -0.575f, -0.575f }, {  0.575f, -0.575f, -0.575f },
    {  0.575f,  0.575f, -0.575f }, { -0.575f,  0.575f, -0.575f },
};

/* 每三角: 3 顶点索引 + 所属面(取法线/基色) */
typedef struct { int v[3]; int face; } t3_tri;

static const t3_tri T3_CUBE_T[12] = {
    { { 1, 5, 6 }, 0 }, { { 1, 6, 2 }, 0 },   /* +x 右 */
    { { 4, 0, 3 }, 1 }, { { 4, 3, 7 }, 1 },   /* -x 左 */
    { { 3, 2, 6 }, 2 }, { { 3, 6, 7 }, 2 },   /* +y 顶 */
    { { 4, 5, 1 }, 3 }, { { 4, 1, 0 }, 3 },   /* -y 底 */
    { { 0, 1, 2 }, 4 }, { { 0, 2, 3 }, 4 },   /* +z 前 */
    { { 5, 4, 7 }, 5 }, { { 5, 7, 6 }, 5 },   /* -z 后 */
};

static const t3_vec3 T3_FACE_N[6] = {
    { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 },
    { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 },
};

/* six-color 面板(向 three.js 多材质盒致敬; 明暗由光照乘出) */
static const unsigned int T3_FACE_RGB[6] = {
    0xE5484D,  /* +x 红 */
    0x4F7CFF,  /* -x 蓝 */
    0x46D68C,  /* +y 绿 */
    0x8B5CF6,  /* -y 紫 */
    0xF5A623,  /* +z 琥珀 */
    0x22D3EE,  /* -z 青 */
};

/* ---- 场景图(Scene → Group → Mesh): 局部矩阵逐级复合 ----
   切片场景固定, 故用显式两级结构表达"父子矩阵复合"这一件事;
   泛化节点池留给下一片(见 docs/threejs-support.md 下一步)。 */

static void t3_compose_scene(float spin_rad, float tilt_rad, t3_mat4 mesh_world) {
    t3_mat4 scene, pivot_t, rot_y, rot_x, mesh_t;
    t3_mat_identity(scene);                       /* Scene: 恒等 */
    t3_mat_translate(pivot_t, 0, 0, 0);           /* pivot Group: 原点 */
    t3_mat_rot_y(rot_y, spin_rad);                /*   自转(动画驱动) */
    t3_mat_rot_x(rot_x, tilt_rad);                /*   固定倾角 */
    t3_mat_translate(mesh_t, 0, 0, 0);            /* Mesh: 组内原点 */
    t3_mat_mul(mesh_world, scene, pivot_t);
    {   /* world = scene · pivot · rotY · rotX · mesh */
        t3_mat4 a, b;
        t3_mat_mul(a, mesh_world, rot_y);
        t3_mat_mul(b, a, rot_x);
        t3_mat_mul(mesh_world, b, mesh_t);
    }
}

/* ---- 发射: hn-three 属性元素 → 显示列表 ---- */

void hn_three_paint(hn_context *c, hn_node *n,
                    float bw, float bh, float sx, float sy, float alpha) {
    if (bw < 8.0f || bh < 8.0f) return;           /* 盒过小: 无可画(布局给的) */

    /* 自转周期: hn-three-spin="6" = 6 秒一圈(threejs-ui 口径 0→360°/6s);
       缺省 6s, 下限 0.5s 防呆。角度取节点 ext_clock —— 环境动画通道,
       HN_CLOCK/<--advance> 确定性取样。 */
    float spin_s = 6.0f;
    const char *sp = hn_node_attr(n, "hn-three-spin");
    if (sp && *sp) {
        spin_s = (float)atof(sp);
        if (spin_s < 0.5f) spin_s = 0.5f;
    }
    float spin_rad = (n->ext_clock / (spin_s * 1000.0f)) * 2.0f * T3_PI;
    float tilt_rad = -20.0f * T3_PI / 180.0f;     /* 固定俯角: t=0 即立体感 */

    /* 相机: 固定视点(fixed 视点 = 文档"无 OrbitControls"口径); 高机位让
       顶面入画(经典 three.js 三面观感), 距离调到立方体约占盒高 2/3。
       fov 50°(three.js 缺省近似), aspect = 盒比。 */
    t3_mat4 view, proj;
    t3_mat_lookat(view, 1.7f, 2.3f, 3.1f, 0.0f, 0.0f, 0.0f);
    t3_mat_perspective(proj, 50.0f * T3_PI / 180.0f, bw / bh, 0.1f, 100.0f);
    const float eye[3] = { 1.7f, 2.3f, 3.1f };

    t3_mat4 world;
    t3_compose_scene(spin_rad, tilt_rad, world);

    /* 方向光(three.js DirectionalLight 观感): 归一 (0.45, 0.8, 0.55) */
    const float lx = 0.436f, ly = 0.775f, lz = 0.533f;

    /* 逐三角变换 + 剔除, 收集可见屏幕三角形(≤12) */
    typedef struct {
        float x[3], y[3];        /* 屏幕坐标(元素盒局部) */
        float depth;             /* 视空间质心 z(画家键, 小 = 远) */
        hn_color fill;           /* 平直着色 × 光照 × 元素 alpha */
    } t3_vis;
    t3_vis vis[12];
    int n_vis = 0;

    float a_mul = alpha;         /* paint_walk 已并 opacity, 这里只乘自身 */
    if (a_mul > 1.0f) a_mul = 1.0f;

    for (int t = 0; t < 12; t++) {
        float wx[3], wy[3], wz[3];
        for (int k = 0; k < 3; k++) {
            const t3_vec3 *p = &T3_CUBE_V[T3_CUBE_T[t].v[k]];
            float clip[4];
            t3_mat_point(world, p->x, p->y, p->z, clip);
            wx[k] = clip[0]; wy[k] = clip[1]; wz[k] = clip[2];
        }
        /* 世界空间面法线(旋转随动) + 背面剔除: n·(eye−centroid) ≤ 0 → 背面 */
        float e1x = wx[1] - wx[0], e1y = wy[1] - wy[0], e1z = wz[1] - wz[0];
        float e2x = wx[2] - wx[0], e2y = wy[2] - wy[0], e2z = wz[2] - wz[0];
        /* 三角顶点已按外向序手工排好, 法线 = e1×e2 */
        float nx = e1y * e2z - e1z * e2y;
        float ny = e1z * e2x - e1x * e2z;
        float nz = e1x * e2y - e1y * e2x;
        float cxw = (wx[0] + wx[1] + wx[2]) / 3.0f;
        float cyw = (wy[0] + wy[1] + wy[2]) / 3.0f;
        float czw = (wz[0] + wz[1] + wz[2]) / 3.0f;
        if (nx * (eye[0] - cxw) + ny * (eye[1] - cyw) + nz * (eye[2] - czw) <= 0.0f)
            continue;

        /* 裁剪空间 + 近平面(整三角剔除边界, 见头注) */
        float cx[3], cy[3], cw[3], vcz[3];
        int behind = 0;
        for (int k = 0; k < 3; k++) {
            const t3_vec3 *p = &T3_CUBE_V[T3_CUBE_T[t].v[k]];
            float vp[4], cl[4];
            t3_mat_point(view, p->x, p->y, p->z, vp);
            vcz[k] = vp[2];
            t3_mat_point(proj, vp[0], vp[1], vp[2], cl);
            cx[k] = cl[0]; cy[k] = cl[1]; cw[k] = cl[3];
            if (cw[k] < 0.01f) behind = 1;
        }
        if (behind) continue;

        /* NDC → 元素盒屏幕坐标(y 翻转); 三角沿**边法线**外扩 0.75px ——
           同色相邻三角的共享边上 paint_polygon 的半像素边缘覆盖(各 0.5)
           叠加只有 0.75, 会显出对角接缝; 每条边沿自身外法线平移 0.75px
           (顶点 = 相邻两边法线的 miter 和, 限幅 9px 只挡退化细条)后, 共享
           边两侧各有 0.75px 垂直重叠, 与边中段/端点无关, 过剩覆盖 ≥1,
           接缝消失(三角汤惯用手法, 与 paint_tris 的解析 AA"过剩覆盖"同一
           目的)。2026-10-14 复审核验: 原先"顶点自质心径向推"在共享边
           中段的法向分量不足 0.5px, 实拍共享对角线仍有 ~46% 掉深暗缝,
           不满足本声明; 改边法线外扩后实拍全对角线(含两端角部)欠覆盖
           像素归零, 轮廓位移 ≤1px。 */
        {
            float vx[3], vy[3], enx[3], eny[3];
            for (int k = 0; k < 3; k++) {
                float ndcx = cx[k] / cw[k], ndcy = cy[k] / cw[k];
                vx[k] = sx + bw * (ndcx * 0.5f + 0.5f);
                vy[k] = sy + bh * (1.0f - (ndcy * 0.5f + 0.5f));
            }
            for (int k = 0; k < 3; k++) {   /* 每条边的单位外法线 */
                int a = k, b = (k + 1) % 3, o = (k + 2) % 3;
                float ex = vx[b] - vx[a], ey = vy[b] - vy[a];
                float el = sqrtf(ex * ex + ey * ey);
                if (el < 1e-6f) { enx[k] = 0.0f; eny[k] = 0.0f; continue; }
                float nx = ey / el, ny = -ex / el;
                /* 朝外 = 与"边中点 → 对角顶点"反向 */
                if (nx * ((vx[a] + vx[b]) * 0.5f - vx[o]) +
                    ny * ((vy[a] + vy[b]) * 0.5f - vy[o]) < 0.0f) {
                    nx = -nx; ny = -ny;
                }
                enx[k] = nx; eny[k] = ny;
            }
            for (int k = 0; k < 3; k++) {   /* miter: 两邻边各外移恰 0.75px */
                int p = (k + 2) % 3;
                float sx_ = enx[p] + enx[k], sy_ = eny[p] + eny[k];
                float dt = 1.0f + (enx[p] * enx[k] + eny[p] * eny[k]);
                float ox = 0.0f, oy = 0.0f;
                if (dt > 1e-3f) {
                    ox = 0.75f * sx_ / dt;
                    oy = 0.75f * sy_ / dt;
                    float ol = sqrtf(ox * ox + oy * oy);
                    /* 限幅只挡透视近奇异的退化细条(内角 <~10° 才触发):
                       立方体三角最小内角 45°(正方形对角线端)的 miter 长度
                       0.75/cos67.5° ≈ 1.96px, 透视压缩角更大(本 demo 实测
                       ≤9px)。限幅若落在正常角范围内, 会把共享边的垂直
                       位移一并砍小, 边带重新相遇 → 接缝回归(2026-10-14
                       实拍: 限幅 1.5px 时对角线每两行漏一点、3px 时下角
                       残留 5 像素欠覆盖, 9px 归零且轮廓位移 ≤1px)。 */
                    if (ol > 9.0f) {
                        ox *= 9.0f / ol;
                        oy *= 9.0f / ol;
                    }
                }
                vis[n_vis].x[k] = vx[k] + ox;
                vis[n_vis].y[k] = vy[k] + oy;
            }
        }
        vis[n_vis].depth = (vcz[0] + vcz[1] + vcz[2]) / 3.0f;

        /* 平直着色: base × (0.35 + 0.65·max(0, n̂·L)); 法线归一 */
        const t3_vec3 *fn = &T3_FACE_N[T3_CUBE_T[t].face];
        /* 面法线经 world 旋转(左上 3x3; 切片无缩放, 直接变换) */
        float rnx = world[0] * fn->x + world[4] * fn->y + world[8]  * fn->z;
        float rny = world[1] * fn->x + world[5] * fn->y + world[9]  * fn->z;
        float rnz = world[2] * fn->x + world[6] * fn->y + world[10] * fn->z;
        float nl = sqrtf(rnx * rnx + rny * rny + rnz * rnz);
        float diff = 0.0f;
        if (nl > 1e-6f) {
            diff = (rnx * lx + rny * ly + rnz * lz) / nl;
            if (diff < 0.0f) diff = 0.0f;
        }
        float shade = 0.35f + 0.65f * diff;
        unsigned int rgb = T3_FACE_RGB[T3_CUBE_T[t].face];
        unsigned int r = (unsigned int)(((rgb >> 16) & 0xFF) * shade);
        unsigned int g = (unsigned int)(((rgb >> 8)  & 0xFF) * shade);
        unsigned int b = (unsigned int)((rgb & 0xFF) * shade);
        if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
        vis[n_vis].fill = 0x000000FFu | (b << 8) | (g << 16) | (r << 24);
        if (a_mul < 0.999f)
            vis[n_vis].fill = (vis[n_vis].fill & 0xFFFFFF00u)
                            | (unsigned char)((255.0f * a_mul) + 0.5f);
        n_vis++;
    }

    if (n_vis <= 0) return;

    /* 画家算法: 视空间质心 z 升序(远→近)稳定插入排序 —— 面片级遮挡,
       与 hn_paint.c preserve-3d 深度序同口径 */
    for (int i = 1; i < n_vis; i++) {
        t3_vis key = vis[i];
        int j = i - 1;
        while (j >= 0 && vis[j].depth > key.depth) { vis[j + 1] = vis[j]; j--; }
        vis[j + 1] = key;
    }

    /* 顶点汤进引擎临时区(生命周期 = 本次显示列表), 每三角一条 POLYGON
       —— 复用 paint_polygon 既有扫描内核, 不新写扫描线 */
    hn_arena *tmp = hn_context_tmp(c);
    if (!tmp) return;
    for (int i = 0; i < n_vis; i++) {
        float *poly = (float *)hn_arena_alloc(tmp, sizeof(float) * 6);
        if (!poly) return;
        poly[0] = vis[i].x[0]; poly[1] = vis[i].y[0];
        poly[2] = vis[i].x[1]; poly[3] = vis[i].y[1];
        poly[4] = vis[i].x[2]; poly[5] = vis[i].y[2];
        hn_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.kind = HN_CMD_POLYGON;
        cmd.poly = poly;
        cmd.poly_n = 3;
        cmd.fill = vis[i].fill;
        hn_paint_push(c, &cmd);
    }
}

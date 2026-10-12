/* live2d_probe.c — Live2D 运行时包接入探针(C 解析级, 离屏, 零 GUI 依赖)
 *
 * 边界口径(与引擎实现一致): .moc3 是专有二进制(需 Cubism SDK 商业授权),
 * **不解析**; 接入的是运行时包里的公开 JSON:
 *   model3.json  FileReferences(Moc 声明/Textures/Physics/DisplayInfo/Motions 组)
 *   cdi3.json    参数清单(计数 + 日文显示名)
 *   physics3.json  Meta 计数(v1 不做物理仿真)
 *   motion3.json   v2.4 曲线求值器(线性/贝塞尔/阶梯/反阶梯 + Loop 回绕)
 * 夹具直接引用 examples/assets/miku 真文件(官方 Miku 样例 runtime), 另有
 * examples/fixtures/live2d 的合成 motion3(四种段类型各一段, 端点手工可算)。
 *
 * 断言分层:
 *   1. 包解析: model3 字段事实(has_moc/textures/physics/displayinfo)/
 *      Motion 组与曲线数/cdi3 参数数与显示名;
 *   2. 求值器: 真实 motion(贝塞尔中点=-7、终点值、Loop 回绕)+ 合成
 *      motion(线性中点=中值、阶梯=前值、反阶梯=立即跳、贝塞尔中点);
 *   3. rig 接线: <img hn-mesh hn-live2d> 的 MESH 指令顶点随官方 motion
 *      变化 —— t=0 中性位与无 motion 逐位一致; 眨眼时刻上带下压、
 *      转头时刻整体平移; mouth 通道经 hnMeshParam 桥验证几何。
 *
 * 用法(由 tools/live2d_probe.sh 编译运行; cwd = 仓库根):
 *   cc -std=c99 -O2 tools/live2d_probe.c <引擎核心源> -lm -o live2d_probe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "hn_internal.h"

static int g_pass = 0, g_fail = 0;

static void ck(int cond, const char *label) {
    printf("%s %s\n", cond ? "  v" : "  X", label);
    if (cond) g_pass++; else g_fail++;
}

static void ckf(float got, float want, float tol, const char *label) {
    int ok = fabsf(got - want) <= tol;
    printf("%s %s (got %.4f want %.4f tol %.3f)\n", ok ? "  v" : "  X", label,
           got, want, tol);
    if (ok) g_pass++; else g_fail++;
}

/* ---- 文件资产后端: cwd(仓库根)相对路径直读 ---- */

static const char *file_asset_load(void *ctx, const char *path, size_t *len) {
    (void)ctx;
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    char *d = (char *)malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return NULL; }
    fclose(f);
    if (len) *len = (size_t)n;
    return d;
}

/* 后端用 static:set_assets 只存指针, 栈局部会在 new_ctx 返回后悬垂 */
static hn_asset_backend g_asset_backend;

static hn_context *new_ctx(void) {
    hn_context *c = hn_context_create();
    memset(&g_asset_backend, 0, sizeof(g_asset_backend));
    g_asset_backend.load = file_asset_load;
    hn_context_set_assets(c, &g_asset_backend);
    return c;
}

#define MIKU_MODEL3 "examples/assets/miku/miku_pro/runtime/miku_sample_t04.model3.json"
#define MIKU_TEX    "examples/assets/miku/miku_pro/runtime/miku_sample_t04.2048/texture_00.png"

static hn_text_backend tb;

/* 建一个 mesh + live2d 文档并布局, 返回显示列表里第一条 MESH 指令的顶点
   副本(显示列表会因重布局重分配, 跨布局比较必须先拷出)。 */
static const hn_cmd *mesh_doc(const char *extra_attrs, float clock_ms,
                              float *verts_out, int *nv_out) {
    char html[1024];
    snprintf(html, sizeof(html),
        "<html><head><style>html,body{margin:0}#r{position:absolute;left:0;top:0;"
        "width:100;height:80}</style></head><body>"
        "<img id=\"r\" src=\"examples/assets/avatar-a.png\" hn-mesh=\"8x8\" %s>"
        "</body></html>", extra_attrs);
    hn_context *c = hn_context_create();
    memset(&g_asset_backend, 0, sizeof(g_asset_backend));
    g_asset_backend.load = file_asset_load;
    hn_context_set_assets(c, &g_asset_backend);
    hn_context_set_doc(c, hn_parse_html(html, strlen(html)));
    hn_node *n = hn_doc_find_by_id(hn_context_doc(c), "r");
    if (n) n->ext_clock = clock_ms;
    hn_context_layout(c, 200, 160, &tb);
    const hn_display_list *dl = hn_context_display_list(c);
    for (int i = 0; i < dl->count; i++)
        if (dl->cmds[i].kind == HN_CMD_MESH) {
            int nv = (dl->cmds[i].mesh_cols + 1) * (dl->cmds[i].mesh_rows + 1);
            memcpy(verts_out, dl->cmds[i].mesh_verts, sizeof(float) * (size_t)(nv * 2));
            *nv_out = nv;
            return &dl->cmds[i];
        }
    *nv_out = 0;
    return NULL;
}

/* 顶点 (r 行, u=0.5 中列) 的坐标(x/y 出参可为 NULL) */
static void vtx(const float *verts, int cols, int r, float *x, float *y) {
    int i = r * (cols + 1) + cols / 2;
    if (x) *x = verts[i * 2];
    if (y) *y = verts[i * 2 + 1];
}

int main(void) {
    printf("== Live2D 运行时包探针 ==\n");
    hn_live2d_cache_clear();

    printf("== [1] model3.json 字段事实(miku 官方样例) ==\n");
    hn_context *c = new_ctx();
    struct hn_live2d *m = hn_live2d_load(c, MIKU_MODEL3);
    ck(m != NULL, "model3.json 加载成功");
    hn_live2d_info info;
    hn_live2d_get_info(m, &info);
    ck(info.has_moc == 1, "FileReferences.Moc 已声明(moc3 本体不解析, 只记事实)");
    ck(info.textures == 1, "Textures 贴图数 = 1(2048 atlas)");
    ck(info.has_physics == 1, "physics3.json 已解析");
    ck(info.physics_settings == 13, "physics 物理设置数 = 13");
    ck(info.physics_inputs == 37, "physics 输入总数 = 37");
    ck(info.physics_outputs == 51, "physics 输出总数 = 51");
    ck(info.physics_vertices == 71, "physics 顶点总数 = 71");
    ck(info.has_display_info == 1, "cdi3.json 已解析");
    ck(info.cdi3_params == 59, "cdi3 参数条数 = 59");
    ck(info.motion_groups == 4, "Motions 组数 = 4(Idle/Tap/Flick/FlickUp)");
    ck(info.motions == 8, "motion3 文件数 = 8");
    ck(info.curves_total == 8 * 26, "曲线总数 = 8 x 26 = 208");
    ck(info.duration_max > 5.3f && info.duration_max < 5.5f, "最长 motion ≈ 5.4s(miku_08)");

    printf("== [2] cdi3 参数清单 ==\n");
    const char *nm = hn_live2d_cdi3_name(m, "ParamAngleX");
    ck(nm && !strcmp(nm, "\xe8\xa7\x92\xe5\xba\xa6 X"), "ParamAngleX 显示名 = \"角度 X\"");
    nm = hn_live2d_cdi3_name(m, "ParamMouthOpenY");
    ck(nm && strstr(nm, "\xe5\x8f\xa3") != NULL, "ParamMouthOpenY 显示名含\"口\"");
    ck(hn_live2d_cdi3_name(m, "ParamNoSuch") == NULL, "未知参数 → NULL(不崩)");

    printf("== [3] Motion 组选取 ==\n");
    int idle0 = hn_live2d_motion_pick(m, "Idle", 0);
    int tap0 = hn_live2d_motion_pick(m, "Tap", 0);
    int flickup0 = hn_live2d_motion_pick(m, "FlickUp", 0);
    const char *f_idle = hn_live2d_motion_file(m, idle0);
    const char *f_tap = hn_live2d_motion_file(m, tap0);
    const char *f_fu = hn_live2d_motion_file(m, flickup0);
    ck(idle0 >= 0 && f_idle && strstr(f_idle, "miku_01.motion3.json") != NULL,
       "Idle[0] → motion/miku_01.motion3.json");
    ck(tap0 >= 0 && f_tap && strstr(f_tap, "miku_02.motion3.json") != NULL,
       "Tap[0] → motion/miku_02.motion3.json");
    ck(flickup0 >= 0 && f_fu && strstr(f_fu, "miku_06.motion3.json") != NULL,
       "FlickUp[0] → motion/miku_06.motion3.json");
    ck(hn_live2d_motion_pick(m, "NoSuch", 0) == -1, "未声明组 → -1");
    ck(fabsf(hn_live2d_motion_duration(m, idle0) - 2.4f) < 0.001f,
       "miku_01 Duration = 2.4s");

    printf("== [4] motion3 求值器(真实 motion) ==\n");
    float v = 0;
    /* 贝塞尔段中点: 端点 (0,0)→(0.3,-14), 控制点 (0.1,0),(0.2,-14);
       u=0.5 处 x=0.15, y=(0+0+3x(-14)+(-14))/8 = -7 —— 手工可算的中值 */
    ckf(hn_live2d_motion_sample(m, tap0, "ParamAngleX", 0.15f, &v) ? v : 1e9f,
        -7.0f, 0.05f, "miku_02 ParamAngleX(0.15) = -7(贝塞尔中点=中值)");
    ckf(hn_live2d_motion_sample(m, tap0, "ParamAngleX", 0.30f, &v) ? v : 1e9f,
        -14.0f, 0.05f, "miku_02 ParamAngleX(0.30) = -14(段终点值)");
    ckf(hn_live2d_motion_sample(m, tap0, "ParamEyeLOpen", 0.2335f, &v) ? v : 1e9f,
        0.5f, 0.02f, "miku_02 ParamEyeLOpen(0.2335) = 0.5(贝塞尔中点=中值)");
    /* Loop 回绕: miku_01 时长 2.4s, t=2.6 与 t=0.2 同值 */
    float va = 0, vb = 0;
    hn_live2d_motion_sample(m, idle0, "ParamAngleY", 0.2f, &va);
    hn_live2d_motion_sample(m, idle0, "ParamAngleY", 2.6f, &vb);
    ckf(vb, va, 0.001f, "Loop 回绕: ParamAngleY(2.6) = ParamAngleY(0.2)");
    ck(hn_live2d_motion_sample(m, idle0, "ParamNoSuch", 0.1f, &v) == 0,
       "无该曲线 → 返回 0(不崩)");

    printf("== [5] motion3 求值器(合成四段型夹具) ==\n");
    hn_context *cf = new_ctx();
    struct hn_live2d *mf = hn_live2d_load(cf, "examples/fixtures/live2d/test_model3.json");
    ck(mf != NULL, "合成 model3(单 Test 组)加载成功");
    int ti = hn_live2d_motion_pick(mf, "Test", 0);
    /* Segments: [0,10] 线性→(1,20) 阶梯→(2,0) 贝塞尔(2.25,0)(2.75,40)→(3,40)
       反阶梯→(3.5,0); Duration 3.5, Loop */
    ckf(hn_live2d_motion_sample(mf, ti, "ParamTest", 0.5f, &v) ? v : 1e9f,
        15.0f, 0.01f, "线性段中点 = 中值((10+20)/2 = 15)");
    ckf(hn_live2d_motion_sample(mf, ti, "ParamTest", 1.5f, &v) ? v : 1e9f,
        20.0f, 0.001f, "阶梯段 = 前值(20 保持)");
    ckf(hn_live2d_motion_sample(mf, ti, "ParamTest", 1.999f, &v) ? v : 1e9f,
        20.0f, 0.001f, "阶梯段终点前一刻仍是前值");
    ckf(hn_live2d_motion_sample(mf, ti, "ParamTest", 2.0f, &v) ? v : 1e9f,
        0.0f, 0.001f, "阶梯段到终点跳变为 0");
    ckf(hn_live2d_motion_sample(mf, ti, "ParamTest", 2.5f, &v) ? v : 1e9f,
        20.0f, 0.05f, "贝塞尔段中点 = 中值((0+3x40+40)/8 = 20)");
    ckf(hn_live2d_motion_sample(mf, ti, "ParamTest", 3.05f, &v) ? v : 1e9f,
        0.0f, 0.001f, "反阶梯段立即取新值(0)");
    ckf(hn_live2d_motion_sample(mf, ti, "ParamTest", 4.0f, &v) ? v : 1e9f,
        15.0f, 0.01f, "Loop 回绕: t=4.0 → t=0.5 → 15");

    printf("== [6] 负例 ==\n");
    struct hn_live2d *bad = hn_live2d_load(c, "examples/fixtures/live2d/nope.model3.json");
    ck(bad == NULL, "不存在的 model3 → NULL");
    bad = hn_live2d_load(c, "examples/fixtures/inc-card.html");
    ck(bad == NULL, "非 model3 JSON → NULL(motions 缺失)");

    printf("== [7] rig 接线: 顶点随官方 motion 变化 ==\n");
    /* 三个文档: A=无 motion(纯程序化, 参数全 0) / B=Idle@267ms(眨眼+转头)
       / C=Idle@0(中性位)。8x8 网格, 盒 100x80。 */
    static float va_[1024], vb_[1024], vc_[1024];
    int na = 0, nb = 0, nc = 0;
    mesh_doc("", 0.0f, va_, &na);
    mesh_doc("hn-live2d=\"" MIKU_MODEL3 "\" hn-live2d-motion=\"Idle\"", 0.0f, vc_, &nc);
    mesh_doc("hn-live2d=\"" MIKU_MODEL3 "\" hn-live2d-motion=\"Idle\"", 267.0f, vb_, &nb);
    ck(na == nb && nb == nc && na == 81, "三文档 MESH 顶点数一致(81 顶点)");
    int same_ac = 1;
    for (int i = 0; i < na * 2; i++)
        if (va_[i] != vc_[i]) { same_ac = 0; break; }
    ck(same_ac, "中性位(t=0 全参数 0)与无 motion 逐位一致(零缺省回归)");
    int diff_ab = 0;
    for (int i = 0; i < na * 2; i++)
        if (fabsf(va_[i] - vb_[i]) > 0.001f) { diff_ab = 1; break; }
    ck(diff_ab, "眨眼+转头时刻(267ms)顶点整体随 motion 变化");
    /* 眼通道: miku_01 t=0.267 → EyeLOpen/EyeROpen = 0 → 闭眼量 1,
       眼线行(v=0.625)行程 = 1 x 0.025 x 80 x kernel(≈0.991) ≈ 1.98px。
       同帧 AngleY=-16 还叠加 look_y 上移(-16/30 x 2.5% 盒高 x 行权重
       0.35+0.65x0.625 ≈ -0.81px) —— 通道是叠加的, 期望值取两者合成。 */
    float ay_by_lw = 0.35f + 0.65f * (5.0f / 8.0f);
    float ay_by = (-16.0f / 30.0f) * 0.025f * 80.0f * ay_by_lw;
    float ax, ay, bx, by;
    vtx(va_, 8, 5, &ax, &ay);
    vtx(vb_, 8, 5, &bx, &by);
    ckf(by - ay, 0.025f * 80.0f * 0.99076f + ay_by, 0.05f,
       "眨眼+转头合成: 眼线行 y 位移 = 闭眼下压 1.98 - 转头上移 0.81(通道叠加)");
    (void)ax; (void)bx;
    /* 体摆通道: miku_01 t=0.267 → AngleZ = -17 → body_x = -17/30,
       均匀平移 = -17/30 x 0.05 x 100 = -2.833px(底行 v=0 也动 —— 均匀) */
    float a0x, a0y, b0x, b0y;
    vtx(va_, 8, 0, &a0x, &a0y);
    vtx(vb_, 8, 0, &b0x, &b0y);
    ckf(b0x - a0x, -17.0f / 30.0f * 0.05f * 100.0f, 0.05f,
       "体摆: 底行顶点 x 平移 = AngleZ/30 x 5% 盒宽(-2.83px)");
    (void)a0y; (void)b0y;
    /* mouth 通道几何(miku_06 的口型曲线经 API 钉值 0.9995@0.23s;
       这里用桥直写同一量纲验证几何 —— wiring 行 p_mouth += v 由上面
       diff_ab 与 API 断言共同覆盖): open=0.9995 → 口带 r=6 行(v=0.75)
       下压 = 0.9995 x 0.035 x 80 x kv(0.91)(du=0) ≈ 2.29px */
    static float vm_[1024];
    int nm2 = 0;
    mesh_doc("", 0.0f, vm_, &nm2);   /* 基准(等价 va_) */
    hn_context *cm = new_ctx();
    {
        const char *html =
            "<html><head><style>html,body{margin:0}#r{position:absolute;left:0;top:0;"
            "width:100;height:80}</style></head><body>"
            "<img id=\"r\" src=\"examples/assets/avatar-a.png\" hn-mesh=\"8x8\"></body></html>";
        hn_context_set_doc(cm, hn_parse_html(html, strlen(html)));
        hn_node *n = hn_doc_find_by_id(hn_context_doc(cm), "r");
        hn_node_set_mesh_param(n, "mouth", 0.9995f);
        hn_context_layout(cm, 200, 160, &tb);
        const hn_display_list *dl = hn_context_display_list(cm);
        for (int i = 0; i < dl->count; i++)
            if (dl->cmds[i].kind == HN_CMD_MESH) {
                memcpy(vm_, dl->cmds[i].mesh_verts, sizeof(float) * (size_t)(nm2 * 2));
                break;
            }
    }
    float my0, mym;
    vtx(va_, 8, 6, NULL, &my0);
    vtx(vm_, 8, 6, NULL, &mym);
    ckf(mym - my0, 0.9995f * 0.035f * 80.0f * (1.0f - 0.3f * 0.3f) * (1.0f - 0.3f * 0.3f) *
                   (1.0f - 0.3f * 0.3f), 0.05f,
       "口型: 口带行顶点下压 = open x 3.5% 盒高 x 核(≈2.29px)");
    /* eye 桥: "eye" 传开合度, 0 = 全闭 → 与 motion 眨眼同几何 */
    hn_node_set_mesh_param(hn_doc_find_by_id(hn_context_doc(cm), "r"), "eye", 0.0f);
    hn_context_layout(cm, 200, 160, &tb);
    {
        const hn_display_list *dl = hn_context_display_list(cm);
        for (int i = 0; i < dl->count; i++)
            if (dl->cmds[i].kind == HN_CMD_MESH) {
                memcpy(vm_, dl->cmds[i].mesh_verts, sizeof(float) * (size_t)(nm2 * 2));
                break;
            }
    }
    float ey;
    vtx(vm_, 8, 5, NULL, &ey);
    ckf(ey - ay, 0.025f * 80.0f * 0.99076f, 0.05f,
       "eye 桥(开合度 0 = 全闭)与 motion 眨眼同几何(1.98px)");
    hn_live2d_cache_clear();
    hn_live2d_free(m);
    hn_live2d_free(mf);

    printf("\n通过 %d / 失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

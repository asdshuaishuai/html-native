/* hn_three.c — three.js 子集: 场景图 + 透视相机 + 几何(box/plane/sphere)
 *              + 轨道相机 + 纹理贴图
 * ============================================================
 * docs/threejs-support.md 的软件渲染路径。本引擎无 WebGL(该文档口径),
 * three.js 类 3D 的现实路径是**CPU 侧几何 + 引擎既有光栅**。本模块做:
 *
 *   1. Scene 场景图: t3_node 父子树, 局部 TRS → mat4 逐级复合
 *      (Scene → pivot Group → Mesh(BoxGeometry), 与 three.js 同构)
 *   2. PerspectiveCamera: fov/aspect/near/far + lookAt。
 *      hn-three-camera-orbit 声明后视点走**轨道相机**(方位角/仰角/距离
 *      球坐标): 指针拖拽驱动(hn_context_set_pointer 已有的指针坐标 +
 *      hn_context_hit_node 的元素级命中), 松手后角速度指数衰减(惯性)。
 *      状态存本模块进程级槽表(键 = 文档+节点+arena 三重校验, 见
 *      t3_orbit 一节) —— hn_node 结构体不归本文件所有, 不加字段。
 *      未声明该属性的元素视点仍为编译期常量(逐位不变)。
 *   3. 软件三角光栅: 顶点汤(世界→视→裁剪→NDC→屏幕) → 无贴图走
 *      **HN_CMD_POLYGON 3 顶点三角形**(复用 paint_polygon 既有扫描
 *      内核, 不新写扫描线); 有贴图走 **HN_CMD_TRIS 贴图三角汤**
 *      (hnsoft.c paint_tris: 逐像素重心 UV 采样 + 解析 AA, Cubism
 *      同一颗内核)。
 *      为何两条通道: TRIS 契约是"逐像素 UV 贴图三角汤"(text 缺图 →
 *      静默跳过), 无贴图的逐面平直着色它表达不了; POLYGON 的纯色
 *      填充正是无贴图路径要的通道。有贴图时 Lambert 明暗不参与
 *      (TRIS 内核无逐顶点色输入) —— 贴图路径 = unlit, 如实声明。
 *   4. 几何: hn-three-geo="box|plane|sphere"(缺省 box; 未识别值回退
 *      box, 与 CSS 未知值回退同口径)。box = 既有 8 顶点 12 三角逐面
 *      表(审计过的常量, 未动); plane = 1.3×1.3 双面 2 三角; sphere =
 *      UV 球 32×16 段(960 三角, 极带退化四边形只出单三角), 半径 0.72。
 *      基色: box 沿用六色面板; plane/sphere 单色(可被 hn-three-color
 *      ="#rrggbb" 覆盖全部几何)。几何表惰性构建一次(纯算术, 进程级
 *      常量 —— 与 HN_CLOCK 无关, 不影响确定性)。
 *   5. 贴图: hn-three-tex="path" → **资产后端**取字节(hn_context_assets,
 *      引擎自身不做 I/O 的既有依赖倒置) → hn_png_decode 验证可达且
 *      可解码(PNG 口径) → 每三角角点 UV → HN_CMD_TRIS(text=同一路径,
 *      hnsoft 按路径经自己的解码缓存取样)。解码双重发生是**通道形状
 *      所限**: TRIS 指令只有贴图路径字段没有像素指针(hn.h 公共契约,
 *      本文件不得扩); 资产后端解码是"可贴图"的诚实门 —— 失败(缺
 *      后端/文件不可达/非 PNG)→ 回退 POLYGON Lambert 纯色, 不静默
 *      空白。标准运行时(rt)的资产后端与 hnsoft 图片缓存同一 cwd 相对
 *      路径空间(hn_app 先 chdir 文档目录), 路径两端一致。
 *
 * 逐面明暗(Lambert: ambient + max(0,n·L))在**本模块 CPU**算好进
 * fill 颜色(无贴图路径) —— 引擎光栅仍无光照模型, 文档"无光照"口径不变。
 *
 * 确定性: 自转角 = 节点 ext_clock(HN_CLOCK / --advance 取样,
 * 与 hn-mesh/Lottie 同一环境动画通道, 见 hn_context.c anim_walk)。
 * 轨道相机初值 = 属性常量; 指针拖拽/惯性只由交互事件驱动 —— 无指针
 * 事件的离屏 shot(has_pointer=0)恒停在初值, HN_CLOCK/--advance
 * 双 shot 逐字节确定的口径不变。惯性积分的 dt 同取 ext_clock 差,
 * 与自转同一时钟源。
 *
 * 常驻开销: 惰性几何常量表(box/sphere 首用构建, 数十 KB .bss)+ 进程级
 * 缓存槽(neb3 场景 4 槽 / 轨道状态 8 槽 / 贴图门 4 槽, 均有界); 顶点汤
 * 进引擎临时区(生命周期 = 本次显示列表)。未声明 hn-three 的页面不进
 * 任何新代码路径。
 *
 * 已知边界(诚实清单):
 *   - 近平面按"整三角剔除"处理(任一顶点 clip.w ≤ 0 丢弃), 未做剖分;
 *     demo 相机距离下永不触发, 语义边界如实声明
 *   - 遮挡 = 画家算法(视空间质心 z 远→近发射), 面片级, 凸几何**全量
 *     三角不剔除**(见 t3_paint_soft 注 —— 近面覆盖远面, 与 z-buffer
 *     逐像素同像, 离散采样无剪影缺口); 凹体/互穿面片不保证逐像素正确
 *     (与 CSS 3D 同限); 球体 960 三角的插入排序 O(n²) 在面片数上是
 *     可测量代价
 *   - 贴图路径 unlit(见上第 3 点); 贴图格式 = PNG(引擎自带解码器口径)
 *   - 轨道拖拽的"抓住"判定 = hn_context_hit_node(指针命中的最深元素
 *     == 本元素); 元素被子元素覆盖处按下不抓 —— 与 click 命中同一语义,
 *     不另设第二套命中事实源
 *   - 轨道/贴图/几何扩展是**软件路径**能力: neb3 backend(Phase A)只会
 *     装配缺省白盒, 声明了这些属性的元素恒走软件路径(见
 *     t3_backend_capable —— backend 画不出语义不符的缺省盒, 如实降级)
 *   - 一个 hn-three 元素 = 一个场景(单 Mesh); glTF/自定义几何经
 *     hn-three-src 声明走 neb3 backend(Phase B; 本模块已解析该属性)
 *
 * ---- neb3 backend(可选 GPU 3D 运行时, 2026-10-15 接入) ----
 * backend/neb3-wgpu(Rust cdylib)按 include/neb3.h 的稳定 C ABI 导出
 * neb3_* 符号。本模块**运行期 dlopen** 装载(编译期零依赖, C99 侧不
 * 新增任何链接项):
 *
 *   - 装载: NEB3_LIB 环境变量指向 libneb3_wgpu.dylib; **未设置 → 不
 *     装载, 直接软件路径**(文档口径"未设置 → C99 兜底"; 不试缺省路径,
 *     不写死本机/仓库布局的绝对路径 —— 2026-10-15 复审修复)。已设置
 *     时 dlsym 全部 neb3_* 符号, 任一缺失 → 软件路径, stderr 一行
 *     "[neb3] fallback 原因"(进程级只打一次; 装载结果缓存, 不逐帧
 *     重试)。
 *   - 有 backend: 场景经 C ABI 装配(相机 fov50/盒比、自转 = 节点
 *     ext_clock、unlit 材质、深度开)→ neb3_render 出 RGBA8 非预乘帧
 *     → 拷进引擎 tmp 区(生命周期 = 显示列表, 与媒体双缓冲同一契约)
 *     → HN_CMD_BITMAP 上屏。帧不完整(字节不足 W×H×4, 如 Phase A
 *     骨架的空帧)同样按 fallback 处理一行后回软件路径。
 *   - 无 backend: 下面的 POLYGON 软件路径逐位不变(体现代码本体就在
 *     t3_paint_soft, 未动一行算术)。
 *   - 新属性: hn-three-msaa="4"(MSAA 采样数, 缺省 4, 合法 1/2/4/8,
 *     backend 可按设备能力降级)、hn-three-src(模型/场景资源路径,
 *     Phase B 消费 —— 先解析不生效)、透明背景(元素级联后无
 *     background/gradient 时 clear alpha=0; 有底色则以底色做不透明
 *     clear —— 透明帧与已画背景在光栅侧 alpha 混合, 两种口径合成
 *     都正确)。
 *   - 确定性: neb3_render 的 time_ms = 节点 ext_clock —— 与软件路径
 *     的自转角同一时钟源。HN_CLOCK(hncore)与 --advance(hn_app)都
 *     经 anim_walk 推进 ext_clock, 同参数双 shot 逐字节一致。
 */
/* dlopen/dlsym 是 POSIX 而非 C99: glibc 在 strict -std=c99 下要求
   _POSIX_C_SOURCE ≥ 200809L 才声明(Darwin libc 恒可见, 无需宏)。
   Windows 目标(zig cc *-windows-gnu 等)没有 dlfcn.h: neb3 backend
   在 Windows 不可装载 —— 本文件按 _WIN32 编译期门控, 该平台运行期
   恒走下面的 POLYGON 软件路径(2026-10-15 复审修复: 此前无条件
   #include <dlfcn.h> 打破 build-multiplatform.sh 的两个 Windows
   目标; hn_three.c 因 paint_walk 硬调 hn_three_paint 不能移出其
   Windows 源清单)。 */
#if !defined(_WIN32) && !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _WIN32
#include <dlfcn.h>
#endif
#include <math.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hn_internal.h"
#include "neb3.h"   /* 稳定 C ABI 的 POD 类型(neb3_desc/color/render_info);
                       函数一律经 dlsym 指针调用, 不产生链接依赖 */
#include "hn_png.h" /* hn_png_decode: hn-three-tex 的"可达且可解码"门
                       (引擎自带 PNG 解码器, hnsoft 同一款; 两构建清单
                       均已链 hn_png.c, 不新增链接项) */

#define T3_PI 3.14159265358979f

/* ---- neb3 运行时装载(进程级一次; 失败即缓存, 软件路径兜底) ---- */

/* 后端库路径只认 NEB3_LIB 环境变量, 不设缺省路径(2026-10-15 复审
   修复: 此前缺省写死本 checkout 的绝对路径且指向 target/release ——
   机器特定、树里并不存在; 文档口径是"未设置 → C99 兜底", 现与之
   一致)。 */

/* 函数指针签名一律按 include/neb3.h 的稳定 C ABI 书写: POD 结构一一
   对应。material_unlit 亦按 neb3_color 结构传值(2026-10-15 复审修复:
   此前按 lib.rs 旧实现写 4 个 float —— arm64 HFA 恰好同走 s0..s3,
   SysV x86-64 下结构走单个 xmm、4 个标量走 xmm0-3, 属 ABI 不符;
   ABI 的单一来源是 neb3.h, 实现侧 lib.rs 已同批对齐)。 */
typedef void *(*t3_neb3_create)(const neb3_desc *desc);
typedef void (*t3_neb3_destroy)(void *rt);
typedef void *(*t3_neb3_scene_create)(void *rt);
typedef void (*t3_neb3_scene_destroy)(void *scene);
typedef void *(*t3_neb3_box_mesh)(void *rt, float sx, float sy, float sz);
typedef void (*t3_neb3_mesh_destroy)(void *mesh);
typedef void *(*t3_neb3_material_unlit)(void *rt, neb3_color color);
typedef void (*t3_neb3_material_destroy)(void *material);
typedef int (*t3_neb3_scene_add_box)(void *scene, void *mesh, void *material);
typedef int (*t3_neb3_scene_set_camera)(void *scene, float fov_deg, float aspect,
                                        float near_z, float far_z);
typedef int (*t3_neb3_scene_set_rotation)(void *scene, float radians);
typedef int (*t3_neb3_scene_set_depth)(void *scene, int enabled);
typedef int (*t3_neb3_render)(void *scene, float time_ms, const neb3_color *clear,
                              const unsigned char **rgba, size_t *bytes,
                              neb3_render_info *info);
/* 可选符号(2026-10-22 起后端提供; 旧后端 NULL → 读回路径):
   原生纹理直入(macOS IOSurface 互操作, 零 CPU 回读)。 */
typedef int (*t3_neb3_render_native)(void *scene, float time_ms,
                                     const neb3_color *clear, neb3_native *out);
typedef void (*t3_neb3_release_frame)(void *scene);
typedef const char *(*t3_neb3_last_error)(void *rt);

struct t3_neb3_api {
    t3_neb3_create                 create;
    t3_neb3_destroy                destroy;
    t3_neb3_scene_create           scene_create;
    t3_neb3_scene_destroy          scene_destroy;
    t3_neb3_box_mesh               box_mesh;
    t3_neb3_mesh_destroy           mesh_destroy;
    t3_neb3_material_unlit         material_unlit;
    t3_neb3_material_destroy       material_destroy;
    t3_neb3_scene_add_box          scene_add_box;
    t3_neb3_scene_set_camera       scene_set_camera_perspective;
    t3_neb3_scene_set_rotation     scene_set_rotation;
    t3_neb3_scene_set_depth        scene_set_depth;
    t3_neb3_render                 render;
    t3_neb3_release_frame          release_frame;
    t3_neb3_render_native          render_native;   /* 可选; NULL = 旧后端 */
    t3_neb3_last_error             last_error;
};

static struct t3_neb3_api g_neb3;
static int g_neb3_state = 0;   /* 0=未试 1=可用 2=回退(软件路径) */

static void t3_neb3_release_all_slots(void);   /* 定义在槽表之后 */

/* 一行原因(stderr; 进程级一次 —— g_neb3_state 置 2 后不再进入本函数)。
 * 置 2 即清空全部缓存槽(2026-10-15 复审修复: 此前只释放触发失败的那一
 * 槽, 其余 ≤3 槽已装配的 rt/scene/mesh/mat 在 state=2 后永不可达 ——
 * hn_three_paint 不再进入 t3_paint_backend —— 进程长驻(daemon)时整链
 * 悬留)。安全性: 槽只在 state==1 时被 t3_paint_backend 填充, 装载期
 * 回退(dlopen/缺符号)时槽恒为全零, slot_release 逐字段判空, 不会调
 * 到半装载的符号表。 */
static void t3_neb3_fallback(const char *why) {
    g_neb3_state = 2;
    t3_neb3_release_all_slots();
    fprintf(stderr, "[neb3] fallback: %s\n", why);
}

/* 符号表: 名字 + 在 api 结构里的槽位。全部 neb3_* 必须齐 —— 缺任一
   即回退(半套符号的 backend 比没有更危险)。 */
static const struct { const char *name; size_t off; } T3_NEB3_SYMS[] = {
#define T3_SYM(f) { "neb3_" #f, offsetof(struct t3_neb3_api, f) },
    T3_SYM(create) T3_SYM(destroy) T3_SYM(scene_create) T3_SYM(scene_destroy)
    T3_SYM(box_mesh) T3_SYM(mesh_destroy) T3_SYM(material_unlit)
    T3_SYM(material_destroy) T3_SYM(scene_add_box)
    T3_SYM(scene_set_camera_perspective) T3_SYM(scene_set_rotation)
    T3_SYM(scene_set_depth) T3_SYM(render) T3_SYM(release_frame)
    T3_SYM(last_error)
#undef T3_SYM
};

static void t3_neb3_load(void) {
    const char *path = getenv("NEB3_LIB");
    if (!path || !*path) {
        /* 未设置 = 缺省形态而非故障: 直接软件路径, 不刷 stderr
           (state=2 缓存决策, 不逐帧重试)。 */
        g_neb3_state = 2;
        return;
    }
#ifdef _WIN32
    /* Windows 无 dlopen(见文件头门控说明): NEB3_LIB 在场也无法
       装载, 按装载失败同一契约处理 —— stderr 一行后恒走软件路径。 */
    char why[512];
    snprintf(why, sizeof(why), "%s 打不开: Windows 无 dlopen, NEB3 backend 不可用", path);
    t3_neb3_fallback(why);
    return;
#else
    void *h = dlopen(path, RTLD_NOW);
    if (!h) {
        char why[512];
        const char *e = dlerror();
        snprintf(why, sizeof(why), "%s 打不开: %s", path, e ? e : "dlopen 失败");
        t3_neb3_fallback(why);
        return;
    }
    for (size_t i = 0; i < sizeof(T3_NEB3_SYMS) / sizeof(T3_NEB3_SYMS[0]); i++) {
        void *sym = dlsym(h, T3_NEB3_SYMS[i].name);
        if (!sym) {
            char why[256];
            snprintf(why, sizeof(why), "%s 缺符号 %s", path, T3_NEB3_SYMS[i].name);
            t3_neb3_fallback(why);
            dlclose(h);
            return;
        }
        /* dlsym 返回的 void* 经 memcpy 装入函数指针槽(POSIX 认可的
           装载手法; 直接 cast 在 ISO C 是约束违例, memcpy 无警告) */
        memcpy((char *)&g_neb3 + T3_NEB3_SYMS[i].off, &sym, sizeof(sym));
    }

    /* 可选符号: 旧后端(原生纹理直入之前)没有 → NULL = 读回路径 */
    g_neb3.render_native = (t3_neb3_render_native)dlsym(h, "neb3_render_native");
    g_neb3_state = 1;
    fprintf(stderr, "[neb3] backend: %s\n", path);
#endif
}

/* 装载探测(供宿主在起跑线调用一次; 惰性路径同样会触发)。 */
void hn_three_backend_probe(void) {
    if (!g_neb3_state) t3_neb3_load();
}

/* ---- backend 场景对象缓存(进程级; 尺寸/属性变了才重建) ---- */

/* 小槽位缓存(2026-10-15 复审修复: 此前单槽, 同页 ≥2 个 hn-three 元素
   只要尺寸/MSAA/背景任一不同就逐帧互相踢出, 每元素每帧整链
   destroy+create)。键 = (w,h,msaa,transparent); 槽满逐出最久未用。
   槽位数覆盖常见"同页少数几个 3D 元素", 更多的页按 LRU 抖动, 不破
   正确性。 */
#define T3_NEB3_SLOTS 4
static struct {
    int w, h, msaa, transparent;
    void *rt, *scene, *mesh, *mat;
    unsigned long tick;         /* 最近命中的单调序(槽满逐出最小者) */
} g_neb3_slots[T3_NEB3_SLOTS];
static unsigned long g_neb3_tick = 0;

static void t3_neb3_slot_release(int i) {
    if (i < 0 || i >= T3_NEB3_SLOTS) return;
    if (g_neb3_slots[i].scene) g_neb3.scene_destroy(g_neb3_slots[i].scene);
    if (g_neb3_slots[i].mesh) g_neb3.mesh_destroy(g_neb3_slots[i].mesh);
    if (g_neb3_slots[i].mat) g_neb3.material_destroy(g_neb3_slots[i].mat);
    if (g_neb3_slots[i].rt) g_neb3.destroy(g_neb3_slots[i].rt);
    memset(&g_neb3_slots[i], 0, sizeof(g_neb3_slots[i]));
}

/* 全槽整链释放(永久回退时由 t3_neb3_fallback 调用; 已清槽再放是全零
   no-op, 触发槽先单放的路径不双 free)。 */
static void t3_neb3_release_all_slots(void) {
    for (int i = 0; i < T3_NEB3_SLOTS; i++) t3_neb3_slot_release(i);
}

/* MSAA 采样数: hn-three-msaa, 缺省 4(neb3 契约口径), 合法 1/2/4/8
   就近归格; "0"/"1" = 无 MSAA。backend 仍可按设备能力再降级。 */
static int t3_msaa_of(hn_node *n) {
    const char *v = hn_node_attr(n, "hn-three-msaa");
    if (!v || !*v) return 4;
    int m = atoi(v);
    if (m <= 1) return 1;
    if (m >= 8) return 8;
    if (m >= 4) return 4;
    return 2;
}

/* 透明背景: 元素级联后无 background/gradient(页面无底色的引擎侧口径)
   → clear alpha=0; 有底色 → 以底色做不透明 clear。祖先底色已由显示
   列表先画, 透明帧在光栅侧 alpha 混合透出它 —— 两态合成均正确。 */
static int t3_transparent_of(hn_node *n) {
    return ((n->style.background & 0xFFu) == 0 && !n->style.has_gradient) ? 1 : 0;
}

/* backend 绘制路径: 装配场景 → neb3_render → 帧拷进 tmp 区 → BITMAP。
   返回 1 = 已发帧; 0 = 本帧不可用(调用方落回软件路径)。 */
static int t3_paint_backend(hn_context *c, hn_node *n,
                            float bw, float bh, float sx, float sy, float alpha) {
    int W = (int)bw, H = (int)bh;
    int msaa = t3_msaa_of(n);
    int transparent = t3_transparent_of(n);

    /* hn-three-src: 模型/场景资源路径(Phase B 的 glTF 通道)。
       Phase A 只解析不消费 —— 声明不报错、不生效, 与文档口径一致。 */
    const char *src = hn_node_attr(n, "hn-three-src");
    (void)src;

    /* 自转角: 与软件路径同一公式、同一时钟源(ext_clock) */
    float spin_s = 6.0f;
    const char *sp = hn_node_attr(n, "hn-three-spin");
    if (sp && *sp) {
        spin_s = (float)atof(sp);
        if (spin_s < 0.5f) spin_s = 0.5f;
    }
    float spin_rad = (n->ext_clock / (spin_s * 1000.0f)) * 2.0f * T3_PI;

    /* 对象缓存: 键 (尺寸, msaa, transparent) 命中即复用; 未命中取空
       槽, 全满逐出最久未用的一槽 */
    int slot = -1;
    for (int i = 0; i < T3_NEB3_SLOTS; i++) {
        if (!g_neb3_slots[i].rt) continue;
        if (g_neb3_slots[i].w == W && g_neb3_slots[i].h == H &&
            g_neb3_slots[i].msaa == msaa &&
            g_neb3_slots[i].transparent == transparent) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        for (int i = 0; i < T3_NEB3_SLOTS; i++)
            if (!g_neb3_slots[i].rt) { slot = i; break; }
        if (slot < 0) {
            slot = 0;
            for (int i = 1; i < T3_NEB3_SLOTS; i++)
                if (g_neb3_slots[i].tick < g_neb3_slots[slot].tick) slot = i;
            t3_neb3_slot_release(slot);
        }
    }
    if (!g_neb3_slots[slot].rt) {
        neb3_desc d;
        memset(&d, 0, sizeof(d));
        d.width = W;
        d.height = H;
        d.msaa_samples = msaa;
        d.transparent = transparent;
        d.pixel_ratio = 1.0f;
        void *rt = g_neb3.create(&d);
        if (!rt) {
            t3_neb3_fallback("neb3_create 失败");
            return 0;
        }
        void *sc = g_neb3.scene_create(rt);
        void *mesh = sc ? g_neb3.box_mesh(rt, 1.0f, 1.0f, 1.0f) : NULL;
        neb3_color white;
        memset(&white, 0, sizeof(white));
        white.r = white.g = white.b = white.a = 1.0f;
        void *mat = mesh ? g_neb3.material_unlit(rt, white) : NULL;
        if (!sc || !mesh || !mat ||
            !g_neb3.scene_add_box(sc, mesh, mat) ||
            !g_neb3.scene_set_depth(sc, 1)) {
            t3_neb3_fallback("场景装配失败(scene/mesh/material/add_box)");
            if (mat) g_neb3.material_destroy(mat);
            if (mesh) g_neb3.mesh_destroy(mesh);
            if (sc) g_neb3.scene_destroy(sc);
            g_neb3.destroy(rt);
            return 0;
        }
        g_neb3_slots[slot].rt = rt;
        g_neb3_slots[slot].scene = sc;
        g_neb3_slots[slot].mesh = mesh;
        g_neb3_slots[slot].mat = mat;
        g_neb3_slots[slot].w = W;
        g_neb3_slots[slot].h = H;
        g_neb3_slots[slot].msaa = msaa;
        g_neb3_slots[slot].transparent = transparent;
    }
    g_neb3_slots[slot].tick = ++g_neb3_tick;

    /* 每帧: 相机随盒比、自转随时钟(fov50/near0.1/far100 与软件路径同参) */
    g_neb3.scene_set_camera_perspective(g_neb3_slots[slot].scene, 50.0f,
                                        (H > 0) ? bw / bh : 1.0f, 0.1f, 100.0f);
    g_neb3.scene_set_rotation(g_neb3_slots[slot].scene, spin_rad);

    /* clear: 透明页 alpha=0; 有底色用底色不透明清屏 */
    neb3_color clear;
    memset(&clear, 0, sizeof(clear));
    if (!transparent) {
        unsigned int bg = n->style.background;
        clear.r = (float)((bg >> 24) & 0xFF) / 255.0f;
        clear.g = (float)((bg >> 16) & 0xFF) / 255.0f;
        clear.b = (float)((bg >> 8) & 0xFF) / 255.0f;
        clear.a = 1.0f;
    }

    /* 原生纹理直入(macOS IOSurface 互操作): GPU 合成可用(host 支持
       IOSurface 包裹)时零回读 —— BITMAP 只携带 IOSurface 句柄, Metal
       合成器包同表面消费(生产者 wgpu 已把帧写入, 预乘域, 与合成器
       口径一致); hnsoft 消费场景不进此路(hn_gpu_native_surface_ok
       为 false → 走下方读回路径)。失败回落读回, 不回退整链。 */
    if (g_neb3.render_native && hn_gpu_native_surface_ok() &&
        !hn_context_native_suppressed(c)) {
        neb3_native nat;
        memset(&nat, 0, sizeof(nat));
        if (g_neb3.render_native(g_neb3_slots[slot].scene, n->ext_clock,
                                 &clear, &nat) == 1 &&
            nat.iosurface && nat.width > 0 && nat.height > 0) {
            hn_cmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.kind = HN_CMD_BITMAP;
            cmd.x = sx;
            cmd.y = sy;
            cmd.w = bw;
            cmd.h = bh;
            /* bitmap = NULL: 软件回退帧跳过此框(v1 边界, roadmap 已档);
               GPU 合成是本路的唯一消费方。 */
            cmd.bitmap_w = nat.width;
            cmd.bitmap_h = nat.height;
            cmd.bitmap_stride = nat.width * 4;
            cmd.native_surface = nat.iosurface;
            cmd.bitmap_pts = (double)n->ext_clock / 1000.0;
            cmd.fill = 0xFFFFFF00u
                     | (unsigned char)((alpha >= 1.0f ? 1.0f
                                                       : (alpha < 0.0f ? 0.0f : alpha))
                                           * 255.0f + 0.5f);
            hn_paint_push(c, &cmd);
            return 1;
        }
    }

    const unsigned char *rgba = NULL;
    size_t bytes = 0;
    neb3_render_info info;
    memset(&info, 0, sizeof(info));
    /* time_ms = ext_clock: HN_CLOCK(hncore)/--advance(hn_app)推进的
       同一时钟 —— 同参数双 shot 的 neb3_render 输入逐字节一致 */
    if (!g_neb3.render(g_neb3_slots[slot].scene, n->ext_clock, &clear,
                       &rgba, &bytes, &info)) {
        char why[512];
        const char *e = g_neb3.last_error(g_neb3_slots[slot].rt);
        snprintf(why, sizeof(why), "neb3_render 失败: %s",
                 (e && *e) ? e : "无错误信息");
        /* 永久回退(state=2)后再无路径可达缓存链 —— 就地整链释放,
           不留进程期泄漏(2026-10-15 复审修复); release_frame 归还
           可能已产出的帧缓冲。 */
        g_neb3.release_frame(g_neb3_slots[slot].scene);
        t3_neb3_slot_release(slot);
        t3_neb3_fallback(why);
        return 0;
    }
    int fw = (info.width > 0) ? info.width : W;
    int fh = (info.height > 0) ? info.height : H;
    size_t need = (size_t)fw * (size_t)fh * 4;
    if (!rgba || fw <= 0 || fh <= 0 || bytes < need) {
        /* 帧不完整(Phase A 骨架的空帧等): 不可信, 一行说明后回软件路径;
           同上, 永久回退前整链释放(复审修复)。 */
        char why[160];
        snprintf(why, sizeof(why),
                 "neb3_render 帧不完整(bytes=%zu < %dx%d×4)", bytes, fw, fh);
        g_neb3.release_frame(g_neb3_slots[slot].scene);
        t3_neb3_slot_release(slot);
        t3_neb3_fallback(why);
        return 0;
    }

    /* 帧拷进引擎 tmp 区: BITMAP 指针生命周期 = 本次显示列表(与媒体
       双缓冲同契约); 同页多个 hn-three 元素各自持有拷贝, 不共享
       backend 帧缓冲(下一次 render 会覆写它)。 */
    hn_arena *tmp = hn_context_tmp(c);
    if (!tmp) return 0;
    unsigned char *dst = (unsigned char *)hn_arena_alloc(tmp, need);
    if (!dst) return 0;
    memcpy(dst, rgba, need);
    /* 真帧到手, 归还 backend 缓冲(契约: 下一次 render 前有效, 已拷完) */
    g_neb3.release_frame(g_neb3_slots[slot].scene);

    hn_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = HN_CMD_BITMAP;
    cmd.x = sx;
    cmd.y = sy;
    cmd.w = bw;
    cmd.h = bh;
    cmd.bitmap = dst;
    cmd.bitmap_stride = fw * 4;
    cmd.bitmap_w = fw;
    cmd.bitmap_h = fh;
    cmd.bitmap_pts = (double)n->ext_clock / 1000.0;
    /* hnsoft paint_bitmap 以 fill 的 alpha 字节做整层不透明度(位图像素
       自身 alpha 另计), 与元素 alpha 单次相乘 —— 不再把 alpha 乘进像素 */
    cmd.fill = 0xFFFFFF00u
             | (unsigned char)((alpha >= 1.0f ? 1.0f : (alpha < 0.0f ? 0.0f : alpha))
                               * 255.0f + 0.5f);
    hn_paint_push(c, &cmd);
    return 1;
}

/* ---- 几何(hn-three-geo): box/plane/sphere ----------------------------
 * 三张网格表统一为发射循环消费的形状; box 的顶点/三角/法线/基色沿用
 * 既有审计常量(T3_CUBE_V、T3_CUBE_T、T3_FACE_N、T3_FACE_RGB), 惰性
 * 展开成按三角的汤表; sphere/plane 惰性构建一次(纯算术常量, 与时钟
 * 无关, 不影响确定性)。.bss 常驻约 60KB(sphere 960 三角的汤表是大头)
 * —— 有界、进程级一次。 */

typedef struct { float x, y, z; } t3_vec3;

typedef struct {
    int ntri;                 /* 三角数 */
    const float *pos;         /* 顶点表(n_v*3, xyz) */
    int n_v;
    const int *ix;            /* 三角索引汤(3*ntri; 外向绕序) */
    const float *nrm;         /* 每三角单位法线(3*ntri; box = 解析面法线) */
    const float *uvt;         /* 每三角角点 UV(6*ntri; 贴图路径用) */
    const unsigned int *col;  /* 每三角基色 0xRRGGBB(3*ntri) */
    int dbl;                  /* 双面: 背面翻转法线/绕序(plane; 单面几何全量发射靠画家序) */
} t3_geom;

/* ---- box: 8 顶点 12 三角, 逐面法线与基色(既有审计常量, 未动) ----
   面序: +x -x +y -y +z -z(three.js BoxGeometry 材质组序) */

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

/* ---- plane: 1.3×1.3 双面 2 三角(PlaneGeometry 口径: 单矩形两三角) ---- */

static const float T3_PLANE_POS[12] = {
    -0.65f, -0.65f, 0.0f,    0.65f, -0.65f, 0.0f,
     0.65f,  0.65f, 0.0f,   -0.65f,  0.65f, 0.0f,
};
static const int T3_PLANE_IX[6] = { 0, 1, 2, 0, 2, 3 };   /* +z 外向绕序 */
static const float T3_PLANE_NRM[6] = { 0, 0, 1, 0, 0, 1 };
/* 贴图 v=0 = 图像顶行(hnsoft 行主取样): 平面 +y(上)边贴 v=0 */
static const float T3_PLANE_UVT[12] = {
    0, 1,   1, 1,   1, 0,      /* (v0,v1,v2) */
    0, 1,   1, 0,   0, 0,      /* (v0,v2,v3) */
};

/* ---- sphere: UV 球 32×16 段(three.js SphereGeometry(0.72,32,16) 口径)
   顶点 (ROWS+1)×(COLS+1) = 561; 极带退化四边形只出单三角 → 960 三角。
   绕序外向(顶部 tri (a,d,c) / 其余 (a,b,d)+(a,d,c)), v=0 在顶极。 */

#define T3_SP_C 32
#define T3_SP_R 16
#define T3_SP_NV ((T3_SP_R + 1) * (T3_SP_C + 1))          /* 561 */
#define T3_SP_NT (T3_SP_R * T3_SP_C * 2 - 2 * T3_SP_C)    /* 960 */

static struct {
    float pos[T3_SP_NV * 3];
    int   ix[T3_SP_NT * 3];
    float nrm[T3_SP_NT * 3];
    float uvt[T3_SP_NT * 6];
    unsigned int col[T3_SP_NT];
} g_sph;
static t3_geom g_sph_geom;
static int g_sph_ready = 0;

static void t3_sphere_build(void) {
    int p = 0;
    for (int j = 0; j <= T3_SP_R; j++) {
        float phi = T3_PI * (float)j / (float)T3_SP_R;      /* 0(顶) → π(底) */
        float sp = sinf(phi), cp = cosf(phi);
        for (int i = 0; i <= T3_SP_C; i++) {
            float th = 2.0f * T3_PI * (float)i / (float)T3_SP_C;
            g_sph.pos[p * 3]     = 0.72f * sp * cosf(th);
            g_sph.pos[p * 3 + 1] = 0.72f * cp;
            g_sph.pos[p * 3 + 2] = 0.72f * sp * sinf(th);
            p++;
        }
    }
    int t = 0;
    for (int j = 0; j < T3_SP_R; j++) {
        for (int i = 0; i < T3_SP_C; i++) {
            int a = j * (T3_SP_C + 1) + i;
            int b = a + 1, c = a + T3_SP_C + 1, d = c + 1;
            int tri[2][3];
            int nt = 0;
            if (j == 0) {                 /* 顶极带: a==b 退化, 只出 (a,d,c) */
                tri[nt][0] = a; tri[nt][1] = d; tri[nt][2] = c; nt++;
            } else if (j == T3_SP_R - 1) { /* 底极带: c==d 退化, 只出 (a,b,d) */
                tri[nt][0] = a; tri[nt][1] = b; tri[nt][2] = d; nt++;
            } else {
                tri[nt][0] = a; tri[nt][1] = b; tri[nt][2] = d; nt++;
                tri[nt][0] = a; tri[nt][1] = d; tri[nt][2] = c; nt++;
            }
            for (int k = 0; k < nt; k++) {
                const float *P0 = &g_sph.pos[tri[k][0] * 3];
                const float *P1 = &g_sph.pos[tri[k][1] * 3];
                const float *P2 = &g_sph.pos[tri[k][2] * 3];
                /* 真表面法线 = 质心径向(单位球心在原点)。不用弦平面叉积:
                   剪影细长条的弦平面指向失真, Lambert 会把地平带涂花;
                   表面法线让着色在掠射带稳定。可见性不靠法线(全量发射
                   + 画家序, 见 t3_paint_soft 注)。 */
                float cx = (P0[0] + P1[0] + P2[0]) / 3.0f;
                float cy = (P0[1] + P1[1] + P2[1]) / 3.0f;
                float cz = (P0[2] + P1[2] + P2[2]) / 3.0f;
                float nl = sqrtf(cx * cx + cy * cy + cz * cz);
                if (nl > 1e-9f) { cx /= nl; cy /= nl; cz /= nl; }
                g_sph.nrm[t * 3] = cx; g_sph.nrm[t * 3 + 1] = cy;
                g_sph.nrm[t * 3 + 2] = cz;
                /* UV: u = i/COLS(经度), v = j/ROWS(v=0 = 图像顶行 = 顶极) */
                float ju0 = (float)i / (float)T3_SP_C;
                float ju1 = (float)(i + 1) / (float)T3_SP_C;
                float jv0 = (float)j / (float)T3_SP_R;
                float jv1 = (float)(j + 1) / (float)T3_SP_R;
                int I[3] = { tri[k][0], tri[k][1], tri[k][2] };
                /* 角点 UV 按顶点的 (i,j) 归位: a=(i,j) b=(i+1,j) c=(i,j+1) d=(i+1,j+1) */
                float cu[3], cv[3];
                for (int q = 0; q < 3; q++) {
                    if (I[q] == a)      { cu[q] = ju0; cv[q] = jv0; }
                    else if (I[q] == b) { cu[q] = ju1; cv[q] = jv0; }
                    else if (I[q] == c) { cu[q] = ju0; cv[q] = jv1; }
                    else                { cu[q] = ju1; cv[q] = jv1; }
                }
                for (int q = 0; q < 3; q++) {
                    g_sph.uvt[t * 6 + q * 2]     = cu[q];
                    g_sph.uvt[t * 6 + q * 2 + 1] = cv[q];
                }
                g_sph.ix[t * 3] = I[0]; g_sph.ix[t * 3 + 1] = I[1];
                g_sph.ix[t * 3 + 2] = I[2];
                g_sph.col[t] = 0x5B8DEF;   /* 缺省基色(hn-three-color 可覆盖) */
                t++;
            }
        }
    }
    /* t == T3_SP_NT(构建账面自检; 不相等即构建 bug, 保持未就绪回退 box) */
    if (t == T3_SP_NT) {
        g_sph_geom.ntri = T3_SP_NT;
        g_sph_geom.pos = g_sph.pos; g_sph_geom.n_v = T3_SP_NV;
        g_sph_geom.ix = g_sph.ix;   g_sph_geom.nrm = g_sph.nrm;
        g_sph_geom.uvt = g_sph.uvt; g_sph_geom.col = g_sph.col;
        g_sph_geom.dbl = 0;
        g_sph_ready = 1;
    }
}

/* ---- box 惰性展开(顶点/索引/法线沿用静态表; UV/基色按三角展开) ----
   面内 UV = 两条非法线轴归一(h=0.575); 贴图整面 0..1(BoxGeometry
   每面整贴图口径)。 */

static struct {
    int   ix[36];
    float nrm[36];
    float uvt[72];
    unsigned int col[12];
} g_box;
static t3_geom g_box_geom;
static int g_box_ready = 0;

static void t3_box_build(void) {
    const float h = 0.575f;
    for (int t = 0; t < 12; t++) {
        int face = T3_CUBE_T[t].face;
        g_box.col[t] = T3_FACE_RGB[face];
        for (int k = 0; k < 3; k++) {
            int vi = T3_CUBE_T[t].v[k];
            g_box.ix[t * 3 + k] = vi;
            if (k == 0) {   /* 法线按三角存一份(box 面平直, 三角共享面法线) */
                g_box.nrm[t * 3]     = T3_FACE_N[face].x;
                g_box.nrm[t * 3 + 1] = T3_FACE_N[face].y;
                g_box.nrm[t * 3 + 2] = T3_FACE_N[face].z;
            }
            const t3_vec3 *p = &T3_CUBE_V[vi];
            float a, b;
            switch (face) {
            case 0: case 1: a = p->z; b = p->y; break;   /* ±x: u=z, v=y */
            case 2: case 3: a = p->x; b = p->z; break;   /* ±y: u=x, v=z */
            default:        a = p->x; b = p->y; break;   /* ±z: u=x, v=y */
            }
            g_box.uvt[t * 6 + k * 2]     = (a + h) / (2.0f * h);
            g_box.uvt[t * 6 + k * 2 + 1] = (b + h) / (2.0f * h);
        }
    }
    g_box_geom.ntri = 12;
    g_box_geom.pos = &T3_CUBE_V[0].x; g_box_geom.n_v = 8;
    g_box_geom.ix = g_box.ix; g_box_geom.nrm = g_box.nrm;
    g_box_geom.uvt = g_box.uvt; g_box_geom.col = g_box.col;
    g_box_geom.dbl = 0;
    g_box_ready = 1;
}

static unsigned int g_plane_col[2] = { 0x46D68C, 0x46D68C };
static t3_geom g_plane_geom = {
    2, T3_PLANE_POS, 4, T3_PLANE_IX, T3_PLANE_NRM, T3_PLANE_UVT,
    g_plane_col, 1,
};

/* 几何选择: hn-three-geo(缺省/未识别 → box, 与 CSS 未知值回退同口径) */
static const t3_geom *t3_geom_of(hn_node *n) {
    const char *g = hn_node_attr(n, "hn-three-geo");
    if (g && !strcmp(g, "sphere")) {
        if (!g_sph_ready) t3_sphere_build();
        return &g_sph_geom;
    }
    if (g && !strcmp(g, "plane"))
        return &g_plane_geom;
    if (!g_box_ready) t3_box_build();
    return &g_box_geom;
}

/* 基色覆盖: hn-three-color="#rrggbb"(严格 6 位十六进制; 非法 → 不覆盖) */
static int t3_color_override(hn_node *n, unsigned int *out) {
    const char *v = hn_node_attr(n, "hn-three-color");
    if (!v || strlen(v) != 7 || v[0] != '#') return 0;
    unsigned int rgb = 0;
    for (int i = 1; i <= 6; i++) {
        char ch = v[i];
        int d;
        if (ch >= '0' && ch <= '9') d = ch - '0';
        else if (ch >= 'a' && ch <= 'f') d = ch - 'a' + 10;
        else if (ch >= 'A' && ch <= 'F') d = ch - 'A' + 10;
        else return 0;
        rgb = (rgb << 4) | (unsigned int)d;
    }
    *out = rgb;
    return 1;
}

/* ---- 轨道相机状态(hn-three-camera-orbit) ----------------------------
 * 拖拽/惯性状态不归 hn_node 所有(结构体在 hn_internal.h, 本文件不加
 * 字段) —— 进程级槽表, 键 = (文档, 节点, 节点 arena) 三重校验:
 *   - 节点指针: 同文档内稳定(文档 arena 撞分配不回收)
 *   - 文档指针 + arena: 文档替换(hn_context_set_doc → 旧 doc free)后
 *     新文档的对象地址可能被 malloc 复用 —— 三重一致才认; 撞满三重
 *     的最坏后果是"新页面继承一个初相角", 交互态自愈, 不碰确定性
 *   - 槽满 LRU 逐出(同页 hn-three 元素个数量级; 更多按抖动, 不破正确性)
 * 抓住判定 = hn_context_hit_node(引擎现成命中, 与 hover/click 同一语义,
 * 不设第二事实源); 积分 dt = 节点 ext_clock 差 —— 与自转同一时钟源。
 * 无指针事件(has_pointer=0, 离屏 shot)恒停初值: HN_CLOCK/--advance
 * 双 shot 逐字节确定的口径不被交互态污染。 */

typedef struct {
    hn_doc *doc; hn_node *node; hn_arena *achk;
    float az, el;          /* 方位角/仰角(度) */
    float dist;            /* 轨道半径 */
    float vaz, vel;        /* 角速度(度/秒; 松手惯性) */
    int   grab;            /* 1 = 指针在本元素上(拖拽中) */
    float gx, gy;          /* 抓住期间上次指针采样 */
    float last_clock;      /* 上次取样的 ext_clock(ms; -1 = 未初始化) */
    unsigned long tick;
} t3_orbit;

#define T3_ORBIT_SLOTS 8
static t3_orbit g_orbit[T3_ORBIT_SLOTS];
static unsigned long g_orbit_tick = 0;

/* 轨道初值: 精确反解既有固定视点 eye(1.7, 2.3, 3.1) 的球坐标 ——
   声明 orbit 单独不改变缺省观感, 拖拽才动(回归锚友好) */
#define T3_ORBIT_AZ0  28.7f    /* atan2(1.7, 3.1) ≈ 28.74° */
#define T3_ORBIT_EL0  33.0f    /* atan2(2.3, √(1.7²+3.1²)) ≈ 33.06° */
#define T3_ORBIT_D0   4.22f    /* √(1.7²+2.3²+3.1²) ≈ 4.218 */
#define T3_ORBIT_SENS 0.35f    /* 度/px: 全宽拖拽(~1000px) ≈ 350° */
#define T3_ORBIT_TAU  0.6f     /* 惯性衰减时间常数(秒) */

static float t3_clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

static float t3_orbit_fattr(hn_node *n, const char *name, float dflt,
                            float lo, float hi) {
    const char *v = hn_node_attr(n, name);
    if (!v || !*v) return dflt;
    float f = (float)atof(v);
    return t3_clampf(f, lo, hi);
}

static t3_orbit *t3_orbit_get(hn_context *c, hn_node *n) {
    hn_doc *doc = hn_context_doc(c);
    t3_orbit *hit = NULL, *free_slot = NULL, *lru = &g_orbit[0];
    for (int i = 0; i < T3_ORBIT_SLOTS; i++) {
        t3_orbit *o = &g_orbit[i];
        if (o->doc == doc && o->node == n && o->achk == n->arena) { hit = o; break; }
        if (!o->doc && !free_slot) free_slot = o;
        if (o->tick < lru->tick) lru = o;
    }
    if (hit) { hit->tick = ++g_orbit_tick; return hit; }
    t3_orbit *o = free_slot ? free_slot : lru;
    memset(o, 0, sizeof(*o));
    o->doc = doc; o->node = n; o->achk = n->arena;
    o->az = t3_orbit_fattr(n, "hn-three-azimuth", T3_ORBIT_AZ0, -360.0f, 360.0f);
    o->el = t3_orbit_fattr(n, "hn-three-elevation", T3_ORBIT_EL0, -85.0f, 85.0f);
    o->dist = t3_orbit_fattr(n, "hn-three-dist", T3_ORBIT_D0, 1.5f, 30.0f);
    o->last_clock = -1.0f;
    o->tick = ++g_orbit_tick;
    return o;
}

/* 每次取样推进一步: 拖拽跟随 + 松手惯性(指数衰减)。纯函数式更新,
   全部状态在槽内; 指针缺席时是"速度为零的空转", 恒等变换。 */
static void t3_orbit_step(t3_orbit *o, hn_context *c, hn_node *n) {
    float dt_ms = 0.0f;
    if (o->last_clock >= 0.0f) {
        dt_ms = n->ext_clock - o->last_clock;
        if (dt_ms < 0.0f) dt_ms = 0.0f;
        if (dt_ms > 250.0f) dt_ms = 250.0f;   /* 长眠醒来限步(防大跳) */
    }
    o->last_clock = n->ext_clock;
    float dt = dt_ms / 1000.0f;

    int grab = 0;
    if (c->has_pointer) {
        hn_node *hit = hn_context_hit_node(c, c->pointer_x, c->pointer_y);
        /* 拖拽语义: 命中且主按钮按下中(hn_context_set_pointer_button) ——
           此前无按下态, 悬停滑过即旋转、移出还带惯性, 不是拖拽(复审修复)。 */
        grab = (hit == n && c->pointer_down);
    }
    if (grab) {
        if (!o->grab) {                    /* 抓住: 记锚点, 清惯性 */
            o->grab = 1;
            o->gx = c->pointer_x; o->gy = c->pointer_y;
            o->vaz = 0.0f; o->vel = 0.0f;
        } else {                           /* 拖拽: 角随指针即时跟随 */
            float dx = c->pointer_x - o->gx, dy = c->pointer_y - o->gy;
            o->gx = c->pointer_x; o->gy = c->pointer_y;
            o->az += dx * T3_ORBIT_SENS;
            o->el = t3_clampf(o->el - dy * T3_ORBIT_SENS, -85.0f, 85.0f);
            if (dt > 0.0f) {               /* 松手惯性采样(指数平滑 0.5) */
                o->vaz = 0.5f * o->vaz + 0.5f * (dx * T3_ORBIT_SENS / dt);
                o->vel = 0.5f * o->vel + 0.5f * (-dy * T3_ORBIT_SENS / dt);
            }
        }
    } else {
        o->grab = 0;                       /* 松手: 保留速度 → 惯性 */
        if (dt > 0.0f && (o->vaz != 0.0f || o->vel != 0.0f)) {
            o->az += o->vaz * dt;
            o->el = t3_clampf(o->el + o->vel * dt, -85.0f, 85.0f);
            float decay = expf(-dt / T3_ORBIT_TAU);
            o->vaz *= decay; o->vel *= decay;
            if (fabsf(o->vaz) < 0.02f) o->vaz = 0.0f;   /* 亚阈值停表 */
            if (fabsf(o->vel) < 0.02f) o->vel = 0.0f;
        }
    }
}

/* 球坐标 → 视点(度; 与 lookAt 的右手系/ y-up 口径一致) */
static void t3_orbit_eye(float az_deg, float el_deg, float dist, float eye[3]) {
    float az = az_deg * T3_PI / 180.0f, el = el_deg * T3_PI / 180.0f;
    float ce = cosf(el);
    eye[0] = dist * ce * sinf(az);
    eye[1] = dist * sinf(el);
    eye[2] = dist * ce * cosf(az);
}

/* ---- 贴图门(hn-three-tex): 资产后端可达 + PNG 可解码才走 TRIS ----
 * 判定结果(正/负)进 4 槽缓存 —— 逐帧解码验证太贵, 负缓存免得缺图页
 * 每帧重跑读文件+解码(与 hnsoft 图片缓存的负缓存同一纪律)。只存判定
 * 不存像素; 像素由 hnsoft 的图片缓存按路径解码一次。
 * 生命周期契约(2026-10-15 复审修复口径): 门自有串随 LRU 逐出被 free,
 * 而显示列表是"先全量 push 后消费" —— 门串绝不可直接进 cmd.text,
 * 消费方(t3_paint_soft)一律先拷进引擎临时区。 */

#define T3_TEX_SLOTS 4
static struct {
    hn_doc *doc; hn_node *node; hn_arena *achk;
    char *path;                /* 判定成功时的路径(malloc 副本; 负判定 NULL) */
    int ok;                    /* 1 = 可贴图; 0 = 已判定不可(负缓存) */
    unsigned long tick;
} g_tex_gate[T3_TEX_SLOTS];
static unsigned long g_tex_tick = 0;

/* 本地 strdup(strdup 是 POSIX 不是 C99; Windows 目标/minGW 未必可见 ——
   与 dlfcn 门控同一纪律: 本文件只用 C99 标准面) */
static char *t3_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* 键 = (文档, 节点, 节点 arena) 三重校验 —— 与轨道相机槽同一口径:
 * 文档释放后新文档节点地址可能被 malloc 复用, 两重键(node,arena)会让
 * 新文档节点继承旧判定(2026-10-15 复审修复: 贴图门此前缺 doc 键且
 * 未声明该风险; 撞满三重的最坏后果 = 新页继承一个"可/不可贴图"判定,
 * 首帧纹理门重开自愈, 不碰确定性)。 */
static void t3_tex_gate_put(hn_doc *doc, hn_node *n, const char *path, int ok) {
    int slot = -1;
    for (int i = 0; i < T3_TEX_SLOTS; i++)
        if (!g_tex_gate[i].node) { slot = i; break; }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < T3_TEX_SLOTS; i++)
            if (g_tex_gate[i].tick < g_tex_gate[slot].tick) slot = i;
        free(g_tex_gate[slot].path);
    }
    g_tex_gate[slot].doc = doc;
    g_tex_gate[slot].node = n;
    g_tex_gate[slot].achk = n->arena;
    g_tex_gate[slot].path = t3_strdup(path);
    g_tex_gate[slot].ok = ok;
    g_tex_gate[slot].tick = ++g_tex_tick;
}

/* 命中返回 1 并把可贴图路径(负判定为 NULL)写进 *out; 未命中返回 0。
   *out 指向门自有串 —— 调用方不得把该指针压进显示列表(门槽 LRU 逐出
   会 free 它); 压显示列表前必须拷进引擎临时区(见 t3_paint_soft)。 */
static int t3_tex_gate_get(hn_doc *doc, hn_node *n, const char *path,
                           const char **out) {
    for (int i = 0; i < T3_TEX_SLOTS; i++)
        if (g_tex_gate[i].doc == doc && g_tex_gate[i].node == n &&
            g_tex_gate[i].achk == n->arena && g_tex_gate[i].path &&
            path && !strcmp(g_tex_gate[i].path, path)) {
            *out = g_tex_gate[i].ok ? g_tex_gate[i].path : NULL;
            return 1;
        }
    return 0;
}

/* 判定: 资产后端取字节 → hn_png_decode 解码即弃(成功 = 门开)。
   失败(缺后端/不可达/非 PNG)返回 NULL —— 调用方回退 POLYGON Lambert,
   不静默空白。 */
static const char *t3_tex_resolve(hn_context *c, hn_node *n, const char *path) {
    const char *hit = NULL;
    hn_doc *doc = hn_context_doc(c);
    if (t3_tex_gate_get(doc, n, path, &hit)) return hit;
    const hn_asset_backend *ab = hn_context_assets(c);
    if (!ab || !ab->load || !path || !*path) {
        t3_tex_gate_put(doc, n, path, 0);
        return NULL;
    }
    size_t len = 0;
    const char *bytes = ab->load(ab->ctx, path, &len);
    if (!bytes || len < 8) {
        t3_tex_gate_put(doc, n, path, 0);
        return NULL;
    }
    int w = 0, h = 0;
    unsigned char *px = hn_png_decode((const unsigned char *)bytes, len, &w, &h);
    if (!px || w <= 0 || h <= 0) {
        free(px);
        t3_tex_gate_put(doc, n, path, 0);
        return NULL;
    }
    free(px);
    t3_tex_gate_put(doc, n, path, 1);
    return path;
}

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

/* ---- 场景图(Scene → Group → Mesh): 局部矩阵逐级复合 ----
   显式两级结构表达"父子矩阵复合"这一件事(pivot Group 载自转动画与
   固定倾角, Mesh 挂几何 —— 几何由 hn-three-geo 从上方三张网格表选);
   泛化节点池仍在下一步(见 docs/threejs-support.md 下一步)。 */

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

/* ---- 发射: hn-three 属性元素 → 显示列表(软件路径) ------------------
 * 无贴图: 每三角一条 HN_CMD_POLYGON(既有内核; 盒/球/面同一条流水线,
 * 可见像素与垂直切片同源 —— 外加被覆盖的背面三角, 见下方可见性注)。
 * 有贴图(hn-three-tex 门开): 三角汤按画家序进
 * HN_CMD_TRIS(paint_tris 逐像素 UV 采样; 自带解析 AA, 不做 0.75px
 * 边外扩 —— 那是 POLYGON 通道的接缝补丁, TRIS 内核不需要)。
 * 双面几何(plane): 背面三角翻转法线着色 + 反转绕序发射 —— TRIS 的
 * rows=1 单面剔除按屏幕绕序丢正面积三角, 反转后背面得以保留。 */

static void t3_paint_soft(hn_context *c, hn_node *n,
                          float bw, float bh, float sx, float sy, float alpha) {
    if (bw < 8.0f || bh < 8.0f) return;           /* 盒过小: 无可画(布局给的) */

    /* 几何 + 贴图门 + 基色覆盖 */
    const t3_geom *geo = t3_geom_of(n);
    if (!geo || geo->ntri <= 0) return;
    unsigned int color_ovr = 0;
    int has_ovr = t3_color_override(n, &color_ovr);

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

    /* 相机: 声明 hn-three-camera-orbit → 轨道相机(拖拽/惯性, 见
       t3_orbit 一节); 未声明 → 固定视点(垂直切片口径, 逐位不变)。 */
    float eye[3];
    if (hn_node_attr(n, "hn-three-camera-orbit")) {
        t3_orbit *orb = t3_orbit_get(c, n);
        t3_orbit_step(orb, c, n);
        t3_orbit_eye(orb->az, orb->el, orb->dist, eye);
    } else {
        eye[0] = 1.7f; eye[1] = 2.3f; eye[2] = 3.1f;
    }
    t3_mat4 view, proj;
    t3_mat_lookat(view, eye[0], eye[1], eye[2], 0.0f, 0.0f, 0.0f);
    t3_mat_perspective(proj, 50.0f * T3_PI / 180.0f, bw / bh, 0.1f, 100.0f);

    t3_mat4 world;
    t3_compose_scene(spin_rad, tilt_rad, world);

    /* 方向光(three.js DirectionalLight 观感): 归一 (0.45, 0.8, 0.55) */
    const float lx = 0.436f, ly = 0.775f, lz = 0.533f;

    /* 贴图门: 资产后端可达且可解码 → TRIS 贴图三角汤; 否则 POLYGON */
    const char *tex_path = NULL;
    const char *tex_attr = hn_node_attr(n, "hn-three-tex");
    if (tex_attr && *tex_attr)
        tex_path = t3_tex_resolve(c, n, tex_attr);

    /* 逐三角变换(全量, 不剔除), 收集屏幕三角形(vis 表进引擎临时区,
       生命周期 = 本次显示列表 —— 与顶点汤同一契约) */
    typedef struct {
        float x[3], y[3];        /* 屏幕坐标(元素盒局部, 未外扩) */
        float u[3], v[3];        /* 角点 UV(贴图路径用) */
        float depth;             /* 视空间质心 z(画家键, 小 = 远) */
        hn_color fill;           /* 平直着色 × 光照 × 元素 alpha(无贴图) */
        int flip;                /* 双面背面: TRIS 发射时反转绕序 */
        int front;               /* 近侧面(世界法线朝眼): 只有它做 0.75px
                                   边外扩 —— 背面三角全被近面覆盖, 外扩只会在
                                   轮廓外戳出暗边(实测盒边 2-4px 暗缘) */
    } t3_vis;
    hn_arena *tmp = hn_context_tmp(c);
    if (!tmp) return;
    t3_vis *vis = (t3_vis *)hn_arena_alloc(tmp, sizeof(t3_vis) * (size_t)geo->ntri);
    if (!vis) return;
    int n_vis = 0;

    float a_mul = alpha;         /* paint_walk 已并 opacity, 这里只乘自身 */
    if (a_mul > 1.0f) a_mul = 1.0f;

    for (int t = 0; t < geo->ntri; t++) {
        float wx[3], wy[3], wz[3];
        for (int k = 0; k < 3; k++) {
            const float *p = &geo->pos[geo->ix[t * 3 + k] * 3];
            float clip[4];
            t3_mat_point(world, p[0], p[1], p[2], clip);
            wx[k] = clip[0]; wy[k] = clip[1]; wz[k] = clip[2];
        }
        /* 可见性: **不剔除, 全量发射** —— 凸闭合网格 + 画家算法(下按视
           空间质心 z 远→近排序)与 z-buffer 逐像素同像: 每像素最后画到的
           必是最近的三角。任何"法线/径向/帽"剔除都在离散采样下留剪影
           缺口(边界带三角形恰跨边界, 质心/顶点判都会在采样稀疏方向整段
           跳空 —— 实测 960 三角球右缘缺一条); 全量发射时全体三角的投影
           之并 = 球面投影之并 = 完整圆盘, 缺口在构造上不可能。代价 =
           多画一半背面三角(被近面覆盖, sphere 960 vs ~530 条, demo 量级
           无感)。双面几何(plane)背面标记翻转(照常受光/反转绕序)。 */
        float cxw = (wx[0] + wx[1] + wx[2]) / 3.0f;
        float cyw = (wy[0] + wy[1] + wy[2]) / 3.0f;
        float czw = (wz[0] + wz[1] + wz[2]) / 3.0f;
        int flip = 0;
        if (geo->dbl) {
            /* 双面: 世界法线朝眼为正, 背面翻转(着色 + TRIS 绕序) */
            float fnx = geo->nrm[t * 3], fny = geo->nrm[t * 3 + 1],
                  fnz = geo->nrm[t * 3 + 2];
            float rnx = world[0] * fnx + world[4] * fny + world[8]  * fnz;
            float rny = world[1] * fnx + world[5] * fny + world[9]  * fnz;
            float rnz = world[2] * fnx + world[6] * fny + world[10] * fnz;
            if (rnx * (eye[0] - cxw) + rny * (eye[1] - cyw)
                    + rnz * (eye[2] - czw) < 0.0f)
                flip = 1;
        }

        /* 裁剪空间 + 近平面(整三角剔除边界, 见头注)。
           投影用上面已算好的 **world 变换后**顶点(wx/wy/wz)—— 此前这里
           重新取原始 geo->pos 直穿 view, world(自转+倾角)只影响光照不进
           几何, 画面上的盒子不随 ext_clock 旋转(复审修复)。 */
        float cx[3], cy[3], cw[3], vcz[3];
        int behind = 0;
        for (int k = 0; k < 3; k++) {
            float vp[4], cl[4];
            t3_mat_point(view, wx[k], wy[k], wz[k], vp);
            vcz[k] = vp[2];
            t3_mat_point(proj, vp[0], vp[1], vp[2], cl);
            cx[k] = cl[0]; cy[k] = cl[1]; cw[k] = cl[3];
            if (cw[k] < 0.01f) behind = 1;
        }
        if (behind) continue;

        /* NDC → 元素盒屏幕坐标(y 翻转; 外扩留给发射分支 —— POLYGON
           通道的 0.75px 边法线补丁见下方, TRIS 通道不需要) */
        float vx[3], vy[3];
        for (int k = 0; k < 3; k++) {
            float ndcx = cx[k] / cw[k], ndcy = cy[k] / cw[k];
            vx[k] = sx + bw * (ndcx * 0.5f + 0.5f);
            vy[k] = sy + bh * (1.0f - (ndcy * 0.5f + 0.5f));
        }
        t3_vis *V = &vis[n_vis];
        for (int k = 0; k < 3; k++) {
            V->x[k] = vx[k]; V->y[k] = vy[k];
            V->u[k] = geo->uvt[t * 6 + k * 2];
            V->v[k] = geo->uvt[t * 6 + k * 2 + 1];
        }
        V->depth = (vcz[0] + vcz[1] + vcz[2]) / 3.0f;
        V->flip = flip;
        {
            float fnx0 = geo->nrm[t * 3], fny0 = geo->nrm[t * 3 + 1],
                  fnz0 = geo->nrm[t * 3 + 2];
            float rnx = world[0] * fnx0 + world[4] * fny0 + world[8]  * fnz0;
            float rny = world[1] * fnx0 + world[5] * fny0 + world[9]  * fnz0;
            float rnz = world[2] * fnx0 + world[6] * fny0 + world[10] * fnz0;
            float toeye = rnx * (eye[0] - cxw) + rny * (eye[1] - cyw)
                        + rnz * (eye[2] - czw);
            V->front = geo->dbl ? !flip : (toeye >= 0.0f);
        }

        /* Blinn-Phong 双灯 + 半球环境光(2026-10-15 渲染升级):
           - key 灯(l): 主方向光, 漫反射 + Blinn 高光(shininess 48)
           - fill 灯(-lx, 0.3, -lz 归一): 暗面补光, 强度 0.25
           - 半球环境: 法线朝上偏亮/朝下偏暗(0.32..0.18)
           观感: 立体感/材质感显著优于单灯 Lambert; 高光让球体/圆角
           呈现"光泽"。geo->nrm 经 world 旋转, 双面背面取反(口径不变)。 */
        float fnx = geo->nrm[t * 3], fny = geo->nrm[t * 3 + 1],
              fnz = geo->nrm[t * 3 + 2];
        if (flip) { fnx = -fnx; fny = -fny; fnz = -fnz; }
        float rnx = world[0] * fnx + world[4] * fny + world[8]  * fnz;
        float rny = world[1] * fnx + world[5] * fny + world[9]  * fnz;
        float rnz = world[2] * fnx + world[6] * fny + world[10] * fnz;
        float nl = sqrtf(rnx * rnx + rny * rny + rnz * rnz);
        float shade = 0.25f, spec = 0.0f;
        if (nl > 1e-6f) {
            float nx = rnx / nl, ny = rny / nl, nz = rnz / nl;
            /* key 漫反射 */
            float diff = nx * lx + ny * ly + nz * lz;
            if (diff < 0.0f) diff = 0.0f;
            /* key 高光: 半程向量 H = normalize(L + V), V = (0,0,1)(视空间) */
            float hx = lx, hy = ly, hz = lz + 1.0f;
            float hl = sqrtf(hx * hx + hy * hy + hz * hz);
            if (hl > 1e-6f) {
                float ndh = (nx * hx + ny * hy + nz * hz) / hl;
                if (ndh > 0.0f) {
                    float sp = ndh;
                    for (int si = 0; si < 6; si++) sp *= sp;   /* ^64 */
                    spec = 0.55f * sp;
                }
            }
            /* fill 漫反射(反侧低强度) */
            float fdiff = -(nx * lx) * 0.6f + (ny * 0.25f) - (nz * lz) * 0.6f;
            if (fdiff < 0.0f) fdiff = 0.0f;
            /* 半球环境: 法线 y 分量在 [0.18, 0.32] 线性插值 */
            float hemi = 0.25f + 0.07f * ny;
            shade = hemi + 0.62f * diff + 0.25f * fdiff;
        }
        float lit = shade + spec;
        unsigned int rgb = has_ovr ? color_ovr : geo->col[t];
        unsigned int r = (unsigned int)(((rgb >> 16) & 0xFF) * lit);
        unsigned int g = (unsigned int)(((rgb >> 8)  & 0xFF) * lit);
        unsigned int b = (unsigned int)((rgb & 0xFF) * lit);
        if (r > 255) r = 255; if (g > 255) g = 255; if (b > 255) b = 255;
        V->fill = 0x000000FFu | (b << 8) | (g << 16) | (r << 24);
        if (a_mul < 0.999f)
            V->fill = (V->fill & 0xFFFFFF00u)
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

    if (tex_path) {
        /* 贴图路径: 三角汤按画家序展开(顶点 + UV) → 一条 HN_CMD_TRIS。
           双面背面三角反转绕序(paint_tris rows=1 的屏幕绕序剔除口径)。 */
        float *sv = (float *)hn_arena_alloc(tmp, sizeof(float) * (size_t)n_vis * 6);
        float *su = (float *)hn_arena_alloc(tmp, sizeof(float) * (size_t)n_vis * 6);
        if (!sv || !su) return;
        for (int i = 0; i < n_vis; i++) {
            int ord[3] = { 0, 1, 2 };
            if (vis[i].flip) { ord[0] = 2; ord[1] = 1; ord[2] = 0; }
            for (int k = 0; k < 3; k++) {
                sv[i * 6 + k * 2]     = vis[i].x[ord[k]];
                sv[i * 6 + k * 2 + 1] = vis[i].y[ord[k]];
                su[i * 6 + k * 2]     = vis[i].u[ord[k]];
                su[i * 6 + k * 2 + 1] = vis[i].v[ord[k]];
            }
        }
        hn_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.kind = HN_CMD_TRIS;
        cmd.mesh_verts = sv;
        cmd.mesh_uv = su;
        cmd.mesh_cols = n_vis;                    /* TRIS 语义 = 三角形数 */
        cmd.mesh_rows = 1;                        /* 单面标记: 屏幕绕序剔除 */
        /* 路径串进引擎临时区(生命周期 = 本次显示列表, 与顶点汤同一契约)
           —— 2026-10-15 复审修复: 此前 cmd.text 直指贴图门的自有串, 贴图
           门是 4 槽 LRU, 同一次 paint walk 内第 5 个不同 hn-three-tex
           元素的 t3_tex_gate_put 会 free 第 1 个元素已入队 TRIS 仍引用
           的串 → 光栅期 UAF 读; 同长度 malloc 复用时静默贴成别的元素的
           纹理。门命中时 tex_path 正是门串(t3_tex_gate_get 契约), 故
           两分支统一拷贝。 */
        {
            size_t pn = strlen(tex_path) + 1;
            char *tp = (char *)hn_arena_alloc(tmp, pn);
            if (!tp) return;
            memcpy(tp, tex_path, pn);
            cmd.text = tp;
        }
        cmd.text_len = strlen(tex_path);
        cmd.fill = 0xFFFFFF00u
                 | (unsigned char)((a_mul >= 1.0f ? 1.0f
                                     : (a_mul < 0.0f ? 0.0f : a_mul)) * 255.0f
                                   + 0.5f);
        cmd.x = sx; cmd.y = sy; cmd.w = bw; cmd.h = bh;   /* 后端剔除用 */
        hn_paint_push(c, &cmd);
        return;
    }

    /* 无贴图: 顶点汤进引擎临时区(生命周期 = 本次显示列表), 每三角一条
       POLYGON —— 复用 paint_polygon 既有扫描内核, 不新写扫描线。
       三角沿**边法线**外扩 0.75px —— 同色相邻三角的共享边上 paint_polygon
       的半像素边缘覆盖(各 0.5)叠加只有 0.75, 会显出对角接缝; 每条边沿
       自身外法线平移 0.75px(顶点 = 相邻两边法线的 miter 和, 限幅 9px 只
       挡退化细条)后, 共享边两侧各有 0.75px 垂直重叠, 与边中段/端点无关,
       过剩覆盖 ≥1, 接缝消失(三角汤惯用手法, 与 paint_tris 的解析 AA
       "过剩覆盖"同一目的)。2026-10-14 复审核验: 原先"顶点自质心径向推"
       在共享边中段的法向分量不足 0.5px, 实拍共享对角线仍有 ~46% 掉深
       暗缝, 不满足本声明; 改边法线外扩后实拍全对角线(含两端角部)欠覆盖
       像素归零, 轮廓位移 ≤1px。 */
    for (int i = 0; i < n_vis; i++) {
        t3_vis *V = &vis[i];
        float enx[3], eny[3];
        if (!V->front) {   /* 背面三角: 不外扩(只求被覆盖, 别在轮廓外戳边) */
            float *poly = (float *)hn_arena_alloc(tmp, sizeof(float) * 6);
            if (!poly) return;
            poly[0] = V->x[0]; poly[1] = V->y[0];
            poly[2] = V->x[1]; poly[3] = V->y[1];
            poly[4] = V->x[2]; poly[5] = V->y[2];
            hn_cmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.kind = HN_CMD_POLYGON;
            cmd.poly = poly;
            cmd.poly_n = 3;
            cmd.fill = V->fill;
            hn_paint_push(c, &cmd);
            continue;
        }
        for (int k = 0; k < 3; k++) {   /* 每条边的单位外法线 */
            int a = k, b = (k + 1) % 3, o = (k + 2) % 3;
            float exd = V->x[b] - V->x[a], eyd = V->y[b] - V->y[a];
            float el = sqrtf(exd * exd + eyd * eyd);
            if (el < 1e-6f) { enx[k] = 0.0f; eny[k] = 0.0f; continue; }
            float nx = eyd / el, ny = -exd / el;
            /* 朝外 = 与"边中点 → 对角顶点"反向 */
            if (nx * ((V->x[a] + V->x[b]) * 0.5f - V->x[o]) +
                ny * ((V->y[a] + V->y[b]) * 0.5f - V->y[o]) < 0.0f) {
                nx = -nx; ny = -ny;
            }
            enx[k] = nx; eny[k] = ny;
        }
        float px[3], py[3];
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
                   残留 5 像素欠覆盖, 9px 归零且轮廓位移 ≤1px)。
                   球体 960 三角的屏幕三角形更小更尖, 同一限幅适用
                   (退化细条本就该被挡)。 */
                if (ol > 9.0f) {
                    ox *= 9.0f / ol;
                    oy *= 9.0f / ol;
                }
            }
            px[k] = V->x[k] + ox;
            py[k] = V->y[k] + oy;
        }
        float *poly = (float *)hn_arena_alloc(tmp, sizeof(float) * 6);
        if (!poly) return;
        poly[0] = px[0]; poly[1] = py[0];
        poly[2] = px[1]; poly[3] = py[1];
        poly[4] = px[2]; poly[5] = py[2];
        hn_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.kind = HN_CMD_POLYGON;
        cmd.poly = poly;
        cmd.poly_n = 3;
        cmd.fill = V->fill;
        hn_paint_push(c, &cmd);
    }
}

/* ---- 入口分发: backend 可用走 neb3 帧, 否则软件路径 ----------------
 * backend(Phase A)只会装配"缺省白盒"场景 —— 声明了几何/轨道/贴图/
 * 基色任一扩展的元素恒走软件路径: 让 backend 画出一个语义不符的缺省
 * 盒是静默画错, 如实降级才是降级(ADR 001 同口径)。缺省白盒元素
 * (纯 hn-three + 可选 spin/msaa)维持既有 backend 路径不变。 */

static int t3_backend_capable(hn_node *n) {
    const char *g = hn_node_attr(n, "hn-three-geo");
    if (g && *g && strcmp(g, "box")) return 0;   /* 显式 box 也行, 缺省也行 */
    if (hn_node_attr(n, "hn-three-camera-orbit")) return 0;
    if (hn_node_attr(n, "hn-three-tex")) return 0;
    if (hn_node_attr(n, "hn-three-color")) return 0;
    return 1;
}

void hn_three_paint(hn_context *c, hn_node *n,
                    float bw, float bh, float sx, float sy, float alpha) {
    if (bw < 8.0f || bh < 8.0f) return;           /* 盒过小: 无可画(布局给的) */
    if (!g_neb3_state) t3_neb3_load();
    if (g_neb3_state == 1 && t3_backend_capable(n) &&
        t3_paint_backend(c, n, bw, bh, sx, sy, alpha))
        return;
    t3_paint_soft(c, n, bw, bh, sx, sy, alpha);
}

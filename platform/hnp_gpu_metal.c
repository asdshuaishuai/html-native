/* hnp_gpu_metal.c — GPU 渲染后端 v1: hnp_gpu_host 的 Metal 实现(纯 C99)
 *
 * 与 hnp_macos.c 同一手法: 运行期 dlopen Metal.framework + objc_msgSend,
 * 零 ObjC 头文件 / 零 .m / 零编译期 SDK 依赖 —— 连链接期都不需要 Metal
 * 框架(全 dlsym; MTLCreateSystemDefaultDevice 本身就是 C 符号, 直接
 * dlsym 即得)。MSL 着色器源码以字符串随帧运行期编译(newLibraryWithSource),
 * 没有 .metal 文件、没有离线编译步骤。
 *
 * 原语语义全部见 platform/hn_platform.h 尾部"GPU 渲染后端"契约段; 本文件
 * 只做设备原语, 显示列表 → 原语的翻译在 Sources/CHtmlNative/hn_gpu.c。
 *
 * ── 关键口径(与 hnsoft/readback 的一致性, 逐条对契约) ──
 *   1. 离屏渲染目标 = MTLPixelFormatBGRA8Unorm(Metal 小端字节序 = B,G,R,A,
 *      与契约 end_readback 的 out_bgra 布局一致); 存储模式 Shared(Apple
 *      Silicon 支持; 本仓库 macOS 产物只有 aarch64 —— Intel 机器上纹理
 *      不支持 Shared, newTextureWithDescriptor 失败 → init 返回 0 →
 *      呼出方整体回退 hnsoft, 优雅降级)。Shared 免去 Managed 双副本的
 *      synchronizeResource 同步, getBytes 直接读。
 *   2. 采样纹理 = MTLPixelFormatRGBA8Unorm + **预乘**上传(引擎给非预乘
 *      RGBA8, 本层入纹理前预乘)。混合 = src-one / dst-one-minus-src
 *      (预乘 src-over)。与 hnsoft 的非预乘 readback 之间的换算关系:
 *      hnsoft 在非预乘空间做 over 后写回非预乘; 本层全部预乘, over 数学
 *      等价(逐步推导见 hn_gpu.c 头注释), 差异只剩 8bit 舍入(±1/通道)
 *      与边缘 AA 覆盖率(整数坐标盒逐位一致, 见 hn_gpu.c 的取整注释)。
 *   3. 坐标系: 引擎顶点物理 px、y 向下、原点左上; 顶点着色器把 y 翻到
 *      NDC(clipY = 1 - 2y/H), 帧缓冲第 0 行 = NDC y=+1 = 引擎 y=0 ——
 *      纹理 uv(0,0) 即左上, 图片/字形上传顶上到底下, 不再翻转。
 *      scissor 用 MTLScissorRect(帧缓冲 px, 原点左上) = 引擎坐标直通。
 *   4. 顶点数据超 setBytes 4KB 限制 → 帧内**追加 arena**(Shared MTLBuffer,
 *      每次 draw_quads 追加到下一偏移并按偏移绑定 —— GPU 在 commit 后才
 *      执行, 复用同一段缓冲会让所有 draw 读到最后一次写入, 实测如此);
 *      索引缓冲一次建满容量(同向绕序拆分, 见 IBO 注释), 一次
 *      drawIndexedPrimitives 画整个 draw_quads 调用。
 *
 * supported_cmds 位集(诚实声明 —— 位值 = hn_cmd_kind 数值, 数值对照见
 * include/hn.h; 平台头为免反向依赖以数值注释):
 *   1=RECT 2=TEXT 3=IMAGE 4=CLIP_PUSH 5=CLIP_POP 6=QUAD 9=BITMAP。
 *   7=POLYGON / 8=MESH / 10=CLEAR_RECT 不声明 —— 原语表没有任意路径填充
 *   与"替换式局部清空"; 呼出方(hn_gpu_available)扫到即整帧回退 hnsoft。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <dlfcn.h>

#include "hn_platform.h"

typedef void *obj_t;
typedef void *sel_t;

static obj_t (*o_getClass)(const char *);
static sel_t (*o_sel)(const char *);
static obj_t (*o_msgSend)(obj_t, sel_t, ...);

#define MSG0(RET, O, NAME) ((RET (*)(obj_t, sel_t))o_msgSend)((O), o_sel(NAME))
#define MSG1(RET, O, NAME, A1) \
    ((RET (*)(obj_t, sel_t, obj_t))o_msgSend)((O), o_sel(NAME), (obj_t)(A1))
#define MSGP(RET, O, NAME, A1) \
    ((RET (*)(obj_t, sel_t, void *))o_msgSend)((O), o_sel(NAME), (void *)(A1))
#define MSGUL(RET, O, NAME, UL) \
    ((RET (*)(obj_t, sel_t, unsigned long))o_msgSend)((O), o_sel(NAME), (unsigned long)(UL))
#define MSGULL(RET, O, NAME, A1, A2) \
    ((RET (*)(obj_t, sel_t, unsigned long, unsigned long))o_msgSend) \
        ((O), o_sel(NAME), (unsigned long)(A1), (unsigned long)(A2))

/* ---- Metal C/ObjC 类型镜像(逐值抄自 SDK 头, 不凭记忆) ---- */

/* MTLPixelFormat(Metal.h): RGBA8Unorm = 70, BGRA8Unorm = 80 */
#define MTL_PF_RGBA8 70
#define MTL_PF_BGRA8 80
/* MTLStorageMode: Shared = 0(Apple Silicon 纹理支持; 见文件头注 1) */
#define MTL_SM_SHARED 0
/* MTLTextureUsage: ShaderRead = 1<<0, RenderTarget = 1<<1 */
#define MTL_TU_SHREAD  (1ul << 0)
#define MTL_TU_RENDERT (1ul << 1)
/* MTLLoadAction: DontCare=0 Load=1 Clear=2; MTLStoreAction: DontCare=0 Store=1 */
#define MTL_LA_CLEAR 2
#define MTL_SA_STORE 1
/* MTLBlendFactor: Zero=0 One=1 SourceAlpha=4 OneMinusSourceAlpha=5 */
#define MTL_BF_ONE 1
#define MTL_BF_ONE_MINUS_SRC_ALPHA 5
/* MTLPrimitiveType: Triangle = 3; MTLIndexType: UInt32 = 1 */
#define MTL_PT_TRIANGLE 3
#define MTL_IT_U32 1

typedef struct { double r, g, b, a; } MTLClearColor_;            /* 4×double */
typedef struct { unsigned long x, y, width, height; } MTLScissorRect_;
typedef struct { unsigned long x, y, z; } MTLOrigin_;
typedef struct { unsigned long width, height, depth; } MTLSize_;
typedef struct { MTLOrigin_ origin; MTLSize_ size; } MTLRegion_;
typedef struct { double originX, originY, width, height, znear, zfar; } MTLViewport_;

/* ---- 引擎侧经宿主注入看到的句柄 = 纹理表下标+1(0 = 无效, 契约口径) ---- */
#define TEX_CAP 4096
static struct { obj_t tex; int w, h; } g_tex[TEX_CAP];
static int g_tex_n;

static obj_t g_device;        /* id<MTLDevice> */
static obj_t g_queue;         /* id<MTLCommandQueue> */
static obj_t g_pso_solid;     /* 纯色管线 */
static obj_t g_pso_tex;       /* 带纹理管线 */
static obj_t g_samp;          /* 线性采样器(全部纹理统一线性 —— 1:1 映射时
                                 线性采样在像素中心精确命中文素中心 = 与
                                 最近采样逐位一致; 见 hn_gpu.c 的字形/图片注释) */
static obj_t g_vbuf;          /* 帧内顶点 arena(Shared; 追加偏移制, 见下) */
static unsigned long g_vcap_bytes;   /* g_vbuf 容量(字节) */
static size_t g_voff;         /* 本帧已用顶点字节(arena 写头; begin 归零) */
static obj_t g_ibuf;          /* 索引缓冲(一次建满 IBO_CAP) */
#define IBO_CAP_QUADS 16384   /* 单次 draw_quads 的索引容量上限(超出分块) */

/* 帧状态(begin..end_readback 窗口) */
static obj_t g_rt;            /* 离屏渲染目标纹理 */
static int g_rtw, g_rth;
static obj_t g_cmdbuf;        /* id<MTLCommandBuffer>(本帧) */
static obj_t g_enc;           /* id<MTLRenderCommandEncoder>(本帧) */
static obj_t g_pool;          /* 有界 autorelease 池(帧窗口; 手法同 hnp_macos.c
                                 pool_push —— commandBuffer/encoder/RPD 按
                                 autorelease 归还, 无池逐帧泄漏) */

/* 裁剪栈(轴对齐交叠; MTLScissorRect 是单矩形 —— 栈顶 = 交集) */
#define CLIP_CAP 32
static struct { int x0, y0, x1, y1; } g_clip[CLIP_CAP];
static int g_clip_n;

static int g_ready;           /* init 成功(进程一次) */

/* ---------------- MSL 源码(运行期编译) ----------------
 * 顶点: 物理像素直通变换(y 翻转见文件头注 3); 片段两入口复用同一库:
 *   hn_fs_solid — 纯色(顶点色 uniform)
 *   hn_fs_tex   — 纹理采样 × uniform 色(预乘域相乘 = 颜色 × 覆盖率)
 * struct U 的 C 侧镜像见 mtl_uniforms(偏移逐项对齐: float4@0, w@16, h@20) */
static const char *MSL_SRC =
"#include <metal_stdlib>\n"
"using namespace metal;\n"
"struct U { float4 color; float w; float h; float pad; };\n"
"struct VOut { float4 pos [[position]]; float2 uv; };\n"
"vertex VOut hn_vs(constant float* verts [[buffer(0)]],\n"
"                  constant U& u [[buffer(1)]], uint vid [[vertex_id]]) {\n"
"    float x = verts[vid*4+0], y = verts[vid*4+1];\n"
"    VOut o;\n"
"    o.pos = float4(x / u.w * 2.0 - 1.0, 1.0 - y / u.h * 2.0, 0.0, 1.0);\n"
"    o.uv  = float2(verts[vid*4+2], verts[vid*4+3]);\n"
"    return o;\n"
"}\n"
"fragment float4 hn_fs_solid(VOut i [[stage_in]], constant U& u [[buffer(0)]]) {\n"
"    (void)i;\n"
"    return u.color;\n"
"}\n"
"fragment float4 hn_fs_tex(VOut i [[stage_in]], texture2d<float> t [[texture(0)]],\n"
"                          sampler s [[sampler(0)]], constant U& u [[buffer(0)]]) {\n"
"    return t.sample(s, i.uv) * u.color;\n"
"}\n";

/* mtl_uniforms 的 C 镜像(MSL struct U 逐项对齐, 见上) */
typedef struct { float color[4]; float w, h, pad; } mtl_uniforms;

/* 诊断: NSError 一行(失败原因可见, 与平台层"一行 stderr"惯例一致) */
static void nserr_line(const char *what, obj_t err) {
    char buf[256] = "?";
    if (err) {
        obj_t s = MSG0(obj_t, err, "localizedDescription");
        if (s) {
            const char *u = ((const char *(*)(obj_t, sel_t))o_msgSend)(s, o_sel("UTF8String"));
            if (u) snprintf(buf, sizeof(buf), "%s", u);
        }
    }
    fprintf(stderr, "[gpu-metal] %s: %s\n", what, buf);
}

/* 取 colorAttachments[i](新旧运行时统一口径: 属性返回**数组对象**,
   以 objectAtIndexedSubscript: 取下标 —— 旧的 colorAttachmentsAtIndex:
   在新 macOS 运行时已被移除, 调用即 unrecognized selector 崩溃, 实测) */
static obj_t color_attachment(obj_t pd, unsigned long idx) {
    obj_t arr = MSG0(obj_t, pd, "colorAttachments");
    if (!arr) return NULL;
    return ((obj_t (*)(obj_t, sel_t, unsigned long))o_msgSend)(
        arr, o_sel("objectAtIndexedSubscript:"), idx);
}

/* 单行 NSString */
static obj_t nsstr(const char *s) {
    return ((obj_t (*)(obj_t, sel_t, const char *))o_msgSend)(
        o_getClass("NSString"), o_sel("stringWithUTF8String:"), s);
}

static void tex_release_slot(int i) {
    if (g_tex[i].tex) { MSG0(void, g_tex[i].tex, "release"); g_tex[i].tex = NULL; }
}

/* ---------------- init / shutdown ---------------- */

static int mtl_init(void) {
    if (g_ready) return 1;
    /* libobjc 显式装载 + **从句柄** dlsym: 纯 C99 产物不链接 libobjc, 而
       dlsym(RTLD_DEFAULT) 在部分环境下对 dlopen 装载的库不可见(实测
       hn_platform 各壳惯用的 "dlopen AppKit 后 DEFAULT 找 objc_msgSend"
       在受限执行环境里全 NULL) —— 从库句柄查符号是最稳口径, 两种环境
       通吃。libobjc 路径: 新系统 libobjc.A.dylib, 兜底符号链接。 */
    void *objc = dlopen("/usr/lib/libobjc.A.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!objc) objc = dlopen("/usr/lib/libobjc.dylib", RTLD_NOW | RTLD_LOCAL);
    if (!objc) return 0;
    o_getClass = (obj_t (*)(const char *))dlsym(objc, "objc_getClass");
    o_sel = (sel_t (*)(const char *))dlsym(objc, "sel_registerName");
    o_msgSend = (obj_t (*)(obj_t, sel_t, ...))dlsym(objc, "objc_msgSend");
    if (!o_getClass || !o_sel || !o_msgSend) return 0;

    /* Metal.framework 运行期装载(纯 dlopen, 链接期零框架 —— 与 AppKit
       同手法; C 符号同样从句柄 dlsym) */
    void *metal = dlopen("/System/Library/Frameworks/Metal.framework/Metal", RTLD_NOW | RTLD_LOCAL);
    if (!metal) return 0;
    /* C 符号: 默认系统设备(无 Metal 的环境返回 nil) */
    obj_t (*create_dev)(void) = (obj_t (*)(void))dlsym(metal, "MTLCreateSystemDefaultDevice");
    if (!create_dev) return 0;
    g_device = create_dev();
    if (!g_device) return 0;

    g_queue = MSG0(obj_t, g_device, "newCommandQueue");
    if (!g_queue) return 0;

    /* MSL 运行期编译(同步变体 newLibraryWithSource:options:error: ——
       options 传 nil 合法; 异步变体需要 ObjC block, 纯 C 侧不开编译扩展)。
       选择器缺席(macOS <10.11, 理论不可能)→ 优雅失败回退 hnsoft。 */
    sel_t sel_resp = o_sel("respondsToSelector:");
#define HAS_SEL(O, NAME) ((int)(((signed char (*)(obj_t, sel_t, sel_t))o_msgSend) \
        ((O), sel_resp, o_sel(NAME))))
    if (!HAS_SEL(g_device, "newLibraryWithSource:options:error:")) return 0;
    /* colorAttachments(数组)+ 纹理描述符类方法: 新运行时把旧的
       colorAttachmentsAtIndex: 移除了(实测 unrecognized selector 直接
       abort) —— 缺席即优雅回退, 不给异常崩进程的机会 */
    {
        obj_t pd0 = MSG0(obj_t, o_getClass("MTLRenderPipelineDescriptor"), "alloc");
        pd0 = MSG0(obj_t, pd0, "init");
        int ok_pd = HAS_SEL(pd0, "colorAttachments");
        MSG0(void, pd0, "release");
        if (!ok_pd) return 0;
    }
    obj_t rpd0 = MSG0(obj_t, o_getClass("MTLRenderPassDescriptor"), "renderPassDescriptor");
    /* 注意各选择器的**真实接收者**与版本差异: 便捷类方法
       texture2DDescriptorWithPixelFormat:… 在新 macOS 运行时已不响应
       (实测) —— 纹理描述符走 alloc/init 路线, 守卫改为 setter 存在性;
       采样器创建只有无 error: 变体(newSamplerStateWithDescriptor:)。 */
    int ok_rpd = rpd0 && HAS_SEL(rpd0, "colorAttachments")
              && HAS_SEL(o_getClass("MTLTextureDescriptor"), "alloc");
    if (!ok_rpd) return 0;
    obj_t err = NULL;
    obj_t lib = ((obj_t (*)(obj_t, sel_t, obj_t, void *, void **))o_msgSend)(
        g_device, o_sel("newLibraryWithSource:options:error:"),
        nsstr(MSL_SRC), NULL, &err);
    if (!lib) { nserr_line("MSL 编译失败", err); return 0; }

    /* 便捷构造 newRenderPipelineStateWithVertexFunction:fragmentFunction:error:
       在新 macOS 运行时已被移除(实测 unrecognized selector) —— 统一走
       **描述符**路线 newRenderPipelineStateWithDescriptor:error:(顶/片段
       函数都在描述符里), 新旧运行时都在。 */
    if (!HAS_SEL(g_device, "newRenderPipelineStateWithDescriptor:error:")) return 0;

    obj_t f_vs = MSG1(obj_t, lib, "newFunctionWithName:", nsstr("hn_vs"));
    obj_t f_fs_solid = MSG1(obj_t, lib, "newFunctionWithName:", nsstr("hn_fs_solid"));
    obj_t f_fs_tex = MSG1(obj_t, lib, "newFunctionWithName:", nsstr("hn_fs_tex"));
    if (!f_vs || !f_fs_solid || !f_fs_tex) { MSG0(void, lib, "release"); return 0; }

    /* 两管线(同库不同片段入口): BGRA8Unorm + 预乘 src-over 混合
       (src-one / dst-one-minus-src-alpha —— 与 hnsoft readback 的换算注释
       见文件头注 2 与 hn_gpu.c 头注释) */
    for (int p = 0; p < 2; p++) {
        obj_t pd = MSG0(obj_t, o_getClass("MTLRenderPipelineDescriptor"), "alloc");
        pd = MSG0(obj_t, pd, "init");
        MSG1(void, pd, "setVertexFunction:", f_vs);
        MSG1(void, pd, "setFragmentFunction:", p == 0 ? f_fs_solid : f_fs_tex);
        obj_t ca = color_attachment(pd, 0);
        if (!ca) { MSG0(void, pd, "release"); MSG0(void, lib, "release"); return 0; }
        MSGUL(void, ca, "setPixelFormat:", MTL_PF_BGRA8);
        MSGUL(void, ca, "setBlendingEnabled:", 1ul);
        MSGUL(void, ca, "setSourceRGBBlendFactor:", MTL_BF_ONE);
        MSGUL(void, ca, "setSourceAlphaBlendFactor:", MTL_BF_ONE);
        MSGUL(void, ca, "setDestinationRGBBlendFactor:", MTL_BF_ONE_MINUS_SRC_ALPHA);
        MSGUL(void, ca, "setDestinationAlphaBlendFactor:", MTL_BF_ONE_MINUS_SRC_ALPHA);
        obj_t pso_err = NULL;
        obj_t pso = ((obj_t (*)(obj_t, sel_t, obj_t, void **))o_msgSend)(
            g_device, o_sel("newRenderPipelineStateWithDescriptor:error:"),
            pd, &pso_err);
        MSG0(void, pd, "release");
        if (!pso) { nserr_line("管线创建失败", pso_err);
                    MSG0(void, f_vs, "release"); MSG0(void, f_fs_solid, "release");
                    MSG0(void, f_fs_tex, "release"); MSG0(void, lib, "release");
                    return 0; }
        if (p == 0) g_pso_solid = pso; else g_pso_tex = pso;
    }
    MSG0(void, f_vs, "release");
    MSG0(void, f_fs_solid, "release");
    MSG0(void, f_fs_tex, "release");
    MSG0(void, lib, "release");

    /* 线性采样器(理由见 g_samp 注释)。MTLDevice 协议只有无 error: 变体 */
    obj_t sd = MSG0(obj_t, o_getClass("MTLSamplerDescriptor"), "alloc");
    sd = MSG0(obj_t, sd, "init");
    MSGUL(void, sd, "setMinFilter:", 1ul);   /* MTLSamplerMinMagFilterLinear = 1 */
    MSGUL(void, sd, "setMagFilter:", 1ul);
    g_samp = MSG1(obj_t, g_device, "newSamplerStateWithDescriptor:", sd);
    MSG0(void, sd, "release");
    if (!g_samp) return 0;

    /* 索引缓冲一次建满: 每四边形 6 索引, 拆分 (0,1,2)+(0,2,3) —— 对**周界
       序**(v0→v1→v2→v3 沿边走, 顺/逆时针皆可)的凸四边形, 两个三角形绕序
       必然一致(都沿周界方向), 任何 cull/frontFace 缺省下都完整成面。
       引擎侧顶点沿周界序(契约注释同步); 之字形序(TL,TR,BL,BR)两三角形
       绕序相反, 背面那个被剔除(实测本运行时 cull 缺省开且编码器
       setCullMode:None 不生效 —— 以周界序绕开该不可靠性)。 */
    g_ibuf = MSGULL(obj_t, g_device, "newBufferWithLength:options:",
                    (unsigned long)sizeof(unsigned int) * 6 * IBO_CAP_QUADS, 0ul);
    if (!g_ibuf) return 0;
    unsigned int *idx = (unsigned int *)MSG0(void *, g_ibuf, "contents");
    for (int q = 0; q < IBO_CAP_QUADS; q++) {
        unsigned int v = (unsigned int)q * 4u;
        idx[q * 6 + 0] = v; idx[q * 6 + 1] = v + 1; idx[q * 6 + 2] = v + 2;
        idx[q * 6 + 3] = v; idx[q * 6 + 4] = v + 2; idx[q * 6 + 5] = v + 3;
    }
    /* Shared 缓冲 contents 写入即对 GPU 可见(无需 didModifyRange) */

    g_ready = 1;
    return 1;
}

static void mtl_shutdown(void) {
    if (!g_ready) return;
    for (int i = 0; i < g_tex_n; i++) tex_release_slot(i);
    g_tex_n = 0;
    if (g_rt) { MSG0(void, g_rt, "release"); g_rt = NULL; }
    if (g_vbuf) { MSG0(void, g_vbuf, "release"); g_vbuf = NULL; }
    g_vcap_bytes = 0;
    g_voff = 0;
    if (g_ibuf) { MSG0(void, g_ibuf, "release"); g_ibuf = NULL; }
    if (g_pso_solid) { MSG0(void, g_pso_solid, "release"); g_pso_solid = NULL; }
    if (g_pso_tex) { MSG0(void, g_pso_tex, "release"); g_pso_tex = NULL; }
    if (g_samp) { MSG0(void, g_samp, "release"); g_samp = NULL; }
    if (g_queue) { MSG0(void, g_queue, "release"); g_queue = NULL; }
    if (g_device) { MSG0(void, g_device, "release"); g_device = NULL; }
    if (g_pool) { MSG0(void, g_pool, "drain"); g_pool = NULL; }
    g_ready = 0;
}

/* ---------------- 有界 autorelease 池(帧窗口) ---------------- */

static void pool_push(void) {
    g_pool = MSG0(obj_t, o_getClass("NSAutoreleasePool"), "alloc");
    g_pool = MSG0(obj_t, g_pool, "init");
}
static void pool_pop(void) {
    if (g_pool) { MSG0(void, g_pool, "drain"); g_pool = NULL; }
}

/* ---------------- 纹理 ---------------- */

/* 采样纹理描述符(alloc/init + setter 路线): 便捷类方法
   texture2DDescriptorWithPixelFormat:width:height:resourceOptions: 在本机
   macOS 运行时已不可响应(实测 respondsToSelector=0) —— alloc/init 加
   setter 是跨版本稳口径。RGBA8Unorm(=70), Shared 存储(Apple Silicon 纹理
   支持; 见文件头注 1), 用法 usage 入参(RT 追加 RenderTarget)。 */
static obj_t make_tex_desc(unsigned long pixel_format, unsigned long w, unsigned long h,
                           unsigned long usage) {
    obj_t td = MSG0(obj_t, o_getClass("MTLTextureDescriptor"), "alloc");
    td = MSG0(obj_t, td, "init");
    if (!td) return NULL;
    MSGUL(void, td, "setPixelFormat:", pixel_format);
    MSGUL(void, td, "setWidth:", w);
    MSGUL(void, td, "setHeight:", h);
    MSGUL(void, td, "setStorageMode:", MTL_SM_SHARED);
    MSGUL(void, td, "setUsage:", usage);
    return td;
}

static obj_t make_tex(int w, int h, unsigned usage) {
    obj_t td = make_tex_desc(MTL_PF_RGBA8, (unsigned long)w, (unsigned long)h, usage);
    if (!td) return NULL;
    obj_t t = MSG1(obj_t, g_device, "newTextureWithDescriptor:", td);
    MSG0(void, td, "release");
    return t;
}

/* RGBA8 非预乘 → 预乘(上传前; 混合在预乘域做 —— 契约与文件头注 2) */
static void premul_rgba(unsigned char *px, size_t n) {
    for (size_t i = 0; i < n; i++) {
        unsigned char *p = px + i * 4;
        unsigned a = p[3];
        if (a != 255) {
            p[0] = (unsigned char)((p[0] * a) / 255);
            p[1] = (unsigned char)((p[1] * a) / 255);
            p[2] = (unsigned char)((p[2] * a) / 255);
        }
    }
}

static hnp_gpu_tex mtl_tex_create(int w, int h, const unsigned char *rgba) {
    if (!g_ready || w <= 0 || h <= 0 || w > 16384 || h > 16384) return 0;
    if (g_tex_n >= TEX_CAP) return 0;
    obj_t t = make_tex(w, h, MTL_TU_SHREAD);
    if (!t) return 0;
    /* replaceRegion: 上传整幅(预乘后)。Shared 纹理 CPU 写即生效。 */
    unsigned char *buf = (unsigned char *)malloc((size_t)w * h * 4);
    if (!buf) { MSG0(void, t, "release"); return 0; }
    memcpy(buf, rgba, (size_t)w * h * 4);
    premul_rgba(buf, (size_t)w * h);
    MTLRegion_ r = { { 0, 0, 0 }, { (unsigned long)w, (unsigned long)h, 1 } };
    /* 无 slice 变体(带 slice 的旧选择器在新 macOS 运行时已移除, 实测) */
    ((void (*)(obj_t, sel_t, MTLRegion_, unsigned long,
               const void *, unsigned long))o_msgSend)(
        t, o_sel("replaceRegion:mipmapLevel:withBytes:bytesPerRow:"),
        r, 0ul, buf, (unsigned long)w * 4);
    free(buf);
    g_tex[g_tex_n].tex = t;
    g_tex[g_tex_n].w = w;
    g_tex[g_tex_n].h = h;
    return (hnp_gpu_tex)(++g_tex_n);   /* 句柄 = 下标+1(0 保留为无效) */
}

static void mtl_tex_update(hnp_gpu_tex t, const unsigned char *rgba) {
    int i = (int)t - 1;
    if (!g_ready || t == 0 || i < 0 || i >= g_tex_n || !g_tex[i].tex || !rgba) return;
    int w = g_tex[i].w, h = g_tex[i].h;
    unsigned char *buf = (unsigned char *)malloc((size_t)w * h * 4);
    if (!buf) return;
    memcpy(buf, rgba, (size_t)w * h * 4);
    premul_rgba(buf, (size_t)w * h);
    MTLRegion_ r = { { 0, 0, 0 }, { (unsigned long)w, (unsigned long)h, 1 } };
    ((void (*)(obj_t, sel_t, MTLRegion_, unsigned long,
               const void *, unsigned long))o_msgSend)(
        g_tex[i].tex, o_sel("replaceRegion:mipmapLevel:withBytes:bytesPerRow:"),
        r, 0ul, buf, (unsigned long)w * 4);
    free(buf);
}

static void mtl_tex_free(hnp_gpu_tex t) {
    int i = (int)t - 1;
    if (t == 0 || i < 0 || i >= g_tex_n) return;
    tex_release_slot(i);
    g_tex[i] = g_tex[g_tex_n - 1];   /* 交换删除; 已发出去的句柄失效由引擎侧
                                        缓存管理保证(逐出时先摘缓存再 free) */
    g_tex_n--;
}

/* ---------------- 渲染目标 / 帧窗口 ---------------- */

static int mtl_begin(int w, int h, unsigned bg) {
    if (!g_ready || w <= 0 || h <= 0 || w > 16384 || h > 16384) return 0;
    /* 上一帧若异常中止(begin 后没走到 end_readback), 先把挂着帧收干净
       (encoder 不 endEncoding 就 release 会触发 Metal 校验断言, 实测
       abort), 再回收池 —— 引擎侧"逐帧静默回退"允许任何一帧被放弃。 */
    if (g_enc) {
        MSG0(void, g_enc, "endEncoding");
        g_enc = NULL;
        MSG0(void, g_cmdbuf, "commit");
        MSG0(void, g_cmdbuf, "waitUntilCompleted");
        g_cmdbuf = NULL;
    }
    if (g_pool) pool_pop();
    g_enc = NULL; g_cmdbuf = NULL;

    pool_push();

    if (g_rt && (g_rtw != w || g_rth != h)) {
        MSG0(void, g_rt, "release");
        g_rt = NULL;
    }
    if (!g_rt) {
        /* 渲染目标 = BGRA8Unorm(字节序 = readback 契约) + RenderTarget|ShaderRead */
        obj_t td = make_tex_desc(MTL_PF_BGRA8, (unsigned long)w, (unsigned long)h,
                                 MTL_TU_SHREAD | MTL_TU_RENDERT);
        if (!td) { pool_pop(); return 0; }
        g_rt = MSG1(obj_t, g_device, "newTextureWithDescriptor:", td);
        MSG0(void, td, "release");
        if (!g_rt) { pool_pop(); return 0; }
        g_rtw = w; g_rth = h;
    }

    /* 清屏色 = 引擎底色(RGBA8 非预乘) → **预乘**(帧缓冲是预乘域;
       半透明底色(透明窗口 bg alpha=0)换算后为全零 = 透明黑, 与 hnsoft
       fb_init 的"底色 alpha 原样入帧缓冲"在预乘域等价) */
    MTLClearColor_ cc;
    cc.a = (double)(bg & 0xFFu) / 255.0;
    cc.r = (double)((bg >> 24) & 0xFFu) / 255.0 * cc.a;
    cc.g = (double)((bg >> 16) & 0xFFu) / 255.0 * cc.a;
    cc.b = (double)((bg >> 8) & 0xFFu) / 255.0 * cc.a;

    g_cmdbuf = MSG0(obj_t, g_queue, "commandBuffer");
    if (!g_cmdbuf) { pool_pop(); return 0; }
    obj_t rpd = MSG0(obj_t, o_getClass("MTLRenderPassDescriptor"), "renderPassDescriptor");
    if (!rpd) { pool_pop(); return 0; }
    obj_t ca = color_attachment(rpd, 0);
    if (!ca) { pool_pop(); return 0; }
    MSG1(void, ca, "setTexture:", g_rt);
    MSGUL(void, ca, "setLoadAction:", MTL_LA_CLEAR);
    MSGUL(void, ca, "setStoreAction:", MTL_SA_STORE);
    ((void (*)(obj_t, sel_t, MTLClearColor_))o_msgSend)(ca, o_sel("setClearColor:"), cc);

    g_enc = MSG1(obj_t, g_cmdbuf, "renderCommandEncoderWithDescriptor:", rpd);
    if (!g_enc) { pool_pop(); return 0; }

    /* viewport 显式置满(不依赖缺省); y 翻转在顶点着色器做(文件头注 3) */
    MTLViewport_ vp = { 0, 0, (double)w, (double)h, 0, 1 };
    ((void (*)(obj_t, sel_t, MTLViewport_))o_msgSend)(g_enc, o_sel("setViewport:"), vp);
    /* 无背面剔除(引擎四边形两个朝向都有: 3D 投影可翻转面) */
    MSGUL(void, g_enc, "setCullMode:", 0ul);   /* MTLCullModeNone */

    g_clip_n = 0;   /* 帧内裁剪栈归零(clip_push/pop 由引擎保证配平) */
    g_voff = 0;     /* 顶点 arena 写头归零(帧内追加制, 见 draw_quads 注释) */
    return 1;
}

/* 裁剪: 轴对齐矩形入栈, scissor = 与栈内逐层**交叠**(hnsoft fb_push_clip
 * 同一口径 —— 引擎语义是矩形交叠, 不是替换)。像素坐标直通。 */
static void mtl_clip_push(int x, int y, int w, int h) {
    if (!g_enc) return;
    int x0 = x, y0 = y, x1 = x + w, y1 = y + h;
    int bx0 = 0, by0 = 0, bx1 = g_rtw, by1 = g_rth;
    if (g_clip_n > 0) {
        bx0 = g_clip[g_clip_n - 1].x0; by0 = g_clip[g_clip_n - 1].y0;
        bx1 = g_clip[g_clip_n - 1].x1; by1 = g_clip[g_clip_n - 1].y1;
    }
    if (g_clip_n < CLIP_CAP) {
        g_clip[g_clip_n].x0 = x0 > bx0 ? x0 : bx0;
        g_clip[g_clip_n].y0 = y0 > by0 ? y0 : by0;
        g_clip[g_clip_n].x1 = x1 < bx1 ? x1 : bx1;
        g_clip[g_clip_n].y1 = y1 < by1 ? y1 : by1;
        g_clip_n++;
    }
    /* 空交叠 → 零面积 scissor(什么都不画; hnsoft 的 clip 判定同样全挡) */
    int sw = g_clip[g_clip_n - 1].x1 - g_clip[g_clip_n - 1].x0;
    int sh = g_clip[g_clip_n - 1].y1 - g_clip[g_clip_n - 1].y0;
    if (sw < 0) sw = 0;
    if (sh < 0) sh = 0;
    MTLScissorRect_ sr = { (unsigned long)g_clip[g_clip_n - 1].x0,
                           (unsigned long)g_clip[g_clip_n - 1].y0,
                           (unsigned long)sw, (unsigned long)sh };
    ((void (*)(obj_t, sel_t, MTLScissorRect_))o_msgSend)(g_enc, o_sel("setScissorRect:"), sr);
}

static void mtl_clip_pop(void) {
    if (!g_enc) return;
    if (g_clip_n > 0) g_clip_n--;
    MTLScissorRect_ sr;
    if (g_clip_n > 0) {
        int sw = g_clip[g_clip_n - 1].x1 - g_clip[g_clip_n - 1].x0;
        int sh = g_clip[g_clip_n - 1].y1 - g_clip[g_clip_n - 1].y0;
        if (sw < 0) sw = 0;
        if (sh < 0) sh = 0;
        sr.x = (unsigned long)g_clip[g_clip_n - 1].x0;
        sr.y = (unsigned long)g_clip[g_clip_n - 1].y0;
        sr.width = (unsigned long)sw;
        sr.height = (unsigned long)sh;
    } else {
        sr.x = 0; sr.y = 0;
        sr.width = (unsigned long)g_rtw;
        sr.height = (unsigned long)g_rth;
    }
    ((void (*)(obj_t, sel_t, MTLScissorRect_))o_msgSend)(g_enc, o_sel("setScissorRect:"), sr);
}

/* ---------------- 四边形批量绘制 ----------------
 * 顶点驻留 = **帧内追加 arena**: 每次调用把顶点 memcpy 到帧缓冲的下一
 * 偏移并按该偏移绑定(setVertexBuffer:offset:)。不能复用同一段缓冲 ——
 * GPU 在 commit 后才执行, 复用 offset 0 会让所有 draw 读到最后一次写入
 * 的顶点(实测: 一帧三个 draw 只见最后一个四边形)。setVertexBytes 的
 * uniform 是编码期快照, 无此问题; Shared 缓冲 contents 写入即对 GPU
 * 可见, 配合单调偏移即天然一致。帧开始时 arena 归零(begin)。 */

/* 确保 arena 容量 ≥ 已用 + need 字节(不够则换新缓冲; 旧 draw 已编码,
   command buffer 自持被引用资源, 换缓冲不影响在途命令) */
static int vbuf_ensure(size_t need) {
    if (g_vbuf && g_vcap_bytes - g_voff >= need) return 1;
    unsigned long want = g_vcap_bytes ? g_vcap_bytes : 4096 * 64;
    while (want < g_voff + need) want *= 2;
    obj_t nb = MSGULL(obj_t, g_device, "newBufferWithLength:options:", want, 0ul);
    if (!nb) return 0;
    if (g_vbuf) MSG0(void, g_vbuf, "release");
    g_vbuf = nb;
    g_vcap_bytes = want;
    return 1;
}

/* rgba(0xRRGGBBAA 非预乘) → 预乘 float4 */
static void premul_color(unsigned rgba, float *out4) {
    float a = (float)(rgba & 0xFFu) / 255.0f;
    out4[0] = (float)((rgba >> 24) & 0xFFu) / 255.0f * a;
    out4[1] = (float)((rgba >> 16) & 0xFFu) / 255.0f * a;
    out4[2] = (float)((rgba >> 8) & 0xFFu) / 255.0f * a;
    out4[3] = a;
}

static int mtl_draw_quads(const float *verts, int quad_count,
                          hnp_gpu_tex tex, unsigned rgba) {
    if (!g_enc || !verts || quad_count <= 0) return 0;
    size_t need = (size_t)quad_count * 16 * 4;   /* 4 顶点 × 4 float */
    if (!vbuf_ensure(need)) return 0;
    memcpy((unsigned char *)MSG0(void *, g_vbuf, "contents") + g_voff, verts, need);

    mtl_uniforms u;
    premul_color(rgba, u.color);
    u.w = (float)g_rtw;
    u.h = (float)g_rth;
    u.pad = 0.0f;

    MSG1(void, g_enc, "setRenderPipelineState:", tex ? g_pso_tex : g_pso_solid);
    ((void (*)(obj_t, sel_t, obj_t, unsigned long, unsigned long))o_msgSend)(
        g_enc, o_sel("setVertexBuffer:offset:atIndex:"), g_vbuf,
        (unsigned long)g_voff, 0ul);
    ((void (*)(obj_t, sel_t, const void *, unsigned long, unsigned long))o_msgSend)(
        g_enc, o_sel("setVertexBytes:length:atIndex:"), &u, (unsigned long)sizeof(u), 1ul);
    ((void (*)(obj_t, sel_t, const void *, unsigned long, unsigned long))o_msgSend)(
        g_enc, o_sel("setFragmentBytes:length:atIndex:"), &u, (unsigned long)sizeof(u), 0ul);
    if (tex) {
        int i = (int)tex - 1;
        if (i < 0 || i >= g_tex_n || !g_tex[i].tex) return 0;
        ((void (*)(obj_t, sel_t, obj_t, unsigned long))o_msgSend)(
            g_enc, o_sel("setFragmentTexture:atIndex:"), g_tex[i].tex, 0ul);
        ((void (*)(obj_t, sel_t, obj_t, unsigned long))o_msgSend)(
            g_enc, o_sel("setFragmentSamplerState:atIndex:"), g_samp, 0ul);
    }

    int done = 0;
    while (done < quad_count) {
        int chunk = quad_count - done;
        if (chunk > IBO_CAP_QUADS) chunk = IBO_CAP_QUADS;
        ((void (*)(obj_t, sel_t, unsigned long, unsigned long, unsigned long,
                   obj_t, unsigned long))o_msgSend)(
            g_enc, o_sel("drawIndexedPrimitives:indexCount:indexType:indexBuffer:indexBufferOffset:"),
            MTL_PT_TRIANGLE, (unsigned long)chunk * 6, MTL_IT_U32,
            g_ibuf, (unsigned long)done * 6 * sizeof(unsigned int));
        done += chunk;
    }
    g_voff += need;   /* arena 前进(下一调用不覆盖本调用的顶点) */
    return 1;
}

/* ---------------- 帧结束 / readback ---------------- */

static int mtl_end_readback(unsigned char *out_bgra, int w, int h) {
    if (!g_cmdbuf || !g_enc || !out_bgra || w != g_rtw || h != g_rth) {
        g_enc = NULL; g_cmdbuf = NULL;
        pool_pop();
        return 0;
    }
    MSG0(void, g_enc, "endEncoding");
    g_enc = NULL;
    MSG0(void, g_cmdbuf, "commit");
    MSG0(void, g_cmdbuf, "waitUntilCompleted");   /* v1 同步帧(GPU 离屏 + readback
                                                     本来就要等结果; 直呈模式再异步) */
    /* Shared 纹理: CPU 直接 getBytes(免 Managed 的 synchronizeResource)。 */
    MTLRegion_ r = { { 0, 0, 0 }, { (unsigned long)w, (unsigned long)h, 1 } };
    ((void (*)(obj_t, sel_t, void *, unsigned long, MTLRegion_, unsigned long))o_msgSend)(
        g_rt, o_sel("getBytes:bytesPerRow:fromRegion:mipmapLevel:"),
        out_bgra, (unsigned long)w * 4, r, 0ul);
    g_cmdbuf = NULL;
    pool_pop();
    return 1;
}

/* supported_cmds: 1=RECT 2=TEXT 3=IMAGE 4=CLIP_PUSH 5=CLIP_POP 6=QUAD
   9=BITMAP(数值 = hn_cmd_kind, 对照注释见文件头) */
#define HNC_BIT(kind) (1u << (kind))
static const hnp_gpu_host g_metal_host = {
    { HNC_BIT(1) | HNC_BIT(2) | HNC_BIT(3) | HNC_BIT(4) | HNC_BIT(5)
      | HNC_BIT(6) | HNC_BIT(9), "metal" },
    mtl_init,
    mtl_shutdown,
    mtl_begin,
    mtl_end_readback,
    mtl_tex_create,
    mtl_tex_update,
    mtl_tex_free,
    mtl_draw_quads,
    mtl_clip_push,
    mtl_clip_pop,
};

const hnp_gpu_host *hnp_gpu_backend(void) {
    /* 惰性 init(首次询问才装 Metal/编着色器 —— 缺省 HN_GPU 未设时
       hn_app 根本不会调到这里, 零开销)。函数名见 hn_platform.h 尾注
       (typedef hnp_gpu_host 与同名函数在 C 的单一普通标识符命名空间冲突)。 */
    if (!g_ready && !mtl_init()) return NULL;
    return &g_metal_host;
}

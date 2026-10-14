/* hn_pe.c — PainterEngine 集成(开源软渲染核心出像素, BSD-3)
 * ============================================================
 * hn-live="<image>" 节点: 贴图经 PainterEngine 的 px_surface /
 * PX_TextureRenderEx_sincos(缩放+旋转软光栅)渲染到离屏 surface,
 * 再以 HN_CMD_BITMAP 上屏(透明底 RGBA — PE 缺省 PX_COLOR_FORMAT_RGBA
 * 与引擎位图契约零转换)。rig(呼吸/摇摆/跳跃)由引擎按 ext_clock 驱动
 * —— 演示"引擎驱动 + PE 渲染"的分层; PX_Live 框架接入后续再上。
 * 未定义 HN_HAVE_PE(构建未含 PE 源)时整模块出局, paint 为空操作。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hn_internal.h"
#include "hn_png.h"

#ifdef HN_HAVE_PE
#include "PX_Surface.h"
#include "PX_Texture.h"

#define PE_MP_SIZE (32 * 1024 * 1024)
static unsigned char pe_mp_buf[PE_MP_SIZE];
static px_memorypool pe_mp_storage;
static px_memorypool *pe_mp = NULL;
static int pe_inited = 0;

/* 单角色贴图缓存(进程级 2 槽) */
#define PE_CACHE 2
struct pe_tex {
    char *path;
    px_texture tex;
    int w, h;
    unsigned char *rgba;       /* hn_png 解码缓冲(自有) */
};
static struct pe_tex g_pe_cache[PE_CACHE];

static void pe_init(void) {
    if (pe_inited) return;
    pe_mp_storage = MP_Create(pe_mp_buf, PE_MP_SIZE);
    pe_mp = &pe_mp_storage;     /* PE 约定: 池头按值返回, 使用方持指针 */
    pe_inited = 1;
}

static struct pe_tex *pe_tex_acquire(hn_context *c, const char *path) {
    for (int i = 0; i < PE_CACHE; i++)
        if (g_pe_cache[i].path && !strcmp(g_pe_cache[i].path, path))
            return &g_pe_cache[i];
    if (!c) return NULL;
    const hn_asset_backend *ab = hn_context_assets(c);
    if (!ab || !ab->load) return NULL;
    size_t len = 0;
    const char *bytes = ab->load(ab->ctx, path, &len);
    if (!bytes || len == 0) return NULL;

    int w = 0, h = 0;
    unsigned char *rgba = hn_png_decode((const unsigned char *)bytes, len, &w, &h);
    if (!rgba || w <= 0 || h <= 0) return NULL;

    int slot = -1;
    for (int i = 0; i < PE_CACHE; i++) if (!g_pe_cache[i].path) { slot = i; break; }
    if (slot < 0) {
        slot = 0;
        /* 逐出: tex.surfaceBuffer 是 PE 池内存, 必须 PX_TextureFree 归还;
           只 free rgba/path 会让池在 2 槽轮换后持续漏到 MP_Malloc 失败 */
        PX_TextureFree(&g_pe_cache[slot].tex);
        free(g_pe_cache[slot].rgba);
        free(g_pe_cache[slot].path);
    }

    struct pe_tex *t = &g_pe_cache[slot];
    memset(t, 0, sizeof(*t));
    t->path = (char *)malloc(strlen(path) + 1);
    strcpy(t->path, path);
    t->rgba = rgba;
    t->w = w; t->h = h;
    pe_init();
    if (!pe_mp || !PX_TextureCreate(pe_mp, &t->tex, w, h)) {
        /* 失败即出清本槽: 不留带 path 的毒条目(否则同路径重试会命中空白
           缓存, 永远加载不出)。PX_SurfaceCreate 失败时不写 tex(见
           PX_Surface.c), 本槽无池内存可还, 直接释放宿主侧缓冲。 */
        free(t->rgba);
        free(t->path);
        memset(t, 0, sizeof(*t));
        return NULL;
    }
    /* RGBA → px_color(缺省 PX_COLOR_FORMAT_RGBA: 内存同序, 逐像素拷) */
    px_color *px = t->tex.surfaceBuffer;
    for (int i = 0; i < w * h; i++) {
        px_color cc;
        cc._argb.r = rgba[i * 4];
        cc._argb.g = rgba[i * 4 + 1];
        cc._argb.b = rgba[i * 4 + 2];
        cc._argb.a = rgba[i * 4 + 3];
        px[i] = cc;
    }
    return t;
}

/* 复用双缓冲(BITMAP 契约: 缓冲存活到下次 repaint — 双槽轮转) */
static px_surface g_pe_surf[2];
static int g_pe_slot = 0;
static int g_pe_surf_w[2], g_pe_surf_h[2];

void hn_pe_paint(hn_context *c, hn_node *n,
                 float bw, float bh, float sx, float sy, float alpha) {
    const char *src = hn_node_attr(n, "hn-live");
    if (!src || !*src || bw < 8 || bh < 8) return;
    struct pe_tex *t = pe_tex_acquire(c, src);
    if (!t) return;
    pe_init();
    if (!pe_mp) return;

    int W = (int)bw, H = (int)bh;
    int slot = g_pe_slot ^ 1;          /* 与上帧不同槽(双缓冲) */
    g_pe_slot = slot;
    px_surface *s = &g_pe_surf[slot];
    if (g_pe_surf_w[slot] != W || g_pe_surf_h[slot] != H) {
        if (g_pe_surf_w[slot]) PX_SurfaceFree(s);
        g_pe_surf_w[slot] = 0;
        g_pe_surf_h[slot] = 0;
        memset(s, 0, sizeof(*s));
        if (!PX_SurfaceCreate(pe_mp, W, H, s)) return;
        g_pe_surf_w[slot] = W; g_pe_surf_h[slot] = H;
    }
    {
        px_color z;
        z._argb.r = 0; z._argb.g = 0; z._argb.b = 0; z._argb.a = 0;
        PX_SurfaceClear(s, 0, 0, W - 1, H - 1, z);
    }

    /* rig: 呼吸(整体缩放) + 摇摆(微旋转) + 跳跃(y 位移) */
    float clk = (float)(n->ext_clock) * 0.001f;
    float breathe = 1.0f + 0.025f * sinf(clk * 1.57f);
    float sway = 0.06f * sinf(clk * 0.9f);
    float hop = 6.0f * fabsf(sinf(clk * 2.2f));
    float scale = (W > H ? H : W) / (float)(t->w > t->h ? t->h : t->w) * 0.92f * breathe;
    float cx = W * 0.5f, cy = H * 0.5f + (H - t->h * scale) * 0.5f - hop;
    PX_TextureRenderEx_sincos(s, &t->tex, (int)cx, (int)cy, PX_ALIGN_CENTER,
                              NULL, scale, sinf(sway), cosf(sway));

    /* opacity: hnsoft 的 paint_bitmap 只消费位图像素 alpha(不读 cmd.fill 的
       alpha 字节), 故把节点透明度直接乘进 surface 像素。surface 每帧先
       PX_SurfaceClear(全零) 再重画, 原地乘不会跨帧累积。surface 底为全透明,
       乘后 alpha 即"整层以该透明度合成"的精确结果。 */
    if (alpha < 0.999f) {
        px_color *sp = s->surfaceBuffer;
        int fac = (int)(alpha * 255.0f + 0.5f);
        if (fac < 0) fac = 0;
        if (fac > 255) fac = 255;
        for (int i = 0; i < W * H; i++) {
            unsigned a = sp[i]._argb.a;
            sp[i]._argb.a = (unsigned char)((a * (unsigned)fac + 127u) / 255u);
        }
    }

    hn_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = HN_CMD_BITMAP;
    cmd.bitmap = (const unsigned char *)s->surfaceBuffer;
    cmd.bitmap_stride = W * 4;
    cmd.bitmap_w = W; cmd.bitmap_h = H;
    cmd.x = sx; cmd.y = sy; cmd.w = bw; cmd.h = bh;
    cmd.fill = 0xFFFFFF00u | (unsigned)(alpha * 255.0f);
    hn_paint_push(c, &cmd);
}

#else /* !HN_HAVE_PE —— 构建未含 PE: 空操作(hn-live 节点不落像素) */

void hn_pe_paint(hn_context *c, hn_node *n,
                 float bw, float bh, float sx, float sy, float alpha) {
    (void)c; (void)n; (void)bw; (void)bh; (void)sx; (void)sy; (void)alpha;
}

#endif /* HN_HAVE_PE */

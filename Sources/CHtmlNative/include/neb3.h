/* neb3.h — Neb 3D runtime C ABI
 * =================================
 * Stable POD-only boundary for optional Rust/wgpu backend. The default C99
 * build uses neb3_soft.c; a Rust cdylib may export the same symbols.
 * No Rust/wgpu types cross this header.
 */
#ifndef NEB3_H
#define NEB3_H

#include <stddef.h>
#ifdef __cplusplus
extern "C" {
#endif

typedef struct neb3_runtime neb3_runtime;
typedef struct neb3_scene neb3_scene;
typedef struct neb3_mesh neb3_mesh;
typedef struct neb3_material neb3_material;

typedef struct neb3_vec3 { float x, y, z; } neb3_vec3;
typedef struct neb3_quat { float x, y, z, w; } neb3_quat;
typedef struct neb3_color { float r, g, b, a; } neb3_color;

typedef struct neb3_desc {
    int width;
    int height;
    int msaa_samples;       /* 1, 2, 4, 8; backend may downgrade */
    int transparent;        /* clear alpha = 0 when non-zero */
    float pixel_ratio;
} neb3_desc;

typedef struct neb3_render_info {
    int width;
    int height;
    int msaa_samples;
    int depth_enabled;
    int gpu;
} neb3_render_info;

neb3_runtime *neb3_create(const neb3_desc *desc);
void neb3_destroy(neb3_runtime *rt);
neb3_scene *neb3_scene_create(neb3_runtime *rt);
void neb3_scene_destroy(neb3_scene *scene);
neb3_mesh *neb3_box_mesh(neb3_runtime *rt, float sx, float sy, float sz);
void neb3_mesh_destroy(neb3_mesh *mesh);
neb3_material *neb3_material_unlit(neb3_runtime *rt, neb3_color color);
void neb3_material_destroy(neb3_material *material);
int neb3_scene_add_box(neb3_scene *scene, neb3_mesh *mesh, neb3_material *material);
int neb3_scene_set_camera_perspective(neb3_scene *scene, float fov_deg,
                                       float aspect, float near_z, float far_z);
int neb3_scene_set_rotation(neb3_scene *scene, float radians);
int neb3_scene_set_depth(neb3_scene *scene, int enabled);
int neb3_render(neb3_scene *scene, float time_ms, const neb3_color *clear,
                const unsigned char **rgba, size_t *bytes, neb3_render_info *info);
void neb3_release_frame(neb3_scene *scene);
const char *neb3_last_error(neb3_runtime *rt);

/* ---- 原生纹理直入合成器(macOS, wgpu↔Metal IOSurface 互操作) ----
 * neb3 用 wgpu 把帧**直接渲进 IOSurface**(跨 MTLDevice 共享的 Apple
 * 标准机制), 合成器侧在宿主 Metal 设备上把同一 IOSurface 包成
 * MTLTexture 即可零回读上屏 —— 消灭 GPU→CPU→GPU 的一帧往返。
 * 同步纪律: render 返回前 wgpu device.poll(Wait) 保证 GPU 完成写
 * IOSurface; 宿主同帧在其后消费(严格跨设备同步升级 = MTLSharedEvent,
   排期)。IOSurface 由 scene 持有, 调用方不得释放; surface 尺寸 =
   runtime 创建尺寸(改尺寸 = 重建 runtime)。 */
typedef struct neb3_native {
    void *iosurface;             /* IOSurfaceRef; scene 持有, 不得释放 */
    int width;
    int height;
    unsigned long long generation; /* 每次成功渲染递增(1 起) */
} neb3_native;

/* 进入原生表面模式: 首次调用惰性创建 IOSurface + IOSurface 纹理;
   之后每次渲染复用(尺寸不变)。返回 0 = 失败(见 neb3_last_error)。 */
int neb3_render_native(neb3_scene *scene, float time_ms, const neb3_color *clear,
                       neb3_native *out);
/* 原生模式信息(未进入原生模式时 iosurface 为 NULL)。 */
int neb3_native_info(neb3_scene *scene, neb3_native *out);
/* 读出原生表面当前内容(锁 IOSurface, 去行距): 成功返回写入字节数,
   未进入原生模式返回 0, 失败返回负值。 */
int neb3_native_read_rgba(neb3_scene *scene, unsigned char *buf, size_t cap);

#ifdef __cplusplus
}
#endif
#endif

/* neb3.h — Neb 3D runtime C ABI
 * =================================
 * Stable POD-only boundary for optional Rust/wgpu backend. The default C99
 * build uses hn_three_soft.c; a Rust cdylib may export the same symbols.
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

#ifdef __cplusplus
}
#endif
#endif

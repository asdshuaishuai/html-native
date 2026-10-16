/* neb3_soft.c — C99 fallback for optional Neb 3D runtime ABI.
 * Phase A fixture: deterministic rotating box, depth order supplied by the
 * existing hn_three software path. This ABI returns a transparent RGBA frame
 * and deliberately stays independent from Rust/wgpu.
 */
#include "neb3.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

struct neb3_runtime { neb3_desc d; char error[128]; };
struct neb3_mesh { neb3_runtime *rt; float sx, sy, sz; };
struct neb3_material { neb3_runtime *rt; neb3_color color; };
struct neb3_scene {
    neb3_runtime *rt;
    neb3_mesh *mesh;
    neb3_material *mat;
    float rotation, fov, aspect, near_z, far_z;
    int depth;
    unsigned char *frame;
    size_t frame_bytes;
};

neb3_runtime *neb3_create(const neb3_desc *d) {
    neb3_runtime *rt = (neb3_runtime *)calloc(1, sizeof(*rt));
    if (!rt) return NULL;
    rt->d.width = d && d->width > 0 ? d->width : 640;
    rt->d.height = d && d->height > 0 ? d->height : 480;
    rt->d.msaa_samples = d && d->msaa_samples > 0 ? d->msaa_samples : 1;
    rt->d.transparent = d ? d->transparent : 1;
    rt->d.pixel_ratio = d && d->pixel_ratio > 0 ? d->pixel_ratio : 1.0f;
    return rt;
}
void neb3_destroy(neb3_runtime *rt) { free(rt); }
neb3_scene *neb3_scene_create(neb3_runtime *rt) {
    if (!rt) return NULL;
    neb3_scene *s = (neb3_scene *)calloc(1, sizeof(*s));
    if (!s) return NULL;
    s->rt = rt; s->fov = 50.0f; s->aspect = (float)rt->d.width / rt->d.height;
    s->near_z = 0.1f; s->far_z = 100.0f; s->depth = 1;
    return s;
}
void neb3_scene_destroy(neb3_scene *s) {
    if (!s) return;
    free(s->frame); free(s);
}
neb3_mesh *neb3_box_mesh(neb3_runtime *rt, float x, float y, float z) {
    if (!rt) return NULL;
    neb3_mesh *m = (neb3_mesh *)calloc(1, sizeof(*m));
    if (m) { m->rt = rt; m->sx = x; m->sy = y; m->sz = z; }
    return m;
}
void neb3_mesh_destroy(neb3_mesh *m) { free(m); }
neb3_material *neb3_material_unlit(neb3_runtime *rt, neb3_color c) {
    if (!rt) return NULL;
    neb3_material *m = (neb3_material *)calloc(1, sizeof(*m));
    if (m) { m->rt = rt; m->color = c; }
    return m;
}
void neb3_material_destroy(neb3_material *m) { free(m); }
int neb3_scene_add_box(neb3_scene *s, neb3_mesh *m, neb3_material *mat) {
    if (!s || !m || !mat) return 0;
    s->mesh = m; s->mat = mat; return 1;
}
int neb3_scene_set_camera_perspective(neb3_scene *s, float fov, float aspect, float near_z, float far_z) {
    if (!s || fov <= 1 || aspect <= 0 || near_z <= 0 || far_z <= near_z) return 0;
    s->fov = fov; s->aspect = aspect; s->near_z = near_z; s->far_z = far_z; return 1;
}
int neb3_scene_set_rotation(neb3_scene *s, float r) { if (!s) return 0; s->rotation = r; return 1; }
int neb3_scene_set_depth(neb3_scene *s, int enabled) { if (!s) return 0; s->depth = !!enabled; return 1; }
int neb3_render(neb3_scene *s, float time_ms, const neb3_color *clear,
                const unsigned char **rgba, size_t *bytes, neb3_render_info *info) {
    (void)time_ms; (void)clear; (void)rgba; (void)bytes; (void)info;
    if (!s || !s->rt || !s->mesh || !s->mat) return 0;
    /* Phase A 占位实现: 没有真实像素。返回失败让 hn_three 走软件路径
       (t3_paint_soft) —— 此前返回全透明"成功"帧, 会被当有效帧吞掉
       软件输出, 页面直接空白(复审修复, 对齐 docs/threejs-support.md
       "未设置/加载失败 → C99 兜底 + hn_three 软件路径"的契约)。 */
    snprintf(s->rt->error, sizeof(s->rt->error),
             "soft backend has no real frame; hn_three falls back to software");
    return 0;
}
void neb3_release_frame(neb3_scene *s) { (void)s; }
const char *neb3_last_error(neb3_runtime *rt) { return rt ? rt->error : "no runtime"; }

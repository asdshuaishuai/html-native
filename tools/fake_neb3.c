/* fake_neb3.c — neb3 C ABI 的探针替身(dylib)
 *
 * 用途: 给 neb3_fallback_probe 提供一个**全符号齐、行为可控**的 backend,
 * 用对象级存活计数把 hn_three.c 永久回退路径的缓存槽释放钉成断言
 * (ISSUES.md 附: 存量简化 "neb3 永久回退后其余缓存槽泄漏" 的回归锚)。
 *
 * 与真 backend 的差异(即探针的价值):
 *   - create/scene_create/render 可由探针经 fake_neb3_fail_* 逐点打失败,
 *     精确命中 hn_three 的三条回退路径(创建失败/装配失败/渲染失败);
 *   - fake_neb3_live() 返回未释放的 rt+scene+mesh+material 总数 ——
 *     回退后必须归零, 否则即泄漏(对象级判据, 比 RSS 精确)。
 *
 * 编译: cc -std=c99 -dynamiclib tools/fake_neb3.c -o fake_neb3.dylib
 * (见 tools/neb3_fallback_probe.sh; Linux 用 -shared -fPIC)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "neb3.h"

static int g_live;        /* 未释放的 rt+scene+mesh+material 总数 */
static int g_created;     /* neb3_create 成功次数(永久性断言用) */
static int g_fail_create; /* neb3_create 返回 NULL */
static int g_fail_scene;  /* neb3_scene_create 返回 NULL */
static int g_fail_render; /* neb3_render 返回 0(渲染失败路径) */

typedef struct {
    int w, h;
    char err[128];
} fake_rt;

typedef struct {
    fake_rt *rt;
    unsigned char *frame;   /* render 产出的帧(release_frame/scene_destroy 归还) */
    size_t frame_bytes;
} fake_scene;

void fake_neb3_reset(void) {
    g_fail_create = g_fail_scene = g_fail_render = 0;
    /* 不清 g_live/g_created: 计数器是泄漏判据, 由探针在关键点断言 */
}
void fake_neb3_fail_create(int on) { g_fail_create = on; }
void fake_neb3_fail_scene(int on) { g_fail_scene = on; }
void fake_neb3_fail_render(int on) { g_fail_render = on; }
int fake_neb3_live(void) { return g_live; }
int fake_neb3_created(void) { return g_created; }

neb3_runtime *neb3_create(const neb3_desc *desc) {
    if (g_fail_create) return NULL;
    fake_rt *rt = (fake_rt *)calloc(1, sizeof(fake_rt));
    if (!rt) return NULL;
    rt->w = desc ? desc->width : 0;
    rt->h = desc ? desc->height : 0;
    snprintf(rt->err, sizeof(rt->err), "fake ok");
    g_live++;
    g_created++;
    return (neb3_runtime *)rt;
}

void neb3_destroy(neb3_runtime *rt) {
    if (!rt) return;
    free(rt);
    g_live--;
}

neb3_scene *neb3_scene_create(neb3_runtime *rt) {
    if (g_fail_scene) return NULL;
    fake_scene *sc = (fake_scene *)calloc(1, sizeof(fake_scene));
    if (!sc) return NULL;
    sc->rt = (fake_rt *)rt;
    g_live++;
    return (neb3_scene *)sc;
}

void neb3_scene_destroy(neb3_scene *scene) {
    if (!scene) return;
    fake_scene *sc = (fake_scene *)scene;
    free(sc->frame);
    free(sc);
    g_live--;
}

neb3_mesh *neb3_box_mesh(neb3_runtime *rt, float sx, float sy, float sz) {
    (void)rt; (void)sx; (void)sy; (void)sz;
    void *m = calloc(1, 16);
    if (!m) return NULL;
    g_live++;
    return (neb3_mesh *)m;
}

void neb3_mesh_destroy(neb3_mesh *mesh) {
    if (!mesh) return;
    free(mesh);
    g_live--;
}

neb3_material *neb3_material_unlit(neb3_runtime *rt, neb3_color color) {
    (void)rt; (void)color;
    void *m = calloc(1, 16);
    if (!m) return NULL;
    g_live++;
    return (neb3_material *)m;
}

void neb3_material_destroy(neb3_material *material) {
    if (!material) return;
    free(material);
    g_live--;
}

int neb3_scene_add_box(neb3_scene *scene, neb3_mesh *mesh, neb3_material *material) {
    (void)scene; (void)mesh; (void)material;
    return 1;
}

int neb3_scene_set_camera_perspective(neb3_scene *scene, float fov_deg,
                                      float aspect, float near_z, float far_z) {
    (void)scene; (void)fov_deg; (void)aspect; (void)near_z; (void)far_z;
    return 1;
}

int neb3_scene_set_rotation(neb3_scene *scene, float radians) {
    (void)scene; (void)radians;
    return 1;
}

int neb3_scene_set_depth(neb3_scene *scene, int enabled) {
    (void)scene; (void)enabled;
    return 1;
}

int neb3_render(neb3_scene *scene, float time_ms, const neb3_color *clear,
                const unsigned char **rgba, size_t *bytes, neb3_render_info *info) {
    (void)time_ms; (void)clear;
    fake_scene *sc = (fake_scene *)scene;
    if (!sc || !sc->rt || !rgba || !bytes || !info) return 0;
    if (g_fail_render) {
        snprintf(sc->rt->err, sizeof(sc->rt->err), "fake: render 注定失败(探针)");
        return 0;
    }
    int w = sc->rt->w > 0 ? sc->rt->w : 1;
    int h = sc->rt->h > 0 ? sc->rt->h : 1;
    size_t need = (size_t)w * (size_t)h * 4;
    if (sc->frame_bytes < need) {
        unsigned char *nb = (unsigned char *)realloc(sc->frame, need);
        if (!nb) return 0;
        sc->frame = nb;
        sc->frame_bytes = need;
    }
    /* 不透明蓝灰帧: 探针只断言"帧被接受", 不看像素 */
    memset(sc->frame, 0x40, need);
    for (int i = 3; i < (int)need; i += 4) sc->frame[i] = 0xFF;
    *rgba = sc->frame;
    *bytes = need;
    info->width = w;
    info->height = h;
    info->msaa_samples = 1;
    info->depth_enabled = 1;
    info->gpu = 0;
    return 1;
}

void neb3_release_frame(neb3_scene *scene) {
    if (!scene) return;
    fake_scene *sc = (fake_scene *)scene;
    free(sc->frame);
    sc->frame = NULL;
    sc->frame_bytes = 0;
}

const char *neb3_last_error(neb3_runtime *rt) {
    fake_rt *r = (fake_rt *)rt;
    return (r && r->err[0]) ? r->err : "fake: no error";
}

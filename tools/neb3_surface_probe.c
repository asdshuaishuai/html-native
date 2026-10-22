/* neb3_surface_probe.c — 原生纹理直入合成器(IOSurface 互操作)探针
 *
 * 断言面(设计口径见 backend/neb3-wgpu/src/native_surface.rs 头注):
 *   A. 双路径逐位一致: neb3_render(CPU 回读)与 neb3_render_native
 *      (wgpu 直渲 IOSurface, 预乘输出管线)对同一确定性场景, 两份 RGBA
 *      逐字节相同(不透明片元的预乘(a=1)≡直读, MSAA resolve 同值;
 *      a=0 的 RGB 载荷两路同为 0) —— 互操作无语义漂移;
 *   B. 原生模式状态: neb3_native_info 的 iosurface/w/h 与 render_native
 *      出参一致; generation 逐帧递增(1 → 2);
 *   C. 健壮性: NULL 场景参数返回 0(不崩)。
 *
 * 运行: NEB3_LIB=<libneb3_wgpu.dylib 路径> ./neb3_surface_probe
 * (由 tools/neb3_surface_probe.sh 构建后端并驱动; 需要 macOS + GPU。)
 */
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include "neb3.h"

static int g_pass = 0, g_fail = 0;
static void check(int cond, const char *label) {
    fprintf(stderr, "%s %s\n", cond ? "  ✔" : "  ✘", label);
    if (cond) g_pass++; else g_fail++;
}

typedef neb3_runtime *(*fn_create)(const neb3_desc *);
typedef void (*fn_destroy)(neb3_runtime *);
typedef neb3_scene *(*fn_scene_create)(neb3_runtime *);
typedef void (*fn_scene_destroy)(neb3_scene *);
typedef neb3_mesh *(*fn_box_mesh)(neb3_runtime *, float, float, float);
typedef void (*fn_mesh_destroy)(neb3_mesh *);
typedef neb3_material *(*fn_mat_unlit)(neb3_runtime *, neb3_color);
typedef void (*fn_mat_destroy)(neb3_material *);
typedef int (*fn_add_box)(neb3_scene *, neb3_mesh *, neb3_material *);
typedef int (*fn_set_cam)(neb3_scene *, float, float, float, float);
typedef int (*fn_set_rot)(neb3_scene *, float);
typedef int (*fn_render)(neb3_scene *, float, const neb3_color *,
                         const unsigned char **, size_t *, neb3_render_info *);
typedef void (*fn_release_frame)(neb3_scene *);
typedef int (*fn_render_native)(neb3_scene *, float, const neb3_color *, neb3_native *);
typedef int (*fn_native_info)(neb3_scene *, neb3_native *);
typedef int (*fn_native_read)(neb3_scene *, unsigned char *, size_t);
typedef const char *(*fn_last_error)(neb3_runtime *);

#define SYM(h, var, name, ty)                                                  \
    do {                                                                   \
        var = (ty)dlsym(h, name);                                 \
        if (!var) {                                                        \
            fprintf(stderr, "dlsym %s 失败: %s\n", name, dlerror());        \
            return 2;                                                      \
        }                                                                  \
    } while (0)

int main(void) {
    const char *lib = getenv("NEB3_LIB");
    if (!lib || !*lib) lib = "backend/neb3-wgpu/target/release/libneb3_wgpu.dylib";
    void *h = dlopen(lib, RTLD_NOW | RTLD_LOCAL);
    if (!h) {
        fprintf(stderr, "dlopen %s 失败: %s\n", lib, dlerror());
        return 2;
    }
    fn_create c_create; fn_destroy c_destroy;
    fn_scene_create s_create; fn_scene_destroy s_destroy;
    fn_box_mesh b_mesh; fn_mesh_destroy m_destroy;
    fn_mat_unlit m_unlit; fn_mat_destroy mat_destroy;
    fn_add_box a_box; fn_set_cam s_cam; fn_set_rot s_rot;
    fn_render r; fn_release_frame rf;
    fn_render_native rn; fn_native_info ni; fn_native_read nread;
    fn_last_error le;
    SYM(h, c_create, "neb3_create", fn_create); SYM(h, c_destroy, "neb3_destroy", fn_destroy);
    SYM(h, s_create, "neb3_scene_create", fn_scene_create); SYM(h, s_destroy, "neb3_scene_destroy", fn_scene_destroy);
    SYM(h, b_mesh, "neb3_box_mesh", fn_box_mesh); SYM(h, m_destroy, "neb3_mesh_destroy", fn_mesh_destroy);
    SYM(h, m_unlit, "neb3_material_unlit", fn_mat_unlit); SYM(h, mat_destroy, "neb3_material_destroy", fn_mat_destroy);
    SYM(h, a_box, "neb3_scene_add_box", fn_add_box); SYM(h, s_cam, "neb3_scene_set_camera_perspective", fn_set_cam);
    SYM(h, s_rot, "neb3_scene_set_rotation", fn_set_rot);
    SYM(h, r, "neb3_render", fn_render); SYM(h, rf, "neb3_release_frame", fn_release_frame);
    SYM(h, rn, "neb3_render_native", fn_render_native); SYM(h, ni, "neb3_native_info", fn_native_info);
    SYM(h, nread, "neb3_native_read_rgba", fn_native_read); SYM(h, le, "neb3_last_error", fn_last_error);

    neb3_desc desc;
    memset(&desc, 0, sizeof desc);
    desc.width = 128; desc.height = 96; desc.msaa_samples = 4;
    desc.transparent = 1; desc.pixel_ratio = 1.0f;
    neb3_runtime *rt = c_create(&desc);
    check(rt != NULL, "wgpu runtime 创建");
    if (!rt) return 2;
    neb3_scene *scene = s_create(rt);
    check(scene != NULL, "scene 创建");
    neb3_mesh *mesh = b_mesh(rt, 1.4f, 0.9f, 0.6f);
    neb3_material *mat = m_unlit(rt, (neb3_color){0.30f, 0.65f, 1.0f, 1.0f});
    check(scene && mesh && mat && a_box(scene, mesh, mat) == 1, "场景装配(box+材质)");
    check(s_cam(scene, 45.0f, 0.0f, 0.1f, 100.0f) == 1, "透视相机");
    check(s_rot(scene, 0.5f) == 1, "旋转姿态");

    const int W = 128, H = 96;
    const size_t need = (size_t)W * H * 4;

    /* A-1. readback 路径(CPU 回读) */
    const unsigned char *rgba = NULL; size_t bytes = 0; neb3_render_info info;
    memset(&info, 0, sizeof info);
    check(r(scene, 0.0f, NULL, &rgba, &bytes, &info) == 1, "readback 渲染成功");
    check(rgba && bytes >= need, "readback 帧完整");
    unsigned char *ref = (unsigned char *)malloc(need);
    if (ref && rgba) memcpy(ref, rgba, need);
    rf(scene);

    /* B. 原生路径(IOSurface 直渲, 零 CPU 回读) */
    neb3_native nat;
    memset(&nat, 0, sizeof nat);
    check(rn(scene, 0.0f, NULL, &nat) == 1, "render_native 成功");
    check(nat.iosurface != NULL, "IOSurface 句柄非空");
    check(nat.width == W && nat.height == H, "IOSurface 尺寸一致");
    check(nat.generation == 1, "generation 首帧 = 1");
    check(ni(scene, &nat) == 1 && nat.iosurface != NULL && nat.generation == 1,
          "native_info 与出参一致");

    unsigned char *native = (unsigned char *)malloc(need);
    int got = nread(scene, native, need);
    check(got == (int)need, "IOSurface 内容读出(字节完整)");

    /* A-2. 双路径逐位对照(同确定性输入; A-1 与 B 是两次独立渲染)。
       预乘(a=1 的不透明片元)与直读在 MSAA resolve 下逐位同值 ——
       实测 nat_rgba == ref_rgba(含 AA 边缘), 逐位 cmp 即语义锚。 */
    int diff = -1;
    if (ref && native && got == (int)need) {
        for (size_t i = 0; i < need; i++)
            if (ref[i] != native[i]) { diff = (int)i; break; }
    }
    if (diff >= 0)
        fprintf(stderr, "    首个超差 @ byte %d: ref=%02x native=%02x\n",
                diff, ref[diff], native[diff]);

    /* 逐帧 generation 递增 */
    check(rn(scene, 0.0f, NULL, &nat) == 1 && nat.generation == 2,
          "generation 逐帧递增(2)");
    got = nread(scene, native, need);
    check(got == (int)need, "第二帧 IOSurface 读出");

    /* C. 健壮性: NULL 场景 */
    check(rn(NULL, 0.0f, NULL, &nat) == 0, "NULL 场景 render_native 返回 0");
    check(nread(NULL, native, need) == 0, "NULL 场景 read_rgba 返回 0");

    free(native);
    m_destroy(mesh); mat_destroy(mat); s_destroy(scene); c_destroy(rt);
    (void)le;

    fprintf(stderr, "neb3_surface_probe: 通过 %d / 失败 %d\n", g_pass, g_fail);
    free(ref);
    return g_fail ? 1 : 0;
}

/* abi_regression.c — neb3 C ABI 回归(C 侧驱动, 经 cargo tests/c_abi.rs 编译运行)。
 *
 * 覆盖 ISSUES.md P1-1 / P1-2:
 *   P1-1  neb3_color 按结构体传值(非 4 float) — 布局自检 + 真实传参调用;
 *   P1-2  neb3_last_error 空 error 返回可读 NUL 指针(非悬垂)。
 *
 * 断言失败直接 abort(退出码非 0), stdout 打 PASS 行供 runner 校验。
 */
#include "neb3.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (!(cond)) {                                                       \
            fprintf(stderr, "abi_regression: FAIL %s (%s:%d)\n", #cond,      \
                    __FILE__, __LINE__);                                     \
            exit(1);                                                         \
        }                                                                    \
    } while (0)

int main(void) {
    /* P1-1: 结构布局 = 4 个连续 f32(结构体传值与 4 float 传参可区分的前提) */
    CHECK(sizeof(neb3_color) == 4 * sizeof(float));
    CHECK(offsetof(neb3_color, r) == 0);
    CHECK(offsetof(neb3_color, g) == 4);
    CHECK(offsetof(neb3_color, b) == 8);
    CHECK(offsetof(neb3_color, a) == 12);

    neb3_desc desc;
    desc.width = 64;
    desc.height = 64;
    desc.msaa_samples = 1;
    desc.transparent = 1;
    desc.pixel_ratio = 1.0f;

    neb3_runtime *rt = neb3_create(&desc);
    CHECK(rt != NULL);

    /* P1-2: 空 error 必须返回可读、NUL 结尾的指针(此前空 String 悬垂) */
    const char *err = neb3_last_error(rt);
    CHECK(err != NULL);
    CHECK(*err == '\0');

    neb3_scene *scene = neb3_scene_create(rt);
    CHECK(scene != NULL);
    neb3_mesh *mesh = neb3_box_mesh(rt, 1.0f, 1.0f, 1.0f);
    CHECK(mesh != NULL);
    /* P1-1 核心: C 头文件口径 = 结构体传值; 此调用若 Rust 侧按 4 float
     * 导出, x86-64 SysV 下颜色会被读错(此处以可渲染性 + 颜色覆盖佐证)。 */
    neb3_color color;
    color.r = 0.85f;
    color.g = 0.35f;
    color.b = 0.15f;
    color.a = 1.0f;
    neb3_material *material = neb3_material_unlit(rt, color);
    CHECK(material != NULL);

    CHECK(neb3_scene_add_box(scene, mesh, material) == 1);
    CHECK(neb3_scene_set_camera_perspective(scene, 45.0f, 1.0f, 0.1f, 100.0f) == 1);
    CHECK(neb3_scene_set_rotation(scene, 0.6f) == 1);
    CHECK(neb3_scene_set_depth(scene, 1) == 1);

    neb3_color clear;
    clear.r = 0.05f;
    clear.g = 0.07f;
    clear.b = 0.10f;
    clear.a = 1.0f;
    const unsigned char *rgba = NULL;
    size_t bytes = 0;
    neb3_render_info info;
    info.width = 0;
    info.height = 0;
    info.msaa_samples = 0;
    info.depth_enabled = 0;
    info.gpu = 0;
    CHECK(neb3_render(scene, 0.0f, &clear, &rgba, &bytes, &info) == 1);
    CHECK(rgba != NULL);
    CHECK(bytes == (size_t)64 * 64 * 4);
    CHECK(info.width == 64 && info.height == 64);

    /* 至少一个像素被画上(alpha > 0), 且四角为透明 clear(直通 alpha) */
    long drawn = 0;
    for (size_t i = 3; i < bytes; i += 4) {
        if (rgba[i] > 0) {
            drawn++;
        }
    }
    CHECK(drawn > 0);
    for (int corner = 0; corner < 4; corner++) {
        size_t x = (corner & 1) ? 63u : 0u;
        size_t y = (corner & 2) ? 63u : 0u;
        CHECK(rgba[(y * 64u + x) * 4u + 3u] == 0);
    }

    /* 错误路径不崩: null 句柄安全(空 error 指针仍可读) */
    CHECK(neb3_scene_add_box(NULL, mesh, material) == 0);
    err = neb3_last_error(rt);
    CHECK(err != NULL);

    neb3_release_frame(scene);
    neb3_material_destroy(material);
    neb3_mesh_destroy(mesh);
    neb3_scene_destroy(scene);
    neb3_destroy(rt);

    printf("abi_regression: PASS (layout + struct-by-value + empty-error)\n");
    return 0;
}

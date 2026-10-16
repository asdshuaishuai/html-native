/* neb3_fallback_probe.c — hn_three neb3 永久回退路径的缓存槽泄漏回归
 *
 * 背景(ISSUES.md 附: 存量简化): hn_three 的 neb3 backend 槽位缓存
 * (4 槽, rt/scene/mesh/mat 整链)在**永久回退**(state=2)后永不可达 ——
 * 若只释放触发槽, 其余 ≤3 槽整链悬留, 进程长驻(daemon)即泄漏。
 * 2026-10-15 复审修复: 回退时整链释放全部槽。本探针用 fake_neb3 替身
 * (对象级存活计数)把该修复钉成断言 —— 泄漏判据是"回退后 live==0",
 * 比 RSS/分配计数精确到对象。
 *
 * 场景(每进程一个; state=2 是进程级永久态, 场景间以进程隔离):
 *   render_fail  —— 4 槽全装配(4 个不同尺寸元素)→ 渲染失败触发回退
 *                   → 断言其余槽整链释放(live==0)+ 软件路径仍出图
 *                   + 永久性(后续 paint 不再 create)。
 *   create_fail  —— neb3_create 失败 → 回退 → live==0(槽恒空)。
 *   scene_fail   —— 场景装配失败 → 回退 → 本地半链也释放(live==0)。
 *
 * 用法: bash tools/neb3_fallback_probe.sh   (编 fake dylib + 探针, 三场景)
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn.h"
#include "hn_internal.h"

static int g_pass, g_fail;
static void check(int ok, const char *what) {
    printf("%s %s\n", ok ? "  ✔" : "  ✘", what);
    if (ok) g_pass++; else g_fail++;
}

/* 替身 dylib 的控制面(探针自行 dlopen 同一路径 —— 计数器同源) */
typedef void (*fn_set)(int);
typedef int (*fn_get)(void);

static struct {
    void *h;
    fn_set fail_render, fail_create, fail_scene, reset;
    fn_get live, created;
} ctl;

static void ctl_open(const char *path) {
    ctl.h = dlopen(path, RTLD_NOW);
    if (!ctl.h) { fprintf(stderr, "probe: dlopen %s: %s\n", path, dlerror()); exit(2); }
    struct { const char *n; void **p; } T[] = {
        { "fake_neb3_fail_render", (void **)&ctl.fail_render },
        { "fake_neb3_fail_create", (void **)&ctl.fail_create },
        { "fake_neb3_fail_scene",  (void **)&ctl.fail_scene },
        { "fake_neb3_reset",       (void **)&ctl.reset },
        { "fake_neb3_live",        (void **)&ctl.live },
        { "fake_neb3_created",     (void **)&ctl.created },
    };
    for (size_t i = 0; i < sizeof(T) / sizeof(T[0]); i++) {
        void *s = dlsym(ctl.h, T[i].n);
        if (!s) { fprintf(stderr, "probe: 缺 %s\n", T[i].n); exit(2); }
        memcpy(T[i].p, &s, sizeof(s));
    }
}

/* n 个不同尺寸的 hn-three 元素(几何全 box, 不带 orbit/tex/color → 走 backend) */
static hn_context *build_ctx(int n) {
    char html[2048];
    size_t used = (size_t)snprintf(html, sizeof(html),
        "<html><head><style>html,body{margin:0}</style></head><body>");
    for (int i = 0; i < n; i++)
        used += (size_t)snprintf(html + used, sizeof(html) - used,
            "<div id='e%d' hn-three style='width:%dpx;height:%dpx'></div>",
            i, 64 + i * 8, 64 + i * 8);
    snprintf(html + used, sizeof(html) - used, "</body></html>");
    hn_context *c = hn_context_create();
    hn_context_set_doc(c, hn_parse_html(html, strlen(html)));
    return c;
}

/* 布局+重绘一次; 返回显示列表里 BITMAP(neb3 帧)与 TRIS/POLYGON(软件路径)数 */
static void paint_count(hn_context *c, int *bitmaps, int *soft) {
    hn_context_layout(c, 480, 480, NULL);
    hn_context_repaint(c);
    const hn_display_list *dl = hn_context_display_list(c);
    *bitmaps = *soft = 0;
    for (int i = 0; dl && i < dl->count; i++) {
        int k = dl->cmds[i].kind;
        if (k == HN_CMD_BITMAP) (*bitmaps)++;
        if (k == HN_CMD_TRIS || k == HN_CMD_POLYGON) (*soft)++;
    }
}

int main(int argc, char **argv) {
    const char *scenario = argc > 1 ? argv[1] : "render_fail";
    const char *lib = getenv("FAKE_NEB3_PATH");
    if (!lib || !*lib) { fprintf(stderr, "probe: 缺 FAKE_NEB3_PATH\n"); return 2; }
    setenv("NEB3_LIB", lib, 1);   /* hn_three 首次 paint 时惰性 dlopen */
    ctl_open(lib);

    printf("neb3_fallback_probe — 场景 %s\n", scenario);

    if (!strcmp(scenario, "render_fail")) {
        /* THE ISSUE: 4 槽全装配 → 渲染失败触发永久回退 → 其余槽必须整链释放 */
        hn_context *c = build_ctx(4);
        int bm = 0, soft = 0;
        paint_count(c, &bm, &soft);
        check(bm == 4, "4 个元素各占一槽, neb3 帧全部入显示列表");
        check(ctl.live() == 4 * 4, "4 槽 × 4 对象(rt/scene/mesh/mat)在册");
        check(ctl.created() == 4, "create 恰 4 次");

        ctl.fail_render(1);                 /* 下一次渲染注定失败 → 永久回退 */
        paint_count(c, &bm, &soft);
        check(ctl.live() == 0, "回退后全部槽整链释放(live==0, 其余槽不悬留)");
        check(soft >= 4 && bm == 0, "回退后软件路径出图(TRIS/POLYGON)");
        int created_after = ctl.created();
        paint_count(c, &bm, &soft);
        check(ctl.created() == created_after, "永久性: 回退后不再 create(state=2 不重入)");
        check(ctl.live() == 0, "后续 paint 无对象驻留");
        hn_context_destroy(c);
    } else if (!strcmp(scenario, "create_fail")) {
        /* 装载成功但 create 失败: 槽应恒空, 无半装配对象 */
        ctl.fail_create(1);
        hn_context *c = build_ctx(1);
        int bm = 0, soft = 0;
        paint_count(c, &bm, &soft);
        check(ctl.live() == 0, "create 失败 → 回退 → live==0");
        check(soft >= 1, "create 失败走软件路径");
        hn_context_destroy(c);
    } else if (!strcmp(scenario, "scene_fail")) {
        /* 场景装配失败: 本地半链(rt 已建)必须释放, 不双 free */
        ctl.fail_scene(1);
        hn_context *c = build_ctx(1);
        int bm = 0, soft = 0;
        paint_count(c, &bm, &soft);
        check(ctl.live() == 0, "装配失败 → 回退 → 本地半链释放(live==0)");
        check(soft >= 1, "装配失败走软件路径");
        hn_context_destroy(c);
    } else {
        fprintf(stderr, "probe: 未知场景 %s\n", scenario);
        return 2;
    }

    printf("结果: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

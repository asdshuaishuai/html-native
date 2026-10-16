/* reshoot_docs.c — docs/screenshots 六张官网截图重拍驱动(ISSUES-1 待办 1)
 *
 * 背景: 6 张旧图摄于圆角 SDF 修复(fef4c63)之前, 几何口径已漂。本驱动用
 * 当前引擎按"真窗 backing"同口径重拍: hn_rt_set_scale(2) + hn_rt_shot ——
 * 真 2x FreeType 光栅(hidpi_probe ③ 同一机制), 非拉伸放大。
 *
 * 页面 → 出图对照(docs/index.html 效果画廊):
 *   dashboard.html  480x700  → dashboard.png
 *   webpage.html    520x600  → webpage.png
 *   showcase.html   920x620  → showcase.png
 *   message-card    330x360  → message-card.png
 *   ui-kit.html     920x900  → dev-app.png(include 组件 · 主题令牌)
 *   inputs.html     460x420  → inputs-focus.png(首个输入框聚焦态)
 *
 * 用法: bash tools/reshoot_docs.sh(仓库根运行; 覆盖 docs/screenshots/*.png)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "hn.h"
#include "hn_rt.h"

static char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    if (fread(buf, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(buf); return NULL; }
    fclose(f);
    buf[n] = 0;
    *len = (size_t)n;
    return buf;
}

static int shoot(const char *page, int w, int h, const char *out, int focus_first_input) {
    static char repo_root[1024];
    if (!repo_root[0] && !getcwd(repo_root, sizeof(repo_root))) return 1;
    /* 资产/<link>/<include> 均按页面目录解析(daemon 同口径: chdir 到页面目录) */
    char dir[1024], base[256];
    snprintf(dir, sizeof(dir), "%s/%s", repo_root, page);
    char *sl = strrchr(dir, '/');
    if (!sl) { fprintf(stderr, "  坏页面路径 %s\n", page); return 1; }
    *sl = 0;
    snprintf(base, sizeof(base), "%s", sl + 1);
    if (chdir(dir) != 0) { fprintf(stderr, "  chdir %s 失败\n", dir); return 1; }

    char page_abs[2048];
    snprintf(page_abs, sizeof(page_abs), "%s/%s", dir, base);
    hn_html_set_doc_path(page_abs);   /* <include>/<link> 按文档路径内联 */

    size_t len = 0;
    char *html = read_file(base, &len);
    if (!html) { chdir(repo_root); fprintf(stderr, "  读 %s 失败\n", base); return 1; }

    hn_rt_desc d = { "reshoot", html, len, NULL, 0, (float)w, (float)h };
    hn_rt *rt = hn_rt_open(&d);
    free(html);
    if (!rt) { chdir(repo_root); fprintf(stderr, "  hn_rt_open 失败: %s\n", page); return 1; }

    if (focus_first_input) {
        hn_node *in = hn_doc_find_by_id(hn_rt_doc(rt), "f-name");
        if (in) hn_context_set_focus(hn_rt_context(rt), in);
    }

    hn_rt_set_scale(rt, 2.0f);        /* @2x 真窗 backing 同口径 */
    int rc = hn_rt_shot(rt, out);     /* 契约: 1 = 成功(hn_rt_shot 口径) */
    hn_rt_close(rt);
    chdir(repo_root);                 /* 下一张的页面目录以此为基准 */
    printf("  %s (%dx%d @2x) → %s %s\n", page, w * 2, h * 2, out, rc ? "完成" : "失败");
    return rc ? 0 : 1;
}

int main(int argc, char **argv) {
    /* 出图根目录: 环境钮 DOCS_OUT(脚本注入仓库根 docs/screenshots 绝对路径;
     * 缺省 ./docs/screenshots —— chdir 后相对路径会漂, 必须绝对)。 */
    const char *docs_out = getenv("DOCS_OUT");
    if (!docs_out || !*docs_out) docs_out = "docs/screenshots";
    struct { const char *page; int w, h; char out[1024]; int focus; } S[6];
    const char *names[6] = { "dashboard.png", "webpage.png", "showcase.png",
                             "message-card.png", "dev-app.png", "inputs-focus.png" };
    const char *pages[6] = { "examples/dashboard.html", "examples/webpage.html",
                             "examples/showcase.html", "examples/message-card.html",
                             "examples/ui-kit.html", "examples/inputs.html" };
    int dims[6][2] = { {480,700}, {520,600}, {920,620}, {330,360}, {920,900}, {460,420} };
    for (int i = 0; i < 6; i++) {
        snprintf(S[i].out, sizeof(S[i].out), "%s/%s", docs_out, names[i]);
        S[i].page = pages[i];
        S[i].w = dims[i][0];
        S[i].h = dims[i][1];
        S[i].focus = (i == 5);
    }
    int fails = 0;
    for (int i = 0; i < 6; i++)
        if (shoot(S[i].page, S[i].w, S[i].h, S[i].out, S[i].focus)) fails++;
    return fails ? 1 : 0;
}

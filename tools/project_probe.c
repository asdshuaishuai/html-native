/* tools/project_probe.c — 多 html 项目模式断言
 *
 * 覆盖(阶段1 · 项目 = 目录):
 *   - hn_rt_desc.path 启用项目内导航(内联文档导航关闭)
 *   - hn_rt_navigate: 相对路径同窗换装 / 外部地址与不存在文件拒绝
 *   - 页面栈: navigate 后 back 回入口, 栈空后 back 返 0
 *   - 换装后新文档内容生效(hn_rt_text 断言)
 * 项目根 = 探针创建的临时目录(chdir 进去, 与 hn_app 壳同口径)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/stat.h>
#include "hn.h"
#include "hn_rt.h"

static int g_pass = 0, g_fail = 0;
static void check(int cond, const char *label) {
    printf("  %s %s\n", cond ? "v" : "X", label);
    if (cond) g_pass++; else g_fail++;
}

static void wf(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

static char g_tmp[512];

static void write_project(void) {
    snprintf(g_tmp, sizeof(g_tmp), "/tmp/hn-project-probe-XXXXXX");
    if (!mkdtemp(g_tmp)) { perror("mkdtemp"); exit(1); }
    /* 先 chdir 再写 —— 项目文件必须落在临时目录里(曾写进仓库根, back 断言
       因此失败: 临时项目里没有 index.html) */
    if (chdir(g_tmp) != 0) { perror("chdir"); exit(1); }
    mkdir("pages", 0755);
    wf("project.json",
       "{\n  \"name\": \"probe-project\",\n  \"entry\": \"index.html\"\n}\n");
    wf("index.html",
       "<html><head><meta charset=\"utf-8\"><title>probe-index</title></head>"
       "<body><p id=\"t\">INDEX-PAGE</p>"
       "<a href=\"pages/about.html\">go</a></body></html>");
    wf("pages/about.html",
       "<html><head><meta charset=\"utf-8\"><title>probe-about</title></head>"
       "<body><p id=\"t\">ABOUT-PAGE</p>"
       "<a href=\"../index.html\">back</a></body></html>");
}

int main(void) {
    printf("project_probe: 多 html 项目模式\n");
    write_project();

    /* ① 带源路径打开 → 导航开启 */
    const char *idx_html =
        "<html><head><meta charset=\"utf-8\"></head>"
        "<body><p id=\"t\">INDEX-PAGE</p>"
        "<a href=\"pages/about.html\">go</a></body></html>";
    hn_rt_desc d = { "proj-probe", idx_html, strlen(idx_html), NULL, 0, 300, 200,
                     "index.html" };
    hn_rt *rt = hn_rt_open(&d);
    check(rt != NULL, "项目项目: rt 打开(带源路径)");
    if (rt) {
        char *t = hn_rt_text(rt, "t");
        check(t && !strcmp(t, "INDEX-PAGE"), "入口页内容就位");
        free(t);

        /* ② 导航: 相对路径换装 */
        check(hn_rt_navigate(rt, "pages/about.html") == 1, "navigate 相对路径 = 换装");
        t = hn_rt_text(rt, "t");
        check(t && !strcmp(t, "ABOUT-PAGE"), "换装后新文档生效(ABOUT-PAGE)");
        free(t);

        /* ③ 拒绝: 外部地址 / 不存在文件(导航关闭语义) */
        check(hn_rt_navigate(rt, "https://example.com/x") == 0, "外部地址拒绝(交系统打开)");
        check(hn_rt_navigate(rt, "pages/missing.html") == 0, "不存在文件拒绝");

        /* ④ 页面栈回退 */
        check(hn_rt_back(rt) == 1, "back 回到上一页");
        t = hn_rt_text(rt, "t");
        check(t && !strcmp(t, "INDEX-PAGE"), "back 后内容 = 入口页");
        free(t);
        check(hn_rt_back(rt) == 0, "栈空后 back 返 0");

        /* ⑤ 跨子目录相对路径(pages/about.html → ../index.html 风格) */
        check(hn_rt_navigate(rt, "pages/about.html") == 1, "再次导航(栈可复用)");
        check(hn_rt_navigate(rt, "./index.html") == 1, "./ 前缀相对路径换装");
        t = hn_rt_text(rt, "t");
        check(t && !strcmp(t, "INDEX-PAGE"), "./ 换装后内容正确");
        free(t);
        hn_rt_close(rt);
    }

    /* ⑥ 内联文档(无源路径): 导航整体关闭 */
    const char *inline_html =
        "<html><head><meta charset=\"utf-8\"></head>"
        "<body><p id=\"t\">INLINE</p><a href=\"x.html\">x</a></body></html>";
    hn_rt_desc d2 = { "proj-probe-inline", inline_html, strlen(inline_html),
                      NULL, 0, 200, 120, NULL };
    hn_rt *rt2 = hn_rt_open(&d2);
    check(rt2 != NULL, "内联文档: rt 打开");
    if (rt2) {
        check(hn_rt_navigate(rt2, "x.html") == 0, "内联文档导航关闭(daemon 推流口径不变)");
        hn_rt_close(rt2);
    }

    printf("\nproject_probe: %d 项断言, 失败 %d\n", g_pass + g_fail, g_fail);
    return g_fail ? 1 : 0;
}

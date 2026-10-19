/* pack_probe.c — hn_pack 打包管线的端到端断言(本地文件输入, 零网络)
 *
 * 覆盖:
 *   1. 资产收集: img/script/link 引用 → assets/ 文件落盘
 *   2. 引用改写: index.html 指向 assets/ 新名; data:/锚点原样保留
 *   3. hn-* meta 注入(hn-window/hn-title)
 *   4. 兼容性体检: iframe 计数 + 被忽略 CSS(display:grid/float)计数
 *   5. 工程化项目目录: project.json + audit.txt 落盘
 *   6. .hnapp 胶囊: hn_zip_open 往返, manifest/index/assets 成员齐全
 *
 * 用法: bash tools/pack_probe.sh
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include "hn.h"
#include "hn_pack.h"
#include "hn_zip.h"

static int g_pass, g_fail;
static void check(int ok, const char *what) {
    printf("  %s %s\n", ok ? "v" : "X", what);
    if (ok) g_pass++; else g_fail++;
}

static void wf(const char *path, const char *content) {
    FILE *f = fopen(path, "wb");
    if (!f) { perror(path); exit(1); }
    fwrite(content, 1, strlen(content), f);
    fclose(f);
}

static int file_exists(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fclose(f);
    return 1;
}

int main(void) {
    printf("pack_probe: hn pack 打包管线\n");

    /* 夹具: 源页 + 3 资产 + 不支持特性(iframe/grid/float) */
    const char *html =
        "<html><head><title>Demo</title>"
        "<link rel=\"stylesheet\" href=\"style.css\">"
        "<style>.g{display:grid;float:left}</style></head><body>"
        "<img src=\"pic.png\">"
        "<iframe src=\"frame.html\"></iframe>"
        "<a href=\"#top\">锚点原样</a>"
        "<img src=\"data:image/png;base64,AAAA\">"
        "<script src=\"app.js\"></script></body></html>";
    const char *src_dir = "/tmp/hn-pack-probe-src";
    mkdir(src_dir, 0755);
    {
        char p[512];
        snprintf(p, sizeof(p), "%s/index.html", src_dir);   /* 目录输入的入口 = index.html */
        wf(p, html);
        snprintf(p, sizeof(p), "%s/style.css", src_dir);
        wf(p, "body{color:red}");
        snprintf(p, sizeof(p), "%s/pic.png", src_dir);
        wf(p, "FAKEPNG");
        snprintf(p, sizeof(p), "%s/app.js", src_dir);
        wf(p, "//js");
    }

    /* 打包(本地文件输入; 零网络) */
    hn_pack_opts o;
    memset(&o, 0, sizeof(o));
    o.input = src_dir;            /* 目录输入 → index.html */
    o.out_dir = "/tmp/hn-pack-probe-app";
    o.capsule = "/tmp/hn-pack-probe.hnapp";
    o.title = "打包探针";
    o.id = "pack-probe";
    o.w = 420; o.h = 640;
    hn_pack_result res;
    int rc = hn_pack_run(&o, &res);
    if (rc != 0) fprintf(stderr, "pack report: %s\n", res.report);
    check(rc == 0, "hn_pack_run 成功");
    check(res.project_dir && file_exists(res.project_dir), "项目目录产出");
    check(res.n_assets == 3, "3 个资产收集(css/png/js; iframe/data:/锚点不计)");
    check(res.n_rewritten == 3, "3 处引用改写");

    /* 改写后的 index.html: 引用指向 assets/ 新名; data:/锚点原样 */
    char path[512];
    snprintf(path, sizeof(path), "%s/index.html", res.project_dir);
    FILE *f = fopen(path, "rb");
    check(f != NULL, "改写后 index.html 存在");
    if (f) {
        static char buf[8192];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = 0;
        fclose(f);
        check(strstr(buf, "assets/") != NULL, "index.html 引用 assets/ 新名");
        check(strstr(buf, "href=\"#top\"") != NULL, "锚点引用原样保留");
        check(strstr(buf, "data:image/png") != NULL, "data: URI 原样保留");
        check(strstr(buf, "hn-window") != NULL &&
              strstr(buf, "content=\"420x640\"") != NULL,
              "hn-window meta 注入(--size)");
        check(strstr(buf, "hn-title") != NULL &&
              strstr(buf, "打包探针") != NULL,
              "hn-title meta 注入(--title)");
        check(strstr(buf, "iframe") != NULL, "源标签零改动(iframe 保留, 只报告)");
    }

    /* 资产落盘 */
    snprintf(path, sizeof(path), "%s/assets/a000-style.css", res.project_dir);
    check(file_exists(path), "style.css 收集");
    snprintf(path, sizeof(path), "%s/assets/a001-pic.png", res.project_dir);
    check(file_exists(path), "pic.png 收集");
    snprintf(path, sizeof(path), "%s/assets/a002-app.js", res.project_dir);
    check(file_exists(path), "app.js 收集");

    /* project.json + audit.txt */
    snprintf(path, sizeof(path), "%s/project.json", res.project_dir);
    f = fopen(path, "rb");
    check(f != NULL, "project.json 落盘(工程化项目口径)");
    if (f) {
        static char pj[512];
        size_t n = fread(pj, 1, sizeof(pj) - 1, f);
        pj[n] = 0;
        fclose(f);
        check(strstr(pj, "\"entry\": \"index.html\"") != NULL, "project.json 指向入口");
    }
    snprintf(path, sizeof(path), "%s/audit.txt", res.project_dir);
    check(file_exists(path), "audit.txt 落盘(体检报告)");

    /* 胶囊往返 */
    check(res.capsule_path != NULL, "胶囊路径返回");
    FILE *cf = fopen("/tmp/hn-pack-probe.hnapp", "rb");
    check(cf != NULL, ".hnapp 胶囊写出");
    if (cf) {
        fclose(cf);
        hn_zip z;
        FILE *zf = fopen("/tmp/hn-pack-probe.hnapp", "rb");
        fseek(zf, 0, SEEK_END);
        long zn = ftell(zf);
        fseek(zf, 0, SEEK_SET);
        unsigned char *zbuf = (unsigned char *)malloc((size_t)zn);
        if (fread(zbuf, 1, (size_t)zn, zf) != (size_t)zn) { check(0, "胶囊读回"); }
        fclose(zf);
        check(hn_zip_open(zbuf, (size_t)zn, &z) == 1, "胶囊 hn_zip_open");
        hn_zip_entry *e = NULL;
        check(hn_zip_find(&z, "manifest.json", &e) == 1, "胶囊含 manifest.json");
        if (e) {
            size_t ml = 0;
            unsigned char *mb = hn_zip_extract(&z, e, &ml);
            check(mb != NULL && strstr((char *)mb, "\"format\":\"hnapp\"") != NULL &&
                  strstr((char *)mb, "\"version\":2") != NULL,
                  "manifest format=hnapp version=2");
            free(mb);
        }
        check(hn_zip_find(&z, "index.html", &e) == 1, "胶囊含 index.html");
        check(hn_zip_find(&z, "assets/a000-style.css", &e) == 1, "胶囊含资产(assets/ 前缀)");
        hn_zip_free(&z);
        free(zbuf);
    }

    /* 体检计数 */
    check(res.n_unsupported_tags == 1, "体检: iframe ×1");
    check(res.n_ignored_css == 2, "体检: grid+float ×2");
    check(strstr(res.report, "iframe") != NULL, "报告含 iframe 明细");

    printf("\n结果: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

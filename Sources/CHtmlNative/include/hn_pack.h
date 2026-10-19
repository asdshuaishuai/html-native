/* hn_pack.h — `hn pack`: 既有 H5 网页/URL → 工程化项目目录 + .hnapp 胶囊
 *
 * 路线图"hn pack"项的实现(依赖胶囊 v2 的 hn_zip 与 rt 的 hn_http):
 *   输入  本地 .html 文件 / 目录(取 index.html) / http(s) URL(经运行时
 *         网络层 hn_http 拉取, 重定向手动跟至多 5 跳)
 *   体检  引擎子集口径的兼容性扫描: 不支持的元素(iframe/svg/object…
 *         计数)与被忽略的 CSS 特性(grid/float/sticky/backdrop-filter…
 *         计数), 逐项可机检 —— "符合子集"从口头承诺变成报告
 *   收集  <img|script|link|source> 引用的资产(相对/同源绝对)拉取或拷贝
 *         进 assets/, 引用改写为 assets/ 名(防撞加序号)
 *   改装  注入 hn-window/hn-title meta(原样式零改动)
 *   产出  工程化项目目录(project.json + index.html + assets/) 与
 *         .hnapp 胶囊 v2(hn_zip: manifest.json + index.html + assets/)
 * 边界如实声明: 不执行页面 JS、不渲染后收集(静态扫描)、跨域绝对 URL
 * v1 一律拉取(失败记报告不致命)。
 */
#ifndef HN_PACK_H
#define HN_PACK_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *input;        /* 文件/目录/URL; NULL = 失败 */
    const char *out_dir;      /* 工程化项目目录; NULL = "<id>-project" */
    const char *capsule;      /* .hnapp 输出路径; NULL = 不写胶囊 */
    const char *title;        /* hn-title; NULL = 沿用 <title> 或 id */
    const char *id;           /* 应用 id; NULL = 从文件名/主机名推导 */
    int         w, h;         /* hn-window 尺寸(缺省 480x700) */
    int         no_assets;    /* 1 = 不收集/不改写资产引用 */
} hn_pack_opts;

typedef struct {
    char *project_dir;        /* malloc(调用方 free); 失败 NULL */
    char *capsule_path;       /* malloc(写了胶囊才有); 否则 NULL */
    int   n_assets;           /* 收集的资产数 */
    int   n_rewritten;        /* 改写的引用数 */
    int   n_unsupported_tags; /* 体检: 不支持元素出现次数 */
    int   n_ignored_css;      /* 体检: 被忽略 CSS 特性出现次数 */
    char  report[2048];       /* 体检报告(多行; 供 CLI 打印/写 audit.txt) */
} hn_pack_result;

/* 执行打包。返回 0 = 成功(结果在 res); 非 0 = 失败(res.report 有原因)。 */
int hn_pack_run(const hn_pack_opts *opts, hn_pack_result *res);

#ifdef __cplusplus
}
#endif
#endif

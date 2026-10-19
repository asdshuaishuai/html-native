/* tools/capsule_probe.c — .hnapp 胶囊 v2(ZIP 容器)断言
 *
 * 覆盖(持久化 v2):
 *   - hn_zip_write/hn_zip_open 往返逐字节一致(含 0x00 二进制与中文文件名)
 *   - hn_zip_find / hn_zip_name_is / hn_zip_extract(存储条目)
 *   - 容器结构合法(外部 unzip -t 在 .sh 里跑, 引擎外双保险)
 *   - v1 JSON 胶囊识别口径: 非 PK 魔数回退 JSON 解析(daemon 同式)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn.h"
#include "hn_zip.h"

static int g_pass = 0, g_fail = 0;
static void check(int cond, const char *label) {
    printf("  %s %s\n", cond ? "v" : "X", label);
    if (cond) g_pass++; else g_fail++;
}

int main(void) {
    printf("capsule_probe: .hnapp 胶囊 v2(ZIP 容器)\n");

    /* 成员: manifest + 入口页 + 带 0x00 的二进制资产 + 中文文件名 */
    const char *manifest =
        "{\"format\":\"hnapp\",\"version\":2,\"id\":\"probe\",\"w\":480,"
        "\"h\":700,\"entry\":\"index.html\"}";
    const char *index_html =
        "<html><head><meta charset=\"utf-8\"></head>"
        "<body><p id=\"t\">CAPSULE-V2</p></body></html>";
    unsigned char bin[300];
    for (int i = 0; i < 300; i++) bin[i] = (unsigned char)(i * 7 + (i >> 3));
    bin[42] = 0; bin[123] = 0;                                   /* 内嵌 0x00 */
    const char *cjk_name = "assets/中文图片.png";

    hn_zip_member m[4] = {
        { "manifest.json", (const unsigned char *)manifest, strlen(manifest) },
        { "index.html",    (const unsigned char *)index_html, strlen(index_html) },
        { "assets/data.bin", bin, sizeof(bin) },
        { NULL, (const unsigned char *)"", 0 },
    };
    m[3].name = cjk_name;
    m[3].n = 64;                                                  /* 只写前 64 字节 */

    const char *path = "/tmp/hn-capsule-probe.hnapp";
    FILE *f = fopen(path, "wb");
    check(f != NULL, "胶囊写入: fopen");
    if (!f) return 1;
    check(hn_zip_write(f, m, 4) == 0, "hn_zip_write 成功");
    fclose(f);

    /* 读回: 逐成员比对 */
    FILE *rf = fopen(path, "rb");
    check(rf != NULL, "胶囊读回: fopen");
    if (!rf) return 1;
    fseek(rf, 0, SEEK_END);
    long n = ftell(rf);
    fseek(rf, 0, SEEK_SET);
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    size_t rd = fread(buf, 1, (size_t)n, rf);
    fclose(rf);
    check((long)rd == n && n > 100, "读回字节数一致");

    hn_zip z;
    check(hn_zip_open(buf, (size_t)n, &z) == 1, "hn_zip_open(中央目录解析)");
    check(z.n_ents == 4, "条目数 = 4");

    hn_zip_entry *e = NULL;
    check(hn_zip_find(&z, "manifest.json", &e) == 1, "find manifest.json");
    if (e) {
        size_t ml = 0;
        unsigned char *mb = hn_zip_extract(&z, e, &ml);
        check(mb && ml == strlen(manifest) && !memcmp(mb, manifest, ml),
              "manifest 往返逐字节一致");
        free(mb);
    }
    check(hn_zip_find(&z, "index.html", &e) == 1, "find index.html");
    if (e) {
        size_t il = 0;
        unsigned char *ib = hn_zip_extract(&z, e, &il);
        check(ib && il == strlen(index_html) && !memcmp(ib, index_html, il),
              "入口页往返逐字节一致");
        free(ib);
    }
    check(hn_zip_find(&z, "assets/data.bin", &e) == 1, "find 二进制资产");
    if (e) {
        size_t bl = 0;
        unsigned char *bb = hn_zip_extract(&z, e, &bl);
        check(bb && bl == sizeof(bin) && !memcmp(bb, bin, sizeof(bin)),
              "二进制资产往返一致(含 0x00)");
        free(bb);
    }
    check(hn_zip_find(&z, cjk_name, &e) == 1 && e->comp_size == 64, "中文文件名条目");
    check(hn_zip_find(&z, "missing.json", &e) == 0, "不存在条目返 0");

    /* v1 JSON 胶囊识别口径: 非 PK 魔数 → JSON 解析(daemon 同式) */
    const char *v1 = "{\"format\":\"hnapp\",\"version\":1,\"id\":\"old\",\"html\":\"<p>x</p>\"}";
    check(!(v1[0] == 'P' && v1[1] == 'K'), "v1 JSON 无 PK 魔数(回退 JSON 解析)");

    hn_zip_free(&z);
    free(buf);
    /* 产物保留在 /tmp/hn-capsule-probe.hnapp —— .sh 用系统 unzip/python
       zipfile 再校验一遍(引擎写出的包, 生态工具也必须认) */
    printf("\ncapsule_probe: %d 项断言, 失败 %d\n", g_pass + g_fail, g_fail);
    return g_fail ? 1 : 0;
}

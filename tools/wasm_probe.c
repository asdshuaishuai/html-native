/* wasm_probe.c — WASM 缝探针(docs/media-design.md §7 验证路径)
 *
 * 直用引擎的 hn_wasm API + vendor/wasm3, 不引 rt/QuickJS/平台 —— WASM 缝
 * 与媒体无关, 一个最小宿主即可钉死行为。语义断言全在本文件, 退出码 =
 * 失败项数(0 = 通过), 与 rt_probe.c/sysbridge_probe.c 同一口径。
 *
 * 用法(python3 tools/wasm_probe.py 是本文件的门禁薄封装):
 *   cc -std=c99 -O1 -Wall -I Sources/CHtmlNative/include -I vendor/wasm3 \
 *      tools/wasm_probe.c vendor/wasm3/m3_bind.c vendor/wasm3/m3_code.c \
 *      vendor/wasm3/m3_compile.c vendor/wasm3/m3_core.c vendor/wasm3/m3_env.c \
 *      vendor/wasm3/m3_exec.c vendor/wasm3/m3_function.c vendor/wasm3/m3_info.c \
 *      vendor/wasm3/m3_module.c vendor/wasm3/m3_parse.c vendor/wasm3/m3_validate.c \
 *      Sources/CHtmlNative/hn_wasm.c -o /tmp/hn_wasm_probe
 *   /tmp/hn_wasm_probe [examples/wasm/add.wasm]
 *
 * 检查项:
 *   主断言: load demo 模块 → add(2,3)==5(i32); addf(2,3)==5(f64 签名,
 *           引擎按导出签名转换 —— §7 "i32/f32/f64 参数与返回起步")。
 *   负路径: 坏字节/空指针 load → NULL; 未知导出/实参个数不符 → 0;
 *           NULL 句柄调用返回 0, free(NULL) 幂等。
 *   实例表: install → id≥1; by_id 回环; unload 后槽位清空; unload 幂等;
 *           非法 id(0/33/负)全 NULL。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hn.h"

static int fails = 0;
static void ck(int c, const char *l) { printf("%s %s\n", c ? "  v" : "  X", l); if (!c) fails++; }

int main(int argc, char **argv) {
    printf("wasm_probe\n");
    const char *path = argc > 1 ? argv[1] : "examples/wasm/add.wasm";

    /* ---- 装载 demo 模块 ---- */
    unsigned char *bytes = NULL;
    size_t n = 0;
    FILE *f = fopen(path, "rb");
    ck(f != NULL, "打开 demo 模块");
    if (f) {
        fseek(f, 0, SEEK_END);
        long sz = ftell(f);
        fseek(f, 0, SEEK_SET);
        if (sz > 0) bytes = (unsigned char *)malloc((size_t)sz);
        if (bytes) { n = fread(bytes, 1, (size_t)sz, f); }
        fclose(f);
    }
    ck(bytes && n > 8 && !memcmp(bytes, "\0asm", 4), "模块字节是合法 .wasm(\\0asm)");

    hn_wasm *w = hn_wasm_load(bytes, n);
    ck(w != NULL, "hn_wasm_load 解析+装载 demo 模块");
    ck(hn_wasm_load(bytes, 3) == NULL, "截断字节 load → NULL");
    ck(hn_wasm_load(bytes ? bytes : (const unsigned char *)"x", 0) == NULL, "空字节 load → NULL");
    ck(hn_wasm_load(NULL, 10) == NULL, "NULL 字节 load → NULL");

    /* ---- 主断言: 按名调用 ---- */
    if (w) {
        int32_t out = -1;
        ck(hn_wasm_call(w, "add", (const int32_t[]){2, 3}, 2, &out) == 1 && out == 5,
           "add(2,3) == 5");
        out = -1;
        ck(hn_wasm_call(w, "add", (const int32_t[]){40, 2}, 2, &out) == 1 && out == 42,
           "add(40,2) == 42");
        out = -1;
        ck(hn_wasm_call(w, "addf", (const int32_t[]){2, 3}, 2, &out) == 1 && out == 5,
           "addf(2,3) == 5(f64 签名按导出签名转换)");
        ck(hn_wasm_call(w, "nope", (const int32_t[]){1}, 1, &out) == 0, "未知导出 → 0");
        ck(hn_wasm_call(w, "add", (const int32_t[]){1}, 1, &out) == 0, "实参个数不符 → 0");
        ck(hn_wasm_call(w, "add", NULL, 0, &out) == 0, "签名要实参却没给 → 0");
        ck(hn_wasm_call(NULL, "add", NULL, 0, &out) == 0, "NULL 句柄调用 → 0");
    } else {
        ck(0, "add(2,3) == 5(装载失败, 跳组)");
        ck(0, "addf(2,3) == 5(装载失败, 跳组)");
        ck(0, "负路径组(装载失败, 跳组)");
    }
    hn_wasm_free(w);
    hn_wasm_free(NULL);          /* close(NULL) 幂等 */

    /* ---- 实例表(id→hn_wasm*, JS 桥的寻址路径) ---- */
    int id = hn_wasm_install(bytes, n);
    ck(id >= 1, "hn_wasm_install → id≥1");
    hn_wasm *m = hn_wasm_by_id(id);
    ck(m != NULL, "hn_wasm_by_id 回环非 NULL");
    if (m) {
        int32_t out = 0;
        ck(hn_wasm_call(m, "add", (const int32_t[]){2, 3}, 2, &out) == 1 && out == 5,
           "经 id 句柄 add(2,3) == 5");
    } else {
        ck(0, "经 id 句柄 add(2,3) == 5(槽位空, 跳组)");
    }
    hn_wasm_unload(id);
    ck(hn_wasm_by_id(id) == NULL, "unload 后槽位清空");
    hn_wasm_unload(id);          /* 幂等 */
    ck(hn_wasm_by_id(0) == NULL && hn_wasm_by_id(-1) == NULL && hn_wasm_by_id(33) == NULL,
       "非法 id(0/-1/33)→ NULL");
    ck(hn_wasm_install((const unsigned char *)"junk", 4) == -1, "坏字节 install → -1");

    free(bytes);
    printf("wasm_probe: %s(%d 失败)\n", fails ? "FAIL" : "PASS", fails);
    return fails ? 1 : 0;
}

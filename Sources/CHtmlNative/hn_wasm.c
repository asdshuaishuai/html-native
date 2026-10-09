/* hn_wasm.c — 引擎 WASM 缝(docs/media-design.md §7)
 *
 * wasm3(vendor/wasm3, MIT 快照 @28ecb9af)封装: 解析 .wasm 字节 → 实例,
 * 按名调用导出函数。纯 C99, 与宿主同进程同步调用。
 *
 * 所有权(wasm3.h 的契约, 逐条对上):
 *   - m3_ParseModule 的字节缓冲必须**比模块活得久** → load 内部复制一份,
 *     hn_wasm_free 时与 runtime/env 一起释放。
 *   - m3_LoadModule 把模块所有权移交给 runtime(成败都移交, 失败后不得
 *     再 m3_FreeModule)→ 装载失败只清 runtime/env。
 *   - m3_FreeRuntime 释放其上的全部模块; 顺序 free(runtime) → free(env)。
 *
 * 调用(m3_Call 指针数组族, 不是 m3_CallV 变参族): JS 桥传来的实参个数
 * 是运行期才知道的值, m3_CallV/VL 的 va_list 无法安全动态构造; m3_Call
 * (argc, argptrs[]) 是同一 API 家族里为动态个数准备的入口, i32 走
 * *(i32*)argptrs[i](m3_env.c:2508)。返回值固定单值, 走 m3_GetResultsV。
 *
 * 明确不做(§9): WASI、内存导出操作、表/间接调用、多返回值 —— v1 只收
 * 纯计算导出函数。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hn.h"
#include "wasm3.h"

/* 实例表容量(契约 §7): JS 的 id = 表下标+1, 满则 load 失败。 */
#define HN_WASM_MAX 32

struct hn_wasm {
    IM3Environment env;
    IM3Runtime     runtime;
    unsigned char *bytes;   /* .wasm 字节的内部副本(模块引用其中) */
    size_t         bytes_n;
};

/* ---------------- 装载 ---------------- */

hn_wasm *hn_wasm_load(const unsigned char *bytes, size_t n) {
    if (!bytes || n < 4 || memcmp(bytes, "\0asm", 4) != 0) {
        fprintf(stderr, "hn_wasm: load 失败(缺 \\0asm 魔数或空字节)\n");
        return NULL;
    }
    if (n > 0x7fffffffu) {   /* m3_ParseModule 收 uint32 */
        fprintf(stderr, "hn_wasm: load 失败(模块过大)\n");
        return NULL;
    }

    hn_wasm *w = (hn_wasm *)calloc(1, sizeof(hn_wasm));
    if (!w) return NULL;
    w->bytes = (unsigned char *)malloc(n);
    if (!w->bytes) { free(w); return NULL; }
    memcpy(w->bytes, bytes, n);
    w->bytes_n = n;

    w->env = m3_NewEnvironment();
    if (!w->env) goto fail;
    /* 栈 64KB: wasm3 自家示例的缺省口径, 纯计算导出函数绰绰有余。 */
    w->runtime = m3_NewRuntime(w->env, 64 * 1024, NULL);
    if (!w->runtime) goto fail;

    IM3Module module = NULL;
    M3Result r = m3_ParseModule(w->env, &module, w->bytes, (uint32_t)n);
    if (r) {
        fprintf(stderr, "hn_wasm: parse 失败: %s\n", r);
        goto fail;
    }
    /* 成败都移交所有权(wasm3.h:385-386), 失败不得 m3_FreeModule。 */
    r = m3_LoadModule(w->runtime, module);
    if (r) {
        fprintf(stderr, "hn_wasm: instantiate 失败: %s\n", r);
        goto fail;
    }
    return w;

fail:
    if (w->runtime) m3_FreeRuntime(w->runtime);
    if (w->env) m3_FreeEnvironment(w->env);
    free(w->bytes);
    free(w);
    return NULL;
}

void hn_wasm_free(hn_wasm *w) {
    if (!w) return;                    /* close(NULL) 幂等 */
    m3_FreeRuntime(w->runtime);        /* 连带释放模块 */
    m3_FreeEnvironment(w->env);
    free(w->bytes);
    free(w);
}

/* ---------------- 调用 ---------------- */

/* 实参个数上限: 纯计算导出函数不会碰到; 超过直接拒(防野调用)。 */
#define HN_WASM_MAX_ARGS 64

int hn_wasm_call(hn_wasm *w, const char *fn,
                 const int32_t *args, int n_args, int32_t *out) {
    if (!w || !fn || !out) return 0;
    if (n_args < 0 || n_args > HN_WASM_MAX_ARGS) return 0;
    if (n_args > 0 && !args) return 0;

    IM3Function f = NULL;
    M3Result r = m3_FindFunction(&f, w->runtime, fn);
    if (r || !f) {
        fprintf(stderr, "hn_wasm: 找不到导出 \"%s\": %s\n", fn, r ? r : "?");
        return 0;
    }

    uint32_t na = m3_GetArgCount(f);
    uint32_t nr = m3_GetRetCount(f);
    if (na != (uint32_t)n_args) {
        fprintf(stderr, "hn_wasm: \"%s\" 形参 %u 个, 实参 %d 个\n", fn, na, n_args);
        return 0;
    }
    if (nr > 1) {
        fprintf(stderr, "hn_wasm: \"%s\" 多返回值(v1 不支持)\n", fn);
        return 0;
    }

    /* 就地类型转换: 调用方一律给 int32(桥按 JS Number→i32), 模块签名
       声明什么类型就转什么。union 数组保证 f64/i64 的对齐与尺寸。 */
    union { int32_t i32; int64_t i64; float f32; double f64; } vals[HN_WASM_MAX_ARGS];
    const void *ptrs[HN_WASM_MAX_ARGS];
    for (uint32_t i = 0; i < na; i++) {
        int32_t v = args[i];
        switch (m3_GetArgType(f, i)) {
        case c_m3Type_i32: vals[i].i32 = v; break;
        case c_m3Type_i64: vals[i].i64 = (int64_t)v; break;
        case c_m3Type_f32: vals[i].f32 = (float)v; break;
        case c_m3Type_f64: vals[i].f64 = (double)v; break;
        default:
            fprintf(stderr, "hn_wasm: \"%s\" 第%u参类型(v128/reftype)不支持\n", fn, i);
            return 0;
        }
        ptrs[i] = &vals[i];
    }

    r = m3_Call(f, na, ptrs);
    if (r) {
        fprintf(stderr, "hn_wasm: \"%s\" 调用失败: %s\n", fn, r);
        return 0;   /* 陷阱/缺编译码; m3_Call 失败后结果不可读 */
    }

    /* 单返回值按签名类型读出再折回 int32; 无返回值写 0。 */
    *out = 0;
    if (nr == 1) {
        switch (m3_GetRetType(f, 0)) {
        case c_m3Type_i32: { int32_t v = 0; if (m3_GetResultsV(f, &v)) return 0; *out = v; break; }
        case c_m3Type_i64: { int64_t v = 0; if (m3_GetResultsV(f, &v)) return 0; *out = (int32_t)v; break; }
        case c_m3Type_f32: { float   v = 0; if (m3_GetResultsV(f, &v)) return 0; *out = (int32_t)v; break; }
        case c_m3Type_f64: { double  v = 0; if (m3_GetResultsV(f, &v)) return 0; *out = (int32_t)v; break; }
        default: return 0;
        }
    }
    return 1;
}

/* ---------------- 实例表(id→hn_wasm*) ---------------- */

static hn_wasm *s_mods[HN_WASM_MAX];

int hn_wasm_install(const unsigned char *bytes, size_t n) {
    for (int i = 0; i < HN_WASM_MAX; i++) {
        if (s_mods[i]) continue;
        s_mods[i] = hn_wasm_load(bytes, n);
        return s_mods[i] ? i + 1 : -1;
    }
    fprintf(stderr, "hn_wasm: 实例表已满(%d)\n", HN_WASM_MAX);
    return -1;
}

hn_wasm *hn_wasm_by_id(int id) {
    if (id < 1 || id > HN_WASM_MAX) return NULL;
    return s_mods[id - 1];
}

void hn_wasm_unload(int id) {
    if (id < 1 || id > HN_WASM_MAX) return;
    hn_wasm_free(s_mods[id - 1]);
    s_mods[id - 1] = NULL;
}

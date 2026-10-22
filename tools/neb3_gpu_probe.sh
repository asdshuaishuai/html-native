#!/usr/bin/env bash
# tools/neb3_gpu_probe.sh — neb3 原生纹理直入合成器 端到端探针
# (GPU Metal 合成消费 IOSurface vs hnsoft 消费读回字节, 现场编译 harness,
#  手法同 gpu_probe.sh; 零 framework 链接参数 —— 纯 dlopen。)
#
# 断言面:
#   路由时序: hn_gpu_native_surface_ok 读设置时刻的全局 host 态 ——
#   先建软件上下文(host 未设 → hn_three 走读回字节路径), 再设 host 建
#   GPU 上下文(原生路由激活)。
#   A. 原生路由确实触发: GPU 上下文的显示列表含 native BITMAP
#      (native_surface != NULL; 若 neb3_render_native 失败静默回落读回,
#      此断言即红 —— 防止"假绿");
#   B. hn_gpu_available(GPU dl) == 1(原生 BITMAP 可被 Metal 消费);
#   C. GPU 帧内容: 盒区有内容(非背景)、框外恒背景角字节;
#   D. hnsoft 帧内容: 盒区同样有内容(读回字节路径)。
# (GPU 采样与软件混合的逐像素差异为结构性存在 —— 信息值打印, 不做硬线;
#  逐位级的内容等价已由 neb3_surface_probe 在 IOSurface 读出口钉死。)
#
# 用法: bash tools/neb3_gpu_probe.sh   (macOS; 需 cargo 构建 neb3-wgpu)

set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/_engine_sources.sh"

LABEL="neb3_gpu_probe.sh"

if [ "$(uname -s)" != "Darwin" ]; then
    echo "$LABEL: skip(非 macOS, IOSurface 互操作仅 Apple)"; exit 0
fi
command -v cargo >/dev/null 2>&1 || { echo "$LABEL: skip(缺 cargo)"; exit 0; }

echo "$LABEL: cargo build --release(neb3-wgpu 后端)"
cargo build --release --manifest-path backend/neb3-wgpu/Cargo.toml

DYLIB="$(pwd)/backend/neb3-wgpu/target/release/libneb3_wgpu.dylib"
[ -s "$DYLIB" ] || { echo "$LABEL: FAIL 后端 dylib 缺失"; exit 1; }

D="$(hn_tmpd probe)"
SRC_C="$D/neb3_gpu_probe.c"
BIN="$D/neb3_gpu_probe"

# ---- 内嵌 C99 harness(现场编译, 手法同 gpu_probe.sh) ----
cat > "$SRC_C" <<'CEOF'
/* neb3_gpu_probe.c — 原生纹理直入合成器端到端断言(现场编译) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "hn.h"
#include "hnsoft.h"
#include "hn_platform.h"
#include "hn_internal.h"

static const char *BANNER = "neb3_gpu_probe";
static int g_fail = 0;
static void ck(int ok, const char *name) {
    printf("  [%s] %s\n", ok ? "ok" : "FAIL", name);
    if (!ok) g_fail++;
}

static hn_context *mk_ctx(void) {
    /* 最小 neb3 页: 绝对定位的 hn-three 元素, 无文本/图片 */
    const char *html =
        "<html><head><style>html,body{margin:0;background:#0B0E13}"
        "#v{position:absolute;left:60;top:40;width:120;height:80}"
        "</style></head><body>"
        "<div id='v' hn-three></div></body></html>";
    hn_doc *doc = hn_parse_html(html, strlen(html));
    if (!doc) return NULL;
    hn_context *c = hn_context_create();
    hn_context_set_doc(c, doc);
    return c;
}

static const hn_display_list *layout(hn_context *c, int w, int h) {
    hn_context_layout(c, (float)w, (float)h, NULL);
    hn_context_repaint(c);
    return hn_context_display_list(c);
}

/* 位图区内容量: 非 BG 背景色的采样数(4,4 步进) */
static int content_samples(const unsigned char *rgba, int w, int h) {
    unsigned br = 0x0B, bg = 0x0E, bb = 0x13;
    int n = 0;
    for (int y = 0; y < h; y += 4)
        for (int x = 0; x < w; x += 4) {
            const unsigned char *p = rgba + ((size_t)y * w + x) * 4;
            if (abs(p[0] - br) > 8 || abs(p[1] - bg) > 8 || abs(p[2] - bb) > 8)
                n++;
        }
    return n;
}

#define BW 240
#define BH 160

int main(void) {
    printf("%s: 原生纹理直入合成器端到端\n", BANNER);

    /* ---- D. hnsoft 先行(host 未设 → hn_three 走读回字节路径) ---- */
    hn_color bg = 0x0B0E13FFu;
    hn_context *cb = mk_ctx();
    ck(cb != NULL, "软件上下文创建");
    const hn_display_list *dlb = layout(cb, BW, BH);
    ck(dlb != NULL && dlb->count > 0, "软件上下文显示列表非空");
    int byte_bm_b = 0;
    for (int i = 0; i < dlb->count; i++)
        if (dlb->cmds[i].kind == HN_CMD_BITMAP && dlb->cmds[i].bitmap) byte_bm_b++;
    ck(byte_bm_b >= 1, "软件上下文走读回字节路径(BITMAP 带字节)");
    unsigned char *sw = hnsoft_render_scaled(dlb, BW, BH, bg, 1.0f);
    ck(sw != NULL, "hnsoft 渲染成功");
    int sw_in = sw ? content_samples(sw + ((size_t)40 * BW + 60) * 4, 120, 80) : 0;
    ck(sw_in > 20, "盒区有 neb3 内容(hnsoft 读回路径)");

    /* ---- A-C. GPU 上下文(此刻设 host → 原生路由激活) ---- */
    hn_gpu_set_host(hnp_gpu_backend());

    hn_context *ca = mk_ctx();
    ck(ca != NULL, "GPU 上下文创建");
    const hn_display_list *dla = layout(ca, BW, BH);
    ck(dla != NULL && dla->count > 0, "GPU 上下文显示列表非空");

    /* A. 原生路由触发: dl 含 native BITMAP(防"失败静默回落"假绿) */
    int native_bm = 0, byte_bm = 0;
    for (int i = 0; i < dla->count; i++) {
        if (dla->cmds[i].kind != HN_CMD_BITMAP) continue;
        if (dla->cmds[i].native_surface) native_bm++;
        else if (dla->cmds[i].bitmap) byte_bm++;
    }
    ck(native_bm >= 1, "原生路由触发(native BITMAP ≥1)");
    ck(byte_bm == 0, "无读回字节 BITMAP 混入(全 native)");

    /* B. 可被 Metal 消费 */
    ck(hn_gpu_available(dla) == 1, "hn_gpu_available(原生 BITMAP 可消费)");

    /* GPU 渲染(含原生纹理消费) */
    unsigned char *bgra = NULL;
    int rendered = hn_gpu_render_scaled(dla, BW, BH, bg, 1.0f, &bgra);
    ck(rendered == 1 && bgra != NULL, "GPU 渲染(含原生纹理)成功");
    if (!rendered || !bgra) return 1;

    /* 预乘 BGRA → 非预乘 RGBA(与 hn_rt_shot 同换算) */
    unsigned char *ga = (unsigned char *)malloc((size_t)BW * BH * 4);
    for (int i = 0; i < BW * BH; i++) {
        unsigned a = bgra[i * 4 + 3];
        ga[i * 4 + 0] = a ? (unsigned char)((bgra[i * 4 + 2] * 255 + a / 2) / a) : 0;
        ga[i * 4 + 1] = a ? (unsigned char)((bgra[i * 4 + 1] * 255 + a / 2) / a) : 0;
        ga[i * 4 + 2] = a ? (unsigned char)((bgra[i * 4 + 0] * 255 + a / 2) / a) : 0;
        ga[i * 4 + 3] = (unsigned char)a;
    }

    /* C. 内容断言: 盒区(60,40,120,80)有内容; 角/框外恒背景 */
    int in_content = content_samples(ga + ((size_t)40 * BW + 60) * 4, 120, 80);
    ck(in_content > 20, "盒区有 neb3 内容(GPU 原生纹理已画)");
    unsigned char *corner = ga;   /* (0,0) */
    ck(corner[0] == 0x0B && corner[1] == 0x0E && corner[2] == 0x13,
       "框外角像素恒背景(GPU)");
    unsigned char *outside = ga + ((size_t)20 * BW + 20) * 4;
    ck(outside[0] == 0x0B && outside[1] == 0x0E && outside[2] == 0x13,
       "框外采样点恒背景(GPU)");

    /* 信息值: 两路盒区内容量(GPU 采样 vs 软件逐像素, 结构性差异不判) */
    printf("  [info] 盒区内容采样 GPU=%d hnsoft=%d\n", in_content, sw_in);

    free(ga);
    free(sw);
    hn_context_destroy(ca);
    hn_context_destroy(cb);
    free(bgra);

    printf("%s: 通过 %d / 失败 %d\n", BANNER, g_fail == 0 ? 1 : 0, g_fail);
    return g_fail ? 1 : 0;
}
CEOF

hn_sources_soft   # 引擎核心 + hnsoft(HN_SOURCES)
HN_SOURCES=("$SRC_C" "$HN_REPO_ROOT/platform/hnp_gpu_metal.c"
            ${HN_SOURCES[@]+"${HN_SOURCES[@]}"})
hn_sources_check ${HN_SOURCES[@]+"${HN_SOURCES[@]}"} || exit 1

FT_INC_DIR="$(hn_freetype_inc || true)"
HN_INCS=(-I "$HN_ENG/include" -I "$HN_ENG" -I "$HN_REPO_ROOT/platform" -I "$FT_INC_DIR")
HN_LIBS=(-L/opt/homebrew/lib -lfreetype -lm)
# 注意: **没有** -framework Metal —— 纯 dlopen(hnp_gpu_metal.c 手法),
# IOSurface 互操作同为运行时解析(生产者侧 objc2, 消费侧 msg_send)。
hn_probe_compile "$LABEL" "$BIN" || exit 1

rc=0
NEB3_LIB="$DYLIB" "$BIN" || rc=$?
if [ "$rc" -ne 0 ]; then
    echo "$LABEL: FAIL 退出码 $rc"
    exit 1
fi
echo "$LABEL: OK"

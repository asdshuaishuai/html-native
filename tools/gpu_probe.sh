#!/usr/bin/env bash
# tools/gpu_probe.sh — GPU 渲染后端 v1(Metal)探针薄封装。
#
# 语义断言全部在下方内嵌的 C99 harness 里(与 _engine_sources.sh 的
# "现场编译 C 助手、仓库不新增 C 文件"同一手法), 本脚本负责编译与运行。
#
# 链接口径: 探针 + 引擎核心 + hnsoft + hn_gpu.c(引擎侧翻译器) +
# platform/hnp_gpu_metal.c(Metal 原语表)。**零 framework 链接参数** ——
# 与 hnp_macos.c 的 AppKit 同一手法, Metal.framework 运行期 dlopen
# (MTLCreateSystemDefaultDevice 是 C 符号, dlsym 直取), 链接期只链
# FreeType(文本字形)与 libm。
#
# 断言清单(契约: platform/hn_platform.h 尾部"GPU 渲染后端"):
#   ① 渲染非空非纯底色(三固定页: 纯色矩形 / 渐变+图片 / 含文本);
#   ② GPU 帧与 hnsoft 同页**逐像素**色差 ≤ 16/通道(契约容差 ±40, 收紧;
#      整数坐标盒 + 渐变 LUT + FreeType 字形 1:1 的换算链见 hn_gpu.c 头注);
#   ③ readback 口径: 空白处像素 = 预乘底色字节(BGRA = b,g,r,a);
#   ④ POLYGON 指令 → hn_gpu_available()==0(整帧回退判定), 且
#      hn_gpu_render_scaled 拒绝(呼出方回退 hnsoft → 无伪影, 逐位一致);
#   ⑤ QUAD(任意四顶点)可走 GPU 且与 hnsoft 色差同容差;
#   ⑥ CPU 对照(信息值不判定): 同一页面 metal vs hnsoft 的 rusage 每帧耗时。
#
# 用法:
#   bash tools/gpu_probe.sh                 跑全部断言(门禁口径)
#   bash tools/gpu_probe.sh --window [page] HN_GPU=metal 起真窗
#                                           (dashboard|pet, 手动观感验收)
# 退出码: 0 = 全过(或无 Metal 设备时 SKIP); 非 0 = 有断言失败。
# 真窗手动口径: HN_GPU=metal dist/hnapp-macos-arm64 examples/dashboard.html
#   (hn_app 装配链路; --shot 的 HN_GPU 口径见 hn_rt.c hn_rt_shot 注释)。

set -euo pipefail
cd "$(dirname "$0")/.."
. "$(dirname "$0")/_engine_sources.sh"

LABEL="gpu_probe.sh"

# ---- 真窗手动模式: HN_GPU=metal 起示例页(观感差异肉眼验收) ----
if [ "${1:-}" = "--window" ]; then
    page="${2:-dashboard}"
    hnapp="$HN_REPO_ROOT/dist/hnapp-macos-arm64"
    if [ ! -x "$hnapp" ]; then
        echo "$LABEL: 先构建 dist/hnapp-macos-arm64(bash tools/build-multiplatform.sh)"
        exit 1
    fi
    echo "$LABEL: HN_GPU=metal 真窗: examples/$page.html(观感对照: 不设 HN_GPU 再开一次)"
    exec env HN_GPU=metal "$hnapp" "$HN_REPO_ROOT/examples/$page.html"
fi

HDIR="$(hn_helper_dir)"
SRC_C="$HDIR/hn_gpu_probe.c"

# ---- 内嵌 C99 harness ----
cat > "$SRC_C" <<'CEOF'
/* hn_gpu_probe.c — GPU 后端断言(gpu_probe.sh 现场编译, 非仓库源文件) */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sys/resource.h>
#include "hn.h"
#include "hnsoft.h"
#include "hn_platform.h"
#include "hn_internal.h"   /* hn_gpu_set_host / hn_gpu_available / hn_gpu_render_scaled */
#include "hn_png.h"

static const char *BANNER = "gpu_probe";
static int g_fail = 0;

static void ck(int ok, const char *name, const char *detail) {
    printf("  [%s] %s%s%s\n", ok ? "ok" : "FAIL", name,
           detail && detail[0] ? " — " : "", detail ? detail : "");
    if (!ok) g_fail++;
}

/* ---- 布局/渲染环境 ---- */

static hn_context *g_ctx;

static int img_size(void *ctx, const char *path, float *w, float *h) {
    (void)ctx;
    if (!path || !*path) return 0;
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return 0; }
    unsigned char *buf = (unsigned char *)malloc((size_t)n);
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    if (got != (size_t)n) { free(buf); return 0; }
    int iw = 0, ih = 0;
    unsigned char *px = hn_png_decode(buf, (size_t)n, &iw, &ih);
    free(buf);
    if (!px) return 0;
    free(px);
    *w = (float)iw;
    *h = (float)ih;
    return 1;
}
static hn_image_backend g_img_be = { NULL, img_size };

static float tb_measure(void *ctx, const hn_font_desc *font, const char *utf8, size_t len) {
    (void)ctx;
    return hnsoft_measure(font, utf8, len);
}
static void tb_metrics(void *ctx, const hn_font_desc *font,
                       float *ascent, float *descent, float *leading) {
    (void)ctx;
    hnsoft_metrics(font, ascent, descent, leading);
}
static hn_text_backend g_tb = { NULL, tb_measure, tb_metrics };

/* 布局一次, 返回显示列表(两路渲染共用同一份 → 几何严格同源) */
static const hn_display_list *layout(const char *html, int w, int h) {
    hn_doc *doc = hn_parse_html(html, strlen(html));
    if (!doc) return NULL;
    if (g_ctx) hn_context_destroy(g_ctx);
    g_ctx = hn_context_create();
    hn_context_set_doc(g_ctx, doc);
    hn_context_set_images(g_ctx, &g_img_be);
    hn_context_layout(g_ctx, (float)w, (float)h, &g_tb);
    hn_context_repaint(g_ctx);
    return hn_context_display_list(g_ctx);
}

#define BW 240
#define BH 160
static const hn_color BG = 0x0B0E13FFu;

/* GPU 帧读回: 预乘 BGRA → 非预乘 RGBA(与 hn_rt_shot 同一换算, hn_gpu.c 头注 1) */
static unsigned char *gpu_rgba(unsigned char *bgra, int w, int h) {
    unsigned char *rgba = (unsigned char *)malloc((size_t)w * h * 4);
    if (!rgba) { free(bgra); return NULL; }
    for (int i = 0; i < w * h; i++) {
        unsigned a = bgra[i * 4 + 3];
        rgba[i * 4 + 0] = a ? (unsigned char)((bgra[i * 4 + 2] * 255 + a / 2) / a) : 0;
        rgba[i * 4 + 1] = a ? (unsigned char)((bgra[i * 4 + 1] * 255 + a / 2) / a) : 0;
        rgba[i * 4 + 2] = a ? (unsigned char)((bgra[i * 4 + 0] * 255 + a / 2) / a) : 0;
        rgba[i * 4 + 3] = (unsigned char)a;
    }
    return rgba;
}

/* 页面级断言: 非空 + 全幅色差 + 底色字节口径
   expect_nonempty=0 时空白页: 断言反向(全幅即底色, 两路逐位级一致) */
static void check_page_e(const char *name, const hn_display_list *dl,
                         const char *png_dir, const char *stem, int expect_nonempty) {
    char msg[256];
    if (!hn_gpu_available(dl)) {
        snprintf(msg, sizeof(msg), "%s 有指令被判不支持(见上一条 FAIL)", name);
        ck(0, name, msg);
        return;
    }
    unsigned char *bgra = NULL;
    if (!hn_gpu_render_scaled(dl, BW, BH, BG, 1.0f, &bgra) || !bgra) {
        ck(0, name, "hn_gpu_render_scaled 失败");
        return;
    }
    unsigned char *gpu = gpu_rgba(bgra, BW, BH);
    /* readback 口径③: 空白角像素 = 预乘底色字节(底色不透明 → 预乘=原色) */
    int corner_ok = bgra[0] == ((BG >> 8) & 0xFF) && bgra[1] == ((BG >> 16) & 0xFF)
                 && bgra[2] == ((BG >> 24) & 0xFF) && bgra[3] == (BG & 0xFF);
    free(bgra);
    if (!gpu) { ck(0, name, "unpremul 失败"); return; }
    unsigned char *cpu = hnsoft_render_scaled(dl, BW, BH, BG, 1.0f);
    if (!cpu) { ck(0, name, "hnsoft 渲染失败"); free(gpu); return; }

    int nonempty = 0, maxdiff = 0;
    unsigned br = (BG >> 24) & 0xFF, bg2 = (BG >> 16) & 0xFF, bb = (BG >> 8) & 0xFF;
    for (int i = 0; i < BW * BH; i++) {
        const unsigned char *p = cpu + (size_t)i * 4;
        if (abs(p[0] - (int)br) > 8 || abs(p[1] - (int)bg2) > 8 || abs(p[2] - (int)bb) > 8)
            nonempty++;
        for (int ch = 0; ch < 4; ch++) {
            int d = abs((int)cpu[i * 4 + ch] - (int)gpu[i * 4 + ch]);
            if (d > maxdiff) maxdiff = d;
        }
    }
    snprintf(msg, sizeof(msg), "非空像素=%d 最大色差=%d/255 底色角=%s",
             nonempty, maxdiff, corner_ok ? "ok" : "FAIL");
    int pass = maxdiff <= 16 && corner_ok
            && (expect_nonempty ? nonempty >= 100 : nonempty == 0);
    ck(pass, name, msg);

    if (png_dir) {   /* 落盘两份 PNG 供肉眼对照 */
        char path[512];
        size_t pn = 0;
        snprintf(path, sizeof(path), "%s/%s-cpu.png", png_dir, stem);
        unsigned char *png = hnsoft_encode_png(cpu, BW, BH, &pn);
        if (png) { FILE *fp = fopen(path, "wb"); if (fp) { fwrite(png, 1, pn, fp); fclose(fp); } free(png); }
        /* PNG 期望非预乘 RGBA → 把 BGRA 预乘转 RGBA 再编码(与上面 gpu 同一换算) */
        snprintf(path, sizeof(path), "%s/%s-gpu.png", png_dir, stem);
        png = hnsoft_encode_png(gpu, BW, BH, &pn);
        if (png) { FILE *fp = fopen(path, "wb"); if (fp) { fwrite(png, 1, pn, fp); fclose(fp); } free(png); }
    }
    free(cpu);
    free(gpu);
}

static double ru_ms(void) {
    struct rusage ru;
    getrusage(RUSAGE_SELF, &ru);
    return (double)(ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000.0
         + (double)(ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000.0;
}

int main(int argc, char **argv) {
    const char *png_dir = argc > 1 ? argv[1] : NULL;
    printf("%s: GPU 渲染后端 v1(Metal)探针\n", BANNER);

    /* 宿主装配(与 hn_app 的 HN_GPU=metal 同一动作) */
    const hnp_gpu_host *host = hnp_gpu_backend();
    if (!host) {
        printf("  [SKIP] 本机无可用 Metal 设备(呼出方回退 hnsoft — 合法降级)\n");
        return 0;
    }
    hn_gpu_set_host(host);
    printf("  host=%s supported_cmds=0x%x\n", host->caps.name, host->caps.supported_cmds);

    /* 测试图片(64×64 纯色 PNG, 引擎自带编码器写盘) */
    char img_path[1024];
    if (png_dir) snprintf(img_path, sizeof(img_path), "%s/gp64.png", png_dir);
    else snprintf(img_path, sizeof(img_path), "/tmp/hn_gpu_probe_gp64.png");
    {
        unsigned char *px = (unsigned char *)malloc(64 * 64 * 4);
        for (int i = 0; i < 64 * 64; i++) {
            px[i * 4 + 0] = 0xD9; px[i * 4 + 1] = 0x48; px[i * 4 + 2] = 0x48; px[i * 4 + 3] = 255;
        }
        size_t pn = 0;
        unsigned char *png = hnsoft_encode_png(px, 64, 64, &pn);
        FILE *f = fopen(img_path, "wb");
        if (!png || !f) { printf("  [FAIL] 测试图片写盘失败\n"); return 1; }
        fwrite(png, 1, pn, f);
        fclose(f);
        free(png);
        free(px);
    }

    /* ---- 页 1: 纯色矩形 + 半透明 + 描边 ---- */
    const hn_display_list *dl = layout(
        "<html><body style=\"background:#0B0E13\">"
        "<div style=\"position:absolute;left:10px;top:10px;width:80px;height:60px;background:#FF3B30\"></div>"
        "<div style=\"position:absolute;left:120px;top:10px;width:100px;height:60px;background:rgba(61,116,255,0.50)\"></div>"
        "<div style=\"position:absolute;left:10px;top:90px;width:220px;height:50px;background:#0A0F0A;border:3px solid #FFE066\"></div>"
        "</body></html>", BW, BH);
    if (dl) check_page_e("rect 页(纯色/半透明/描边)", dl, png_dir, "rect", 1);
    else ck(0, "rect 页", "布局失败");

    /* ---- 页 2: 线性渐变 + 图片(1:1) ---- */
    {
        char html[1600];
        snprintf(html, sizeof(html),
            "<html><body style=\"background:#0B0E13\">"
            "<div style=\"position:absolute;left:10px;top:10px;width:100px;height:60px;"
            "background:linear-gradient(90deg,#FF0000,#0000FF)\"></div>"
            "<img src=\"%s\" style=\"position:absolute;left:120px;top:10px;width:64px;height:64px\">"
            "<div style=\"position:absolute;left:10px;top:90px;width:220px;height:50px;"
            "background:linear-gradient(180deg,#00FF00,#000033)\"></div>"
            "</body></html>", img_path);
        dl = layout(html, BW, BH);
        if (dl) check_page_e("渐变+图片页", dl, png_dir, "grad", 1);
        else ck(0, "渐变+图片页", "布局失败");
    }

    /* ---- 页 3: 文本(FreeType 字形) ---- */
    dl = layout(
        "<html><body style=\"background:#0B0E13\">"
        "<div style=\"position:absolute;left:10px;top:10px;font-size:20px;font-weight:700;color:#FFFFFF\">Hello Metal GPU</div>"
        "<div style=\"position:absolute;left:10px;top:60px;font-size:14px;color:#7FDBCA\">渲染 quick brown fox 0123</div>"
        "</body></html>", BW, BH);
    if (dl) check_page_e("文本页(FreeType 字形)", dl, png_dir, "text", 1);
    else ck(0, "文本页", "布局失败");

    /* ---- 空显示列表: 底色位图, 两路一致(契约: 空列表合法) ---- */
    {
        hn_display_list empty;
        empty.cmds = NULL;
        empty.count = 0;
        if (hn_gpu_available(&empty)) check_page_e("空显示列表(纯底色)", &empty, NULL, "empty", 0);
        else ck(0, "空显示列表", "available==0(应支持)");
    }

    /* ---- ④ POLYGON → 整帧回退判定 + render 拒绝(无伪影) ---- */
    {
        float tri[6] = { 10, 10, 60, 10, 30, 60 };
        hn_cmd cmds[2];
        hn_display_list m;
        memset(cmds, 0, sizeof(cmds));
        cmds[0].kind = HN_CMD_RECT;
        cmds[0].x = 10; cmds[0].y = 10; cmds[0].w = 50; cmds[0].h = 40;
        cmds[0].fill = 0xFF3B30FF;
        cmds[1].kind = HN_CMD_POLYGON;
        cmds[1].poly = tri;
        cmds[1].poly_n = 3;
        cmds[1].fill = 0x00FF7FFF;
        m.cmds = cmds;
        m.count = 2;
        char msg[128];
        snprintf(msg, sizeof(msg), "mixed(RECT+POLYGON) available=%d(期望 0)", hn_gpu_available(&m));
        ck(hn_gpu_available(&m) == 0, "POLYGON 页整帧回退判定", msg);
        unsigned char *out = NULL;
        int rejected = !hn_gpu_render_scaled(&m, 64, 64, BG, 1.0f, &out);
        free(out);
        ck(rejected, "不支持指令 render 拒绝(回退 hnsoft = 逐位一致, 无伪影)",
           rejected ? "render 返回 0" : "render 竟然成功");
    }

    /* ---- ⑤ QUAD(任意四顶点)可走 GPU; 契约口径 = 采样盒内关键点 ----
       斜边像素 hnsoft 是 0.5px 覆盖率 AA、GPU 是硬边 —— 边界像素结构性
       不同(±半覆盖), 全幅 maxdiff 只作信息值; 断言采**内部点**(质心 +
       各顶点方向对半内收, 全部离边 ≥ 数像素), 容差按契约 ±40/通道。 */
    {
        hn_cmd c;
        hn_display_list q;
        memset(&c, 0, sizeof(c));
        c.kind = HN_CMD_QUAD;
        c.fill = 0x3D74FFFF;
        c.qx[0] = 20;  c.qy[0] = 20;
        c.qx[1] = 200; c.qy[1] = 30;
        c.qx[2] = 180; c.qy[2] = 130;
        c.qx[3] = 30;  c.qy[3] = 120;
        q.cmds = &c;
        q.count = 1;
        int av = hn_gpu_available(&q);
        unsigned char *bgra = NULL;
        int ok = av && hn_gpu_render_scaled(&q, BW, BH, BG, 1.0f, &bgra) && bgra;
        if (ok) {
            unsigned char *gpu = gpu_rgba(bgra, BW, BH);
            free(bgra);
            unsigned char *cpu = hnsoft_render_scaled(&q, BW, BH, BG, 1.0f);
            float cx = (20 + 200 + 180 + 30) / 4.0f, cy = (20 + 30 + 130 + 120) / 4.0f;
            int pts[5][2] = {
                { (int)cx, (int)cy },
                { (int)(cx + (20 - cx) / 2),  (int)(cy + (20 - cy) / 2) },
                { (int)(cx + (200 - cx) / 2), (int)(cy + (30 - cy) / 2) },
                { (int)(cx + (180 - cx) / 2), (int)(cy + (130 - cy) / 2) },
                { (int)(cx + (30 - cx) / 2),  (int)(cy + (120 - cy) / 2) },
            };
            int worst = 0, maxdiff = 0;
            for (int i = 0; i < 5; i++) {
                int x = pts[i][0], y = pts[i][1];
                for (int ch = 0; ch < 4; ch++) {
                    int d = abs((int)cpu[(y * BW + x) * 4 + ch]
                              - (int)gpu[(y * BW + x) * 4 + ch]);
                    if (d > worst) worst = d;
                }
            }
            for (int i = 0; i < BW * BH * 4; i++) {
                int d = abs((int)cpu[i] - (int)gpu[i]);
                if (d > maxdiff) maxdiff = d;
            }
            free(cpu);
            free(gpu);
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "内部点色差=%d/255(容差40) 全幅max=%d(斜边AA结构差, 信息值)",
                     worst, maxdiff);
            ck(worst <= 40, "QUAD 任意四顶点(3D 面片口径)", msg);
        } else {
            ck(0, "QUAD 任意四顶点", "GPU 渲染失败");
        }
    }

    /* ---- ⑥ CPU 对照(信息值): 同页 metal vs hnsoft 每帧耗时 ---- */
    {
        dl = layout(
            "<html><body style=\"background:#0B0E13\">"
            "<div style=\"position:absolute;left:10px;top:10px;width:80px;height:60px;background:#FF3B30\"></div>"
            "<div style=\"position:absolute;left:10px;top:80px;font-size:14px;color:#FFFFFF\">fps 0123456789</div>"
            "</body></html>", BW, BH);
        if (dl && hn_gpu_available(dl)) {
            enum { N = 120 };
            double t0, t1;
            unsigned char *b = NULL;
            for (int i = 0; i < 5; i++) {   /* 预热(着色器编译/纹理缓存) */
                hn_context_repaint(g_ctx);
                if (b) free(b);
                b = NULL;
                hn_gpu_render_scaled(dl, BW, BH, BG, 1.0f, &b);
            }
            free(b);
            t0 = ru_ms();
            for (int i = 0; i < N; i++) {
                hn_context_repaint(g_ctx);
                b = NULL;
                hn_gpu_render_scaled(dl, BW, BH, BG, 1.0f, &b);
                free(b);
            }
            t1 = ru_ms();
            double gpu_ms = (t1 - t0) / N;
            for (int i = 0; i < 5; i++) {
                hn_context_repaint(g_ctx);
                unsigned char *rgba = hnsoft_render_scaled(dl, BW, BH, BG, 1.0f);
                free(rgba);
            }
            t0 = ru_ms();
            for (int i = 0; i < N; i++) {
                hn_context_repaint(g_ctx);
                unsigned char *rgba = hnsoft_render_scaled(dl, BW, BH, BG, 1.0f);
                free(rgba);
            }
            t1 = ru_ms();
            printf("  [perf] metal=%.3f ms/帧 vs hnsoft=%.3f ms/帧(%d 帧 CPU 时间, "
                   "信息值不判定; GPU 版含 readback 回拷)\n", gpu_ms, (t1 - t0) / N, N);
        } else {
            printf("  [perf] 跳过(页不可走 GPU)\n");
        }
    }

    if (g_ctx) { hn_context_destroy(g_ctx); g_ctx = NULL; }
    if (g_fail) { printf("%s: FAIL(%d 项)\n", BANNER, g_fail); return 1; }
    printf("%s: OK\n", BANNER);
    return 0;
}
CEOF

hn_sources_soft   # 引擎核心 + hnsoft(HN_SOURCES)
HN_SOURCES=("$SRC_C" "$HN_ENG/hn_gpu.c" "$HN_REPO_ROOT/platform/hnp_gpu_metal.c"
            ${HN_SOURCES[@]+"${HN_SOURCES[@]}"})
hn_sources_check ${HN_SOURCES[@]+"${HN_SOURCES[@]}"} || exit 1

FT_INC_DIR="$(hn_freetype_inc)" || { echo "$LABEL: FAIL 缺 FreeType 头(brew install freetype)"; exit 1; }
HN_CFLAGS=(-std=c99 -O2 -Wall -ffp-contract=off)
HN_INCS=(-I "$HN_ENG/include" -I "$HN_ENG" -I "$HN_REPO_ROOT/platform" -I "$FT_INC_DIR")
HN_LIBS=(-L/opt/homebrew/lib -lfreetype -lm)
# 注意: **没有** -framework Metal —— 纯 dlopen(hnp_gpu_metal.c 手法),
# 链接期零 framework 参数正是"零编译期 SDK 依赖"的字面含义。

BIN="$HDIR/hn_gpu_probe_bin"
hn_probe_compile "$LABEL" "$BIN" || exit 1

PNGDIR="$(hn_tmpd gpushot)"
echo "$LABEL: PNG 对照产物: $PNGDIR/{rect,grad,text}-{cpu,gpu}.png"

# 门禁口径: 缺 Metal 设备时 harness 自行 SKIP(退出 0); 断言失败非 0。
hn_probe_run "$LABEL" "$BIN" "$PNGDIR"

# ---- hnapp 集成冒烟(存在才做): 真装配链路 hn_app → hn_rt_set_gpu_host →
#      hn_rt 的弱桩被 hn_gpu.c 强定义覆盖 → HN_GPU=metal 下 --shot 走 GPU
#      (HN_GPU 未设时同命令恒 hnsoft —— 两种落盘都必须成功)。 ----
hnapp="$HN_REPO_ROOT/dist/hnapp-macos-arm64"
if [ -x "$hnapp" ]; then
    page="$HN_REPO_ROOT/examples/fmt-demo.html"
    if env -u HN_GPU "$hnapp" "$page" --shot "$PNGDIR/hnapp-cpu.png" 480 700 >/dev/null 2>&1 \
       && env HN_GPU=metal "$hnapp" "$page" --shot "$PNGDIR/hnapp-gpu.png" 480 700 >/dev/null 2>&1 \
       && [ -s "$PNGDIR/hnapp-cpu.png" ] && [ -s "$PNGDIR/hnapp-gpu.png" ]; then
        echo "$LABEL: hnapp --shot 集成冒烟 OK(cpu/metal 两路落盘: $PNGDIR/hnapp-*.png)"
    else
        echo "$LABEL: FAIL hnapp --shot 集成冒烟"
        exit 1
    fi
else
    echo "$LABEL: (跳过 hnapp 集成冒烟: dist/hnapp-macos-arm64 不存在)"
fi

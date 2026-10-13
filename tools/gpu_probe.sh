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
#      v2 回归: 含 MESH 的页同样整帧回退(位集不含);
#   ⑤ QUAD(任意四顶点)可走 GPU 且与 hnsoft 色差同容差;
#   ⑥ 混合模式四页(multiply/screen/lighter/source-over 各一): 重叠/非重叠
#      采样点 GPU vs hnsoft 色差 ≤ 40/通道; hnsoft 各模式输出互异(测试有牙);
#   ⑦ pattern 三页(repeat/repeat-x/no-repeat): 象限色块源图的平铺周期
#      采样断言(带外像素 = 底色, 两路同判);
#   ⑧ pdata 页: RGBA 块(脏矩形子区)贴入的像素断言;
#   ⑨ cpath 裁剪页: 路径内/外像素断言(GPU 与 hnsoft 同判定);
#   ⑩ CPU 对照(信息值不判定): 同页 metal vs hnsoft 的 rusage 每帧耗时
#      (含混合页与 pattern 页)。
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

/* ---- v2 断言助手(原始 hn_cmd 页: GPU 与 hnsoft 同一显示列表双渲染) ---- */

/* 双路渲染: 返回非预乘 RGBA(gpu 失败 = NULL) */
static int render_pair(const hn_display_list *dl,
                       unsigned char **gpu, unsigned char **cpu) {
    *gpu = NULL;
    *cpu = NULL;
    unsigned char *bgra = NULL;
    if (!hn_gpu_render_scaled(dl, BW, BH, BG, 1.0f, &bgra) || !bgra) return 0;
    *gpu = gpu_rgba(bgra, BW, BH);
    free(bgra);
    if (!*gpu) return 0;
    *cpu = hnsoft_render_scaled(dl, BW, BH, BG, 1.0f);
    return *cpu != NULL;
}

/* 采样点四通道最大差 */
static int pt_diff(const unsigned char *a, const unsigned char *b, int x, int y) {
    int worst = 0;
    for (int ch = 0; ch < 4; ch++) {
        int d = abs((int)a[(y * BW + x) * 4 + ch] - (int)b[(y * BW + x) * 4 + ch]);
        if (d > worst) worst = d;
    }
    return worst;
}

/* 采样点与期望色(RGBA 非预乘)的距离 */
static int pt_near(const unsigned char *px, int x, int y, unsigned rgba, int tol) {
    int want[4] = { (int)((rgba >> 24) & 0xFF), (int)((rgba >> 16) & 0xFF),
                    (int)((rgba >> 8) & 0xFF), (int)(rgba & 0xFF) };
    for (int ch = 0; ch < 4; ch++) {
        int d = abs((int)px[(y * BW + x) * 4 + ch] - want[ch]);
        if (d > tol) return d;
    }
    return 0;
}

/* 原始 cmd 页的 CPU 对照计时(信息值): metal vs hnsoft ms/帧 */
static void perf_cmds(const char *tag, const hn_display_list *dl) {
    if (!hn_gpu_available(dl)) {
        printf("  [perf] %s: 跳过(页不可走 GPU)\n", tag);
        return;
    }
    enum { N = 120 };
    double t0, t1;
    unsigned char *b = NULL;
    for (int i = 0; i < 5; i++) {   /* 预热(管线/纹理缓存) */
        free(b);
        b = NULL;
        hn_gpu_render_scaled(dl, BW, BH, BG, 1.0f, &b);
    }
    free(b);
    t0 = ru_ms();
    for (int i = 0; i < N; i++) {
        b = NULL;
        hn_gpu_render_scaled(dl, BW, BH, BG, 1.0f, &b);
        free(b);
    }
    t1 = ru_ms();
    double gpu_ms = (t1 - t0) / N;
    for (int i = 0; i < 5; i++) {
        unsigned char *r = hnsoft_render_scaled(dl, BW, BH, BG, 1.0f);
        free(r);
    }
    t0 = ru_ms();
    for (int i = 0; i < N; i++) {
        unsigned char *r = hnsoft_render_scaled(dl, BW, BH, BG, 1.0f);
        free(r);
    }
    t1 = ru_ms();
    printf("  [perf] %s: metal=%.3f ms/帧 vs hnsoft=%.3f ms/帧(信息值不判定)\n",
           tag, gpu_ms, (t1 - t0) / N);
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

    /* ---- ④b v2 回退回归: 含 MESH 的页仍整帧回退(位集不含) ---- */
    {
        hn_cmd cmds[2];
        hn_display_list m;
        float mv[12] = { 0 };
        memset(cmds, 0, sizeof(cmds));
        cmds[0].kind = HN_CMD_RECT;
        cmds[0].x = 10; cmds[0].y = 10; cmds[0].w = 50; cmds[0].h = 40;
        cmds[0].fill = 0xFF3B30FF;
        cmds[1].kind = HN_CMD_MESH;
        cmds[1].mesh_verts = mv;
        cmds[1].mesh_uv = mv;
        cmds[1].mesh_cols = 1;
        cmds[1].mesh_rows = 1;
        m.cmds = cmds;
        m.count = 2;
        char msg[128];
        snprintf(msg, sizeof(msg), "mixed(RECT+MESH) available=%d(期望 0)", hn_gpu_available(&m));
        ck(hn_gpu_available(&m) == 0, "MESH 页整帧回退判定(v2 回归)", msg);
        unsigned char *out = NULL;
        int rejected = !hn_gpu_render_scaled(&m, 64, 64, BG, 1.0f, &out);
        free(out);
        ck(rejected, "MESH 页 render 拒绝(整帧回退 hnsoft)",
           rejected ? "render 返回 0" : "render 竟然成功");
    }

    /* ---- ⑥ 混合模式四页(multiply/screen/lighter/source-over 各一) ----
       页面: 不透明底色上不透明红盒 + 50% 蓝盒(带 blend) —— 重叠区目标
       不透明 = 契约等价换算的封闭域(推导见 hn_platform.h draw_quads 注),
       采样点 GPU vs hnsoft ≤ 40/通道(预期 0-1); 另断言 hnsoft 各模式
       输出互异(证明断言有牙 —— GPU 若把 blend 当 source-over 画立刻超差)。 */
    {
        const int modes[4] = { HN_BLEND_MULTIPLY, HN_BLEND_SCREEN,
                               HN_BLEND_LIGHTER, HN_BLEND_SRC_OVER };
        const char *mnames[4] = { "multiply", "screen", "lighter", "source-over" };
        unsigned ov_hnsoft[4] = { 0 };
        int ox = 80, oy = 50;   /* 重叠区采样点(红盒∩蓝盒内部) */
        for (int mi = 0; mi < 4; mi++) {
            hn_cmd cmds[2];
            hn_display_list m;
            memset(cmds, 0, sizeof(cmds));
            cmds[0].kind = HN_CMD_RECT;
            cmds[0].x = 10; cmds[0].y = 10; cmds[0].w = 100; cmds[0].h = 80;
            cmds[0].fill = 0xFF3B30FF;                    /* 不透明红 */
            cmds[1].kind = HN_CMD_RECT;
            cmds[1].x = 60; cmds[1].y = 30; cmds[1].w = 120; cmds[1].h = 90;
            cmds[1].fill = 0x3D74FF80;                    /* 50% 蓝 */
            cmds[1].blend = (unsigned char)modes[mi];
            m.cmds = cmds;
            m.count = 2;
            if (!hn_gpu_available(&m)) { ck(0, mnames[mi], "available==0(应支持)"); continue; }
            unsigned char *gpu, *cpu;
            if (!render_pair(&m, &gpu, &cpu)) { ck(0, mnames[mi], "渲染失败"); continue; }
            int d_ov = pt_diff(cpu, gpu, ox, oy);         /* 重叠区(目标不透明) */
            int d_bonly = pt_diff(cpu, gpu, 150, 110);    /* 蓝独占区(底色上) */
            int d_ronly = pt_diff(cpu, gpu, 30, 20);      /* 红独占区(混合外) */
            int fullmax = 0;
            for (int i = 0; i < BW * BH * 4; i++) {
                int d = abs((int)cpu[i] - (int)gpu[i]);
                if (d > fullmax) fullmax = d;
            }
            ov_hnsoft[mi] = ((unsigned)cpu[(oy * BW + ox) * 4] << 24)
                          | ((unsigned)cpu[(oy * BW + ox) * 4 + 1] << 16)
                          | ((unsigned)cpu[(oy * BW + ox) * 4 + 2] << 8)
                          | (unsigned)cpu[(oy * BW + ox) * 4 + 3];
            char msg[160];
            snprintf(msg, sizeof(msg),
                     "重叠=%d 蓝独占=%d 红独占=%d 全幅max=%d(容差40)", d_ov, d_bonly, d_ronly, fullmax);
            ck(d_ov <= 40 && d_bonly <= 40 && d_ronly <= 40,
               mnames[mi], msg);
            free(gpu);
            free(cpu);
        }
        /* 有牙: hnsoft 的 multiply/screen/lighter 重叠区与 source-over 互异 */
        int teeth = 1;
        for (int mi = 0; mi < 3; mi++) {
            int diff = 0;
            for (int ch = 0; ch < 4; ch++) {
                int a = (int)((ov_hnsoft[mi] >> (24 - ch * 8)) & 0xFF);
                int b = (int)((ov_hnsoft[3] >> (24 - ch * 8)) & 0xFF);
                int d = abs(a - b);
                if (d > diff) diff = d;
            }
            if (diff < 8) teeth = 0;
        }
        ck(teeth, "混合模式断言有牙(hnsoft 各模式重叠区互异)",
           teeth ? "multiply/screen/lighter 均偏离 source-over ≥8/通道" : "有模式与 source-over 撞色");
    }

    /* ---- ⑦ pattern 三页(repeat/repeat-x/no-repeat) ----
       源图 = 64×64 四象限色块(TL 红/TR 绿/BL 蓝/BR 黄), 锚点 = 画布原点
       (0,0)。断言: 平铺周期采样点(首瓦片四象限 + 跨瓦片折回)+ 覆盖带外
       像素 = 底色(repeat-x 纵越界 / no-repeat 双轴越界, GPU 与 hnsoft
       同判)。容差 ±40/通道(既有口径; 预期 0-1 —— 1:1 平铺文素精确)。 */
    {
        char quad_path[1024];
        if (png_dir) snprintf(quad_path, sizeof(quad_path), "%s/gp64quad.png", png_dir);
        else snprintf(quad_path, sizeof(quad_path), "/tmp/hn_gpu_probe_gp64quad.png");
        {
            unsigned char *px = (unsigned char *)malloc(64 * 64 * 4);
            for (int y = 0; y < 64; y++)
                for (int x = 0; x < 64; x++) {
                    /* RGBA 字节序逐字节写(hn_png/hnsoft 口径; 不按机器序
                       整块 memcpy —— 小端下会把 R 与 A 对调) */
                    unsigned c = (x < 32 ? (y < 32 ? 0xD94848FFu : 0x3D74FFFFu)
                                         : (y < 32 ? 0x2ECC71FFu : 0xFFD60AFFu));
                    unsigned char *p = px + ((size_t)y * 64 + x) * 4;
                    p[0] = (unsigned char)((c >> 24) & 0xFF);
                    p[1] = (unsigned char)((c >> 16) & 0xFF);
                    p[2] = (unsigned char)((c >> 8) & 0xFF);
                    p[3] = (unsigned char)(c & 0xFF);
                }
            size_t pn = 0;
            unsigned char *png = hnsoft_encode_png(px, 64, 64, &pn);
            FILE *f = fopen(quad_path, "wb");
            if (!png || !f) { printf("  [FAIL] pattern 测试图片写盘失败\n"); return 1; }
            fwrite(png, 1, pn, f);
            fclose(f);
            free(png);
            free(px);
        }
        const int reps[3] = { HN_PAT_REPEAT, HN_PAT_REPEAT_X, HN_PAT_NONE };
        const char *rnames[3] = { "pattern repeat", "pattern repeat-x", "pattern no-repeat" };
        /* 采样表: x, y, 期望色(平铺折回后的象限色 / BG = 带外) */
        const struct { int x, y; unsigned want; const char *tag; } PT[3][6] = {
            { /* repeat: 四象限 + 双轴折回 */
              { 12,  12, 0xD94848FFu, "TL" }, { 44,  12, 0x2ECC71FFu, "TR" },
              { 12,  44, 0x3D74FFFFu, "BL" }, { 44,  44, 0xFFD60AFFu, "BR" },
              { 76,  76, 0xD94848FFu, "折回TL" }, { 140, 108, 0x3D74FFFFu, "折回BL" } },
            { /* repeat-x: 纵带 [0,64) 内平铺, 带外 = 底色 */
              { 12,  12, 0xD94848FFu, "TL" }, { 44,  44, 0xFFD60AFFu, "BR" },
              { 76,  12, 0xD94848FFu, "横折回" }, { 12,  80, 0x00000000u, "带外=底色" },
              { 44,  40, 0xFFD60AFFu, "BR2" }, { 172, 30, 0x2ECC71FFu, "TR折回" } },
            { /* no-repeat: 只画源矩形投影 (0..64)², 之外 = 底色 */
              { 12,  12, 0xD94848FFu, "TL" }, { 44,  44, 0xFFD60AFFu, "BR" },
              { 76,  12, 0x00000000u, "x越界" }, { 12,  80, 0x00000000u, "y越界" },
              { 30,  60, 0x3D74FFFFu, "BL" }, { 60,  60, 0xFFD60AFFu, "BR2" } },
        };
        for (int ri = 0; ri < 3; ri++) {
            hn_cmd c;
            hn_display_list m;
            memset(&c, 0, sizeof(c));
            c.kind = HN_CMD_PATTERN;
            c.text = quad_path;
            c.pattern_rep = (unsigned char)reps[ri];
            c.pat_ox = 0; c.pat_oy = 0;               /* 锚点 = 画布原点 */
            c.x = 10; c.y = 10; c.w = 200; c.h = 120;
            m.cmds = &c;
            m.count = 1;
            if (!hn_gpu_available(&m)) { ck(0, rnames[ri], "available==0(应支持)"); continue; }
            unsigned char *gpu, *cpu;
            if (!render_pair(&m, &gpu, &cpu)) { ck(0, rnames[ri], "渲染失败"); continue; }
            int worst = 0, worst_at = -1, want_fail = 0;
            for (int pi = 0; pi < 6; pi++) {
                int d = pt_diff(cpu, gpu, PT[ri][pi].x, PT[ri][pi].y);
                if (d > worst) { worst = d; worst_at = pi; }
                if (PT[ri][pi].want == 0x00000000u) {
                    /* 带外: 两路都应是底色(对 hnsoft 权威判定) */
                    int db = pt_near(cpu, PT[ri][pi].x, PT[ri][pi].y, BG, 8)
                           + pt_near(gpu, PT[ri][pi].x, PT[ri][pi].y, BG, 40);
                    if (db > 48) want_fail = 1;
                } else if (pt_near(cpu, PT[ri][pi].x, PT[ri][pi].y, PT[ri][pi].want, 40)) {
                    want_fail = 1;   /* hnsoft 自身就该是象限色(采样表对表) */
                }
            }
            char msg[160];
            snprintf(msg, sizeof(msg), "最大色差=%d/255(@%s) 采样表=%s(容差40)",
                     worst, worst_at >= 0 ? PT[ri][worst_at].tag : "?",
                     want_fail ? "MISMATCH" : "ok");
            ck(worst <= 40 && !want_fail, rnames[ri], msg);
            free(gpu);
            free(cpu);
        }
    }

    /* ---- ⑧ pdata 页(putImageData 块 + 脏矩形子区贴入) ---- */
    {
        /* 32×20 RGBA 块: 左 16 列红 / 右 16 列绿(全不透明; 逐字节 RGBA) */
        static unsigned char blk[32 * 20 * 4];
        for (int y = 0; y < 20; y++)
            for (int x = 0; x < 32; x++) {
                unsigned c = x < 16 ? 0xD94848FFu : 0x2ECC71FFu;
                unsigned char *p = blk + ((size_t)y * 32 + x) * 4;
                p[0] = (unsigned char)((c >> 24) & 0xFF);
                p[1] = (unsigned char)((c >> 16) & 0xFF);
                p[2] = (unsigned char)((c >> 8) & 0xFF);
                p[3] = (unsigned char)(c & 0xFF);
            }
        hn_cmd c;
        hn_display_list m;
        memset(&c, 0, sizeof(c));
        c.kind = HN_CMD_PDATA;
        c.bitmap = blk;
        c.bitmap_stride = 32 * 4;
        c.bitmap_w = 32;
        c.bitmap_h = 20;
        c.x = 40; c.y = 30; c.w = 16; c.h = 10;      /* 脏矩形 1:1 贴入 */
        c.isx = 4; c.isy = 2; c.isw = 16; c.ish = 10;
        c.has_isrc = 1;
        m.cmds = &c;
        m.count = 1;
        if (hn_gpu_available(&m)) {
            unsigned char *gpu, *cpu;
            if (render_pair(&m, &gpu, &cpu)) {
                int d1 = pt_diff(cpu, gpu, 45, 34);   /* 块内红列(9,2) */
                int d2 = pt_diff(cpu, gpu, 52, 34);   /* 块内绿列(16,2) */
                int d3 = pt_diff(cpu, gpu, 55, 39);   /* 块内绿列(19,9) */
                int o1 = pt_near(cpu, 38, 34, BG, 8) + pt_near(gpu, 38, 34, BG, 40);
                int o2 = pt_near(cpu, 45, 41, BG, 8) + pt_near(gpu, 45, 41, BG, 40);
                int r_ok = !pt_near(cpu, 45, 34, 0xD94848FFu, 40)
                        && !pt_near(gpu, 45, 34, 0xD94848FFu, 40);
                int g_ok = !pt_near(cpu, 52, 34, 0x2ECC71FFu, 40)
                        && !pt_near(gpu, 52, 34, 0x2ECC71FFu, 40);
                char msg[160];
                snprintf(msg, sizeof(msg),
                         "块内差=%d/%d/%d 带外底色=%s 象限色=%s(容差40)",
                         d1, d2, d3, (o1 <= 48 && o2 <= 48) ? "ok" : "MISMATCH",
                         (r_ok && g_ok) ? "ok" : "MISMATCH");
                ck(d1 <= 40 && d2 <= 40 && d3 <= 40 && o1 <= 48 && o2 <= 48 && r_ok && g_ok,
                   "pdata 块贴入(脏矩形子区)", msg);
            } else {
                ck(0, "pdata 块贴入(脏矩形子区)", "渲染失败");
            }
            free(gpu);
            free(cpu);
        } else {
            ck(0, "pdata 块贴入(脏矩形子区)", "available==0(应支持)");
        }
    }

    /* ---- ⑨ cpath 裁剪页(路径内/外像素断言, GPU 与 hnsoft 同判定) ---- */
    {
        float tri[6] = { 30, 20, 210, 60, 60, 140 };   /* 非零环绕单多边形 */
        hn_cmd cmds[4];
        hn_display_list m;
        memset(cmds, 0, sizeof(cmds));
        cmds[0].kind = HN_CMD_RECT;
        cmds[0].x = 10; cmds[0].y = 10; cmds[0].w = 220; cmds[0].h = 140;
        cmds[0].fill = 0x30D5C8FF;                    /* 裁剪下的内容(青) */
        cmds[1].kind = HN_CMD_CLIP_PATH_PUSH;
        cmds[1].poly = tri;
        cmds[1].poly_n = 3;
        cmds[2].kind = HN_CMD_RECT;
        cmds[2].x = 10; cmds[2].y = 10; cmds[2].w = 220; cmds[2].h = 140;
        cmds[2].fill = 0xFFD60AFF;                    /* 被裁的黄 */
        cmds[3].kind = HN_CMD_CLIP_POP;
        m.cmds = cmds;
        m.count = 4;
        if (hn_gpu_available(&m)) {
            unsigned char *gpu, *cpu;
            if (render_pair(&m, &gpu, &cpu)) {
                /* 质心(100,73)在三角形内 = 黄; (15,15)在矩形不在路径 = 青;
                   (235,155)在盒外 = 底色 —— 两路同判 */
                int d_in = pt_diff(cpu, gpu, 100, 73);
                int d_out = pt_diff(cpu, gpu, 15, 15);
                int d_bg = pt_diff(cpu, gpu, 235, 155);
                int in_ok = !pt_near(cpu, 100, 73, 0xFFD60AFFu, 40)
                         && !pt_near(gpu, 100, 73, 0xFFD60AFFu, 40);
                int out_ok = !pt_near(cpu, 15, 15, 0x30D5C8FFu, 40)
                          && !pt_near(gpu, 15, 15, 0x30D5C8FFu, 40);
                int bg_ok = !pt_near(cpu, 235, 155, BG, 8)
                         && !pt_near(gpu, 235, 155, BG, 40);
                int fullmax = 0;
                for (int i = 0; i < BW * BH * 4; i++) {
                    int d = abs((int)cpu[i] - (int)gpu[i]);
                    if (d > fullmax) fullmax = d;
                }
                char msg[160];
                snprintf(msg, sizeof(msg),
                         "内=%d 外=%d 盒外=%d 全幅max=%d 判定=%s/%s/%s(容差40)",
                         d_in, d_out, d_bg, fullmax,
                         in_ok ? "内黄ok" : "内MISMATCH",
                         out_ok ? "外青ok" : "外MISMATCH",
                         bg_ok ? "底ok" : "底MISMATCH");
                ck(d_in <= 40 && d_out <= 40 && d_bg <= 40 && in_ok && out_ok && bg_ok,
                   "cpath 精确裁剪(非零环绕)", msg);
            } else {
                ck(0, "cpath 精确裁剪(非零环绕)", "渲染失败");
            }
            free(gpu);
            free(cpu);
        } else {
            ck(0, "cpath 精确裁剪(非零环绕)", "available==0(应支持)");
        }
    }

    /* ---- ⑨b 路径层上叠矩形裁剪(掩码跨层存活): 栈 [cpath, clip, ...] ----
       hnsoft 逐层判定(多边形层 + 矩形层同时生效); GPU 侧掩码绑定必须
       跨矩形层存活(最近路径层搜索)。序: 橙画在 路径∩矩形(矩形还开着) →
       弹矩形层 → 黄画在整条路径内(掩码必须还在) —— 路径∩矩形 = 橙,
       路径独占 = 黄, 路径外 = 青。 */
    {
        float tri[6] = { 30, 20, 210, 60, 60, 140 };
        hn_cmd cmds[6];
        memset(cmds, 0, sizeof(cmds));
        cmds[0].kind = HN_CMD_RECT;
        cmds[0].x = 10; cmds[0].y = 10; cmds[0].w = 220; cmds[0].h = 140;
        cmds[0].fill = 0x30D5C8FF;
        cmds[1].kind = HN_CMD_CLIP_PATH_PUSH;
        cmds[1].poly = tri;
        cmds[1].poly_n = 3;
        cmds[2].kind = HN_CMD_CLIP_PUSH;              /* 路径层上叠矩形层 */
        cmds[2].x = 50; cmds[2].y = 40; cmds[2].w = 120; cmds[2].h = 90;
        cmds[3].kind = HN_CMD_RECT;
        cmds[3].x = 10; cmds[3].y = 10; cmds[3].w = 220; cmds[3].h = 140;
        cmds[3].fill = 0xFF9F0AFF;                    /* 路径∩矩形 = 橙 */
        cmds[4].kind = HN_CMD_CLIP_POP;               /* 弹矩形层(路径层仍开) */
        cmds[5].kind = HN_CMD_RECT;
        cmds[5].x = 10; cmds[5].y = 100; cmds[5].w = 220; cmds[5].h = 50;
        cmds[5].fill = 0xFFD60AFF;                    /* 只剩路径层(下半): 黄
                                                         —— 不覆盖 ∩ 区的橙 */
        hn_display_list m = { cmds, 6 };
        if (hn_gpu_available(&m)) {
            unsigned char *gpu, *cpu;
            if (render_pair(&m, &gpu, &cpu)) {
                int d1 = pt_diff(cpu, gpu, 100, 73);  /* 路径∩矩形 = 橙 */
                int d2 = pt_diff(cpu, gpu, 63, 135);  /* 路径内矩形外 = 黄 */
                int d3 = pt_diff(cpu, gpu, 15, 15);   /* 路径外 = 青 */
                int o_ok = !pt_near(cpu, 100, 73, 0xFF9F0AFFu, 40)
                        && !pt_near(gpu, 100, 73, 0xFF9F0AFFu, 40);
                int y_ok = !pt_near(cpu, 63, 135, 0xFFD60AFFu, 40)
                        && !pt_near(gpu, 63, 135, 0xFFD60AFFu, 40);
                int t_ok = !pt_near(cpu, 15, 15, 0x30D5C8FFu, 40)
                        && !pt_near(gpu, 15, 15, 0x30D5C8FFu, 40);
                char msg[160];
                snprintf(msg, sizeof(msg), "路径∩矩形=%d 路径独占=%d 路径外=%d 判定=%s/%s/%s",
                         d1, d2, d3,
                         o_ok ? "橙ok" : "橙MISMATCH",
                         y_ok ? "黄ok" : "黄MISMATCH",
                         t_ok ? "青ok" : "青MISMATCH");
                ck(d1 <= 40 && d2 <= 40 && d3 <= 40 && o_ok && y_ok && t_ok,
                   "cpath+clip 叠栈(掩码跨层存活)", msg);
            } else {
                ck(0, "cpath+clip 叠栈(掩码跨层存活)", "渲染失败");
            }
            free(gpu);
            free(cpu);
        } else {
            ck(0, "cpath+clip 叠栈(掩码跨层存活)", "available==0(应支持)");
        }
    }

    /* ---- ⑨c 多子路径 cpath(segs 点数表, 子路径各自闭合) ----
       两个不相交三角形 = 一条 CLIP_PATH_PUSH(segs=[3,3]): 任一三角形内
       = 黄, 两形之间 = 青。覆盖 path_mask_raster 的 segs 分段闭合边。 */
    {
        float two[12] = { 30, 20, 100, 30, 50, 80,     /* 子路径 1: 左上三角 */
                          150, 60, 210, 130, 130, 130 }; /* 子路径 2: 右下三角 */
        int segs[2] = { 3, 3 };
        hn_cmd cmds[4];
        memset(cmds, 0, sizeof(cmds));
        cmds[0].kind = HN_CMD_RECT;
        cmds[0].x = 10; cmds[0].y = 10; cmds[0].w = 220; cmds[0].h = 140;
        cmds[0].fill = 0x30D5C8FF;
        cmds[1].kind = HN_CMD_CLIP_PATH_PUSH;
        cmds[1].poly = two;
        cmds[1].poly_n = 6;
        cmds[1].clip_segs = segs;
        cmds[1].clip_nsegs = 2;
        cmds[2].kind = HN_CMD_RECT;
        cmds[2].x = 10; cmds[2].y = 10; cmds[2].w = 220; cmds[2].h = 140;
        cmds[2].fill = 0xFFD60AFF;
        cmds[3].kind = HN_CMD_CLIP_POP;
        hn_display_list m = { cmds, 4 };
        if (hn_gpu_available(&m)) {
            unsigned char *gpu, *cpu;
            if (render_pair(&m, &gpu, &cpu)) {
                int d1 = pt_diff(cpu, gpu, 55, 40);    /* 子路径 1 内 = 黄 */
                int d2 = pt_diff(cpu, gpu, 165, 105);  /* 子路径 2 内 = 黄 */
                int d3 = pt_diff(cpu, gpu, 110, 90);   /* 两形之间 = 青 */
                int in1 = !pt_near(cpu, 55, 40, 0xFFD60AFFu, 40)
                       && !pt_near(gpu, 55, 40, 0xFFD60AFFu, 40);
                int in2 = !pt_near(cpu, 165, 105, 0xFFD60AFFu, 40)
                       && !pt_near(gpu, 165, 105, 0xFFD60AFFu, 40);
                int mid = !pt_near(cpu, 110, 90, 0x30D5C8FFu, 40)
                       && !pt_near(gpu, 110, 90, 0x30D5C8FFu, 40);
                char msg[160];
                snprintf(msg, sizeof(msg), "子路径1=%d 子路径2=%d 形间=%d 判定=%s/%s/%s",
                         d1, d2, d3,
                         in1 ? "黄ok" : "黄MISMATCH",
                         in2 ? "黄ok" : "黄MISMATCH",
                         mid ? "青ok" : "青MISMATCH");
                ck(d1 <= 40 && d2 <= 40 && d3 <= 40 && in1 && in2 && mid,
                   "cpath 多子路径(segs 分段)", msg);
            } else {
                ck(0, "cpath 多子路径(segs 分段)", "渲染失败");
            }
            free(gpu);
            free(cpu);
        } else {
            ck(0, "cpath 多子路径(segs 分段)", "available==0(应支持)");
        }
    }

    /* ---- ⑨d ppath(pattern 多边形目标 = 掩码基建共用) ----
       象限色块 repeat 平铺 + 三角形目标: 形内 = 平铺象限色, 形外 = 底色。 */
    {
        char quad_path[1024];
        if (png_dir) snprintf(quad_path, sizeof(quad_path), "%s/gp64quad.png", png_dir);
        else snprintf(quad_path, sizeof(quad_path), "/tmp/hn_gpu_probe_gp64quad.png");
        float tri[6] = { 30, 20, 210, 40, 80, 140 };
        hn_cmd c;
        memset(&c, 0, sizeof(c));
        c.kind = HN_CMD_PATTERN;
        c.text = quad_path;
        c.pattern_rep = HN_PAT_REPEAT;
        c.pat_ox = 0;
        c.pat_oy = 0;
        c.poly = tri;
        c.poly_n = 3;
        hn_display_list m = { &c, 1 };
        if (hn_gpu_available(&m)) {
            unsigned char *gpu, *cpu;
            if (render_pair(&m, &gpu, &cpu)) {
                int d1 = pt_diff(cpu, gpu, 40, 30);    /* 形内(第一瓦 TR 绿) */
                int d2 = pt_diff(cpu, gpu, 60, 50);    /* 形内(BR 黄) */
                int d3 = pt_diff(cpu, gpu, 15, 100);   /* 形外 = 底色 */
                int in1 = !pt_near(cpu, 40, 30, 0x2ECC71FFu, 40)
                       && !pt_near(gpu, 40, 30, 0x2ECC71FFu, 40);
                int in2 = !pt_near(cpu, 60, 50, 0xFFD60AFFu, 40)
                       && !pt_near(gpu, 60, 50, 0xFFD60AFFu, 40);
                int out = !pt_near(cpu, 15, 100, BG, 8)
                       && !pt_near(gpu, 15, 100, BG, 40);
                char msg[160];
                snprintf(msg, sizeof(msg), "形内=%d/%d 形外=%d 判定=%s/%s/%s",
                         d1, d2, d3,
                         in1 ? "绿ok" : "绿MISMATCH",
                         in2 ? "黄ok" : "黄MISMATCH",
                         out ? "底ok" : "底MISMATCH");
                ck(d1 <= 40 && d2 <= 40 && d3 <= 40 && in1 && in2 && out,
                   "ppath pattern 多边形目标(掩码)", msg);
            } else {
                ck(0, "ppath pattern 多边形目标(掩码)", "渲染失败");
            }
            free(gpu);
            free(cpu);
        } else {
            ck(0, "ppath pattern 多边形目标(掩码)", "available==0(应支持)");
        }
    }

    /* ---- ⑩ CPU 对照(信息值): 同页 metal vs hnsoft 每帧耗时 ----
       基线页(既有)之外追混合页与 pattern 页(v2 新消费路径的对照)。 */
    {
        /* 混合页: 大盒 multiply 两层(重叠区大, 混合逐像素工作量可观) */
        hn_cmd bcmds[3];
        hn_display_list bm;
        memset(bcmds, 0, sizeof(bcmds));
        bcmds[0].kind = HN_CMD_RECT;
        bcmds[0].x = 0; bcmds[0].y = 0; bcmds[0].w = BW; bcmds[0].h = BH;
        bcmds[0].fill = 0x30D5C8FF;
        bcmds[1].kind = HN_CMD_RECT;
        bcmds[1].x = 0; bcmds[1].y = 0; bcmds[1].w = BW; bcmds[1].h = BH;
        bcmds[1].fill = 0xD94848C0u;
        bcmds[1].blend = HN_BLEND_MULTIPLY;
        bcmds[2].kind = HN_CMD_RECT;
        bcmds[2].x = 30; bcmds[2].y = 20; bcmds[2].w = BW - 60; bcmds[2].h = BH - 40;
        bcmds[2].fill = 0x3D74FF80u;
        bcmds[2].blend = HN_BLEND_SCREEN;
        bm.cmds = bcmds;
        bm.count = 3;
        perf_cmds("混合页(全幅 multiply+screen)", &bm);

        /* pattern 页(repeat 平铺 200×120) */
        {
            char quad_path[1024];
            if (png_dir) snprintf(quad_path, sizeof(quad_path), "%s/gp64quad.png", png_dir);
            else snprintf(quad_path, sizeof(quad_path), "/tmp/hn_gpu_probe_gp64quad.png");
            hn_cmd pc;
            hn_display_list pm;
            memset(&pc, 0, sizeof(pc));
            pc.kind = HN_CMD_PATTERN;
            pc.text = quad_path;
            pc.pattern_rep = HN_PAT_REPEAT;
            pc.pat_ox = 0;
            pc.pat_oy = 0;
            pc.x = 10; pc.y = 10; pc.w = 200; pc.h = 120;
            pm.cmds = &pc;
            pm.count = 1;
            perf_cmds("pattern 页(repeat 平铺)", &pm);
        }

        /* cpath 页(掩码栅格每帧重算 —— 成本口径的信息值) */
        {
            float tri[6] = { 30, 20, 210, 60, 60, 140 };
            hn_cmd cc[4];
            hn_display_list cm;
            memset(cc, 0, sizeof(cc));
            cc[0].kind = HN_CMD_RECT;
            cc[0].x = 10; cc[0].y = 10; cc[0].w = 220; cc[0].h = 140;
            cc[0].fill = 0x30D5C8FF;
            cc[1].kind = HN_CMD_CLIP_PATH_PUSH;
            cc[1].poly = tri;
            cc[1].poly_n = 3;
            cc[2].kind = HN_CMD_RECT;
            cc[2].x = 10; cc[2].y = 10; cc[2].w = 220; cc[2].h = 140;
            cc[2].fill = 0xFFD60AFF;
            cc[3].kind = HN_CMD_CLIP_POP;
            cm.cmds = cc;
            cm.count = 4;
            perf_cmds("cpath 页(掩码栅格)", &cm);
        }

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

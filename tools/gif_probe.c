/* gif_probe.c — GIF 动图全链路验收(纯 C99, 离屏, 零 GUI 依赖)
 *
 * 管线与生产同一条: 内嵌夹具字节 → hn_gif_decode(帧序列合成) →
 * <img src="x.gif"> 经 hn_context 布局/绘制 → BITMAP 指令(帧像素) →
 * hn_context_anim_tick 推时钟 → 帧号变化/循环回绕。
 *
 * 夹具(手写字节数组内嵌, 见下; 生成器为纯 Python LZW 编码器, 不依赖 PIL):
 *   GIF_DISP1  4x4 两帧, disposal=1(保留), 每帧 delay 100/200ms;
 *   GIF_DISP2  4x4 三帧, disposal=2(恢复背景色)+1, 覆盖帧矩形叠加与恢复;
 *   GIF_TRANS  4x4 两帧, 首帧透明索引 + 次帧全不透明, 验透明索引与画布起点;
 *   GIF_LOOP3  4x4 两帧, NETSCAPE 循环 3 次(非无限), 验循环计数解析。
 *
 * 断言分四层:
 *   1. 解码: 帧数/尺寸/色表索引→RGBA/disposal 合成结果/透明索引/delay;
 *   2. 播放: hn_gif_frame_index 按 delay 累计 —— 跨过 delay 帧号变化,
 *      总时长回绕, 有限循环播完停在末帧;
 *   3. 引擎接线: 文档里 <img src="x.gif"> 出 BITMAP(几何=元素盒,
 *      像素随 HN_CLOCK 对应的 ext_clock 变); .gif 后缀与 hn-gif 显式属性
 *      两条挂载路径等价;
 *   4. 反例: 非 GIF 字节解码失败返回 NULL; 单帧 GIF 也走同一路径不崩。
 *
 * 用法(由 tools/gif_probe.py 薄封装编译运行; 手工编译见该脚本头部注释)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn.h"
#include "hn_internal.h"
#include "hn_gif.h"
#include "../Sources/CHtmlNative/hnsoft.h"

static int g_pass = 0, g_fail = 0;

static void check(int cond, const char *label) {
    printf("%s %s\n", cond ? "  ✔" : "  ✘", label);
    if (cond) g_pass++; else g_fail++;
}

static void check_int(long got, long want, const char *label) {
    int ok = got == want;
    printf("%s %s (got %ld want %ld)\n", ok ? "  ✔" : "  ✘", label, got, want);
    if (ok) g_pass++; else g_fail++;
}

static void check_f(float got, float want, const char *label) {
    int ok = got > want - 0.51f && got < want + 0.51f;
    printf("%s %s (got %.1f want %.1f)\n", ok ? "  ✔" : "  ✘", label, got, want);
    if (ok) g_pass++; else g_fail++;
}

/* ---------------- 内嵌夹具 ----------------
 * 生成方式: 纯 Python LZW 编码器(GIF89a 标准编码路径, 不依赖 PIL),
 * 生成脚本原样保留在 tools/gif_probe.py 头部 —— 夹具字节在此直嵌,
 * 探针自包含(无生成步骤 = 无环境依赖, 任何机器上字节都一致)。
 * 字节流与 PIL 的解码结果逐像素对照过(含 disposal 合成与透明索引)。 */
/* 4x4 两帧 disposal=1(保留), delay 100/200ms(119 字节) */
static const unsigned char GIF_DISP1[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x04, 0x00, 0x04, 0x00, 0x81, 0x00, 0x00, 0xff, 0x00, 0x00,
    0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0xff, 0x00, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53,
    0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x04,
    0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x04, 0x00, 0x81, 0xff, 0x00,
    0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0xff, 0x00, 0x02, 0x04, 0x84, 0x8f, 0x09, 0x05,
    0x00, 0x21, 0xf9, 0x04, 0x04, 0x14, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
    0x04, 0x00, 0x81, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0xff, 0x00, 0x02,
    0x04, 0x8c, 0x8f, 0x19, 0x05, 0x00, 0x3b,
};

/* 4x4 三帧: 全幅红 disposal=2 + 两个 2x2 子矩形(152 字节) */
static const unsigned char GIF_DISP2[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x04, 0x00, 0x04, 0x00, 0x81, 0x02, 0x00, 0xff, 0x00, 0x00,
    0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0xff, 0x00, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53,
    0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x08,
    0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x04, 0x00, 0x81, 0xff, 0x00,
    0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0xff, 0x00, 0x02, 0x04, 0x84, 0x8f, 0x09, 0x05,
    0x00, 0x21, 0xf9, 0x04, 0x04, 0x05, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x02, 0x00,
    0x02, 0x00, 0x81, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0xff, 0x00, 0x02,
    0x02, 0x8c, 0x53, 0x00, 0x21, 0xf9, 0x04, 0x04, 0x07, 0x00, 0x00, 0x00, 0x2c, 0x02, 0x00, 0x02,
    0x00, 0x02, 0x00, 0x02, 0x00, 0x81, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff,
    0xff, 0x00, 0x02, 0x02, 0x8c, 0x53, 0x00, 0x3b,
};

/* 4x4 两帧, 首帧透明索引=2(121 字节) */
static const unsigned char GIF_TRANS[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x04, 0x00, 0x04, 0x00, 0x81, 0x02, 0x00, 0xff, 0x00, 0x00,
    0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0xff, 0x00, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53,
    0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x05,
    0x0a, 0x00, 0x02, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x04, 0x00, 0x81, 0xff, 0x00,
    0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0xff, 0x00, 0x02, 0x06, 0x04, 0x24, 0x86, 0xa8,
    0x09, 0x05, 0x00, 0x21, 0xf9, 0x04, 0x04, 0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00,
    0x04, 0x00, 0x04, 0x00, 0x81, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0xff,
    0x00, 0x02, 0x04, 0x8c, 0x8f, 0x19, 0x05, 0x00, 0x3b,
};

/* 4x4 两帧(delay 300/100ms), NETSCAPE 循环 3 次(119 字节) */
static const unsigned char GIF_LOOP3[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x04, 0x00, 0x04, 0x00, 0x81, 0x00, 0x00, 0xff, 0x00, 0x00,
    0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0xff, 0x00, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53,
    0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x03, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x04,
    0x1e, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x04, 0x00, 0x81, 0xff, 0x00,
    0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0xff, 0x00, 0x02, 0x04, 0x84, 0x8f, 0x09, 0x05,
    0x00, 0x21, 0xf9, 0x04, 0x04, 0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
    0x04, 0x00, 0x81, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0xff, 0x00, 0x02,
    0x04, 0x8c, 0x8f, 0x19, 0x05, 0x00, 0x3b,
};

/* 4x4 单帧(等价静态图)(82 字节) */
static const unsigned char GIF_SINGLE[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x04, 0x00, 0x04, 0x00, 0x81, 0x00, 0x00, 0xff, 0x00, 0x00,
    0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0xff, 0x00, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53,
    0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x04,
    0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x04, 0x00, 0x81, 0xff, 0x00,
    0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0xff, 0x00, 0x02, 0x04, 0x84, 0x8f, 0x09, 0x05,
    0x00, 0x3b,
};

/* ---------------- 1. 解码层 ---------------- */

static void probe_decode(void) {
    printf("== 1. 解码 ==\n");

    /* disposal=1 两帧 */
    hn_gif *g = hn_gif_decode(GIF_DISP1, sizeof(GIF_DISP1));
    check(g != NULL, "DISP1 解码成功");
    if (!g) return;
    check_int(hn_gif_width(g), 4, "DISP1 宽");
    check_int(hn_gif_height(g), 4, "DISP1 高");
    check_int(hn_gif_frame_count(g), 2, "DISP1 帧数");
    check_f(hn_gif_frame_delay_ms(g, 0), 100.0f, "DISP1 帧0 delay(10cs)");
    check_f(hn_gif_frame_delay_ms(g, 1), 200.0f, "DISP1 帧1 delay(20cs)");
    const unsigned char *f0 = hn_gif_frame_rgba(g, 0);
    const unsigned char *f1 = hn_gif_frame_rgba(g, 1);
    check(f0 && f1, "DISP1 帧像素非空");
    /* 帧0 = 全红(索引0), 帧1 = 全蓝(索引1) */
    check(f0[0] == 255 && f0[1] == 0 && f0[2] == 0 && f0[3] == 255, "DISP1 帧0 像素=红");
    check(f1[0] == 0 && f1[1] == 0 && f1[2] == 255 && f1[3] == 255, "DISP1 帧1 像素=蓝");
    /* 两帧像素不同(帧差的基础) */
    check(memcmp(f0, f1, 4 * 4 * 4) != 0, "DISP1 帧0≠帧1(逐字节)");
    hn_gif_free(g);

    /* disposal=2: 帧1 只画左上 2x2 蓝, 帧2 前把帧1 区域恢复为背景(绿),
       再画右下 2x2 蓝 —— 帧2 的左上 2x2 必须回到背景色而不是残留蓝。 */
    g = hn_gif_decode(GIF_DISP2, sizeof(GIF_DISP2));
    check(g != NULL, "DISP2 解码成功");
    if (!g) return;
    check_int(hn_gif_frame_count(g), 3, "DISP2 帧数");
    check_f(hn_gif_frame_delay_ms(g, 0), 100.0f, "DISP2 帧0 delay");
    check_f(hn_gif_frame_delay_ms(g, 1), 50.0f, "DISP2 帧1 delay(5cs)");
    check_f(hn_gif_frame_delay_ms(g, 2), 70.0f, "DISP2 帧2 delay(7cs)");
    const unsigned char *d1 = hn_gif_frame_rgba(g, 1);
    const unsigned char *d2 = hn_gif_frame_rgba(g, 2);
    check(d1 && d2, "DISP2 帧像素非空");
    /* 合成口径(帧0 全幅红 disposal=2 → 帧1 前整幅恢复为背景绿):
       帧1 = 左上 2x2 蓝 + 其余背景绿; 帧2 = 保留帧1 + 右下 2x2 蓝。 */
    check(d1[0] == 0 && d1[2] == 255, "DISP2 帧1 左上=蓝(子矩形叠加)");
    check(d1[(2 * 4 + 2) * 4] == 0 && d1[(2 * 4 + 2) * 4 + 1] == 255,
          "DISP2 帧1 右下=背景绿(disposal=2 把帧0 整幅恢复)");
    check(d2[0] == 0 && d2[2] == 255, "DISP2 帧2 左上=蓝(帧1 保留, disposal=1)");
    check(d2[(2 * 4 + 2) * 4] == 0 && d2[(2 * 4 + 2) * 4 + 2] == 255, "DISP2 帧2 右下=蓝(新子矩形)");
    check(d2[(3 * 4 + 0) * 4 + 1] == 255, "DISP2 帧2 左下=背景绿(未被覆盖)");
    check(memcmp(hn_gif_frame_rgba(g, 0), d2, 4 * 4 * 4) != 0, "DISP2 帧0≠帧2");
    hn_gif_free(g);

    /* 透明索引 */
    g = hn_gif_decode(GIF_TRANS, sizeof(GIF_TRANS));
    check(g != NULL, "TRANS 解码成功");
    if (!g) return;
    check_int(hn_gif_frame_transparent(g, 0), 2, "TRANS 帧0 透明索引=2");
    check_int(hn_gif_frame_transparent(g, 1), -1, "TRANS 帧1 无透明");
    const unsigned char *t0 = hn_gif_frame_rgba(g, 0);
    /* 索引 2 的像素 alpha 必须为 0(透明), 索引 0 的不透明 */
    check(t0[3] == 255, "TRANS 帧0 索引0 不透明");
    check(t0[2 * 4 + 3] == 0, "TRANS 帧0 索引2 alpha=0(透明索引生效)");
    check(memcmp(hn_gif_frame_rgba(g, 0), hn_gif_frame_rgba(g, 1), 4 * 4 * 4) != 0,
          "TRANS 帧0≠帧1");
    hn_gif_free(g);

    /* NETSCAPE 循环次数: 3 次(非 0 无限) */
    g = hn_gif_decode(GIF_LOOP3, sizeof(GIF_LOOP3));
    check(g != NULL, "LOOP3 解码成功");
    if (g) {
        check_int(hn_gif_loop_count(g), 3, "LOOP3 循环次数=3(非无限)");
        /* 总时长 400ms(delay 300+100), 播 3 遍 = 1200ms;
           599ms 在第二遍中段(599-400=199 → 帧0), 1199ms 仍在播(帧1),
           1200ms 播完停在末帧。 */
        check_int(hn_gif_frame_index(g, 599), 0, "LOOP3 t=599 仍在循环(第二遍帧0)");
        check_int(hn_gif_frame_index(g, 1199), 1, "LOOP3 t=1199 循环内(帧1)");
        check_int(hn_gif_frame_index(g, 1200), 1, "LOOP3 t=1200 播完停在末帧");
        check_int(hn_gif_frame_index(g, 5000), 1, "LOOP3 远超 span 仍停末帧");
        hn_gif_free(g);
    }

    /* 反例: 非 GIF 字节 */
    const unsigned char junk[32] = { 0x89, 'P', 'N', 'G' };
    check(hn_gif_decode(junk, sizeof(junk)) == NULL, "非 GIF 字节解码返回 NULL");
    check(hn_gif_decode(NULL, 0) == NULL, "空输入返回 NULL");
}

/* ---------------- 2. 播放层(取帧是时钟的纯函数) ---------------- */

static void probe_timing(void) {
    printf("== 2. 播放(时钟→帧号) ==\n");
    hn_gif *g = hn_gif_decode(GIF_DISP1, sizeof(GIF_DISP1));   /* delay 100+200=300ms */
    if (!g) { check(0, "DISP1 二次解码"); return; }
    check_int(hn_gif_frame_index(g, 0), 0, "t=0 → 帧0");
    check_int(hn_gif_frame_index(g, 50), 0, "t=50 → 帧0(delay 未到)");
    check_int(hn_gif_frame_index(g, 99), 0, "t=99 → 帧0");
    check_int(hn_gif_frame_index(g, 100), 1, "t=100 跨帧0 delay → 帧1");
    check_int(hn_gif_frame_index(g, 299), 1, "t=299 → 帧1");
    check_int(hn_gif_frame_index(g, 300), 0, "t=300 回绕 → 帧0(循环)");
    check_int(hn_gif_frame_index(g, 400), 1, "t=400 → 帧1(第二遍)");
    check_int(hn_gif_frame_index(g, 900), 0, "t=900 三整轮 → 帧0");
    hn_gif_free(g);
}

/* ---------------- 3. 引擎接线 ---------------- */

static char g_src[256];
static size_t g_src_len;

static const char *fake_asset_load(void *ctx, const char *path, size_t *len) {
    (void)ctx;
    if (!strcmp(path, "x.gif")) {
        if (len) *len = sizeof(GIF_DISP1);
        return (const char *)GIF_DISP1;
    }
    if (!strcmp(path, "y.gif")) {
        if (len) *len = sizeof(GIF_DISP2);
        return (const char *)GIF_DISP2;
    }
    return NULL;
}

static int paint_has_bitmap(hn_context *c, const char **path_out) {
    const hn_display_list *dl = hn_context_display_list(c);
    if (!dl) return 0;
    for (int i = 0; i < dl->count; i++) {
        if (dl->cmds[i].kind == HN_CMD_BITMAP) {
            if (path_out) *path_out = NULL;
            return 1;
        }
    }
    return 0;
}

static const hn_cmd *paint_find_bitmap(hn_context *c) {
    const hn_display_list *dl = hn_context_display_list(c);
    if (!dl) return NULL;
    for (int i = 0; i < dl->count; i++)
        if (dl->cmds[i].kind == HN_CMD_BITMAP) return &dl->cmds[i];
    return NULL;
}

static void probe_engine(void) {
    printf("== 3. 引擎接线(<img src=x.gif> → BITMAP) ==\n");
    const char *html =
        "<html><head><style>html,body{margin:0}</style></head><body>"
        "<img id=g src=\"x.gif\" style=\"width:40;height:40\">"
        "<img id=h src=\"y.gif\" hn-gif style=\"width:40;height:40\">"
        "</body></html>";
    hn_doc *doc = hn_parse_html(html, strlen(html));
    check(doc != NULL, "文档解析成功");
    if (!doc) return;
    hn_context *c = hn_context_create();
    hn_context_set_doc(c, doc);
    hn_asset_backend ab;
    memset(&ab, 0, sizeof(ab));
    ab.load = fake_asset_load;
    hn_context_set_assets(c, &ab);
    hn_context_layout(c, 200, 200, NULL);

    const hn_cmd *bm = paint_find_bitmap(c);
    check(bm != NULL, "GIF 元素出 BITMAP 指令");
    if (bm) {
        check_int(bm->bitmap_w, 4, "BITMAP 源宽=4(解码尺寸)");
        check_int(bm->bitmap_h, 4, "BITMAP 源高=4");
        check(bm->bitmap_stride == 16, "BITMAP stride=w*4");
        check(bm->w > 39.0f && bm->w < 41.0f && bm->h > 39.0f && bm->h < 41.0f,
              "BITMAP 几何=元素盒 40x40");
        check(bm->bitmap && bm->bitmap[0] == 255 && bm->bitmap[2] == 0,
              "BITMAP 首像素=红(帧0)");
    }
    /* 两张图都出 BITMAP(.gif 后缀 + hn-gif 显式属性两条挂载路径) */
    int nbm = 0;
    {
        const hn_display_list *dl = hn_context_display_list(c);
        if (dl) for (int i = 0; i < dl->count; i++)
            if (dl->cmds[i].kind == HN_CMD_BITMAP) nbm++;
    }
    check_int(nbm, 2, "两条挂载路径(.gif 后缀 / hn-gif 属性)都出 BITMAP");

    /* 时钟推进: 帧号变化 → 像素变化 */
    const unsigned char *px0 = NULL;
    bm = paint_find_bitmap(c);
    if (bm) px0 = bm->bitmap;
    /* 300ms = 一轮回绕后帧0; 取 250ms → 帧1(蓝) */
    for (float t = 0; t < 250.0f; t += 25.0f) hn_context_anim_tick(c, 25.0f);
    hn_context_repaint(c);
    const hn_cmd *bm2 = paint_find_bitmap(c);
    check(bm2 != NULL, "推时钟后仍出 BITMAP");
    if (bm2 && px0) {
        check(bm2->bitmap[0] == 0 && bm2->bitmap[2] == 255, "250ms 后像素=蓝(帧1)");
        check(memcmp(px0, bm2->bitmap, 4 * 4 * 4) != 0, "推时钟后像素与首帧不同");
    }
    /* 循环回绕: 再 +350ms → 累计 600ms = 两整轮, 回到帧0 */
    for (float t = 0; t < 350.0f; t += 25.0f) hn_context_anim_tick(c, 25.0f);
    hn_context_repaint(c);
    const hn_cmd *bm3 = paint_find_bitmap(c);
    if (bm3) check(bm3->bitmap[0] == 255 && bm3->bitmap[2] == 0,
                   "累计 600ms(两整轮)回绕到帧0(红)");

    /* 帧循环活跃: 动画在播时 anim_tick 返回活跃(帧驱动不熄火) */
    int active = hn_context_anim_tick(c, 16.0f);
    check(active == 1, "GIF 在播 → anim_tick 报告活跃");
    hn_context_destroy(c);
}

/* ---------------- 4. 无后端降级 ---------------- */

static void probe_no_asset(void) {
    printf("== 4. 无资产后端(GIF 未解出 → 不退化成崩溃) ==\n");
    const char *html =
        "<html><body><img src=\"nope.gif\" style=\"width:20;height:20\"></body></html>";
    hn_doc *doc = hn_parse_html(html, strlen(html));
    hn_context *c = hn_context_create();
    hn_context_set_doc(c, doc);
    hn_context_layout(c, 100, 100, NULL);
    int ok = (paint_find_bitmap(c) == NULL);
    check(ok, "无资产后端时不出 BITMAP(走常规 img 路径)");
    hn_context_destroy(c);

    /* 单帧 GIF: 同一路径可播(等价静态图), 但不算"在动" */
    hn_gif *g = hn_gif_decode(GIF_SINGLE, sizeof(GIF_SINGLE));
    check(g != NULL, "单帧 GIF 解码成功");
    if (g) {
        check_int(hn_gif_frame_count(g), 1, "单帧 GIF 帧数=1");
        check_int(hn_gif_frame_index(g, 12345), 0, "单帧恒为帧0(无回绕)");
        hn_gif_free(g);
    }
}

int main(void) {
    printf("gif_probe — GIF 动图全链路\n");
    probe_decode();
    probe_timing();
    probe_engine();
    probe_no_asset();
    printf("\n结果: %d 通过, %d 失败\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

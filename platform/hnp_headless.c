/* hnp_headless.c — 无头平台实现(零 GUI, CI/服务器/agent 无显示环境)
 *
 * 这是 hn_platform.h 的**最小实现**, 也是抽象层的完备性检验:
 * 一个平台只需要实现这十几个函数。真平台(macOS/Linux/Windows)在此之上
 * 只多出各自的窗口/事件真实来源。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "hn_platform.h"

static unsigned long g_now_offset;

int hnp_init(int argc, char **argv) { (void)argc; (void)argv; return 1; }
void hnp_shutdown(void) {}
const char *hnp_name(void) { return "headless"; }

struct hnp_window {
    int w, h, transparent;
    unsigned char *last_blit;   /* 最后一张(shot/诊断可读) */
    char title[256];
};

hnp_window *hnp_window_open(const hnp_window_desc *d) {
    if (!d) return NULL;
    hnp_window *w = (hnp_window *)calloc(1, sizeof(*w));
    if (!w) return NULL;
    w->w = d->width; w->h = d->height; w->transparent = d->transparent;
    snprintf(w->title, sizeof(w->title), "%s", d->title ? d->title : "hn");
    /* always_on_top/no_shadow/click_through/join_all_spaces/borderless:
       桌宠形态字段在无头平台优雅忽略(没有窗口表面可物化; 字段存在是为了
       调用方三平台同源, 见 hn_platform.h)。 */
    return w;
}

void hnp_window_close(hnp_window *w) {
    if (!w) return;
    free(w->last_blit);
    free(w);
}

void hnp_window_resize(hnp_window *w, int nw, int nh) {
    if (!w) return;
    w->w = nw; w->h = nh;
    free(w->last_blit);
    w->last_blit = NULL;
}

void hnp_window_blit(hnp_window *w, const unsigned char *bgra_pre, int bw, int bh, size_t stride) {
    if (!w || !bgra_pre) return;
    free(w->last_blit);
    w->last_blit = (unsigned char *)malloc((size_t)bw * bh * 4);
    if (!w->last_blit) return;
    for (int y = 0; y < bh; y++)
        memcpy(w->last_blit + (size_t)y * bw * 4, bgra_pre + y * stride, (size_t)bw * 4);
    (void)w;
}

void hnp_window_invalidate(hnp_window *w) { (void)w; }
/* GPU 直呈层: 无表面平台不支持, 恒 NULL(壳回落 blit 路径)。 */
void *hnp_window_attach_metal_layer(hnp_window *w, int phys_w, int phys_h) {
    (void)w; (void)phys_w; (void)phys_h; return NULL;
}
void hnp_window_metal_layer_resize(hnp_window *w, int phys_w, int phys_h) {
    (void)w; (void)phys_w; (void)phys_h;
}
void hnp_window_set_title(hnp_window *w, const char *utf8) {
    if (!w) return;
    snprintf(w->title, sizeof(w->title), "%s", utf8 ? utf8 : "hn");
}

/* 运行期鼠标穿透: 无头平台没有窗口表面, 声明照常接受(调用方不必分平台),
   物化语义不存在 —— 空实现 + 注释, 与 always_on_top 等 desc 字段同一口径。 */
void hnp_window_set_click_through(hnp_window *w, int on) { (void)w; (void)on; }

/* 几何记忆采样口: 无头平台没有屏幕坐标可读, 返回 0 —— 调用方(hn_app)
   据此整体跳过几何轮询/落盘, 与 desc.x/y 的"优雅忽略"同一口径。 */
int hnp_window_origin(hnp_window *w, int *x, int *y) {
    (void)w; (void)x; (void)y;
    return 0;
}

/* 窗口移动: 无头平台没有窗口表面, 空实现(与 origin 的"拿不到"同一口径:
   夹取链在 origin 返回 0 时整体跳过, 到不了这里)。 */
void hnp_window_move(hnp_window *w, int x, int y) { (void)w; (void)x; (void)y; }

/* 可见区查询: 无头平台无屏, 返回 0 —— 调用方跳过几何夹取(声明照常通过;
   与 hnp_window_scale 恒 1.0 同一"无屏"口径)。 */
int hnp_screen_workarea(hnp_window *w, int px, int py,
                        int *vx, int *vy, int *vw, int *vh) {
    (void)w; (void)px; (void)py; (void)vx; (void)vy; (void)vw; (void)vh;
    return 0;
}

/* 设备像素比: 无头平台没有屏, 恒 1.0(离屏/shot 的 1x 确定性口径不破;
   HiDPI 只在真窗平台物化, 见 hn_platform.h 的 hnp_window_scale 注释)。 */
float hnp_window_scale(hnp_window *w) {
    (void)w;
    return 1.0f;
}

/* IME/剪贴板: 无头平台无输入法与剪贴板服务 —— 空实现(声明照常通过)。
   **无 IME 事件产出**: hnp_pump 恒不返回 HNP_EV_IME_* / SCALE_CHANGED /
   FOCUS_* / SCREEN_CHANGED(契约见 hn_platform.h 的枚举注释), 引擎侧
   永不进入预编辑态, 行为与无 IME 的旧版逐位一致。 */
void hnp_ime_focus(hnp_window *w, int x, int y, int iw, int ih) {
    (void)w; (void)x; (void)y; (void)iw; (void)ih;
}
int hnp_clipboard_get(hnp_window *w, char *out, size_t cap) {
    (void)w; (void)out; (void)cap;
    return 0;
}
int hnp_clipboard_set(hnp_window *w, const char *utf8) {
    (void)w; (void)utf8;
    return 0;
}

/* 键码映射: 无头平台无原生键码, 恒 NONE(契约见 hn_platform.h)。 */
hnp_key hnp_key_from_native(unsigned long native_code) {
    (void)native_code;
    return HNP_KEY_NONE;
}

int hnp_pump(hnp_event *ev, int timeout_ms) {
    if (!ev) return 0;
    ev->kind = HNP_EV_NONE;
    if (timeout_ms > 0) {
        struct timespec ts = { timeout_ms / 1000, (long)(timeout_ms % 1000) * 1000000L };
        nanosleep(&ts, NULL);
    }
    return 0;
}

unsigned long hnp_now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (unsigned long)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000) + g_now_offset;
}

double hnp_now_ms_f(void) {
    /* 亚毫秒单调钟(纳秒精度): 帧时间戳先量化到 1/64ms 栅格再做差
       (handwired host-timing 同法); g_now_offset 与 hnp_now_ms 同一口径。 */
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0 + g_now_offset;
}

void hnp_set_cursor(hnp_window *w, hnp_cursor c) { (void)w; (void)c; }

/* ---------------- 媒体 ABI 留位(docs/media-design.md §2) ----------------
   无头平台不带媒体实现: supported()=0, 引擎/桥据此整体降级(不播, 不炸)。
   其余 hnp_media_* 有意不提供 —— 调用方必须先看 supported(), 因此
   hncore/headless 路径对媒体零符号依赖(不走 hn_platform.h 里的
   static inline 兜底: 那会把真实实现静默遮蔽成 0, 属于坑)。 */
int hnp_media_supported(void) { return 0; }

/* ---------------- GPU 渲染后端(v1 无 GPU 路径) ----------------
   headless 无 GPU 原语表: 返回 NULL —— 呼出方(引擎经 hn_rt_set_gpu_host
   注入槽)据此整体回退 hnsoft(与 hnp_media_supported()==0 的 ABI 留位
   同一口径; docs/platforms.md 的实现矩阵里 headless = 无)。 */
const hnp_gpu_host *hnp_gpu_backend(void) { return NULL; }

/* 原生视频图层: 无头平台没有窗口表面可挂(GPU 合成无从谈起), 空实现 ——
   attach 返回 0 = 不支持, app 侧保持 CPU 逐帧路径(与媒体 ABI 的
   "声明照常通过, 物化看平台"同一口径)。 */
int hnp_video_layer_attach(hnp_window *w, const char *player_id,
                           float x, float y, float fw, float fh) {
    (void)w; (void)player_id; (void)x; (void)y; (void)fw; (void)fh;
    return 0;
}
void hnp_video_layer_frame(hnp_window *w, const char *player_id,
                           float x, float y, float fw, float fh) {
    (void)w; (void)player_id; (void)x; (void)y; (void)fw; (void)fh;
}
void hnp_video_layer_detach(hnp_window *w, const char *player_id) {
    (void)w; (void)player_id;
}

/* Smart-UX 出现/消失动效(headless 无窗: 空实现) */
void hnp_window_reveal_begin(hnp_window *w) { (void)w; }
int hnp_window_reveal_active(hnp_window *w) { (void)w; return 0; }
void hnp_window_close_faded(hnp_window *w) { hnp_window_close(w); }
int hnp_window_close_fade_step(int dt_ms) { (void)dt_ms; return 0; }

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
void hnp_window_set_title(hnp_window *w, const char *utf8) {
    if (!w) return;
    snprintf(w->title, sizeof(w->title), "%s", utf8 ? utf8 : "hn");
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

void hnp_set_cursor(hnp_window *w, hnp_cursor c) { (void)w; (void)c; }

/* ---------------- 媒体 ABI 留位(docs/media-design.md §2) ----------------
   无头平台不带媒体实现: supported()=0, 引擎/桥据此整体降级(不播, 不炸)。
   其余 hnp_media_* 有意不提供 —— 调用方必须先看 supported(), 因此
   hncore/headless 路径对媒体零符号依赖(不走 hn_platform.h 里的
   static inline 兜底: 那会把真实实现静默遮蔽成 0, 属于坑)。 */
int hnp_media_supported(void) { return 0; }

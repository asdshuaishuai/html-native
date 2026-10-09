/* hn_media.c — 媒体元素引擎侧: 状态机 + 会话表 + 帧缓存
 *
 * 平台无关: 只认 include/hn.h 的 hn_media_host 函数指针表, 不 include 任何
 * 平台头文件(与图片/资产后端同一依赖倒置)。解码/出声全部在宿主侧
 * (macOS = AVFoundation, 测试 = hncore 合成宿主), 引擎只维护:
 *   - 六态状态机(docs/media-design.md §3: IDLE/LOADING/READY/PLAYING/
 *     PAUSED/ENDED, 仿 HTMLMediaElement 子集);
 *   - 每元素一条会话(首次 layout/paint 触达时建, 文档替换时全销毁);
 *   - 当前帧的 RGBA 缓存(双缓冲, 供 BITMAP 绘制指令引用)。
 *
 * 时钟所有权(契约钉死): cur_time 的唯一真源是宿主 position(); 引擎不用
 * dt 自累计(避免与真解码漂移), 每帧把 dt 转交 tick() 供无钟宿主累计,
 * 随后读 position() 写回。frame_poll 拿到有效 pts 时同样写回 —— 它是
 * 当前显示帧的时间戳, 与 position 同源(合成宿主两者相等)。
 *
 * 明确不做(契约 §9): <source> 子元素、字幕、DRM、流媒体(http(s) 交由
 * 宿主 open 失败)、浏览器控件 UI、poster、playbackRate。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn_internal.h"

/* strdup 是 POSIX 而非 C99 —— 与 hn_context.c 同一份本地实现 */
static char *dup_str(const char *s) {
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

void hn_context_set_media(hn_context *c, const hn_media_host *host) {
    if (!c) return;
    /* 换宿主 = 旧会话的句柄全部失效, 先销毁(与换资产后端清 Lottie 同位) */
    hn_media_sessions_clear(c);
    c->media = host;
}

/* ---- 会话表 ---- */

static hn_media_session *sess_find(hn_context *c, hn_node *n) {
    for (int i = 0; i < c->n_med; i++)
        if (c->med_sessions[i].node == n) return &c->med_sessions[i];
    return NULL;
}

/* 布尔属性: 存在且值不为 "false"/"0" 即为真(与 hn_context.c flag_attr 同口径) */
static int flag_attr(hn_node *n, const char *name) {
    const char *v = hn_node_attr(n, name);
    if (!v) return 0;
    return strcmp(v, "false") != 0 && strcmp(v, "0") != 0;
}

/* IDLE→LOADING: 调宿主 open。失败回 IDLE 并 stderr 一行(契约状态机)。 */
static void sess_open(hn_context *c, hn_media_session *s) {
    char err[192] = { 0 };
    s->handle = c->media->open(c->media->ctx, s->src, err, sizeof(err));
    if (!s->handle) {
        s->state = HN_MEDIA_IDLE;
        s->want_play = 0;
        fprintf(stderr, "[media] open 失败: %s%s%s\n", s->src,
                err[0] ? ": " : "", err[0] ? err : "");
        return;
    }
    s->state = HN_MEDIA_LOADING;
    s->duration = -1;
    /* 声明属性在 open 时落到宿主(音量/静音是宿主侧状态; loop 是引擎侧语义) */
    if (c->media->set_volume) c->media->set_volume(c->media->ctx, s->handle, s->volume);
    if (c->media->set_muted)  c->media->set_muted(c->media->ctx, s->handle, s->muted);
}

/* 首次触达建会话; 无宿主/无 src 则 NULL(整套媒体代码路径零激活)。
   autoplay 属性 → open 即刻发生(IDLE→LOADING)。 */
static hn_media_session *sess_get(hn_context *c, hn_node *n) {
    if (!c || !c->media || !n || n->kind != HN_ELEM || !n->tag) return NULL;
    if (strcmp(n->tag, "video") && strcmp(n->tag, "audio")) return NULL;
    hn_media_session *s = sess_find(c, n);
    if (s) return s;
    const char *src = hn_node_attr(n, "src");
    if (!src || !*src) return NULL;          /* 无 src 不加载(与 HTML 一致) */
    if (c->n_med == c->cap_med) {
        int nc = c->cap_med ? c->cap_med * 2 : 8;
        hn_media_session *ns = (hn_media_session *)realloc(
            c->med_sessions, sizeof(hn_media_session) * (size_t)nc);
        if (!ns) return NULL;
        c->med_sessions = ns;
        c->cap_med = nc;
    }
    s = &c->med_sessions[c->n_med++];
    memset(s, 0, sizeof(*s));
    s->node = n;
    s->src = dup_str(src);
    if (!s->src) { c->n_med--; return NULL; }
    s->state = HN_MEDIA_IDLE;
    s->duration = -1;
    s->volume = 1.0f;                        /* HTML 缺省音量 */
    const char *v = hn_node_attr(n, "volume");
    if (v && *v) {
        s->volume = (float)atof(v);
        if (s->volume < 0) s->volume = 0;
        if (s->volume > 1) s->volume = 1;
    }
    s->muted = flag_attr(n, "muted");
    s->loop = flag_attr(n, "loop");
    if (flag_attr(n, "autoplay")) {
        s->want_play = 1;
        sess_open(c, s);
    }
    return s;
}

void hn_media_sessions_clear(hn_context *c) {
    if (!c) return;
    for (int i = 0; i < c->n_med; i++) {
        hn_media_session *s = &c->med_sessions[i];
        if (c->media && c->media->close && s->handle)
            c->media->close(c->media->ctx, s->handle);
        free(s->src);
        free(s->frame);
        free(s->frame_back);
    }
    c->n_med = 0;
}

/* ---- 帧循环驱动(hn_context_anim_tick 内部调用) ---- */

int hn_media_tick_ret(hn_context *c, float dt_ms) {
    if (!c || !c->media) return 0;
    int active = 0;
    for (int i = 0; i < c->n_med; i++) {
        hn_media_session *s = &c->med_sessions[i];
        if (s->state == HN_MEDIA_PLAYING ||
            (s->state == HN_MEDIA_LOADING && s->want_play))
            active = 1;                      /* 保持宿主帧循环 */
        if (!s->handle) continue;
        /* dt 转交宿主: 无钟宿主(合成宿主)据此累计假时钟; 真宿主忽略 */
        if (c->media->tick) c->media->tick(c->media->ctx, s->handle, dt_ms);
        if (s->state == HN_MEDIA_LOADING) {
            /* LOADING→READY: 宿主确认元数据就绪后读 duration(可能触发重排) */
            int ready = 0, ended = 0;
            if (c->media->state &&
                c->media->state(c->media->ctx, s->handle, &ready, &ended) && ready) {
                double d = -1;
                if (c->media->duration &&
                    c->media->duration(c->media->ctx, s->handle, &d) && d > 0)
                    s->duration = d;
                else
                    s->duration = -1;    /* 直播流/未知 */
                s->state = HN_MEDIA_READY;
                if (s->want_play && c->media->play &&
                    c->media->play(c->media->ctx, s->handle))
                    s->state = HN_MEDIA_PLAYING;
            }
        }
        if (s->state == HN_MEDIA_PLAYING) {
            /* cur_time 唯一真源 = 宿主 position(契约钉死, 不自累计) */
            double pos = -1;
            if (c->media->position &&
                c->media->position(c->media->ctx, s->handle, &pos) == 1 && pos >= 0)
                s->cur_time = pos;
            /* ended 按 position≥duration 推导(真宿主的 rate 在播完时不可靠) */
            if (s->duration > 0 && s->cur_time >= s->duration - 1e-3) {
                if (s->loop) {
                    if (c->media->seek && c->media->seek(c->media->ctx, s->handle, 0))
                        s->cur_time = 0;     /* loop 回绕: 保持 PLAYING */
                } else {
                    if (c->media->pause) c->media->pause(c->media->ctx, s->handle);
                    s->state = HN_MEDIA_ENDED;
                    s->cur_time = s->duration;
                }
            }
        }
    }
    return active;
}

void hn_media_tick(hn_context *c, float dt_ms) {
    (void)hn_media_tick_ret(c, dt_ms);
}

/* ---- 元素寻址的控制 API(JS 桥与探针共用) ---- */

int hn_media_play(hn_context *c, hn_node *n) {
    hn_media_session *s = sess_get(c, n);    /* 首次 play 也建会话 */
    if (!s || !c->media) return 0;
    s->want_play = 1;
    switch (s->state) {
    case HN_MEDIA_IDLE:
        sess_open(c, s);
        return s->state != HN_MEDIA_IDLE;    /* LOADING = 进入播放管道 */
    case HN_MEDIA_LOADING:
        return 1;                            /* 就绪后自动起播(want_play) */
    case HN_MEDIA_READY:
    case HN_MEDIA_PAUSED:
        if (c->media->play && c->media->play(c->media->ctx, s->handle)) {
            s->state = HN_MEDIA_PLAYING;
            return 1;
        }
        return 0;
    case HN_MEDIA_ENDED:
        /* play() 重新起播: 先回 0 再 play */
        if (c->media->seek) c->media->seek(c->media->ctx, s->handle, 0);
        s->cur_time = 0;
        if (c->media->play && c->media->play(c->media->ctx, s->handle)) {
            s->state = HN_MEDIA_PLAYING;
            return 1;
        }
        return 0;
    case HN_MEDIA_PLAYING:
        return 1;                            /* 已在播放(保持) */
    }
    return 0;
}

int hn_media_pause(hn_context *c, hn_node *n) {
    hn_media_session *s = sess_find(c, n);
    if (!s) return 0;
    s->want_play = 0;                        /* 撤销起播意图(含 LOADING 中) */
    if (s->state == HN_MEDIA_PLAYING) {
        if (c->media && c->media->pause) c->media->pause(c->media->ctx, s->handle);
        s->state = HN_MEDIA_PAUSED;
    }
    return 1;
}

int hn_media_seek(hn_context *c, hn_node *n, double sec) {
    hn_media_session *s = sess_find(c, n);
    if (!s || !s->handle || !c->media || !c->media->seek) return 0;
    /* 钳制到 [0, duration](已知时) */
    if (sec < 0) sec = 0;
    if (s->duration > 0 && sec > s->duration) sec = s->duration;
    if (!c->media->seek(c->media->ctx, s->handle, sec)) return 0;
    s->cur_time = sec;
    if (s->state == HN_MEDIA_ENDED) s->state = HN_MEDIA_PAUSED;
    return 1;
}

double hn_media_time(hn_context *c, hn_node *n) {
    hn_media_session *s = c && n ? sess_find(c, n) : NULL;
    return s ? s->cur_time : 0;
}

double hn_media_duration(hn_context *c, hn_node *n) {
    hn_media_session *s = c && n ? sess_find(c, n) : NULL;
    return s ? s->duration : -1;
}

void hn_media_set_volume(hn_context *c, hn_node *n, float v) {
    hn_media_session *s = sess_get(c, n);    /* 未播先设音量也成立 */
    if (!s) return;
    if (v < 0) v = 0;
    if (v > 1) v = 1;
    s->volume = v;
    if (s->handle && c->media->set_volume)
        c->media->set_volume(c->media->ctx, s->handle, v);
}

void hn_media_set_muted(hn_context *c, hn_node *n, int muted) {
    hn_media_session *s = sess_get(c, n);
    if (!s) return;
    s->muted = muted ? 1 : 0;
    if (s->handle && c->media->set_muted)
        c->media->set_muted(c->media->ctx, s->handle, s->muted);
}

int hn_media_state(hn_context *c, hn_node *n) {
    hn_media_session *s = c && n ? sess_find(c, n) : NULL;
    return s ? (int)s->state : (int)HN_MEDIA_IDLE;
}

/* ---- 布局钩子: 固有尺寸 ---- */

void hn_media_intrinsic(hn_context *c, hn_node *n, int *w, int *h) {
    *w = *h = -1;
    hn_media_session *s = sess_get(c, n);    /* 首次 layout 触达建会话 */
    if (!s || !s->has_frame) return;
    if (s->frame_w > 0 && s->frame_h > 0) { *w = s->frame_w; *h = s->frame_h; }
}

/* ---- 绘制钩子: 拉当前帧(双缓冲) ---- */

/* 两个缓冲按帧尺寸对齐(含显示侧 —— 换尺寸后旧显示帧几何失效, 一并重分配) */
static int frames_resize(hn_media_session *s, int w, int h) {
    size_t need = (size_t)w * (size_t)h * 4;
    unsigned char *nb = (unsigned char *)realloc(s->frame_back, need);
    if (!nb) return 0;
    s->frame_back = nb;
    unsigned char *nf = (unsigned char *)realloc(s->frame, need);
    if (!nf) return 0;
    s->frame = nf;
    return 1;
}

int hn_media_poll_frame(hn_context *c, hn_node *n, const unsigned char **rgba,
                        int *w, int *h, int *stride, double *pts) {
    *rgba = NULL; *w = *h = *stride = 0; *pts = 0;
    hn_media_session *s = sess_get(c, n);    /* 首次 paint 触达建会话 */
    if (!s || !s->handle || !c->media->frame) return 0;
    if (s->state == HN_MEDIA_IDLE || s->state == HN_MEDIA_LOADING) return 0;

    int fw = 0, fh = 0;
    double fpts = -1;
    size_t cap = s->frame_back ? (size_t)s->frame_w * (size_t)s->frame_h * 4 : 0;
    int r = c->media->frame(c->media->ctx, s->handle, s->frame_back, cap,
                            &fw, &fh, &fpts);
    if (r < 0 && fw > 0 && fh > 0 && (fw != s->frame_w || fh != s->frame_h)) {
        /* cap 不足: 宿主已写出 meta 尺寸 —— 重分配后重试一次(契约口径) */
        if (!frames_resize(s, fw, fh)) return 0;
        s->frame_w = fw;
        s->frame_h = fh;
        cap = (size_t)fw * (size_t)fh * 4;
        fpts = -1;
        r = c->media->frame(c->media->ctx, s->handle, s->frame_back, cap,
                            &fw, &fh, &fpts);
    }
    if (r == 1) {
        if (fw <= 0 || fh <= 0 || (size_t)fw * (size_t)fh * 4 > cap)
            return s->has_frame;   /* 宿主汇报尺寸与缓冲不符: 弃用本帧(宿主 bug) */
        /* 新帧: 交换双缓冲。旧显示帧变为写入侧, 下次 repaint 前无人覆盖它。 */
        unsigned char *tmp = s->frame;
        s->frame = s->frame_back;
        s->frame_back = tmp;
        s->frame_w = fw;
        s->frame_h = fh;
        s->frame_stride = fw * 4;
        s->frame_pts = fpts;
        s->has_frame = 1;
        /* currentTime 由 frame_poll 的 pts 驱动(与 position 同源; 纯音频
           没有帧, 只走 tick 里的 position 回写) */
        if (fpts >= 0) s->cur_time = fpts;
    }
    if (!s->has_frame) return 0;             /* 无视频轨/还没拉到首帧 → 占位 */
    *rgba = s->frame;
    *w = s->frame_w;
    *h = s->frame_h;
    *stride = s->frame_stride;
    *pts = s->frame_pts;
    return 1;
}

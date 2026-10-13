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
    /* 声明属性在 open 时落到宿主(音量/静音是宿主侧状态; loop 既是引擎侧
       状态机语义, 也经显式 set_loop ABI 落宿主 —— 平台宿主按 loop=1 在到尾
       通知直接回绕, 不再靠"到尾后 1s 内 seek(0)"的闩锁推断)。表项可缺
       (旧宿主), 缺则跳过。 */
    if (c->media->set_volume) c->media->set_volume(c->media->ctx, s->handle, s->volume);
    if (c->media->set_muted)  c->media->set_muted(c->media->ctx, s->handle, s->muted);
    if (c->media->set_loop)   c->media->set_loop(c->media->ctx, s->handle, s->loop);
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
        /* 事故③修复挂点: rt_render 的 anim_tick(0) 是零步进重采样, 不代表
           时间前进 —— dt<=0 一律不做宿主调用(此前每帧把 position/状态查询
           打满 120 次/秒, 每次还泵一遍主 runloop)。状态分类(active)照算,
           它只读会话自身字段。 */
        if (dt_ms <= 0) continue;
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
                c->media->position(c->media->ctx, s->handle, &pos) == 1 && pos >= 0) {
                /* 回绕 seek 落位窗口: seekToTime 是异步的, 落位前 position
                   仍读到旧值(≈duration)。此时不覆盖 cur_time(保持回绕写入
                   的 0), 也不再看齐 duration —— 否则下一拍又触发回绕, 每
                   16ms 重发一次 seek(seek 风暴), 偶发把 AVPlayer 落成停住
                   (实证: 30s 真窗 trace 第三次回绕后 t=0.000 连刷 41 行)。 */
                int wrap_inflight = (s->duration > 0 && pos >= s->duration - 1e-3 &&
                                     s->cur_time < 1e-3);
                if (!wrap_inflight) s->cur_time = pos;
            }
            /* ended 按 position≥duration 推导(真宿主的 rate 在播完时不可靠) */
            if (s->duration > 0 && s->cur_time >= s->duration - 1e-3) {
                if (s->loop) {
                    if (c->media->seek && c->media->seek(c->media->ctx, s->handle, 0)) {
                        s->cur_time = 0;     /* loop 回绕: 保持 PLAYING */
                        /* 事故③实证: AVPlayer 走到结尾后只 seek 不 play,
                           position 永久停在 0(真窗 trace t=0.000 连刷 8 秒+),
                           引擎却按 PLAYING 驱动 60fps 帧循环 —— 显式补 play
                           让解码真正续上。 */
                        if (c->media->play) c->media->play(c->media->ctx, s->handle);
                    }
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

/* ---- poll 降频(事故③: HN_MEDIA_FPS, 缺省 30, 环境钮可调 1..120) ----
   渲染节拍在媒体播放时恒 16ms(60fps), 逐帧 poll 会把"宿主泵 + 像素转换"
   打满 60 次/秒, 而样片帧率普遍 ≤30。引擎平台无关不摸墙钟, 用渲染拍计数
   降频: 每 N 次 poll 放行一次(N = 60/FPS, 向上取整) —— 60fps 渲染拍下
   即 FPS 上限; 渲染更慢(如 ambient 33ms)时只少不多, 方向安全。
   全局一个计数器(预算口径): 多视频页共享同一预算, 总转换成本封顶。 */
static int media_fps_divisor(void) {
    static char memo[24] = "\x01";      /* 首次必不相等 → 走一次换算 */
    static int div = 2;                 /* 缺省 30fps ÷ 60fps 渲染拍 = 隔次 */
    const char *e = getenv("HN_MEDIA_FPS");
    const char *cur = (e && *e) ? e : "";
    if (strcmp(cur, memo) != 0) {
        snprintf(memo, sizeof(memo), "%s", cur);
        long fps = atol(cur);
        if (fps <= 0) fps = 30;
        if (fps > 120) fps = 120;
        div = (int)(60 / fps);
        if (div < 1) div = 1;
    }
    return div;
}

int hn_media_poll_frame(hn_context *c, hn_node *n, const unsigned char **rgba,
                        int *w, int *h, int *stride, double *pts) {
    *rgba = NULL; *w = *h = *stride = 0; *pts = 0;
    hn_media_session *s = sess_get(c, n);    /* 首次 paint 触达建会话 */
    if (!s || !s->handle || !c->media->frame) return 0;
    if (s->state == HN_MEDIA_IDLE || s->state == HN_MEDIA_LOADING) return 0;

    /* 事故③修复挂点: PAUSED/ENDED 画面静止 —— 不再向宿主要帧(此前暂停态
       仍每帧泵 runloop + 查 hasNew), 既有缓存帧照常显示。 */
    static int poll_tick;
    /* 降频只咬"已有帧"的稳态轮询; 首帧必达(单遍 paint 的探针/无窗管线
       依赖它, 多视频同帧各自出 BITMAP 的确定性也靠它)。 */
    int host_poll = (s->state == HN_MEDIA_PLAYING) &&
                    (!s->has_frame || (poll_tick++ % media_fps_divisor()) == 0);
    int fw = 0, fh = 0;
    double fpts = -1;
    int r = 0;
    size_t cap = s->frame_back ? (size_t)s->frame_w * (size_t)s->frame_h * 4 : 0;
    if (host_poll) {
        r = c->media->frame(c->media->ctx, s->handle, s->frame_back, cap,
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

/* ---- 媒体-only 轻渲染判据(hn_rt_frame 用; 契约同 ambient 轻路径) ----
   返回 1 = 有会话在播(帧循环必须保持)且几何已随全路径落位(在播 video 的
   pts 驱动 cur_time>0 = 首帧已过全路径 relayout, 固有尺寸变化已被兜住)。
   此时帧推进只换像素不改样式/几何, 免整遍 relayout(参照 ambient)。 */
int hn_media_light_render_ok(hn_context *c) {
    if (!c || !c->media) return 0;
    int active = 0;
    for (int i = 0; i < c->n_med; i++) {
        hn_media_session *s = &c->med_sessions[i];
        if (s->state == HN_MEDIA_PLAYING ||
            (s->state == HN_MEDIA_LOADING && s->want_play)) {
            active = 1;
            if (s->cur_time <= 0) return 0;   /* 首帧几何还没落位: 全路径 */
        }
    }
    return active;
}

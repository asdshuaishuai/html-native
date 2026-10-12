/* hn_media_platform.c — app 壳的媒体宿主装配: 平台真宿主 / 合成替身
 *
 * 缺口(本轮补齐): 引擎状态机(hn_media.c)与平台后端(platform/hnp_media_*.c)
 * 早已就位, 但没有任何壳把 hnp_media_* 适配成 hn_media_host 装进 hn_rt ——
 * 真窗口里 <video> 永不播。本文件就是那一层适配:
 *
 *   "platform" 平台真宿主: hnp_media_supported()==1 时(macOS = AVFoundation,
 *     运行期 dlopen)全部回调直转 hnp_media_*。AVPlayer 负责解码 + 音频出声;
 *     视频帧由 hnp_media_frame 的 NV12→RGBA 转换交出来, 引擎经既有
 *     HN_CMD_BITMAP 指令路径画进 <video> 元素盒(与合成宿主同一消费点)。
 *   "synth" 合成替身: 平台无媒体实现(supported()==0, headless/Linux 交叉
 *     目标)时的回退 —— 梯度帧 + 假时钟, 与 tools/hncore.c 的 g_synth_media
 *     同一语义(逐条对齐, 契约 §8.1)。元素照常解析/布局/出图(确定性假帧),
 *     只是不出真声音、不解真码。这样三平台产物同源可跑, 探针确定性不破。
 *
 * 编译门(构建脚本按目标给; 见 tools/build-multiplatform.sh 的 hnapp 段):
 *   -DHN_MEDIA_PLATFORM_AVFOUNDATION —— 仅 hnapp-macos-arm64(唯一链接
 *   platform/hnp_media_macos.c 的 hnapp 目标)。**不开这个宏就不许引用任何
 *   hnp_media_* 符号**: headless/Linux 目标没有它们的定义, zig/ld 对弱
 *   引用按强引用处理, 直接链接失败(本仓库工具链实测)。
 *
 * 降级语义(逐档, 设计决定, media_window_probe.sh 钉住观测点):
 *   平台宿主在场但单个文件 open 失败(http(s)/路径不存在/dlopen 失败)
 *     → 引擎会话回 IDLE 并 stderr 一行 "[media] open 失败: …";
 *        <video> 保留深底占位 RECT(hn_paint.c 的占位分支), 不静默空白、不崩。
 *   平台宿主整个缺席 → synth 宿主(假帧), 不是"空宿主"。
 *   宿主表未装配(如 daemon 路径 / hnweb-* 壳) → 引擎 c->media==NULL:
 *        元素照常解析、布局走缺省尺寸、绘制走占位、JS 桥静默 no-op(契约 §6)。
 *
 * 环境钮(仅本壳; 引擎不读环境变量 —— 与 hncore 合成宿主的确定性开关同一纪律):
 *   HN_MEDIA_MUTE=1            绑定后全部媒体置静音(探针跑不吵人)
 *   HN_MEDIA_UNMUTE=1|all      绑定后全部取消静音(+音量 1; 对手工试听)
 *   HN_MEDIA_UNMUTE=id[,id…]   仅列出的元素取消静音
 *   HN_MEDIA_TRACE=1           真窗口每 ~500ms 打一行会话状态(验收观测点)
 *   HN_MEDIA_DURATION=s        synth 宿主全局时长覆盖(与 hncore 同名同义)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hn_media_platform.h"
#include "hn_platform.h"   /* hnp_media_* ABI(仅 macro 构建下真正引用) */

/* ====================================================================
 * 合成替身(平台无媒体实现时的回退宿主)
 * ==================================================================== */

typedef struct {
    char   src[256];   /* open 时的 src(惰性解析 data-duration 用) */
    double dur;        /* 时长(秒); <=0 = 未解析 */
    double t;          /* 假时钟(秒): tick 是唯一时间来源 */
    int    playing;
    float  vol;
    int    muted;
    double last_pts;   /* frame() 判"新帧": pts 未变 → 0(内容未变) */
} synth_media;

#define SYNTH_FW 64    /* 梯度帧尺寸(与 hncore 合成宿主同口径) */
#define SYNTH_FH 64

/* bind 记下的运行时: synth 的 src→data-duration 表按需从这里查。 */
static hn_rt *g_bound_rt;

/* 绑定门: false 时 synth 报告"元数据未就绪"(state ready=0 / duration 未知)。
   为什么需要: 引擎的 LOADING→READY 在**首次布局期的 anim_tick(0)** 就读
   duration(hn_media.c), 而那发生在 hn_rt_open 内部 —— 即 bind(拿到 rt,
   才能按 src 反查 data-duration)之前。报告未就绪把这次读推迟到 bind 后的
   首个帧节拍, 时长按元素声明解析, 与 hncore 的权威口径一致(zero 引擎改动)。 */
static int g_bound;

static int    g_env_ready;
static double g_env_dur = -1;      /* HN_MEDIA_DURATION(>0 时覆盖全部) */

static void env_load(void) {
    if (g_env_ready) return;
    const char *d = getenv("HN_MEDIA_DURATION");
    g_env_dur = (d && *d) ? atof(d) : -1;
    g_env_ready = 1;
}

/* 布尔属性口径: 存在且不为 "false"/"0" 即为真(与引擎 flag_attr 一致) */
static int attr_flag(const char *v) {
    return v && strcmp(v, "false") != 0 && strcmp(v, "0") != 0;
}

static hn_node *find_media_by_src(hn_node *n, const char *src) {
    if (!n) return NULL;
    const char *tag = hn_node_tag(n);
    if (tag && (!strcmp(tag, "video") || !strcmp(tag, "audio"))) {
        const char *s = hn_node_attr(n, "src");
        if (s && !strcmp(s, src)) return n;
    }
    for (hn_node *c = hn_node_first_child(n); c; c = hn_node_next_sibling(c)) {
        hn_node *hit = find_media_by_src(c, src);
        if (hit) return hit;
    }
    return NULL;
}

/* 时长惰性解析(仅在 g_bound 后调用): 环境钮 > 元素 data-duration > 2.0 */
static double synth_resolve_dur(synth_media *s) {
    if (s->dur > 0) return s->dur;
    env_load();
    if (g_env_dur > 0) { s->dur = g_env_dur; return s->dur; }
    hn_node *n = NULL;
    if (g_bound_rt) {
        hn_doc *doc = hn_rt_doc(g_bound_rt);
        n = doc ? find_media_by_src(hn_doc_root(doc), s->src) : NULL;
    }
    const char *dd = n ? hn_node_attr(n, "data-duration") : NULL;
    s->dur = (dd && *dd) ? atof(dd) : 2.0;
    if (s->dur <= 0) s->dur = 2.0;
    return s->dur;
}
static void *synth_open(void *ctx, const char *url, char *err, size_t err_cap) {
    (void)ctx;
    if (!url || !*url) {
        if (err && err_cap) snprintf(err, err_cap, "空 src");
        return NULL;
    }
    synth_media *m = (synth_media *)calloc(1, sizeof(*m));
    if (!m) { if (err && err_cap) snprintf(err, err_cap, "oom"); return NULL; }
    snprintf(m->src, sizeof(m->src), "%s", url);
    m->dur = -1;          /* 未解析: 首次 duration()/state() 时查表 */
    m->vol = 1.0f;
    return m;
}

static void synth_close(void *ctx, void *m) {
    (void)ctx;
    free(m);   /* 幂等: free(NULL) 安全 */
}

static void synth_tick(void *ctx, void *m, float dt_ms) {
    (void)ctx;
    synth_media *s = (synth_media *)m;
    if (!s || !s->playing) return;
    synth_resolve_dur(s);
    s->t += dt_ms / 1000.0;
    if (s->dur > 0 && s->t > s->dur) s->t = s->dur;   /* 停在末尾, ended 交引擎推导 */
}

static int synth_play(void *ctx, void *m) {
    (void)ctx;
    if (!m) return 0;
    ((synth_media *)m)->playing = 1;
    return 1;
}

static void synth_pause(void *ctx, void *m) {
    (void)ctx;
    if (m) ((synth_media *)m)->playing = 0;
}

static int synth_seek(void *ctx, void *m, double sec) {
    (void)ctx;
    synth_media *s = (synth_media *)m;
    if (!s) return 0;
    synth_resolve_dur(s);
    if (sec < 0) sec = 0;
    if (s->dur > 0 && sec > s->dur) sec = s->dur;     /* 钳制(真宿主同款) */
    s->t = sec;
    return 1;
}

static void synth_set_volume(void *ctx, void *m, float vol) {
    (void)ctx;
    synth_media *s = (synth_media *)m;
    if (!s) return;
    if (!(vol >= 0)) vol = 0;
    if (vol > 1) vol = 1;
    s->vol = vol;
}

static void synth_set_muted(void *ctx, void *m, int muted) {
    (void)ctx;
    if (m) ((synth_media *)m)->muted = muted ? 1 : 0;
}

static int synth_duration(void *ctx, void *m, double *sec) {
    (void)ctx;
    synth_media *s = (synth_media *)m;
    if (!s) return 0;
    if (!g_bound) {              /* bind 前: 未知(见 g_bound 注释) */
        if (sec) *sec = -1.0;
        return 0;
    }
    if (sec) *sec = synth_resolve_dur(s);
    return 1;
}

static int synth_position(void *ctx, void *m, double *sec) {
    (void)ctx;
    synth_media *s = (synth_media *)m;
    if (!s) return 0;
    if (sec) *sec = s->t;
    return 1;
}

static int synth_state(void *ctx, void *m, int *ready, int *ended) {
    (void)ctx;
    synth_media *s = (synth_media *)m;
    if (!s) return 0;
    if (!g_bound) {              /* bind 前: 元数据未就绪(见 g_bound 注释) */
        if (ready) *ready = 0;
        if (ended) *ended = 0;
        return 1;
    }
    synth_resolve_dur(s);
    if (ready) *ready = 1;                            /* 元数据瞬时就绪 */
    if (ended) *ended = (s->dur > 0 && s->t >= s->dur - 1e-3);
    return 1;
}

/* 梯度帧(64x64): R=x*255/w, G=y*255/h, B=((int)(pts*10)*37)%256 ——
   像素既验证几何又验证 pts 前进, 探针逐字节可断言(契约 §8.1)。
   cap 不足时照真宿主契约写出实际尺寸并返回 -1(引擎按 meta 重分配后重试)。 */
static int synth_frame(void *ctx, void *m, unsigned char *rgba, size_t cap,
                       int *w, int *h, double *pts_sec) {
    (void)ctx;
    synth_media *s = (synth_media *)m;
    if (!s) return -1;
    double p = synth_resolve_dur(s) > 0 && s->t > s->dur ? s->dur : s->t;
    if (w) *w = SYNTH_FW;
    if (h) *h = SYNTH_FH;
    if (pts_sec) *pts_sec = p;
    size_t need = (size_t)SYNTH_FW * SYNTH_FH * 4;
    if (!rgba || cap < need) return -1;
    if (p == s->last_pts) return 0;                   /* 内容未变(暂停在同一位置) */
    int bucket = (int)(p * 10.0);
    unsigned char b = (unsigned char)((bucket * 37) % 256);
    for (int y = 0; y < SYNTH_FH; y++)
        for (int x = 0; x < SYNTH_FW; x++) {
            unsigned char *px = rgba + ((size_t)y * SYNTH_FW + x) * 4;
            px[0] = (unsigned char)(x * 255 / SYNTH_FW);
            px[1] = (unsigned char)(y * 255 / SYNTH_FH);
            px[2] = b;
            px[3] = 255;
        }
    s->last_pts = p;
    return 1;
}

static const hn_media_host g_synth_host = {
    NULL,
    synth_open, synth_close, synth_tick,
    synth_play, synth_pause, synth_seek,
    synth_set_volume, synth_set_muted,
    synth_duration, synth_position, synth_state,
    synth_frame
};

/* ====================================================================
 * 平台真宿主(macOS AVFoundation; 仅 -DHN_MEDIA_PLATFORM_AVFOUNDATION 编入)
 * ==================================================================== */
#ifdef HN_MEDIA_PLATFORM_AVFOUNDATION

static void *pf_open(void *ctx, const char *url, char *err, size_t err_cap) {
    (void)ctx;
    return hnp_media_open(url, err, err_cap);
}
static void pf_close(void *ctx, void *m) { (void)ctx; hnp_media_close((hnp_media *)m); }

/* tick 对真宿主是 no-op: AVPlayer 自带时钟(position 是唯一真源, 契约 §3),
   引擎转交的 dt 只服务合成宿主。函数照给(表形统一), 显式注释不做事。 */
static void pf_tick(void *ctx, void *m, float dt_ms) { (void)ctx; (void)m; (void)dt_ms; }

static int  pf_play(void *ctx, void *m) { (void)ctx; return hnp_media_play((hnp_media *)m); }
static void pf_pause(void *ctx, void *m) { (void)ctx; hnp_media_pause((hnp_media *)m); }
static int  pf_seek(void *ctx, void *m, double sec) { (void)ctx; return hnp_media_seek((hnp_media *)m, sec); }
static void pf_set_volume(void *ctx, void *m, float vol) { (void)ctx; hnp_media_set_volume((hnp_media *)m, vol); }
static void pf_set_muted(void *ctx, void *m, int muted) { (void)ctx; hnp_media_set_muted((hnp_media *)m, muted); }
static int  pf_duration(void *ctx, void *m, double *sec) { (void)ctx; return hnp_media_duration((hnp_media *)m, sec); }
static int  pf_position(void *ctx, void *m, double *sec) { (void)ctx; return hnp_media_position((hnp_media *)m, sec); }
static int  pf_state(void *ctx, void *m, int *ready, int *ended) { (void)ctx; return hnp_media_state((hnp_media *)m, ready, ended); }
static int  pf_frame(void *ctx, void *m, unsigned char *rgba, size_t cap,
                     int *w, int *h, double *pts_sec) {
    (void)ctx;
    return hnp_media_frame((hnp_media *)m, rgba, cap, w, h, pts_sec);
}

static const hn_media_host g_platform_host = {
    NULL,
    pf_open, pf_close, pf_tick,
    pf_play, pf_pause, pf_seek,
    pf_set_volume, pf_set_muted,
    pf_duration, pf_position, pf_state,
    pf_frame
};

#endif /* HN_MEDIA_PLATFORM_AVFOUNDATION */

/* ====================================================================
 * 宿主选择 / 绑定 / 静音开关 / 状态跟踪
 * ==================================================================== */

static const hn_media_host *g_chosen;

const hn_media_host *hn_media_platform_host(void) {
    if (!g_chosen) {
        const hn_media_host *h = &g_synth_host;
#ifdef HN_MEDIA_PLATFORM_AVFOUNDATION
        /* 不在 supported() 里猜能播: dlopen 失败由 open 的 err 如实报告
           (与 hnp_media_macos.c 的 supported 口径一致)。这里只决定"装哪张表"。 */
        if (hnp_media_supported()) h = &g_platform_host;
#endif
        g_chosen = h;
    }
    return g_chosen;
}

const char *hn_media_platform_kind(void) {
    return hn_media_platform_host() == &g_synth_host ? "synth" : "platform";
}

/* ---- 媒体元素遍历: 静音/音量施加与状态行共用 ---- */

static hn_node *first_media_with_src(hn_node *n) {
    if (!n) return NULL;
    const char *tag = hn_node_tag(n);
    if (tag && (!strcmp(tag, "video") || !strcmp(tag, "audio"))) {
        const char *s = hn_node_attr(n, "src");
        if (s && *s) return n;
    }
    for (hn_node *c = hn_node_first_child(n); c; c = hn_node_next_sibling(c)) {
        hn_node *hit = first_media_with_src(c);
        if (hit) return hit;
    }
    return NULL;
}

static int id_in_list(const char *id, const char *list) {
    if (!id || !list) return 0;
    size_t il = strlen(id);
    const char *p = list;
    while (*p) {
        const char *e = strchr(p, ',');
        size_t n = e ? (size_t)(e - p) : strlen(p);
        if (n == il && memcmp(p, id, n) == 0) return 1;
        if (!e) break;
        p = e + 1;
    }
    return 0;
}

/* 对全部(或 ids 列出的)video/audio 元素施加 muted(+可选音量 1)。
   muted 走 hn_media_set_muted(元素寻址, 无会话时建会话记声明, 有会话落宿主);
   vol1 只在 HN_MEDIA_UNMUTE 场景(与 hnMediaVolume(id,1) 桥语义对齐)。 */
static int media_apply_walk(hn_node *n, hn_context *c, int muted, int vol1,
                            const char *ids) {
    int cnt = 0;
    if (!n) return 0;
    const char *tag = hn_node_tag(n);
    if (tag && (!strcmp(tag, "video") || !strcmp(tag, "audio"))) {
        const char *src = hn_node_attr(n, "src");
        if (src && *src && (!ids || id_in_list(hn_node_attr(n, "id"), ids))) {
            hn_media_set_muted(c, n, muted);
            if (vol1) hn_media_set_volume(c, n, 1.0f);
            cnt++;
        }
    }
    for (hn_node *ch = hn_node_first_child(n); ch; ch = hn_node_next_sibling(ch))
        cnt += media_apply_walk(ch, c, muted, vol1, ids);
    return cnt;
}

/* 媒体状态跟踪全局 */
static int g_mute_state = -1;   /* -1 = 未初始化(以首个元素的 muted 声明为准) */

int hn_media_platform_toggle_mute(hn_rt *rt) {
    if (!rt) return 0;
    hn_context *c = hn_rt_context(rt);
    hn_doc *doc = hn_rt_doc(rt);
    if (!c || !doc) return 0;
    hn_node *root = hn_doc_root(doc);
    if (!first_media_with_src(root)) return 0;   /* 无媒体元素: 交还按键 */
    if (g_mute_state < 0) {
        hn_node *f = first_media_with_src(root);
        g_mute_state = attr_flag(f ? hn_node_attr(f, "muted") : NULL) ? 1 : 0;
    }
    g_mute_state = !g_mute_state;
    int n = media_apply_walk(root, c, g_mute_state, 0, NULL);
    printf("[media] m 键静音切换 → muted=%d (%d 个媒体元素)\n", g_mute_state, n);
    fflush(stdout);
    return 1;
}

void hn_media_platform_bind(hn_rt *rt) {
    g_bound_rt = rt;      /* synth 的时长表来源(见 g_bound_rt 注释) */
    g_bound = 1;          /* 元数据就绪门开(此前 synth 报告未就绪, 见 g_bound) */
    env_load();
    if (!rt) return;
    hn_context *c = hn_rt_context(rt);
    hn_doc *doc = hn_rt_doc(rt);
    if (!c || !doc) return;
    /* 环境钮(逐项, UNMUTE 优先于 MUTE —— 显式试听意图压过探针静音)。
       无媒体元素时静默(n 为 0 不打行, 免在非媒体页刷噪音)。 */
    const char *mu = getenv("HN_MEDIA_UNMUTE");
    const char *mm = getenv("HN_MEDIA_MUTE");
    if (mu && *mu) {
        int n;
        if (!strcmp(mu, "1") || !strcmp(mu, "all")) {
            n = media_apply_walk(hn_doc_root(doc), c, 0, 1, NULL);
            g_mute_state = 0;
        } else {
            n = media_apply_walk(hn_doc_root(doc), c, 0, 1, mu);
        }
        if (n > 0) {
            printf("[media] HN_MEDIA_UNMUTE=%s → 取消静音 %d 个元素\n", mu, n);
            fflush(stdout);
        }
    } else if (mm && *mm && strcmp(mm, "0") != 0) {
        int n = media_apply_walk(hn_doc_root(doc), c, 1, 0, NULL);
        g_mute_state = 1;
        if (n > 0) {
            printf("[media] HN_MEDIA_MUTE=1 → 静音 %d 个元素\n", n);
            fflush(stdout);
        }
    }
}

/* ---- 会话状态行(HN_MEDIA_TRACE=1; 真窗口与验收脚本的观测点) ---- */

static const char *state_name(int st) {
    static const char *nm[] = { "IDLE", "LOADING", "READY", "PLAYING", "PAUSED", "ENDED" };
    return (st >= 0 && st <= 5) ? nm[st] : "?";
}

static void trace_walk(hn_node *n, hn_context *c) {
    if (!n) return;
    const char *tag = hn_node_tag(n);
    if (tag && (!strcmp(tag, "video") || !strcmp(tag, "audio"))) {
        const char *src = hn_node_attr(n, "src");
        if (src && *src) {
            const char *id = hn_node_attr(n, "id");
            /* app_mute = 本壳静音开关的当前态(m 键/环境钮; -1 = 未动过,
               以元素声明为准)。引擎无 muted 读回口(写入口 ABI), 这是
               验收脚本能观测"静音开关真的动过"的唯一真源。 */
            printf("[media] id=%s tag=%s state=%s t=%.3f dur=%.3f app_mute=%d\n",
                   id ? id : "-", tag, state_name(hn_media_state(c, n)),
                   hn_media_time(c, n), hn_media_duration(c, n),
                   g_mute_state);
        }
    }
    for (hn_node *ch = hn_node_first_child(n); ch; ch = hn_node_next_sibling(ch))
        trace_walk(ch, c);
}

void hn_media_platform_trace(hn_rt *rt) {
    hn_context *c = rt ? hn_rt_context(rt) : NULL;
    hn_doc *doc = rt ? hn_rt_doc(rt) : NULL;
    if (!c || !doc) return;
    trace_walk(hn_doc_root(doc), c);
    fflush(stdout);
}

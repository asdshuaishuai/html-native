/* hn_media_synth.c — 合成媒体宿主(单一来源; 契约 §8.1)
 *
 * 谁在用(两处收敛自各自的旧实现, 行为逐语义保留):
 *   tools/hncore.c       —— 引擎离屏 CLI, paint/render 的 BITMAP/MEDIA 行
 *                           直接消费本宿主(回归锚: media_element_probe.py);
 *   app/hn_media_platform.c —— app 壳的回退宿主(平台真宿主缺席时兜底,
 *                           回归锚: media_window_probe.sh 的 headless 档)。
 *
 * 与真宿主的时序同构: duration() 在 open 之后才可问 —— "打开文件读元数据"
 * 在这里被换成了"按 src 反查文档 data-duration"(惰性, bind 后可用)。
 * bind 门(g_bound)之前 state 报告未就绪、duration 报告未知: 引擎的
 * LOADING→READY 在 hn_rt_open 内部的首个 anim_tick(0) 就读 duration, 而那
 * 发生在 app 壳拿到 rt 之前 —— 关门把这次读推迟到 bind 后的首个帧节拍。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hn_media_synth.h"

#define SYNTH_FW 64    /* 梯度帧尺寸(探针逐字节断言的口径, 勿动) */
#define SYNTH_FH 64
#define SYNTH_OPEN_MAX 64   /* src 回读表容量(与 hncore 旧表同口径) */

typedef struct {
    char   src[256];   /* open 时的 src(惰性解析 data-duration 用) */
    double dur;        /* 时长(秒); <=0 = 未解析 */
    double t;          /* 假时钟(秒): tick 是唯一时间来源 */
    int    playing;
    float  vol;
    int    muted;
    double last_pts;   /* frame() 判"新帧": pts 未变 → 0(内容未变) */
} synth_media;

/* ---- 宿主侧状态 ---- */

static hn_doc *g_bind_doc;   /* src→data-duration 查表基准(bind 时记下) */
static int g_bound;          /* 0 = 元数据未就绪(duration/state 挡在门外) */

/* 打开过的句柄按 src 记下: MEDIA 状态行的 vol/muted 从宿主侧回读,
   验证"元素声明 → 引擎钳制 → 宿主落位"整条链(hncore 消费)。 */
static struct { char src[256]; synth_media *m; } g_open_tab[SYNTH_OPEN_MAX];
static int g_open_n = 0;

static int    g_env_ready;
static double g_env_dur = -1;      /* HN_MEDIA_DURATION(>0 时覆盖全部) */

static void env_load(void) {
    if (g_env_ready) return;
    const char *d = getenv("HN_MEDIA_DURATION");
    g_env_dur = (d && *d) ? atof(d) : -1;
    g_env_ready = 1;
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

/* 时长惰性解析(仅在 g_bound 后被调用方放行): 环境钮 > 元素 data-duration
 * > 2.0。解析结果缓存进 s->dur(后续调用直取)。 */
static double synth_resolve_dur(synth_media *s) {
    if (s->dur > 0) return s->dur;
    env_load();
    if (g_env_dur > 0) { s->dur = g_env_dur; return s->dur; }
    hn_node *n = NULL;
    if (g_bind_doc)
        n = find_media_by_src(hn_doc_root(g_bind_doc), s->src);
    const char *dd = n ? hn_node_attr(n, "data-duration") : NULL;
    s->dur = (dd && *dd) ? atof(dd) : 2.0;
    if (s->dur <= 0) s->dur = 2.0;
    return s->dur;
}

/* ---------------- 宿主回调表(hn_media_host 契约) ---------------- */

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
    if (g_open_n < SYNTH_OPEN_MAX) {
        snprintf(g_open_tab[g_open_n].src, sizeof(g_open_tab[0].src), "%s", url);
        g_open_tab[g_open_n].m = m;
        g_open_n++;
    }
    return m;
}

static void synth_close(void *ctx, void *m) {
    (void)ctx;
    free(m);   /* 幂等: free(NULL) 安全(回读表不摘账: 句柄失联时 query 报缺省) */
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
    if (!g_bound) {              /* bind 前: 未知(见文件头注释) */
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
    if (!g_bound) {              /* bind 前: 元数据未就绪(见文件头注释) */
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

/* ---------------- 公开入口 ---------------- */

const hn_media_host *hn_media_synth_host(void) {
    return &g_synth_host;
}

void hn_media_synth_bind_doc(hn_doc *doc) {
    g_bind_doc = doc;     /* 查表基准(热重载后换新 doc; NULL = 只关门) */
    g_bound = 1;          /* 元数据就绪门开(此前 duration/state 报未就绪) */
    env_load();
}

int hn_media_synth_query(const char *src, float *vol, int *muted) {
    if (!src || !*src) return 0;
    for (int i = 0; i < g_open_n; i++)
        if (!strcmp(g_open_tab[i].src, src)) {
            if (vol) *vol = g_open_tab[i].m ? g_open_tab[i].m->vol : 1.0f;
            if (muted) *muted = g_open_tab[i].m ? g_open_tab[i].m->muted : 0;
            return 1;
        }
    return 0;
}

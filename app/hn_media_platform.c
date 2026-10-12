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
 *     目标)时的回退 —— 梯度帧 + 假时钟, **单一来源在 app/hn_media_synth.c**
 *     (原先是本文件里的第二份实现, 与 tools/hncore.c 的 g_synth_media 重复,
 *     已收敛; 逐语义保留, 契约 §8.1)。元素照常解析/布局/出图(确定性假帧),
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
 * 环境钮(仅本壳; 引擎不读环境变量 —— 与合成宿主的确定性开关同一纪律):
 *   HN_MEDIA_MUTE=1            绑定后全部媒体置静音(探针跑不吵人)
 *   HN_MEDIA_UNMUTE=1|all      绑定后全部取消静音(+音量 1; 对手工试听)
 *   HN_MEDIA_UNMUTE=id[,id…]   仅列出的元素取消静音
 *   HN_MEDIA_TRACE=1           真窗口每 ~500ms 打一行会话状态(验收观测点)
 *   HN_MEDIA_DURATION=s        synth 宿主全局时长覆盖(单一来源在
 *                              hn_media_synth.c, 与 hncore 同名同义)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hn_media_platform.h"
#include "hn_media_synth.h"
#include "hn_platform.h"   /* hnp_media_* ABI(仅 macro 构建下真正引用) */

/* ====================================================================
 * 合成替身: 实现已收敛到 app/hn_media_synth.c(单一来源)。本文件只
 * 在"平台真宿主缺席"时把它的宿主表装进选择器(见 hn_media_platform_host)。
 * ==================================================================== */

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
        const hn_media_host *h = hn_media_synth_host();
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
    return hn_media_platform_host() == hn_media_synth_host() ? "synth" : "platform";
}

/* ---- 媒体元素遍历: 静音/音量施加与状态行共用 ---- */

/* 布尔属性口径: 存在且不为 "false"/"0" 即为真(与引擎 flag_attr 一致) */
static int attr_flag(const char *v) {
    return v && strcmp(v, "false") != 0 && strcmp(v, "0") != 0;
}

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
    /* 合成替身的绑定: 就绪门开 + src→data-duration 查表基准换成本文档
       (门语义/热重载换基准见 hn_media_synth_bind_doc —— bind 前引擎在
       hn_rt_open 内部读 duration, 被挡成"未就绪"推迟到首个帧节拍)。 */
    hn_media_synth_bind_doc(rt ? hn_rt_doc(rt) : NULL);
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

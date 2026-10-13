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

/* hnp_media_macos.c 的会话私有问询(本文件私有声明; 定义见该文件) */
extern int hnp_media_layer_mode(const void *m);
/* 显式 loop ABI(同上: 本文件私有声明, hn_platform.h 冻结不动) */
extern void hnp_media_set_loop(hnp_media *m, int loop);

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
static void pf_set_loop(void *ctx, void *m, int loop) { (void)ctx; hnp_media_set_loop((hnp_media *)m, loop); }
static int  pf_duration(void *ctx, void *m, double *sec) { (void)ctx; return hnp_media_duration((hnp_media *)m, sec); }
static int  pf_position(void *ctx, void *m, double *sec) { (void)ctx; return hnp_media_position((hnp_media *)m, sec); }
static int  pf_state(void *ctx, void *m, int *ready, int *ended) { (void)ctx; return hnp_media_state((hnp_media *)m, ready, ended); }
static int  pf_frame(void *ctx, void *m, unsigned char *rgba, size_t cap,
                     int *w, int *h, double *pts_sec) {
    (void)ctx;
    /* 图层直出的会话(hnp_video_layer_attach 挂过原生 AVPlayerLayer):
       像素由 AVPlayer 解码、系统合成器上屏, CPU 拉帧/NV12→RGBA 转换整链
       跳过 —— 返回 -1 = 无帧, 引擎照常对该元素画占位 RECT(被原生图层
       盖住, 视觉无感; HN_CMD_BITMAP 不再产生, 软件光栅少一遍大拷贝)。 */
    if (hnp_media_layer_mode(m)) return -1;
    return hnp_media_frame((hnp_media *)m, rgba, cap, w, h, pts_sec);
}

static const hn_media_host g_platform_host = {
    NULL,
    pf_open, pf_close, pf_tick,
    pf_play, pf_pause, pf_seek,
    pf_set_volume, pf_set_muted,
    pf_duration, pf_position, pf_state,
    pf_frame,
    pf_set_loop
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

/* ---- 真窗视频图层同步(GPU 合成直通; hn_app 主循环每拍调用) ----
 *
 * 双路径口径(本函数是切换点):
 *   真窗(HN_MEDIA_LAYER != "0", 缺省开)—— 对每个"会话已开"的 <video>:
 *     attach(原生 AVPlayerLayer 挂进窗口, 图层模式置位 → pf_frame 早退,
 *     CPU 拉帧/位图绘制归零) + 每拍 frame 同步元素盒(布局/滚动/resize 跟随)。
 *   离屏(--shot/--advance/--probe)不经本函数(hn_app 只在真窗主循环调)
 *     —— 确定性探针口径不破; HN_MEDIA_LAYER=0 时只摘不挂, 等价退回 CPU 逐帧。
 *
 * 元素盒的**视口坐标**取法: 引擎每次 repaint 都会给无帧 <video> 发占位 RECT
 * (深底 0x14171E + 边框 0x3A4150 是引擎硬编码色, 不走页面样式), 其几何 =
 * 元素盒经绘制变换(祖先滚动偏移/缩放/3D)后的最终视口坐标 —— 比
 * hn_node_box(布局盒, 未减祖先滚动)更准, 页面滚动时图层跟着走全靠它。
 * 按文档序与 <video> 列表 1:1 配对; 配不齐/几何对不上(页面作者恰用了同款
 * 配色等)退回布局盒(滚动跟随降级, 播放不受影响)。 */

#define HN_VLW_CAP 8   /* 同走查/平台图层表的容量口径(会话数超出的元素留 CPU 路径) */

static struct { char src[256]; } g_vl_attached[HN_VLW_CAP];
static int g_vl_attached_n;

static int vl_attached_find(const char *src) {
    for (int i = 0; i < g_vl_attached_n; i++)
        if (strcmp(g_vl_attached[i].src, src) == 0) return i;
    return -1;
}

/* HN_MEDIA_LAYER 开关(环境钮, 记忆化读): 缺省开; "0" = 退回 CPU 逐帧 */
static int video_layers_enabled(void) {
    static char memo[8] = "\x01";
    static int on = 1;
    const char *e = getenv("HN_MEDIA_LAYER");
    const char *cur = e ? e : "";
    if (strcmp(cur, memo) != 0) {
        snprintf(memo, sizeof(memo), "%s", cur);
        on = !(e && strcmp(e, "0") == 0);
    }
    return on;
}

/* 文档序收集"会话已开"(state != IDLE)的 <video src=…>; 返回数量 */
static int vl_collect(hn_node *n, hn_context *c, hn_node **out, const char **srcs, int cap) {
    if (!n) return 0;
    int cnt = 0;
    const char *tag = hn_node_tag(n);
    if (tag && !strcmp(tag, "video")) {
        const char *src = hn_node_attr(n, "src");
        if (src && *src && hn_media_state(c, n) != HN_MEDIA_IDLE && cnt < cap) {
            out[cnt] = n;
            srcs[cnt] = src;
            cnt++;
        }
    }
    for (hn_node *ch = hn_node_first_child(n); ch; ch = hn_node_next_sibling(ch))
        cnt += vl_collect(ch, c, out + cnt, srcs + cnt, cap - cnt);
    return cnt;
}

void hn_media_platform_video_layers(hnp_window *w, hn_rt *rt) {
    if (!w || !rt) return;
    hn_context *c = hn_rt_context(rt);
    hn_doc *doc = hn_rt_doc(rt);
    if (!c || !doc) return;
    hn_node *want[HN_VLW_CAP];
    const char *srcs[HN_VLW_CAP];
    int nwant = vl_collect(hn_doc_root(doc), c, want, srcs, HN_VLW_CAP);

    /* 占位 RECT 扫描(视口坐标源, 见段注释): 引擎硬编码深底色 + 1px 边框 */
    const hn_display_list *dl = hn_context_display_list(c);
    hn_cmd rects[HN_VLW_CAP];
    int nrect = 0;
    if (dl) {
        for (int i = 0; i < dl->count && nrect < HN_VLW_CAP; i++) {
            const hn_cmd *cm = &dl->cmds[i];
            if (cm->kind != HN_CMD_RECT ||
                cm->fill != 0x14171EFFu || cm->stroke != 0x3A4150FFu ||
                cm->stroke_w != 1.0f || cm->gradient || cm->shadow ||
                cm->text || cm->poly || cm->bitmap) continue;
            rects[nrect++] = *cm;
        }
    }
    for (int i = 0; i < nwant; i++) {
        /* 盒: 配对成功用视口坐标, 否则退布局盒(几何粗校 ±2x 防误配) */
        float x, y, bw, bh;
        hn_node_box(want[i], &x, &y, &bw, &bh);
        if (nrect == nwant && rects[i].w > 0 && rects[i].h > 0 &&
            rects[i].w > bw * 0.5f && rects[i].w < bw * 2.0f &&
            rects[i].h > bh * 0.5f && rects[i].h < bh * 2.0f) {
            x = rects[i].x; y = rects[i].y; bw = rects[i].w; bh = rects[i].h;
        }
        /* 每拍都调 frame(): 位置/尺寸跟随布局与滚动, 平台侧对"盒未变"自跳
           (setFrame + CA flush 不做白工), 并顺手轮询图层首帧就绪诊断 */
        hnp_video_layer_frame(w, srcs[i], x, y, bw, bh);
        if (vl_attached_find(srcs[i]) < 0 && video_layers_enabled() &&
            hnp_video_layer_attach(w, srcs[i], x, y, bw, bh)) {
            if (g_vl_attached_n < HN_VLW_CAP) {
                snprintf(g_vl_attached[g_vl_attached_n].src,
                         sizeof(g_vl_attached[0].src), "%s", srcs[i]);
                g_vl_attached_n++;
            }
        }
    }
    /* 摘除不在当前 want 集里的(会话关闭/元素移除/热重载换文档) */
    for (int i = g_vl_attached_n - 1; i >= 0; i--) {
        int still = 0;
        for (int k = 0; k < nwant; k++)
            if (strcmp(g_vl_attached[i].src, srcs[k]) == 0) { still = 1; break; }
        if (!still) {
            hnp_video_layer_detach(w, g_vl_attached[i].src);
            g_vl_attached[i] = g_vl_attached[g_vl_attached_n - 1];
            g_vl_attached_n--;
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
               验收脚本能观测"静音开关真的动过"的唯一真源。
               尾标 layer = 该会话当前由原生 AVPlayerLayer 直出(GPU 合成,
               CPU 拉帧已停) —— 真窗图层模式的验收观测点。 */
            printf("[media] id=%s tag=%s state=%s t=%.3f dur=%.3f app_mute=%d%s\n",
                   id ? id : "-", tag, state_name(hn_media_state(c, n)),
                   hn_media_time(c, n), hn_media_duration(c, n),
                   g_mute_state,
                   (tag[0] == 'v' && vl_attached_find(src) >= 0) ? " layer" : "");
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

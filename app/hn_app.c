/* hn_app.c — 新架构的通用应用入口(平台壳主循环, 三平台共用这一个)
 *
 * 分层(见 platform/hn_platform.h): 本文件 = "hn_rt + 平台 API"的主循环,
 * ~120 行。平台差异全部在 platform/hnp_*.c 里; 文档/htmx/sys:// 全部在
 * rt/hn_rt.c 里。**这一个 main 链接不同平台实现即为该平台的应用**:
 *   macOS:  hnp_macos.c    Linux: hnp_linux.c    Windows: hnp_win32.c
 *   无头:   hnp_headless.c(CI/服务器)
 *
 * 用法:
 *   hn_app <file.html> [W H]                开窗口
 *   hn_app <file.html> --probe [W H]        无窗口自检(管线统计+命中)
 *   hn_app <file.html> --shot <png> [W H] [--advance MS]  渲染落盘
 *
 * mtime 热重载(--watch, 缺省开): 窗口主循环每 ~300ms stat 源文件,
 * mtime/尺寸变了就整文档换装(窗口表面/进程不动, 内容替换 + 重绘)——
 * daemon 的 update op 对 window 应用即"原子写回源文件 + 子进程自行重载",
 * `hn dev` 的核心语义(mtime 热重载)由此落地。
 *
 * 媒体宿主接线(本轮补齐, 见 app/hn_media_platform.{h,c}):
 *   启动时 hn_rt_set_media(hn_media_platform_host()) —— macOS = AVFoundation
 *   真宿主(视频解码 + 音频直出), 无媒体平台 = 合成替身(梯度帧假时钟)。
 *   帧推进沿用既有节拍: hn_rt_frame → hn_context_anim_tick 内部已并入
 *   hn_media_tick_ret(PLAYING 会话保持活跃), 视频在播时自动 16ms 帧循环,
 *   无需额外时钟。窗口内 'm' 键 = 媒体静音开关(手工试听 walkthrough);
 *   HN_MEDIA_TRACE=1 时每 ~500ms 打一行会话状态(验收脚本观测点)。
 */
/* 特性宏: watch 要纳秒 mtime(秒级会漏同秒内的流式连推)。Darwin 的
 * st_mtimespec 在 _DARWIN_C_SOURCE 里(超集, 含全部 POSIX); 其余平台
 * 走 POSIX 2008(st_mtim)。 */
#if defined(__APPLE__)
#define _DARWIN_C_SOURCE 1
#else
#define _POSIX_C_SOURCE 200809L
#endif
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <time.h>
#include <sys/stat.h>

#include "hn_rt.h"
#include "hn_platform.h"
#include "hn_media_platform.h"

static char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    *len = got;
    return buf;
}

/* HiDPI 渲染缩放(开窗后解析一次: HN_RENDER_SCALE 覆盖 | 平台 backing,
   cap 2.0)。热重载整文档换装时带进新 rt —— 否则重载一帧退回 1x。 */
static float g_render_scale = 1.0f;

/* 真窗视频图层同步(app/hn_media_platform.c 提供; 原型就地声明 —— 壳头
   文件本轮不动)。每拍把"会话已开"的 <video> 盒同步给原生 AVPlayerLayer
   (GPU 合成直通, CPU 拉帧归零); 仅真窗主循环调用 —— --shot/--advance
   离屏自检不经过这里, 探针确定性口径不破。HN_MEDIA_LAYER=0 退回 CPU。 */
void hn_media_platform_video_layers(hnp_window *w, hn_rt *rt);

static void blit_now(hn_rt *rt, hnp_window *w) {
    /* 位图物理尺寸(= 逻辑 × scale; 1x 两者相同): blit 喂的是位图,
       逻辑尺寸由平台侧的 NSImage 点尺寸表达(hnp_macos 的 HiDPI 口径)。 */
    const unsigned char *px = hn_rt_pixels(rt);
    int bw = hn_rt_bitmap_width(rt), bh = hn_rt_bitmap_height(rt);
    if (px && bw > 0 && bh > 0)
        hnp_window_blit(w, px, bw, bh, (size_t)bw * 4);
}

/* IME 焦点盒同步: 把引擎当前焦点控件盒(无焦点则 0 尺寸)喂平台 ——
   平台据此激活/停用输入上下文并把候选窗/输入法图标摆到控件处。
   平台不支持时空实现(签名同源, 零分平台代码)。点点击/布局/Tab 后调用。 */
static void sync_ime_focus(hn_rt *rt, hnp_window *w) {
    int x = 0, y = 0, bw = 0, bh = 0;
    if (hn_rt_focus_box(rt, &x, &y, &bw, &bh))
        hnp_ime_focus(w, x, y, bw, bh);
    else
        hnp_ime_focus(w, 0, 0, 0, 0);
}

/* ---------------- 媒体: 落盘前的确定性推进(--shot --advance MS) ---------------- */

/* 全部 video 会话里最大的当前时刻(秒; 无媒体/无会话 = 0)。
   只走 hn.h 公开遍历 API 与元素寻址媒体 API(与引擎内部零耦合)。 */
static double max_video_time(hn_rt *rt) {
    hn_doc *doc = rt ? hn_rt_doc(rt) : NULL;
    hn_context *c = rt ? hn_rt_context(rt) : NULL;
    hn_node *root = doc ? hn_doc_root(doc) : NULL;
    if (!c || !root) return 0;
    double best = 0;
    hn_node *stack[256];
    int sp = 0;
    stack[sp++] = root;
    while (sp > 0) {
        hn_node *n = stack[--sp];
        const char *tag = hn_node_tag(n);
        if (tag && !strcmp(tag, "video")) {
            double t = hn_media_time(c, n);
            if (t > best) best = t;
        }
        for (hn_node *ch = hn_node_first_child(n); ch && sp < 256;
             ch = hn_node_next_sibling(ch))
            stack[sp++] = ch;
    }
    return best;
}

/* --shot --advance MS: 把媒体推进到 MS 毫秒处再落盘 —— 视频"动帧"的
   确定性取样口径(两次不同 advance 的落盘必须给出不同像素, 验收 §③)。
   synth 宿主(无媒体平台): 假时钟 16ms 步瞬时快进, 逐字节确定;
   平台真宿主(AVPlayer): 只能真播 —— 墙钟每 16ms 走一帧直到目标时刻
   (6s 上限防呆), 上限内未到也照落当前帧(位置单调, 帧序仍正确)。 */
static void advance_media(hn_rt *rt, int ms) {
    if (!rt || ms <= 0) return;
    int repaint = 0;
    if (!strcmp(hn_media_platform_kind(), "synth")) {
        for (long t = 0; t < ms; t += 16) hn_rt_frame(rt, 16.0f, &repaint);
        printf("advance(synth): media t=%.3f s(目标 %.3f)\n",
               max_video_time(rt), ms / 1000.0);
        return;
    }
    double target = ms / 1000.0;
    unsigned long t0 = hnp_now_ms();
    while (hnp_now_ms() - t0 < 6000) {
        hn_rt_frame(rt, 16.0f, &repaint);
        if (max_video_time(rt) >= target) break;
        /* 墙钟等待一帧(usleep 不在 POSIX 2008; nanosleep 三平台都有) */
        struct timespec ts = { 0, 16 * 1000 * 1000 };
        nanosleep(&ts, NULL);
    }
    printf("advance(platform): media t=%.3f s(目标 %.3f)\n",
           max_video_time(rt), target);
    fflush(stdout);
}

/* ---------------- mtime 热重载 ---------------- */

/* watch 轮询周期(ms): 与 16ms 帧节拍解耦, 文件系统事件不值得更密 */
#define HN_WATCH_INTERVAL_MS 300

/* 空闲 pump 周期(ms): hn_rt_frame 报告"无事可睡"(-1)时的唤醒节奏。
   250ms = 4Hz —— 足以兜住 300ms 的 mtime watch 周期(由循环内计时判定),
   又让真窗口在无动画/无轮询时彻底离开 60fps 空转。 */
#define HN_IDLE_PUMP_MS 250

/* 源文件指纹(mtime 纳秒 + 尺寸): 秒级 mtime 会漏掉同秒内的连推,
 * 纳秒口径才能接住 daemon 每几百毫秒一帧的流式推送。 */
typedef struct { long long mtime_ns; long long size; } file_fp;

static int file_fp_of(const char *path, file_fp *fp) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    fp->size = (long long)st.st_size;
#if defined(__APPLE__)
    fp->mtime_ns = (long long)st.st_mtimespec.tv_sec * 1000000000LL
                 + (long long)st.st_mtimespec.tv_nsec;
#elif defined(__linux__)
    fp->mtime_ns = (long long)st.st_mtim.tv_sec * 1000000000LL
                 + (long long)st.st_mtim.tv_nsec;
#else
    fp->mtime_ns = (long long)st.st_mtime * 1000000000LL;
#endif
    return 1;
}

/* 整文档换装: 先开新 rt, 成功才关旧的 —— 新装载失败(文件被写劈/解析失败)
 * 时旧文档原样在屏, 窗口不闪不退。hn_rt_close 对含 JS 定义的 runtime 安全:
 * JS 上下文挂在进程级 scope 表(js_scopes, rt/hn_rt.c, 按 rt->id 记名),
 * hn_rt_close 不回收它, 同 id 重开在下次 eval 时复用同一上下文并刷新
 * doc/ctx 桥 —— daemon 的 open/close/open 循环同款姿势, 已被 chat-stream
 * 五步推流反复验证。 */
static hn_rt *doc_reload(const char *path, int W, int H) {
    size_t len = 0;
    char *html = read_all(path, &len);
    if (!html) return NULL;   /* 撞上原子替换瞬间(读到旧 inode/半截): 重试 */
    hn_rt_desc d = { "app", html, len, NULL, 0, W, H };
    hn_rt *rt = hn_rt_open(&d);
    free(html);
    /* HiDPI: 新 rt 缺省 1x, 带上解析好的渲染缩放(hn_rt_set_scale 内部
       立即按新缩放重出图) —— 热重载一帧都不退回 1x。 */
    if (rt && g_render_scale != 1.0f) hn_rt_set_scale(rt, g_render_scale);
    return rt;
}

/* ---------------- 窗口几何记忆(~/.html-native/geometry/<doc-stem>.json) ----------------
 *
 * 桌宠/数字人"关了再开回到原位": 主循环 ~2s 轮询 hnp_window_origin,
 * 位置变了 → 滑动 debounce 2s 后落盘(拖动期间顺延, 停手即写);
 * SIGTERM(daemon 收口 win_kill)与正常关闭(HNP_EV_CLOSE/Esc)在退窗前
 * 补写最后一次。SIGKILL/崩溃允许丢最后一次(≤2s 的几何, 可接受)。
 * 开窗前读回: 位置必须随 hnp_window_desc.x/y 进开窗调用, 不能开了再挪
 * (manifest 显式 @x,y 优先 —— 作者声明压过记忆)。
 * 文件是普通 fwrite(非原子), 崩溃最多留下半截 JSON —— 读回端解析失败
 * 按无记忆处理, 下次落盘自愈。 */

#define HN_GEO_POLL_MS     2000   /* 轮询周期(拖动采样粒度) */
#define HN_GEO_DEBOUNCE_MS 2000   /* 变更后的静默期(拖动中不断顺延) */

typedef struct { int x, y, w, h; } geo_rect;

/* doc-stem: base 去扩展名("pet.html" → "pet")—— geometry 的文件名键 */
static void geo_stem(const char *base, char *out, size_t cap) {
    snprintf(out, cap, "%s", base);
    char *dot = strrchr(out, '.');
    if (dot && dot != out) *dot = 0;
}

static void geo_path(const char *stem, char *out, size_t cap) {
    const char *home = getenv("HOME");
    snprintf(out, cap, "%s/.html-native/geometry/%s.json",
             home && *home ? home : ".", stem);
}

/* 极简 JSON 数字读取(与 tools/hn_daemon.c 的 json_num 同思路) */
static int geo_num(const char *json, const char *key, int dflt) {
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return dflt;
    p += strlen(pat);
    while (*p == ' ' || *p == '\t') p++;
    if (*p != ':') return dflt;
    p++;
    while (*p == ' ' || *p == '\t') p++;
    char *e = NULL;
    long v = strtol(p, &e, 10);
    return e == p ? dflt : (int)v;
}

static int geo_load(const char *path, geo_rect *g) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;
    char buf[512] = { 0 };
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    if (!n) return 0;
    g->x = geo_num(buf, "x", 0);
    g->y = geo_num(buf, "y", 0);
    g->w = geo_num(buf, "w", 0);
    g->h = geo_num(buf, "h", 0);
    /* 字段全 0 = 半截/空文件: 按无记忆处理 */
    return (g->x || g->y || g->w || g->h);
}

static void geo_store(const char *path, const geo_rect *g) {
    const char *home = getenv("HOME");
    if (!home || !*home) return;
    char dir[1024];
    /* mkdir 只建末级: 隔离 HOME(验收脚本)里 .html-native 本身可能不存在,
       两级都建(mkdir 已存在时 EEXIST, 无妨)。 */
    snprintf(dir, sizeof(dir), "%s/.html-native", home);
    mkdir(dir, 0755);
    snprintf(dir, sizeof(dir), "%s/.html-native/geometry", home);
    mkdir(dir, 0755);
    FILE *f = fopen(path, "wb");
    if (!f) return;
    fprintf(f, "{\"x\":%d,\"y\":%d,\"w\":%d,\"h\":%d}\n", g->x, g->y, g->w, g->h);
    fclose(f);
}

/* SIGTERM(daemon win_kill 收口)/SIGINT: 置位由主循环消费 —— 用 sigaction
   且不带 SA_RESTART, 阻塞中的 hnp_pump 被 EINTR 打断立即醒, 退窗前还能
   把最后一次几何写盘(signal() 的 BSD 语义带 SA_RESTART 会睡满超时)。 */
static volatile sig_atomic_t g_app_term = 0;
static void hn_app_on_term(int sig) { (void)sig; g_app_term = 1; }

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "用法: hn_app <file.html> [W H] [--watch 0|1] | --probe | --shot <png> [W H] [--advance MS]\n");
        return 2;
    }
    const char *path = argv[1];
    int probe = (argc > 2 && !strcmp(argv[2], "--probe"));
    int shot = (argc > 2 && !strcmp(argv[2], "--shot"));
    int watch = 1;   /* mtime 热重载缺省开(向后兼容); --watch 0 显式关 */
    int W = 480, H = 700;
    int advance_ms = 0;   /* --shot --advance MS: 落盘前推进媒体到该时刻 */
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], "--watch") && i + 1 < argc) watch = atoi(argv[i + 1]);
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], "--advance") && i + 1 < argc) advance_ms = atoi(argv[i + 1]);
    if (!probe && !shot) {
        /* 位置参数 W H: 跳过 --watch/--advance 及其值(选项与位置参数可混排) */
        int got = 0;
        for (int i = 2; i < argc && got < 2; i++) {
            if (!strcmp(argv[i], "--watch") || !strcmp(argv[i], "--advance")) { i++; continue; }
            if (got == 0) W = atoi(argv[i]); else H = atoi(argv[i]);
            got++;
        }
    } else {
        /* --shot <png> [W H] [--advance MS]: <png> 固定在 argv[3]; W/H 是
           其后**非选项**的前两个数(逐项跳过 --advance/--watch 及其值,
           避免把选项值(如 advance 的 1500)错当尺寸)。 */
        int a = shot ? 4 : 3;
        int got = 0;
        for (int i = a; i < argc && got < 2; i++) {
            if (argv[i][0] == '-') {
                if ((!strcmp(argv[i], "--advance") || !strcmp(argv[i], "--watch")) &&
                    i + 1 < argc) i++;   /* 连值一起跳过 */
                continue;
            }
            if (got == 0) W = atoi(argv[i]); else H = atoi(argv[i]);
            got++;
        }
    }

    /* 媒体宿主装配(必须先于 hn_rt_open —— 每个 context 创建时经 g_media_be
       注入; autoplay 会话在首轮布局就 open)。probe/shot 属自检口径:
       未显式声明 HN_MEDIA_UNMUTE 时默认静音跑(探针不吵人; 真窗口不设)。 */
    if ((probe || shot) && !getenv("HN_MEDIA_UNMUTE") && !getenv("HN_MEDIA_MUTE"))
        setenv("HN_MEDIA_MUTE", "1", 1);
    hn_rt_set_media(hn_media_platform_host());

    /* GPU 渲染后端装配(必须先于 hn_rt_open): **缺省关** —— HN_GPU 未设或
       非 "metal" 时什么都不注入, 一切走 hnsoft(行为零漂移); HN_GPU=metal
       显式开启 → 注入平台 GPU 原语表(macOS = hnp_gpu_metal.c 的 Metal 实现,
       hnp_gpu_backend(); headless/linux 平台文件返回 NULL → 注了个空表,
       等价缺省)。开启后渲染分支按帧判定(hn_rt.c rt_raster): 指令集不支持
       → 整帧回退 hnsoft, 回退逐帧静默。--shot 探针口径见 hn_rt.c
       hn_rt_shot 注释: 未设 HN_GPU 时恒 hnsoft 1x 确定性; 显式 metal 时
       离屏与真窗同走 GPU。 */
    {
        const char *gpu = getenv("HN_GPU");
        if (gpu && !strcmp(gpu, "metal"))
            hn_rt_set_gpu_host(hnp_gpu_backend());
    }

    size_t hlen = 0;
    char *html = read_all(path, &hlen);
    if (!html) { fprintf(stderr, "无法读取 %s\n", path); return 1; }

    /* 路径口径: 资产/图片/<link> 相对文档目录(与浏览器一致)。
       base = chdir 后的文档目录内文件名 —— watch/reload 也用它 stat/读,
       重载发生在主循环里, 那时 cwd 已是文档目录。 */
    const char *base = path;
    const char *slash = strrchr(path, '/');
    if (slash) {
        char dir[1024];
        size_t dl = (size_t)(slash - path + 1);
        if (dl < sizeof(dir)) {
            memcpy(dir, path, dl);
            dir[dl] = 0;
            chdir(dir);
        }
        base = slash + 1;
    }

    hn_rt_desc d = { "app", html, hlen, NULL, 0, W, H };
    hn_rt *rt = hn_rt_open(&d);
    if (!rt) { fprintf(stderr, "文档初始化失败\n"); free(html); return 1; }
    hn_media_platform_bind(rt);   /* 环境钮(静音/取消静音)在装载后施加 */

    if (probe) {
        hn_rt_set_scale(rt, 1.0f);   /* 自检口径锚定 1x(与 HN_RENDER_SCALE 环境解耦) */
        printf("platform=%s cmds=%d w=%d h=%d media=%s\n",
               hnp_name(), hn_rt_cmd_count(rt), hn_rt_width(rt), hn_rt_height(rt),
               hn_media_platform_kind());
        char *dom = hn_rt_dom(rt, 2);
        if (dom) { printf("%s", dom); free(dom); }
        char *t = hn_rt_text(rt, "out");
        if (t) { printf("text[out]=%s\n", t); free(t); }
        hn_rt_close(rt);
        free(html);
        printf("probe OK\n");
        return 0;
    }
    if (shot) {
        hn_rt_set_scale(rt, 1.0f);   /* --shot 恒 1x(探针确定性锚, 不随环境钮漂) */
        if (advance_ms > 0) advance_media(rt, advance_ms);
        int ok = hn_rt_shot(rt, argv[3]);
        printf("%s\n", ok ? "shot OK" : "shot 失败");
        hn_rt_close(rt);
        free(html);
        return ok ? 0 : 1;
    }

    /* ---- 开窗口 + 主循环(平台差异全部在 hnp_* 里) ---- */
    if (!hnp_init(0, NULL)) {
        fprintf(stderr, "平台初始化失败(%s)\n", hnp_name());
        hn_rt_close(rt); free(html);
        return 1;
    }
    /* 窗口形态 = 文档清单声明(透明/无边框/层级/跨空间/投影/穿透)。
       桌宠形态就是这一组字段的组合, 见 platform/hn_platform.h 的字段注释;
       平台不支持时优雅忽略(声明是意图, 物化看平台)。 */
    hn_manifest man;
    hn_rt_manifest(rt, &man);
    hnp_window_desc wd;
    memset(&wd, 0, sizeof(wd));
    wd.title = "hn";                       /* borderless 时仅任务栏/调试显示名 */
    wd.width = hn_rt_width(rt);
    wd.height = hn_rt_height(rt);
    wd.transparent = man.transparent;
    wd.resizable = 1;
    wd.borderless = man.frameless;
    wd.always_on_top = (man.level == HN_LEVEL_FLOATING) ? 1
                     : (man.level == HN_LEVEL_DESKTOP) ? 2 : 0;
    wd.no_shadow = !man.shadow;
    wd.join_all_spaces = man.spaces_all;
    wd.click_through = man.click_through;
    wd.x = HNP_WINDOW_POS_AUTO;
    wd.y = HNP_WINDOW_POS_AUTO;
    /* 环境钮(验收脚本/调试): HN_CLICK_THROUGH=0|1 覆盖文档声明,
       便于对同一份桌宠页面演示"幽灵模式"两个态而不改文档。 */
    {
        const char *ct = getenv("HN_CLICK_THROUGH");
        if (ct && *ct) wd.click_through = atoi(ct) ? 1 : 0;
    }
    /* ---- 几何记忆读回(必须赶在开窗前 —— 位置随 desc 进开窗调用) ----
       manifest 显式 @x,y 时作者声明优先(此前只拿它挡记忆、从未赋给
       wd.x/wd.y —— 多屏机器上窗口落到"键盘焦点屏"的居中点, 随机漂;
       现已按声明落位); 否则读回上次会话的摆放。
       只有 w/h 记进文件备查: 开窗尺寸仍按 manifest/CLI 口径(daemon 每次
       都显式传 W H, 记忆尺寸不掺和)。 */
    {
        char stem[256], gpath[1152];
        geo_stem(base, stem, sizeof(stem));
        geo_path(stem, gpath, sizeof(gpath));
        geo_rect mem;
        if (man.x != 0 || man.y != 0) {
            wd.x = (int)man.x;
            wd.y = (int)man.y;
        } else if (geo_load(gpath, &mem)) {
            wd.x = mem.x;
            wd.y = mem.y;
        }
    }
    hnp_window *w = hnp_window_open(&wd);
    if (!w) {
        fprintf(stderr, "窗口创建失败\n");
        hn_rt_close(rt); free(html);
        return 1;
    }
    /* ---- HiDPI(真窗 Retina 2x): 解析渲染缩放并落到 rt ----
       HN_RENDER_SCALE=1|2 显式覆盖(探针/强制 1x 用), 缺省 = 平台 backing
       scale(hnp_window_scale, Retina=2), 一律 cap 2.0。离屏/--shot 路径
       不经过这里(上面已锚定 1x), 探针口径不破。hn_rt_set_scale 内部
       立即按新缩放整幅重出图(文字 FreeType 2x 光栅)。 */
    {
        float rs = hnp_window_scale(w);
        const char *e = getenv("HN_RENDER_SCALE");
        if (e && *e) {
            float v = (float)atof(e);
            if (v >= 1.0f) rs = v;               /* 非法/0 → 平台 backing */
        }
        if (rs > 2.0f) rs = 2.0f;
        if (rs < 1.0f) rs = 1.0f;
        g_render_scale = rs;                     /* 热重载带进新 rt */
        hn_rt_set_scale(rt, rs);
    }
    blit_now(rt, w);
    /* 真实键盘焦点在窗口上: 插入符可见(闪烁时机由引擎动画时钟推进;
       失焦由 HNP_EV_FOCUS_OUT 熄灭 —— 桌宠常驻窗口不闪无谓的 caret)。 */
    hn_context_set_caret_visible(hn_rt_context(rt), 1);
    sync_ime_focus(rt, w);   /* 初始 IME 焦点盒(通常无焦点 → 停用态) */

    /* ---- 收尾信号(SIGTERM = daemon win_kill 的优雅关闭路径) ---- */
    {
        struct sigaction sa;
        memset(&sa, 0, sizeof(sa));
        sa.sa_handler = hn_app_on_term;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = 0;   /* 不带 SA_RESTART: pump 立刻醒, 见 handler 注释 */
        sigaction(SIGTERM, &sa, NULL);
        sigaction(SIGINT, &sa, NULL);
    }

    /* ---- 几何记忆状态: 基线记账(开窗即写一次, 记忆与窗口同生命周期) ----
       平台读不到 origin(headless/linux)时 geo_have=0, 整条记忆路径静默跳过。
       首帧即落盘基线: 未挪动过的窗口也有账(下次开窗回到同一处, 而不是
       每次都换居中点), 验收脚本也由此拿到确定性的"文件会出现"。 */
    geo_rect geo = { wd.x, wd.y, wd.width, wd.height };
    int geo_have = 0;
    unsigned long geo_dirty_at = 0;   /* 0 = 干净; 非 0 = 变脏时刻(debounce 起) */
    unsigned long last_geo_poll = hnp_now_ms();
    char gpath[1152];
    {
        char stem[256];
        geo_stem(base, stem, sizeof(stem));
        geo_path(stem, gpath, sizeof(gpath));
        int gx, gy;
        if (hnp_window_origin(w, &gx, &gy)) {
            geo.x = gx; geo.y = gy;
            geo_have = 1;
            geo_store(gpath, &geo);
        }
    }

    /* ---- mtime 热重载 watch: 指纹基线(重载只认它, 与窗口表面无关) ---- */
    file_fp fp = { 0, 0 };
    int have_fp = file_fp_of(base, &fp);
    unsigned long last_poll = hnp_now_ms();
    int ww = hn_rt_width(rt), wh = hn_rt_height(rt);   /* 当前窗口表面尺寸 */

    int quit = 0;
    unsigned long last = hnp_now_ms();
    /* 唤醒节奏(ms): 首帧前按空闲节奏起步, 之后由 hn_rt_frame 的返回值
       逐帧刷新(见循环尾部的"帧节拍接线")。 */
    int pump_ms = HN_IDLE_PUMP_MS;
    /* HN_MEDIA_TRACE=1: 每 ~500ms 一行会话状态(验收脚本/reverse 调试) */
    int trace = 0;
    {
        const char *e = getenv("HN_MEDIA_TRACE");
        if (e && *e && strcmp(e, "0") != 0) {
            trace = 1;
            printf("[media] 宿主=%s surf=%s(HN_MEDIA_TRACE=1, 按 m 切换静音)\n",
                   hn_media_platform_kind(), hnp_name());
            fflush(stdout);
        }
    }
    unsigned long last_trace = 0;
    /* blit 合并(性能治理): 事件处理只置 need_blit, 排干队列后一次 blit。
       之前每个状态变化事件立即全量 blit —— HiDPI 下一次 blit 是整面
       位图拷贝(1440p@2x ≈ 20MB), 鼠标快速划过 hover 面时一次爆发可
       连发十几次, 内存带宽大头全浪费在中间态上。引擎侧 hn_rt_pointer_
       move 的逐事件重渲染不动(状态必须每步跟上), 只省平台拷贝。 */
    int need_blit = 0;
    while (!quit) {
        if (g_app_term) quit = 1;   /* SIGTERM/SIGINT: 退窗前补写几何(下方出口) */
        hnp_event ev;
        /* 内层排干: 首次调用按 pump_ms 睡到事件来, 之后 0 超时纯轮询,
           队列空即出。爆发事件在同一拍内连续派发(先于本拍帧 tick),
           与浏览器"输入事件先于 rAF 合并"的语义一致。
           排干期间绘制挂起: 输入路径只标脏(hn_rt_paint_defer),
           队尾 flush 一次整渲染 —— 中间态像素从不被呈现,
           N 次 layout+raster 合 1(与 blit 合并配套的第二刀)。 */
        int drained = 0;
        hn_rt_paint_defer(rt, 1);
        while (!quit && hnp_pump(&ev, drained ? 0 : pump_ms)) {
            drained = 1;
            switch (ev.kind) {
            case HNP_EV_CLOSE: quit = 1; break;
            case HNP_EV_RESIZE:
                hnp_window_resize(w, ev.x, ev.y);
                /* 尺寸也进几何账(下次开窗仍按 manifest/CLI, 仅备查) */
                if (geo_have && (geo.w != ev.x || geo.h != ev.y)) {
                    geo.w = ev.x; geo.h = ev.y;
                    geo_dirty_at = hnp_now_ms();
                }
                hn_rt_reflow(rt);
                need_blit = 1;
                sync_ime_focus(rt, w);   /* 重排后焦点盒可能变 */
                break;
            case HNP_EV_EXPOSE: need_blit = 1; break;
            case HNP_EV_POINTER_MOVE:
                if (hn_rt_pointer_move(rt, (float)ev.x, (float)ev.y)) need_blit = 1;
                break;
            case HNP_EV_BUTTON_DOWN:
                if (hn_rt_button(rt, 1, (float)ev.x, (float)ev.y, ev.button)) need_blit = 1;
                /* IME 焦点盒同步: 点击后焦点可能落到/离开可编辑控件 ——
                   把新焦点盒喂平台(候选窗摆位/输入上下文激活态)。 */
                sync_ime_focus(rt, w);
                break;
            case HNP_EV_SCROLL:
                if (hn_rt_scroll(rt, ev.dx, ev.dy, (float)ev.x, (float)ev.y)) need_blit = 1;
                break;
            case HNP_EV_KEY_DOWN:
                /* Esc: 组字中由 IME 消费(平台在组字期间不发本事件); 到这里
                   就是真 Esc → 关窗(既有语义)。 */
                if (ev.key == HNP_KEY_ESCAPE) { quit = 1; break; }
                /* ⌘/Ctrl 编辑快捷键(复制/粘贴/剪切/全选): 平台剪贴板优先
                   (hnp_clipboard_get 拿系统剪贴板), 无平台桥时落回引擎的
                   同源后端(sys://clipboard)。Ctrl+C 等是终端习惯; mac 用 ⌘。 */
                if ((ev.mods & 10) && ev.utf8 && (ev.utf8[0] == 'c' || ev.utf8[0] == 'C')) {
                    if (hn_rt_edit(rt, HN_EDIT_COPY, NULL)) need_blit = 1;
                    break;
                }
                if ((ev.mods & 10) && ev.utf8 && (ev.utf8[0] == 'v' || ev.utf8[0] == 'V')) {
                    char cb[8192] = { 0 };
                    const char *txt = NULL;
                    if (hnp_clipboard_get(w, cb, sizeof(cb)) && cb[0]) txt = cb;
                    if (hn_rt_edit(rt, HN_EDIT_PASTE, txt)) need_blit = 1;
                    break;
                }
                if ((ev.mods & 10) && ev.utf8 && (ev.utf8[0] == 'x' || ev.utf8[0] == 'X')) {
                    if (hn_rt_edit(rt, HN_EDIT_CUT, NULL)) need_blit = 1;
                    break;
                }
                if ((ev.mods & 10) && ev.utf8 && (ev.utf8[0] == 'a' || ev.utf8[0] == 'A')) {
                    if (hn_rt_edit(rt, HN_EDIT_SELECT_ALL, NULL)) need_blit = 1;
                    break;
                }
                /* m = 媒体静音开关(手工验收 walkthrough: 试听/肃静两态);
                   带 cmd/ctrl 修饰的 m 不拦(留给系统/编辑器快捷键),
                   无媒体元素时也不消费, 照旧交引擎。 */
                if (ev.utf8 && (ev.utf8[0] == 'm' || ev.utf8[0] == 'M') &&
                    !(ev.mods & 10) && hn_media_platform_toggle_mute(rt))
                    break;
                if (hn_rt_key(rt, 1, (int)ev.key, ev.utf8)) need_blit = 1;
                /* Tab 轮换/点击换焦点后 IME 盒跟动 */
                if (ev.key == HNP_KEY_TAB) sync_ime_focus(rt, w);
                break;
            case HNP_EV_IME_PREEDIT:
                /* 输入法组字中间态: 预编辑串直插(最小实现见 hn_rt_ime_preedit) */
                if (hn_rt_ime_preedit(rt, ev.utf8 ? ev.utf8 : "", ev.cursor)) need_blit = 1;
                sync_ime_focus(rt, w);   /* 组字中焦点盒可能随行变化 */
                break;
            case HNP_EV_IME_COMMIT:
                if (hn_rt_ime_commit(rt, ev.utf8 ? ev.utf8 : "")) need_blit = 1;
                sync_ime_focus(rt, w);
                break;
            case HNP_EV_SCALE_CHANGED: {
                /* ---- 深度 3: 跨屏/系统缩放变化 → 重设渲染缩放 + 重 blit ----
                   HN_RENDER_SCALE 显式覆盖时不跟动(探针/强制 1x 语义)。 */
                float rs = ev.scale;
                if (rs > 2.0f) rs = 2.0f;
                if (rs < 1.0f) rs = 1.0f;
                const char *e2 = getenv("HN_RENDER_SCALE");
                if (!(e2 && *e2)) {
                    g_render_scale = rs;
                    hn_rt_set_scale(rt, rs);
                    need_blit = 1;
                    printf("[scale] 事件驱动重设: scale=%.0f bmp=%dx%d\n",
                           (double)rs, hn_rt_bitmap_width(rt), hn_rt_bitmap_height(rt));
                    fflush(stdout);
                }
                break;
            }
            case HNP_EV_SCREEN_CHANGED:
                /* ---- 深度 3: 屏幕参数变化 → 几何记忆越界夹取 ----
                   显示器摘除/重排后记忆坐标可能落进不存在区域: 读当前
                   origin + 可见区, 越界就 move 回可见区内。 */
                if (geo_have) {
                    int gx, gy, vx, vy, vw, vh;
                    if (hnp_window_origin(w, &gx, &gy) &&
                        hnp_screen_workarea(w, gx + geo.w / 2, gy + geo.h / 2,
                                            &vx, &vy, &vw, &vh)) {
                        int nx = gx, ny = gy, moved = 0;
                        if (nx + 40 < vx) { nx = vx; moved = 1; }
                        if (nx > vx + vw - 40) { nx = vx + vw - 40; moved = 1; }
                        if (ny < vy) { ny = vy; moved = 1; }
                        if (ny > vy + vh - 40) { ny = vy + vh - 40; moved = 1; }
                        if (moved) {
                            hnp_window_move(w, nx, ny);
                            geo.x = nx; geo.y = ny;
                            geo_store(gpath, &geo);
                            printf("[screen] 参数变化: 几何夹取 (%d,%d)→(%d,%d)\n",
                                   gx, gy, nx, ny);
                            fflush(stdout);
                        }
                    }
                }
                break;
            case HNP_EV_FOCUS_IN:  hn_context_set_caret_visible(hn_rt_context(rt), 1); break;
            case HNP_EV_FOCUS_OUT: hn_context_set_caret_visible(hn_rt_context(rt), 0); break;
            default: break;
            }
        }
        /* 队尾冲刷: 摘挂起, 排干期间积累的脏帧一次整渲染(hn_rt_paint_flush
           返回 1 = 真画过 → 本拍要 blit; 热重载整文档换装后旧 rt 的脏标记
           随之销毁, 重载路径自带 need_blit)。 */
        hn_rt_paint_defer(rt, 0);
        if (hn_rt_paint_flush(rt)) need_blit = 1;
        unsigned long now = hnp_now_ms();
        /* ---- 热重载轮询(~300ms 一次): 指纹变了 → 整文档换装 ----
           窗口表面/进程不动: 先开新 rt, 成功才关旧 + 重绘; 失败重试一次
           (写方 tmp+rename 的瞬时旧 inode/半截读), 仍失败则保留旧文档,
           指纹不落账 —— 下个轮询周期自然再试, 窗口不闪不退。 */
        if (watch && have_fp && now - last_poll >= HN_WATCH_INTERVAL_MS) {
            last_poll = now;
            file_fp cur;
            if (file_fp_of(base, &cur) &&
                (cur.mtime_ns != fp.mtime_ns || cur.size != fp.size)) {
                hn_rt *nr = doc_reload(base, W, H);
                if (!nr) nr = doc_reload(base, W, H);   /* 重试一次 */
                if (nr) {
                    /* 顺序: 先绑定新 rt(适配层的 g_bound_rt 立刻指向新文档,
                       不悬空), 再关旧 —— 旧 rt 的媒体会话随 context 销毁
                       统一 close(hn_media_sessions_clear), AVPlayer 及时释放。 */
                    hn_media_platform_bind(nr);   /* 新文档: 时长表 + 环境钮 */
                    hn_rt_close(rt);   /* JS 安全性见 doc_reload 注释 */
                    rt = nr;
                    fp = cur;
                    have_fp = 1;
                    /* 文档清单改了尺寸 → 窗口表面跟着走(同 RESIZE 口径)。
                       注意 macOS 壳的 hnp_window_resize 只 setContentSize/
                       调内容表面, NSWindow 外框不跟动 —— 重载期间窗口外框
                       保持开窗时几何, 新文档尺寸不同则表面按新文档重绘
                       (演示口径: 前后帧声明同一 hn-window 即无此差异)。 */
                    if (hn_rt_width(rt) != ww || hn_rt_height(rt) != wh) {
                        ww = hn_rt_width(rt); wh = hn_rt_height(rt);
                        hnp_window_resize(w, ww, wh);
                    }
                    need_blit = 1;
                }
            }
        }
        int repaint = 0;
        int beat = hn_rt_frame(rt, (float)(now - last), &repaint);
        last = now;
        /* 唯一 blit 出口: 本拍排干的事件(need_blit)与帧 tick(repaint)
           合并成一次平台拷贝。 */
        if (need_blit || repaint) { blit_now(rt, w); need_blit = 0; }
        /* ---- 视频图层同步(真窗 GPU 合成): <video> 盒跟随布局/滚动/resize,
           首拍 attach(此后该会话 pf_frame 早退, CPU 拉帧归零), 元素消失/
           热重载换文档时 detach。离屏路径(--shot/--advance)不到这里。 ---- */
        hn_media_platform_video_layers(w, rt);
        /* ---- 几何记忆: ~2s 轮询 origin; 变了 → 滑动 debounce 2s 后落盘 ----
           拖动中不断顺延(停手 2s 才写, 一场拖动最多落一次盘);
           SIGTERM/正常关闭走出口处的补写, 不等 debounce。 */
        if (geo_have && now - last_geo_poll >= HN_GEO_POLL_MS) {
            last_geo_poll = now;
            int gx, gy;
            if (hnp_window_origin(w, &gx, &gy) && (gx != geo.x || gy != geo.y)) {
                geo.x = gx; geo.y = gy;
                geo_dirty_at = now;   /* 滑动: 每次变化都把静默期顺延 */
            }
        }
        if (geo_have && geo_dirty_at && now - geo_dirty_at >= HN_GEO_DEBOUNCE_MS) {
            geo_store(gpath, &geo);
            geo_dirty_at = 0;
        }
        if (trace && now - last_trace >= 500) {
            last_trace = now;
            hn_media_platform_trace(rt);
        }
        /* ---- 帧节拍接线: hn_rt_frame 的返回值就是"下一次该醒的时刻" ----
           16      动画在跑(过渡/keyframes/Lottie/媒体) → 60fps 帧循环;
           N > 16  距最近 hx 轮询的期限(ms) → 到点即醒;
           -1      空闲 → 250ms(4Hz)醒来看热重载 watch —— watch 的 300ms
                   周期由循环内计时兜住, 事件到达即醒(hnp_pump 被事件打断),
                   真窗口不再 60fps 空转。
           只改唤醒节奏, 不改任何渲染语义: 重绘仍只发生在 hn_rt_frame/
           事件处理报告 repaint 时, dt 口径(now-last)与原先一致。 */
        pump_ms = beat < 0 ? HN_IDLE_PUMP_MS : (beat < 1 ? 1 : beat);
    }

    /* 出口补写: SIGTERM(daemon 收口)/红叉/Esc 都在这里退窗前把最后一次
       几何落盘 —— 不依赖 debounce 到点。SIGKILL/崩溃到此为止不了, 最多丢
       最后 ~2s 的几何(可接受, 见文件头注释)。 */
    if (geo_have) {
        int gx, gy;
        if (hnp_window_origin(w, &gx, &gy)) { geo.x = gx; geo.y = gy; }
        geo_store(gpath, &geo);
    }

    hnp_window_close(w);
    hnp_shutdown();
    hn_rt_close(rt);
    free(html);
    return 0;
}

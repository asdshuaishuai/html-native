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

static void blit_now(hn_rt *rt, hnp_window *w) {
    const unsigned char *px = hn_rt_pixels(rt);
    if (px) hnp_window_blit(w, px, hn_rt_width(rt), hn_rt_height(rt),
                            (size_t)hn_rt_width(rt) * 4);
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
    return rt;
}

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
    /* 环境钮(验收脚本/调试): HN_CLICK_THROUGH=0|1 覆盖文档声明,
       便于对同一份桌宠页面演示"幽灵模式"两个态而不改文档。 */
    {
        const char *ct = getenv("HN_CLICK_THROUGH");
        if (ct && *ct) wd.click_through = atoi(ct) ? 1 : 0;
    }
    hnp_window *w = hnp_window_open(&wd);
    if (!w) {
        fprintf(stderr, "窗口创建失败\n");
        hn_rt_close(rt); free(html);
        return 1;
    }
    blit_now(rt, w);

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
    while (!quit) {
        hnp_event ev;
        if (hnp_pump(&ev, pump_ms)) {
            switch (ev.kind) {
            case HNP_EV_CLOSE: quit = 1; break;
            case HNP_EV_RESIZE:
                hnp_window_resize(w, ev.x, ev.y);
                hn_rt_reflow(rt);
                blit_now(rt, w);
                break;
            case HNP_EV_EXPOSE: blit_now(rt, w); break;
            case HNP_EV_POINTER_MOVE:
                if (hn_rt_pointer_move(rt, (float)ev.x, (float)ev.y)) blit_now(rt, w);
                break;
            case HNP_EV_BUTTON_DOWN:
                if (hn_rt_button(rt, 1, (float)ev.x, (float)ev.y, ev.button)) blit_now(rt, w);
                break;
            case HNP_EV_SCROLL:
                if (hn_rt_scroll(rt, ev.dx, ev.dy, (float)ev.x, (float)ev.y)) blit_now(rt, w);
                break;
            case HNP_EV_KEY_DOWN:
                if (ev.key == HNP_KEY_ESCAPE) { quit = 1; break; }
                /* m = 媒体静音开关(手工验收 walkthrough: 试听/肃静两态);
                   带 cmd/ctrl 修饰的 m 不拦(留给系统/编辑器快捷键),
                   无媒体元素时也不消费, 照旧交引擎。 */
                if (ev.utf8 && (ev.utf8[0] == 'm' || ev.utf8[0] == 'M') &&
                    !(ev.mods & 10) && hn_media_platform_toggle_mute(rt))
                    break;
                if (hn_rt_key(rt, 1, (int)ev.key, ev.utf8)) blit_now(rt, w);
                break;
            default: break;
            }
        }
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
                    blit_now(rt, w);
                }
            }
        }
        int repaint = 0;
        int beat = hn_rt_frame(rt, (float)(now - last), &repaint);
        last = now;
        if (repaint) blit_now(rt, w);
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

    hnp_window_close(w);
    hnp_shutdown();
    hn_rt_close(rt);
    free(html);
    return 0;
}

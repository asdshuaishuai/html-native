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
 *   hn_app <file.html> --shot <png> [W H]   渲染落盘
 *
 * mtime 热重载(--watch, 缺省开): 窗口主循环每 ~300ms stat 源文件,
 * mtime/尺寸变了就整文档换装(窗口表面/进程不动, 内容替换 + 重绘)——
 * daemon 的 update op 对 window 应用即"原子写回源文件 + 子进程自行重载",
 * `hn dev` 的核心语义(mtime 热重载)由此落地。
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
#include <sys/stat.h>

#include "hn_rt.h"
#include "hn_platform.h"

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

/* ---------------- mtime 热重载 ---------------- */

/* watch 轮询周期(ms): 与 16ms 帧节拍解耦, 文件系统事件不值得更密 */
#define HN_WATCH_INTERVAL_MS 300

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
        fprintf(stderr, "用法: hn_app <file.html> [W H] [--watch 0|1] | --probe | --shot <png>\n");
        return 2;
    }
    const char *path = argv[1];
    int probe = (argc > 2 && !strcmp(argv[2], "--probe"));
    int shot = (argc > 2 && !strcmp(argv[2], "--shot"));
    int watch = 1;   /* mtime 热重载缺省开(向后兼容); --watch 0 显式关 */
    int W = 480, H = 700;
    for (int i = 2; i < argc; i++)
        if (!strcmp(argv[i], "--watch") && i + 1 < argc) watch = atoi(argv[i + 1]);
    if (!probe && !shot) {
        /* 位置参数 W H: 跳过 --watch 及其值(选项与位置参数可混排) */
        int got = 0;
        for (int i = 2; i < argc && got < 2; i++) {
            if (!strcmp(argv[i], "--watch")) { i++; continue; }
            if (got == 0) W = atoi(argv[i]); else H = atoi(argv[i]);
            got++;
        }
    } else {
        int a = shot ? 4 : 3;
        if (argc > a) W = atoi(argv[a]);
        if (argc > a + 1) H = atoi(argv[a + 1]);
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

    if (probe) {
        printf("platform=%s cmds=%d w=%d h=%d\n",
               hnp_name(), hn_rt_cmd_count(rt), hn_rt_width(rt), hn_rt_height(rt));
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
    hnp_window_desc wd = { "hn", hn_rt_width(rt), hn_rt_height(rt), 0, 1 };
    /* 透明背板: 文档清单声明时透传 */
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
    while (!quit) {
        hnp_event ev;
        int timeout = 16;   /* 帧节拍; hn_rt_frame 会给更精确的值 */
        if (hnp_pump(&ev, timeout)) {
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
        hn_rt_frame(rt, (float)(now - last), &repaint);
        last = now;
        if (repaint) blit_now(rt, w);
    }

    hnp_window_close(w);
    hnp_shutdown();
    hn_rt_close(rt);
    free(html);
    return 0;
}

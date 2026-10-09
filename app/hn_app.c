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
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

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

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "用法: hn_app <file.html> [W H] | --probe | --shot <png>\n");
        return 2;
    }
    const char *path = argv[1];
    int probe = (argc > 2 && !strcmp(argv[2], "--probe"));
    int shot = (argc > 2 && !strcmp(argv[2], "--shot"));
    int W = 480, H = 700;
    if (!probe && !shot) {
        if (argc > 2) W = atoi(argv[2]);
        if (argc > 3) H = atoi(argv[3]);
    } else {
        int a = shot ? 4 : 3;
        if (argc > a) W = atoi(argv[a]);
        if (argc > a + 1) H = atoi(argv[a + 1]);
    }

    size_t hlen = 0;
    char *html = read_all(path, &hlen);
    if (!html) { fprintf(stderr, "无法读取 %s\n", path); return 1; }

    /* 路径口径: 资产/图片/<link> 相对文档目录(与浏览器一致) */
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

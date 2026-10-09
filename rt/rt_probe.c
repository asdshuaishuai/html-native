/* rt_probe.c — 分层架构探针(hn_rt + 平台抽象的完备性检验)
 *
 * 分层(见 platform/hn_platform.h 的架构图):
 *   引擎(Sources/CHtmlNative) → hn_rt(公共运行时, 单源) → 平台 API(hn_platform.h)
 *
 * 本探针在 headless 平台上验证 hn_rt 的全部公开能力 —— 这同时是
 * 抽象层的完备性检验: 一个平台只需实现十几个函数即可承载完整应用。
 *
 * 用法: cc -I Sources/CHtmlNative/include -I Sources/CHtmlNative -I rt -I platform \
 *          rt/rt_probe.c rt/hn_rt.c platform/hnp_headless.c <引擎源> -lfreetype -lm
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn_rt.h"
#include "hn_platform.h"

static int fails = 0;
static void ck(int c, const char *l) { printf("%s %s\n", c ? "  v" : "  X", l); if (!c) fails++; }

int main(void) {
    const char *html =
        "<html><head><style>body{margin:0;background:#0e1117;color:#fff}"
        "#out{background:#161b24;padding:10;border-radius:8}</style></head>"
        "<body><div id=\"out\">hello</div>"
        "<button id=\"go\" hx-post=\"sys://store/set?key=lang&value=rt\" hx-target=\"out\">go</button>"
        "</body></html>";

    hn_rt_desc d = { "rt-test", html, strlen(html), NULL, 0, 400, 300 };
    hn_rt *rt = hn_rt_open(&d);
    ck(rt != NULL, "hn_rt_open 成功");
    if (!rt) return 1;

    ck(hn_rt_width(rt) == 400 && hn_rt_height(rt) == 300, "尺寸正确");
    ck(hn_rt_pixels(rt) != NULL, "像素就绪(预乘 BGRA)");
    ck(hn_rt_cmd_count(rt) > 0, "显示列表非空");

    char *t = hn_rt_text(rt, "out");
    ck(t && !strcmp(t, "hello"), "text 读取");
    free(t);

    char *dom = hn_rt_dom(rt, 3);
    ck(dom && strstr(dom, "<button"), "DOM 树含 button");
    free(dom);

    /* 合成事件: click → hx → sys:// → swap */
    ck(hn_rt_synthetic(rt, "click", "go") == 1, "synthetic click 触发 hx");
    t = hn_rt_text(rt, "out");
    ck(t && !strcmp(t, "rt"), "swap 后文本更新(=rt)");
    free(t);

    /* store 单源验证 */
    char v[64] = { 0 };
    ck(hn_rt_store_get("rt-test", "lang", v, sizeof(v)) && !strcmp(v, "rt"),
       "store_get 读回刚写入的值");

    /* shot(PNG 落盘) */
    ck(hn_rt_shot(rt, "/tmp/rt_shot.png"), "shot 落盘 PNG");

    /* headless 平台 */
    ck(hnp_init(0, NULL) == 1, "hnp_init");
    ck(!strcmp(hnp_name(), "headless"), "平台名");
    hnp_window_desc wd = { "t", 400, 300, 0, 1 };
    hnp_window *w = hnp_window_open(&wd);
    ck(w != NULL, "窗口创建(headless)");
    hnp_window_blit(w, hn_rt_pixels(rt), hn_rt_width(rt), hn_rt_height(rt),
                    (size_t)hn_rt_width(rt) * 4);
    hnp_event ev;
    ck(hnp_pump(&ev, 0) == 0 && ev.kind == HNP_EV_NONE, "pump 无事件");
    hnp_window_close(w);
    hnp_shutdown();

    hn_rt_close(rt);
    printf("== %s (%d 项失败) ==\n", fails ? "失败" : "全部通过", fails);
    return fails ? 1 : 0;
}

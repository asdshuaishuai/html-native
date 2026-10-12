/* timer_probe.c — JS 定时器与 RAF(帧驱动)全链路探针(确定性, 退出码判定)
 *
 * 管线与生产同一条: hn_rt_open(脚本装载 → setTimeout/requestAnimationFrame
 * 注册) → hn_rt_frame 帧驱动(虚拟单调钟: 每帧 += dt, 探针喂固定 16ms ⇒
 * "第 N 帧触发"可精确断言, 不受真实睡眠抖动影响) → 回调执行 → 桥
 * (hnSetText/hnCanvas2D)经 rt_render 落像素。
 *
 * 断言面:
 *   1. setTimeout: 3 帧×16ms 不触发, 第 4 帧触发一次; 触发后句柄失效;
 *      setTimeout(fn,0) 下一帧即触发; 参数展开最小版(...args);
 *   2. setInterval: 300ms 驱动恰好 ≈3 次(虚拟钟下精确 3); clearInterval
 *      后不再触发;
 *   3. RAF: 注册后下一帧恰好执行一次; 连续 RAF 每帧一次(待决节拍 16);
 *      多个按注册序; cancelAnimationFrame 生效; 回调里再挂 RAF = 下一帧;
 *   4. 回调纪律: 单帧最多 8 个回调(12 个 0ms 定时器分两帧跑完);
 *      异常回调不拖垮同帧后续回调;
 *   5. 桥联动: 定时器里 hnSetText → repaint 置位 + 文本真的变了;
 *      RAF 里画 canvas → 账本 op 落账;
 *   6. 节拍: RAF 待决 → 16; 定时器表非空 → min(250, 最近到期-现在)
 *      (上限治理: 表非空不再钉死 16ms —— 远期定时器 4Hz 醒来到期即触发);
 *      空表无动画 → -1(回归既有口径);
 *   7. hn_rt_close: 带活定时器/RAF/闭包环的表清理无崩溃;
 *   8. 动画分级节拍(性能治理): 仅 ambient(hn-mesh)活跃 → 33ms
 *      (HN_ANIM_FPS=60 → 16 / =16 → 62); interactive(@keyframes 无限循环)
 *      恒 16, 不受环境钮影响; ambient + 远期定时器 → 62(ambient 压过 250);
 *   9. GIF 缓存预算(内存治理): HN_GIF_CACHE_MB=1 下超限旧条目被逐出,
 *      条数与估算总占用有界(≤ 预算), 逐出后重访问可重解(不悬空)。
 *
 * 用法(py 薄封装编好即跑; 手工编译见 tools/timer_probe.py):
 *   bash tools/tools/timer_probe.sh
 * 断言失败打印 got/want, 退出码 1; 全绿退出 0。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn.h"
#include "hn_rt.h"
#include "hn_internal.h"   /* hn_context_anim_tick_ex/repaint_light 的宿主: GIF 缓存验收用 hn_context_gif/统计 */
#include "hn_gif.h"

static int g_pass = 0, g_fail = 0;

static void check(int cond, const char *label) {
    printf("%s %s\n", cond ? "  ✔" : "  ✘", label);
    if (cond) g_pass++; else g_fail++;
}

static void check_int(long got, long want, const char *label) {
    int ok = got == want;
    printf("%s %s (got %ld want %ld)\n", ok ? "  ✔" : "  ✘", label, got, want);
    if (ok) g_pass++; else g_fail++;
}

static void check_range(long got, long lo, long hi, const char *label) {
    int ok = got >= lo && got <= hi;
    printf("%s %s (got %ld want %ld..%ld)\n", ok ? "  ✔" : "  ✘", label, got, lo, hi);
    if (ok) g_pass++; else g_fail++;
}

/* 推进 n 帧(每帧 dt=16ms 虚拟步进), 返回最后一帧的节拍 */
static int drive(hn_rt *rt, int n) {
    int repaint = 0, beat = -1;
    for (int i = 0; i < n; i++)
        beat = hn_rt_frame(rt, 16.0f, &repaint);
    return beat;
}

/* eval 一段 JS, 返回整型结果(状态断言用: 计数器/句柄都是数字) */
static long eval_int(hn_rt *rt, const char *js) {
    char *r = hn_rt_eval(js, "timer-probe", hn_rt_doc(rt));
    long v = r ? strtol(r, NULL, 10) : -999999;
    free(r);
    return v;
}

/* 探针文档: 静态布局(无动画无 hx 轮询 ⇒ 节拍断言不受干扰) + 装载期
   注册一个 setTimeout(50)(clock=0 起, 第 4 帧到期)。 */
static const char *DOC_HTML =
"<html><head><style>body{margin:0}canvas{display:block}</style></head>"
"<body>"
"<div id=\"t1\">a</div>"
"<div id=\"t2\">b</div>"
"<canvas id=\"cv\" width=\"80\" height=\"40\"></canvas>"
"<script>\n"
"globalThis.hits = 0;\n"
"globalThis.ticks = 0;\n"
"globalThis.rafN = 0;\n"
"globalThis.rafC = 0;\n"
"globalThis.chain = 0;\n"
"globalThis.order = '';\n"
"globalThis.budgetN = 0;\n"
"globalThis.okAfterThrow = 0;\n"
"setTimeout(function(){ globalThis.hits++; }, 50);\n"
"</script>"
"</body></html>";

/* 分级探针文档(节拍断言互不干扰):
   AMBIENT: 只有 hn-mesh 环境摆动(ext_clock 类, 不写样式字段);
   KF:      只有 @keyframes 无限循环(每帧写样式字段 = interactive)。 */
static const char *AMBIENT_HTML =
"<html><body><div id=\"m\" hn-mesh=\"8x8\" hn-mesh-sway=\"6\" "
"style=\"width:80;height:80\"></div></body></html>";
static const char *KF_HTML =
"<html><head><style>"
"@keyframes spin { from { transform: rotate(0deg); } to { transform: rotate(360deg); } }"
"#k { width:40;height:40; animation: spin 1s linear infinite; }"
"</style></head><body><div id=\"k\">x</div></body></html>";

/* GIF 缓存预算夹具: 4x4 两帧(解码后 2*4*4*4 = 128B/条), 资产后端对
   任意路径都吐这份字节 —— 每个唯一路径解出独立 hn_gif, 小预算下即可
   验证"超限逐出最久未用 + 总占用有界"。字节与 tools/gif_probe.c 的
   GIF_DISP1 同源(生成器见其头部注释)。 */
static const unsigned char GIF_2F[] = {
    0x47, 0x49, 0x46, 0x38, 0x39, 0x61, 0x04, 0x00, 0x04, 0x00, 0x81, 0x00, 0x00, 0xff, 0x00, 0x00,
    0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0xff, 0x00, 0x21, 0xff, 0x0b, 0x4e, 0x45, 0x54, 0x53,
    0x43, 0x41, 0x50, 0x45, 0x32, 0x2e, 0x30, 0x03, 0x01, 0x00, 0x00, 0x00, 0x21, 0xf9, 0x04, 0x04,
    0x0a, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00, 0x04, 0x00, 0x81, 0xff, 0x00,
    0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0xff, 0x00, 0x02, 0x04, 0x84, 0x8f, 0x09, 0x05,
    0x00, 0x21, 0xf9, 0x04, 0x04, 0x14, 0x00, 0x00, 0x00, 0x2c, 0x00, 0x00, 0x00, 0x00, 0x04, 0x00,
    0x04, 0x00, 0x81, 0xff, 0x00, 0x00, 0x00, 0x00, 0xff, 0x00, 0xff, 0x00, 0xff, 0xff, 0x00, 0x02,
    0x04, 0x8c, 0x8f, 0x19, 0x05, 0x00, 0x3b,
};

static const char *gif_fixture_load(void *ctx, const char *path, size_t *len) {
    (void)ctx; (void)path;
    *len = sizeof(GIF_2F);
    return (const char *)GIF_2F;
}

int main(void) {
    printf("timer_probe — JS 定时器与 RAF(帧驱动: setTimeout/setInterval/requestAnimationFrame)\n\n");

    hn_rt_desc d = { "timer-probe", DOC_HTML, strlen(DOC_HTML), NULL, 0, 320, 200 };
    hn_rt *rt = hn_rt_open(&d);
    check(rt != NULL, "hn_rt_open 成功(文档 + 装载脚本已注册 setTimeout(50))");
    if (!rt) return 1;
    hn_context *c = hn_rt_context(rt);
    check(c != NULL, "hn_rt_context 就绪");

    /* == 1. setTimeout(fn,50): 3 帧×16ms 不触发, 第 4 帧触发一次 ==
       虚拟钟: 装载时刻 clock=0 → due=50; 帧 1..3 = 16/32/48(不触发),
       帧 4 = 64 ≥ 50 → 触发。帧 3 的节拍 = min(16, 50-48) = 2。 */
    printf("== setTimeout: 到期帧精确性与节拍 ==\n");
    int rp = -1;
    int b = hn_rt_frame(rt, 16.0f, &rp);
    check_int(eval_int(rt, "globalThis.hits"), 0, "帧 1(clock 16): 未触发");
    check_int(b, 34, "帧 1 节拍 = 34(min(250, 到期 50-16) — 定时器不钉死 16)");
    hn_rt_frame(rt, 16.0f, &rp);
    check_int(eval_int(rt, "globalThis.hits"), 0, "帧 2(clock 32): 未触发");
    b = hn_rt_frame(rt, 16.0f, &rp);
    check_int(eval_int(rt, "globalThis.hits"), 0, "帧 3(clock 48): 未触发");
    check_int(b, 2, "帧 3 节拍 = 2(min(250, 50-48) — 近期到期压低节拍)");
    b = hn_rt_frame(rt, 16.0f, &rp);
    check_int(eval_int(rt, "globalThis.hits"), 1, "帧 4(clock 64): 恰好触发一次");
    b = hn_rt_frame(rt, 16.0f, &rp);
    check_int(eval_int(rt, "globalThis.hits"), 1, "帧 5: 不重复(单次定时器已摘槽)");
    check_int(b, -1, "触发后节拍回落 -1(表已空)");
    long stale = eval_int(rt,
        "var h = setTimeout(function(){}, 5000); clearTimeout(h);"
        "clearTimeout(h); clearTimeout(0); clearTimeout(99999); 1");
    check_int(stale, 1, "失效句柄 clearTimeout 幂等(触发后/未知句柄/0 均静默)");
    hn_rt_frame(rt, 16.0f, &rp);   /* 5000ms 定时器已当场 clear, 本帧清场 */

    /* == 2. setTimeout(fn,0): 下一帧即触发(零延迟 = 下帧开头) == */
    printf("== setTimeout(fn,0) ==\n");
    eval_int(rt, "setTimeout(function(){ globalThis.hits += 10; }, 0);");
    hn_rt_frame(rt, 16.0f, &rp);
    check_int(eval_int(rt, "globalThis.hits"), 11, "0ms 定时器下一帧触发(+10)");

    /* == 3. setTimeout 参数展开最小版: setTimeout(fn, ms, ...args) == */
    printf("== setTimeout 参数展开 ==\n");
    eval_int(rt, "globalThis.sum = 0;"
                 "setTimeout(function(a, b, c){ globalThis.sum = a + b + c; }, 1, 3, 4, 5);");
    hn_rt_frame(rt, 16.0f, &rp);
    check_int(eval_int(rt, "globalThis.sum"), 12, "回调收到 (3,4,5) → sum 12");

    /* == 4. setInterval(fn,100): 从当前 clock 起 300ms 驱动恰好 3 次 ==
       虚拟钟确定性: due 每次触发顺延 100ms(不追发), 21 帧(336ms) → 3 次。 */
    printf("== setInterval: 周期触发与 clearInterval ==\n");
    long iv = eval_int(rt,
        "globalThis.__iv = setInterval(function(){ globalThis.ticks++; }, 100); globalThis.__iv");
    check(iv > 0, "setInterval 返回正句柄");
    drive(rt, 21);   /* clock +336ms */
    long ticks = eval_int(rt, "globalThis.ticks");
    check_range(ticks, 2, 4, "300ms 驱动 → 恰好 ≈3 次(容差 ±1)");
    check_int(ticks, 3, "虚拟钟下精确 3 次");
    eval_int(rt, "clearInterval(globalThis.__iv); 0");
    long before = eval_int(rt, "globalThis.ticks");
    drive(rt, 10);
    check_int(eval_int(rt, "globalThis.ticks"), before, "clearInterval 后不再触发");

    /* == 5. RAF: 下一帧恰好一次; 待决/续挂节拍 16; 注册序; cancel; 链式 == */
    printf("== requestAnimationFrame ==\n");
    eval_int(rt,
        "globalThis.rafN = 0;"
        "globalThis.__loop = function(){"
        "  globalThis.rafN++;"
        "  globalThis.__rh = requestAnimationFrame(globalThis.__loop);"
        "};"
        "globalThis.__rh = requestAnimationFrame(globalThis.__loop); 0");
    b = hn_rt_frame(rt, 16.0f, &rp);
    check_int(eval_int(rt, "globalThis.rafN"), 1, "RAF 下一帧恰好执行一次");
    check_int(b, 16, "RAF 待决(回调内续挂) → 节拍 16");
    hn_rt_frame(rt, 16.0f, &rp);
    check_int(eval_int(rt, "globalThis.rafN"), 2, "连续 RAF: 每帧一次(同帧不重复)");
    eval_int(rt, "cancelAnimationFrame(globalThis.__rh); 0");
    hn_rt_frame(rt, 16.0f, &rp);
    check_int(eval_int(rt, "globalThis.rafN"), 2, "cancelAnimationFrame 生效(不再执行)");
    b = hn_rt_frame(rt, 16.0f, &rp);
    check_int(b, -1, "RAF 队列空 → 节拍回到空闲");
    eval_int(rt,
        "globalThis.order = '';"
        "requestAnimationFrame(function(){ globalThis.order += 'a'; });"
        "requestAnimationFrame(function(){ globalThis.order += 'b'; }); 0");
    hn_rt_frame(rt, 16.0f, &rp);
    {
        char *r = hn_rt_eval("globalThis.order", "timer-probe", hn_rt_doc(rt));
        check(r && strcmp(r, "ab") == 0, "同一帧多个 RAF 按注册序(got 见上, want 'ab')");
        free(r);
    }
    hn_rt_frame(rt, 16.0f, &rp);
    eval_int(rt,
        "requestAnimationFrame(function f(){"
        "  globalThis.chain++;"
        "  if (globalThis.chain < 3) requestAnimationFrame(f);"
        "}); 0");
    drive(rt, 3);
    check_int(eval_int(rt, "globalThis.chain"), 3, "回调里再挂 RAF → 下一帧(链式 3 帧 3 次)");

    /* == 6. 回调纪律: 单帧 ≤8 个; 异常不拖垮同帧后续 == */
    printf("== 回调纪律(预算与异常隔离) ==\n");
    eval_int(rt, "globalThis.budgetN = 0;"
                 "for (var i = 0; i < 12; i++) setTimeout(function(){ globalThis.budgetN++; }, 0); 0");
    hn_rt_frame(rt, 16.0f, &rp);
    check_int(eval_int(rt, "globalThis.budgetN"), 8, "单帧最多 8 个回调(12 个顺延 4 个)");
    hn_rt_frame(rt, 16.0f, &rp);
    check_int(eval_int(rt, "globalThis.budgetN"), 12, "下一帧补完剩余 4 个");
    eval_int(rt,
        "setTimeout(function(){ throw new Error('boom-intentional'); }, 5);"
        "setTimeout(function(){ globalThis.okAfterThrow = 1; }, 6); 0");
    hn_rt_frame(rt, 16.0f, &rp);
    check_int(eval_int(rt, "globalThis.okAfterThrow"), 1,
              "异常回调吞掉打 stderr, 同帧后续回调照常执行");

    /* == 7. 桥联动: 定时器里 hnSetText → repaint 置位; RAF 里画 canvas == */
    printf("== 桥联动(repaint 管道) ==\n");
    eval_int(rt, "setTimeout(function(){ hnSetText('t1', 'A1'); }, 10); 0");
    rp = 0;
    hn_rt_frame(rt, 16.0f, &rp);
    check(rp == 1, "定时器回调触发 → repaint 置位");
    {
        char *txt = hn_rt_text(rt, "t1");
        check(txt && strcmp(txt, "A1") == 0, "hnSetText 真的改了文本(got 实测, want 'A1')");
        free(txt);
    }
    eval_int(rt,
        "requestAnimationFrame(function(){"
        "  var g = hnGet2D('cv'); g.fillStyle = '#ff0000'; g.fillRect(0, 0, 10, 10);"
        "}); 0");
    rp = 0;
    hn_rt_frame(rt, 16.0f, &rp);
    check(rp == 1, "RAF 回调触发 → repaint 置位");
    check_int(hn_canvas_count(c, "cv"), 1, "RAF 里的 canvas 绘制落账(1 op)");
    drive(rt, 2);

    /* == 8. 节拍口径(回归) == */
    printf("== 节拍口径 ==\n");
    b = hn_rt_frame(rt, 16.0f, &rp);
    check_int(b, -1, "空表无动画无轮询 → -1(既有空闲口径)");
    eval_int(rt, "setTimeout(function(){}, 20); 0");
    b = hn_rt_frame(rt, 16.0f, &rp);
    check_int(b, 4, "到期定时器: 节拍 = min(250, 20-16) = 4");
    hn_rt_frame(rt, 16.0f, &rp);   /* 跑掉它 */
    eval_int(rt, "globalThis.__far = setTimeout(function(){}, 5000); 0");
    b = hn_rt_frame(rt, 16.0f, &rp);
    check_int(b, 250, "远期定时器: 节拍 = min(250, 4984) = 250(治理: 表非空不再 60fps 空转)");
    eval_int(rt, "clearTimeout(globalThis.__far); 0");
    hn_rt_frame(rt, 16.0f, &rp);
    b = hn_rt_frame(rt, 16.0f, &rp);
    check_int(b, -1, "全清后回到 -1");

    hn_rt_close(rt);

    /* == 8b. 动画分级节拍(性能治理) ==
       仅 ambient(hn-mesh ext_clock)活跃 → 33ms 缺省, HN_ANIM_FPS 覆盖;
       interactive(@keyframes 无限循环, 每帧写样式)恒 16, 不受钮影响;
       ambient + 远期定时器并存 → 62(ambient 节拍压过定时器的 250)。 */
    printf("== 动画分级节拍(ambient 33 / interactive 16) ==\n");
    {
        hn_rt_desc ad = { "timer-probe-ambient", AMBIENT_HTML, strlen(AMBIENT_HTML), NULL, 0, 200, 120 };
        hn_rt *art = hn_rt_open(&ad);
        check(art != NULL, "hn_rt_open(ambient 文档)");
        if (art) {
            int rp = 0;
            int b0 = hn_rt_frame(art, 16.0f, &rp);
            check_int(b0, 33, "仅 ambient 活跃 → 节拍 33(缺省 ~30fps)");
            check(rp == 1, "ambient 帧仍报告 repaint(画面在动)");

            setenv("HN_ANIM_FPS", "60", 1);
            b0 = hn_rt_frame(art, 16.0f, &rp);
            check_int(b0, 16, "HN_ANIM_FPS=60 → ambient 节拍 16(回到 60fps)");
            setenv("HN_ANIM_FPS", "16", 1);
            b0 = hn_rt_frame(art, 16.0f, &rp);
            check_int(b0, 62, "HN_ANIM_FPS=16 → ambient 节拍 62");
            setenv("HN_ANIM_FPS", "30", 1);
            b0 = hn_rt_frame(art, 16.0f, &rp);
            check_int(b0, 33, "HN_ANIM_FPS=30 → ambient 节拍 33");
            unsetenv("HN_ANIM_FPS");
            b0 = hn_rt_frame(art, 16.0f, &rp);
            check_int(b0, 33, "未设环境钮 → 回落缺省 33");

            /* ambient + 远期定时器并存: ambient 33 压过定时器封顶 250,
               定时器到期仍精确触发(表非空只提供 min 上限信号)。 */
            hn_rt_close(art);
        }
        hn_rt *art2 = hn_rt_open(&ad);
        if (art2) {
            int rp = 0;
            /* 定时器必须注册到本 rt 的 scope(桥按 scope_key 反查宿主 rt) */
            char *r = hn_rt_eval("setTimeout(function(){}, 5000); 0",
                                 "timer-probe-ambient", hn_rt_doc(art2));
            free(r);
            int b0 = hn_rt_frame(art2, 16.0f, &rp);
            check_int(b0, 33, "ambient + 远期定时器 → 节拍 33(ambient 信号最小)");
            hn_rt_close(art2);
        }
        hn_rt_desc kd = { "timer-probe-kf", KF_HTML, strlen(KF_HTML), NULL, 0, 200, 120 };
        setenv("HN_ANIM_FPS", "16", 1);   /* 钮已放宽到 62ms —— interactive 不受影响 */
        hn_rt *krt = hn_rt_open(&kd);
        check(krt != NULL, "hn_rt_open(keyframes 文档)");
        if (krt) {
            int rp = 0;
            int b0 = hn_rt_frame(krt, 16.0f, &rp);
            check_int(b0, 16, "interactive(keyframes 无限循环) → 恒 16, 不受 HN_ANIM_FPS 影响");
            hn_rt_close(krt);
        }
        unsetenv("HN_ANIM_FPS");
    }

    /* == 8c. ambient 轻路径的像素正确性 ==
       GIF(纯 ambient)经 hn_rt_frame 的轻路径(免 relayout)渲染后,
       帧必须随 ext_clock 逐帧前进、逐帧位图可取且帧间不同 —— 轻路径
       省掉的是 relayout, 不是画面更新。 */
    printf("== ambient 轻路径像素(GIF 帧前进) ==\n");
    {
        const char *gif_path = "timer-probe-anim.gif";
        FILE *fp = fopen(gif_path, "wb");
        check(fp != NULL, "8c: GIF 夹具落盘(资产按 cwd 相对路径解析)");
        if (fp) { fwrite(GIF_2F, 1, sizeof(GIF_2F), fp); fclose(fp); }
        const char *GIFDOC =
            "<html><body><img id=\"g\" src=\"timer-probe-anim.gif\" "
            "width=\"64\" height=\"64\"></body></html>";
        hn_rt_desc gd = { "timer-probe-gif", GIFDOC, strlen(GIFDOC), NULL, 0, 96, 96 };
        hn_rt *grt = hn_rt_open(&gd);
        check(grt != NULL, "8c: hn_rt_open(GIF 文档)");
        if (grt) {
            int rp = 0;
            /* 位图缓冲在每次光栅后整体重分配 —— 先快照再推进, 别持旧指针 */
            unsigned char snap[96 * 96 * 4];
            const unsigned char *p0 = hn_rt_pixels(grt);
            check(p0 != NULL, "8c: 初始位图就绪(clock 0 → 帧 0)");
            if (p0) memcpy(snap, p0, sizeof(snap));
            int b0 = hn_rt_frame(grt, 150.0f, &rp);
            const unsigned char *p1 = hn_rt_pixels(grt);
            check_int(b0, 33, "8c: GIF 页节拍 33(ambient 轻路径)");
            check(rp == 1, "8c: 轻路径帧报告 repaint");
            check(p1 != NULL && memcmp(snap, p1, sizeof(snap)) != 0,
                  "8c: 帧 0 → 帧 1 位图真的变了(150ms 跨过 delay 100)");
            if (p1) memcpy(snap, p1, sizeof(snap));
            hn_rt_frame(grt, 150.0f, &rp);
            const unsigned char *p2 = hn_rt_pixels(grt);
            check(p2 != NULL && memcmp(snap, p2, sizeof(snap)) != 0,
                  "8c: 帧 1 → 帧 0 循环回绕(300ms 总时长取模)");
            hn_rt_close(grt);
        }
        remove(gif_path);
    }

    /* == 9. GIF 缓存预算(内存治理) ==
       HN_GIF_CACHE_MB=1: 128B/条夹具连灌 2 万条唯一路径 → 最久未用被
       逐出, 条数/估算总占用有界(≤ 预算), 旧条目重访问可重解。 */
    printf("== GIF 缓存预算(超限逐出, 总占用有界) ==\n");
    {
        setenv("HN_GIF_CACHE_MB", "1", 1);
        hn_context *gc = hn_context_create();
        const hn_asset_backend be = { NULL, gif_fixture_load };
        hn_context_set_assets(gc, &be);
        const int N = 20000;
        const size_t PER = (size_t)2 * 4 * 4 * 4;   /* 2 帧×4×4×4B = 128 */
        char p[64];
        for (int i = 0; i < N; i++) {
            snprintf(p, sizeof(p), "budget-fixture-%05d.gif", i);
            if (!hn_context_gif(gc, p)) break;
        }
        size_t bytes = hn_context_gif_bytes(gc);
        int cnt = hn_context_gif_count(gc);
        check(cnt < N, "超预算触发逐出(条数远小于灌入数)");
        check(bytes <= 1024 * 1024, "估算总占用有界(≤ 1MB 预算)");
        check((size_t)cnt * PER <= 1024 * 1024 + PER, "条数×单条占用 ≤ 预算 + 单条");
        /* 最旧条目已被逐出: 重访问 → 重解成功, 总占用仍有界 */
        hn_gif *old = hn_context_gif(gc, "budget-fixture-00000.gif");
        check(old != NULL && hn_gif_frame_count(old) == 2, "逐出条目重访问 → 重新解码(不悬空)");
        check(hn_context_gif_bytes(gc) <= 1024 * 1024, "重访问后总占用仍有界");
        hn_context_destroy(gc);
        unsetenv("HN_GIF_CACHE_MB");
    }

    /* == 9. close 纪律: 带活定时器/RAF/闭包环的表清理无崩溃无泄漏 == */
    printf("== hn_rt_close(持有 JS 引用的表清理) ==\n");
    {
        const char *LIVE_HTML =
            "<html><body><div id=\"x\">x</div><script>\n"
            "var selfref = function(){}; selfref.self = selfref;\n"   /* 闭包环 */
            "setInterval(function(){ hnSetText('x', 'tick'); }, 30);\n"
            "setInterval(selfref, 60);\n"
            "var rafloop = function(){ requestAnimationFrame(rafloop); };\n"
            "requestAnimationFrame(rafloop);\n"
            "</script></body></html>";
        hn_rt_desc ld = { "timer-probe-live", LIVE_HTML, strlen(LIVE_HTML), NULL, 0, 200, 120 };
        hn_rt *live = hn_rt_open(&ld);
        check(live != NULL, "hn_rt_open(带活定时器 + RAF + 闭包环)");
        hn_rt_frame(live, 16.0f, &rp);   /* 让 RAF/定时器至少跑过一次 */
        hn_rt_close(live);               /* 表里还有活引用 → 统一释放 */
        hn_rt_close(NULL);               /* NULL 安全 */
        check(1, "hn_rt_close 带活引用清理 + NULL 关闭(无崩溃)");
    }

    printf("\n结果: %d 通过, %d 失败\n", g_pass, g_fail);
    if (g_fail == 0) printf("全绿 — JS 定时器与 RAF 帧驱动链路健康\n");
    return g_fail ? 1 : 0;
}

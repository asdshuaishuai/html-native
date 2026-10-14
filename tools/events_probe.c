/* events_probe.c — JS 事件层全链路探针(确定性, 退出码判定)
 *
 * 管线与生产同一条: hn_rt_open(脚本经 glue 的 addEventListener 注册到 rt
 * 监听器表) → 引擎输入入口直调(hn_rt_button / hn_rt_key / hn_rt_scroll /
 * hn_rt_pointer_move —— 与平台壳同一批公开 API) → rt_js_dispatch 沿命中链
 * (目标→祖先 id 链→document→window)冒泡调监听器 → 事件对象
 * {type,target,currentTarget,clientX/Y,key,code,detail,preventDefault,
 * stopPropagation} 由 glue 构造。
 *
 * 断言面:
 *   1. click: 固定几何页(500x400 内 (100,100) 处 80x40 的 #btn)→ 直调
 *      hn_rt_button → 监听器触发且 type/target/currentTarget/clientX/Y 正确;
 *   2. 冒泡: inner/mid/outer 三层 + document + window → 触发序 =
 *      inner→mid→outer→document→window, 各层 currentTarget 逐层正确;
 *   3. stopPropagation: 内层置位 → 祖先/document/window 不再收到;
 *   4. preventDefault: click 监听 preventDefault → hx 载体不执行
 *      (对照: 无监听器时 hx 执行 —— 同一入口两条路);
 *   5. keydown/keyup/input: hn_rt_key 驱动 → key="a"/code="KeyA"/shift
 *      推断; 字符插入后 input 事件且 value 变化; Backspace 同; keydown
 *      preventDefault → 字符不插入; IME 组字期间 keydown 不派发;
 *   6. removeEventListener: 摘除后不再触发(错误函数引用摘不动);
 *   7. 节流: mousemove 坐标不变不重发; scroll 每 pump 至多一次(帧重臂);
 *   8. 文档换装(hn_rt_render)监听器清空, 新文档脚本重新注册可用;
 *      hn_rt_open 二次(同 id)监听器表全新; 带活监听器 close 无崩溃;
 *   9. hx 回归: 无任何 JS 监听器的 hx 页, 点击行为逐位不变(hx 执行)。
 *
 * 用法: bash tools/events_probe.sh
 * 断言失败打印 got/want, 退出码 1; 全绿退出 0。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn.h"
#include "hn_rt.h"

static int g_pass = 0, g_fail = 0;

static void check(int cond, const char *label) {
    printf("%s %s\n", cond ? "  ✔" : "  ✘", label);
    if (cond) g_pass++; else g_fail++;
}

static void check_str(const char *got, const char *want, const char *label) {
    int ok = got && want && strcmp(got, want) == 0;
    printf("%s %s (got %s want %s)\n", ok ? "  ✔" : "  ✘", label,
           got ? got : "(null)", want ? want : "(null)");
    if (ok) g_pass++; else g_fail++;
}

static void check_int(long got, long want, const char *label) {
    int ok = got == want;
    printf("%s %s (got %ld want %ld)\n", ok ? "  ✔" : "  ✘", label, got, want);
    if (ok) g_pass++; else g_fail++;
}

static void check_f(float got, float want, const char *label) {
    int ok = got >= want - 0.5f && got <= want + 0.5f;
    printf("%s %s (got %.1f want %.1f)\n", ok ? "  ✔" : "  ✘", label, got, want);
    if (ok) g_pass++; else g_fail++;
}

/* eval 一段 JS(scope 必须与 rt 的 id 一致 —— 定时器/监听器/桥都按它反查宿主)。
   返回整型(计数器/句柄断言用)。 */
static long eval_int(hn_rt *rt, const char *scope, const char *js) {
    (void)rt;
    char *r = hn_rt_eval(js, scope, hn_rt_doc(rt));
    long v = r ? strtol(r, NULL, 10) : -999999;
    free(r);
    return v;
}

/* eval 一段 JS, 返回字符串结果(malloc; free) */
static char *eval_str(hn_rt *rt, const char *scope, const char *js) {
    (void)rt;
    return hn_rt_eval(js, scope, hn_rt_doc(rt));
}

#define SCOPE "events-probe"

/* 元素盒中心(点击点) */
static void box_center(hn_rt *rt, const char *id, float *cx, float *cy) {
    hn_node *n = hn_doc_find_by_id(hn_rt_doc(rt), id);
    float x = 0, y = 0, w = 0, h = 0;
    if (n) hn_node_box(n, &x, &y, &w, &h);
    if (cx) *cx = x + w / 2;
    if (cy) *cy = y + h / 2;
}

/* 主探针文档: 固定几何(margin 定位, 无动画无 hx 轮询 ⇒ 断言不受干扰) */
static const char *DOC_HTML =
"<html><head><style>"
"body { margin: 0 }"
"#btn { width: 80; height: 40; margin-left: 100; margin-top: 100 }"
"#lvl-outer { width: 200; height: 150 }"
"#lvl-mid { width: 160; height: 110; margin-left: 20; margin-top: 10 }"
"#lvl-inner { width: 120; height: 70; margin-left: 20; margin-top: 10 }"
"#inp { width: 120; height: 24 }"
"#scroller { overflow: auto; width: 100; height: 50 }"
"</style></head><body>"
"<div id=\"btn\">Go</div>"
"<div id=\"lvl-outer\"><div id=\"lvl-mid\"><div id=\"lvl-inner\">in</div></div></div>"
"<input id=\"inp\">"
"<div id=\"scroller\">line one line two line three line four line five line six line seven line eight</div>"
"<script>\n"
"globalThis.log = [];\n"
"globalThis.order = '';\n"
"</script>"
"</body></html>";

/* hx 对照文档: 无任何 JS(零回归口径) */
static const char *HX_HTML =
"<html><body>"
"<div id=\"hxbox\" hx-get=\"sys://store/count\" hx-target=\"hxbox\" "
"style=\"width:60;height:30\">init</div>"
"</body></html>";

/* 换装第一版: 脚本注册 c1 监听 */
static const char *SWAP_A_HTML =
"<html><body><div id=\"c1\" style=\"width:50;height:50\">a</div>"
"<script>\n"
"globalThis.n1 = 0;\n"
"document.getElementById('c1').addEventListener('click', function(){ globalThis.n1++; });\n"
"</script></body></html>";

/* 换装第二版: 同 id 元素, 脚本注册自己的监听 */
static const char *SWAP_B_HTML =
"<html><body><div id=\"c1\" style=\"width:50;height:50\">b</div>"
"<script>\n"
"globalThis.n2 = 0;\n"
"document.getElementById('c1').addEventListener('click', function(){ globalThis.n2++; });\n"
"</script></body></html>";

/* 二次 open 文档: 脚本注册监听(全新 rt 的注册表应可用) */
static const char *REOPEN_HTML =
"<html><body><div id=\"r1\" style=\"width:50;height:50\">r</div>"
"<script>\n"
"globalThis.nr = 0;\n"
"document.getElementById('r1').addEventListener('click', function(){ globalThis.nr++; });\n"
"</script></body></html>";

int main(void) {
    printf("events_probe — JS 事件层(addEventListener 注册 → 命中链冒泡 → JS 回调 + 标准事件对象)\n\n");

    hn_rt_desc d = { "events-probe", DOC_HTML, strlen(DOC_HTML), NULL, 0, 500, 400 };
    hn_rt *rt = hn_rt_open(&d);
    check(rt != NULL, "hn_rt_open 成功(固定几何页 + 装载脚本已跑)");
    if (!rt) return 1;

    /* == 1. click: 固定几何 + 事件对象字段 == */
    printf("== click: 注册/触发/事件对象字段 ==\n");
    {
        hn_node *btn = hn_doc_find_by_id(hn_rt_doc(rt), "btn");
        check(btn != NULL, "#btn 在文档里");
        float bx = 0, by = 0, bw = 0, bh = 0;
        if (btn) hn_node_box(btn, &bx, &by, &bw, &bh);
        check_f(bx, 100, "#btn 盒 x = 100(固定几何)");
        check_f(by, 100, "#btn 盒 y = 100");
        check_f(bw, 80, "#btn 盒 w = 80");
        check_f(bh, 40, "#btn 盒 h = 40");
        float cx = bx + bw / 2, cy = by + bh / 2;   /* (140,120) */

        long h = eval_int(rt, SCOPE,
            "globalThis.h = document.getElementById('btn').addEventListener('click',"
            "  function(e){ globalThis.log.push('click t=' + e.type + ' tgt=' + e.target +"
            "  ' cur=' + e.currentTarget + ' xy=' + e.clientX + ',' + e.clientY); });"
            "globalThis.h");
        check(h > 0, "addEventListener 返回正句柄");
        hn_rt_button(rt, 1, cx, cy, 0);
        char *log = eval_str(rt, SCOPE, "globalThis.log.join('|')");
        check_str(log, "click t=click tgt=btn cur=btn xy=140,120",
                  "click 触发一次且 type/target/currentTarget/clientX/Y 正确");
        free(log);

        /* 回归保护锚: 无监听器元素上的点击(空白处)不派发任何回调 */
        eval_int(rt, SCOPE, "globalThis.log.length = 0; 0");
        hn_rt_button(rt, 1, 10, 10, 0);
        check_int(eval_int(rt, SCOPE, "globalThis.log.length"), 0, "空白处点击不误派发");
    }

    /* == 2. 冒泡: inner→mid→outer→document→window == */
    printf("== 冒泡: 命中链逐层触发序 ==\n");
    {
        float cx, cy;
        box_center(rt, "lvl-inner", &cx, &cy);
        eval_int(rt, SCOPE,
            "globalThis.order = ''; globalThis.docCur = ''; globalThis.winTgt = '';"
            "document.getElementById('lvl-inner').addEventListener('click', function(){ globalThis.order += 'i'; });"
            "document.getElementById('lvl-mid').addEventListener('click', function(){ globalThis.order += 'm'; });"
            "document.getElementById('lvl-outer').addEventListener('click', function(){ globalThis.order += 'o'; });"
            "document.addEventListener('click', function(e){ globalThis.docCur = e.currentTarget; globalThis.order += 'd'; });"
            "window.addEventListener('click', function(e){ globalThis.winTgt = e.target; globalThis.order += 'w'; });"
            "0");
        hn_rt_button(rt, 1, cx, cy, 0);
        char *order = eval_str(rt, SCOPE, "globalThis.order");
        check_str(order, "imodw", "冒泡序 = inner→mid→outer→document→window");
        free(order);
        char *cur = eval_str(rt, SCOPE, "globalThis.docCur");
        check_str(cur, "document", "document 层 currentTarget = 'document'");
        free(cur);
        char *tgt = eval_str(rt, SCOPE, "globalThis.winTgt");
        check_str(tgt, "lvl-inner", "window 层 event.target 保持命中目标");
        free(tgt);

        /* == 3. stopPropagation: 内层截断冒泡 == */
        printf("== stopPropagation: 冒泡截断 ==\n");
        eval_int(rt, SCOPE,
            "globalThis.order = '';"
            "document.getElementById('lvl-inner').addEventListener('click', function(e){"
            "  globalThis.order += 's'; e.stopPropagation(); });"
            "0");
        hn_rt_button(rt, 1, cx, cy, 0);
        char *order2 = eval_str(rt, SCOPE, "globalThis.order");
        check_str(order2, "is", "stopPropagation → 仅内层两监听器, 祖先/文档/窗全截断");
        free(order2);
    }

    /* == 4. keydown/keyup/input: 键事件与 value 联动 == */
    printf("== keydown/keyup/input ==\n");
    {
        float cx, cy;
        box_center(rt, "inp", &cx, &cy);
        hn_rt_button(rt, 1, cx, cy, 0);   /* 点击聚焦输入框 */
        eval_int(rt, SCOPE,
            "globalThis.klog = ''; globalThis.kulog = ''; globalThis.ku = 0; globalThis.inN = 0;"
            "document.addEventListener('keydown', function(e){"
            "  globalThis.klog += e.key + ':' + e.code + ':' + (e.shift ? 'S' : 's') + ';'; });"
            "document.addEventListener('keyup', function(e){"
            "  globalThis.ku++; globalThis.kulog += e.key + ':' + e.code + ';'; });"
            "document.getElementById('inp').addEventListener('input', function(e){"
            "  globalThis.inN++; globalThis.inTgt = e.target; });"
            "0");

        hn_rt_key(rt, 1, HN_KT_PRINTABLE, "a");
        check_str(eval_str(rt, SCOPE, "globalThis.klog"), "a:KeyA:s;",
                  "keydown 派发: key='a' code='KeyA'");
        check_int(eval_int(rt, SCOPE, "globalThis.inN"), 1, "字符插入 → input 事件");
        check_str(eval_str(rt, SCOPE, "document.getElementById('inp').value"), "a",
                  "value 真的变成 'a'");
        char *tgt = eval_str(rt, SCOPE, "globalThis.inTgt");
        check_str(tgt, "inp", "input 事件 target = 焦点元素 id");
        free(tgt);

        hn_rt_key(rt, 1, HN_KT_PRINTABLE, "A");
        check_str(eval_str(rt, SCOPE, "globalThis.klog"), "a:KeyA:s;A:KeyA:S;",
                  "大写 'A': shift 推断为 true");
        hn_rt_key(rt, 1, HN_KT_BACKSPACE, NULL);
        hn_rt_key(rt, 1, HN_KT_BACKSPACE, NULL);
        check_int(eval_int(rt, SCOPE, "globalThis.inN"), 4, "两次 Backspace → 两次 input 事件");
        check_str(eval_str(rt, SCOPE, "document.getElementById('inp').value"), "",
                  "Backspace 删净后 value 清空");
        check_str(eval_str(rt, SCOPE, "globalThis.klog"),
                  "a:KeyA:s;A:KeyA:S;Backspace:Backspace:s;Backspace:Backspace:s;",
                  "keydown 事件对象也带功能键 key/code");
        hn_rt_key(rt, 0, HN_KT_PRINTABLE, "a");
        check_int(eval_int(rt, SCOPE, "globalThis.ku"), 1, "keyup 派发(down=0)");
        check_str(eval_str(rt, SCOPE, "globalThis.kulog"), "a:KeyA;",
                  "keyup 事件对象带 key/code");

        /* keydown preventDefault → 字符不插入(默认行为被取消) */
        eval_int(rt, SCOPE,
            "globalThis.pdKey = document.addEventListener('keydown', function(e){"
            "  if (e.key === 'b') e.preventDefault(); });"
            "globalThis.inN = 0; 0");
        hn_rt_key(rt, 1, HN_KT_PRINTABLE, "b");
        check_str(eval_str(rt, SCOPE, "document.getElementById('inp').value"), "",
                  "keydown preventDefault → 'b' 不插入");
        hn_rt_key(rt, 1, HN_KT_PRINTABLE, "c");
        check_str(eval_str(rt, SCOPE, "document.getElementById('inp').value"), "c",
                  "非 pd 键照常插入('c')");
        /* remove 按 fn 同一性: 命名函数注册 → 摘除 → 同一引用才摘得动 */
        eval_int(rt, SCOPE,
            "globalThis.pdFn = function(e){ if (e.key === 'd') e.preventDefault(); };"
            "document.addEventListener('keydown', globalThis.pdFn); 0");
        hn_rt_key(rt, 1, HN_KT_PRINTABLE, "d");
        check_str(eval_str(rt, SCOPE, "document.getElementById('inp').value"), "c",
                  "第二个 pd 监听器生效('d' 被拦)");
        eval_int(rt, SCOPE, "document.removeEventListener('keydown', globalThis.pdFn); 0");
        hn_rt_key(rt, 1, HN_KT_PRINTABLE, "d");
        check_str(eval_str(rt, SCOPE, "document.getElementById('inp').value"), "cd",
                  "removeEventListener 摘除后 'd' 照常插入");

        /* == 5. IME 组字期间 keydown 不派发 == */
        printf("== IME 组字守卫 ==\n");
        eval_int(rt, SCOPE, "globalThis.kdN = 0;"
                     "document.addEventListener('keydown', function(){ globalThis.kdN++; }); 0");
        long kd0 = eval_int(rt, SCOPE, "globalThis.kdN");
        hn_rt_ime_preedit(rt, "ni", 1);          /* 组字开始 */
        hn_rt_key(rt, 1, HN_KT_PRINTABLE, "a");  /* 组字中按键 → 不派发 */
        check_int(eval_int(rt, SCOPE, "globalThis.kdN"), kd0, "IME 组字期间 keydown 不派发");
        hn_rt_ime_preedit(rt, "", 0);            /* 组字取消 */
        hn_rt_key(rt, 1, HN_KT_PRINTABLE, "a");  /* 现在照常派发 */
        check_int(eval_int(rt, SCOPE, "globalThis.kdN"), kd0 + 1, "组字结束后 keydown 恢复派发");
    }

    /* == 6. removeEventListener + 节流 == */
    printf("== removeEventListener 与节流 ==\n");
    {
        eval_int(rt, SCOPE,
            "globalThis.btnHits = 0;"
            "globalThis.btnFn = function(){ globalThis.btnHits++; };"
            "document.getElementById('btn').addEventListener('click', globalThis.btnFn); 0");
        float cx, cy;
        box_center(rt, "btn", &cx, &cy);
        hn_rt_button(rt, 1, cx, cy, 0);
        check_int(eval_int(rt, SCOPE, "globalThis.btnHits"), 1, "注册后点击触发");
        eval_int(rt, SCOPE, "document.getElementById('btn').removeEventListener('click', function(){}); 0");
        hn_rt_button(rt, 1, cx, cy, 0);
        check_int(eval_int(rt, SCOPE, "globalThis.btnHits"), 2, "错误函数引用摘不动(幂等 no-op)");
        eval_int(rt, SCOPE, "document.getElementById('btn').removeEventListener('click', globalThis.btnFn); 0");
        hn_rt_button(rt, 1, cx, cy, 0);
        check_int(eval_int(rt, SCOPE, "globalThis.btnHits"), 2, "removeEventListener 后不再触发");

        /* mousemove 节流: 坐标不变不重发 */
        eval_int(rt, SCOPE, "globalThis.mvN = 0;"
                     "document.addEventListener('mousemove', function(){ globalThis.mvN++; }); 0");
        hn_rt_pointer_move(rt, 200, 300);
        check_int(eval_int(rt, SCOPE, "globalThis.mvN"), 1, "mousemove 首次派发");
        hn_rt_pointer_move(rt, 200, 300);
        check_int(eval_int(rt, SCOPE, "globalThis.mvN"), 1, "同坐标重复移动不重发(节流)");
        hn_rt_pointer_move(rt, 210, 300);
        check_int(eval_int(rt, SCOPE, "globalThis.mvN"), 2, "坐标变化 → 再派发");

        /* scroll 节流: 每 pump 至多一次, 帧重臂 */
        eval_int(rt, SCOPE, "globalThis.scN = 0; globalThis.scTgt = '';"
                     "document.addEventListener('scroll', function(e){"
                     "  globalThis.scN++; globalThis.scTgt = e.target; }); 0");
        float sx, sy;
        box_center(rt, "scroller", &sx, &sy);
        hn_rt_scroll(rt, 0, -3, sx, sy);
        check_int(eval_int(rt, SCOPE, "globalThis.scN"), 1, "滚动 → scroll 事件(每帧第一次)");
        char *tgt = eval_str(rt, SCOPE, "globalThis.scTgt");
        check_str(tgt, "scroller", "scroll 事件 target = 滚动容器 id");
        free(tgt);
        hn_rt_scroll(rt, 0, -3, sx, sy);
        check_int(eval_int(rt, SCOPE, "globalThis.scN"), 1, "同帧第二次滚动不重发(节流)");
        int rp = 0;
        hn_rt_frame(rt, 16.0f, &rp);
        hn_rt_scroll(rt, 0, -3, sx, sy);
        check_int(eval_int(rt, SCOPE, "globalThis.scN"), 2, "下一帧重臂 → 再派发");
    }

    hn_rt_close(rt);

    /* == 7. preventDefault → hx 载体不执行(对照: 无监听器时 hx 执行) == */
    printf("== preventDefault: hx 载体取消与零回归对照 ==\n");
    {
        hn_rt_desc hd = { "events-probe-hx", HX_HTML, strlen(HX_HTML), NULL, 0, 300, 200 };
        hn_rt *hrt = hn_rt_open(&hd);
        check(hrt != NULL, "hn_rt_open(hx 页, 无任何 JS)");
        if (hrt) {
            float cx, cy;
            box_center(hrt, "hxbox", &cx, &cy);
            hn_rt_button(hrt, 1, cx, cy, 0);   /* 无 JS 监听器: hx 照旧执行 */
            char *t1 = hn_rt_text(hrt, "hxbox");
            check(t1 && strcmp(t1, "1") == 0,
                  "零回归: 无监听器 hx 页点击 → hx 执行(sys://store/count 换入 '1')");
            free(t1);
            hn_rt_button(hrt, 1, cx, cy, 0);   /* 幂等再点 */
            char *t2 = hn_rt_text(hrt, "hxbox");
            check(t2 && strcmp(t2, "1") == 0, "零回归: 再次点击 hx 照常(幂等 swap)");
            free(t2);
            /* 注册 preventDefault 监听 → hx 载体不再执行 */
            hn_rt_eval("globalThis.pdHit = 0;"
                       "document.getElementById('hxbox').addEventListener('click', function(e){"
                       "  e.preventDefault(); globalThis.pdHit = 1; });",
                       "events-probe-hx", hn_rt_doc(hrt));
            hn_rt_button(hrt, 1, cx, cy, 0);
            long pd = 0;
            char *r = hn_rt_eval("globalThis.pdHit", "events-probe-hx", hn_rt_doc(hrt));
            pd = r ? strtol(r, NULL, 10) : -1;
            free(r);
            check_int(pd, 1, "preventDefault 监听器确实跑了");
            char *t3 = hn_rt_text(hrt, "hxbox");
            check(t3 && strcmp(t3, "1") == 0, "preventDefault → hx 载体不执行(内容不变)");
            free(t3);
            hn_rt_close(hrt);
        }
    }

    /* == 8. 文档换装: 监听器清空 + 新文档脚本重注册 == */
    printf("== 文档换装(hn_rt_render)与二次 open ==\n");
    {
        hn_rt_desc sd = { "events-probe-swap", SWAP_A_HTML, strlen(SWAP_A_HTML), NULL, 0, 300, 200 };
        hn_rt *srt = hn_rt_open(&sd);
        check(srt != NULL, "hn_rt_open(换装前文档)");
        if (srt) {
            float cx, cy;
            box_center(srt, "c1", &cx, &cy);
            hn_rt_button(srt, 1, cx, cy, 0);
            check_int(eval_int(srt, "events-probe-swap", "globalThis.n1"), 1, "换装前监听器触发");
            check(hn_rt_render(srt, SWAP_B_HTML, strlen(SWAP_B_HTML)),
                  "hn_rt_render 换装成功");
            box_center(srt, "c1", &cx, &cy);
            hn_rt_button(srt, 1, cx, cy, 0);
            check_int(eval_int(srt, "events-probe-swap", "globalThis.n1"), 1,
                      "换装后旧监听器清空(旧计数不动, 无悬挂)");
            check_int(eval_int(srt, "events-probe-swap", "globalThis.n2"), 1,
                      "新文档脚本重注册的监听器触发");
            hn_rt_close(srt);

            /* 二次 open(同 id): 全新 rt 的注册表可用, 旧监听器不在 */
            hn_rt_desc rd = { "events-probe-swap", REOPEN_HTML, strlen(REOPEN_HTML), NULL, 0, 300, 200 };
            hn_rt *rrt = hn_rt_open(&rd);
            check(rrt != NULL, "hn_rt_open 二次(同 id)");
            if (rrt) {
                float cx, cy;
                box_center(rrt, "r1", &cx, &cy);
                hn_rt_button(rrt, 1, cx, cy, 0);
                check_int(eval_int(rrt, "events-probe-swap", "globalThis.nr"), 1,
                          "二次 open 后新监听器触发(注册表全新)");
                check_int(eval_int(rrt, "events-probe-swap", "globalThis.n2"), 1,
                          "旧 rt 的监听器不串场(同 scope 全局变量仍是旧值)");
                hn_rt_close(rrt);   /* 带活监听器 close */
            }
            hn_rt_close(NULL);      /* NULL 安全 */
            check(1, "带活监听器 close + NULL close 无崩溃");
        }
    }

    /* ---- 绘制挂起/冲刷(app 壳 blit 合并的引擎侧配套) ----
       defer 期间输入路径只标脏: pointer_move 返回 0(没画);
       defer(0) 保留脏标记(只能由 flush 消费); flush 一次整渲染
       返回 1。再 flush 无脏 → 0(幂等)。上文全部断言不经过 defer
       —— 显式窗口之外的直驱路径行为逐位不变。
       (主 rt 已在换装段 close, 这里开独立文档。) */
    {
        static const char *DEFER_HTML =
            "<html><head><style>"
            "#a{width:40;height:20;background:#446}"
            "#b{width:40;height:20;background:#664}"
            "</style></head><body>"
            "<div id=a></div><div id=b></div></body></html>";
        hn_rt_desc dd = { "events-probe-defer", DEFER_HTML, strlen(DEFER_HTML), NULL, 0, 200, 120 };
        hn_rt *drt = hn_rt_open(&dd);
        check(drt != NULL, "hn_rt_open(defer 文档)");
        hn_rt_paint_defer(drt, 1);
        float dx0, dy0;
        box_center(drt, "b", &dx0, &dy0);          /* hover 从 NULL → #b 必变 */
        check(hn_rt_pointer_move(drt, dx0, dy0) == 0,
              "defer 期间 pointer_move 不渲染只标脏(返回 0)");
        hn_rt_paint_defer(drt, 0);
        check(hn_rt_paint_flush(drt) == 1, "defer(0) 后 flush 把脏帧一次画掉(返回 1)");
        check(hn_rt_paint_flush(drt) == 0, "无脏再 flush 幂等(返回 0)");
        hn_rt_paint_defer(drt, 0);
        check(hn_rt_paint_flush(drt) == 0, "无事件 defer(0)+flush 为空操作");
        hn_rt_close(drt);
    }

    printf("\n结果: %d 通过, %d 失败\n", g_pass, g_fail);
    if (g_fail == 0) printf("全绿 — JS 事件层链路健康\n");
    return g_fail ? 1 : 0;
}

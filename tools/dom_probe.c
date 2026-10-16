/* dom_probe.c — JS DOM 标准面 + Canvas 标准扩展探针(确定性, 退出码判定)
 *
 * 管线与生产同一条: hn_rt_open(脚本经 EVENT_GLUE/CANVAS_GLUE 装载标准 API)
 * → hn_rt_eval 直接断言 JS 侧观察 → 关键处用 C 侧引擎读口(hn_node_text_content /
 * hn_node_attr)交叉验证"写透"(JS 改的确实是引擎 DOM, 不是 JS 侧影子)。
 *
 * 断言面:
 *   A. DOM: textContent get(含子树拼接) / get·set·has·removeAttribute /
 *      classList add·remove·toggle·contains / dataset camel→data-dash 写透 /
 *      style 代理写透(el.style.color → style 属性可查) / checked·disabled /
 *      querySelector(tag/#id/.class/复合/后代)与 querySelectorAll 计数 /
 *      createElement + appendChild + getElementById 读回 + remove() /
 *      children/parentElement/firstElementChild / getElementById null 语义
 *   B. Canvas: rect+stroke 像素 / roundRect 四角凸圆(角像素透明·中心实) /
 *      globalAlpha 落像素 / createImageData+putImageData 往返 /
 *      getTransform·setTransform 往返 / isPointInPath 内外 / toDataURL PNG 头
 *
 * 用法: bash tools/dom_probe.sh
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

static long eval_int(hn_rt *rt, const char *scope, const char *js) {
    char *r = hn_rt_eval(js, scope, hn_rt_doc(rt));
    long v = r ? strtol(r, NULL, 10) : -999999;
    free(r);
    return v;
}

static char *eval_str(hn_rt *rt, const char *scope, const char *js) {
    (void)rt;
    return hn_rt_eval(js, scope, hn_rt_doc(rt));
}

/* C 侧交叉验证: 引擎节点属性事实源 */
static int engine_attr_is(hn_rt *rt, const char *id, const char *attr, const char *want) {
    hn_node *n = hn_doc_find_by_id(hn_rt_doc(rt), id);
    if (!n) return 0;
    const char *v = hn_node_attr(n, attr);
    return v && strcmp(v, want) == 0;
}

#define SCOPE "dom-probe"

static void test_dom(hn_rt *rt) {
    printf("== DOM 标准面 ==\n");
    char *s;
    s = eval_str(rt, SCOPE, "var t=document.getElementById('t'); t.textContent");
    check_str(s, "外内", "textContent get 含子树拼接");
    free(s);
    s = eval_str(rt, SCOPE, "t.getAttribute('data-k')");
    check_str(s, "v1", "getAttribute");
    free(s);
    check_int(eval_int(rt, SCOPE,
        "t.setAttribute('data-j','w'); t.hasAttribute('data-j') ? 1 : 0"), 1,
        "setAttribute + hasAttribute");
    check(engine_attr_is(rt, "t", "data-j", "w"), "setAttribute 写透引擎 DOM(C 侧验证)");
    check_int(eval_int(rt, SCOPE,
        "t.removeAttribute('data-j'); t.hasAttribute('data-j') ? 0 : 1"), 1,
        "removeAttribute");
    check_int(eval_int(rt, SCOPE,
        "t.classList.add('on'); t.classList.contains('on') ? 1 : 0"), 1,
        "classList.add + contains");
    check_int(eval_int(rt, SCOPE,
        "t.classList.toggle('on'), t.classList.contains('on') ? 0 : 1"), 1,
        "classList.toggle(移除)");
    check_int(eval_int(rt, SCOPE,
        "t.classList.add('a','b'); (t.classList.contains('a')&&t.classList.contains('b')) ? 1 : 0"), 1,
        "classList.add 多参");
    check(engine_attr_is(rt, "t", "class", "box a b"), "classList 写透 class 属性(C 侧验证)");
    check_int(eval_int(rt, SCOPE,
        "t.dataset.state='ready'; t.getAttribute('data-state')==='ready' ? 1 : 0"), 1,
        "dataset camel→data-dash 写透");
    check_str(engine_attr_is(rt, "t", "data-state", "ready") ? "ready" : "-", "ready",
              "dataset 属性名拆线(C 侧验证)");
    check_int(eval_int(rt, SCOPE,
        "t.style.color='red'; t.style.width='120px';"
        "t.getAttribute('style').indexOf('color: red')>=0 ? 1 : 0"), 1,
        "style 代理写透 style 属性");
    eval_str(rt, SCOPE, "t.style.color='blue'");
    s = eval_str(rt, SCOPE, "t.style.color");
    check_str(s, "blue", "style 代理读回(二次写)");
    free(s);
    s = eval_str(rt, SCOPE, "t.style.cssText");
    check_str(s, "color: blue; width: 120px", "style cssText 归一(插入序)");
    free(s);
    check_int(eval_int(rt, SCOPE,
        "var ck=document.getElementById('ck'); ck.checked=false, ck.checked ? 0 : 1"), 1,
        "checked 属性读写");
    check_int(eval_int(rt, SCOPE,
        "ck.disabled=true, ck.disabled ? 1 : 0"), 1, "disabled 属性写");
    /* querySelector 家族 */
    s = eval_str(rt, SCOPE, "document.querySelector('em').id");
    check_str(s, "em", "querySelector(tag)");
    free(s);
    s = eval_str(rt, SCOPE, "document.querySelector('.box.em').id");
    check_str(s, "em", "querySelector(复合 .class.class)");
    free(s);
    s = eval_str(rt, SCOPE, "document.querySelector('#p em').id");
    check_str(s, "em2", "querySelector(后代组合)");
    s = eval_str(rt, SCOPE, "document.querySelector('nothing-here')===null ? 'null' : 'hit'");
    check_str(s, "null", "querySelector 未命中 → null");
    free(s);
    check_int(eval_int(rt, SCOPE, "document.querySelectorAll('#p *').length"), 1,
        "querySelectorAll 计数");
    /* 选择器引擎新伪类经查询口可达 */
    check_int(eval_int(rt, SCOPE,
        "document.querySelector(':root')!==null ? 1 : 0"), 1, "querySelector(:root)");
    check_int(eval_int(rt, SCOPE,
        "document.querySelector('#only')!==null ? 1 : 0"), 1, "querySelector(#id) 命中");
    check_int(eval_int(rt, SCOPE,
        "document.querySelector('em:only-child').id==='em' ? 1 : 0"), 1,
        "querySelector(:only-child 伪类)");
    /* 动态树 */
    check_int(eval_int(rt, SCOPE,
        "var d=document.createElement('div'); d.id='dyn';"
        "d.textContent='D'; document.body.appendChild(d);"
        "document.getElementById('dyn').textContent==='D' ? 1 : 0"), 1,
        "createElement + appendChild + getElementById 读回");
    check_int(eval_int(rt, SCOPE,
        "var d2=document.getElementById('dyn'); d2.remove(),"
        "document.getElementById('dyn')===null ? 1 : 0"), 1,
        "remove() + getElementById null 语义");
    check_int(eval_int(rt, SCOPE,
        "document.getElementById('no-such-el')===null ? 1 : 0"), 1,
        "getElementById 不存在 → null");
    /* 树关系 */
    check_int(eval_int(rt, SCOPE,
        "var em=document.getElementById('em'); em.parentElement.id==='t' ? 1 : 0"), 1,
        "parentElement");
    check_int(eval_int(rt, SCOPE,
        "document.getElementById('p').children.length"), 1, "children 计数");
    s = eval_str(rt, SCOPE, "document.getElementById('p').firstElementChild.id");
    check_str(s, "em2", "firstElementChild");
    free(s);
    check(engine_attr_is(rt, "t", "class", "box a b"), "树操作后属性稳定(回归)");
}

static void test_canvas(hn_rt *rt) {
    printf("== Canvas 标准扩展 ==\n");
    char *s;
    /* 基线: clearRect 全清后按用例画 */
    eval_str(rt, SCOPE,
        "var cv=document.getElementById('cv'); var c=cv.getContext('2d');"
        "c.clearRect(0,0,200,120); 'ok'");
    /* rect + stroke: (150..190, 20..60) 竖线在 x=150 */
    eval_str(rt, SCOPE,
        "c.strokeStyle='#ff0000'; c.lineWidth=2;"
        "c.beginPath(); c.rect(150,20,40,40); c.stroke(); 'ok'");
    s = eval_str(rt, SCOPE,
        "var im=c.getImageData(150,40,1,1).data; im[0]>200&&im[1]<60 ? 'red' : 'no'");
    check_str(s, "red", "rect()+stroke 落像素");
    free(s);
    /* roundRect: (10,10,80,50,r=10) — 角像素透明, 中心实 */
    eval_str(rt, SCOPE,
        "c.clearRect(0,0,200,120);"
        "c.fillStyle='#00cc44'; c.beginPath(); c.roundRect(10,10,80,50,10); c.fill(); 'ok'");
    s = eval_str(rt, SCOPE,
        "var a=c.getImageData(11,11,1,1).data[3], b=c.getImageData(50,35,1,1).data[3];"
        "(a<128&&b>200) ? 'ok' : 'bad:'+a+','+b");
    check_str(s, "ok", "roundRect 四角削角 + 中心实");
    free(s);
    /* globalAlpha 落像素 */
    eval_str(rt, SCOPE,
        "c.clearRect(0,0,200,120); c.globalAlpha=0.5; c.fillStyle='#ff0000';"
        "c.fillRect(10,10,60,40); c.globalAlpha=1; 'ok'");
    s = eval_str(rt, SCOPE,
        "var a=c.getImageData(30,30,1,1).data[3]; (a>110&&a<150) ? 'ok' : 'bad:'+a");
    check_str(s, "ok", "globalAlpha 半透明落像素");
    free(s);
    /* createImageData + putImageData 往返 */
    eval_str(rt, SCOPE,
        "c.clearRect(0,0,200,120);"
        "var im=c.createImageData(4,4); im.data[0]=255;im.data[1]=128;im.data[2]=0;im.data[3]=255;"
        "c.putImageData(im,50,50); 'ok'");
    s = eval_str(rt, SCOPE,
        "var d=c.getImageData(50,50,1,1).data; (d[0]===255&&d[1]===128&&d[2]===0) ? 'ok' : 'bad'");
    check_str(s, "ok", "createImageData + putImageData 往返(块内已设像素)");
    free(s);
    /* 变换往返 + 旋转下的 fillRect 落点 */
    eval_str(rt, SCOPE,
        "c.clearRect(0,0,200,120); c.setTransform(1,0,0,1,0,0);"
        "var m1=c.getTransform(); c.setTransform(2,0,0,2,10,5);"
        "var m2=c.getTransform();"
        "(m1.a===1&&m2.a===2&&m2.e===10&&m2.f===5) ? 'ok' : 'bad'");
    s = eval_str(rt, SCOPE,
        "var m=c.getTransform(); (m.a===2&&m.e===10) ? 'ok' : 'bad'");
    check_str(s, "ok", "getTransform/setTransform 往返");
    free(s);
    eval_str(rt, SCOPE, "c.setTransform(1,0,0,1,0,0); 'ok'");
    /* isPointInPath: 画一个矩形路径后内外各测一点 */
    eval_str(rt, SCOPE,
        "c.clearRect(0,0,200,120); c.fillStyle='#123456';"
        "c.beginPath(); c.rect(20,20,60,40); c.fill(); 'ok'");
    check_int(eval_int(rt, SCOPE,
        "(c.isPointInPath(30,30)?1:0)+(c.isPointInPath(150,80)?0:2)"), 3,
        "isPointInPath 内外(像素近似)");
    /* toDataURL */
    s = eval_str(rt, SCOPE,
        "var u=c.toDataURL(); (u&&u.slice(0,22)==='data:image/png;base64,'&&u.length>100) ? 'png' : 'bad'");
    check_str(s, "png", "toDataURL PNG data URL");
    free(s);
}

int main(void) {
    const char *html =
        "<!doctype html><html><head><style>"
        "canvas{background:#ffffff}"
        "</style></head><body>"
        "<div id=\"t\" class=\"box\" data-k=\"v1\">外<em id=\"em\" class=\"box em\">内</em></div>"
        "<p id=\"p\"><em id=\"em2\">x</em></p>"
        "<div id=\"only\"></div>"
        "<input type=\"checkbox\" id=\"ck\" checked>"
        "<canvas id=\"cv\" width=\"200\" height=\"120\"></canvas>"
        "</body></html>";
    hn_rt_desc d = { "dom-probe", html, strlen(html), NULL, 0, 480, 400 };
    hn_rt *rt = hn_rt_open(&d);
    if (!rt) { fprintf(stderr, "dom_probe: rt open failed\n"); return 1; }

    test_dom(rt);
    test_canvas(rt);

    hn_rt_close(rt);
    printf("\n结果: %d 通过, %d 失败\n", g_pass, g_fail);
    if (g_fail == 0) printf("全绿 — DOM/Canvas 标准面健康\n");
    return g_fail ? 1 : 0;
}

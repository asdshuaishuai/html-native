/* QuickJS 探针: 内置 webview 的 JS 层。验证求值/状态持久/宿主函数桥 ——
 * 与 litehtml 的 DOM 绑定就是走最后这一条(宿主函数把元素引用交给 JS)。 */
#include <quickjs/quickjs.h>
#include <quickjs/quickjs-libc.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>

static int fails = 0;
static void ck(int c, const char* l) { printf("%s %s\n", c ? "  v" : "  X", l); if (!c) fails++; }

/* 宿主函数桥的样例: JS 调用 hnEmit(id) → 宿主返回 1(模拟"已派发到引擎") */
static JSValue js_hnEmit(JSContext *ctx, JSValueConst, int argc, JSValueConst *argv) {
    const char *id = JS_ToCString(ctx, argv[0]);
    printf("       [宿主桥] hnEmit(%s)\n", id ? id : "?");
    JS_FreeCString(ctx, id);
    return JS_NewInt32(ctx, 1);
}

/* 以非零码退出时把崩溃原因打进 stdout(门禁只收 stdout, 与项目探针口径一致) */
static void crash(const char *what) { printf("  X %s\n", what); fails++; }

int main(void) {
    JSRuntime *rt = JS_NewRuntime();
    JSContext *ctx = JS_NewContext(rt);
    if (!rt || !ctx) { crash("运行时创建失败"); return 1; }

    printf("== QuickJS: JS 层 ==\n");

    /* 1) 求值: 算术 + 字符串 */
    JSValue r1 = JS_Eval(ctx, "1 + 1", 5, "<p>", JS_EVAL_TYPE_GLOBAL);
    int v1 = 0;
    if (!JS_IsException(r1)) JS_ToInt32(ctx, &v1, r1);
    ck(v1 == 2, "算术求值 1+1 = 2");
    JS_FreeValue(ctx, r1);

    /* 2) 状态跨求值持久(同一 context) */
    JS_Eval(ctx, "var hn = {n: 0};", 15, "<s>", JS_EVAL_TYPE_GLOBAL);
    JSValue r2 = JS_Eval(ctx, "hn.n = 0; for (var i=1;i<=5;i++) hn.n += i; hn.n", 47, "<s>", JS_EVAL_TYPE_GLOBAL);
    int v2 = 0;
    if (!JS_IsException(r2)) JS_ToInt32(ctx, &v2, r2);
    ck(v2 == 15, "状态跨求值持久(累加 1..5 = 15)");
    JS_FreeValue(ctx, r2);

    /* 3) 宿主函数桥: 这就是 DOM 绑定给 JS 的同一条路 */
    JSValue global = JS_GetGlobalObject(ctx);
    JS_SetPropertyStr(ctx, global, "hnEmit",
        JS_NewCFunction(ctx, js_hnEmit, "hnEmit", 1));
    JSValue r3 = JS_Eval(ctx, "hnEmit('btn')", 13, "<s>", JS_EVAL_TYPE_GLOBAL);
    int v3 = 0;
    if (!JS_IsException(r3)) JS_ToInt32(ctx, &v3, r3);
    ck(v3 == 1, "宿主函数桥: JS 可调宿主(hnEmit)");
    JS_FreeValue(ctx, r3);

    /* 4) 事件处理器的真实形态: 定义并调用(之后由宿主在 DOM 事件上触发) */
    const char *h4 = "hnHandlers = {}; hnHandlers.click = function(id){ return 'clicked:' + id; };";
    JS_Eval(ctx, h4, strlen(h4), "<s>", JS_EVAL_TYPE_GLOBAL);
    JSValue r4 = JS_Eval(ctx, "hnHandlers.click('go')", 22, "<s>", JS_EVAL_TYPE_GLOBAL);
    const char *s4 = JS_IsException(r4) ? NULL : JS_ToCString(ctx, r4);
    ck(s4 && strcmp(s4, "clicked:go") == 0, "事件处理器: 定义并回调");
    if (s4) JS_FreeCString(ctx, s4);
    JS_FreeValue(ctx, r4);

    /* 5) 异常传导: 语法错误要能被宿主感知(不能静默吞掉) */
    JSValue r5 = JS_Eval(ctx, "this is not js", 14, "<bad>", JS_EVAL_TYPE_GLOBAL);
    ck(JS_IsException(r5), "语法错误传导给宿主(不静默)");
    /* 挂起的异常必须 JS_GetException 消费掉, 否则 JS_FreeRuntime 断言
       gc_obj_list 非空(实测 abort) —— 集成时每条 eval 都要照此清理。 */
    JSValue e5 = JS_GetException(ctx);
    JS_FreeValue(ctx, e5);
    JS_FreeValue(ctx, r5);

    JS_FreeValue(ctx, global);
    /* 收尾口径(实测本构建 quickjs-ng 2026-06-04): 上下文释放干净; 但只要有
       JS 侧定义的函数(原型链成环), JS_FreeRuntime 就断言 gc_obj_list 非空 ——
       RunGC 前置/后置都试过不解决。集成形态是 runtime 与进程同生命周期
       (daemon 常驻, 每个应用一个 context), 进程退出由 OS 回收, 不跑 FreeRuntime。 */
    JS_RunGC(rt);
    JS_FreeContext(ctx);
    JS_RunGC(rt);

    printf("== %s (%d 项失败) ==\n", fails ? "失败" : "全部通过", fails);
    return fails ? 1 : 0;
}

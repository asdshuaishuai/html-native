/* hn_canvas.c — <canvas> 2D 账本(QuickJS 桥的落账侧)
 *
 * 分工(与 Skia canvaskit 的 htmlcanvas 同构, 但状态机在 JS glue 里):
 *   JS 层( rt/hn_rt.c 的 glue )—— fillStyle/strokeStyle 颜色解析、
 *     save/restore 栈、当前路径、CTM 2x3、渐变对象; 每个绘制调用在这里
 *     解析成"已解析的原语 op"(坐标已 CTM 变换、颜色已 RGBA), 序列化为
 *     JSON 调 hnCanvas2D 桥。
 *   本文件 —— 桥的落账侧: 解析 op JSON, 按元素 id 记入账本(定长数组,
 *     上限 4096, 满则丢弃新 op 并置溢出标志 + 打一行 stderr 提示);
 *     提供清空/内省与销毁纪律。
 *   hn_paint.c —— 绘制 <canvas> 时把账本重放成显示列表指令。
 *
 * **账本有界化: 满幅不透明 fillRect 视同清账**(详见 frect_frame_reset):
 *   canvas 动画的惯用法是"每帧 fillRect 盖底"(不调 clearRect)—— 若账本
 *   只在显式 clear/全幅 clearRect 时重置, 账本会以整帧 op 数/帧 无界增长
 *   (canvas-demo 实测 56 op/tick 线性涨, 80 tick 撞 4096 上限后新帧全被
 *   丢弃, 画面冻结)。因此落账前判定: 不透明 + 满幅覆盖 + 无激活裁剪的
 *   frect = "新的一帧", 清空账本后仅记录该 op(它就是新底色)。账本从此
 *   稳态有界 = 底色 op + 增量 op, 与浏览器 canvas 的视觉语义一致。
 *
 * hnCanvas2D op 协议(JSON 对象, 每次一条):
 *   {"cv":"<canvas id>","op":"frect","x","y","w","h","col",
 *                       "shc"?,"shx"?,"shy"?,"shb"?}                   矩形填充(可带阴影)
 *   {"cv":..,"op":"spath","pts":[x0,y0,..],"col","w","cap",
 *              渐变可选: "radial","x0","y0","x1","y1","r0","r1","c0","c1","stops"}  开放折线描边
 *   {"cv":..,"op":"fpath","pts":[..],"col"}                            闭合多边形填充
 *   {"cv":..,"op":"grect","x","y","w","h","radial"?,
 *               "x0","y0","x1","y1"[,"r0","r1"],"c0","c1","stops"?}    渐变矩形
 *   {"cv":..,"op":"gpath","pts":[..],"radial"?,"x0",..,"c0","c1","stops"?}  渐变多边形填充
 *   {"cv":..,"op":"clip","x","y","w","h"}                              矩形裁剪入栈
 *   {"cv":..,"op":"cpop"}                                              裁剪出栈
 *   {"cv":..,"op":"text","s":"..","x","y","px","col"}                  文本(x, 基线 y)
 *   {"cv":..,"op":"image","src":"..","x","y","w","h"[,"sx","sy","sw","sh"]}  图片
 *              (源子矩形四字段齐备 = 9 参 drawImage: 源图像素 (sx,sy,sw,sh))
 *   {"cv":..,"op":"pdata","x","y","w","h","px":[r,g,b,a,...],
 *              "dx"?,"dy"?,"dw"?,"dh"?}                                putImageData(RGBA 块
 *              非预乘; 脏矩形可选; 块字节上限 HN_CV_PDATA_MAX_BYTES)
 *   {"cv":..,"op":"prect","src":"..","rep","x","y","w","h"}            pattern 平铺填充矩形
 *   {"cv":..,"op":"ppath","src":"..","rep","pts":[..]}                 pattern 平铺填充多边形
 *   {"cv":..,"op":"cpath","pts":[..],"segs":[n0,n1,..]?}               精确路径裁剪入栈
 *              (segs 缺省 = 单多边形; 与 cpop 配对, 计裁剪深度)
 *   {"cv":..,"op":"clear"}                                             清空账本
 *   {"cv":..,"op":"clear_rect","x","y","w","h"}                        局部清空(见下)
 * 颜色一律 0xRRGGBBAA 的十进制数(JS 侧 `>>>0`), 坐标已含 CTM。
 * 绘制类 op 可带 "blend":"source-over|lighter|multiply|screen"
 * (globalCompositeOperation 子集; 未列模式回落 source-over 并提示一次)。
 * 特例: frect 满足"不透明 + 覆盖画布全幅 + 无激活裁剪"时落账前先清账本
 * (视同新帧, 见 frect_frame_reset) —— 账本稳态有界的关键; pdata 以
 * "块内被贴像素全不透明 + 覆盖全幅 + 无裁剪"同一口径清账(pdata_frame_reset)。
 * 渐变字段口径: linear 时 x0,y0→x1,y1 为轴两端; "radial":1 时为双圆
 * (x0,y0,r0)内圆 → (x1,y1,r1)外圆(CSS createRadialGradient 全口径);
 * "stops" 是扁平数组 [t0,c0,t1,c1,...](升序给定; t∈[0,1]) —— 给了 stops
 * 就走多 stop 分段线性, 否则退回首末两端色(向后兼容)。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn_internal.h"
#include "hn_json.h"

/* 每条 op 的顶点数上限(arc 折线化 15° 一段, 全圆也只有 24 段 = 25 点;
   上限防的是脚本写出失控大路径 —— 到顶截断, 不崩溃)。 */
#define HN_CANVAS_MAX_PTS 8192

/* strdup 是 POSIX 而非 C99 —— 与 hn_context.c / hn_media.c 同一份本地实现 */
static char *cv_strdup(const char *s) {
    size_t n = strlen(s) + 1;
    char *p = (char *)malloc(n);
    if (p) memcpy(p, s, n);
    return p;
}

/* ---------------- 账本表 ---------------- */

hn_canvas_ledger *hn_canvas_ledger_find(hn_context *c, const char *id) {
    if (!c || !id) return NULL;
    for (int i = 0; i < c->n_canvases; i++)
        if (!strcmp(c->canvases[i].id, id)) return &c->canvases[i];
    return NULL;
}

hn_canvas_ledger *hn_canvas_ledger_get(hn_context *c, const char *id) {
    hn_canvas_ledger *l = hn_canvas_ledger_find(c, id);
    if (l) return l;
    if (!id[0]) return NULL;
    if (c->n_canvases == c->cap_canvases) {
        int nc = c->cap_canvases ? c->cap_canvases * 2 : 4;
        hn_canvas_ledger *nv =
            (hn_canvas_ledger *)realloc(c->canvases, sizeof(hn_canvas_ledger) * (size_t)nc);
        if (!nv) return NULL;
        c->canvases = nv;
        c->cap_canvases = nc;
    }
    l = &c->canvases[c->n_canvases];
    memset(l, 0, sizeof(*l));
    l->id = cv_strdup(id);
    if (!l->id) return NULL;
    c->n_canvases++;
    return l;
}

/* 释放单条 op 持有的堆内存 */
static void op_free(hn_canvas_op *o) {
    free(o->pts);
    free(o->str);
    free(o->stops);
    free(o->pdata);
    free(o->segs);
    memset(o, 0, sizeof(*o));
}

/* 清空账本(保留槽位本身; clearRect / 销毁 / 满幅不透明 frect 新帧共用) */
static void ledger_reset(hn_canvas_ledger *l) {
    for (int i = 0; i < l->n_ops; i++) op_free(&l->ops[i]);
    l->n_ops = 0;
    l->overflow = 0;
}

void hn_canvas_dispose(hn_context *c) {
    if (!c) return;
    for (int i = 0; i < c->n_canvases; i++) {
        ledger_reset(&c->canvases[i]);
        free(c->canvases[i].id);
        free(c->canvases[i].ops);
    }
    free(c->canvases);
    c->canvases = NULL;
    c->n_canvases = c->cap_canvases = 0;
}

/* ---------------- op 解析 ---------------- */

/* 顶点数组: JSON 数组 [x0,y0,x1,y1,...] → malloc float 数组 */
static int parse_pts(const hn_json *o, float **out, int *n_out) {
    const hn_json *arr = hn_json_obj_get(o, "pts");
    if (!arr || arr->kind != HN_JSON_ARR) return 0;
    int n = arr->count;
    if (n < 2) return 0;             /* 至少一个点 */
    if (n % 2) n--;                  /* 奇数长度: 丢弃尾巴(坏输入不崩溃) */
    if (n > HN_CANVAS_MAX_PTS * 2) n = HN_CANVAS_MAX_PTS * 2;
    float *p = (float *)malloc(sizeof(float) * (size_t)n);
    if (!p) return 0;
    for (int i = 0; i < n; i++)
        p[i] = (float)hn_json_num(hn_json_at(arr, i), 0.0);
    *out = p;
    *n_out = n / 2;                  /* 点数(非 float 数) */
    return 1;
}

static hn_color json_color(const hn_json *o, const char *key) {
    double d = hn_json_num(hn_json_obj_get(o, key), 0.0);
    if (d < 0) d = 0;
    return (hn_color)(uint32_t)d;
}

/* 渐变 stop 数组上限(CSS 理论无上限; 16 个足够任何观感用途, 超出截断) */
#define HN_CANVAS_MAX_STOPS 16

/* "stops":[t0,c0,t1,c1,...] 扁平数组 → malloc hn_grad_stop 数组。
   t 严格升序化由 JS 侧负责, 这里只按到达顺序收下(坏输入不动主流程)。 */
static int parse_stops(const hn_json *o, hn_grad_stop **out, int *n_out) {
    const hn_json *arr = hn_json_obj_get(o, "stops");
    *out = NULL;
    *n_out = 0;
    if (!arr || arr->kind != HN_JSON_ARR) return 0;
    int n = arr->count / 2;                 /* 成对 (t, color) */
    if (n < 2) return 0;                    /* 不足两个 stop: 回落首末两端色 */
    if (n > HN_CANVAS_MAX_STOPS) n = HN_CANVAS_MAX_STOPS;
    hn_grad_stop *s = (hn_grad_stop *)malloc(sizeof(hn_grad_stop) * (size_t)n);
    if (!s) return 0;
    for (int i = 0; i < n; i++) {
        s[i].t = (float)hn_json_num(hn_json_at(arr, i * 2), 0.0);
        double d = hn_json_num(hn_json_at(arr, i * 2 + 1), 0.0);
        if (d < 0) d = 0;
        s[i].c = (hn_color)(uint32_t)d;
    }
    *out = s;
    *n_out = n;
    return 1;
}

/* grect/gpath/spath 共用的渐变字段: 轴/圆端点 + 两端色 + 可选 stops/radial */
static void parse_grad_fields(const hn_json *root, hn_canvas_op *o) {
    o->gx0 = (float)hn_json_num(hn_json_obj_get(root, "x0"), 0);
    o->gy0 = (float)hn_json_num(hn_json_obj_get(root, "y0"), 0);
    o->gx1 = (float)hn_json_num(hn_json_obj_get(root, "x1"), 0);
    o->gy1 = (float)hn_json_num(hn_json_obj_get(root, "y1"), 0);
    o->gr0 = (float)hn_json_num(hn_json_obj_get(root, "r0"), 0);
    o->gr1 = (float)hn_json_num(hn_json_obj_get(root, "r1"), 0);
    if (o->gr0 < 0) o->gr0 = 0;
    if (o->gr1 < 0) o->gr1 = 0;
    o->grad_radial = hn_json_bool(hn_json_obj_get(root, "radial"), 0);
    o->color = json_color(root, "c0");
    o->color2 = json_color(root, "c1");
    parse_stops(root, &o->stops, &o->n_stops);
}

/* fillRect 系(frect/grect)的阴影字段(canvas shadowColor/Blur/OffsetX/Y;
   阴影色 alpha==0 时引擎侧自然关闸, 不需要额外开关位) */
static void parse_shadow_fields(const hn_json *root, hn_canvas_op *o) {
    o->sh_color = json_color(root, "shc");
    o->sh_ox = (float)hn_json_num(hn_json_obj_get(root, "shx"), 0);
    o->sh_oy = (float)hn_json_num(hn_json_obj_get(root, "shy"), 0);
    o->sh_blur = (float)hn_json_num(hn_json_obj_get(root, "shb"), 0);
    if (o->sh_blur < 0) o->sh_blur = 0;
    o->shadow = ((o->sh_color & 0xFFu) != 0);
}

/* ---------------- globalCompositeOperation(子集) ---------------- */

/* 未列出的混合模式回落 source-over: 一次性 stderr 提示(进程级一次 ——
   与引擎其他"诚实降级"提示同口径, 不刷屏)。 */
static int g_blend_warned = 0;

/* "blend" 字符串 → HN_BLEND_*。可识别: source-over(含缺省)/lighter/
   multiply/screen; 其余(差集/异或/色相族…)回落 source-over 并提示一次。 */
static unsigned char parse_blend(const hn_json *root) {
    const char *s = hn_json_str(hn_json_obj_get(root, "blend"), "source-over");
    if (!s || !*s || !strcmp(s, "source-over")) return HN_BLEND_SRC_OVER;
    if (!strcmp(s, "lighter")) return HN_BLEND_LIGHTER;
    if (!strcmp(s, "multiply")) return HN_BLEND_MULTIPLY;
    if (!strcmp(s, "screen")) return HN_BLEND_SCREEN;
    if (!g_blend_warned) {
        fprintf(stderr,
                "[hn_canvas] globalCompositeOperation '%s' 未支持(子集: "
                "source-over/lighter/multiply/screen), 回落 source-over。\n", s);
        g_blend_warned = 1;
    }
    return HN_BLEND_SRC_OVER;
}

/* ---------------- 满幅不透明 fillRect 视同清账(账本有界化) ---------------- */

/* 全幅判定容差(px): 设备空间坐标是浮点落账, 允许半像素的出血/欠覆盖
   (如 x=-0.4 或 x+w=W-0.4 仍算盖满); 超过半像素的欠覆盖(如 x=0.6)不算。 */
#define HN_CV_FULLCOVER_EPS 0.5f

/* 账本内处于激活状态的裁剪深度(未配对 clip 数)。
   按账本序扫描 push/pop 计数; 空栈上的多余 cpop 按"无操作"建模(与浏览器
   restore 空栈口径一致), 不产生负深度。>0 = 有裁剪未配平。
   cpath(精确路径裁剪)与矩形 clip 同价 —— 都压裁剪栈, 都挡新帧判定。 */
static int ledger_clip_depth(const hn_canvas_ledger *l) {
    int depth = 0;
    for (int i = 0; i < l->n_ops; i++) {
        if (l->ops[i].kind == HN_CV_CLIP_PUSH ||
            l->ops[i].kind == HN_CV_CLIP_PATH) depth++;
        else if (l->ops[i].kind == HN_CV_CLIP_POP && depth > 0) depth--;
    }
    return depth;
}

/* frect 落账前的"新帧判定"。返回 1 = 账本已清空(调用方随即落这条 op,
 * 它就是新底色)。三个条件缺一不可, 有争议的场景一律不清(保守侧):
 *   ② 填充色不透明(alpha==255) —— 半透明满幅在视觉上**混合**历史而非
 *      替换, 清账会丢层;
 *   ① 矩形覆盖画布全幅 —— 画布属性尺寸从元素 width/height 取(缺省
 *      300x150, 与 clearRect 全清 / paint_canvas 重放同一口径); 坐标已是
 *      JS glue CTM 变换后的设备空间坐标。元素不在文档里(直录探针/孤立
 *      账本)拿不到属性尺寸, 退回普通落账。NaN 参与比较恒假 → 不清。
 *   ③ 无未配对的 clip —— 裁剪中的"满幅"矩形盖不到被裁掉的区域, 历史
 *      在裁剪区外仍然可见。
 * 刻意不清账的边界(写明即契约):
 *   - 渐变满幅(grect)不清: 渐变可带透明 stop, 视觉上不替换全部历史;
 *   - 旋转/斜切 CTM 的 fillRect 走 JS glue 的 fpath 分支, 天然不进这里;
 *   - 带阴影的满幅 frect **仍算**清账: 阴影画在本 op 之下/之外, 不透明
 *     填充自身已覆盖全部画布, 历史不可见;
 *   - fpath/spath/gpath/text/image 一律不清(非矩形或非纯色填充);
 *   - pdata(putImageData)走**同一口径的自己的判定**(pdata_frame_reset):
 *     目标矩形覆盖全幅 **且块内逐像素 alpha==255** 才算新帧 —— "op 自身
 *     覆盖全幅且不透明"的放宽判定(块是具体像素, 不透明性可严格验证)。
 * 页作者注意: 渐变/半透明盖底的动画想保持账本有界, 应每帧先全幅
 * clearRect 再画(那是显式清账口径)。 */
static int frect_frame_reset(hn_context *c, hn_canvas_ledger *l,
                             const char *cv_id, const hn_canvas_op *o) {
    if ((o->color & 0xFFu) != 0xFFu) return 0;           /* ② 不透明 */
    hn_node *n = c->doc ? hn_doc_find_by_id(c->doc, cv_id) : NULL;
    if (!n) return 0;                                    /* 无元素 = 无尺寸口径 */
    const char *aw = hn_node_attr(n, "width");
    const char *ah = hn_node_attr(n, "height");
    float cw = aw && *aw ? (float)atof(aw) : 300.0f;
    float ch = ah && *ah ? (float)atof(ah) : 150.0f;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    /* ① 全幅覆盖(含半像素容差) */
    if (!(o->x <= HN_CV_FULLCOVER_EPS && o->y <= HN_CV_FULLCOVER_EPS &&
          o->x + o->w >= cw - HN_CV_FULLCOVER_EPS &&
          o->y + o->h >= ch - HN_CV_FULLCOVER_EPS)) return 0;
    if (ledger_clip_depth(l) > 0) return 0;              /* ③ 裁剪激活中 */
    ledger_reset(l);                                     /* 新的一帧 */
    return 1;
}

/* pdata 落账前的"新帧判定"(与 frect 同口径, 判定源换成 op 自身携带的
   像素块): 实际贴入区域(有脏矩形时 = 脏矩形在目标坐标系的投影)覆盖画布
   全幅(同半像素容差)+ **被贴的像素**逐个 alpha==255 + 无激活裁剪 → 清账,
   这条 pdata 就是新底。半透明/含透明像素的块视觉上混合历史, 一律不清
   (保守侧)。 */
static int pdata_frame_reset(hn_context *c, hn_canvas_ledger *l,
                             const char *cv_id, const hn_canvas_op *o) {
    /* 实际贴入的源子矩形(块内像素范围)与目标原点 */
    int sx0 = 0, sy0 = 0, sw = o->pw, sh = o->ph;
    float ox = o->x, oy = o->y;
    if (o->has_dirty) {
        sx0 = (int)o->dtx; sy0 = (int)o->dty;
        sw = (int)o->dtw;  sh = (int)o->dth;
        ox = o->x + o->dtx; oy = o->y + o->dty;
        if (sx0 < 0) { sw += sx0; sx0 = 0; }
        if (sy0 < 0) { sh += sy0; sy0 = 0; }
        if (sx0 + sw > o->pw) sw = o->pw - sx0;
        if (sy0 + sh > o->ph) sh = o->ph - sy0;
        if (sw <= 0 || sh <= 0) return 0;                /* 空脏矩形: 不算新帧 */
    }
    for (int y = 0; y < sh; y++)
        for (int x = 0; x < sw; x++)
            if (o->pdata[((y + sy0) * o->pw + (x + sx0)) * 4 + 3] != 0xFF)
                return 0;                                /* ② 被贴像素全不透明 */
    hn_node *n = c->doc ? hn_doc_find_by_id(c->doc, cv_id) : NULL;
    if (!n) return 0;
    const char *aw = hn_node_attr(n, "width");
    const char *ah = hn_node_attr(n, "height");
    float cw = aw && *aw ? (float)atof(aw) : 300.0f;
    float ch = ah && *ah ? (float)atof(ah) : 150.0f;
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    if (!(ox <= HN_CV_FULLCOVER_EPS && oy <= HN_CV_FULLCOVER_EPS &&
          ox + sw >= cw - HN_CV_FULLCOVER_EPS &&
          oy + sh >= ch - HN_CV_FULLCOVER_EPS)) return 0;        /* ① 全幅 */
    if (ledger_clip_depth(l) > 0) return 0;              /* ③ 裁剪激活中 */
    ledger_reset(l);
    return 1;
}

/* 记录一条 op。返回记录后的账本 op 数(clear 返回清掉的条数); 坏输入 -1。
   "cv" 字段 = canvas 元素 id(JS 桥的寻址键)。 */
int hn_canvas_record(hn_context *c, const char *op_json, size_t len) {
    if (!c || !op_json || !len) return -1;
    /* hn_json_parse 就地修改缓冲 → 复制一份(const 入参不许动) */
    char *buf = (char *)malloc(len + 1);
    if (!buf) return -1;
    memcpy(buf, op_json, len);
    buf[len] = 0;
    void *arena = NULL;
    hn_json *root = hn_json_parse(buf, len, &arena);
    if (!root || root->kind != HN_JSON_OBJ) {
        hn_json_free(arena);
        free(buf);
        return -1;
    }
    const char *cv_id = hn_json_str(hn_json_obj_get(root, "cv"), "");
    hn_canvas_ledger *l = hn_canvas_ledger_get(c, cv_id);
    if (!l) { hn_json_free(arena); free(buf); return -1; }

    const char *kind = hn_json_str(hn_json_obj_get(root, "op"), "");
    hn_canvas_op o;
    memset(&o, 0, sizeof(o));
    int ok = 0;

    if (!strcmp(kind, "clear")) {
        int removed = l->n_ops;
        ledger_reset(l);
        hn_json_free(arena);
        free(buf);
        return removed;              /* 清空语义: 返回清掉的条数 */
    } else if (!strcmp(kind, "clear_rect")) {
        /* 局部清空: 覆盖整块画布属性坐标空间的清空 = 全清 —— 直接清账本
           (不记 op)。"清账重录"动画循环(setInterval/RAF 每帧 clearRect 全幅
           重画)保持 op 有界, 与旧 clearRect 全清口径完全一致; 真正的局部
           矩形才记录 HN_CV_CLEAR_RECT, 绘制期填渲染底色。画布属性尺寸从
           元素 width/height 属性取(缺省 300x150, 与绘制期同一口径)。 */
        float cx = (float)hn_json_num(hn_json_obj_get(root, "x"), 0);
        float cy = (float)hn_json_num(hn_json_obj_get(root, "y"), 0);
        float cw2 = (float)hn_json_num(hn_json_obj_get(root, "w"), 0);
        float ch2 = (float)hn_json_num(hn_json_obj_get(root, "h"), 0);
        int full = 0;
        hn_node *n = c->doc ? hn_doc_find_by_id(c->doc, cv_id) : NULL;
        if (n) {
            const char *aw = hn_node_attr(n, "width");
            const char *ah = hn_node_attr(n, "height");
            float aw2 = aw && *aw ? (float)atof(aw) : 300.0f;
            float ah2 = ah && *ah ? (float)atof(ah) : 150.0f;
            if (aw2 < 1) aw2 = 1;
            if (ah2 < 1) ah2 = 1;
            full = (cx <= 0 && cy <= 0 && cx + cw2 >= aw2 && cy + ch2 >= ah2);
        }
        if (full) {
            int removed = l->n_ops;
            ledger_reset(l);
            hn_json_free(arena);
            free(buf);
            return removed;
        }
        o.kind = HN_CV_CLEAR_RECT;
        o.x = cx; o.y = cy; o.w = cw2; o.h = ch2;
        ok = 1;
    } else if (!strcmp(kind, "frect")) {
        o.kind = HN_CV_FILL_RECT;
        o.x = (float)hn_json_num(hn_json_obj_get(root, "x"), 0);
        o.y = (float)hn_json_num(hn_json_obj_get(root, "y"), 0);
        o.w = (float)hn_json_num(hn_json_obj_get(root, "w"), 0);
        o.h = (float)hn_json_num(hn_json_obj_get(root, "h"), 0);
        o.color = json_color(root, "col");
        o.blend = parse_blend(root);
        parse_shadow_fields(root, &o);
        /* 满幅不透明 fillRect 视同清账: 命中则账本已清空, 下面公共落账段
           把这条 op 记为账本唯一内容(它就是新底色) —— 稳态 = 底色 + 增量。 */
        frect_frame_reset(c, l, cv_id, &o);
        ok = 1;
    } else if (!strcmp(kind, "spath")) {
        o.kind = HN_CV_STROKE_PATH;
        ok = parse_pts(root, &o.pts, &o.n_pts);
        o.color = json_color(root, "col");
        o.blend = parse_blend(root);
        o.stroke_w = (float)hn_json_num(hn_json_obj_get(root, "w"), 1);
        o.cap_round = hn_json_bool(hn_json_obj_get(root, "cap"), 0);
        if (ok && hn_json_obj_get(root, "stops")) parse_grad_fields(root, &o);
    } else if (!strcmp(kind, "fpath")) {
        o.kind = HN_CV_FILL_PATH;
        ok = parse_pts(root, &o.pts, &o.n_pts);
        o.color = json_color(root, "col");
        o.blend = parse_blend(root);
    } else if (!strcmp(kind, "grect")) {
        o.kind = HN_CV_GRAD_RECT;
        o.x = (float)hn_json_num(hn_json_obj_get(root, "x"), 0);
        o.y = (float)hn_json_num(hn_json_obj_get(root, "y"), 0);
        o.w = (float)hn_json_num(hn_json_obj_get(root, "w"), 0);
        o.h = (float)hn_json_num(hn_json_obj_get(root, "h"), 0);
        o.blend = parse_blend(root);
        parse_grad_fields(root, &o);
        parse_shadow_fields(root, &o);
        ok = 1;
    } else if (!strcmp(kind, "gpath")) {
        o.kind = HN_CV_GRAD_PATH;
        ok = parse_pts(root, &o.pts, &o.n_pts);
        o.blend = parse_blend(root);
        parse_grad_fields(root, &o);
    } else if (!strcmp(kind, "pdata")) {
        /* putImageData: RGBA 块(px 扁平数组 r,g,b,a,...)+ 目标 (x,y)
           + 可选脏矩形(dx,dy,dw,dh)。块字节上限 4MB, 超限拒录并置溢出
           提示(语义上限, 防单 op 无界; 页作者分块贴)。 */
        o.kind = HN_CV_PDATA;
        o.x = (float)hn_json_num(hn_json_obj_get(root, "x"), 0);
        o.y = (float)hn_json_num(hn_json_obj_get(root, "y"), 0);
        o.pw = (int)hn_json_num(hn_json_obj_get(root, "w"), 0);
        o.ph = (int)hn_json_num(hn_json_obj_get(root, "h"), 0);
        if (o.pw > 0 && o.ph > 0 && o.pw <= 65535 && o.ph <= 65535 &&
            (long long)o.pw * o.ph * 4 <= (long long)HN_CV_PDATA_MAX_BYTES) {
            const hn_json *arr = hn_json_obj_get(root, "px");
            if (arr && arr->kind == HN_JSON_ARR &&
                arr->count >= (long long)o.pw * o.ph * 4) {
                unsigned char *blk =
                    (unsigned char *)malloc((size_t)o.pw * o.ph * 4);
                if (blk) {
                    int need = o.pw * o.ph * 4;
                    for (int i2 = 0; i2 < need; i2++) {
                        double d = hn_json_num(hn_json_at(arr, i2), 0.0);
                        if (d < 0) d = 0;
                        if (d > 255) d = 255;
                        blk[i2] = (unsigned char)(d + 0.5);
                    }
                    o.pdata = blk;
                    o.has_dirty = hn_json_obj_get(root, "dx") != NULL;
                    o.dtx = (float)hn_json_num(hn_json_obj_get(root, "dx"), 0);
                    o.dty = (float)hn_json_num(hn_json_obj_get(root, "dy"), 0);
                    o.dtw = (float)hn_json_num(hn_json_obj_get(root, "dw"),
                                               (double)o.pw);
                    o.dth = (float)hn_json_num(hn_json_obj_get(root, "dh"),
                                               (double)o.ph);
                    /* 满幅不透明 putImageData 与 frect 同口径清账 */
                    pdata_frame_reset(c, l, cv_id, &o);
                    ok = 1;
                }
            }
        }
        if (!ok) {
            /* 拒录: 块超限/缺像素/坏尺寸 —— 置溢出标志让内省可见, 打一行
               提示(一次性由 overflow 标志拦着, 与账本溢出同一闸门)。 */
            if (!l->overflow)
                fprintf(stderr,
                        "[hn_canvas] putImageData 拒录: 块超限(单块上限 "
                        "%dMB)或缺 px 数据(w=%d h=%d)。\n",
                        (int)(HN_CV_PDATA_MAX_BYTES >> 20), o.pw, o.ph);
            l->overflow = 1;
        }
    } else if (!strcmp(kind, "prect") || !strcmp(kind, "ppath")) {
        /* createPattern 平铺填充: str = 源图路径(绘制期经图片解码缓存),
           prep = 平铺模式(0 repeat / 1 repeat-x / 2 repeat-y / 3 no-repeat),
           prect 带 x,y,w,h; ppath 带 pts(闭合多边形)。 */
        o.kind = !strcmp(kind, "prect") ? HN_CV_PATTERN_RECT : HN_CV_PATTERN_PATH;
        const char *s = hn_json_str(hn_json_obj_get(root, "src"), "");
        o.str = s ? cv_strdup(s) : NULL;
        o.blend = parse_blend(root);
        const char *rep = hn_json_str(hn_json_obj_get(root, "rep"), "repeat");
        o.prep = HN_PAT_REPEAT;
        if (!strcmp(rep, "repeat-x")) o.prep = HN_PAT_REPEAT_X;
        else if (!strcmp(rep, "repeat-y")) o.prep = HN_PAT_REPEAT_Y;
        else if (!strcmp(rep, "no-repeat")) o.prep = HN_PAT_NONE;
        if (o.kind == HN_CV_PATTERN_RECT) {
            o.x = (float)hn_json_num(hn_json_obj_get(root, "x"), 0);
            o.y = (float)hn_json_num(hn_json_obj_get(root, "y"), 0);
            o.w = (float)hn_json_num(hn_json_obj_get(root, "w"), 0);
            o.h = (float)hn_json_num(hn_json_obj_get(root, "h"), 0);
            ok = o.str != NULL && o.str[0];
        } else {
            ok = parse_pts(root, &o.pts, &o.n_pts) && o.str && o.str[0];
        }
    } else if (!strcmp(kind, "cpath")) {
        /* 精确路径裁剪: pts(可多条子路径, 以 segs 点数表分段)+ 配对的
           cpop 出栈。计裁剪深度(与矩形 clip 同价)。 */
        o.kind = HN_CV_CLIP_PATH;
        ok = parse_pts(root, &o.pts, &o.n_pts);
        const hn_json *sg = hn_json_obj_get(root, "segs");
        if (ok && sg && sg->kind == HN_JSON_ARR && sg->count > 0) {
            int n = sg->count > 256 ? 256 : sg->count;
            int *sv = (int *)malloc(sizeof(int) * (size_t)n);
            if (sv) {
                int total = 0;
                for (int i2 = 0; i2 < n; i2++) {
                    sv[i2] = (int)hn_json_num(hn_json_at(sg, i2), 0);
                    if (sv[i2] < 0) sv[i2] = 0;
                    total += sv[i2];
                }
                /* 点数表与 pts 对不上时按单多边形回落(坏输入不崩) */
                if (total == o.n_pts) { o.segs = sv; o.n_segs = n; }
                else free(sv);
            }
        }
    } else if (!strcmp(kind, "clip")) {
        o.kind = HN_CV_CLIP_PUSH;
        o.x = (float)hn_json_num(hn_json_obj_get(root, "x"), 0);
        o.y = (float)hn_json_num(hn_json_obj_get(root, "y"), 0);
        o.w = (float)hn_json_num(hn_json_obj_get(root, "w"), 0);
        o.h = (float)hn_json_num(hn_json_obj_get(root, "h"), 0);
        ok = 1;
    } else if (!strcmp(kind, "cpop")) {
        o.kind = HN_CV_CLIP_POP;
        ok = 1;                      /* 无参; 配平由 JS save/restore 栈负责 */
    } else if (!strcmp(kind, "text")) {
        o.kind = HN_CV_TEXT;
        const char *s = hn_json_str(hn_json_obj_get(root, "s"), "");
        o.str = s ? cv_strdup(s) : NULL;
        o.x = (float)hn_json_num(hn_json_obj_get(root, "x"), 0);
        o.y = (float)hn_json_num(hn_json_obj_get(root, "y"), 0);
        o.font_px = (float)hn_json_num(hn_json_obj_get(root, "px"), 14);
        o.color = json_color(root, "col");
        o.blend = parse_blend(root);
        ok = o.str != NULL;
    } else if (!strcmp(kind, "image")) {
        o.kind = HN_CV_IMAGE;
        const char *s = hn_json_str(hn_json_obj_get(root, "src"), "");
        o.str = s ? cv_strdup(s) : NULL;
        o.x = (float)hn_json_num(hn_json_obj_get(root, "x"), 0);
        o.y = (float)hn_json_num(hn_json_obj_get(root, "y"), 0);
        o.w = (float)hn_json_num(hn_json_obj_get(root, "w"), 0);
        o.h = (float)hn_json_num(hn_json_obj_get(root, "h"), 0);
        o.blend = parse_blend(root);
        /* 9 参 drawImage 的源子矩形(源图像素坐标): 四字段齐备才启用,
           缺任一 = 整图(3/5 参形态, 旧口径不变) */
        if (hn_json_obj_get(root, "sx") && hn_json_obj_get(root, "sy") &&
            hn_json_obj_get(root, "sw") && hn_json_obj_get(root, "sh")) {
            o.sx = (float)hn_json_num(hn_json_obj_get(root, "sx"), 0);
            o.sy = (float)hn_json_num(hn_json_obj_get(root, "sy"), 0);
            o.sw = (float)hn_json_num(hn_json_obj_get(root, "sw"), 0);
            o.sh = (float)hn_json_num(hn_json_obj_get(root, "sh"), 0);
            if (o.sw > 0 && o.sh > 0) o.has_src = 1;
        }
        ok = o.str != NULL && o.str[0];
    }

    int result = -1;
    if (ok) {
        if (l->n_ops >= HN_CANVAS_MAX_OPS) {
            /* 保险丝(保持"丢弃新 op"现状, 不回滚最旧 —— 半截历史比缺帧更
               难排查): 只在首次溢出时打一行 stderr 提示页作者按帧清账
               (清账后标志复位, 再溢出会再提示一次)。 */
            if (!l->overflow)
                fprintf(stderr,
                        "[hn_canvas] 账本溢出: canvas '%s' 达 %d op 上限, 之后的新 op "
                        "被丢弃(画面停在溢出前的样子)。请每帧清账: 全幅 "
                        "clearRect(0,0,w,h), 或以不透明满幅 fillRect 盖底。\n",
                        l->id, l->n_ops);
            l->overflow = 1;         /* 满则丢弃: 不崩溃, 标志可内省 */
        } else {
            if (l->n_ops == l->cap_ops) {
                int nc = l->cap_ops ? l->cap_ops * 2 : 64;
                hn_canvas_op *nv =
                    (hn_canvas_op *)realloc(l->ops, sizeof(hn_canvas_op) * (size_t)nc);
                if (!nv) {
                    op_free(&o);
                    hn_json_free(arena);
                    free(buf);
                    return -1;
                }
                l->ops = nv;
                l->cap_ops = nc;
            }
            l->ops[l->n_ops++] = o;
        }
        result = l->n_ops;
    } else {
        op_free(&o);
    }
    hn_json_free(arena);
    free(buf);
    return result;
}

/* ---------------- 内省(探针/宿主) ---------------- */

/* canvas measureText 测量缝(供 rt 层 JS 桥调用; hn_context 对 rt 不透明,
   文本后端住在 context 里, 所以入口放在引擎侧)。 */
float hn_canvas_measure(hn_context *c, float size_px, const char *utf8, size_t len) {
    if (!utf8 || !len) return 0;
    if (!(size_px > 0)) size_px = 10;
    if (c && c->tb_valid && c->tb.measure) {
        hn_font_desc fd;
        memset(&fd, 0, sizeof(fd));
        fd.size_px = size_px;
        fd.weight = 400;
        return c->tb.measure(c->tb.ctx, &fd, utf8, len);
    }
    return (float)len * size_px * 0.55f;   /* 引擎无后端同口径 */
}

int hn_canvas_clear(hn_context *c, const char *canvas_id) {
    hn_canvas_ledger *l = hn_canvas_ledger_find(c, canvas_id);
    if (!l) return -1;
    int removed = l->n_ops;
    ledger_reset(l);
    return removed;
}

int hn_canvas_count(hn_context *c, const char *canvas_id) {
    hn_canvas_ledger *l = hn_canvas_ledger_find(c, canvas_id);
    return l ? l->n_ops : -1;
}

int hn_canvas_overflow(hn_context *c, const char *canvas_id) {
    hn_canvas_ledger *l = hn_canvas_ledger_find(c, canvas_id);
    return l ? l->overflow : -1;
}

/* ---------------- getImageData(像素出口) ----------------
 * 离屏栅格化当前账本(hn_paint_raster_ledger), 裁出 (x,y,w,h) 子区返回
 * 非预乘 RGBA(标准 ImageData 口径)。越界部分补透明黑(浏览器同口径);
 * 账本为空也出图(全透明), 与"画布初态 = 透明黑"一致。 */
int hn_canvas_get_image_data(hn_context *c, const char *canvas_id,
                             int x, int y, int w, int h,
                             unsigned char **out, int *out_w, int *out_h) {
    if (out) *out = NULL;
    if (out_w) *out_w = 0;
    if (out_h) *out_h = 0;
    if (!c || !canvas_id || !out || w <= 0 || h <= 0) return 0;
    if (w > 8192 || h > 8192) return 0;
    hn_canvas_ledger *l = hn_canvas_ledger_find(c, canvas_id);
    if (!l) return 0;
    /* 画布属性尺寸(与账本坐标/重放/新帧判定同一口径; 缺省 300x150) */
    float cw = 300.0f, ch = 150.0f;
    hn_node *n = c->doc ? hn_doc_find_by_id(c->doc, canvas_id) : NULL;
    if (n) {
        const char *aw = hn_node_attr(n, "width");
        const char *ah = hn_node_attr(n, "height");
        if (aw && *aw) cw = (float)atof(aw);
        if (ah && *ah) ch = (float)atof(ah);
    }
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    unsigned char *full = hn_paint_raster_ledger(c, l, cw, ch);
    if (!full) return 0;
    int fw = (int)cw;
    if ((float)fw < cw) fw++;
    int fh = (int)ch;
    if ((float)fh < ch) fh++;
    /* 子区裁剪: 与整幅的交集逐行拷贝, 交集外保持透明黑(calloc 零) */
    unsigned char *sub = (unsigned char *)calloc((size_t)w * h * 4, 1);
    if (!sub) { free(full); return 0; }
    int x0 = x < 0 ? 0 : x, y0 = y < 0 ? 0 : y;
    int x1 = x + w > fw ? fw : x + w, y1 = y + h > fh ? fh : y + h;
    for (int sy = y0; sy < y1; sy++) {
        memcpy(sub + ((size_t)(sy - y) * w + (size_t)(x0 - x)) * 4,
               full + ((size_t)sy * fw + (size_t)x0) * 4,
               (size_t)(x1 - x0) * 4);
    }
    free(full);
    *out = sub;
    if (out_w) *out_w = w;
    if (out_h) *out_h = h;
    return 1;
}

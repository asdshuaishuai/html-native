/* hn_canvas.c — <canvas> 2D 账本(QuickJS 桥的落账侧)
 *
 * 分工(与 Skia canvaskit 的 htmlcanvas 同构, 但状态机在 JS glue 里):
 *   JS 层( rt/hn_rt.c 的 glue )—— fillStyle/strokeStyle 颜色解析、
 *     save/restore 栈、当前路径、CTM 2x3、渐变对象; 每个绘制调用在这里
 *     解析成"已解析的原语 op"(坐标已 CTM 变换、颜色已 RGBA), 序列化为
 *     JSON 调 hnCanvas2D 桥。
 *   本文件 —— 桥的落账侧: 解析 op JSON, 按元素 id 记入账本(定长数组,
 *     上限 4096, 满则丢弃并置溢出标志); 提供清空/内省与销毁纪律。
 *   hn_paint.c —— 绘制 <canvas> 时把账本重放成显示列表指令。
 *
 * hnCanvas2D op 协议(JSON 对象, 每次一条):
 *   {"cv":"<canvas id>","op":"frect","x","y","w","h","col"}            矩形填充
 *   {"cv":..,"op":"spath","pts":[x0,y0,..],"col","w","cap"}            开放折线描边
 *   {"cv":..,"op":"fpath","pts":[..],"col"}                            闭合多边形填充
 *   {"cv":..,"op":"grect","x","y","w","h",
 *               "x0","y0","c0","x1","y1","c1"}                         线性渐变矩形
 *   {"cv":..,"op":"clip","x","y","w","h"}                              矩形裁剪入栈
 *   {"cv":..,"op":"cpop"}                                              裁剪出栈
 *   {"cv":..,"op":"text","s":"..","x","y","px","col"}                  文本(x, 基线 y)
 *   {"cv":..,"op":"image","src":"..","x","y","w","h"}                  图片
 *   {"cv":..,"op":"clear"}                                             清空账本(clearRect)
 * 颜色一律 0xRRGGBBAA 的十进制数(JS 侧 `>>>0`), 坐标已含 CTM。
 */
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
    memset(o, 0, sizeof(*o));
}

/* 清空账本(保留槽位本身; clearRect 与销毁共用) */
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
    } else if (!strcmp(kind, "frect")) {
        o.kind = HN_CV_FILL_RECT;
        o.x = (float)hn_json_num(hn_json_obj_get(root, "x"), 0);
        o.y = (float)hn_json_num(hn_json_obj_get(root, "y"), 0);
        o.w = (float)hn_json_num(hn_json_obj_get(root, "w"), 0);
        o.h = (float)hn_json_num(hn_json_obj_get(root, "h"), 0);
        o.color = json_color(root, "col");
        ok = 1;
    } else if (!strcmp(kind, "spath")) {
        o.kind = HN_CV_STROKE_PATH;
        ok = parse_pts(root, &o.pts, &o.n_pts);
        o.color = json_color(root, "col");
        o.stroke_w = (float)hn_json_num(hn_json_obj_get(root, "w"), 1);
        o.cap_round = hn_json_bool(hn_json_obj_get(root, "cap"), 0);
    } else if (!strcmp(kind, "fpath")) {
        o.kind = HN_CV_FILL_PATH;
        ok = parse_pts(root, &o.pts, &o.n_pts);
        o.color = json_color(root, "col");
    } else if (!strcmp(kind, "grect")) {
        o.kind = HN_CV_GRAD_RECT;
        o.x = (float)hn_json_num(hn_json_obj_get(root, "x"), 0);
        o.y = (float)hn_json_num(hn_json_obj_get(root, "y"), 0);
        o.w = (float)hn_json_num(hn_json_obj_get(root, "w"), 0);
        o.h = (float)hn_json_num(hn_json_obj_get(root, "h"), 0);
        o.gx0 = (float)hn_json_num(hn_json_obj_get(root, "x0"), 0);
        o.gy0 = (float)hn_json_num(hn_json_obj_get(root, "y0"), 0);
        o.gx1 = (float)hn_json_num(hn_json_obj_get(root, "x1"), 0);
        o.gy1 = (float)hn_json_num(hn_json_obj_get(root, "y1"), 0);
        o.color = json_color(root, "c0");
        o.color2 = json_color(root, "c1");
        ok = 1;
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
        ok = o.str != NULL;
    } else if (!strcmp(kind, "image")) {
        o.kind = HN_CV_IMAGE;
        const char *s = hn_json_str(hn_json_obj_get(root, "src"), "");
        o.str = s ? cv_strdup(s) : NULL;
        o.x = (float)hn_json_num(hn_json_obj_get(root, "x"), 0);
        o.y = (float)hn_json_num(hn_json_obj_get(root, "y"), 0);
        o.w = (float)hn_json_num(hn_json_obj_get(root, "w"), 0);
        o.h = (float)hn_json_num(hn_json_obj_get(root, "h"), 0);
        ok = o.str != NULL && o.str[0];
    }

    int result = -1;
    if (ok) {
        if (l->n_ops >= HN_CANVAS_MAX_OPS) {
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

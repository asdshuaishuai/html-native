/* hn_fmt.c — 结构化文本直染: <div hn-json> / <div hn-csv> 源 → 普通 DOM 子树
 *
 * 定位: agent 工具的输出常是 JSON 与 CSV。与 markdown 直染(hn_md.c)同一模式:
 * 装载期(hn_context_set_doc / hn_context_render / hn_doc_swap 三个入口, 都在
 * 首次 layout 之前、<script> 执行之前)把容器元素的文本**替换成普通 DOM 子树**
 * (head 条 / 缩进树 / table), 之后样式/布局/绘制/交互全部走既有管线, 零特例。
 *
 * JSON(<div hn-json>):
 *   - 解析走既有 hn_json.c(Lottie 用同一份: 就地解析 + 节点独立分配; 这里
 *     复制一份缓冲再解析, 用完 hn_json_free —— 不引入任何第三方 JSON 库,
 *     API 直接可用, 无需适配层)。
 *   - 渲染成语法着色树: 每行一个 div.json-row; 键 span.tok-k、字符串
 *     span.tok-s、数字 span.tok-n、布尔/null span.tok-b(数字与字面量两档是
 *     md 三色令牌之外的新增令牌, 色值只在 hn_theme.c)。
 *   - 截断口径(agent 场景的巨 JSON 必须不炸布局 —— 四道闸都必要:
 *     深度 6 限制子树高度, 数组/对象上限限制扇出, 总行预算兜住
 *     "深度×扇出"的乘积爆炸):
 *       深度  容器深度 > 6(根容器 = 1) → 该值截断为 `…`(span.json-ellip);
 *       数组  元素 > 50  → 前 50 条 + 尾行 `… more N items`(div.json-more);
 *       对象  成员 > 100 → 前 100 条 + 尾行 `… more N keys`;
 *       总量  已渲染行 > 400 → 停止 + 一行 `… more (output limit)`。
 *   - 解析失败(JSON 在 agent 场景常半截) / 空: 顶部一行错误条
 *     (div.json-err) + 原样代码块(pre > code, 与 hn-md 代码块同外观),
 *     不抛错、不中断装载 —— 失败必须优雅。
 *   - 顶部一行摘要条 div.json-head: `{…} 12 keys · 0.8 KB` /
 *     `[…] 87 items · 3.2 KB`(字节数 = 源文本长度)。
 *
 * CSV(<div hn-csv>, 分隔符 hn-csv-sep=";" 缺省 ','):
 *   - RFC4180 最小子集: 首行表头; 引号包围字段(引号内分隔符/换行原样保留),
 *     `""` 转义为字面 `"`; 行终止 \n 与 \r\n(CRLF 归一为 LF)。
 *     闭引号后只允许 分隔符/行尾/EOF, 否则视为解析失败。
 *   - 渲染成既有 table 语义(hn_md 表格同款 DOM: table/tr/th/td; UA 表格规则
 *     hn_style.c + 主题皮肤), 表头行 class=csv-head。
 *   - 数字列右对齐: 逐列在已渲染数据行上嗅探, 可解析为数的比例 > 70% 则
 *     th/td 落内联 text-align: right(与 hn_md 表格对齐同款手法)。
 *   - 截断: 数据行(不含表头) > 200 → 前 200 行 + 尾行 `… 共 N 行`;
 *     列 > 12 → 前 12 列 + 尾行提示(与行提示合并)`… 共 M 列`。
 *   - 解析失败 / 空(全空白): 回落原样代码块。
 *   - 缩进书写: HTML 里缩进书写的 CSV 源按"首尾空白行剥离 + 公共缩进剥层"
 *     预清洗(与 hn_md 的 dedent 同口径), 顶格写则零影响。单元格值若有意
 *     保留行首空格, 请让整块顶格书写(公共缩进为 0 时不剥)。
 *     解析失败回落时显示的是**未清洗的原文**。
 *
 * 与 hn-md 相同的两条作者约束(注释即契约):
 *   1. 源里不要写 HTML 标签 —— 会被 HTML 解析器解析成元素(文本仍会拼回,
 *      但标签本身消失; 尖括号若是数据的一部分会失真)。需要字面 < 写 &lt;。
 *   2. 实体会被 HTML 层解码一次(&amp; → &); 要表达字面 & 写 &amp;。
 * 幂等与更新: 转换后元素的**首个子元素**即产物指纹(JSON: div.json-head /
 * div.json-err; CSV: table.csv / 回落 pre)—— 再次装载批次(hn_doc_swap 会
 * 全文档再扫描)看到指纹就跳过, 不会把已渲染的子树当源再转; 若内容被宿主
 * 换成了新的原文(无指纹), 则照常重新转换 —— 原地更新可用。元素上另外打
 * hn-fmt="json"/"csv" 仅作调试标记(不参与渲染)。
 * 容器嵌套口径: 不要把 hn-json/hn-csv 放进 hn-md 容器内(md 装载会整体替换
 * 其内容); 反过来 md 源里的 <div hn-json> 也会先被 HTML 层解析成元素。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn_internal.h"
#include "hn_json.h"

/* ---------- DOM 构造小工具(与 hn_md.c 同一口径: 新节点走 hn_dom_*) ---------- */

static hn_node *fmt_elem(hn_arena *ar, const char *tag, const char *cls) {
    hn_node *e = hn_dom_elem(ar, tag);
    if (cls) hn_node_set_attr(e, "class", cls);
    return e;
}

static void fmt_text(hn_arena *ar, hn_node *parent, const char *s, size_t n) {
    if (s && n) hn_dom_append(parent, hn_dom_text(ar, s, n));
}

static void fmt_cspan(hn_arena *ar, hn_node *parent, const char *cls, const char *s) {
    hn_node *sp = fmt_elem(ar, "span", cls);
    hn_dom_append(parent, sp);
    fmt_text(ar, sp, s, s ? strlen(s) : 0);
}

static void fmt_cspan_n(hn_arena *ar, hn_node *parent, const char *cls,
                        const char *s, size_t n) {
    hn_node *sp = fmt_elem(ar, "span", cls);
    hn_dom_append(parent, sp);
    fmt_text(ar, sp, s, n);
}

/* 源文本长度 → `N B` / `N.N KB`(摘要条用) */
static void fmt_size_text(size_t bytes, char *out, size_t cap) {
    if (bytes < 1024) snprintf(out, cap, "%lu B", (unsigned long)bytes);
    else snprintf(out, cap, "%.1f KB", (double)bytes / 1024.0);
}

/* ---------- JSON: 标量文本 ---------- */

/* 数字显示: 整数值(±9e15 内, double 可精确表示)按整数打印, 其余 %.10g */
static void fmt_num_text(double d, char *out, size_t cap) {
    if (d >= -9.0e15 && d <= 9.0e15) {
        long long ll = (long long)d;
        if ((double)ll == d) { snprintf(out, cap, "%lld", ll); return; }
    }
    snprintf(out, cap, "%.10g", d);
}

/* 字符串显示: 补引号 + 最小转义(显示保真; 解码后的控制字符重新转义) */
static void fmt_quote(hn_arena *ar, hn_node *parent, const char *cls, const char *s) {
    size_t n = s ? strlen(s) : 0;
    char *buf = (char *)hn_arena_alloc(ar, n * 6 + 3);
    if (!buf) return;
    size_t o = 0;
    buf[o++] = '"';
    for (size_t i = 0; i < n; i++) {
        unsigned char ch = (unsigned char)s[i];
        switch (ch) {
        case '"':  buf[o++] = '\\'; buf[o++] = '"'; break;
        case '\\': buf[o++] = '\\'; buf[o++] = '\\'; break;
        case '\n': buf[o++] = '\\'; buf[o++] = 'n'; break;
        case '\r': buf[o++] = '\\'; buf[o++] = 'r'; break;
        case '\t': buf[o++] = '\\'; buf[o++] = 't'; break;
        default:
            if (ch < 0x20) o += (size_t)snprintf(buf + o, 7, "\\u%04x", (unsigned)ch);
            else buf[o++] = (char)ch;
        }
    }
    buf[o++] = '"';
    buf[o] = 0;
    fmt_cspan_n(ar, parent, cls, buf, o);
}

/* ---------- JSON: 缩进树渲染 ---------- */

#define FMT_JSON_MAX_DEPTH 6     /* 容器深度上限(根 = 1) */
#define FMT_JSON_MAX_ITEMS 50    /* 单数组元素上限 */
#define FMT_JSON_MAX_KEYS  100   /* 单对象成员上限 */
#define FMT_JSON_MAX_ROWS  400   /* 全树渲染行预算 */

typedef struct {
    hn_arena *ar;
    int rows;      /* 已渲染行数(行 = div.json-row) */
    int limited;   /* 行预算已耗尽(只补一行提示, 不再逐层补) */
} json_ctx;

/* 一行(div.json-row [extra])挂到 parent 下 */
static hn_node *json_row(hn_arena *ar, hn_node *parent, const char *extra) {
    hn_node *r;
    if (!extra) {
        r = fmt_elem(ar, "div", "json-row");
    } else {
        char cls[64];
        snprintf(cls, sizeof(cls), "json-row %s", extra);
        r = fmt_elem(ar, "div", cls);
    }
    hn_dom_append(parent, r);
    return r;
}

static void json_scalar(hn_arena *ar, hn_node *row, const hn_json *v) {
    char nb[48];
    switch (v->kind) {
    case HN_JSON_NULL: fmt_cspan(ar, row, "tok-b", "null"); break;
    case HN_JSON_BOOL: fmt_cspan(ar, row, "tok-b", v->boolean ? "true" : "false"); break;
    case HN_JSON_NUM:  fmt_num_text(v->num, nb, sizeof(nb)); fmt_cspan(ar, row, "tok-n", nb); break;
    case HN_JSON_STR:  fmt_quote(ar, row, "tok-s", v->str ? v->str : ""); break;
    default: break;   /* 容器不走这里 */
    }
}

static void json_key_colon(hn_arena *ar, hn_node *row, const char *key) {
    if (!key) return;
    fmt_quote(ar, row, "tok-k", key);
    fmt_cspan(ar, row, "json-colon", ": ");
}

static const char *json_brace_ch(hn_json_kind k, int close) {
    if (k == HN_JSON_OBJ) return close ? "}" : "{";
    return close ? "]" : "[";
}

static void json_container(json_ctx *j, hn_node *parent, const hn_json *c, int depth);

/* 行预算耗尽: 补一行 `… more (output limit)`(只补一次) */
static void json_limit_row(json_ctx *j, hn_node *parent) {
    if (j->limited) return;
    static const char MSG[] = "\xE2\x80\xA6 more (output limit)";
    hn_node *row = json_row(j->ar, parent, "json-more");
    fmt_text(j->ar, row, MSG, sizeof(MSG) - 1);
    j->limited = 1;
}

/* 把容器 c 的行渲染进 parent(调用方已保证 c 是 OBJ/ARR) */
static void json_container(json_ctx *j, hn_node *parent, const hn_json *c, int depth) {
    int is_obj = (c->kind == HN_JSON_OBJ);
    int cap = is_obj ? FMT_JSON_MAX_KEYS : FMT_JSON_MAX_ITEMS;
    int shown = c->count < cap ? c->count : cap;
    for (int i = 0; i < shown; i++) {
        if (j->rows >= FMT_JSON_MAX_ROWS) { json_limit_row(j, parent); return; }
        const hn_json *v = c->items[i];
        hn_node *row = json_row(j->ar, parent, NULL);
        j->rows++;
        if (is_obj) json_key_colon(j->ar, row, v->key);
        if (v->kind == HN_JSON_OBJ || v->kind == HN_JSON_ARR) {
            if (depth + 1 > FMT_JSON_MAX_DEPTH) {
                /* 深度截断: 整棵子树一个 `…`(不递归, 不占预算) */
                fmt_cspan(j->ar, row, "json-ellip", "\xE2\x80\xA6");
                continue;
            }
            fmt_cspan(j->ar, row, "json-brace", json_brace_ch(v->kind, 0));
            /* 子层缩进: 内联 padding(结构属性, 不依赖主题; 与 hn_md 的
               text-align 内联同一手法), 颜色/字号在 hn_theme.c */
            hn_node *kids = fmt_elem(j->ar, "div", "json-kids");
            hn_node_set_attr(kids, "style", "padding-left: 14");
            hn_dom_append(parent, kids);
            json_container(j, kids, v, depth + 1);
            if (j->limited) continue;   /* 预算已尽: 省略闭括号(截断态) */
            {
                hn_node *cl = json_row(j->ar, parent, "json-close");
                j->rows++;
                fmt_cspan(j->ar, cl, "json-brace", json_brace_ch(v->kind, 1));
            }
        } else {
            json_scalar(j->ar, row, v);
        }
    }
    if (c->count > shown && !j->limited) {
        hn_node *row = json_row(j->ar, parent, "json-more");
        char buf[64];
        snprintf(buf, sizeof(buf), "\xE2\x80\xA6 more %d %s", c->count - shown,
                 is_obj ? "keys" : "items");
        fmt_cspan(j->ar, row, "json-more", buf);
    }
}

/* 成功路径: 摘要条 + 树主体 */
static void json_render(hn_arena *ar, hn_node *into, size_t src_len, const hn_json *root) {
    char sizebuf[32], head[96];
    fmt_size_text(src_len, sizebuf, sizeof(sizebuf));
    if (root->kind == HN_JSON_OBJ)
        snprintf(head, sizeof(head), "{…} %d keys · %s", root->count, sizebuf);
    else if (root->kind == HN_JSON_ARR)
        snprintf(head, sizeof(head), "[…] %d items · %s", root->count, sizebuf);
    else
        snprintf(head, sizeof(head), "value · %s", sizebuf);

    hn_node *h = fmt_elem(ar, "div", "json-head");
    hn_dom_append(into, h);
    fmt_text(ar, h, head, strlen(head));

    hn_node *body = fmt_elem(ar, "div", "json-body");
    hn_dom_append(into, body);
    if (root->kind == HN_JSON_OBJ || root->kind == HN_JSON_ARR) {
        json_ctx j;
        j.ar = ar;
        j.rows = 0;
        j.limited = 0;
        json_container(&j, body, root, 1);
    } else {
        json_scalar(ar, json_row(ar, body, NULL), root);
    }
}

/* 失败路径: 错误条 + 原样代码块(hn-md 围栏代码块同款外观) */
static void fmt_src_block(hn_arena *ar, hn_node *into, const char *src, size_t len) {
    hn_node *pre = hn_dom_elem(ar, "pre");
    hn_node *code = hn_dom_elem(ar, "code");
    hn_dom_append(into, pre);
    hn_dom_append(pre, code);
    fmt_text(ar, code, src, len);
}

static void json_fallback(hn_arena *ar, hn_node *into, const char *src, size_t len) {
    hn_node *err = fmt_elem(ar, "div", "json-err");
    hn_dom_append(into, err);
    fmt_cspan(ar, err, "json-err", "JSON 解析失败 · 已按原文显示");
    fmt_src_block(ar, into, src, len);
}

/* ---------- CSV: 预清洗 + RFC4180 最小子集解析 ---------- */

#define FMT_CSV_MAX_ROWS 200   /* 数据行渲染上限(不含表头) */
#define FMT_CSV_MAX_COLS 12    /* 渲染列数上限 */

typedef struct { const char *s; size_t n; } fmt_cell;

typedef struct {
    fmt_cell *cells;      /* 表头 + 前 200 数据行 × 前 12 列的展平单元格 */
    int       n_cells, cap_cells;
    int      *row_len;    /* 每已存行的单元格数(≤ 12) */
    int       n_rows, cap_rows;
    int       total_rows; /* 数据行总数(不含表头, 全量计数) */
    int       n_cols;     /* 表头列数(全量计数) */
    char     *dec;        /* 引号字段解码缓冲(解析期持有) */
} csv_doc;

static void csv_free(csv_doc *P) {
    free(P->cells);
    free(P->row_len);
    free(P->dec);
    memset(P, 0, sizeof(*P));
}

static int csv_push_cell(csv_doc *P, const char *s, size_t n) {
    if (P->n_cells == P->cap_cells) {
        int nc = P->cap_cells ? P->cap_cells * 2 : 64;
        fmt_cell *nv = (fmt_cell *)realloc(P->cells, sizeof(fmt_cell) * (size_t)nc);
        if (!nv) return 0;
        P->cells = nv;
        P->cap_cells = nc;
    }
    P->cells[P->n_cells].s = s;
    P->cells[P->n_cells].n = n;
    P->n_cells++;
    return 1;
}

static int csv_push_row(csv_doc *P, int cells) {
    if (P->n_rows == P->cap_rows) {
        int nc = P->cap_rows ? P->cap_rows * 2 : 32;
        int *nv = (int *)realloc(P->row_len, sizeof(int) * (size_t)nc);
        if (!nv) return 0;
        P->row_len = nv;
        P->cap_rows = nc;
    }
    P->row_len[P->n_rows++] = cells;
    return 1;
}

/* 行首缩进预清洗: 剥掉首尾空白行 + 各内容行公共前导空格(与 hn_md 的
   dedent 同口径 —— HTML 里缩进书写的 CSV 源不必顶格)。行终止归一为 '\n';
   末尾不补换行。全空白/分配失败返回 NULL(调用方回落原样代码块)。 */
static char *csv_prep(const char *src, size_t len, size_t *out_len) {
    size_t *bs = NULL, *es = NULL;
    int n = 0, cap = 0;
    size_t i = 0;
    while (i < len) {
        size_t b = i;
        while (i < len && src[i] != '\n') i++;
        size_t e = i;
        if (e > b && src[e - 1] == '\r') e--;
        if (n == cap) {
            int nc = cap ? cap * 2 : 32;
            size_t *nb = (size_t *)realloc(bs, sizeof(size_t) * (size_t)nc);
            size_t *ne = (size_t *)realloc(es, sizeof(size_t) * (size_t)nc);
            if (!nb || !ne) { free(nb ? nb : bs); free(ne ? ne : es); return NULL; }
            bs = nb;
            es = ne;
            cap = nc;
        }
        bs[n] = b;
        es[n] = e;
        n++;
        if (i >= len) break;
        i++;                                   /* 跳过 '\n' */
    }
    if (n == 0) { free(bs); free(es); return NULL; }
    /* 空白行判定(空格/制表符) */
    int lo = -1, hi = -1;
    for (int k = 0; k < n; k++) {
        int blank = 1;
        for (size_t q = bs[k]; q < es[k]; q++)
            if (src[q] != ' ' && src[q] != '\t') { blank = 0; break; }
        if (!blank) { if (lo < 0) lo = k; hi = k; }
    }
    if (lo < 0) { free(bs); free(es); return NULL; }
    /* 公共前导空格(只数空格; 制表符视为数据, 不参与缩进剥离) */
    size_t base = (size_t)-1;
    for (int k = lo; k <= hi; k++) {
        size_t ind = 0;
        while (bs[k] + ind < es[k] && src[bs[k] + ind] == ' ') ind++;
        if (ind < base) base = ind;
    }
    if (base == (size_t)-1) base = 0;
    size_t total = 0;
    for (int k = lo; k <= hi; k++) {
        size_t bl = es[k] - bs[k];
        size_t ind = 0;
        while (ind < bl && src[bs[k] + ind] == ' ') ind++;
        total += bl - (ind < base ? ind : base) + 1;
    }
    char *out = (char *)malloc(total + 1);
    if (!out) { free(bs); free(es); return NULL; }
    size_t o = 0;
    for (int k = lo; k <= hi; k++) {
        size_t bl = es[k] - bs[k];
        size_t ind = 0;
        while (ind < bl && src[bs[k] + ind] == ' ') ind++;
        size_t strip = ind < base ? ind : base;
        if (bl > strip) { memcpy(out + o, src + bs[k] + strip, bl - strip); o += bl - strip; }
        if (k < hi) out[o++] = '\n';
    }
    out[o] = 0;
    free(bs);
    free(es);
    *out_len = o;
    return out;
}

/* 解析(输入是预清洗后的缓冲)。成功返回 1; 失败/全空返回 0 且已释放。 */
static int csv_parse(const char *src, size_t n, char sep, csv_doc *P) {
    memset(P, 0, sizeof(*P));
    if (!n) return 0;
    P->dec = (char *)malloc(n + 1);
    if (!P->dec) return 0;
    size_t i = 0, dw = 0;
    int row_no = 0;
    while (i < n) {
        int stored = (row_no <= FMT_CSV_MAX_ROWS);   /* 表头 + 前 200 数据行 */
        int cells_in_row = 0;
        for (;;) {
            fmt_cell c;
            if (src[i] == '"') {
                /* 引号字段: "" 转义; 闭引号后只允许 分隔符/行尾/EOF */
                size_t b = dw;
                int closed = 0;
                i++;
                while (i < n) {
                    if (src[i] == '"') {
                        if (i + 1 < n && src[i + 1] == '"') { P->dec[dw++] = '"'; i += 2; continue; }
                        i++;
                        closed = 1;
                        break;
                    }
                    P->dec[dw++] = src[i++];
                }
                if (!closed) { csv_free(P); return 0; }
                /* 闭引号后允许尾随空白再跟分隔符/行尾(务实口径: 真实 CSV 与
                   HTML 里缩进书写的源常带尾空格; RFC4180 严格口径会让整块
                   回落成代码块, 对 agent 场景是错误的选择)。 */
                while (i < n && (src[i] == ' ' || src[i] == '\t')) i++;
                if (i < n && src[i] != sep && src[i] != '\n' && src[i] != '\r') {
                    csv_free(P);
                    return 0;
                }
                c.s = P->dec + b;
                c.n = dw - b;
            } else {
                size_t b = i;
                while (i < n && src[i] != sep && src[i] != '\n' && src[i] != '\r') i++;
                c.s = src + b;
                c.n = i - b;
            }
            cells_in_row++;
            if (stored && cells_in_row <= FMT_CSV_MAX_COLS &&
                !csv_push_cell(P, c.s, c.n)) { csv_free(P); return 0; }
            if (i < n && src[i] == sep) { i++; continue; }
            break;
        }
        if (i < n) {
            if (src[i] == '\r') { i++; if (i < n && src[i] == '\n') i++; }
            else if (src[i] == '\n') i++;
        }
        if (row_no == 0) P->n_cols = cells_in_row;
        else P->total_rows++;
        if (stored && !csv_push_row(P, cells_in_row < FMT_CSV_MAX_COLS
                                       ? cells_in_row : FMT_CSV_MAX_COLS)) {
            csv_free(P);
            return 0;
        }
        row_no++;
    }
    if (P->n_rows < 1 || P->n_cols < 1) { csv_free(P); return 0; }
    return 1;
}

/* 已存单元格下标(行内第 col 个); 越界返回 -1 */
static int csv_cell_index(const csv_doc *P, int row, int col) {
    if (row < 0 || row >= P->n_rows) return -1;
    if (col < 0 || col >= P->row_len[row]) return -1;
    int off = 0;
    for (int r = 0; r < row; r++) off += P->row_len[r];
    return off + col;
}

/* 整格可解析为数字(去首尾空白; 必须完整消费; 拒绝 inf/nan 等词) */
static int csv_is_num(const char *s, size_t n) {
    size_t b = 0, e = n;
    while (b < e && (s[b] == ' ' || s[b] == '\t')) b++;
    while (e > b && (s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
    if (e <= b || e - b >= 63) return 0;
    unsigned char c = (unsigned char)s[b];
    if (!((c >= '0' && c <= '9') || c == '-' || c == '+' || c == '.')) return 0;
    char buf[64];
    memcpy(buf, s + b, e - b);
    buf[e - b] = 0;
    char *end = NULL;
    strtod(buf, &end);
    return end == buf + (e - b);
}

/* 单元格内联样式: 右对齐(数字列)与 pre-wrap(引号内换行保留, 否则默认
   white-space 会把换行折成空格 —— 解析保留的换行必须在排版上看得见)。
   两者可同时出现, 合并成一条声明串。 */
static void csv_cell_style(hn_node *cell, int right, const fmt_cell *c) {
    int has_nl = 0;
    if (c) for (size_t i = 0; i < c->n; i++)
        if (c->s[i] == '\n' || c->s[i] == '\r') { has_nl = 1; break; }
    if (!right && !has_nl) return;
    if (right && has_nl) hn_node_set_attr(cell, "style", "text-align: right; white-space: pre-wrap");
    else if (right) hn_node_set_attr(cell, "style", "text-align: right");
    else hn_node_set_attr(cell, "style", "white-space: pre-wrap");
}

/* 成功路径: 表头(class=csv-head)+ 体行 + 数字列右对齐 + 截断尾行 */
static void csv_render(hn_arena *ar, hn_node *into, const csv_doc *P) {
    int cols = P->n_cols < FMT_CSV_MAX_COLS ? P->n_cols : FMT_CSV_MAX_COLS;
    int body = P->n_rows - 1;
    if (body > FMT_CSV_MAX_ROWS) body = FMT_CSV_MAX_ROWS;

    /* 数字列嗅探: 分母 = 已渲染数据行数; > 70% 可解析为数 → 右对齐 */
    int align[FMT_CSV_MAX_COLS];
    for (int c = 0; c < cols; c++) {
        int numer = 0;
        for (int r = 1; r <= body; r++) {
            int idx = csv_cell_index(P, r, c);
            if (idx >= 0 && csv_is_num(P->cells[idx].s, P->cells[idx].n)) numer++;
        }
        align[c] = (body > 0 && numer * 10 > body * 7);
    }

    hn_node *table = fmt_elem(ar, "table", "csv");
    hn_dom_append(into, table);

    hn_node *htr = fmt_elem(ar, "tr", "csv-head");
    hn_dom_append(table, htr);
    for (int c = 0; c < cols; c++) {
        hn_node *th = hn_dom_elem(ar, "th");
        hn_dom_append(htr, th);
        int idx = csv_cell_index(P, 0, c);
        if (idx >= 0) fmt_text(ar, th, P->cells[idx].s, P->cells[idx].n);
        csv_cell_style(th, align[c], idx >= 0 ? &P->cells[idx] : NULL);
    }
    for (int r = 1; r <= body; r++) {
        hn_node *tr = hn_dom_elem(ar, "tr");
        hn_dom_append(table, tr);
        for (int c = 0; c < cols; c++) {
            hn_node *td = hn_dom_elem(ar, "td");
            hn_dom_append(tr, td);
            int idx = csv_cell_index(P, r, c);
            if (idx >= 0) fmt_text(ar, td, P->cells[idx].s, P->cells[idx].n);
            csv_cell_style(td, align[c], idx >= 0 ? &P->cells[idx] : NULL);
        }
    }
    if (P->total_rows > FMT_CSV_MAX_ROWS || P->n_cols > FMT_CSV_MAX_COLS) {
        hn_node *tr = fmt_elem(ar, "tr", "csv-more");
        hn_node *td = fmt_elem(ar, "td", "csv-more");
        char buf[96];
        if (P->total_rows > FMT_CSV_MAX_ROWS && P->n_cols > FMT_CSV_MAX_COLS)
            snprintf(buf, sizeof(buf), "\xE2\x80\xA6 共 %d 行 · 共 %d 列", P->total_rows, P->n_cols);
        else if (P->total_rows > FMT_CSV_MAX_ROWS)
            snprintf(buf, sizeof(buf), "\xE2\x80\xA6 共 %d 行", P->total_rows);
        else
            snprintf(buf, sizeof(buf), "\xE2\x80\xA6 共 %d 列", P->n_cols);
        hn_dom_append(table, tr);
        hn_dom_append(tr, td);
        fmt_text(ar, td, buf, strlen(buf));
    }
}

/* ---------- 装载期入口 ---------- */

/* 收集元素后代的文本总量(文档序; 与 hn_node_text_content 同口径) */
static size_t fmt_measure_text(hn_node *n) {
    size_t total = 0;
    for (hn_node *ch = n->first; ch; ch = ch->next) {
        if (ch->kind == HN_TEXT && ch->text && ch->text_len) total += ch->text_len;
        else if (ch->kind == HN_ELEM) total += fmt_measure_text(ch);
    }
    return total;
}

static void fmt_collect_text(hn_node *n, char *out, size_t *used) {
    for (hn_node *ch = n->first; ch; ch = ch->next) {
        if (ch->kind == HN_TEXT && ch->text && ch->text_len) {
            memcpy(out + *used, ch->text, ch->text_len);
            *used += ch->text_len;
        } else if (ch->kind == HN_ELEM) {
            fmt_collect_text(ch, out, used);
        }
    }
}

/* 幂等判定: 已转换 ⇔ (带了 hn-fmt 属性) 且 (首个子节点是元素)。
   两个条件缺一不可 —— 只看属性会把宿主经 hn_doc_swap 换上的新原文当已转换
   而跳过(属性不随 inner swap 消失); 只看首节点类型会把"源里以 HTML 标签
   开头"的容器(违反文档约束但可能出现)误判为已转换。转换产物首节点必为
   json-head / json-err / table / pre 之一。 */
static int fmt_is_converted(hn_node *el) {
    return hn_node_attr(el, "hn-fmt") != NULL &&
           el->first && el->first->kind == HN_ELEM;
}

int hn_fmt_apply(hn_doc *doc) {
    if (!doc || !doc->root) return 0;
    /* 收集与转换分离(与 hn_md.c 同一教训: 转换后 hn-json/hn-csv 属性仍在,
       边找边转会重复命中同一元素)。 */
    int n = 0, cap = 0;
    hn_node **els = NULL;
    struct { hn_node **v; int n, cap; } st = {0};
    #define FMT_PUSH(p) do { \
        if (st.n == st.cap) { \
            int nc = st.cap ? st.cap * 2 : 64; \
            hn_node **nv = (hn_node **)realloc(st.v, sizeof(hn_node *) * (size_t)nc); \
            if (!nv) break; \
            st.v = nv; st.cap = nc; \
        } \
        st.v[st.n++] = (p); \
    } while (0)
    FMT_PUSH(doc->root);
    while (st.n > 0) {
        hn_node *nd = st.v[--st.n];
        if (nd->kind == HN_ELEM &&
            (hn_node_attr(nd, "hn-json") || hn_node_attr(nd, "hn-csv")) &&
            !fmt_is_converted(nd)) {
            if (n == cap) {
                int nc = cap ? cap * 2 : 8;
                hn_node **nv = (hn_node **)realloc(els, sizeof(hn_node *) * (size_t)nc);
                if (!nv) break;
                els = nv;
                cap = nc;
            }
            els[n++] = nd;
        }
        for (hn_node *ch = nd->first; ch; ch = ch->next) FMT_PUSH(ch);
    }
    free(st.v);
    #undef FMT_PUSH

    int converted = 0;
    for (int i = 0; i < n; i++) {
        hn_node *el = els[i];
        if (!el->arena) continue;
        const char *kind = hn_node_attr(el, "hn-json") ? "json" : "csv";
        size_t need = fmt_measure_text(el);
        char *buf = (char *)malloc(need + 1);
        if (!buf) continue;
        size_t got = 0;
        fmt_collect_text(el, buf, &got);
        buf[got] = 0;
        /* 清空原内容: 源文本替换成普通 DOM 子树(元素自身与属性保留) */
        el->first = el->last = NULL;
        if (!strcmp(kind, "json")) {
            if (got == 0) {
                json_fallback(el->arena, el, buf, got);
            } else {
                /* 解析器就地改写输入 → 复制一份再解析(入参缓冲留给回落用) */
                char *jb = (char *)malloc(got + 1);
                if (!jb) {
                    json_fallback(el->arena, el, buf, got);
                } else {
                    memcpy(jb, buf, got);
                    jb[got] = 0;
                    void *ja = NULL;
                    hn_json *root = hn_json_parse(jb, got, &ja);
                    if (root) json_render(el->arena, el, got, root);
                    else json_fallback(el->arena, el, buf, got);
                    if (ja) hn_json_free(ja);
                    free(jb);
                }
            }
        } else {
            const char *sep_attr = hn_node_attr(el, "hn-csv-sep");
            char sep = (sep_attr && strlen(sep_attr) == 1) ? sep_attr[0] : ',';
            size_t clean_len = 0;
            char *clean = (got > 0) ? csv_prep(buf, got, &clean_len) : NULL;
            csv_doc P;
            if (clean && csv_parse(clean, clean_len, sep, &P)) {
                csv_render(el->arena, el, &P);
                csv_free(&P);
            } else {
                fmt_src_block(el->arena, el, buf, got);   /* 失败/空: 原样代码块 */
            }
            free(clean);
        }
        hn_node_set_attr(el, "hn-fmt", kind);   /* 幂等标记(不参与渲染) */
        free(buf);
        converted++;
    }
    free(els);
    return converted;
}

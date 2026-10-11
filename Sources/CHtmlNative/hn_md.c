/* hn_md.c — Markdown 直染: <div hn-md>源</div> → 普通 DOM 子树
 *
 * 定位: agent 的母语是 markdown。智能交互 UI 必须能直接吃 markdown 并渲染
 * 成样式化桌面 UI —— agent 生成 <div hn-md>...</div>, 引擎在装载序列里
 * (hn_context_set_doc / hn_context_render / hn_doc_swap 三个入口, 都在
 * 首次 layout 之前、<script> 执行之前)把 md 源**替换成普通 DOM 子树**
 * (h1/ul/table/... ), 之后样式/布局/绘制/交互全部走既有管线, 零特例。
 * 这是本特性的核心设计决策: md 一进来就变成 hn 编码。
 *
 * 源文本的取得: HTML 解析器把元素内容当普通内容收集 —— 纯 md 源(无内嵌
 * 标签)会成为原样空白的文本节点, 这里按文档序拼接全部后代的文本节点。
 * 由此带来两条作者约束(注释即契约, v1 不做防护):
 *   1. md 源里不要写 HTML 标签 —— 会被 HTML 解析器解析成元素(其文本
 *      仍会被拼进源里, 但结构标签本身消失); v1 本就不支持内嵌 HTML。
 *   2. 实体会被 HTML 层解码一次(&amp; → &), 这正是 HTML 语义;
 *      md 源里要表达字面 & 应写 &amp;。
 *
 * v1 块级子集:
 *   ATX 标题 # ~ ######        → h1..h6
 *   段落                        → p(软换行折成空格)
 *   无序/有序列表 - * + / N. N) → ul / ol(嵌套缩进 2 空格, 层级自然递归)
 *   任务清单 - [ ] / - [x]      → li + <input type=checkbox checked>
 *                                 (既有控件语义; li 加 class=task 与
 *                                  list-style:none 取消圆点标记)
 *   围栏代码块 ```lang          → pre > code(data-lang 存语言)
 *   引用 >                      → blockquote(内容递归走块级解析)
 *   管道表格 | a | b |          → table > tr > th/td(表头行 + 分隔行
 *                                 识别; :---:/---: 对齐落 text-align;
 *                                 tr 走 flex 行、单元格 flex:1 等分)
 *   分隔线 --- / *** / ___      → hr
 * v1 行内子集:
 *   **粗** → strong   *斜* → em   `行内码` → code(内不递归)
 *   ~~删除~~ → s   [文本](url) → a(href 存属性; 点击行为后续轮次)
 *   \ 转义后随字符(\` \* \~ \[ \\ 等)。
 * 明确不做(声明即契约): 图片 ![]()(按字面文本渲染)、HTML 内嵌、脚注、
 * 定义列表、setext 标题(=== / --- 下划线式)、缩进式代码块、引用的懒惰
 * 续行(> 行之间夹非 > 行会断开引用)、列表项间空行(空行结束列表 ——
 * 只做紧凑列表)。
 */
#include <stdlib.h>
#include <string.h>
#include "hn_internal.h"

/* ---------- 行视图 ---------- */

typedef struct {
    const char *s;      /* 行起点(原始缓冲内的切片) */
    size_t n;           /* 行长(不含换行; 行尾 '\r' 已剥) */
    int indent;         /* 前导空格数(v1 只认空格缩进, tab 不作缩进) */
    int blank;          /* 空行 / 纯空白行 */
} md_line;

static int is_space_ch(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* 去首尾空白的行内容 */
static void line_trim(const md_line *l, const char **s, size_t *n) {
    const char *b = l->s;
    size_t k = l->n;
    while (k && is_space_ch(*b)) { b++; k--; }
    while (k && is_space_ch(b[k - 1])) k--;
    *s = b;
    *n = k;
}

/* 把源文本切成行视图(切片指向 text, 不改写缓冲)。失败返回 NULL。 */
static md_line *md_split_lines(const char *text, size_t len, int *out_n) {
    size_t cap = 1;
    for (size_t i = 0; i < len; i++)
        if (text[i] == '\n') cap++;
    md_line *L = (md_line *)malloc(sizeof(md_line) * cap);
    if (!L) return NULL;
    int n = 0;
    size_t i = 0;
    while (i <= len) {
        if (i == len && n > 0 && text[len - 1] == '\n') break;  /* 末换行不产空行 */
        size_t b = i;
        while (i < len && text[i] != '\n') i++;
        size_t e = i;                                    /* 行尾(不含 '\n') */
        if (e > b && text[e - 1] == '\r') e--;           /* CRLF */
        int indent = 0;
        while (b + (size_t)indent < e && text[b + indent] == ' ') indent++;
        int blank = 1;
        for (size_t k = b; k < e; k++)
            if (!is_space_ch(text[k])) { blank = 0; break; }
        L[n].s = text + b;
        L[n].n = e - b;
        L[n].indent = indent;
        L[n].blank = blank;
        n++;
        if (i >= len) break;
        i++;                                             /* 跳过 '\n' */
    }
    *out_n = n;
    return L;
}

/* 公共缩进剥离: 所有非空行共同的前导空格剥掉一层 —— HTML 里缩进书写的
   md 源(美化排版)不必顶格也能正确分块。 */
static void md_dedent(md_line *L, int n) {
    int base = -1;
    for (int i = 0; i < n; i++)
        if (!L[i].blank) {
            int k = L[i].indent;
            base = base < 0 ? k : (k < base ? k : base);
        }
    if (base <= 0) return;
    for (int i = 0; i < n; i++) {
        if (L[i].blank) continue;
        int k = L[i].indent < base ? L[i].indent : base;
        L[i].s += k;
        L[i].n -= (size_t)k;
        L[i].indent -= k;
    }
}

/* 行区间 [lo,hi) 的子视图(剥 dedent 层前导空格), 调用方 free */
static md_line *md_subview(const md_line *L, int lo, int hi, int dedent, int *out_n) {
    int n = hi - lo;
    md_line *v = (md_line *)malloc(sizeof(md_line) * (size_t)(n > 0 ? n : 1));
    if (!v) { *out_n = 0; return NULL; }
    for (int i = 0; i < n; i++) {
        v[i] = L[lo + i];
        if (!v[i].blank) {
            int k = v[i].indent < dedent ? v[i].indent : dedent;
            v[i].s += k;
            v[i].n -= (size_t)k;
            v[i].indent -= k;
        }
    }
    *out_n = n;
    return v;
}

/* ---------- 块级识别 ---------- */

/* 围栏代码块开栏: 返回反引号个数(>=3), 0 = 不是; lang 为信息串首 token */
static int fence_open(const md_line *l, const char **lang, size_t *lang_n) {
    if (l->blank || l->indent > 3) return 0;
    const char *s;
    size_t n;
    line_trim(l, &s, &n);
    if (n < 3 || s[0] != '`' || s[1] != '`' || s[2] != '`') return 0;
    int k = 0;
    while ((size_t)k < n && s[k] == '`') k++;
    size_t i = (size_t)k;
    while (i < n && s[i] == ' ') i++;
    size_t b = i;
    while (i < n && !is_space_ch(s[i])) i++;   /* 首个空白分隔 token = 语言 */
    *lang = s + b;
    *lang_n = i - b;
    return k;
}

/* 围栏闭栏: 反引号个数 >= 开栏个数 */
static int fence_close(const md_line *l, int open_k) {
    if (l->blank || l->indent > 3) return 0;
    const char *s;
    size_t n;
    line_trim(l, &s, &n);
    if (n < (size_t)open_k) return 0;
    for (size_t i = 0; i < n; i++)
        if (s[i] != '`') return 0;
    return 1;
}

static const char *H_TAG[7] = { NULL, "h1", "h2", "h3", "h4", "h5", "h6" };

/* ATX 标题: 1..6 个 '#' 后跟空格或行尾 */
static int atx_level(const md_line *l, const char **s, size_t *n) {
    if (l->blank || l->indent > 3) return 0;
    const char *p;
    size_t k;
    line_trim(l, &p, &k);
    int lv = 0;
    while ((size_t)lv < k && p[lv] == '#') lv++;
    if (lv < 1 || lv > 6) return 0;
    if ((size_t)lv < k && p[lv] != ' ') return 0;   /* #tag 不算标题 */
    size_t b = (size_t)lv;
    while (b < k && p[b] == ' ') b++;
    *s = p + b;
    *n = k - b;
    return lv;
}

/* 分隔线: 同一字符(- * _)出现 >=3 次, 之间只允许空格 */
static int is_hr(const md_line *l) {
    if (l->blank || l->indent > 3) return 0;
    const char *s;
    size_t n;
    line_trim(l, &s, &n);
    if (n < 3) return 0;
    char c = s[0];
    if (c != '-' && c != '*' && c != '_') return 0;
    int cnt = 0;
    for (size_t i = 0; i < n; i++) {
        if (s[i] == c) cnt++;
        else if (s[i] != ' ') return 0;
    }
    return cnt >= 3;
}

/* 引用标记: 剥 '>' 及其后空白, 返回剩余内容 */
static int quote_rest(const md_line *l, const char **s, size_t *n) {
    if (l->blank || l->indent > 3) return 0;
    const char *p;
    size_t k;
    line_trim(l, &p, &k);
    if (!k || p[0] != '>') return 0;
    size_t b = 1;
    while (b < k && p[b] == ' ') b++;
    *s = p + b;
    *n = k - b;
    return 1;
}

/* 列表项标记: 无序 - * +(后跟空格/行尾); 有序 数字 + . )(后跟空格/行尾)。
   命中返回 1, ol/num/content(剥标记与后续空格)写出。 */
static int list_marker(const md_line *l, int *ol, long *num,
                       const char **content, size_t *cn) {
    if (l->blank || l->indent > 3) return 0;
    const char *s;
    size_t n;
    line_trim(l, &s, &n);
    if (!n) return 0;
    size_t m = 0;
    if (s[0] == '-' || s[0] == '*' || s[0] == '+') {
        m = 1;
        *ol = 0;
    } else {
        while (m < n && s[m] >= '0' && s[m] <= '9') m++;
        if (m == 0 || m > 9 || m >= n || (s[m] != '.' && s[m] != ')')) return 0;
        long v = 0;
        for (size_t i = 0; i < m; i++) v = v * 10 + (s[i] - '0');
        *ol = 1;
        *num = v;
        m++;                                       /* 跳过 . 或 ) */
    }
    if (m < n && s[m] != ' ') return 0;            /* 必须跟空格/行尾 */
    while (m < n && s[m] == ' ') m++;
    *content = s + m;
    *cn = n - m;
    return 1;
}

/* 任务清单前缀: [ ] / [x] / [X] 后跟空格或行尾 */
static int task_marker(const char *s, size_t n, int *checked,
                       const char **rest, size_t *rn) {
    if (n < 3 || s[0] != '[') return 0;
    if (s[2] != ']') return 0;
    if (s[1] != ' ' && s[1] != 'x' && s[1] != 'X') return 0;
    if (n > 3 && s[3] != ' ') return 0;
    *checked = (s[1] != ' ');
    size_t b = n > 3 ? 4 : 3;
    while (b < n && s[b] == ' ') b++;
    *rest = s + b;
    *rn = n - b;
    return 1;
}

/* 行是否含未转义管道(表格候选) */
static int line_has_pipe(const md_line *l) {
    const char *s;
    size_t n;
    line_trim(l, &s, &n);
    int esc = 0;
    for (size_t i = 0; i < n; i++) {
        if (esc) { esc = 0; continue; }
        if (s[i] == '\\') { esc = 1; continue; }
        if (s[i] == '|') return 1;
    }
    return 0;
}

/* 按未转义 '|' 切单元格(去首尾管道; 支持 \| 转义 —— 反斜杠保留, 由行内
   解析统一处理)。最多 max 个, 返回实际个数。 */
static int split_cells(const md_line *l, const char **cs, size_t *cns, int max) {
    const char *s;
    size_t n;
    line_trim(l, &s, &n);
    if (n && s[0] == '|') { s++; n--; }
    if (n && s[n - 1] == '|') n--;
    int cnt = 0;
    size_t b = 0;
    int esc = 0;
    for (size_t i = 0; i <= n; i++) {
        int end = (i == n);
        char c = end ? '|' : s[i];
        if (!end && esc) { esc = 0; continue; }
        if (!end && c == '\\') { esc = 1; continue; }
        if (c != '|') continue;
        size_t e = i;
        while (e > b && is_space_ch(s[e - 1])) e--;
        size_t bb = b;
        while (bb < e && is_space_ch(s[bb])) bb++;
        if (cnt < max) { cs[cnt] = s + bb; cns[cnt] = e - bb; }
        cnt++;
        b = i + 1;
        esc = 0;
    }
    return cnt;
}

/* 分隔行: 全部单元格匹配 :?-+:?(至少一行) */
static int is_delim_row(const md_line *l) {
    const char *cs[32];
    size_t cns[32];
    int n = split_cells(l, cs, cns, 32);
    if (n < 1) return 0;
    for (int i = 0; i < n; i++) {
        size_t k = cns[i];
        const char *p = cs[i];
        size_t b = 0, e = k;
        if (b < e && p[b] == ':') b++;
        if (e > b && p[e - 1] == ':') e--;
        if (e <= b) return 0;
        for (size_t j = b; j < e; j++)
            if (p[j] != '-') return 0;
    }
    return 1;
}

/* 行内某单元格的对齐(:---: 1=center, ---: 2=right, 其余 0=left) */
static int cell_align(const md_line *l, int col) {
    const char *cs[32];
    size_t cns[32];
    int n = split_cells(l, cs, cns, 32);
    if (col < 0 || col >= n) return 0;
    const char *p = cs[col];
    size_t k = cns[col];
    int lead = k && p[0] == ':';
    int trail = k && p[k - 1] == ':';
    if (lead && trail) return 1;
    if (trail) return 2;
    return 0;
}

/* ---------- 前置声明 ---------- */
static void parse_blocks(hn_arena *ar, hn_node *parent, const md_line *L, int lo, int hi);
static void md_inline(hn_arena *ar, hn_node *parent, const char *s, size_t n);

/* ---------- 块级解析 ---------- */

/* 当前行是否开启新块(段落收集的终止条件) */
static int block_starts(const md_line *l) {
    const char *d;
    size_t dn;
    int ol;
    long num;
    const char *c;
    size_t cn;
    return fence_open(l, &d, &dn) || atx_level(l, &d, &dn) || is_hr(l) ||
           quote_rest(l, &d, &dn) || list_marker(l, &ol, &num, &c, &cn);
}

/* 列表: 从 lo 开始收集同级同型条目, 返回消费到的行号(未命中返回 lo)。 */
static int parse_list(hn_arena *ar, hn_node *parent, const md_line *L, int lo, int hi) {
    int ol;
    long num;
    const char *c;
    size_t cn;
    if (!list_marker(&L[lo], &ol, &num, &c, &cn)) return lo;
    hn_node *list = hn_dom_elem(ar, ol ? "ol" : "ul");
    hn_dom_append(parent, list);
    int base_indent = L[lo].indent;
    int i = lo;
    while (i < hi) {
        if (L[i].blank) break;                       /* 空行结束列表(紧凑口径) */
        int iol;
        long inum;
        const char *ic;
        size_t icn;
        if (L[i].indent != base_indent ||
            !list_marker(&L[i], &iol, &inum, &ic, &icn) || iol != ol)
            break;                                   /* 同级同型标记才续列表 */
        hn_node *li = hn_dom_elem(ar, "li");
        hn_dom_append(list, li);
        /* 任务清单: [ ] / [x] 前缀 → 既有 checkbox 控件语义 + 取消圆点标记 */
        int checked;
        const char *rc;
        size_t rcn;
        if (task_marker(ic, icn, &checked, &rc, &rcn)) {
            hn_node_set_attr(li, "class", "task");
            hn_node_set_attr(li, "style", "list-style: none");
            hn_node *cb = hn_dom_elem(ar, "input");
            hn_node_set_attr(cb, "type", "checkbox");
            if (checked) hn_node_set_attr(cb, "checked", "");
            hn_dom_append(li, cb);
            ic = rc;
            icn = rcn;
        }
        if (icn) md_inline(ar, li, ic, icn);
        /* 收集本项的后续行: 缩进更深 = 嵌套块(递归解析进 li);
           同级非标记行 = 懒惰续行(并入内联文本)。 */
        int j = i + 1;
        int sub_lo = -1, sub_hi = -1;
        while (j < hi) {
            if (L[j].blank) {
                /* 空行: 后面若仍是本项的更深缩进行则越过, 否则结束收集 */
                if (j + 1 < hi && !L[j + 1].blank && L[j + 1].indent > base_indent) { j++; continue; }
                break;
            }
            if (L[j].indent > base_indent) {
                if (sub_lo < 0) sub_lo = j;
                sub_hi = j + 1;
                j++;
                continue;
            }
            int ol2;
            long n2;
            const char *c2;
            size_t cn2;
            if (L[j].indent == base_indent && list_marker(&L[j], &ol2, &n2, &c2, &cn2) && ol2 == ol)
                break;                               /* 下一条目 */
            /* 懒惰续行: 以空格并入本项内联 */
            hn_dom_append(li, hn_dom_text(ar, " ", 1));
            size_t off = (size_t)L[j].indent;
            md_inline(ar, li, L[j].s + off, L[j].n - off);
            j++;
        }
        if (sub_lo >= 0) {
            int sn;
            /* 嵌套块按"基准缩进 + 2"剥层(v1 口径: 一级嵌套 2 空格) */
            md_line *sub = md_subview(L, sub_lo, sub_hi, base_indent + 2, &sn);
            if (sub) {
                parse_blocks(ar, li, sub, 0, sn);
                free(sub);
            }
        }
        i = j;
    }
    return i > lo ? i : lo + 1;                      /* 至少消费首行, 防死循环 */
}

/* 表格: 表头行(lo) + 分隔行(lo+1) + 若干体行; 返回消费到的行号 */
static int parse_table(hn_arena *ar, hn_node *parent, const md_line *L, int lo, int hi) {
    const char *hcs[32];
    size_t hcns[32];
    int ncols = split_cells(&L[lo], hcs, hcns, 32);
    if (ncols < 1) return lo + 1;

    hn_node *table = hn_dom_elem(ar, "table");
    hn_dom_append(parent, table);

    /* 表头 */
    hn_node *tr = hn_dom_elem(ar, "tr");
    hn_dom_append(table, tr);
    for (int i = 0; i < ncols; i++) {
        hn_node *th = hn_dom_elem(ar, "th");
        hn_dom_append(tr, th);
        md_inline(ar, th, hcs[i], hcns[i]);
    }
    int align[32];
    for (int i = 0; i < 32; i++) align[i] = cell_align(&L[lo + 1], i);

    /* 体行: 连续含管道的非空行 */
    int i = lo + 2;
    while (i < hi && !L[i].blank && line_has_pipe(&L[i])) {
        const char *bcs[32];
        size_t bcns[32];
        int nb = split_cells(&L[i], bcs, bcns, 32);
        hn_node *btr = hn_dom_elem(ar, "tr");
        hn_dom_append(table, btr);
        for (int k = 0; k < nb; k++) {
            hn_node *td = hn_dom_elem(ar, "td");
            hn_dom_append(btr, td);
            /* 对齐: 分隔行列出的声明落 text-align(单元格自身样式) */
            if (k < 32 && align[k] == 1) hn_node_set_attr(td, "style", "text-align: center");
            else if (k < 32 && align[k] == 2) hn_node_set_attr(td, "style", "text-align: right");
            md_inline(ar, td, bcs[k], bcns[k]);
        }
        i++;
    }
    return i;
}

static void parse_blocks(hn_arena *ar, hn_node *parent, const md_line *L, int lo, int hi) {
    int i = lo;
    while (i < hi) {
        const md_line *l = &L[i];
        if (l->blank) { i++; continue; }

        /* 1. 围栏代码块: 内容原样(raw), 不剥缩进、不做行内解析 */
        {
            const char *lang = NULL;
            size_t lang_n = 0;
            int fk = fence_open(l, &lang, &lang_n);
            if (fk) {
                int j = i + 1;
                while (j < hi && !fence_close(&L[j], fk)) j++;
                hn_node *pre = hn_dom_elem(ar, "pre");
                hn_node *code = hn_dom_elem(ar, "code");
                if (lang_n) {
                    char lv[64];
                    size_t take = lang_n < sizeof(lv) - 1 ? lang_n : sizeof(lv) - 1;
                    memcpy(lv, lang, take);
                    lv[take] = 0;
                    hn_node_set_attr(code, "data-lang", lv);
                }
                hn_dom_append(parent, pre);
                hn_dom_append(pre, code);
                /* 内容行以 '\n' 连接(不带尾换行: 否则 pre 排版多出空行) */
                size_t total = 0;
                for (int k = i + 1; k < j; k++)
                    total += L[k].n + (k + 1 < j ? 1 : 0);
                if (total) {
                    char *buf = (char *)hn_arena_alloc(ar, total);
                    size_t o = 0;
                    for (int k = i + 1; k < j; k++) {
                        memcpy(buf + o, L[k].s, L[k].n);
                        o += L[k].n;
                        if (k + 1 < j) buf[o++] = '\n';
                    }
                    hn_dom_append(code, hn_dom_text(ar, buf, o));
                }
                i = j < hi ? j + 1 : j;              /* 消费闭栏(若有) */
                continue;
            }
        }

        /* 2. ATX 标题 */
        {
            const char *c = NULL;
            size_t cn = 0;
            int lv = atx_level(l, &c, &cn);
            if (lv) {
                hn_node *h = hn_dom_elem(ar, H_TAG[lv]);
                hn_dom_append(parent, h);
                if (cn) md_inline(ar, h, c, cn);
                i++;
                continue;
            }
        }

        /* 3. 分隔线 */
        if (is_hr(l)) {
            hn_dom_append(parent, hn_dom_elem(ar, "hr"));
            i++;
            continue;
        }

        /* 4. 引用: 连续的 '>' 行剥标记后递归走块级解析 */
        {
            const char *q = NULL;
            size_t qn = 0;
            if (quote_rest(l, &q, &qn)) {
                int j = i;
                while (j < hi) {
                    const char *d = NULL;
                    size_t dn = 0;
                    if (!quote_rest(&L[j], &d, &dn)) break;
                    j++;
                }
                hn_node *bq = hn_dom_elem(ar, "blockquote");
                hn_dom_append(parent, bq);
                /* 子视图: 每行替换为剥 '>' 后的内容 */
                int cnt = j - i;
                md_line *sub = (md_line *)malloc(sizeof(md_line) * (size_t)(cnt > 0 ? cnt : 1));
                if (sub) {
                    for (int k = 0; k < cnt; k++) {
                        const char *d = NULL;
                        size_t dn = 0;
                        quote_rest(&L[i + k], &d, &dn);
                        int ind = 0;
                        while ((size_t)ind < dn && d[ind] == ' ') ind++;
                        sub[k].s = d + ind;
                        sub[k].n = dn - (size_t)ind;
                        sub[k].indent = ind;
                        sub[k].blank = dn == 0;
                    }
                    parse_blocks(ar, bq, sub, 0, cnt);
                    free(sub);
                }
                i = j;
                continue;
            }
        }

        /* 5. 表格: 当前行含管道且下一行是分隔行 */
        if (i + 1 < hi && line_has_pipe(l) && is_delim_row(&L[i + 1])) {
            i = parse_table(ar, parent, L, i, hi);
            continue;
        }

        /* 6. 列表(无序/有序/任务清单) */
        {
            int used = parse_list(ar, parent, L, i, hi);
            if (used > i) { i = used; continue; }
        }

        /* 7. 段落: 连续普通行折成一个 p(软换行 → 空格) */
        {
            int j = i;
            while (j < hi && !L[j].blank && !block_starts(&L[j])) {
                /* 段中遇到"表格头+分隔行"也断开, 交回下一轮解析 */
                if (j > i && j + 1 < hi && line_has_pipe(&L[j]) && is_delim_row(&L[j + 1])) break;
                j++;
            }
            size_t total = 0;
            for (int k = i; k < j; k++) {
                const char *cs;
                size_t csn;
                line_trim(&L[k], &cs, &csn);
                total += csn + (k + 1 < j ? 1 : 0);
            }
            hn_node *p = hn_dom_elem(ar, "p");
            hn_dom_append(parent, p);
            if (total) {
                char *buf = (char *)hn_arena_alloc(ar, total);
                size_t o = 0;
                for (int k = i; k < j; k++) {
                    const char *cs;
                    size_t csn;
                    line_trim(&L[k], &cs, &csn);
                    memcpy(buf + o, cs, csn);
                    o += csn;
                    if (k + 1 < j) buf[o++] = ' ';
                }
                md_inline(ar, p, buf, o);
            }
            i = j > i ? j : i + 1;
        }
    }
}

/* ---------- 行内解析 ----------
 * 在文本流里生成混排子节点(文本 + strong/em/code/s/a)。
 * 口径: 找最早的特殊标记, 标记前的字节累积成文本节点; 各构造的内容
 * 递归走行内解析(嵌套自然成立, 如 **粗里有`码`**)。未闭合的标记按
 * 字面文本处理 —— 不吞字、不报错, 与引擎"解析失败跳过"的口径一致。 */

static void md_inline(hn_arena *ar, hn_node *parent, const char *s, size_t n) {
    size_t i = 0, tok = 0;
#define MD_FLUSH() do { \
        if (i > tok) hn_dom_append(parent, hn_dom_text(ar, s + tok, i - tok)); \
    } while (0)

    while (i < n) {
        char c = s[i];

        /* 转义: 反斜杠 + 标点 → 字面字符 */
        if (c == '\\' && i + 1 < n && strchr("\\`*_~[]()#<>|!", s[i + 1])) {
            MD_FLUSH();
            hn_dom_append(parent, hn_dom_text(ar, s + i + 1, 1));
            i += 2;
            tok = i;
            continue;
        }

        /* 行内码: `...`(内容不递归) */
        if (c == '`') {
            const char *cl = (i + 1 < n) ? (const char *)memchr(s + i + 1, '`', n - i - 1) : NULL;
            if (cl) {
                MD_FLUSH();
                hn_node *code = hn_dom_elem(ar, "code");
                hn_dom_append(parent, code);
                size_t clen = (size_t)(cl - (s + i + 1));
                if (clen) hn_dom_append(code, hn_dom_text(ar, s + i + 1, clen));
                i = (size_t)(cl - s) + 1;
                tok = i;
                continue;
            }
            i++;
            continue;
        }

        /* 删除: ~~...~~ */
        if (c == '~' && i + 1 < n && s[i + 1] == '~') {
            size_t j = i + 2;
            while (j + 1 < n && !(s[j] == '~' && s[j + 1] == '~')) j++;
            if (j + 1 < n) {
                MD_FLUSH();
                hn_node *el = hn_dom_elem(ar, "s");
                hn_dom_append(parent, el);
                md_inline(ar, el, s + i + 2, j - i - 2);
                i = j + 2;
                tok = i;
                continue;
            }
            i++;
            continue;
        }

        /* 粗体: **...**(先于斜体判定) */
        if (c == '*' && i + 1 < n && s[i + 1] == '*') {
            size_t j = i + 2;
            while (j + 1 < n && !(s[j] == '*' && s[j + 1] == '*')) j++;
            if (j + 1 < n) {
                MD_FLUSH();
                hn_node *el = hn_dom_elem(ar, "strong");
                hn_dom_append(parent, el);
                md_inline(ar, el, s + i + 2, j - i - 2);
                i = j + 2;
                tok = i;
                continue;
            }
            MD_FLUSH();
            hn_dom_append(parent, hn_dom_text(ar, "**", 2));
            i += 2;
            tok = i;
            continue;
        }

        /* 斜体: *...*(内容非空) */
        if (c == '*') {
            const char *cl = (i + 1 < n) ? (const char *)memchr(s + i + 1, '*', n - i - 1) : NULL;
            if (cl && cl > s + i + 1) {
                MD_FLUSH();
                hn_node *el = hn_dom_elem(ar, "em");
                hn_dom_append(parent, el);
                md_inline(ar, el, s + i + 1, (size_t)(cl - (s + i + 1)));
                i = (size_t)(cl - s) + 1;
                tok = i;
                continue;
            }
            i++;
            continue;
        }

        /* 图片 ![](): v1 明确不做 —— 完整语法按字面文本渲染 */
        if (c == '!' && i + 1 < n && s[i + 1] == '[') {
            size_t j = i + 2;
            while (j < n && s[j] != ']') j++;
            if (j < n && j + 1 < n && s[j + 1] == '(') {
                size_t k = j + 2;
                while (k < n && s[k] != ')') k++;
                if (k < n) {
                    MD_FLUSH();
                    hn_dom_append(parent, hn_dom_text(ar, s + i, k - i + 1));
                    i = k + 1;
                    tok = i;
                    continue;
                }
            }
            i++;
            continue;
        }

        /* 链接 [文本](url): href 存属性, 点击行为由后续轮次接线 */
        if (c == '[') {
            size_t j = i + 1;
            while (j < n && s[j] != ']') j++;
            if (j < n && j + 1 < n && s[j + 1] == '(') {
                size_t k = j + 2;
                while (k < n && s[k] != ')') k++;
                if (k < n) {
                    const char *u = s + j + 2;
                    size_t un = k - (j + 2);
                    while (un && is_space_ch(*u)) { u++; un--; }
                    while (un && is_space_ch(u[un - 1])) un--;
                    MD_FLUSH();
                    hn_node *a = hn_dom_elem(ar, "a");
                    char *uv = hn_arena_strndup(ar, u, un);
                    hn_node_set_attr(a, "href", uv);
                    hn_dom_append(parent, a);
                    if (j > i + 1) md_inline(ar, a, s + i + 1, j - i - 1);
                    i = k + 1;
                    tok = i;
                    continue;
                }
            }
            i++;
            continue;
        }

        i++;
    }
    MD_FLUSH();
#undef MD_FLUSH
}

/* ---------- 入口 ---------- */

/* 收集元素后代的文本总量(文档序; 与 hn_node_text_content 同口径) */
static size_t md_measure_text(hn_node *n) {
    size_t total = 0;
    for (hn_node *ch = n->first; ch; ch = ch->next) {
        if (ch->kind == HN_TEXT && ch->text && ch->text_len) total += ch->text_len;
        else if (ch->kind == HN_ELEM) total += md_measure_text(ch);
    }
    return total;
}

static void md_collect_text(hn_node *n, char *out, size_t *used) {
    for (hn_node *ch = n->first; ch; ch = ch->next) {
        if (ch->kind == HN_TEXT && ch->text && ch->text_len) {
            memcpy(out + *used, ch->text, ch->text_len);
            *used += ch->text_len;
        } else if (ch->kind == HN_ELEM) {
            md_collect_text(ch, out, used);
        }
    }
}

static void md_parse(hn_arena *ar, hn_node *parent, const char *src, size_t len) {
    int n = 0;
    md_line *L = md_split_lines(src, len, &n);
    if (!L) return;
    md_dedent(L, n);
    parse_blocks(ar, parent, L, 0, n);
    free(L);
}

int hn_md_apply(hn_doc *doc) {
    if (!doc || !doc->root) return 0;
    /* 先一次性收集全部 hn-md 元素再转换: 转换后元素仍带着 hn-md 属性
       (引擎没有删属性的原语), 若"找一个转一个"会无限重找同一元素 ——
       实测 OOM 教训。收集与转换分离, 天然幂等(单次装载只过一遍)。 */
    int n = 0, cap = 0;
    hn_node **els = NULL;
    /* 显式栈遍历(与 hn_doc_validate 同款), 找出全部带 hn-md 的元素 */
    struct { hn_node **v; int n, cap; } st = {0};
    #define MD_PUSH(p) do { \
        if (st.n == st.cap) { \
            int nc = st.cap ? st.cap * 2 : 64; \
            hn_node **nv = (hn_node **)realloc(st.v, sizeof(hn_node *) * (size_t)nc); \
            if (!nv) break; \
            st.v = nv; st.cap = nc; \
        } \
        st.v[st.n++] = (p); \
    } while (0)
    MD_PUSH(doc->root);
    while (st.n > 0) {
        hn_node *nd = st.v[--st.n];
        if (nd->kind == HN_ELEM && hn_node_attr(nd, "hn-md")) {
            if (n == cap) {
                int nc = cap ? cap * 2 : 8;
                hn_node **nv = (hn_node **)realloc(els, sizeof(hn_node *) * (size_t)nc);
                if (!nv) break;
                els = nv;
                cap = nc;
            }
            els[n++] = nd;
        }
        for (hn_node *ch = nd->first; ch; ch = ch->next) MD_PUSH(ch);
    }
    free(st.v);
    #undef MD_PUSH
    int converted = 0;
    for (int i = 0; i < n; i++) {
        hn_node *el = els[i];
        if (!el->arena) continue;
        size_t need = md_measure_text(el);
        char *buf = (char *)malloc(need + 1);
        if (!buf) continue;
        size_t got = 0;
        md_collect_text(el, buf, &got);
        /* 清空原内容, md 源替换为普通 DOM 子树(元素自身与属性保留) */
        el->first = el->last = NULL;
        if (got) md_parse(el->arena, el, buf, got);
        free(buf);
        converted++;
    }
    free(els);
    return converted;
}

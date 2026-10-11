/* hn_html.c — HTML 子集解析器 → DOM
 * 支持: 元素/属性(void 元素, 自闭合), 文本实体解码 + 空白折叠,
 * <style>/<script> 原文收集(style 内容直接解析为样式表), 注释/doctype 跳过,
 * <include src> 装载期文本级拼接(零构建组件, 见文件尾部)。
 * 有意不做的: 错误恢复语义完全对齐浏览器规范(足够解析良构文档)。
 */
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn_internal.h"
#include "hn_port.h"

static int is_ws(int c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f'; }

static void append_child(hn_node *parent, hn_node *child) {
    child->parent = parent;
    child->next = child->prev = NULL;
    if (parent->last) {
        parent->last->next = child;
        child->prev = parent->last;
        parent->last = child;
    } else {
        parent->first = parent->last = child;
    }
}

static hn_node *new_node(hn_arena *ar, hn_node_kind k) {
    hn_node *n = hn_arena_alloc(ar, sizeof(hn_node));
    memset(n, 0, sizeof(hn_node));
    n->kind = k;
    n->arena = ar;
    /* 标记为"新建": 若该元素声明了入场动画, 首次 tick 时播放
       (新插入的内容淡入上浮, 而不是硬闪出现)。 */
    n->anim.fresh = 1;
    return n;
}

/* ---------- DOM 构造(内部可见, hn_md.c 复用) ----------
 * Markdown 直染把 md 源替换成普通 DOM 子树 —— 造节点的口径必须与 HTML
 * 解析器完全一致(anim.fresh 入场标记 / arena 归属 / 双向链接线), 因此把
 * 解析器内部的构造函数提为内部可见, 而不是让 hn_md.c 自造一套。
 * tag 传入字面量小写标签(不复制); 文本复制进 arena。 */
hn_node *hn_dom_elem(hn_arena *ar, const char *tag) {
    hn_node *el = new_node(ar, HN_ELEM);
    el->tag = tag;
    return el;
}

hn_node *hn_dom_text(hn_arena *ar, const char *s, size_t n) {
    hn_node *t = new_node(ar, HN_TEXT);
    t->text = hn_arena_strndup(ar, s, n);
    t->text_len = n;
    return t;
}

void hn_dom_append(hn_node *parent, hn_node *child) {
    append_child(parent, child);
}

static char *lower_dup(hn_arena *ar, const char *s, size_t n) {
    char *p = hn_arena_alloc(ar, n + 1);
    for (size_t i = 0; i < n; i++) p[i] = (char)tolower((unsigned char)s[i]);
    p[n] = 0;
    return p;
}

static int is_void_tag(const char *t) {
    static const char *v[] = { "area","base","br","col","embed","hr","img","input",
                               "link","meta","param","source","track","wbr", NULL };
    for (int i = 0; v[i]; i++) if (!strcmp(v[i], t)) return 1;
    return 0;
}

/* 大小写不敏感查找子串 */
static const char *find_ci(const char *hay, size_t n, const char *needle) {
    size_t m = strlen(needle);
    if (n < m) return NULL;
    for (size_t i = 0; i + m <= n; i++)
        if (!strncasecmp(hay + i, needle, m)) return hay + i;
    return NULL;
}

/* 码点 → utf8(至多 4 字节), 返回字节数 */
static size_t cp_utf8(unsigned cp, char *out) {
    size_t o = 0;
    if (cp < 0x80) out[o++] = (char)cp;
    else if (cp < 0x800) {
        out[o++] = (char)(0xC0 | (cp >> 6));
        out[o++] = (char)(0x80 | (cp & 0x3F));
    } else if (cp < 0x10000) {
        out[o++] = (char)(0xE0 | (cp >> 12));
        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[o++] = (char)(0x80 | (cp & 0x3F));
    } else {
        out[o++] = (char)(0xF0 | (cp >> 18));
        out[o++] = (char)(0x80 | ((cp >> 12) & 0x3F));
        out[o++] = (char)(0x80 | ((cp >> 6) & 0x3F));
        out[o++] = (char)(0x80 | (cp & 0x3F));
    }
    return o;
}

/* 实体: 成功时写出(至多 4 字节 utf8), 返回消耗的源长度; 失败返回 0 */
static size_t entity_out(const char *s, size_t n, char *out, size_t *out_n) {
    struct { const char *name; const char *rep; } E[] = {
        { "&amp;",  "&" },      { "&lt;",  "<" },   { "&gt;",  ">" },
        { "&quot;", "\"" },     { "&apos;", "'" },  { "&#39;", "'" },
        { "&nbsp;", "\xC2\xA0" }, { "&copy;", "\xC2\xA9" }, { "&mdash;", "\xE2\x80\x94" },
    };
    for (size_t i = 0; i < sizeof(E) / sizeof(E[0]); i++) {
        size_t l = strlen(E[i].name);
        if (n >= l && !strncmp(s, E[i].name, l)) {
            size_t rl = strlen(E[i].rep);
            memcpy(out, E[i].rep, rl);
            *out_n = rl;
            return l;
        }
    }
    if (n >= 4 && s[1] == '#' && (s[2] == 'x' || s[2] == 'X')) { /* 十六进制实体 &#x2014; */
        size_t i = 3;
        long v = 0;
        while (i < n && isxdigit((unsigned char)s[i])) {
            char c = s[i];
            v = v * 16 + (c <= '9' ? c - '0' : (c | 32) - 'a' + 10);
            i++;
        }
        if (i > 3 && i < n && s[i] == ';' && v > 0 && v < 0x110000) {
            *out_n = cp_utf8((unsigned)v, out);
            return i + 1;
        }
    }
    if (n >= 3 && s[1] == '#') { /* 十进制数字实体 &#123; */
        size_t i = 2;
        long v = 0;
        while (i < n && s[i] >= '0' && s[i] <= '9') { v = v * 10 + (s[i] - '0'); i++; }
        if (i > 2 && i < n && s[i] == ';' && v > 0 && v < 0x110000) {
            *out_n = cp_utf8((unsigned)v, out);
            return i + 1;
        }
    }
    return 0;
}

/* 文本入树: 实体解码 + 空白折叠(nbsp 不折叠) */
/* 文本节点保存**原始空白**(只做实体解码, 不折叠)。
 *
 * 为什么不能在这里折叠: 空白折叠是 white-space 的计算值决定的, 而样式要在
 * 解析之后的级联阶段才知道。在 tokenizer 里就把 "a   b" 压成 "a b" 之后,
 * white-space:pre/pre-wrap 就再也拿不回原始空白了 —— 实测三种取值输出完全
 * 一致(都塌成单个空格), 代码块 / ASCII 表格 / 对齐文本全部错乱。
 * 浏览器也是这么分工: DOM 存原始文本, 折叠发生在布局期。
 * 折叠逻辑现由 flatten_inline 按 white-space 分别处理。 */
static void push_text(hn_arena *ar, hn_node *cur, const char *s, size_t n) {
    char *buf = hn_arena_alloc(ar, n + 8);
    size_t o = 0;
    for (size_t i = 0; i < n; ) {
        if (s[i] == '&') {
            char rep[4]; size_t rl = 0;
            size_t used = entity_out(s + i, n - i, rep, &rl);
            if (used) {
                for (size_t k = 0; k < rl; k++) buf[o++] = rep[k];
                i += used;
                continue;
            }
        }
        buf[o++] = s[i++];
    }
    /* 纯空白的文本节点直接丢弃(它在折叠与保留两种模式下都没有可见内容,
       且留着会让行首判断复杂化)。非纯空白则原样保留首尾空白。 */
    int only_ws = 1;
    for (size_t k = 0; k < o; k++) {
        unsigned char c = (unsigned char)buf[k];
        if (c != ' ' && c != '\t' && c != '\n' && c != '\r' && c != '\f') { only_ws = 0; break; }
    }
    if (!o || only_ws) return;
    hn_node *t = new_node(ar, HN_TEXT);
    t->text = buf;
    t->text_len = o;
    append_child(cur, t);
}

/* 原样文本(raw text 元素用: script / style / pre / textarea)。
 *
 * 为什么必须原样: 这些元素的内容是**字面量**。空白折叠会把换行变成空格,
 * 于是 JS 里的 `// 行注释` 会吞掉其后整段代码(SyntaxError), CSS 里的
 * 规则也会被拼接错乱 —— HTML 规范要求这些元素内容不折叠。
 * 只做实体解码(&amp; 等), 不做空白处理。 */
static void push_text_raw(hn_arena *ar, hn_node *cur, const char *s, size_t n) {
    if (!n) return;
    char *buf = hn_arena_alloc(ar, n + 8);
    size_t o = 0;
    for (size_t i = 0; i < n; ) {
        if (s[i] == '&') {
            char rep[4]; size_t rl = 0;
            size_t used = entity_out(s + i, n - i, rep, &rl);
            if (used) {
                for (size_t k = 0; k < rl; k++) buf[o++] = rep[k];
                i += used;
                continue;
            }
        }
        buf[o++] = s[i++];
    }
    if (!o) return;
    buf[o] = 0;
    hn_node *t = new_node(ar, HN_TEXT);
    t->text = buf;
    t->text_len = o;
    append_child(cur, t);
}

/* 解析后整理: 混排场景下的边界裁剪(前后是元素或端点时裁掉首尾空格) */
/* 首尾空白的裁剪移到了布局期(flatten_inline): 只有非 white-space:pre 的
   上下文才裁, 否则代码块开头的缩进会被吃掉。这里不再做事。 */

static void push_attr(hn_arena *ar, hn_node *el, const char *name, const char *value) {
    hn_attr *na = hn_arena_alloc(ar, sizeof(hn_attr) * (size_t)(el->n_attrs + 1));
    if (el->n_attrs) memcpy(na, el->attrs, sizeof(hn_attr) * (size_t)el->n_attrs);
    el->attrs = na;
    na[el->n_attrs].name = name;
    na[el->n_attrs].value = value;
    el->n_attrs++;
}

/* ---------- <include src> 装载期文本级拼接(零构建组件) ----------
 *
 * 设计: HTML 解析**之前**把被引文件的内容原样拼进源文本 —— 解析器/样式/
 * 布局对组件零特例, include 进来的一切(元素/<style>/嵌套 include)与手写
 * 在同一位置完全等价。文档承诺见 README「<include src=…> 零构建组件」。
 *
 * 为什么不用资产回调: 装载发生在 hn_parse_html, 而 context(连同
 * hn_context_set_assets 的回调)在 parse 之后才存在 —— 装载期没有资产
 * 后端可借。因此这里与 hncore 读 <link rel=stylesheet> 同口径: 直接
 * fopen, 相对路径相对**包含它的文件**所在目录解析(嵌套时基准随层级走
 * —— 这是组件库可移植的关键: sub/card.html 引同目录的 foot.html 与
 * 文档根无关)。
 *
 * 路径归一化是词法级的(消化 "//" "/./" "seg/.."), 不碰 OS(getcwd 是
 * POSIX 而 C99 不保证): 环检测只依赖"同一路径拼出同一串", 词法一致即够。
 *
 * 容错纪律: 任何失败 —— 缺文件/读失败/超深/环/超配额 —— 都替换成注释
 * 占位并继续, 绝不让装载失败(注释节点本就被解析器跳过, 等价于"空")。
 */

#define HN_INC_MAX_DEPTH 6     /* 嵌套上限(文档承诺: 嵌套上限 6, 自动检测循环) */
#define HN_INC_PATH_MAX  1024  /* 归一化路径缓冲 */
#define HN_INC_MAX_FILES 256   /* 单文档 include 总配额(防病态文档拖死装载) */

/* 正在解析的文档路径(装载通道)。宿主在 hn_parse_html 之前设置, **一次性
   消费**: parse 完成即清空, 避免同进程后续的内存文档(热更新/流式推送)
   悄悄继承上一个文档的基准目录。纯内存文档不设置时, 相对路径按 cwd 解析。 */
static char g_inc_doc[HN_INC_PATH_MAX];

void hn_html_set_doc_path(const char *path) {
    if (!path) { g_inc_doc[0] = 0; return; }
    size_t n = strlen(path);
    if (n >= sizeof(g_inc_doc)) n = sizeof(g_inc_doc) - 1;   /* 超长截断: 后续按缺文件容错 */
    memcpy(g_inc_doc, path, n);
    g_inc_doc[n] = 0;
}

/* 可增长字节缓冲(拼接目标)。oom 后静默丢弃后续写入 —— 装载绝不失败。 */
typedef struct { char *p; size_t n, cap; int oom; } inc_buf;

static void inc_put(inc_buf *b, const char *s, size_t n) {
    if (b->oom || !n) return;
    if (b->n + n + 1 > b->cap) {
        size_t nc = b->cap ? b->cap * 2 : 4096;
        while (nc < b->n + n + 1) nc *= 2;
        char *np = (char *)realloc(b->p, nc);
        if (!np) { b->oom = 1; return; }
        b->p = np;
        b->cap = nc;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}
static void inc_puts(inc_buf *b, const char *s) { inc_put(b, s, strlen(s)); }

/* 词法归一化: base 目录(含结尾'/', 空串 = cwd)拼上 src, 逐段消化
   "//" "/./" "seg/.."; ".." 越过根时停在根。绝对 src(以'/'开头)不拼基准。 */
static void inc_norm(const char *base, const char *src, size_t sn,
                     char *out, size_t cap) {
    char joined[HN_INC_PATH_MAX];
    size_t j = 0;
    if (!(sn && src[0] == '/')) {              /* 相对: 先拼上包含者目录 */
        size_t bl = strlen(base);
        if (bl >= sizeof(joined)) bl = sizeof(joined) - 1;
        memcpy(joined, base, bl);
        j = bl;
    }
    size_t take = sn < sizeof(joined) - j - 1 ? sn : sizeof(joined) - j - 1;
    memcpy(joined + j, src, take);
    j += take;
    joined[j] = 0;

    int absolute = (j > 0 && joined[0] == '/');
    size_t o = 0;
    const char *p = joined;
    while (*p) {
        while (*p == '/') p++;
        const char *st = p;
        while (*p && *p != '/') p++;
        size_t sl = (size_t)(p - st);
        if (!sl) continue;
        if (sl == 1 && st[0] == '.') continue;                     /* "." 丢弃 */
        if (sl == 2 && st[0] == '.' && st[1] == '.') {             /* ".." 回退一段 */
            while (o > 0 && out[o - 1] != '/') o--;
            if (o > 0) o--;                 /* 连同段前的 '/' 一起去掉; 到根为止 */
            continue;
        }
        if (o + sl + 2 > cap - 1) break;    /* 放不下: 截断(后续按缺文件容错) */
        if (o > 0 || absolute) out[o++] = '/';
        memcpy(out + o, st, sl);
        o += sl;
    }
    out[o] = 0;
}

/* 路径的目录部分(含结尾 '/'); 无 '/' 则空串(= cwd) */
static void inc_dir_of(const char *path, char *out, size_t cap) {
    const char *last = NULL;
    for (const char *p = path; *p; p++) if (*p == '/') last = p;
    size_t n = last ? (size_t)(last - path) + 1 : 0;
    if (n >= cap) n = cap - 1;
    memcpy(out, path, n);
    out[n] = 0;
}

/* 读整个文件(malloc, 调用方 free; 失败 NULL)。纯 stdio —— 引擎零平台依赖。 */
static char *inc_read_file(const char *path, size_t *out_len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    if (fseek(f, 0, SEEK_END) != 0) { fclose(f); return NULL; }
    long n = ftell(f);
    if (n < 0) { fclose(f); return NULL; }
    rewind(f);
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    if (out_len) *out_len = got;
    return buf;
}

/* 在开始标签内部找 src 属性值(引号包裹或裸词)。找到返回 1。
   与主解析器的属性口径一致: 名字后可有空白, '=' 两侧可有空白。 */
static int inc_attr_src(const char *a, const char *gt,
                        const char **vs, size_t *vn) {
    const char *p = a;
    while (p < gt) {
        while (p < gt && is_ws((unsigned char)*p)) p++;
        if (p >= gt || *p == '/' || *p == '>') break;
        const char *ns = p;
        size_t nl = 0;
        while (p + nl < gt && !is_ws((unsigned char)p[nl]) &&
               p[nl] != '=' && p[nl] != '/' && p[nl] != '>') nl++;
        p += nl;
        while (p < gt && is_ws((unsigned char)*p)) p++;
        if (p < gt && *p == '=') {
            p++;
            while (p < gt && is_ws((unsigned char)*p)) p++;
            const char *v = p;
            size_t vl = 0;
            if (p < gt && (*p == '"' || *p == '\'')) {
                char qc = *p++;
                v = p;
                while (p < gt && *p != qc) p++;
                vl = (size_t)(p - v);
                if (p < gt) p++;       /* 跳过闭合引号 */
            } else {
                while (p < gt && !is_ws((unsigned char)*p) && *p != '>') p++;
                vl = (size_t)(p - v);
            }
            if (nl == 3 && (ns[0] | 32) == 's' && (ns[1] | 32) == 'r' &&
                (ns[2] | 32) == 'c' && vl) {
                *vs = v;
                *vn = vl;
                return 1;
            }
        }
        /* 无值属性: 名字已在上面跳过 */
    }
    return 0;
}

/* 拼接上下文: 当前包含链(环检测的"访问集合" —— 只记**正在展开**的链,
   不记已完成的兄弟: 同一片段在文档里引两次是组件复用的正常用法,
   不是环; 链上再现才是 A→B→A 的真环)。chain[0] = 根文档。 */
typedef struct {
    char chain[HN_INC_MAX_DEPTH + 1][HN_INC_PATH_MAX];
    int  depth;    /* 当前文本所处的包含层级: 0 = 根文档 */
    int  budget;   /* 剩余 include 处理配额 */
} inc_ctx;

/* 递归拼接: 把 s[0..len) 拷进 out, 途中把 <include src=…> 替换为文件内容 */
static void inc_expand(inc_ctx *ix, const char *base,
                       const char *s, size_t len, inc_buf *out) {
    const char *p = s, *end = s + len;
    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        if (!lt) { inc_put(out, p, (size_t)(end - p)); return; }
        /* 先把 lt 之前的待处理文本冲出去(否则进入 include 分支时,
           标签前的正文会被静默丢弃 —— 曾因此吃掉 </h1>, 整页嵌套错乱) */
        if (lt > p) { inc_put(out, p, (size_t)(lt - p)); p = lt; }

        /* 注释原样保留: 注释里的 <include> 不展开(注释就是注释) */
        if ((size_t)(end - lt) >= 4 && !memcmp(lt, "<!--", 4)) {
            const char *cl = find_ci(lt + 4, (size_t)(end - lt - 4), "-->");
            const char *stop = cl ? cl + 3 : end;
            inc_put(out, p, (size_t)(stop - p));
            p = stop;
            continue;
        }

        /* <include(大小写不敏感), 标签名后必须跟空白/'>'/'/' 才算命中 */
        if ((size_t)(end - lt) >= 9 && !strncasecmp(lt, "<include", 8) &&
            (is_ws((unsigned char)lt[8]) || lt[8] == '>' || lt[8] == '/')) {
            const char *gt = memchr(lt, '>', (size_t)(end - lt));
            if (!gt) { inc_put(out, p, (size_t)(end - p)); return; }
            const char *vs = NULL;
            size_t vn = 0;
            if (inc_attr_src(lt + 8, gt, &vs, &vn)) {
                /* 占位统一口径: 注释 + 原样 src(排查"为什么没引上"全靠它) */
                if (ix->budget <= 0) {
                    inc_puts(out, "<!--include budget: ");
                    inc_put(out, vs, vn);
                    inc_puts(out, "-->");
                } else {
                    ix->budget--;
                    char norm[HN_INC_PATH_MAX];
                    inc_norm(base, vs, vn, norm, sizeof(norm));
                    int cyc = 0;
                    for (int i = 0; i <= ix->depth && !cyc; i++)
                        if (!strcmp(ix->chain[i], norm)) cyc = 1;
                    if (cyc) {
                        inc_puts(out, "<!--include cycle: ");
                        inc_put(out, vs, vn);
                        inc_puts(out, "-->");
                    } else if (ix->depth + 1 > HN_INC_MAX_DEPTH) {
                        inc_puts(out, "<!--include too deep: ");
                        inc_put(out, vs, vn);
                        inc_puts(out, "-->");
                    } else {
                        size_t flen = 0;
                        char *content = inc_read_file(norm, &flen);
                        if (!content) {
                            inc_puts(out, "<!--include missing: ");
                            inc_put(out, vs, vn);
                            inc_puts(out, "-->");
                        } else {
                            /* 新基准 = 被引文件所在目录(相对**包含它的文件**);
                               归一化路径入链, 回来时弹出(链式环检测) */
                            char dir[HN_INC_PATH_MAX];
                            inc_dir_of(norm, dir, sizeof(dir));
                            snprintf(ix->chain[ix->depth + 1],
                                     sizeof(ix->chain[0]), "%s", norm);
                            ix->depth++;
                            inc_expand(ix, dir, content, flen, out);
                            ix->depth--;
                            free(content);
                        }
                    }
                }
            }
            /* 无 src 属性: 整个标签原地丢弃(等价于一个空未知元素) */
            p = gt + 1;
            continue;
        }

        /* 其它 '<' 原样保留一个字符(</div>、<div …> 都走这里,
           由下一轮的文本冲刷带上标签其余部分) */
        inc_put(out, "<", 1);
        p = lt + 1;
    }
}

/* 装载入口: 有文档路径(或相对 cwd)时把 <include> 拼平。
   返回 malloc 的拼接结果(调用方 free); 无 include/分配失败返回 NULL
   (调用方回退解析原文 —— 装载路径零失败)。 */
static char *hn_html_splice_includes(const char *src, size_t len, size_t *out_len) {
    /* 一次性消费文档路径: 取出后立即清空, 不给下一次 parse 留旧基准 */
    char docpath[HN_INC_PATH_MAX];
    snprintf(docpath, sizeof(docpath), "%s", g_inc_doc);
    g_inc_doc[0] = 0;

    /* 快路径: 源文本里没有 "<include" 就不分配不做功 */
    if (!find_ci(src, len, "<include")) return NULL;

    inc_ctx ix;
    memset(&ix, 0, sizeof(ix));
    ix.budget = HN_INC_MAX_FILES;
    char base[HN_INC_PATH_MAX];
    inc_dir_of(docpath, base, sizeof(base));      /* 根文档目录; 无路径 = ""(cwd) */
    if (docpath[0]) {
        char rootn[HN_INC_PATH_MAX];
        inc_norm("", docpath, strlen(docpath), rootn, sizeof(rootn));
        snprintf(ix.chain[0], sizeof(ix.chain[0]), "%s", rootn);
    } else {
        ix.chain[0][0] = 0;                       /* 内存文档: 根不入链(无路径可比) */
    }

    inc_buf b;
    memset(&b, 0, sizeof(b));
    inc_expand(&ix, base, src, len, &b);
    if (!b.p || b.oom) { free(b.p); return NULL; }
    if (out_len) *out_len = b.n;
    return b.p;
}

void hn_parse_into(hn_doc *doc, hn_node *root, const char *src, size_t len) {
    hn_arena *ar = doc->arena;
    const char *p = src, *end = src + len;
    if (len >= 3 && !memcmp(src, "\xEF\xBB\xBF", 3)) p += 3;
    hn_node *cur = root;

    while (p < end) {
        const char *lt = memchr(p, '<', (size_t)(end - p));
        if (!lt) { push_text(ar, cur, p, (size_t)(end - p)); break; }
        if (lt > p) push_text(ar, cur, p, (size_t)(lt - p));

        if ((size_t)(end - lt) >= 4 && !memcmp(lt, "<!--", 4)) {
            const char *cl = find_ci(lt + 4, (size_t)(end - lt - 4), "-->");
            p = cl ? cl + 3 : end;
            continue;
        }
        if (lt[1] == '!' || lt[1] == '?') { /* doctype / 处理指令 */
            const char *g = memchr(lt, '>', (size_t)(end - lt));
            p = g ? g + 1 : end;
            continue;
        }
        if (lt[1] == '/') { /* 结束标签: 宽松向上匹配 */
            const char *g = memchr(lt, '>', (size_t)(end - lt));
            if (!g) break;
            const char *np = lt + 2;
            size_t nl = 0;
            while (np + nl < g && !is_ws((unsigned char)np[nl])) nl++;
            if (nl) {
                char *name = lower_dup(ar, np, nl);
                hn_node *a = cur;
                while (a && a != root && (!a->tag || strcmp(a->tag, name))) a = a->parent;
                if (a && a != root) cur = a->parent;
            }
            p = g + 1;
            continue;
        }
        if (isalpha((unsigned char)lt[1])) { /* 开始标签 */
            const char *np = lt + 1;
            size_t nl = 0;
            while (np + nl < end && (isalnum((unsigned char)np[nl]) || np[nl] == '-' || np[nl] == '_')) nl++;
            char *tag = lower_dup(ar, np, nl);
            hn_node *el = new_node(ar, HN_ELEM);
            el->tag = tag;
            append_child(cur, el);

            const char *a = np + nl;
            int self_close = 0;
            while (a < end) {
                while (a < end && is_ws((unsigned char)*a)) a++;
                if (a >= end) break;
                if (*a == '>') { a++; break; }
                if (*a == '/') { self_close = 1; a++; continue; }
                const char *an = a;
                size_t anl = 0;
                while (a < end && !is_ws((unsigned char)*a) && *a != '=' && *a != '>' && *a != '/') { a++; anl++; }
                if (!anl) { a++; continue; }
                char *aname = lower_dup(ar, an, anl);
                const char *val = NULL;
                const char *val_end = NULL;
                const char *q = a;
                while (q < end && is_ws((unsigned char)*q)) q++;
                if (q < end && *q == '=') {
                    q++;
                    while (q < end && is_ws((unsigned char)*q)) q++;
                    if (q < end && (*q == '"' || *q == '\'')) {
                        char qc = *q++;
                        val = q;
                        while (q < end && *q != qc) q++;
                        val_end = q;
                        a = (q < end) ? q + 1 : end;
                    } else {
                        val = q;
                        while (q < end && !is_ws((unsigned char)*q) && *q != '>') q++;
                        val_end = q;
                        a = q;
                    }
                    /* 属性值实体解码并复制进 arena(闭合引号不计入) */
                    if (val) {
                        char dec[512];
                        size_t o = 0;
                        const char *i = val;
                        size_t rem = (size_t)(val_end - val);
                        while (rem && o + 4 < sizeof(dec)) {
                            if (*i == '&') {
                                char rep[4]; size_t rl = 0;
                                size_t used = entity_out(i, rem, rep, &rl);
                                if (used) { memcpy(dec + o, rep, rl); o += rl; i += used; rem -= used; continue; }
                            }
                            dec[o++] = *i++; rem--;
                        }
                        push_attr(ar, el, aname, hn_arena_strndup(ar, dec, o));
                        continue;
                    }
                }
                push_attr(ar, el, aname, "");
            }

            int voidel = is_void_tag(tag);
            if (!voidel && !self_close && (!strcmp(tag, "style") || !strcmp(tag, "script"))) {
                const char *close = find_ci(a, (size_t)(end - a), strcmp(tag, "style") ? "</script" : "</style");
                const char *gt = close ? memchr(close, '>', (size_t)(end - close)) : NULL;
                /* 内容止于结束标签的**开头**(close), 不是它的 '>':
                   否则内容里会带上 "</script", 交给 CSS/JS 解析器就是语法错误。
                   (<style> 一直带着, CSS 解析器宽容才没暴露; JS 会直接报错。) */
                const char *stop = close ? close : (gt ? gt : end);
                if (!strcmp(tag, "style") && stop > a) {
                    hn_sheet *sh = hn_parse_css(a, (size_t)(stop - a));
                    if (sh) {
                        if (doc->n_inline == doc->cap_inline) {
                            int nc = doc->cap_inline ? doc->cap_inline * 2 : 4;
                            struct hn_sheet **ns = hn_arena_alloc(ar, sizeof(hn_sheet *) * (size_t)nc);
                            if (doc->inline_sheets) memcpy(ns, doc->inline_sheets, sizeof(hn_sheet *) * (size_t)doc->n_inline);
                            doc->inline_sheets = ns;
                            doc->cap_inline = nc;
                        }
                        doc->inline_sheets[doc->n_inline++] = sh;
                    }
                } else if (!strcmp(tag, "script") && stop > a) {
                    /* 脚本内容存为文本子节点: 运行时(JavaScriptCore)据此执行。
                       引擎自身不解释 JS —— 它只负责把这段文本留在 DOM 里,
                       保持"引擎平台无关、不依赖脚本引擎"的边界。
                       必须用 raw 版本: 空白折叠会让 `//` 注释吞掉后续代码。 */
                    push_text_raw(ar, el, a, (size_t)(stop - a));
                }
                p = gt ? gt + 1 : end;
                continue;
            }
            if (!voidel && !self_close) cur = el;
            p = a;
            continue;
        }
        /* 孤立 '<' 视为文本 */
        push_text(ar, cur, "<", 1);
        p = lt + 1;
    }
}

hn_doc *hn_parse_html(const char *src, size_t len) {
    hn_doc *doc = calloc(1, sizeof(hn_doc));
    if (!doc) abort();
    doc->arena = hn_arena_create();
    doc->root = new_node(doc->arena, HN_ELEM);
    doc->root->tag = "html";
    if (src && len) {
        /* <include> 装载期拼接在解析**前**完成 —— 解析器对组件零感知。
           拼接失败/无 include 时回退原文, 装载路径零失败。 */
        size_t elen = 0;
        char *expanded = hn_html_splice_includes(src, len, &elen);
        hn_parse_into(doc, doc->root, expanded ? expanded : src,
                      expanded ? elen : len);
        free(expanded);
    }
    return doc;
}

void hn_doc_free(hn_doc *doc) {
    if (!doc) return;
    for (int i = 0; i < doc->n_inline; i++) hn_sheet_free(doc->inline_sheets[i]);
    free(doc->pending_link);   /* 待消费链接是 malloc 副本(消费即 free) */
    hn_arena_destroy(doc->arena);
    free(doc);
}

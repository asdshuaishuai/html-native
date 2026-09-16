/* hn_theme.c — 设计令牌基底(C99, 平台无关)
 *
 * 为什么放在引擎里: 主题是"渲染语义"的一部分, 不是平台能力。
 * 放在 C 里, 三条渲染路径(C 引擎 / macOS 运行时 / WebKit 兜底)共用同一份
 * 令牌定义 —— 单一事实来源, 不会出现"某个平台主题不一样"。
 *
 * 用法: 页面声明 <meta name="hn-theme" content="dark">,
 * 引擎在装载文档时把对应基座插在 UA 之后、作者样式之前,
 * 因此作者 CSS 同特异性即可覆盖任意令牌。
 */
#include <stdlib.h>
#include <string.h>
#include "hn_internal.h"

static const char *DARK_CSS =
    ":root, body { --accent: #4f7cff; --accent-2: #7a9bff; --bg: #0f1218; --card: #171c26;\n"
    "  --line: #262e3d; --ink: #e8eaf0; --dim: #8a93a8; }\n"
    "html, body { margin: 0; width: 100%; height: 100%; background: var(--bg); color: var(--ink);\n"
    "  font-size: 13; line-height: 1.55; }\n"
    "h1 { font-size: 20; font-weight: 700; margin: 14 0 8; }\n"
    "h2 { font-size: 16; font-weight: 700; margin: 12 0 6; }\n"
    "h3, h4, h5, h6 { font-size: 14; font-weight: 700; margin: 10 0 5; }\n"
    "p { margin: 0 0 10; color: #b6bfd0; line-height: 1.6; }\n"
    "ul, ol { margin: 6 0; padding: 4 0 4 24; }\n"
    "li { margin: 3 0; color: #b6bfd0; }\n"
    "strong { color: var(--ink); }\n"
    "code { font-size: 12; border-radius: 4; padding: 1 5; background: #4f7cff1f; color: #9ec1ff; }\n"
    "blockquote { border-left: 3 solid var(--accent); padding: 8 12; margin: 10 0;\n"
    "  color: var(--dim); background: #4f7cff0f; }\n"
    "hr { border-color: var(--line); }\n"
    "a { color: var(--accent); }\n"
    "input, textarea { background: #10151f; }\n"
    ".sysbar { background: #232733; }\n"
    ".sk { background: #ffffff12; }\n"
    ".card { box-shadow: 0 2 10 rgba(0, 0, 0, 0.25); }\n"
    ".card:hover { border-color: #3a4560; }\n";

static const char *LIGHT_CSS =
    ":root, body { --accent: #2f6bff; --accent-2: #5b8bff; --bg: #f5f6f8; --card: #ffffff;\n"
    "  --line: #e2e5eb; --ink: #1c2027; --dim: #6b7386; }\n"
    "html, body { margin: 0; width: 100%; height: 100%; background: var(--bg); color: var(--ink);\n"
    "  font-size: 13; line-height: 1.55; }\n"
    "h1 { font-size: 20; font-weight: 700; margin: 14 0 8; }\n"
    "h2 { font-size: 16; font-weight: 700; margin: 12 0 6; }\n"
    "h3, h4, h5, h6 { font-size: 14; font-weight: 700; margin: 10 0 5; }\n"
    "p { margin: 0 0 10; color: #4a5262; line-height: 1.6; }\n"
    "ul, ol { margin: 6 0; padding: 4 0 4 24; }\n"
    "li { margin: 3 0; color: #4a5262; }\n"
    "strong { color: var(--ink); }\n"
    "code { font-size: 12; border-radius: 4; padding: 1 5; background: #2f6bff14; color: #1d4fd7; }\n"
    "blockquote { border-left: 3 solid var(--accent); padding: 8 12; margin: 10 0;\n"
    "  color: var(--dim); background: #2f6bff08; }\n"
    "hr { border-color: var(--line); }\n"
    "a { color: var(--accent); }\n"
    "input, textarea { background: #ffffff; }\n"
    ".sysbar { background: #e8eaef; }\n"
    ".sk { background: #00000010; }\n"
    ".card { box-shadow: 0 1 6 rgba(16, 24, 40, 0.08); }\n"
    ".card:hover { border-color: #c9cfd9; }\n";

/* 组件基类: 与主题色无关, 两个主题共用 */
static const char *BASE_CSS =
    ".card { background: var(--card); border: 1 solid var(--line); border-radius: 12;\n"
    "  padding: 14; margin: 0 0 10; transition: 180ms; }\n"
    ".btn { background: var(--accent); color: #ffffff; padding: 8 14; border-radius: 8;\n"
    "  font-weight: 600; font-size: 12; text-align: center; transition: 160ms; cursor: pointer; }\n"
    ".btn:hover { background: var(--accent-2); }\n"
    ".btn:active { opacity: 0.85; }\n"
    "input, textarea { border: 1 solid var(--line); border-radius: 8; padding: 7 10;\n"
    "  color: var(--ink); font-size: 13; }\n"
    "input:focus, textarea:focus { border-color: var(--accent); }\n"
    ".cap { color: var(--dim); font-size: 11; font-weight: 700; letter-spacing: 0.5; margin: 0 0 7; }\n"
    ".sub { color: var(--dim); font-size: 11; }\n"
    ".row { display: flex; gap: 8; }\n"
    ".grid { display: flex; gap: 8; }\n"
    ".sk { height: 9; border-radius: 4; margin: 4 0; }\n"
    ".syshead { color: var(--ink); font-size: 12; font-weight: 700; margin: 0 0 4; }\n"
    ".sysrow { display: flex; padding: 3 0; }\n"
    ".sysk { color: var(--dim); font-size: 10; width: 46; }\n"
    ".sysv { color: var(--ink); font-size: 12; font-weight: 600; }\n"
    ".sysbar { height: 4; border-radius: 2; margin: 6 0 3; }\n"
    ".sysfill { height: 100%; border-radius: 2;\n"
    "  background: linear-gradient(90deg, var(--accent), var(--accent-2)); transition: 400ms; }\n";

const char *hn_theme_css(const char *name) {
    if (!name) return NULL;
    if (!strcmp(name, "dark")) return DARK_CSS;
    if (!strcmp(name, "light")) return LIGHT_CSS;
    return NULL;
}

const char *hn_theme_base_css(void) { return BASE_CSS; }

/* 解析文档声明的主题名; 无声明返回 NULL(指针指向文档 arena, 生命周期同文档) */
const char *hn_doc_theme(hn_doc *doc) {
    if (!doc || !doc->root) return NULL;
    hn_node *stack[256];
    int sp = 0;
    stack[sp++] = doc->root;
    while (sp) {
        hn_node *n = stack[--sp];
        if (n->kind == HN_ELEM && n->tag && !strcmp(n->tag, "meta")) {
            const char *nm = hn_node_attr(n, "name");
            const char *ct = hn_node_attr(n, "content");
            if (nm && ct && !strcmp(nm, "hn-theme")) return ct;
        }
        for (hn_node *ch = n->last; ch; ch = ch->prev)
            if (sp < 256) stack[sp++] = ch;
    }
    return NULL;
}

void hn_context_apply_theme(hn_context *c) {
    if (!c) return;
    /* 已有主题表(热更新前)先摘除: 索引 1 为主题槽(0 永远是 UA) */
    if (c->has_theme && c->n_sheets > 1) {
        hn_sheet_free(c->sheets[1]);
        for (int i = 1; i + 1 < c->n_sheets; i++) c->sheets[i] = c->sheets[i + 1];
        c->n_sheets--;
        c->has_theme = 0;
    }
    const char *theme = hn_doc_theme(c->doc);
    const char *css = hn_theme_css(theme);
    if (!css) return;

    /* 主题基座 = 令牌/排版 + 组件基类, 合成一张表插入 UA 之后 */
    size_t n1 = strlen(css), n2 = strlen(BASE_CSS);
    char *buf = malloc(n1 + n2 + 2);
    if (!buf) return;
    memcpy(buf, css, n1);
    buf[n1] = '\n';
    memcpy(buf + n1 + 1, BASE_CSS, n2);
    hn_sheet *sh = hn_parse_css(buf, n1 + n2 + 1);
    free(buf);

    /* 插到索引 1(UA 之后, 作者样式之前) */
    if (c->n_sheets + 1 > c->cap_sheets) {
        int nc = c->cap_sheets ? c->cap_sheets * 2 : 8;
        hn_sheet **nv = realloc(c->sheets, sizeof(hn_sheet *) * (size_t)nc);
        if (nv) { c->sheets = nv; c->cap_sheets = nc; }
    }
    for (int i = c->n_sheets; i > 1; i--) c->sheets[i] = c->sheets[i - 1];
    c->sheets[1] = sh;
    c->n_sheets++;
    c->has_theme = 1;
}

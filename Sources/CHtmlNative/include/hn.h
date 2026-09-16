/* hn.h — html-native engine, public C API
 *
 * html-native: 与 RN/Flutter 同级的原生 UI 框架, 形态是系统级
 * PWA/小程序引擎。引擎只做一件事: 把 hn 编码(HTML 语法)渲染成
 * 系统级 UI —— 图层/窗口/弹窗/应用。上层的 UI/UX 实现(组件库/
 * 设计规范/交互模式)不属于引擎。
 *
 * 分层:
 *   渲染核心(C99, 本库) — 解析 → 级联 → 布局 → 绘制指令列表
 *   平台运行时(每系统一个) — 把绘制指令画到原生表面, 物化窗口/弹窗
 *   宿主(可选) — 常驻进程, 供 agent/工具创建/热更新/销毁/持久化应用
 *
 * 引擎不含网络: htmx 风格的 hx-* 属性由运行时解释(transport 可注入)。
 */
#ifndef HN_H
#define HN_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hn_context hn_context;
typedef struct hn_doc     hn_doc;
typedef struct hn_sheet   hn_sheet;
typedef struct hn_node    hn_node;

/* 颜色打包: 0xRRGGBBAA (alpha 0 = 透明/无) */
typedef uint32_t hn_color;

typedef struct hn_font_desc {
    float size_px;
    int   weight;         /* 100..900, 400=normal 700=bold */
    int   italic;
    float letter_spacing; /* 字间额外像素(kern) */
    int   family;         /* 0=system 1=monospace 2=serif */
} hn_font_desc;

/* 运行时注入的文本后端: 布局时测量文本宽度与字体竖向度量 */
typedef struct hn_text_backend {
    void *ctx;
    /* 返回 utf8[0..byte_len) 在该字体下的排版宽度(px) */
    float (*measure)(void *ctx, const hn_font_desc *font,
                     const char *utf8, size_t byte_len);
    /* 返回字号竖向度量(ascent/descent/leading, px) */
    void (*metrics)(void *ctx, const hn_font_desc *font,
                    float *ascent, float *descent, float *leading);
} hn_text_backend;

/* ---- 绘制指令(平台运行时直接翻译成 CoreGraphics/D2D/cairo) ---- */

typedef enum hn_cmd_kind {
    HN_CMD_RECT = 1,      /* 圆角矩形: 填充/渐变 + 描边 + 阴影 */
    HN_CMD_TEXT = 2,      /* 一行文本, (x, baseline) 定位 */
    HN_CMD_IMAGE = 3,     /* 图片(路径), 覆盖 rect */
    HN_CMD_CLIP_PUSH = 4, /* 圆角矩形裁剪入栈 */
    HN_CMD_CLIP_POP = 5
} hn_cmd_kind;

typedef struct hn_cmd {
    hn_cmd_kind kind;
    /* RECT */
    float    x, y, w, h;
    float    radius;
    hn_color fill;     /* alpha==0 且无渐变则跳过填充 */
    hn_color stroke;   /* alpha==0 则跳过描边 */
    float    stroke_w;
    /* RECT 渐变(background: linear-gradient(...)) */
    unsigned char gradient;
    hn_color grad_from, grad_to;
    float    grad_angle;   /* CSS 角度: 0deg=向上, 90deg=向右 */
    /* RECT 阴影(box-shadow: ox oy blur color) */
    unsigned char shadow;
    hn_color shadow_color;
    float    shadow_blur, shadow_ox, shadow_oy;
    /* TEXT: fill 复用为文字颜色 */
    const char *text;      /* IMAGE/CLIP_PUSH 之外的文本; IMAGE 时为图片路径 */
    size_t      text_len;
    float       tx, baseline;
    hn_font_desc font;
} hn_cmd;

typedef struct hn_display_list {
    hn_cmd *cmds;
    int     count;
} hn_display_list;

/* 图片后端: 布局时查询图片固有尺寸(路径由运行时解析/加载) */
typedef struct hn_image_backend {
    void *ctx;
    int (*size)(void *ctx, const char *path, float *w, float *h); /* 成功返回 1 */
} hn_image_backend;

/* ---- 应用清单: hn 编码中的表面声明, 供宿主物化 ----
 *
 * 在 hn 文档里用 meta 声明(引擎只解析, 不物化):
 *   <meta name="hn-surface" content="window|popup|layer">
 *   <meta name="hn-window"  content="360x520">          或 "360x520@100,200"
 *   <meta name="hn-title"   content="标题">
 * title 指向文档 arena 内存, 文档存活期间有效。
 */

typedef enum hn_surface_kind {
    HN_SURFACE_WINDOW = 0, /* 有标题栏的应用窗口 */
    HN_SURFACE_POPUP,      /* 无边框浮动弹窗(HUD/通知/卡片) */
    HN_SURFACE_LAYER       /* 覆盖图层(依附宿主窗口或浮层) */
} hn_surface_kind;

typedef struct hn_manifest {
    hn_surface_kind surface;
    float w, h;            /* 0 = 自动 */
    float x, y;            /* 左上角; 0,0 = 自动(居中) */
    const char *title;     /* 可为 NULL */
    const char *theme;     /* hn-theme: "dark" / "light", 可为 NULL */
} hn_manifest;

/* 从文档解析清单(meta 缺省时给默认值: window / 自动) */
void hn_doc_manifest(const hn_doc *doc, hn_manifest *out);

/* ---- 主题(设计令牌基底, 平台无关) ---- */
/* 取内置主题 CSS(按 hn-theme 名); 未知名返回 NULL */
const char *hn_theme_css(const char *name);
/* 与主题色无关的组件基类(card/btn/input/sys 片段皮肤) */
const char *hn_theme_base_css(void);
/* 解析文档声明的 hn-theme 名; 无声明返回 NULL */
const char *hn_doc_theme(hn_doc *doc);
/* 按文档声明装载/替换主题样式表(UA 之后, 作者样式之前) */
void hn_context_apply_theme(hn_context *c);

/* ---- 解析 ---- */
hn_doc   *hn_parse_html(const char *src, size_t len);
hn_sheet *hn_parse_css(const char *src, size_t len);
void      hn_doc_free(hn_doc *doc);
void      hn_sheet_free(hn_sheet *sheet);

/* ---- 上下文 ---- */
hn_context *hn_context_create(void);
void        hn_context_destroy(hn_context *c);
void        hn_context_set_doc(hn_context *c, hn_doc *doc);
void        hn_context_add_sheet(hn_context *c, hn_sheet *sheet);
hn_doc     *hn_context_doc(hn_context *c);

/* 布局 + 生成绘制指令; 可随窗口尺寸变化反复调用 */
void hn_context_layout(hn_context *c, float width, float height,
                       const hn_text_backend *backend);
/* 仅重跑绘制(滚动等不改变布局的更新) */
void hn_context_repaint(hn_context *c);
const hn_display_list *hn_context_display_list(const hn_context *c);

/* 图片后端(尺寸查询); 运行时持有 backend 内存 */
void hn_context_set_images(hn_context *c, const hn_image_backend *backend);

/* ---- 过渡动画 ---- */
/* 推进过渡并写回样式; dt_ms < 0 = 仅初始化(首帧, 不产生动画)。
   返回 1 表示仍有动画在跑 —— 运行时据此继续帧循环。 */
int hn_context_anim_tick(hn_context *c, float dt_ms);

/* ---- 交互: 状态(伪类) ---- */
/* 设置/清除当前悬浮与按下元素(触发 :hover/:active 重新匹配), 随后需重布局 */
void hn_context_set_hover(hn_context *c, hn_node *n);
void hn_context_set_active(hn_context *c, hn_node *n);
/* 焦点元素(:focus 伪类)与插入符可见性 */
void hn_context_set_focus(hn_context *c, hn_node *n);
void hn_context_set_caret_visible(hn_context *c, int on);
/* 元素计算样式的 cursor(0=default 1=pointer) */
int  hn_node_cursor(hn_node *n);

/* ---- 输入控件 ---- */
/* 控件当前值(可编辑文本); 无 value 状态时回退到 value 属性, 再无则空串 */
const char *hn_node_value(hn_node *n, size_t *len_out);
/* 设置值并移动 caret 到末尾; 返回 1 表示该节点是输入控件 */
int  hn_node_set_value(hn_node *n, const char *utf8, size_t len);
/* caret(字节偏移)读写 */
int  hn_node_caret(hn_node *n);
void hn_node_set_caret(hn_node *n, int byte_off);
/* 是否为可编辑输入控件(input / textarea) */
int  hn_node_is_input(hn_node *n);
/* 表单参数: 收集文档内所有控件的 name=value, 写成 URL 编码串; 返回写入长度 */
size_t hn_doc_form_encode(hn_doc *doc, char *out, size_t cap);

/* ---- 节点遍历(宿主事件/工具用) ---- */
hn_node *hn_node_first_child(hn_node *n);
hn_node *hn_node_next_sibling(hn_node *n);
/* 文档内第 idx 个输入控件(按文档序); idx 越界返回 NULL */
hn_node *hn_doc_input_at(hn_doc *doc, int idx);
/* 文档内是否存在带指定属性的元素(如 hn-drag / hn-dismiss) */
int hn_doc_has_attr(hn_doc *doc, const char *name);

/* ---- 排版结果查询(宿主/测试用) ---- */
/* 节点布局盒(绝对坐标, border-box); 未布局时全 0 */
void hn_node_box(hn_node *n, float *x, float *y, float *w, float *h);
/* 节点计算样式的 display(0=block 1=flex 2=none 3=inline 4=inline-block) */
int  hn_node_display(hn_node *n);
/* 计算样式读取(测试/宿主读取动画当前值用) */
void hn_node_debug_background(hn_node *n, hn_color *out);
void hn_node_debug_border(hn_node *n, hn_color *out);
void hn_node_debug_color(hn_node *n, hn_color *out);
/* 节点自身的行内片段数; 文本节点为其被摆放的段数 */
int hn_node_run_count(hn_node *n);
/* 取第 i 个片段: 写出绝对 x/基线/宽 与所属行盒顶/高; 越界返回 0 */
int hn_node_run_at(hn_node *n, int i, float *x, float *baseline, float *w,
                   float *y_top, float *h);

/* ---- 交互: 滚动 ---- */
/* 点位下最深的 overflow 容器(可滚动元素), 无则 NULL */
hn_node *hn_context_scrollable_at(hn_context *c, float x, float y);
/* 滚动该元素(自动钳制到内容范围); 位置变化返回 1(调用方 repaint) */
int  hn_node_scroll_by(hn_node *n, float dx, float dy);
/* 读取当前滚动偏移 */
void hn_node_scroll_get(hn_node *n, float *x, float *y);
/* 可滚动上限(content 高 - 盒高); 不可滚动写 0 */
void hn_node_scroll_range(hn_node *n, float *max_x, float *max_y);
/* 枚举带指定属性的元素: idx 从 0 起, 命中返回节点 */
hn_node *hn_doc_find_attr(hn_doc *doc, const char *attr, int idx);
/* 只保留最后 keep_last 个元素子节点(流式视图的环形缓冲); 返回移除数量 */
int  hn_node_trim_children(hn_node *n, int keep_last);
/* 结构自检: 检测链表环、自引用、父子指针不一致。返回问题数量(0=健康)。
   node_budget 为本次遍历允许访问的最大节点数(防环导致的死循环)。 */
int  hn_doc_validate(hn_doc *doc, int node_budget);

/* ---- 交互: 命中测试 ---- */
const char *hn_context_hit_test(hn_context *c, float x, float y); /* 元素 id 或 NULL */
hn_node    *hn_context_hit_node(hn_context *c, float x, float y); /* 最深层元素节点 */

/* ---- 交互: 属性读取(htmx 风格) ---- */
/* 元素标签名(小写), 文本节点或无标签返回 NULL */
const char *hn_node_tag(hn_node *n);
/* 返回元素属性值, 不存在返回 NULL */
const char *hn_node_attr(hn_node *n, const char *name);
/* 从 n(含自身)向上找最近一个拥有指定属性的元素 */
hn_node    *hn_node_ancestor_with_attr(hn_node *n, const char *name);
/* 表单回车语义: 自身 → 各级祖先 → 最近一级祖先的子树(文档序, 跳过 n 所在分支)
 * 中第一个携带 hx-post(优先)或 hx-get 的元素; 用于 input 上按回车触发提交 */
hn_node    *hn_node_form_carrier(hn_node *n);
hn_node    *hn_doc_find_by_id(hn_doc *doc, const char *id);
/* 文档的 <body> 元素(无则 NULL) */
hn_node    *hn_doc_body(hn_doc *doc);
/* 文档根节点(内省/遍历入口) */
hn_node    *hn_doc_root(hn_doc *doc);
/* 找到第一个 hx-trigger 含 "load" 的元素 id(页面启动自动请求用) */
const char *hn_doc_find_trigger_on_load(hn_doc *doc);
/* 枚举所有 hx-trigger 含 "load" 的元素(全量启动请求): idx 从 0 起, 命中写出 id */
int hn_doc_load_at(hn_doc *doc, int idx, const char **out_id);
/* 为携带 hx-get/hx-post 但缺少 id 的元素自动分配 "hx-auto-N"。
 * 返回分配数量。load/poll/click 的目标定位都依赖 id, 这让无 id 文档也可靠工作。 */
int hn_doc_autoid_hx(hn_doc *doc);
/* 枚举带 hx-trigger="every <n>[ms|s]" 的元素:
 * idx 从 0 起, 命中时写出元素 id 与间隔毫秒; 无更多返回 0 */
int hn_doc_poll_at(hn_doc *doc, int idx, const char **out_id, int *out_ms);

/* ---- 交互: 文本更新 / 片段交换(htmx swap) ---- */

typedef enum {
    HN_SWAP_INNER = 0,  /* 替换目标元素内容(默认, innerHTML) */
    HN_SWAP_OUTER,      /* 替换目标元素自身(outerHTML) */
    HN_SWAP_APPEND,     /* 追加到目标末尾 */
    HN_SWAP_PREPEND     /* 插到目标开头 */
} hn_swap_mode;

/* 更新元素文本(替换其子节点为单个文本节点), 返回 1 表示找到目标 */
int hn_doc_set_text(hn_doc *doc, const char *element_id, const char *utf8_text);

/* 把 HTML 片段换入目标元素; 运行时随后调用 hn_context_layout 生效 */
int hn_doc_swap(hn_doc *doc, const char *target_id, hn_swap_mode mode,
                const char *html_src, size_t len);

/* 热渲染: 用新 HTML 整体替换文档内容(样式表保留), 随后 layout 生效。
 * 这是 agent 驱动 UI 的核心原语: 现场生成 HTML → 推入常驻窗口。 */
void hn_context_render(hn_context *c, const char *html_src, size_t len);

#ifdef __cplusplus
}
#endif
#endif /* HN_H */

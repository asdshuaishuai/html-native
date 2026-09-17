/* hn_internal.h — 内部共享结构 */
#ifndef HN_INTERNAL_H
#define HN_INTERNAL_H
#include "hn.h"

/* ---------- arena ---------- */
typedef struct hn_arena hn_arena;
hn_arena *hn_arena_create(void);
void      hn_arena_destroy(hn_arena *a);
void     *hn_arena_alloc(hn_arena *a, size_t n);
char     *hn_arena_strndup(hn_arena *a, const char *s, size_t n);

/* ---------- DOM ---------- */
typedef enum { HN_ELEM, HN_TEXT } hn_node_kind;

typedef struct hn_attr { const char *name, *value; } hn_attr;

typedef enum { HN_U_AUTO = 0, HN_U_PX, HN_U_PCT, HN_U_EM } hn_size_unit;
typedef enum { HN_JUST_START = 0, HN_JUST_CENTER, HN_JUST_END, HN_JUST_BETWEEN } hn_justify;
typedef enum { HN_ALIGN_STRETCH = 0, HN_ALIGN_START, HN_ALIGN_CENTER, HN_ALIGN_END } hn_align;
typedef enum { HN_DISP_BLOCK = 0, HN_DISP_FLEX, HN_DISP_NONE, HN_DISP_INLINE, HN_DISP_INLINE_BLOCK } hn_display;

/* 定位方式: static 参与常规流; relative 在流中但作为定位参照;
   absolute 脱离流(参照最近 positioned 祖先); fixed 脱离流(参照视口) */
typedef enum { HN_POS_STATIC = 0, HN_POS_RELATIVE, HN_POS_ABSOLUTE, HN_POS_FIXED } hn_position;

/* CSS 自定义属性(--name / var(--name)): 级联时收集, 继承自父, 子可覆盖 */
typedef struct { const char *name, *value; } hn_var;

typedef struct hn_style {
    hn_display display;
    int        flex_row;      /* 1=row 0=column */
    hn_justify justify;
    hn_align   align;
    float      flex_grow;
    float      flex_shrink;   /* 默认 1: 超出时按基数比例收缩 */
    float      flex_basis;    unsigned char flex_basis_u; /* flex:<n> ⇒ basis 0 */
    float      gap;
    float      width;    unsigned char width_u;
    float      height;   unsigned char height_u;
    float      margin[4];    /* top right bottom left */
    float      padding[4];
    float      border_w;
    hn_color   border_color;
    float      radius;
    hn_color   background;
    hn_color   color;
    float      font_size;
    int        font_weight, font_italic;
    float      letter_spacing;
    int        text_align;   /* 0=left 1=center 2=right */
    float      line_height;  /* 倍数 */
    int        box_border;   /* box-sizing: border-box */
    float      opacity;
    /* 渐变背景(background: linear-gradient(a, c1, c2)) */
    unsigned char has_gradient;
    hn_color   grad_from, grad_to;
    float      grad_angle;
    /* 盒阴影(box-shadow: ox oy blur color) */
    unsigned char has_shadow;
    hn_color   sh_color;
    float      sh_ox, sh_oy, sh_blur;
    float      transition_ms; /* 过渡时长(0 = 关闭) */
    /* ---- 2D/3D 变换(transform) ---- */
    float      rotate;        /* rotate()/rotateZ(): 绕 Z 轴, 度 */
    float      rotate_x;      /* rotateX(): 绕 X 轴, 度 */
    float      rotate_y;      /* rotateY(): 绕 Y 轴, 度 */
    float      perspective;   /* 父级 perspective(n): 视距 px; >0 启用透视 */
    float      skew_x, skew_y;/* skew(): 斜切, 度 */
    /* ---- 定位(脱离流的浮层: 弹窗/下拉/遮罩/固定头) ---- */
    unsigned char position;    /* HN_POS_* */
    unsigned char has_top, has_right, has_bottom, has_left;
    float top, right, bottom, left;
    int   z_index;             /* 层叠顺序(默认 0; 仅 positioned 元素有效) */
    /* ---- 四边独立边框 ---- */
    float border_w4[4];        /* top right bottom left; 0 = 用 border_w */
    hn_color border_c4[4];     /* 同上, 0 = 用 border_color */
    /* ---- 文本溢出与折行 ---- */
    unsigned char text_overflow;  /* 1 = ellipsis */
    unsigned char white_space;    /* 0=normal 1=nowrap 2=pre 3=pre-wrap */
    /* ---- 几何与动画声明(可插值) ---- */
    float      translate_x, translate_y;  /* translate: x y */
    float      scale;                     /* scale: n (默认 1) */
    unsigned char anim_ease;              /* 过渡缓动(HN_EASE_*) */
    float      cb[4];                     /* cubic-bezier 参数 */
    unsigned char anim_enter;             /* 入场动画预设(HN_ENTER_*), 0=无 */
    float      enter_ms;                  /* 入场动画时长 */
    /* @keyframes 动画: name 指向样式表内的定义(生命周期同表) */
    const char *kf_name;
    float      kf_ms;                     /* 单次时长 */
    int        kf_iter;                   /* 循环次数; -1 = 无限 */
    int        kf_dir;                    /* 0=normal 1=reverse 2=alternate 3=alternate-reverse */
    unsigned char kf_fill;                /* 1 = forwards(结束后保持末帧) */
    int        overflow;     /* 0=visible 1=hidden/scroll(裁剪+可滚) */
    /* 自定义属性表(每次 layout 在 tmp arena 重建; 继承 = 复制父表后叠加) */
    hn_var    *vars; int n_vars;
    int        cursor;       /* 0=default 1=pointer */
    /* ---- 生态精修 ---- */
    unsigned char radius_pct;  /* border-radius 为 % 值(绘制时按盒短边解析, 50% 即圆) */
    unsigned char text_deco;   /* bit1 underline bit2 line-through bit4 overline */
    unsigned char disp_inline; /* display:inline-flex — 外层作原子行内盒, 内部仍 flex */
    int        list_style;     /* li 标记: 0=auto(disc/decimal) 1=none 2=square 3=circle */
    /* ---- 排版精细化 ---- */
    int        font_family;    /* 0=system 1=monospace 2=serif (继承) */
    float      min_w, max_w;   unsigned char min_w_u, max_w_u;  /* -1/AUTO = 未设 */
    float      min_h, max_h;   unsigned char min_h_u, max_h_u;
    unsigned char margin_auto; /* bit1 top bit2 right bit4 bottom bit8 left */
} hn_style;

/* 行内片段: 某节点文本的一段在行盒中的位置(绝对坐标)。
 * 一个文本节点可拥有多个 run(跨行/被块级兄弟打断)。 */
typedef struct hn_run {
    size_t begin, end;      /* 在该节点文本中的字节区间 */
    float  x, width;        /* 绝对 x / 宽度 */
    float  baseline;        /* 绝对基线 */
    float  y_top, h;        /* 所属行盒的顶部与高度(命中测试用) */
} hn_run;

/* 过渡动画状态: 引擎持有"当前值", 运行时按帧 tick 推进 */
/* 一条 CSS 声明(名值对)。定义在此处: @keyframes 与样式规则都要用。 */
typedef struct { const char *name, *value; } hn_decl;

/* @keyframes 定义: 名字 + 若干关键帧(百分比位置 + 声明组) */
typedef struct {
    float      at;            /* 0..1 */
    hn_decl   *decls;
    int        n_decls;
} hn_kf_stop;

typedef struct {
    const char *name;
    hn_kf_stop  stops[16];
    int         n_stops;
} hn_keyframes;

/* 缓动函数(与 CSS 命名对齐) */
enum {
    HN_EASE_SMOOTH = 0,   /* 默认: smoothstep(C¹ 连续, 起收都柔和) */
    HN_EASE_LINEAR = 1,
    HN_EASE_IN     = 2,
    HN_EASE_OUT    = 3,
    HN_EASE_IN_OUT = 4,
    HN_EASE_CSS    = 5,   /* CSS 的 ease(等价 cubic-bezier(.25,.1,.25,1)) */
    HN_EASE_CUBIC  = 6    /* 显式 cubic-bezier(a,b,c,d) */
};

/* 入场动画预设: 元素新出现时的起始状态 */
enum {
    HN_ENTER_NONE = 0,
    HN_ENTER_UP,        /* 下方 8px 淡入上浮(列表/消息条目的默认观感) */
    HN_ENTER_DOWN,
    HN_ENTER_FADE,      /* 仅淡入 */
    HN_ENTER_SCALE,     /* 0.96 放大淡入(卡片/弹窗) */
    HN_ENTER_LEFT
};

typedef struct hn_anim {
    int   inited;        /* 是否已初始化(初值直接取样式, 不产生动画) */
    int   dirty_written; /* 上次 tick 是否把插值写回过样式(用于区分样式来源) */
    int   active;        /* 仍在过渡 */
    float ms;            /* 时长(0 = 关闭过渡) */
    float t;             /* 进度 0..1 */
    float o,  o_from,  o_to;
    float bg[4], bg_from[4], bg_to[4];
    float fg[4], fg_from[4], fg_to[4];
    /* 几何动画: 位移与缩放(与颜色同样走插值) */
    float tx, tx_from, tx_to;   /* translate x */
    float ty, ty_from, ty_to;   /* translate y */
    float sc, sc_from, sc_to;   /* scale */
    float rot, rot_from, rot_to;      /* rotate 角度 */
    /* @keyframes 运行状态 */
    const hn_keyframes *kf;     /* 当前动画定义 */
    float kf_clock;             /* 动画时钟(ms, 累加) */
    int   kf_done;              /* 已播完(非无限循环且到达终点) */
    int   ease;                 /* 本次过渡的缓动 */
    float cb[4];                /* cubic-bezier 参数 */
    int   fresh;                /* 刚创建且声明了入场动画: 首次 tick 播放 */
    int   entering;             /* 正在播放入场动画(独立于样式重算) */
} hn_anim;

struct hn_node {
    hn_node_kind kind;
    hn_arena *arena;   /* 所属文档的 arena(值缓冲/片段插入用) */
    struct hn_node *parent, *first, *last, *next, *prev;
    /* element */
    const char *tag;
    hn_attr    *attrs; int n_attrs;
    const char *id;
    /* text */
    const char *text; size_t text_len;
    /* computed style(每次 layout 重算) */
    hn_style style;
    /* 布局结果: border-box 绝对坐标 */
    float bx, by, bw, bh;
    /* 滚动: 内容总高与当前偏移(overflow 容器) */
    float content_h;
    float scroll_x, scroll_y;
    /* 测量模式下收缩后的内容宽(shrink-to-fit); pref_h = flex 自然主轴高(定高被压缩前的内容需求) */
    float pref_w, pref_h;
    /* 行内排版结果(每次 layout 重建; 元素节点也会持有 — 输入控件用) */
    hn_run *runs; int n_runs;
    /* 输入控件状态 */
    char  *value; size_t value_len, value_cap;  /* 可编辑当前值(arena) */
    int    caret;                                /* 字节偏移 */
    /* 过渡动画当前值(opacity / 背景 / 前景, 0..1) */
    hn_anim anim;
    /* 外部资源动画时钟(ms)。Lottie 与网格变形共用 —— 与 CSS 动画同一帧循环,
       但独立计时: CSS 动画时钟从 0 开始播一遍, Lottie 需要持续累加(循环取模)。 */
    float ext_clock;
    /* 脚本写入的网格(hn_node_set_mesh_verts)。非 NULL 时覆盖 hn-mesh 声明的
       程序化网格 —— 让应用层实现自己的 rig(Live2D 的装配属于应用层)。 */
    float *mesh_verts;
    int    mesh_cols, mesh_rows;
};

struct hn_doc {
    hn_arena *arena;
    hn_node  *root;
    struct hn_sheet **inline_sheets;
    int n_inline, cap_inline;
};

/* 解析到既有 root(整文档用 <html> root, 片段用临时 root) */
void hn_parse_into(hn_doc *doc, hn_node *root, const char *src, size_t len);

/* ---------- CSS AST ---------- */

typedef struct {
    const char *tag;           /* 可为 NULL */
    const char *id;            /* 可为 NULL */
    const char *cls[8]; int n_cls;
    int pseudo;                /* 0=无 1=:hover 2=:active 3=:focus */
    int nth;                   /* 0=无 -1=:nth-child(odd) -2=even >0=第 N 个(1 起)
                                  -3=:first-child -4=:last-child */
    int comb;                  /* 与左侧的组合器: 0 后代 1 子代(>) 2 相邻(+) 3 通用兄弟(~) */
} hn_compound;

typedef struct { hn_compound *parts; int n_parts; } hn_selector;

/* 一条规则 = 单个 selector + 声明组(选择器组会被展开成多条) */
typedef struct {
    hn_selector sel;
    hn_decl    *decls; int n_decls;
    int spec;    /* id*256 + class*16 + tag */
    int order;   /* 样式表内出现顺序 */
    float media_min_w, media_max_w;  /* @media 条件; -1 = 无约束 */
} hn_rule;

struct hn_sheet {
    hn_arena *arena;
    hn_rule *rules; int n_rules;
    hn_keyframes *kfs; int n_kfs; int cap_kfs;   /* 本表内的 @keyframes */
};

/* 查找 @keyframes 定义(跨所有已加载样式表) */
hn_keyframes *hn_sheet_find_kf(hn_sheet *sh, const char *name);

/* ---------- context ---------- */
struct hn_context {
    hn_doc   *doc;
    hn_sheet **sheets; int n_sheets, cap_sheets;
    hn_arena *tmp;                  /* 每次 layout 重建的临时区 */
    hn_cmd   *cmds; int n_cmds, cap_cmds;
    hn_display_list dl;             /* 指向 cmds/n_cmds 的稳定视图 */
    float vw, vh;
    const hn_image_backend *images; /* 运行时持有 */
    /* 文本后端按**值**持有。绝不能存调用方传入的指针 —— 那通常是栈上局部变量,
       函数返回后失效; 而绘制阶段(hn_context_repaint)仍需用它测量文本,
       会读到已失效内存(表现为随机的 SIGBUS/EXC_BAD_ACCESS)。 */
    hn_text_backend tb;
    int tb_valid;
    hn_node *hover_node, *active_node, *focus_node; /* 伪类状态 */
    int      caret_on;   /* 是否绘制插入符(窗口聚焦时) */
    int      has_theme;  /* 索引 1 是否为主题槽(热更新时替换) */
    /* 透明背板: 1 = 每次样式计算后把 html/body 的底色清掉。
       必须是**持久的上下文状态**而非一次性改写 —— 样式每次 layout 都会
       从级联重算, 只改一次的话下一次 resize/热更新就把底色又装回来了
       (表现为"透明在首次显示时正常, 一改尺寸就变回不透明")。 */
    int      transparent;
    const hn_asset_backend *assets;  /* 运行时持有; 可为 NULL(则无外部资产) */
    /* Lottie 缓存: 按路径惰性解析一次(路径字符串在 tmp arena, 故键用副本)。
       文档重渲染时由 hn_context_compact 清空(旧 arena 已随文档失效)。 */
    struct { char *path; struct hn_lottie *lottie; } *lot;
    int n_lot, cap_lot;
};

/* ---------- 共享工具 ---------- */
/* 解析颜色字符串(#hex / rgb() / rgba() / 命名色), 成功返回 1 */
int   hn_color_parse(const char *s, size_t n, hn_color *out);
void  hn_style_default(hn_style *st);
void  hn_style_inherit(hn_style *dst, const hn_style *parent);
void  hn_style_compute_all(hn_context *c);
/* 解析内联 style 属性字符串为声明数组(给定 arena 分配) */
void  hn_parse_inline_decls(hn_arena *ar, const char *src, size_t len,
                            hn_decl **out, int *n_out);

void hn_layout_root(hn_context *c);
void hn_paint_root(hn_context *c);

/* ---------- 跨文件内部接口 ---------- */
/* 向当前显示列表追加指令(hn_paint.c)。供 lottie/网格等"几何生成器"复用,
   使它们不必知道指令列表的存储细节。 */
void hn_paint_push(hn_context *c, const hn_cmd *cmd);
/* 本次 layout 的临时区(生命周期 = 显示列表): 指令引用的顶点数组放这里 */
hn_arena *hn_context_tmp(hn_context *c);
/* 资产后端读取(hn_context.c) */
const hn_asset_backend *hn_context_assets(hn_context *c);
/* 立即清除根链底色(不打开持续开关); 由 layout 在样式重算后调用 */
void hn_strip_root_background_now(hn_context *c);

/* Lottie(hn_lottie.c): 解析并缓存; 释放缓存 */
struct hn_lottie *hn_context_lottie(hn_context *c, const char *path);
void hn_context_lottie_clear(hn_context *c);
/* 按时间求值并发射绘制指令 */
void hn_lottie_emit(hn_context *c, struct hn_lottie *l, float clock_ms,
                    float bw, float bh, float sx, float sy, float alpha,
                    const char *fit);
struct hn_lottie *hn_lottie_load(hn_context *c, const char *path);
void hn_lottie_free(struct hn_lottie *l);

/* 网格变形(hn_mesh.c): 发射 MESH 指令 */
void hn_mesh_emit(hn_context *c, hn_node *n, const char *src,
                  float bw, float bh, float sx, float sy, float alpha);

#endif

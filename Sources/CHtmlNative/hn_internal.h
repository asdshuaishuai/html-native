/* hn_internal.h — 内部共享结构 */
#ifndef HN_INTERNAL_H
#define HN_INTERNAL_H
#include "hn.h"
#include "hn_gif.h"   /* GIF 帧序列 API(hn_gif.c); 上下文缓存/绘制点两处都用 */

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
/* justify-content: 全部六种。
   AROUND/EVENLY 曾缺失, 声明后静默落到 flex-start —— 视觉上是"写了没反应",
   而且不报错, 属于最难自查的那类缺陷。 */
typedef enum {
    HN_JUST_START = 0, HN_JUST_CENTER, HN_JUST_END, HN_JUST_BETWEEN,
    HN_JUST_AROUND, HN_JUST_EVENLY
} hn_justify;
typedef enum { HN_ALIGN_STRETCH = 0, HN_ALIGN_START, HN_ALIGN_CENTER, HN_ALIGN_END } hn_align;
typedef enum { HN_DISP_BLOCK = 0, HN_DISP_FLEX, HN_DISP_NONE, HN_DISP_INLINE, HN_DISP_INLINE_BLOCK } hn_display;

/* 定位方式: static 参与常规流; relative 在流中但作为定位参照;
   absolute 脱离流(参照最近 positioned 祖先); fixed 脱离流(参照视口) */
typedef enum { HN_POS_STATIC = 0, HN_POS_RELATIVE, HN_POS_ABSOLUTE, HN_POS_FIXED } hn_position;

/* CSS 自定义属性(--name / var(--name)): 级联时收集, 继承自父, 子可覆盖 */
typedef struct { const char *name, *value; } hn_var;

/* transition 的属性槽: 独立时长/延迟/缓动按下标存放。
   transform 槽覆盖 translate-x/translate-y/scale/rotate(CSS 里 transform
   是单条属性, 整体插值)。 */
enum {
    HN_TPROP_OPACITY = 0,
    HN_TPROP_BG,          /* background / background-color */
    HN_TPROP_FG,          /* color */
    HN_TPROP_TRANSFORM,   /* translate/scale/rotate */
    HN_TPROP_N
};

typedef struct hn_style {
    hn_display display;
    int        flex_row;      /* 1=row 0=column */
    /* flex-wrap: 0=nowrap(默认, 溢出收缩) 1=wrap(换行/换列)。
       缺这个字段时 flex-wrap:wrap 完全无效 —— 子项被压到一行里
       (实测 3 个 width:60 塞进 width:100 容器, 各缩到 33.3 而不是换行)。 */
    int        flex_wrap;
    hn_justify justify;
    hn_align   align;      /* align-items: 容器级(作用于所有子项) */
    /* align-self: 子项级, 覆盖父容器的 align-items。
       必须与 align 分开存 —— 二者曾共用同一个字段, 于是 align-self 的级联值
       会与 align-items 互相覆盖, 表现为"align-self 写了没反应"。 */
    unsigned char has_self_align;
    hn_align   self_align;
    float      flex_grow;
    float      flex_shrink;   /* 默认 1: 超出时按基数比例收缩 */
    float      flex_basis;    unsigned char flex_basis_u; /* flex:<n> ⇒ basis 0 */
    int        flex_order;    /* order: 视觉顺序, 默认 0 */
    float      gap;
    float      width;    unsigned char width_u;
    float      height;   unsigned char height_u;
    /* calc(<pct>% ± <len>) 的长度偏移(px): 值仍走 PCT 通道, 偏移在尺寸
       解析处加上。纯绝对项的 calc 在 sv_len 里就地算完, 不落这里。 */
    float      width_calc_px, height_calc_px;
    float      margin[4];    /* top right bottom left */
    /* margin/padding 的单位(HN_U_*)。百分比必须留到布局期才能解析 ——
       它基于**包含块的行内尺寸(宽度)**, 而样式计算时还拿不到容器宽。
       早期实现把百分比当 px 直接用, 于是 margin-top:50% 变成 50px
       而不是容器宽的 50%(200px), 且不报错。 */
    unsigned char margin_u[4];
    unsigned char padding_u[4];
    float      padding[4];
    float      border_w;
    hn_color   border_color;
    float      radius;
    hn_color   background;
    hn_color   color;
    float      font_size;
    int        font_weight, font_italic;
    float      letter_spacing;
    /* word-spacing: 每个"词间隔"(空白分隔符)附加的宽度(px; 继承)。
       折叠模式下作用于被压成的那一枚空格; pre/pre-wrap 作用于串内每个空白。
       之前完全没有 —— 声明了不生效也不报错。 */
    float      word_spacing;
    int        text_align;   /* 0=left 1=center 2=right */
    float      line_height;  /* 倍数 */
    /* -webkit-line-clamp / line-clamp: 最多显示的行盒数(0 = 不限制, 不继承)。
       内容超出时最后一行以省略号收尾 —— 此前 ellipsis 只能配 nowrap 做单行
       截断, 多行场景(卡片摘要)无法表达。 */
    int        line_clamp;
    int        box_border;   /* box-sizing: border-box */
    float      opacity;
    /* ---- filter(brightness/contrast/saturate) ----
       绘制期对 fill 颜色做调色(乘法口径, 不做像素级离屏合成 ——
       与"QUAD 纯色填充"同级的简化)。缺省 1 = 原色。
       CSS 同名函数的百分比形式(120%)与数字形式(1.2)等价, 解析期统一。 */
    float      filter_br;      /* brightness: RGB 乘数 */
    float      filter_ct;      /* contrast: 围绕中灰 0.5 拉伸 */
    float      filter_sat;     /* saturate: 向亮度插值, 0 = 灰度 */
    /* ---- clip-path(circle/inset 简化版) ----
       裁剪发生在绘制期(CLIP_PUSH), 覆盖元素自身背景与全部子树;
       ellipse()/polygon()/引用 SVG 均不支持(声明被忽略)。 */
    unsigned char clip_shape;  /* 0=无 1=circle 2=inset */
    float      clip_a, clip_b, clip_c, clip_d;
    /* clip_a..d 按 shape 取值:
       circle: clip_a = 半径(clip_pct bit4 = 1 时为百分数, 绘制期按
               sqrt(w²+h²)/√2 解析; 0 且非百分数 = closest-side, 即短边一半)
       inset:  clip_a..d = top right bottom left(clip_pct bit0..3 = 对应边为百分比) */
    unsigned char clip_pct;
    float      clip_round;     /* inset(... round R) 的圆角(px; % 按短边解析) */
    /* 渐变背景(background: linear-gradient(a, c1, c2)) */
    unsigned char has_gradient;
    hn_color   grad_from, grad_to;
    float      grad_angle;
    /* 盒阴影(box-shadow: ox oy blur color) */
    unsigned char has_shadow;
    hn_color   sh_color;
    float      sh_ox, sh_oy, sh_blur;
    float      transition_ms; /* 过渡时长(0 = 关闭) */
    /* transition-delay: 全局延迟(ms, 简写第二个时间值或 longhand)。
       延迟用"负进度"实现 —— t 从 -delay/ms 起步, ease_apply 对 t<=0 返回 0,
       即停在起始态直至延迟耗尽; 负延迟则直接从中途开始。 */
    float      transition_delay;
    /* 按属性独立过渡(transition: opacity .3s, transform .5s / longhand 列表)。
       槽位下标 = HN_TPROP_*。has_tprop=1 时未单独声明的槽不过渡(时长 0),
       与 CSS 一致; 没有任何属性段时全体走 transition_ms(旧简写行为不变)。 */
    unsigned char has_tprop;
    float      tprop_ms[HN_TPROP_N];
    float      tprop_delay[HN_TPROP_N];
    unsigned char tprop_ease[HN_TPROP_N];
    float      tprop_cb[HN_TPROP_N][4];
    /* animation-delay: 关键帧动画与入场预设共用的启动延迟(ms, 可负 ——
       负值 = 跳过前段直接从中途开始; stagger 列表用
       style="--i:N" + animation-delay: calc(var(--i) * 0.1s) 表达) */
    float      anim_delay;
    /* ---- 2D/3D 变换(transform) ---- */
    float      rotate;        /* rotate()/rotateZ(): 绕 Z 轴, 度 */
    float      rotate_x;      /* rotateX(): 绕 X 轴, 度 */
    float      rotate_y;      /* rotateY(): 绕 Y 轴, 度 */
    float      perspective;   /* 父级 perspective(n): 视距 px; >0 启用透视 */
    float      skew_x, skew_y;/* skew(): 斜切, 度 */
    /* translateZ(): 沿 Z 的深度位移(px)。>0 靠近观察者 → 透视下变大。
       之前只解析不投影 —— 声明了毫无视觉效果。 */
    float      translate_z;
    /* rotate3d(x, y, z, angle): 绕**任意轴**旋转。轴在解析期归一化。
       之前只解析不投影。 */
    float      r3d_x, r3d_y, r3d_z;   /* 归一化轴 */
    unsigned char has_r3d;
    float      r3d_deg;
    /* transform-style: 0 = flat(缺省, 子级被摊平到父的投影面上)
                       1 = preserve-3d(子级继承父的复合 3D 矩阵继续叠)。
       缺这个字段时 preserve-3d 声明无效 —— 立方体各面只按自身 transform
       独立投影, 场景旋转根本进不了子级。 */
    unsigned char transform_style;
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
    /* text-shadow: <ox> <oy> <blur>? <color>(多个阴影取第一个)。
       之前完全没有 —— 声明了不生效也不报错。 */
    unsigned char text_shadow;
    hn_color      text_shadow_color;
    float         text_shadow_blur, text_shadow_ox, text_shadow_oy;
    unsigned char white_space;    /* 0=normal 1=nowrap 2=pre 3=pre-wrap */
    /* ---- 几何与动画声明(可插值) ---- */
    float      translate_x, translate_y;  /* translate: x y */
    float      scale;                     /* scale: n (默认 1) —— 等比 */
    /* scaleX()/scaleY()/scale(x,y) 的**非等比**分量。与 scale 相乘生效:
       scale 既是"等比那一路"的载体也是动画插值字段, 所以非等比单独存,
       不能把 scale 覆盖成 x 分量 —— 那会让动画只驱动一个轴。 */
    float      scale_x, scale_y;          /* 默认 1; 与 scale 相乘 */
    /* transform-origin: <x> <y>(px 或 %)。缺省 = 盒中心(50% 50%)。
       之前完全没有 —— 于是所有 rotate/scale 都以盒中心为原点,
       想绕左上角转(铰链/表盘)只能靠 translate 硬凑。 */
    float      origin_x, origin_y;        /* 相对盒左上角的 px 偏移 */
    unsigned char has_origin;
    unsigned char origin_pct_x, origin_pct_y;  /* 1 = 上两项是 0..1 的分数 */
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
    /* object-fit: <img> 的内容适配(替换元素的"内容盒内放置"语义)。
       0=fill(缺省, 拉伸铺满 —— 与旧口径逐位一致) 1=contain 2=cover 3=none。
       此前完全没有: 声明了不生效也不报错, 所有图片一律被拉伸。
       非 <img> 元素声明它无副作用(绘制点只在 <img> 分支消费)。 */
    unsigned char object_fit;
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
    HN_EASE_CUBIC  = 6,   /* 显式 cubic-bezier(a,b,c,d) */
    /* 弹性族(闭式解, 无逐帧积分状态 —— 与引擎"按 t 求值"的缓动口径一致) */
    HN_EASE_SPRING  = 7,  /* 弹簧: 衰减振荡逼近目标, 少量过冲后落位 */
    HN_EASE_BOUNCE  = 8,  /* ease-out-bounce: 落地弹跳(标准 CSS 分段) */
    HN_EASE_ELASTIC = 9,  /* ease-out-elastic: 橡皮筋(c4 = 2π/3) */
    HN_EASE_BACK    = 10, /* ease-out-back: 先回拉再越过目标收回(c1 = 1.70158) */
    HN_EASE_STEPS   = 11  /* steps(n[, start|end]): cb[0]=n, cb[1]=1 为 start */
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
    float t_delay;       /* 简写过渡的延迟(ms; 重启时换算成负进度) */
    float t;             /* 进度 0..1(可为负 = 延迟等待) */
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
    /* 按属性独立过渡(HN_TPROP_*): 每槽进度与激活位图。
       槽配置不缓存 —— 每次 tick 现读 st->tprop_*(样式表热换立即生效);
       需要重启(重取 from)的信号是"槽目标值变化", 配置变化只影响推进速率。 */
    unsigned char per_prop;     /* 1 = 按槽推进(样式声明了 per-property 列表) */
    unsigned char slot_active;  /* 位图: bit s = 槽 s 仍在过渡 */
    float slot_t[HN_TPROP_N];   /* 各槽进度(可为负 = 延迟等待) */
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
    /* hn-stream 流式容器: 1 = 用户上翻让出跟随(离底 > 48px; 滚回底部
       附近自动恢复)。存节点上: 文档替换(节点重建)即回默认跟随 —— 与
       流式推送"新内容优先贴底"的直觉一致。 */
    int stream_yield;
    /* hn-stream 平滑追上: 1 = 跟随动画在途(布局期武装, hn_context_anim_tick
       逐帧向底部指数逼近; gap < 1px 落位即清)。用户让出立即清 —— 动画
       中断, 让出状态机只有 stream_yield 一套。 */
    int stream_anim;
    /* 锚点平滑滚动(#锚导航): 1 = 滚动动画在途(hn_context_scroll_to 武装
       "把锚滚进可视区"的最终偏移 scroll_target, hn_context_anim_tick 逐帧
       指数逼近 —— 与 stream 平滑追上同一时间常数; gap < 1px 落位即清)。
       用户滚轮/脚本滚动(hn_node_scroll_by)立即清 —— 与 stream 同一套
       "用户接管即中断"纪律。离屏/无帧循环管线不武装, 直接落位。 */
    int   scroll_anim;
    float scroll_target;   /* 动画目标偏移(武装时按布局盒现算并钳界) */
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
    /* 网格视线跟随(hn-mesh-look)的一阶滞后状态: 每次网格求值向"指针方向
       目标"逼近固定系数(见 hn_mesh.c)。存节点上 —— 文档替换(节点重建)
       自然归零, 不需要额外清理; 状态在两次求值之间延续才有"平滑转头"。 */
    float  mesh_look_x, mesh_look_y;
    /* 数字人参数桥(hnMeshParam(id, name, v)): JS 写入、网格求值时叠加对应
       形变。全部钳在 -1..1(写入处钳); 缺省 0 = 无形变 —— 未接桥的文档
       与旧引擎逐位一致。 */
    float  mesh_param_look_x, mesh_param_look_y;   /* 视线: 水平/竖直偏移 */
    float  mesh_param_mouth;                       /* 口型: 上带局部张开 */
    float  mesh_param_body_x, mesh_param_body_y;   /* 整体摆 */
    /* 闭眼压缩量 0..1(0=睁眼缺省 → 无形变; 1=全闭)。注意 hnMeshParam 的
       "eye" 传的是**开合度**(1=睁眼), 引擎侧取 1-v 存这里 —— 字段零缺省
       必须对应"无形变"。Live2D motion 接线(EyeLOpen/EyeROpen 均值)同样
       以闭眼量写入。 */
    float  mesh_param_eye;
};

struct hn_doc {
    hn_arena *arena;
    hn_node  *root;
    struct hn_sheet **inline_sheets;
    int n_inline, cap_inline;
    /* 待消费链接(markdown 外链点击时置入; 宿主经
       hn_doc_take_pending_link 取走, 一次消费即清空)。malloc 副本 ——
       消费即 free, 不进 arena。 */
    char *pending_link;
};

/* ---------- 媒体(<video>/<audio>)会话 ----------
 * 一个媒体元素一条会话: 状态机(仿 HTMLMediaElement 六态) + 宿主句柄 +
 * 当前帧缓存。引擎只认 hn_media_host 函数指针表, 不知道任何解码器。 */
typedef struct hn_media_session {
    hn_node *node;              /* 归属元素(文档替换时随会话一起销毁) */
    char    *src;               /* 资源键(malloc 副本; 元素 arena 可能更早失效) */
    void    *handle;            /* 宿主句柄(NULL = 未 open) */
    hn_media_state_t state;
    int      want_play;         /* play()/autoplay 意图: LOADING→READY 后自动起播 */
    double   cur_time, duration;/* cur_time 每帧同步自宿主 position/frame pts */
    float    volume;
    int      muted, loop;
    /* 帧缓存(引擎 malloc)。双缓冲: 写入 frame_back, 成功后与 frame 交换
       指针 —— 显示列表持有的指针在下次 repaint 前有效(引擎单线程,
       paint→raster 顺序固定)。 */
    unsigned char *frame, *frame_back;
    int    frame_w, frame_h, frame_stride;
    double frame_pts;
    int    has_frame;
} hn_media_session;

/* 解析到既有 root(整文档用 <html> root, 片段用临时 root) */
void hn_parse_into(hn_doc *doc, hn_node *root, const char *src, size_t len);

/* ---------- DOM 构造(hn_html.c 实现; hn_md.c 复用同一口径) ----------
 * tag 传入字面量小写标签(指针被节点直接持有, 不复制); 文本复制进 arena。
 * 新节点带 anim.fresh 入场标记, 与 HTML 解析器产物完全一致。 */
hn_node *hn_dom_elem(hn_arena *ar, const char *tag);
hn_node *hn_dom_text(hn_arena *ar, const char *s, size_t n);
void     hn_dom_append(hn_node *parent, hn_node *child);

/* ---------- Markdown 直染(hn_md.c) ----------
 * 把带 hn-md 属性的元素的文本内容(markdown 源)替换成普通 DOM 子树。
 * 在装载序列里调用(首次 layout 之前、<script> 执行之前), 返回转换的
 * 元素数量。之后样式/布局/绘制/交互全部走既有管线, 零特例。 */
int hn_md_apply(hn_doc *doc);

/* ---------- JSON / CSV 直染(hn_fmt.c) ----------
 * 与 hn_md 同一挂点批次: <div hn-json> 源 → 语法着色树(.json-head 摘要 +
 * .json-row 缩进行 + tok-k/tok-s/tok-n/tok-b 令牌); <div hn-csv>
 * (hn-csv-sep 可选分隔符)→ 既有 table 语义(.csv-head 表头 + 数字列右对齐)。
 * 深度/扇出/行数截断, 失败优雅回落原样代码块。返回转换的元素数量。 */
int hn_fmt_apply(hn_doc *doc);

/* ---------- 链接导航(pending link; hn_context.c 实现) ---------- */
/* 置入待消费链接(替换旧值); 供 hn_context_activate_link 内部使用 */
void hn_doc_set_pending_link(hn_doc *doc, const char *href);

/* ---------- CSS AST ---------- */

typedef struct hn_compound {
    const char *tag;           /* 可为 NULL */
    const char *id;            /* 可为 NULL */
    const char *cls[8]; int n_cls;
    int pseudo;                /* 0=无 1=:hover 2=:active 3=:focus
                                  4=:disabled 5=:enabled 6=:checked */
    int nth;                   /* 0=无 -1=:nth-child(odd) -2=even >0=第 N 个(1 起)
                                  -3=:first-child -4=:last-child */
    int comb;                  /* 与左侧的组合器: 0 后代 1 子代(>) 2 相邻(+) 3 通用兄弟(~) */
    /* 属性选择器: [attr] / [attr=v] / [attr^=v] / [attr$=v] / [attr*=v] / [attr~=v]。
       之前完全没有 —— 声明了不报错也不生效, 是最典型的静默失败。 */
    struct { const char *name; const char *val; unsigned char op; } attr[4];
    int n_attr;                /* op: 0 存在 1 = 2 ^= 3 $= 4 *= 5 ~= */
    /* :not(...) 的实参(一层简单复合; **链式** :not(a):not(b) 各占一槽 ——
       此前单槽实现会让后者覆盖前者, "排除两种类型"写法静默失效)。
       :not 自身不计特异性, 实参按其自身级别计。 */
    struct hn_compound *not_cp[4];
    int n_not;
    /* :nth-of-type / :first-of-type / :last-of-type: 同 nth 的编码, 但只数同标签兄弟 */
    int nth_type;
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

/* ---------- <canvas> 2D 账本(hn_canvas.c 记录, hn_paint.c 翻译) ----------
 * QuickJS 桥把每个 2D context 绘制调用解析成"已解析的原语 op"(坐标已经
 * JS 侧 CTM 变换、颜色已是 RGBA), 按元素 id 记入账本; 绘制该元素时
 * hn_paint 把账本重放成显示列表指令。账本属于元素生命周期: 只有
 * clearRect(op=clear)/文档替换(元素销毁)清空 —— 样式重算与重放不动它。
 * 定长上限 + 溢出标志: 账本满后新 op 被丢弃(不崩溃, 标志可内省)。 */
typedef enum {
    HN_CV_FILL_RECT = 1,   /* frect: 矩形填充(x,y,w,h + color; 可带 shadow 字段) */
    HN_CV_STROKE_PATH,     /* spath: 开放折线描边(pts + color + stroke_w +
                              cap_round); 每段展开成 QUAD, 端/交点补圆。
                              带渐变(stops)时按"每段取中点色画纯色 QUAD"
                              的分段近似(与光栅器无逐段 pattern 的取舍一致) */
    HN_CV_FILL_PATH,       /* fpath: 闭合多边形填充(pts, 隐式闭合) */
    HN_CV_GRAD_RECT,       /* grect: 矩形渐变(轴两端 gx0,gy0→gx1,gy1 或
                              radial 双圆 gx0=cx0..gr1, 形态看 grad_radial;
                              颜色 = 首末两端色 color→color2 或 stops 数组) */
    HN_CV_GRAD_PATH,       /* gpath: 闭合多边形渐变填充(几何同 fpath,
                              渐变字段同 grect —— fill() 遇渐变不再回落纯色) */
    HN_CV_CLIP_PUSH,       /* clip: 矩形裁剪入栈(x,y,w,h) */
    HN_CV_CLIP_POP,        /* cpop: 裁剪出栈(JS save/restore 栈配平) */
    HN_CV_TEXT,            /* text: str 在 (x, y) 基线处, 字号 font_px + color */
    HN_CV_IMAGE,           /* image: src 图片画到 (x,y,w,h); has_src 时按
                              源子矩形 (sx,sy,sw,sh) 贴出(9 参 drawImage) */
    HN_CV_CLEAR_RECT,      /* clear_rect: 局部清空(x,y,w,h; 绘制期填渲染底色) */
    HN_CV_PDATA,           /* pdata: putImageData —— RGBA 块(pdata/pw/ph,
                              malloc)贴到 (x,y); has_dirty 时只贴脏矩形
                              (dtx..dth, 记录侧裁进源子矩形)。块字节上限
                              HN_CV_PDATA_MAX_BYTES, 超限拒录并置溢出提示 */
    HN_CV_PATTERN_RECT,    /* prect: createPattern 平铺填充矩形
                              (str=源图路径, prep=平铺模式, x,y,w=h 目标) */
    HN_CV_PATTERN_PATH,    /* ppath: createPattern 平铺填充闭合多边形(pts) */
    HN_CV_CLIP_PATH        /* cpath: 精确路径裁剪入栈(pts 折线多边形;
                              segs/n_segs = 子路径点数表, 缺省单多边形) */
} hn_canvas_op_kind;

/* 每块 putImageData 的像素字节上限(单 op 有界化; 超限拒录)。JSON 协议
   携带像素是十进制数数组, 4MB 块对应 ~16MB 文本 + 解析期 arena —— 语义
   上限压在这里, 页作者应分块贴(热力图/瓦片本来就是分块的)。 */
#define HN_CV_PDATA_MAX_BYTES (4u << 20)

/* 平铺模式枚举 HN_PAT_* 在 hn.h(hn_cmd.pattern_rep 是公共字段,
   光栅后端也要认)。 */

typedef struct hn_canvas_op {
    hn_canvas_op_kind kind;
    float x, y, w, h;
    hn_color color;        /* 填充/描边/渐变 from 端/文字色(0xRRGGBBAA) */
    hn_color color2;       /* 渐变 to 端 */
    float stroke_w;        /* 描边宽(px, 画布局部坐标) */
    int   cap_round;       /* 1 = 圆头/圆交点 */
    float *pts;            /* 折线/多边形顶点(x,y 交替; malloc, clear/销毁释放) */
    int    n_pts;
    float gx0, gy0, gx1, gy1;   /* 渐变: linear = 轴两端; radial = 圆心 (cx0,cy0)/(cx1,cy1) */
    float gr0, gr1;             /* radial 双圆半径(createRadialGradient 口径, 双圆全支持) */
    unsigned char grad_radial;  /* 渐变形态: 0 = linear 1 = radial */
    hn_grad_stop *stops;        /* 多 stop 色标(malloc 数组; op_free 释放; 可 NULL = 首末两端) */
    int    n_stops;
    /* fillRect 系(frect/grect)的阴影(canvas shadowColor/Blur/OffsetX/Y;
       路径填充/描边暂不带影 —— 注释即契约) */
    unsigned char shadow;
    hn_color sh_color;
    float sh_ox, sh_oy, sh_blur;
    /* 每指令混合模式(HN_BLEND_*; globalCompositeOperation 子集)。
       所有落账 op 共带: 未列出的模式解析期回落 source-over 并提示一次。 */
    unsigned char blend;
    /* image 的源子矩形(9 参 drawImage); has_src=0 = 整图(3/5 参不变) */
    float sx, sy, sw, sh;
    unsigned char has_src;
    /* pdata 的 RGBA 块(非预乘, pw*4 紧排; malloc, op_free 释放) */
    unsigned char *pdata;
    int pw, ph;
    /* pdata 的脏矩形(canvas 原语: 只贴块内 (dtx,dty,dw,dh) 子区) */
    float dtx, dty, dtw, dth;
    unsigned char has_dirty;
    /* cpath 的子路径点数表(每条子路径顶点数; NULL = pts 是单多边形)。
       malloc 数组, op_free 释放; 生命周期同 op(显示列表重放读它)。 */
    int *segs;
    int n_segs;
    char  *str;            /* text 文本 / image·pattern 源路径(malloc 副本) */
    float font_px;
    /* pattern 的平铺模式(HN_PAT_*; prect/ppath 消费) */
    unsigned char prep;
} hn_canvas_op;

typedef struct hn_canvas_ledger {
    char *id;              /* canvas 元素 id(malloc 副本; JS 桥的寻址键) */
    hn_canvas_op *ops;
    int n_ops, cap_ops;
    int overflow;          /* 1 = 曾达上限, 之后的 op 被丢弃 */
} hn_canvas_ledger;

/* 每块画布账本的 op 上限(给足; 满则丢弃并置溢出标志) */
#define HN_CANVAS_MAX_OPS 4096

/* 查找(不创建)/查找或创建 账本 */
hn_canvas_ledger *hn_canvas_ledger_find(hn_context *c, const char *id);
hn_canvas_ledger *hn_canvas_ledger_get(hn_context *c, const char *id);
/* 释放全部账本(文档替换 = 元素销毁 / context 销毁时调) */
void hn_canvas_dispose(hn_context *c);

/* getImageData 的栅格化核心(hn_paint.c 实现 —— 账本重放是它的地盘):
   把账本按 canvas 属性坐标 1:1 重放到一张临时显示列表, 交注入的渲染
   后端渲成 w×h 的 RGBA8 非预乘位图(malloc, 调用方 free)。失败返回 NULL。 */
unsigned char *hn_paint_raster_ledger(hn_context *c, hn_canvas_ledger *l,
                                      float cw, float ch);
/* 依赖缝: 核心不硬链渲染后端 —— 运行时侧(rt, 已链 hnsoft)open 时注入
   渲染函数(hnsoft_render); 核心专属构建(include_probe 等, 无 hnsoft)
   不注入 = getImageData 的像素出口如实不可用。 */
typedef unsigned char *(*hn_paint_render_fn)(const hn_display_list *, int, int,
                                             hn_color);
void hn_paint_set_ledger_raster(hn_paint_render_fn fn);

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
    /* 最近指针坐标(视口参照系): hn_rt_pointer_move 每次事件喂入。网格变形
       的 look(视线/头部跟随)在求值时按"指针相对元素中心"取指向 —— 引擎
       此前不存坐标, 形变无从"看向"指针。只此一份存储, 命中测试仍按事件
       现场重算, 不引入第二事实源。 */
    float    pointer_x, pointer_y;
    int      has_pointer;   /* 0 = 尚未收到过事件(look 目标 = 回中) */
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
    /* GIF 动图缓存(hn_gif.c): 与 Lottie 同一纪律 —— 按路径惰性解码一次,
       键用副本, 文档替换/compact/destroy/换资产后端时全部清空。
       帧序列(合成后的整幅 RGBA)生命周期跟随本缓存, 显示列表的 BITMAP
       指令直接引用它 —— 下次 repaint 前不会被回收。
       内存治理(性能治理): 缓存带总量预算(帧数×宽×高×4 估算, 缺省 64MB,
       HN_GIF_CACHE_MB 可覆盖), 超限按最久未用逐出 —— 逐出 = 摘出缓存表
       并把 hn_gif 挂进"坟场"(gif_grave), 延迟到下一次重绘**之后**才
       hn_gif_free: 本帧已装进显示列表的帧指针必须活到重绘读完(摘表不
       释放 = 引用仍有效; 重绘重建显示列表后坟场里的对象必然不再被引用,
       此时释放无悬空)。 */
    struct {
        char *path; hn_gif *gif;
        unsigned long last_use;   /* 最近触达序号(g_gif_clock; LRU 逐出序) */
        size_t bytes;             /* 帧数×宽×高×4 估算占用 */
    } *gifs;
    int n_gif, cap_gif;
    size_t gif_bytes;             /* 缓存当前估算总占用(预算逐出用) */
    hn_gif **gif_grave;           /* 已逐出待释放(下一次重绘后统一 hn_gif_free) */
    int n_grave, cap_grave;
    /* 媒体宿主与会话表(hn_media.c)。宿主指针由运行时持有(NULL = 禁用);
       会话随文档替换/compact/destroy 全部 close+free —— 与 Lottie 缓存同一
       清理纪律(节点指针随旧文档 arena 失效)。 */
    const hn_media_host *media;
    hn_media_session *med_sessions; int n_med, cap_med;
    /* <canvas> op 账本表(hn_canvas.c 持有; 按 canvas id 寻址) */
    hn_canvas_ledger *canvases; int n_canvases, cap_canvases;
    /* 帧循环活跃标志: anim_tick 收到过 dt>0 即置位(不复位)。布局期锚定
       据此分流 —— 有帧循环的窗口走"平滑追上"动画(逐帧推进, 需要 dt);
       无帧循环的管线(离屏 shot/dump, 无从推进动画)保持"直接贴底"的旧
       口径, 单帧出图仍是终态。 */
    int live_frame;
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

/* GIF 动图(hn_gif.c 解码 + hn_context.c 缓存)。
   缓存口径与 Lottie 一致: 按路径惰性解码一次, 负结果也缓存(避免每帧重试)。
   带总量预算(HN_GIF_CACHE_MB, 缺省 64MB): 超限按最久未用逐出, 释放延迟到
   下一次重绘之后(见 struct hn_context 的 gif 字段注释)。 */
hn_gif *hn_context_gif(hn_context *c, const char *path);
void hn_context_gif_clear(hn_context *c);
/* 缓存现状(探针/诊断; 预算逐出的验收面) */
size_t hn_context_gif_bytes(const hn_context *c);
int    hn_context_gif_count(const hn_context *c);
/* 该元素要按 GIF 播放的源(见 hn_gif.c 尾部注释); 非 GIF 返回 NULL */
const char *hn_node_gif_src(hn_node *n);

/*/* 网格变形(hn_mesh.c): 发射 MESH 指令 */
void hn_mesh_emit(hn_context *c, hn_node *n, const char *src,
                  float bw, float bh, float sx, float sy, float alpha);

/*/* ---------- Cubism(moc3) 真引擎(hn_cubism.c; 官方 Core 绑定) ----------
 * hn-cubism 属性节点: 加载 model3.json → moc3(官方 Core 解析形变) →
 * motion3 逐参数驱动 → drawable 三角汤发射(HN_CMD_TRIS)。
 * Core 未链接(HN_HAVE_CUBISM 未定义)时 acquire 恒 NULL —— paint 落
 * 可见占位(下载指引+许可提示), 页面不静默空白。 */
void hn_cubism_paint(hn_context *c, hn_node *n,
                     float bw, float bh, float sx, float sy, float alpha);
const char *hn_cubism_runtime_version(void);

/* ---------- PainterEngine 软渲染集成(hn_pe.c; BSD-3 开源) ----------
 * hn-live="<image>": 贴图经 PE 的 px_surface + PX_TextureRenderEx_sincos
 * 渲染到离屏 surface, HN_CMD_BITMAP 上屏(透明底)。rig(呼吸/摇摆/跳跃)
 * 由引擎按 ext_clock 驱动 —— "引擎驱动 + PE 渲染"分层。 */
void hn_pe_paint(hn_context *c, hn_node *n,
                 float bw, float bh, float sx, float sy, float alpha);

/* canvas_ity 后端(hn_canvas_ity.cpp; HN_CANVAS_BACKEND=ity 启用):
 * 账本 op 经 canvas_ity 软光栅出帧 → BITMAP 并入显示列表; 失败返回 0
 * 由调用方回落账本重放。 */
int hn_canvas_paint_ity(hn_context *c, const char *id, const struct hn_canvas_ledger *l,
                        float cw, float ch, float bw, float bh,
                        float sx, float sy, float alpha);

/* CoreGraphics 后端带 hn_context 的工厂(hn_canvas_cg.cpp): ctx 注入后端,
 * 供 IMAGE/PDATA/PATTERN 经资产后端(hn_context_assets)加载源图;
 * 不带 ctx 的 hn_canvas_backend_cg_create 等价 ctx=NULL(图像类 op 跳过)。 */
struct hn_canvas_backend *hn_canvas_backend_cg_create_ctx(int width, int height,
                                                          void *ctx);

/* CoreGraphics 后端 paint 接线(hn_canvas_cg.cpp; HN_CANVAS_BACKEND=
 * system|cg 启用): 口径与 hn_canvas_paint_ity 一致 —— 账本 op 重放 →
 * 帧出 BITMAP 并入显示列表; 失败返回 0 由调用方回落。接线内经上面
 * create_ctx 注入 ctx(图像类 op 的源图通道)。 */
int hn_canvas_paint_cg(hn_context *c, const char *id,
                       const struct hn_canvas_ledger *l,
                       float cw, float ch, float bw, float bh,
                       float sx, float sy, float alpha);

/* ---------- three.js 子集垂直切片(hn_three.c) ----------
 * hn-three 属性节点: CPU 场景图 + 透视相机 + 旋转立方体, 顶点汤经
 * HN_CMD_POLYGON(3 顶点三角形)复用 paint_polygon 既有扫描内核出像素。
 * 自转取节点 ext_clock(hn_context.c 环境动画通道)。见
 * docs/threejs-support.md "已落地"节。 */
void hn_three_paint(hn_context *c, hn_node *n,
                    float bw, float bh, float sx, float sy, float alpha);

/* ---------- Live2D 运行时包(hn_lottie.c 尾部; moc3 边界诚实接入) ----------
 * 解析 model3.json(FileReferences/Motions 组)、cdi3.json(参数清单)、
 * physics3.json(Meta 计数)与 motion3.json(v2.4 曲线求值器)。
 * moc3 本体是专有二进制 —— 不解析, 引擎以 motion 数据驱动自有 rig
 * (hn_mesh 形变通道)替代, 事实经 hn_live2d_get_info 如实暴露。 */
typedef struct {
    int has_moc;             /* FileReferences.Moc 已声明(本体不解析) */
    int textures;            /* 贴图数 */
    int has_physics;         /* physics3.json 已解析 */
    int physics_settings, physics_inputs, physics_outputs, physics_vertices;
    int has_display_info;    /* cdi3.json 已解析 */
    int cdi3_params;         /* 参数清单条数 */
    int motion_groups;       /* Motions 组数 */
    int motions;             /* motion3 文件数(解析成功) */
    int curves_total;        /* 全部 motion 的曲线总数 */
    float duration_max;      /* 最长 motion 时长(秒) */
} hn_live2d_info;

struct hn_live2d *hn_live2d_load(hn_context *c, const char *model3_path);
void hn_live2d_free(struct hn_live2d *m);
/* 带进程级 4 槽 LRU 缓存的加载(实时窗口每帧采样不复解); hn_live2d_cache_clear 复位 */
struct hn_live2d *hn_live2d_acquire(hn_context *c, const char *model3_path);
void hn_live2d_cache_clear(void);
void hn_live2d_get_info(const struct hn_live2d *m, hn_live2d_info *info);
int hn_live2d_motion_count(const struct hn_live2d *m);
/* 按组名 + 组内序号取 motion 下标; 组缺省 → 第一个; 未命中 → -1 */
int hn_live2d_motion_pick(const struct hn_live2d *m, const char *group, int index);
/* motion3 求值: 取 motion_idx 在 t_sec 秒的 param 曲线值; 无该曲线 → 0 */
int hn_live2d_motion_sample(const struct hn_live2d *m, int motion_idx,
                            const char *param, float t_sec, float *out);
const char *hn_live2d_motion_file(const struct hn_live2d *m, int motion_idx);
float hn_live2d_motion_duration(const struct hn_live2d *m, int motion_idx);
/* cdi3.json 里参数 Id 的显示名(如 ParamAngleX → "角度 X"); 无 → NULL */
const char *hn_live2d_cdi3_name(const struct hn_live2d *m, const char *param_id);

/* ---------- 动画活跃类别(性能治理: 帧节拍分级) ----------
 * hn_context_anim_tick_ex 的类别出口。hn_rt 据此合成唤醒节拍与渲染路径:
 *   NONE        本帧无活跃动画 → 帧循环可熄火(节拍 -1, 宿主按空闲节奏醒)
 *   AMBIENT     仅"环境动画"在跑(ext_clock 类: hn-mesh sway/GIF/Lottie
 *               循环)—— 不写任何样式字段, 节拍可放宽到 ~30fps, 渲染走
 *               轻路径(免 relayout, 见 hn_context_repaint_light)
 *   INTERACTIVE 有"交互动画"在跑(CSS 过渡/@keyframes/入场预设/媒体在播/
 *               平滑滚动在途)—— 每帧写样式字段, 必须 16ms 节拍 + 全路径
 *               渲染(relayout + tick(0) 重采样, 既有口径)。 */
enum { HN_ANIM_NONE = 0, HN_ANIM_AMBIENT = 1, HN_ANIM_INTERACTIVE = 2 };

/* 带类别出口的动画推进(行为与 hn_context_anim_tick 完全一致, 多回填 *cls;
   cls 可为 NULL)。hn_context_anim_tick 即本函数的 NULL 出口薄封装。 */
int hn_context_anim_tick_ex(hn_context *c, float dt_ms, int *cls);

/* 轻重画(ambient 帧): 不动级联/布局 —— 环境动画只前进 ext_clock, 样式
   字段无变化, 重建显示列表即可。tmp 区与显示列表同生命周期(指令引用的
   顶点/文本副本从这里分配), 必须一并翻新, 否则 ambient 帧循环下逐帧
   无界增长。GIF 缓存的坟场也在此冲刷(重绘后旧引用必已消亡)。 */
void hn_context_repaint_light(hn_context *c);

/* 媒体(hn_media.c): 推进会话(宿主 tick + 状态机轮询), 返回 1 = 有活跃
   会话(PLAYING 或待起播) —— hn_context_anim_tick 据此保持帧循环 */
int  hn_media_tick_ret(hn_context *c, float dt_ms);
/* 销毁全部会话(host.close + 释放帧缓存与 src)。文档替换/compact/destroy 时调 */
void hn_media_sessions_clear(hn_context *c);
/* 布局: 替换元素固有尺寸(READY 会话已拉到帧时为帧尺寸); 未知写 -1。
   首次触达时建会话(autoplay 则同时 open)。 */
void hn_media_intrinsic(hn_context *c, hn_node *n, int *w, int *h);
/* 绘制: 轮询当前帧并更新缓存; 返回 1 且写出缓存指针/尺寸/pts
   (指针生命周期 = 本次显示列表, 双缓冲保证)。video 专用, audio 恒 0。 */
int  hn_media_poll_frame(hn_context *c, hn_node *n, const unsigned char **rgba,
                         int *w, int *h, int *stride, double *pts);

/* ---------- GPU 渲染后端(引擎侧翻译器, hn_gpu.c; 契约见
   platform/hn_platform.h 尾部"GPU 渲染后端"段) ----------
 * 宿主槽与 hn_media_host 同款: 壳(经 hn_rt_set_gpu_host)注入平台原语表,
 * 表类型 = hnp_gpu_host —— 引擎头不反向包含平台头, 以 const void* 进出。
 * hn_rt 对这三个符号有**弱桩**(rt/hn_rt.c): 不链 hn_gpu.c 的构建里桩生效
 * (恒回退 hnsoft), hnapp 目标里本文件的强定义覆盖 —— 与 hnp_macos.c 的
 * 媒体弱桩同一手法(仓库已验证的 Mach-O/ELF 通吃口径)。 */
void hn_gpu_set_host(const void *host);
/* 本帧显示列表是否可整体走 GPU(表在 + init 成功 + 逐指令 ⊆ supported 位集
   + 细粒度条件: 圆角/阴影/文字阴影/图片解码等, 见 hn_gpu.c 头注 6)。
   O(n) 便宜; 每帧渲染前调一次。 */
int  hn_gpu_available(const hn_display_list *dl);
/* GPU 光栅一帧: 等价于 hnsoft_render_scaled 的 GPU 版, 但直接产出**预乘
   BGRA**(Metal readback 原样, 与 rt_raster 的 CPU 路径产物同口径, blit 直用;
   与 hnsoft 非预乘 RGBA 的换算推导见 hn_gpu.c 头注 1)。缓冲 malloc(调用方
   free)。任何一步失败返回 0(调用方整帧回退 hnsoft —— 逐帧静默)。 */
int  hn_gpu_render_scaled(const hn_display_list *dl, int w, int h, hn_color bg,
                          float scale, unsigned char **out_bgra);

#endif

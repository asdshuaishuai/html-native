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
    HN_CMD_CLIP_POP = 5,
    HN_CMD_QUAD = 6,      /* 任意四边形(3D 投影结果); qx/qy 为四个顶点 */
    HN_CMD_POLYGON = 7,   /* 任意多边形(矢量路径/Lottie 形状层); 顶点数可变 */
    HN_CMD_MESH = 8,      /* 网格变形贴图(Live2D 类: 顶点网格 + UV 采样源图) */
    HN_CMD_BITMAP = 9     /* 内存位图(媒体当前帧), 覆盖 rect */

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
    /* QUAD: 4 个顶点(顺时针, 屏幕坐标). 用于 3D 变换后的面片填充 ——
       透视投影后矩形不再是矩形, 必须按四边形光栅化。 */
    float       qx[4], qy[4];
    /* POLYGON: 任意顶点数的多边形(矢量路径 / Lottie 形状层)。
       poly 指向顶点数组(引擎临时区分配, 生命周期 = 本次显示列表);
       poly_n 为顶点数; even_odd=1 使用奇偶填充规则(否则非零环绕)。 */
    const float *poly;
    int          poly_n;
    unsigned char even_odd;
    /* MESH: 网格变形(Live2D 类效果)。
       mesh_verts: 屏幕坐标顶点, 2*(cols+1)*(rows+1) 个 float (x,y 交替);
       mesh_uv:    对应源图归一化 UV, 同长度;
       mesh_cols/rows: 网格单元数;
       text: 源图路径(经图片后端加载)。 */
    const float *mesh_verts;
    const float *mesh_uv;
    int          mesh_cols, mesh_rows;
    /* BITMAP: 内存位图(RGBA8 非预乘, 左上原点, 顶上到底下)。
       指针生命周期 = 本次显示列表有效(引擎媒体会话双缓冲保证下次
       repaint 前不被覆盖)。bitmap_pts 为该帧显示时间戳(秒), 供宿主
       探针打印/断言(契约 §5 的 BITMAP 行需要它)。 */
    const unsigned char *bitmap;
    int    bitmap_stride, bitmap_w, bitmap_h;
    double bitmap_pts;
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

/* 资产后端: 读取外部数据文件(Lottie JSON 等)。
 * 引擎自身不做磁盘/网络 I/O —— 与文本/图片后端同样是依赖倒置:
 * 运行时决定文件从哪来(磁盘/内存/远端), 引擎只解析拿到的字节。
 * load 返回的缓冲由运行时持有, 引擎不释放; 失败返回 NULL。 */
typedef struct hn_asset_backend {
    void *ctx;
    const char *(*load)(void *ctx, const char *path, size_t *len);
} hn_asset_backend;

void hn_context_set_assets(hn_context *c, const hn_asset_backend *backend);

/* ---- 音频/视频(<video>/<audio>) ----
 *
 * 引擎侧只是"状态机 + 布局盒 + 当前帧位图"(仿 HTMLMediaElement 六态);
 * 解码/出声委托平台, 经 hn_media_host 回调表注入 —— 引擎不认识解码器,
 * 不包含任何平台头文件(与图片/资产后端同一依赖倒置)。
 * <video> 与 <audio> 走同一会话表、同一宿主回调; 纯音频只是"没有帧"的会话。
 *
 * 明确不做(docs/media-design.md §9): <source> 子元素、字幕/track、DRM、
 * 流媒体(http(s) 一律 open 失败)、FFmpeg、浏览器控件 UI、poster、
 * playbackRate、WebAudio。声明属性: src(相对文档目录) loop muted autoplay
 * volume="0.5"。
 */
typedef enum {
    HN_MEDIA_IDLE = 0,    /* 无会话/未打开 */
    HN_MEDIA_LOADING,     /* 已 open, 元数据未就绪 */
    HN_MEDIA_READY,       /* 可起播(未播) */
    HN_MEDIA_PLAYING,
    HN_MEDIA_PAUSED,
    HN_MEDIA_ENDED        /* 播完且非循环; play() 重新起播 */
} hn_media_state_t;       /* 契约名为 hn_media_state —— 与同名 API 在 C 的
                             同一命名空间冲突, typedef 加 _t 后缀 */

typedef struct hn_media_host {
    void *ctx;
    /* 打开资源; 返回宿主句柄(引擎不解释), 失败 NULL + err 一行原因。 */
    void *(*open)(void *ctx, const char *url, char *err, size_t err_cap);
    void  (*close)(void *ctx, void *m);   /* 幂等; NULL 安全 */
    /* 帧循环提示(dt 毫秒)。无钟宿主(合成宿主)用它自行累计假时钟;
       真实宿主(AVPlayer)忽略。PLAYING 会话每帧必达。 */
    void  (*tick)(void *ctx, void *m, float dt_ms);
    int   (*play)(void *ctx, void *m);                    /* 1 = 已接受 */
    void  (*pause)(void *ctx, void *m);
    int   (*seek)(void *ctx, void *m, double sec);        /* 1 = 接受 */
    void  (*set_volume)(void *ctx, void *m, float vol);   /* 0..1(越界钳制) */
    void  (*set_muted)(void *ctx, void *m, int muted);
    int   (*duration)(void *ctx, void *m, double *sec);   /* 0 = 未知(写 -1) */
    int   (*position)(void *ctx, void *m, double *sec);
    int   (*state)(void *ctx, void *m, int *ready, int *ended);
    /* 拉当前视频帧(RGBA8 非预乘, 左上原点): 1=新帧 0=无新帧(内容未变)
       -1=无视频轨或失败(cap 不足时 w/h 写所需尺寸, 调用方重分配后重试)。
       pts_sec 写该帧显示时间戳(秒)。 */
    int   (*frame)(void *ctx, void *m, unsigned char *rgba, size_t cap,
                   int *w, int *h, double *pts_sec);
} hn_media_host;

/* 注入媒体宿主(运行时持有表内存; NULL = 媒体禁用 —— 元素照常解析,
   只是不播, 布局走缺省尺寸, 绘制走占位)。 */
void hn_context_set_media(hn_context *c, const hn_media_host *host);

/* 元素寻址的媒体控制(JS 桥与探针共用)。n 非 video/audio 或无 src 时
   为 no-op; 宿主未注入时 play 返回 0、其余返回缺省值。 */
void   hn_media_tick(hn_context *c, float dt_ms);   /* hn_context_anim_tick 内部驱动 */
int    hn_media_play(hn_context *c, hn_node *n);    /* IDLE→open; 1 = 进入/保持播放 */
int    hn_media_pause(hn_context *c, hn_node *n);
int    hn_media_seek(hn_context *c, hn_node *n, double sec);  /* 钳制 [0, duration] */
double hn_media_time(hn_context *c, hn_node *n);               /* 无会话返回 0 */
double hn_media_duration(hn_context *c, hn_node *n);           /* 未知返回 -1 */
void   hn_media_set_volume(hn_context *c, hn_node *n, float v);/* 钳制 0..1 */
void   hn_media_set_muted(hn_context *c, hn_node *n, int muted);
int    hn_media_state(hn_context *c, hn_node *n);              /* HN_MEDIA_* */

/* ---- WASM(纯计算导出函数; docs/media-design.md §7) ----
 *
 * wasm3(vendor/wasm3, MIT 快照)封装: 解析 .wasm 字节 → 实例, 按名调用
 * 导出函数。v1 只收纯计算: i32/f32/f64 参数与返回(值一律按 int32 进出,
 * 引擎按模块导出签名转换); 不做 WASI、不做内存导出操作、不做表/间接调用。
 * 与宿主同进程同步调用(m3_Call), 无线程。
 */
typedef struct hn_wasm hn_wasm;
/* 解析+装载; 失败 NULL(错误一行 stderr)。字节由调用方持有, load 内部
   复制一份(wasm3 模块在整个生命周期引用原始字节, 见 wasm3.h 的
   ParseModule 注释 —— "data must be persistent during the lifetime")。 */
hn_wasm *hn_wasm_load(const unsigned char *bytes, size_t n);
/* 调用导出函数; args/out 均按 int32 进出(签名声明为 f32/f64/i64 时
   引擎就地转换)。实参个数必须等于签名形参个数。找不到函数/签名不符/
   陷阱/多返回值返回 0(结果不定), 成功返回 1。无返回值的导出把 *out 写 0。 */
int  hn_wasm_call(hn_wasm *w, const char *fn,
                  const int32_t *args, int n_args, int32_t *out);
void hn_wasm_free(hn_wasm *w);

/* 实例表(id→hn_wasm*): JS 桥寻址用(hnWasmLoad→id, hnWasmCall(id,..))。
   表由 hn_wasm.c 持有, 容量 32; id = 表下标+1(1..32)。install 复用
   hn_wasm_load(失败 -1); unload 幂等; by_id 对非法 id/空槽返回 NULL。 */
int      hn_wasm_install(const unsigned char *bytes, size_t n);
hn_wasm *hn_wasm_by_id(int id);
void     hn_wasm_unload(int id);

/* ---- Lottie(bodymovin)矢量动画 ----
 *
 * <img src="a.json" hn-lottie> 或任意元素 hn-lottie="a.json" 时,
 * 引擎解析 JSON 并在每帧按时间轴求值, 产出 POLYGON/IMAGE 绘制指令。
 * 支持子集: 形状层(组/矩形/椭圆/路径/填充/描边)与图片层。
 * precomp/文本/表达式/特效层不支持(会被跳过, 不报错)。
 *
 * 引擎负责"矢量图形 + 时间轴"; 播放控制由声明属性表达:
 *   hn-lottie-loop="0|1"  是否循环(默认 1)
 *   hn-lottie-speed="1.5" 播放倍速(默认 1)
 *   hn-lottie-fit="contain|cover|fill|none"  缩放适配(默认 contain)
 */
/* 解析并缓存(同一路径只解析一次); 失败返回 NULL */
struct hn_lottie;
struct hn_lottie *hn_lottie_load(hn_context *c, const char *path);
/* 动画时长(ms); 用于外部做进度条/一次性播放判定 */
float hn_lottie_duration_ms(struct hn_lottie *l);
/* 该 Lottie 的原始画布尺寸 */
void  hn_lottie_size(struct hn_lottie *l, float *w, float *h);

/* ---- 网格变形(Live2D 类效果的原语) ----
 *
 * Live2D 的 .moc3 是专有格式(Cubism SDK 商业授权), 无法解码。
 * 但它公开的核心技术是"把贴图映射到可变形网格上" —— 本引擎提供同一原语:
 *
 *   <img src="c.png" hn-mesh="10x8" hn-mesh-sway="6" hn-mesh-speed="1.2"
 *        hn-mesh-anchor="bottom" hn-mesh-freq="1">
 *
 * 顶点位置 = 网格 + 正弦形变(按 anchor 端固定, 向另一端渐强)。
 * 更复杂的骨骼/物理可由脚本通过 hn_node_set_mesh_verts() 逐帧驱动 ——
 * 引擎只提供网格与贴图合成, 装配逻辑(即 Live2D 的 rig)属于应用层。
 */
/* 脚本驱动网格: verts 为 2*(cols+1)*(rows+1) 个 float(x,y 交替, 元素盒局部坐标)。
   返回 1 表示节点已接受该网格。坐标原点 = 元素盒左上角。 */
int hn_node_set_mesh_verts(hn_node *n, const float *verts, int cols, int rows);
/* 该节点当前是否为网格绘制(声明了 hn-mesh 或被脚本写入顶点) */
int hn_node_mesh_info(hn_node *n, int *cols, int *rows);

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

/* 存在感(presence): 应用在系统里"多像一个 App"。
   这是"引擎弱化存在感"的核心开关 —— 同一份文档可以只是桌面上的一张卡片,
   也可以是一个有 Dock 图标的常规应用, 由文档自己声明。 */
typedef enum {
    HN_PRESENCE_AUTO = 0, /* 默认: 跟随 surface(window → app, 其余 → ghost) */
    HN_PRESENCE_GHOST,    /* 完全不占系统身份: 不进 Dock、无图标、不进 Cmd+Tab */
    HN_PRESENCE_APP       /* 明确要求应用身份: 驻留 Dock 并带图标 */
} hn_presence;

typedef struct hn_manifest {
    hn_surface_kind surface;
    float w, h;            /* 0 = 自动 */
    float x, y;            /* 左上角; 0,0 = 自动(居中) */
    const char *title;     /* 可为 NULL */
    const char *theme;     /* hn-theme: "dark" / "light", 可为 NULL */
    int transparent;       /* hn-transparent: 1 = 窗口无底色(逐像素 alpha) */
    int shadow;            /* hn-shadow: 1 = 窗口投影(默认开); 0 = 关闭 */
    int draggable;         /* hn-draggable: 1 = 空白区域可拖动窗口(默认开) */
    /* 小程序式生命周期: 页面关心自己被显示/隐藏/销毁(见 hn_lifecycle)。
       声明后运行时按顺序派发同名事件; 缺省不派发。 */
    const char *lifecycle;
    hn_presence presence;  /* hn-presence: 见上 */
    /* 轻应用(applet)槽位名: 声明后该表面成为"随用随消、半固化"的桌面轻应用
       (KDE Plasmoid 那类形态)。槽位按名字记住几何与配置 —— 销毁后再打开
       会还原到原处, 而不是回到屏幕中央或上次的随机位置。
       NULL = 普通表面(不占槽位)。 */
    const char *applet;
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
/* arena 压缩: 销毁旧 DOM, 重新解析 HTML, 回收已移除节点的内存 */
void hn_context_compact(hn_context *c, const char *html, size_t len);
/* 透明背板: 把 body 的实色背景改为透明(配合窗口 isOpaque=false) */
void hn_context_strip_root_background(hn_context *c);

/* ---- 解析 ---- */
hn_doc   *hn_parse_html(const char *src, size_t len);
hn_sheet *hn_parse_css(const char *src, size_t len);
void      hn_doc_free(hn_doc *doc);
void      hn_sheet_free(hn_sheet *sheet);

/* ---- 上下文 ---- */

/* 所有权约定(重要, 弄错会造成 double-free 或 use-after-free):
 *   - hn_context_destroy() 会释放通过 hn_context_set_doc / add_sheet 交给它的
 *     文档与样式表。也就是说: **set 之后不要再自己 free**。
 *   - 反过来, 未交给 context 的 doc/sheet 必须由调用方自己 free。
 * 简记: "谁 set 谁放手"。 */
hn_context *hn_context_create(void);
void        hn_context_destroy(hn_context *c);
/* 把文档交给 context(后者负责释放); doc 可为 NULL 表示清空 */
void        hn_context_set_doc(hn_context *c, hn_doc *doc);
/* 追加样式表(context 负责释放; 顺序 = 优先级, 后加的覆盖先加的) */
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
/* 焦点元素(:focus 伪类)与插入符可见性。
   set_focus 在已有布局时会**立即重渲染**(样式重算 + 重布局 + 重绘),
   使 :focus 规则与引擎默认焦点环即刻生效; 禁用的控件拒绝聚焦。 */
void hn_context_set_focus(hn_context *c, hn_node *n);
void hn_context_set_caret_visible(hn_context *c, int on);
/* 元素计算样式的 cursor(0=default 1=pointer) */
int  hn_node_cursor(hn_node *n);

/* ---- 输入控件 ---- */
/* 控件 type 语义。此前引擎只认 tag 名(input/textarea), checkbox/radio/password
 * 会被当成可编辑文本 —— 布局给出 180px 宽的文本框、键盘往 value 里塞字符。 */
enum {
    HN_IN_NONE = 0,       /* 非 input/textarea */
    HN_IN_TEXT,           /* <input>(缺省) */
    HN_IN_TEXTAREA,       /* <textarea> */
    HN_IN_PASSWORD,       /* <input type=password>(绘制时掩码, 值保持原文) */
    HN_IN_CHECKBOX,       /* <input type=checkbox>(checked 属性表达状态) */
    HN_IN_RADIO           /* <input type=radio>(checked 属性表达状态) */
};
/* 输入控件的 type 语义(HN_IN_*); 非 input/textarea 返回 HN_IN_NONE */
int  hn_node_input_kind(hn_node *n);
/* checkbox/radio 是否选中(checked 属性; 值为 "false"/"0" 视为未选)。
 * 运行时切换状态用 hn_node_set_attr(n, "checked", "" / "false")。 */
int  hn_node_is_checked(hn_node *n);
/* 控件是否禁用(disabled 属性; 值为 "false"/"0" 视为启用)。
 * 禁用的控件不参与表单提交、不可聚焦, 且命中 :disabled 伪类。 */
int  hn_node_is_disabled(hn_node *n);
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
/* 内省: 读取节点当前 opacity(动画插值后的观测值) */
void hn_node_debug_opacity(hn_node *n, float *out);
/* 内省: 动画声明(name/时长/循环次数) */
void hn_node_debug_anim(hn_node *n, const char **name, float *ms, int *iter);
/* 内省: 当前旋转角度(2D rotate) */
void hn_node_debug_rotate(hn_node *n, float *deg);
void hn_node_debug_color(hn_node *n, hn_color *out);
/* 节点自身的行内片段数; 文本节点为其被摆放的段数 */
int hn_node_run_count(hn_node *n);
/* 取第 i 个片段: 写出绝对 x/基线/宽 与所属行盒顶/高; 越界返回 0 */
int hn_node_run_at(hn_node *n, int i, float *x, float *baseline, float *w,
                   float *y_top, float *h);
/* 取第 i 个片段在该节点文本中的字节区间(排查空白折叠/换行时必须看到内容:
   只有坐标和宽度时无法判断某个 run 到底是词还是一串空格)。越界返回 0。 */
int hn_node_run_range(hn_node *n, int i, size_t *begin, size_t *end);
/* 取节点原始文本(文本节点用); 非文本节点返回 NULL。
   配合 hn_node_run_range 可打印某个片段的实际内容。 */
const char *hn_node_text(hn_node *n, size_t *len_out);

/* ---- 事件(引擎只提供事件数据与冒泡路径; 管道由运行时实现, 消费者可是 JS/hx-*) ---- */

typedef enum {
    HN_EV_NONE = 0,
    HN_EV_CLICK, HN_EV_DBLCLICK,
    HN_EV_MOUSEDOWN, HN_EV_MOUSEUP,
    HN_EV_MOUSEMOVE, HN_EV_MOUSEENTER, HN_EV_MOUSELEAVE,
    HN_EV_KEYDOWN, HN_EV_KEYUP,
    HN_EV_FOCUS, HN_EV_BLUR,
    HN_EV_INPUT, HN_EV_CHANGE,
    HN_EV_SUBMIT, HN_EV_SCROLL,
    HN_EV_HOVER,                    /* hx 语义: 进入与离开共用 */
    /* 小程序式生命周期: 运行时在表面状态变化时派发, 页面用 hn-lifecycle
       声明关心哪些(空格分隔, 如 "launch show hide destroy")。
       target 为该表面声明的 id, 缺省文档根。 */
    HN_EV_LAUNCH,                   /* 表面首次可见 */
    HN_EV_SHOW,                     /* 变为可见(含从隐藏恢复) */
    HN_EV_HIDE,                     /* 变为不可见 */
    HN_EV_DESTROY                   /* 表面即将销毁(可做收尾) */
} hn_event_kind;

/* 修饰键位掩码 */
enum { HN_MOD_SHIFT = 1, HN_MOD_CTRL = 2, HN_MOD_ALT = 4, HN_MOD_META = 8 };

typedef struct hn_event {
    hn_event_kind kind;
    hn_node *target;      /* 命中的最深元素(冒泡起点) */
    float    x, y;        /* 视口坐标(鼠标/滚动类事件) */
    int      key_code;    /* 平台无关键码(见 hn_key_*) */
    const char *key;      /* 键名: "a" / "Enter" / "Escape" / "ArrowUp" … */
    unsigned modifiers;   /* HN_MOD_* 掩码 */
    int      repeat;      /* 按键重复(长按) */
    float    delta;       /* 滚轮增量(SCROLL) */
    const char *text;     /* INPUT 事件的新值(可为 NULL) */
} hn_event;

/* 常用键码(平台无关; 运行时负责把系统键码映射到这里) */
enum {
    HN_KEY_NONE = 0, HN_KEY_ENTER = 1, HN_KEY_ESC = 2, HN_KEY_TAB = 3,
    HN_KEY_BACKSPACE = 4, HN_KEY_DELETE = 5,
    HN_KEY_LEFT = 10, HN_KEY_RIGHT = 11, HN_KEY_UP = 12, HN_KEY_DOWN = 13,
    HN_KEY_HOME = 14, HN_KEY_END = 15, HN_KEY_PAGEUP = 16, HN_KEY_PAGEDOWN = 17,
    HN_KEY_SPACE = 20
};

/* 事件名(与 DOM 命名对齐: "click" / "keydown" …); 未知返回 "unknown" */
const char *hn_event_name(hn_event_kind k);

/* 冒泡路径: 从 target 自身向上直至根; idx 从 0 起。
 * 只返回**有 id 的元素**(id 是运行时与 JS 的寻址键), 无 id 的祖先被跳过。
 * 越界返回 NULL。 */
hn_node *hn_event_path_at(hn_node *target, int idx);

/* 冒泡路径长度(有 id 的元素个数) */
int hn_event_path_len(hn_node *target);

/* 从 n(含自身)向上找最近的 input/textarea(供事件派发定位编辑目标) */
hn_node *hn_node_ancestor_input(hn_node *n);

/* 生命周期事件名("launch" / "show" / "hide" / "destroy"); 非生命周期返回 NULL */
const char *hn_lifecycle_name(hn_event_kind k);
/* hn-lifecycle 声明里是否包含该事件 */
int  hn_lifecycle_wants(const char *decl, hn_event_kind k);

/* ---- 交互: 滚动 ---- */
/* 点位下最深的 overflow 容器(可滚动元素), 无则 NULL */
hn_node *hn_context_scrollable_at(hn_context *c, float x, float y);
/* 滚动该元素(自动钳制到内容范围); 位置变化返回 1(调用方 repaint) */
int  hn_node_scroll_by(hn_node *n, float dx, float dy);
/* 读取当前滚动偏移 */
void hn_node_scroll_get(hn_node *n, float *x, float *y);
/* 可滚动上限(content 高 - 盒高); 不可滚动写 0 */
void hn_node_scroll_range(hn_node *n, float *max_x, float *max_y);
typedef enum {
    HN_SWAP_INNER = 0,  /* 替换目标元素内容(默认, innerHTML) */
    HN_SWAP_OUTER,      /* 替换目标元素自身(outerHTML) */
    HN_SWAP_APPEND,     /* 追加到目标末尾 */
    HN_SWAP_PREPEND     /* 插到目标开头 */
} hn_swap_mode;

/* ---- DOM 变更(供脚本层与宿主使用; 引擎只做结构操作, 不关心调用者是谁) ---- */
/* 设置/新建属性(值为空串表示布尔属性); 成功返回 1 */
int  hn_node_set_attr(hn_node *n, const char *name, const char *value);
/* 取元素文本内容(深度优先拼接子文本节点, 写入 out, 返回写入长度) */
size_t hn_node_text_content(hn_node *n, char *out, size_t cap);
/* 设置元素文本内容(清空子节点后写入一个文本节点); 成功返回 1 */
int  hn_node_set_text_content(hn_node *n, const char *utf8, size_t len);
/* 创建元素(挂到 n 下, 返回新节点; 供片段拼装) */
hn_node *hn_node_append_element(hn_node *parent, const char *tag);
/* 移除子节点 */
int  hn_node_remove_child(hn_node *parent, hn_node *child);
/* 把 HTML 片段解析为若干节点并插入到 parent(append/prepend/inner 语义) */
int  hn_node_insert_html(hn_node *parent, const char *html, size_t len, hn_swap_mode mode);

/* 枚举带指定属性的元素: idx 从 0 起, 命中返回节点 */
hn_node *hn_doc_find_attr(hn_doc *doc, const char *attr, int idx);
/* 只保留最后 keep_last 个元素子节点(流式视图的环形缓冲); 返回移除数量 */
int  hn_node_trim_children(hn_node *n, int keep_last);
/* 结构自检: 检测链表环、自引用、父子指针不一致。返回问题数量(0=健康)。
   node_budget 为本次遍历允许访问的最大节点数(防环导致的死循环)。 */
int  hn_doc_validate(hn_doc *doc, int node_budget);

/* 命中自洽自检: 每个带 id 且尺寸非零的元素, 其盒中心点必须命中到自身或
   某个带 id 的后代。返回问题数量(0=健康), 并把检查过的元素数写入 *checked。
   这是**纯引擎语义**(命中测试与平台无关), 因此放在引擎里而不是某个运行时
   —— 此前只有 Windows 的 hnwin --probe 做了这项检查, macOS/Linux 一直没验,
   而它恰恰是最容易因布局改动而回归的一环。 */
int  hn_context_verify_hits(hn_context *c, int *checked);

/* ---- 交互: 命中测试 ---- */
const char *hn_context_hit_test(hn_context *c, float x, float y); /* 元素 id 或 NULL */
hn_node    *hn_context_hit_node(hn_context *c, float x, float y); /* 最深层元素节点 */

/* ---- 交互: 属性读取(htmx 风格) ---- */
/* 元素标签名(小写), 文本节点或无标签返回 NULL */
const char *hn_node_tag(hn_node *n);
/* 返回元素属性值, 不存在返回 NULL */
const char *hn_node_attr(hn_node *n, const char *name);
/* 属性遍历(供宿主/工具内省: 排查"为什么这个 hx-get / hn-mesh 没生效"时,
   必须能看到元素到底带了哪些声明, 而不是只有 id 和 class)。
   idx 越界返回 0; 命中时写出属性名与值(值可为空串 = 布尔属性)。 */
int hn_node_attr_at(hn_node *n, int idx, const char **name, const char **value);
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



/* 更新元素文本(替换其子节点为单个文本节点), 返回 1 表示找到目标 */
int hn_doc_set_text(hn_doc *doc, const char *element_id, const char *utf8_text);

/* 把 HTML 片段换入目标元素; 运行时随后调用 hn_context_layout 生效 */
int hn_doc_swap(hn_doc *doc, const char *target_id, hn_swap_mode mode,
                const char *html_src, size_t len);

/* 热渲染: 用新 HTML 整体替换文档内容(样式表保留), 随后 layout 生效。
 * 这是 agent 驱动 UI 的核心原语: 现场生成 HTML → 推入常驻窗口。 */
void hn_context_render(hn_context *c, const char *html_src, size_t len);

/* ---- 软件光栅后端的图像解码 ----
 * 平台原生后端可以调系统 API(AppKit / GDI+)解图; 软件光栅后端(hnsoft)的
 * 承诺是"只要有 C 编译器就能构建", 因此自带 PNG 解码, 让 Linux / Windows /
 * 无头环境也能真正显示图片, 而不是退化成占位图案。
 * 返回 RGBA8(自上而下, 4 字节/像素), 调用方 free; 不支持该格式返回 NULL。
 */
typedef struct hn_png_decoder hn_png_decoder;
unsigned char *hn_png_decode(const unsigned char *data, size_t n, int *w, int *h);

#ifdef __cplusplus
}
#endif
#endif /* HN_H */

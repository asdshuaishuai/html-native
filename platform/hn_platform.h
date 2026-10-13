/* hn_platform.h — 平台抽象 API(工程化分层的关键契约)
 *
 * 分层架构(2026-10 定案):
 *
 *   ┌────────────────────────────────────────────────┐
 *   │ 应用: hn_daemon / hn_cli / 用户程序             │  tools/hn_daemon.c
 *   ├────────────────────────────────────────────────┤
 *   │ 公共运行时 hn_rt: 文档生命周期/htmx/sys://桥/   │  rt/hn_rt.c
 *   │ 资产/文本后端/几何记忆 —— 单源, 三平台共享      │  (本次新增)
 *   ├────────────────────────────────────────────────┤
 *   │ 引擎: 解析→级联→布局→显示列表(纯计算)           │  Sources/CHtmlNative
 *   ├────────────────────────────────────────────────┤
 *   │ 平台 API(本头文件): 窗口/事件/时钟/像素上屏     │  各平台一个实现
 *   │   macOS:  platform/hnp_macos.c  (objc_msgSend) │
 *   │   Linux:  platform/hnp_linux.c  (dlopen X11)   │
 *   │   Windows: platform/hnp_win.c   (Win32)        │
 *   │   无头:   platform/hnp_headless.c (零 GUI)     │
 *   └────────────────────────────────────────────────┘
 *
 * **平台实现只需要这十几个函数** —— 其余全部由 hn_rt 提供。
 * 这让"新增一个平台"的工作量从"重写 900-2000 行壳"降到
 * "实现 ~300 行平台 API"。
 *
 * 约定对齐 hn.h: 成功返回 1 失败返回 0; 坐标 CSS px、原点左上、y 向下
 * (与显示列表/命中测试同系; 平台内部自行翻转, 如 macOS AppKit 原点在左下)。
 * 单线程使用(引擎非线程安全)。
 */
#ifndef HN_PLATFORM_H
#define HN_PLATFORM_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* 不透明句柄(平台实现自行定义内部结构) */
typedef struct hnp_window hnp_window;

/* ---------------- 生命周期 ---------------- */

/* 初始化平台(装动态库/注册类等)。返回 1 成功; 无 GUI 环境返回 0
   (调用方可降级 headless)。argc/argv 传平台需要的(如 X 的 -display),
   可传 0/NULL。 */
int  hnp_init(int argc, char **argv);

/* 关停平台(释放显示连接等)。所有窗口销毁后调用一次。 */
void hnp_shutdown(void);

/* 平台名("macos"/"linux"/"windows"/"headless"), 供诊断输出 */
const char *hnp_name(void);

/* ---------------- 窗口 ---------------- */

typedef struct {
    const char *title;      /* UTF-8; NULL = "hn"。
                               borderless=1 时窗口没有原生标题栏, 此字段退化为
                               任务栏/Dock(悬停缩略图)/调试输出的显示名 —— macOS
                               accessory 策略下不进 Dock, 仅作诊断可见性。 */
    int width, height;      /* 内容区尺寸(CSS px) */
    int transparent;        /* 1 = 逐像素 alpha 窗口 */
    int resizable;          /* 1 = 用户可拖边界 */

    /* ---------------- 桌宠形态(无背景 + 无边框 + 悬浮桌面) ----------------
       五个字段是"不以程序应用方式存在"的窗口侧开关: 桌宠/桌面小组件。
       平台不支持时优雅忽略(不需要报错; 声明是意图, 物化看平台)。 */

    int always_on_top;      /* 窗口层级: 0=普通(normal); 1=浮动(floating, 悬浮于
                              普通窗口之上); 2=桌面(desktop, 壁纸之上、桌面图标与
                              普通窗口之下 —— 桌面小组件语义)。
                              macOS: 1→setLevel: NSFloatingWindowLevel(NSWindow.h:194
                              = kCGFloatingWindowLevel = 3, CGWindowLevel.h:68);
                              2→kCGDesktopWindowLevel(CGWindowLevel.h:64)。
                              Linux: 1→_NET_WM_STATE_ABOVE、2→_NET_WM_STATE_BELOW
                              (EWMH 最近似; 精确"壁纸之上"层为各 WM 私有协议,
                              降级为 BELOW)。 */
    int no_shadow;          /* 关闭窗口投影(setHasShadow:NO)。窗口整块是内容时
                              投影会勾出一个不存在的"窗口矩形", 桌宠必须关。
                              Linux: 无标准对应(compositor 私有, EWMH 未定义),
                              优雅忽略 —— 平台能力矩阵的已知差异点。 */
    int click_through;      /* 初始鼠标穿透(事件落到下层窗口)。
                              macOS: setIgnoresMouseEvents:YES —— 幽灵模式初始态;
                              Linux: XShape 输入形状置空(libXext 缺席时降级);
                              运行期切换见 hnp_window_set_click_through。 */
    int join_all_spaces;    /* 跟随所有空间(含全屏空间旁边)。
                              macOS: setCollectionBehavior: 含 CanJoinAllSpaces|
                              Stationary|IgnoresCycle(CG/NSWindow.h 精确值见 hnp_macos.c)。
                              Linux: _NET_WM_STATE_STICKY(所有桌面可见)。 */
    int borderless;         /* 无标题栏无边框(styleMask 不含 Titled/Closable/…)。
                              frame/bounds 即内容区, 无标题条可点。
                              Linux: _MOTIF_WM_HINTS decorations=0。 */

    /* ---- 位置记忆(几何持久化的开窗入参, 见 hnp_window_origin) ----
       左上原点屏幕坐标(与引擎坐标同口径); 双双 = HNP_WINDOW_POS_AUTO 时
       自动居中(缺省行为不变)。桌宠/数字人"关了再开回到原位"由此落地:
       hn_app 开窗前读 ~/.html-native/geometry/<doc-stem>.json 填进来。 */
    int x, y;               /* -32768 = 自动居中(哨兵; 屏幕坐标到不了那里) */
} hnp_window_desc;

/* hnp_window_desc.x/y 的"自动居中"哨兵(几何记忆未命中时的缺省值) */
#define HNP_WINDOW_POS_AUTO (-32768)

/* 创建窗口。返回 NULL 失败(原因一行 stderr)。 */
hnp_window *hnp_window_open(const hnp_window_desc *desc);

/* 关闭并释放窗口。 */
void hnp_window_close(hnp_window *w);

/* 内容区尺寸变化后同步(平台已由 hnp_pump 报告 resize 并由 hn_rt 重排,
   这里只是把新尺寸同步进平台视图)。 */
void hnp_window_resize(hnp_window *w, int new_w, int new_h);

/* ---------------- 像素上屏 ---------------- */

/* 把一张 BGRA 预乘位图送进窗口内容区。
   w/h 是位图尺寸(与窗口内容区一致), stride = 每行字节数(通常 w*4)。
   平台内部做翻转/格式转换(如 Win32 DIB 是 BGRA 顶到底, macOS CGImage
   预乘 ARGB32 little-endian —— 数据布局一致, 原点方向不同)。 */
void hnp_window_blit(hnp_window *win, const unsigned char *bgra_pre,
                     int w, int h, size_t stride);

/* 请求重绘(下一次 pump 时收到 HNP_EV_EXPOSE)。 */
void hnp_window_invalidate(hnp_window *w);

/* 标题(热更新后清单变化同步到窗口) */
void hnp_window_set_title(hnp_window *w, const char *utf8);

/* 运行期切换鼠标穿透(桌宠"幽灵模式": 穿透时点击落到桌面/下层窗口,
   再切回来重新接收事件)。macOS 走 setIgnoresMouseEvents:, Linux 走
   XShape 输入形状(libXext 运行期 dlopen, 缺席时优雅降级); 其余平台
   空实现(声明照常通过, 物化看平台)。幂等; NULL 安全。 */
void hnp_window_set_click_through(hnp_window *w, int on);

/* 读窗口外框原点(屏幕坐标, 左上原点, y 向下 —— 与引擎/显示列表同口径)。
   几何记忆的采样口: hn_app 主循环 ~2s 轮询一次, 变了就 debounce 落盘
   ~/.html-native/geometry/<doc-stem>.json, 下次开窗经 hnp_window_desc.x/y
   摆回原位。返回 1 = x/y 已写出; 0 = 平台拿不到(headless 无对应; macOS
   读 NSWindow frame, Linux 用 XTranslateCoordinates 读根窗口坐标) 返回 0
   让调用方整体跳过记忆。NULL 安全。 */
int hnp_window_origin(hnp_window *w, int *x, int *y);

/* 读窗口所在屏的设备像素比(backing scale; Retina = 2.0)。
   HiDPI 渲染的查询口: 真窗壳用它把 hn_rt 的渲染缩放设成平台倍率
   (hn_rt_set_scale), 位图按 (逻辑尺寸 × scale) 出图、文字按 ×scale 走
   FreeType 光栅, blit 时平台按"视图尺寸=逻辑点、位图=物理像素"呈现 ——
   Retina 上 1:1 物理像素, 文字/图形明显更锐。缺省(非 Retina/拿不到/
   headless/Linux)返回 1.0 —— 全链路行为与 1x 完全一致。NULL 安全。
   上限(caps)由调用方管(hn_app cap 2.0; 环境钮 HN_RENDER_SCALE 可覆盖)。 */
float hnp_window_scale(hnp_window *w);

/* ---------------- 事件 ---------------- */

typedef enum {
    HNP_EV_NONE = 0,
    HNP_EV_CLOSE,           /* 用户请求关闭(红叉/⌘W/WM_DELETE) */
    HNP_EV_EXPOSE,          /* 需要重绘(把缓存位图再 blit 一次) */
    HNP_EV_RESIZE,          /* 尺寸变化: w/h 有效 */
    HNP_EV_POINTER_MOVE,    /* 悬停/拖动: x/y 有效(内容区坐标) */
    HNP_EV_BUTTON_DOWN,     /* 按下: x/y/button(0=左) */
    HNP_EV_BUTTON_UP,       /* 抬起: x/y/button */
    HNP_EV_SCROLL,          /* 滚轮: dx/dy(像素, 向下为正) */
    HNP_EV_KEY_DOWN,        /* 按键: key(枚举)/utf8(可打印字符) */
    HNP_EV_KEY_UP,
    HNP_EV_TEXT,            /* 输入法文本: utf8(编辑路径, 平台可不给) */
} hnp_event_kind;

/* 语义键(平台实现把本机键码映射到这一套) */
typedef enum {
    HNP_KEY_NONE = 0,
    HNP_KEY_ESCAPE, HNP_KEY_RETURN, HNP_KEY_TAB, HNP_KEY_BACKSPACE, HNP_KEY_DELETE,
    HNP_KEY_LEFT, HNP_KEY_RIGHT, HNP_KEY_UP, HNP_KEY_DOWN,
    HNP_KEY_HOME, HNP_KEY_END, HNP_KEY_PAGE_UP, HNP_KEY_PAGE_DOWN,
    HNP_KEY_F1, HNP_KEY_F2, HNP_KEY_F3, HNP_KEY_F4, HNP_KEY_F5, HNP_KEY_F6,
    HNP_KEY_F7, HNP_KEY_F8, HNP_KEY_F9, HNP_KEY_F10, HNP_KEY_F11, HNP_KEY_F12,
    HNP_KEY_PRINTABLE,      /* utf8 里带字符 */
} hnp_key;

typedef struct {
    hnp_event_kind kind;
    int x, y;               /* 内容区坐标(CSS px, 左上原点) */
    int button;             /* 0=左 1=中 2=右 */
    float dx, dy;           /* 滚轮增量(像素) */
    hnp_key key;            /* 语义键 */
    const char *utf8;       /* 可打印字符(指向平台内部缓冲, pump 内有效) */
    unsigned mods;          /* 位: 1=shift 2=ctrl 4=alt 8=cmd/meta */
} hnp_event;

/* 泵一个事件。timeout_ms:
   <0  = 阻塞直到有事件
   0   = 立即返回(无事件返回 kind=NONE)
   >0  = 最多等这么久(帧循环节拍用)
   返回 1 且 ev->kind != NONE 表示取到事件; 0 表示超时/无事件。
   **必须**在收到 EXPOSE 时把缓存位图再 blit(窗口最小化恢复后内容丢失),
   收到 RESIZE 时通知 hn_rt 重排。 */
int  hnp_pump(hnp_event *ev, int timeout_ms);

/* ---------------- 时钟 ---------------- */

/* 毫秒级单调时钟(动画/轮询的时基; 平台实现选各自的高精度源) */
unsigned long hnp_now_ms(void);

/* ---------------- 光标 ---------------- */

typedef enum { HNP_CURSOR_DEFAULT = 0, HNP_CURSOR_POINTER, HNP_CURSOR_TEXT } hnp_cursor;

void hnp_set_cursor(hnp_window *w, hnp_cursor c);

/* ---------------- 媒体(追加在 hn_platform.h 光标段之后) ---------------- */

typedef struct hnp_media hnp_media;   /* 不透明句柄(平台实现自定义内部结构) */

/* 本平台是否带媒体实现。headless/Linux/Windows 返回 0(ABI 留位不实现)。
   macOS 返回 1(不探测 AVFoundation 是否可 dlopen —— dlopen 失败在
   open 的 err 里如实报告, 不在 supported 里猜)。 */
int hnp_media_supported(void);

/* 打开媒体(url 为本地路径或 file:// URL; 引擎侧保证先于任何其他调用)。
   同步返回句柄; 元数据在后台解析, 就绪用 hnp_media_state 查询。
   失败返回 NULL 并写一行原因进 err(err/err_cap 可为 NULL/0 = 不关心)。
   纯音频文件合法(没有视频轨 → frame 恒返回 0)。 */
hnp_media *hnp_media_open(const char *url, char *err, size_t err_cap);

/* 关闭并释放(幂等; NULL 安全)。之后句柄不得再使用。 */
void hnp_media_close(hnp_media *m);

/* 播放控制。play 返回 1 = 已接受; seek 钳制到 [0, duration](已知时)。 */
int  hnp_media_play(hnp_media *m);
void hnp_media_pause(hnp_media *m);
int  hnp_media_seek(hnp_media *m, double sec);

/* 音量 0..1(越界钳制)与静音。视频音轨与纯音频同此入口。 */
void hnp_media_set_volume(hnp_media *m, float vol);
void hnp_media_set_muted(hnp_media *m, int muted);

/* 元数据。duration 未知(直播流)时写 -1 并返回 0。 */
int  hnp_media_duration(hnp_media *m, double *sec_out);

/* 当前播放位置(秒; 单调性由平台保证: pause 后恒定, seek 落位后更新)。 */
int  hnp_media_position(hnp_media *m, double *sec_out);

/* 就绪与结束: *ready=1 表示元数据+可起播(时长已知);
   *ended=1 表示位置到达 duration 平台侧确认(实现可直接用 position>=duration,
   实测 AVPlayer 的 rate 在播完时不可靠)。两者可同时为 1。 */
int  hnp_media_state(hnp_media *m, int *ready, int *ended);

/* 拉当前视频帧(RGBA8 非预乘, 原点左上, 顶上到底下)。
   调用方给缓冲与容量 cap(字节); *w/*h 写实际帧尺寸; *pts_sec 写该帧
   显示时间戳(秒)。返回: 1=新帧已写入(内容与上次不同), 0=无新帧(内容未变,
   *w/*h 仍有效), -1=无视频轨或失败。
   cap 不足以容纳整帧时写 -1 并返回 -1(调用方按 meta 重新分配后重试)。
   实现注意(实机验证): 实际产出的 CVPixelBuffer 是 '420v' planar,
   请求 BGRA 的 attributes 会被忽略 —— 用 GetBaseAddressOfPlane(0/1)+
   GetBytesPerRowOfPlane 取 Y/UV 平面, 平台内做 NV12→RGBA 转换。 */
int  hnp_media_frame(hnp_media *m, unsigned char *rgba, size_t cap,
                     int *w, int *h, double *pts_sec);

/* ---------------- 原生视频图层(GPU 合成直通) ----------------
 * 真窗模式下 <video> 的第二呈现路径: 引擎帧不再走 "NV12/BGRA 解码拷贝 →
 * RGBA 转换 → HN_CMD_BITMAP 软件光栅", 而是 AVPlayer 解码后由系统合成器
 * (WindowServer)把 AVPlayerLayer 直接合成进窗口 —— 像素不过 CPU。
 * 语义(与窗口表面同参照系):
 *   - player_id: 媒体 src(与 hnp_media_open 收到的 url 同串; 平台侧以
 *     open/close 维护 src→player 注册表, attach 时查到才有图层);
 *   - x/y/fw/fh: 元素盒的**逻辑点坐标**(CSS px, 左上原点, y 向下 ——
 *     与引擎显示列表/命中测试同口径), 平台内部自行翻到 AppKit 坐标;
 *   - 图层添加为 contentView layer 的 sublayer, 层级在位图图层之上。
 * 返回: attach 1 = 图层已挂(调用方此后每帧调 frame 同步盒位置/尺寸,
 *   会话关闭/元素消失时调 detach); 0 = 本平台不支持或该 src 没有 player
 *   (调用方保持 CPU 逐帧路径, 优雅降级)。frame/detach 幂等, NULL 安全。
 * 非 macOS 平台为空实现(声明照常通过, 物化看平台 —— 与媒体 ABI 同一口径)。 */
int  hnp_video_layer_attach(hnp_window *w, const char *player_id,
                            float x, float y, float fw, float fh);
/* 每帧同步元素盒(布局/滚动/resize 后跟随)。未 attach 过的 id 静默忽略。 */
void hnp_video_layer_frame(hnp_window *w, const char *player_id,
                           float x, float y, float fw, float fh);
/* 摘除图层(元素关闭/热重载换文档/回退 CPU 模式)。 */
void hnp_video_layer_detach(hnp_window *w, const char *player_id);

#ifdef __cplusplus
}
#endif

#endif /* HN_PLATFORM_H */

/* ---------------- GPU 渲染后端(契约: 平台出原语, 引擎出画法) ----------------
 *
 * 分层铁律的 GPU 版: 本头只定义**原语表**(设备/纹理/四边形/裁剪/读回),
 * 显示列表 → 原语的全部翻译逻辑在引擎侧(Sources/CHtmlNative/hn_gpu.c),
 * 平台实现(hnp_gpu_metal.c 等)只提供设备原语。语义与 hnsoft_render
 * 完全一致: 同一显示列表 + 同一背景色 + 同一缩放 → 等价 RGBA 位图;
 * 覆盖不了的指令集, 后端用 supported 掩码声明, 呼出方整帧回退 hnsoft。
 *
 * 各平台实现矩阵(docs/platforms.md 为准):
 *   macOS  = hnp_gpu_metal.c(Metal, 运行期 dlopen, MSL 源码运行期编译)
 *   Linux  = 路线图(GL/mesa, 同一原语表)
 *   headless = 无( supported()==0, 呼出方回退 hnsoft )
 *   Windows = 路线图(D3D11)
 *
 * 呼出口径: 引擎每帧检查显示列表是否只含后端声明支持的指令;
 * 是 → 走 GPU(readback 或直呈), 否 → hnsoft。回退是逐帧的、静默的。 */
typedef struct hnp_gpu_host hnp_gpu_host;

/* 原语句柄(纹理) */
typedef unsigned int hnp_gpu_tex;

/* 后端声明自己支持的显示列表指令位集(HN_CMD_* 逐位; 从 hn.h 数值导入,
 * 平台头为免反向依赖以数值注释对照 —— 呼出方负责按位与检查) */
typedef struct {
    unsigned supported_cmds;        /* 位集: 1<<kind(按 hn_cmd.kind 数值) */
    const char *name;               /* "metal" / "gl" / ... 诊断用 */
} hnp_gpu_caps;

struct hnp_gpu_host {
    hnp_gpu_caps caps;
    /* 设备生命周期(进程一次; init 失败 supported 视为 0) */
    int  (*init)(void);
    void (*shutdown)(void);
    /* 帧开始: 目标尺寸(物理 px)与背景色(引擎口径 RGBA8 非预乘);
       返回 0 = 本帧不可用(资源紧张等), 呼出方整帧回退 hnsoft */
    int  (*begin)(int w, int h, unsigned bg);
    /* 帧结束: 光栅完成。offscreen 口径下 readback 把 BGRA 预乘位图
       拷进调用方缓冲(行距 = w*4); 直呈模式(后续)readback 可为空 */
    int  (*end_readback)(unsigned char *out_bgra, int w, int h);
    /* 纹理: RGBA8 非预乘上传; 句柄 0 = 无效。tex_update 就地更新(同尺寸) */
    hnp_gpu_tex (*tex_create)(int w, int h, const unsigned char *rgba);
    void (*tex_update)(hnp_gpu_tex t, const unsigned char *rgba);
    void (*tex_free)(hnp_gpu_tex t);
    /* 四边形批量绘制: 顶点按 4 浮点一顶点 [x,y,u,v](物理 px, y 向下),
       四顶点一四边形, 顶点沿**周界序**(v0→v1→v2→v3 沿四边形边走,
       顺/逆时针皆可 —— QUAD 是 3D 投影后的任意凸四边形(见 hn.h 的
       qx/qy), RECT/图片退化为矩形时即 TL→TR→BR→BL)。周界序是 GPU 端
       三角形拆分 (0,1,2)+(0,2,3) 绕序一致的前提(见 hnp_gpu_metal.c 的
       IBO 注)。每四边形恒定色 + 可选纹理(uv 有效时); 混合 = 标准反预乘
       src-over(src-one / dst-one-minus-src-alpha —— 预乘口径, 与 readback
       的预乘约定一致; 引擎侧 hn_gpu_render_scaled 出的非预乘 RGBA 与
       hnsoft 之间的换算见 hn_gpu.c 头注释) */
    int  (*draw_quads)(const float *verts, int quad_count,
                       hnp_gpu_tex tex, unsigned rgba);
    /* 矩形裁剪栈(轴对齐; 圆角/任意路径裁剪 v1 不在此层 —— 呼出方
       遇到这类 CLIP 整帧回退 hnsoft) */
    void (*clip_push)(int x, int y, int w, int h);
    void (*clip_pop)(void);
};

/* 平台装配: 返回平台 GPU 原语表; 无 GPU 后端返回 NULL(呼出方回退 hnsoft)。
 * 呼出方(引擎经宿主注入, 与 hn_media_host/hn_gpu_host 同款全局槽)。
 * 函数名取 hnp_gpu_backend 而非类型同名的 hnp_gpu_host: C 的普通标识符
 * 单一命名空间里 typedef(hnp_gpu_host)与同名函数不能共存(定稿契约的
 * 笔误 —— 原样声明在 clang 下即 "redefinition of 'hnp_gpu_host' as
 * different kind of symbol", 任何包含本头的文件都编译不过), 修名不改结构。 */
const hnp_gpu_host *hnp_gpu_backend(void);

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
    const char *title;      /* UTF-8; NULL = "hn" */
    int width, height;      /* 内容区尺寸(CSS px) */
    int transparent;        /* 1 = 逐像素 alpha 窗口 */
    int resizable;          /* 1 = 用户可拖边界 */
} hnp_window_desc;

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

#ifdef __cplusplus
}
#endif

#endif /* HN_PLATFORM_H */

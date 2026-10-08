/* hnweb.h — 跨平台 "引擎背书 webview" 门面(平台壳只实现这一头文件)。分层: 壳管窗口表面/事件收集/位图上屏, 门面管引擎生命周期/像素/命中/帧步进。约定对齐 hn.h: 成功返回 1 失败返回 0; 坐标一律 CSS px、原点左上、y 向下(与显示列表/命中测试同系); 单线程使用(引擎非线程安全)。DPI 沿用 hnwin.c:20 的已知取舍: CSS px = 物理 px, 不读 Xft.dpi(高分屏未缩放)。 */
#ifndef HNWEB_H
#define HNWEB_H

#include <stddef.h>
#include "hn.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hnweb_win hnweb_win;  /* 不透明句柄, 对齐 hn.h:25-28 的 opaque 风格 */

/* 绘制后端切换点(与 tools/hnwin.c:39-44 同一宏手法): #ifndef HNWEB_RENDER /
 * #define HNWEB_RENDER(dl,w,h,bg) hnsoft_render((dl),(w),(h),(bg)) / #endif
 * —— 带 -DHN_USE_CAIRO 的构建可重定义为 hncairo_render(同签名, hn_cairo.h:36)。 */
#ifndef HNWEB_RENDER
#ifdef HN_USE_CAIRO
#include "hn_cairo.h"
#define HNWEB_RENDER(dl, w, h, bg) hncairo_render((dl), (w), (h), (bg))
#else
#include "hnsoft.h"
#define HNWEB_RENDER(dl, w, h, bg) hnsoft_render((dl), (w), (h), (bg))
#endif
#endif

typedef struct hnweb_opts {
    const char *title;     /* NULL=取 manifest hn-title */
    float w, h;            /* 0=取 manifest hn-window */
    int transparent;       /* 1=逐像素 alpha 无边框背板(镜像 hn-transparent) */
    int resizable;         /* 0=固定尺寸表面 */
    const char *store_id;  /* 本地 KV 隔离 id, NULL="default" */
} hnweb_opts;  /* 字段与 hn.h:190-208 清单及 hnwin.c:721-731 消费方式对齐 */

int hnweb_open(const hnweb_opts *opts, const char *html, size_t html_len,
               const char *css, size_t css_len, hnweb_win **out);
/* 开窗口并装载文档: 解析→清单→context 组装→建窗→首帧(相当于 HNEngine.open
 * 首开分支 + hnwin main 的 669-814 段)。html/css 内部复制, 调用方随后可 free。
 * 平台后端不可用返回 0(带 stderr 原因, 对齐 hnwebview.c:599-609 的失败口径)。 */

int hnweb_update(hnweb_win *w, const char *html, size_t len);
/* 热更新: 整文档换入、窗口与焦点保持(HNEngine.swift:308-320 语义, 引擎侧走
 * hn_context_render, hn.h:477)。返回 1 表示已接受; 上屏由 hnweb_frame 完成 */

int hnweb_resize(hnweb_win *w, int width_px, int height_px);
/* 重排: 尺寸变了才动作→重 layout→重建背板缓冲→重渲染上屏(WM_SIZE 语义,
 * hnwin.c:434-455)。返回 1 表示实际发生了变化 */

void hnweb_mouse_move(hnweb_win *w, float x, float y);
/* 喂 :hover + cursor(hn_context_set_hover / hn_node_cursor, hn.h:265/271) */

void hnweb_mouse_leave(hnweb_win *w);
/* 清 hover(X11 LeaveNotify ↔ hnwin.c:490-497 WM_MOUSELEAVE) */

void hnweb_mouse_down(hnweb_win *w, float x, float y, int button);
/* 命中测试 + 焦点 + hx 派发(hnwin.c on_click 404-422); 产生的事件经
 * hnweb_poll 交还 */

void hnweb_scroll(hnweb_win *w, float x, float y, float delta_y);
/* delta_y 一格=48px(hnwin.c:508), 内部 hn_context_scrollable_at/
 * hn_node_scroll_by(hn.h:387-389) */

void hnweb_key(hnweb_win *w, int key, const char *utf8);
/* key 用 hn.h:358-364 的 HN_KEY_*; Enter→表单载体提交(hnwin.c:538-550 语义) */

typedef struct hnweb_event {
    hn_event_kind kind;   /* 复用 hn.h:323-340 */
    float x, y;           /* 点击/滚动坐标(窗口客户区, y 向下) */
    const char *target_id; /* 命中最深元素 id(NULL=无 id); 借引擎 arena, 本轮 poll 内有效 */
    hn_node *target;      /* 命中最深节点(宿主读属性/续派发用) */
    int key;              /* HN_KEY_* */
} hnweb_event;
/* —— 这就是"点击坐标回传": x/y 给几何, target_id 给语义, 二者都来自引擎命中测试 */

int hnweb_poll(hnweb_win *w, hnweb_event *out);
/* 非阻塞取一条应用级事件; 有则返回 1。供需要与其它消息源共存的嵌入者
 * (等价轮询模型) */

int hnweb_frame(hnweb_win *w, float dt_ms);
/* 推一帧: hn_context_anim_tick(dt_ms, hn.h:261) + 到期 hx-poll + 脏/动画时
 * layout→显示列表→HNWEB_RENDER→上屏。返回 1=本次有上屏。平台主循环按 ~16ms 调
 * (等价 hnwin WM_TIMER 分支 518-536) */

void hnweb_run(hnweb_win *w);
/* 委托式主循环: 阻塞等事件 + 16ms 帧步进, 窗口关闭即返回(GetMessage 等价,
 * hnwin.c:828-832)。要与其它事件源共存就别用这个, 改 hnweb_poll+hnweb_frame */

void hnweb_close(hnweb_win *w);
/* 销毁窗口与引擎上下文; 所有权对齐 hn.h:235-239 "谁 set 谁放手"——交给窗口的
 * doc/sheet 由窗口统一释放 */

hn_context *hnweb_context(hnweb_win *w);
/* 逃生舱: 引擎上下文只读用法(镜像 HNWebKitHost.swift:27-32 的
 * engineContextOrNil 判空哲学) */

const unsigned char *hnweb_pixels(hnweb_win *w, int *w_out, int *h_out);
/* 最近上屏帧 RGBA8: 非预乘、自上而下(与 hnsoft 输出同格式) */

int hnweb_shot(hnweb_win *w, const char *png_path);
/* 当前帧编码 PNG(hnsoft_encode_png)——CI 的 --shot/--probe 自检出口
 * (hnwin.c:614-665 等价物); dlopen 失败的无头环境也必须能走 probe/shot */

#ifdef __cplusplus
}
#endif
#endif /* HNWEB_H */
/* Linux 壳 = hnweb_linux.c: dlopen("libX11.so.6") + 最小 Xlib 声明,
 * 实现 22 个符号; 未来 Wayland/其它平台各写一个实现文件, 门面零改动 */

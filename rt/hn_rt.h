/* hn_rt.h — 公共运行时层(三平台共享的应用逻辑, 单源)
 *
 * 分层见 platform/hn_platform.h 的架构图。hn_rt 持有引擎上下文 +
 * 渲染位图 + htmx 执行 + sys:// 桥 + 本地 KV, 平台壳只管:
 *   1. 实现 hn_platform.h 的十几个函数
 *   2. 主循环: hnp_pump → hn_rt_dispatch → hn_rt_frame
 *
 * 一个平台壳因此可以缩到 ~150 行(创建窗口/泵事件/转发),
 * 其余全部复用 —— "新增平台"从重写 900-2000 行降到实现平台 API。
 */
#ifndef HN_RT_H
#define HN_RT_H

#include <stddef.h>
#include "hn.h"
#include "hnsoft.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hn_rt hn_rt;

/* ---------------- 创建/销毁 ---------------- */

typedef struct {
    const char *id;         /* 应用 id(sys:// store 隔离键) */
    const char *html;       /* 文档(UTF-8) */
    size_t html_len;
    const char *css;        /* 附加样式表; NULL 无 */
    size_t css_len;
    int w, h;               /* 初始尺寸(CSS px) */
} hn_rt_desc;

/* 创建一个应用实例(解析+级联+布局+光栅一次完成)。
   cwd 决定资产(图片/<link>)的解析基准 —— 调用方先 chdir 到文档目录。 */
hn_rt *hn_rt_open(const hn_rt_desc *d);
void hn_rt_close(hn_rt *rt);

/* ---------------- 文档生命周期 ---------------- */

/* 热更新: 整体替换文档(样式表保留), 布局+光栅一次完成。 */
int hn_rt_render(hn_rt *rt, const char *html, size_t len);

/* 触发布局+光栅(内容尺寸变化/hover 变化后)。 */
int hn_rt_reflow(hn_rt *rt);

/* 首次渲染的位图(预乘 BGRA)与尺寸 —— 平台壳拿去 hnp_window_blit。
   返回值在下次 reflow 前有效。 */
const unsigned char *hn_rt_pixels(hn_rt *rt);
int hn_rt_width(hn_rt *rt);
int hn_rt_height(hn_rt *rt);

/* ---------------- 帧步进(动画/轮询) ---------------- */

/* 推进动画时钟并按需重渲染; 轮询到期触发 hx。
   返回下一次需要调用的间隔(毫秒; 0 = 立即; -1 = 无事可睡)。
   若重渲染了, *repaint 置 1(平台壳据此 blit)。 */
int hn_rt_frame(hn_rt *rt, float dt_ms, int *repaint);

/* ---------------- 输入事件(平台壳转发) ---------------- */

/* 指针移动(悬停伪类/光标)。返回 1 表示重渲染过(需 blit)。 */
int hn_rt_pointer_move(hn_rt *rt, float x, float y);

/* 按钮(点击命中 + hx 派发 + focus)。返回 1 表示重渲染过。 */
int hn_rt_button(hn_rt *rt, int down, float x, float y, int button);

/* 滚轮(命中元素滚动)。返回 1 表示重渲染过。 */
int hn_rt_scroll(hn_rt *rt, float dx, float dy, float x, float y);

/* 按键。返回 1 表示重渲染过。Esc 由平台壳自己处理(窗口关闭)。 */
int hn_rt_key(hn_rt *rt, int down, int key, const char *utf8);

/* ---------------- 内省(agent 感知) ---------------- */

/* DOM 概要(缩进树, malloc 串; 调用方 free) */
char *hn_rt_dom(hn_rt *rt, int max_depth);

/* 元素文本(malloc; free) */
char *hn_rt_text(hn_rt *rt, const char *element_id);

/* 当前文档(供宿主/工具直接驱动 hn_rt_eval 的桥 —— eval 按 doc 反查
   上下文, 媒体桥需要它)。返回值在下次 hn_rt_render 前有效。 */
hn_doc *hn_rt_doc(hn_rt *rt);

/* 显示列表指令数 */
int hn_rt_cmd_count(hn_rt *rt);

/* 合成事件(agent 驱动入口): 对 target 派发 kind("click" 等)。
   返回 1 表示发生 hx 执行或状态变化。 */
int hn_rt_synthetic(hn_rt *rt, const char *kind, const char *target);

/* 渲染为 PNG 落盘(--shot / daemon shot op) */
int hn_rt_shot(hn_rt *rt, const char *path);

/* JS 运行时(QuickJS): eval 一段脚本, 返回结果字符串(malloc; 调用方 free)。
   状态跨调用持久(同一 store_id 的 JS 上下文保留)。
   不依赖 hn_rt 结构 —— daemon 可以直接调(裸引擎也有 JS 能力)。 */
char *hn_rt_eval(const char *js, const char *scope_key, hn_doc *doc);

/* ---------------- 媒体宿主(壳注入) ---------------- */

/* 全局媒体宿主槽(仿资产后端 g_assets_be 的单源注入): 壳在 hn_rt_open
   之前调用, 之后创建的每个 context 都拿到它。NULL = 媒体禁用(缺省)。 */
void hn_rt_set_media(const hn_media_host *host);

/* ---------------- sys:// 桥(单源) ---------------- */

/* sys:// → HTML 片段(malloc; free)。非 sys:// 或未知路由返回 NULL。
   这是统一层: 三个壳与 daemon 都调它, 不再各自实现。 */
char *hn_rt_sys_fragment(const char *url, const char *form_body, const char *store_id);

/* 本地 KV(与 sys://store 同一份文件; 独立入口供 daemon 直接用) */
int hn_rt_store_get(const char *store_id, const char *key, char *out, size_t cap);
int hn_rt_store_set(const char *store_id, const char *key, const char *val);

#ifdef __cplusplus
}
#endif

#endif /* HN_RT_H */

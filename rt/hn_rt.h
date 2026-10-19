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
    const char *path;       /* 文档源文件路径(相对 cwd; NULL/"" = 内联文档)。
                               非 NULL 时启用**项目内导航**: a[href] 指向 cwd 下
                               存在的文件 → 同窗换装(hn_rt_navigate); 页面/资产
                               全部相对 cwd(壳已 chdir 到项目根)。 */
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

/* ---------------- HiDPI(真窗 Retina 2x 渲染) ----------------
 * 尺寸语义保持两套: hn_rt_width/height 恒为**逻辑 px**(CSS px, 布局/
 * 命中/窗口表面都用它); 位图物理尺寸 = 逻辑 × scale(blit/像素统计用
 * bitmap_* 访问器)。缺省 scale = 1.0 —— 不设置时全链路与旧版逐位一致,
 * 离屏/--shot 探针的 1x 确定性锚由此保住。 */

/* 设置设备渲染缩放并**立即重渲染**(位图按新尺寸重出, 调用方直接 blit)。
 * 文本随 scale 走 FreeType 光栅(真 2x 字形, 非 1x 拉伸)。scale < 0.5
 * 按 1.0 处理; 建议取值 = hnp_window_scale()(cap 由调用方管)。
 * 平台壳在开窗后调用一次(hn_app: HN_RENDER_SCALE 环境钮覆盖);
 * 热重载新建 rt 后需重新设置(doc 路径已在 hn_app 内处理)。 */
void hn_rt_set_scale(hn_rt *rt, float scale);

/* 当前渲染缩放(缺省 1.0; 诊断/日志用) */
float hn_rt_scale(const hn_rt *rt);

/* 位图物理尺寸(hn_rt_pixels 的宽高 = 逻辑 × scale, 与 blit stride 配套) */
int hn_rt_bitmap_width(const hn_rt *rt);
int hn_rt_bitmap_height(const hn_rt *rt);

/* ---------------- 项目内导航(多 html 项目模式) ----------------
 * hn_rt_desc.path 非 NULL 时, a[href] 点到 cwd 下真实存在的文件 → 同窗换装
 * (换 DOM/重布局/滚动归零, 窗口与进程不动); http(s):// 等仍交系统打开。
 * 换装前的页面进栈, hn_rt_back 回退。壳(hn_app)把入口页路径传进来,
 * 项目根 = 壳 chdir 的目录。 */

/* 导航到 url(相对 cwd; 指向存在的普通文件)。1 = 已换装;
   0 = 非项目模式/外部地址/文件不存在(调用方回落系统打开)。 */
int hn_rt_navigate(hn_rt *rt, const char *url);

/* 页面栈回退。1 = 已回到上一页; 0 = 栈空(已在入口页)。 */
int hn_rt_back(hn_rt *rt);

/* ---------------- 桌面画板呈现(画刷铺色 reveal) ----------------
 * 形态语义(用户裁决): **画上去 = 逐像素透明窗的呈现** —— 未铺区域
 * alpha=0 直接透出桌面, 内容像颜料画上去; 不透明窗(标题栏/无边框不透明)
 * 会先出现一块"画板"再画内容(用户指出的错误形态), 缺省只走平台淡入。
 * hn-reveal=none 一律硬出现。
 *
 * "整个桌面是一块画板, 内容被画上去": 首帧就绪后 ~480ms 内, 内容沿对角
 * 轴以羽化笔锋(波纹摆动的软边)逐步铺开 —— rt 层单源实现, 三平台壳共用。
 * 不透明窗: 未铺到区域显示"画纸"(画布底色), 内容像颜料一样盖上来;
 * 透明窗: 逐像素 alpha 缩放, 窗口真被画到桌面上。GPU 直呈帧无 CPU 位图,
 * 该帧自动退化为平台淡入(如实口径, 非直呈路径不受影响)。
 * 探针确定性: 纯遮罩函数 hn_rt_paint_mask_cov 无状态可单测; apply 只在
 * 真窗主循环被壳调用 —— --shot/--probe 离屏路径恒不经过, 像素锚不破。 */

/* 画刷覆盖(0..1): u ∈ [0,1] 铺色进度, (fx,fy) 物理像素坐标(中心),
   w/h 位图尺寸。u≤0 恒 0, u≥1 恒 1; 单调; 羽化带 ~20% 对角跨 + 波纹笔锋。 */
float hn_rt_paint_mask_cov(float u, float fx, float fy, float w, float h);

/* 生命周期(壳驱动; now_ms = 壳的单调毫秒钟):
   begin   首帧就绪时调一次(幂等);
   active  在途返回 1(壳据此强制重绘 + 16ms 节拍);
   apply   把当前进度的遮罩盖到 rt 的 BGRA 帧上(blit 前调用;
           每帧都基于**新光栅**的帧, 不会叠加污染)。 */
void hn_rt_paint_reveal_begin(hn_rt *rt, unsigned long now_ms);
int  hn_rt_paint_reveal_active(const hn_rt *rt, unsigned long now_ms);
void hn_rt_paint_reveal_apply(hn_rt *rt, unsigned long now_ms);

/* ---------------- 帧步进(动画/轮询) ---------------- */

/* 推进动画时钟并按需重渲染; 轮询到期触发 hx。
   返回下一次需要调用的间隔(毫秒; 0 = 立即; -1 = 无事可睡)。
   若重渲染了, *repaint 置 1(平台壳据此 blit)。 */
int hn_rt_frame(hn_rt *rt, float dt_ms, int *repaint);

/* 绘制挂起/冲刷(爆发输入的成本合并, app 壳事件排干专用):
   defer(1) 后输入路径只标脏不渲染; flush 把脏帧一次画掉(返回 1 = 真画过,
   调用方据此 blit)。不进入 defer 的调用方(探针直驱输入函数)行为逐位不变。 */
void hn_rt_paint_defer(hn_rt *rt, int on);
int  hn_rt_paint_flush(hn_rt *rt);

/* ---------------- 输入事件(平台壳转发) ---------------- */

/* 指针移动(悬停伪类/光标)。返回 1 表示重渲染过(需 blit)。 */
int hn_rt_pointer_move(hn_rt *rt, float x, float y);

/* 按钮(点击命中 + hx 派发 + focus)。返回 1 表示重渲染过。 */
int hn_rt_button(hn_rt *rt, int down, float x, float y, int button);

/* 滚轮(命中元素滚动)。返回 1 表示重渲染过。 */
int hn_rt_scroll(hn_rt *rt, float dx, float dy, float x, float y);

/* 按键。返回 1 表示重渲染过。Esc 由平台壳自己处理(窗口关闭)。
   key 取 platform/hn_platform.h 的 hnp_key 值 —— 本层不 include 平台头
   (平台无关是 hn_rt 的存在理由), 故在此镜像一份 HN_KT_*(Key Type)
   枚举, **值必须与 hnp_key 逐项一致**; tools/macos_deep_probe.sh 现场
   编译一个同时包含两个头的对账器做编译期断言(漂移即探针红)。
   支持: 可打印字符/IME 文本插入(若 utf8 非空)、RETURN(hx 提交; textarea
   内插入换行)、TAB(焦点轮换)、BACKSPACE/DELETE(按 caret 删除)、
   LEFT/RIGHT/HOME/END(按字符/caret 移动)、上下/翻页(预留, 当前 no-op)。 */
enum {
    HN_KT_NONE = 0,
    HN_KT_ESCAPE, HN_KT_RETURN, HN_KT_TAB, HN_KT_BACKSPACE, HN_KT_DELETE,
    HN_KT_LEFT, HN_KT_RIGHT, HN_KT_UP, HN_KT_DOWN,
    HN_KT_HOME, HN_KT_END, HN_KT_PAGE_UP, HN_KT_PAGE_DOWN,
    HN_KT_F1, HN_KT_F2, HN_KT_F3, HN_KT_F4, HN_KT_F5, HN_KT_F6,
    HN_KT_F7, HN_KT_F8, HN_KT_F9, HN_KT_F10, HN_KT_F11, HN_KT_F12,
    HN_KT_PRINTABLE,
    HN_KT_INSERT, HN_KT_MENU, HN_KT_CAPS_LOCK, HN_KT_NUM_LOCK,
    HN_KT_SCROLL_LOCK, HN_KT_PAUSE, HN_KT_SPACE, HN_KT_ENTER_NUMPAD
};
int hn_rt_key(hn_rt *rt, int down, int key, const char *utf8);

/* ---------------- IME 上屏(平台 IME 事件转发) ----------------
 * 最小实现口径("预编辑串直插"): 组字期间把"基础值 + 预编辑串"写进焦点
 * 控件的 value(caret 落在预编辑串末尾), commit 时用"基础值 + 提交串"
 * 一次性替换; 取消(preedit 空串)恢复基础值。基础值 = 组字开始那一刻的
 * 已上屏值, 引擎内部记录。返回 1 表示重渲染过。
 * 平台不支持 IME 时不调用(0 成本); NULL 安全。 */

/* 预编辑串变化(组字中每个中间态; preedit 空串 = 组字结束/取消)。
   cursor 为预编辑串内光标(字节偏移; 诊断/未来高亮用, 最小实现忽略)。 */
int hn_rt_ime_preedit(hn_rt *rt, const char *preedit, int cursor);

/* 提交串(选字上屏): 基础值 + text 落进 value, 清预编辑态。 */
int hn_rt_ime_commit(hn_rt *rt, const char *text);

/* 焦点控件盒(逻辑 px, 左上原点; 供平台把 IME 候选窗/输入法图标摆到光标处)。
   焦点非可编辑控件/无焦点时返回 0(调用方按"无焦点盒"降级)。 */
int hn_rt_focus_box(hn_rt *rt, int *x, int *y, int *w, int *h);

/* ---------------- 编辑命令(快捷键的引擎侧实现) ----------------
 * 平台壳把 ⌘C/⌘V/⌘X/⌘A 映射到这里; 剪贴板读写复用本文件既有 sys://
 * 剪贴板后端(macOS NSPasteboard / Linux 留位), 不新增平台依赖。
 * text 仅 PASTE 需要: NULL = 用本层剪贴板读(clip_get); 非 NULL = 调用方
 * 已从平台剪贴板拿到文本(hnp_clipboard_get), 优先用它。返回 1 = 文档变过
 * (调用方 blit)。无焦点控件时静默 0。 */
enum { HN_EDIT_COPY = 0, HN_EDIT_CUT = 1, HN_EDIT_PASTE = 2, HN_EDIT_SELECT_ALL = 3 };
int hn_rt_edit(hn_rt *rt, int op, const char *text);

/* ---------------- 内省(agent 感知) ---------------- */

/* DOM 概要(缩进树, malloc 串; 调用方 free) */
char *hn_rt_dom(hn_rt *rt, int max_depth);

/* 元素文本(malloc; free) */
char *hn_rt_text(hn_rt *rt, const char *element_id);

/* 当前文档(供宿主/工具直接驱动 hn_rt_eval 的桥 —— eval 按 doc 反查
   上下文, 媒体桥需要它)。返回值在下次 hn_rt_render 前有效。 */
hn_doc *hn_rt_doc(hn_rt *rt);

/* 文档的应用清单(meta 声明; 引擎侧 hn_doc_manifest 的同源封装)。
   平台壳物化窗口形态用: 透明/无边框/层级/跨空间/投影/鼠标穿透 ——
   桌宠形态的字段在这里被 hn_app 读走填进 hnp_window_desc。
   rt 为 NULL 时写出缺省清单(不崩)。 */
void hn_rt_manifest(const hn_rt *rt, hn_manifest *out);

/* 引擎上下文(探针/宿主内省用: hn_context_display_list / hn_canvas_count
   等引擎 API 的入口)。所有权仍归 hn_rt —— 只读访问, 勿销毁。 */
hn_context *hn_rt_context(hn_rt *rt);

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

/* ---------------- GPU 渲染后端宿主(壳注入; 缺省 CPU/hnsoft) ----------------
 * 全局槽(与 hn_rt_set_media 同款): 壳在 hn_rt_open 之前调用。表类型 =
 * platform/hn_platform.h 的 hnp_gpu_host —— 本头不 include 平台头, 以
 * const void* 进(类型安全由调用点保证: hn_app.c 同时含两个头)。NULL =
 * 缺省, 一切走 hnsoft(行为零漂移)。开关与环境钮(HN_GPU=metal)在壳侧。
 * 渲染分支与回退口径见 hn_rt.c 的 rt_raster / hn_rt_shot。 */
void hn_rt_set_gpu_host(const void *host);

/* GPU 窗口直呈(v2, 壳注入; 不透明指针 = CAMetalLayer*): 非空时 rt_raster
   优先走 hn_gpu_render_present(成功帧 0 readback/0 CPU 位图, rt->presented
   置位); 任何失败整帧落回既有 readback/CPU 路径。截图(shot)自动临时摘层
   走像素路径。透明窗口/无 GPU 时壳不应注入。 */
void hn_rt_set_gpu_present_layer(hn_rt *rt, void *ca_metal_layer);
/* 最近一帧是否经 GPU 直呈出帧(壳据此跳过 CPU blit; 层未注入恒 0) */
int  hn_rt_presented(const hn_rt *rt);

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

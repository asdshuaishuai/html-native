/* hnsoft.h — 软件光栅化后端(纯 C, 把显示列表渲染成 RGBA 像素)
 *
 * 为什么存在: Linux 桌面图形栈碎片化、Windows Direct2D 重、引入 Skia 会
 * 终结"零外部依赖"。而本引擎的显示列表指令集刻意收敛(14 种, 见 hn.h 的
 * HN_CMD_*) —— 一个纯 C 软件光栅器即可覆盖, 让任何有 C 编译器的平台
 * 都能把 HTML 渲染成像素。
 *
 * 文本: FreeType(CJK 完善, Linux/Android/Chrome 的事实标准)。
 * 编译期定义 HN_NO_TEXT 可省略文本(布局不受影响)。
 */
#ifndef HNSOFT_H
#define HNSOFT_H

#include "hn.h"

/* 渲染显示列表到 RGBA8 缓冲(预乘前直通, stride = width*4, 自上而下)。
 * bg 为画布底色(#RRGGBBAA 打包, 与 hn_color 一致)。
 * 返回 malloc 的缓冲(调用方 free), 失败返回 NULL。
 * 【两变体并存】本函数是**一次性**口径: 每次调用新分配一张全页位图,
 * 适合 shot/CLI/探针等单发渲染; 帧循环宿主(动画/媒体页)请用下面的
 * hnsoft_render_reuse —— 两变体走同一条渲染核心, 像素输出逐位一致。 */
unsigned char *hnsoft_render(const hn_display_list *dl, int width, int height,
                             hn_color bg);

/* 同上, 但带**绘制缩放** scale(HiDPI/Retina 2x 的光栅入口):
 *   - width/height 是**位图物理尺寸**(调用方给 逻辑尺寸×scale);
 *   - 显示列表坐标仍是逻辑 px, paint 时统一 ×scale 落到物理像素;
 *   - 文本字号 ×scale 后走 FreeType 光栅 —— 真 2x 字形, 不是 1x 拉伸
 *     (这是高清化的意义所在; 字形缓存按 (码点, 物理字号) 分键);
 *   - 3D 投影面片(QUAD)/网格(MESH)/canvas 账本坐标同乘 scale, 语义是
 *     整幅均匀放大, 与"布局恒逻辑 px"的正交关系不破。
 * scale = 1.0 时与 hnsoft_render 逐位一致(既有探针的确定性锚不动)。
 * hnsoft_render 即本函数 scale=1.0 的薄封装。 */
unsigned char *hnsoft_render_scaled(const hn_display_list *dl, int width, int height,
                                    hn_color bg, float scale);

/* 【复用变体】渲染进**调用方持有的缓冲**(slot/cap 由调用方持有, 旧签名
 * 全部不动 —— 向后兼容靠并存, 不靠改约):
 *   - *slot == NULL / *slot_cap == 0 起步; 需求字节(w*h*4)超过 *slot_cap
 *     时 realloc 扩容(**只增不减** —— 窗口缩小不缩容, Resize 抖动不反复
 *     分配), 渲染进该缓冲并返回它;
 *   - 每帧先全像素覆盖底色再跑同一条指令循环(与 hnsoft_render 同一条
 *     渲染核心), 像素输出**逐位一致** —— 差别只在缓冲归谁分配;
 *   - 失败返回 NULL 且 *slot/*slot_cap 保持原值(旧缓冲原地保留, 上一帧
 *     位图仍可 blit)。
 * 契约: 缓冲生命周期归调用方(用完 free(*slot)); hnsoft 不缓存不释放。
 * 动画/媒体宿主每帧调用即可消除"每帧 malloc+free 一张全页位图"的
 * 分配器 churn(920x520 下 ~2MB × 帧率)。 */
unsigned char *hnsoft_render_reuse(const hn_display_list *dl, int width, int height,
                                   hn_color bg, unsigned char **slot, int *slot_cap);

/* hnsoft_render_reuse 的带 scale 版(HiDPI 2x 主实现; rt 运行时帧循环走
 * 这里) —— 缓冲复用语义与上完全一致, 像素与 hnsoft_render_scaled 逐位
 * 一致(scale=1.0 时两变体互换逐位相等)。 */
unsigned char *hnsoft_render_reuse_scaled(const hn_display_list *dl, int width, int height,
                                          hn_color bg, float scale,
                                          unsigned char **slot, int *slot_cap);

/* 渲染热路径的堆分配累计计数(HN_ALLOC_TRACE churn 实证口径): 计 fb 位图
 * 缓冲(一次性变体每帧新分配 / 复用变体仅扩容)、超长多边形堆缓冲、新字形
 * 位图三类事件。帧循环走复用缓冲后稳态增量应 ≈0/帧 —— rt 层每 300 帧差分
 * 打印一行, perf_probe 据此断言"分配差分有界"。 */
long hnsoft_heap_allocs(void);

/* 把 RGBA 缓冲编码为 PNG(存储型 deflate, 无压缩但格式合法)。
 * 返回 malloc 的 PNG 数据, *out_len 写出长度; 失败返回 NULL。 */
unsigned char *hnsoft_encode_png(const unsigned char *rgba, int w, int h,
                                 size_t *out_len);

/* 供测试: FreeType 字体是否加载成功(0=未加载) */
int hnsoft_font_loaded(void);

/* ---------------- 桌面画板呈现: 画刷覆盖(纯函数, 壳共用) ----------------
 * "窗口被画上桌面"的遮罩核心: u ∈ [0,1] 铺色进度, (fx,fy) = 像素中心
 * 物理坐标, w/h = 位图尺寸。对角轴铺开 + 20% 羽化笔锋 + 波纹摆边。
 * u≤0 恒 0, u≥1 恒 1, 对 u 单调 —— 壳(hn_app/hnwin)每帧对位图逐像素
 * 乘它(不透明窗向画纸底色混合, 透明窗缩 alpha); rt 层包了生命周期
 * (hn_rt_paint_reveal_*), 本函数无状态可单测。 */
float hnsoft_paint_mask_cov(float u, float fx, float fy, float w, float h);

/* 布局期文本测量后端(对应 hn.h 的 hn_text_backend 回调):
   运行时把它注入 hn_context_layout, 引擎即按真实字形宽度排布,
   而不是等宽估算(CJK 按字节数×字号×0.55 会显著偏宽, 窄容器里
   文字被错误地逐字换行 —— macOS 上 CoreText 后端不存在此问题)。
   FreeType 不可用/未加载时按引擎同口径回退。 */
float hnsoft_measure(const hn_font_desc *font, const char *utf8, size_t len);
void  hnsoft_metrics(const hn_font_desc *font, float *ascent, float *descent,
                     float *leading);

#endif

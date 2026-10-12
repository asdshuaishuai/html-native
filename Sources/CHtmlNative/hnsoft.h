/* hnsoft.h — 软件光栅化后端(纯 C, 把显示列表渲染成 RGBA 像素)
 *
 * 为什么存在: Linux 桌面图形栈碎片化、Windows Direct2D 重、引入 Skia 会
 * 终结"零外部依赖"。而本引擎的显示列表只有 5 种指令 —— 一个几百行的
 * 软件光栅器即可覆盖, 让任何有 C 编译器的平台都能把 HTML 渲染成像素。
 *
 * 文本: FreeType(CJK 完善, Linux/Android/Chrome 的事实标准)。
 * 编译期定义 HN_NO_TEXT 可省略文本(布局不受影响)。
 */
#ifndef HNSOFT_H
#define HNSOFT_H

#include "hn.h"

/* 渲染显示列表到 RGBA8 缓冲(预乘前直通, stride = width*4, 自上而下)。
 * bg 为画布底色(#RRGGBBAA 打包, 与 hn_color 一致)。
 * 返回 malloc 的缓冲(调用方 free), 失败返回 NULL。 */
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

/* 把 RGBA 缓冲编码为 PNG(存储型 deflate, 无压缩但格式合法)。
 * 返回 malloc 的 PNG 数据, *out_len 写出长度; 失败返回 NULL。 */
unsigned char *hnsoft_encode_png(const unsigned char *rgba, int w, int h,
                                 size_t *out_len);

/* 供测试: FreeType 字体是否加载成功(0=未加载) */
int hnsoft_font_loaded(void);

/* 布局期文本测量后端(对应 hn.h 的 hn_text_backend 回调):
   运行时把它注入 hn_context_layout, 引擎即按真实字形宽度排布,
   而不是等宽估算(CJK 按字节数×字号×0.55 会显著偏宽, 窄容器里
   文字被错误地逐字换行 —— macOS 上 CoreText 后端不存在此问题)。
   FreeType 不可用/未加载时按引擎同口径回退。 */
float hnsoft_measure(const hn_font_desc *font, const char *utf8, size_t len);
void  hnsoft_metrics(const hn_font_desc *font, float *ascent, float *descent,
                     float *leading);

#endif

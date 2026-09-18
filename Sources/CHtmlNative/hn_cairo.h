#ifndef HNCAIRO_H
#define HNCAIRO_H

/* hn_cairo.h — cairo 绘制后端(跨平台, 一个实现喂所有平台)
 *
 * 为什么要有它: 此前绘制层有**两份独立实现** —— macOS 走 CoreGraphics
 * (HNPainter.swift), Windows/无头走 hnsoft.c 手写软件光栅。同一个显示列表
 * 被翻译两遍, 于是两边的圆角、渐变、阴影、裁剪精度各自漂移; 每加一个平台
 * 就再写第三遍。Windows 就是这个代价的标本。
 *
 * 现在换成 cairo: 一份实现, 通过 `hncairo_render` 的**同签名**入口供所有
 * 平台调用(Windows 的 hnwin.c、无头的 hncore.c、PNG 输出全都只调这一个函数),
 * 换链接目标即可, 调用方一行不改。
 *
 * 为什么选 cairo: 对照显示列表的 8 类指令逐个实测过 ——
 *   圆角矩形 + 实色/渐变填充   ✓ 生产级路径与渐变插值
 *   任意路径裁剪(含圆角)       ✓ hnsoft 只能做轴对齐矩形交叠
 *   多边形 even-odd/非零环绕   ✓ 原生填充规则
 *   四边形(透视投影面片)       ✓ 任意路径
 *   抗锯齿                     ✓ 真实覆盖率(hnsoft 是 0.5px 手算)
 *   UV 贴图(Live2D 类网格)     ✓ clip + 仿射逐三角形
 *   阴影模糊                   ⚠ cairo 无内置模糊, 用降采样放大近似(见下)
 * 文本仍走 FreeType 光栅化 —— 不是为了省事, 而是因为**测量与绘制必须同源**:
 * 布局期用 FreeType 的字形 advance 排布, 绘制若换成另一套字体后端, 字形就会
 * 落在排好的行盒之外。
 *
 * 编译: 需要 cairo。文本需要 FreeType(HN_NO_TEXT 可省)。
 */

#include "hn.h"

/* 渲染显示列表到 RGBA8 缓冲(预乘前直通, stride = width*4, 自上而下)。
 * bg 为画布底色(#RRGGBBAA 打包, 与 hn_color 一致)。
 * 返回 malloc 的缓冲(调用方 free), 失败返回 NULL。
 * 与 hnsoft_render 同签名 —— 二者可互换链接。 */
unsigned char *hncairo_render(const hn_display_list *dl, int width, int height,
                              hn_color bg);

/* 把 RGBA 缓冲编码为 PNG(cairo 自带, 真压缩)。
 * 返回 malloc 的 PNG 数据, *out_len 写出长度; 失败返回 NULL。 */
unsigned char *hncairo_encode_png(const unsigned char *rgba, int w, int h,
                                  size_t *out_len);

/* 供测试: FreeType 字体是否加载成功(0=未加载) */
int hncairo_font_loaded(void);

/* 布局期文本测量后端(与 hnsoft_measure 同口径, 见 hnsoft.h)。
   两者返回相同宽度 —— 这一点是断言过的, 否则换了绘制后端排版就会变。 */
float hncairo_measure(const hn_font_desc *font, const char *utf8, size_t len);
void  hncairo_metrics(const hn_font_desc *font, float *ascent, float *descent,
                      float *leading);

#endif

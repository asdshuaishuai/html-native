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

/* 把 RGBA 缓冲编码为 PNG(存储型 deflate, 无压缩但格式合法)。
 * 返回 malloc 的 PNG 数据, *out_len 写出长度; 失败返回 NULL。 */
unsigned char *hnsoft_encode_png(const unsigned char *rgba, int w, int h,
                                 size_t *out_len);

/* 供测试: FreeType 字体是否加载成功(0=未加载) */
int hnsoft_font_loaded(void);

#endif

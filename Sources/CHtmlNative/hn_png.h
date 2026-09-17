/* hn_png.h — PNG 解码(纯 C, 零依赖)
 *
 * 软件光栅后端需要能真正显示图片, 否则 Linux / Windows / 无头环境里
 * 所有图像都只能退化成占位图案 —— 跨平台能力在"有图"场景下就不完整。
 */
#ifndef HN_PNG_H
#define HN_PNG_H

#include <stddef.h>

/* 解码 PNG, 输出 RGBA8(自上而下, 4 字节/像素)。调用方 free。
   失败返回 NULL。不支持的格式也返回 NULL(调用方应退到占位图案)。 */
unsigned char *hn_png_decode(const unsigned char *data, size_t n, int *w, int *h);

#endif /* HN_PNG_H */

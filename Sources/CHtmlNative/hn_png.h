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

/* 裸 deflate 解压(RFC 1951, 无 zlib 头尾)。调用方 free。
   消费方: PNG 的 IDAT(PNG 侧自行跳过 2 字节 zlib 头后传入)与
   dotLottie 的 ZIP 条目(method=8, 本就是裸码流)—— 同一份实现。
   失败/空输出返回 NULL。 */
unsigned char *hn_inflate(const unsigned char *src, size_t n, size_t *out_len);

#endif /* HN_PNG_H */

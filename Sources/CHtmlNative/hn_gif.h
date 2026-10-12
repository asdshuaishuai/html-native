/* hn_gif.h — GIF87a/89a 动图解码(纯 C, 零依赖)
 *
 * 与 hn_png.h 同一层级: 引擎核心的格式解码器。软件光栅后端(hnsoft)
 * 与显示列表的 BITMAP 指令(帧序列像素)都从这里取帧。
 *
 * 产物是"合成好的整幅 RGBA8 帧序列 + 每帧 delay", 播放取帧是纯时间函数
 * (hn_gif_frame_index) —— 循环回绕/截图跳时钟/倒退取帧都成立, 与
 * Lottie / 网格变形共用同一帧时钟口径。
 */
#ifndef HN_GIF_H
#define HN_GIF_H

#include <stddef.h>

typedef struct hn_gif_s hn_gif;

/* 解码 GIF 字节流。成功返回对象(调用方 hn_gif_free), 失败返回 NULL
   (此时调用方应退到静态图/占位路径)。 */
hn_gif *hn_gif_decode(const unsigned char *data, size_t n);
void    hn_gif_free(hn_gif *g);

int hn_gif_width(const hn_gif *g);
int hn_gif_height(const hn_gif *g);
int hn_gif_frame_count(const hn_gif *g);
/* NETSCAPE 循环次数(0 = 无限; 无该扩展也按 0 无限处理) */
int hn_gif_loop_count(const hn_gif *g);
/* 第 i 帧的显示时长(ms; 源为 0 时已按 100ms 归一) */
float hn_gif_frame_delay_ms(const hn_gif *g, int i);
/* 第 i 帧的透明色索引(-1 = 该帧无透明) */
int hn_gif_frame_transparent(const hn_gif *g, int i);
/* 第 i 帧合成后的整幅 RGBA8(自上而下, 非预乘; 生命周期 = hn_gif 对象) */
const unsigned char *hn_gif_frame_rgba(const hn_gif *g, int i);
/* 时钟 → 当前帧号(纯函数: 按 delay 累计取模/单次播完钳末帧) */
int hn_gif_frame_index(const hn_gif *g, float clock_ms);

#endif /* HN_GIF_H */

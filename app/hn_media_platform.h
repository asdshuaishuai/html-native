/* hn_media_platform.h — app 侧媒体宿主装配(实现在 hn_media_platform.c)
 *
 * hn_app 只认这几个入口: 宿主表(交给 hn_rt_set_media)、宿主类型名(诊断)、
 * 装载后的绑定(合成替身的时长表 + 环境钮)、静音开关与状态跟踪。
 *
 * 两条宿主路径(取舍逻辑见 hn_media_platform.c 头注释):
 *   "platform" 平台真宿主: macOS=AVFoundation(真解码/真出声);
 *   "synth"    合成替身  : 梯度帧 + 假时钟(与 tools/hncore.c 的 g_synth_media
 *              语义同源), 平台无媒体实现(hnp_media_supported()==0, 如
 *              headless/Linux 目标)时兜底 —— 元素照常播(确定性假帧)。
 */
#ifndef HN_MEDIA_PLATFORM_H
#define HN_MEDIA_PLATFORM_H

#include "hn_rt.h"

/* 装配用宿主表(恒非 NULL —— 引擎侧 "NULL = 媒体禁用" 的降级留给未装配的壳)。
   hn_rt_set_media() 之前调用一次; 之后创建的每个 context 都拿到它。 */
const hn_media_host *hn_media_platform_host(void);

/* 宿主类型名: "platform"(平台真宿主) / "synth"(合成替身)。诊断/探针输出用。 */
const char *hn_media_platform_kind(void);

/* 文档装载后调用(hn_rt_open 之后; 热重载对新的 rt 再调):
   - 合成替身: 记下当前 doc/context, 供 src→data-duration 惰性解析;
   - 环境钮: HN_MEDIA_MUTE=1 全静音 / HN_MEDIA_UNMUTE=1|all|id[,id…] 取消静音。
   幂等; rt 为 NULL 时 no-op。 */
void hn_media_platform_bind(hn_rt *rt);

/* 全媒体静音翻转(真窗内 'm' 键; 手工验收"能听到声音"的开关)。
   首次依据元素 muted 属性同步内部状态, 之后逐次翻转。
   返回 1 = 已处理(m 键消费); 0 = 文档无媒体元素(交还按键给引擎)。 */
int hn_media_platform_toggle_mute(hn_rt *rt);

/* 媒体会话状态行(每个 video/audio 一行, stdout):
     [media] id=v state=PLAYING t=1.234 dur=4.000
   验收脚本的观测点(真窗里 t 前进 = 视频真的在动)。 */
void hn_media_platform_trace(hn_rt *rt);

#endif /* HN_MEDIA_PLATFORM_H */

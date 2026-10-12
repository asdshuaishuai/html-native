/* hn_media_synth.h — 合成媒体宿主(**单一来源**, 实现在 hn_media_synth.c)
 *
 * 收敛自两份重复实现: 原 tools/hncore.c 的 static g_synth_media 与原
 * app/hn_media_platform.c 的 synth 段 —— 同一语义两份代码, 改一处漏一处。
 * 自此 hncore(paint/render 探针)与 app 壳(hn_media_platform 装配)共用
 * 这一文件, 行为口径只有一份账。
 *
 * 是什么: 真宿主(AVFoundation)的**规约测试替身**(契约 §8.1) ——
 *   梯度帧 64x64(按 pts 变色, 逐字节可断言: R=x*255/64, G=y*255/64,
 *   B=((int)(pts*10)*37)%256, A=255)+ 假时钟(tick 是唯一时间来源,
 *   停在时长末尾, ended 交引擎推导)。state/duration/position/seek 钳制
 *   语义与真宿主逐条对齐 —— 行为差异就是 bug。
 *
 * 确定性开关(环境钮, 引擎不读环境变量 —— 测试宿主的职责):
 *   HN_MEDIA_DURATION=s  全局时长覆盖(> 元素 data-duration > 缺省 2.0)
 */
#ifndef HN_MEDIA_SYNTH_H
#define HN_MEDIA_SYNTH_H

#include "hn.h"

/* 宿主表(进程内单例, 恒非 NULL)。装配方: hncore 直装 hn_context_set_media;
 * app 壳经 hn_media_platform_host() 在平台真宿主缺席时兜底。 */
const hn_media_host *hn_media_synth_host(void);

/* 元数据就绪门 + src→data-duration 查表基准。传 NULL = 只关门不换基准。
 *   hncore: 文档预处理后、建 context 前调 —— 门从一开始就开, duration 在
 *           首个 anim_tick(0) 即可解析(与旧 hncore "open 时读表"同效);
 *   app 壳: hn_rt_open 之后调(hn_rt_doc(rt)) —— 引擎 LOADING→READY 在
 *           bind 前的首次 anim_tick(0) 读 duration, 那时 doc 还拿不到,
 *           关门把这次读挡成"未就绪", 推迟到 bind 后首个帧节拍
 *           (zero 引擎改动)。文档热重载后对新的 doc 再调。
 * 幂等; 线程约定与引擎同(单线程帧循环)。 */
void hn_media_synth_bind_doc(hn_doc *doc);

/* 按 src 回读已打开句柄的 vol/muted(0..1 / 0|1)。验"元素声明 → 引擎钳制
 * → 宿主落位"整条链(hncore 的 MEDIA 状态行用)。未打开过该 src 返回 0,
 * 出参不动(调用方按缺省 vol=1 muted=0 处理)。 */
int hn_media_synth_query(const char *src, float *vol, int *muted);

#endif /* HN_MEDIA_SYNTH_H */

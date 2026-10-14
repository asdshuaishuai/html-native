/* hn_canvas_backend.h — <canvas> 2D 绘制后端抽象(平台自动适配)
 * ================================================================
 * 主线不变: HTML 生态。`<canvas>` 是 HTML 元素, 标准 Canvas2D API 由
 * JS glue(即 Canvas2D 规范面)承载; 本抽象只定义"绘制指令落到哪里"。
 *
 * 三种实现, 开窗时按平台/声明自动选择, 页面代码零改动:
 *   ledger  (缺省, 跨平台) — 现有账本+hnsoft 重放; 离屏确定性口径
 *   system  (macOS=CoreGraphics, Linux=cairo) — 系统级 AA/排版质量,
 *             绘制直接进系统 2D 上下文, 帧完成以 BITMAP 并入显示列表
 *   gpu     (路线图) — 多边形/图像批走 hnp_gpu_host 三角/四边形原语
 *
 * 契约:
 *   - create/resize/dispose 管理后端自有的帧缓冲(尺寸 = 画布属性尺寸;
 *     HiDPI 由调用方把属性尺寸×scale 传入)
 *   - op() 收"已解析的账本 op"(hn_canvas.c 现有解析结果; 后端不再碰
 *     JSON) — 同一份解析喂三种实现, 语义单一来源
 *   - flush() 帧结束: 返回非预乘 RGBA8(行距=w*4)+指针, 生命周期到下次
 *     flush/resize/dispose; 绘制侧以 HN_CMD_BITMAP 并入显示列表(透明
 *     底 = alpha 0, 天然支持透明窗挖洞)
 *   - text: v1 仍由引擎文本测量+账本文本 op 承载(system 后端可选择用
 *     系统排版重画 text op; 测量口径分离)
 *   - 平台不可用/创建失败 → 自动回落 ledger(可见而非静默: stderr 一行)
 *
 * 选择规则(v1): 环境变量 HN_CANVAS_BACKEND=ledger|system|auto(缺省 auto:
 *   macOS→system, 其他→ledger); 页面可 <canvas hn-canvas-ledger> 显式
 *   固定账本(离屏确定性/探针口径不受影响)。
 */
#ifndef HN_CANVAS_BACKEND_H
#define HN_CANVAS_BACKEND_H

#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct hn_canvas_backend hn_canvas_backend;
typedef struct hn_canvas_op hn_canvas_op;   /* hn_canvas.c 的已解析 op */

typedef struct hn_canvas_backend_vt {
    /** 帧缓冲(重)建; 失败返回 0(调用方回落 ledger)。 */
    int (*resize)(hn_canvas_backend *self, int width, int height);
    /** 消费一条已解析 op(绘制即时生效于后端帧缓冲)。 */
    void (*op)(hn_canvas_backend *self, const hn_canvas_op *o);
    /** 帧结束: 输出非预乘 RGBA8(行距 = width*4)。返回 0 = 本帧无效。 */
    int (*frame)(hn_canvas_backend *self, const unsigned char **rgba,
                 int *width, int *height, int *stride);
    void (*dispose)(hn_canvas_backend *self);
} hn_canvas_backend_vt;

struct hn_canvas_backend {
    const hn_canvas_backend_vt *vt;
    int width, height;          /* 属性尺寸 × HiDPI scale */
};

/* 工厂: kind = 0 ledger(缺省)/1 system/2 gpu(v2)。kind<0 = 按平台 auto。 */
hn_canvas_backend *hn_canvas_backend_create(int kind, int width, int height);
void hn_canvas_backend_destroy(hn_canvas_backend *b);

#ifdef __cplusplus
}
#endif
#endif

/* canvas_ity 实现(可移植高质量后端, ISC, vendored; C++ 适配器
 * hn_canvas_ity.cpp; v1 支持 frect/spath/fpath/渐变/clip, 见文件头)。 */
hn_canvas_backend *hn_canvas_backend_ity_create(int width, int height);

/* CoreGraphics 实现(macOS 专属, C++ 适配器 hn_canvas_cg.cpp;
 * 预乘 RGBA 位图 + 帧末去预乘; CoreText 文字; 全 blend 精确映射)。 */
hn_canvas_backend *hn_canvas_backend_cg_create(int width, int height);

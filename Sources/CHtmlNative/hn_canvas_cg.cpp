// hn_canvas_cg.cpp — canvas 后端之 CoreGraphics 实现(macOS, 系统框架)
// ================================================================
// 消费"已解析的账本 op"(hn_internal.h 的 hn_canvas_op, 与 hn_canvas_ity.cpp
// 同一解析面), 绘制进 CGBitmapContext, 帧完成以非预乘 RGBA8 输出(与
// HN_CMD_BITMAP 契约零转换)。CG 的 AA/混合/文字是 macOS 系统质量。
//
// 关键实测结论(2026-10, 本机 SDK + 运行时验证):
//   非预乘直通不可得。kCGImageAlphaLast(=3, "non-premultiplied RGBA")被
//   CGBitmapContextCreate 拒(实测返回 NULL); 枚举值 5 是
//   kCGImageAlphaNoneSkipLast(RGBX, alpha 字节无效), 误用则画布恒不透明。
//   唯一可行的 RGBA8 形态是 kCGImageAlphaPremultipliedLast(=1)—— 故位图
//   上下文恒为预乘, 帧末 frame() 逐像素去预乘到自有 out 缓冲补回契约。
//   (代价: 每帧一次 w*h*4 整数遍 + ±1 舍入; 与 canvas_ity 输出同级。)
//
// v1 支持: FILL_RECT(含阴影)/STROKE_PATH/FILL_PATH/GRAD_RECT/GRAD_PATH
//          /CLIP_PUSH/CLIP_POP/CLIP_PATH(多子路径)/CLEAR_RECT(CG 透明
//          擦除原生可得)/TEXT(CoreText 排版重画, 见下)/blend 四模式全
//          映射(CGContextSetBlendMode, 非"无逐调用混合")
//          /IMAGE(资产后端取字节 → hn_png_decode → 预乘 CGImage, 按路径
//          缓存; 9 参源子矩形走 CGImageCreateWithImageInRect, 左上原点
//          与 canvas 同口径)/PDATA(脏矩形预乘拷贝, clear+source-over
//          贴入 = putImageData 替换语义)/PATTERN_RECT/PATH(裁剪到形状
//          后按平铺模式循环 DrawImage, 锚点 = 画布原点 —— 与账本重放
//          pat_ox/oy 口径一致)。
// v1 限制: 图片解码只认 PNG(hn_png_decode 同源); 后端经
//          hn_canvas_backend_cg_create_ctx 挂 hn_context 加载源图(paint
//          接线注入; 不挂则图像类 op 打一行提示后跳过)。
// 接线: hn_paint.c 的 paint_canvas 在 HN_HAVE_CANVAS_CG 下调用本文件
//          尾部的 hn_canvas_paint_cg(HN_CANVAS_BACKEND=system|cg 启用;
//          2026-10-15 复审修复 —— 此前本 TU 零调用方, 整面不可达)。
//
// 纪律:
//   - dlsym(RTLD_DEFAULT) 绑定; CoreGraphics 必需(缺则工厂返回 NULL,
//     调用方回落 ledger), CoreText 可选(缺则 TEXT 一次性 stderr 跳过),
//     CGImage 图片面可选(缺则 IMAGE/PDATA/PATTERN 一次性 stderr 跳过)。
//   - CGFloat 在 macOS = double, 所有函数指针签名按 SDK 逐值镜像;
//     CGRect/CGPoint/CGSize/CGAffineTransform 按值传(AAPCS64: HFA 走
//     d 寄存器/48 字节结构体间接传 —— 由声明类型驱动, 不得拆成标量)。
//   - y 翻转: 建上下文即 translate(0,h)+scale(1,-1), 之后全部 op 用引擎
//     口径(左上原点, y 向下); 文字另设文本矩阵 d=-1 抵消(否则字形倒置)。
//   - 阴影只允许出现在 save/restore 对内(FILL_RECT 带影分支), 上下文
//     状态(影/混合/颜色/线宽)逐 op 重设, 不跨 op 泄漏。阴影 oy 按翻转
//     CTM 取负(引擎向下为正)。
//   - 渐变: stops→CGGradientCreateWithColorComponents(>64 截断一次性
//     提示), 无 stops 则 color→color2 两端; clip 后画(Before|After 双
//     选项= canvas 端点外延语义), 用完 CGGradientRelease(op 逐帧重放,
//     不释放即每次重放漏一个)。
//   - 工厂签名/链接与 hn_canvas_backend_ity_create 同款(C++ 链接, 头
//     文件在 include guard 外声明; 本文件按铁律不改头, 声明由接线方补)。
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>

extern "C" {
#include "hn_canvas_backend.h"
#include "hn_internal.h"
}

// ---- CG/CT 类型镜像(逐值抄自 SDK; CGFloat = double on macOS) ----
typedef struct CGContext *CGContextRef;
typedef struct CGColorSpace *CGColorSpaceRef;
typedef struct CGColor *CGColorRef;
typedef struct CGGradient *CGGradientRef;
typedef struct CGImage *CGImageRef;
typedef struct CGDataProvider *CGDataProviderRef;
typedef struct CTLine *CTLineRef;        /* SDK: struct CTLine * 的 opaque 指针 */
typedef long CFIndex;                    /* SDK: signed long */
typedef double CGFloat;

typedef struct { double x, y; } CGPoint;
typedef struct { double width, height; } CGSize;
typedef struct { double x, y, width, height; } CGRect;   /* {origin, size} 平铺 */
typedef struct { double a, b, c, d, tx, ty; } CGAffineTransform;

/* bitmapInfo 低 5 位 = CGImageAlphaInfo(逐值抄自 CGImageBase.h):
 *   0 None / 1 PremultipliedLast / 2 PremultipliedFirst / 3 AlphaLast /
 *   4 AlphaFirst / 5 NoneSkipLast / 6 NoneSkipFirst / 7 AlphaOnly。 */
#define CG_ALPHA_PREMULTIPLIED_LAST 1u
/* kCGGradientDrawsBeforeStartLocation|AfterEndLocation = canvas 端点外延 */
#define CG_GRAD_PAD 3u
/* kCFStringEncodingUTF8 */
#define CF_ENC_UTF8 0x08000100u
/* kCGLineCapButt/Round = 0/1; kCGLineJoinMiter/Round = 0/1 */
#define CG_CAP_ROUND 1
#define CG_JOIN_ROUND 1
/* CGBlendMode: Normal=0, Multiply=1, Screen=2, …, PlusLighter=27 */
#define CG_BLEND_NORMAL 0
#define CG_BLEND_MULTIPLY 1
#define CG_BLEND_SCREEN 2
#define CG_BLEND_PLUS_LIGHTER 27

// ---- dlsym 函数指针(CoreGraphics 必需面) ----
static struct {
    // colorspace / color
    CGColorSpaceRef (*cs_device_rgb)(void);
    CGColorRef (*color_create)(CGColorSpaceRef, const double *);
    void (*color_release)(CGColorRef);
    // bitmap context
    CGContextRef (*bmp_create)(void *, unsigned long, unsigned long, unsigned long,
                               unsigned long, CGColorSpaceRef, unsigned);
    void (*ctx_release)(CGContextRef);
    void (*save)(CGContextRef);
    void (*restore)(CGContextRef);
    void (*translate)(CGContextRef, double, double);
    void (*scale)(CGContextRef, double, double);
    void (*set_fill_rgba)(CGContextRef, double, double, double, double);
    void (*set_stroke_rgba)(CGContextRef, double, double, double, double);
    void (*set_line_width)(CGContextRef, double);
    void (*set_line_cap)(CGContextRef, int);
    void (*set_line_join)(CGContextRef, int);
    void (*set_blend_mode)(CGContextRef, int);
    void (*set_shadow_color)(CGContextRef, CGSize, double, CGColorRef);
    void (*clip_rect)(CGContextRef, CGRect);
    void (*clip)(CGContextRef);
    void (*clear_rect)(CGContextRef, CGRect);
    void (*fill_rect)(CGContextRef, CGRect);
    // path(上下文路径足够: begin 一次可容多条子路径, 无需 CGPath)
    void (*begin_path)(CGContextRef);
    void (*move_to)(CGContextRef, double, double);
    void (*line_to)(CGContextRef, double, double);
    void (*close_path)(CGContextRef);
    void (*fill_path)(CGContextRef);
    void (*stroke_path)(CGContextRef);
    // gradient
    CGGradientRef (*grad_create)(CGColorSpaceRef, const double *, const double *,
                                 unsigned long);
    void (*grad_release)(CGGradientRef);
    void (*draw_linear_grad)(CGContextRef, CGGradientRef, CGPoint, CGPoint, unsigned);
    void (*draw_radial_grad)(CGContextRef, CGGradientRef, CGPoint, double,
                             CGPoint, double, unsigned);
    // text state(写侧)
    void (*set_text_pos)(CGContextRef, double, double);
    void (*set_text_matrix)(CGContextRef, CGAffineTransform);
} cg;

// ---- dlsym 函数指针(图片面: IMAGE/PDATA/PATTERN 消费; 缺任一 → 图像类
//      op 一次性 stderr 跳过, 后端其余功能不受影响 —— 与 CoreText 同款
//      可选口径) ----
static struct {
    CGImageRef (*img_create)(unsigned long, unsigned long, unsigned long,
                             unsigned long, unsigned long, CGColorSpaceRef,
                             unsigned, CGDataProviderRef, const double *,
                             bool, int);                        // CGImageCreate
    void (*img_release)(CGImageRef);                            // CGImageRelease
    unsigned long (*img_width)(CGImageRef);                     // CGImageGetWidth
    unsigned long (*img_height)(CGImageRef);                    // CGImageGetHeight
    CGImageRef (*img_sub)(CGImageRef, CGRect);                  // CGImageCreateWithImageInRect
    CGDataProviderRef (*prov_create_data)(void *, const void *, unsigned long,
                                          void (*)(void *, const void *,
                                                   unsigned long)); // CGDataProviderCreateWithData
    void (*prov_release)(CGDataProviderRef);                    // CGDataProviderRelease
    void (*draw_image)(CGContextRef, CGImageRef, CGRect);       // CGContextDrawImage
    bool ready;
} ci;

// ---- dlsym 函数指针(CoreText 可选面; 缺任一 → 文字 op 跳过) ----
static struct {
    void *(*str_create_cstr)(void *, const char *, unsigned);       // CFStringCreateWithCString
    void *(*dict_create)(void *, const void *const *, const void *const *,
                         CFIndex, const void *, const void *);      // CFDictionaryCreate
    void *(*attr_str_create)(void *, void *, void *);               // CFAttributedStringCreate(alloc, str, attrs)
    void *(*line_create)(void *);                                   // CTLineCreateWithAttributedString
    void (*line_draw)(CTLineRef, CGContextRef);                     // CTLineDraw(line, ctx)
    void *(*font_create)(void *, double, const CGAffineTransform *);// CTFontCreateWithName
    void (*cf_release)(void *);                                     // CFRelease
    void *k_font_attr;                                              // kCTFontAttributeName(数据符号, 解一层)
    void *k_color_attr;                                             // kCTForegroundColorAttributeName
    void *font_name;                                                // ".AppleSystemUIFont" CFString(进程期缓存)
    bool ready;
} ct;

static bool cg_ready = false;
static CGColorSpaceRef g_cs = NULL;   /* DeviceRGB, +1 进程期持有 */

static bool dlsym_req(void *slot, const char *name) {
    *(void **)slot = dlsym(RTLD_DEFAULT, name);
    return *(void **)slot != NULL;
}
static bool dlsym_data(void **out, const char *name) {
    /* 数据符号: dlsym 返回"变量地址", 解一层才是 CFStringRef 值。 */
    void **p = (void **)dlsym(RTLD_DEFAULT, name);
    if (!p || !*p) return false;
    *out = *p;
    return true;
}

static void ct_load(void) {
    if (ct.ready) return;
    /* CoreFoundation 随 CoreGraphics 装入(dlsym RTLD_DEFAULT 可见)。 */
    if (!dlopen("/System/Library/Frameworks/CoreText.framework/CoreText",
                RTLD_NOW | RTLD_LOCAL))
        return;
    if (!dlsym_req(&ct.str_create_cstr, "CFStringCreateWithCString") ||
        !dlsym_req(&ct.dict_create, "CFDictionaryCreate") ||
        !dlsym_req(&ct.attr_str_create, "CFAttributedStringCreate") ||
        !dlsym_req(&ct.line_create, "CTLineCreateWithAttributedString") ||
        !dlsym_req(&ct.line_draw, "CTLineDraw") ||
        !dlsym_req(&ct.font_create, "CTFontCreateWithName") ||
        !dlsym_req(&ct.cf_release, "CFRelease") ||
        !dlsym_data(&ct.k_font_attr, "kCTFontAttributeName") ||
        !dlsym_data(&ct.k_color_attr, "kCTForegroundColorAttributeName"))
        return;
    ct.font_name = ct.str_create_cstr(NULL, ".AppleSystemUIFont", CF_ENC_UTF8);
    if (!ct.font_name) return;
    ct.ready = true;
}

static void ci_load(void) {
    if (ci.ready) return;
    /* 符号都在已装入的 CoreGraphics 里, 无需再次 dlopen。 */
    if (!dlsym_req(&ci.img_create, "CGImageCreate") ||
        !dlsym_req(&ci.img_release, "CGImageRelease") ||
        !dlsym_req(&ci.img_width, "CGImageGetWidth") ||
        !dlsym_req(&ci.img_height, "CGImageGetHeight") ||
        !dlsym_req(&ci.img_sub, "CGImageCreateWithImageInRect") ||
        !dlsym_req(&ci.prov_create_data, "CGDataProviderCreateWithData") ||
        !dlsym_req(&ci.prov_release, "CGDataProviderRelease") ||
        !dlsym_req(&ci.draw_image, "CGContextDrawImage"))
        return;
    ci.ready = true;
}

static bool cg_load(void) {
    if (cg_ready) return true;
    if (!dlopen("/System/Library/Frameworks/CoreGraphics.framework/CoreGraphics",
                RTLD_NOW | RTLD_LOCAL))
        return false;
    if (!dlsym_req(&cg.cs_device_rgb, "CGColorSpaceCreateDeviceRGB") ||
        !dlsym_req(&cg.color_create, "CGColorCreate") ||
        !dlsym_req(&cg.color_release, "CGColorRelease") ||
        !dlsym_req(&cg.bmp_create, "CGBitmapContextCreate") ||
        !dlsym_req(&cg.ctx_release, "CGContextRelease") ||
        !dlsym_req(&cg.save, "CGContextSaveGState") ||
        !dlsym_req(&cg.restore, "CGContextRestoreGState") ||
        !dlsym_req(&cg.translate, "CGContextTranslateCTM") ||
        !dlsym_req(&cg.scale, "CGContextScaleCTM") ||
        !dlsym_req(&cg.set_fill_rgba, "CGContextSetRGBFillColor") ||
        !dlsym_req(&cg.set_stroke_rgba, "CGContextSetRGBStrokeColor") ||
        !dlsym_req(&cg.set_line_width, "CGContextSetLineWidth") ||
        !dlsym_req(&cg.set_line_cap, "CGContextSetLineCap") ||
        !dlsym_req(&cg.set_line_join, "CGContextSetLineJoin") ||
        !dlsym_req(&cg.set_blend_mode, "CGContextSetBlendMode") ||
        !dlsym_req(&cg.set_shadow_color, "CGContextSetShadowWithColor") ||
        !dlsym_req(&cg.clip_rect, "CGContextClipToRect") ||
        !dlsym_req(&cg.clip, "CGContextClip") ||
        !dlsym_req(&cg.clear_rect, "CGContextClearRect") ||
        !dlsym_req(&cg.fill_rect, "CGContextFillRect") ||
        !dlsym_req(&cg.begin_path, "CGContextBeginPath") ||
        !dlsym_req(&cg.move_to, "CGContextMoveToPoint") ||
        !dlsym_req(&cg.line_to, "CGContextAddLineToPoint") ||
        !dlsym_req(&cg.close_path, "CGContextClosePath") ||
        !dlsym_req(&cg.fill_path, "CGContextFillPath") ||
        !dlsym_req(&cg.stroke_path, "CGContextStrokePath") ||
        !dlsym_req(&cg.grad_create, "CGGradientCreateWithColorComponents") ||
        !dlsym_req(&cg.grad_release, "CGGradientRelease") ||
        !dlsym_req(&cg.draw_linear_grad, "CGContextDrawLinearGradient") ||
        !dlsym_req(&cg.draw_radial_grad, "CGContextDrawRadialGradient") ||
        !dlsym_req(&cg.set_text_pos, "CGContextSetTextPosition") ||
        !dlsym_req(&cg.set_text_matrix, "CGContextSetTextMatrix"))
        return false;
    g_cs = cg.cs_device_rgb();          /* +1, 进程期缓存不释放 */
    if (!g_cs) return false;
    cg_ready = true;
    ct_load();                          /* 文字面可选, 失败不影响本后端 */
    ci_load();                          /* 图片面可选, 失败仅图像类 op 跳过 */
    return true;
}

// ---- 后端对象 ----
/* 解码缓存条目: 账本逐帧重放, 同一路径每帧都来 —— 按路径缓存解码产物
   (预乘 CGImage, 缓存持有 +1, dispose 释放), 失败路径不缓存(资产可能
   稍后才可用), 只 warn_once 闸住 stderr。 */
struct cg_img_ent {
    char *path;                /* malloc 副本(账本 str 生命周期更短) */
    CGImageRef img;            /* 预乘, provider release 链接管像素缓冲 */
};
#define CG_IMG_CACHE 12

struct cg_backend : hn_canvas_backend {
    CGContextRef ctx;          /* 预乘 RGBA8 位图上下文(y 已翻转) */
    unsigned char *data;       /* 上下文像素(自有 malloc, stride = w*4 恒定) */
    unsigned char *out;        /* 帧末去预乘输出(frame 契约面) */
    unsigned char warned[8];
    void *hc;                  /* hn_context*(工厂 create_ctx 注入; 图像类
                                  op 经 hn_context_assets 加载源图; 可 NULL) */
    cg_img_ent imgs[CG_IMG_CACHE];
    unsigned imgs_gen;         /* 覆盖槽游标(循环替换, 非严格 LRU) */
};

enum {  /* warned 槽位 */
    W_TEXT = 0,      /* CoreText 不可用 */
    W_IMAGE = 1,     /* 源图加载/解码失败(仅支持 PNG) */
    W_PDATA = 2,
    W_PATTERN = 3,   /* 平铺数量异常 */
    W_STOPS = 4,     /* 渐变 stop 截断 */
    W_OP = 5,        /* 未知 op */
    W_CGBIND = 6,    /* CGImage 绑定不可用 */
};
#define W_N 8

static void warn_once(cg_backend *b, int slot, const char *msg) {
    if (slot >= 0 && slot < W_N) {
        if (b->warned[slot]) return;
        b->warned[slot] = 1;
    }
    fprintf(stderr, "[canvas-cg] 暂不支持/受限: %s(该 op 已跳过)\n", msg);
}

/* hn_color 字节序 = 引擎统一约定(高→低 r,g,b,a; 见 hnsoft unpack 与
   rt glue rgba() 的 (r<<24)|(g<<16)|(b<<8)|a)—— 2026-10-15 修复: 此前
   按 0xRRGGBBAA 读, 红蓝互换且 alpha 取错字节。 */
static void rgba_set(CGContextRef ctx, unsigned c, bool stroke) {
    double r = (double)((c >> 24) & 255) / 255.0;
    double g = (double)((c >> 16) & 255) / 255.0;
    double bl = (double)((c >> 8) & 255) / 255.0;
    double a = (double)(c & 255) / 255.0;
    if (stroke) cg.set_stroke_rgba(ctx, r, g, bl, a);
    else        cg.set_fill_rgba(ctx, r, g, bl, a);
}

static CGColorRef make_color(unsigned c) {
    double comps[4] = {
        (double)((c >> 24) & 255) / 255.0, (double)((c >> 16) & 255) / 255.0,
        (double)((c >> 8) & 255) / 255.0, (double)(c & 255) / 255.0,
    };
    return cg.color_create(g_cs, comps);
}

static int blend_to_cg(unsigned char b) {
    switch (b) {
    case HN_BLEND_LIGHTER:  return CG_BLEND_PLUS_LIGHTER;
    case HN_BLEND_MULTIPLY: return CG_BLEND_MULTIPLY;
    case HN_BLEND_SCREEN:   return CG_BLEND_SCREEN;
    default:                return CG_BLEND_NORMAL;   /* SRC_OVER + 解析期回落 */
    }
}

static void subpath_add(CGContextRef ctx, const float *p, int n, bool closed) {
    if (n <= 0 || !p) return;
    cg.move_to(ctx, (double)p[0], (double)p[1]);
    for (int i = 1; i < n; i++)
        cg.line_to(ctx, (double)p[i * 2], (double)p[i * 2 + 1]);
    if (closed) cg.close_path(ctx);
}

/* 折线/多边形入上下文路径。segs(cpath 子路径点数表, NULL=单多边形):
 * 逐段 move/line/close —— begin 一次可容多条子路径, 精确对应 cpath 语义。 */
static void build_ctx_path(CGContextRef ctx, const float *pts, int n_pts,
                           bool closed, const int *segs, int n_segs) {
    cg.begin_path(ctx);
    if (!segs || n_segs <= 0) {
        subpath_add(ctx, pts, n_pts, closed);
        return;
    }
    int off = 0;
    for (int s = 0; s < n_segs && off < n_pts; s++) {
        int cnt = segs[s];
        if (cnt <= 0) continue;
        if (off + cnt > n_pts) cnt = n_pts - off;
        subpath_add(ctx, pts + (size_t)off * 2, cnt, closed);
        off += cnt;
    }
}

/* 渐变(栈上, 每次构一份): stops 数组优先, 否则 color→color2 两端。 */
#define CG_MAX_STOPS 64
static CGGradientRef make_gradient(cg_backend *b, const hn_canvas_op *o) {
    double comps[CG_MAX_STOPS * 4], locs[CG_MAX_STOPS];
    unsigned long n = 0;
    if (o->n_stops >= 2 && o->stops) {
        int cnt = o->n_stops;
        if (cnt > CG_MAX_STOPS) {
            warn_once(b, W_STOPS, "渐变 stop 超 64, 已截断");
            cnt = CG_MAX_STOPS;
        }
        for (int i = 0; i < cnt; i++) {
            unsigned col = (unsigned)o->stops[i].c;
            comps[i * 4 + 0] = (double)((col >> 24) & 255) / 255.0;
            comps[i * 4 + 1] = (double)((col >> 16) & 255) / 255.0;
            comps[i * 4 + 2] = (double)((col >> 8) & 255) / 255.0;
            comps[i * 4 + 3] = (double)(col & 255) / 255.0;
            locs[i] = (double)o->stops[i].t;
        }
        n = (unsigned long)cnt;
    } else {
        unsigned c0 = (unsigned)o->color, c1 = (unsigned)o->color2;
        comps[0] = (double)((c0 >> 24) & 255) / 255.0;
        comps[1] = (double)((c0 >> 16) & 255) / 255.0;
        comps[2] = (double)((c0 >> 8) & 255) / 255.0;
        comps[3] = (double)(c0 & 255) / 255.0;
        comps[4] = (double)((c1 >> 24) & 255) / 255.0;
        comps[5] = (double)((c1 >> 16) & 255) / 255.0;
        comps[6] = (double)((c1 >> 8) & 255) / 255.0;
        comps[7] = (double)(c1 & 255) / 255.0;
        locs[0] = 0.0; locs[1] = 1.0;
        n = 2;
    }
    return cg.grad_create(g_cs, comps, locs, n);
}

static void draw_grad(cg_backend *b, const hn_canvas_op *o) {
    CGGradientRef g = make_gradient(b, o);
    if (!g) return;
    CGContextRef ctx = b->ctx;
    cg.save(ctx);                            /* clip 是状态, save/restore 包住 */
    if (o->kind == HN_CV_GRAD_RECT) {
        cg.clip_rect(ctx, CGRect{(double)o->x, (double)o->y,
                                 (double)o->w, (double)o->h});
    } else {
        build_ctx_path(ctx, o->pts, o->n_pts, true, NULL, 0);
        cg.clip(ctx);
    }
    if (o->grad_radial) {
        cg.draw_radial_grad(ctx, g,
                            CGPoint{(double)o->gx0, (double)o->gy0}, (double)o->gr0,
                            CGPoint{(double)o->gx1, (double)o->gy1}, (double)o->gr1,
                            CG_GRAD_PAD);
    } else {
        cg.draw_linear_grad(ctx, g,
                            CGPoint{(double)o->gx0, (double)o->gy0},
                            CGPoint{(double)o->gx1, (double)o->gy1},
                            CG_GRAD_PAD);
    }
    cg.restore(ctx);
    cg.grad_release(g);                      /* op 逐帧重放, 不释放即漏 */
}

static void op_text(cg_backend *b, const hn_canvas_op *o) {
    if (!ct.ready) {
        warn_once(b, W_TEXT, "CoreText 绑定不可用");
        return;
    }
    if (!o->str || !o->str[0]) return;
    CGContextRef ctx = b->ctx;
    double px = o->font_px > 0 ? (double)o->font_px : 16.0;

    void *cfs = ct.str_create_cstr(NULL, o->str, CF_ENC_UTF8);
    void *font = cfs ? ct.font_create(ct.font_name, px, NULL) : NULL;
    CGColorRef col = font ? make_color((unsigned)o->color) : NULL;
    const void *keys[2] = { ct.k_font_attr, ct.k_color_attr };
    const void *vals[2] = { font, col };
    void *dict = (font && col) ? ct.dict_create(NULL, keys, vals, 2, NULL, NULL)
                               : NULL;
    /* CFAttributedStringCreate 对属性值按 CF 语义自持; 下方即刻释放
       font/col 安全, 生命周期由 astr 单独接管。 */
    void *astr = (dict && col) ? ct.attr_str_create(NULL, cfs, dict) : NULL;
    if (astr) {
        void *line = ct.line_create(astr);
        if (line) {
            /* CTM 已翻 y(建上下文时), 文本矩阵再翻一次, 字形才正立;
               文字位置 = (x, y) 基线(行原点即基线左端, canvas 口径)。 */
            CGAffineTransform flip = {1.0, 0.0, 0.0, -1.0, 0.0, 0.0};
            cg.set_text_matrix(ctx, flip);
            cg.set_text_pos(ctx, (double)o->x, (double)o->y);
            ct.line_draw((CTLineRef)line, ctx);
            ct.cf_release(line);
        }
        ct.cf_release(astr);
    }
    if (dict) ct.cf_release(dict);
    if (col) cg.color_release(col);
    if (font) ct.cf_release(font);
    if (cfs) ct.cf_release(cfs);
}

// ---- 图片通道(IMAGE/PDATA/PATTERN 共用) ----
/* 非预乘 RGBA → 预乘拷贝(canvas 契约值 → CG 位图唯一可行形态, 见文件头
   实测结论); s' = round(s*a/255), 不变式 s<=a 使结果天然 <=255。 */
static void premult_copy(const unsigned char *src, unsigned char *dst,
                         size_t npx) {
    for (size_t i = 0; i < npx; i++) {
        unsigned a = src[i * 4 + 3];
        dst[i * 4 + 3] = (unsigned char)a;
        if (a == 255) {
            dst[i * 4 + 0] = src[i * 4 + 0];
            dst[i * 4 + 1] = src[i * 4 + 1];
            dst[i * 4 + 2] = src[i * 4 + 2];
        } else if (a == 0) {
            dst[i * 4 + 0] = 0;
            dst[i * 4 + 1] = 0;
            dst[i * 4 + 2] = 0;
        } else {
            for (int c = 0; c < 3; c++) {
                unsigned v = ((unsigned)src[i * 4 + c] * a + 127) / 255;
                dst[i * 4 + c] = (unsigned char)(v > 255 ? 255 : v);
            }
        }
    }
}

/* provider 释放回调 → 归还像素缓冲(CGImage 释放链自然触发, 不漏)。 */
static void rel_data(void *, const void *data, unsigned long) {
    free((void *)data);
}

/* 预乘 RGBA 缓冲(**接管所有权**)→ CGImage。失败释放缓冲并返回 NULL。 */
static CGImageRef image_from_premult(unsigned char *pm, int w, int h) {
    CGDataProviderRef prov =
        ci.prov_create_data(NULL, pm, (unsigned long)w * (unsigned long)h * 4,
                            rel_data);
    if (!prov) {
        free(pm);
        return NULL;
    }
    CGImageRef img =
        ci.img_create((unsigned long)w, (unsigned long)h, 8, 32,
                      (unsigned long)w * 4, g_cs,
                      CG_ALPHA_PREMULTIPLIED_LAST, prov, NULL, true, 0);
    if (!img)
        ci.prov_release(prov);   /* 触发 rel_data → free(pm), 不漏 */
    return img;
}

static void ci_ent_release(cg_img_ent *e) {
    if (e->img) { ci.img_release(e->img); e->img = NULL; }
    free(e->path);
    e->path = NULL;
}

/* 路径 → 解码缓存里的 CGImage(缓存持有, 调用方不得 release)。
   字节经资产后端(hn_context_assets→load, 缓冲运行时持有不 free),
   解码走 hn_png_decode(同源, 仅 PNG), 预乘拷贝后建图。 */
static CGImageRef image_for_path(cg_backend *b, const char *path) {
    if (!ci.ready) {
        warn_once(b, W_CGBIND, "CGImage 绑定不可用");
        return NULL;
    }
    if (!path || !path[0]) return NULL;
    for (int i = 0; i < CG_IMG_CACHE; i++)
        if (b->imgs[i].path && !strcmp(b->imgs[i].path, path))
            return b->imgs[i].img;
    if (!b->hc) {
        warn_once(b, W_IMAGE, "后端未挂 hn_context(create_ctx 注入), 无法加载源图");
        return NULL;
    }
    const hn_asset_backend *ab = hn_context_assets((hn_context *)b->hc);
    if (!ab || !ab->load) {
        warn_once(b, W_IMAGE, "资产后端不可用, 无法加载源图");
        return NULL;
    }
    size_t len = 0;
    const char *bytes = ab->load(ab->ctx, path, &len);   /* 运行时持有, 不 free */
    if (!bytes || !len) {
        warn_once(b, W_IMAGE, "源图字节加载失败(查资产后端与路径)");
        return NULL;
    }
    int w = 0, h = 0;
    unsigned char *px = hn_png_decode((const unsigned char *)bytes, len, &w, &h);
    if (!px || w <= 0 || h <= 0) {
        free(px);
        warn_once(b, W_IMAGE, "PNG 解码失败(图像源仅支持 PNG)");
        return NULL;
    }
    size_t npx = (size_t)w * (size_t)h;
    unsigned char *pm = (unsigned char *)malloc(npx * 4);
    if (!pm) {
        free(px);
        return NULL;
    }
    premult_copy(px, pm, npx);
    free(px);
    CGImageRef img = image_from_premult(pm, w, h);       /* pm 所有权移交 */
    if (!img) return NULL;
    cg_img_ent *e = &b->imgs[b->imgs_gen % CG_IMG_CACHE];
    b->imgs_gen++;
    ci_ent_release(e);
    size_t pn = strlen(path) + 1;
    e->path = (char *)malloc(pn);
    if (e->path) memcpy(e->path, path, pn);
    e->img = img;
    return img;
}

/* 图片正立绘制: 上下文 CTM 已翻转(引擎坐标), 翻转空间直接 DrawImage
   会上下颠倒(iOS 同款著名行为)—— 逐次贴局部翻转抵消: 平移到目标矩形
   底边再翻 y, 图片首行(视觉顶)落在引擎矩形顶边。 */
static void draw_image_upright(CGContextRef c, CGImageRef img, CGRect r) {
    cg.save(c);
    cg.translate(c, r.x, r.y + r.height);
    cg.scale(c, 1.0, -1.0);
    ci.draw_image(c, img, CGRect{r.x, 0.0, r.width, r.height});
    cg.restore(c);
}

static void op_image(cg_backend *b, const hn_canvas_op *o) {
    CGImageRef img = image_for_path(b, o->str);
    if (!img) return;
    CGImageRef sub = img;
    if (o->has_src) {
        /* 9 参 drawImage: 源子矩形按规范裁进源图(越界裁除, 空则无操作);
           CGImageCreateWithImageInRect 左上原点, 与 canvas 源坐标同口径。 */
        double iw = (double)ci.img_width(img), ih = (double)ci.img_height(img);
        double cx = o->sx < 0 ? 0 : (double)o->sx;
        double cy = o->sy < 0 ? 0 : (double)o->sy;
        double cw = (double)o->sw - (cx - (double)o->sx);
        double ch = (double)o->sh - (cy - (double)o->sy);
        if (cx + cw > iw) cw = iw - cx;
        if (cy + ch > ih) ch = ih - cy;
        if (cw <= 0 || ch <= 0) return;
        sub = ci.img_sub(img, CGRect{cx, cy, cw, ch});
        if (!sub) return;
    }
    /* 负目标宽高 = canvas 规范的翻转语义, CG 对负尺寸矩形原生镜像。 */
    draw_image_upright(b->ctx, sub, CGRect{(double)o->x, (double)o->y,
                                           (double)o->w, (double)o->h});
    if (sub != img) ci.img_release(sub);
}

static void op_pdata(cg_backend *b, const hn_canvas_op *o) {
    if (!o->pdata || o->pw <= 0 || o->ph <= 0) return;
    /* 脏矩形裁进块内(与 hn_paint 发射侧同一裁法), 贴到 (x+dx, y+dy)。 */
    int dx = 0, dy = 0, dw = o->pw, dh = o->ph;
    if (o->has_dirty) {
        dx = (int)o->dtx; dy = (int)o->dty;
        dw = (int)o->dtw; dh = (int)o->dth;
        if (dx < 0) { dw += dx; dx = 0; }
        if (dy < 0) { dh += dy; dy = 0; }
        if (dx + dw > o->pw) dw = o->pw - dx;
        if (dy + dh > o->ph) dh = o->ph - dy;
        if (dw <= 0 || dh <= 0) return;
    }
    /* 只预乘脏子区(行段拷贝成紧排块), 建图贴入。 */
    size_t rowb = (size_t)dw * 4;
    unsigned char *pm = (unsigned char *)malloc(rowb * (size_t)dh);
    if (!pm) return;
    for (int r = 0; r < dh; r++) {
        const unsigned char *src =
            o->pdata + ((size_t)(dy + r) * (size_t)o->pw + (size_t)dx) * 4;
        premult_copy(src, pm + (size_t)r * rowb, (size_t)dw);
    }
    CGImageRef img = image_from_premult(pm, dw, dh);     /* pm 所有权移交 */
    if (!img) return;
    /* putImageData 不受 gco/混合影响: 恒 source-over; 先 clear 后贴 =
       替换语义(含 alpha=0 源像素压成透明, 纯 source-over 贴做不到)。 */
    CGContextRef ctx = b->ctx;
    CGRect dst = {(double)o->x + dx, (double)o->y + dy, (double)dw, (double)dh};
    cg.set_blend_mode(ctx, CG_BLEND_NORMAL);
    cg.clear_rect(ctx, dst);
    draw_image_upright(ctx, img, dst);
    ci.img_release(img);
}

static void op_pattern(cg_backend *b, const hn_canvas_op *o) {
    if (o->kind == HN_CV_PATTERN_PATH && o->n_pts < 3) return;
    CGImageRef img = image_for_path(b, o->str);
    if (!img) return;
    double iw = (double)ci.img_width(img), ih = (double)ci.img_height(img);
    if (iw < 1.0 || ih < 1.0) return;
    /* 形状包围盒 = 平铺循环范围(clip 后多余贴片被裁, 循环按 bbox 计数)。 */
    double minx, miny, maxx, maxy;
    if (o->kind == HN_CV_PATTERN_RECT) {
        if (!(o->w > 0 && o->h > 0)) return;
        minx = o->x; miny = (double)o->y;
        maxx = (double)o->x + o->w; maxy = (double)o->y + o->h;
    } else {
        minx = maxx = o->pts[0];
        miny = maxy = o->pts[1];
        for (int i = 1; i < o->n_pts; i++) {
            double px = o->pts[i * 2], py = o->pts[i * 2 + 1];
            if (px < minx) minx = px;
            if (px > maxx) maxx = px;
            if (py < miny) miny = py;
            if (py > maxy) maxy = py;
        }
    }
    /* 平铺锚点 = 画布原点(0,0), 与账本重放 pat_ox/oy 口径一致;
       repeat-x 只铺 y=0 一行, repeat-y 只铺 x=0 一列, no-repeat 一贴。 */
    int rep = o->prep & 3;               /* HN_PAT_REPEAT/X/Y/NONE */
    double x0 = 0, y0 = 0;
    long nx = 1, ny = 1;
    if (rep != 3 /* HN_PAT_NONE */) {
        if (rep != 2 /* HN_PAT_REPEAT_Y */) {
            x0 = floor(minx / iw) * iw;
            nx = (long)((maxx - x0) / iw) + 1;
        }
        if (rep != 1 /* HN_PAT_REPEAT_X */) {
            y0 = floor(miny / ih) * ih;
            ny = (long)((maxy - y0) / ih) + 1;
        }
    }
    if (nx <= 0 || ny <= 0) return;
    if (nx > 8192 || ny > 8192 || nx * ny > 262144) {
        warn_once(b, W_PATTERN, "平铺贴片数量异常(图过小或形状过大), 已跳过");
        return;
    }
    CGContextRef ctx = b->ctx;
    cg.save(ctx);
    if (o->kind == HN_CV_PATTERN_RECT) {
        cg.clip_rect(ctx, CGRect{(double)o->x, (double)o->y,
                                 (double)o->w, (double)o->h});
    } else {
        build_ctx_path(ctx, o->pts, o->n_pts, true, o->segs, o->n_segs);
        cg.clip(ctx);
    }
    for (long j = 0; j < ny; j++)
        for (long i = 0; i < nx; i++)
            draw_image_upright(ctx, img,
                               CGRect{x0 + (double)i * iw, y0 + (double)j * ih,
                                      iw, ih});
    cg.restore(ctx);
}

// ---- vtable ----
static int cg_backend_init(cg_backend *b, int w, int h);

static void cg_clear(hn_canvas_backend *self) {
    auto *b = (cg_backend *)self;
    /* 新帧清面(2026-10-15 复审修复): 直接 memset 预乘底缓冲 —— 透明黑
       与 cg_backend_init 的初始态逐位一致, 且不受上一帧残留 clip/save
       栈与 CTM 影响(CGContextClearRect 会吃当前裁剪, 需先复位状态栈,
       memset 一步到位)。b->out 不必清: frame() 每次整幅覆写。 */
    if (b->data)
        memset(b->data, 0, (size_t)b->width * (size_t)b->height * 4);
}

static int cg_resize(hn_canvas_backend *self, int w, int h) {
    auto *b = (cg_backend *)self;
    if (w <= 0 || h <= 0) return 0;
    /* 旧上下文先 release(它引用 data), 再 free 缓冲 —— 逆序即悬垂。 */
    if (b->ctx) { cg.ctx_release(b->ctx); b->ctx = NULL; }
    free(b->data); b->data = NULL;
    free(b->out);  b->out = NULL;
    return cg_backend_init(b, w, h);
}

static void cg_op(hn_canvas_backend *self, const hn_canvas_op *o) {
    auto *b = (cg_backend *)self;
    if (!b->ctx || !o) return;
    CGContextRef ctx = b->ctx;
    /* 逐 op 重设可变状态: 混合模式按 op 映射(CG 逐调用混合原生可得,
       四种引擎模式精确对应); 阴影只在 save/restore 对内出现(见
       FILL_RECT), 其余 op 天然无影/线宽残留。 */
    cg.set_blend_mode(ctx, blend_to_cg(o->blend));

    switch (o->kind) {
    case HN_CV_FILL_RECT: {
        rgba_set(ctx, (unsigned)o->color, false);
        if (o->shadow) {
            cg.save(ctx);                    /* 阴影不出对: 状态零泄漏 */
            CGColorRef sc = make_color((unsigned)o->sh_color);
            /* 影偏移随翻转 CTM: 引擎 oy 向下为正 → CG 用户空间取负。 */
            cg.set_shadow_color(ctx, CGSize{(double)o->sh_ox, -(double)o->sh_oy},
                                (double)o->sh_blur, sc);
            cg.fill_rect(ctx, CGRect{(double)o->x, (double)o->y,
                                     (double)o->w, (double)o->h});
            cg.restore(ctx);
            if (sc) cg.color_release(sc);
        } else {
            cg.fill_rect(ctx, CGRect{(double)o->x, (double)o->y,
                                     (double)o->w, (double)o->h});
        }
        break;
    }
    case HN_CV_STROKE_PATH:
        if (o->stroke_w <= 0) break;         /* canvas: 宽 0 不出墨 */
        build_ctx_path(ctx, o->pts, o->n_pts, false, NULL, 0);   /* 开放折线 */
        cg.set_line_width(ctx, (double)o->stroke_w);
        cg.set_line_cap(ctx, o->cap_round ? CG_CAP_ROUND : 0);
        cg.set_line_join(ctx, o->cap_round ? CG_JOIN_ROUND : 0);
        rgba_set(ctx, (unsigned)o->color, true);
        cg.stroke_path(ctx);
        break;
    case HN_CV_FILL_PATH:
        build_ctx_path(ctx, o->pts, o->n_pts, true, NULL, 0);    /* 隐式闭合 */
        rgba_set(ctx, (unsigned)o->color, false);
        cg.fill_path(ctx);                   /* 非零环绕 = canvas 默认 */
        break;
    case HN_CV_GRAD_RECT:
    case HN_CV_GRAD_PATH:
        draw_grad(b, o);
        break;
    case HN_CV_CLIP_PUSH:
        cg.save(ctx);
        cg.clip_rect(ctx, CGRect{(double)o->x, (double)o->y,
                                 (double)o->w, (double)o->h});
        break;
    case HN_CV_CLIP_POP:
        cg.restore(ctx);
        break;
    case HN_CV_CLIP_PATH:
        cg.save(ctx);
        build_ctx_path(ctx, o->pts, o->n_pts, true, o->segs, o->n_segs);
        cg.clip(ctx);
        break;
    case HN_CV_CLEAR_RECT:
        /* CG 对带 alpha 位图的透明擦除原生可得(canvas clearRect 语义:
           无视混合/alpha, 置透明黑)—— ity 缺的这块这里补齐。 */
        cg.clear_rect(ctx, CGRect{(double)o->x, (double)o->y,
                                  (double)o->w, (double)o->h});
        break;
    case HN_CV_TEXT:
        op_text(b, o);
        break;
    case HN_CV_IMAGE:
        op_image(b, o);
        break;
    case HN_CV_PDATA:
        op_pdata(b, o);
        break;
    case HN_CV_PATTERN_RECT:
    case HN_CV_PATTERN_PATH:
        op_pattern(b, o);
        break;
    default: {
        warn_once(b, W_OP, "未知 op");
        break;
    }
    }
}

static int cg_frame(hn_canvas_backend *self, const unsigned char **rgba,
                    int *width, int *height, int *stride) {
    auto *b = (cg_backend *)self;
    if (!b->ctx || !b->data || !b->out) return 0;
    /* 预乘 → 非预乘(文件头"关键实测结论"): a=255 直通, a=0 清零,
       其余 s' = round(s*255/a); 预乘不变式 s<=a 使结果天然 <=255,
       钳制只为脏数据兜底。out 生命周期到下次 frame/resize/dispose。 */
    size_t npx = (size_t)b->width * (size_t)b->height;
    const unsigned char *src = b->data;
    unsigned char *dst = b->out;
    for (size_t i = 0; i < npx; i++) {
        unsigned a = src[i * 4 + 3];
        dst[i * 4 + 3] = (unsigned char)a;
        if (a == 255) {
            dst[i * 4 + 0] = src[i * 4 + 0];
            dst[i * 4 + 1] = src[i * 4 + 1];
            dst[i * 4 + 2] = src[i * 4 + 2];
        } else if (a == 0) {
            dst[i * 4 + 0] = 0;
            dst[i * 4 + 1] = 0;
            dst[i * 4 + 2] = 0;
        } else {
            for (int c = 0; c < 3; c++) {
                unsigned v = ((unsigned)src[i * 4 + c] * 255u + a / 2) / a;
                dst[i * 4 + c] = (unsigned char)(v > 255 ? 255 : v);
            }
        }
    }
    *rgba = b->out;
    *width = b->width;
    *height = b->height;
    *stride = b->width * 4;
    return 1;
}

static void cg_dispose(hn_canvas_backend *self) {
    auto *b = (cg_backend *)self;
    if (ci.ready)
        for (int i = 0; i < CG_IMG_CACHE; i++) ci_ent_release(&b->imgs[i]);
    if (b->ctx) { cg.ctx_release(b->ctx); b->ctx = NULL; }
    free(b->data);
    free(b->out);
    free(b);
}

static const hn_canvas_backend_vt CG_VT = {
    cg_resize, cg_op, cg_clear, cg_frame, cg_dispose,
};

// ---- 工厂(与 hn_canvas_backend_ity_create 同款链接/失败口径) ----
static int cg_backend_init(cg_backend *b, int w, int h) {
    b->width = w;
    b->height = h;
    size_t bytes = (size_t)w * (size_t)h * 4;
    b->data = (unsigned char *)malloc(bytes);
    b->out = (unsigned char *)malloc(bytes);
    if (!b->data || !b->out) {
        free(b->data); b->data = NULL;
        free(b->out);  b->out = NULL;
        return 0;
    }
    memset(b->data, 0, bytes);
    memset(b->out, 0, bytes);
    /* kCGImageAlphaPremultipliedLast(=1): 唯一可行的 RGBA8 位图形态,
       非预乘(3)被 CGBitmapContextCreate 拒 —— 详见文件头实测结论。 */
    b->ctx = cg.bmp_create(b->data, (unsigned long)w, (unsigned long)h, 8,
                           (unsigned long)w * 4, g_cs,
                           CG_ALPHA_PREMULTIPLIED_LAST);
    if (!b->ctx) {
        free(b->data); b->data = NULL;
        free(b->out);  b->out = NULL;
        return 0;
    }
    /* y 翻转: CG 原点左下 → 引擎左上, 之后全部 op 坐标免换算直填。 */
    cg.translate(b->ctx, 0.0, (double)h);
    cg.scale(b->ctx, 1.0, -1.0);
    return 1;
}

/* 带 hn_context 的工厂: hc 供 IMAGE/PDATA/PATTERN 经资产后端加载源图;
   NULL = 图像类 op 打一行提示后跳过(其余功能不受影响)。 */
hn_canvas_backend *hn_canvas_backend_cg_create_ctx(int width, int height,
                                                   void *hc) {
    if (width <= 0 || height <= 0) return NULL;
    if (!cg_load()) {
        fprintf(stderr, "[canvas-cg] CoreGraphics 绑定失败, 回落 ledger\n");
        return NULL;
    }
    auto *b = (cg_backend *)calloc(1, sizeof(cg_backend));
    if (!b) return NULL;
    b->vt = &CG_VT;
    b->hc = hc;
    if (!cg_backend_init(b, width, height)) {
        free(b);
        return NULL;
    }
    return (hn_canvas_backend *)b;
}

hn_canvas_backend *hn_canvas_backend_cg_create(int width, int height) {
    return hn_canvas_backend_cg_create_ctx(width, height, NULL);
}

/* ================= paint 分支接线 =================
 * HN_CANVAS_BACKEND=system(别名 cg)时启用; 按画布 id 缓存后端(尺寸
 * 变化重建), 重放账本全部 op → frame 出帧 → HN_CMD_BITMAP(盒映射缩放)。
 * 工厂走**带 ctx 的 create_ctx**(2026-10-15 复审修复: 此前工厂零调用方,
 * CG 面整面不可达, 本 TU 只是链入的死重) —— hn_context 注入后,
 * IMAGE/PDATA/PATTERN 才能经资产后端加载源图。任何一步失败返回 0,
 * 调用方回落 ity/账本重放(页面不坏; cg_load 失败自带一行 stderr)。 */
static int cg_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("HN_CANVAS_BACKEND");
        v = (e && (!strcmp(e, "system") || !strcmp(e, "cg"))) ? 1 : 0;
    }
    return v;
}

#define CG_IDS 8
struct cg_slot { char id[128]; hn_canvas_backend *b; int w, h; };
static cg_slot g_cg_paint_slots[CG_IDS];

int hn_canvas_paint_cg(hn_context *c, const char *id, const hn_canvas_ledger *l,
                       float cw, float ch, float bw, float bh,
                       float sx, float sy, float alpha) {
    /* 缺省口径: 未显式声明 system/cg 一律回落 —— 与 ity 同款声明开关,
       探针/离屏确定性口径不被缺省渲染路径悄悄改写。 */
    if (!cg_enabled()) return 0;
    if (!c || !id || !l || l->n_ops <= 0) return 0;
    int W = (int)cw, H = (int)ch;
    if (W <= 0 || H <= 0) return 0;

    cg_slot *slot = NULL;
    for (int i = 0; i < CG_IDS; i++)
        if (g_cg_paint_slots[i].id[0] && !strcmp(g_cg_paint_slots[i].id, id)) {
            slot = &g_cg_paint_slots[i]; break;
        }
    if (slot && (slot->w != W || slot->h != H)) {
        slot->b->vt->dispose(slot->b);
        slot->b = NULL;
        slot->id[0] = 0;
        slot = NULL;
    }
    if (!slot) {
        for (int i = 0; i < CG_IDS; i++)
            if (!g_cg_paint_slots[i].id[0]) { slot = &g_cg_paint_slots[i]; break; }
        if (!slot) { slot = &g_cg_paint_slots[0];
            slot->b->vt->dispose(slot->b); slot->id[0] = 0; }
        hn_canvas_backend *b = hn_canvas_backend_cg_create_ctx(W, H, c);
        if (!b) return 0;
        slot->b = b; slot->w = W; slot->h = H;
        snprintf(slot->id, sizeof(slot->id), "%s", id);
    }

    /* 新帧清面(与 ity 同一修复): 账本满幅 clearRect/清账不记 op, 面按 id
       跨帧缓存 —— 重放前清成透明底, 否则历史帧内容在持久面上累积(拖影)。 */
    slot->b->vt->clear(slot->b);
    for (int i = 0; i < l->n_ops; i++)
        slot->b->vt->op(slot->b, &l->ops[i]);

    const unsigned char *px = NULL;
    int fw = 0, fh = 0, fstride = 0;
    if (!slot->b->vt->frame(slot->b, &px, &fw, &fh, &fstride)) {
        /* 帧失败: 就地 dispose 后端, 下次重建(与 ity 同一口径)。 */
        slot->b->vt->dispose(slot->b);
        slot->b = NULL;
        slot->id[0] = 0;
        return 0;
    }

    /* 帧拷进引擎 tmp 区: BITMAP 指针生命周期 = 本次显示列表(与 ity/neb3
       路径同一契约)。后端 out 缓冲在下次 frame/resize/dispose 时被
       覆写释放, 不得直接压入显示列表悬留。 */
    size_t need = (size_t)fw * (size_t)fh * 4;
    hn_arena *tmp = hn_context_tmp(c);
    if (!tmp) return 0;
    unsigned char *dst = (unsigned char *)hn_arena_alloc(tmp, need);
    if (!dst) return 0;
    memcpy(dst, px, need);

    hn_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = HN_CMD_BITMAP;
    cmd.bitmap = dst;
    cmd.bitmap_stride = fstride;
    cmd.bitmap_w = fw; cmd.bitmap_h = fh;
    cmd.x = sx; cmd.y = sy; cmd.w = bw; cmd.h = bh;
    /* alpha 字节 0..255 截断(引擎恒供 0..1; 与 ity/hn_three 同一纪律)。 */
    float a = alpha < 0.0f ? 0.0f : (alpha > 1.0f ? 1.0f : alpha);
    cmd.fill = 0xFFFFFF00u | (unsigned)(a * 255.0f + 0.5f);
    hn_paint_push(c, &cmd);
    return 1;
}

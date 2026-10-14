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
//          映射(CGContextSetBlendMode, 非"无逐调用混合")。
// v1 暂不支持(遇到打一次 stderr 并跳过, 不静默刷屏): IMAGE/PDATA
//          /PATTERN_RECT/PATTERN_PATH —— 同 ity 的 v1 清单, 见主接线。
//
// 纪律:
//   - dlsym(RTLD_DEFAULT) 绑定; CoreGraphics 必需(缺则工厂返回 NULL,
//     调用方回落 ledger), CoreText 可选(缺则 TEXT 一次性 stderr 跳过)。
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
    return true;
}

// ---- 后端对象 ----
struct cg_backend : hn_canvas_backend {
    CGContextRef ctx;          /* 预乘 RGBA8 位图上下文(y 已翻转) */
    unsigned char *data;       /* 上下文像素(自有 malloc, stride = w*4 恒定) */
    unsigned char *out;        /* 帧末去预乘输出(frame 契约面) */
    unsigned char warned[8];
};

enum {  /* warned 槽位 */
    W_TEXT = 0,      /* CoreText 不可用 */
    W_IMAGE = 1,
    W_PDATA = 2,
    W_PATTERN = 3,
    W_STOPS = 4,     /* 渐变 stop 截断 */
    W_OP = 5,        /* 未知 op */
};
#define W_N 8

static void warn_once(cg_backend *b, int slot, const char *msg) {
    if (slot >= 0 && slot < W_N) {
        if (b->warned[slot]) return;
        b->warned[slot] = 1;
    }
    fprintf(stderr, "[canvas-cg] 暂不支持/受限: %s(该 op 已跳过)\n", msg);
}

static void rgba_set(CGContextRef ctx, unsigned c, bool stroke) {
    double r = (double)((c >> 16) & 255) / 255.0;
    double g = (double)((c >> 8) & 255) / 255.0;
    double bl = (double)(c & 255) / 255.0;
    double a = (double)((c >> 24) & 255) / 255.0;
    if (stroke) cg.set_stroke_rgba(ctx, r, g, bl, a);
    else        cg.set_fill_rgba(ctx, r, g, bl, a);
}

static CGColorRef make_color(unsigned c) {
    double comps[4] = {
        (double)((c >> 16) & 255) / 255.0, (double)((c >> 8) & 255) / 255.0,
        (double)(c & 255) / 255.0, (double)((c >> 24) & 255) / 255.0,
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
            comps[i * 4 + 0] = (double)((col >> 16) & 255) / 255.0;
            comps[i * 4 + 1] = (double)((col >> 8) & 255) / 255.0;
            comps[i * 4 + 2] = (double)(col & 255) / 255.0;
            comps[i * 4 + 3] = (double)((col >> 24) & 255) / 255.0;
            locs[i] = (double)o->stops[i].t;
        }
        n = (unsigned long)cnt;
    } else {
        unsigned c0 = (unsigned)o->color, c1 = (unsigned)o->color2;
        comps[0] = (double)((c0 >> 16) & 255) / 255.0;
        comps[1] = (double)((c0 >> 8) & 255) / 255.0;
        comps[2] = (double)(c0 & 255) / 255.0;
        comps[3] = (double)((c0 >> 24) & 255) / 255.0;
        comps[4] = (double)((c1 >> 16) & 255) / 255.0;
        comps[5] = (double)((c1 >> 8) & 255) / 255.0;
        comps[6] = (double)(c1 & 255) / 255.0;
        comps[7] = (double)((c1 >> 24) & 255) / 255.0;
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

// ---- vtable ----
static int cg_backend_init(cg_backend *b, int w, int h);

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
    default: {
        int slot;
        switch (o->kind) {
        case HN_CV_IMAGE:        slot = W_IMAGE; break;
        case HN_CV_PDATA:        slot = W_PDATA; break;
        case HN_CV_PATTERN_RECT:
        case HN_CV_PATTERN_PATH: slot = W_PATTERN; break;
        default:                 slot = W_OP; break;
        }
        warn_once(b, slot, "该 op 见文件头 v1 清单");
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
    if (b->ctx) { cg.ctx_release(b->ctx); b->ctx = NULL; }
    free(b->data);
    free(b->out);
    free(b);
}

static const hn_canvas_backend_vt CG_VT = {
    cg_resize, cg_op, cg_frame, cg_dispose,
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

hn_canvas_backend *hn_canvas_backend_cg_create(int width, int height) {
    if (width <= 0 || height <= 0) return NULL;
    if (!cg_load()) {
        fprintf(stderr, "[canvas-cg] CoreGraphics 绑定失败, 回落 ledger\n");
        return NULL;
    }
    auto *b = (cg_backend *)calloc(1, sizeof(cg_backend));
    if (!b) return NULL;
    b->vt = &CG_VT;
    if (!cg_backend_init(b, width, height)) {
        free(b);
        return NULL;
    }
    return (hn_canvas_backend *)b;
}

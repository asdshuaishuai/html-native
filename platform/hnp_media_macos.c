/* hnp_media_macos.c — macOS 媒体实现(dlopen AVFoundation/CoreMedia/CoreVideo)
 *
 * hn_platform.h 媒体追加段的 macOS 实现, 与 hnp_macos.c 完全同一手法:
 * 纯 C99, 运行期 dlopen + dlsym(objc_getClass/sel_registerName/objc_msgSend),
 * 零 ObjC 头文件 / 零 .m / 链接期不依赖任何 framework。
 *
 * 分工(docs/media-design.md §0): 解码委托 OS 媒体框架 —— AVPlayer 负责解码
 * 与音频出声(不另起炉灶, 纯音频同入口只是没有帧); 视频帧经
 * AVPlayerItemVideoOutput 拉取 CVPixelBuffer, 本文件做 NV12→RGBA 转换后
 * 拷给调用方。
 *
 * 实机验证过的坑(docs/media-design.md §10, 实现逐条照办, 别再踩):
 *  1) dlsym 必须用 RTLD_DEFAULT(= (void*)-2), 不是 NULL(macOS 上
 *     dlsym(NULL,…) 恒失败);
 *  2) NSApplication finishLaunching 必须先于 AVPlayer 异步加载,
 *     否则 duration 永远 NaN(hnp_init 已装过则幂等);
 *  3) runloop 泵必须有界日期 —— distantFuture 在 pause 态永久阻塞(实测挂死);
 *  4) CVPixelBuffer 实际产 '420v' planar(请求 BGRA 的 attributes 被忽略),
 *     平面 stride 首选 GetBytesPerRowOfPlane —— 但它**会说谎**(macOS 27
 *     实测过个别运行返回 26 倍垃圾值: 400px 帧给 13312, 真实 512, 按它
 *     直读 SEGV), 所以每个 stride 都以 CVPixelBufferGetDataSize 总长为界
 *     校验(sane_stride), 不合法回退 GetBytesPerRow/紧排宽度;
 *  5) pause 态直接 seek 视频轨停在 0.000 —— 必须 play→seek→settle→pause;
 *  6) ended 用 position>=duration 推导(rate 播完仍 1.00, 不可靠);
 *  7) CMTime(24B 结构体)按值传/收, 函数指针原型写对即可(arm64 实测正确)。
 *
 * 明确不做(media-design.md §9): http(s) 流媒体(open 如实失败)、字幕、DRM、
 * FFmpeg、wasm 解码、播放器 UI、playbackRate。单线程使用(与引擎同一约定)。
 */
#ifdef __APPLE__   /* 仅苹果平台编入; 构建脚本也只把它入列 macos 目标 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <errno.h>
#include <dlfcn.h>
#include <sys/stat.h>

#include "hn_platform.h"

typedef void *obj_t;
typedef void *sel_t;

/* —— objc 运行时入口(dlsym RTLD_DEFAULT, 与 hnp_macos.c:60-62 同款) —— */
static obj_t (*o_getClass)(const char *);
static sel_t (*o_sel)(const char *);
static obj_t (*o_msgSend)(obj_t, sel_t, ...);

#define MSG0(RET, O, NAME) ((RET (*)(obj_t, sel_t))o_msgSend)((O), o_sel(NAME))
#define MSG1(RET, O, NAME, A1) \
    ((RET (*)(obj_t, sel_t, obj_t))o_msgSend)((O), o_sel(NAME), (obj_t)(A1))

/* CMTime(24B: value/timescale/flags/epoch, 取自 CoreMedia/CMTime.h 布局)。
   按值传给 objc_msgSend / 按值接收, 原型即真相(§10.7, arm64 实测正确)。 */
typedef int64_t  cm_time_value;
typedef int32_t  cm_time_scale;
typedef uint32_t cm_time_flags;
typedef int64_t  cm_time_epoch;
typedef struct {
    cm_time_value  value;
    cm_time_scale  timescale;
    cm_time_flags  flags;
    cm_time_epoch  epoch;
} CMTime;

#define CM_TIME_VALID 1u          /* kCMTimeFlags_Valid(CMTime.h) */
#define CM_TIME_SCALE 600         /* AVPlayer 推荐时标上界 */

/* CGSize(2×CGFloat = 2×double)按值收, HFA 走寄存器(与 hnp_macos.c 同款) */
typedef struct { double w, h; } CGSize;

/* CoreMedia/CoreVideo 的纯 C 函数(dlopen 后 dlsym) */
static double   (*f_CMTimeGetSeconds)(CMTime);
static void     (*f_CFRelease)(const void *);
static size_t   (*f_CVPixelBufferGetWidth)(const void *);
static size_t   (*f_CVPixelBufferGetHeight)(const void *);
static int      (*f_CVPixelBufferLockBaseAddress)(const void *, uint32_t);
static int      (*f_CVPixelBufferUnlockBaseAddress)(const void *, uint32_t);
static void    *(*f_CVPixelBufferGetBaseAddress)(const void *);
static size_t   (*f_CVPixelBufferGetBytesPerRow)(const void *);
static void    *(*f_CVPixelBufferGetBaseAddressOfPlane)(const void *, size_t);
static size_t   (*f_CVPixelBufferGetBytesPerRowOfPlane)(const void *, size_t);
static size_t   (*f_CVPixelBufferGetPlaneCount)(const void *);
static size_t   (*f_CVPixelBufferGetDataSize)(const void *);

#define CV_LOCK_READONLY 1u       /* kCVPixelBufferLock_ReadOnly(CoreVideo) */

static int g_once;   /* 0=未初始化, 1=OK, -1=失败(如实进 open 的 err) */

static obj_t nsstr(const char *s) {
    return ((obj_t (*)(obj_t, sel_t, const char *))o_msgSend)(
        o_getClass("NSString"), o_sel("stringWithUTF8String:"), s);
}

/* alloc+init 两步走(本文件全部手动引用计数, 不依赖 ARC) */
static obj_t ns_alloc_init(obj_t cls) {
    obj_t o = MSG0(obj_t, cls, "alloc");
    return o ? MSG0(obj_t, o, "init") : NULL;
}

/* 有界 runloop 泵: duration/position/state/frame/seek 每次被调内部跑一次。
   beforeDate = now+10ms。**禁止 distantFuture**(pause 态永久阻塞, §10.3)。
   泵外套 autorelease 池: playerWithURL: 这类便利构造是 autoreleased,
   长播放不泵池会无限堆积。 */
static void media_pump(void) {
    if (g_once <= 0) return;
    obj_t pool = ns_alloc_init(o_getClass("NSAutoreleasePool"));
    obj_t lim = ((obj_t (*)(obj_t, sel_t, double))o_msgSend)(
        o_getClass("NSDate"), o_sel("dateWithTimeIntervalSinceNow:"), 0.010);
    obj_t rl = MSG0(obj_t, o_getClass("NSRunLoop"), "mainRunLoop");
    ((int (*)(obj_t, sel_t, obj_t, obj_t))o_msgSend)(
        rl, o_sel("runMode:beforeDate:"), nsstr("kCFRunLoopDefaultMode"), lim);
    if (pool) MSG0(void, pool, "drain");
}

static int media_init_once(void) {
    if (g_once) return g_once > 0;
    /* 幂等 dlopen(dlopen 对已装载框架只加引用计数; AVF 会连带带出
       CoreMedia/CoreVideo, 这里显式装以便 dlsym 平面 API; CoreAudio 是
       AVPlayer 音轨的下游, 缺了不致命 —— 音频由 AVPlayer 自带出声)。 */
    if (!dlopen("/System/Library/Frameworks/AVFoundation.framework/AVFoundation", RTLD_NOW) ||
        !dlopen("/System/Library/Frameworks/AppKit.framework/AppKit", RTLD_NOW) ||
        !dlopen("/System/Library/Frameworks/CoreMedia.framework/CoreMedia", RTLD_NOW) ||
        !dlopen("/System/Library/Frameworks/CoreVideo.framework/CoreVideo", RTLD_NOW)) {
        g_once = -1;
        return 0;
    }
    dlopen("/System/Library/Frameworks/CoreAudio.framework/CoreAudio", RTLD_NOW);
    o_getClass = (obj_t (*)(const char *))dlsym(RTLD_DEFAULT, "objc_getClass");
    o_sel      = (sel_t (*)(const char *))dlsym(RTLD_DEFAULT, "sel_registerName");
    o_msgSend  = (obj_t (*)(obj_t, sel_t, ...))dlsym(RTLD_DEFAULT, "objc_msgSend");
    f_CMTimeGetSeconds = (double (*)(CMTime))dlsym(RTLD_DEFAULT, "CMTimeGetSeconds");
    f_CFRelease        = (void (*)(const void *))dlsym(RTLD_DEFAULT, "CFRelease");
    f_CVPixelBufferGetWidth      = (size_t (*)(const void *))dlsym(RTLD_DEFAULT, "CVPixelBufferGetWidth");
    f_CVPixelBufferGetHeight     = (size_t (*)(const void *))dlsym(RTLD_DEFAULT, "CVPixelBufferGetHeight");
    f_CVPixelBufferLockBaseAddress   = (int (*)(const void *, uint32_t))dlsym(RTLD_DEFAULT, "CVPixelBufferLockBaseAddress");
    f_CVPixelBufferUnlockBaseAddress = (int (*)(const void *, uint32_t))dlsym(RTLD_DEFAULT, "CVPixelBufferUnlockBaseAddress");
    f_CVPixelBufferGetBaseAddress    = (void *(*)(const void *))dlsym(RTLD_DEFAULT, "CVPixelBufferGetBaseAddress");
    f_CVPixelBufferGetBytesPerRow    = (size_t (*)(const void *))dlsym(RTLD_DEFAULT, "CVPixelBufferGetBytesPerRow");
    f_CVPixelBufferGetBaseAddressOfPlane = (void *(*)(const void *, size_t))dlsym(RTLD_DEFAULT, "CVPixelBufferGetBaseAddressOfPlane");
    f_CVPixelBufferGetBytesPerRowOfPlane = (size_t (*)(const void *, size_t))dlsym(RTLD_DEFAULT, "CVPixelBufferGetBytesPerRowOfPlane");
    f_CVPixelBufferGetPlaneCount     = (size_t (*)(const void *))dlsym(RTLD_DEFAULT, "CVPixelBufferGetPlaneCount");
    f_CVPixelBufferGetDataSize       = (size_t (*)(const void *))dlsym(RTLD_DEFAULT, "CVPixelBufferGetDataSize");
    if (!o_getClass || !o_sel || !o_msgSend || !f_CMTimeGetSeconds || !f_CFRelease ||
        !f_CVPixelBufferGetWidth || !f_CVPixelBufferGetHeight ||
        !f_CVPixelBufferLockBaseAddress || !f_CVPixelBufferUnlockBaseAddress ||
        !f_CVPixelBufferGetBaseAddress || !f_CVPixelBufferGetBytesPerRow ||
        !f_CVPixelBufferGetBaseAddressOfPlane || !f_CVPixelBufferGetBytesPerRowOfPlane ||
        !f_CVPixelBufferGetPlaneCount || !f_CVPixelBufferGetDataSize) {
        g_once = -1;
        return 0;
    }
    /* finishLaunching 必须先于 AVPlayer 异步加载(§10.2);
       hnp_init 已装过 AppKit 时 sharedApplication 幂等返回既有实例。 */
    obj_t app = MSG0(obj_t, o_getClass("NSApplication"), "sharedApplication");
    if (!app) { g_once = -1; return 0; }
    MSG0(void, app, "finishLaunching");
    g_once = 1;
    return 1;
}

static void set_err(char *err, size_t cap, const char *msg) {
    if (err && cap) snprintf(err, cap, "%s", msg);
}

struct hnp_media {
    obj_t player, item, output;
    double duration;               /* 最近确认的时长(秒); duration_known 时有效 */
    int duration_known, playing;   /* playing = play/pause 的内部记账(seek 序列用) */
    int last_w, last_h, has_dims;  /* 最近一帧尺寸(frame 返回 0 时仍须写出) */
};

int hnp_media_supported(void) { return 1; }

hnp_media *hnp_media_open(const char *url, char *err, size_t err_cap) {
    if (err && err_cap) err[0] = '\0';
    if (!media_init_once()) { set_err(err, err_cap, "dlopen AVFoundation/AppKit/CoreMedia/CoreVideo 失败"); return NULL; }
    if (!url || !*url) { set_err(err, err_cap, "url 为空"); return NULL; }
    if (strncmp(url, "http://", 7) == 0 || strncmp(url, "https://", 8) == 0) {
        set_err(err, err_cap, "不支持 http(s) 流媒体(只收本地路径/file://, media-design §9)");
        return NULL;
    }
    /* file:// 前缀剥成路径(不做百分号解码 —— 本设计只承诺本地文件) */
    char path[4096];
    if (strncmp(url, "file://", 7) == 0) snprintf(path, sizeof path, "%s", url + 7);
    else snprintf(path, sizeof path, "%s", url);
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) {
        if (err && err_cap)
            snprintf(err, err_cap, "无法打开本地文件 %s: %s", path, strerror(errno));
        return NULL;
    }

    hnp_media *m = (hnp_media *)calloc(1, sizeof(*m));
    if (!m) { set_err(err, err_cap, "内存不足"); return NULL; }

    obj_t pool = ns_alloc_init(o_getClass("NSAutoreleasePool"));
    obj_t u = MSG1(obj_t, o_getClass("NSURL"), "fileURLWithPath:", nsstr(path));
    obj_t player = u ? MSG1(obj_t, o_getClass("AVPlayer"), "playerWithURL:", u) : NULL;
    obj_t item = player ? MSG0(obj_t, player, "currentItem") : NULL;
    /* 帧输出对象: attributes 传 NULL —— 实测请求 BGRA 也会被忽略,
       实际产 '420v' planar(§10.4), 索性不装。本运行时只有
       initWithPixelBufferAttributes: 变体(无 dispatchQueue 变体, §10.8)。 */
    obj_t out = ns_alloc_init(o_getClass("AVPlayerItemVideoOutput"));
    if (out)
        out = ((obj_t (*)(obj_t, sel_t, obj_t))o_msgSend)(
            out, o_sel("initWithPixelBufferAttributes:"), NULL);
    if (out && item) MSG1(void, item, "addOutput:", out);
    if (pool) MSG0(void, pool, "drain");

    if (!player || !item || !out) {
        if (out) f_CFRelease(out);                 /* 平衡 alloc(未被 item 持有则立即释放) */
        if (player) { MSG0(void, player, "release"); }  /* 见下: retain 配对 */
        free(m);
        set_err(err, err_cap, "AVPlayer/AVPlayerItemVideoOutput 创建失败");
        return NULL;
    }
    MSG0(void, player, "retain");  /* playerWithURL: 便利构造是 autoreleased,
                                      自己 +1(池已 drain), close 时 release 配平 */
    m->player = player; m->item = item; m->output = out;
    return m;
}

void hnp_media_close(hnp_media *m) {
    if (!m) return;   /* 幂等; NULL 安全 */
    if (g_once > 0 && m->player) {
        obj_t pool = ns_alloc_init(o_getClass("NSAutoreleasePool"));
        MSG0(void, m->player, "pause");
        MSG1(void, m->player, "replaceCurrentItemWithPlayerItem:", NULL); /* 摘 item(连带 output) */
        if (m->output) f_CFRelease(m->output);     /* 平衡 open 的 alloc */
        MSG0(void, m->player, "release");          /* 平衡 open 的 retain */
        if (pool) MSG0(void, pool, "drain");
    }
    free(m);
}

int hnp_media_play(hnp_media *m) {
    if (!m || !m->player || g_once <= 0) return 0;
    MSG0(void, m->player, "play");
    m->playing = 1;
    return 1;
}

void hnp_media_pause(hnp_media *m) {
    if (!m || !m->player || g_once <= 0) return;
    MSG0(void, m->player, "pause");
    m->playing = 0;
}

static CMTime sec_to_cm(double s) {
    CMTime t;
    t.value = (int64_t)(s * (double)CM_TIME_SCALE + (s >= 0 ? 0.5 : -0.5));
    t.timescale = CM_TIME_SCALE;
    t.flags = CM_TIME_VALID;
    t.epoch = 0;
    return t;
}

static double cm_to_sec(CMTime t) {
    double s = f_CMTimeGetSeconds(t);
    return (s == s && s > 0.0) ? s : 0.0;   /* NaN/负值按 0 报告 */
}

static double cur_position(hnp_media *m) {
    return cm_to_sec(((CMTime (*)(obj_t, sel_t))o_msgSend)(m->player, o_sel("currentTime")));
}

/* duration 异步解析; 每次元数据被问时顺手刷新(就绪即记账) */
static void refresh_duration(hnp_media *m) {
    if (m->duration_known || !m->item) return;
    CMTime d = ((CMTime (*)(obj_t, sel_t))o_msgSend)(m->item, o_sel("duration"));
    double s = f_CMTimeGetSeconds(d);
    if (s == s && s > 0.0) { m->duration = s; m->duration_known = 1; }
}

int hnp_media_seek(hnp_media *m, double sec) {
    if (!m || !m->player || g_once <= 0) return 0;
    media_pump();
    refresh_duration(m);
    /* 钳制到 [0, duration](已知时; 未知只钳下界) */
    if (m->duration_known && m->duration > 0 && sec > m->duration) sec = m->duration;
    if (sec < 0) sec = 0;
    CMTime t = sec_to_cm(sec);
    if (m->playing) {
        /* 播放态直接 seek(§10.5 只在 pause 态有问题) */
        ((void (*)(obj_t, sel_t, CMTime))o_msgSend)(m->player, o_sel("seekToTime:"), t);
        return 1;
    }
    /* pause 态: 实测直接 seek 视频轨停在 0.000 —— 统一走
       play→seekToTime→轮询落位(|Δ|<0.05 连续 2 次, ≤2s 上限)→pause(§10.5) */
    MSG0(void, m->player, "play");
    ((void (*)(obj_t, sel_t, CMTime))o_msgSend)(m->player, o_sel("seekToTime:"), t);
    for (int i = 0, stable = 0; i < 200 && stable < 2; i++) {
        media_pump();
        double p = cur_position(m);
        double d = p - sec;
        if (d < 0) d = -d;
        stable = (d < 0.05) ? stable + 1 : 0;
    }
    MSG0(void, m->player, "pause");   /* 回到进入 seek 时的暂停态 */
    return 1;
}

void hnp_media_set_volume(hnp_media *m, float vol) {
    if (!m || !m->player || g_once <= 0) return;
    if (!(vol >= 0)) vol = 0;        /* NaN 一并钳到 0 */
    if (vol > 1) vol = 1;            /* 越界钳制到 0..1 */
    ((void (*)(obj_t, sel_t, float))o_msgSend)(m->player, o_sel("setVolume:"), vol);
}

void hnp_media_set_muted(hnp_media *m, int muted) {
    if (!m || !m->player || g_once <= 0) return;
    ((void (*)(obj_t, sel_t, int))o_msgSend)(m->player, o_sel("setMuted:"), muted ? 1 : 0);
}

int hnp_media_duration(hnp_media *m, double *sec_out) {
    if (!m || !m->item || g_once <= 0) return 0;
    media_pump();
    refresh_duration(m);
    if (!m->duration_known) {        /* 直播流/未解析出: 未知写 -1 返回 0 */
        if (sec_out) *sec_out = -1.0;
        return 0;
    }
    if (sec_out) *sec_out = m->duration;
    return 1;
}

int hnp_media_position(hnp_media *m, double *sec_out) {
    if (!m || !m->player || g_once <= 0) return 0;
    media_pump();
    if (sec_out) *sec_out = cur_position(m);
    return 1;
}

int hnp_media_state(hnp_media *m, int *ready, int *ended) {
    if (!m || !m->player || g_once <= 0) return 0;
    media_pump();
    refresh_duration(m);
    /* ready = 元数据就绪(时长已知); ended = position≥duration-1e-3 推导
       (rate 播完不归零, 不可靠, §10.6)。两者可同时为 1。 */
    if (ready) *ready = m->duration_known ? 1 : 0;
    if (ended) *ended = (m->duration_known && m->duration > 0 &&
                         cur_position(m) >= m->duration - 1e-3) ? 1 : 0;
    return 1;
}

/* stride 合法性防线: 声称值必须让"整帧读入"不越过分配区总长, 否则依次
   回退(备选值 → 宽度紧排)。返回值保证 >= w 且 (h-1)*stride+w <= total。 */
static size_t sane_stride(size_t claim, size_t fallback, size_t w, size_t h, size_t total) {
    if (claim >= w && (h - 1) * claim + w <= total) return claim;
    if (fallback >= w && (h - 1) * fallback + w <= total) return fallback;
    return w;
}

/* CVPixelBuffer → RGBA8 非预乘, 原点左上、顶上到底下。
   实际缓冲是 '420v'/'420f' NV12 双平面(BGRA 请求被忽略, §10.4):
   plane0=Y, plane1=交织 UV(次采样), stride 优先取 GetBytesPerRowOfPlane。
   实测补充(macOS 27 / 2026-10, media_probe 跑出来的): 个别运行里
   GetBytesPerRowOfPlane 返回 ~26 倍垃圾值(400px 帧给 13312, 真实 512),
   直接按它做行距 258 行会冲出分配区 SEGV —— 因此每个 stride 都过
   sane_stride(以 CVPixelBufferGetDataSize 的总长为界), 不合法即回退。
   非双平面缓冲按 32bpp BGRA 兜底。色彩: BT.601 limited(video range)。 */
static unsigned char clampb(int v) { return v < 0 ? 0 : v > 255 ? 255 : (unsigned char)v; }

static void pb_to_rgba(const void *pb, unsigned char *dst, int w, int h) {
    f_CVPixelBufferLockBaseAddress(pb, CV_LOCK_READONLY);
    size_t total = f_CVPixelBufferGetDataSize(pb);
    size_t planes = f_CVPixelBufferGetPlaneCount(pb);
    if (planes >= 2) {
        const unsigned char *Y  = (const unsigned char *)f_CVPixelBufferGetBaseAddressOfPlane(pb, 0);
        const unsigned char *UV = (const unsigned char *)f_CVPixelBufferGetBaseAddressOfPlane(pb, 1);
        size_t sy  = sane_stride(f_CVPixelBufferGetBytesPerRowOfPlane(pb, 0),
                                 f_CVPixelBufferGetBytesPerRow(pb), (size_t)w, (size_t)h, total);
        size_t suv = sane_stride(f_CVPixelBufferGetBytesPerRowOfPlane(pb, 1),
                                 sy / 2, (size_t)w / 2, (size_t)(h + 1) / 2, total);
        for (int y = 0; y < h; y++) {
            const unsigned char *yrow  = Y + (size_t)y * sy;
            const unsigned char *uvrow = UV + (size_t)(y >> 1) * suv;
            unsigned char *d = dst + (size_t)y * (size_t)w * 4;
            for (int x = 0; x < w; x++) {
                double yv = ((int)yrow[x] - 16) * (255.0 / 219.0);
                int u = (int)uvrow[(x >> 1) << 1] - 128;
                int v = (int)uvrow[((x >> 1) << 1) + 1] - 128;
                double uf = u * (255.0 / 224.0), vf = v * (255.0 / 224.0);
                d[0] = clampb((int)(yv + 1.402 * vf + 0.5));                 /* R */
                d[1] = clampb((int)(yv - 0.344136 * uf - 0.714136 * vf + 0.5)); /* G */
                d[2] = clampb((int)(yv + 1.772 * uf + 0.5));                 /* B */
                d[3] = 255;   /* 媒体帧无 alpha, 输出不透明 */
                d += 4;
            }
        }
    } else {
        const unsigned char *base = (const unsigned char *)f_CVPixelBufferGetBaseAddress(pb);
        size_t stride = f_CVPixelBufferGetBytesPerRow(pb);
        if (base) {
            if (stride < (size_t)w * 4) stride = (size_t)w * 4;
            if ((size_t)(h - 1) * stride + (size_t)w * 4 > total) stride = (size_t)w * 4;
            for (int y = 0; y < h; y++) {
                const unsigned char *s = base + (size_t)y * stride;
                unsigned char *d = dst + (size_t)y * (size_t)w * 4;
                for (int x = 0; x < w; x++) {   /* BGRA → RGBA */
                    d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
                    s += 4; d += 4;
                }
            }
        }
    }
    f_CVPixelBufferUnlockBaseAddress(pb, CV_LOCK_READONLY);
}

int hnp_media_frame(hnp_media *m, unsigned char *rgba, size_t cap,
                    int *w, int *h, double *pts_sec) {
    if (w) *w = 0;
    if (h) *h = 0;
    if (pts_sec) *pts_sec = 0.0;
    if (!m || !m->player || !m->output || g_once <= 0) return -1;
    media_pump();
    CMTime t = ((CMTime (*)(obj_t, sel_t))o_msgSend)(m->player, o_sel("currentTime"));
    int has = ((int (*)(obj_t, sel_t, CMTime))o_msgSend)(
        m->output, o_sel("hasNewPixelBufferForItemTime:"), t);
    if (!has) {
        if (!m->has_dims) {
            /* 从未有过帧: 判是否根本没有视频轨(presentationSize 为 0 = 纯音频) */
            CGSize ps = MSG0(CGSize, m->item, "presentationSize");
            return (ps.w <= 0 || ps.h <= 0) ? -1 : 0;
        }
        if (w) *w = m->last_w;   /* 内容未变: 复用缓存, meta 仍有效 */
        if (h) *h = m->last_h;
        return 0;
    }
    obj_t pb = ((obj_t (*)(obj_t, sel_t, CMTime, obj_t))o_msgSend)(
        m->output, o_sel("copyPixelBufferForItemTime:itemTimeForDisplay:"), t, NULL);
    if (!pb) {
        if (m->has_dims) { if (w) *w = m->last_w; if (h) *h = m->last_h; return 0; }
        return -1;
    }
    int pw = (int)f_CVPixelBufferGetWidth(pb);
    int ph = (int)f_CVPixelBufferGetHeight(pb);
    m->last_w = pw; m->last_h = ph; m->has_dims = 1;
    if (w) *w = pw;
    if (h) *h = ph;
    double pts = f_CMTimeGetSeconds(t);
    if (pts_sec) *pts_sec = (pts == pts) ? pts : 0.0;
    if (!rgba || (size_t)pw * (size_t)ph * 4 > cap) {
        /* cap 不足以容纳整帧: 写出真实 *w/*h(即"按 meta 重新分配"的 meta),
           pts 置 -1, 返回 -1 —— 调用方按 meta 重分配后重试 */
        if (pts_sec) *pts_sec = -1.0;
        f_CFRelease(pb);
        return -1;
    }
    pb_to_rgba(pb, rgba, pw, ph);
    f_CFRelease(pb);   /* copy 出来的 buffer 归调用方释放 */
    return 1;
}

#endif /* __APPLE__ */

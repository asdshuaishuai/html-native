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
 * 原生视频图层(真窗 GPU 合成, hn_media_platform/hn_app 接线): open/close
 * 维护 src→player 注册表, hnp_video_layer_*(实现在 hnp_macos.c, 本文件的
 * 会话私有成员经 hnp_media_player_for_src / set_layer_mode / layer_mode
 * 三个最小查询口暴露)把 AVPlayerLayer 挂进窗口图层树 —— 图层模式下
 * hnp_media_frame 早退, CPU 拉帧/转换整链归零(见 layer_mode 字段注释)。
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
 *  7) CMTime(24B 结构体)按值传/收, 函数指针原型写对即可(arm64 实测正确);
 *  8) loop 回绕根治(2026-10): actionAtItemEnd=None(撞尾不 pause, 补发的
 *     play 不再被吃) + 到尾通知 AVPlayerItemDidPlayToEndTimeNotification
 *     的 selector 观察者直接 seek(0)+play —— position≈duration 启发式只兜
 *     首圈(loop 闩锁后退役), 风暴与秒级停顿都从根上消失, 见"到尾通知"段。
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
#include <time.h>

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
static unsigned int (*f_CVPixelBufferGetPixelFormatType)(const void *);

/* libdispatch 的 _f 变体(dlsym 即得 —— libdispatch 经 libSystem 在每个
   进程在场): 到尾通知若被 AVFoundation 在非主线程投递, 用它把处理转交
   主线程。零 block(block 字面量要 ObjC 编译器支持), _f 变体收 C 函数
   指针 + 上下文指针, 与本文件"纯 C99 + 运行期 dlsym"同口径。 */
static void (*f_dispatch_async_f)(obj_t, void *, void (*)(void *));
static obj_t g_main_queue;   /* dlsym "_dispatch_main_q"(导出的队列对象) */
static obj_t g_media_obs;    /* 到尾通知观察者单例(进程同寿, 永不释放) */

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
    /* 到尾通知非主线程投递时的主线程转交(缺了不致命 —— 退化为就地处理) */
    f_dispatch_async_f = (void (*)(obj_t, void *, void (*)(void *)))
                         dlsym(RTLD_DEFAULT, "dispatch_async_f");
    g_main_queue       = dlsym(RTLD_DEFAULT, "_dispatch_main_q");
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
    f_CVPixelBufferGetPixelFormatType = (unsigned int (*)(const void *))dlsym(RTLD_DEFAULT, "CVPixelBufferGetPixelFormatType");
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
    double stall_pos;              /* 卡死自愈: 上一次 position 采样值 */
    int stall_cnt;                 /* 同值连续采样计数(playing 态专用) */
    int layer_mode;                /* 1 = 原生 AVPlayerLayer 直出(帧不再外送,
                                      hnp_media_frame 早退 —— CPU 拉帧/转换归零) */
    /* —— loop 回绕根治(到尾通知主路径, 2026-10) ——
       旧路线: 引擎靠 position≈duration 启发式轮询触发 seek(0), 落位窗口里
       每 16ms 重触发(seek 风暴), 且播放器撞尾自动 pause 吃掉补发的 play
       (最坏 ≤2s 停顿)。现: actionAtItemEnd=None 撞尾不 pause; 到尾系统
       通知(AVPlayerItemDidPlayToEndTimeNotification)经观察者直接回绕;
       引擎在"到尾后 1s 内"发 seek(0)+play 即闩为 loop 会话 —— 平台无 loop
       属性 ABI(hn_platform.h 冻结), 以引擎回绕签名推断, 首wrap仍走引擎
       (启发式只兜第一圈, 之后通知是主路径; 0.5s 停滞自愈继续兜底)。 */
    int obs_registered;            /* 到尾通知已注册(close 配对摘除, 防悬空) */
    int loop_latched;              /* 引擎回绕签名已见 → 后续到尾通知直接回绕 */
    int wrap_pending;              /* 通知回绕 seek 在途: position 对旧尾读数报 0 */
    int ended_seen;                /* 到尾通知累计次数(诊断/探针观测点) */
    int wrap_cnt;                  /* 通知驱动回绕累计次数(探针观测点) */
    int seek_cnt;                  /* seek 累计次数(探针 seek 风暴观测点) */
    double ended_wall;             /* 最近一次到尾通知的单调时刻(now_mono) */
};

int hnp_media_supported(void) { return 1; }

/* 到尾通知的进程级装配与会话级注册/摘除(实现在通知段; open 调 setup+
   watch, close 调 watch(m,0) —— 必须配对摘除, 否则 NSNotificationCenter
   持 item 引用悬空/解码器资源滞留)。 */
static void media_note_setup(void);
static void media_note_watch(hnp_media *m, int on);

/* ---- src → 会话注册表(原生视频图层的查询口) ----
   open 成功登记(url 原串为键 —— 引擎会话传给 open 的 src 与 <video src>
   属性同串, app 侧 walk 即可用属性值寻到同一 player), close 摘除。
   容量 16(页面同时可播的视频数; 满则新开不入表 —— 图层模式对该会话
   优雅退 CPU 逐帧, 行为与 HN_MEDIA_LAYER=0 一致)。单线程使用。 */
#define HN_MEDIA_REG_CAP 16
static struct { char *src; hnp_media *m; } g_reg[HN_MEDIA_REG_CAP];
static int g_reg_n;

static void reg_add(const char *url, hnp_media *m) {
    if (g_reg_n >= HN_MEDIA_REG_CAP) return;
    size_t n = strlen(url);
    char *cp = (char *)malloc(n + 1);
    if (!cp) return;
    memcpy(cp, url, n + 1);
    g_reg[g_reg_n].src = cp;
    g_reg[g_reg_n].m = m;
    g_reg_n++;
}

static void reg_del(const hnp_media *m) {
    for (int i = 0; i < g_reg_n; i++) {
        if (g_reg[i].m == m) {
            free(g_reg[i].src);
            g_reg[i] = g_reg[g_reg_n - 1];
            g_reg_n--;
            i--;
        }
    }
}

/* 查注册表返回该 src 的 AVPlayer(未登记/open 失败/已 close 返回 NULL)。
   消费方: hnp_macos.c 的 hnp_video_layer_attach(原生图层直通挂载)。 */
void *hnp_media_player_for_src(const char *src) {
    if (!src || !*src) return NULL;
    for (int i = 0; i < g_reg_n; i++)
        if (strcmp(g_reg[i].src, src) == 0) return g_reg[i].m->player;
    return NULL;
}

/* 按 src 置/清图层模式(attach 置 1 / detach 清 0)。未知 src 静默忽略
   (会话可能已被 close —— 热重载顺序里注册表摘除先于图层摘除)。 */
void hnp_media_set_layer_mode(const char *src, int on) {
    if (!src || !*src) return;
    for (int i = 0; i < g_reg_n; i++)
        if (strcmp(g_reg[i].src, src) == 0) { g_reg[i].m->layer_mode = on ? 1 : 0; return; }
}

/* 该会话是否图层直出(pf_frame 的早退问询; 帧不外送 → CPU 链归零) */
int hnp_media_layer_mode(const void *m) {
    const hnp_media *s = (const hnp_media *)m;
    return s ? s->layer_mode : 0;
}

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
    /* 帧输出对象: **显式声明 kCVPixelFormatType_32BGRA**(事故①修复挂点)。
       旧代码 attributes 传 NULL —— 实测产 '420v' planar, 而其平面行距
       GetBytesPerRowOfPlane 在本运行时会谎报(§10.4), 旧兜底 sy/2 对 NV12
       又是错的(事故①实证: Y stride=384 / UV 真实 320 / 谎报 5120 / 兜底
       192 → 色度逐行错位 → 纯绿雪花帧)。请求 BGRA 后缓冲是单平面紧排
       (bytesPerRow 就是权威), 转换只剩字节序重排, 雪花与绿屏一起消失。
       BGRA = 0x42475241(CoreVideo/CVPixelBuffer.h 枚举, 非导出符号, 硬编码;
       key 经 dlsym 取 kCVPixelBufferPixelFormatTypeKey 的 CFStringRef)。
       查不到 key/类型(异常运行时)退回 NULL attributes —— 平面转换路径仍在。 */
    obj_t out = ns_alloc_init(o_getClass("AVPlayerItemVideoOutput"));
    obj_t attrs = NULL;
    {
        obj_t keyptr = dlsym(RTLD_DEFAULT, "kCVPixelBufferPixelFormatTypeKey");
        if (keyptr && *(obj_t *)keyptr && o_getClass("NSNumber") && o_getClass("NSDictionary")) {
            obj_t num = ((obj_t (*)(obj_t, sel_t, unsigned long))o_msgSend)(
                o_getClass("NSNumber"), o_sel("numberWithUnsignedLong:"), 0x42475241UL);
            obj_t objs[1] = { num };
            obj_t keys[1] = { *(obj_t *)keyptr };
            attrs = ((obj_t (*)(obj_t, sel_t, obj_t *, obj_t *, unsigned long))o_msgSend)(
                o_getClass("NSDictionary"), o_sel("dictionaryWithObjects:forKeys:count:"),
                objs, keys, 1);
        }
        out = ((obj_t (*)(obj_t, sel_t, obj_t))o_msgSend)(
            out, o_sel("initWithPixelBufferAttributes:"), attrs);
    }
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
    /* 到尾动作 = None(**枚举值 2**, AVPlayerActionAtItemEndNone): 不 Advance
       不 Pause —— 播完停在末帧等通知处理器 seek(0)。铲掉风暴根因一: 缺省
       Pause 在撞尾瞬间把 rate 打到 0, 引擎补发的 play 被吃掉(position 永久
       停在 0)。⚠️ 不能传 0 —— 那是 Advance(AVQueuePlayer 专属, AVPlayer
       setActionAtItemEnd:(0) 直接抛 NSInvalidArgumentException 实测崩)。 */
    MSG1(void, player, "setActionAtItemEnd:", (obj_t)2);
    m->player = player; m->item = item; m->output = out;
    media_note_setup();            /* 进程一次: 观察者运行期类 + 单例对象 */
    media_note_watch(m, 1);        /* 会话级注册(close 配对摘除) */
    reg_add(url, m);   /* 图层模式查询口(见 g_reg 注释); 满表时静默不入 */
    return m;
}

void hnp_media_close(hnp_media *m) {
    if (!m) return;   /* 幂等; NULL 安全 */
    reg_del(m);       /* 图层注册表先摘(app 侧下一拍 walk 即 detach 图层) */
    media_note_watch(m, 0);   /* 到尾通知配对摘除(先于 item 释放, 防通知中心
                                 持悬空 item / 解码器资源滞留 —— 热重载同路) */
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
    m->wrap_pending = 0;   /* 显式播放意图最高: 作废回绕在途抑制 */
    MSG0(void, m->player, "play");
    m->playing = 1;
    return 1;
}

void hnp_media_pause(hnp_media *m) {
    if (!m || !m->player || g_once <= 0) return;
    m->wrap_pending = 0;   /* 暂停后回绕 seek 已无意义(显式意图最高) */
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

/* ---- 到尾通知(AVPlayerItemDidPlayToEndTimeNotification)主路径回绕 ----
 *
 * 标准 Cocoa 姿态, 零 block: NSNotificationCenter + selector 观察者。
 * 观察者方法是运行期子类方法(objc_allocateClassPair + class_addMethod,
 * 与 hnp_macos.c 的 HNPNoteObserver 同手法; ⚠️ 方法必须在 objc_register-
 * ClassPair **之前**挂, 注册后 add_method 静默返回 NO → 分发即消息转发崩)。
 *
 * 处理器流程: 通知.object = 播完的 AVPlayerItem → 按指针查会话注册表 →
 * 记账(ended_wall/ended_seen) → loop 已闩锁且播放态则 seek(0)+play 一次。
 * 非主线程投递(AVFoundation 内部队可能)时经 dispatch_async_f 转交主线程
 * —— 全部会话状态变更保持在引擎同一线程(单线程契约), 零竞态。
 *
 * 探针观测点(HN_MEDIA_TRACE=1, stderr): [media-end](到尾通知)/
 * [media-wrap](通知驱动回绕)/[media-seek](引擎 seek, 风暴计数)。 */

static double now_mono(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 1e-9;
}

/* HN_MEDIA_TRACE 记忆化读(与 app 壳/hnp_macos.c 的环境钮纪律同款) */
static int media_trace(void) {
    static char memo[8] = "\x01";
    static int on = -1;
    const char *e = getenv("HN_MEDIA_TRACE");
    const char *cur = (e && *e) ? e : "";
    if (strcmp(cur, memo) != 0) {
        snprintf(memo, sizeof memo, "%s", cur);
        on = (e && *e) ? 1 : 0;
    }
    return on;
}

/* 回绕本体: seek(0) 零容差(与 hnp_media_seek 播放态同口径 —— coarse 容差
   会向关键帧窗口妥协, 落位时间不可预期) + play 保险(actionAtItemEnd=None
   下 rate 本应仍在, 补一发防同类再犯) + wrap_pending(在途窗口 position
   对旧尾读数报 0, 引擎启发式永不成立 —— 风暴根因二连根拔除)。 */
static void media_wrap_now(hnp_media *m, const char *via) {
    CMTime z = sec_to_cm(0.0);
    ((void (*)(obj_t, sel_t, CMTime, CMTime, CMTime))o_msgSend)(
        m->player, o_sel("seekToTime:toleranceBefore:toleranceAfter:"), z, z, z);
    MSG0(void, m->player, "play");
    m->wrap_pending = 1;
    m->wrap_cnt++;
    if (media_trace())
        fprintf(stderr, "[media-wrap] n=%d via=%s dur=%.3f\n",
                m->wrap_cnt, via, m->duration);
}

/* 按通知.object(item 指针)查会话并处理。**只比较指针不解引用 ctx 语义的
   item**(dispatch 转交路上 item 可能已随 close 释放 —— 悬空指针值做比较
   安全; 注册表摘除后查不到 = 会话已亡, 安全吞掉)。 */
static void media_handle_item_end(obj_t item) {
    if (!item) return;
    hnp_media *m = NULL;
    for (int i = 0; i < g_reg_n; i++)
        if (g_reg[i].m->item == item) { m = g_reg[i].m; break; }
    if (!m || !m->player) return;
    m->ended_wall = now_mono();
    m->ended_seen++;
    int wrap = 0;
    if (m->loop_latched && m->playing && m->duration_known) {
        media_wrap_now(m, "notif");
        wrap = 1;
    }
    if (media_trace())
        fprintf(stderr, "[media-end] n=%d loop=%d playing=%d wrap=%d\n",
                m->ended_seen, m->loop_latched, m->playing, wrap);
}

/* dispatch_async_f 主线程落点(通知非主线程投递时) */
static void media_end_trampoline(void *ctx) {
    media_handle_item_end((obj_t)ctx);
}

/* NSNotificationCenter 分发入口(观察者方法, 签名 v@:@) */
static void media_on_item_end(obj_t self, sel_t cmd, obj_t note) {
    (void)self; (void)cmd;
    obj_t item = note ? MSG0(obj_t, note, "object") : NULL;
    if (!item) return;
    if (MSG0(int, o_getClass("NSThread"), "isMainThread") ||
        !f_dispatch_async_f || !g_main_queue) {
        media_handle_item_end(item);      /* 主线程: 就地处理 */
    } else {
        f_dispatch_async_f(g_main_queue, item, media_end_trampoline);
    }
}

/* 进程一次: 运行期观察者类 + 单例对象。单例与进程同寿(永不释放) ——
   观察者无悬空; 会话级 item 注册在 media_note_watch 里配对增删。 */
static void media_note_setup(void) {
    static obj_t (*alloc_pair)(obj_t, const char *, size_t);
    static signed char (*add_method)(obj_t, sel_t, void (*)(void), const char *);
    static void (*reg_pair)(obj_t);
    static int done;
    if (done) return;
    done = 1;
    alloc_pair = (obj_t (*)(obj_t, const char *, size_t))
                 dlsym(RTLD_DEFAULT, "objc_allocateClassPair");
    add_method = (signed char (*)(obj_t, sel_t, void (*)(void), const char *))
                 dlsym(RTLD_DEFAULT, "class_addMethod");
    reg_pair   = (void (*)(obj_t))dlsym(RTLD_DEFAULT, "objc_registerClassPair");
    if (!alloc_pair || !add_method || !reg_pair) return;
    obj_t cls = alloc_pair(o_getClass("NSObject"), "HNPMediaNoteObserver", 0);
    if (!cls) return;
    /* ⚠️ 必须 objc_registerClassPair **之前**挂(hnp_macos.c 同一教训) */
    add_method(cls, o_sel("onItemEnd:"), (void (*)(void))media_on_item_end, "v@:@");
    reg_pair(cls);
    obj_t o = MSG0(obj_t, cls, "alloc");
    g_media_obs = o ? MSG0(obj_t, o, "init") : NULL;
}

/* 会话级注册/摘除(配对: open 置 1 / close 置 0; obs_registered 防重复)。
   套一次性 autorelease 池: nsstr 产物是 autoreleased, 而 open 的池在注册
   前已 drain(无池 autorelease = 泄漏), 与 media_pump 同一纪律。 */
static void media_note_watch(hnp_media *m, int on) {
    if (!m || !m->item) return;
    if (on) {
        if (!g_media_obs || m->obs_registered) return;
        obj_t nc = MSG0(obj_t, o_getClass("NSNotificationCenter"), "defaultCenter");
        if (!nc) return;
        m->obs_registered = 1;
        obj_t pool = ns_alloc_init(o_getClass("NSAutoreleasePool"));
        ((void (*)(obj_t, sel_t, obj_t, sel_t, obj_t, obj_t))o_msgSend)(
            nc, o_sel("addObserver:selector:name:object:"),
            g_media_obs, o_sel("onItemEnd:"),
            nsstr("AVPlayerItemDidPlayToEndTimeNotification"), m->item);
        if (pool) MSG0(void, pool, "drain");
    } else {
        if (!m->obs_registered) return;
        m->obs_registered = 0;
        obj_t nc = MSG0(obj_t, o_getClass("NSNotificationCenter"), "defaultCenter");
        if (!nc) return;
        obj_t pool = ns_alloc_init(o_getClass("NSAutoreleasePool"));
        ((void (*)(obj_t, sel_t, obj_t, obj_t, obj_t))o_msgSend)(
            nc, o_sel("removeObserver:name:object:"), g_media_obs,
            nsstr("AVPlayerItemDidPlayToEndTimeNotification"), m->item);
        if (pool) MSG0(void, pool, "drain");
    }
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
    m->seek_cnt++;
    /* loop 闩锁: 引擎在"到尾通知后 1s 内"发来的 seek(0)(播放态)是 loop
       回绕签名 —— 闩上后, 后续到尾由通知处理器直接回绕(启发式主路径退役;
       非 loop 的 ENDED→replay 发生在 pause 态, 不命中)。窗口 1s: 引擎的
       启发式在到尾后 ≤1 拍(~16ms)必触发, 用户手动回开头远晚于此。 */
    double since_end = m->ended_seen ? now_mono() - m->ended_wall : -1.0;
    if (m->playing && sec <= 0.001 && since_end >= 0 && since_end < 1.0)
        m->loop_latched = 1;
    if (media_trace())
        fprintf(stderr, "[media-seek] n=%d sec=%.3f playing=%d loop=%d since_end=%.3f\n",
                m->seek_cnt, sec, m->playing, m->loop_latched, since_end);
    m->wrap_pending = 0;   /* 显式 seek 意图最高: 作废回绕在途抑制 */
    media_pump();
    refresh_duration(m);
    /* 钳制到 [0, duration](已知时; 未知只钳下界) */
    if (m->duration_known && m->duration > 0 && sec > m->duration) sec = m->duration;
    if (sec < 0) sec = 0;
    CMTime t = sec_to_cm(sec);
    if (m->playing) {
        /* 播放态直接 seek(§10.5 只在 pause 态有问题); 零容差精seek ——
           缺省 coarse 容差在回绕(结尾→0)时会向关键帧窗口妥协, 落位时间
           不可预期, 与引擎的回绕抑制窗口相互踩(实证 30s 内第三次回绕卡死)。 */
        ((void (*)(obj_t, sel_t, CMTime, CMTime, CMTime))o_msgSend)(
            m->player, o_sel("seekToTime:toleranceBefore:toleranceAfter:"),
            t, t, t);
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
    media_pump();   /* 泵里可能正是到尾通知的投递点(处理器已就地回绕) */
    double p = cur_position(m);
    /* 通知驱动回绕的在途窗口: seekToTime 异步, 落位前 currentTime 仍读
       旧尾值(≈duration)。wrap_pending 期间对"仍在尾部(离尾 <0.5s)"的
       读数改报 seek 目标 0 —— 引擎 cur_time 直接落 0, position≈duration
       的启发式触发条件永不成立(风暴根因二: 落位窗口每 16ms 重触发 seek,
       连根拔除); 读数已离尾 = 落位, 恢复真值并清窗。用户显式 seek/play/
       pause 已在入口清窗(意图最高)。 */
    if (m->wrap_pending) {
        if (m->duration_known && p >= m->duration - 0.5) p = 0.0;
        else m->wrap_pending = 0;
    }
    /* 回绕/重启卡死自愈(兜底保留, 主路径已换到到尾通知): 通知丢失/闩锁
       未成(如用户手动 seek 到尾)时仍可能撞尾自动停; playing 态在 0 附近
       ~0.5s 纹丝不动 → 再催一次 play。合法起点 0.5s 内早已前进; paused 态
       (m->playing==0)不进这段。 */
    if (m->playing) {
        if (p < 0.05 && p == m->stall_pos) {
            if (++m->stall_cnt >= 30) {
                MSG0(void, m->player, "play");
                m->stall_cnt = 0;
            }
        } else {
            m->stall_cnt = 0;
            m->stall_pos = p;
        }
    }
    if (sec_out) *sec_out = p;
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

/* 首帧几何日志(诊断实锤): format fourcc / planes / strides / base 地址 /
   数据总长 / UV 头 16 字节 —— 锁内取数(锁前取 base 恒 0x0, 是 IOSurface
   惰性映射)。每次进程一行, 不刷屏。 */
static void pb_geometry_trace(const void *pb, int w, int h) {
    static int traced = 0;
    if (traced || !f_CVPixelBufferGetPixelFormatType) return;
    traced = 1;
    unsigned int fourcc = f_CVPixelBufferGetPixelFormatType(pb);
    size_t total = f_CVPixelBufferGetDataSize(pb);
    size_t planes = f_CVPixelBufferGetPlaneCount(pb);
    fprintf(stderr, "[media-pb] fmt=%c%c%c%c planes=%zu %dx%d total=%zu "
            "bpr=%zu p0stride=%zu p1stride=%zu p0=%p p1=%p\n",
            (char)(fourcc & 255), (char)((fourcc >> 8) & 255),
            (char)((fourcc >> 16) & 255), (char)((fourcc >> 24) & 255),
            planes, w, h, total,
            f_CVPixelBufferGetBytesPerRow(pb),
            planes ? f_CVPixelBufferGetBytesPerRowOfPlane(pb, 0) : 0,
            planes > 1 ? f_CVPixelBufferGetBytesPerRowOfPlane(pb, 1) : 0,
            f_CVPixelBufferGetBaseAddressOfPlane(pb, 0),
            planes > 1 ? f_CVPixelBufferGetBaseAddressOfPlane(pb, 1) : NULL);
    if (planes > 1) {
        const unsigned char *uv = f_CVPixelBufferGetBaseAddressOfPlane(pb, 1);
        if (uv) fprintf(stderr, "[media-pb] uv[0..15]=%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d,%d\n",
                        uv[0], uv[1], uv[2], uv[3], uv[4], uv[5], uv[6], uv[7],
                        uv[8], uv[9], uv[10], uv[11], uv[12], uv[13], uv[14], uv[15]);
    }
}

/* CVPixelBuffer → RGBA8 非预乘, 原点左上、顶上到底下。**逐行**按各自
   stride 走(事故①修复口径), 返回 1=转换完成 / 0=格式不支持或锁失败
   —— 调用方跳过该帧沿用上一帧, **绝不硬读**(硬读 = 绿雪花/越界)。
   主路径: open 时已请求 BGRA(单平面, bytesPerRow 权威) → 字节序重排。
   兜底: '420v'/'420f' NV12 双平面 —— Y 行距优先 GetBytesPerRowOfPlane,
   但它会说谎(§10.4, 400px 帧谎报 13312), sane_stride 以总长为界回退;
   UV 行距**从总长推导**(总长 − Y 平面尺寸 = UV 平面尺寸, 除以行数),
   旧代码的 sy/2 兜底对 NV12 是错的(实证 192 vs 真实 320)。
   色彩: BT.601 limited(video range)。 */
static int pb_to_rgba(const void *pb, unsigned char *dst, int w, int h) {
    if (f_CVPixelBufferLockBaseAddress(pb, CV_LOCK_READONLY) != 0) return 0;
    pb_geometry_trace(pb, w, h);
    size_t total = f_CVPixelBufferGetDataSize(pb);
    size_t planes = f_CVPixelBufferGetPlaneCount(pb);
    unsigned int fmt = f_CVPixelBufferGetPixelFormatType
                       ? f_CVPixelBufferGetPixelFormatType(pb) : 0;
    int ok = 0;
    if (planes >= 2) {
        const unsigned char *Y  = (const unsigned char *)f_CVPixelBufferGetBaseAddressOfPlane(pb, 0);
        const unsigned char *UV = (const unsigned char *)f_CVPixelBufferGetBaseAddressOfPlane(pb, 1);
        size_t sy  = sane_stride(f_CVPixelBufferGetBytesPerRowOfPlane(pb, 0),
                                 f_CVPixelBufferGetBytesPerRow(pb), (size_t)w, (size_t)h, total);
        size_t ch_rows = ((size_t)h + 1) / 2;
        size_t y_size = sy * (size_t)h;
        size_t p1 = (total > y_size) ? total - y_size : 0;
        size_t suv = p1 ? sane_stride(f_CVPixelBufferGetBytesPerRowOfPlane(pb, 1),
                                      p1 / ch_rows, (size_t)w / 2, ch_rows, p1)
                        : sy;
        if (Y && UV && sy >= (size_t)w && suv >= (size_t)(w / 2)) {
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
            ok = 1;
        }
    } else if (planes == 0 && fmt == 0x42475241u /* 'BGRA' */) {
        const unsigned char *base = (const unsigned char *)f_CVPixelBufferGetBaseAddress(pb);
        size_t stride = f_CVPixelBufferGetBytesPerRow(pb);
        if (base && stride >= (size_t)w * 4 &&
            (size_t)(h - 1) * stride + (size_t)w * 4 <= total) {
            for (int y = 0; y < h; y++) {
                const unsigned char *s = base + (size_t)y * stride;
                unsigned char *d = dst + (size_t)y * (size_t)w * 4;
                for (int x = 0; x < w; x++) {   /* BGRA → RGBA */
                    d[0] = s[2]; d[1] = s[1]; d[2] = s[0]; d[3] = s[3];
                    s += 4; d += 4;
                }
            }
            ok = 1;
        }
    }
    /* 其余格式(2vuy/444/…): 不认识就跳过 —— 沿用上一帧, 绝不硬读。 */
    f_CVPixelBufferUnlockBaseAddress(pb, CV_LOCK_READONLY);
    return ok;
}

int hnp_media_frame(hnp_media *m, unsigned char *rgba, size_t cap,
                    int *w, int *h, double *pts_sec) {
    if (w) *w = 0;
    if (h) *h = 0;
    if (pts_sec) *pts_sec = 0.0;
    if (!m || !m->player || !m->output || g_once <= 0) return -1;
    if (m->layer_mode) {
        /* 图层直出: 像素由 AVPlayerLayer 经系统合成器上屏, CPU 拉帧/
           NV12→RGBA 转换整链跳过(连 media_pump 都不跑 —— 真窗 runloop
           由事件泵与 position 的既有节拍兜着)。-1 = 无帧, 引擎照常对
           该元素画占位 RECT(被图层盖住, 视觉无感)。 */
        return -1;
    }
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
    if (!pb_to_rgba(pb, rgba, pw, ph)) {
        /* 格式不支持/锁失败: 跳过该帧(沿用上一帧), 绝不把没转换的缓冲交出去 */
        f_CFRelease(pb);
        if (w) *w = m->last_w;
        if (h) *h = m->last_h;
        return 0;
    }
    f_CFRelease(pb);   /* copy 出来的 buffer 归调用方释放 */
    return 1;
}

#endif /* __APPLE__ */

# media-design — 音频/视频 + WASM 契约(三个实现代理照此执行)

状态: 设计定稿(2026-10-10)。本文是契约, 不是教程 —— 每个签名、每条状态转移、
每个文件的主人都在这里钉死。实现者遇到本文没写的行为, 按 HTMLMediaElement
语义就近对齐; 遇到本文明确"不做"的, 不要做。

## 0. 背景判断(已在代码与实机验证)

**H5/webview 播视频不打包 FFmpeg**: 解码委托 OS 媒体框架(macOS 即
AVFoundation/CoreMedia), `<video>` 在 DOM 侧只是一个状态机 + 合成器贴图。
本引擎照抄这个分工:

- **引擎侧**: 媒体元素状态机(仿 HTMLMediaElement 的六态) + 布局盒 +
  "当前帧位图"绘制指令。引擎不认识解码器。
- **平台侧**: 运行期 `dlopen` AVFoundation, 全部经 `objc_msgSend` 调用 ——
  与 `platform/hnp_macos.c` 的既有手法逐字一致(dlopen AppKit + dlsym
  objc_getClass/sel_registerName/objc_msgSend, 见 hnp_macos.c:56-68;
  零 ObjC 头文件/零 .m/链接期不依赖 framework, hnp_macos.c:1-7 注释)。
- 音频不另起炉灶: 音轨由 AVPlayer 直接出声, 与视频同一入口(纯音频文件
  也走 `hnp_media_open`, 只是没有帧)。

**已实机验证**(本设计定稿前在 macOS arm64 / App clang 21 / macOS 27 上,
用 /tmp 预演程序跑通全管线, ffmpeg 生成 64x64 H.264+AAC 2s 样片):

| 验证点 | 结果 |
|---|---|
| dlopen AVFoundation + objc_msgSend 建 AVPlayer | ✓ |
| `NSApplication finishLaunching` 后异步加载才推进(不装 AppKit duration 永远 NaN) | ✓ 必需前置 |
| runloop 泵必须**有界**(distantFuture 在 pause 态永久阻塞挂死) | ✓ 已复现挂死 |
| `AVPlayerItemVideoOutput initWithPixelBufferAttributes:`(本运行时无 dispatchQueue 变体) | ✓ 方法表确认 |
| 请求 BGRA 的 attributes 被忽略, 实际产 '420v' planar | ✓ 必须做 NV12→RGBA 转换 |
| 拉帧 64x64 / plane0 stride=64 / duration=2.000 | ✓ |
| 播放中 seek 精确; **pause 态直接 seek 视频轨停在 0.000**(音频正常) | ✓ 必须 play→seek→settle→pause |
| ended 不能靠 rate(rate 播完仍 1.00) | ✓ 引擎按 position≥duration 推导 |
| volume/setMuted/currentTime(CMTime 结构体按值传参) | ✓ |
| wasm3 最小 11 文件 + zig wasm32-freestanding demo, add(2,3)=5 | ✓ |

**顺带发现一个既有隐患**(与本设计相关, 实现时不得照抄):
`rt/hn_rt.c:478-510` 的剪贴板/openURL 用 `dlsym((void *)0, "objc_getClass")`
—— macOS 上 `RTLD_DEFAULT` 是 `((void *)-2)` 而非 NULL(实测 dlsym(NULL,…)
对所有符号返回 NULL), 这三个函数在 macOS 上恒失败。媒体代码一律用
`RTLD_DEFAULT` 宏。此隐患的修复不属于本设计范围, 但不要复制这个写法。

---

## 1. 分层与数据流

```
JS(QuickJS)──hnMedia*/hnWasm*──┐
                               ▼
  rt/hn_rt.c ── hn_media_host 回调表注入 ──► Sources/CHtmlNative/hn_media.c
  (平台无关)        (宿主注入)                 引擎状态机 + 会话表 + 帧缓存
                                            │           ▲
              ┌─────────────────────────────┘           │ frame/duration/position
              ▼                                          │
  hn_layout.c(替换元素尺寸) / hn_paint.c(BITMAP 指令)   │
                                                        │
                              platform/hnp_media_macos.c(新, dlopen AVFoundation)
                                            ▲
              tools/hncore.c 合成宿主(梯度帧+假时钟) ┘(无平台, 确定性)
```

要点:

- **引擎零平台依赖**: hn_media.c 只认 `hn_media_host` 函数指针表, 不知道
  AVFoundation 的存在(与 hn.h:112-115 图片后端、hn.h:121-126 资产后端
  同一依赖倒置模式)。
- **平台零引擎依赖**: hnp_media_macos.c 只实现 hn_platform.h 追加段,
  被 rt 层包装成 hn_media_host 注入(包装点在 app/hn_app.c 与各壳)。
- **同一入口**: `<video>` 与 `<audio>` 走同一会话表、同一宿主回调;
  纯音频只是"没有帧"的会话(实测 tone.m4a frames=0)。

---

## 2. hnp_media_* ABI(platform/hn_platform.h 追加段)

```c
/* ---------------- 媒体(追加在 hn_platform.h 光标段之后) ---------------- */

typedef struct hnp_media hnp_media;   /* 不透明句柄(平台实现自定义内部结构) */

/* 本平台是否带媒体实现。headless/Linux/Windows 返回 0(ABI 留位不实现)。
   macOS 返回 1(不探测 AVFoundation 是否可 dlopen —— dlopen 失败在
   open 的 err 里如实报告, 不在 supported 里猜)。 */
int hnp_media_supported(void);

/* 打开媒体(url 为本地路径或 file:// URL; 引擎侧保证先于任何其他调用)。
   同步返回句柄; 元数据在后台解析, 就绪用 hnp_media_state 查询。
   失败返回 NULL 并写一行原因进 err(err/err_cap 可为 NULL/0 = 不关心)。
   纯音频文件合法(没有视频轨 → frame 恒返回 0)。 */
hnp_media *hnp_media_open(const char *url, char *err, size_t err_cap);

/* 关闭并释放(幂等; NULL 安全)。之后句柄不得再使用。 */
void hnp_media_close(hnp_media *m);

/* 播放控制。play 返回 1 = 已接受; seek 钳制到 [0, duration](已知时)。 */
int  hnp_media_play(hnp_media *m);
void hnp_media_pause(hnp_media *m);
int  hnp_media_seek(hnp_media *m, double sec);

/* 音量 0..1(越界钳制)与静音。视频音轨与纯音频同此入口。 */
void hnp_media_set_volume(hnp_media *m, float vol);
void hnp_media_set_muted(hnp_media *m, int muted);

/* 元数据。duration 未知(直播流)时写 -1 并返回 0。 */
int  hnp_media_duration(hnp_media *m, double *sec_out);

/* 当前播放位置(秒; 单调性由平台保证: pause 后恒定, seek 落位后更新)。 */
int  hnp_media_position(hnp_media *m, double *sec_out);

/* 就绪与结束: *ready=1 表示元数据+可起播(时长已知);
   *ended=1 表示位置到达 duration 平台侧确认(实现可直接用 position>=duration,
   实测 AVPlayer 的 rate 在播完时不可靠)。两者可同时为 1。 */
int  hnp_media_state(hnp_media *m, int *ready, int *ended);

/* 拉当前视频帧(RGBA8 非预乘, 原点左上, 顶上到底下)。
   调用方给缓冲与容量 cap(字节); *w/*h 写实际帧尺寸; *pts_sec 写该帧
   显示时间戳(秒)。返回: 1=新帧已写入(内容与上次不同), 0=无新帧(内容未变,
   *w/*h 仍有效), -1=无视频轨或失败。
   cap 不足以容纳整帧时写 -1 并返回 -1(调用方按 meta 重新分配后重试)。
   实现注意(实机验证): 实际产出的 CVPixelBuffer 是 '420v' planar,
   请求 BGRA 的 attributes 会被忽略 —— 用 GetBaseAddressOfPlane(0/1)+
   GetBytesPerRowOfPlane 取 Y/UV 平面, 平台内做 NV12→RGBA 转换。 */
int  hnp_media_frame(hnp_media *m, unsigned char *rgba, size_t cap,
                     int *w, int *h, double *pts_sec);
```

### macOS 实现要点(hnp_media_macos.c, 实机验证过的序列)

1. `hnp_media_supported()` → 1(仅 `#ifdef __APPLE__` 编入)。
2. **open**: dlopen AVFoundation/CoreMedia/CoreVideo(幂等) → 确保
   `NSApplication sharedApplication + finishLaunching`(与 hnp_init:64-67
   同款; **实测不装 AppKit 则异步加载永不推进**) → `AVPlayer
   playerWithURL:`(fileURLWithPath) → `AVPlayerItemVideoOutput
   alloc/initWithPixelBufferAttributes:`(本运行时没有 dispatchQueue 变体,
   方法表已确认) → `[item addOutput:]`。
3. **泵**: 每次被调(duration/position/state/frame)内部跑一次**有界**
   `[[NSRunLoop mainRunLoop] runMode:beforeDate:]`,
   beforeDate = now+~10ms。**禁止 distantFuture**(pause 态永久阻塞, 实测挂死)。
4. **seek**: 播放中直接 `seekToTime:`(CMTime 按值传参, arm64 原型
   `void (*)(id, SEL, CMTime)` 实测正确); **pause 态必须走
   play→seekToTime→轮询 position 到位(|Δ|<0.05s 连续 2 次)→pause**
   (实测视频轨 pause 态直接 seek 位置停在 0.000; 音频无此问题, 统一走
   同一序列)。
5. **帧**: `hasNewPixelBufferForItemTime:[player currentTime]` →
   `copyPixelBufferForItemTime:itemTimeForDisplay:(t,NULL)` →
   Lock/拷贝转换/Unlock/CFRelease。420v: plane0=Y(stride 用
   GetBytesPerRowOfPlane, 实测 64px 帧为 64), plane1=交织 UV。
6. **ended**: `(position >= duration - 1e-3) && duration > 0`(rate 不可靠)。

headless(hnp_headless.c)/linux(hnp_linux.c) 不新增实现文件:
`hnp_media_supported()` 提供**弱符号缺省实现**返回 0(在 hn_platform.h 的
追加段里以 `#ifndef HNP_MEDIA_NOIMPL` 的 static inline 兜底, 或由各平台
文件显式给出 0 —— 实现代理二选一, 但 hncore/headless 路径必须零符号依赖)。

---

## 3. 引擎侧 hn_media_host 回调表 + 引擎 API

### 回调表(注入点: `hn_context_set_media`, 与 assets 后端同模式 hn.h:121-126)

```c
/* —— 写进 include/hn.h(公共), 实现在 Sources/CHtmlNative/hn_media.c —— */

typedef enum {
    HN_MEDIA_IDLE = 0,    /* 无会话/未打开 */
    HN_MEDIA_LOADING,     /* 已 open, 元数据未就绪 */
    HN_MEDIA_READY,       /* 可起播(未播) */
    HN_MEDIA_PLAYING,
    HN_MEDIA_PAUSED,
    HN_MEDIA_ENDED        /* 播完且非循环; play() 重新起播 */
} hn_media_state;

typedef struct hn_media_host {
    void *ctx;
    /* 打开资源; 返回宿主句柄(引擎不解释), 失败 NULL+err 一行原因。 */
    void *(*open)(void *ctx, const char *url, char *err, size_t err_cap);
    void  (*close)(void *ctx, void *m);
    /* 帧循环提示(dt 毫秒)。无钟宿主(合成宿主)用它自行累计假时钟;
       真实宿主(AVPlayer)忽略。PLAYING 会话每帧必达。 */
    void  (*tick)(void *ctx, void *m, float dt_ms);
    int   (*play)(void *ctx, void *m);
    void  (*pause)(void *ctx, void *m);
    int   (*seek)(void *ctx, void *m, double sec);       /* 返回 1=接受 */
    void  (*set_volume)(void *ctx, void *m, float vol);  /* 0..1 */
    void  (*set_muted)(void *ctx, void *m, int muted);
    int   (*duration)(void *ctx, void *m, double *sec);  /* 0=未知(写 -1) */
    int   (*position)(void *ctx, void *m, double *sec);
    int   (*state)(void *ctx, void *m, int *ready, int *ended);
    /* 语义与 hnp_media_frame 完全一致(见上); 会话帧缓存由引擎持有。 */
    int   (*frame)(void *ctx, void *m, unsigned char *rgba, size_t cap,
                   int *w, int *h, double *pts_sec);
} hn_media_host;

void hn_context_set_media(hn_context *c, const hn_media_host *host); /* 宿主持有表内存 */
```

**时钟所有权(钉死)**: `currentTime` 的唯一真源是宿主 `position()`。
引擎**不用 dt 自累计**(避免与真解码漂移); 引擎每帧把 dt 转交
`tick()` 供无钟宿主累计, 随后读 `position()` 写回会话的 `cur_time`。
合成宿主的确定性 = 探针用固定 16ms 步进喂 tick(hncore 已有同一手法:
HN_CLOCK 分步推进, hncore.c:322-332)。

### 引擎公开 API(hn.h 追加; 元素寻址, JS 桥与探针共用)

```c
void   hn_media_tick(hn_context *c, float dt_ms);   /* hn_context_anim_tick 内部调用 */
int    hn_media_play(hn_context *c, hn_node *n);     /* IDLE→open; 返回 1=进入/保持播放 */
int    hn_media_pause(hn_context *c, hn_node *n);
int    hn_media_seek(hn_context *c, hn_node *n, double sec);
double hn_media_time(hn_context *c, hn_node *n);      /* 无会话返回 0 */
double hn_media_duration(hn_context *c, hn_node *n);  /* 未知返回 -1 */
void   hn_media_set_volume(hn_context *c, hn_node *n, float v);
void   hn_media_set_muted(hn_context *c, hn_node *n, int muted);
int    hn_media_state(hn_context *c, hn_node *n);    /* HN_MEDIA_* */
```

### 状态机(仿 HTMLMediaElement 子集)

| 现态 | 事件 | 次态 | 动作 |
|---|---|---|---|
| IDLE | 元素带 src 且 autoplay 或首次 play() | LOADING | host.open; 失败→IDLE+stderr 一行 |
| LOADING | host.state ready=1 | READY | 读 duration; 触发重排(尺寸可能变) |
| READY / PAUSED | play() | PLAYING | host.play |
| PLAYING | pause() | PAUSED | host.pause |
| PLAYING | position≥duration 且 !loop | ENDED | host.pause |
| PLAYING | position≥duration 且 loop | PLAYING | host.seek(0) |
| ENDED | play() | PLAYING | host.seek(0)+host.play |
| PAUSED | play() | PLAYING | host.play |
| 任意 | 文档替换/compact/destroy | (会话销毁) | host.close 全部(hn_context.c 的 lottie_clear 同位: hn_context.c:25/86/1922) |

属性读取(声明即语义, 与 lottie/mesh 属性风格一致):
`src`(路径, 相对文档目录)、`loop`(布尔)、`muted`(布尔)、
`autoplay`(布尔)、`volume="0.5"`、`poster` **不做**。

### 数据结构(hn_internal.h)

`hn_context` 追加(与 lottie 缓存 `lot` 同模式, hn_internal.h:407-410):

```c
const hn_media_host *media;              /* 运行时持有; NULL = 媒体禁用 */
struct hn_media_session {
    hn_node *node; char *src;            /* 归属元素与资源键(副本) */
    void    *handle;                     /* 宿主句柄 */
    hn_media_state state;
    double   cur_time, duration;         /* cur_time 每帧同步自宿主 */
    float    volume; int muted, loop;
    unsigned char *frame;                /* 最新帧缓存(引擎 malloc; 双缓冲) */
    int frame_w, frame_h, frame_stride; double frame_pts; int has_frame;
} *med_sessions; int n_med, cap_med;
```

会话生命周期: 首次 layout/paint 触达该元素时建; 文档替换(compact/set_doc/
destroy)时全部 close+free(帧缓冲 free)。帧缓存用**双缓冲**: 写入另一块再
交换指针 —— 保证显示列表持有的指针在下次 repaint 前有效(引擎单线程,
paint→raster 顺序固定, hn_internal.h:426-431 的 tmp 区生命周期同理)。

---

## 4. 文件所有权表(一个文件只有一个主人)

| 文件 | 主人 | 本设计中的改动 |
|---|---|---|
| Sources/CHtmlNative/hn_media.c | **引擎-媒体** | **新**。状态机/会话表/宿主调用/帧缓存 |
| Sources/CHtmlNative/include/hn.h | **引擎-公共头** | 追加: HN_CMD_BITMAP、hn_cmd 位图字段、hn_media_host、hn_media_state、hn_media_* API、hn_wasm_* API |
| Sources/CHtmlNative/hn_internal.h | **引擎-内部头** | 追加: hn_context 的 media 字段与会话表; hn_media_tick/会话清理的内部声明 |
| Sources/CHtmlNative/hn_layout.c | **引擎-布局** | 替换元素尺寸链扩展(hn_layout.c:1375-1394 的 img 链旁): video = 样式 > width/height 属性 > (READY 时宿主 meta) > 300x150; audio = 高 0(不可见, 盒仍在可命中) |
| Sources/CHtmlNative/hn_paint.c | **引擎-绘制** | `<video>/<audio>` 分支(img 分支旁, hn_paint.c:645): 轮询新帧→更新缓存→发 HN_CMD_BITMAP; 无帧(LOADING/纯音频)画占位 RECT(深底+边框, 探针可见非静默空白) |
| Sources/CHtmlNative/hn_html.c | **引擎-解析** | **零改动**(video/audio 是普通元素, src 属性已可解析; void 表不动 —— 不做 `<source>` 子元素) |
| Sources/CHtmlNative/hnsoft.c | **引擎-软光栅** | 指令分派 switch(hnsoft.c:775-801)加 HN_CMD_BITMAP: RGBA 拉伸混合到 BGRA 画布, 圆角走既有裁剪路径 |
| Sources/CHtmlNative/hn_context.c | **引擎-上下文** | anim_tick 内调 hn_media_tick; 会话表随 lottie 缓存的三处清理点销毁(hn_context.c:25/86/1922) |
| Sources/CHtmlNative/hn_wasm.c | **引擎-WASM** | **新**。wasm3 封装(见 §7) |
| rt/hn_rt.c | **桥** | QuickJS 桥函数注册(hn_rt.c:703-708 模式): hnMedia*/hnWasm*; hn_rt 全局媒体宿主槽位(仿 g_assets_be, hn_rt.c:87)注入到每个 context; opaque 扩为 {doc, ctx} |
| rt/hn_rt.h | **桥-头** | 追加 `void hn_rt_set_media(const hn_media_host *host);` |
| platform/hn_platform.h | **平台-头** | 追加 §2 声明段 |
| platform/hnp_media_macos.c | **平台-macOS** | **新**。§2 全部实现(仅 `__APPLE__` 编入) |
| platform/hnp_headless.c / hnp_linux.c | **平台-其余** | 仅 supported()=0 缺省(见 §2 末尾的两种落法之一) |
| app/hn_app.c | **壳-主循环** | hnp_init 后装配 hn_media_host(hnp_media_* 直包)→ hn_rt_set_media; 帧循环把 hnp_now_ms 差值喂 hn_rt_frame(既有) |
| tools/hncore.c | **壳-CLI/合成宿主** | 合成宿主(§8.1): 梯度帧+假时钟; paint 输出追加 BITMAP 行与 MEDIA 状态行; HN_MEDIA_AUTOPLAY/HN_MEDIA_DURATION 环境钮 |
| tools/media_probe.c | **探针-真机** | **新**。§8.2 |
| tools/media_probe.py | **探针-封装** | **新**。§8.2 |
| tools/build-multiplatform.sh | **构建** | vendor/wasm3 11 文件 + hn_media.c + hn_wasm.c 入列全部 SRC/ENGINE_SRC 数组; -Ivendor/wasm3; media_probe 的本机构建自检项 |
| （无 SPM） | **构建-可选件** | 仓库无 Package.swift(已按全 C99 方针删除, 构建链只有 bash + zig cc): hn_cairo.c/hn_wasm.c 由 build-multiplatform.sh 显式入列, 不需要 exclude 机制; hn_media.c 纯引擎自动编入 |
| vendor/wasm3/** | **第三方-只读** | vendored 快照(MIT, 连 LICENSE), 见 §7 |
| examples/wasm/add.c | **验证物料** | **新**。demo 模块源(§7 验证路径) |

禁止: 任何文件同时改两处语义; 引擎文件 include 平台头; 平台文件 include
引擎内部头(平台只见 hn_platform.h)。

---

## 5. 布局与绘制契约(带现状锚点)

- **布局**: 沿用 img 的回退链(hn_layout.c:1375-1394):
  样式 > width/height 属性 > 固有尺寸。video 的固有尺寸在 READY 前未知
  → 兜底 300x150(HTML 默认); audio 兜底高 0。READY 到手后如未显式定尺寸
  → 下一次 layout 用宿主 meta 尺寸(触发一次重排)。
- **绘制**: 每次 repaint 时对 PLAYING/READY 会话轮询一次 `host.frame`;
  返回 1 → 换缓冲+发 BITMAP; 返回 0 → 复用缓存(仍在显示列表里); 无帧 →
  占位 RECT。`<audio>` 永不发 BITMAP。
- **显示列表**: 新指令 `HN_CMD_BITMAP = 9`, hn_cmd 追加字段
  (x/y/w/h/radius 复用既有; 追加在结构体尾部):

  ```c
  /* BITMAP: 内存位图(RGBA8 非预乘, 左上原点)。指针生命周期 = 本次
     显示列表有效(引擎媒体会话双缓冲保证下次 repaint 前不被覆盖)。 */
  const unsigned char *bitmap;
  int bitmap_stride, bitmap_w, bitmap_h;
  ```

- **hncore paint 打印**(供 python 探针断言, 对齐既有打印格式
  hncore.c:365-367 的 IMAGE 行):

  ```
  BITMAP  x=... y=... w=... h=... src=64x64 pts=0.400
  MEDIA   id=<id> state=PLAYING t=0.496 dur=2.000
  ```

---

## 6. JS 桥(QuickJS, rt/hn_rt.c)

注册在 `hn_rt_eval` 的既有桥旁(hn_rt.c:703-708 的 hnSetText/hnSetValue 模式),
`JS_SetContextOpaque` 从 `hn_doc*` 扩为 `{hn_doc *doc; hn_context *ctx;}`(桥内
取用; hnSetText 改读 `.doc`)。宿主未注入或目标 id 不存在 → 全部静默 no-op
或返回缺省值(不抛异常, 与 hnSetText 一致):

| JS | C 语义 | 返回 |
|---|---|---|
| `hnMediaPlay(id)` | hn_media_play | true/false |
| `hnMediaPause(id)` | hn_media_pause | undefined |
| `hnMediaSeek(id, sec)` | hn_media_seek | true/false |
| `hnMediaTime(id)` | hn_media_time | Number(秒) |
| `hnMediaDuration(id)` | hn_media_duration | Number(未知 -1) |
| `hnMediaVolume(id, v)` | hn_media_set_volume + set_muted(v>0) | undefined |
| `hnWasmLoad(bytesOrPath)` | hn_wasm_load; bytes=ArrayBuffer/Uint8Array, 字符串按资产路径经 asset 后端读 | Number(id≥1; 失败 -1) |
| `hnWasmCall(id, fn, ...args)` | hn_wasm_call(args 按 Number→i32) | Number(i32 结果; 失败 NaN) |

---

## 7. WASM(vendor/wasm3 + hn_wasm.c)

- **vendor/wasm3**: 快照 wasm3/wasm3 @ `28ecb9af6d2040e474a70f7cb7f43666740141fb`
  (2026-09-29, MIT —— 连 LICENSE 一并 vendored, 目录只读不改)。
  **最小源集(已实机构建验证, `-std=c99` 零警告零外部依赖, 11 个 .c)**:

  ```
  m3_bind.c m3_code.c m3_compile.c m3_core.c m3_env.c m3_exec.c
  m3_function.c m3_info.c m3_module.c m3_parse.c m3_validate.c
  + 全部随附 .h(m3_config*.h/m3_core.h/m3_env.h/m3_exec*.h/m3_*.h/
    wasm3.h/wasm3_defs.h/m3_math_utils.h/m3_exception.h)
  ```

  排除: 5 个 API/WASI 文件(m3_api_libc/wasi/uvwasi/meta_wasi/tracer —— 不做
  WASI)、m3_snapshot.c/m3_deterministic.c/m3_xxh64.c(实测可去;
  **m3_info.c 与 m3_module.c 不可去** —— 去掉分别缺
  m3_PrintProfilerInfo/m3_FreeModule 符号, 实测链接失败)。

- **封装 ABI(include/hn.h; 纯 C99, 与宿主同进程同步调用)**:

  ```c
  typedef struct hn_wasm hn_wasm;
  /* 解析+装载; 失败 NULL(错误行 stderr)。字节由调用方持有, load 内部复制。 */
  hn_wasm *hn_wasm_load(const unsigned char *bytes, size_t n);
  /* 调用导出函数; v1 仅 i32 签名(args/out 均按 int32; m3_CallV 变参)。
     找不到函数/签名不符/陷阱返回 0(结果不定), 成功返回 1。 */
  int  hn_wasm_call(hn_wasm *w, const char *fn,
                    const int32_t *args, int n_args, int32_t *out);
  void hn_wasm_free(hn_wasm *w);
  ```

  实例表(id→hn_wasm*)由 hn_wasm.c 持有(容量 32, 满则 load 失败);
  JS 的 id 即表下标+1。

- **构建入列**: build-multiplatform.sh 的 SRC/ENGINE_SRC/WIN_RT/LINUX_RT/
  MAC_RT 数组追加 `vendor/wasm3/m3_*.c`(11 个)与
  `Sources/CHtmlNative/hn_wasm.c`、`Sources/CHtmlNative/hn_media.c`,
  CFLAGS 追加 `-Ivendor/wasm3`。纯 C99 → 三平台交叉产物无需条件编译。

- **demo 模块验证路径(examples/wasm/add.c, 已实机验证)**:

  ```sh
  zig cc -target wasm32-freestanding --no-standard-libraries \
         -Wl,--export=add -Wl,--no-entry examples/wasm/add.c -o /tmp/add.wasm
  # (zig 0.13 不支持 --export-all; 用 --export=<fn>。产物 840B, 魔数 \0asm 已验)
  ```

  验证宿主即 hn_wasm 的单测: load /tmp/add.wasm → call("add",[2,3]) → 断言 5
  (本设计已用等价裸 wasm3 程序跑通: `add(2,3) = 5`, exit 0)。该路径进
  media_probe.py 的第三段(wasm 段)。

---

## 8. 两个测试宿主策略

### 8.1 hncore 合成宿主(确定性; python 探针可跑)

tools/hncore.c 内置一份 `hn_media_host`:

- **假时钟**: 宿主内 `t += dt`(play 时), `position()` 返回 t; `tick(dt)` 是
  唯一时间来源。dt 由 HN_CLOCK 的既有分步推进喂入(hncore.c:322-332, 每步
  16ms)→ 探针可精确断言 `t≈HN_CLOCK 设定值`(容差 = 一个步长 0.016s)。
- **梯度帧**: `frame()` 生成确定性 RGBA:
  `R=x*255/w, G=y*255/h, B=((int)(pts*10)*37)%256` —— 像素既验证几何又验证
  pts 前进, 逐字节可断言。
- **duration**: 读元素 `data-duration` 属性(缺省 2.0)。
- **开关**: `HN_MEDIA_AUTOPLAY=1` / 元素 `autoplay` 属性; 环境变量不设时
  媒体会话惰性建立但停在 IDLE(不注入宿主时整套代码路径零激活)。
- state/duration/position 语义与真宿主逐条对齐(合成宿主是真宿主的
  **规约测试替身**, 行为差异就是 bug)。

探针(tools/media_probe.py 第一/二段)走 `hncore paint` 的 BITMAP/MEDIA 行断言:
autoplay 起播、状态转移序列、loop 回绕、seek 钳制、纯 audio 无 BITMAP、
HN_CMD_BITMAP 几何与元素盒一致。**全平台可跑**(hncore 是零依赖 C99)。

### 8.2 tools/media_probe.c(真 AVFoundation; 原生 cc 编译运行)

- **构建/运行**(仓库根):

  ```sh
  cc -std=c99 -O1 -I Sources/CHtmlNative/include -I Sources/CHtmlNative \
     tools/media_probe.c platform/hnp_media_macos.c \
     <引擎源 + vendor/wasm3 11 源> -o /tmp/hn_media_probe
  /tmp/hn_media_probe <sample.mp4> <tone.m4a>
  ```

  (不链 rt/hn_rt.c —— 探针直用引擎 + hn_wasm + 平台宿主, 不引 QuickJS
  条件编译。)

  退出码 = 失败项数(0 = 通过); 与 sysbridge_probe.c/rt_probe.c 的既有
  探针口径一致(ck() 计数 + main 返回 fails)。
- **Tier 1(无参数, 恒跑)**: supported()==1; open 不存在路径 → NULL+err;
  NULL 句柄全 API 安全(close(NULL) 幂等); wasm 段(load add.wasm → add(2,3)=5)。
- **Tier 2(带 fixture 参数, fixture 缺失时打印一行跳过原因而非静默)**:
  duration≈2.0±0.1; ready 在 3s 内; play 后 frames>0 且 420v→RGBA 尺寸
  64x64; 播放中 seek(1.2) 落位 ±0.05; play→seek→pause 序列落位; 纯音频
  frames==0 且 duration>0; volume/muted 写读一致。断言用范围不用精确像素
  (真解码非逐字节确定)。
- **fixture**(tools/fixtures/media/, 实现代理生成后入库, 命令即文档):

  ```sh
  ffmpeg -y -f lavfi -i testsrc=size=64x64:rate=10:duration=2 \
         -f lavfi -i sine=frequency=440:duration=2 \
         -c:v libx264 -pix_fmt yuv420p -c:a aac -shortest sample.mp4
  ffmpeg -y -f lavfi -i sine=frequency=440:duration=1.5 -c:a aac tone.m4a
  ```

  (本设计预演时用同一命令生成于 /tmp/mprobe/, 22KB/14KB, 全管线验证通过。)
- **media_probe.py 薄封装**: 组装上述 cc 命令(源清单与 build 脚本一致)、
  编译、运行、断言退出码 0; 编译器或 fixture 缺失 → 打印原因并按配置
  决定 exit(门禁下 exit 1)。它**只**做编译+运行+退出码, 不解析输出语义
  (语义断言都在 .c 里)。

---

## 9. 明确不做(写进代码注释, 防跑偏)

- `<source>` 子元素与 `type` 协商(只认元素 `src` 属性)。
- 字幕/`<track>`/textTracks; DRM/EME/MediaSource; 流媒体(HLS/DASH —— url
  只收本地路径/file://, http(s) 一律 open 失败并写明原因)。
- FFmpeg: 不打包、不链接、不 dlopen(解码 = OS 媒体框架, 这是本设计的
  第一性判断)。
- wasm 软解码: WASM 只跑纯计算导出函数, **不用** wasm 解码视频/音频;
  无 WASI、无内存导出操作、无表/间接调用约定的额外封装(v1 仅 i32 纯函数)。
- Windows Media Foundation 与 Linux GStreamer: **ABI 留位不实现** ——
  hn_platform.h 追加段全平台可见, 非苹果平台 supported()=0, 引擎与桥在
  无媒体平台上整体降级(无宿主 = 媒体属性与元素解析照常, 只是不播)。
- 浏览器控件 UI(播放条/进度条/全屏按钮)、poster 属性、playbackRate、
  WebAudio/音量以外的音频处理、多音轨选择、画中画。

---

## 10. 实现者备忘(全部实测过, 别再踩)

1. macOS dlsym 用 `RTLD_DEFAULT`(= `(void*)-2`), 不是 NULL
   (rt/hn_rt.c:478 起的既有写法在 macOS 恒失败, 已实测)。
2. NSApplication finishLaunching 必须先于 AVPlayer 异步加载, 否则
   duration 永远 NaN(hnp_init 已装过则幂等)。
3. runloop 泵必须有界日期; distantFuture 在 pause 态永久阻塞(实测挂死)。
4. CVPixelBuffer 实际是 '420v' planar(BGRA 请求被忽略); 用 plane API 取
   平面, stride 用 GetBytesPerRowOfPlane(实测 64px 帧 plane0=64,
   而 GetBytesPerRow 给出无意义 104)。
5. 视频轨 pause 态直接 seek 位置停在 0.000; 必须 play→seek→settle→pause
   (实测视频 0.494/音频 0.500 精确落位)。
6. ended 用 position≥duration 推导; rate 在播完时不归零(实测 1.00)。
7. CMTime(24B 结构体)按值传给 objc_msgSend 在 arm64 正确, 前提是函数指针
   原型写对(`R (*)(id, SEL, CMTime)`); 返回值同理(duration/currentTime)。
8. `AVPlayerItemVideoOutput` 在本运行时只有 `initWithPixelBufferAttributes:`
   (无 dispatchQueue 变体); copyPixelBuffer 用
   `copyPixelBufferForItemTime:itemTimeForDisplay:(t, NULL)`。
9. dispatch_queue_create 无需 dlopen(libSystem 直出), 但 v1 输出对象可以
   不传队列(pull 模式轮询即可, 预演未建队列分支)。
10. zig 0.13 的 wasm32-freestanding 不支持 `-Wl,--export-all`; 用
    `-Wl,--export=<fn>` + `--no-standard-libraries`。
11. wasm3 新 master(2026-09)比旧文档多了 m3_deterministic/snapshot/validate/
    xxh64 四文件 —— 以本文 §7 的 11 文件清单为准(逐文件链接实测过)。

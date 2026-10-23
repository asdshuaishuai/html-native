/* hn_rt.c — 公共运行时层实现(单源, 三平台共享)
 *
 * 全部逻辑曾散落在 hnweb_macos.c / hnweb_linux.c / hnwin.c / hn_daemon.c
 * 各自的一份(实测重复: read_all×3, hx_perform×3, sys_fragment×2, store×2)。
 * 这里收敛为一份; 平台壳只留 hn_platform.h 的实现 + ~150 行主循环。
 *
 * 依赖: 引擎(Sources/CHtmlNative) + hnsoft(软件光栅)。
 * 不依赖任何平台 API —— 平台无关是这一层的存在理由。
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <ctype.h>   /* tolower(createElement 的标签归一) */
#include <unistd.h>
#include <sys/stat.h>
#include <time.h>
#include <dlfcn.h>
#include <math.h>   /* NAN(hnWasmCall 失败返回值, 经 quickjs.h 的 JS_NAN) */
#include "hn_rt.h"
#include "hn_sys.h"
#include "hn_http.h"
/* 引擎内部接口(动画活跃类别 + ambient 轻重画): 性能治理需要
   hn_context_anim_tick_ex 的类别出口与 hn_context_repaint_light ——
   这两个钩子是引擎专为帧节拍分级开的缝, 不触及其他内部结构。 */
#include "hn_internal.h"
#include "hn_json.h"   /* hx-vals/hx-headers 的 JSON 解析 */
#ifdef HN_HAVE_QUICKJS
#include <quickjs.h>
#endif

/* ---------------- 结构 ---------------- */

struct hn_rt {
    /* input[type=range] 拖拽态(pointer_move 消费; 输入路径在 QuickJS
       守卫之外 —— 字段必须对所有构建可见) */
    int range_drag;
    char range_id[64];
    /* handwired #1 事件时间戳(所有构建可见):
       mono_last_q = 已消费到的单调钟位置(1/64ms 栅格, 输入与帧共享同一条链);
       ev_consumed_ms = 自上帧以来**事件已推进**的动画时长 —— 帧调度扣除,
       防止同一真实跨度被事件+帧各推进一次(handwired "状态只在事件/整定点
       写回"的 hn 落地)。 */
    double mono_last_q;
    int mono_have;
    double ev_consumed_ms;    hn_context *ctx;
    int w, h;
    float scale;                /* HiDPI 设备缩放(位图 = 逻辑 × scale; 布局/
                                   命中/窗口表面恒逻辑 px; 缺省 1.0 —— 不设置
                                   时离屏/shot 路径与旧版逐位一致) */
    int transparent;
    unsigned char *bgra;        /* 预乘 BGRA(平台 blit 的直接输入) */
    size_t bgra_cap;            /* bgra 容量(字节; **只增不减**复用 —— 每帧
                                   换算写满全图, 免掉稳态帧的 malloc/free churn) */
    unsigned char *soft_buf;    /* hnsoft RGBA 复用缓冲(hnsoft_render_reuse_
                                   scaled 的 slot; 归 rt 持有, 只增不减) */
    int soft_cap;               /* soft_buf 容量(字节; 0 = 未分配) */
    hn_node *hover;
    hn_node *focus;
    char id[128];              /* store 隔离键 */
    void *js_rt;               /* JSRuntime *(QuickJS; lazy init) */
    void *js_ctx;              /* JSContext * */
    /* JS 事件层节流状态(字段本身与 QuickJS 无关, 无 JS 构建也占这几字节):
       scroll 事件每帧至多一次(hn_rt_frame 重臂); mousemove 坐标/命中变化
       才派发。 */
    int scroll_armed;          /* 1 = 本帧还没发过 scroll(hn_rt_open 置 1) */
    int mm_valid;              /* mousemove 上次派发状态有效 */
    float mm_x, mm_y;          /* mousemove 上次派发坐标 */
    hn_node *mm_hit;           /* mousemove 上次派发命中元素 */
    /* 绘制挂起(app 事件排干窗口): 爆发输入(hover 连变/滚轮/编辑)只标脏,
       队尾 hn_rt_paint_flush 一次整渲染 —— 中间态像素从不被呈现,
       N 次 layout+raster 合 1(与 blit 合并配套, 内存带宽第二刀)。 */
    int paint_defer;
    int paint_dirty;
    /* GPU 直呈(v2): 平台壳注入的 CAMetalLayer(不透明指针); presented =
       最近一帧经 present 出帧(shot/blit 据此分流 —— shot 需要像素时
       强制走一次 CPU/readback 渲染)。 */
    void *gpu_present_layer;
    int presented;
    /* 桌面画板呈现(画刷铺色): paint_t0 = 起始单调毫秒(0 = 未开始/已结束),
       paint_done 防重入; paint_bg = 画纸底色(不透明窗未铺区域的混合目标,
       rt_raster 每帧随清屏色记账)。reveal_mode 见 hn.h hn_reveal。
       paint_eligible = **逐像素透明窗**才 eligible(用户裁决: "直接在
       桌面上绘制这个窗口" —— 未铺区域 alpha=0 透出桌面; 不透明窗会先
       出现一块画板再画内容, 是错的, 缺省只走平台淡入)。 */
    unsigned long paint_t0;
    int paint_done;
    int paint_eligible;
    hn_color paint_bg;
    int reveal_mode;
    /* 多 html 项目模式: page_path = 当前页相对 cwd 的路径(NULL = 内联文档,
       导航关闭); back 栈存此前页(hn_rt_back 回退, 上限 64)。 */
    char *page_path;
    char **back;
    int back_n, back_cap;
    /* 遮罩帧代数: rt_raster 每次成功出帧自增; apply 记上次盖遮罩的代数 ——
       同一代数上二次 apply = 原地叠加污染(遮罩乘两次), 守卫强制先重出
       干净帧(静态页壳只强制 blit 不重光栅, 不守卫会在几十拍内把内容
       乘没 —— 实测: 不透明页收敛成纯画板, 透明页收敛成全透明)。 */
    unsigned long raster_gen;
    unsigned long paint_applied_gen;
    /* 轮询表(hx-trigger="every Ns"): 时钟域是**帧步进虚拟钟**(poll_clock_ms,
       每帧 += dt)而非墙钟 —— 与下方 JS 定时器的 clock_ms 同一口径(探针喂
       固定 16 ⇒ 轮询语义确定性可断言, --advance 虚拟快进也能到期; 真窗壳
       喂真实 dt ⇒ 行为与墙钟一致)。此前用 CLOCK_MONOTONIC: --advance 在
       几个真实毫秒里推进 2500ms 虚拟时间, 墙钟没动 → 轮询永不到期,
       headless 下列 trigger 无法验收(实测 hx-test-every 占位不动)。 */
    double poll_clock_ms;      /* 轮询域虚拟单调钟(ms; 随帧步进) */
    struct { hn_node *node; double due, sec; } polls[16];   /* due/sec 域 = 秒 */
    int poll_n;
    int poll_built;
#ifdef HN_HAVE_QUICKJS
    /* JS 时钟(虚拟单调毫秒): 每帧 += dt_ms。真窗壳喂真实间隔 ⇒ 与单调钟
       一致; 探针喂固定 16 ⇒ 定时器语义可确定性断言(不受真实睡眠抖动影响)。 */
    double clock_ms;
    /* 定时器表(setTimeout/setInterval 共用; 上限 64) */
    struct {
        int id;              /* 句柄(>0; 0 = 空槽) */
        double due;          /* 到期时刻(clock_ms 域的绝对值) */
        double interval_ms;  /* >0 = 周期定时器; 0 = 单次 */
        JSValue func;        /* 回调(持有引用; hn_rt_close 统一释放) */
        JSValue args;        /* 附加参数(JS Array; JS_UNDEFINED = 无) */
    } timers[64];
    int timer_n;             /* 占用槽位数(0 = 表空) */
    int timer_seq;           /* 句柄发号器 */
    /* requestAnimationFrame 队列: 下一帧开头按注册序执行一次 */
    struct { int id; JSValue cb; } raf_q[64];
    int raf_n;
    int raf_seq;
    /* JS 事件监听器表(addEventListener; 上限 256, 与定时器表同款管理):
       target = 元素 id 串或 "document"/"window" —— 只存 id 串不存节点指针,
       派发时按 id 现查命中链, 文档换装天然无悬挂。capture 记录但 v1 派发
       是单相冒泡(见 rt_js_dispatch 头注)。 */
#define HN_LISTENERS_MAX 256
    struct {
        int id;              /* 句柄(>0; 0 = 空槽) */
        char target[64];
        char type[24];
        int capture;
        JSValue fn;          /* 回调(持有引用; 关闭/换装统一释放) */
    } listeners[HN_LISTENERS_MAX];
    int listen_n;
    int listen_seq;
    /* input[type=range] 拖拽态(按住期间 pointer move 持续设值):
       记元素 id 而非节点指针 —— 文档替换/arena 压缩会让指针悬空。 */
    /* HTTP async scope + pending fetch map: hn.fetch() returns Promise immediately,
       request submitted to scope, completed in hn_rt_frame via drain callback */
    void *http_scope;          /* hn_http_scope * (lazy init on first fetch) */
    uint64_t fetch_id_seq;     /* monotonic ID generator */
    struct {
        uint64_t id;           /* scope request ID */
        JSValue resolve;       /* Promise resolve function */
        JSValue reject;        /* Promise reject function */
    } pending_fetches[32];     /* max 32 concurrent fetches per runtime */
    int pending_fetch_n;
#endif
};

/* ---------------- 文件读取 / 资产后端 ---------------- */

typedef struct { char *path; char *data; size_t len; } asset_ent;
static asset_ent g_assets[64];
static int g_asset_n;

static char *read_all(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n < 0) { fclose(f); return NULL; }
    char *buf = (char *)malloc((size_t)n + 1);
    if (!buf) { fclose(f); return NULL; }
    size_t got = fread(buf, 1, (size_t)n, f);
    fclose(f);
    buf[got] = 0;
    *len = got;
    return buf;
}

static const char *asset_load(void *ctx, const char *path, size_t *len) {
    (void)ctx;
    for (int i = 0; i < g_asset_n; i++)
        if (!strcmp(g_assets[i].path, path)) { *len = g_assets[i].len; return g_assets[i].data; }
    size_t n = 0;
    char *d = read_all(path, &n);
    if (!d) return NULL;
    if (g_asset_n < 64) {
        g_assets[g_asset_n].path = strdup(path);
        g_assets[g_asset_n].data = d;
        g_assets[g_asset_n].len = n;
        g_asset_n++;
    }
    *len = n;
    return d;
}

static const hn_asset_backend g_assets_be = { NULL, asset_load };

/* 媒体宿主槽(仿 g_assets_be): 壳经 hn_rt_set_media 注入, hn_rt_open 时
   装进每个 context。NULL = 媒体禁用(引擎整体降级: 元素照常解析只是不播)。 */
static const hn_media_host *g_media_be = NULL;

void hn_rt_set_media(const hn_media_host *host) { g_media_be = host; }

/* <link rel=stylesheet> 内联(引擎不做 I/O; 相对 cwd —— 调用方先 chdir 文档目录) */
static char *load_css_links(const char *html, size_t *out_len) {
    const char *p = html;
    char *out = NULL;
    size_t cap = 0, used = 0;
    while ((p = strstr(p, "<link")) != NULL) {
        const char *rel = strstr(p, "stylesheet");
        const char *href = strstr(p, "href=");
        const char *tag_end = strchr(p, '>');
        if (rel && href && tag_end && href < tag_end) {
            char q = href[5];
            if (q == '"' || q == '\'') {
                const char *v = href + 6;
                const char *ve = strchr(v, q);
                if (ve && ve < tag_end) {
                    size_t vl = (size_t)(ve - v);
                    char path[1024];
                    if (vl + 1 < sizeof(path)) {
                        memcpy(path, v, vl);
                        path[vl] = 0;
                        size_t cl = 0;
                        char *css = read_all(path, &cl);
                        if (css) {
                            size_t need = used + cl + 2;
                            if (need > cap) { cap = need * 2 + 256; out = (char *)realloc(out, cap); }
                            if (out) { memcpy(out + used, css, cl); used += cl; out[used++] = '\n'; }
                            free(css);
                        }
                    }
                }
            }
        }
        p = tag_end ? tag_end : p + 5;
    }
    if (out) out[used] = 0;
    *out_len = used;
    return out;
}

/* ---------------- 文本后端(FreeType; 无则引擎等宽估算) ---------------- */

static float tb_measure(void *ctx, const hn_font_desc *font, const char *utf8, size_t len) {
    (void)ctx;
    return hnsoft_measure(font, utf8, len);
}
static void tb_metrics(void *ctx, const hn_font_desc *font,
                       float *ascent, float *descent, float *leading) {
    (void)ctx;
    hnsoft_metrics(font, ascent, descent, leading);
}
static hn_text_backend g_tb = { NULL, tb_measure, tb_metrics };

/* ---------------- sys:// 桥 ----------------
 * 路由实现单源在 rt/hn_sys.c(hn_sys_fragment); store 经回调带各自命名空间。
 * 此处仅保留 rt 的 store 后端(与 hn_rt_store_* 同一文件)与兼容入口。 */

static char *store_path(const char *store_id) {
    const char *home = getenv("HOME");
    if (!home) return NULL;
    char *p = (char *)malloc(strlen(home) + strlen(store_id) + 64);
    if (!p) return NULL;
    sprintf(p, "%s/.html-native/store/%s.json", home, store_id);
    return p;
}

int hn_rt_store_get(const char *store_id, const char *key, char *out, size_t cap) {
    char *sp = store_path(store_id);
    if (!sp) return 0;
    size_t n = 0;
    char *d = read_all(sp, &n);
    free(sp);
    if (!d) return 0;
    char pat[256];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(d, pat);
    int found = 0;
    if (p && (p = strchr(p + strlen(pat), ':')) != NULL) {
        p++;
        while (*p == ' ') p++;
        if (*p == '"') {
            p++;
            size_t i = 0;
            while (*p && *p != '"' && i + 1 < cap) out[i++] = *p++;
            out[i] = 0;
            found = 1;
        }
    }
    free(d);
    return found;
}

int hn_rt_store_set(const char *store_id, const char *key, const char *val) {
    char *sp = store_path(store_id);
    if (!sp) return 0;
    char *obj = NULL;
    size_t obj_n = 0, cap = 4096;
    obj = (char *)malloc(cap);
    if (!obj) { free(sp); return 0; }
    obj[0] = 0;
    int have_key = 0;
    size_t n = 0;
    char *d = read_all(sp, &n);
    if (d) {
        const char *p = d;
        while (*p) {
            while (*p && (*p == ' ' || *p == '{' || *p == ',' || *p == '\n')) p++;
            if (*p != '"') break;
            const char *ks = ++p;
            while (*p && *p != '"') p++;
            size_t kl = (size_t)(p - ks);
            if (*p) p++;
            while (*p && *p != ':') p++;
            if (*p) p++;
            while (*p == ' ') p++;
            if (*p != '"') break;
            const char *vs = ++p;
            while (*p && *p != '"') p++;
            size_t vl = (size_t)(p - vs);
            if (*p) p++;
            int is_t = (kl == strlen(key) && !strncmp(ks, key, kl));
            const char *wv = is_t ? val : vs;
            size_t wl = is_t ? strlen(val) : vl;
            while (obj_n + kl + wl + 8 >= cap) { cap *= 2; obj = (char *)realloc(obj, cap); }
            if (!obj) { free(d); free(sp); return 0; }
            if (obj_n) obj[obj_n++] = ',';
            obj_n += (size_t)snprintf(obj + obj_n, cap - obj_n, "\"%.*s\":\"%.*s\"",
                                      (int)kl, ks, (int)wl, wv);
            if (is_t) have_key = 1;
        }
        free(d);
    }
    if (!have_key) {
        while (obj_n + strlen(key) + strlen(val) + 8 >= cap) { cap *= 2; obj = (char *)realloc(obj, cap); }
        if (!obj) { free(sp); return 0; }
        if (obj_n) obj[obj_n++] = ',';
        obj_n += (size_t)snprintf(obj + obj_n, cap - obj_n, "\"%s\":\"%s\"", key, val);
    }
    char dir[1024];
    snprintf(dir, sizeof(dir), "%s", sp);
    char *sl = strrchr(dir, '/');
    if (sl) { *sl = 0; mkdir(dir, 0755); }
    FILE *f = fopen(sp, "wb");
    int ok = 0;
    if (f) { fprintf(f, "{%s}", obj); fclose(f); ok = 1; }
    free(obj);
    free(sp);
    return ok;
}

/* 文档内联 <script> 执行(QuickJS 有实现; 无 JS 引擎时是 no-op stub) */
static void run_doc_scripts(hn_rt *rt);
/* JS 定时器/RAF 宿主绑定与帧驱动(QuickJS 有实现; 无 JS 构建为 no-op stub) */
static void js_scope_bind_rt(hn_rt *rt);
static void js_scope_unbind_rt(hn_rt *rt);
static int js_timers_drain(hn_rt *rt, int *repaint);
/* JS 事件层(addEventListener 注册表与命中链冒泡派发; QuickJS 有实现,
   无 JS 构建为恒 0 的 stub —— 调用点无需知道差别) */
static void js_listeners_clear(hn_rt *rt);
static int rt_js_has_listener(hn_rt *rt, const char *type);
static int rt_js_dispatch(hn_rt *rt, const char *type, hn_node *target,
                          float x, float y, const char *utf8, int key, int detail);
/* 核心动画实体事件出口 → JS 派发(hn-timeline:complete 等; 单相冒泡同款) */
static void rt_event_sink(hn_context *c, hn_node *target, const char *type,
                          const char *detail, void *ud) {
    (void)c;
    rt_js_dispatch((hn_rt *)ud, type, target, 0, 0, detail, 0, 0);
}
static void rt_range_set_from_x(hn_rt *rt, hn_node *inp, float x);
static int rt_native_click(hn_rt *rt, hn_node *n, float click_x);

/* ---- handwired #1: 事件在时间戳处应用 ----
 * 单调钟读数(1/64ms 栅格量化, 与帧时间戳同一手势)。Windows 走 QPC,
 * POSIX 走 CLOCK_MONOTONIC —— 自包含, 不拉平台层依赖。 */
static double rt_mono_q(void) {
#if defined(_WIN32)
    LARGE_INTEGER f, c2;
    if (!f.QuadPart) QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&c2);
    double ms = (double)c2.QuadPart * 1000.0 / (double)f.QuadPart;
#else
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    double ms = (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
#endif
    return floor(ms * 64.0) / 64.0;
}

/* 每个输入入口先调: 把动画钟推进到**事件到达时刻**(而非等下帧), 消耗量
   记入 ev_consumed_ms 供帧调度扣除(同一真实跨度不双推进)。返回推进档位
   (HN_ANIM_*, 0 = 无动画可推进); 调用方据此决定是否即时重渲染。 */
static int rt_events_advance(hn_rt *rt) {
    if (!rt || !rt->ctx) return 0;
    double q = rt_mono_q();
    double dt = rt->mono_have ? q - rt->mono_last_q : 0.0;
    rt->mono_last_q = q;
    rt->mono_have = 1;
    if (dt < 0.0) dt = 0.0;
    if (dt > 250.0) dt = 250.0;                 /* 长眠限步(与帧同口径) */
    if (dt < 1.0 / 64.0) return 0;              /* 不足一个栅格步: 不推进 */
    int cls = 0;
    if (hn_context_anim_tick_ex(rt->ctx, (float)dt, &cls))
        rt->ev_consumed_ms += dt;
    return cls;
}

static int rt_sys_store_get(void *ctx, const char *key, char *out, size_t cap) {
    return hn_rt_store_get((const char *)ctx, key, out, cap);
}
static int rt_sys_store_set(void *ctx, const char *key, const char *value) {
    return hn_rt_store_set((const char *)ctx, key, value);
}

/* Compatibility entry for callers that already have a store namespace. */
char *hn_rt_sys_fragment(const char *url, const char *form_body, const char *store_id) {
    hn_sys_host host = { (void *)store_id, rt_sys_store_get, rt_sys_store_set };
    return hn_sys_fragment(&host, url, form_body);
}


static int page_read(const char *rel, char **out, size_t *out_len);
static void rt_render_trace(const char *tag);

/* htmx 方法词表: 属性名 → HTTP 动词(查询/载荷口径见 hx_perform)。
   2026-10-22 起全五动词(get/post/put/delete/patch —— put/delete/patch
   为本轮 parity 补齐)。 */
static const char *const HX_METHOD_ATTRS[] = {
    "hx-get", "hx-post", "hx-put", "hx-delete", "hx-patch"
};
static const char *const HX_METHOD_VERBS[] = {
    "GET", "POST", "PUT", "DELETE", "PATCH"
};

static hn_node *hx_carrier(hn_node *n) {
    if (!n) return NULL;
    for (int i = 0; i < 5; i++)
        if (hn_node_attr(n, HX_METHOD_ATTRS[i])) return n;
    for (int i = 0; i < 5; i++) {
        hn_node *a = hn_node_ancestor_with_attr(n, HX_METHOD_ATTRS[i]);
        if (a) return a;
    }
    return NULL;
}

/* 载体上的 (动词, URL): 按词表序取第一个声明的属性 */
static void hx_method_of(hn_node *carrier, const char **verb, const char **url) {
    *verb = "GET";
    *url = NULL;
    for (int i = 0; i < 5; i++) {
        const char *u = hn_node_attr(carrier, HX_METHOD_ATTRS[i]);
        if (u) { *url = u; *verb = HX_METHOD_VERBS[i]; return; }
    }
}

/* ---- htmx 兼容语义(2026-10-15 htmx 轮) ----
 * hx-target: "this"/"" = 载体; "closest <sel>" / "find <sel>" /
 *   "next [sel]" / "previous [sel]"; 其余按 CSS 选择器查(兼容旧 id 直查)。
 * hx-swap: innerHTML(缺省)/outerHTML/beforeend/afterbegin/afterend/
 *   delete/none。
 * hx-trigger: 事件名逗号列表(click 缺省; change = 值变化触发;
 *   load/every 已有), 修饰符 delay:N[ms|s](防抖) / changed(值未变跳过)。
 * hx-vals / hx-headers: JSON 对象(合并进表单体 / HTTP 头; "js:" 前缀不支持)。
 * 事件: htmx:beforeRequest(pd 可取消) / htmx:afterRequest / htmx:afterSwap。 */

/* hx-trigger 串里是否含事件名 token(逗号分隔; 忽略修饰符) */
static int hx_trigger_has(const char *trig, const char *name) {
    if (!trig) return 0;
    size_t nl = strlen(name);
    for (const char *p = trig; *p;) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        const char *st = p;
        while (*p && *p != ',') p++;
        const char *en = p;
        while (en > st && (en[-1] == ' ' || en[-1] == '\t')) en--;
        /* token 首词(去修饰符): 与 name 比较大小写不敏感 */
        const char *w = st;
        while (w < en && *w != ' ' && *w != '\t') w++;
        size_t wl = (size_t)(w - st);
        if (wl == nl) {
            int same = 1;
            for (size_t i = 0; i < nl; i++)
                if (tolower((unsigned char)st[i]) != tolower((unsigned char)name[i])) { same = 0; break; }
            if (same) return 1;
        }
    }
    return 0;
}

/* hx-trigger 的 delay:N[ms|s] 修饰符 → 毫秒; 无则 0 */
static int hx_trigger_delay(const char *trig, const char *event) {
    if (!trig || !event) return 0;
    size_t el = strlen(event);
    for (const char *p = trig; *p;) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        const char *st = p;
        while (*p && *p != ',') p++;
        const char *en = p;
        while (en > st && (en[-1] == ' ' || en[-1] == '\t')) en--;
        /* token 以事件名开头(词边界)? */
        if ((size_t)(en - st) > el &&
            strncasecmp(st, event, el) == 0 &&
            (st[el] == ' ' || st[el] == '\t')) {
            /* 找 delay: */
            for (const char *q = st + el; q < en; q++) {
                if (strncasecmp(q, "delay:", 6) == 0) {
                    double v = strtod(q + 6, NULL);
                    if (v <= 0) return 0;
                    int ms = (strstr(q + 6, "ms") && strncmp(q + 6 + (strstr(q+6,"ms")-(q+6)) - 2, "s", 1) != 0)
                             ? (int)v : (int)(v * 1000.0);
                    if (strstr(q + 6, "ms")) {
                        const char *u = strstr(q + 6, "ms");
                        if (u > q + 6 && u[-1] != 's') ms = (int)v;
                        else ms = (int)(v * 1000.0);
                    }
                    if (ms < 10) ms = 10;
                    if (ms > 60000) ms = 60000;
                    return ms;
                }
            }
        }
    }
    return 0;
}

/* hx-trigger 含 changed 修饰符(针对 event)? */
static int hx_trigger_changed(const char *trig, const char *event) {
    if (!trig || !event) return 0;
    size_t el = strlen(event);
    for (const char *p = trig; *p;) {
        while (*p == ' ' || *p == '\t' || *p == ',') p++;
        if (!*p) break;
        const char *st = p;
        while (*p && *p != ',') p++;
        const char *en = p;
        while (en > st && (en[-1] == ' ' || en[-1] == '\t')) en--;
        if ((size_t)(en - st) > el &&
            strncasecmp(st, event, el) == 0 &&
            (st[el] == ' ' || st[el] == '\t')) {
            for (const char *q = st + el; q + 7 <= en; q++) {
                if (strncasecmp(q, "changed", 7) == 0 &&
                    (q + 7 == en || q[7] == ' ' || q[7] == '\t'))
                    return 1;
            }
        }
    }
    return 0;
}

/* hx-target 解析(htmx 语法)。命中返回节点并置 *mode_out 的目标修正为 NULL;
 * 未命中返回 NULL。 */
static hn_node *hx_resolve_target(hn_rt *rt, hn_node *carrier) {
    hn_doc *doc = hn_context_doc(rt->ctx);
    const char *tid = hn_node_attr(carrier, "hx-target");
    if (!tid || !tid[0] || !strcmp(tid, "this")) return carrier;
    hn_context *c = rt->ctx;
    if (!strncmp(tid, "closest ", 8)) {
        for (hn_node *it = carrier; it; it = it->parent)
            if (hn_node_matches_selector(doc, it, tid + 8, c)) return it;
        return carrier;
    }
    if (!strncmp(tid, "find ", 5)) {
        hn_node *hits[8];
        char scoped[512];
        const char *id = hn_node_attr(carrier, "id");
        if (id && *id) {
            snprintf(scoped, sizeof(scoped), "#%s %s", id, tid + 5);
            /* id 含特殊字符的页面少见; 查询失败回退载体 */
            int n = hn_doc_query_selector(doc, scoped, c, hits, 8);
            if (n > 0) return hits[0];
        }
        /* 无 id 载体: 遍历子树找首个命中 */
        hn_node *stack[256];
        int sp = 0, guard = 0;
        stack[sp++] = carrier;
        while (sp > 0 && guard++ < 100000) {
            hn_node *it = stack[--sp];
            if (it != carrier && hn_node_matches_selector(doc, it, tid + 5, c)) return it;
            for (hn_node *ch = it->first; ch; ch = ch->next)
                if (ch->kind == HN_ELEM && sp < 256) stack[sp++] = ch;
        }
        return carrier;
    }
    if (!strncmp(tid, "next", 4) && (tid[4] == 0 || tid[4] == ' ')) {
        hn_node *sib = carrier->next;
        const char *want = tid[4] == ' ' ? tid + 5 : NULL;
        for (; sib; sib = sib->next) {
            if (sib->kind != HN_ELEM) continue;
            if (!want || hn_node_matches_selector(doc, sib, want, c)) return sib;
        }
        return carrier;
    }
    if (!strncmp(tid, "previous", 8) && (tid[8] == 0 || tid[8] == ' ')) {
        hn_node *sib = carrier->prev;
        const char *want = tid[8] == ' ' ? tid + 9 : NULL;
        for (; sib; sib = sib->prev) {
            if (sib->kind != HN_ELEM) continue;
            if (!want || hn_node_matches_selector(doc, sib, want, c)) return sib;
        }
        return carrier;
    }
    /* CSS 选择器(兼容旧"id 直查": 先选择器, 无命中回退 id) */
    hn_node *hits[4];
    int n = hn_doc_query_selector(doc, tid, c, hits, 4);
    if (n > 0) return hits[0];
    return hn_doc_find_by_id(doc, tid);
}

/* hx-swap 名 → 模式(含 htmx 全名与别名)。**首 token** 匹配 —— 属性可带
   修饰符后缀("innerHTML swap:100ms", 修饰符由 hx_swap_delay_parse 消费) */
static hn_swap_mode hx_swap_parse(const char *sm) {
    hn_swap_mode m = HN_SWAP_INNER;
    if (sm && *sm) {
        char first[64];
        size_t i = 0;
        while (sm[i] && sm[i] != ' ' && i + 1 < sizeof(first)) { first[i] = sm[i]; i++; }
        first[i] = 0;
        if (!strcmp(first, "outerHTML")) m = HN_SWAP_OUTER;
        else if (!strcmp(first, "afterend")) m = HN_SWAP_AFTEREND;
        else if (!strcmp(first, "beforeend")) m = HN_SWAP_APPEND;
        else if (!strcmp(first, "afterbegin")) m = HN_SWAP_PREPEND;
        else if (!strcmp(first, "append")) m = HN_SWAP_APPEND;
        else if (!strcmp(first, "prepend")) m = HN_SWAP_PREPEND;
        else if (!strcmp(first, "delete")) m = HN_SWAP_DELETE;
        else if (!strcmp(first, "none")) m = HN_SWAP_NONE;
    }
    return m;
}

/* hx-swap 属性 → (模式, swap 延迟 ms)。属性形态: "innerHTML swap:100ms"
   (首 token = 模式, 其余 swap:N[ms|s] 生效; settle 与 ignoreTitle 等
    其余修饰符诚实忽略)。
   返回延迟 ms(0 = 立即)。 */
static float hx_swap_delay_parse(const char *sm) {
    if (!sm || !*sm) return 0;
    float delay = 0;
    char buf[256];
    snprintf(buf, sizeof(buf), "%s", sm);
    for (char *tok = strtok(buf, " "); tok; tok = strtok(NULL, " ")) {
        if (strncmp(tok, "swap:", 5) == 0) {
            const char *v = tok + 5;
            char *end = NULL;
            double n = strtod(v, &end);
            if (end && end != v) {
                if (end[0] == 's' && end[1] == 0) n *= 1000.0;   /* "2s" */
                delay = (float)n;
            }
        }
        /* settle:/ignoreTitle 等其余修饰符: 诚实忽略 */
    }
    return delay;
}

/* hx-vals/hx-headers 的 JSON 对象 → "k=urlencode(v)" 追加到 body。
   返回追加后的长度; 非对象/解析失败返回 0(不追加)。嵌套值按 hn_json_str
   字符串化; 数字/布尔按标准序列化。 */
static int hx_vals_append(char *body, size_t cap, const char *json, size_t jlen,
                          int *first_out, int *n_added) {
    if (!json || !jlen) return 0;
    char *buf = (char *)malloc(jlen + 1);
    if (!buf) return 0;
    memcpy(buf, json, jlen);
    buf[jlen] = 0;
    void *arena = NULL;
    hn_json *root = hn_json_parse(buf, jlen, &arena);
    if (!root || root->kind != HN_JSON_OBJ) { hn_json_free(arena); free(buf); return 0; }
    int used = (int)strlen(body);
    int added = 0;
    for (int i = 0; i < root->count; i++) {
        hn_json *it = root->items[i];
        if (!it || !it->key) continue;
        char vbuf[256];
        if (it->kind == HN_JSON_STR) snprintf(vbuf, sizeof(vbuf), "%s", it->str ? it->str : "");
        else if (it->kind == HN_JSON_NUM) snprintf(vbuf, sizeof(vbuf), "%g", it->num);
        else if (it->kind == HN_JSON_BOOL) snprintf(vbuf, sizeof(vbuf), it->boolean ? "true" : "false");
        else continue;   /* 嵌套对象/数组: v1 不支持(如实截断) */
        /* k=v(&k=v): k/v 做 percent-encode(保留字母数字与 -_.,~) */
        char enc_k[128], enc_v[512];
        size_t ek = 0;
        for (const char *q = it->key; *q && ek + 3 < sizeof(enc_k); q++) {
            unsigned char ch = (unsigned char)*q;
            if (isalnum(ch) || strchr("-_.~", ch)) enc_k[ek++] = (char)ch;
            else ek += (size_t)snprintf(enc_k + ek, 4, "%%%02X", ch);
        }
        enc_k[ek] = 0;
        size_t ev = 0;
        for (const char *q = vbuf; *q && ev + 3 < sizeof(enc_v); q++) {
            unsigned char ch = (unsigned char)*q;
            if (isalnum(ch) || strchr("-_.~", ch)) enc_v[ev++] = (char)ch;
            else ev += (size_t)snprintf(enc_v + ev, 4, "%%%02X", ch);
        }
        enc_v[ev] = 0;
        int n = snprintf(body + used, cap > (size_t)used ? cap - (size_t)used : 0,
                         "%s%s=%s", (used > 0 || added > 0) ? "&" : "", enc_k, enc_v);
        if (n < 0 || (size_t)(used + n) >= cap) break;
        used += n;
        added++;
    }
    hn_json_free(arena);
    free(buf);
    if (n_added) *n_added = added;
    return used;
}

#ifdef HN_HAVE_QUICKJS
static JSContext *js_scope_find(const char *key);   /* 作用域表(定义在后) */
/* "js:" 前缀求值: 表达式在 rt 的 QuickJS 作用域**全局**求值 →
   JSON.stringify 字符串。返回 malloc 串(调用方 free);
   失败/异常/非序列化结果返回 NULL(htmx 的 this 绑定为 v1 边界)。 */
static char *js_eval_to_json(hn_rt *rt, const char *expr, const char *tag) {
    JSContext *ctx = js_scope_find(rt->id);
    if (!ctx) return NULL;
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue jobj = JS_GetPropertyStr(ctx, global, "JSON");
    JSValue sfn = JS_GetPropertyStr(ctx, jobj, "stringify");
    JSValue rv = JS_Eval(ctx, expr, strlen(expr), tag, JS_EVAL_TYPE_GLOBAL);
    char *out = NULL;
    if (JS_IsException(rv)) {
        JSValue e = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, e);
        fprintf(stderr, "[hx] %s 求值异常: %s\n", tag, msg ? msg : "?");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, e);
    } else {
        JSValue sfn_v = JS_DupValue(ctx, sfn);
        JSValue s = JS_Call(ctx, sfn_v, JS_UNDEFINED, 1, (JSValueConst *)&rv);
        if (!JS_IsException(s) && JS_IsString(s)) {
            size_t len = 0;
            const char *str = JS_ToCStringLen(ctx, &len, s);
            if (str) {
                out = (char *)malloc(len + 1);
                if (out) memcpy(out, str, len + 1);
            }
        }
        JS_FreeValue(ctx, s);
    }
    JS_FreeValue(ctx, sfn);
    JS_FreeValue(ctx, jobj);
    JS_FreeValue(ctx, global);
    return out;
}
#endif

/* k=v percent 编码追加(&k=v; 与 hx_vals_append 同编码口径)。
   返回追加后的 body 长度; 编码截断时不追加该对。 */
static size_t hx_pair_append(char *body, size_t cap, size_t used,
                             const char *name, const char *value) {
    if (!name || !*name) return used;
    char enc[1024];
    size_t el = 0;
    for (const char *q = name; *q && el + 3 < sizeof(enc); q++) {
        unsigned char ch = (unsigned char)*q;
        if (isalnum(ch) || strchr("-_.~", ch)) enc[el++] = (char)ch;
        else el += (size_t)snprintf(enc + el, 4, "%%%02X", ch);
    }
    enc[el] = 0;
    char enc_v[768];
    size_t ev = 0;
    for (const char *q = value; *q && ev + 3 < sizeof(enc_v); q++) {
        unsigned char ch = (unsigned char)*q;
        if (isalnum(ch) || strchr("-_.~", ch)) enc_v[ev++] = (char)ch;
        else ev += (size_t)snprintf(enc_v + ev, 4, "%%%02X", ch);
    }
    enc_v[ev] = 0;
    int w = snprintf(body + used, cap - used, "%s%s=%s",
                     used ? "&" : "", enc, enc_v);
    return (w > 0 && (size_t)w < cap - used) ? used + (size_t)w : used;
}

/* 最小 HTML 序列化器(hx-select 的换入面): 元素(属性转义)+文本(转义)
   + 子级递归; void 元素(br/hr/img/input/meta/link)不自闭。能力面与
   sys 路由响应的生成侧对齐(元素/属性/文本 —— 响应不做内联样式以外的
   复杂结构, 此处如实覆盖)。 */
static void html_serialize_escape(char *out, size_t cap, size_t *used,
                                  const char *s, size_t n, int attr_mode) {
    for (size_t i = 0; i < n && *used + 6 < cap; i++) {
        unsigned char ch = (unsigned char)s[i];
        if (ch == '&') { out[(*used)++] = '&'; out[(*used)++] = 'a'; out[(*used)++] = 'm'; out[(*used)++] = 'p'; out[(*used)++] = ';'; }
        else if (ch == '<' && !attr_mode) { memcpy(out + *used, "&lt;", 4); *used += 4; }
        else if (ch == '>' && !attr_mode) { memcpy(out + *used, "&gt;", 4); *used += 4; }
        else if (ch == '"' && attr_mode) { memcpy(out + *used, "&quot;", 6); *used += 6; }
        else out[(*used)++] = (char)ch;
    }
}

static void html_serialize_node(hn_node *n, char *out, size_t cap, size_t *used) {
    if (!n || *used + 8 >= cap) return;
    if (n->kind != HN_ELEM) {
        /* TEXT: 文本在 text/text_len(hn_node_value 是表单/值缓冲通道,
           对解析出的文本节点返回空 —— 首跑实测序列化空内容) */
        if (n->text && n->text_len)
            html_serialize_escape(out, cap, used, n->text, n->text_len, 0);
        return;
    }
    const char *tag = n->tag ? n->tag : "div";
    out[(*used)++] = '<';
    html_serialize_escape(out, cap, used, tag, strlen(tag), 1);
    for (int i = 0;; i++) {
        const char *an = NULL, *av = NULL;
        if (!hn_node_attr_at(n, i, &an, &av)) break;
        if (!an) continue;
        out[(*used)++] = ' ';
        html_serialize_escape(out, cap, used, an, strlen(an), 1);
        out[(*used)++] = '=';
        out[(*used)++] = '"';
        if (av) html_serialize_escape(out, cap, used, av, strlen(av), 1);
        out[(*used)++] = '"';
    }
    out[(*used)++] = '>';
    /* void 元素(br/hr/img/input/meta/link): 无闭合无子级 */
    if (!strcmp(tag, "br") || !strcmp(tag, "hr") || !strcmp(tag, "img") ||
        !strcmp(tag, "input") || !strcmp(tag, "meta") || !strcmp(tag, "link"))
        return;
    for (hn_node *ch = n->first; ch; ch = ch->next)
        html_serialize_node(ch, out, cap, used);
    out[(*used)++] = '<';
    out[(*used)++] = '/';
    html_serialize_escape(out, cap, used, tag, strlen(tag), 1);
    out[(*used)++] = '>';
}

/* hx-trigger 事件语义: 值变化后(change 触发器)是否执行 hx 请求。
 * - 载体(自身或祖先)有 hx-get/post/put/delete/patch 且 hx-trigger 含 "change" → 检查
 * - changed 修饰符: 请求体与上次相同 → 跳过(last 存于 rt->hx_sched)
 * - delay:N[ms|s]: 到期前不执行(重触发刷新到期 = 防抖)
 * 返回 1 = 已(延迟)执行调度, 调用方应重渲染。 */
static void hx_change_maybe(hn_rt *rt, hn_node *n);

/* 触发表(每载体一槽; delay/changed 状态) */
#define HN_HX_SCHED_MAX 16
struct {
    char id[64];
    double due;          /* poll_clock 域(0 = 无挂起) */
    char last[128];
    int has_last;
} hx_sched_tab[HN_HX_SCHED_MAX];

/* hx-swap 修饰符 swap:N[ms|s] 的延迟换入槽(响应已到, 换入延后):
   到期在 hn_rt_frame 的 poll 域冲刷(重找目标 → swap → 布局)。
   frag 副本有界(4 槽 × 2KB), 超长/槽满 → 立即换入(诚实降级)。 */
#define HN_HX_SWAP_PEND_MAX 4
#define HN_HX_SWAP_PEND_FRAG 2048
static struct {
    int active;
    double due;
    char target_id[64];
    hn_swap_mode m;
    char frag[HN_HX_SWAP_PEND_FRAG];
    size_t len;
} hx_swap_pend[HN_HX_SWAP_PEND_MAX];

static int hx_perform(hn_rt *rt, hn_node *carrier) {
    const char *verb = "GET", *url = NULL;
    hx_method_of(carrier, &verb, &url);
    if (!url) return 0;
    int is_get = (verb[0] == 'G');
    hn_doc *doc = hn_context_doc(rt->ctx);
    char form[4096];
    form[0] = 0;
    hn_doc_form_encode(doc, form, sizeof(form));
    /* hx-include: 逗号分隔 CSS 选择器 → 追加元素值(htmx 语义)。
       输入控件取当前值; 其余元素取 name/value 属性 —— 覆盖 form_walk
       只收集控件的缺口(非控件载体的显式包含在此补齐)。选择器复用
       hn_doc_query_selector(查全语义, 数组出参)。 */
    {
        const char *inc = hn_node_attr(carrier, "hx-include");
        if (inc && *inc && inc[0] != 'j' && inc[0] != 'J') {
            /* 逗号分隔选择器列表: 逐个查询累加(htmx 同语义) */
            char sel[256];
            int si = 0;
            size_t used = strlen(form);
            const char *p2 = inc;
            int done = 0;
            while (!done) {
                char ch = *p2;
                if (ch == ',' || ch == 0) {
                    sel[si] = 0;
                    if (si > 0) {
                        hn_node *matches[16];
                        int nm = hn_doc_query_selector(hn_context_doc(rt->ctx), sel,
                                                       NULL, matches, 16);
                        for (int i = 0; i < nm; i++) {
                            hn_node *m = matches[i];
                            const char *name = hn_node_attr(m, "name");
                            if (!name || !*name) continue;
                            const char *v = NULL;
                            size_t vlen = 0;
                            if (hn_node_is_input(m) && !hn_node_is_disabled(m))
                                v = hn_node_value(m, &vlen);
                            else
                                v = hn_node_attr(m, "value");
                            if (!v) continue;
                            used = hx_pair_append(form, sizeof(form), used, name, v);
                        }
                    }
                    si = 0;
                    if (ch == 0) done = 1;
                    p2++;
                    continue;
                }
                if (si + 1 < (int)sizeof(sel)) sel[si++] = ch;
                p2++;
            }
        }
    }
    /* hx-vals: JSON 对象合并进请求体(htmx 语义); "js:" 前缀 = QuickJS
       求值表达式 → JSON.stringify → 同一合并(局部作者 JS 面, 表达式在
       全局作用域求值 —— htmx 的 this 绑定为 v1 边界)。 */
    {
        const char *vals = hn_node_attr(carrier, "hx-vals");
        char *js_json = NULL;
        if (vals && *vals) {
            if (vals[0] == 'j' && vals[1] == 's' && vals[2] == ':') {
#ifdef HN_HAVE_QUICKJS
                js_json = js_eval_to_json(rt, vals + 3, "hx-vals.js");
#endif
                if (!js_json) {
                    fprintf(stderr, "[hx] hx-vals js: 求值失败(需 QuickJS 构建)\n");
                    fflush(stderr);
                }
            } else {
                hx_vals_append(form, sizeof(form), vals, strlen(vals), NULL, NULL);
            }
        }
        if (js_json) {
            hx_vals_append(form, sizeof(form), js_json, strlen(js_json), NULL, NULL);
            free(js_json);
        }
    }
    /* hx-confirm: htmx:confirm 事件(preventDefault 取消), 问题串走
       utf8 载荷。引擎无 window.confirm —— 对话框形态归平台层(与
       sys://notify 同级); 无 JS 监听 = 直接放行(与 beforeRequest 的
       无监听者语义一致)。 */
    {
        const char *confirm_q = hn_node_attr(carrier, "hx-confirm");
        if (confirm_q && *confirm_q &&
            rt_js_dispatch(rt, "htmx:confirm", carrier, 0, 0, confirm_q, 0, 0))
            return 0;
    }
    /* htmx:beforeRequest: pd → 取消(浏览器同序: 事件先于网络);
       payload = HTTP 动词(JS 可按方法分流) */
    if (rt_js_dispatch(rt, "htmx:beforeRequest", carrier, 0, 0, verb, 0, 0))
        return 0;
    /* sys:// remains the local routing boundary. HTTP uses the optional
       runtime transport; unsupported schemes retain hn_sys diagnostics. */
    char *frag = NULL;
    hn_http_response http = {0};
    int is_http = 0;
    if (url && (strncasecmp(url, "http://", 7) == 0 || strncasecmp(url, "https://", 8) == 0)) {
        is_http = 1;
        /* htmx 语义: GET 把表单拼进 query(?/& 接续), 其余动词带
           urlencoded 体(表单+hx-vals 合并) */
        const char *req_url = url;
        char url_q[8192];
        if (is_get && form[0]) {
            snprintf(url_q, sizeof(url_q), "%s%s%s", url,
                     strchr(url, '?') ? "&" : "?", form);
            req_url = url_q;
        }
        hn_http_request_opts opts;
        memset(&opts, 0, sizeof(opts));
        opts.url = req_url;
        opts.method = verb;
        opts.body = (!is_get && form[0]) ? form : NULL;
        opts.body_len = (!is_get && form[0]) ? strlen(form) : 0;
        hn_http_header hdr[9];
        int nhdr = 0;
        if (!is_get && form[0]) {
            hdr[nhdr].name = "Content-Type";
            hdr[nhdr].value = "application/x-www-form-urlencoded";
            nhdr++;
        }
        /* hx-headers: JSON 对象 → 自定义头(htmx 语义; 上限 8);
           "js:" 前缀同 hx-vals(QuickJS 求值 → JSON) */
        {
            const char *hj = hn_node_attr(carrier, "hx-headers");
            char *hjs_json = NULL;
            if (hj && *hj && hj[0] == 'j' && hj[1] == 's' && hj[2] == ':') {
#ifdef HN_HAVE_QUICKJS
                hjs_json = js_eval_to_json(rt, hj + 3, "hx-headers.js");
#endif
                hj = hjs_json;
            }
            if (hj && *hj) {
                char *hbuf = (char *)malloc(strlen(hj) + 1);
                if (hbuf) {
                    memcpy(hbuf, hj, strlen(hj) + 1);
                    void *arena = NULL;
                    hn_json *root = hn_json_parse(hbuf, strlen(hbuf), &arena);
                    if (root && root->kind == HN_JSON_OBJ)
                        for (int i = 0; i < root->count && nhdr < 8; i++) {
                            hn_json *it = root->items[i];
                            if (it && it->key && it->kind == HN_JSON_STR) {
                                hdr[nhdr].name = it->key;
                                hdr[nhdr].value = it->str ? it->str : "";
                                nhdr++;
                            }
                        }
                    hn_json_free(arena);
                    free(hbuf);
                }
            }
            free(hjs_json);
        }
        opts.headers = nhdr ? hdr : NULL;
        opts.header_count = nhdr;
        if (hn_http_request(&opts, &http)) {
            if (http.status >= 400) {
                fprintf(stderr, "[hx] http 失败: %s: HTTP %ld\n", url, http.status);
                fflush(stderr);
                hn_http_response_free(&http);
                return 0;
            }
            frag = (char *)malloc(http.body_len + 1);
            if (frag) { memcpy(frag, http.body, http.body_len); frag[http.body_len] = 0; }
            hn_http_response_free(&http);
        } else {
            fprintf(stderr, "[hx] http 失败: %s: %s\n", url, hn_http_last_error());
            fflush(stderr);
            return 0;
        }
    } else {
        hn_sys_host host = { (void *)rt->id, rt_sys_store_get, rt_sys_store_set };
        frag = hn_sys_request(&host, url, form);
    }
    if (!frag) {
        if (is_http) {
            fprintf(stderr, "[hx] http 失败: %s: 响应体分配失败\n", url);
            fflush(stderr);
        }
        return 0;
    }
    /* hx-select: 响应子树选择(htmx 语义: 换入**所有**匹配选择器的元素;
       无匹配 = 换入空)。frag 解析为临时文档, 逐匹配序列化 —— 序列化器
       做文本/属性转义(响应片段为 HTML, 重解析后转义态一致)。 */
    {
        const char *sel = hn_node_attr(carrier, "hx-select");
        if (sel && *sel && frag) {
            hn_doc *fd = hn_parse_html(frag, strlen(frag));
            if (fd) {
                hn_node *matches[32];
                int nm = hn_doc_query_selector(fd, sel, NULL, matches, 32);
                char picked[8192];
                size_t used = 0;
                for (int i = 0; i < nm; i++)
                    html_serialize_node(matches[i], picked, sizeof(picked), &used);
                picked[used] = 0;
                free(frag);
                frag = (char *)malloc(used + 1);
                if (frag) memcpy(frag, picked, used + 1);
                else { picked[0] = 0; frag = (char *)malloc(1); if (frag) frag[0] = 0; }
                hn_doc_free(fd);
            }
            /* 解析失败: 保留原 frag(诚实降级 —— select 是优化不是门禁) */
        }
    }
    hn_node *target = hx_resolve_target(rt, carrier);
    if (!target) { free(frag); return 0; }

    hn_swap_mode m = hx_swap_parse(hn_node_attr(carrier, "hx-swap"));
    float swap_delay = hx_swap_delay_parse(hn_node_attr(carrier, "hx-swap"));
    /* swap:N 延迟换入: 目标有 id → frag 副本入延迟槽(poll 域到期执行);
       无 id/槽满/超长 → 立即换入(诚实降级)。htmx 事件序: afterRequest
       响应到达即派发, afterSwap 随换入(延迟时与 swap 一起到 poll 域)。 */
    const char *tid = hn_node_attr(target, "id");
    int delayed = 0;
    if (swap_delay > 0 && tid && *tid && strlen(frag) < HN_HX_SWAP_PEND_FRAG) {
        for (int i = 0; i < HN_HX_SWAP_PEND_MAX && !delayed; i++) {
            if (hx_swap_pend[i].active) continue;
            hx_swap_pend[i].active = 1;
            hx_swap_pend[i].due = rt->poll_clock_ms + (double)swap_delay;
            snprintf(hx_swap_pend[i].target_id, sizeof(hx_swap_pend[i].target_id),
                     "%s", tid);
            hx_swap_pend[i].m = m;
            memcpy(hx_swap_pend[i].frag, frag, strlen(frag) + 1);
            hx_swap_pend[i].len = strlen(frag);
            delayed = 1;
        }
    }
    if (!delayed) {
        int ok = hn_doc_swap_node(doc, target, m, frag, strlen(frag));
        if (ok) hn_context_layout(rt->ctx, (float)rt->w, (float)rt->h, &g_tb);
    }
    /* hx-push-url: swap 成功后把请求 URL 记入导航栈(htmx 语义: back
       可返回)。映射口径: 项目内文件 URL → back 重渲该文件(与项目导航
       同一语义); sys:///http(s) URL 无文件形态 → no-op(如实声明)。
       "true" = 请求 URL; 其余非 false 值 = 字面 URL; "false" = 关闭。 */
    {
        const char *pu = hn_node_attr(carrier, "hx-push-url");
        if (pu && *pu && strcmp(pu, "false") != 0) {
            const char *push_url = (strcmp(pu, "true") == 0) ? url : pu;
            char *html = NULL; size_t len = 0;
            if (page_read(push_url, &html, &len)) {
                free(html);
                if (!rt->page_path) rt->page_path = strdup(push_url);
                if (rt->back_n == rt->back_cap) {
                    int nc = rt->back_cap ? rt->back_cap * 2 : 8;
                    char **nb = (char **)realloc(rt->back, sizeof(char *) * (size_t)nc);
                    if (nb) { rt->back = nb; rt->back_cap = nc; }
                }
                if (rt->back_n < 64) {
                    rt->back[rt->back_n++] = strdup(push_url);
                    rt_render_trace("hx-push-url");
                }
            }
        }
    }

    free(frag);
    rt_js_dispatch(rt, "htmx:afterRequest", carrier, 0, 0, NULL, 0, 1);
    if (!delayed && m != HN_SWAP_NONE)
        rt_js_dispatch(rt, "htmx:afterSwap", target, 0, 0, NULL, 0, 0);
    return 1;
}

/* 值变化后的 change 触发器(hx-trigger="change [changed] [delay:Nms]")。
 * 无 id 载体无法建状态槽 → 直接执行(无 changed/延迟语义)。 */
static void hx_change_maybe(hn_rt *rt, hn_node *n) {
    if (!rt || !n) return;
    hn_node *carrier = hx_carrier(n);
    if (!carrier) return;
    const char *trig = hn_node_attr(carrier, "hx-trigger");
    if (!trig || !hx_trigger_has(trig, "change")) return;

    /* 当前请求体(编辑后的表单 + vals) — changed 比较/延迟挂起都用它 */
    char body[512];
    body[0] = 0;
    hn_doc_form_encode(hn_context_doc(rt->ctx), body, sizeof(body));
    const char *vals = hn_node_attr(carrier, "hx-vals");
    if (vals && *vals && vals[0] != 'j' && vals[0] != 'J')
        hx_vals_append(body, sizeof(body), vals, strlen(vals), NULL, NULL);
    size_t bl = strlen(body);

    const char *id = hn_node_attr(carrier, "id");
    int slot = -1;
    if (id && *id) {
        for (int i = 0; i < HN_HX_SCHED_MAX; i++)
            if (hx_sched_tab[i].id[0] && !strcmp(hx_sched_tab[i].id, id)) { slot = i; break; }
        if (slot < 0)
            for (int i = 0; i < HN_HX_SCHED_MAX; i++)
                if (!hx_sched_tab[i].id[0]) { slot = i; break; }
        if (slot >= 0) {
            snprintf(hx_sched_tab[slot].id, sizeof(hx_sched_tab[slot].id), "%s", id);
            hx_sched_tab[slot].id[sizeof(hx_sched_tab[slot].id) - 1] = 0;
        }
    }

    int want_changed = hx_trigger_changed(trig, "change");
    if (want_changed && slot >= 0 && hx_sched_tab[slot].has_last &&
        bl == strlen(hx_sched_tab[slot].last) &&
        memcmp(hx_sched_tab[slot].last, body, bl) == 0)
        return;   /* 值未变: changed 修饰符跳过 */

    int delay = hx_trigger_delay(trig, "change");
    if (delay > 0 && slot >= 0) {
        hx_sched_tab[slot].due = rt->poll_clock_ms + (double)delay;
        if (bl < sizeof(hx_sched_tab[slot].last)) {
            memcpy(hx_sched_tab[slot].last, body, bl);
            hx_sched_tab[slot].last[bl] = 0;
            hx_sched_tab[slot].has_last = 1;
        }
        return;   /* 到期在 hn_rt_frame 统一冲刷 */
    }
    if (slot >= 0 && bl < (int)sizeof(hx_sched_tab[slot].last)) {
        memcpy(hx_sched_tab[slot].last, body, bl + 1);
        hx_sched_tab[slot].has_last = 1;
    }
    hx_perform(rt, carrier);
}

/* 启动动作: hx-trigger 含 "load" 的元素立即执行一次 */
static void run_load_actions(hn_rt *rt) {
    hn_doc *doc = hn_context_doc(rt->ctx);
    for (int i = 0;; i++) {
        const char *id = NULL;
        if (!hn_doc_load_at(doc, i, &id)) break;
        hn_node *n = hn_doc_find_by_id(doc, id);
        if (n) hx_perform(rt, n);
    }
    rt->poll_built = 0;   /* 内容变了, 轮询表重建 */
}

/* ---------------- 渲染 ---------------- */

/* 当前渲染缩放与位图物理尺寸(逻辑 × scale; 缺省/非法回落 1.0) */
static float rt_scale(const hn_rt *rt) {
    return (rt && rt->scale >= 0.5f) ? rt->scale : 1.0f;
}
static int rt_bitmap_w(const hn_rt *rt) {
    if (!rt) return 0;
    int v = (int)((float)rt->w * rt_scale(rt) + 0.5f);
    return v > 0 ? v : 1;
}
static int rt_bitmap_h(const hn_rt *rt) {
    if (!rt) return 0;
    int v = (int)((float)rt->h * rt_scale(rt) + 0.5f);
    return v > 0 ? v : 1;
}

/* HN_RENDER_TRACE=1: 渲染口径变化时打一行(与 [media] trace 同风格,
   验收脚本观测点) —— [render] scale=2 bmp=720x720。首帧打一次, 之后只在
   scale 变化(hn_rt_set_scale)时再打 —— 真窗壳"开窗 1x → 抬到 backing"
   的两步各留一行, 探针 grep 末态。未设环境钮时零输出(读一次 getenv 即封口)。 */
static void render_trace(int bw, int bh, float scale) {
    static int inited = 0;
    static float last_scale = -1.0f;
    if (inited && scale == last_scale) return;
    inited = 1;
    last_scale = scale;
    const char *e = getenv("HN_RENDER_TRACE");
    if (!e || !*e || !strcmp(e, "0")) return;
    printf("[render] scale=%g bmp=%dx%d\n", (double)scale, bw, bh);
    fflush(stdout);
}

/* ---------------- GPU 渲染后端(弱桩 + 渲染分支) ----------------
 * hn_gpu.c(hnapp 目标入列)提供强定义覆盖这里的弱桩 —— 与 hnp_macos.c 的
 * hnp_media_player_for_src 弱桩同一手法("弱定义+强定义覆盖是 Mach-O/ELF
 * 通吃的口径")。不链 hn_gpu.c 的构建(probe 现场编译等)落空到桩:
 * 恒"不可用" → 一切走 hnsoft, 行为零漂移。 */
#if defined(__GNUC__)
__attribute__((weak))
#endif
void hn_gpu_set_host(const void *host) { (void)host; }
#if defined(__GNUC__)
__attribute__((weak))
#endif
int hn_gpu_available(const hn_display_list *dl) { (void)dl; return 0; }
#if defined(__GNUC__)
__attribute__((weak))
#endif
int hn_gpu_render_scaled(const hn_display_list *dl, int w, int h, hn_color bg,
                         float scale, unsigned char **out_bgra) {
    (void)dl; (void)w; (void)h; (void)bg; (void)scale; (void)out_bgra;
    return 0;
}
#if defined(__GNUC__)
__attribute__((weak))
#endif
int hn_gpu_render_present(void *ca_metal_layer, const hn_display_list *dl,
                          int w, int h, hn_color bg, float scale) {
    (void)ca_metal_layer; (void)dl; (void)w; (void)h; (void)bg; (void)scale;
    return 0;
}
#if defined(__GNUC__)
__attribute__((weak))
#endif
unsigned long hn_gpu_present_count(void) { return 0; }
#if defined(__GNUC__)
__attribute__((weak))
#endif
unsigned long hn_gpu_readback_count(void) { return 0; }

void hn_rt_set_gpu_host(const void *host) { hn_gpu_set_host(host); }

void hn_rt_set_gpu_present_layer(hn_rt *rt, void *ca_metal_layer) {
    if (!rt) return;
    rt->gpu_present_layer = ca_metal_layer;
    if (!ca_metal_layer) rt->presented = 0;
}

int hn_rt_presented(const hn_rt *rt) { return rt ? rt->presented : 0; }

/* 光栅段(全路径/ambient 轻路径共用): 显示列表 → RGBA → 预乘 BGRA。
   HiDPI: 位图按 (逻辑 × scale) 出图, 显示列表坐标在 paint 时乘 scale
   (hnsoft_render_scaled), 文本随字号 ×scale 走 FreeType —— 真 2x 光栅。
   GPU 分支(契约口径): 宿主注入(壳在 HN_GPU=metal 时调 hn_rt_set_gpu_host)
   且本帧指令全支持(hn_gpu_available 逐指令扫) → hn_gpu_render_scaled 直接
   产出**预乘 BGRA**(与下方 CPU 路径 "RGBA 非预乘 → 预乘 BGRA" 的换算产物
   同口径, blit 语义一致); 表缺席/init 失败/指令不支持/任一步失败 → 静默
   落回 hnsoft —— 回退是**逐帧**的(同一会话里 GPU 帧与 CPU 帧可自由穿插)。 */
/* HN_ALLOC_TRACE=1: 分配 churn 实证(每 300 帧 ~2MB 级全页缓冲的堆分配
   差分)。计数口径 = hnsoft 热路径(fb 位图/多边形/字形)+ 本文件 bgra
   缓冲的分配/扩容; 治理前稳态 ≈2.0/帧(fb 一次 + bgra 一次), 治理后
   ≈0/帧(只剩暖机扩容)—— perf_probe 断言"差分有界"。getenv 只读一次。 */
static long g_rt_allocs = 0;

static void alloc_trace_tick(void) {
    static int frames = 0, on = -1;
    static long last = 0;
    if (on < 0) on = getenv("HN_ALLOC_TRACE") ? 1 : 0;
    if (!on) return;
    frames++;
    if (frames % 300) return;
    long cur = hnsoft_heap_allocs() + g_rt_allocs;
    fprintf(stderr, "[alloc-trace] frames=%d new=%ld (%.3f/帧)\n",
            frames, cur - last, (double)(cur - last) / 300.0);
    fflush(stderr);
    last = cur;
}

static int rt_raster(hn_rt *rt) {
    const hn_display_list *dl = hn_context_display_list(rt->ctx);
    if (!dl) return 0;
    float scale = rt_scale(rt);
    int bw = rt_bitmap_w(rt), bh = rt_bitmap_h(rt);
    hn_color bg = rt->transparent ? 0x00000000u : 0x0B0E13FFu;
    /* html/body 背景向画布传播(浏览器标准): 根链声明了实色背景时清屏随它。
       透明窗不传播 —— 逐像素 alpha 契约优先。 */
    if (!rt->transparent) {
        hn_color rb = hn_doc_canvas_background(hn_context_doc(rt->ctx));
        if (rb) bg = rb;
    }
    rt->paint_bg = bg;   /* 画纸底色(画刷 reveal 的未铺区域混合目标)记账 */
    /* 直呈分流: 平台层注入 CAMetalLayer 且本帧行可 GPU → 直接 present
       (成功 = 0 次 readback / 0 次 CPU 位图, rt->presented 置位供
       shot/blit 分流); 失败落回既有 readback/CPU 路径。 */
    if (rt->gpu_present_layer) {
        if (hn_gpu_render_present(rt->gpu_present_layer, dl, bw, bh, bg, scale)) {
            rt->presented = 1;
            return 1;
        }
    }
    rt->presented = 0;
    unsigned char *bgra = NULL;
    if (hn_gpu_available(dl))
        hn_gpu_render_scaled(dl, bw, bh, bg, scale, &bgra);
    if (!bgra && hn_context_native_suppressed(rt->ctx) == 0) {
        /* 原生 BITMAP(IOSurface 直入)在 CPU 路径无字节 —— GPU 中途失败
           回退 hnsoft 前先压制原生路由并重出显示列表(重走 paint, neb3
           换回读回字节), hnsoft 拿到完整帧; 随后 GPU 重试一次(压制态
           仍可 GPU, 只是该帧不带原生框)。双失败才落 hnsoft(此时字节
           完整, 无洞)。 */
        hn_context_native_suppress(rt->ctx);
        hn_context_repaint(rt->ctx);
        dl = hn_context_display_list(rt->ctx);
        if (hn_gpu_available(dl))
            hn_gpu_render_scaled(dl, bw, bh, bg, scale, &bgra);
    }
    if (bgra) {
        hn_context_native_resume(rt->ctx);   /* GPU 恢复: 下一帧重新启用原生路由 */
        /* GPU 产出直接接管(旧口径; GPU 是显式选配路径, 不在本 churn 治理
           面 —— CPU 稳态不走此支)。 */
        free(rt->bgra);
        rt->bgra = bgra;
        rt->bgra_cap = (size_t)bw * (size_t)bh * 4;
    } else {
        /* CPU 路径(稳态): RGBA 走**复用缓冲**(hnsoft_render_reuse_scaled,
           slot 由 rt 持有、只增不减), BGRA 走 rt 的持久缓冲(同样只增不减)
           —— 治理前这里每帧 malloc+free 两张全页位图(920x520 下 ~2MB ×
           帧率的分配器 churn), 现在只有窗口首次出图/放大时扩容一次。 */
        unsigned char *rgba = hnsoft_render_reuse_scaled(dl, bw, bh, bg, scale,
                                                         &rt->soft_buf, &rt->soft_cap);
        if (!rgba) return 0;
        size_t need = (size_t)bw * (size_t)bh * 4;
        if (!rt->bgra || need > rt->bgra_cap) {
            unsigned char *nb = (unsigned char *)realloc(rt->bgra, need);
            if (!nb) return 0;   /* 旧 bgra 原地保留(与旧 OOM 口径一致: 上一帧像素仍可 blit) */
            rt->bgra = nb;
            rt->bgra_cap = need;
            g_rt_allocs++;
        }
        int n = bw * bh;
        /* RGBA 非预乘 → BGRA 预乘(三平台 blit 的统一约定) */
        for (int i = 0; i < n; i++) {
            unsigned char r = rgba[i * 4], g = rgba[i * 4 + 1], b = rgba[i * 4 + 2], a = rgba[i * 4 + 3];
            if (a != 255) { r = (unsigned char)((r * a) / 255); g = (unsigned char)((g * a) / 255); b = (unsigned char)((b * a) / 255); }
            rt->bgra[i * 4] = b; rt->bgra[i * 4 + 1] = g; rt->bgra[i * 4 + 2] = r; rt->bgra[i * 4 + 3] = a;
        }
    }
    alloc_trace_tick();
    render_trace(bw, bh, scale);
    rt->raster_gen++;
    return 1;
}

/* ---------------- 桌面画板呈现(画刷铺色) ---------------- */

/* 铺色时长(ms): HN_REVEAL_MS 环境钮可覆盖(验收连拍/演示慢放, 与
   HN_ANIM_FPS 同款观测口径); 缺省 480ms。 */
static float paint_reveal_ms(void) {
    static float ms = -1;
    if (ms < 0) {
        ms = 480.0f;
        const char *e = getenv("HN_REVEAL_MS");
        if (e && *e) {
            float v = (float)atof(e);
            if (v >= 50) ms = v;
        }
    }
    return ms;
}

/* 画刷覆盖: 内容沿对角轴(左上 → 右下)以软笔锋铺开。
   - 头部位置 ease-out(起笔快、收笔稳);
   - 笔锋带宽 = 20% 对角跨(smoothstep 软边, 不是硬擦除线);
   - 边缘叠加沿"垂直轴坐标"的慢波纹 —— 拉刷的手腕摆动, 边缘不是直线。 */
float hn_rt_paint_mask_cov(float u, float fx, float fy, float w, float h) {
    return hnsoft_paint_mask_cov(u, fx, fy, w, h);   /* 数学在引擎单源(hnwin 同用) */
}

void hn_rt_paint_reveal_begin(hn_rt *rt, unsigned long now_ms) {
    if (!rt || rt->paint_t0 || rt->paint_done) return;
    if (rt->reveal_mode != HN_REVEAL_PAINT || !rt->paint_eligible) return;
    rt->paint_t0 = now_ms ? now_ms : 1;   /* 0 保留为"未开始"哨兵 */
}

int hn_rt_paint_reveal_active(const hn_rt *rt, unsigned long now_ms) {
    if (!rt || !rt->paint_t0 || rt->paint_done || !rt->paint_eligible) return 0;
    return (float)(now_ms - rt->paint_t0) < paint_reveal_ms();
}

/* 把当前进度的遮罩盖到 BGRA 帧(预乘)。只由真窗主循环在**新光栅**之后
   调用 —— 帧缓冲每帧整体重出, 不存在遮罩叠加污染。GPU 直呈帧(presented)
   没有 CPU 位图, 自然跳过(该窗口的呈现退化为平台淡入, 如实口径)。 */
void hn_rt_paint_reveal_apply(hn_rt *rt, unsigned long now_ms) {
    if (!rt || !rt->bgra || rt->presented) return;
    if (rt->reveal_mode != HN_REVEAL_PAINT || !rt->paint_eligible) return;
    float u = (float)(double)(now_ms - rt->paint_t0) / paint_reveal_ms();
    if (u <= 0) return;
    if (u >= 1) {
        rt->paint_done = 1;
        rt->paint_applied_gen = rt->raster_gen;   /* 收口帧=干净帧, 记账防叠加 */
        return;
    }
    /* 干净帧守卫: 本拍尚未重光栅(rt_raster 未跑过) → 内部重出一次。
       遮罩是乘法, 只允许作用于当帧新像素; 壳若只强制 blit 不重光栅,
       没有这一步就会原地叠加(见 raster_gen 注)。
       重出走 **ambient 轻路径**(repaint_light + raster, 免 relayout ——
       铺色期间布局不变, 全量 rt_render 的 relayout 是 4 倍浪费; 且
       rt_raster 不查 paint_defer, 守卫在挂起窗口也成立, 没有 defer 洞)。 */
    if (rt->paint_applied_gen == rt->raster_gen) {
        hn_context_anim_tick(rt->ctx, 0);
        hn_context_repaint_light(rt->ctx);
        if (!rt_raster(rt)) return;   /* 重出失败(OOM): 不盖旧帧, 防叠加 */
    }
    rt->paint_applied_gen = rt->raster_gen;
    /* HN_REVEAL_TRACE=1: 铺色推进取证(与平台 reveal trace 同一环境钮) */
    {
        static int tr = -1;
        if (tr < 0) tr = getenv("HN_REVEAL_TRACE") ? 1 : 0;
        if (tr) { fprintf(stderr, "[paint-reveal] apply u=%.2f bmp=%dx%d t=%d\n",
                          (double)u, rt_bitmap_w(rt), rt_bitmap_h(rt), (int)rt->transparent);
                  fflush(stderr); }
    }
    int bw = rt_bitmap_w(rt), bh = rt_bitmap_h(rt);
    unsigned char *px = rt->bgra;
    if (rt->transparent) {
        /* 透明窗: 预乘 BGRA 四通道同乘覆盖 —— 窗口真被逐像素画上桌面 */
        for (int y = 0; y < bh; y++) {
            float fy = (float)y + 0.5f;
            for (int x = 0; x < bw; x++) {
                float m = hn_rt_paint_mask_cov(u, (float)x + 0.5f, fy,
                                               (float)bw, (float)bh);
                if (m >= 1) continue;
                unsigned char *p = px + ((size_t)y * bw + x) * 4;
                if (m <= 0) { p[0] = p[1] = p[2] = p[3] = 0; continue; }
                p[0] = (unsigned char)(p[0] * m);
                p[1] = (unsigned char)(p[1] * m);
                p[2] = (unsigned char)(p[2] * m);
                p[3] = (unsigned char)(p[3] * m);
            }
        }
        return;
    }
    /* 不透明窗: 未铺区域显示"画纸"(画布底色), 内容像颜料盖上来 */
    hn_color bg = rt->paint_bg ? rt->paint_bg : 0x0B0E13FFu;
    unsigned char pr = (unsigned char)(bg >> 24), pg = (unsigned char)(bg >> 16),
                  pb = (unsigned char)(bg >> 8);
    for (int y = 0; y < bh; y++) {
        float fy = (float)y + 0.5f;
        for (int x = 0; x < bw; x++) {
            float m = hn_rt_paint_mask_cov(u, (float)x + 0.5f, fy,
                                           (float)bw, (float)bh);
            if (m >= 1) continue;
            unsigned char *p = px + ((size_t)y * bw + x) * 4;
            if (m <= 0) { p[0] = pb; p[1] = pg; p[2] = pr; continue; }
            p[0] = (unsigned char)((float)pb + ((float)p[0] - pb) * m);
            p[1] = (unsigned char)((float)pg + ((float)p[1] - pg) * m);
            p[2] = (unsigned char)((float)pr + ((float)p[2] - pr) * m);
        }
    }
}

/* HN_RT_RENDER_TRACE=1: 每次真渲染落一行 stderr(帧拍取证口径 —— 对比
   "唤醒次数 vs 真渲染次数"、同帧多次渲染治理前后计数; 与 HN_CUBISM_TRACE/
   HN_IMG_CACHE_STATS 同一约定, 缺省静默零成本)。 */
static int g_rt_trace_on = -1;
static long g_rt_render_n = 0;
static void rt_render_trace(const char *kind) {
    if (g_rt_trace_on < 0) g_rt_trace_on = getenv("HN_RT_RENDER_TRACE") ? 1 : 0;
    if (!g_rt_trace_on) return;
    g_rt_render_n++;
    fprintf(stderr, "[rt-render] #%ld %s\n", g_rt_render_n, kind);
}

static int rt_render(hn_rt *rt) {
    /* 挂起窗口(app 事件排干中): 只标脏不渲染, 队尾 paint_flush 一次画。
       返回 0 —— 调用方(app)以 flush 的返回值决定是否 blit。 */
    if (rt->paint_defer) { rt->paint_dirty = 1; return 0; }
    /* nopaint 布局: layout 内建的 paint 会被下方 anim_tick(0) 重采样后的
       repaint 整体覆盖 —— 交互帧的显示列表构建从两遍收敛为一遍(性能
       治理; 语义不变, 见 hn_context_layout_nopaint 注)。 */
    hn_context_layout_nopaint(rt->ctx, (float)rt->w, (float)rt->h, &g_tb);
    /* relayout 会把动画采样值从级联重算冲掉(每帧 tick 后紧跟 render,
       kf_clock 不变)—— 零步进重采样一次再建显示列表, 过渡/关键帧的
       中间态才真正落到像素(与 tools/hncore.c render 路径同一修法)。 */
    hn_context_anim_tick(rt->ctx, 0);
    hn_context_repaint(rt->ctx);
    int r = rt_raster(rt);
    rt_render_trace("full");
    return r;
}

/* ambient 轻路径: 本帧只有 ext_clock 类动画前进(anim_tick_ex 报
   AMBIENT)—— 样式/几何字段无变化, 免整遍 relayout(hn_context_layout),
   只重建显示列表 + 光栅。静态立绘桌宠 + GIF/网格摆动的 CPU 大头就在
   relayout: 30fps 节拍 × 轻路径, 渲染成本降到全路径的约 1/4。 */
static int rt_render_ambient(hn_rt *rt) {
    hn_context_repaint_light(rt->ctx);
    int r = rt_raster(rt);
    rt_render_trace("ambient");
    return r;
}

/* ---------------- 公开 API ---------------- */

/* doc → ctx 注册表: hn_rt_eval 的既有签名只拿得到 doc(桥内寻址元素),
   而媒体桥需要 context(宿主 + 会话表)。hn_rt 创建/换文档时登记,
   eval 时反查。表满/未登记(如 daemon 自建 context)时媒体桥静默 no-op
   —— 与"宿主未注入"同一降级口径(契约 §6)。 */
static struct { hn_doc *doc; hn_context *ctx; } g_doc_ctx[64];
static int g_doc_ctx_n = 0;

static void doc_ctx_bind(hn_doc *doc, hn_context *ctx) {
    if (!doc || !ctx) return;
    for (int i = 0; i < g_doc_ctx_n; i++)
        if (g_doc_ctx[i].doc == doc) { g_doc_ctx[i].ctx = ctx; return; }
    if (g_doc_ctx_n < 64) {
        g_doc_ctx[g_doc_ctx_n].doc = doc;
        g_doc_ctx[g_doc_ctx_n].ctx = ctx;
        g_doc_ctx_n++;
    }
}

static void doc_ctx_unbind_ctx(hn_context *ctx) {
    for (int i = 0; i < g_doc_ctx_n; i++) {
        if (g_doc_ctx[i].ctx == ctx) {
            g_doc_ctx[i] = g_doc_ctx[g_doc_ctx_n - 1];
            g_doc_ctx_n--;
            i--;
        }
    }
}

static void doc_ctx_unbind_doc(hn_doc *doc) {
    for (int i = 0; i < g_doc_ctx_n; i++) {
        if (g_doc_ctx[i].doc == doc) {
            g_doc_ctx[i] = g_doc_ctx[g_doc_ctx_n - 1];
            g_doc_ctx_n--;
            i--;
        }
    }
}

static hn_context *doc_ctx_find(hn_doc *doc) {
    if (!doc) return NULL;
    for (int i = 0; i < g_doc_ctx_n; i++)
        if (g_doc_ctx[i].doc == doc) return g_doc_ctx[i].ctx;
    return NULL;
}

hn_rt *hn_rt_open(const hn_rt_desc *d) {
    if (!d || !d->html) return NULL;
    /* canvas getImageData 的栅格化缝: rt 链了 hnsoft, 在此把渲染后端注入
       引擎核心(核心专属构建不链 rt/hnsoft → 不注入, 像素出口如实不可用)。
       幂等: 多次 open 覆盖同一值。 */
    hn_paint_set_ledger_raster(hnsoft_render);
    hn_rt *rt = (hn_rt *)calloc(1, sizeof(*rt));
    if (!rt) return NULL;
    snprintf(rt->id, sizeof(rt->id), "%s", d->id ? d->id : "default");
    rt->w = d->w > 0 ? d->w : 480;
    rt->h = d->h > 0 ? d->h : 700;
    rt->scale = 1.0f;   /* HiDPI: 真窗壳开窗后经 hn_rt_set_scale 抬到 backing */
    rt->scroll_armed = 1;   /* scroll 事件节流: 首帧前就允许一次 */

    hn_doc *doc = hn_parse_html(d->html, d->html_len);
    if (!doc) { free(rt); return NULL; }
    hn_doc_autoid_hx(doc);
    rt->ctx = hn_context_create();
    hn_context_set_doc(rt->ctx, doc);
    hn_context_set_event_sink(rt->ctx, rt_event_sink, rt);
    hn_context_set_assets(rt->ctx, &g_assets_be);
    if (g_media_be) hn_context_set_media(rt->ctx, g_media_be);

    hn_manifest m;
    memset(&m, 0, sizeof(m));
    hn_doc_manifest(doc, &m);
    if (m.w > 0) rt->w = m.w;
    if (m.h > 0) rt->h = m.h;
    rt->transparent = (m.transparent == 1);
    /* 画刷 eligible = **逐像素透明窗**(用户裁决第二轮: "直接在桌面上
       绘制这个窗口" —— 未铺区域必须透出桌面, 只有逐像素 alpha 做得到;
       不透明窗(含无边框不透明)会出现"先一块画板再画内容"的假象, 缺省
       只走平台淡入)。 */
    rt->paint_eligible = rt->transparent;
    /* 呈现动画风格: 文档声明为底, HN_REVEAL 环境钮覆盖(探针/演示不改文档
       翻风格, 与 HN_CLICK_THROUGH 同款纪律)。未知值 = 声明原样(缺省 paint)。 */
    rt->reveal_mode = (int)m.reveal;
    {
        const char *rv = getenv("HN_REVEAL");
        if (rv && *rv) {
            if (!strcmp(rv, "fade")) rt->reveal_mode = HN_REVEAL_FADE;
            else if (!strcmp(rv, "none")) rt->reveal_mode = HN_REVEAL_NONE;
            else rt->reveal_mode = HN_REVEAL_PAINT;
        }
    }

    if (d->path && *d->path) rt->page_path = strdup(d->path);

    if (d->css && d->css_len) hn_context_add_sheet(rt->ctx, hn_parse_css(d->css, d->css_len));
    doc_ctx_bind(doc, rt->ctx);
    {
        /* 文档里的 <link rel=stylesheet> 内联(相对 cwd) */
        size_t cl = 0;
        char *css = load_css_links(d->html, &cl);
        if (css && cl) hn_context_add_sheet(rt->ctx, hn_parse_css(css, cl));
        free(css);
    }
    if (!rt_render(rt)) { hn_rt_close(rt); return NULL; }
    /* 定时器/RAF 的宿主绑定先于文档脚本: 装载期 setTimeout 就要把回调
       记到本 rt 的表上(桥函数只有 JSContext*, 经 opaque 里的 rt 反查) */
    js_scope_bind_rt(rt);
    run_doc_scripts(rt);   /* 文档内联 <script>: canvas 2D 等纯 JS 页面的驱动 */
    run_load_actions(rt);
    rt_render(rt);
    /* Smart-UX: 首帧完成后, 此后运行时插入的内容默认"淡入+上浮"浮现 */
    hn_context_set_dynamic_enter(rt->ctx, 1);
    return rt;
}

/* ---------------- JS 运行时(QuickJS, lazy init) ----------------
 * QuickJS 是构建期可选项: 构建脚本探测到本机安装才定义 HN_HAVE_QUICKJS。
 * 交叉目标(如 musl)没有对应架构的 quickjs 库可链, 不定义 —— 整段编译
 * 出局, hn_rt_eval 按既有"Error: ..."口径如实报告未启用(不静默)。 */
#ifdef HN_HAVE_QUICKJS

/* 桥函数经 opaque 取用的环境(契约 §6: 从 hn_doc* 扩为 {doc, ctx})。
   实例挂在 js_scopes 槽位上(生命周期同 JS 上下文), opaque 存其指针。
   rt = 定时器/RAF 的宿主(hn_rt_open 时绑定; setTimeout 经它落到 rt 的表)。 */
typedef struct { hn_doc *doc; hn_context *ctx; hn_rt *rt; } js_bridge;

static void js_rt_free(hn_rt *rt) {
    if (rt->js_ctx) { JS_FreeContext((JSContext *)rt->js_ctx); rt->js_ctx = NULL; }
    if (rt->js_rt) { JS_RunGC((JSRuntime *)rt->js_rt); JS_FreeRuntime((JSRuntime *)rt->js_rt); rt->js_rt = NULL; }
}

/* hn_set_text(id, text): JS 桥 → 引擎文本更新(与 hn_doc_set_text 同口径) */
static JSValue js_hn_set_text(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_UNDEFINED;
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    const char *id = JS_ToCString(ctx, argv[0]);
    const char *text = JS_ToCString(ctx, argv[1]);
    if (b && b->doc && id && text) hn_doc_set_text(b->doc, id, text);
    if (id) JS_FreeCString(ctx, id);
    if (text) JS_FreeCString(ctx, text);
    return JS_UNDEFINED;
}

/* hn_set_value(id, value): input 的 value 更新 */
static JSValue js_hn_set_value(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_UNDEFINED;
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    const char *id = JS_ToCString(ctx, argv[0]);
    const char *val = JS_ToCString(ctx, argv[1]);
    if (b && b->doc && id && val) {
        hn_node *n = hn_doc_find_by_id(b->doc, id);
        if (n) hn_node_set_value(n, val, strlen(val));
    }
    if (id) JS_FreeCString(ctx, id);
    if (val) JS_FreeCString(ctx, val);
    return JS_UNDEFINED;
}

/* hnMeshParam(id, name, value): 数字人参数桥 —— 引擎侧 hn_node_set_mesh_param
   把形变参数写到节点上, 下一次网格求值生效。name ∈ look_x/look_y/mouth/
   body_x/body_y(值 -1..1, 引擎侧钳位; 未知名静默忽略)。与 hnSetText 同一
   纪律: 找不到 id → 返回 false, 不抛异常。 */
static JSValue js_hn_mesh_param(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    if (argc < 3 || !JS_IsString(argv[1])) return JS_FALSE;
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    const char *id = JS_ToCString(ctx, argv[0]);
    const char *name = JS_ToCString(ctx, argv[1]);
    double v = 0;
    JS_ToFloat64(ctx, &v, argv[2]);
    hn_node *n = (b && b->doc && id && name) ? hn_doc_find_by_id(b->doc, id) : NULL;
    if (n) hn_node_set_mesh_param(n, name, (float)v);
    if (id) JS_FreeCString(ctx, id);
    if (name) JS_FreeCString(ctx, name);
    return JS_NewBool(ctx, n != NULL);
}

static JSValue js_hn_fetch_response_text(JSContext *ctx, JSValueConst this_obj,
                                         int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    JSValue body = JS_GetPropertyStr(ctx, this_obj, "__hnBody");
    if (!JS_IsString(body)) { JS_FreeValue(ctx, body); return JS_NewString(ctx, ""); }
    return body;
}

static JSValue js_hn_fetch_response_json(JSContext *ctx, JSValueConst this_obj,
                                         int argc, JSValueConst *argv) {
    (void)argc; (void)argv;
    JSValue body = JS_GetPropertyStr(ctx, this_obj, "__hnBody");
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue json = JS_GetPropertyStr(ctx, global, "JSON");
    JSValue parse = JS_GetPropertyStr(ctx, json, "parse");
    JSValue result = JS_IsFunction(ctx, parse)
        ? JS_Call(ctx, parse, json, 1, (JSValueConst *)&body) : JS_EXCEPTION;
    JS_FreeValue(ctx, parse); JS_FreeValue(ctx, json); JS_FreeValue(ctx, global);
    JS_FreeValue(ctx, body);
    return result;
}

static JSValue js_hn_fetch(JSContext *ctx, JSValueConst this_obj,
                           int argc, JSValueConst *argv) {
    (void)this_obj;
    JSValue funcs[2] = { JS_UNDEFINED, JS_UNDEFINED };
    JSValue promise = JS_NewPromiseCapability(ctx, funcs);
    if (JS_IsException(promise)) return promise;
    const char *url = argc > 0 ? JS_ToCString(ctx, argv[0]) : NULL;
    if (!url) {
        JSValue err = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, "fetch URL is required"));
        JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, (JSValueConst *)&err);
        JS_FreeValue(ctx, err); JS_FreeValue(ctx, funcs[0]); JS_FreeValue(ctx, funcs[1]);
        return promise;
    }
    const char *method = "GET";
    char *method_owned = NULL;
    const char *body_s = NULL;
    hn_http_header headers[32]; size_t header_n = 0;
    memset(headers, 0, sizeof(headers));
    if (argc > 1 && JS_IsObject(argv[1])) {
        JSValue mv = JS_GetPropertyStr(ctx, argv[1], "method");
        if (JS_IsString(mv)) { const char *m = JS_ToCString(ctx, mv); if (m && *m) { method_owned = strdup(m); method = method_owned; JS_FreeCString(ctx, m); } else if (m) JS_FreeCString(ctx, m); }
        JSValue bv = JS_GetPropertyStr(ctx, argv[1], "body");
        if (JS_IsString(bv)) body_s = JS_ToCString(ctx, bv);
        JSValue hv = JS_GetPropertyStr(ctx, argv[1], "headers");
        if (JS_IsObject(hv)) {
            JSPropertyEnum *props = NULL; uint32_t count = 0;
            if (JS_GetOwnPropertyNames(ctx, &props, &count, hv, JS_GPN_STRING_MASK | JS_GPN_ENUM_ONLY) == 0) {
                for (uint32_t i = 0; i < count && header_n < 32; i++) {
                    const char *name = JS_AtomToCString(ctx, props[i].atom);
                    JSValue val = JS_GetProperty(ctx, hv, props[i].atom);
                    const char *value = JS_ToCString(ctx, val);
                    if (name && value) { headers[header_n].name = name; headers[header_n].value = value; header_n++; }
                    else { if (name) JS_FreeCString(ctx, name); if (value) JS_FreeCString(ctx, value); }
                    JS_FreeValue(ctx, val); JS_FreeAtom(ctx, props[i].atom);
                }
                js_free(ctx, props);
            }
        }
        JS_FreeValue(ctx, mv); JS_FreeValue(ctx, bv); JS_FreeValue(ctx, hv);
    }

    /* Get runtime for async scope */
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    hn_rt *rt = b ? b->rt : NULL;
    if (!rt) {
        JSValue err = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, "fetch runtime unavailable"));
        JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, (JSValueConst *)&err);
        JS_FreeValue(ctx, err);
        goto cleanup;
    }

    /* Lazy init HTTP scope */
    if (!rt->http_scope) {
        rt->http_scope = hn_http_scope_create();
        if (!rt->http_scope) {
            JSValue err = JS_NewError(ctx);
            JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, "HTTP not available"));
            JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, (JSValueConst *)&err);
            JS_FreeValue(ctx, err);
            goto cleanup;
        }
    }

    /* Check pending capacity */
    if (rt->pending_fetch_n >= 32) {
        JSValue err = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, "too many concurrent fetches"));
        JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, (JSValueConst *)&err);
        JS_FreeValue(ctx, err);
        goto cleanup;
    }

    /* Submit async request */
    hn_http_request_opts opts;
    memset(&opts, 0, sizeof(opts)); opts.url = url; opts.method = method;
    opts.headers = headers; opts.header_count = header_n; opts.body = body_s; opts.body_len = body_s ? strlen(body_s) : 0;

    uint64_t req_id = ++rt->fetch_id_seq;
    if (!hn_http_async_submit((hn_http_scope *)rt->http_scope, req_id, &opts)) {
        JSValue err = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx, hn_http_last_error()));
        JS_Call(ctx, funcs[1], JS_UNDEFINED, 1, (JSValueConst *)&err);
        JS_FreeValue(ctx, err);
        goto cleanup;
    }

    /* Save Promise callbacks for completion */
    rt->pending_fetches[rt->pending_fetch_n].id = req_id;
    rt->pending_fetches[rt->pending_fetch_n].resolve = JS_DupValue(ctx, funcs[0]);
    rt->pending_fetches[rt->pending_fetch_n].reject = JS_DupValue(ctx, funcs[1]);
    rt->pending_fetch_n++;

cleanup:
    for (size_t i = 0; i < header_n; i++) { JS_FreeCString(ctx, headers[i].name); JS_FreeCString(ctx, headers[i].value); }
    if (body_s) JS_FreeCString(ctx, body_s);
    if (method_owned) free(method_owned);
    JS_FreeCString(ctx, url);
    JS_FreeValue(ctx, funcs[0]); JS_FreeValue(ctx, funcs[1]);
    return promise;
}

/* HTTP async completion callback: called from hn_rt_frame drain loop */
static JSContext *js_scope_find(const char *key);   /* defined below (scope table) */
static void http_complete_cb(uint64_t id, int ok, const hn_http_response *response,
                             const char *error, void *opaque) {
    hn_rt *rt = (hn_rt *)opaque;
    if (!rt) return;
    JSContext *ctx = js_scope_find(rt->id);
    if (!ctx) return;

    /* Find pending fetch by ID */
    int idx = -1;
    for (int i = 0; i < rt->pending_fetch_n; i++) {
        if (rt->pending_fetches[i].id == id) { idx = i; break; }
    }
    if (idx < 0) return; /* already removed or not found */

    JSValue resolve = rt->pending_fetches[idx].resolve;
    JSValue reject = rt->pending_fetches[idx].reject;

    if (!ok || !response || response->status >= 400) {
        JSValue err = JS_NewError(ctx);
        JS_SetPropertyStr(ctx, err, "message", JS_NewString(ctx,
            !ok ? (error ? error : "fetch failed")
                : "HTTP request failed"));
        if (ok && response)
            JS_SetPropertyStr(ctx, err, "status", JS_NewInt32(ctx, (int)response->status));
        JS_Call(ctx, reject, JS_UNDEFINED, 1, (JSValueConst *)&err);
        JS_FreeValue(ctx, err);
    } else {
        JSValue result = JS_NewObject(ctx);
        char *text = response->body_len > 0 ? (char *)malloc(response->body_len + 1) : NULL;
        if (text) { memcpy(text, response->body, response->body_len); text[response->body_len] = 0; }
        JS_SetPropertyStr(ctx, result, "ok", JS_NewBool(ctx, response->status >= 200 && response->status < 300));
        JS_SetPropertyStr(ctx, result, "status", JS_NewInt32(ctx, (int)response->status));
        const char *status_text = response->status == 200 ? "OK" :
                                  (response->status == 201 ? "Created" : "HTTP response");
        JS_SetPropertyStr(ctx, result, "statusText", JS_NewString(ctx, status_text));
        JSValue header_obj = JS_NewObject(ctx);
        if (response->content_type)
            JS_SetPropertyStr(ctx, header_obj, "content-type", JS_NewString(ctx, response->content_type));
        JS_SetPropertyStr(ctx, result, "headers", header_obj);
        JS_SetPropertyStr(ctx, result, "__hnBody",
                         JS_NewStringLen(ctx, text ? text : "", response->body_len));
        JS_SetPropertyStr(ctx, result, "text",
                         JS_NewCFunction(ctx, js_hn_fetch_response_text, "text", 0));
        JS_SetPropertyStr(ctx, result, "json",
                         JS_NewCFunction(ctx, js_hn_fetch_response_json, "json", 0));
        JS_Call(ctx, resolve, JS_UNDEFINED, 1, (JSValueConst *)&result);
        JS_FreeValue(ctx, result);
        free(text);
    }

    /* Remove from pending list */
    JS_FreeValue(ctx, resolve);
    JS_FreeValue(ctx, reject);
    memmove(&rt->pending_fetches[idx], &rt->pending_fetches[idx + 1],
            (size_t)(rt->pending_fetch_n - idx - 1) * sizeof(rt->pending_fetches[0]));
    rt->pending_fetch_n--;
}

/* Media bridge: missing host or target is a no-op, matching hnSetText. */

/* Resolve the context carried by the QuickJS bridge. */
static hn_context *js_media_ctx(JSContext *ctx) {
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    if (!b) return NULL;
    if (b->ctx) return b->ctx;
    return doc_ctx_find(b->doc);
}

static JSValue js_hn_media_play(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_FALSE;
    hn_context *c = js_media_ctx(ctx);
    const char *id = JS_ToCString(ctx, argv[0]);
    hn_node *n = (c && id) ? hn_doc_find_by_id(hn_context_doc(c), id) : NULL;
    int ok = (c && n) ? hn_media_play(c, n) : 0;
    if (id) JS_FreeCString(ctx, id);
    return JS_NewBool(ctx, ok);
}

static JSValue js_hn_media_pause(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_UNDEFINED;
    hn_context *c = js_media_ctx(ctx);
    const char *id = JS_ToCString(ctx, argv[0]);
    hn_node *n = (c && id) ? hn_doc_find_by_id(hn_context_doc(c), id) : NULL;
    if (c && n) hn_media_pause(c, n);
    if (id) JS_FreeCString(ctx, id);
    return JS_UNDEFINED;
}

static JSValue js_hn_media_seek(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_FALSE;
    hn_context *c = js_media_ctx(ctx);
    const char *id = JS_ToCString(ctx, argv[0]);
    double sec = 0;
    JS_ToFloat64(ctx, &sec, argv[1]);
    hn_node *n = (c && id) ? hn_doc_find_by_id(hn_context_doc(c), id) : NULL;
    int ok = (c && n) ? hn_media_seek(c, n, sec) : 0;
    if (id) JS_FreeCString(ctx, id);
    return JS_NewBool(ctx, ok);
}

static JSValue js_hn_media_time(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NewFloat64(ctx, 0);
    hn_context *c = js_media_ctx(ctx);
    const char *id = JS_ToCString(ctx, argv[0]);
    hn_node *n = (c && id) ? hn_doc_find_by_id(hn_context_doc(c), id) : NULL;
    double t = (c && n) ? hn_media_time(c, n) : 0;
    if (id) JS_FreeCString(ctx, id);
    return JS_NewFloat64(ctx, t);
}

static JSValue js_hn_media_duration(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NewFloat64(ctx, -1);
    hn_context *c = js_media_ctx(ctx);
    const char *id = JS_ToCString(ctx, argv[0]);
    hn_node *n = (c && id) ? hn_doc_find_by_id(hn_context_doc(c), id) : NULL;
    double d = (c && n) ? hn_media_duration(c, n) : -1;
    if (id) JS_FreeCString(ctx, id);
    return JS_NewFloat64(ctx, d);
}

/* hnMediaVolume(id, v): 音量 + 静音联动(契约表: set_volume + set_muted)。
   v>0 = 解除静音, v<=0 = 静音 —— 音量归零与静音同义。 */
static JSValue js_hn_media_volume(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    if (argc < 2) return JS_UNDEFINED;
    hn_context *c = js_media_ctx(ctx);
    const char *id = JS_ToCString(ctx, argv[0]);
    double v = 0;
    JS_ToFloat64(ctx, &v, argv[1]);
    hn_node *n = (c && id) ? hn_doc_find_by_id(hn_context_doc(c), id) : NULL;
    if (c && n) {
        hn_media_set_volume(c, n, (float)v);
        hn_media_set_muted(c, n, v <= 0.0);
    }
    if (id) JS_FreeCString(ctx, id);
    return JS_UNDEFINED;
}

/* ---- WASM 桥(hnWasm*, 契约 §6/§7): 模块表在引擎侧(hn_wasm.c),
   id = 表下标+1。失败返回 -1 / NaN, 不抛异常(与 hnMedia* 一致) ---- */

/* hnWasmLoad(bytesOrPath) → id(≥1; 失败 -1)。bytes = ArrayBuffer/Uint8Array;
   字符串按资产路径经资产后端读(引擎不做 I/O, 复用 hn_rt 的资产表)。
   hn_wasm_load 内部复制字节, JS 侧缓冲生命周期无关。 */
static JSValue js_hn_wasm_load(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    if (argc < 1) return JS_NewInt32(ctx, -1);
    const unsigned char *bytes = NULL;
    size_t n = 0;
    if (JS_IsString(argv[0])) {
        const char *path = JS_ToCString(ctx, argv[0]);
        if (path) {
            bytes = (const unsigned char *)asset_load(NULL, path, &n);
            JS_FreeCString(ctx, path);
        }
    } else {
        /* TypedArray(Uint8Array)→ 取后备 ArrayBuffer 的字节区间;
           本身就是 ArrayBuffer 时直接取。都不是 → 清掉残留异常, 返回 -1。 */
        size_t off = 0, view_len = 0, bpe = 0;
        JSValue tab = JS_GetTypedArrayBuffer(ctx, argv[0], &off, &view_len, &bpe);
        if (!JS_IsException(tab)) {
            size_t total = 0;
            uint8_t *buf = JS_GetArrayBuffer(ctx, &total, tab);
            if (buf && view_len <= total && off <= total - view_len) {
                bytes = buf + off;
                n = view_len;
            }
            JS_FreeValue(ctx, tab);
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx));   /* 桥不抛异常 */
            bytes = JS_GetArrayBuffer(ctx, &n, argv[0]);
        }
    }
    if (!bytes) {
        JS_FreeValue(ctx, JS_GetException(ctx));       /* JS_GetArrayBuffer 的失败残留 */
        return JS_NewInt32(ctx, -1);
    }
    return JS_NewInt32(ctx, hn_wasm_install(bytes, n));
}

/* hnWasmCall(id, fn, ...args) → i32 结果; 失败 NaN(契约 §6: args 按 Number→i32) */
static JSValue js_hn_wasm_call(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    if (argc < 2 || argc - 2 > 64) return JS_NAN;
    int32_t id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    const char *fn = JS_ToCString(ctx, argv[1]);
    hn_wasm *w = fn ? hn_wasm_by_id(id) : NULL;
    int32_t vals[64];
    int na = 0;
    for (int i = 2; i < argc; i++, na++)
        JS_ToInt32(ctx, &vals[na], argv[i]);
    int32_t out = 0;
    int ok = (w && fn) ? hn_wasm_call(w, fn, vals, na, &out) : 0;
    if (fn) JS_FreeCString(ctx, fn);
    return ok ? JS_NewInt32(ctx, out) : JS_NAN;
}

/* ---- <canvas> 2D 桥(hnCanvas2D) ----
 * glue(见下方 CANVAS_GLUE_JS)在 JS 层维护状态机(fillStyle/save/restore
 * 栈/当前路径/CTM/渐变对象 —— 与 canvaskit htmlcanvas/canvas2dcontext.js 的
 * JS 层设计同构), 每个绘制调用在 JS 里解析成"已解析的原语 op"(坐标已经
 * CTM 变换、颜色已 RGBA), JSON 序列化后调本桥 → 引擎 hn_canvas_record
 * 落账(协议见 Sources/CHtmlNative/hn_canvas.c 头注)。
 * 返回记录后的账本 op 数(失败 -1; clear 返回清掉的条数)。 */
static JSValue js_hn_canvas_2d(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    if (argc < 1 || !JS_IsString(argv[0])) return JS_NewInt32(ctx, -1);
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    hn_context *c = b ? (b->ctx ? b->ctx : doc_ctx_find(b->doc)) : NULL;
    if (!c) return JS_NewInt32(ctx, -1);
    size_t len = 0;
    const char *json = JS_ToCStringLen(ctx, &len, argv[0]);
    if (!json) return JS_NewInt32(ctx, -1);
    int n = hn_canvas_record(c, json, len);
    JS_FreeCString(ctx, json);
    return JS_NewInt32(ctx, n);
}

/* __hnMeasureText(deviceFontSizePx, text) → 宽度 px(canvas measureText 的
 * 测量缝)。走 hn_canvas_measure —— 引擎 context 的文本后端(与布局排版
 * 同一测量入口); 无后端构建 = 字节数×字号×0.55 估算, 与引擎同口径。
 * 字号按 glue 现值传入(canvas font 只解析字号, 缺省 400/无衬线)。 */
static JSValue js_hn_measure_text(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    hn_context *c = b ? (b->ctx ? b->ctx : doc_ctx_find(b->doc)) : NULL;
    double px = 10.0;
    if (argc > 0) JS_ToFloat64(ctx, &px, argv[0]);
    float w = 0;
    if (argc > 1 && JS_IsString(argv[1])) {
        size_t len = 0;
        const char *s = JS_ToCStringLen(ctx, &len, argv[1]);
        if (s) w = hn_canvas_measure(c, (float)px, s, len);
        if (s) JS_FreeCString(ctx, s);
    }
    return JS_NewFloat64(ctx, w);
}

/* __hnCanvasPixels(cvId, x, y, w, h) → ArrayBuffer(RGBA 非预乘) | null
 * canvas getImageData 的原生数据出口: 引擎把该画布账本离屏栅格化后裁出
 * 子区(标准 ImageData 口径: 越界补透明黑, 非预乘)。glue 侧包成
 * {width, height, data:Uint8Array}; 失败返回 null(不抛异常, 桥纪律)。 */
static JSValue js_hn_canvas_pixels(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    hn_context *c = b ? (b->ctx ? b->ctx : doc_ctx_find(b->doc)) : NULL;
    if (!c || argc < 5) return JS_NULL;
    const char *id = JS_ToCString(ctx, argv[0]);
    if (!id) return JS_NULL;
    int32_t x = 0, y = 0, w = 0, h = 0;
    JS_ToInt32(ctx, &x, argv[1]);
    JS_ToInt32(ctx, &y, argv[2]);
    JS_ToInt32(ctx, &w, argv[3]);
    JS_ToInt32(ctx, &h, argv[4]);
    unsigned char *px = NULL;
    int ow = 0, oh = 0;
    int ok = hn_canvas_get_image_data(c, id, x, y, w, h, &px, &ow, &oh);
    JS_FreeCString(ctx, id);
    if (!ok || !px) return JS_NULL;
    JSValue ab = JS_NewArrayBufferCopy(ctx, px, (size_t)ow * oh * 4);
    free(px);
    return ab;
}

/* __hnCanvasPng(cvId) → "data:image/png;base64,..."(toDataURL 的原生缝):
 * 画布账本离屏栅格化(元素 width/height 属性口径) → hnsoft_encode_png →
 * base64。失败返回 null(桥纪律)。 */
static void b64_enc(const unsigned char *in, size_t n, char *out) {
    static const char *T = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t i, o = 0;
    for (i = 0; i + 2 < n; i += 3) {
        unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8) | in[i + 2];
        out[o++] = T[(v >> 18) & 63]; out[o++] = T[(v >> 12) & 63];
        out[o++] = T[(v >> 6) & 63];  out[o++] = T[v & 63];
    }
    size_t rem = n - i;
    if (rem == 1) {
        unsigned v = (unsigned)in[i] << 16;
        out[o++] = T[(v >> 18) & 63]; out[o++] = T[(v >> 12) & 63];
        out[o++] = '='; out[o++] = '=';
    } else if (rem == 2) {
        unsigned v = ((unsigned)in[i] << 16) | ((unsigned)in[i + 1] << 8);
        out[o++] = T[(v >> 18) & 63]; out[o++] = T[(v >> 12) & 63];
        out[o++] = T[(v >> 6) & 63];  out[o++] = '=';
    }
    out[o] = 0;
}

static JSValue js_hn_canvas_png(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    hn_context *c = b ? (b->ctx ? b->ctx : doc_ctx_find(b->doc)) : NULL;
    if (!c || argc < 1) return JS_NULL;
    const char *id = JS_ToCString(ctx, argv[0]);
    if (!id) return JS_NULL;
    /* 画布属性尺寸(与 hn_canvas_get_image_data 内部同口径) */
    float cw = 300, ch = 150;
    hn_node *n = b->doc ? hn_doc_find_by_id(b->doc, id) : NULL;
    if (n) {
        const char *aw = hn_node_attr(n, "width");
        const char *ah = hn_node_attr(n, "height");
        if (aw && *aw) cw = (float)atof(aw);
        if (ah && *ah) ch = (float)atof(ah);
    }
    if (cw < 1) cw = 1;
    if (ch < 1) ch = 1;
    int w = (int)cw, h = (int)ch;
    if ((float)w < cw) w++;
    if ((float)h < ch) h++;
    unsigned char *px = NULL;
    int ow = 0, oh = 0;
    int ok = hn_canvas_get_image_data(c, id, 0, 0, w, h, &px, &ow, &oh);
    JS_FreeCString(ctx, id);
    if (!ok || !px) return JS_NULL;
    size_t pn = 0;
    unsigned char *png = hnsoft_encode_png(px, ow, oh, &pn);
    free(px);
    if (!png) return JS_NULL;
    size_t b64n = ((pn + 2) / 3) * 4;
    char *b64 = (char *)malloc(b64n + 1);
    if (!b64) { free(png); return JS_NULL; }
    b64_enc(png, pn, b64);
    free(png);
    char head[] = "data:image/png;base64,";
    size_t hn_ = sizeof(head) - 1;
    char *url = (char *)malloc(hn_ + b64n + 1);
    if (!url) { free(b64); return JS_NULL; }
    memcpy(url, head, hn_);
    memcpy(url + hn_, b64, b64n + 1);
    free(b64);
    JSValue r = JS_NewString(ctx, url);
    free(url);
    return r;
}

/* ---------------- <canvas> 2D glue(JS 层状态机) ----------------
 * 标准 2D context 的方法面全部在此实现, 汇到 hnCanvas2D:
 *   fillRect/strokeRect/beginPath/moveTo/lineTo/arc/ellipse/quadraticCurveTo/
 *   bezierCurveTo/arcTo/closePath/fill/stroke/clip/save/restore/translate/
 *   scale/rotate/setTransform/transform/resetTransform/fillText/strokeText/
 *   measureText/createLinearGradient/createRadialGradient/clearRect/
 *   drawImage(3/5/9 参)/setLineDash/getLineDash/createPattern/
 *   getImageData/putImageData
 *   + fillStyle/strokeStyle/lineWidth/lineCap/font/lineDashOffset/textAlign/
 *     textBaseline/shadowColor/shadowBlur/shadowOffsetX/shadowOffsetY/
 *     globalCompositeOperation 属性。
 * 简化口径(注释即契约):
 *   - arc/ellipse 以 Math.cos/sin 折线化, 15° 一段(与账本 op 的折线模型
 *     一致); 二次曲线固定 16 段、三次曲线固定 24 段(恒定段数, 段内直线
 *     近似 —— 与折线模型同档, 不做自适应细分);
 *   - arcTo 用切点/圆心闭式解; 退化输入(半径非正/共线/折返)退化为
 *     lineTo 到拐点 —— 不崩溃是第一契约;
 *   - clearRect 记录 clear_rect op(绘制期矩形内回填渲染底色); 覆盖整块
 *     画布的清空由引擎识别为清账("清账重录"动画循环保持 op 有界);
 *   - clip() 下送精确路径 op(cpath, 折线多边形 + 子路径点数表) —— 引擎
 *     光栅期按多边形内外判定(非零环绕); 旧的矩形包围盒口径(clip op)
 *     保留在引擎侧, 但 glue 不再产生它;
 *   - 渐变: createLinearGradient/createRadialGradient 全部 stop 按扁平数组
 *     下送(引擎分段线性插值); radial 为 CSS createRadialGradient 双圆口径;
 *     fill()/stroke() 遇渐变下送 gpath/带渐变 spath(不再回落纯色);
 *   - pattern: createPattern(图片路径, repetition)返回 JS 侧标记对象
 *     (与渐变对象同构); fillRect/fill 遇 pattern 下送 prect/ppath(平铺
 *     填充); **stroke 遇 pattern 暂回落纯色黑**(描边是逐段 QUAD 展开,
 *     平铺描边带不在本轮范围 —— 见 hn_canvas.c 的注释契约);
 *   - getImageData 返回 {width,height,data:Uint8Array}(非预乘 RGBA,
 *     账本离屏栅格化, 越界补透明黑); putImageData 接收同构对象(或
 *     {width,height,data} 形状的普通数组), 脏矩形 4 参齐备才启用;
 *   - globalCompositeOperation 支持 source-over(缺省)/lighter/multiply/
 *     screen, 随每条 op 下送 blend 字段; 其余模式照常存储, 由引擎侧
 *     回落 source-over 并提示一次;
 *   - setLineDash 在 stroke 落账前按折线累计弧长切段(每段一个 spath;
 *     单次 stroke 上限 512 段, 到顶截断防失控路径灌爆账本);
 *   - textAlign(left/center/right)按 measureText 宽度修正锚点; textBaseline
 *     (top/middle/alphabetic)按 0.8/0.3×字号 的比例修正基线(与引擎无
 *     后端回退口径一致);
 *   - measureText 走 __hnMeasureText 原生桥(引擎文本后端同源测量;
 *     无后端构建 = 字节数×字号×0.55);
 *   - shadow 仅随 fillRect 系(frect/grect)下送(路径填充/描边暂不带影);
 *     引擎阴影按 blur>0 生效, blur=0 的硬阴影不支持;
 *   - 线宽/字号随 CTM 按sqrt(|det|) 均匀近似(折线模型的既定简化);
 *   - 文字 y 缺省 = 基线(textBaseline 缺省 alphabetic)。
 * 入口: hnGet2D(id) 直接返回 context; 宿主没有 document 时补一个仅支持
 * canvas 的最小 getElementById(形如标准用法的替代入口)。 */
static const char *CANVAS_GLUE_JS =
"(function(g){"
"  if (typeof g.hnGet2D === 'function') return;"
"  function clamp255(v){v=+v;return v<0?0:(v>255?255:v);}"
"  var NAMED={black:[0,0,0],silver:[192,192,192],gray:[128,128,128],grey:[128,128,128],"
"    white:[255,255,255],red:[255,0,0],green:[0,128,0],blue:[0,0,255],yellow:[255,255,0],"
"    orange:[255,165,0],purple:[128,0,128],fuchsia:[255,0,255],cyan:[0,255,255],"
"    magenta:[255,0,255],lime:[0,255,0],maroon:[128,0,0],navy:[0,0,128],olive:[128,128,0],"
"    teal:[0,128,128],aqua:[0,255,255],gold:[255,215,0],pink:[255,192,203],"
"    transparent:[0,0,0,0]};"
"  function parseColor(v){"
"    var c=[0,0,0,255];"
"    if (v && v.__hngrad) return c;"
"    if (typeof v!=='string') return c;"
"    var s=v.trim();"
"    if (s.charAt(0)==='#'){"
"      var h=s.slice(1);"
"      if (h.length===3||h.length===4) h=h[0]+h[0]+h[1]+h[1]+h[2]+h[2]+(h.length===4?h[3]+h[3]:'');"
"      if (h.length>=6){"
"        c[0]=parseInt(h.slice(0,2),16)||0;"
"        c[1]=parseInt(h.slice(2,4),16)||0;"
"        c[2]=parseInt(h.slice(4,6),16)||0;"
"        if (h.length>=8) c[3]=parseInt(h.slice(6,8),16)||0;"
"      }"
"      return c;"
"    }"
"    if (s.slice(0,4)==='rgba'&&s.charAt(4)==='('){"
"      var p=s.slice(5,s.lastIndexOf(')')).split(',');"
"      c[0]=clamp255(parseFloat(p[0]));c[1]=clamp255(parseFloat(p[1]));c[2]=clamp255(parseFloat(p[2]));"
"      c[3]=Math.round((p.length>3?parseFloat(p[3]):1)*255);"
"      return c;"
"    }"
"    if (s.slice(0,4)==='rgb('&&s.charAt(3)==='('){"
"      var q=s.slice(4,s.lastIndexOf(')')).split(',');"
"      c[0]=clamp255(parseFloat(q[0]));c[1]=clamp255(parseFloat(q[1]));c[2]=clamp255(parseFloat(q[2]));"
"      return c;"
"    }"
"    var n=NAMED[s.toLowerCase()];"
"    if (n) return [n[0],n[1],n[2], n.length>3?n[3]:255];"
"    return c;"
"  }"
"  function rgba(c){"
"    return ((clamp255(Math.round(c[0]))<<24)|(clamp255(Math.round(c[1]))<<16)|"
"            (clamp255(Math.round(c[2]))<<8)|clamp255(Math.round(c[3])))>>>0;"
"  }"
"  var cache={};"
"  function hnGet2D(id){"
"    id=String(id);"
"    if (cache[id]) return cache[id];"
"    var z=make2D(id);"
"    cache[id]=z;"
"    return z;"
"  }"
"  g.hnGet2D=hnGet2D;"
"  if (!g.document) {"
"    g.document={getElementById:function(id){"
"      return {getContext:function(kind){return (kind==='2d'||kind==='2D')?hnGet2D(id):null;}};"
"    }};"
"  }"
"  function make2D(cvId){"
"    function send(o){"
"      o.cv=cvId;"
"      if (st.gco && st.gco!=='source-over') o.blend=st.gco;   /* gco 随 op 下送(pdata 由引擎侧忽略, 标准) */"
"      try{hnCanvas2D(JSON.stringify(o));}catch(e){}"
"    }"
"    function stopsOf(gd){return gd.stops.slice().sort(function(p,q){return p.t-q.t;});}"
"    function solidOf(style){"
"      if (style && style.__hngrad){"
"        var s=stopsOf(style);"
"        var a=s.length?s[0].c:[0,0,0,255];"
"        var b=s.length?s[s.length-1].c:a;"
"        return [(a[0]+b[0])/2,(a[1]+b[1])/2,(a[2]+b[2])/2,(a[3]+b[3])/2];"
"      }"
"      /* pattern 作描边色: 回落纯色黑(描边平铺带不在本轮范围, 见头注) */"
"      return parseColor(style);"
"    }"
"    function defaultState(){return {fill:[0,0,0,255],stroke:[0,0,0,255],"
"      fillSrc:'#000000',strokeSrc:'#000000',lw:1,cap:'butt',fontPx:10,"
"      fontSrc:'10px sans-serif',m:[1,0,0,1,0,0],clips:0,dash:[],dashOff:0,"
"      align:'left',base:'alphabetic',gco:'source-over',"
"      shSrc:'rgba(0,0,0,0)',shC:[0,0,0,0],shX:0,shY:0,shB:0,"
"      ga:1,join:'miter',ml:10,lsp:0,wsp:0,flt:0};}"
"    var st=defaultState();"
"    var stack=[],subs=[],cur=null;"
"    /* globalAlpha 统一乘点: make2D 内所有下送颜色都过这里(影子/渐变 stop 同乘)。 */"
"    function rgbaGA(c){"
"      var r=rgba(c);"
"      if (st.ga>=1) return r;"
"      if (st.ga<=0) return (r & 0xFFFFFF00) >>> 0;"
"      /* >>>0 归正: 位运算产生的是 int32(负数), JSON 负数到引擎侧会丢 */"
"      return ((r & 0xFFFFFF00) | (((r & 255) * st.ga) | 0)) >>> 0;"
"    }"
"    function xf(x,y){var m=st.m;return [m[0]*x+m[2]*y+m[4], m[1]*x+m[3]*y+m[5]];}"
"    function mul(n){var m=st.m;"
"      st.m=[m[0]*n[0]+m[2]*n[1], m[1]*n[0]+m[3]*n[1],"
"            m[0]*n[2]+m[2]*n[3], m[1]*n[2]+m[3]*n[3],"
"            m[0]*n[4]+m[2]*n[5]+m[4], m[1]*n[4]+m[3]*n[5]+m[5]];}"
"    function detScale(){var m=st.m;return Math.sqrt(Math.abs(m[0]*m[3]-m[1]*m[2]))||1;}"
"    function ensure(){if(!cur){cur=[];subs.push({p:cur,closed:false});}}"
"    function cloneState(s){return {fill:s.fill.slice(),stroke:s.stroke.slice(),"
"      fillSrc:s.fillSrc,strokeSrc:s.strokeSrc,lw:s.lw,cap:s.cap,fontPx:s.fontPx,"
"      fontSrc:s.fontSrc,m:s.m.slice(),clips:s.clips,dash:s.dash.slice(),dashOff:s.dashOff,"
"      align:s.align,base:s.base,gco:s.gco,"
"      shSrc:s.shSrc,shC:s.shC.slice(),shX:s.shX,shY:s.shY,shB:s.shB,"
"      ga:s.ga,join:s.join,ml:s.ml,lsp:s.lsp,wsp:s.wsp,flt:s.flt};}"
"    /* 渐变 op 公共字段: 轴/圆坐标经 CTM 变换(渐变坐标属 user space),"
"       全部 stop 按 [t,rgba,...] 扁平数组下送(引擎分段线性插值)。 */"
"    function gradOp(gd){"
"      var ds=detScale(),s=stopsOf(gd);"
"      if (!s.length) s=[{t:0,c:[0,0,0,255]},{t:1,c:[0,0,0,255]}];"
"      var flat=[];"
"      for (var i=0;i<s.length;i++) flat.push(s[i].t,rgba(s[i].c));"
"      var o={radial:gd.radial?1:0,c0:rgba(s[0].c),c1:rgba(s[s.length-1].c),stops:flat};"
"      if (gd.radial){"
"        var q0=xf(gd.x0,gd.y0),q1=xf(gd.x1,gd.y1);"
"        o.x0=q0[0];o.y0=q0[1];o.x1=q1[0];o.y1=q1[1];"
"        o.r0=(gd.r0||0)*ds;o.r1=(gd.r1||0)*ds;"
"      } else {"
"        var p0=xf(gd.x0,gd.y0),p1=xf(gd.x1,gd.y1);"
"        o.x0=p0[0];o.y0=p0[1];o.x1=p1[0];o.y1=p1[1];"
"      }"
"      return o;"
"    }"
"    /* 文本测量: __hnMeasureText 原生桥(设备字号, 串) → 宽 px;"
"       桥缺失/异常时按 字符数×字号×0.55 估算(与引擎无后端口径一致)。 */"
"    function measureW(s){"
"      var w=0;"
"      try{w=__hnMeasureText(st.fontPx*detScale(),s);}catch(e){}"
"      w=(typeof w==='number'&&isFinite(w)&&w>0)?w:String(s).length*st.fontPx*0.55;"
"      /* letterSpacing 计入度量(canvas 标准: 每字符后加字距) */"
"      if (st.lsp>0){var cps=0;"
"        for(var i=0;i<s.length;i++){if((s.charCodeAt(i)&0xC0)!==0x80)cps++;}"
"        w+=st.lsp*cps;}"
"      return w;"
"    }"
"    function textOp(s,x,y,style){"
"      var q=xf(x,y);"
"      var px=st.fontPx*detScale();"
"      if (st.align==='center') q[0]-=measureW(String(s))/2;"
"      else if (st.align==='right') q[0]-=measureW(String(s));"
"      if (st.base==='top') q[1]+=px*0.8;"
"      else if (st.base==='middle') q[1]+=px*0.3;"
"      send({op:'text',s:String(s),x:q[0],y:q[1],px:px,lsp:st.lsp+st.wsp*0,col:rgbaGA(solidOf(style))});"
"    }"
"    function rectPts(x,y,w,h){return [x,y, x+w,y, x+w,y+h, x,y+h];}"
"    function shadowOn(o){"
"      if (st.shC[3]>0&&st.shB>0){o.shc=rgbaGA(st.shC);o.shx=st.shX;o.shy=st.shY;o.shb=st.shB;}"
"      return o;"
"    }"
"    function isPat(v){return v && v.__hnpat;}"
"    function isGrad(v){return v && v.__hngrad;}"
"    function patOp(op,src,x,y,w,h,pts){"
"      var o={op:op,src:src.src,rep:src.rep};"
"      if (pts) o.pts=pts; else {o.x=x;o.y=y;o.w=w;o.h=h;}"
"      return o;"
"    }"
"    function fillRectImpl(x,y,w,h){"
"      var m=st.m,fs=st.fillSrc,grad=isGrad(fs),pat=isPat(fs),rotated=(m[1]!==0||m[2]!==0),o,gg;"
"      if (pat){"
"        var p=rectPts(x,y,w,h),t=[];"
"        for (var i0=0;i0<8;i0+=2){var q3=xf(p[i0],p[i0+1]);t.push(q3[0],q3[1]);}"
"        if (!rotated){"
"          var r0=xf(x,y),rw=m[0]*w,rh=m[3]*h;"
"          if (rw<0){r0[0]+=rw;rw=-rw;}"
"          if (rh<0){r0[1]+=rh;rh=-rh;}"
"          send(patOp('prect',fs,r0[0],r0[1],rw,rh));   /* 矩形快路 */"
"        } else send(patOp('ppath',fs,0,0,0,0,t));      /* 旋转: 多边形路径 */"
"        return;"
"      }"
"      if (rotated){"
"        var p=rectPts(x,y,w,h),t=[];"
"        for (var i=0;i<8;i+=2){var q=xf(p[i],p[i+1]);t.push(q[0],q[1]);}"
"        if (grad){o={op:'gpath',pts:t};gg=gradOp(st.fillSrc);"
"          for (var k in gg)o[k]=gg[k];}"
"        else o={op:'fpath',pts:t,col:rgbaGA(solidOf(st.fillSrc))};"
"        send(shadowOn(o));"
"        return;"
"      }"
"      var q0=xf(x,y),tw=m[0]*w,th=m[3]*h;"
"      if (tw<0){q0[0]+=tw;tw=-tw;}"
"      if (th<0){q0[1]+=th;th=-th;}"
"      if (grad){o={op:'grect',x:q0[0],y:q0[1],w:tw,h:th};"
"        gg=gradOp(st.fillSrc);for (var k2 in gg)o[k2]=gg[k2];}"
"      else o={op:'frect',x:q0[0],y:q0[1],w:tw,h:th,col:rgbaGA(st.fill)};"
"      /* ctx.filter blur: 借 shadow 管线(shb=2σ, 偏移 0) —— 引擎侧"
"         3-pass 高斯; 本体不画(col=0), 只留模糊蒙版 */"
"      if (st.flt>0 && !o.shc){o.shc=rgbaGA(st.fill);o.shx=0;o.shy=0;"
"        o.shb=st.flt*2; o.col=0;}"
"      send(shadowOn(o));"
"    }"
"    var api={};"
"    Object.defineProperty(api,'fillStyle',{"
"      get:function(){return st.fillSrc;},"
"      set:function(v){st.fillSrc=v;st.fill=(typeof v==='object'&&v)?null:parseColor(v);}});"
"    Object.defineProperty(api,'strokeStyle',{"
"      get:function(){return st.strokeSrc;},"
"      set:function(v){st.strokeSrc=v;st.stroke=(typeof v==='object'&&v)?null:parseColor(v);}});"
"    Object.defineProperty(api,'globalCompositeOperation',{"
"      get:function(){return st.gco;},set:function(v){st.gco=String(v||'source-over');}});"
"    Object.defineProperty(api,'lineWidth',{"
"      get:function(){return st.lw;},set:function(v){st.lw=(+v)>0?+v:1;}});"
"    Object.defineProperty(api,'lineCap',{"
"      get:function(){return st.cap;},set:function(v){st.cap=String(v);}});"
"    Object.defineProperty(api,'font',{"
"      get:function(){return st.fontSrc;},"
"      set:function(v){st.fontSrc=String(v);"
"        var m=/([0-9.]+)\\s*px/.exec(st.fontSrc);"
"        if (m) st.fontPx=parseFloat(m[1]);}});"
"    Object.defineProperty(api,'lineDashOffset',{"
"      get:function(){return st.dashOff;},set:function(v){st.dashOff=+v||0;}});"
"    Object.defineProperty(api,'textAlign',{"
"      get:function(){return st.align;},set:function(v){st.align=String(v);}});"
"    Object.defineProperty(api,'textBaseline',{"
"      get:function(){return st.base;},set:function(v){st.base=String(v);}});"
"    Object.defineProperty(api,'shadowColor',{"
"      get:function(){return st.shSrc;},"
"      set:function(v){st.shSrc=String(v);st.shC=parseColor(v);}});"
"    Object.defineProperty(api,'shadowBlur',{"
"      get:function(){return st.shB;},"
"      set:function(v){var b=+v;st.shB=(b>0)?b:0;}});"
"    Object.defineProperty(api,'shadowOffsetX',{"
"      get:function(){return st.shX;},set:function(v){st.shX=+v||0;}});"
"    Object.defineProperty(api,'shadowOffsetY',{"
"      get:function(){return st.shY;},set:function(v){st.shY=+v||0;}});"
"    api.save=function(){stack.push(cloneState(st));};"
"    api.restore=function(){"
"      if (!stack.length) return;"
"      var s=stack.pop();"
"      while (st.clips>s.clips){send({op:'cpop'});st.clips--;}"
"      st.fill=s.fill;st.stroke=s.stroke;st.fillSrc=s.fillSrc;st.strokeSrc=s.strokeSrc;"
"      st.lw=s.lw;st.cap=s.cap;st.fontPx=s.fontPx;st.fontSrc=s.fontSrc;st.m=s.m;"
"      st.dash=s.dash;st.dashOff=s.dashOff;st.align=s.align;st.base=s.base;st.gco=s.gco;"
"      st.shSrc=s.shSrc;st.shC=s.shC;st.shX=s.shX;st.shY=s.shY;st.shB=s.shB;"
"    };"
"    api.translate=function(x,y){mul([1,0,0,1,x,y]);};"
"    api.scale=function(x,y){mul([x,0,0,y,0,0]);};"
"    api.rotate=function(a){var c=Math.cos(a),s=Math.sin(a);mul([c,s,-s,c,0,0]);};"
"    api.transform=function(a,b,c,d,e,f){mul([a,b,c,d,e,f]);};"
"    api.setTransform=function(a,b,c,d,e,f){st.m=[a,b,c,d,e,f];};"
"    api.resetTransform=function(){st.m=[1,0,0,1,0,0];};"
"    api.beginPath=function(){subs=[];cur=null;};"
"    api.closePath=function(){"
"      if (cur&&cur.length>=4){cur.push(cur[0],cur[1]);subs[subs.length-1].closed=true;}"
"    };"
"    api.moveTo=function(x,y){cur=[x,y];subs.push({p:cur,closed:false});};"
"    api.lineTo=function(x,y){ensure();cur.push(x,y);};"
"    /* 曲线族: 全部在落账前折线化(固定段数, 口径见文件头注释)。 */"
"    function arcPts(x,y,rx,ry,rot,a0,a1,ccw){"
"      var TAU=Math.PI*2,sweep=a1-a0;"
"      if (ccw){while(sweep>0)sweep-=TAU;if(sweep===0)sweep=-TAU;}"
"      else{while(sweep<0)sweep+=TAU;if(sweep===0)return;}"
"      var n=Math.max(2,Math.ceil(Math.abs(sweep)/(Math.PI/12)));"
"      var co=Math.cos(rot||0),si=Math.sin(rot||0);"
"      function pt(a){var ex=rx*Math.cos(a),ey=ry*Math.sin(a);"
"        return [x+ex*co-ey*si, y+ex*si+ey*co];}"
"      var s0=pt(a0);"
"      if (cur&&cur.length>=2)cur.push(s0[0],s0[1]);"
"      else{cur=[s0[0],s0[1]];subs.push({p:cur,closed:false});}"
"      for (var i=1;i<=n;i++){var q2=pt(a0+sweep*i/n);cur.push(q2[0],q2[1]);}"
"    }"
"    api.arc=function(x,y,r,a0,a1,ccw){"
"      if (!(r>0)) return;"
"      arcPts(x,y,r,r,0,a0,a1,ccw);"
"    };"
"    api.ellipse=function(x,y,rx,ry,rot,a0,a1,ccw){"
"      if (!(rx>0&&ry>0)) return;"
"      arcPts(x,y,rx,ry,+rot||0,a0,a1,ccw);"
"    };"
"    api.quadraticCurveTo=function(cpx,cpy,x,y){"
"      ensure();var l=cur.length,p0x=cur[l-2],p0y=cur[l-1],n=16;"
"      for (var i=1;i<=n;i++){var t=i/n,u=1-t;"
"        cur.push(u*u*p0x+2*u*t*cpx+t*t*x, u*u*p0y+2*u*t*cpy+t*t*y);}"
"    };"
"    api.bezierCurveTo=function(c1x,c1y,c2x,c2y,x,y){"
"      ensure();var l=cur.length,p0x=cur[l-2],p0y=cur[l-1],n=24;"
"      for (var i=1;i<=n;i++){var t=i/n,u=1-t;"
"        cur.push(u*u*u*p0x+3*u*u*t*c1x+3*u*t*t*c2x+t*t*t*x,"
"                 u*u*u*p0y+3*u*u*t*c1y+3*u*t*t*c2y+t*t*t*y);}"
"    };"
"    /* arcTo: 与两条切线相切的圆弧(切点/圆心闭式解); 退化输入(半径非正/"
"       共线/折返)退化为 lineTo 到拐点。 */"
"    api.arcTo=function(x1,y1,x2,y2,r){"
"      ensure();var l=cur.length,p0x=cur[l-2],p0y=cur[l-1];"
"      var v1x=p0x-x1,v1y=p0y-y1,v2x=x2-x1,v2y=y2-y1;"
"      var l1=Math.sqrt(v1x*v1x+v1y*v1y),l2=Math.sqrt(v2x*v2x+v2y*v2y);"
"      if(!(r>0)||l1<1e-9||l2<1e-9){cur.push(x1,y1);return;}"
"      v1x/=l1;v1y/=l1;v2x/=l2;v2y/=l2;"
"      var cosA=v1x*v2x+v1y*v2y;"
"      if(cosA<=-0.99999||cosA>=0.99999){cur.push(x1,y1);return;}"
"      var sinA=Math.sqrt(1-cosA*cosA);"
"      var tLen=r*sinA/(1+cosA);"
"      var t1x=x1+v1x*tLen,t1y=y1+v1y*tLen,t2x=x1+v2x*tLen,t2y=y1+v2y*tLen;"
"      var bx=v1x+v2x,by=v1y+v2y,bl=Math.sqrt(bx*bx+by*by);"
"      if(bl<1e-9){cur.push(x1,y1);return;}"
"      var cd=r/Math.sqrt((1-cosA)/2);"
"      var cx=x1+bx/bl*cd,cy=y1+by/bl*cd;"
"      var a0=Math.atan2(t1y-cy,t1x-cx),a1=Math.atan2(t2y-cy,t2x-cx);"
"      /* 弧向判据: 先取短弧, 再验证中点方向朝向顶点 x1(弧向顶点鼓出)。"
"         旧判据(v1×v2 叉积 → ccw)在 y-down 屏幕坐标下不成立 ——"
"         '向右走向上转'与'向左走向下转'叉积同号但短弧方向相反,"
"         实测直角会画出 270° 长弧(roundRect 全坏)。 */"
"      var sweep=a1-a0;"
"      while(sweep>Math.PI)sweep-=2*Math.PI;"
"      while(sweep<-Math.PI)sweep+=2*Math.PI;"
"      var mid=a0+sweep/2;"
"      var vx=x1-cx,vy=y1-cy;"
"      if (Math.cos(mid)*vx+Math.sin(mid)*vy<0)"
"        sweep=sweep>0?sweep-2*Math.PI:sweep+2*Math.PI;"
"      arcPts(cx,cy,r,r,0,a0,a0+sweep,sweep<0);"
"    };"
"    api.fill=function(){"
"      var fs=st.fillSrc,grad=isGrad(fs),pat=isPat(fs);"
"      for (var i=0;i<subs.length;i++){"
"        var p=subs[i].p;"
"        if (p.length<6) continue;"
"        if (pat) send(patOp('ppath',fs,0,0,0,0,p.slice()));"
"        else if (!grad) send({op:'fpath',pts:p.slice(),col:rgbaGA(st.fill)});"
"        else{var o={op:'gpath',pts:p.slice()};"
"          var gg=gradOp(st.fillSrc);for (var k in gg)o[k]=gg[k];send(o);}"
"      }"
"    };"
"    /* setLineDash: stroke 落账前按折线累计弧长切段(每段一个 spath;"
"       单次 stroke 上限 512 段, 到顶截断)。 */"
"    function dashSegs(p){"
"      var pat=st.dash,off=+st.dashOff||0;"
"      if(!pat||!pat.length)return [p];"
"      var L=0,i;for(i=0;i<pat.length;i++){pat[i]=+pat[i];if(!(pat[i]>0))pat[i]=0;L+=pat[i];}"
"      if(!(L>0))return [p];"
"      var out=[],run=null,q=off%L;if(q<0)q+=L;"
"      var idx=0,guard=0;"
"      while(q>=pat[idx]&&guard++<4096){q-=pat[idx];idx=(idx+1)%pat.length;}"
"      var on=(idx%2===0);"
"      if(on)run=[p[0],p[1]];"
"      var n=p.length/2;"
"      for(i=1;i<n;i++){"
"        var x0=p[(i-1)*2],y0=p[(i-1)*2+1],x1=p[i*2],y1=p[i*2+1];"
"        var dx=x1-x0,dy=y1-y0,seg=Math.sqrt(dx*dx+dy*dy);"
"        if(seg<1e-9)continue;"
"        var t=0;"
"        while(t<seg-1e-9){"
"          var step=pat[idx]-q;if(step>seg-t)step=seg-t;"
"          t+=step;q+=step;"
"          var bx2=x0+dx*(t/seg),by2=y0+dy*(t/seg);"
"          if(on)run.push(bx2,by2);"
"          if(q>=pat[idx]-1e-9){"
"            if(on&&run&&run.length>=4)out.push(run);"
"            q=0;idx=(idx+1)%pat.length;on=(idx%2===0);"
"            run=on?[bx2,by2]:null;"
"          }"
"        }"
"      }"
"      if(on&&run&&run.length>=4)out.push(run);"
"      return out;"
"    }"
"    api.stroke=function(){"
"      var grad=isGrad(st.strokeSrc);"
"      var col=grad?0:rgbaGA(solidOf(st.strokeSrc));"
"      var w=st.lw*detScale(),cap=(st.cap==='round');"
"      for (var i=0;i<subs.length;i++){"
"        var p=subs[i].p;"
"        if (p.length<4) continue;"
"        var segs=(st.dash&&st.dash.length)?dashSegs(p):[p];"
"        var total=0;"
"        for (var d=0;d<segs.length&&total<512;d++,total++){"
"          var o={op:'spath',pts:segs[d],col:col,w:w,cap:cap};"
"          if (grad){var gg=gradOp(st.strokeSrc);for (var k in gg)o[k]=gg[k];}"
"          send(o);"
"        }"
"      }"
"    };"
"    /* clip(): 精确路径裁剪 —— 当前路径的每条子路径(≥3 点)按 CTM 变换后"
"       作为折线多边形下送 cpath(含 segs 点数表), 引擎光栅期按非零环绕"
"       内外判定; <3 点的子路径为零面积, 跳过(与 fill 的点数门禁同档)。 */"
"    api.clip=function(){"
"      var pts=[],segs=[],any=false;"
"      for (var i=0;i<subs.length;i++){"
"        var p=subs[i].p;"
"        if (p.length<6) continue;"
"        var cnt=0;"
"        for (var j=0;j+1<p.length;j+=2){"
"          var q=xf(p[j],p[j+1]);pts.push(q[0],q[1]);cnt++;"
"        }"
"        segs.push(cnt);any=true;"
"      }"
"      if (!any) return;"
"      send({op:'cpath',pts:pts,segs:segs});"
"      st.clips++;"
"    };"
"    api.fillRect=function(x,y,w,h){fillRectImpl(x,y,w,h);};"
"    api.strokeRect=function(x,y,w,h){"
"      var p=rectPts(x,y,w,h),t=[];"
"      for (var i=0;i<8;i+=2){var q=xf(p[i],p[i+1]);t.push(q[0],q[1]);}"
"      t.push(t[0],t[1]);"
"      send({op:'spath',pts:t,col:rgbaGA(solidOf(st.strokeSrc)),w:st.lw*detScale(),"
"           cap:(st.cap==='round')});"
"    };"
"    api.fillText=function(s,x,y){textOp(s,x,y,st.fillSrc);};"
"    api.strokeText=function(s,x,y){textOp(s,x,y,st.strokeSrc);};"
"    api.measureText=function(s){return {width:measureW(String(s))};};"
"    api.createLinearGradient=function(x0,y0,x1,y1){"
"      return {__hngrad:true,x0:+x0||0,y0:+y0||0,x1:+x1||0,y1:+y1||0,stops:[],"
"        addColorStop:function(t,color){this.stops.push({t:+t||0,c:parseColor(color)});}};"
"    };"
"    api.createRadialGradient=function(x0,y0,r0,x1,y1,r1){"
"      return {__hngrad:true,radial:true,x0:+x0||0,y0:+y0||0,r0:+r0||0,"
"              x1:+x1||0,y1:+y1||0,r1:+r1||0,stops:[],"
"        addColorStop:function(t,color){this.stops.push({t:+t||0,c:parseColor(color)});}};"
"    };"
"    api.setLineDash=function(a){"
"      if(!a||!a.length){st.dash=[];return;}"
"      var d=[];for(var i=0;i<a.length;i++)d.push(+a[i]||0);"
"      st.dash=d;"
"    };"
"    api.getLineDash=function(){return st.dash.slice();};"
"    api.clearRect=function(x,y,w,h){"
"      if (w===undefined){send({op:'clear'});return;}"
"      var m=st.m,q=xf(x,y),tw=m[0]*w,th=m[3]*h;"
"      if (tw<0){q[0]+=tw;tw=-tw;}"
"      if (th<0){q[1]+=th;th=-th;}"
"      send({op:'clear_rect',x:q[0],y:q[1],w:tw,h:th});"
"    };"
"    /* drawImage: 3 参(img,x,y)→ 100x100 缺省(旧口径), 5 参(img,x,y,w,h)"
"       拉伸, 9 参(img, sx,sy,sw,sh, dx,dy,dw,dh)源子矩形裁贴 —— 协议"
"       image op 带 sx/sy/sw/sh(源图像素坐标), 引擎绘制期直接裁拷。 */"
"    api.drawImage=function(img,a2,a3,a4,a5,a6,a7,a8,a9){"
"      var src=(typeof img==='string')?img:(img&&img.src?img.src:'');"
"      if (!src) return;"
"      var m=st.m;"
"      if (arguments.length>=9){"
"        var q=xf(a6,a7);"
"        send({op:'image',src:src,x:q[0],y:q[1],w:a8*m[0],h:a9*m[3],"
"              sx:a2,sy:a3,sw:a4,sh:a5});"
"        return;"
"      }"
"      var q2=xf(a2,a3);"
"      var dw=(a4===undefined)?100:a4,dh=(a5===undefined)?100:a5;"
"      send({op:'image',src:src,x:q2[0],y:q2[1],w:dw*m[0],h:dh*m[3]});"
"    };"
"    /* createPattern(image, repetition): JS 侧标记对象(与渐变对象同构)。"
"       image = 图片路径字符串(引擎绘制期经解码缓存取像素)。 */"
"    api.createPattern=function(img,rep){"
"      var src=(typeof img==='string')?img:(img&&img.src?img.src:'');"
"      if (!src) return null;"
"      var r=String(rep||'repeat');"
"      if (r!=='repeat'&&r!=='repeat-x'&&r!=='repeat-y'&&r!=='no-repeat') r='repeat';"
"      return {__hnpat:true,src:src,rep:r};"
"    };"
"    /* getImageData(x,y,w,h) → {width,height,data:Uint8Array}(非预乘 RGBA;"
"       账本离屏栅格化, 越界补透明黑)。w/h<=0 返回 null(标准是抛"
"       IndexSizeError, glue 纪律是不抛)。 */"
"    api.getImageData=function(x,y,w,h){"
"      x=Math.round(+x||0);y=Math.round(+y||0);w=Math.round(+w||0);h=Math.round(+h||0);"
"      if (w<=0||h<=0) return null;"
"      var d=__hnCanvasPixels(cvId,x,y,w,h);"
"      if (!d) return null;"
"      return {width:w,height:h,data:new Uint8Array(d)};"
"    };"
"    /* putImageData(im, dx, dy[, dirtyX, dirtyY, dirtyW, dirtyH]): im 为"
"       getImageData 的返回(或 {width,height,data} 同构对象)。像素按"
"       RGBA 数组下送 pdata op(引擎侧 4MB 单块上限); 脏矩形 4 参齐备才启用。 */"
"    api.putImageData=function(im,x,y,dx,dy,dw2,dh2){"
"      if (!im||!im.data||!(im.width>0)||!(im.height>0)) return;"
"      var n=im.width*im.height*4,px=new Array(n);"
"      for (var i=0;i<n;i++) px[i]=im.data[i]|0;"
"      var o={op:'pdata',x:+x||0,y:+y||0,w:im.width,h:im.height,px:px};"
"      if (dx!==undefined&&dy!==undefined){"
"        o.dx=+dx||0;o.dy=+dy||0;"
"        o.dw=(dw2===undefined)?im.width:+dw2;"
"        o.dh=(dh2===undefined)?im.height:+dh2;"
"      }"
"      send(o);"
"    };"
"    api.rect=function(x,y,w,h){"
"      var p=rectPts(x,y,w,h),t=[];"
"      for (var i=0;i<8;i+=2){var q=xf(p[i],p[i+1]);t.push(q[0],q[1]);}"
"      t.push(t[0],t[1]);"
"      subs.push({p:t,closed:true});   /* 闭子路径进当前路径(fill/stroke/clip 消费) */"
"    };"
"    api.roundRect=function(x,y,w,h,r){"
"      var rr=(typeof r==='number')?[r]:Array.isArray(r)?r:[0];"
"      function cl(v){var m=Math.min(w,h)/2;return Math.max(0,Math.min(+v||0,m));}"
"      var r0=cl(rr[0]),r1=cl(rr[1]!==undefined?rr[1]:rr[0]),"
"          r2=cl(rr[2]!==undefined?rr[2]:rr[0]),r3=cl(rr[3]!==undefined?rr[3]:rr[1]!==undefined?rr[1]:rr[0]);"
"      api.moveTo(x+r0,y);"
"      api.lineTo(x+w-r1,y); api.arcTo(x+w,y,x+w,y+r1,r1);"
"      api.lineTo(x+w,y+h-r2); api.arcTo(x+w,y+h,x+w-r2,y+h,r2);"
"      api.lineTo(x+r3,y+h); api.arcTo(x,y+h,x,y+h-r3,r3);"
"      api.lineTo(x,y+r0); api.arcTo(x,y,x+r0,y,r0);"
"      api.closePath();"
"    };"
"    api.getTransform=function(){"
"      var m=st.m;return {a:m[0],b:m[1],c:m[2],d:m[3],e:m[4],f:m[5]};"
"    };"
"    api.createImageData=function(a,b){"
"      var w2,h2;"
"      if (typeof a==='object'&&a){w2=a.width;h2=a.height;}"
"      else {w2=+a||0;h2=+b||0;}"
"      if (w2<1||h2<1)return null;"
"      return {width:w2,height:h2,data:new Uint8ClampedArray(w2*h2*4)};"
"    };"
"    api.isPointInPath=function(px,py){"
"      /* 诚实近似: 取该点已渲染像素的 alpha>0(离屏栅格化账本; 命中按像素,"
"         与路径数学命中在抗锯齿边缘可有 1px 级差异 —— 注释即契约)。 */"
"      var im=api.getImageData(+px|0,+py|0,1,1);"
"      return !!(im&&im.data&&im.data[3]>0);"
"    };"
"    api.toDataURL=function(type){"
"      /* 口径: 引擎统一出 PNG(type 参数收下不区分, 注释即契约)。 */"
"      try{return __hnCanvasPng(cvId)||null;}catch(e){return null;}"
"    };"
"    Object.defineProperty(api,'globalAlpha',{"
"      get:function(){return st.ga;},"
"      set:function(v){v=+v;if(!(v>=0))v=0;if(v>1)v=1;st.ga=v;}});"
"    Object.defineProperty(api,'lineJoin',{"
"      get:function(){return st.join;},"
"      set:function(v){if(v)st.join=String(v);}});"
"    Object.defineProperty(api,'miterLimit',{"
"      get:function(){return st.ml;},"
"      set:function(v){var n=+v;if(n>=1)st.ml=n;}});"
"    Object.defineProperty(api,'letterSpacing',{"
"      get:function(){return st.lsp+'px';},"
"      set:function(v){var n=parseFloat(String(v));st.lsp=isFinite(n)?n:0;}});"
"    Object.defineProperty(api,'wordSpacing',{"
"      get:function(){return st.wsp+'px';},"
"      set:function(v){var n=parseFloat(String(v));st.wsp=isFinite(n)?n:0;}});"
"    Object.defineProperty(api,'filter',{"
"      get:function(){return st.flt>0?('blur('+st.flt+'px)'):'none';},"
"      set:function(v){"
"        var m2=/blur\\(([-0-9.]+)px\\)/.exec(String(v||''));"
"        st.flt=m2?Math.max(0,+m2[1]):0;}});"
"    api.reset=function(){"
"      /* 标准 reset(): 清画布 + 全部状态(含路径/栈)归缺省 */"
"      send({op:'clear'});"
"      st=defaultState();stack=[];subs=[];cur=null;"
"    };"
"    return api;"
"  }"
"})(globalThis);";

/* JS 运行时表(按 scope_key 隔离; daemon 的不同应用用不同 key)。
   bridge 槽随上下文持有(opaque 指向它), 每次 eval 按 doc/ctx 刷新;
   glue_done/timer_glue_done = 各 glue 已装入(每 scope 只装一次)。 */
#define JS_MAX_SCOPES 32
static struct { char key[128]; JSRuntime *rt; JSContext *ctx; js_bridge bridge;
                int glue_done, timer_glue_done, event_glue_done; } js_scopes[JS_MAX_SCOPES];
static int js_scope_n = 0;

static JSContext *js_scope_get(const char *key) {
    for (int i = 0; i < js_scope_n; i++)
        if (!strcmp(js_scopes[i].key, key)) return js_scopes[i].ctx;
    if (js_scope_n >= JS_MAX_SCOPES) return NULL;
    JSRuntime *rt = JS_NewRuntime();
    if (!rt) return NULL;
    JSContext *ctx = JS_NewContext(rt);
    if (!ctx) { JS_FreeRuntime(rt); return NULL; }
    snprintf(js_scopes[js_scope_n].key, sizeof(js_scopes[0].key), "%s", key);
    js_scopes[js_scope_n].rt = rt;
    js_scopes[js_scope_n].ctx = ctx;
    memset(&js_scopes[js_scope_n].bridge, 0, sizeof(js_bridge));
    js_scopes[js_scope_n].glue_done = 0;
    js_scopes[js_scope_n].timer_glue_done = 0;
    js_scopes[js_scope_n].event_glue_done = 0;
    JS_SetContextOpaque(ctx, &js_scopes[js_scope_n].bridge);
    js_scope_n++;
    return ctx;
}

/* 只查找不创建(hn_rt_frame 驱动定时器用 —— 帧路径不允许副作用建表) */
static JSContext *js_scope_find(const char *key) {
    for (int i = 0; i < js_scope_n; i++)
        if (!strcmp(js_scopes[i].key, key)) return js_scopes[i].ctx;
    return NULL;
}

/* rt ↔ scope 绑定: 桥函数(setTimeout 等)只有 JSContext*, 经 opaque 的
   js_bridge.rt 反查到挂定时器表的 rt。hn_rt_open 绑定 / hn_rt_close 解绑;
   同 key 的旧 rt 被新 rt 覆盖(后者接管此后注册的定时器 —— 每窗一进程,
   同 key 双活不是真实场景)。 */
static void js_scope_bind_rt(hn_rt *rt) {
    if (!rt) return;
    JSContext *ctx = js_scope_get(rt->id);   /* 惰性建 scope(与文档脚本同一上下文) */
    if (!ctx) return;
    for (int i = 0; i < js_scope_n; i++)
        if (js_scopes[i].ctx == ctx) { js_scopes[i].bridge.rt = rt; return; }
}

static void js_scope_unbind_rt(hn_rt *rt) {
    if (!rt) return;
    for (int i = 0; i < js_scope_n; i++)
        if (js_scopes[i].bridge.rt == rt) js_scopes[i].bridge.rt = NULL;
}

/* 释放 rt 定时器/RAF/监听器表持有的全部 JSValue 引用(hn_rt_close 调;
   必须在 JS 上下文消亡前 —— 上下文属 js_scopes 全局表, 生命周期长于任何 rt)。 */
static void js_rt_release_timers(hn_rt *rt) {
    JSContext *ctx = js_scope_find(rt->id);
    if (ctx) {
        for (int i = 0; i < 64; i++) {
            if (!rt->timers[i].id) continue;
            JS_FreeValue(ctx, rt->timers[i].func);
            if (!JS_IsUndefined(rt->timers[i].args)) JS_FreeValue(ctx, rt->timers[i].args);
            rt->timers[i].id = 0;
        }
        for (int i = 0; i < rt->raf_n; i++) JS_FreeValue(ctx, rt->raf_q[i].cb);
        /* 顺手 GC: 回调闭包可能成环(自引用), 引用清零后让环可回收 */
        for (int i = 0; i < js_scope_n; i++)
            if (!strcmp(js_scopes[i].key, rt->id)) JS_RunGC(js_scopes[i].rt);
    }
    rt->timer_n = 0;
    rt->raf_n = 0;
    js_listeners_clear(rt);   /* 监听器表同批归还(引用同属 scope 上下文) */
}

/* ---------------- JS 定时器与 RAF(帧驱动) ----------------
 * QuickJS 没有后台事件线程 —— 标准定时语义由 hn_rt_frame 每帧驱动:
 *   setTimeout/setInterval: 到期即调(hn_rt_frame 开头, 每帧一次扫描);
 *   requestAnimationFrame: 下一帧开头执行一次(注册序, 不做时间对齐)。
 * 时钟是 rt 的虚拟单调毫秒(clock_ms += dt): 真窗壳喂真实间隔 ⇒ 与单调钟
 * 一致; 探针喂固定 16 ⇒ "3 帧×16ms 不触发, 第 4 帧触发"可确定性断言。
 * 纪律(帧驱动定时器的通用约束):
 *   - 单帧最多 8 个回调(RAF+到期定时器合计), 超出顺延下一帧 —— 回调
 *     再狂也不饿死主循环;
 *   - 单次定时器先摘槽再调用(回调里 clearTimeout 自身安全), 调用前
 *     DupValue(回调里 clearInterval 自身安全);
 *   - 回调异常吞掉打 stderr(与 hn_rt_eval 同口径), 不向上传播;
 *   - 跑过回调即按"可能改了画面"处理: rt_render + repaint 置位, 桥改动
 *     (hnSetText/hnCanvas2D)经既有管线落像素。
 * C 原生走 __hn* 内部名, 标准签名由 TIMER_GLUE_JS 转接(可变参收敛为
 * 参数数组)。无宿主 rt 的 eval(daemon 裸 scope)注册定时器返回 0 ——
 * 定时器是帧驱动的, 帧属于 rt, 没有宿主就没有时钟。 */

/* __hnTimer(fn, ms, argsArray, periodic) → 句柄(>0; 表满/无效 0) */
static JSValue js_hn_timer(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    hn_rt *rt = b ? b->rt : NULL;
    if (!rt || argc < 2 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
    int slot = -1;
    for (int i = 0; i < 64; i++)
        if (!rt->timers[i].id) { slot = i; break; }
    if (slot < 0) return JS_NewInt32(ctx, 0);          /* 表满: 句柄 0 */
    double ms = 0;
    JS_ToFloat64(ctx, &ms, argv[1]);
    if (!(ms >= 0)) ms = 0;                            /* NaN/负数 → 0(浏览器同义) */
    int periodic = 0;
    if (argc > 3) { int32_t p = 0; JS_ToInt32(ctx, &p, argv[3]); periodic = p != 0; }
    int id = ++rt->timer_seq;
    rt->timers[slot].id = id;
    rt->timers[slot].due = rt->clock_ms + ms;
    rt->timers[slot].interval_ms = periodic ? ms : 0.0;
    rt->timers[slot].func = JS_DupValue(ctx, argv[0]);
    rt->timers[slot].args = (argc > 2 && JS_IsArray(ctx, argv[2]))
                          ? JS_DupValue(ctx, argv[2]) : JS_UNDEFINED;
    rt->timer_n++;
    return JS_NewInt32(ctx, id);
}

/* __hnTimerClear(id): clearTimeout/clearInterval 共用(同一张表, 浏览器
   也允许互换) */
static JSValue js_hn_timer_clear(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    hn_rt *rt = b ? b->rt : NULL;
    if (!rt || argc < 1) return JS_UNDEFINED;
    int32_t id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    if (id <= 0) return JS_UNDEFINED;                  /* 失效句柄: 静默 */
    for (int i = 0; i < 64; i++) {
        if (rt->timers[i].id != id) continue;
        JS_FreeValue(ctx, rt->timers[i].func);
        if (!JS_IsUndefined(rt->timers[i].args)) JS_FreeValue(ctx, rt->timers[i].args);
        rt->timers[i].func = JS_UNDEFINED;
        rt->timers[i].args = JS_UNDEFINED;
        rt->timers[i].id = 0;
        rt->timer_n--;
        break;
    }
    return JS_UNDEFINED;
}

/* __hnRAF(cb) → 句柄; __hnRAFCancel(id)。挂进下一帧队列(本帧回调里
   再挂的落到队尾 = 下下帧, 天然防重入) */
static JSValue js_hn_raf(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    hn_rt *rt = b ? b->rt : NULL;
    if (!rt || argc < 1 || !JS_IsFunction(ctx, argv[0])) return JS_NewInt32(ctx, 0);
    if (rt->raf_n >= 64) return JS_NewInt32(ctx, 0);   /* 队满: 丢弃 */
    int id = ++rt->raf_seq;
    rt->raf_q[rt->raf_n].id = id;
    rt->raf_q[rt->raf_n].cb = JS_DupValue(ctx, argv[0]);
    rt->raf_n++;
    return JS_NewInt32(ctx, id);
}

static JSValue js_hn_raf_cancel(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    hn_rt *rt = b ? b->rt : NULL;
    if (!rt || argc < 1) return JS_UNDEFINED;
    int32_t id = 0;
    JS_ToInt32(ctx, &id, argv[0]);
    if (id <= 0) return JS_UNDEFINED;
    for (int i = 0; i < rt->raf_n; i++) {
        if (rt->raf_q[i].id != id) continue;
        JS_FreeValue(ctx, rt->raf_q[i].cb);
        rt->raf_q[i].cb = JS_UNDEFINED;
        memmove(&rt->raf_q[i], &rt->raf_q[i + 1],
                (size_t)(rt->raf_n - i - 1) * sizeof(rt->raf_q[0]));   /* 保注册序 */
        rt->raf_n--;
        break;
    }
    return JS_UNDEFINED;
}

/* ---------------- JS 事件层(addEventListener → 命中链冒泡派发) ----------------
 * 注册表在 rt 上({target id 串, type 串, fn, capture}, 上限 256), C 原生
 * __hn* 桥登记/摘除, 标准签名由 EVENT_GLUE_JS 转接(元素方法形态
 * el.addEventListener / document.addEventListener / window.addEventListener,
 * 另给元素对象挂 textContent/value 读写)。派发在 C 侧(rt_js_dispatch):
 * 引擎输入事件(点击/按键/输入/滚动/移动)按命中链(目标→祖先 id 链, 链尾补
 * document/window)冒泡序调监听器, 事件对象由 glue 构造(preventDefault/
 * stopPropagation 在对象上置标志字段), 派发后 C 读标志 —— _pd 返回给调用
 * 点跳过默认行为, _sp 截断剩余链路。
 * 无宿主 rt 的 eval(daemon 裸 scope)注册监听器返回 0 —— 事件由输入路径
 * 派发, 输入属于 rt, 没有宿主就没有派发点(与定时器同一纪律)。 */

/* __hnAddListener(target, type, fn, capture) → 句柄(>0; 表满/非函数 0) */
static JSValue js_hn_add_listener(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    hn_rt *rt = b ? b->rt : NULL;
    if (!rt || argc < 3 || !JS_IsFunction(ctx, argv[2])) return JS_NewInt32(ctx, 0);
    const char *target = JS_ToCString(ctx, argv[0]);
    const char *type = JS_ToCString(ctx, argv[1]);
    if (!target || !type || !target[0] || !type[0]) {
        if (target) JS_FreeCString(ctx, target);
        if (type) JS_FreeCString(ctx, type);
        return JS_NewInt32(ctx, 0);
    }
    int slot = -1;
    for (int i = 0; i < HN_LISTENERS_MAX; i++)
        if (!rt->listeners[i].id) { slot = i; break; }
    if (slot >= 0) {
        int32_t cap = 0;
        if (argc > 3) JS_ToInt32(ctx, &cap, argv[3]);
        int id = ++rt->listen_seq;
        rt->listeners[slot].id = id;
        snprintf(rt->listeners[slot].target, sizeof(rt->listeners[slot].target), "%s", target);
        snprintf(rt->listeners[slot].type, sizeof(rt->listeners[slot].type), "%s", type);
        rt->listeners[slot].capture = cap != 0;
        rt->listeners[slot].fn = JS_DupValue(ctx, argv[2]);
        rt->listen_n++;
    }   /* 表满: 句柄 0(与定时器表同口径) */
    JS_FreeCString(ctx, target);
    JS_FreeCString(ctx, type);
    return JS_NewInt32(ctx, slot >= 0 ? rt->listeners[slot].id : 0);
}

/* __hnRemoveListener(target, type, fn): 按函数同一性(JS_SameValue)摘除
   全部匹配(目标+类型+同一函数引用; 与浏览器 removeEventListener 语义对齐) */
static JSValue js_hn_remove_listener(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    hn_rt *rt = b ? b->rt : NULL;
    if (!rt || argc < 3) return JS_UNDEFINED;
    const char *target = JS_ToCString(ctx, argv[0]);
    const char *type = JS_ToCString(ctx, argv[1]);
    if (target && type) {
        for (int i = 0; i < HN_LISTENERS_MAX; i++) {
            if (!rt->listeners[i].id) continue;
            if (strcmp(rt->listeners[i].target, target) || strcmp(rt->listeners[i].type, type))
                continue;
            if (!JS_SameValue(ctx, rt->listeners[i].fn, argv[2])) continue;
            JS_FreeValue(ctx, rt->listeners[i].fn);
            rt->listeners[i].fn = JS_UNDEFINED;
            rt->listeners[i].id = 0;
            rt->listen_n--;
        }
    }
    if (target) JS_FreeCString(ctx, target);
    if (type) JS_FreeCString(ctx, type);
    return JS_UNDEFINED;
}

/* __hnGetValue(id) → 控件当前值(元素包装 value 属性的读端; 找不到 id = 空串) */
static JSValue js_hn_get_value(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    if (argc < 1 || !b || !b->doc) return JS_NewString(ctx, "");
    const char *id = JS_ToCString(ctx, argv[0]);
    hn_node *n = id ? hn_doc_find_by_id(b->doc, id) : NULL;
    size_t vl = 0;
    const char *v = n ? hn_node_value(n, &vl) : NULL;
    JSValue r = JS_NewStringLen(ctx, v && vl ? v : "", v ? vl : 0);
    if (id) JS_FreeCString(ctx, id);
    return r;
}

/* ---------------- DOM 标准面桥(getAttribute/querySelector/树操作) ----------------
 * 全部按 id 寻址(与 hnSetText 同一寻址纪律); 失败返回 null/false/空,
 * 不抛异常 —— 语法错(querySelector)由 glue 侧映射为异常。 */

static hn_node *bridge_node(JSContext *ctx, int argc, JSValueConst *argv, int idx) {
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    if (!b || !b->doc || argc <= idx) return NULL;
    const char *id = JS_ToCString(ctx, argv[idx]);
    hn_node *n = id ? hn_doc_find_by_id(b->doc, id) : NULL;
    if (id) JS_FreeCString(ctx, id);
    return n;
}

static JSValue js_hn_get_attr(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    hn_node *n = bridge_node(ctx, argc, argv, 0);
    if (!n || argc < 2 || !JS_IsString(argv[1])) return JS_NULL;
    const char *name = JS_ToCString(ctx, argv[1]);
    const char *v = name ? hn_node_attr(n, name) : NULL;
    JSValue r = v ? JS_NewString(ctx, v) : JS_NULL;
    if (name) JS_FreeCString(ctx, name);
    return r;
}

static JSValue js_hn_set_attr(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    hn_node *n = bridge_node(ctx, argc, argv, 0);
    if (!n || argc < 3 || !JS_IsString(argv[1])) return JS_FALSE;
    const char *name = JS_ToCString(ctx, argv[1]);
    const char *val = argc >= 3 && JS_IsString(argv[2]) ? JS_ToCString(ctx, argv[2]) : "";
    int ok = (name && hn_node_set_attr(n, name, val ? val : ""));
    if (name) JS_FreeCString(ctx, name);
    if (val) JS_FreeCString(ctx, val);
    return ok ? JS_TRUE : JS_FALSE;
}

static JSValue js_hn_remove_attr(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    hn_node *n = bridge_node(ctx, argc, argv, 0);
    if (!n || argc < 2 || !JS_IsString(argv[1])) return JS_FALSE;
    const char *name = JS_ToCString(ctx, argv[1]);
    int ok = (name && hn_node_remove_attr(n, name));
    if (name) JS_FreeCString(ctx, name);
    return ok ? JS_TRUE : JS_FALSE;
}

static JSValue js_hn_has_attr(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    hn_node *n = bridge_node(ctx, argc, argv, 0);
    if (!n || argc < 2 || !JS_IsString(argv[1])) return JS_FALSE;
    const char *name = JS_ToCString(ctx, argv[1]);
    const char *v = name ? hn_node_attr(n, name) : NULL;
    if (name) JS_FreeCString(ctx, name);
    return v ? JS_TRUE : JS_FALSE;
}

/* 属性名枚举(dataset 的 data-* 前缀扫描要用) */
static JSValue js_hn_attr_names(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    hn_node *n = bridge_node(ctx, argc, argv, 0);
    JSValue arr = JS_NewArray(ctx);
    if (!n) return arr;
    int k = 0;
    for (int i = 0; i < n->n_attrs; i++) {
        JSValue s = JS_NewString(ctx, n->attrs[i].name);
        JS_SetPropertyUint32(ctx, arr, (uint32_t)k++, s);
    }
    return arr;
}

static JSValue js_hn_get_text(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    hn_node *n = bridge_node(ctx, argc, argv, 0);
    if (!n) return JS_NewString(ctx, "");
    char buf[8192];
    size_t used = hn_node_text_content(n, buf, sizeof(buf) - 1);
    return JS_NewStringLen(ctx, buf, used);
}

static JSValue js_hn_tag(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    hn_node *n = bridge_node(ctx, argc, argv, 0);
    return JS_NewString(ctx, (n && n->tag) ? n->tag : "");
}


static JSValue js_hn_append(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    hn_node *parent = bridge_node(ctx, argc, argv, 0);
    hn_node *child = bridge_node(ctx, argc, argv, 1);
    if (!parent || !child || child == parent || !child->parent) return JS_FALSE;
    if (child->parent == parent) return JS_TRUE;           /* 幂等 */
    if (!hn_node_remove_child(child->parent, child)) return JS_FALSE;
    /* 挂到新父尾部(remove_child 已断链, 复用引擎 append 链接语义) */
    child->parent = parent;
    child->next = NULL;
    child->prev = parent->last;
    if (parent->last) { parent->last->next = child; parent->last = child; }
    else { parent->first = parent->last = child; }
    child->anim.fresh = 1;                                 /* 新位置播入场动画 */
    return JS_TRUE;
}

static JSValue js_hn_detach(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    hn_node *g = bridge_node(ctx, argc, argv, 0);
    if (b && b->ctx && g) hn_context_push_exit_ghost(b->ctx, g);   /* 原位淡出 */
    hn_node *n = bridge_node(ctx, argc, argv, 0);
    if (!n || !n->parent) return JS_FALSE;
    return hn_node_remove_child(n->parent, n) ? JS_TRUE : JS_FALSE;
}

/* __hnGetRect(id) → "{x,y,w,h}"(border-box 绝对坐标, CSS px)
 * getBoundingClientRect/offset* 的原生数据出口。缺节点 → null。 */
static JSValue js_hn_get_rect(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    hn_node *n = bridge_node(ctx, argc, argv, 0);
    if (!n) return JS_NULL;
    char buf[128];
    snprintf(buf, sizeof(buf),
             "{\"x\":%.1f,\"y\":%.1f,\"width\":%.1f,\"height\":%.1f}",
             (double)n->bx, (double)n->by, (double)n->bw, (double)n->bh);
    return JS_NewString(ctx, buf);
}

/* __hnScrollInfo(id) → [x, y, maxX, maxY]; __hnScrollTo(id, x, y) 置位。
 * 引擎口是 scroll_by(增量), 置位 = 差量换算(当前值 + 钳制范围内)。 */
/* __hnInsertBefore(parentId, childId, refId|null): 移动语义(与 append 同)。 */
/* __hnDispatch(id, type, detail): 合成事件派发(与平台输入同一入口
   rt_js_dispatch → 冒泡链 → JS 监听器)。返回是否有监听器消费。 */
static JSValue js_hn_dispatch(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    hn_node *n = bridge_node(ctx, argc, argv, 0);
    if (!b || !b->rt || !n || argc < 2 || !JS_IsString(argv[1])) return JS_FALSE;
    const char *type = JS_ToCString(ctx, argv[1]);
    if (!type || !type[0]) { if (type) JS_FreeCString(ctx, type); return JS_FALSE; }
    double detail = 0;
    if (argc >= 3) JS_ToFloat64(ctx, &detail, argv[2]);
    int handled = rt_js_dispatch(b->rt, type, n, 0, 0, NULL, 0, (int)detail);
    JS_FreeCString(ctx, type);
    return handled ? JS_TRUE : JS_FALSE;
}

/* __hnViewport() → [w, h, scale](innerWidth/innerHeight/devicePixelRatio) */
static JSValue js_hn_viewport(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    JSValue arr = JS_NewArray(ctx);
    float w = 0, h = 0, sc = 1;
    if (b && b->rt) {
        w = (float)b->rt->w;
        h = (float)b->rt->h;
        sc = rt_scale(b->rt);
    }
    JSValue vs[3] = { JS_NewFloat64(ctx, w), JS_NewFloat64(ctx, h), JS_NewFloat64(ctx, sc) };
    for (int i = 0; i < 3; i++) JS_SetPropertyUint32(ctx, arr, (uint32_t)i, vs[i]);
    return arr;
}

static JSValue js_hn_exists(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    if (!b || !b->doc || argc < 1) return JS_FALSE;
    const char *id = JS_ToCString(ctx, argv[0]);
    hn_node *n = id ? hn_doc_find_by_id(b->doc, id) : NULL;
    if (id) JS_FreeCString(ctx, id);
    return n ? JS_TRUE : JS_FALSE;
}

static JSValue js_hn_focus(JSContext *ctx, JSValueConst _, int argc, JSValueConst *argv) {
    js_bridge *b = (js_bridge *)JS_GetContextOpaque(ctx);
    hn_node *n = bridge_node(ctx, argc, argv, 0);
    if (!b || !b->ctx || !n) return JS_FALSE;
    hn_context_set_focus(b->ctx, n);
    return JS_TRUE;
}

/* 释放监听器表持有的全部 JSValue 引用(hn_rt_close / 文档换装调用)。
   必须在 JS 上下文消亡前 —— 上下文属 js_scopes 全局表。换装时旧元素 id
   随旧文档消失, 监听器一并清空(文档脚本重跑会重新注册)。 */
static void js_listeners_clear(hn_rt *rt) {
    JSContext *ctx = js_scope_find(rt->id);
    if (ctx) {
        for (int i = 0; i < HN_LISTENERS_MAX; i++) {
            if (!rt->listeners[i].id) continue;
            JS_FreeValue(ctx, rt->listeners[i].fn);
            rt->listeners[i].fn = JS_UNDEFINED;
            rt->listeners[i].id = 0;
        }
        /* 顺手 GC: 回调闭包可能成环(自引用), 引用清零后让环可回收
           (与 js_rt_release_timers 同一手势; GC 走 scope 的 runtime) */
        for (int i = 0; i < js_scope_n; i++)
            if (!strcmp(js_scopes[i].key, rt->id)) JS_RunGC(js_scopes[i].rt);
    }
    rt->listen_n = 0;
}

/* 表里是否存在该类型的监听器(派发的快速路径: 无监听器零成本早退 ——
   无 JS 页面的输入行为逐位不变的锚) */
static int rt_js_has_listener(hn_rt *rt, const char *type) {
    if (!rt) return 0;
    for (int i = 0; i < HN_LISTENERS_MAX; i++) {
        if (!rt->listeners[i].id) continue;
        if (!strcmp(rt->listeners[i].type, type)) return 1;
    }
    return 0;
}

/* 键名映射: HN_KT_* → DOM 风格 key 串(可打印字符用 utf8 原文) */
static const char *kt_key_name(int key, const char *utf8) {
    switch (key) {
    case HN_KT_RETURN: case HN_KT_ENTER_NUMPAD: return "Enter";
    case HN_KT_ESCAPE: return "Escape";
    case HN_KT_TAB: return "Tab";
    case HN_KT_BACKSPACE: return "Backspace";
    case HN_KT_DELETE: return "Delete";
    case HN_KT_LEFT: return "ArrowLeft";
    case HN_KT_RIGHT: return "ArrowRight";
    case HN_KT_UP: return "ArrowUp";
    case HN_KT_DOWN: return "ArrowDown";
    case HN_KT_HOME: return "Home";
    case HN_KT_END: return "End";
    case HN_KT_PAGE_UP: return "PageUp";
    case HN_KT_PAGE_DOWN: return "PageDown";
    case HN_KT_SPACE: return " ";
    case HN_KT_F1: return "F1";
    case HN_KT_F2: return "F2";
    case HN_KT_F3: return "F3";
    case HN_KT_F4: return "F4";
    case HN_KT_F5: return "F5";
    case HN_KT_F6: return "F6";
    case HN_KT_F7: return "F7";
    case HN_KT_F8: return "F8";
    case HN_KT_F9: return "F9";
    case HN_KT_F10: return "F10";
    case HN_KT_F11: return "F11";
    case HN_KT_F12: return "F12";
    default: return (utf8 && *utf8) ? utf8 : "";
    }
}

/* code 串: DOM KeyboardEvent.code 风格(KeyA/Digit3/Space/Enter/ArrowLeft/…;
   无法识别的键 "Unidentified")。修饰键: 平台壳当前不向 hn_rt_key 传 mods,
   shift 由可打印大写字母推断(其余恒 false —— 事件对象字段齐备, 管道后补) */
static void kt_code_name(int key, const char *utf8, char *out, size_t cap) {
    const char *special = NULL;
    switch (key) {
    case HN_KT_RETURN: case HN_KT_ENTER_NUMPAD: special = "Enter"; break;
    case HN_KT_ESCAPE: special = "Escape"; break;
    case HN_KT_TAB: special = "Tab"; break;
    case HN_KT_BACKSPACE: special = "Backspace"; break;
    case HN_KT_DELETE: special = "Delete"; break;
    case HN_KT_LEFT: special = "ArrowLeft"; break;
    case HN_KT_RIGHT: special = "ArrowRight"; break;
    case HN_KT_UP: special = "ArrowUp"; break;
    case HN_KT_DOWN: special = "ArrowDown"; break;
    case HN_KT_HOME: special = "Home"; break;
    case HN_KT_END: special = "End"; break;
    case HN_KT_PAGE_UP: special = "PageUp"; break;
    case HN_KT_PAGE_DOWN: special = "PageDown"; break;
    case HN_KT_SPACE: special = "Space"; break;
    case HN_KT_F1: special = "F1"; break;
    case HN_KT_F2: special = "F2"; break;
    case HN_KT_F3: special = "F3"; break;
    case HN_KT_F4: special = "F4"; break;
    case HN_KT_F5: special = "F5"; break;
    case HN_KT_F6: special = "F6"; break;
    case HN_KT_F7: special = "F7"; break;
    case HN_KT_F8: special = "F8"; break;
    case HN_KT_F9: special = "F9"; break;
    case HN_KT_F10: special = "F10"; break;
    case HN_KT_F11: special = "F11"; break;
    case HN_KT_F12: special = "F12"; break;
    default: break;
    }
    if (special) { snprintf(out, cap, "%s", special); return; }
    if (utf8 && utf8[0] && utf8[1] == 0) {
        unsigned char c = (unsigned char)utf8[0];
        if (c >= 'a' && c <= 'z') { snprintf(out, cap, "Key%c", c - ('a' - 'A')); return; }
        if (c >= 'A' && c <= 'Z') { snprintf(out, cap, "Key%c", c); return; }
        if (c >= '0' && c <= '9') { snprintf(out, cap, "Digit%c", c); return; }
    }
    snprintf(out, cap, "Unidentified");
}

/* 事件对象标志读取(_pd/_sp; glue 的 preventDefault/stopPropagation 置位) */
static int rt_ev_flag(JSContext *ctx, JSValue ev, const char *name) {
    JSValue v = JS_GetPropertyStr(ctx, ev, name);
    int b = JS_ToBool(ctx, v);   /* 非布尔/缺失 → 0 */
    JS_FreeValue(ctx, v);
    return b == 1;
}

/* 派发: type 事件沿命中链冒泡。链 = 引擎 hn_event_path_*(目标→祖先, 只含
   有 id 的元素) + "document" + "window" 两级链尾。单相派发(v1): capture
   监听器也在冒泡序执行(注册表记录标志, 捕获阶段留给后续轮次)。同节点
   多监听器按注册序; stopPropagation 在当前节点全部监听器跑完后截断
   (DOM 语义: 不打断同层, 只停冒泡)。返回 preventDefault 与否。
   无监听器(快速路径)/无 JS 上下文/glue 未装载 → 0(零成本零回归)。 */
static int rt_js_dispatch(hn_rt *rt, const char *type, hn_node *target,
                          float x, float y, const char *utf8, int key, int detail) {
    if (!rt || !type) return 0;
    int is_click = strcmp(type, "click") == 0;
    if (!rt_js_has_listener(rt, type)) {
        /* 零监听器: 无 preventDefault 来源, 原生默认行为直接执行
           (纯声明式页面 —— details/checkbox/radio 无 JS 也工作) */
        if (is_click && target) return rt_native_click(rt, target, x);
        return 0;
    }
    JSContext *ctx = js_scope_find(rt->id);
    if (!ctx) return 0;
    /* 桥环境对准当前文档(hot reload 语义, 与 js_timers_drain 同一份) */
    hn_doc *doc = hn_context_doc(rt->ctx);
    for (int i = 0; i < js_scope_n; i++) {
        if (js_scopes[i].ctx == ctx) {
            js_scopes[i].bridge.doc = doc;
            js_scopes[i].bridge.ctx = rt->ctx;
            break;
        }
    }
    JSValue global = JS_GetGlobalObject(ctx);
    JSValue mk = JS_GetPropertyStr(ctx, global, "__hnMakeEvent");
    int pd = 0;
    if (JS_IsFunction(ctx, mk)) {
        /* 事件对象由 glue 构造(字段面 + preventDefault/stopPropagation) */
        JSValue t = JS_NewString(ctx, type);
        JSValue ev = JS_Call(ctx, mk, JS_UNDEFINED, 1, (JSValueConst *)&t);
        JS_FreeValue(ctx, t);
        if (!JS_IsException(ev)) {
            /* 命中链(冒泡序): id 串拷进栈缓冲 —— 派发期间回调可能改文档,
               不持节点属性指针。上限 32 个 id 节点 + document + window。 */
#define HN_EV_CHAIN_MAX 34
            char chain[HN_EV_CHAIN_MAX][64];
            int chain_n = 0;
            int plen = target ? hn_event_path_len(target) : 0;
            for (int i = 0; i < plen && chain_n < HN_EV_CHAIN_MAX - 2; i++) {
                hn_node *pn = hn_event_path_at(target, i);
                const char *pid = pn ? hn_node_attr(pn, "id") : NULL;
                if (pid && *pid) snprintf(chain[chain_n++], sizeof(chain[0]), "%s", pid);
            }
            snprintf(chain[chain_n++], sizeof(chain[0]), "document");
            snprintf(chain[chain_n++], sizeof(chain[0]), "window");
            JS_SetPropertyStr(ctx, ev, "target",
                              JS_NewString(ctx, chain_n > 2 ? chain[0] : ""));
            JS_SetPropertyStr(ctx, ev, "clientX", JS_NewFloat64(ctx, (double)x));
            JS_SetPropertyStr(ctx, ev, "clientY", JS_NewFloat64(ctx, (double)y));
            JS_SetPropertyStr(ctx, ev, "key", JS_NewString(ctx, kt_key_name(key, utf8)));
            char code[32];
            kt_code_name(key, utf8, code, sizeof(code));
            JS_SetPropertyStr(ctx, ev, "code", JS_NewString(ctx, code));
            JS_SetPropertyStr(ctx, ev, "detail", JS_NewInt32(ctx, detail));
            if (utf8 && utf8[0] && strncmp(type, "htmx:", 5) == 0)
                JS_SetPropertyStr(ctx, ev, "payload", JS_NewString(ctx, utf8));
            if (utf8 && utf8[0] && utf8[1] == 0 &&
                utf8[0] >= 'A' && utf8[0] <= 'Z')
                JS_SetPropertyStr(ctx, ev, "shift", JS_TRUE);
            /* 冒泡: 每节点先快照命中监听器(回调里可能 remove 改表), 再按
               注册序 Dup-调用-释放; 节点跑完读 _sp 决定截断。 */
            for (int ci = 0; ci < chain_n; ci++) {
                JS_SetPropertyStr(ctx, ev, "currentTarget", JS_NewString(ctx, chain[ci]));
                JSValue fns[16];
                int fn_n = 0;
                for (int i = 0; i < HN_LISTENERS_MAX && fn_n < 16; i++) {
                    if (!rt->listeners[i].id) continue;
                    if (strcmp(rt->listeners[i].type, type)) continue;
                    if (strcmp(rt->listeners[i].target, chain[ci])) continue;
                    fns[fn_n++] = JS_DupValue(ctx, rt->listeners[i].fn);
                }
                for (int i = 0; i < fn_n; i++) {
                    JSValue r = JS_Call(ctx, fns[i], JS_UNDEFINED, 1, (JSValueConst *)&ev);
                    if (JS_IsException(r)) {
                        JSValue e = JS_GetException(ctx);
                        const char *msg = JS_ToCString(ctx, e);
                        fprintf(stderr, "[event] %s\n", msg ? msg : "exception");
                        if (msg) JS_FreeCString(ctx, msg);
                        JS_FreeValue(ctx, e);
                    }
                    JS_FreeValue(ctx, r);
                    JS_FreeValue(ctx, fns[i]);
                }
                if (rt_ev_flag(ctx, ev, "_sp")) break;   /* 冒泡截断 */
            }
            pd = rt_ev_flag(ctx, ev, "_pd");
        } else {
            JS_FreeValue(ctx, JS_GetException(ctx));   /* 事件对象构造失败: 静默 */
        }
        JS_FreeValue(ctx, ev);
    }
    JS_FreeValue(ctx, mk);
    JS_FreeValue(ctx, global);
    /* preventDefault → 原生默认动作不执行; 否则执行(浏览器同序) */
    if (!pd && is_click && target)
        pd = rt_native_click(rt, target, x);
    return pd;
}


/* 统一回调调用: RAF 带时间戳(clock_ms, 仿 DOMHighResTimeStamp);
   定时器展开参数数组(上限 8 个, 多余丢弃 —— 最小版可变参)。异常吞掉
   打 stderr 不崩(与 hn_rt_eval 同一口径)。func/args 由调用方 Dup 过,
   生命周期覆盖本次调用。 */
static void js_invoke(JSContext *ctx, JSValue func, JSValue args,
                      int with_ts, double ts) {
    JSValue argv[8];
    int na = 0;
    if (with_ts) {
        argv[na++] = JS_NewFloat64(ctx, ts);
    } else if (!JS_IsUndefined(args)) {
        JSValue jlen = JS_GetPropertyStr(ctx, args, "length");
        int32_t len = 0;
        JS_ToInt32(ctx, &len, jlen);
        JS_FreeValue(ctx, jlen);
        if (len > 8) len = 8;
        for (; na < len; na++)
            argv[na] = JS_GetPropertyUint32(ctx, args, (uint32_t)na);
    }
    JSValue r = JS_Call(ctx, func, JS_UNDEFINED, na, argv);
    for (int i = 0; i < na; i++) JS_FreeValue(ctx, argv[i]);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, e);
        fprintf(stderr, "[timer] %s\n", msg ? msg : "exception");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, e);
    }
    JS_FreeValue(ctx, r);
}

/* hn_rt_frame 的回调驱动: RAF 队头 → 到期定时器, 合计 ≤8 个。只跑回调
   不渲染 —— 返回"是否跑过回调", 渲染由 hn_rt_frame 合并进帧尾一次。 */
static int js_timers_drain(hn_rt *rt, int *repaint) {
    JSContext *ctx = js_scope_find(rt->id);
    if (!ctx) return 0;
    JSRuntime *job_rt = NULL;
    for (int si = 0; si < js_scope_n; si++)
        if (js_scopes[si].ctx == ctx) { job_rt = js_scopes[si].rt; break; }
    if (!job_rt) return 0;
    int pending_ran = 0;
    for (int i = 0; i < 8; i++) {
        JSContext *job_ctx = ctx;
        int job_rc = JS_ExecutePendingJob(job_rt, &job_ctx);
        if (job_rc <= 0) break;
        pending_ran = 1;
    }
    if (rt->timer_n == 0 && rt->raf_n == 0) return pending_ran;
    /* 桥环境对准当前文档(hot reload 换 doc 后回调不悬空; 与 hn_rt_eval
       每次的刷新同一份语义)。rt 槽不动 —— 归属关系不经帧改写。 */
    hn_doc *doc = hn_context_doc(rt->ctx);
    for (int i = 0; i < js_scope_n; i++) {
        if (js_scopes[i].ctx == ctx) {
            js_scopes[i].bridge.doc = doc;
            js_scopes[i].bridge.ctx = rt->ctx;
            break;
        }
    }
    int budget = 8, ran = 0;

    /* RAF: 队头快照(注册序; 预算外留队, 下一帧优先)。先摘队再调用 ——
       回调里挂新 RAF 追加到队尾, 不会被本帧重入。 */
    int n = rt->raf_n < budget ? rt->raf_n : budget;
    JSValue cbs[8];
    for (int i = 0; i < n; i++) cbs[i] = JS_DupValue(ctx, rt->raf_q[i].cb);
    if (n < rt->raf_n)
        memmove(&rt->raf_q[0], &rt->raf_q[n],
                (size_t)(rt->raf_n - n) * sizeof(rt->raf_q[0]));
    rt->raf_n -= n;
    for (int i = 0; i < n; i++) {
        js_invoke(ctx, cbs[i], JS_UNDEFINED, 1, rt->clock_ms);
        JS_FreeValue(ctx, cbs[i]);
        budget--;
        ran = 1;
    }

    /* 到期定时器: 单趟扫描(各限一次)。单次触发先摘槽再调用; 周期触发
       due 顺延 interval(错过的帧不追发 —— 折叠为一次, 与浏览器掉帧语义
       一致)。回调里 clearTimeout/clearInterval 自身都安全(func 已 Dup,
       单次槽已摘)。 */
    double now = rt->clock_ms;
    for (int i = 0; i < 64 && budget > 0; i++) {
        if (!rt->timers[i].id || rt->timers[i].due > now) continue;
        JSValue func = JS_DupValue(ctx, rt->timers[i].func);
        JSValue args = JS_UNDEFINED;
        if (!JS_IsUndefined(rt->timers[i].args)) args = JS_DupValue(ctx, rt->timers[i].args);
        int once = (rt->timers[i].interval_ms <= 0);
        if (once) {
            JS_FreeValue(ctx, rt->timers[i].func);
            if (!JS_IsUndefined(rt->timers[i].args)) JS_FreeValue(ctx, rt->timers[i].args);
            rt->timers[i].func = JS_UNDEFINED;
            rt->timers[i].args = JS_UNDEFINED;
            rt->timers[i].id = 0;
            rt->timer_n--;
        } else {
            rt->timers[i].due = now + rt->timers[i].interval_ms;
        }
        js_invoke(ctx, func, args, 0, 0);
        JS_FreeValue(ctx, func);
        if (!JS_IsUndefined(args)) JS_FreeValue(ctx, args);
        budget--;
        ran = 1;
    }

    /* 不在此渲染: hn_rt_frame 按返回值把渲染合并进帧尾一次(同帧多次
       整渲染的浪费, 见 hn_rt_frame 帧头说明)。repaint 形参保留签名
       兼容(现由帧尾统一置位)。 */
    (void)repaint;
    return ran || pending_ran;
}

/* 定时器/RAF 标准签名 glue: __hn* 内部桥 → 浏览器同款全局函数。
   幂等(typeof 检查), 与 canvas glue 同一装载纪律(每 scope 一次)。 */
static const char *TIMER_GLUE_JS =
"(function(g){"
"  if (typeof g.setTimeout === 'function') return;"
"  g.setTimeout=function(fn,ms){"
"    if (typeof fn!=='function') return 0;"
"    return __hnTimer(fn,+ms||0,[].slice.call(arguments,2),0);};"
"  g.setInterval=function(fn,ms){"
"    if (typeof fn!=='function') return 0;"
"    return __hnTimer(fn,+ms||0,[].slice.call(arguments,2),1);};"
"  g.clearTimeout=function(id){__hnTimerClear(+id||0);};"
"  g.clearInterval=function(id){__hnTimerClear(+id||0);};"
"  g.requestAnimationFrame=function(cb){"
"    if (typeof cb!=='function') return 0;"
"    return __hnRAF(cb);};"
"  g.cancelAnimationFrame=function(h){__hnRAFCancel(+h||0);};"
"})(globalThis);";

/* 事件监听标准签名 glue: __hn* 内部桥 → 浏览器同款 API。
 *   - 元素方法形态: document.getElementById(id) 返回**缓存的元素包装**
 *     (addEventListener/removeEventListener + textContent/value 读写 +
 *     getContext —— canvas glue 的最小 document 由本 glue 先行创建并
 *     取代, canvas 用法不受影响: hnGet2D 自带按 id 缓存);
 *   - document.addEventListener / window.addEventListener 直达注册表
 *     ("document"/"window" 两个保留 target, 冒泡链尾自动命中);
 *   - __hnMakeEvent(type): 事件对象工厂 —— C 派发器调用它造对象, 再逐
 *     节点填 currentTarget。preventDefault/stopPropagation 按 DOM 纪律
 *     只置标志字段(_pd/_sp), 由 C 派发后读取。
 * 幂等(__hnMakeEvent 检查), 每 scope 只装一次; **装载顺序在 canvas glue
 * 之前**(document 得由本 glue 创建, canvas glue 的 `if (!g.document)` 让位)。 */
static const char *EVENT_GLUE_JS =
"(function(g){"
"  if (g.__hnMakeEvent) return;"
"  function capOf(o){return (o===true||(o&&o.capture))?1:0;}"
"  var EVCACHE={};"
"  function parseDecls(str){"
"    var out={};"
"    String(str||'').split(';').forEach(function(d){"
"      var i=d.indexOf(':');"
"      if(i>0){out[d.slice(0,i).trim().toLowerCase()]=d.slice(i+1).trim();}"
"    });"
"    return out;"
"  }"
"  function declsStr(o){"
"    var s='';"
"    for(var k in o){ if(o[k]!==''&&o[k]!==undefined){ s+=(s?'; ':'')+k+': '+o[k]; } }"
"    return s;"
"  }"
"  function rekey(z,nid){"
"    try{delete EVCACHE[String(z.__bid)];}catch(e){}"
"    z.__bid=nid;EVCACHE[nid]=z;"
"  }"
"  /* style 属性代理: camelCase 读写 style 属性(cssText 特判); 每次读写"
"     都以引擎 style 属性为事实源, 不维护 JS 侧副本。 */"
"  function makeStyleProxy(zid){"
"    function toDash(camel){return String(camel).replace(/[A-Z]/g,function(c){return '-'+c.toLowerCase();});}"
"    function readDecls(){return parseDecls(__hnGetAttr(zid,'style'));}"
"    function writeDecls(o){__hnSetAttr(zid,'style',declsStr(o));}"
"    return new Proxy({},{"
"      get:function(o,k){"
"        if(k==='cssText')return declsStr(readDecls());"
"        if(k==='getPropertyValue')return function(p){return readDecls()[String(p).toLowerCase()]||'';};"
"        if(k==='setProperty')return function(p,v){var d=readDecls();d[String(p).toLowerCase()]=String(v);writeDecls(d);};"
"        if(k==='removeProperty')return function(p){var d=readDecls();delete d[String(p).toLowerCase()];writeDecls(d);};"
"        return readDecls()[toDash(k)]||'';"
"      },"
"      set:function(o,k,v){"
"        var d=readDecls();"
"        if(k==='cssText'){d=parseDecls(String(v));}else{d[toDash(k)]=String(v);}"
"        writeDecls(d);return true;"
"      },"
"      deleteProperty:function(o,k){var d=readDecls();delete d[toDash(k)];writeDecls(d);return true;}"
"    });"
"  }"
"  function hnEl(id){"
"    id=String(id);"
"    var z=EVCACHE[id];"
"    if (z) return z;"
"    z={id:id,__bid:id};"
"    z.addEventListener=function(type,fn,opts){"
"      if (typeof fn!=='function') return 0;"
"      if (opts && opts.once){"
"        var w=function(ev){try{__hnRemoveListener(String(z.id),String(type),w);}catch(e){} return fn(ev);};"
"        return __hnAddListener(String(z.id),String(type),w,capOf(opts));"
"      }"
"      return __hnAddListener(String(z.id),String(type),fn,capOf(opts));};"
"    z.removeEventListener=function(type,fn){"
"      if (typeof fn!=='function') return;"
"      __hnRemoveListener(String(z.id),String(type),fn);};"
"    z.getContext=function(kind){return (kind==='2d'||kind==='2D')?hnGet2D(String(z.id)):null;};"
"    z.getAttribute=function(name){"
"      var v=__hnGetAttr(String(z.id),String(name));return v===null?null:v;};"
"    z.setAttribute=function(name,value){"
"      return !!__hnSetAttr(String(z.id),String(name),String(value));};"
"    z.removeAttribute=function(name){return !!__hnRemoveAttr(String(z.id),String(name));};"
"    z.hasAttribute=function(name){return !!__hnHasAttr(String(z.id),String(name));};"
"    z.getBoundingClientRect=function(){"
"      var r=__hnGetRect(String(z.id));"
"      if(!r)return {left:0,top:0,right:0,bottom:0,width:0,height:0,x:0,y:0};"
"      r=JSON.parse(r);"
"      return {left:r.x,top:r.y,right:r.x+r.width,bottom:r.y+r.height,"
"              width:r.width,height:r.height,x:r.x,y:r.y};};"
"    Object.defineProperty(z,'offsetWidth',{get:function(){"
"      var r=__hnGetRect(String(z.id));return r?JSON.parse(r).width:0;}});"
"    Object.defineProperty(z,'offsetHeight',{get:function(){"
"      var r=__hnGetRect(String(z.id));return r?JSON.parse(r).height:0;}});"
"    Object.defineProperty(z,'offsetLeft',{get:function(){"
"      var r=__hnGetRect(String(z.id));return r?JSON.parse(r).x:0;}});"
"    Object.defineProperty(z,'offsetTop',{get:function(){"
"      var r=__hnGetRect(String(z.id));return r?JSON.parse(r).y:0;}});"
"    Object.defineProperty(z,'id',{"
"      get:function(){return __hnGetAttr(String(z.__bid),'id')||String(z.__bid);},"
"      set:function(v){__hnSetAttr(String(z.__bid),'id',String(v));rekey(z,String(v));}});"
"    Object.defineProperty(z,'tagName',{get:function(){return String(__hnTag(String(z.id))).toUpperCase();}});"
"    Object.defineProperty(z,'textContent',{"
"      set:function(v){hnSetText(String(z.id),String(v));},"
"      get:function(){return __hnGetText(String(z.id));}});"
"    Object.defineProperty(z,'value',{"
"      set:function(v){hnSetValue(String(z.id),String(v));},"
"      get:function(){return __hnGetValue(String(z.id));}});"
"    Object.defineProperty(z,'className',{"
"      set:function(v){__hnSetAttr(String(z.id),'class',String(v));},"
"      get:function(){return __hnGetAttr(String(z.id),'class')||'';}});"
"    z.classList=(function(){"
"      function list(){return (__hnGetAttr(String(z.id),'class')||'').split(/\\s+/).filter(Boolean);}"
"      function write(a){__hnSetAttr(String(z.id),'class',a.join(' '));}"
"      return {"
"        contains:function(c){return list().indexOf(c)>=0;},"
"        add:function(){var a=list();for(var i=0;i<arguments.length;i++){if(a.indexOf(arguments[i])<0)a.push(arguments[i]);}write(a);},"
"        remove:function(){var a=list();for(var i=0;i<arguments.length;i++){var j=a.indexOf(arguments[i]);if(j>=0)a.splice(j,1);}write(a);},"
"        toggle:function(c,force){var a=list(),j=a.indexOf(c);"
"          var want=(force===undefined)?(j<0):!!force;"
"          if(want&&j<0){a.push(c);}else if(!want&&j>=0){a.splice(j,1);}"
"          write(a);return want;},"
"        get length(){return list().length;},"
"        item:function(i){return list()[i]||null;},"
"        toString:function(){return list().join(' ');}};"
"    })();"
"    z.dataset=(function(){"
"      function toDash(k){return String(k).replace(/[A-Z]/g,function(c){return '-'+c.toLowerCase();});}"
"      return new Proxy({},{"
"        get:function(o,k){return __hnGetAttr(String(z.id),'data-'+toDash(k));},"
"        set:function(o,k,v){__hnSetAttr(String(z.id),'data-'+toDash(k),String(v));return true;},"
"        deleteProperty:function(o,k){__hnRemoveAttr(String(z.id),'data-'+toDash(k));return true;},"
"        has:function(o,k){return !!__hnHasAttr(String(z.id),'data-'+toDash(k));}"
"      });"
"    })();"
"    Object.defineProperty(z,'style',{get:function(){return makeStyleProxy(String(z.id));}});"
"    Object.defineProperty(z,'checked',{"
"      set:function(v){if(v)__hnSetAttr(String(z.id),'checked','');else __hnRemoveAttr(String(z.id),'checked');},"
"      get:function(){return !!__hnHasAttr(String(z.id),'checked');}});"
"    Object.defineProperty(z,'disabled',{"
"      set:function(v){if(v)__hnSetAttr(String(z.id),'disabled','');else __hnRemoveAttr(String(z.id),'disabled');},"
"      get:function(){return !!__hnHasAttr(String(z.id),'disabled');}});"
"    Object.defineProperty(z,'hidden',{"
"      set:function(v){if(v)__hnSetAttr(String(z.id),'hidden','');else __hnRemoveAttr(String(z.id),'hidden');},"
"      get:function(){return !!__hnHasAttr(String(z.id),'hidden');}});"
"    z.appendChild=function(ch){"
"      if(!ch||typeof ch.id!=='string')return ch;"
"      __hnAppend(String(z.id),String(ch.id));return ch;};"
"    z.removeChild=function(ch){"
"      if(!ch||typeof ch.id!=='string')return ch;"
"      __hnDetach(String(ch.id));return ch;};"
"    z.remove=function(){__hnDetach(String(z.id));};"
"    z.focus=function(){__hnFocus(String(z.id));};"
"    z.dispatchEvent=function(ev){"
"      if(!ev||typeof ev.type!=='string')return false;"
"      return !!__hnDispatch(String(z.id),ev.type,ev&&ev.detail?+ev.detail:0);};"
"    z.click=function(){__hnDispatch(String(z.id),'click',0);};"
"    /* width/height 属性语义(canvas/img: 位图/固有尺寸属性, 非 style) */"
"    Object.defineProperty(z,'width',{"
"      set:function(v){__hnSetAttr(String(z.id),'width',String(+v||0));},"
"      get:function(){var v=__hnGetAttr(String(z.id),'width');return v===null?0:+v;}});"
"    Object.defineProperty(z,'height',{"
"      set:function(v){__hnSetAttr(String(z.id),'height',String(+v||0));},"
"      get:function(){var v=__hnGetAttr(String(z.id),'height');return v===null?0:+v;}});"
"    EVCACHE[id]=z;"
"    return z;"
"  }"
"  if (!g.document || !g.document.__hnevents) {"
"    g.document={"
"      __hnevents:true,"
"      getElementById:function(id){"
"        if(!__hnExists(String(id)))return null;"
"        return hnEl(id);},"
"      addEventListener:function(type,fn,opts){"
"        if (typeof fn!=='function') return 0;"
"        return __hnAddListener('document',String(type),fn,capOf(opts));},"
"      removeEventListener:function(type,fn){"
"        if (typeof fn!=='function') return;"
"        __hnRemoveListener('document',String(type),fn);}"
"    };"
"  }"
"  g.window=g;"
"  Object.defineProperty(g,'innerWidth',{get:function(){return __hnViewport()[0];}});"
"  Object.defineProperty(g,'innerHeight',{get:function(){return __hnViewport()[1];}});"
"  Object.defineProperty(g,'devicePixelRatio',{get:function(){return __hnViewport()[2];}});"
"  g.addEventListener=function(type,fn,opts){"
"    if (typeof fn!=='function') return 0;"
"    return __hnAddListener('window',String(type),fn,capOf(opts));};"
"  g.removeEventListener=function(type,fn){"
"    if (typeof fn!=='function') return;"
"    __hnRemoveListener('window',String(type),fn);};"
"  g.__hnMakeEvent=function(type){"
"    var ev={type:String(type||''),target:'',currentTarget:'',"
"            clientX:0,clientY:0,key:'',code:'',detail:0,"
"            shift:false,ctrl:false,alt:false,meta:false,"
"            _pd:false,_sp:false};"
"    ev.preventDefault=function(){this._pd=true;};"
"    ev.stopPropagation=function(){this._sp=true;};"
"    return ev;"
"  };"
"  /* ---- hn.animate(targets, params) —— anime.js 形状薄桥(A4; 第二刀补 loop) ----\n"
"     targets: id 串/元素包装/数组; params: {<属性>: 目标|[from,to],\n"
"     duration, delay, stagger, ease, loop(N|'infinite'), alternate,\n"
"     reversed, onComplete}。执行**走引擎自有时钟**: 写 transition 声明\n"
"     + 目标值, 插值由引擎 anim_tick 完成 —— JS 不做每帧驱动(双驱动\n"
"     时钟冲突的教训); JS 只排重播节拍(引擎定时器)。alternate/reversed\n"
"     轮间恒异向 → 每轮单写(引擎从当前绘制值起插); 非 alternate 全正向\n"
"     → 重置轮两写(transition:none 落 from → transition+to)。\n"
"     loop 语义需要 [from,to] 数组形态(标量目标重播无视觉差, 同 anime)。 */\n"
"  if (!g.hn) g.hn = {};\n"
"  g.hn.animate = function(targets, params) {\n"
"    params = params || {};\n"
"    var dur = params.duration != null ? +params.duration : 300;\n"
"    if (!(dur > 0)) dur = 300;\n"
"    var delay = params.delay ? +params.delay : 0;\n"
"    var stag = params.stagger ? +params.stagger : 0;\n"
"    var ease = params.ease ? String(params.ease) : 'ease-out';\n"
"    var infinite = (params.loop === 'infinite');\n"
"    var total = infinite ? -1 : (params.loop != null ? (+params.loop > 0 ? +params.loop : 1) : 1);\n"
"    var alternate = !!params.alternate;\n"
"    var reversed = !!params.reversed;\n"
"    var list = targets == null ? [] : (Array.isArray(targets) ? targets : [targets]);\n"
"    var ids = [];\n"
"    list.forEach(function(t) {\n"
"      var id = (t && typeof t === 'object') ? String(t.id) : String(t);\n"
"      if (id && __hnExists(id)) ids.push(id);\n"
"    });\n"
"    var props = {};\n"
"    for (var k in params) {\n"
"      if (k === 'duration' || k === 'delay' || k === 'ease' ||\n"
"          k === 'stagger' || k === 'onComplete' || k === 'loop' ||\n"
"          k === 'alternate' || k === 'reversed') continue;\n"
"      props[String(k).replace(/[A-Z]/g, function(c) { return '-' + c.toLowerCase(); })] = params[k];\n"
"    }\n"
"    if (!ids.length) {\n"
"      if (typeof params.onComplete === 'function') params.onComplete({targets: []});\n"
"      return {targets: [], duration: dur, delay: delay, loops: total};\n"
"    }\n"
"    var pending = ids.length;\n"
"    function oneDone() {\n"
"      if (--pending === 0 && total > 0 && typeof params.onComplete === 'function')\n"
"        params.onComplete({targets: ids});\n"
"    }\n"
"    ids.forEach(function(id, idx) {\n"
"      var dl = delay + stag * idx;\n"
"      var trans = [], to = {}, from = {}, hasFrom = false;\n"
"      var d0 = parseDecls(__hnGetAttr(id, 'style'));\n"
"      for (var p in props) {\n"
"        var v = props[p];\n"
"        if (Array.isArray(v)) { from[p] = String(v[0]); to[p] = String(v[1]); hasFrom = true; }\n"
"        else to[p] = String(v);\n"
"        trans.push(p + ' ' + dur + 'ms ' + ease + ' ' + dl + 'ms');\n"
"      }\n"
"      function roundFwd(r) {\n"
"        if (!alternate) return true;\n"
"        return ((reversed ? 1 : 0) + r) % 2 === 0;   /* reversed 首轮即反向 */\n"
"      }\n"
"      function writeRound(r) {\n"
"        var d = parseDecls(__hnGetAttr(id, 'style'));\n"
"        if (r === 0) {\n"
"          if (hasFrom && !reversed) { for (var p1 in from) d[p1] = from[p1]; }\n"
"          d.transition = trans.join(', ');\n"
"          __hnSetAttr(id, 'style', declsStr(d));      /* 第一写: from + transition */\n"
"          if (hasFrom && !reversed) {\n"
"            setTimeout(function() {                    /* 第二写: 目标值 → 引擎过渡接管 */\n"
"              var dd = parseDecls(__hnGetAttr(id, 'style'));\n"
"              for (var p2 in to) dd[p2] = to[p2];\n"
"              __hnSetAttr(id, 'style', declsStr(dd));\n"
"            }, dl > 16 ? dl : 16);\n"
"          }\n"
"          return;\n"
"        }\n"
"        if (!alternate) {\n"
"          /* 全正向重播: 上一轮停在 to, 两写重置到 from */\n"
"          d.transition = 'none';\n"
"          if (hasFrom) { for (var p3 in from) d[p3] = from[p3]; }\n"
"          __hnSetAttr(id, 'style', declsStr(d));\n"
"          setTimeout(function() {\n"
"            var dd = parseDecls(__hnGetAttr(id, 'style'));\n"
"            dd.transition = trans.join(', ');\n"
"            for (var p4 in to) dd[p4] = to[p4];\n"
"            __hnSetAttr(id, 'style', declsStr(dd));\n"
"          }, 16);\n"
"          return;\n"
"        }\n"
"        /* alternate/reversed: 轮间恒异向, 上一轮终点 = 本轮起点 → 单写 */\n"
"        d.transition = trans.join(', ');\n"
"        for (var p5 in to) d[p5] = roundFwd(r) ? to[p5] : from[p5];\n"
"        __hnSetAttr(id, 'style', declsStr(d));\n"
"      }\n"
"      var roundMs = dur + dl + 16;\n"
"      writeRound(0);\n"
"      if (total !== 1) {\n"
"        var r = 1;\n"
"        var timer = setInterval(function() {\n"
"          writeRound(r);\n"
"          r++;\n"
"          if (total > 0 && r >= total) clearInterval(timer);\n"
"        }, roundMs);\n"
"        if (total > 0) setTimeout(oneDone, (total - 1) * roundMs + dl + dur + 32);\n"
"      } else {\n"
"        setTimeout(oneDone, dl + dur + 32);\n"
"      }\n"
"    });\n"
"    return {targets: ids, duration: dur, delay: delay, loops: total};\n"
"  };\n"
"})(globalThis);";

char *hn_rt_eval(const char *js, const char *scope_key, hn_doc *doc) {
    JSContext *ctx = js_scope_get(scope_key ? scope_key : "default");
    if (!ctx) return NULL;
    /* 桥环境: opaque = {doc, ctx}(hnSetText/hnSetValue 读 .doc; hnMedia*/
    /* 读 .ctx —— ctx 由 doc 反查表得到, 未登记的 context 媒体桥降级 no-op) */
    int glue_loaded = 0;
    for (int i = 0; i < js_scope_n; i++) {
        if (js_scopes[i].ctx == ctx) {
            js_scopes[i].bridge.doc = doc;
            js_scopes[i].bridge.ctx = doc ? doc_ctx_find(doc) : NULL;
            glue_loaded = js_scopes[i].glue_done;
            break;
        }
    }

    JSValue global = JS_GetGlobalObject(ctx);
    /* hn 命名空间幂等保留: 每次 eval 都走到这里, 若每次都新建对象,
       glue 挂上去的扩展(hn.animate 等)会被静默冲掉 —— 已存在则复用,
       只补缺失的 fetch。注意 JS_SetPropertyStr 是**接管语义**(ng 系;
       写入即消耗传入值, 不得再 Free —— 此前按拷贝语义补 Free 造成
       gc 双重释放, abort trap 6)。 */
    JSValue hn_api = JS_GetPropertyStr(ctx, global, "hn");
    if (JS_IsObject(hn_api)) {
        JSValue f = JS_GetPropertyStr(ctx, hn_api, "fetch");
        int has_fetch = JS_IsFunction(ctx, f);
        JS_FreeValue(ctx, f);
        if (!has_fetch)
            JS_SetPropertyStr(ctx, hn_api, "fetch",
                JS_NewCFunction(ctx, js_hn_fetch, "fetch", 2));
        JS_FreeValue(ctx, hn_api);   /* GetPropertyStr 返回的引用 */
    } else {
        JS_FreeValue(ctx, hn_api);   /* undefined/异常: 免了再建 */
        hn_api = JS_NewObject(ctx);
        JS_SetPropertyStr(ctx, hn_api, "fetch",
            JS_NewCFunction(ctx, js_hn_fetch, "fetch", 2));
        JS_SetPropertyStr(ctx, global, "hn", hn_api);  /* 接管, 不再 Free */
    }
    JS_SetPropertyStr(ctx, global, "hnSetText",
        JS_NewCFunction(ctx, js_hn_set_text, "hnSetText", 2));
    JS_SetPropertyStr(ctx, global, "hnSetValue",
        JS_NewCFunction(ctx, js_hn_set_value, "hnSetValue", 2));
    JS_SetPropertyStr(ctx, global, "hnMeshParam",
        JS_NewCFunction(ctx, js_hn_mesh_param, "hnMeshParam", 3));
    JS_SetPropertyStr(ctx, global, "hnMediaPlay",
        JS_NewCFunction(ctx, js_hn_media_play, "hnMediaPlay", 1));
    JS_SetPropertyStr(ctx, global, "hnMediaPause",
        JS_NewCFunction(ctx, js_hn_media_pause, "hnMediaPause", 1));
    JS_SetPropertyStr(ctx, global, "hnMediaSeek",
        JS_NewCFunction(ctx, js_hn_media_seek, "hnMediaSeek", 2));
    JS_SetPropertyStr(ctx, global, "hnMediaTime",
        JS_NewCFunction(ctx, js_hn_media_time, "hnMediaTime", 1));
    JS_SetPropertyStr(ctx, global, "hnMediaDuration",
        JS_NewCFunction(ctx, js_hn_media_duration, "hnMediaDuration", 1));
    JS_SetPropertyStr(ctx, global, "hnMediaVolume",
        JS_NewCFunction(ctx, js_hn_media_volume, "hnMediaVolume", 2));
    JS_SetPropertyStr(ctx, global, "hnWasmLoad",
        JS_NewCFunction(ctx, js_hn_wasm_load, "hnWasmLoad", 1));
    JS_SetPropertyStr(ctx, global, "hnWasmCall",
        JS_NewCFunction(ctx, js_hn_wasm_call, "hnWasmCall", 2));
    JS_SetPropertyStr(ctx, global, "hnCanvas2D",
        JS_NewCFunction(ctx, js_hn_canvas_2d, "hnCanvas2D", 1));
    JS_SetPropertyStr(ctx, global, "hnMeasureText",
        JS_NewCFunction(ctx, js_hn_measure_text, "hnMeasureText", 2));
    JS_SetPropertyStr(ctx, global, "__hnCanvasPixels",
        JS_NewCFunction(ctx, js_hn_canvas_pixels, "__hnCanvasPixels", 5));
    JS_SetPropertyStr(ctx, global, "__hnCanvasPng",
        JS_NewCFunction(ctx, js_hn_canvas_png, "__hnCanvasPng", 1));
    /* 定时器/RAF 内部桥(__hn*; 标准签名由 TIMER_GLUE_JS 转接) */
    JS_SetPropertyStr(ctx, global, "__hnTimer",
        JS_NewCFunction(ctx, js_hn_timer, "__hnTimer", 4));
    JS_SetPropertyStr(ctx, global, "__hnTimerClear",
        JS_NewCFunction(ctx, js_hn_timer_clear, "__hnTimerClear", 1));
    JS_SetPropertyStr(ctx, global, "__hnRAF",
        JS_NewCFunction(ctx, js_hn_raf, "__hnRAF", 1));
    JS_SetPropertyStr(ctx, global, "__hnRAFCancel",
        JS_NewCFunction(ctx, js_hn_raf_cancel, "__hnRAFCancel", 1));
    /* 事件层内部桥(标准签名由 EVENT_GLUE_JS 转接) */
    JS_SetPropertyStr(ctx, global, "__hnAddListener",
        JS_NewCFunction(ctx, js_hn_add_listener, "__hnAddListener", 4));
    JS_SetPropertyStr(ctx, global, "__hnRemoveListener",
        JS_NewCFunction(ctx, js_hn_remove_listener, "__hnRemoveListener", 3));
    JS_SetPropertyStr(ctx, global, "__hnGetValue",
        JS_NewCFunction(ctx, js_hn_get_value, "__hnGetValue", 1));
    /* DOM 标准面桥(glue 转接为 getAttribute/querySelector/classList/...) */
    JS_SetPropertyStr(ctx, global, "__hnGetAttr",
        JS_NewCFunction(ctx, js_hn_get_attr, "__hnGetAttr", 2));
    JS_SetPropertyStr(ctx, global, "__hnSetAttr",
        JS_NewCFunction(ctx, js_hn_set_attr, "__hnSetAttr", 3));
    JS_SetPropertyStr(ctx, global, "__hnRemoveAttr",
        JS_NewCFunction(ctx, js_hn_remove_attr, "__hnRemoveAttr", 2));
    JS_SetPropertyStr(ctx, global, "__hnHasAttr",
        JS_NewCFunction(ctx, js_hn_has_attr, "__hnHasAttr", 2));
    JS_SetPropertyStr(ctx, global, "__hnAttrNames",
        JS_NewCFunction(ctx, js_hn_attr_names, "__hnAttrNames", 1));
    JS_SetPropertyStr(ctx, global, "__hnGetText",
        JS_NewCFunction(ctx, js_hn_get_text, "__hnGetText", 1));
    JS_SetPropertyStr(ctx, global, "__hnTag",
        JS_NewCFunction(ctx, js_hn_tag, "__hnTag", 1));
    JS_SetPropertyStr(ctx, global, "__hnAppend",
        JS_NewCFunction(ctx, js_hn_append, "__hnAppend", 2));
    JS_SetPropertyStr(ctx, global, "__hnDetach",
        JS_NewCFunction(ctx, js_hn_detach, "__hnDetach", 1));
    JS_SetPropertyStr(ctx, global, "__hnFocus",
        JS_NewCFunction(ctx, js_hn_focus, "__hnFocus", 1));
    JS_SetPropertyStr(ctx, global, "__hnExists",
        JS_NewCFunction(ctx, js_hn_exists, "__hnExists", 1));
    /* DOM 第二轮桥(rect/scroll/insert/dispatch/viewport) */
    JS_SetPropertyStr(ctx, global, "__hnGetRect",
        JS_NewCFunction(ctx, js_hn_get_rect, "__hnGetRect", 1));
    JS_SetPropertyStr(ctx, global, "__hnDispatch",
        JS_NewCFunction(ctx, js_hn_dispatch, "__hnDispatch", 3));
    JS_SetPropertyStr(ctx, global, "__hnViewport",
        JS_NewCFunction(ctx, js_hn_viewport, "__hnViewport", 0));
    JS_FreeValue(ctx, global);

    /* 事件 glue(元素包装 + document/window addEventListener + 事件对象
       工厂): **先于 canvas glue 装载** —— document 由本 glue 创建(带
       getContext), canvas glue 的 `if (!g.document)` 自然让位 */
    int event_glue_loaded = 0;
    for (int i = 0; i < js_scope_n; i++)
        if (js_scopes[i].ctx == ctx) { event_glue_loaded = js_scopes[i].event_glue_done; break; }
    if (!event_glue_loaded) {
        JSValue gr = JS_Eval(ctx, EVENT_GLUE_JS, strlen(EVENT_GLUE_JS),
                             "<event-glue>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(gr)) {
            JSValue e = JS_GetException(ctx);
            JS_FreeValue(ctx, e);
        }
        JS_FreeValue(ctx, gr);
        for (int i = 0; i < js_scope_n; i++)
            if (js_scopes[i].ctx == ctx) { js_scopes[i].event_glue_done = 1; break; }
    }

    /* canvas 2D glue(hnGet2D + 状态机): 每 scope 只装一次(幂等定义,
       重复 eval 也安全, 这里省掉重复解析成本) */
    if (!glue_loaded) {
        JSValue gr = JS_Eval(ctx, CANVAS_GLUE_JS, strlen(CANVAS_GLUE_JS),
                             "<canvas-glue>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(gr)) {
            /* glue 坏了不拖垮 eval, 但必须留痕 —— 静默吞掉会让 canvas 全面
               失效却无从查起(本轮实测踩过: 语法错 → hnGet2D 未定义)。 */
            JSValue e = JS_GetException(ctx);
            const char *msg = JS_ToCString(ctx, e);
            fprintf(stderr, "[canvas-glue] %s\n", msg ? msg : "(exception)");
            if (msg) JS_FreeCString(ctx, msg);
            JS_FreeValue(ctx, e);
        }
        JS_FreeValue(ctx, gr);
        for (int i = 0; i < js_scope_n; i++)
            if (js_scopes[i].ctx == ctx) { js_scopes[i].glue_done = 1; break; }
    }

    /* 定时器/RAF glue(setTimeout/setInterval/requestAnimationFrame 标准
       签名): 同样每 scope 一次 */
    int timer_glue_loaded = 0;
    for (int i = 0; i < js_scope_n; i++)
        if (js_scopes[i].ctx == ctx) { timer_glue_loaded = js_scopes[i].timer_glue_done; break; }
    if (!timer_glue_loaded) {
        JSValue gr = JS_Eval(ctx, TIMER_GLUE_JS, strlen(TIMER_GLUE_JS),
                             "<timer-glue>", JS_EVAL_TYPE_GLOBAL);
        if (JS_IsException(gr)) {
            JSValue e = JS_GetException(ctx);
            JS_FreeValue(ctx, e);
        }
        JS_FreeValue(ctx, gr);
        for (int i = 0; i < js_scope_n; i++)
            if (js_scopes[i].ctx == ctx) { js_scopes[i].timer_glue_done = 1; break; }
    }

    size_t len = strlen(js);
    JSValue r = JS_Eval(ctx, js, len, "<eval>", JS_EVAL_TYPE_GLOBAL);
    if (JS_IsException(r)) {
        JSValue e = JS_GetException(ctx);
        const char *msg = JS_ToCString(ctx, e);
        char *out = (char *)malloc(strlen(msg ? msg : "exception") + 16);
        if (out) sprintf(out, "Error: %s", msg ? msg : "?");
        if (msg) JS_FreeCString(ctx, msg);
        JS_FreeValue(ctx, e);
        return out;
    }
    const char *str = JS_ToCString(ctx, r);
    char *out = str ? strdup(str) : NULL;
    JS_FreeCString(ctx, str);
    JS_FreeValue(ctx, r);
    return out;
}

/* ---------------- 文档内联 <script> 自动执行 ----------------
 * 整文档装载(hn_rt_open / hn_rt_render 热替换)后按文档序执行 <script>
 * 的文本 —— 与浏览器"解析即执行"对齐, 让 canvas 2D 这类纯 JS 驱动的
 * 页面无需外部 eval 驱动即可离线渲染(--shot / headless 全链路可用)。
 * hx 片段交换不重跑既有脚本(片段是数据, 不是新文档)。
 * 只用 hn.h 的公开遍历 API(first_child/next_sibling/tag/text —— rt 层
 * 不接触引擎内部结构); 脚本指针先快照后执行: eval 内的 DOM 改写不会
 * 移动 arena 内存, 指针稳定。 */
static void run_doc_scripts(hn_rt *rt) {
    hn_doc *doc = hn_context_doc(rt->ctx);
    if (!doc) return;
    hn_node *root = hn_doc_root(doc);
    if (!root) return;
    const char *texts[32];
    size_t lens[32];
    hn_node *stack[128];
    int sp = 0, n = 0;
    stack[sp++] = root;
    while (sp > 0 && n < 32) {
        hn_node *cur = stack[--sp];
        const char *tag = hn_node_tag(cur);
        if (tag && !strcmp(tag, "script")) {
            for (hn_node *ch = hn_node_first_child(cur); ch && n < 32;
                 ch = hn_node_next_sibling(ch)) {
                size_t len = 0;
                const char *t = hn_node_text(ch, &len);
                if (t && len) { texts[n] = t; lens[n] = len; n++; }
            }
        }
        /* 子节点逆序入栈 → 出栈为文档序(先序遍历) */
        hn_node *kids[64];
        int nk = 0;
        for (hn_node *ch = hn_node_first_child(cur); ch && nk < 64;
             ch = hn_node_next_sibling(ch))
            kids[nk++] = ch;
        for (int k = nk - 1; k >= 0 && sp < 128; k--)
            stack[sp++] = kids[k];
    }
    for (int i = 0; i < n; i++) {
        char *r = hn_rt_eval(texts[i], rt->id, doc);
        if (r && !strncmp(r, "Error:", 6))
            fprintf(stderr, "[script] %s\n", r);   /* 脚本异常可见而非静默 */
        free(r);
    }
}

#else
/* 无 QuickJS 构建: 定时器/RAF/事件层全套 no-op(hn_rt_open/close/frame/
   button/key/scroll/pointer_move 的调用点无需知道差别 —— 表恒空, 帧节拍
   不含 JS 信号, 派发恒 0)。js_rt_free 空实现。 */
static void js_rt_free(hn_rt *rt) { (void)rt; }
static void js_rt_release_timers(hn_rt *rt) { (void)rt; }
static void js_scope_bind_rt(hn_rt *rt) { (void)rt; }
static void js_scope_unbind_rt(hn_rt *rt) { (void)rt; }
static int js_timers_drain(hn_rt *rt, int *repaint) { (void)rt; (void)repaint; return 0; }
static void js_listeners_clear(hn_rt *rt) { (void)rt; }
static int rt_js_has_listener(hn_rt *rt, const char *type) { (void)rt; (void)type; return 0; }
static int rt_js_dispatch(hn_rt *rt, const char *type, hn_node *target,
                          float x, float y, const char *utf8, int key, int detail) {
    (void)rt; (void)type; (void)target; (void)x; (void)y; (void)utf8; (void)key; (void)detail;
    return 0;
}

/* 文档内联 <script> 执行同样依赖 QuickJS: 无 JS 引擎时不跑(eval 口径
   会如实报告"QuickJS 未编入本产物", 这里保持同一降级纪律)。 */
static void run_doc_scripts(hn_rt *rt) { (void)rt; }

char *hn_rt_eval(const char *js, const char *scope_key, hn_doc *doc) {
    (void)js; (void)scope_key; (void)doc;
    static const char msg[] = "QuickJS 未编入本产物(构建时未定义 HN_HAVE_QUICKJS)";
    char *out = (char *)malloc(sizeof(msg) + 8);
    if (out) sprintf(out, "Error: %s", msg);
    return out;
}
#endif

void hn_rt_close(hn_rt *rt) {
    if (!rt) return;
    free(rt->page_path);
    for (int i = 0; i < rt->back_n; i++) free(rt->back[i]);
    free(rt->back);
    js_rt_release_timers(rt);   /* 定时器/RAF 持有的 JSValue 先还(上下文仍活着) */
#ifdef HN_HAVE_QUICKJS
    /* Release pending fetch Promise functions before destroying scope */
    {
        JSContext *ctx = js_scope_find(rt->id);
        if (ctx) {
            for (int i = 0; i < rt->pending_fetch_n; i++) {
                JS_FreeValue(ctx, rt->pending_fetches[i].resolve);
                JS_FreeValue(ctx, rt->pending_fetches[i].reject);
            }
        }
    }
    rt->pending_fetch_n = 0;
    /* Destroy HTTP scope: cancels in-flight requests, waits for threads */
    if (rt->http_scope) {
        hn_http_scope_destroy((hn_http_scope *)rt->http_scope);
        rt->http_scope = NULL;
    }
#endif
    js_scope_unbind_rt(rt);     /* 桥反查表里不悬空 —— 宿主没了定时器即失效 */
    js_rt_free(rt);
    if (rt->ctx) {
        doc_ctx_unbind_ctx(rt->ctx);   /* 悬空 ctx 不留在反查表里 */
        hn_context_destroy(rt->ctx);
    }
    free(rt->bgra);
    free(rt->soft_buf);   /* hnsoft 复用缓冲归 rt 持有, 一并归还 */
    free(rt);
}

int hn_rt_render(hn_rt *rt, const char *html, size_t len) {
    if (!rt || !html) return 0;
    hn_doc *doc = hn_parse_html(html, len);
    if (!doc) return 0;
    hn_doc_autoid_hx(doc);
    doc_ctx_unbind_doc(hn_context_doc(rt->ctx));   /* 旧文档的登记一并撤掉 */
    hn_context_set_doc(rt->ctx, doc);
    doc_ctx_bind(doc, rt->ctx);        /* 新文档重新登记(eval 桥按 doc 反查) */
    rt->poll_built = 0;
    js_listeners_clear(rt);            /* 换装清空监听器: 旧元素 id 已随旧文档消失, 不留悬挂 */
    int ok = rt_render(rt);
    if (ok) { run_doc_scripts(rt); run_load_actions(rt); rt_render(rt); }
    return ok;
}

int hn_rt_reflow(hn_rt *rt) { return rt ? rt_render(rt) : 0; }

const unsigned char *hn_rt_pixels(hn_rt *rt) { return rt ? rt->bgra : NULL; }
int hn_rt_width(hn_rt *rt) { return rt ? rt->w : 0; }
int hn_rt_height(hn_rt *rt) { return rt ? rt->h : 0; }

/* ---- HiDPI(hn_rt.h 契约): 设缩放并立即重渲染; 位图尺寸访问器 ---- */
void hn_rt_set_scale(hn_rt *rt, float scale) {
    if (!rt) return;
    float s = (scale >= 0.5f) ? scale : 1.0f;
    if (s > 8.0f) s = 8.0f;              /* 防呆上限(调用方另 cap 2.0) */
    if (rt->scale == s) return;          /* 同值幂等: 不做无谓重渲染 */
    rt->scale = s;
    rt_render(rt);                       /* 新缩放整幅重出图, 调用方直接 blit */
}

float hn_rt_scale(const hn_rt *rt) { return rt_scale(rt); }
int hn_rt_bitmap_width(const hn_rt *rt) { return rt_bitmap_w(rt); }
int hn_rt_bitmap_height(const hn_rt *rt) { return rt_bitmap_h(rt); }

/* ambient 节拍(纯环境动画页面的帧间隔): 环境钮 HN_ANIM_FPS = 16/30/60
   覆盖(62/33/16ms), 缺省 30 ≈ 33ms。只作用于"仅 ambient 活跃"的帧 ——
   interactive(过渡/关键帧/媒体/滚动)恒 16ms, 不受此钮影响。按值失效的
   记忆: 探针可同进程改钮(其余情况 getenv 只在值变时真正换算)。 */
static int ambient_beat_ms(void) {
    static char memo[24] = "\x01";      /* 首次必不相等 → 走一次换算 */
    static int beat = 33;
    const char *e = getenv("HN_ANIM_FPS");
    const char *cur = (e && *e) ? e : "";
    if (strcmp(cur, memo) != 0) {
        snprintf(memo, sizeof(memo), "%s", cur);
        long fps = atol(cur);
        beat = 33;                       /* 缺省 ~30fps */
        if (fps == 16) beat = 62;
        else if (fps == 60) beat = 16;
    }
    return beat;
}

/* 媒体-only 轻渲染判据(hn_media.c 实现; 本文件媒体时钟段的使用点):
   有会话在播且在播 video 的首帧已过全路径落位(cur_time>0)。引擎的动画
   (过渡/关键帧/入场)全部只写绘制期字段(opacity/transform/色彩), 轻路径
   重建显示列表即重读它们 —— 唯一必须全路径的窗口是"首帧固有尺寸变化",
   由 cur_time<=0 门挡住, 之后的视频帧推进只换像素, 免 relayout。 */
extern int hn_media_light_render_ok(hn_context *c);

int hn_rt_frame(hn_rt *rt, float dt_ms, int *repaint) {
    if (!rt) return -1;
    if (repaint) *repaint = 0;
    rt->scroll_armed = 1;   /* scroll 事件节流重臂(每 pump 至多派发一次) */
    /* hx 轮询域虚拟钟(与 QuickJS 构建里 clock_ms 同一手势, 但**不受**构建
       开关门控 —— 轮询在无 JS 构建同样存在): 每帧 += dt。真窗壳喂真实间隔
       ⇒ 与墙钟一致; 探针/--advance 喂虚拟步进 ⇒ 轮询确定性到期。 */
    rt->poll_clock_ms += dt_ms > 0 ? (double)dt_ms : 0;   /* 轮询/hx: 墙钟语义 */
    /* hx-trigger change 的 delay 防抖冲刷: 到期即执行(一次性; 重触发刷新到期) */
    for (int i = 0; i < HN_HX_SCHED_MAX; i++) {
        if (!hx_sched_tab[i].id[0] || hx_sched_tab[i].due <= 0) continue;
        if (rt->poll_clock_ms >= hx_sched_tab[i].due) {
            hx_sched_tab[i].due = 0;
            hn_node *cn = hn_doc_find_by_id(hn_context_doc(rt->ctx), hx_sched_tab[i].id);
            if (cn) hx_perform(rt, cn);
        }
    }
    /* hx-swap swap:N 延迟换入冲刷: 到期 → 重找目标 → swap + 布局 +
       afterSwap(目标重找失败 = 载体已被换掉, 静默弃帧) */
    for (int i = 0; i < HN_HX_SWAP_PEND_MAX; i++) {
        if (!hx_swap_pend[i].active || rt->poll_clock_ms < hx_swap_pend[i].due)
            continue;
        hx_swap_pend[i].active = 0;
        hn_node *t = hn_doc_find_by_id(hn_context_doc(rt->ctx),
                                       hx_swap_pend[i].target_id);
        if (t && hn_doc_swap_node(hn_context_doc(rt->ctx), t, hx_swap_pend[i].m,
                                  hx_swap_pend[i].frag, hx_swap_pend[i].len)) {
            hn_context_layout(rt->ctx, (float)rt->w, (float)rt->h, &g_tb);
            if (hx_swap_pend[i].m != HN_SWAP_NONE)
                rt_js_dispatch(rt, "htmx:afterSwap", t, 0, 0, NULL, 0, 0);
            if (repaint) *repaint = 1;
        }
    }
    /* handwired #1: 事件已在各自时间戳推进过动画的部分从帧推进中扣除
       —— 同一真实跨度恰被推进一次(事件时刻 + 帧尾余量), 任何帧率下
       动画总进度 = 真实流逝。 */
    float dt_anim = dt_ms - (float)rt->ev_consumed_ms;
    if (dt_anim < 0) dt_anim = 0;
    rt->ev_consumed_ms = 0;
#ifdef HN_HAVE_QUICKJS
    rt->clock_ms += dt_anim > 0 ? (double)dt_anim : 0;   /* 虚拟单调钟: 随帧步进 */
#endif
    /* JS 定时器与 RAF 先行(帧开头语义): 到期回调、画布/文本改动在此
       生效。回调是否真的跑过由返回值给出 —— 渲染合并进帧尾一次
       (见 pend)。 */
    /* ---- 帧内渲染合并(性能治理): 一帧至多一次 rt_render ----
       旧口径里 定时器/RAF 排干、动画 tick、hx 轮询各自立即整渲染:
       RAF+动画并存的页面(仪表盘/聊天流常见形态)每帧 2 次全量
       layout+raster, 前一次的像素同拍内必被后一次覆盖 —— 从不呈现,
       纯浪费(HN_RT_RENDER_TRACE=1 实测 120 帧 242 渲染)。
       现改为"标记待渲染档位, 帧尾渲染一次": full > ambient(满档优先
       —— JS 回调可能改几何, 必须 relayout 才落位)。可见输出与旧口径
       一致: 旧口径可见的本就是最后一次渲染的帧尾状态, 只是中途多画
       了看不见的中间帧; 定时器/回调的触发时机与顺序不动(语义零漂移)。 */
    int pend = 0;   /* 0 无 / 1 ambient 轻路径 / 2 全路径 */
    if (js_timers_drain(rt, NULL))
        pend = 2;

    /* Drain HTTP async completions: resolve/reject pending fetch Promises */
#ifdef HN_HAVE_QUICKJS
    if (rt->http_scope && rt->pending_fetch_n > 0) {
        size_t drained = hn_http_async_drain((hn_http_scope *)rt->http_scope,
                                             http_complete_cb, rt);
        (void)drained; /* consumed count not used; callback handles state */
    }
#endif

    /* 动画推进 + 活跃类别分级(性能治理): interactive(过渡/关键帧/媒体/
       平滑滚动)→ 全路径渲染 + 16ms 节拍; 仅 ambient(ext_clock 类)→
       轻路径(免 relayout)+ HN_ANIM_FPS 节拍(缺省 33ms ≈ 30fps)。
       媒体-only 在播(interactive 且无首帧几何待落位)同走轻路径:
       视频帧推进不改样式/几何, relayout 是纯浪费(事故③的 CPU 大头之一)。 */
    int anim_cls = HN_ANIM_NONE;
    int anim = hn_context_anim_tick_ex(rt->ctx, dt_anim, &anim_cls);
    if (anim) {
        if (anim_cls == HN_ANIM_AMBIENT ||
            (anim_cls == HN_ANIM_INTERACTIVE && hn_media_light_render_ok(rt->ctx))) {
            if (pend < 1) pend = 1;
        } else {
            pend = 2;
        }
    }

    /* 轮询表(懒建: 内容变化后重建)。时钟域 = poll_clock_ms(帧步进), 秒换算 */
    hn_doc *doc = hn_context_doc(rt->ctx);
    if (!rt->poll_built) {
        rt->poll_n = 0;
        double now = rt->poll_clock_ms / 1000.0;
        for (int i = 0;; i++) {
            const char *id = NULL;
            int ms = 0;
            if (!hn_doc_poll_at(doc, i, &id, &ms)) break;
            if (rt->poll_n >= 16) break;
            hn_node *n = hn_doc_find_by_id(doc, id);
            if (n) {
                double sec = ms > 0 ? ms / 1000.0 : 1.0;
                if (sec < 0.05) sec = 0.05;
                rt->polls[rt->poll_n].node = n;
                rt->polls[rt->poll_n].due = now + sec;
                rt->polls[rt->poll_n].sec = sec;
                rt->poll_n++;
            }
        }
        rt->poll_built = 1;
    }
    double now = rt->poll_clock_ms / 1000.0;
    double next = -1;
    for (int i = 0; i < rt->poll_n; i++) {
        if (now >= rt->polls[i].due) {
            if (hx_perform(rt, rt->polls[i].node))
                pend = 2;      /* hx 换了内容: 档位抬满(帧尾一次渲染) */
            rt->polls[i].due = now + rt->polls[i].sec;
        }
        double wait = rt->polls[i].due - now;
        if (next < 0 || wait < next) next = wait;
    }

    /* 帧尾一次渲染(见帧头的合并说明): 档位高者胜 —— 全路径含 relayout,
       ambient 轻路径免 relayout, 与旧口径的路径选择逐情形一致。 */
    if (pend) {
        if (pend == 1) rt_render_ambient(rt);
        else rt_render(rt);
        if (repaint) *repaint = 1;
    }

    /* ---- 唤醒节拍合成(取各信号的最小者; -1 = 无事可睡) ----
       RAF 待决          → 16 (活画面: 60fps 帧循环)
       interactive 动画  → 16 (过渡/keyframes/媒体/平滑滚动: 每帧写样式)
       仅 ambient 动画   → HN_ANIM_FPS 节拍(缺省 33 ≈ 30fps; 桌宠环境
                           动画肉眼平滑度无损, layout+渲染次数减半)
       定时器表非空      → min(最近到期-现在, 250) —— 上限治理: 无 RAF/
                           动画时不再为一张远期定时器表 60fps 空转(5s 的
                           setTimeout 页面 4Hz 醒来, 到期即触发后自然回
                           短节拍); 封顶 250 与 hn_app 的空闲 pump 同拍,
                           mtime watch 的 300ms 节奏不被拖慢
       hx 轮询期限       → 距最近到期的毫秒数
       全无              → -1 (宿主按自己的空闲节奏低频醒来) */
    int beat = -1;
#ifdef HN_HAVE_QUICKJS
    if (rt->raf_n > 0) beat = 16;
    if (rt->timer_n > 0) {
        double dmin = -1;
        for (int i = 0; i < 64; i++) {
            if (!rt->timers[i].id) continue;
            double d = rt->timers[i].due - rt->clock_ms;
            if (d < 0) d = 0;                        /* 预算耗尽没跑成的: 立刻再约 */
            if (dmin < 0 || d < dmin) dmin = d;
        }
        if (dmin >= 0) {
            int tb = (int)dmin;
            if (tb < 1) tb = 1;
            if (tb > 250) tb = 250;                  /* 定时器不钉死帧节拍 */
            if (beat < 0 || tb < beat) beat = tb;
        }
    }
#endif
    if (anim) {
        int ab = (anim_cls == HN_ANIM_AMBIENT) ? ambient_beat_ms() : 16;
        if (beat < 0 || ab < beat) beat = ab;
    }
    if (next > 0) {
        int pb = (int)(next * 1000.0);
        if (beat < 0 || pb < beat) beat = pb;
    }
    return beat;
}

int hn_rt_pointer_move(hn_rt *rt, float x, float y) {
    int ev_cls = rt_events_advance(rt);
    if (!rt) return 0;
    /* input[type=range] 拖拽: 按住期间持续按 x 设值(松手/命中漂移出
       元素则结束 —— id 重查不到也算结束, 文档替换安全)。 */
    if (rt->range_drag) {
        hn_node *inp = hn_doc_find_by_id(hn_context_doc(rt->ctx), rt->range_id);
        if (!inp || hn_node_input_kind(inp) != HN_IN_RANGE) {
            rt->range_drag = 0;
            rt->range_id[0] = 0;
        } else {
            rt_range_set_from_x(rt, inp, x);
            hx_change_maybe(rt, inp);   /* hx-trigger="change" 载体(拖拽续) */
            return rt_render(rt);
        }
    }
    /* 喂坐标(在 hover 无变化的早退之前 —— 指针在同一元素内滑动时
       hover 不变, 但网格 hn-mesh-look 的指向每一步都要跟上)。 */
    hn_context_set_pointer(rt->ctx, x, y);
    hn_node *n = hn_context_hit_node(rt->ctx, x, y);
    int rendered = 0;
    if (n != rt->hover) {
        rt->hover = n;
        hn_context_set_hover(rt->ctx, n);
        rendered = rt_render(rt);
    }
    /* JS mousemove: 仅存在监听器时派发(否则零成本, hover 语义照旧);
       节流至"坐标或命中元素变化" —— 同一位置的重复移动不重发。 */
    if (rt_js_has_listener(rt, "mousemove") &&
        (!rt->mm_valid || rt->mm_x != x || rt->mm_y != y || rt->mm_hit != n)) {
        rt->mm_valid = 1;
        rt->mm_x = x;
        rt->mm_y = y;
        rt->mm_hit = n;
        if (rt_js_dispatch(rt, "mousemove", n, x, y, NULL, 0, 0))
            rendered = rt_render(rt);
    }
    return rendered;
}

/* 绘制挂起/冲刷(与 app 壳的 blit 合并配套):
   defer(1) 期间所有输入路径的 rt_render 只标脏 —— 爆发输入的中间态
   像素从不被呈现, N 次 layout+raster 合 1;
   flush 在 defer 关闭后(或任意时刻 —— 内部先摘 defer)把脏帧一次画掉,
   返回 1 表示真的渲染过(调用方据此 blit)。 */
void hn_rt_paint_defer(hn_rt *rt, int on) {
    if (!rt) return;
    rt->paint_defer = on;
    /* 注意: 关闭**不清** dirty —— 壳的口径是 defer(0) → flush, 脏标记
       只能由 flush 消费; 提前清掉会让挂起的帧永远刷不出来。 */
}

int hn_rt_paint_flush(hn_rt *rt) {
    if (!rt || !rt->paint_dirty) return 0;
    rt->paint_dirty = 0;
    int saved = rt->paint_defer;
    rt->paint_defer = 0;
    int r = rt_render(rt);
    rt->paint_defer = saved;
    return r;
}

int hn_rt_scroll(hn_rt *rt, float dx, float dy, float x, float y) {
    (void)dx;
    if (!rt) return 0;
    /* <select> 弹层滚动: (x,y) 在打开的弹层/盒区域时拦截滚轮 = 滚动选项窗 */
    {
        hn_node *stack[256];
        int sp2 = 0, guard2 = 0;
        stack[sp2++] = hn_context_doc(rt->ctx)->root;
        while (sp2 > 0 && guard2++ < 100000) {
            hn_node *it = stack[--sp2];
            if (it->select_open && it->tag && !strcmp(it->tag, "select") &&
                x >= it->bx - 2 && x <= it->bx + it->bw + 2 &&
                y >= it->by - 2 && y <= it->by + it->bh + 200) {
                int n_opt = 0;
                for (hn_node *ch = it->first; ch; ch = ch->next)
                    if (ch->kind == HN_ELEM && ch->tag && !strcmp(ch->tag, "option"))
                        n_opt++;
                it->select_row0 += (dy > 0) ? -3 : 3;
                if (it->select_row0 > n_opt - 1) it->select_row0 = n_opt - 1;
                if (it->select_row0 < 0) it->select_row0 = 0;
                return rt_render(rt);
            }
            for (hn_node *ch = it->first; ch; ch = ch->next)
                if (ch->kind == HN_ELEM && sp2 < 256) stack[sp2++] = ch;
        }
    }
    hn_node *n = hn_context_hit_node(rt->ctx, x, y);
    if (n && hn_node_scroll_by(n, 0, -dy * 18.0f)) {
        /* JS scroll: 节流每帧至多一次(hn_rt_frame 重臂); 无监听器零成本 */
        if (rt->scroll_armed) {
            rt->scroll_armed = 0;
            rt_js_dispatch(rt, "scroll", n, x, y, NULL, 0, 0);
        }
        return rt_render(rt);
    }
    return 0;
}

/* range 点击/拖拽的设值: x 在元素盒内的水平占比 → [min,max] 线性映射
   → 写回 value 属性(H5 标准)。 */
static void rt_range_set_from_x(hn_rt *rt, hn_node *inp, float x) {
    float bx = 0, by = 0, bw = 0, bh = 0;
    hn_node_box(inp, &bx, &by, &bw, &bh);
    if (bw <= 0) return;
    float vmin = 0, vmax = 100;
    const char *as = hn_node_attr(inp, "min");
    const char *ax = hn_node_attr(inp, "max");
    if (as && *as) vmin = (float)atof(as);
    if (ax && *ax) vmax = (float)atof(ax);
    if (vmax <= vmin) vmax = vmin + 100.0f;
    float frac = (x - bx) / bw;
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    char vbuf[32];
    snprintf(vbuf, sizeof(vbuf), "%g", (double)(vmin + frac * (vmax - vmin)));
    hn_node_set_attr(inp, "value", vbuf);
}

/* 原生激活行为(HTML5 默认动作; 浏览器语义 = 事件派发后、无 preventDefault
   才执行): checkbox 切换 / radio 同名组互斥 / range 点击设值(拖拽在
   pointer move 里续) / <summary> 切换祖先 <details> 的 open。
   返回 1 = 采取了默认动作(调用方需重渲染)。click_x < 0 = 无坐标来源
   (el.click()), range 不动滑块。 */
static int rt_native_click(hn_rt *rt, hn_node *n, float click_x) {
    if (!rt || !n) return 0;
    /* <select>: 点击开/合弹层(node 标记; 绘制/命中两处消费)。
       打开时滚动窗围绕 selected 行初始化(键盘导航的起点)。 */
    {
        hn_node *it = n;
        for (int hop = 0; it && hop < 8; hop++, it = it->parent) {
            const char *t = hn_node_tag(it);
            if (t && !strcmp(t, "select")) {
                it->select_open = !it->select_open;
                if (it->select_open) {
                    int n_opt = 0, sel_i = 0;
                    for (hn_node *ch = it->first; ch; ch = ch->next) {
                        if (ch->kind != HN_ELEM || !ch->tag || strcmp(ch->tag, "option")) continue;
                        if (hn_node_attr(ch, "selected")) sel_i = n_opt;
                        n_opt++;
                    }
                    it->select_hi = -1;
                    /* v1 简化: 弹层可见行数 ≈ 视口高/行高 - 1, row0 令 selected 居中 */
                    float rowh = it->style.font_size * 1.6f;
                    int visible = rowh > 0 ? (int)(hn_context_viewport_h(rt->ctx) / rowh) : n_opt;
                    if (visible < 1) visible = 1;
                    int row0 = sel_i - visible / 2;
                    if (row0 > n_opt - visible) row0 = n_opt - visible;
                    if (row0 < 0) row0 = 0;
                    it->select_row0 = row0;
                } else {
                    it->select_row0 = 0;
                    it->select_hi = -1;
                }
                return 1;
            }
        }
    }
    hn_node *inp = n;
    for (int hop = 0; inp && hop < 4; hop++, inp = inp->parent) {
        if (!hn_node_is_input(inp)) continue;
        int k = hn_node_input_kind(inp);
        if (k == HN_IN_CHECKBOX) {
            const char *cv = hn_node_attr(inp, "checked");
            int on = cv && strcmp(cv, "false") != 0 && strcmp(cv, "0") != 0;
            if (on) hn_node_remove_attr(inp, "checked");
            else hn_node_set_attr(inp, "checked", "");
            return 1;
        }
        if (k == HN_IN_RADIO) {
            const char *name = hn_node_attr(inp, "name");
            hn_node *scope = hn_context_doc(rt->ctx)->root;
            for (hn_node *a = inp->parent; a; a = a->parent) {
                const char *t = hn_node_tag(a);
                if (t && !strcmp(t, "form")) { scope = a; break; }
            }
            hn_node *stack[256];
            int sp = 0, guard = 0;
            stack[sp++] = scope;
            while (sp > 0 && guard++ < 100000) {
                hn_node *it = stack[--sp];
                if (hn_node_is_input(it) && it != inp &&
                    hn_node_input_kind(it) == HN_IN_RADIO && name) {
                    const char *nm = hn_node_attr(it, "name");
                    if (nm && !strcmp(nm, name))
                        hn_node_remove_attr(it, "checked");
                }
                for (hn_node *ch = it->first; ch; ch = ch->next)
                    if (ch->kind == HN_ELEM && sp < 256) stack[sp++] = ch;
            }
            hn_node_set_attr(inp, "checked", "");
            return 1;
        }
        if (k == HN_IN_RANGE) {
            const char *id = hn_node_attr(inp, "id");
            if (id && *id && click_x >= 0) {
                rt_range_set_from_x(rt, inp, click_x);
                snprintf(rt->range_id, sizeof(rt->range_id), "%s", id);
                rt->range_drag = 1;
            }
            return 1;
        }
        break;   /* 其它 input 类型不吞点击 */
    }
    {
        hn_node *it = n;
        hn_node *summary = NULL;
        for (int hop = 0; it && hop < 16; hop++, it = it->parent) {
            const char *tag = hn_node_tag(it);
            if (tag && !strcmp(tag, "summary")) { summary = it; break; }
            if (tag && !strcmp(tag, "details")) break;   /* 先见 details = 点在正文 */
        }
        if (summary && summary->parent) {
            const char *ptag = hn_node_tag(summary->parent);
            if (ptag && !strcmp(ptag, "details")) {
                const char *open = hn_node_attr(summary->parent, "open");
                if (open && strcmp(open, "false") != 0 && strcmp(open, "0") != 0)
                    hn_node_remove_attr(summary->parent, "open");
                else
                    hn_node_set_attr(summary->parent, "open", "");
                return 1;
            }
        }
    }
    return 0;
}

/* ---------------- 项目内导航(多 html 项目模式) ----------------
 * 页面读取相对 cwd(壳已 chdir 项目根); 带协议/#锚/不存在的路径都不是
 * 项目内页面 —— 返回 0, 调用方回落系统打开。换装走 hn_rt_render(整文档
 * 替换, 与热更新同一通道), 新文档从滚动 0 开始。 */
static int page_read(const char *rel, char **out, size_t *out_len) {
    if (!rel || !*rel) return 0;
    if (strstr(rel, "://")) return 0;                 /* 外部地址 */
    if (rel[0] == '#') return 0;                      /* 锚点(引擎内滚动) */
    struct stat st;
    if (stat(rel, &st) != 0 || !S_ISREG(st.st_mode)) return 0;
    size_t len = 0;
    char *html = read_all(rel, &len);
    if (!html) return 0;
    *out = html;
    *out_len = len;
    return 1;
}

int hn_rt_navigate(hn_rt *rt, const char *url) {
    if (!rt || !rt->page_path || !url || !*url) return 0;   /* 内联文档无导航 */
    if (strstr(url, "://") || url[0] == '#') return 0;
    char *html = NULL; size_t len = 0;
    if (!page_read(url, &html, &len)) return 0;
    if (rt->back_n == rt->back_cap) {
        int nc = rt->back_cap ? rt->back_cap * 2 : 8;
        char **nb = (char **)realloc(rt->back, sizeof(char *) * (size_t)nc);
        if (!nb) { free(html); return 0; }
        rt->back = nb; rt->back_cap = nc;
    }
    if (rt->back_n >= 64) { free(html); return 0; }         /* 栈上限防失控 */
    rt->back[rt->back_n++] = rt->page_path;                 /* 当前页入栈 */
    if (hn_rt_render(rt, html, len)) {
        free(html);
        rt->page_path = strdup(url);
        rt_render_trace("navigate");
        return 1;
    }
    free(html);
    /* 换装失败(半截文件/解析失败): 回滚栈顶, 旧文档原样在屏 */
    rt->page_path = rt->back[--rt->back_n];
    return 0;
}

int hn_rt_back(hn_rt *rt) {
    if (!rt || rt->back_n == 0 || !rt->page_path) return 0;
    char *prev = rt->back[--rt->back_n];
    char *cur = rt->page_path;
    char *html = NULL; size_t len = 0;
    if (!page_read(prev, &html, &len)) {
        rt->back[rt->back_n++] = prev;
        return 0;
    }
    if (hn_rt_render(rt, html, len)) {
        rt->page_path = prev;
        free(cur); free(html);
        rt_render_trace("back");
        return 1;
    }
    free(html);
    rt->page_path = cur;
    rt->back[rt->back_n++] = prev;
    return 0;
}

int hn_rt_button(hn_rt *rt, int down, float x, float y, int button) {
    int ev_cls = rt_events_advance(rt);
    if (!rt) return 0;
    /* 按下态先于早退喂入: up 事件也要清 pointer_down(轨道相机松手进惯性),
       但 click 管线仍只在 down 跑(原语义, 不双发)。 */
    hn_context_set_pointer_button(rt->ctx, down && button == 0);
    if (!down || button != 0) {
        rt->range_drag = 0;   /* 松手结束 range 拖拽 */
        return 0;
    }
    const char *id = hn_context_hit_test(rt->ctx, x, y);
    hn_node *n = hn_context_hit_node(rt->ctx, x, y);
    printf("[click] (%.0f,%.0f) -> <%s id=%s>\n", x, y,
           n ? hn_node_tag(n) : "?", id ? id : "(none)");
    rt->focus = n ? hn_node_ancestor_input(n) : NULL;
    if (rt->focus) hn_context_set_focus(rt->ctx, rt->focus);
    /* JS click: 命中链冒泡派发(目标→祖先 id 链→document→window)。无监听
       器时在注册表扫描处零成本早退 —— 无 JS 页面行为逐位不变(hx 页零回归
       锚)。preventDefault → 跳过 hx 载体与链接激活(默认行为被取消),
       监听器可能已改文档, 重渲染后返回。 */
    /* <select> 弹层开合(先于一般命中 —— 弹层盖在一切之上):
       命中行 = 选中该 option(selected 属性迁移)并关闭; 其余点击 = 仅关闭。
       (行几何与 paint_select_popups 同一套: 行高 = 字号×1.6, 盒下缘,
        视口装不下上翻。) */
    {
        hn_node *opened = NULL;
        hn_node *stack[256];
        int sp2 = 0, guard2 = 0;
        stack[sp2++] = hn_context_doc(rt->ctx)->root;
        while (sp2 > 0 && guard2++ < 100000) {
            hn_node *it = stack[--sp2];
            if (it->select_open && it->tag && !strcmp(it->tag, "select")) { opened = it; break; }
            for (hn_node *ch = it->first; ch; ch = ch->next)
                if (ch->kind == HN_ELEM && sp2 < 256) stack[sp2++] = ch;
        }
        if (opened) {
            float fs = opened->style.font_size > 8 ? opened->style.font_size : 12;
            float row_h = fs * 1.6f;
            /* 收 option + 几何(与 paint 同式) */
            hn_node *opts[64];
            int n_opt = 0;
            for (hn_node *ch = opened->first; ch; ch = ch->next) {
                if (ch->kind == HN_ELEM && ch->tag && !strcmp(ch->tag, "option") &&
                    n_opt < 64)
                    opts[n_opt++] = ch;
            }
            float x0 = opened->bx, y0 = opened->by + opened->bh;
            int visible = n_opt;
            if (y0 + row_h * visible > hn_context_viewport_h(rt->ctx) - 4) {
                visible = (int)((hn_context_viewport_h(rt->ctx) - 4 - y0) / row_h);
                if (visible < 1) {
                    y0 = opened->by;
                    visible = (int)(y0 / row_h);
                    if (visible > n_opt) visible = n_opt;
                    y0 = opened->by - row_h * visible;
                } else if (visible > n_opt) visible = n_opt;
            }
            /* 滚动窗钳制(与 paint_select_popups 同式) */
            if (opened->select_row0 < 0) opened->select_row0 = 0;
            if (opened->select_row0 > n_opt - visible) opened->select_row0 = n_opt - visible;
            if (opened->select_row0 < 0) opened->select_row0 = 0;
            float lw = opened->bw > 120 ? opened->bw : 120;
            int in_list = (x >= x0 && x <= x0 + lw && y >= y0 && y < y0 + row_h * visible);
            int in_box = (x >= opened->bx && x <= opened->bx + opened->bw &&
                          y >= opened->by && y <= opened->by + opened->bh);
            if (in_list) {
                int pick = opened->select_row0 + (int)((y - y0) / row_h);
                if (pick >= 0 && pick < visible && pick < n_opt) {
                    for (int i = 0; i < n_opt; i++)
                        hn_node_remove_attr(opts[i], "selected");
                    hn_node_set_attr(opts[pick], "selected", "");
                    hx_change_maybe(rt, opened);   /* hx-trigger="change" 载体 */
                }
                opened->select_open = 0;
                opened->select_row0 = 0;
                opened->select_hi = -1;
                return rt_render(rt);
            }
            if (in_box) { opened->select_open = 0; opened->select_row0 = 0; opened->select_hi = -1; return rt_render(rt); }
            if (!in_box) { opened->select_open = 0; opened->select_row0 = 0; opened->select_hi = -1; return rt_render(rt); }
            /* 点在盒上 = 收起(切换) */
            opened->select_open = 0;
            return rt_render(rt);
        }
    }
    /* 点击派发(监听器 → _pd → 原生默认动作, 全在 dispatch 内完成):
       返回非 0 = 有 JS 消费或原生动作已执行 —— 重渲染, hx/链接跳过。 */
    int had_listeners = rt_js_has_listener(rt, "click");
    if (rt_js_dispatch(rt, "click", n, x, y, NULL, 0, 1))
        return rt_render(rt);
    if (had_listeners)
        /* 监听器跑过且未 preventDefault: 可能已改样式/文档 —— 全路径重算。
           handwired #1: 新目的地(过渡目标)在事件时刻启动, 不等下帧。
           (无 hx 载体时旧口径在这里 return 0, 过渡晚一拍才启动 —— 实测
           事件后 500ms 步进只走到 ~0。) */
        return rt_render(rt);
    hn_node *carrier = hx_carrier(n);
    if (carrier) {
        /* hx-trigger 语义门: 显式声明了触发器且不含 "click"(如 change 载体)
           → 点击不执行(浏览器同序: 触发器决定触发事件) */
        const char *trig = hn_node_attr(carrier, "hx-trigger");
        if (trig && *trig && !hx_trigger_has(trig, "click")) {
            /* 非 click 触发器的载体: 点击不触发请求 */
        } else {
            const char *verb = "GET", *url = NULL;
            hx_method_of(carrier, &verb, &url);
            printf("[hx] %s %s\n", verb, url ? url : "?");
            fflush(stdout);
            if (hx_perform(rt, carrier)) return rt_render(rt);
        }
    }
    /* 链接导航(md 直染的 a[href]): #锚引擎内滚进可视区;
       外部链接交系统默认方式打开(sys://open 同一出口), 不在窗内导航 */
    int lk = hn_context_activate_link(rt->ctx, x, y);
    if (lk == HN_LINK_ANCHOR) return rt_render(rt);
    if (lk == HN_LINK_EXTERNAL) {
        char url[2048] = { 0 };
        hn_doc_take_pending_link(hn_context_doc(rt->ctx), url, sizeof url);
        if (url[0]) {
            /* 项目内相对路径(cwd 下存在的文件) → 同窗换装(多 html 项目模式);
               http(s):// 等外部地址照旧交系统打开 */
            if (hn_rt_navigate(rt, url)) return rt_render(rt);
            hn_sys_open_url(url);
        }
        return rt_render(rt);
    }
    return 0;
}

/* ---------------- 输入编辑(caret 感知的文本编辑模型) ----------------
 *
 * 旧实现的三个缺陷(本轮修): ① 追加写的是 `hn_node_attr(n,"value")`(属性),
 * 而 hn_node_set_value 写的是内部 value 缓冲 + 移动 caret —— 但 attr 读回
 * 优先看内部缓冲(hn_node_value), 于是每次"追加"都从**属性原值**重算,
 * 连打两个字符只剩最后一个(实测: h,i,! → "!"); ② 无 caret 概念, 只能
 * 在末尾增删; ③ 删除分支比对 key==3, 而 HNP_KEY_BACKSPACE 实际是 4
 * (3 是 TAB) —— 对照 hnp_key 枚举即可看出。
 * 本段按"基础值 + caret"模型重写, 由 hn_rt_key / hn_rt_ime_* / hn_rt_edit
 * 共用。全部走 hn_node_value/hn_node_set_value/hn_node_caret/
 * hn_node_set_caret(引擎公开 API)。 */

/* UTF-8 边界辅助 */
static int u8_prev(const char *s, int i) {           /* i 之前一个字符的起点 */
    if (i <= 0) return 0;
    int j = i - 1;
    while (j > 0 && ((unsigned char)s[j] & 0xC0) == 0x80) j--;
    return j;
}
static int u8_next(const char *s, int len, int i) {  /* i 之后一个字符的起点 */
    if (i >= len) return len;
    int j = i + 1;
    while (j < len && ((unsigned char)s[j] & 0xC0) == 0x80) j++;
    return j;
}

/* caret 位置读当前值(字符串 + 长度; 指向节点内部缓冲, 生命周期 = 本次调用) */
static const char *rt_focus_text(hn_rt *rt, int *len) {
    *len = 0;
    if (!rt || !rt->focus) return "";
    size_t vl = 0;
    const char *v = hn_node_value(rt->focus, &vl);
    if (!v) { v = ""; vl = 0; }
    if (vl > 0x7FFFFFFF) vl = 0x7FFFFFFF;
    *len = (int)vl;
    return v;
}

/* 在 caret 处插入串, 同时删除 caret 前 del_before 字节与 caret 后 del_after
   字节(Backspace = del_before; Delete = del_after; 普通插入 = 0/0)。
   caret 落在插入串之后。 */
static int rt_edit_insert2(hn_rt *rt, const char *ins, int del_before, int del_after) {
    int vl = 0;
    const char *v = rt_focus_text(rt, &vl);
    int caret = hn_node_caret(rt->focus);
    if (caret < 0) caret = 0;
    if (caret > vl) caret = vl;
    int db = del_before < 0 ? 0 : del_before;
    int da = del_after < 0 ? 0 : del_after;
    if (db > caret) db = caret;
    if (da > vl - caret) da = vl - caret;
    size_t in = ins ? strlen(ins) : 0;
    if ((size_t)vl + in + 1 > 65536) return 0;   /* 防呆上限 */
    static char buf[65536];                      /* 单线程主循环; 静态免堆分配 */
    int head = caret - db;                       /* 保留的前段(= 插入点) */
    memcpy(buf, v, (size_t)head);
    if (in) memcpy(buf + head, ins, in);
    size_t tail_off = (size_t)caret + (size_t)da;
    memcpy(buf + head + in, v + tail_off, (size_t)(vl - (int)tail_off));
    size_t nl = (size_t)head + in + (size_t)(vl - (int)tail_off);
    buf[nl] = 0;
    hn_node_set_value(rt->focus, buf, nl);   /* set_value 把 caret 移到末尾 */
    hn_node_set_caret(rt->focus, head + (int)in);
    /* value 变更 → 焦点元素派发 input(键入/删除/粘贴共用此路径; 无监听器
       时派发在注册表扫描处零成本早退) */
    rt_js_dispatch(rt, "input", rt->focus, 0, 0, NULL, 0, 0);
    hx_change_maybe(rt, rt->focus);   /* hx-trigger="change" 的载体(编辑即触发) */
    return 1;
}
static int rt_edit_insert(hn_rt *rt, const char *ins, int del_bytes) {
    return rt_edit_insert2(rt, ins, 0, del_bytes);   /* 兼容旧调用: caret 后删 */
}

/* IME 基础值: 组字开始时的已上屏值。组字期间 value = base + preedit。 */
static char g_ime_base[65536];
static int g_ime_base_len;
static int g_ime_active;

int hn_rt_ime_preedit(hn_rt *rt, const char *preedit, int cursor) {
    (void)cursor;
    if (!rt || !rt->focus) return 0;
    if (hn_node_input_kind(rt->focus) == HN_IN_CHECKBOX ||
        hn_node_input_kind(rt->focus) == HN_IN_RADIO) return 0;
    if (!g_ime_active) {
        /* 组字开始: 记基础值(此后 preedit 变化都从它重算, 不叠加) */
        int vl = 0;
        const char *v = rt_focus_text(rt, &vl);
        if ((size_t)vl >= sizeof(g_ime_base)) vl = (int)sizeof(g_ime_base) - 1;
        memcpy(g_ime_base, v, (size_t)vl);
        g_ime_base[vl] = 0;
        g_ime_base_len = vl;
        g_ime_active = 1;
    }
    if (!preedit || !*preedit) {
        /* 组字结束/取消: 恢复基础值 */
        hn_node_set_value(rt->focus, g_ime_base, (size_t)g_ime_base_len);
        g_ime_active = 0;
        return rt_render(rt);
    }
    /* 预编辑串直插: value = base + preedit(caret 落其末尾) */
    static char buf[65536];
    size_t pl = strlen(preedit);
    if ((size_t)g_ime_base_len + pl + 1 > sizeof(buf)) pl = sizeof(buf) - 1 - (size_t)g_ime_base_len;
    memcpy(buf, g_ime_base, (size_t)g_ime_base_len);
    memcpy(buf + g_ime_base_len, preedit, pl);
    buf[g_ime_base_len + pl] = 0;
    hn_node_set_value(rt->focus, buf, (size_t)g_ime_base_len + pl);
    return rt_render(rt);
}

int hn_rt_ime_commit(hn_rt *rt, const char *text) {
    if (!rt || !rt->focus) return 0;
    if (!g_ime_active) {
        /* 无组字态的提交(直接字符输入经 IME 通道): 当普通插入 */
        return text && *text ? (rt_edit_insert(rt, text, 0) ? rt_render(rt) : 0) : 0;
    }
    if (!text) text = "";
    static char buf[65536];
    size_t tl = strlen(text);
    if ((size_t)g_ime_base_len + tl + 1 > sizeof(buf)) tl = sizeof(buf) - 1 - (size_t)g_ime_base_len;
    memcpy(buf, g_ime_base, (size_t)g_ime_base_len);
    memcpy(buf + g_ime_base_len, text, tl);
    buf[g_ime_base_len + tl] = 0;
    hn_node_set_value(rt->focus, buf, (size_t)g_ime_base_len + tl);
    /* caret 落上屏串末尾(set_value 已是末尾), 清基础态; value 变更 → input */
    g_ime_active = 0;
    g_ime_base_len = 0;
    g_ime_base[0] = 0;
    rt_js_dispatch(rt, "input", rt->focus, 0, 0, NULL, 0, 0);
    hx_change_maybe(rt, rt->focus);   /* hx-trigger="change" 的载体(编辑即触发) */
    return rt_render(rt);
}

int hn_rt_focus_box(hn_rt *rt, int *x, int *y, int *w, int *h) {
    if (x) *x = 0;
    if (y) *y = 0;
    if (w) *w = 0;
    if (h) *h = 0;
    if (!rt || !rt->focus) return 0;
    float bx = 0, by = 0, bw = 0, bh = 0;
    hn_node_box(rt->focus, &bx, &by, &bw, &bh);
    if (bw <= 0 || bh <= 0) return 0;
    if (x) *x = (int)bx;
    if (y) *y = (int)by;
    if (w) *w = (int)bw;
    if (h) *h = (int)bh;
    return 1;
}

/* 编辑命令(⌘C/⌘V/⌘X/⌘A)。剪贴板读写复用 sys:// 同源后端 clip_get/clip_set。
   选区模型: 本引擎 v1 无字符级选区 —— COPY/CUT 操作"整个当前值"(文档
   语义的最小正确近似), SELECT_ALL 同口径(整值 = 全选); PASTE 在 caret
   处插入(替换语义留给后续选区模型)。 */
int hn_rt_edit(hn_rt *rt, int op, const char *text) {
    if (!rt || !rt->focus) return 0;
    int kind = hn_node_input_kind(rt->focus);
    if (kind == HN_IN_CHECKBOX || kind == HN_IN_RADIO) return 0;
    if (op == HN_EDIT_COPY) {
        int vl = 0;
        const char *v = rt_focus_text(rt, &vl);
        return (vl > 0 && hn_sys_clip_set(v)) ? 0 : 0;   /* 复制不改文档: 不重渲染 */
    }
    if (op == HN_EDIT_CUT) {
        int vl = 0;
        const char *v = rt_focus_text(rt, &vl);
        if (vl > 0) hn_sys_clip_set(v);
        hn_node_set_value(rt->focus, "", 0);
        hn_node_set_caret(rt->focus, 0);
        return rt_render(rt);
    }
    if (op == HN_EDIT_PASTE) {
        char tmp[8192];
        const char *src = text;
        if (!src) {
            tmp[0] = 0;
            if (!hn_sys_clip_get(tmp, sizeof(tmp))) return 0;
            src = tmp;
        }
        return (src && *src) ? (rt_edit_insert(rt, src, 0) ? rt_render(rt) : 0) : 0;
    }
    if (op == HN_EDIT_SELECT_ALL) {
        /* v1 无字符选区: 全选 = caret 归零的整值语义(复制/剪切即整值) */
        hn_node_set_caret(rt->focus, 0);
        return rt_render(rt);
    }
    return 0;
}

/* Tab 焦点轮换(文档序环形): 当前焦点在 inputs 里找下一个可聚焦控件。 */
static hn_node *rt_focus_next(hn_rt *rt, int back) {
    hn_doc *doc = hn_context_doc(rt->ctx);
    int n = 0;
    while (hn_doc_input_at(doc, n)) n++;
    if (n <= 0) return NULL;
    int cur = -1;
    for (int i = 0; i < n; i++)
        if (hn_doc_input_at(doc, i) == rt->focus) { cur = i; break; }
    for (int step = 1; step <= n; step++) {
        int i = cur < 0
            ? (back ? n - step : step - 1)
            : ((cur + (back ? -step : step)) % n + n) % n;
        hn_node *cand = hn_doc_input_at(doc, i);
        /* 按钮不进 Tab 焦点链(不可编辑; 与"点击不落焦点"同口径) */
        if (cand && !hn_node_is_disabled(cand) &&
            hn_node_input_kind(cand) != HN_IN_BUTTON) return cand;
    }
    return NULL;
}

int hn_rt_key(hn_rt *rt, int down, int key, const char *utf8) {
    int ev_cls = rt_events_advance(rt);
    if (!rt) return 0;
    hn_node *target = rt->focus ? rt->focus : hn_doc_root(hn_context_doc(rt->ctx));
    if (!down) {
        /* 松开方向键: 无动作(与既有口径一致, 按下才处理) */
    }
    /* <select> 弹层键盘导航(优先于一切聚焦逻辑): Up/Down 移高亮行,
       Enter 提交(迁移 selected), Esc 关闭。 */
    if (down && (key == HN_KEY_UP || key == HN_KEY_DOWN ||
                 key == HN_KEY_ENTER || key == HN_KEY_ESC)) {
        hn_node *opened = NULL;
        hn_node *stack[256];
        int sp2 = 0, guard2 = 0;
        stack[sp2++] = hn_context_doc(rt->ctx)->root;
        while (sp2 > 0 && guard2++ < 100000) {
            hn_node *it = stack[--sp2];
            if (it->select_open && it->tag && !strcmp(it->tag, "select")) { opened = it; break; }
            for (hn_node *ch = it->first; ch; ch = ch->next)
                if (ch->kind == HN_ELEM && sp2 < 256) stack[sp2++] = ch;
        }
        if (opened) {
            hn_node *opts[64];
            int n_opt = 0, sel_i = 0;
            for (hn_node *ch = opened->first; ch; ch = ch->next) {
                if (ch->kind != HN_ELEM || !ch->tag || strcmp(ch->tag, "option")) continue;
                if (n_opt < 64) opts[n_opt++] = ch;
                if (hn_node_attr(ch, "selected")) sel_i = n_opt - 1;
            }
            int hi = (opened->select_hi >= 0) ? opened->select_hi : sel_i;
            if (key == HN_KEY_UP && hi > 0) hi--;
            if (key == HN_KEY_DOWN && hi < n_opt - 1) hi++;
            if (key == HN_KEY_ENTER) {
                for (int i = 0; i < n_opt; i++) hn_node_remove_attr(opts[i], "selected");
                hn_node_set_attr(opts[hi], "selected", "");
                opened->select_open = 0;
                opened->select_row0 = 0;
                opened->select_hi = -1;
                return rt_render(rt);
            }
            if (key == HN_KEY_ESC) {
                opened->select_open = 0;
                opened->select_row0 = 0;
                opened->select_hi = -1;
                return rt_render(rt);
            }
            opened->select_hi = hi;
            /* 高亮行滚入可见窗(行高与 paint 同式) */
            float rowh = opened->style.font_size * 1.6f;
            int visible = rowh > 0 ? (int)(hn_context_viewport_h(rt->ctx) / rowh) : n_opt;
            if (visible < 1) visible = 1;
            if (hi < opened->select_row0) opened->select_row0 = hi;
            if (hi >= opened->select_row0 + visible) opened->select_row0 = hi - visible + 1;
            return rt_render(rt);
        }
    }
    if (down && rt->focus &&
        hn_node_input_kind(rt->focus) == HN_IN_RANGE &&
        (key == HN_KEY_LEFT || key == HN_KEY_RIGHT ||
         key == HN_KEY_UP || key == HN_KEY_DOWN)) {
        /* range 方向键调值(H5 标准): Left/Down = -step, Right/Up = +step
           (step 属性, 缺省 1; Shift = 10 倍) */
        float vmin = 0, vmax = 100, val = 0, step = 1;
        const char *as = hn_node_attr(rt->focus, "min");
        const char *ax = hn_node_attr(rt->focus, "max");
        const char *av = hn_node_attr(rt->focus, "value");
        const char *ast = hn_node_attr(rt->focus, "step");
        if (as && *as) vmin = (float)atof(as);
        if (ax && *ax) vmax = (float)atof(ax);
        if (av && *av) val = (float)atof(av);
        if (ast && *ast) step = (float)atof(ast);
        if (step <= 0) step = 1;
        if (key == HN_KEY_RIGHT || key == HN_KEY_UP) val += step;
        else val -= step;
        if (val < vmin) val = vmin;
        if (val > vmax) val = vmax;
        char vbuf[32];
        snprintf(vbuf, sizeof(vbuf), "%g", (double)val);
        hn_node_set_attr(rt->focus, "value", vbuf);
        return rt_render(rt);
    }
    if (!down) {
        /* keyup: 派发后无内建键语义(引擎只处理按下) */
        rt_js_dispatch(rt, "keyup", target, 0, 0, utf8, key, 0);
        return 0;
    }
    /* JS keydown: 焦点链冒泡派发(焦点→祖先→document→window)。IME 组字期间
       不派发(组字由 IME 事件独占; 平台在组字期本就不发本路径, 这里双保险,
       见 hnp_macos.c 的 IME 路由)。preventDefault → 跳过全部内建键行为
       (插字符/删除/焦点轮换/hx 提交), 监听器可能已改文档, 重渲染后返回。 */
    if (!g_ime_active && rt_js_dispatch(rt, "keydown", target, 0, 0, utf8, key, 0))
        return rt_render(rt);
    if (key == HN_KT_RETURN) {
        /* textarea: 回车插入换行(浏览器语义); input: hx 提交 */
        if (rt->focus && hn_node_input_kind(rt->focus) == HN_IN_TEXTAREA)
            return rt_edit_insert(rt, "\n", 0) ? rt_render(rt) : 0;
        hn_node *carrier = hx_carrier(target);
        if (carrier && hx_perform(rt, carrier)) return rt_render(rt);
        return 0;
    }
    if (key == HN_KT_TAB) {
        hn_node *nx = rt_focus_next(rt, 0);
        if (nx) {
            rt->focus = nx;
            hn_context_set_focus(rt->ctx, nx);
            return rt_render(rt);
        }
        return 0;
    }
    if (key == HN_KT_BACKSPACE && rt->focus) {
        int vl = 0;
        const char *v = rt_focus_text(rt, &vl);
        int caret = hn_node_caret(rt->focus);
        if (caret > 0) {
            int prev = u8_prev(v, caret);
            return rt_edit_insert2(rt, NULL, caret - prev, 0) ? rt_render(rt) : 0;
        }
        return 0;
    }
    if (key == HN_KT_DELETE && rt->focus) {
        int vl = 0;
        const char *v = rt_focus_text(rt, &vl);
        int caret = hn_node_caret(rt->focus);
        if (caret < vl) {
            int nx = u8_next(v, vl, caret);
            return rt_edit_insert2(rt, NULL, 0, nx - caret) ? rt_render(rt) : 0;
        }
        return 0;
    }
    if ((key == HN_KT_LEFT || key == HN_KT_RIGHT ||
         key == HN_KT_HOME || key == HN_KT_END) && rt->focus) {
        int vl = 0;
        const char *v = rt_focus_text(rt, &vl);
        int caret = hn_node_caret(rt->focus);
        int nc = caret;
        if (key == HN_KT_LEFT) nc = u8_prev(v, caret);
        else if (key == HN_KT_RIGHT) nc = u8_next(v, vl, caret);
        else if (key == HN_KT_HOME) nc = 0;
        else nc = vl;
        if (nc != caret) {
            hn_node_set_caret(rt->focus, nc);
            return rt_render(rt);
        }
        return 0;
    }
    /* 可打印字符/IME 文本 → 在 caret 处插入(组字中由 IME 事件独占: 平台
       在组字期间不发本路径, 见 hnp_macos.c 的 IME 路由) */
    if (utf8 && *utf8) {
        if (!rt->focus) return 0;
        return rt_edit_insert(rt, utf8, 0) ? rt_render(rt) : 0;
    }
    return 0;
}

char *hn_rt_dom(hn_rt *rt, int max_depth) {
    if (!rt) return NULL;
    hn_doc *doc = hn_context_doc(rt->ctx);
    hn_node *root = hn_doc_root(doc);
    if (!root) return NULL;
    size_t cap = 8192, n = 0;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    /* 深度优先缩进树 */
    typedef struct { hn_node *n; int d; } st_t;
    st_t stack[256];
    int sp = 0;
    stack[sp].n = root; stack[sp].d = 0; sp++;
    while (sp > 0) {
        st_t cur = stack[--sp];
        if (hn_node_tag(cur.n)) {
            const char *tag = hn_node_tag(cur.n);
            const char *id = hn_node_attr(cur.n, "id");
            const char *cls = hn_node_attr(cur.n, "class");
            if (cur.d <= max_depth) {
                while (n + 256 >= cap) { cap *= 2; out = (char *)realloc(out, cap); if (!out) return NULL; }
                n += (size_t)snprintf(out + n, cap - n, "%*s<%s%s%s%s>\n",
                                      cur.d * 2, "", tag ? tag : "?",
                                      id ? " id=" : "", id ? id : "",
                                      cls ? " class=" : "");
                if (cls) {
                    /* class 值可能含空格, 追加 */
                    size_t l = strlen(out);
                    n = l + (size_t)snprintf(out + l, cap - l, "\"%s\"", cls);
                }
            }
            for (hn_node *c = hn_node_first_child(cur.n); c; c = hn_node_next_sibling(c))
                if (sp < 256) { stack[sp].n = c; stack[sp].d = cur.d + 1; sp++; }
        }
    }
    out[n] = 0;
    return out;
}

char *hn_rt_text(hn_rt *rt, const char *element_id) {
    if (!rt || !element_id) return NULL;
    hn_doc *doc = hn_context_doc(rt->ctx);
    hn_node *n = hn_doc_find_by_id(doc, element_id);
    if (!n) return NULL;
    char *out = (char *)malloc(4096);
    if (!out) return NULL;
    size_t tn = hn_node_text_content(n, out, 4096);
    out[tn < 4096 ? tn : 4095] = 0;
    return out;
}

hn_doc *hn_rt_doc(hn_rt *rt) { return rt ? hn_context_doc(rt->ctx) : NULL; }

/* 文档清单访问器: 平台壳在开窗时读一次(透明/无边框/层级/跨空间/穿透),
   不重新解析 —— 与 hn_rt_open 内部同一份 hn_doc_manifest 语义。 */
void hn_rt_manifest(const hn_rt *rt, hn_manifest *out) {
    if (!out) return;
    memset(out, 0, sizeof(*out));
    hn_doc_manifest(rt ? hn_context_doc(rt->ctx) : NULL, out);
}

hn_context *hn_rt_context(hn_rt *rt) { return rt ? rt->ctx : NULL; }

int hn_rt_cmd_count(hn_rt *rt) {
    if (!rt) return 0;
    const hn_display_list *dl = hn_context_display_list(rt->ctx);
    return dl ? dl->count : 0;
}

int hn_rt_synthetic(hn_rt *rt, const char *kind, const char *target) {
    if (!rt || !kind) return 0;
    hn_doc *doc = hn_context_doc(rt->ctx);
    hn_node *n = target ? hn_doc_find_by_id(doc, target) : hn_doc_root(doc);
    if (!n) return 0;
    if (!strcmp(kind, "click")) {
        hn_node *carrier = hx_carrier(n);
        if (carrier && hx_perform(rt, carrier)) { rt_render(rt); return 1; }
    }
    return 0;
}

/* 渲染为 PNG 落盘(--shot / daemon shot op)。
   口径: 跟随 rt 的渲染缩放 —— 缺省 1.0, 所以既有探针(hnapp --shot /
   hncore render / pet_probe 等)的 360x360 确定性锚不变; 只有显式
   hn_rt_set_scale(2) 的路径(真窗壳 / hidpi_probe 离屏单测)才出 2x PNG。
   后端口径(与 rt_raster 一致): HN_GPU 未设(宿主槽 NULL)→ 恒 hnsoft 1x
   确定性 —— 既有探针零漂移; HN_GPU=metal **显式给出** → shot 也走 GPU
   (离屏与真窗同一后端; gpu_probe 借此拿 GPU 落盘做对照)。GPU 产出是
   **预乘 BGRA**, PNG 需要**非预乘 RGBA**(hnsoft 口径) → 反预乘换算
   (rgb×255/a, 换算链推导见 hn_gpu.c 头注 1): 底色不透明时 alpha 恒 255,
   往返无损; 半透明像素有 ±1 舍入(探针容差 ±40 吸收)。 */
int hn_rt_shot(hn_rt *rt, const char *path) {
    if (!rt || !path) return 0;
    /* 截图需要像素: 直呈模式下 present 帧不产生 CPU 位图 —— 本帧临时
       摘掉 present 层, rt_render 走 readback/CPU 出像素(下一帧恢复)。 */
    void *pl = rt->gpu_present_layer;
    rt->gpu_present_layer = NULL;
    rt_render(rt);
    rt->gpu_present_layer = pl;
    const hn_display_list *dl = hn_context_display_list(rt->ctx);
    if (!dl) return 0;
    int bw = rt_bitmap_w(rt), bh = rt_bitmap_h(rt);
    hn_color bg = rt->transparent ? 0x00000000u : 0x0B0E13FFu;
    unsigned char *rgba = NULL;
    if (hn_gpu_available(dl)) {
        unsigned char *bgra = NULL;
        if (hn_gpu_render_scaled(dl, bw, bh, bg, rt_scale(rt), &bgra) && bgra) {
            rgba = (unsigned char *)malloc((size_t)bw * bh * 4);
            if (rgba) {
                for (int i = 0; i < bw * bh; i++) {
                    unsigned a = bgra[i * 4 + 3];
                    rgba[i * 4 + 0] = a ? (unsigned char)((bgra[i * 4 + 2] * 255 + a / 2) / a) : 0;
                    rgba[i * 4 + 1] = a ? (unsigned char)((bgra[i * 4 + 1] * 255 + a / 2) / a) : 0;
                    rgba[i * 4 + 2] = a ? (unsigned char)((bgra[i * 4 + 0] * 255 + a / 2) / a) : 0;
                    rgba[i * 4 + 3] = (unsigned char)a;
                }
            }
            free(bgra);
        } else if (hn_context_native_suppressed(rt->ctx) == 0) {
            /* 原生 BITMAP 无字节: 压制原生路由重出显示列表再试 GPU
               (与 rt_raster 回退纪律一致 —— shot 拿完整帧) */
            hn_context_native_suppress(rt->ctx);
            hn_context_repaint(rt->ctx);
            dl = hn_context_display_list(rt->ctx);
            if (hn_gpu_available(dl)) {
                unsigned char *bgra2 = NULL;
                if (hn_gpu_render_scaled(dl, bw, bh, bg, rt_scale(rt), &bgra2) && bgra2) {
                    rgba = (unsigned char *)malloc((size_t)bw * bh * 4);
                    if (rgba) {
                        for (int i = 0; i < bw * bh; i++) {
                            unsigned a = bgra2[i * 4 + 3];
                            rgba[i * 4 + 0] = a ? (unsigned char)((bgra2[i * 4 + 2] * 255 + a / 2) / a) : 0;
                            rgba[i * 4 + 1] = a ? (unsigned char)((bgra2[i * 4 + 1] * 255 + a / 2) / a) : 0;
                            rgba[i * 4 + 2] = a ? (unsigned char)((bgra2[i * 4 + 0] * 255 + a / 2) / a) : 0;
                            rgba[i * 4 + 3] = (unsigned char)a;
                        }
                    }
                    free(bgra2);
                }
            }
        }
    }
    if (!rgba)
        rgba = hnsoft_render_scaled(dl, bw, bh, bg, rt_scale(rt));
    if (!rgba) return 0;
    size_t pn = 0;
    unsigned char *png = hnsoft_encode_png(rgba, bw, bh, &pn);
    free(rgba);
    if (!png) return 0;
    FILE *f = fopen(path, "wb");
    if (!f) { free(png); return 0; }
    fwrite(png, 1, pn, f);
    fclose(f);
    free(png);
    return 1;
}

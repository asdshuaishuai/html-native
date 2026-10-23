/* hn_context.c — 上下文: 组合文档/样式表, 布局编排, 命中测试, 热更新(set_text/swap/render), 清单解析 */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn_internal.h"

const char *hn_ua_css(void); /* hn_style.c */
static int anim_walk(hn_context *c, hn_node *n, float dt_ms, int *amb, int *inter);
/* hn-stagger 错档(anime.js stagger 语义采纳): 容器声明 hn-stagger="每项ms",
   子项入场按"前序同批入场兄弟数"错档 —— 负进度起步(t<0 停在起始态直至
   延迟耗尽, 与 animation-delay 同机制)。同批 = 前序兄弟 anim.entering
   仍在途; 已入场(fresh 消费完且 entering 结束)不计 —— 单条追加(聊天流)
   恒 idx=0 不受影响。返回错档延迟 ms。 */
static float hn_stagger_delay_for(const hn_node *n) {
    const hn_node *parent = n->parent;
    const char *stg = parent ? hn_node_attr(parent, "hn-stagger") : NULL;
    if (!stg || !*stg) return 0;
    float per = (float)strtod(stg, NULL);
    if (per <= 0) return 0;
    int idx = 0;
    for (const hn_node *s = n->prev; s; s = s->prev)
        if (s->kind == HN_ELEM && s->anim.entering) idx++;
    return (float)idx * per;
}
static void stream_anchor_walk(hn_context *c, hn_node *n); /* hn-stream 贴底/让出(滚动节) */
static int  stream_loop_walk(hn_node *n);   /* hn-stream-loop 滑窗摘除(滚动节) */
static int  stream_anim_walk(hn_node *n, float dt_ms); /* 平滑追上推进(滚动节) */
static int  scroll_anim_walk(hn_node *n, float dt_ms); /* 锚点平滑滚动推进(滚动节) */
static int  doc_scroll_to_now(hn_doc *doc, const char *id);  /* 锚点瞬跳(离屏口径) */
static int  doc_scroll_to_anim(hn_context *c, const char *id); /* 锚点武装(实时口径) */
static void gif_grave_flush(hn_context *c); /* GIF 逐出坟场冲刷(重绘后释放) */

hn_context *hn_context_create(void) {
    hn_context *c = calloc(1, sizeof(hn_context));
    if (!c) abort();
    c->tmp = hn_arena_create();
    c->mcache = hn_layout_mcache_create();   /* 文本测量缓存(freeze L2 前置) */
    /* UA 样式表永远排在最前(最低优先级) */
    const char *ua = hn_ua_css();
    hn_sheet *sh = hn_parse_css(ua, strlen(ua));
    hn_context_add_sheet(c, sh);
    return c;
}

void hn_context_destroy(hn_context *c) {
    if (!c) return;
    hn_context_lottie_clear(c);
    free(c->lot);
    hn_context_gif_clear(c);      /* GIF 缓存与 Lottie 同一纪律: 先于结构释放 */
    free(c->gifs);
    free(c->gif_grave);           /* 坟场槽位数组(对象已在 clear 里释放) */
    hn_media_sessions_clear(c);   /* 媒体会话持宿主句柄与帧缓存, 必须先于结构释放 */
    free(c->med_sessions);
    hn_canvas_dispose(c);
    hn_paint_frag_clear(c);         /* <canvas> op 账本持堆顶点/字符串, 随上下文销毁 */
    hn_layout_mcache_free(c->mcache);   /* 测量缓存持堆键副本, 随上下文销毁 */
    for (int i = 0; i < c->n_sheets; i++) hn_sheet_free(c->sheets[i]);
    free(c->sheets);
    hn_doc_free(c->doc);
    hn_arena_destroy(c->tmp);
    free(c->cmds);
    free(c);
}

void hn_context_set_doc(hn_context *c, hn_doc *doc) {
    hn_context_clear_exit_ghosts(c);   /* 换文档: ghost 节点指针随旧 arena 悬空 */
    /* 文档替换 = 旧文档的节点指针全部失效, 媒体会话先随旧文档销毁
       (与 Lottie 缓存的清理纪律一致 —— compact 内部也走这里)。
       <canvas> 账本同口径: 账本属于 canvas **元素**生命周期, 元素随
       文档销毁 = 账本清空; 样式重算/重放不经过这里, 不丢账。 */
    int replaced = (c->doc != doc);
    if (replaced) {
        hn_media_sessions_clear(c);
        hn_canvas_dispose(c);
    hn_paint_frag_clear(c);
        hn_context_gif_clear(c);   /* GIF 缓存独立于 DOM: 文档换掉即失效 */
    }
    c->doc = doc;
    /* Markdown 直染后处理: 在首次 layout 之前、<script> 执行之前
       (运行时的 run_doc_scripts 在 open/render 之后才跑)把 hn-md 源
       替换成普通 DOM 子树 —— md 一进来就变成 hn 编码。 */
    if (replaced) hn_md_apply(c->doc);
    if (replaced) hn_fmt_apply(c->doc);   /* JSON/CSV 直染: 同一装载批次 */
    /* 主题是渲染语义: 声明了 hn-theme 就装载基座(所有平台一致) */
    hn_context_apply_theme(c);
}

hn_doc *hn_context_doc(hn_context *c) { return c->doc; }

void hn_context_native_suppress(hn_context *c) {
    if (!c || c->native_suppressed) return;
    c->native_suppressed = 1;
    hn_paint_frag_clear(c);   /* 路由变化改变 cmds: L1 片段缓存必须重出 */
}

void hn_context_native_resume(hn_context *c) {
    if (c) c->native_suppressed = 0;
}

int hn_context_native_suppressed(const hn_context *c) {
    return c ? c->native_suppressed : 0;
}

void hn_context_add_sheet(hn_context *c, hn_sheet *sheet) {
    if (c->n_sheets == c->cap_sheets) {
        int nc = c->cap_sheets ? c->cap_sheets * 2 : 8;
        hn_sheet **ns = realloc(c->sheets, sizeof(hn_sheet *) * (size_t)nc);
        if (!ns) abort();
        c->sheets = ns;
        c->cap_sheets = nc;
    }
    c->sheets[c->n_sheets++] = sheet;
    c->style_serial++;   /* 表集变更: 级联记忆化全文档失配(下一遍整树重算) */
}

/* 一遍布局: 临时区/显示列表重建 → 级联 → 布局根。
   hn-stream-loop 摘除后要走第二遍 —— 摘除改变子项栈叠, 保留子项的
   坐标与容器 content_h 必须重新计算(见 hn_context_layout)。 */
/* <details> 折叠态(HTML5): 无 open 属性 → 仅 <summary> 保留, 其余子级
   display:none(标准: 内容不渲染; open 属性由 <summary> 点击/JS 切换)。
   在级联之后、布局之前跑 —— 直接改写计算样式的 display, 布局/绘制/命中
   三条管线自动一致; 每次布局重跑(级联会覆盖回来)。 */
static void details_apply_state(hn_context *c) {
    if (!c->doc || !c->doc->root) return;
    hn_node *stack[256];
    int sp = 0;
    stack[sp++] = c->doc->root;
    int guard = 0;
    while (sp > 0 && guard++ < 100000) {
        hn_node *n = stack[--sp];
        int closed_details = 0;
        if (n->kind == HN_ELEM && n->tag && !strcmp(n->tag, "details")) {
            const char *open = hn_node_attr(n, "open");
            closed_details = !(open && strcmp(open, "false") != 0 && strcmp(open, "0") != 0);
        }
        for (hn_node *ch = n->first; ch; ch = ch->next) {
            if (ch->kind != HN_ELEM) continue;
            if (closed_details) {
                const char *tag = ch->tag;
                if (!tag || strcmp(tag, "summary") != 0)
                    ch->style.display = HN_DISP_NONE;   /* 折叠: 非 summary 隐 */
            }
            if (sp < 256) stack[sp++] = ch;
        }
    }
}

static void layout_pass(hn_context *c) {
    hn_arena_destroy(c->tmp);
    c->tmp = hn_arena_create();
    c->n_cmds = 0;
    hn_style_compute_all(c);
    /* 透明背板: 样式重算会把底色从级联装回来, 必须在这之后重新剥离。
       这是"持久开关"语义的关键一步(见 hn_context_strip_root_background)。 */
    if (c->transparent) hn_strip_root_background_now(c);
    details_apply_state(c);   /* <details> 折叠态(级联后改写 display) */
    hn_layout_root(c);
}

void hn_context_layout(hn_context *c, float width, float height,
                       const hn_text_backend *backend) {
    if (!c->doc) return;
    c->vw = width;
    c->vh = height;
    if (backend) { c->tb = *backend; c->tb_valid = 1; } else { c->tb_valid = 0; }
    layout_pass(c);
    /* hn-stream-loop: 布局后、贴底锚定前数窗, 摘除超窗旧子元素。
       摘了就再走一遍布局: 保留子项的坐标与容器 content_h 还是按摘除前
       的子项算的, 不重算的话贴底会贴向已不存在的内容。摘除单调递减,
       第二遍必已稳定(不会再触发)。 */
    if (stream_loop_walk(c->doc->root))
        layout_pass(c);
    stream_anchor_walk(c, c->doc->root);   /* hn-stream: 贴底/让出锚定(绘制前) */
    hn_paint_root(c);
    c->dl.cmds = c->cmds;
    c->dl.count = c->n_cmds;
    gif_grave_flush(c);   /* 显示列表已重建: 逐出 GIF 的旧引用必已消亡, 放心释放 */
}

/* 布局不绘制(内部): rt_render 的全路径用 —— layout 后紧跟 anim_tick(0)
   重采样 + repaint, layout 内建的那次 paint 会被立刻整体覆盖, 纯浪费
   (交互帧的 paint 走了两遍; 显示列表构建是 CPU 大头之一)。语义不变:
   调用方必须在返回后自行 repaint 才有可消费的显示列表。 */
void hn_context_layout_nopaint(hn_context *c, float width, float height,
                               const hn_text_backend *backend) {
    if (!c->doc) return;
    c->vw = width;
    c->vh = height;
    if (backend) { c->tb = *backend; c->tb_valid = 1; } else { c->tb_valid = 0; }
    layout_pass(c);
    if (stream_loop_walk(c->doc->root))
        layout_pass(c);
    stream_anchor_walk(c, c->doc->root);
}

void hn_context_repaint(hn_context *c) {
    if (!c->doc) return;
    hn_paint_root(c);
    c->dl.cmds = c->cmds;
    c->dl.count = c->n_cmds;
    gif_grave_flush(c);
}

void hn_context_publish_display_list(hn_context *c) {
    if (!c || !c->doc) return;
    c->dl.cmds = c->cmds;
    c->dl.count = c->n_cmds;
}

/* 轻重画(ambient 帧): 不动级联/布局 —— 环境动画(纯 ext_clock 类)不改
   样式字段, 重建显示列表即可, 省掉整遍 relayout(30fps 帧循环下渲染
   成本减半; 静态立绘桌宠的 CPU 大头)。tmp 区与显示列表同生命周期
   (指令引用的顶点/文本副本从这里分配), 必须一并翻新, 否则逐帧无界
   增长。调用纪律: 仅在 anim_tick_ex 报 AMBIENT 时使用 —— 任何写样式
   字段的动画(interactive)必须走 hn_context_layout 全路径。 */
void hn_context_repaint_light(hn_context *c) {
    if (!c || !c->doc) return;
    hn_arena_destroy(c->tmp);
    c->tmp = hn_arena_create();
    hn_paint_root(c);
    c->dl.cmds = c->cmds;
    c->dl.count = c->n_cmds;
    gif_grave_flush(c);
}

void hn_context_set_images(hn_context *c, const hn_image_backend *backend) {
    c->images = backend;
}

void hn_context_set_assets(hn_context *c, const hn_asset_backend *backend) {
    c->assets = backend;
    hn_context_lottie_clear(c);     /* 后端换了, 旧解析结果可能不再可得 */
    hn_context_gif_clear(c);        /* 同上: GIF 解码结果同样随资产后端失效 */
}

const hn_asset_backend *hn_context_assets(hn_context *c) { return c ? c->assets : NULL; }

/* strdup 是 POSIX 而非 C99 —— 在 `-std=c99` 下 musl/Windows libc 不声明它,
   交叉构建直接报 implicit declaration。自己实现一份, 保持引擎纯 C99。 */
static char *hn_strdup(const char *s) {
    if (!s) return NULL;
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (!p) return NULL;
    memcpy(p, s, n + 1);
    return p;
}

/* ---- Lottie 缓存(按路径) ---- */

struct hn_lottie *hn_context_lottie(hn_context *c, const char *path) {
    if (!c || !path || !*path) return NULL;
    for (int i = 0; i < c->n_lot; i++)
        if (c->lot[i].path && !strcmp(c->lot[i].path, path)) return c->lot[i].lottie;
    struct hn_lottie *l = hn_lottie_load(c, path);
    if (!l) {
        /* 缓存失败结果(负缓存), 避免每帧重复尝试读文件 */
        if (c->n_lot == c->cap_lot) {
            int nc = c->cap_lot ? c->cap_lot * 2 : 8;
            void *nv = realloc(c->lot, sizeof(*c->lot) * (size_t)nc);
            if (!nv) return NULL;
            c->lot = nv; c->cap_lot = nc;
        }
        c->lot[c->n_lot].path = hn_strdup(path);
        c->lot[c->n_lot].lottie = NULL;
        c->n_lot++;
        return NULL;
    }
    if (c->n_lot == c->cap_lot) {
        int nc = c->cap_lot ? c->cap_lot * 2 : 8;
        void *nv = realloc(c->lot, sizeof(*c->lot) * (size_t)nc);
        if (!nv) { hn_lottie_free(l); return NULL; }
        c->lot = nv; c->cap_lot = nc;
    }
    c->lot[c->n_lot].path = hn_strdup(path);
    c->lot[c->n_lot].lottie = l;
    c->n_lot++;
    return l;
}

void hn_context_lottie_clear(hn_context *c) {
    if (!c || !c->lot) return;
    for (int i = 0; i < c->n_lot; i++) {
        free(c->lot[i].path);
        if (c->lot[i].lottie) hn_lottie_free(c->lot[i].lottie);
    }
    c->n_lot = 0;
}

/* ---- GIF 动图缓存(按路径, 带总量预算 + LRU 逐出) ----
   与 Lottie 缓存同构: 命中即返回(含负缓存); 解码走资产后端(引擎自身
   不做 I/O)。帧序列较大, 但一个文档里 GIF 数量有限, 每路径一份是
   正确的平衡(比每帧重解省掉 LZW 全量重跑)。
   内存治理(性能治理): 帧序列是"合成好的整幅 RGBA8", 大图/多图的占用
   无界会吃光内存 —— 缓存带总量预算(帧数×宽×高×4 估算, 缺省 64MB,
   HN_GIF_CACHE_MB 可覆盖, 探针用小预算验逐出策略)。超限按最久未用
   (last_use 触达序号)逐出。逐出**不立即** hn_gif_free: 摘出缓存表、
   挂进坟场(gif_grave), 延迟到下一次重绘(重建显示列表)之后再统一
   释放 —— 本帧已装进显示列表的帧指针必须活到重绘读完(重绘之后坟场
   里的对象必然不再被任何指令引用, 释放无悬空)。 */
#define HN_GIF_CACHE_BUDGET_DEFAULT ((size_t)64 * 1024 * 1024)

/* 预算(MB): 环境钮 HN_GIF_CACHE_MB 覆盖(值变即重算, 探针可同进程改) */
static size_t gif_budget(void) {
    static char memo[24] = "\x01";      /* 首次必不相等 → 走一次换算 */
    static size_t cap = 0;
    const char *e = getenv("HN_GIF_CACHE_MB");
    const char *cur = (e && *e) ? e : "";
    if (strcmp(cur, memo) != 0) {
        snprintf(memo, sizeof(memo), "%s", cur);
        long mb = atol(cur);
        cap = mb > 0 ? (size_t)mb * 1024 * 1024 : HN_GIF_CACHE_BUDGET_DEFAULT;
    }
    return cap;
}

/* LRU 触达序号(进程级单调; 简单时间戳口径) */
static unsigned long g_gif_clock = 0;

/* 单条 GIF 的估算占用(合成帧 = 帧数×宽×高×4 字节) */
static size_t gif_bytes_of(const hn_gif *g) {
    return (size_t)hn_gif_frame_count(g) * (size_t)hn_gif_width(g)
         * (size_t)hn_gif_height(g) * 4;
}

/* 坟场里的 GIF 统一释放(重绘之后调 —— 旧显示列表那时已整体作废) */
static void gif_grave_flush(hn_context *c) {
    if (!c || c->n_grave == 0) return;
    for (int i = 0; i < c->n_grave; i++) hn_gif_free(c->gif_grave[i]);
    c->n_grave = 0;
}

/* 超预算时逐出最久未用(腾到装得下 need 为止; 全空则止 —— 单条超预算
   的大 GIF 保留, 预算是软上限, 总占用仍有界 = 预算 + 单条)。 */
static void gif_cache_evict(hn_context *c, size_t need) {
    size_t budget = gif_budget();
    while (c->gif_bytes + need > budget) {
        int victim = -1;
        for (int i = 0; i < c->n_gif; i++) {
            if (!c->gifs[i].gif) continue;
            if (victim < 0 || c->gifs[i].last_use < c->gifs[victim].last_use)
                victim = i;
        }
        if (victim < 0) break;
        c->gif_bytes -= c->gifs[victim].bytes;
        if (c->n_grave == c->cap_grave) {
            int nc = c->cap_grave ? c->cap_grave * 2 : 8;
            void *nv = realloc(c->gif_grave, sizeof(hn_gif *) * (size_t)nc);
            if (!nv) { hn_gif_free(c->gifs[victim].gif); }   /* 坟场扩不动: 立即释放 */
            else { c->gif_grave = nv; c->cap_grave = nc; c->gif_grave[c->n_grave++] = c->gifs[victim].gif; }
        } else {
            c->gif_grave[c->n_grave++] = c->gifs[victim].gif;
        }
        free(c->gifs[victim].path);
        c->gifs[victim] = c->gifs[c->n_gif - 1];
        c->n_gif--;
    }
}

size_t hn_context_gif_bytes(const hn_context *c) { return c ? c->gif_bytes : 0; }
int hn_context_gif_count(const hn_context *c) { return c ? c->n_gif : 0; }

hn_gif *hn_context_gif(hn_context *c, const char *path) {
    if (!c || !path || !*path) return NULL;
    for (int i = 0; i < c->n_gif; i++) {
        if (c->gifs[i].path && !strcmp(c->gifs[i].path, path)) {
            c->gifs[i].last_use = ++g_gif_clock;   /* 命中即刷新 LRU 序 */
            return c->gifs[i].gif;
        }
    }
    hn_gif *g = NULL;
    const hn_asset_backend *ab = c->assets;
    if (ab && ab->load) {
        size_t len = 0;
        const char *bytes = ab->load(ab->ctx, path, &len);
        if (bytes && len) g = hn_gif_decode((const unsigned char *)bytes, len);
    }
    if (g) gif_cache_evict(c, gif_bytes_of(g));   /* 先腾地方再入表(新条目最年轻) */
    if (c->n_gif == c->cap_gif) {
        int nc = c->cap_gif ? c->cap_gif * 2 : 8;
        void *nv = realloc(c->gifs, sizeof(*c->gifs) * (size_t)nc);
        if (!nv) { if (g) hn_gif_free(g); return NULL; }
        c->gifs = nv;
        c->cap_gif = nc;
    }
    c->gifs[c->n_gif].path = hn_strdup(path);
    c->gifs[c->n_gif].gif = g;      /* 失败也入表(负缓存) */
    c->gifs[c->n_gif].last_use = ++g_gif_clock;
    c->gifs[c->n_gif].bytes = g ? gif_bytes_of(g) : 0;
    c->gif_bytes += c->gifs[c->n_gif].bytes;
    c->n_gif++;
    return g;
}

void hn_context_gif_clear(hn_context *c) {
    if (!c || !c->gifs) return;
    for (int i = 0; i < c->n_gif; i++) {
        free(c->gifs[i].path);
        if (c->gifs[i].gif) hn_gif_free(c->gifs[i].gif);
    }
    c->n_gif = 0;
    c->gif_bytes = 0;
    /* 坟场一并清(文档替换/销毁时旧显示列表本就整体作废, 无悬空窗口) */
    gif_grave_flush(c);
}

/* ---- 脚本驱动网格 ---- */

int hn_node_set_mesh_verts(hn_node *n, const float *verts, int cols, int rows) {
    if (!n || n->kind != HN_ELEM || cols <= 0 || rows <= 0) return 0;
    if (cols > 128) cols = 128;
    if (rows > 128) rows = 128;
    int nv = (cols + 1) * (rows + 1);
    float *p = (float *)hn_arena_alloc(n->arena, sizeof(float) * (size_t)(nv * 2));
    if (!p) return 0;
    if (verts) memcpy(p, verts, sizeof(float) * (size_t)(nv * 2));
    else {
        /* 未给顶点则用均匀网格 */
        for (int r = 0; r <= rows; r++)
            for (int cc = 0; cc <= cols; cc++) {
                int i = r * (cols + 1) + cc;
                p[i * 2]     = (float)cc / (float)cols * n->bw;
                p[i * 2 + 1] = (float)r / (float)rows * n->bh;
            }
    }
    n->mesh_verts = p;
    n->mesh_cols = cols;
    n->mesh_rows = rows;
    return 1;
}

int hn_node_mesh_info(hn_node *n, int *cols, int *rows) {
    if (!n || n->kind != HN_ELEM) return 0;
    if (n->mesh_verts) {
        if (cols) *cols = n->mesh_cols;
        if (rows) *rows = n->mesh_rows;
        return 1;
    }
    const char *g = hn_node_attr(n, "hn-mesh");
    if (!g || !*g || !strcmp(g, "false") || !strcmp(g, "0")) return 0;
    if (cols) *cols = 0;
    if (rows) *rows = 0;
    return 1;
}

/* 推进过渡动画并写回样式; 返回 1 表示仍有动画在跑(dt_ms<0 → 仅初始化)。
   媒体会话同帧驱动: PLAYING/待起播会话每帧必达宿主 tick(契约 §3),
   返回值并入 —— 视频在播时帧循环不能停, 否则画面冻结在最后一帧。
   hn-stream 平滑追上同一帧循环: dt>0 即标记帧循环活跃(布局期锚定从
   "直接贴底"切换到动画口径), 在途容器逐帧指数逼近, 返回值并入 ——
   收敛前帧循环不熄火, 否则追上动画停在半途。
   锚点平滑滚动(#锚导航)同一帧循环: 实时帧下武装的容器逐帧向目标偏移
   指数逼近, 返回值并入 —— 收敛前帧循环不熄火, 否则滚动停在半途。 */
int hn_context_anim_tick_ex(hn_context *c, float dt_ms, int *cls) {
    if (!c->doc || !c->doc->root) {
        if (cls) *cls = HN_ANIM_NONE;
        return 0;
    }
    /* 活跃类别(性能治理): interactive = 写样式字段的动画(过渡/关键帧/
       入场/媒体在播/平滑滚动在途), ambient = 纯 ext_clock 类(hn-mesh/
       GIF/Lottie 循环, 不碰样式字段)。二者并存按 interactive 计 ——
       帧节拍与渲染路径必须按最严格的一档走。 */
    int inter = 0, amb = 0;
    int media = 0;
    if (dt_ms >= 0) {
        media = hn_media_tick_ret(c, dt_ms);
        if (media) inter = 1;            /* 媒体在播 = interactive */
    }
    int streams = 0, anchors = 0;
    if (dt_ms > 0) {
        c->live_frame = 1;
        streams = stream_anim_walk(c->doc->root, dt_ms);
        anchors = scroll_anim_walk(c->doc->root, dt_ms);
        if (streams || anchors) inter = 1;   /* 平滑滚动在途 = interactive */
    }
    int act = anim_walk(c, c->doc->root, dt_ms, &amb, &inter);
    /* exit ghost 推进: 淡出完成后出列(帧循环持续渲染 → 淡出自然可见) */
    if (c->exit_ghost_n > 0 && dt_ms > 0) {
        for (int i = c->exit_ghost_n - 1; i >= 0; i--) {
            c->exit_ghost[i].t_ms += (int)dt_ms;
            if (c->exit_ghost[i].t_ms >= c->exit_ghost[i].ms) {
                for (int j = i; j < c->exit_ghost_n - 1; j++)
                    c->exit_ghost[j] = c->exit_ghost[j + 1];
                c->exit_ghost_n--;
            }
        }
        if (c->exit_ghost_n > 0) inter = 1;   /* 淡出在途 = interactive */
    }
    if (cls) *cls = inter ? HN_ANIM_INTERACTIVE
                          : (amb ? HN_ANIM_AMBIENT : HN_ANIM_NONE);
    return act || media || streams || anchors;
}

int hn_context_anim_tick(hn_context *c, float dt_ms) {
    return hn_context_anim_tick_ex(c, dt_ms, NULL);
}

void hn_context_set_hover(hn_context *c, hn_node *n) { c->hover_node = n; }
void hn_context_set_active(hn_context *c, hn_node *n) { c->active_node = n; }

/* 最近指针坐标(视口参照系): 运行时每次 pointer_move 喂入。网格变形的
   look(视线/头部跟随)按"指针相对元素中心"取指向 —— 引擎此前不存坐标,
   形变无从"看向"指针。未收到过事件时 has_pointer 保持 0, look 目标 = 回中。 */
void hn_context_set_pointer(hn_context *c, float x, float y) {
    if (!c) return;
    c->pointer_x = x;
    c->pointer_y = y;
    c->has_pointer = 1;
}

/* 主按钮按下态(hn_rt_button down/up 喂入): 轨道相机拖拽等"按下才生效"
   的交互用 —— 只存这一份状态, 不引入第二事实源。 */
void hn_context_set_pointer_button(hn_context *c, int down) {
    if (!c) return;
    c->pointer_down = down ? 1 : 0;
}

float hn_context_viewport_h(hn_context *c) {
    return c ? c->vh : 0.0f;
}

/* ---- Smart-UX 出现/消失默认动效 ---- */

/* 运行时插入的节点默认播"淡入+上浮"入场(rt 首帧后置位; 首帧文档与
 * 作者显式 animation 声明不受影响)。off = 回到瞬现(旧口径)。 */
void hn_context_set_dynamic_enter(hn_context *c, int on) {
    if (c) c->enter_dynamic_default = on ? 1 : 0;
}

/* JS 移除带盒子树时投喂 exit ghost: 子树在**原位**(最后布局盒位)淡出
 * 160ms 后消亡。容量 8(环形丢弃最老); 上下文销毁/换文档/compact 时
 * 由各清口统一出列(节点指针随 arena 生命周期)。 */
void hn_context_push_exit_ghost(hn_context *c, hn_node *n) {
    if (!c || !n || n->kind != HN_ELEM) return;
    if (n->bw <= 1.0f || n->bh <= 1.0f) return;          /* 无盒(未画过)不 ghost */
    if (c->exit_ghost_n >= 8) {                           /* 满: 丢最老 */
        for (int j = 0; j < c->exit_ghost_n - 1; j++)
            c->exit_ghost[j] = c->exit_ghost[j + 1];
        c->exit_ghost_n--;
    }
    struct hn_exit_ghost *g = &c->exit_ghost[c->exit_ghost_n++];
    g->node = n;
    g->x = n->bx;
    g->y = n->by;
    g->t_ms = 0;
    g->ms = 160;
}

void hn_context_clear_exit_ghosts(hn_context *c) {
    if (c) c->exit_ghost_n = 0;
}


void hn_context_set_focus(hn_context *c, hn_node *n) {
    /* 禁用的控件不可聚焦(与浏览器一致): 运行时点击命中也不改变焦点 */
    if (n && hn_node_is_disabled(n)) return;
    if (c->focus_node == n) return;
    c->focus_node = n;
    /* :focus 是样式态 —— 只记指针的话, 若宿主不主动 relayout,
       :focus 规则与焦点环就静默不生效。已有布局时立即重渲染
       (样式重算 + 重布局 + 重绘); 首次布局前(vw==0)只记指针,
       等宿主的第一次 layout 自然带上焦点态。 */
    if (c->doc && c->vw > 0)
        hn_context_layout(c, c->vw, c->vh, c->tb_valid ? &c->tb : NULL);
}
void hn_context_set_caret_visible(hn_context *c, int on) { c->caret_on = on; }

/* ---------------- 输入控件 ---------------- */

int hn_node_is_input(hn_node *n) {
    if (!n || n->kind != HN_ELEM || !n->tag) return 0;
    return !strcmp(n->tag, "input") || !strcmp(n->tag, "textarea");
}

/* 布尔属性: 存在且值不为 "false"/"0" 即为真(运行时用 set_attr 写 ""/切换
   "false" 就能翻转状态, 不需要额外的引擎 API)。 */
static int flag_attr(hn_node *n, const char *name) {
    const char *v = hn_node_attr(n, name);
    if (!v) return 0;
    return strcmp(v, "false") != 0 && strcmp(v, "0") != 0;
}

int hn_node_input_kind(hn_node *n) {
    if (!n || n->kind != HN_ELEM || !n->tag) return HN_IN_NONE;
    if (!strcmp(n->tag, "textarea")) return HN_IN_TEXTAREA;
    if (!strcmp(n->tag, "input")) {
        const char *t = hn_node_attr(n, "type");
        if (t) {
            if (!strcmp(t, "checkbox")) return HN_IN_CHECKBOX;
            if (!strcmp(t, "radio")) return HN_IN_RADIO;
            if (!strcmp(t, "password")) return HN_IN_PASSWORD;
            if (!strcmp(t, "button") || !strcmp(t, "submit") || !strcmp(t, "reset"))
                return HN_IN_BUTTON;
            if (!strcmp(t, "range")) return HN_IN_RANGE;
        }
        return HN_IN_TEXT;
    }
    return HN_IN_NONE;
}

int hn_node_is_checked(hn_node *n) {
    int k = hn_node_input_kind(n);
    if (k != HN_IN_CHECKBOX && k != HN_IN_RADIO) return 0;
    return flag_attr(n, "checked");
}

int hn_node_is_disabled(hn_node *n) {
    if (!n || n->kind != HN_ELEM) return 0;
    return flag_attr(n, "disabled");
}

const char *hn_node_value(hn_node *n, size_t *len_out) {
    if (len_out) *len_out = 0;
    if (!hn_node_is_input(n)) return NULL;
    if (n->value) {
        if (len_out) *len_out = n->value_len;
        return n->value;
    }
    const char *a = hn_node_attr(n, "value");
    if (a) {
        if (len_out) *len_out = strlen(a);
        return a;
    }
    if (len_out) *len_out = 0;
    return "";
}

/* 保证值缓冲区有容量(在节点所属文档的 arena 上增长) */
static void value_ensure(hn_node *n, size_t need) {
    if (n->value_cap >= need + 1) return;
    size_t nc = n->value_cap ? n->value_cap * 2 : 64;
    while (nc < need + 1) nc *= 2;
    char *nv = hn_arena_alloc(n->arena, nc);
    if (n->value && n->value_len) memcpy(nv, n->value, n->value_len);
    nv[n->value_len] = 0;
    n->value = nv;
    n->value_cap = nc;
}

int hn_node_set_value(hn_node *n, const char *utf8, size_t len) {
    if (!hn_node_is_input(n) || !n->arena) return 0;
    int k = hn_node_input_kind(n);
    /* range: JS el.value = N 写 value 属性(滑条位 = f(value)); 键盘不入 */
    if (k == HN_IN_RANGE) {
        if (!utf8) { hn_node_remove_attr(n, "value"); return 1; }
        char vbuf[48];
        size_t take = len < sizeof(vbuf) - 1 ? len : sizeof(vbuf) - 1;
        memcpy(vbuf, utf8, take);
        vbuf[take] = 0;
        hn_node_set_attr(n, "value", vbuf);
        return 1;
    }
    /* checkbox/radio/button 没有可编辑文本: 值语义是 checked/value 属性,
       键盘输入不应写进 value 缓冲 */
    if (k == HN_IN_CHECKBOX || k == HN_IN_RADIO || k == HN_IN_BUTTON) return 0;
    value_ensure(n, len);
    if (len) memcpy(n->value, utf8, len);
    n->value[len] = 0;
    n->value_len = len;
    n->caret = (int)len;
    return 1;
}

int hn_node_caret(hn_node *n) {
    return (n && hn_node_is_input(n)) ? n->caret : 0;
}

void hn_node_set_caret(hn_node *n, int byte_off) {
    if (!hn_node_is_input(n)) return;
    int k = hn_node_input_kind(n);
    if (k == HN_IN_CHECKBOX || k == HN_IN_RADIO || k == HN_IN_BUTTON ||
        k == HN_IN_RANGE) return;
    if (byte_off < 0) byte_off = 0;
    if ((size_t)byte_off > n->value_len) byte_off = (int)n->value_len;
    n->caret = byte_off;
}

/* 收集 name=value 并 URL 编码(application/x-www-form-urlencoded)。
   值语义与浏览器对齐:
   - disabled 控件不参与提交;
   - checkbox/radio 仅在选中时提交, 值取 value 属性, 缺省 "on"。 */
static void form_walk(hn_node *n, char *out, size_t cap, size_t *used, int *first) {
    if (n->kind == HN_ELEM) {
        if (hn_node_is_input(n) && !hn_node_is_disabled(n)) {
            int k = hn_node_input_kind(n);
            int flag = (k == HN_IN_CHECKBOX || k == HN_IN_RADIO);
            const char *v = NULL;
            size_t vlen = 0;
            if (flag) {
                if (hn_node_is_checked(n)) {
                    v = hn_node_attr(n, "value");
                    if (!v) v = "on";
                    vlen = strlen(v);
                }
            } else {
                v = hn_node_value(n, &vlen);
            }
            const char *name = hn_node_attr(n, "name");
            if (name && v) {
                if (*used < cap) {
                    if (!*first) *used += (size_t)snprintf(out + *used, cap - *used, "&");
                }
                *first = 0;
                for (const char *p = name; *p && *used + 4 < cap; p++) {
                    int c = (unsigned char)*p;
                    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                        out[(*used)++] = (char)c;
                    } else {
                        static const char *HX = "0123456789ABCDEF";
                        out[(*used)++] = '%';
                        out[(*used)++] = HX[(c >> 4) & 0xF];
                        out[(*used)++] = HX[c & 0xF];
                    }
                }
                if (*used + 1 < cap) out[(*used)++] = '=';
                for (size_t i = 0; i < vlen && *used + 4 < cap; i++) {
                    int c = (unsigned char)v[i];
                    if (isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
                        out[(*used)++] = (char)c;
                    } else {
                        static const char *HX = "0123456789ABCDEF";
                        out[(*used)++] = '%';
                        out[(*used)++] = HX[(c >> 4) & 0xF];
                        out[(*used)++] = HX[c & 0xF];
                    }
                }
            }
        }
        for (hn_node *ch = n->first; ch; ch = ch->next) form_walk(ch, out, cap, used, first);
    }
}

size_t hn_doc_form_encode(hn_doc *doc, char *out, size_t cap) {
    size_t used = 0;
    int first = 1;
    if (doc && doc->root) form_walk(doc->root, out, cap - 1, &used, &first);
    out[used] = 0;
    return used;
}

hn_node *hn_node_first_child(hn_node *n) { return n ? n->first : NULL; }
hn_node *hn_node_next_sibling(hn_node *n) { return n ? n->next : NULL; }

static int has_attr_walk(hn_node *n, const char *name) {
    if (n->kind == HN_ELEM && hn_node_attr(n, name)) return 1;
    for (hn_node *ch = n->first; ch; ch = ch->next)
        if (has_attr_walk(ch, name)) return 1;
    return 0;
}

int hn_doc_has_attr(hn_doc *doc, const char *name) {
    return (doc && doc->root) ? has_attr_walk(doc->root, name) : 0;
}

hn_node *hn_doc_input_at(hn_doc *doc, int idx) {
    if (!doc || !doc->root || idx < 0) return NULL;
    hn_node *stack[512];
    hn_node *found = NULL;
    int sp = 0, seen = 0;
    stack[sp++] = doc->root;
    while (sp > 0 && !found) {
        hn_node *n = stack[--sp];
        if (hn_node_is_input(n)) {
            if (seen == idx) { found = n; break; }
            seen++;
        }
        hn_node *kids[512];
        int kn = 0;
        for (hn_node *ch = n->first; ch && kn < 512; ch = ch->next) kids[kn++] = ch;
        for (int i = kn - 1; i >= 0 && sp < 512; i--) stack[sp++] = kids[i];
    }
    return found;
}

void hn_node_box(hn_node *n, float *x, float *y, float *w, float *h) {
    if (x) *x = n ? n->bx : 0;
    if (y) *y = n ? n->by : 0;
    if (w) *w = n ? n->bw : 0;
    if (h) *h = n ? n->bh : 0;
}

void hn_node_debug_background(hn_node *n, hn_color *out) {
    if (out) *out = n ? n->style.background : 0;
}

/* ---------------- DOM 变更(push_attr / splice 复用) ---------------- */

/* 移除属性(removeAttribute 语义)。attrs 数组本体在 arena 上, 就地前压,
   旧槽不回收(arena 语义, 无需回收)。命中返回 1。 */
/* 把 child 插到 parent 子列表中 ref 之前(ref NULL = 追加到尾部)。
   child 必须已在文档树某处(arena 归属); 在旧父下则先摘除 —— 与
   appendChild 的移动语义同口径(浏览器 insertBefore 同款)。 */
void hn_node_insert_before(hn_node *parent, hn_node *child, hn_node *ref) {
    if (!parent || !child || child == parent || !parent->arena || child->arena != parent->arena)
        return;
    if (ref && ref->parent != parent) ref = NULL;   /* ref 不是 parent 的孩子 → 追加 */
    if (child->parent) hn_node_remove_child(child->parent, child);
    child->parent = parent;
    child->prev = NULL;
    child->next = ref;
    if (ref) {
        child->prev = ref->prev;
        if (ref->prev) ref->prev->next = child;
        else parent->first = child;
        ref->prev = child;
    } else {
        child->prev = parent->last;
        if (parent->last) parent->last->next = child;
        else parent->first = child;
        parent->last = child;
    }
    child->anim.fresh = 2;   /* 运行时插入: Smart-UX 默认入场(fade+rise) */
}

int hn_node_remove_attr(hn_node *n, const char *name) {
    if (!n || n->kind != HN_ELEM || !name) return 0;
    int found = 0;
    for (int i = 0; i < n->n_attrs; i++) {
        if (!found && !strcmp(n->attrs[i].name, name)) { found = 1; continue; }
        if (found) n->attrs[i - 1] = n->attrs[i];
    }
    if (found) n->n_attrs--;
    return found;
}

/* ---------------- DOM 查询(querySelector) ----------------
 * 选择器语法与样式表同一套(含组合器/属性选择器/伪类/:not)。
 * 文档序返回命中元素(上限 max); 选择器语法错返回 -1(调用方映射为
 * JS 异常), 无命中返回 0。c 可为 NULL —— hover/active/focus 状态伪类
 * 在无上下文时按未激活匹配。合成包裹根不参与匹配(is_doc_wrapper)。 */
int hn_doc_query_selector(hn_doc *doc, const char *sel, hn_context *c,
                          hn_node **out, int max) {
    if (!doc || !sel || !out || max <= 0) return 0;
    hn_selgroup *g = hn_css_parse_selgroup(sel, strlen(sel));
    if (!g) return -1;
    int n = 0;
    hn_node *stack[512];
    int sp = 0;
    stack[sp++] = doc->root;
    while (sp > 0 && n < max) {
        hn_node *nd = stack[--sp];
        if (nd->kind == HN_ELEM && !nd->is_doc_wrapper &&
            hn_style_selgroup_match(g, nd, c))
            out[n++] = nd;
        hn_node *kids[512];
        int kn = 0;
        for (hn_node *ch = nd->first; ch && kn < 512; ch = ch->next) kids[kn++] = ch;
        for (int i = kn - 1; i >= 0 && sp < 512; i--) stack[sp++] = kids[i];
    }
    hn_css_selgroup_free(g);
    return n;
}

int hn_node_set_attr(hn_node *n, const char *name, const char *value) {
    /* 伪元素合成文本节点也带内联 style 标记(HN_TEXT 有 arena) ——
       kind 不再限定 ELEM; 其余读口(get_attr/hn_node_attr)本就通用。 */
    if (!n || !name || !n->arena) return 0;
    /* 已存在则原地改值(值在 arena 上重新分配, 旧值不回收) */
    for (int i = 0; i < n->n_attrs; i++) {
        if (!strcmp(n->attrs[i].name, name)) {
            n->attrs[i].value = hn_arena_strndup(n->arena, value ? value : "",
                                                 value ? strlen(value) : 0);
            return 1;
        }
    }
    hn_attr *na = hn_arena_alloc(n->arena, sizeof(hn_attr) * (size_t)(n->n_attrs + 1));
    if (!na) return 0;
    for (int i = 0; i < n->n_attrs; i++) na[i] = n->attrs[i];
    na[n->n_attrs].name = hn_arena_strndup(n->arena, name, strlen(name));
    na[n->n_attrs].value = hn_arena_strndup(n->arena, value ? value : "",
                                            value ? strlen(value) : 0);
    n->attrs = na;
    n->n_attrs++;
    return 1;
}

static size_t text_rec(hn_node *n, char *out, size_t cap, size_t used) {
    for (hn_node *ch = n->first; ch; ch = ch->next) {
        if (ch->kind == HN_TEXT && ch->text && ch->text_len) {
            size_t room = cap > used + 1 ? cap - used - 1 : 0;
            size_t take = ch->text_len < room ? ch->text_len : room;
            if (take) memcpy(out + used, ch->text, take);
            used += take;
        } else if (ch->kind == HN_ELEM) {
            used = text_rec(ch, out, cap, used);
        }
    }
    return used;
}

size_t hn_node_text_content(hn_node *n, char *out, size_t cap) {
    if (!n || !out || cap == 0) return 0;
    size_t used = text_rec(n, out, cap, 0);
    out[used] = 0;
    return used;
}

int hn_node_set_text_content(hn_node *n, const char *utf8, size_t len) {
    if (!n || n->kind != HN_ELEM || !n->arena) return 0;
    n->first = n->last = NULL;
    n->tt_applied = 0;   /* 新文本按当前 text-transform 重新变换(旧守卫作废) */
    if (!utf8 || !len) return 1;
    hn_node *t = hn_arena_alloc(n->arena, sizeof(hn_node));
    if (!t) return 0;
    memset(t, 0, sizeof(hn_node));
    t->kind = HN_TEXT;
    t->arena = n->arena;
    t->parent = n;
    t->text = hn_arena_strndup(n->arena, utf8, len);
    t->text_len = len;
    n->first = n->last = t;
    return 1;
}

hn_node *hn_node_append_element(hn_node *parent, const char *tag) {
    if (!parent || !tag || !parent->arena) return NULL;
    hn_node *el = hn_arena_alloc(parent->arena, sizeof(hn_node));
    if (!el) return NULL;
    memset(el, 0, sizeof(hn_node));
    el->kind = HN_ELEM;
    el->arena = parent->arena;
    el->tag = hn_arena_strndup(parent->arena, tag, strlen(tag));
    el->anim.fresh = 2;   /* 运行时插入: Smart-UX 默认入场 */
    el->parent = parent;
    if (parent->last) { parent->last->next = el; el->prev = parent->last; parent->last = el; }
    else { parent->first = parent->last = el; }
    return el;
}

int hn_node_remove_child(hn_node *parent, hn_node *child) {
    if (!parent || !child) return 0;
    for (hn_node *ch = parent->first; ch; ch = ch->next) {
        if (ch != child) continue;
        if (ch->prev) ch->prev->next = ch->next; else parent->first = ch->next;
        if (ch->next) ch->next->prev = ch->prev; else parent->last = ch->prev;
        ch->next = NULL; ch->prev = NULL; ch->parent = NULL;
        return 1;
    }
    return 0;
}

/* 解析 HTML 片段并插入(供脚本的 innerHTML/append 语义) */
int hn_node_insert_html(hn_node *parent, const char *html, size_t len, hn_swap_mode mode) {
    if (!parent || !html || !parent->arena) return 0;
    hn_node *frag = hn_arena_alloc(parent->arena, sizeof(hn_node));
    if (!frag) return 0;
    memset(frag, 0, sizeof(hn_node));
    frag->kind = HN_ELEM;
    frag->arena = parent->arena;
    frag->tag = "fragment";
    /* 注意: hn_parse_into 会通过 doc 登记内联样式表(<style>), 不能传 NULL。
       片段里的 <style> 也应生效, 因此借用宿主文档的 arena 建一个临时 doc 视图。 */
    hn_doc tmpdoc;
    memset(&tmpdoc, 0, sizeof(tmpdoc));
    tmpdoc.arena = parent->arena;
    tmpdoc.root = frag;
    hn_parse_into(&tmpdoc, frag, html, len);
    hn_node *ff = frag->first, *fl = frag->last;
    if (!ff) {
        if (mode == HN_SWAP_INNER) parent->first = parent->last = NULL;
        return 1;
    }
    ff->prev = NULL;
    fl->next = NULL;
    if (mode == HN_SWAP_INNER) {
        parent->first = parent->last = NULL;
        for (hn_node *c = ff; c; c = c->next) c->parent = parent;
        parent->first = ff; parent->last = fl;
    } else if (mode == HN_SWAP_PREPEND) {
        for (hn_node *c = ff; c; c = c->next) c->parent = parent;
        if (parent->first) { parent->first->prev = fl; fl->next = parent->first; parent->first = ff; }
        else { parent->first = ff; parent->last = fl; }
    } else { /* APPEND */
        for (hn_node *c = ff; c; c = c->next) c->parent = parent;
        if (parent->last) { parent->last->next = ff; ff->prev = parent->last; parent->last = fl; }
        else { parent->first = ff; parent->last = fl; }
    }
    return 1;
}

void hn_node_debug_anim(hn_node *n, const char **name, float *ms, int *iter) {
    if (name) *name = n ? n->style.kf_name : NULL;
    if (ms) *ms = n ? n->style.kf_ms : 0;
    if (iter) *iter = n ? n->style.kf_iter : 0;
}

void hn_node_debug_rotate(hn_node *n, float *deg) {
    if (deg) *deg = n ? n->style.rotate : 0;
}

void hn_node_debug_opacity(hn_node *n, float *out) {
    if (out) *out = n ? n->style.opacity : 0;
}

void hn_node_debug_border(hn_node *n, hn_color *out) {
    if (out) *out = n ? n->style.border_color : 0;
}

void hn_node_debug_color(hn_node *n, hn_color *out) {
    if (out) *out = n ? n->style.color : 0;
}

int hn_node_display(hn_node *n) {
    return n ? (int)n->style.display : 2;
}

int hn_node_run_range(hn_node *n, int i, size_t *begin, size_t *end) {
    if (!n || !n->runs || i < 0 || i >= n->n_runs) return 0;
    if (begin) *begin = n->runs[i].begin;
    if (end) *end = n->runs[i].end;
    return 1;
}

const char *hn_node_text(hn_node *n, size_t *len_out) {
    /* 元素节点也可能持有文本: 布局为省略号/列表序号合成的片段就是挂在
       容器元素上的(见 la_clip_ellipsis)。之前对非 TEXT 节点一律返回 NULL,
       于是 `hn text` 读不到这类 run —— 而绘制路径(paint_runs 读 n->text)
       是能画出来的, 表现为"看得见却读不到", agent 侧极难排查。 */
    if (!n) { if (len_out) *len_out = 0; return NULL; }
    if (n->kind != HN_TEXT && !n->text) { if (len_out) *len_out = 0; return NULL; }
    if (len_out) *len_out = n->text_len;
    return n->text;
}

int hn_node_run_count(hn_node *n) {
    return n ? n->n_runs : 0;
}

int hn_node_run_at(hn_node *n, int i, float *x, float *baseline, float *w,
                   float *y_top, float *h) {
    if (!n || i < 0 || i >= n->n_runs) return 0;
    hn_run *r = &n->runs[i];
    if (x) *x = r->x;
    if (baseline) *baseline = r->baseline;
    if (w) *w = r->width;
    if (y_top) *y_top = r->y_top;
    if (h) *h = r->h;
    return 1;
}

int hn_node_cursor(hn_node *n) {
    return (n && n->kind == HN_ELEM) ? n->style.cursor : 0;
}

const hn_display_list *hn_context_display_list(const hn_context *c) {
    if (!c || !c->doc) return NULL;
    return &((hn_context *)c)->dl;
}

/* ---------------- 命中测试 ---------------- */

static hn_node *hit_walk(hn_node *n, float x, float y) {
    if (n->kind == HN_ELEM && n->style.display == HN_DISP_NONE) return NULL;
    /* pointer-events:none: 子树不参与命中(点击落到下层) —— CSS 标准语义 */
    if (n->kind == HN_ELEM && n->style.pointer_events) return NULL;
    /* 脱流子级(position:absolute/fixed)不受祖先盒包含约束 —— absolute
       脱离流, 祖先(常见 html/body)零高时也必须命中(浏览器同语义;
       实测: abs-positioned select 在无内容页里曾永远命不中)。 */
    for (hn_node *ch = n->last; ch; ch = ch->prev) {
        if (ch->kind == HN_ELEM &&
            (ch->style.position == HN_POS_ABSOLUTE || ch->style.position == HN_POS_FIXED)) {
            hn_node *r = hit_walk(ch, x + n->scroll_x, y + n->scroll_y);
            if (r) return r;
        }
    }
    /* 根链(html/body)按浏览器语义覆盖整块画布: 脱流内容(absolute 弹层/
       桌宠浮窗)常使 html/body 无流内容而盒高为 0 —— 若按盒剪枝, 脱流
       元素永远命不中(实测 select 弹层页 hit 全落 wrapper)。 */
    int root_chain = (n->parent == NULL || n->parent->is_doc_wrapper ||
                      (n->parent->tag && !strcmp(n->parent->tag, "html") &&
                       n->tag && !strcmp(n->tag, "body")));
    if (!root_chain &&
        (x < n->bx || y < n->by || x >= n->bx + n->bw || y >= n->by + n->bh))
        return NULL;
    for (hn_node *ch = n->last; ch; ch = ch->prev) {
        if (ch->kind == HN_ELEM &&
            (ch->style.position == HN_POS_ABSOLUTE || ch->style.position == HN_POS_FIXED))
            continue;   /* 已在上面优先试过 */
        hn_node *r = hit_walk(ch, x + n->scroll_x, y + n->scroll_y);
        if (r) return r;
    }
    return n->kind == HN_ELEM ? n : NULL; /* 文本节点向上冒泡到父元素 */
}

hn_node *hn_context_hit_node(hn_context *c, float x, float y) {
    if (!c->doc) return NULL;
    return hit_walk(c->doc->root, x, y);
}

/* ---------------- 过渡动画 ----------------
 *
 * 引擎持有"当前值"(n->anim), 样式计算得到"目标值"。每次 tick 时:
 *   目标变化 → 记录 from/to 并重置进度; 否则推进进度。
 * 绘制前把插值结果覆盖到样式字段, 绘制器无需感知动画。
 */

static void rgba_of(hn_color c, float out[4]) {
    out[0] = (float)((c >> 24) & 0xFF) / 255.0f;
    out[1] = (float)((c >> 16) & 0xFF) / 255.0f;
    out[2] = (float)((c >> 8) & 0xFF) / 255.0f;
    out[3] = (float)(c & 0xFF) / 255.0f;
}

static hn_color color_of(const float v[4]) {
    int r = (int)(v[0] * 255.0f + 0.5f), g = (int)(v[1] * 255.0f + 0.5f);
    int b = (int)(v[2] * 255.0f + 0.5f), a = (int)(v[3] * 255.0f + 0.5f);
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    if (a < 0) a = 0; if (a > 255) a = 255;
    return ((hn_color)r << 24) | ((hn_color)g << 16) | ((hn_color)b << 8) | (hn_color)a;
}

static void lerp4(float out[4], const float a[4], const float b[4], float t) {
    for (int i = 0; i < 4; i++) out[i] = a[i] + (b[i] - a[i]) * t;
}

/* 遍历树: 对齐动画目标, 必要时推进进度, 把当前值写回样式。
 * dt_ms < 0 表示"只初始化不推进"(首帧)。返回 1 表示仍有动画在跑。 */
/* ---------- @keyframes ---------- */

hn_keyframes *hn_sheet_find_kf(hn_sheet *sh, const char *name) {
    if (!sh || !name) return NULL;
    for (int i = 0; i < sh->n_kfs; i++)
        if (sh->kfs[i].name && !strcmp(sh->kfs[i].name, name)) return &sh->kfs[i];
    return NULL;
}

/* 从所有已加载样式表里按名字找动画定义 */
static const hn_keyframes *find_kf(hn_context *c, const char *name) {
    if (!c || !name) return NULL;
    for (int i = 0; i < c->n_sheets; i++) {
        hn_keyframes *kf = hn_sheet_find_kf(c->sheets[i], name);
        if (kf) return kf;
    }
    if (c->doc)
        for (int i = 0; i < c->doc->n_inline; i++) {
            hn_keyframes *kf = hn_sheet_find_kf(c->doc->inline_sheets[i], name);
            if (kf) return kf;
        }
    return NULL;
}

/* 一帧内可动画属性的端点集合。帧里声明了的属性用帧值, 没声明的属性
   以元素当前值(级联)充当该端 —— 与 CSS"缺失关键帧取当前值"一致。 */
typedef struct {
    float op;  unsigned char has_op;
    float tx, ty, sc, rot, rotx, roty;               /* 几何: has_* 同下 */
    unsigned char has_tx, has_ty, has_sc, has_rot, has_rotx, has_roty;
    float bg[4], fg[4];
    unsigned char has_bg, has_fg;
} kf_endpoints;

static void kf_collect(const hn_kf_stop *stp, hn_style *st, kf_endpoints *e) {
    memset(e, 0, sizeof(*e));
    e->sc = st->scale;
    e->tx = st->translate_x;
    e->ty = st->translate_y;
    e->rot = st->rotate;
    e->rotx = st->rotate_x;
    e->roty = st->rotate_y;
    rgba_of(st->background, e->bg);
    rgba_of(st->color, e->fg);
    for (int d = 0; d < stp->n_decls; d++) {
        const char *nm = stp->decls[d].name;
        const char *val = stp->decls[d].value;
        float num = (float)strtod(val, NULL);
        if (!strcmp(nm, "opacity")) { e->op = num; e->has_op = 1; }
        else if (!strcmp(nm, "translate-x") || !strcmp(nm, "translate")) { e->tx = num; e->has_tx = 1; }
        else if (!strcmp(nm, "translate-y")) { e->ty = num; e->has_ty = 1; }
        else if (!strcmp(nm, "scale")) { e->sc = num; e->has_sc = 1; }
        else if (!strcmp(nm, "rotate") || !strcmp(nm, "rotate-z")) { e->rot = num; e->has_rot = 1; }
        else if (!strcmp(nm, "rotate-x")) { e->rotx = num; e->has_rotx = 1; }
        else if (!strcmp(nm, "rotate-y")) { e->roty = num; e->has_roty = 1; }
        else if (!strcmp(nm, "background") || !strcmp(nm, "background-color")) {
            hn_color col;
            if (hn_color_parse(val, strlen(val), &col)) { rgba_of(col, e->bg); e->has_bg = 1; }
        } else if (!strcmp(nm, "color")) {
            hn_color col;
            if (hn_color_parse(val, strlen(val), &col)) { rgba_of(col, e->fg); e->has_fg = 1; }
        }
    }
}

/* 在关键帧时间轴 t(0..1) 处取样: 找到相邻两帧, 按属性端点插值写入 style。
   可动画属性与 transition 支持的范围一致。 */
static void kf_sample(const hn_keyframes *kf, float t, hn_style *st) {
    if (!kf || kf->n_stops == 0) return;
    int a = 0, b = kf->n_stops - 1;
    if (kf->n_stops == 1) {
        /* 单帧定义: 帧位置之前保持级联样式, 之后取帧值 */
        if (t < kf->stops[0].at) return;
        b = a;
    } else {
        /* 找 t 落在的区间 */
        int found = 0;
        for (int i = 0; i < kf->n_stops - 1; i++) {
            if (t >= kf->stops[i].at && t <= kf->stops[i + 1].at) { a = i; b = i + 1; found = 1; break; }
        }
        if (!found) {
            /* 关键帧未覆盖时间轴两端(如只写了 50%)时, 首帧前/末帧后保持
               级联样式 —— CSS 语义: 缺失的 0%/100% 帧取元素当前值。
               此前回退成"首尾帧大跨度插值", 会把整条时间轴压扁到边界外。 */
            return;
        }
    }
    const hn_kf_stop *sa = &kf->stops[a], *sb = &kf->stops[b];
    float span = sb->at - sa->at;
    float f = span > 0.0001f ? (t - sa->at) / span : 0;
    if (f < 0) f = 0; else if (f > 1) f = 1;
    /* 端点式插值: 先收集两端属性值(缺省端 = 当前级联值), 再统一 lerp。
       此前的顺序混合会把级联当前值混进"两端都声明了"的属性 —— 采样起点
       不在关键帧曲线上, 首帧值随级联漂移。 */
    kf_endpoints ea, eb;
    kf_collect(sa, st, &ea);
    kf_collect(sb, st, &eb);
    float op_a = ea.has_op ? ea.op : st->opacity;
    float op_b = eb.has_op ? eb.op : st->opacity;
    st->opacity = op_a + (op_b - op_a) * f;
    st->translate_x = ea.tx + (eb.tx - ea.tx) * f;
    st->translate_y = ea.ty + (eb.ty - ea.ty) * f;
    st->scale = ea.sc + (eb.sc - ea.sc) * f;
    st->rotate = ea.rot + (eb.rot - ea.rot) * f;
    st->rotate_x = ea.rotx + (eb.rotx - ea.rotx) * f;
    st->rotate_y = ea.roty + (eb.roty - ea.roty) * f;
    if (ea.has_bg || eb.has_bg) {
        float c4[4];
        lerp4(c4, ea.bg, eb.bg, f);
        st->background = color_of(c4);
        st->has_gradient = 0;
    }
    if (ea.has_fg || eb.has_fg) {
        float c4[4];
        lerp4(c4, ea.fg, eb.fg, f);
        st->color = color_of(c4);
    }
}

/* 推进 @keyframes 动画: 返回 1 表示仍在播放(含正延迟等待)。
   fill(forwards/both) 由调用方消费 —— 播完保持末帧需要每帧重放采样,
   不是单次 tick 能表达的。 */
static int kf_tick(hn_node *n, const hn_keyframes *kf, float ms, int iter, int dir,
                   float *clock_io, int *done_io, hn_style *st) {
    (void)n;
    if (!kf || ms <= 0) return 0;
    /* 正延迟未到: 保持级联样式, 仍算活跃(等待中)。负延迟直接落到
       下面的取模数学里 —— total 为负时 t = total - floor(total) 恰好
       是"跳过前段"的进度(CSS 负延迟语义)。 */
    if (*clock_io < 0) return 1;
    float dur = ms;
    float total = *clock_io / dur;          /* 已播放周期数(可为小数) */
    float t;
    if (iter < 0) {                          /* 无限循环 */
        t = total - floorf(total);
    } else {
        if (total >= (float)iter) { *done_io = 1; t = 1.0f; }
        else t = total - floorf(total);
    }
    /* 方向: alternate 时偶数轮正向、奇数轮反向 */
    int cyc = (int)floorf(total);
    int rev = 0;
    if (dir == 1) rev = 1;
    else if (dir == 2) rev = (cyc & 1);
    else if (dir == 3) rev = !(cyc & 1);
    if (rev) t = 1.0f - t;
    kf_sample(kf, t, st);
    return iter < 0 || !*done_io;
}

/* ---------- 缓动 ---------- */

/* 三次贝塞尔求值: 先由 x 反解参数 t(牛顿迭代), 再算 y —— 与 CSS 同法 */
static float bezier_axis(float t, float a1, float a2) {
    float c = 3.0f * a1, b = 3.0f * (a2 - a1) - c, a = 1.0f - c - b;
    return ((a * t + b) * t + c) * t;
}
static float bezier_solve(float x, float x1, float x2) {
    if (x <= 0) return 0;
    if (x >= 1) return 1;
    float t = x;                       /* 初值 */
    for (int i = 0; i < 8; i++) {      /* 牛顿迭代: 收敛快且确定性(无随机) */
        float fx = bezier_axis(t, x1, x2) - x;
        if (fabsf(fx) < 1e-5f) break;
        float c = 3.0f * x1, b = 3.0f * (x2 - x1) - c, a = 1.0f - c - b;
        float d = (3.0f * a * t + 2.0f * b) * t + c;
        if (fabsf(d) < 1e-6f) break;
        t -= fx / d;
        if (t < 0) t = 0; else if (t > 1) t = 1;
    }
    return t;
}
static float ease_apply(int ease, const float cb[4], float t0) {
    if (t0 <= 0) return 0;
    if (t0 >= 1) return 1;
    switch (ease) {
    case HN_EASE_LINEAR: return t0;
    case HN_EASE_IN:     return t0 * t0;
    case HN_EASE_OUT:    return 1.0f - (1.0f - t0) * (1.0f - t0);
    case HN_EASE_IN_OUT: return t0 < 0.5f ? 2.0f * t0 * t0
                                           : 1.0f - 2.0f * (1.0f - t0) * (1.0f - t0);
    case HN_EASE_CSS:    return bezier_axis(bezier_solve(t0, 0.25f, 0.25f), 0.1f, 1.0f);
    case HN_EASE_CUBIC:  return bezier_axis(bezier_solve(t0, cb[0], cb[2]), cb[1], cb[3]);
    case HN_EASE_SPRING:
        /* 弹簧: 欠阻尼衰减振荡(闭式解, 无逐帧积分状态)。
           f(t) = 1 - e^(-7t)·cos(9t) —— 峰值约 11% 过冲, t=1 处衰减到位,
           越界由外层 t>=1 钳制精确落位。 */
        return 1.0f - expf(-7.0f * t0) * cosf(9.0f * t0);
    case HN_EASE_ELASTIC:
        /* ease-out-elastic(CSS 缓动函数族标准系数 c4 = 2π/3) */
        return powf(2.0f, -10.0f * t0) * sinf((t0 * 10.0f - 0.75f) * (2.0f * 3.14159265f / 3.0f)) + 1.0f;
    case HN_EASE_BOUNCE: {
        /* ease-out-bounce 标准分段(与 CSS 缓动函数族一致)。
           分段式先减后乘必须拆成两条语句 —— (x -= c) * x 是未定序修改,
           属未定义行为(不同编译器/优化档会算出不同值)。 */
        const float n1 = 7.5625f, d1 = 2.75f;
        float x = t0;
        if (x < 1.0f / d1) return n1 * x * x;
        if (x < 2.0f / d1) { x -= 1.5f / d1; return n1 * x * x + 0.75f; }
        if (x < 2.5f / d1) { x -= 2.25f / d1; return n1 * x * x + 0.9375f; }
        x -= 2.625f / d1;
        return n1 * x * x + 0.984375f;
    }
    case HN_EASE_BACK: {
        /* ease-out-back: 先回拉再越过目标收回(c1 = 1.70158, CSS 标准) */
        const float c1 = 1.70158f, c3 = c1 + 1.0f;
        float p = t0 - 1.0f;
        return 1.0f + c3 * p * p * p + c1 * p * p;
    }
    case HN_EASE_STEPS: {
        /* steps(n[, start|end]): cb[0]=段数, cb[1]=1 为 start(段首取值) */
        float n = cb[0] >= 1.0f ? cb[0] : 1.0f;
        float x = t0 * n;
        return (cb[1] > 0.5f ? ceilf(x) : floorf(x)) / n;
    }
    default:             return t0 * t0 * (3.0f - 2.0f * t0);   /* smoothstep */
    }
}

/* 入场动画的起始状态: 返回 from 值(位移/缩放/透明度) */
static void enter_from(int preset, float *tx, float *ty, float *sc, float *op) {
    *tx = 0; *ty = 0; *sc = 1.0f; *op = 1.0f;
    switch (preset) {
    case HN_ENTER_UP:    *ty = 10.0f; *op = 0.0f; break;
    case HN_ENTER_DOWN:  *ty = -10.0f; *op = 0.0f; break;
    case HN_ENTER_LEFT:  *tx = -12.0f; *op = 0.0f; break;
    case HN_ENTER_FADE:  *op = 0.0f; break;
    case HN_ENTER_SCALE: *sc = 0.94f; *op = 0.0f; break;
    default: break;
    }
}

/* ---- 按属性独立过渡(HN_TPROP_*)的槽位操作 ----
   各槽写入互不重叠的 anim 字段组, 因此可以独立推进再合成。 */

/* 槽内字段 from ← 当前值(重启该槽) */
static void slot_snap(hn_anim *a, int s) {
    switch (s) {
    case HN_TPROP_OPACITY: a->o_from = a->o; break;
    case HN_TPROP_BG:      memcpy(a->bg_from, a->bg, sizeof(a->bg)); break;
    case HN_TPROP_FG:      memcpy(a->fg_from, a->fg, sizeof(a->fg)); break;
    default:               /* TRANSFORM: translate/scale/rotate 整体插值 */
        a->tx_from = a->tx; a->ty_from = a->ty;
        a->sc_from = a->sc; a->rot_from = a->rot;
        break;
    }
}

/* 槽内字段按缓动值 e(0..1, 弹性族可越界)插值 */
static void slot_lerp(hn_anim *a, int s, float e) {
    switch (s) {
    case HN_TPROP_OPACITY: a->o = a->o_from + (a->o_to - a->o_from) * e; break;
    case HN_TPROP_BG:      lerp4(a->bg, a->bg_from, a->bg_to, e); break;
    case HN_TPROP_FG:      lerp4(a->fg, a->fg_from, a->fg_to, e); break;
    default:
        a->tx  = a->tx_from  + (a->tx_to  - a->tx_from)  * e;
        a->ty  = a->ty_from  + (a->ty_to  - a->ty_from)  * e;
        a->sc  = a->sc_from  + (a->sc_to  - a->sc_from)  * e;
        a->rot = a->rot_from + (a->rot_to - a->rot_from) * e;
        break;
    }
}

/* 槽内字段直接落位目标(该槽未在过渡时) */
static void slot_settle(hn_anim *a, int s) {
    switch (s) {
    case HN_TPROP_OPACITY: a->o = a->o_to; break;
    case HN_TPROP_BG:      memcpy(a->bg, a->bg_to, sizeof(a->bg)); break;
    case HN_TPROP_FG:      memcpy(a->fg, a->fg_to, sizeof(a->fg)); break;
    default:
        a->tx = a->tx_to; a->ty = a->ty_to;
        a->sc = a->sc_to; a->rot = a->rot_to;
        break;
    }
}

static int anim_walk(hn_context *c, hn_node *n, float dt_ms, int *amb, int *inter) {
    int active = 0;
    if (n->kind == HN_ELEM) {
        hn_style *st = &n->style;
        hn_anim *a = &n->anim;

        /* 样式计算刚写入的字段就是"目标值"; 但绘制前我们会把插值结果写回同一字段。
           为区分二者: 每次写回后记录 last_bg/last_fg; 下次比较时若与记录一致,
           说明该字段是插值产物, 真正的目标值仍是 a->*_to。 */
        float bg_now[4], fg_now[4];
        rgba_of(st->background, bg_now);
        rgba_of(st->color, fg_now);

        /* 外部资源动画时钟(Lottie / 网格变形 / GIF 动图): 与 CSS 动画共用帧
           循环, 但独立累加。CSS 动画有"播完"概念, Lottie 与 GIF 需要持续
           时钟(自身循环取模)。有声明就把节点标记为活跃, 帧驱动才会持续
           调用 repaint。GIF 只在**真的解出多帧**时才算活跃(单帧 GIF 或
           解码失败退化为静态图, 不该让帧循环空转)。
           这一类是"环境动画"(amb): 只前进 ext_clock, 不写样式字段 ——
           帧节拍可放宽(~30fps)、渲染可走轻路径(免 relayout)。 */
        if (dt_ms > 0 &&
            (hn_node_attr(n, "hn-lottie") || hn_node_attr(n, "hn-mesh") ||
             hn_node_attr(n, "hn-cubism") || hn_node_attr(n, "hn-live") ||
             hn_node_attr(n, "hn-three") || n->mesh_verts)) {
            n->ext_clock += dt_ms;
            *amb = 1;
            active = 1;
        } else if (dt_ms > 0 && hn_node_gif_src(n)) {
            hn_gif *gg = hn_context_gif(c, hn_node_gif_src(n));
            if (gg && hn_gif_frame_count(gg) > 1) {
                /* hn-gif-speed: 与 hn-lottie-speed 同一语义(倍速)。
                   时钟本身保持统一推进, 取帧时按倍速缩放 —— 与 Lottie 在
                   绘制点乘 speed 的做法一致(节点时钟不因倍速而不同频)。 */
                n->ext_clock += dt_ms;
                *amb = 1;
                active = 1;
            }
        }

        /* @keyframes 动画: 声明了 animation-name 且能在样式表里找到定义。
           每帧按时间轴采样, 直接写入样式字段(几何由绘制阶段消费)。
           与入场预设互斥: 有关键帧动画就不再播入场。
           animation-delay 以负时钟实现: 首次挂载时 kf_clock = -delay,
           正延迟期间 kf_tick 只报告"等待"不采样(保持级联样式); 负延迟
           则由取模数学直接从中途进度开始。 */
        if (st->kf_name && st->kf_ms > 0) {
            if (!a->kf) {
                a->kf = find_kf(c, st->kf_name);
                a->kf_clock = -st->anim_delay;
                a->kf_done = 0;
            }
            if (a->kf) {
                int still = 0;
                if (!a->kf_done) {
                    if (dt_ms > 0) a->kf_clock += dt_ms;
                    still = kf_tick(n, a->kf, st->kf_ms, st->kf_iter, st->kf_dir,
                                    &a->kf_clock, &a->kf_done, st);
                } else if (st->kf_fill) {
                    /* kf_fill = forwards/both: 播完保持末帧。样式每次 layout
                       都会从级联重算(把采样值冲掉), 所以每帧重放终点采样 ——
                       保持 still=1 让帧循环驱动补写(此前 fill 声明被完全
                       无视, 动画结束一遇 relayout 就跳回级联值)。 */
                    kf_tick(n, a->kf, st->kf_ms, st->kf_iter, st->kf_dir,
                            &a->kf_clock, &a->kf_done, st);
                    still = 1;
                }
                if (a->kf_clock >= 0) {   /* 正延迟等待期间不写回(样式保持级联值) */
                    a->o = st->opacity;   /* kf_sample 已就地写入 */
                    a->tx = st->translate_x; a->ty = st->translate_y;
                    a->sc = st->scale;       a->rot = st->rotate;
                    a->dirty_written = 1;
                }
                if (still) {
                    active = 1;
                    *inter = 1;   /* 关键帧在采样/保持: 写样式字段 = interactive */
                }
                goto children;
            }
        }

        /* 入场动画: 元素首次出现且声明了 animation 时, 从预设起始态过渡到目标态。
           用独立标志跟踪, 使其不被随后的样式重算打断(每次 relayout 都会把
           st->opacity 等重置为级联结果, 若无独立状态入场会瞬间跳到终态)。
           这让新插入的内容(消息/列表项/卡片)平滑浮现, 而不是硬闪出现。 */
        if (a->entering) {
            if (dt_ms > 0) a->t += dt_ms / a->ms;
            if (a->t >= 1.0f) { a->t = 1.0f; a->entering = 0; a->active = 0; }
            float e2 = ease_apply(a->ease, a->cb, a->t);
            a->o  = a->o_from  + (a->o_to  - a->o_from)  * e2;
            a->tx = a->tx_from + (a->tx_to - a->tx_from) * e2;
            a->ty = a->ty_from + (a->ty_to - a->ty_from) * e2;
            a->sc = a->sc_from + (a->sc_to - a->sc_from) * e2;
            memcpy(a->bg, a->bg_to, sizeof(a->bg));
            memcpy(a->fg, a->fg_to, sizeof(a->fg));
            st->background = color_of(a->bg);
            st->color = color_of(a->fg);
            st->opacity = a->o;
            st->translate_x = a->tx;
            st->translate_y = a->ty;
            st->scale = a->sc;
            a->dirty_written = 1;
            active = 1;
            *inter = 1;   /* 入场过渡在跑 = interactive */
            goto children;
        }
        if (a->fresh && st->anim_enter != HN_ENTER_NONE && st->enter_ms > 0) {
            a->fresh = 0;
            a->inited = 1;
            a->entering = 1;
            float fx, fy, fsc, fop;
            enter_from(st->anim_enter, &fx, &fy, &fsc, &fop);
            a->o = fop;  a->o_from = fop;  a->o_to = st->opacity;
            a->tx = fx;  a->tx_from = fx;  a->tx_to = st->translate_x;
            a->ty = fy;  a->ty_from = fy;  a->ty_to = st->translate_y;
            a->sc = fsc; a->sc_from = fsc; a->sc_to = st->scale;
            a->ms = st->enter_ms;
            a->ease = st->anim_ease;
            memcpy(a->cb, st->cb, sizeof(a->cb));
            rgba_of(st->background, a->bg);
            rgba_of(st->color, a->fg);
            memcpy(a->bg_from, a->bg, sizeof(a->bg));
            memcpy(a->fg_from, a->fg, sizeof(a->fg));
            memcpy(a->bg_to, a->bg, sizeof(a->bg));
            memcpy(a->fg_to, a->fg, sizeof(a->fg));
            /* animation-delay: 负进度起步 —— t<0 期间 ease_apply 返回 0,
               停在起始态(不可见/偏移位)直至延迟耗尽; 负延迟则直接从中途
               进度开始。hn-stagger 错档延迟与声明 delay 合并。 */
            {
                float delay_ms = st->anim_delay + hn_stagger_delay_for(n);
                a->t = a->ms > 0 ? -delay_ms / a->ms : 0;
            }
            a->active = 1;
            a->dirty_written = 0;
            goto children;
        }
        if (a->fresh == 2 && c->enter_dynamic_default &&
            st->anim_enter == HN_ENTER_NONE && st->display != HN_DISP_NONE) {
            /* Smart-UX 默认入场(仅运行时插入; 首帧文档与显式声明不在此路):
               淡入 + 上浮 8px, 200ms ease-out —— 内容自然浮现而非硬闪。 */
            a->fresh = 0;
            a->inited = 1;
            a->entering = 1;
            a->o = 0.0f;  a->o_from = 0.0f;  a->o_to = st->opacity;
            a->ty = 8.0f; a->ty_from = 8.0f; a->ty_to = st->translate_y;
            a->tx = a->tx_from = a->tx_to = st->translate_x;
            a->sc = a->sc_from = a->sc_to = st->scale;
            a->ms = 200.0f;
            a->ease = st->anim_ease;            /* 作者缓动优先(transition 同款) */
            memcpy(a->cb, st->cb, sizeof(a->cb));
            rgba_of(st->background, a->bg);
            rgba_of(st->color, a->fg);
            memcpy(a->bg_from, a->bg, sizeof(a->bg));
            memcpy(a->fg_from, a->fg, sizeof(a->fg));
            memcpy(a->bg_to, a->bg, sizeof(a->bg));
            memcpy(a->fg_to, a->fg, sizeof(a->fg));
            /* hn-stagger 错档: 前序同批入场兄弟数 × 每项延迟, 负进度起步 */
            a->t = -hn_stagger_delay_for(n) / (a->ms > 0 ? a->ms : 1.0f);
            a->active = 1;
            a->dirty_written = 0;
            *inter = 1;
            goto children;
        }
        if (a->fresh) a->fresh = 0;   /* 无入场声明: 直接以终态出现 */

        if (!a->inited) {
            a->inited = 1;
            a->ms = st->transition_ms;
            a->t_delay = st->transition_delay;
            a->tx = a->tx_from = a->tx_to = st->translate_x;
            a->ty = a->ty_from = a->ty_to = st->translate_y;
            a->sc = a->sc_from = a->sc_to = st->scale;
            a->rot = a->rot_from = a->rot_to = st->rotate;
            a->ease = st->anim_ease;
            a->per_prop = st->has_tprop ? 1 : 0;   /* 槽配置现读现用, 不缓存 */
            a->slot_active = 0;
            for (int s = 0; s < HN_TPROP_N; s++) a->slot_t[s] = 0;
            a->o = a->o_to = st->opacity;
            memcpy(a->bg, bg_now, sizeof(bg_now));
            memcpy(a->fg, fg_now, sizeof(fg_now));
            memcpy(a->bg_to, bg_now, sizeof(bg_now));
            memcpy(a->fg_to, fg_now, sizeof(fg_now));
            memcpy(a->bg_from, bg_now, sizeof(bg_now));
            memcpy(a->fg_from, fg_now, sizeof(fg_now));
            a->o_from = a->o_to;
            a->active = 0;
            goto children;
        }

        {
            /* 采用哪个值作为目标:
               - 若上次 tick 写回过(dirty_written) 且当前样式字段与我们写回的一致
                 → 字段是插值产物, 目标取 a->*_to
               - 否则字段就是样式计算刚写入的目标 */
            /* "写回者"判定按**量化后的打包色**比较(u32 相等), 不按 float
               位型 memcmp: 插值产物写回样式要经 color_of 量化, 下帧重读
               (rgba_of)的分量与插值 float 几乎必有位差 —— 按位比较永远判
               成"样式变过", 中途帧被当成新目标(量化后的当前值)重启, 过渡
               在第一步后自我回吞、永远到不了端点(与本文件下方 opacity
               判定的修复同一类坑: 写回者要按"经样式打包往返后的值"认领)。 */
            int style_is_ours = a->dirty_written &&
                color_of(a->bg) == st->background &&
                color_of(a->fg) == st->color;
            float bg_target[4], fg_target[4];
            if (style_is_ours) {
                memcpy(bg_target, a->bg_to, sizeof(bg_target));
                memcpy(fg_target, a->fg_to, sizeof(fg_target));
            } else {
                memcpy(bg_target, bg_now, sizeof(bg_target));
                memcpy(fg_target, fg_now, sizeof(fg_target));
                a->dirty_written = 0;   /* 新目标来自样式计算 */
            }

            /* opacity 的"写回者"判定**独立**于背景/前景: 过渡系统每次 tick
               都把插值浮点直拷进 st->opacity, 因此"样式字段 == 当前插值"
               即是我们写的。此前 opacity 搭乘 bg/fg 的 memcmp 判定 —— 只改
               opacity 的样式重算(透明度 hover/fade)会被误判成"还是我们写的",
               新目标被 a->o_to 顶掉: 过渡只对第一次变更生效, 回切永远卡死
               (实测: 1→0.4 正常, 0.4→1 永远停在 0.4)。 */
            int style_o_ours = a->dirty_written && st->opacity == a->o;

            /* 几何目标: 与颜色同样要区分"样式写入的目标"和"上次写回的插值"。
               rotate 与 translate/scale 同属 TRANSFORM 槽(CSS transform 是
               单条属性) —— 此前 rotate 完全不可过渡(kf 路径之外无写点)。 */
            float tx_target, ty_target, sc_target, o_target, rot_target;
            if (style_is_ours) {
                tx_target = a->tx_to; ty_target = a->ty_to;
                sc_target = a->sc_to;
                rot_target = a->rot_to;
            } else {
                tx_target = st->translate_x; ty_target = st->translate_y;
                sc_target = st->scale;
                rot_target = st->rotate;
            }
            o_target = style_o_ours ? a->o_to : st->opacity;

            /* per-property 过渡开关(样式声明了属性列表 → 按槽推进) */
            unsigned char per_prop = st->has_tprop ? 1 : 0;

            if (per_prop) {
                /* ---- 每槽独立时长/延迟/缓动 ----
                   目标变化 → 该槽从当前值重启(延迟换算成负进度);
                   槽配置现读 st->tprop_*(样式表热换立即生效)。 */
                int ch[HN_TPROP_N] = {0, 0, 0, 0};
                int any_active = 0;
                if (per_prop != a->per_prop) {
                    /* 单一配置 ↔ 按槽配置翻转: 所有槽视为"目标已变",
                       从当前值按各自配置重启(避免中途跳变)。 */
                    a->per_prop = per_prop;
                    a->slot_active = 0;
                    for (int s = 0; s < HN_TPROP_N; s++) { ch[s] = 1; a->slot_t[s] = 0; }
                }
                a->active = 0;   /* 简写路径的活跃位不参与按槽推进 */
                for (int i = 0; i < 4; i++) {
                    if (fabsf(bg_target[i] - a->bg_to[i]) > 0.004f) { a->bg_to[i] = bg_target[i]; ch[HN_TPROP_BG] = 1; }
                    if (fabsf(fg_target[i] - a->fg_to[i]) > 0.004f) { a->fg_to[i] = fg_target[i]; ch[HN_TPROP_FG] = 1; }
                }
                if (fabsf(o_target - a->o_to) > 0.004f) { a->o_to = o_target; ch[HN_TPROP_OPACITY] = 1; }
                if (fabsf(tx_target - a->tx_to) > 0.02f) { a->tx_to = tx_target; ch[HN_TPROP_TRANSFORM] = 1; }
                if (fabsf(ty_target - a->ty_to) > 0.02f) { a->ty_to = ty_target; ch[HN_TPROP_TRANSFORM] = 1; }
                if (fabsf(sc_target - a->sc_to) > 0.0005f) { a->sc_to = sc_target; ch[HN_TPROP_TRANSFORM] = 1; }
                if (fabsf(rot_target - a->rot_to) > 0.02f) { a->rot_to = rot_target; ch[HN_TPROP_TRANSFORM] = 1; }

                for (int s = 0; s < HN_TPROP_N; s++) {
                    float ms_s = st->tprop_ms[s];
                    if (ch[s]) {
                        slot_snap(a, s);
                        a->slot_t[s] = ms_s > 0 ? -st->tprop_delay[s] / ms_s : 0;
                        if (ms_s > 0) a->slot_active |= (unsigned char)(1 << s);
                        else a->slot_active &= (unsigned char)~(1 << s);
                    }
                    if (a->slot_active & (1 << s)) {
                        if (dt_ms > 0) a->slot_t[s] += dt_ms / ms_s;
                        if (a->slot_t[s] >= 1.0f) { a->slot_t[s] = 1.0f; a->slot_active &= (unsigned char)~(1 << s); }
                        float e = ease_apply(st->tprop_ease[s], st->tprop_cb[s], a->slot_t[s]);
                        slot_lerp(a, s, e);
                        if (a->slot_active & (1 << s)) any_active = 1;
                    } else {
                        slot_settle(a, s);   /* 未过渡槽直接落位目标 */
                    }
                }

                st->background = color_of(a->bg);
                st->color = color_of(a->fg);
                st->opacity = a->o;
                st->translate_x = a->tx;
                st->translate_y = a->ty;
                st->scale = a->sc;
                st->rotate = a->rot;
                a->dirty_written = 1;
                if (any_active) {
                    active = 1;
                    *inter = 1;   /* 按槽过渡在途 = interactive */
                }
            } else {
                /* ---- 单一配置过渡(旧路径 + rotate + 延迟) ---- */
                int changed = 0;
                for (int i = 0; i < 4; i++) {
                    if (fabsf(bg_target[i] - a->bg_to[i]) > 0.004f) { a->bg_to[i] = bg_target[i]; changed = 1; }
                    if (fabsf(fg_target[i] - a->fg_to[i]) > 0.004f) { a->fg_to[i] = fg_target[i]; changed = 1; }
                }
                if (fabsf(tx_target - a->tx_to) > 0.02f) { a->tx_to = tx_target; changed = 1; }
                if (fabsf(ty_target - a->ty_to) > 0.02f) { a->ty_to = ty_target; changed = 1; }
                if (fabsf(sc_target - a->sc_to) > 0.0005f) { a->sc_to = sc_target; changed = 1; }
                if (fabsf(rot_target - a->rot_to) > 0.02f) { a->rot_to = rot_target; changed = 1; }
                if (fabsf(o_target - a->o_to) > 0.004f) { a->o_to = o_target; changed = 1; }
                if (st->transition_ms != a->ms) { a->ms = st->transition_ms; changed = 1; }
                if (st->transition_delay != a->t_delay) { a->t_delay = st->transition_delay; changed = 1; }
                if (st->anim_ease != a->ease) { a->ease = st->anim_ease; changed = 1; }
                memcpy(a->cb, st->cb, sizeof(a->cb));

                if (changed) {
                    memcpy(a->bg_from, a->bg, sizeof(a->bg));
                    memcpy(a->fg_from, a->fg, sizeof(a->fg));
                    a->o_from = a->o;
                    a->tx_from = a->tx;
                    a->ty_from = a->ty;
                    a->sc_from = a->sc;
                    a->rot_from = a->rot;
                    /* 延迟 = 负进度: t<0 期间 ease_apply 返回 0, 停在起始态 */
                    a->t = a->ms > 0 ? -a->t_delay / a->ms : 0;
                    a->active = (a->ms > 0);
                }

                if (a->active && a->ms > 0) {
                    if (dt_ms > 0) a->t += dt_ms / a->ms;
                    if (a->t >= 1.0f) { a->t = 1.0f; a->active = 0; }
                    float e = ease_apply(a->ease, a->cb, a->t);
                    lerp4(a->bg, a->bg_from, a->bg_to, e);
                    lerp4(a->fg, a->fg_from, a->fg_to, e);
                    a->o  = a->o_from  + (a->o_to  - a->o_from)  * e;
                    a->tx = a->tx_from + (a->tx_to - a->tx_from) * e;
                    a->ty = a->ty_from + (a->ty_to - a->ty_from) * e;
                    a->sc = a->sc_from + (a->sc_to - a->sc_from) * e;
                    a->rot = a->rot_from + (a->rot_to - a->rot_from) * e;
                } else {
                    memcpy(a->bg, a->bg_to, sizeof(a->bg));
                    memcpy(a->fg, a->fg_to, sizeof(a->fg));
                    a->o = a->o_to;
                    a->tx = a->tx_to; a->ty = a->ty_to; a->sc = a->sc_to;
                    a->rot = a->rot_to;
                }

                st->background = color_of(a->bg);
                st->color = color_of(a->fg);
                st->opacity = a->o;
                /* 几何插值写回样式字段: 绘制阶段按此偏移/缩放(read-only 消费) */
                st->translate_x = a->tx;
                st->translate_y = a->ty;
                st->scale = a->sc;
                st->rotate = a->rot;
                a->dirty_written = 1;
            }
        }
        if (a->active) {
            active = 1;
            *inter = 1;   /* 单一配置过渡在途 = interactive */
        }
    }
children:
    for (hn_node *ch = n->first; ch; ch = ch->next)
        if (anim_walk(c, ch, dt_ms, amb, inter)) active = 1;
    return active;
}

/* 推进过渡动画并写回样式; 返回 1 表示仍有动画在跑(dt_ms<0 → 仅初始化) */
/* ---------------- 滚动 ---------------- */

/* ---- hn-stream: 流式容器自动跟随底部 ----
 *
 * README 承诺的 hn-stream 语义(自 Swift 版移植): 声明 hn-stream 的
 * overflow 容器, 内容增长时滚动偏移自动贴底(跟随); 用户上翻(离底
 * > HN_STREAM_NEAR px)则让出不追; 用户滚回底部附近(≤ HN_STREAM_NEAR)
 * 恢复跟随。实现挂在 hn_context_layout 尾部: 布局定稿后按让出状态
 * 校正偏移, 再进绘制 —— 显示列表与位图直接反映锚定结果。
 * 跟随的"贴底"分两种口径: 上下文有过帧循环(c->live_frame)则武装平滑
 * 追上动画, 由 hn_context_anim_tick 逐帧指数逼近; 无帧循环的管线
 * (离屏 shot/dump, 无从推进动画)保持直接贴底 —— 单帧出图仍是终态。
 * 让出状态存节点上: 文档替换(节点重建)即回默认跟随; 同一容器内的
 * swap 追加保持节点身份, 状态随之保留。 */
#define HN_STREAM_NEAR 48.0f
#define HN_STREAM_TAU  80.0f   /* 平滑追上的时间常数(ms) */

static int is_stream_container(hn_node *n) {
    return n->kind == HN_ELEM && n->style.overflow != 0 &&
           hn_node_attr(n, "hn-stream") != NULL;
}

/* ---- hn-stream-loop="N": 环形缓冲滑窗 ----
 * 声明 hn-stream-loop="N" 的 hn-stream 容器只保留最近 N 个**直接子元素**,
 * 超出即从最旧(链首)摘除 —— 内容可无限追加而 DOM 与视觉收敛(agent
 * 轨迹/日志场景; examples/agent-stream.html 的 14 条窗口)。
 * 摘除走 hn_node_trim_children: 从兄弟链表解链(first/last/prev/next
 * 重接, 被摘节点自身指针清空防遍历成环)。arena 不回收单节点 —— 既有
 * 口径是整体销毁/compact 重建(hn_context_compact), 摘除后的节点再无
 * 任何遍历触达, 内存留待 arena 回收周期。
 * N<=0 或非数字(atoi 得 0)= 不限。
 * 挂点: 布局后、贴底锚定前(与 stream_anchor_walk 同一 walkers)。返回
 * 摘除数量; 非 0 时 hn_context_layout 再走一遍布局 —— 摘除改变子项
 * 栈叠, 保留子项的坐标与容器 content_h 必须重算(见 hn_context_layout)。 */
static int stream_loop_walk(hn_node *n) {
    if (n->kind != HN_ELEM) return 0;
    int removed = 0;
    if (is_stream_container(n)) {
        const char *v = hn_node_attr(n, "hn-stream-loop");
        int keep = v ? atoi(v) : 0;
        if (keep > 0) removed = hn_node_trim_children(n, keep);
    }
    for (hn_node *ch = n->first; ch; ch = ch->next)
        removed += stream_loop_walk(ch);
    return removed;
}

/* ---- 平滑追上: 跟随从"直接贴底"改为逐帧指数逼近 ----
 * 每帧向目标(当前底部 max_y)推进 step = gap·(1−e^(−dt/τ)), τ≈80ms ——
 * 帧率无关: 同一墙钟走过同一比例(大 dt 一步走掉大半 gap, 小 dt 小步)。
 * gap 落入 ±1px 直接贴到位并结束动画。目标每帧现算(content_h-bh):
 * 动画途中内容再增长, 目标自动前移, 不需要重启。
 * 用户让出(上翻 > 48px, hn_node_scroll_by 置位 stream_yield 并清
 * stream_anim)立即中断 —— 让出状态机只有一套。
 * 确定性: 纯 float 固定顺序运算 + libm expf(同平台同结果), 无随机 ——
 * 同一 dt 序列喂出同一轨迹(HN_CLOCK/anim_tick 驱动下多帧收敛贴底)。
 * 返回 1 = 仍有容器在途: hn_context_anim_tick 并入返回值保持帧循环。 */
static int stream_anim_walk(hn_node *n, float dt_ms) {
    int active = 0;
    if (n->kind == HN_ELEM && is_stream_container(n) &&
        n->stream_anim && !n->stream_yield) {
        float max_y = n->content_h - n->bh;
        if (max_y < 0) max_y = 0;
        float gap = max_y - n->scroll_y;
        if (gap > -1.0f && gap < 1.0f) {
            n->scroll_y = max_y;        /* 已贴底: 当帧落位, 动画结束 */
            n->stream_anim = 0;
        } else if (dt_ms > 0) {
            /* 朝底部推进不会触发让出(离底单调减小), 直接写偏移并钳界 */
            float ny = n->scroll_y + gap * (1.0f - expf(-dt_ms / HN_STREAM_TAU));
            if (ny < 0) ny = 0;
            if (ny > max_y) ny = max_y;
            n->scroll_y = ny;
            active = 1;                 /* 未到底: 下一帧继续 */
        }
    }
    for (hn_node *ch = n->first; ch; ch = ch->next)
        if (stream_anim_walk(ch, dt_ms)) active = 1;
    return active;
}

/* ---- 锚点平滑滚动推进: #锚导航的逐帧逼近(与 stream_anim_walk 同款) ----
 * hn_context_scroll_to 在实时帧口径下武装各溢出容器(记下"把锚滚进可视
 * 区"的最终偏移 scroll_target), 这里每帧向目标推进 step = gap·(1−e^(−dt/τ)),
 * τ 与 stream 平滑追上一致(HN_STREAM_TAU = 80ms) —— 帧率无关, 纯 float
 * 固定顺序运算 + libm expf, 同一 dt 序列喂出同一轨迹(确定性口径与 stream
 * 相同)。|gap| < 1px 当帧落位并结束动画。
 * 用户滚轮/脚本滚动(hn_node_scroll_by)清 scroll_anim 立即中断 —— 与
 * stream 让出同一套"用户接管即停"纪律。
 * 返回 1 = 仍有容器在途: hn_context_anim_tick 并入返回值保持帧循环。 */
static int scroll_anim_walk(hn_node *n, float dt_ms) {
    int active = 0;
    if (n->kind == HN_ELEM && n->scroll_anim) {
        float max_y = n->content_h - n->bh;
        if (max_y < 0) max_y = 0;
        float target = n->scroll_target;
        if (target < 0) target = 0;
        if (target > max_y) target = max_y;   /* 布局中途变矮: 目标重钳 */
        float gap = target - n->scroll_y;
        if (gap > -1.0f && gap < 1.0f) {
            n->scroll_y = target;       /* 已到位: 当帧落位, 动画结束 */
            n->scroll_anim = 0;
        } else if (dt_ms > 0) {
            float ny = n->scroll_y + gap * (1.0f - expf(-dt_ms / HN_STREAM_TAU));
            if (ny < 0) ny = 0;
            if (ny > max_y) ny = max_y;
            n->scroll_y = ny;
            active = 1;                 /* 未到位: 下一帧继续 */
        }
    }
    for (hn_node *ch = n->first; ch; ch = ch->next)
        if (scroll_anim_walk(ch, dt_ms)) active = 1;
    return active;
}

/* layout 尾部遍历: 对所有 hn-stream 容器做贴底/让出校正(绘制前) */
static void stream_anchor_walk(hn_context *c, hn_node *n) {
    if (n->kind != HN_ELEM) return;
    if (is_stream_container(n)) {
        float max_y = n->content_h - n->bh;
        if (max_y < 0) max_y = 0;
        if (n->stream_yield) {
            /* 用户在看历史: 不追。仅钳制越界(内容变矮), 滚回底部附近
               (≤ HN_STREAM_NEAR)则恢复跟随。 */
            if (n->scroll_y > max_y) n->scroll_y = max_y;
            if (max_y - n->scroll_y <= HN_STREAM_NEAR) n->stream_yield = 0;
        }
        if (!n->stream_yield) {
            float gap = max_y - n->scroll_y;
            if (gap > -1.0f && gap < 1.0f) {
                n->scroll_y = max_y;    /* 已在底部(<1px): 当帧落位 */
                n->stream_anim = 0;
            } else if (c->live_frame) {
                /* 跟随(帧循环活跃): 武装平滑追上, anim_tick 逐帧逼近 */
                n->stream_anim = 1;
            } else {
                /* 跟随(无帧循环: 离屏/守护进程, 无从推进动画): 保持
                   旧口径直接贴底 —— 单帧出图即终态, 截图管线不变 */
                n->scroll_y = max_y;
                n->stream_anim = 0;
            }
        }
    }
    for (hn_node *ch = n->first; ch; ch = ch->next) stream_anchor_walk(c, ch);
}

/* 找点位下最深的 overflow 容器 */
static hn_node *scroll_walk(hn_node *n, float x, float y) {
    if (n->kind == HN_ELEM && n->style.display == HN_DISP_NONE) return NULL;
    if (x < n->bx || y < n->by || x >= n->bx + n->bw || y >= n->by + n->bh) return NULL;
    hn_node *found = NULL;
    for (hn_node *ch = n->last; ch; ch = ch->prev) {
        hn_node *r = scroll_walk(ch, x + n->scroll_x, y + n->scroll_y);
        if (r) { found = r; break; }
    }
    if (found) return found;
    return (n->kind == HN_ELEM && n->style.overflow) ? n : NULL;
}

hn_node *hn_context_scrollable_at(hn_context *c, float x, float y) {
    if (!c->doc) return NULL;
    return scroll_walk(c->doc->root, x, y);
}

int hn_node_scroll_by(hn_node *n, float dx, float dy) {
    if (!n || n->kind != HN_ELEM || !n->style.overflow) return 0;
    /* 用户滚轮/脚本滚动即接管: 在途的锚点平滑滚动立即中断 —— 与 stream
       让出同一套"用户介入即停"纪律, 防止动画把用户刚滚到的位置又拉走。 */
    n->scroll_anim = 0;
    float max_y = n->content_h - n->bh;
    if (max_y < 0) max_y = 0;
    float max_x = 0; /* v1 仅纵向滚动 */
    float ny = n->scroll_y + dy;
    if (ny < 0) ny = 0;
    if (ny > max_y) ny = max_y;
    (void)dx; (void)max_x;
    /* hn-stream 容器: 滚动即更新"让出"状态 —— 离底 > 48px 视为用户在
       看历史(布局不再贴底); 滚回底部附近(≤ 48px)恢复跟随。
       让出同时清掉在途的平滑追上动画 —— 用户接管, 立即中断。 */
    if (is_stream_container(n)) {
        n->stream_yield = (max_y - ny > HN_STREAM_NEAR) ? 1 : 0;
        if (n->stream_yield) n->stream_anim = 0;
    }
    if (ny == n->scroll_y) return 0;
    n->scroll_y = ny;
    return 1;
}

/* ---------------- 事件 ---------------- */

const char *hn_event_name(hn_event_kind k) {
    switch (k) {
    case HN_EV_CLICK:      return "click";
    case HN_EV_DBLCLICK:   return "dblclick";
    case HN_EV_MOUSEDOWN:  return "mousedown";
    case HN_EV_MOUSEUP:    return "mouseup";
    case HN_EV_MOUSEMOVE:  return "mousemove";
    case HN_EV_MOUSEENTER: return "mouseenter";
    case HN_EV_MOUSELEAVE: return "mouseleave";
    case HN_EV_KEYDOWN:    return "keydown";
    case HN_EV_KEYUP:      return "keyup";
    case HN_EV_FOCUS:      return "focus";
    case HN_EV_BLUR:       return "blur";
    case HN_EV_INPUT:      return "input";
    case HN_EV_CHANGE:     return "change";
    case HN_EV_SUBMIT:     return "submit";
    case HN_EV_SCROLL:     return "scroll";
    case HN_EV_HOVER:      return "hover";
    case HN_EV_LAUNCH:     return "launch";
    case HN_EV_SHOW:       return "show";
    case HN_EV_HIDE:       return "hide";
    case HN_EV_DESTROY:    return "destroy";
    default:               return "unknown";
    }
}

const char *hn_lifecycle_name(hn_event_kind k) {
    switch (k) {
    case HN_EV_LAUNCH:  return "launch";
    case HN_EV_SHOW:    return "show";
    case HN_EV_HIDE:    return "hide";
    case HN_EV_DESTROY: return "destroy";
    default:            return NULL;
    }
}

int hn_lifecycle_wants(const char *decl, hn_event_kind k) {
    const char *name = hn_lifecycle_name(k);
    if (!decl || !*decl || !name) return 0;
    size_t nl = strlen(name);
    const char *p = decl;
    while (*p) {
        while (*p == ' ' || *p == ',') p++;
        const char *st = p;
        while (*p && *p != ' ' && *p != ',') p++;
        if ((size_t)(p - st) == nl && !strncmp(st, name, nl)) return 1;
    }
    return 0;
}

/* 冒泡路径只含"有 id 的元素": 运行时与 JS 都以 id 寻址。
   这样路径长度小(通常 2-4 层), 派发成本低。 */
hn_node *hn_event_path_at(hn_node *target, int idx) {
    if (!target || idx < 0) return NULL;
    int seen = 0;
    for (hn_node *n = target; n; n = n->parent) {
        if (n->kind != HN_ELEM) continue;
        if (!hn_node_attr(n, "id")) continue;
        if (seen == idx) return n;
        seen++;
    }
    return NULL;
}

int hn_event_path_len(hn_node *target) {
    if (!target) return 0;
    int seen = 0;
    for (hn_node *n = target; n; n = n->parent)
        if (n->kind == HN_ELEM && hn_node_attr(n, "id")) seen++;
    return seen;
}

hn_node *hn_node_ancestor_input(hn_node *n) {
    for (; n; n = n->parent)
        /* 按钮: 不能成为键盘输入目标; range 可以(方向键调值) */
        if (hn_node_is_input(n)) {
            if (hn_node_input_kind(n) != HN_IN_BUTTON) return n;
        }
    return NULL;
}

void hn_node_scroll_get(hn_node *n, float *x, float *y) {
    if (x) *x = n ? n->scroll_x : 0;
    if (y) *y = n ? n->scroll_y : 0;
}

void hn_node_scroll_range(hn_node *n, float *max_x, float *max_y) {
    float mx = 0, my = 0;
    if (n && n->kind == HN_ELEM && n->style.overflow) {
        my = n->content_h - n->bh;
        if (my < 0) my = 0;
    }
    if (max_x) *max_x = mx;
    if (max_y) *max_y = my;
}

/* 可增长的遍历栈 —— 固定数组在 DOM 变大后会静默丢节点(遍历不完整),
   表现为"某些元素查不到/流式容器丢失"。这里按需扩容, 不设上限。 */
typedef struct { hn_node **v; int n, cap; } nstack;

static void ns_init(nstack *st, int cap) {
    st->v = malloc(sizeof(hn_node *) * (size_t)cap);
    st->n = 0; st->cap = st->v ? cap : 0;
}
static void ns_push(nstack *st, hn_node *n) {
    if (st->n == st->cap) {
        int nc = st->cap ? st->cap * 2 : 512;
        hn_node **nv = realloc(st->v, sizeof(hn_node *) * (size_t)nc);
        if (!nv) return;              /* 内存耗尽: 放弃该节点(不崩溃) */
        st->v = nv; st->cap = nc;
    }
    st->v[st->n++] = n;
}
static int  ns_pop(nstack *st, hn_node **out) {
    if (st->n == 0) return 0;
    *out = st->v[--st->n];
    return 1;
}
static void ns_free(nstack *st) { free(st->v); st->v = NULL; st->n = st->cap = 0; }

hn_node *hn_doc_find_attr(hn_doc *doc, const char *attr, int idx) {
    if (!doc || !doc->root || !attr) return NULL;
    nstack st;
    ns_init(&st, 512);
    if (!st.v) return NULL;
    ns_push(&st, doc->root);
    hn_node *found = NULL;
    hn_node *n;
    int seen = 0;
    while (ns_pop(&st, &n)) {
        if (n->kind == HN_ELEM && hn_node_attr(n, attr)) {
            if (seen == idx) { found = n; break; }
            seen++;
        }
        for (hn_node *ch = n->last; ch; ch = ch->prev) ns_push(&st, ch);
    }
    ns_free(&st);
    return found;
}

/* 环形缓冲: 流式视图(日志/对话/agent 轨迹)只需保留最近 N 条。
   移除的是最旧的若干个元素子节点, 因此内容可以无限追加而内存与视觉都收敛。 */
int hn_node_trim_children(hn_node *n, int keep_last) {
    if (!n || keep_last <= 0) return 0;
    /* 数元素子节点 */
    int total = 0;
    for (hn_node *ch = n->first; ch; ch = ch->next)
        if (ch->kind == HN_ELEM) total++;
    int drop = total - keep_last;
    if (drop <= 0) return 0;

    int removed = 0;
    hn_node *ch = n->first;
    while (ch && removed < drop) {
        hn_node *nx = ch->next;
        if (ch->kind == HN_ELEM) {
            /* 摘链: 双向链表两侧都要接好 */
            if (ch->prev) ch->prev->next = ch->next; else n->first = ch->next;
            if (ch->next) ch->next->prev = ch->prev; else n->last = ch->prev;
            /* 关键: 被摘节点自身指针必须清空 —— 否则旧 next/prev 仍指向链上,
               任何沿旧指针的遍历(如 paint_walk)都可能绕回已摘节点形成环 → 栈溢出。 */
            ch->next = NULL;
            ch->prev = NULL;
            ch->parent = NULL;
            removed++;
        }
        ch = nx;
    }
    /* 一致性兜底: 链表首尾若仍指向已摘节点则修正 */
    if (n->first && n->first->prev) n->first->prev = NULL;
    if (n->last && n->last->next) n->last->next = NULL;
    return removed;
}

/* 结构自检: 显式 visited 表 + 预算上限, 任何环都会被检出而不是死循环 */
/* 命中自洽: 带 id 的元素盒中心必须命中自身或带 id 后代。
   深度上限防 DOM 异常时爆栈(与 hn_doc_validate 的 node_budget 同理)。 */
#define HN_VERIFY_MAX_DEPTH 256

static int verify_hits_d(hn_context *c, hn_node *n, int *checked, int depth) {
    int bad = 0;
    if (depth > HN_VERIFY_MAX_DEPTH) return 0;
    if (n && hn_node_tag(n)) {
        const char *id = hn_node_attr(n, "id");
        if (id && id[0]) {
            float x, y, w, h;
            hn_node_box(n, &x, &y, &w, &h);
            if (w > 0 && h > 0) {
                (*checked)++;
                const char *hit = hn_context_hit_test(c, x + w / 2, y + h / 2);
                if (!hit) {
                    fprintf(stderr, "  ✘ 命中缺失: id=%s 中心点 (%.0f,%.0f) 无命中\n",
                            id, x + w / 2, y + h / 2);
                    bad++;
                } else if (strcmp(hit, id) != 0) {
                    /* 命中到带 id 的后代是正常的(后代覆盖父级中心) */
                }
            }
        }
    }
    for (hn_node *ch = hn_node_first_child(n); ch; ch = hn_node_next_sibling(ch))
        bad += verify_hits_d(c, ch, checked, depth + 1);
    return bad;
}

int hn_context_verify_hits(hn_context *c, int *checked) {
    if (checked) *checked = 0;
    if (!c || !c->doc || !c->doc->root) return 0;
    return verify_hits_d(c, c->doc->root, checked, 0);
}

int hn_doc_validate(hn_doc *doc, int node_budget) {
    if (!doc || !doc->root) return 1;
    if (node_budget <= 0) node_budget = 100000;
    int issues = 0, visited = 0;
    nstack st;
    ns_init(&st, 512);
    if (!st.v) return 1;
    ns_push(&st, doc->root);
    hn_node *n;
    while (ns_pop(&st, &n)) {
        if (++visited > node_budget) { issues++; break; }   /* 超预算: 疑似环 */
        /* 自引用 */
        if (n->next == n || n->prev == n || n->first == n || n->last == n) issues++;
        /* 父子一致性 */
        for (hn_node *ch = n->first; ch; ch = ch->next) {
            if (ch->parent != n) issues++;
            if (ch == n) { issues++; break; }               /* 自己是自己的子节点 */
            ns_push(&st, ch);
        }
        /* 双向链一致性: first->prev 应为 NULL, last->next 应为 NULL */
        if (n->first && n->first->prev) issues++;
        if (n->last && n->last->next) issues++;
    }
    ns_free(&st);
    return issues;
}

const char *hn_context_hit_test(hn_context *c, float x, float y) {
    hn_node *n = hn_context_hit_node(c, x, y);
    /* 无 id 的元素继续向上找最近有 id 的祖先(冒泡语义) */
    while (n) {
        if (n->id) return n->id;
        n = n->parent;
    }
    return NULL;
}

/* ---------------- 属性 / 查找 ---------------- */

const char *hn_node_tag(hn_node *n) {
    if (!n || n->kind != HN_ELEM) return NULL;
    return n->tag;
}

const char *hn_node_attr(hn_node *n, const char *name) {
    if (!n) return NULL;
    for (int i = 0; i < n->n_attrs; i++)
        if (!strcmp(n->attrs[i].name, name)) return n->attrs[i].value;
    return NULL;
}

int hn_node_attr_at(hn_node *n, int idx, const char **name, const char **value) {
    if (!n || n->kind != HN_ELEM || idx < 0 || idx >= n->n_attrs) return 0;
    if (name) *name = n->attrs[idx].name;
    if (value) *value = n->attrs[idx].value;
    return 1;
}

hn_node *hn_node_ancestor_with_attr(hn_node *n, const char *name) {
    for (; n; n = n->parent)
        if (n->kind == HN_ELEM && hn_node_attr(n, name)) return n;
    return NULL;
}

/* root 子树内(文档序, 跳过 exclude 所在分支)第一个携带指定属性的元素 */
static hn_node *subtree_first_with_attr(hn_node *root, hn_node *exclude, const char *name) {
    if (root != exclude && root->kind == HN_ELEM && hn_node_attr(root, name)) return root;
    for (hn_node *ch = root->first; ch; ch = ch->next) {
        if (ch == exclude) continue;
        hn_node *r = subtree_first_with_attr(ch, exclude, name);
        if (r) return r;
    }
    return NULL;
}

hn_node *hn_node_form_carrier(hn_node *n) {
    if (!n || n->kind != HN_ELEM) return NULL;
    if (hn_node_attr(n, "hx-post") || hn_node_attr(n, "hx-get")) return n;
    int depth = 0;
    for (hn_node *p = n->parent; p && depth < 6; p = p->parent, depth++) {
        if (p->kind == HN_ELEM && (hn_node_attr(p, "hx-post") || hn_node_attr(p, "hx-get")))
            return p;
        hn_node *f = subtree_first_with_attr(p, n, "hx-post");
        if (!f) f = subtree_first_with_attr(p, n, "hx-get");
        if (f) return f;
    }
    return NULL;
}

static hn_node *find_by_id_rec(hn_node *n, const char *id) {
    if (n->kind == HN_ELEM) {
        const char *vid = hn_node_attr(n, "id");
        if (vid && !strcmp(vid, id)) return n;
        for (hn_node *ch = n->first; ch; ch = ch->next) {
            hn_node *r = find_by_id_rec(ch, id);
            if (r) return r;
        }
    }
    return NULL;
}

int hn_doc_autoid_hx(hn_doc *doc) {
    if (!doc || !doc->root) return 0;
    static int counter = 0;   /* 单调递增: 新分配不与既有 id 冲突, 避免重复命名 */
    int assigned = 0;
    hn_node *stack[256];
    int sp = 0;
    stack[sp++] = doc->root;
    while (sp) {
        hn_node *n = stack[--sp];
        if (n->kind != HN_ELEM) continue;
        int has_hx = hn_node_attr(n, "hx-get") || hn_node_attr(n, "hx-post");
        if (has_hx && !hn_node_attr(n, "id") && n->arena) {
            char buf[32];
            snprintf(buf, sizeof(buf), "hx-auto-%d", ++counter);
            /* 若该 id 已被占用(热更新残留), 继续递增 */
            while (find_by_id_rec(doc->root, buf)) {
                snprintf(buf, sizeof(buf), "hx-auto-%d", ++counter);
            }
            size_t vl = strlen(buf) + 1;
            char *v = hn_arena_alloc(n->arena, vl);
            if (v) {
                memcpy(v, buf, vl);
                hn_attr *na = hn_arena_alloc(n->arena, sizeof(hn_attr) * (size_t)(n->n_attrs + 1));
                if (na) {
                    for (int i = 0; i < n->n_attrs; i++) na[i] = n->attrs[i];
                    na[n->n_attrs].name = "id";
                    na[n->n_attrs].value = v;
                    n->attrs = na;
                    n->n_attrs++;
                    assigned++;
                }
            }
        }
        for (hn_node *ch = n->last; ch; ch = ch->prev)
            if (sp < 256) stack[sp++] = ch;
    }
    return assigned;
}

hn_node *hn_doc_root(hn_doc *doc) {
    return (doc && doc->root) ? doc->root : NULL;
}

hn_node *hn_doc_body(hn_doc *doc) {
    if (!doc || !doc->root) return NULL;
    for (hn_node *ch = doc->root->first; ch; ch = ch->next)
        if (ch->kind == HN_ELEM && ch->tag && !strcmp(ch->tag, "body")) return ch;
    return doc->root;
}

hn_node *hn_doc_find_by_id(hn_doc *doc, const char *id) {
    if (!doc || !doc->root || !id) return NULL;
    return find_by_id_rec(doc->root, id);
}

/* ---------------- 链接导航(markdown a[href] 的点击语义) ----------------
 *
 * 引擎不做"打开浏览器"这类平台动作 —— 外部链接只置入待消费链接, 宿主
 * (daemon/壳/rt)在 click 派发处调 hn_context_activate_link 后按返回值
 * 处理: ANCHOR 已在引擎内滚动(实时帧下是平滑滚动动画在途, 需持续帧
 * 循环 + repaint), EXTERNAL 则 take 出 href 交系统打开(与 sys://open
 * 同款动作)。 #锚滚动复用既有溢出容器语义: 布局盒是内容坐标(未含滚动
 * 偏移, 点击/绘制时才叠加各层 scroll), 因此溢出容器的可视窗口 =
 * [容器.by + scroll, +bh], 目标盒滚进窗口即可, 钳制口径与
 * hn_node_scroll_by 一致。
 *
 * 两种口径(与 hn-stream 贴底同一套 live_frame 分法):
 *   瞬跳 doc_scroll_to_now —— 无帧循环的管线(离屏 shot/dump、不跑
 *     anim_tick 的宿主)无从推进动画, 直接落位, 单帧出图仍是终态;
 *   平滑 doc_scroll_to_anim —— 实时帧窗口武装动画(记下各层容器的最终
 *     偏移), hn_context_anim_tick 逐帧指数逼近(τ = 80ms, 与 stream
 *     平滑追上同款), 用户滚动立即中断。 */

/* 瞬跳口径: 沿祖先链把目标滚进每一层溢出容器的可视区(嵌套容器逐层
   校正; 单层容器即最常见情形, 坐标精确)。 */
static int doc_scroll_to_now(hn_doc *doc, const char *id) {
    if (!doc || !doc->root || !id || !*id) return 0;
    hn_node *t = find_by_id_rec(doc->root, id);
    if (!t) return 0;
    for (hn_node *a = t->parent; a; a = a->parent) {
        if (a->kind != HN_ELEM || !a->style.overflow) continue;
        float top = a->by + a->scroll_y;
        if (t->by < top - 0.5f)
            hn_node_scroll_by(a, 0, t->by - top);            /* 上方不可见 → 顶对齐 */
        else if (t->by + t->bh > top + a->bh + 0.5f)
            hn_node_scroll_by(a, 0, (t->by + t->bh) - (top + a->bh)); /* 底对齐 */
    }
    return 1;
}

/* 平滑口径(实时帧): 不动偏移, 只武装 —— 每层涉及的溢出容器记下"把锚
   滚进可视区"的最终偏移(顶对齐/底对齐终态, 与瞬跳同一坐标推导, 只是
   推导自当前 scroll_y 的等价闭式), scroll_anim_walk 逐帧逼近。已在可视
   区的容器不武装(不产生多余动画)。返回 1 = 目标存在(与瞬跳同语义)。 */
static int doc_scroll_to_anim(hn_context *c, const char *id) {
    hn_doc *doc = c->doc;
    if (!doc || !doc->root || !id || !*id) return 0;
    hn_node *t = find_by_id_rec(doc->root, id);
    if (!t) return 0;
    for (hn_node *a = t->parent; a; a = a->parent) {
        if (a->kind != HN_ELEM || !a->style.overflow) continue;
        float max_y = a->content_h - a->bh;
        if (max_y < 0) max_y = 0;
        float top = a->by + a->scroll_y;
        float target = a->scroll_y;
        if (t->by < top - 0.5f)
            target = t->by - a->by;                          /* 顶对齐终态 */
        else if (t->by + t->bh > top + a->bh + 0.5f)
            target = (t->by + t->bh) - a->by - a->bh;        /* 底对齐终态 */
        if (target < 0) target = 0;
        if (target > max_y) target = max_y;
        float gap = target - a->scroll_y;
        if (gap > -1.0f && gap < 1.0f) continue;             /* 已在可视区 */
        a->scroll_target = target;
        a->scroll_anim = 1;
    }
    return 1;
}

/* #锚导航的滚动入口(activate_link 落点): 有帧循环的窗口(live_frame,
   真窗/有 anim_tick 的宿主)武装平滑滚动; 无帧循环的管线直接落位。
   返回 1 = 目标存在。 */
static int hn_context_scroll_to(hn_context *c, const char *id) {
    if (!c) return 0;
    if (!c->live_frame) return doc_scroll_to_now(c->doc, id);
    return doc_scroll_to_anim(c, id);
}

int hn_doc_scroll_to(hn_doc *doc, const char *id) {
    return doc_scroll_to_now(doc, id);   /* 公开 API 保持瞬跳语义(离屏安全) */
}

void hn_doc_set_pending_link(hn_doc *doc, const char *href) {
    if (!doc || !href) return;
    free(doc->pending_link);
    size_t n = strlen(href);
    doc->pending_link = (char *)malloc(n + 1);
    if (doc->pending_link) memcpy(doc->pending_link, href, n + 1);
}

int hn_doc_take_pending_link(hn_doc *doc, char *buf, size_t cap) {
    if (!doc || !buf || cap == 0) return 0;
    if (!doc->pending_link) { buf[0] = 0; return 0; }
    size_t n = strlen(doc->pending_link);
    size_t take = n < cap - 1 ? n : cap - 1;   /* cap 不足安全截断 */
    memcpy(buf, doc->pending_link, take);
    buf[take] = 0;
    free(doc->pending_link);                   /* 一次消费即清空 */
    doc->pending_link = NULL;
    return 1;
}

/* 点击激活: 命中测试 + 向上找最近 a[href](冒泡语义, 与命中找 id 同款)。
 * 返回 HN_LINK_*, 宿主按返回值决定 repaint / 打开外链。 */
int hn_context_activate_link(hn_context *c, float x, float y) {
    if (!c || !c->doc) return HN_LINK_NONE;
    hn_node *n = hn_context_hit_node(c, x, y);
    if (!n) return HN_LINK_NONE;
    hn_node *a = hn_node_ancestor_with_attr(n, "href");
    const char *href = a ? hn_node_attr(a, "href") : NULL;
    if (!href || !*href) return HN_LINK_NONE;
    if (href[0] == '#')                          /* 页内锚 → 引擎内滚动
                                                    (实时帧=平滑, 离屏=瞬跳) */
        return hn_context_scroll_to(c, href + 1) ? HN_LINK_ANCHOR : HN_LINK_NONE;
    hn_doc_set_pending_link(c->doc, href);       /* 外链 → 宿主消费 */
    return HN_LINK_EXTERNAL;
}

const char *hn_doc_find_trigger_on_load(hn_doc *doc) {
    const char *id = NULL;
    hn_doc_load_at(doc, 0, &id);
    return id;
}

/* 枚举 hx-trigger 含 "load" 的元素(页面启动自动请求用; 全量, 非仅首个) */
int hn_doc_load_at(hn_doc *doc, int idx, const char **out_id) {
    if (out_id) *out_id = NULL;
    if (!doc || !doc->root) return 0;
    nstack st;
    ns_init(&st, 256);
    if (!st.v) return 0;
    ns_push(&st, doc->root);
    hn_node *n;
    int found = 0;
    while (ns_pop(&st, &n)) {
        if (n->kind != HN_ELEM) continue;
        const char *trig = hn_node_attr(n, "hx-trigger");
        if (trig && strstr(trig, "load")) {
            /* token 级匹配, 排除 "autoload" 之类的误命中 */
            const char *p = trig;
            int hit = 0;
            while (*p && !hit) {
                while (*p == ' ' || *p == ',') p++;
                const char *st = p;
                while (*p && *p != ' ' && *p != ',') p++;
                if ((size_t)(p - st) == 4 && !strncmp(st, "load", 4)) hit = 1;
            }
            if (hit) {
                if (found == idx) {
                    const char *id = hn_node_attr(n, "id");
                    if (id && out_id) *out_id = id;
                    ns_free(&st);
                    return 1;
                }
                found++;
            }
        }
        /* 子节点逆序入栈保持文档序 */
        for (hn_node *ch = n->last; ch; ch = ch->prev) ns_push(&st, ch);
    }
    ns_free(&st);
    return 0;
}

/* ---------------- 应用清单 ---------------- */

void hn_doc_manifest(const hn_doc *doc, hn_manifest *out) {
    out->surface = HN_SURFACE_WINDOW;
    out->w = out->h = 0;
    out->x = out->y = 0;
    out->title = NULL;
    out->theme = NULL;
    out->transparent = 0;
    out->shadow = 1;        /* 默认带投影 */
    out->draggable = 1;     /* 默认可拖动 */
    out->reveal = HN_REVEAL_PAINT;   /* 缺省画刷铺色 */
    out->lifecycle = NULL;
    out->presence = HN_PRESENCE_AUTO;
    out->applet = NULL;
    /* 桌宠形态缺省: 普通层级/有边框/不穿透/不跨空间(与既有行为一致) */
    out->level = HN_LEVEL_NORMAL;
    out->frameless = 1;     /* 默认无标题栏(hn 语义: 窗口即画布, 标题栏是
                               例外 —— hn-frameless="0" 显式要系统标题栏) */
    out->click_through = 0;
    out->spaces_all = 0;
    if (!doc || !doc->root) return;

    hn_node *stack[256];
    int sp = 0;
    stack[sp++] = doc->root;
    while (sp > 0) {
        hn_node *n = stack[--sp];
        if (n->kind == HN_ELEM && n->tag && !strcmp(n->tag, "meta")) {
            const char *name = hn_node_attr(n, "name");
            const char *content = hn_node_attr(n, "content");
            if (name && content) {
                if (!strcmp(name, "hn-theme")) {
                    out->theme = content;
                } else if (!strcmp(name, "hn-transparent")) {
                    out->transparent = strcmp(content, "false") && strcmp(content, "0");
                } else if (!strcmp(name, "hn-shadow")) {
                    out->shadow = strcmp(content, "false") && strcmp(content, "0");
                } else if (!strcmp(name, "hn-draggable")) {
                    out->draggable = strcmp(content, "false") && strcmp(content, "0");
                } else if (!strcmp(name, "hn-reveal")) {
                    /* 呈现动画风格: paint = 画刷铺色(缺省), fade = 平台淡入,
                       none = 硬出现。未知值回落缺省。 */
                    if (!strcmp(content, "fade")) out->reveal = HN_REVEAL_FADE;
                    else if (!strcmp(content, "none")) out->reveal = HN_REVEAL_NONE;
                    else out->reveal = HN_REVEAL_PAINT;
                } else if (!strcmp(name, "hn-surface")) {
                    if (!strncmp(content, "popup", 5)) out->surface = HN_SURFACE_POPUP;
                    else if (!strncmp(content, "layer", 5)) out->surface = HN_SURFACE_LAYER;
                    else out->surface = HN_SURFACE_WINDOW;
                } else if (!strcmp(name, "hn-window")) {
                    float w = 0, h = 0, x = 0, y = 0;
                    if (sscanf(content, "%fx%f@%f,%f", &w, &h, &x, &y) >= 2) {
                        out->w = w; out->h = h; out->x = x; out->y = y;
                    } else {
                        int nw = 0, nh = 0;
                        if (sscanf(content, "%dx%d", &nw, &nh) == 2) {
                            out->w = (float)nw; out->h = (float)nh;
                        } else if (sscanf(content, "%fx%f", &w, &h) == 2) {
                            out->w = w; out->h = h;
                        }
                    }
                } else if (!strcmp(name, "hn-title")) {
                    out->title = content;
                } else if (!strcmp(name, "hn-applet")) {
                    /* 槽位名: 声明后成为随用随消、半固化的桌面轻应用 */
                    if (content[0]) out->applet = content;
                } else if (!strcmp(name, "hn-presence")) {
                    /* ghost = 完全不占系统身份; app = 明确要 Dock 身份 */
                    if (!strcmp(content, "ghost") || !strcmp(content, "none"))
                        out->presence = HN_PRESENCE_GHOST;
                    else if (!strcmp(content, "app"))
                        out->presence = HN_PRESENCE_APP;
                    else
                        out->presence = HN_PRESENCE_AUTO;
                } else if (!strcmp(name, "hn-lifecycle")) {
                    /* 关心哪些生命周期事件: "launch show hide destroy"(空格分隔) */
                    out->lifecycle = content;
                } else if (!strcmp(name, "hn-level")) {
                    /* 窗口层级: floating = 悬浮于普通窗口之上;
                       desktop = 桌面级(壁纸之上、普通窗口之下);
                       normal 或未知值 = 普通层级 */
                    if (!strcmp(content, "floating")) out->level = HN_LEVEL_FLOATING;
                    else if (!strcmp(content, "desktop")) out->level = HN_LEVEL_DESKTOP;
                    else out->level = HN_LEVEL_NORMAL;
                } else if (!strcmp(name, "hn-frameless")) {
                    out->frameless = strcmp(content, "false") && strcmp(content, "0");
                } else if (!strcmp(name, "hn-click-through")) {
                    out->click_through = strcmp(content, "false") && strcmp(content, "0");
                } else if (!strcmp(name, "hn-spaces")) {
                    /* "all" = 跟随所有空间(含全屏空间旁边); 其余值不跨空间 */
                    out->spaces_all = !strcmp(content, "all");
                }
            }
        }
        hn_node *kids[256];
        int kn = 0;
        for (hn_node *ch = n->first; ch && kn < 256; ch = ch->next) kids[kn++] = ch;
        for (int i = kn - 1; i >= 0 && sp < 256; i--) stack[sp++] = kids[i];
    }
}

/* 解析 hx-trigger 中的 "every N[ms|s]" → 毫秒; 无则 0 */
static int parse_every(const char *trig) {
    if (!trig) return 0;
    /* 在整串里找 "every", 其后的数值 token 即间隔(可带 ms/s 后缀) */
    const char *hit = NULL;
    for (const char *p = trig; *p; p++) {
        if (!strncmp(p, "every", 5) &&
            (p == trig || p[-1] == ' ' || p[-1] == ',') &&
            (p[5] == 0 || p[5] == ' ' || p[5] == '\t')) { hit = p + 5; break; }
    }
    if (!hit) return 0;
    while (*hit == ' ' || *hit == '\t') hit++;
    if (!*hit) return 0;
    const char *st = hit;
    while (*hit && *hit != ' ' && *hit != ',' && *hit != '\t') hit++;
    char buf[24];
    size_t bl = (size_t)(hit - st);
    if (bl == 0 || bl >= sizeof(buf)) return 0;
    memcpy(buf, st, bl);
    buf[bl] = 0;
    double v = strtod(buf, NULL);
    if (v <= 0) return 0;
    int ms = strstr(buf, "ms") ? (int)v : (int)(v * 1000.0);
    if (ms < 50) ms = 50;             /* 下限: 避免忙轮询 */
    if (ms > 3600000) ms = 3600000;   /* 上限: 1 小时 */
    return ms;
}

int hn_doc_poll_at(hn_doc *doc, int idx, const char **out_id, int *out_ms) {
    if (!doc || !doc->root || idx < 0) return 0;
    nstack st;
    ns_init(&st, 512);
    if (!st.v) return 0;
    ns_push(&st, doc->root);
    hn_node *n;
    int seen = 0;
    while (ns_pop(&st, &n)) {
        if (n->kind == HN_ELEM) {
            const char *trig = hn_node_attr(n, "hx-trigger");
            const char *id = hn_node_attr(n, "id");
            int ms = parse_every(trig);
            if (ms > 0 && id) {
                if (seen == idx) {
                    if (out_id) *out_id = id;
                    if (out_ms) *out_ms = ms;
                    ns_free(&st);
                    return 1;
                }
                seen++;
            }
        }
        for (hn_node *ch = n->last; ch; ch = ch->prev) ns_push(&st, ch);
    }
    ns_free(&st);
    return 0;
}

/* ---------------- 热更新 ---------------- */

int hn_doc_set_text(hn_doc *doc, const char *element_id, const char *utf8_text) {
    if (!doc || !doc->root) return 0;
    hn_node *n = find_by_id_rec(doc->root, element_id);
    if (!n) return 0;
    n->first = n->last = NULL;
    hn_node *t = hn_arena_alloc(doc->arena, sizeof(hn_node));
    memset(t, 0, sizeof(hn_node));
    t->kind = HN_TEXT;
    t->parent = n;
    t->text = hn_arena_strndup(doc->arena, utf8_text ? utf8_text : "", utf8_text ? strlen(utf8_text) : 0);
    t->text_len = t->text ? strlen(t->text) : 0;
    n->first = n->last = t;
    return 1;
}

/* 片段子节点从临时 root 摘下, 链入目标 */
static void splice_children(hn_doc *doc, hn_node *frag, hn_node *target, hn_swap_mode mode) {
    (void)doc;
    hn_node *ff = frag->first, *fl = frag->last;
    if (!ff) {
        if (mode == HN_SWAP_INNER) target->first = target->last = NULL;
        return;
    }
    /* 切断片段首尾的外向指针: 片段是被挂接的独立链, 若尾节点的 next 还指着
       别的节点, 挂到目标链上会形成环 → 遍历(paint_walk)无限递归栈溢出。 */
    ff->prev = NULL;
    fl->next = NULL;
    switch (mode) {
    case HN_SWAP_INNER:
        target->first = target->last = NULL;
        for (hn_node *ch = ff; ch; ch = ch->next) ch->parent = target;
        target->first = ff;
        target->last = fl;
        break;
    case HN_SWAP_APPEND: {
        for (hn_node *ch = ff; ch; ch = ch->next) ch->parent = target;
        if (target->last) {
            target->last->next = ff;
            ff->prev = target->last;
            target->last = fl;
        } else {
            target->first = ff;
            target->last = fl;
        }
        break;
    }
    case HN_SWAP_PREPEND: {
        for (hn_node *ch = ff; ch; ch = ch->next) ch->parent = target;
        if (target->first) {
            target->first->prev = fl;
            fl->next = target->first;
            target->first = ff;
        } else {
            target->first = ff;
            target->last = fl;
        }
        break;
    }
    case HN_SWAP_AFTEREND: {
        /* 插到 target 之后(target 的父级内; afterend 语义) */
        hn_node *p = target->parent;
        if (!p) break;
        hn_node *after = target->next;
        for (hn_node *ch = ff; ch; ch = ch->next) ch->parent = p;
        if (after) after->prev = fl; else p->last = fl;
        target->next = ff;
        ff->prev = target;
        fl->next = after;
        if (after) after->prev = fl;
        break;
    }
    case HN_SWAP_OUTER: {
        hn_node *p = target->parent;
        if (!p) break; /* 根不可替换 */
        hn_node *before = target->prev, *after = target->next;
        if (before) before->next = after; else p->first = after;
        if (after) after->prev = before; else p->last = before;
        for (hn_node *ch = ff; ch; ch = ch->next) ch->parent = p;
        if (before) { before->next = ff; ff->prev = before; }
        else p->first = ff;
        if (after) { after->prev = fl; fl->next = after; }
        else p->last = fl;
        break;
    }
    }
}

int hn_doc_swap_node(hn_doc *doc, hn_node *target, hn_swap_mode mode,
                     const char *html_src, size_t len) {
    if (!doc || !doc->root || !target) return 0;
    if (mode == HN_SWAP_NONE) return 1;                 /* 事件已派发, 不换入 */
    if (mode == HN_SWAP_DELETE) {
        if (target->parent) {
            hn_node_remove_child(target->parent, target);
            return 1;
        }
        return 0;
    }
    hn_node *frag = hn_arena_alloc(doc->arena, sizeof(hn_node));
    if (!frag) return 0;
    memset(frag, 0, sizeof(*frag));
    frag->kind = HN_ELEM;
    frag->tag = "fragment";
    if (html_src && len) hn_parse_into(doc, frag, html_src, len);
    splice_children(doc, frag, target, mode);
    hn_md_apply(doc);   /* 片段里带入的 hn-md 同样先变成普通 DOM */
    hn_fmt_apply(doc);  /* 片段里带入的 hn-json/hn-csv 同批转换 */
    return 1;
}

int hn_doc_swap(hn_doc *doc, const char *target_id, hn_swap_mode mode,
                const char *html_src, size_t len) {
    if (!doc || !doc->root || !target_id) return 0;
    hn_node *target = find_by_id_rec(doc->root, target_id);
    if (!target) return 0;
    return hn_doc_swap_node(doc, target, mode, html_src, len);
}

int hn_node_matches_selector(hn_doc *doc, hn_node *n, const char *sel, hn_context *c) {
    if (!doc || !n || !sel) return 0;
    hn_selgroup *g = hn_css_parse_selgroup(sel, strlen(sel));
    if (!g) return 0;
    int hit = (n->kind == HN_ELEM && !n->is_doc_wrapper && hn_style_selgroup_match(g, n, c));
    hn_css_selgroup_free(g);
    return hit;
}

/* 热渲染: 用新 HTML 整体替换文档内容(保留样式表) — agent 直接推送 UI 的原语 */
void hn_context_render(hn_context *c, const char *html_src, size_t len) {
    if (!c->doc) return;
    hn_node *root = c->doc->root;
    root->first = root->last = NULL;
    if (html_src && len) {
        hn_node *frag = hn_arena_alloc(c->doc->arena, sizeof(hn_node));
        memset(frag, 0, sizeof(hn_node));
        frag->kind = HN_ELEM;
        frag->tag = "fragment";
        hn_parse_into(c->doc, frag, html_src, len);
        for (hn_node *ch = frag->first; ch; ch = ch->next) ch->parent = root;
        root->first = frag->first;
        root->last = frag->last;
    }
    /* 热更新可能改写了 hn-theme: 重新装载主题基座 */
    hn_md_apply(c->doc);   /* 新内容里的 hn-md 同样先变成普通 DOM */
    hn_fmt_apply(c->doc);  /* 新内容里的 hn-json/hn-csv 同批转换 */
    hn_context_apply_theme(c);
}

/* arena 压缩: 全量重渲染时回收旧 DOM 内存。
 * 调用方须持有完整的 HTML(将重新解析)。用于流式应用的长期内存控制。 */
void hn_context_compact(hn_context *c, const char *html, size_t len) {
    if (!c || !c->doc) return;
    hn_context_clear_exit_ghosts(c);     /* 旧文档节点指针已失效 */
    hn_context_lottie_clear(c);          /* 旧文档的路径键已随 arena 释放 */
    hn_media_sessions_clear(c);          /* 旧文档的节点指针已失效(host.close 全部) */
    hn_canvas_dispose(c);
    hn_paint_frag_clear(c);                /* 旧文档的 canvas 元素已销毁 = 账本清空 */
    hn_arena_destroy(c->doc->arena);
    hn_doc_free(c->doc);
    c->doc = hn_parse_html(html, len);
    if (c->doc) {
        hn_context_set_doc(c, c->doc);
        hn_context_apply_theme(c);
    }
}

/* hn_cubism.c — Cubism(moc3) 真引擎运行时绑定
 * ============================================================
 * moc3 = Live2D Cubism 专有二进制。解析与形变计算走**官方 Core**
 * (libLive2DCubismCore, 纯 C API 的闭源运行库 —— 与 QuickJS/FreeType/
 * wasm3 同类的第三方件; 二进制不入 git, tools/fetch-cubism-core.sh
 * 自取, 授权口径见 README "Live2D (Cubism moc3) 授权说明")。
 *
 * Neb 侧职责(Core 之外的一切):
 *   - model3.json 解析(Moc/Textures 路径) — hn_json
 *   - 动作: 复用既有 motion3 求值器(hn_lottie.c) —— 逐参数采样,
 *     未驱动的参数回落 moc 缺省值(睁眼/静止的正确姿态)
 *   - 渲染: drawable → 屏幕坐标三角汤(索引展开) → HN_CMD_TRIS
 *     (贴图/atlas、逐 drawable 透明度、Additive/Multiply 混合映射)
 *
 * 形变流水线每帧: 参数落值 → csmUpdateModel → 顶点/透明度/渲染序
 * 全部从 Core 读回 —— 不重实现 Cubism 的形变数学(keyform 插值/
 * deformer 树/部件透明度传播都是 Core 的十年沉淀)。
 *
 * 遮罩(mask): mask drawable 三角汤 → CLIP_PATH_PUSH 多子路径裁剪 →
 * 画本 drawable → CLIP_POP; 反转 mask(csmIsInvertedMask) 暂按普通。
 * 已知差距: 部件拼装观感仍有散乱(疑似逐 drawable Multiply/Screen
 * 颜色与离屏掩码口径), 精修方法: Playwright + 官方 Web runtime
 * (live2dcubismcore.min.js + pixi-live2d-display)渲染同模型出对照图,
 * 逐层比对定位差异层。moc 一致性已验(consistency=1, mocVer=3)。
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "hn_internal.h"
#include "hn_json.h"

#ifdef HN_HAVE_CUBISM
#include "Live2DCubismCore.h"

struct hn_cubism;
void hn_cubism_release(struct hn_cubism *cb);
void hn_cubism_free(struct hn_cubism *cb);

#define CB_TEX_MAX 4

struct hn_cubism {
    char *m3_path;              /* 缓存键(model3.json 路径) */
    unsigned char *moc_buf;     /* 原始 moc3(64B 对齐; Core 就地复活) */
    unsigned int  moc_size;
    csmMoc       *moc;
    unsigned char *model_buf;   /* 模型实例(16B 对齐) */
    csmModel      *model;

    int n_params;
    float *pmin, *pmax, *pdef;  /* 参数域(Core 指针只读, 拷贝便于钳制) */

    int   n_draw;
    int  *order;                /* 渲染序→drawable 下标(每次 paint 重排) */

    char *tex[CB_TEX_MAX];      /* 贴图路径(model3.json FileReferences) */
    int   n_tex;

    float canvas_w, canvas_h;   /* csmReadCanvasInfo(像素) */
    float origin_x, origin_y;   /* 模型原点在画布上的像素位(非必中心) */
    float ppu;                  /* PixelsPerUnit */
};

/* ---- 进程级 4 槽 LRU(同 hn_live2d_acquire 口径): 实时窗每帧 paint
   不复解; 模型实例跨帧持有参数状态。 ---- */
#define CB_CACHE 4
static struct { char *path; struct hn_cubism *cb; unsigned long last; } g_cb_cache[CB_CACHE];
static unsigned long g_cb_clock = 0;

static char *cb_strdup(const char *s) {
    size_t n = strlen(s);
    char *p = (char *)malloc(n + 1);
    if (p) memcpy(p, s, n + 1);
    return p;
}

/* 目录拼接(与 hn_lottie 的 l2_join 同语义): dir 含尾 '/' 或为空 */
static char *cb_join(const char *dir, const char *rel) {
    if (!dir) dir = "";
    size_t dn = strlen(dir), rn = strlen(rel);
    while (dn > 0 && dir[dn - 1] == '/') dn--;   /* 去尾斜杠再补一个 */
    char *p = (char *)malloc(dn + 1 + rn + 1);
    if (!p) return NULL;
    memcpy(p, dir, dn);
    p[dn] = '/';
    memcpy(p + dn + 1, rel, rn + 1);
    return p;
}

static const char *cb_dir_of(const char *path) {
    static char dir[1024];
    snprintf(dir, sizeof(dir), "%s", path);
    char *slash = strrchr(dir, '/');
    if (slash) slash[1] = 0; else dir[0] = 0;
    return dir;
}

/* model3.json FileReferences: Moc + Textures[](其余字段 v1 不消费) */
static int cb_parse_model3(struct hn_cubism *cb, hn_context *c,
                           char *buf, size_t len, char **moc_rel) {
    void *arena = NULL;
    hn_json *root = hn_json_parse(buf, len, &arena);
    if (!root) return 0;
    hn_json *fr = hn_json_obj_get(root, "FileReferences");
    hn_json *moc = fr ? hn_json_obj_get(fr, "Moc") : NULL;
    const char *mp = hn_json_str(moc, NULL);
    if (!mp) { hn_json_free(arena); return 0; }
    *moc_rel = cb_strdup(mp);

    hn_json *texs = fr ? hn_json_obj_get(fr, "Textures") : NULL;
    if (texs && texs->kind == HN_JSON_ARR) {
        const char *dir = cb_dir_of(cb->m3_path);
        for (int i = 0; i < texs->count && i < CB_TEX_MAX; i++) {
            const char *tp = hn_json_str(hn_json_at(texs, i), NULL);
            if (tp) {
                char *abs = cb_join(dir, tp);   /* model3 目录 + 包内相对路径 */
                if (abs) cb->tex[cb->n_tex++] = abs;
            }
        }
    }
    hn_json_free(arena);
    return *moc_rel != NULL;
    (void)c;
}

struct hn_cubism *hn_cubism_acquire(hn_context *c, const char *m3_path) {
    if (!m3_path || !*m3_path) return NULL;
    g_cb_clock++;
    for (int i = 0; i < CB_CACHE; i++)
        if (g_cb_cache[i].path && !strcmp(g_cb_cache[i].path, m3_path)) {
            g_cb_cache[i].last = g_cb_clock;
            return g_cb_cache[i].cb;
        }
    if (!c) return NULL;
    const hn_asset_backend *ab = hn_context_assets(c);
    if (!ab || !ab->load) return NULL;

    struct hn_cubism *cb = (struct hn_cubism *)calloc(1, sizeof(*cb));
    if (!cb) return NULL;
    cb->m3_path = cb_strdup(m3_path);

    /* model3.json → Moc/Textures 相对路径 */
    size_t jlen = 0;
    char *jbuf = NULL;
    {
        const char *jb = ab->load(ab->ctx, m3_path, &jlen);
        if (!jb || jlen == 0) { hn_cubism_release(cb); return NULL; }
        jbuf = (char *)malloc(jlen + 1);
        if (!jbuf) { hn_cubism_release(cb); return NULL; }
        memcpy(jbuf, jb, jlen);
        jbuf[jlen] = 0;
    }
    char *moc_rel = NULL;
    int ok = cb_parse_model3(cb, c, jbuf, jlen, &moc_rel);
    free(jbuf);
    if (!ok) { hn_cubism_release(cb); return NULL; }

    /* moc3 载入(64B 对齐; Core 就地复活) */
    const char *dir = cb_dir_of(m3_path);
    char *moc_rel_path = cb_join(dir, moc_rel);
    free(moc_rel);
    if (!moc_rel_path) { hn_cubism_release(cb); return NULL; }
    size_t mlen = 0;
    const char *moc_bytes = ab->load(ab->ctx, moc_rel_path, &mlen);
    if (!moc_bytes || mlen == 0) { free(moc_rel_path); hn_cubism_release(cb); return NULL; }
    cb->moc_size = (unsigned int)mlen;
    cb->moc_buf = (unsigned char *)malloc((size_t)cb->moc_size + 64);
    if (!cb->moc_buf) { free(moc_rel_path); hn_cubism_release(cb); return NULL; }
    {
        unsigned long long a = ((unsigned long long)(size_t)cb->moc_buf + 63) & ~63ull;
        memcpy((void *)a, moc_bytes, mlen);
            int cons = csmHasMocConsistency((void *)a, cb->moc_size);
        if (getenv("HN_CUBISM_TRACE"))
            fprintf(stderr, "[cubism] mocVer=%d latest=%d consistency=%d\\n",
                    csmGetMocVersion((void *)a, cb->moc_size),
                    csmGetLatestMocVersion(), cons);
        cb->moc = csmReviveMocInPlace((void *)a, cb->moc_size);
    }
    if (!cb->moc) { free(moc_rel_path); hn_cubism_release(cb); return NULL; }

    unsigned int msz = csmGetSizeofModel(cb->moc);
    cb->model_buf = (unsigned char *)malloc((size_t)msz + 16);
    if (!cb->model_buf) { free(moc_rel_path); hn_cubism_release(cb); return NULL; }
    {
        unsigned long long a = ((unsigned long long)(size_t)cb->model_buf + 15) & ~15ull;
        cb->model = csmInitializeModelInPlace(cb->moc, (void *)a, msz);
    }
    if (!cb->model) { free(moc_rel_path); hn_cubism_release(cb); return NULL; }

    /* 参数域快照(缺省姿态 = 正确静止: 睁眼/闭嘴/立正) */
    cb->n_params = csmGetParameterCount(cb->model);
    const float *mn = csmGetParameterMinimumValues(cb->model);
    const float *mx = csmGetParameterMaximumValues(cb->model);
    const float *df = csmGetParameterDefaultValues(cb->model);
    cb->pmin = (float *)malloc(sizeof(float) * (size_t)(cb->n_params > 0 ? cb->n_params : 1));
    cb->pmax = (float *)malloc(sizeof(float) * (size_t)(cb->n_params > 0 ? cb->n_params : 1));
    cb->pdef = (float *)malloc(sizeof(float) * (size_t)(cb->n_params > 0 ? cb->n_params : 1));
    if (!cb->pmin || !cb->pmax || !cb->pdef) { free(moc_rel_path); hn_cubism_release(cb); return NULL; }
    for (int i = 0; i < cb->n_params; i++) {
        cb->pmin[i] = mn[i]; cb->pmax[i] = mx[i]; cb->pdef[i] = df[i];
    }

    cb->n_draw = csmGetDrawableCount(cb->model);
    cb->order = (int *)malloc(sizeof(int) * (size_t)(cb->n_draw > 0 ? cb->n_draw : 1));
    if (!cb->order) { free(moc_rel_path); hn_cubism_release(cb); return NULL; }

    csmVector2 csz, corig;
    float ppu = 1.0f;
    csmReadCanvasInfo(cb->model, &csz, &corig, &ppu);
    cb->canvas_w = csz.X; cb->canvas_h = csz.Y;
    cb->origin_x = corig.X; cb->origin_y = corig.Y;
    cb->ppu = ppu > 0.01f ? ppu : 1.0f;

    free(moc_rel_path);

    /* 入缓存(LRU 逐出) */
    int slot = -1;
    for (int i = 0; i < CB_CACHE; i++) if (!g_cb_cache[i].path) { slot = i; break; }
    if (slot < 0) {
        slot = 0;
        for (int i = 1; i < CB_CACHE; i++)
            if (g_cb_cache[i].last < g_cb_cache[slot].last) slot = i;
        hn_cubism_free(g_cb_cache[slot].cb);
        g_cb_cache[slot].path = NULL;
        g_cb_cache[slot].cb = NULL;
    }
    g_cb_cache[slot].path = cb_strdup(m3_path);
    g_cb_cache[slot].cb = cb;
    g_cb_cache[slot].last = g_cb_clock;
    return cb;
}

void hn_cubism_release(struct hn_cubism *cb) { hn_cubism_free(cb); }

void hn_cubism_free(struct hn_cubism *cb) {
    if (!cb) return;
    for (int i = 0; i < CB_CACHE; i++)
        if (g_cb_cache[i].cb == cb) { g_cb_cache[i].cb = NULL; g_cb_cache[i].path = NULL; }
    free(cb->m3_path);
    free(cb->moc_buf);
    free(cb->model_buf);
    free(cb->pmin); free(cb->pmax); free(cb->pdef);
    free(cb->order);
    for (int i = 0; i < cb->n_tex; i++) free(cb->tex[i]);
    free(cb);
}

/* 每帧形变: 缺省姿态打底 → motion3 逐参数覆盖(钳到域) → csmUpdateModel */
void hn_cubism_tick(struct hn_cubism *cb, struct hn_live2d *l2, int motion_idx, float t_sec) {
    if (!cb || !cb->model) return;
    float *vals = csmGetParameterValues(cb->model);
    const char **ids = csmGetParameterIds(cb->model);
    for (int i = 0; i < cb->n_params; i++) vals[i] = cb->pdef[i];
    if (l2 && motion_idx >= 0) {
        for (int i = 0; i < cb->n_params; i++) {
            float v = 0.0f;
            if (hn_live2d_motion_sample(l2, motion_idx, ids[i], t_sec, &v)) {
                if (v < cb->pmin[i]) v = cb->pmin[i];
                else if (v > cb->pmax[i]) v = cb->pmax[i];
                vals[i] = v;
            }
        }
    }
    csmUpdateModel(cb->model);
}

int hn_cubism_drawable_count(const struct hn_cubism *cb) { return cb ? cb->n_draw : 0; }

int hn_cubism_texture_count(const struct hn_cubism *cb) { return cb ? cb->n_tex : 0; }
const char *hn_cubism_texture_path(const struct hn_cubism *cb, int ti) {
    if (!cb || ti < 0 || ti >= cb->n_tex) return NULL;
    return cb->tex[ti];
}
float hn_cubism_canvas_w(const struct hn_cubism *cb) { return cb ? cb->canvas_w : 0.0f; }
float hn_cubism_canvas_h(const struct hn_cubism *cb) { return cb ? cb->canvas_h : 0.0f; }
float hn_cubism_ppu(const struct hn_cubism *cb) { return cb ? cb->ppu : 1.0f; }
float hn_cubism_origin_x(const struct hn_cubism *cb) { return cb ? cb->origin_x : 0.0f; }
float hn_cubism_origin_y(const struct hn_cubism *cb) { return cb ? cb->origin_y : 0.0f; }

/* 第 k 个渲染序槽位的 drawable 下标(paint 侧每次重排, n 小到可插入排序) */
static const int *cb_render_orders(struct hn_cubism *cb) {
    return csmGetRenderOrders(cb->model);
}

int hn_cubism_order_at(struct hn_cubism *cb, int k) {
    if (!cb || k < 0 || k >= cb->n_draw) return -1;
    const int *ro = cb_render_orders(cb);
    for (int i = 0; i < cb->n_draw; i++) cb->order[i] = i;
    for (int i = 1; i < cb->n_draw; i++) {          /* 插入排序(稳定, n≈几十) */
        int v = cb->order[i], j = i - 1;
        while (j >= 0 && ro[cb->order[j]] > ro[v]) { cb->order[j + 1] = cb->order[j]; j--; }
        cb->order[j + 1] = v;
    }
    return cb->order[k];
}

float hn_cubism_opacity(struct hn_cubism *cb, int i) {
    if (!cb || i < 0 || i >= cb->n_draw) return 0.0f;
    return csmGetDrawableOpacities(cb->model)[i];
}

int hn_cubism_vertex_count(struct hn_cubism *cb, int i) {
    if (!cb || i < 0 || i >= cb->n_draw) return 0;
    return csmGetDrawableVertexCounts(cb->model)[i];
}

const float *hn_cubism_verts(struct hn_cubism *cb, int i) {
    if (!cb || i < 0 || i >= cb->n_draw) return NULL;
    const csmVector2 *v = csmGetDrawableVertexPositions(cb->model)[i];
    return (const float *)v;
}

const float *hn_cubism_uvs(struct hn_cubism *cb, int i) {
    if (!cb || i < 0 || i >= cb->n_draw) return NULL;
    const csmVector2 *v = csmGetDrawableVertexUvs(cb->model)[i];
    return (const float *)v;
}

int hn_cubism_index_count(struct hn_cubism *cb, int i) {
    if (!cb || i < 0 || i >= cb->n_draw) return 0;
    return csmGetDrawableIndexCounts(cb->model)[i];
}

const unsigned short *hn_cubism_indices(struct hn_cubism *cb, int i) {
    if (!cb || i < 0 || i >= cb->n_draw) return NULL;
    const unsigned short **ix = csmGetDrawableIndices(cb->model);
    return ix ? ix[i] : NULL;
}

int hn_cubism_blend(struct hn_cubism *cb, int i) {
    if (!cb || i < 0 || i >= cb->n_draw) return 0;
    csmFlags f = csmGetDrawableConstantFlags(cb->model)[i];
    if (f & csmBlendAdditive) return 1;        /* HN_BLEND_LIGHTER */
    if (f & csmBlendMultiplicative) return 2;  /* HN_BLEND_MULTIPLY */
    return 0;                                  /* HN_BLEND_SRC_OVER */
}

int hn_cubism_texture_index(struct hn_cubism *cb, int i) {
    if (!cb || i < 0 || i >= cb->n_draw) return 0;
    return csmGetDrawableTextureIndices(cb->model)[i];
}

const char *hn_cubism_version(void) { (void)0; return NULL; } /* Core 5-r.5 无 VersionString(仅 csmGetVersion); 版本字符串探针侧拼 */

#else /* !HN_HAVE_CUBISM —— 未链 Core: 全部降级(paint 落占位, 页面可见) */

struct hn_cubism;
struct hn_cubism *hn_cubism_acquire(hn_context *c, const char *m3_path) {
    (void)c; (void)m3_path; return NULL;
}
void hn_cubism_release(struct hn_cubism *cb) { (void)cb; }
void hn_cubism_free(struct hn_cubism *cb) { (void)cb; }
void hn_cubism_tick(struct hn_cubism *cb, struct hn_live2d *l2, int motion_idx, float t_sec) {
    (void)cb; (void)l2; (void)motion_idx; (void)t_sec;
}
int hn_cubism_drawable_count(const struct hn_cubism *cb) { (void)cb; return 0; }
int hn_cubism_texture_count(const struct hn_cubism *cb) { (void)cb; return 0; }
const char *hn_cubism_texture_path(const struct hn_cubism *cb, int ti) { (void)cb; (void)ti; return NULL; }
float hn_cubism_canvas_w(const struct hn_cubism *cb) { (void)cb; return 0.0f; }
float hn_cubism_canvas_h(const struct hn_cubism *cb) { (void)cb; return 0.0f; }
float hn_cubism_ppu(const struct hn_cubism *cb) { (void)cb; return 1.0f; }
int hn_cubism_order_at(struct hn_cubism *cb, int k) { (void)cb; (void)k; return -1; }
float hn_cubism_opacity(struct hn_cubism *cb, int i) { (void)cb; (void)i; return 0.0f; }
int hn_cubism_vertex_count(struct hn_cubism *cb, int i) { (void)cb; (void)i; return 0; }
const float *hn_cubism_verts(struct hn_cubism *cb, int i) { (void)cb; (void)i; return NULL; }
const float *hn_cubism_uvs(struct hn_cubism *cb, int i) { (void)cb; (void)i; return NULL; }
int hn_cubism_index_count(struct hn_cubism *cb, int i) { (void)cb; (void)i; return 0; }
const unsigned short *hn_cubism_indices(struct hn_cubism *cb, int i) { (void)cb; (void)i; return NULL; }
int hn_cubism_blend(struct hn_cubism *cb, int i) { (void)cb; (void)i; return 0; }
int hn_cubism_texture_index(struct hn_cubism *cb, int i) { (void)cb; (void)i; return 0; }
const char *hn_cubism_version(void) { return NULL; }

#endif /* HN_HAVE_CUBISM */

/** ================= paint 发射(paint walk 调用) =================
 * hn-cubism 节点: tick → 渲染序遍历 drawable → 索引展开成三角汤 →
 * HN_CMD_TRIS(贴图路径/透明度/混合)。Core 缺失 → 可见占位卡
 * (下载指引 + 许可提示), 页面不静默空白。
 * (本函数体通用: 真实现靠 HN_HAVE_CUBISM 分支, 未链接只落占位卡。) */
static void cb_placeholder(hn_context *c, float bw, float bh, float sx, float sy) {
    hn_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = HN_CMD_RECT;
    cmd.x = sx; cmd.y = sy; cmd.w = bw; cmd.h = bh;
    cmd.radius = 10;
    cmd.fill = 0xFF1C2330u;                  /* BGRA 深底 */
    cmd.stroke = 0xFFD28A5A;                 /* 琥珀描边(专有边界色) */
    cmd.stroke_w = 1;
    hn_paint_push(c, &cmd);
    const char *l1 = "Live2D (moc3) 需要官方 Cubism Core";
    const char *l2 = "运行 tools/fetch-cubism-core.sh 自取(许可见 README)";
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = HN_CMD_TEXT;
    cmd.text = l1; cmd.text_len = strlen(l1);
    cmd.tx = sx + 12; cmd.baseline = sy + bh * 0.5f - 6;
    cmd.font.size_px = 12.0f;
    cmd.fill = 0xFF7AE0C5u;
    hn_paint_push(c, &cmd);
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = HN_CMD_TEXT;
    cmd.text = l2; cmd.text_len = strlen(l2);
    cmd.tx = sx + 12; cmd.baseline = sy + bh * 0.5f + 14;
    cmd.font.size_px = 10.0f;
    cmd.fill = 0xFF9AAABBu;
    hn_paint_push(c, &cmd);
}

void hn_cubism_paint(hn_context *c, hn_node *n,
                     float bw, float bh, float sx, float sy, float alpha) {
    if (!c || !n) return;
    const char *m3 = hn_node_attr(n, "hn-cubism");
    if (!m3 || !*m3) return;
#ifndef HN_HAVE_CUBISM
    cb_placeholder(c, bw, bh, sx, sy);
    (void)alpha;
#else
    struct hn_cubism *cb = hn_cubism_acquire(c, m3);
    int trace = 0;
    { const char *e = getenv("HN_CUBISM_TRACE"); trace = e && *e && strcmp(e, "0") != 0; }
    if (trace) fprintf(stderr, "[cubism] acquire(%s)=%p\n", m3, (void *)cb);
    if (!cb) {
        cb_placeholder(c, bw, bh, sx, sy);
        return;
    }

    /* 动作驱动: hn-cubism-motion 组(缺省 Idle) + hn-cubism-index */
    struct hn_live2d *l2 = hn_live2d_acquire(c, m3);
    int midx = -1;
    if (l2) {
        const char *grp = hn_node_attr(n, "hn-cubism-motion");
        const char *ixs = hn_node_attr(n, "hn-cubism-index");
        midx = hn_live2d_motion_pick(l2, grp && *grp ? grp : "Idle",
                                     ixs && *ixs ? atoi(ixs) : 0);
    }
    hn_cubism_tick(cb, l2, midx, (float)(n->ext_clock * 0.001));

    /* 画布 → 元素盒映射: 等比缩放居中; y 翻转(Cubism y 向上) */
    float cw = hn_cubism_canvas_w(cb), chh = hn_cubism_canvas_h(cb);
    float ppu = hn_cubism_ppu(cb);
    float orgx = hn_cubism_origin_x(cb), orgy = hn_cubism_origin_y(cb);
    if (cw < 1.0f || chh < 1.0f) return;
    float s = bw / cw < bh / chh ? bw / cw : bh / chh;
    float ox = sx + (bw - cw * s) * 0.5f;
    float oy = sy + (bh - chh * s) * 0.5f;
    hn_arena *a = c->tmp;

    int nvis = 0;
    if (trace) fprintf(stderr, "[cubism] drawables=%d canvas=%.0fx%.0f ppu=%.0f box=%.0fx%.0f origin=%.0f,%.0f\n",
                       hn_cubism_drawable_count(cb), hn_cubism_canvas_w(cb),
                       hn_cubism_canvas_h(cb), hn_cubism_ppu(cb), bw, bh,
                       hn_cubism_origin_x(cb), hn_cubism_origin_y(cb));
    for (int k = 0; k < hn_cubism_drawable_count(cb); k++) {
        int i = hn_cubism_order_at(cb, k);
        if (i < 0) break;
        float op = hn_cubism_opacity(cb, i);
        if (op <= 0.01f) continue;
        csmFlags dflag = csmGetDrawableDynamicFlags(cb->model)[i];
        if (!(dflag & csmIsVisible)) continue;     /* 本帧不可见(Core 动态) */
        int nix = hn_cubism_index_count(cb, i);
        const unsigned short *ix = hn_cubism_indices(cb, i);
        const float *pv = hn_cubism_verts(cb, i);
        const float *puv = hn_cubism_uvs(cb, i);
        if (nix <= 0 || nix % 3 != 0 || !ix || !pv || !puv) continue;

        const char *tp = hn_cubism_texture_path(cb, hn_cubism_texture_index(cb, i));
        if (!tp || !*tp) continue;

        /* ---- 遮罩(mask): 被 mask 的 drawable 以 mask 形状裁剪后再画。
            mask 形状 = 各 mask drawable 的三角汤(屏幕坐标), 每三角独立
            子路径(segs=3)做非零环绕并集 —— 与官方"离屏掩码"的可见效果
            一致; 反转 mask(csmIsInvertedMask) v1 暂按普通 mask 处理。 *\/ */
        int nm = csmGetDrawableMaskCounts(cb->model)[i];
        const int *mlist = csmGetDrawableMasks(cb->model)[i];
        int pushed_clip = 0;
        if (nm > 0 && mlist) {
            int total = 0, nseg = 0;
            for (int m = 0; m < nm; m++) {
                int mi = mlist[m];
                if (mi < 0 || mi >= cb->n_draw) continue;
                total += hn_cubism_index_count(cb, mi);
                nseg += hn_cubism_index_count(cb, mi) / 3;
            }
            if (total > 0 && nseg > 0) {
                float *soup = (float *)hn_arena_alloc(a, sizeof(float) * (size_t)total * 2);
                int *segs = (int *)hn_arena_alloc(a, sizeof(int) * (size_t)nseg);
                if (soup && segs) {
                    int p = 0, sg = 0;
                    for (int m = 0; m < nm; m++) {
                        int mi = mlist[m];
                        if (mi < 0 || mi >= cb->n_draw) continue;
                        int cnt = hn_cubism_index_count(cb, mi);
                        const unsigned short *mix = hn_cubism_indices(cb, mi);
                        const float *mv = hn_cubism_verts(cb, mi);
                        if (cnt <= 0 || !mix || !mv) continue;
                        for (int j = 0; j < cnt; j++) {
                            int vi = mix[j];
                            soup[p * 2]     = ox + (orgx + mv[vi * 2] * ppu) * s;
                            soup[p * 2 + 1] = oy + (orgy - mv[vi * 2 + 1] * ppu) * s;
                            p++;
                            if (j % 3 == 2) segs[sg++] = 3;
                        }
                    }
                    if (p == total && sg == nseg) {
                        hn_cmd mc;
                        memset(&mc, 0, sizeof(mc));
                        mc.kind = HN_CMD_CLIP_PATH_PUSH;
                        mc.poly = soup; mc.poly_n = total;
                        mc.clip_segs = segs; mc.clip_nsegs = nseg;
                        hn_paint_push(c, &mc);
                        pushed_clip = 1;
                    }
                }
            }
        }
        if (trace && k == 0) {
            int vc = hn_cubism_vertex_count(cb, i);
            const float *vv = hn_cubism_verts(cb, i);
            if (vc > 0 && vv) {
                float mnx = 1e9f, mxx = -1e9f, mny = 1e9f, mxy = -1e9f;
                for (int q = 0; q < vc; q++) {
                    float ux = vv[q * 2], uy = vv[q * 2 + 1];
                    if (ux < mnx) mnx = ux; if (ux > mxx) mxx = ux;
                    if (uy < mny) mny = uy; if (uy > mxy) mxy = uy;
                }
                fprintf(stderr, "[cubism] d0 verts=%d range=[%.3f..%.3f, %.3f..%.3f]\n",
                        vc, mnx, mxx, mny, mxy);
            }
        }

        int ntri = nix / 3;
        int nsv = nix;                            /* 汤: 每三角 3 顶点 */
        float *sv = (float *)hn_arena_alloc(a, sizeof(float) * (size_t)nsv * 2);
        float *su = (float *)hn_arena_alloc(a, sizeof(float) * (size_t)nsv * 2);
        if (!sv || !su) return;
        for (int j = 0; j < nix; j++) {
            int vi = ix[j];
            float mx = pv[vi * 2] * ppu;          /* 模型单位 ×ppu = 画布像素 */
            float my = pv[vi * 2 + 1] * ppu;
            sv[j * 2]     = ox + (orgx + mx) * s; /* x: 原点 + 模型 → 画布像素 */
            sv[j * 2 + 1] = oy + (orgy - my) * s; /* y: 原点翻转(上→下) */
            su[j * 2]     = puv[vi * 2];
            su[j * 2 + 1] = puv[vi * 2 + 1];
        }

        hn_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.kind = HN_CMD_TRIS;
        cmd.mesh_verts = sv;
        cmd.mesh_uv = su;
        cmd.mesh_cols = ntri;                     /* TRIS 语义 = 三角形数 */
        cmd.mesh_rows = 1;                        /* 单面标记: 背面剔除 */
        cmd.text = tp;
        cmd.text_len = strlen(tp);
        cmd.fill = 0xFFFFFF00u | (unsigned)(op * alpha * 255.0f);
        int b = hn_cubism_blend(cb, i);
        cmd.blend = (unsigned char)(b == 1 ? HN_BLEND_LIGHTER
                                 : b == 2 ? HN_BLEND_MULTIPLY
                                 : HN_BLEND_SRC_OVER);
        cmd.x = sx; cmd.y = sy; cmd.w = bw; cmd.h = bh;   /* 后端剔除用 */
        hn_paint_push(c, &cmd);
        nvis++;
        if (pushed_clip) {
            hn_cmd pop;
            memset(&pop, 0, sizeof(pop));
            pop.kind = HN_CMD_CLIP_POP;
            hn_paint_push(c, &pop);
        }
    }
    if (trace) fprintf(stderr, "[cubism] emitted=%d\n", nvis);
    (void)nvis;
#endif /* HN_HAVE_CUBISM */
}

const char *hn_cubism_runtime_version(void) {
#ifdef HN_HAVE_CUBISM
    return hn_cubism_version();
#else
    return NULL;
#endif
}

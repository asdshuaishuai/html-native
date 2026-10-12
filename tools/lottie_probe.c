/* lottie_probe.c — dotLottie(.lottie ZIP)引擎接入探针(C 级, 离屏)
 *
 * .lottie = ZIP 容器(dotLottie 规范 v1: manifest.json 声明 animations 次序,
 * animations/<id>.json 是本体)。引擎内建最小 ZIP 读取器(hn_lottie.c):
 * EOCD 定位 → 中央目录遍历 → 选动画(manifest 优先, 回退第一个
 * animations/ 下 .json)→ 解压(存储 / 裸 deflate=复用 hn_png 的 hn_inflate)。
 *
 * 断言(夹具 = 真实 CDN 包 examples/assets/dotlottie/TVFEMLriby.lottie
 * + 内嵌手写 ZIP 字节):
 *   1. zip 加载: 真实包解析成功, 尺寸 500x690, 时长 (26-0)/24 = 1083ms;
 *   2. 帧推进: 同一文档 ext_clock 0 vs 500ms 的显示列表不同;
 *   3. fit 各档: contain(缺省, 等比 + letterbox 居中, 画面完全落在
 *      元素盒内)/ fill(独立轴拉伸铺满)/ cover(等比铺满, 允许出盒)/
 *      none(1:1, 画布中心对盒中心);
 *   4. zip 内幕: 内嵌存储法(无压缩)小包 → manifest 优先选动画; 无
 *      manifest → 回退第一个 animations/*.json; 坏包(截断)→ NULL。
 *
 * 用法(由 tools/lottie_probe.sh 编译运行; cwd = 仓库根):
 *   cc -std=c99 -O2 tools/lottie_probe.c <引擎核心源> -lm -o lottie_probe
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include "hn_internal.h"

static int g_pass = 0, g_fail = 0;

static void ck(int cond, const char *label) {
    printf("%s %s\n", cond ? "  v" : "  X", label);
    if (cond) g_pass++; else g_fail++;
}

/* ---- 文件资产后端(cwd 相对直读)+ 可注入字节(内嵌 zip 夹具) ---- */

#define INJECT_MAX 4
static struct { const char *path; const char *bytes; size_t n; } g_inject[INJECT_MAX];
static int g_inject_n;

static const char *file_asset_load(void *ctx, const char *path, size_t *len) {
    (void)ctx;
    for (int i = 0; i < g_inject_n; i++)
        if (!strcmp(g_inject[i].path, path)) {
            if (len) *len = g_inject[i].n;
            return g_inject[i].bytes;
        }
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    if (n <= 0) { fclose(f); return NULL; }
    char *d = (char *)malloc((size_t)n);
    if (!d || fread(d, 1, (size_t)n, f) != (size_t)n) { fclose(f); free(d); return NULL; }
    fclose(f);
    if (len) *len = (size_t)n;
    return d;
}

/* 后端用 static:set_assets 只存指针, 栈局部会在 new_ctx 返回后悬垂 */
static hn_asset_backend g_asset_backend;

static hn_context *new_ctx(void) {
    hn_context *c = hn_context_create();
    memset(&g_asset_backend, 0, sizeof(g_asset_backend));
    g_asset_backend.load = file_asset_load;
    hn_context_set_assets(c, &g_asset_backend);
    return c;
}

static hn_text_backend tb;

/* 渲染一个 <img hn-lottie> 文档, 返回全部 POLY 指令的整体 bbox。
   返回 0 = 无 POLY(加载失败或空白)。fit/style 通过参数注入。 */
static int render_bbox(const char *img_attrs, const char *style, float clock_ms,
                       float *bx0, float *by0, float *bx1, float *by1) {
    char html[1024];
    snprintf(html, sizeof(html),
        "<html><head><style>html,body{margin:0;background:#000}"
        "#lot{%s}</style></head><body>"
        "<img id=\"lot\" src=\"examples/assets/dotlottie/TVFEMLriby.lottie\" hn-lottie %s>"
        "</body></html>", style, img_attrs);
    hn_context *c = new_ctx();
    hn_context_set_doc(c, hn_parse_html(html, strlen(html)));
    hn_node *n = hn_doc_find_by_id(hn_context_doc(c), "lot");
    if (n) n->ext_clock = clock_ms;
    hn_context_layout(c, 800, 800, &tb);
    const hn_display_list *dl = hn_context_display_list(c);
    int found = 0;
    float mnx = 1e9f, mny = 1e9f, mxx = -1e9f, mxy = -1e9f;
    for (int i = 0; i < dl->count; i++) {
        const hn_cmd *cmd = &dl->cmds[i];
        if (cmd->kind != HN_CMD_POLYGON || !cmd->poly) continue;
        for (int q = 0; q < cmd->poly_n; q++) {
            float x = cmd->poly[q * 2], y = cmd->poly[q * 2 + 1];
            if (x < mnx) mnx = x;
            if (x > mxx) mxx = x;
            if (y < mny) mny = y;
            if (y > mxy) mxy = y;
        }
        found = 1;
    }
    if (found && bx0) {
        *bx0 = mnx; *by0 = mny; *bx1 = mxx; *by1 = mxy;
    }
    return found;
}

/* ---- 内嵌手写 ZIP(存储法, 无压缩)----
 * CRC 字段写 0: 读取器从简不校验 CRC(资产通道负责完整性), 夹具如实利用
 * 这一口径。两个条目: manifest.json(指向 b)+ animations/a.json(干扰项)。 */
static char g_zipbuf[2048];
static size_t g_zipbuf_n;

static void put16(unsigned v) {
    g_zipbuf[g_zipbuf_n++] = (char)(v & 0xff);
    g_zipbuf[g_zipbuf_n++] = (char)((v >> 8) & 0xff);
}
static void put32(unsigned v) {
    put16(v & 0xffff);
    put16((v >> 16) & 0xffff);
}

/* 生成一个最小 ZIP: entries = {名字, 内容}[]。返回总长。 */
static size_t make_zip(const char *files[][2], int n, int manifest_idx) {
    struct { unsigned lfh_off, comp, unc; unsigned char nl; } ent[8];
    g_zipbuf_n = 0;
    for (int i = 0; i < n; i++) {
        const char *name = files[i][0];
        const char *data = files[i][1];
        size_t dl = strlen(data);
        unsigned char nl = (unsigned char)strlen(name);
        ent[i].lfh_off = (unsigned)g_zipbuf_n;
        ent[i].comp = ent[i].unc = (unsigned)dl;
        ent[i].nl = nl;
        put32(0x04034b50u);          /* 本地文件头 */
        put16(20);                   /* 版本 */
        put16(0);                    /* 标志 */
        put16(0);                    /* method = 0 存储 */
        put16(0); put16(0);          /* 时间/日期 */
        put32(0);                    /* CRC(不校验) */
        put32(ent[i].comp);
        put32(ent[i].unc);
        put16(nl);
        put16(0);                    /* extra */
        memcpy(g_zipbuf + g_zipbuf_n, name, nl);
        g_zipbuf_n += nl;
        memcpy(g_zipbuf + g_zipbuf_n, data, dl);
        g_zipbuf_n += dl;
    }
    size_t cd_off = g_zipbuf_n;
    for (int i = 0; i < n; i++) {
        put32(0x02014b50u);          /* 中央目录 */
        put16(20); put16(20);        /* 制作版本 / 需要版本 */
        put16(0);                    /* 标志 */
        put16(0);                    /* method = 0 存储 */
        put16(0); put16(0);          /* 时间 / 日期 */
        put32(0);                    /* CRC */
        put32(ent[i].comp);
        put32(ent[i].unc);
        put16(ent[i].nl);
        put16(0); put16(0); put16(0); put16(0);
        put32(0);                    /* 注释/盘号 */
        put32(ent[i].lfh_off);
        memcpy(g_zipbuf + g_zipbuf_n, files[i][0], ent[i].nl);
        g_zipbuf_n += ent[i].nl;
        (void)manifest_idx;
    }
    size_t cd_size = g_zipbuf_n - cd_off;
    put32(0x06054b50u);              /* EOCD */
    put16(0); put16(0);
    put16((unsigned)n); put16((unsigned)n);
    put32((unsigned)cd_size);
    put32((unsigned)cd_off);
    put16(0);                        /* 注释长 */
    return g_zipbuf_n;
}

int main(void) {
    printf("== dotLottie(.lottie ZIP)探针 ==\n");
    const char *ZIP = "examples/assets/dotlottie/TVFEMLriby.lottie";

    printf("== [1] 真实包加载(CDN 采样 TVFEMLriby.lottie) ==\n");
    hn_context *c = new_ctx();
    struct hn_lottie *l = hn_lottie_load(c, ZIP);
    ck(l != NULL, "zip 加载成功(manifest → animations/f198971c….json)");
    float w = 0, h = 0, dur = 0;
    hn_lottie_size(l, &w, &h);
    dur = hn_lottie_duration_ms(l);
    ck(w == 500.0f && h == 690.0f, "尺寸解析 = 500x690");
    int dur_ok = dur > 1083.0f && dur < 1084.0f;
    printf("%s 时长 = %.1fms(期望 (26-0)/24 帧 = 1083.3ms)\n",
           dur_ok ? "  v" : "  X", dur);
    if (dur_ok) g_pass++; else g_fail++;

    printf("== [2] 帧推进 ==\n");
    /* 首顶点逐点对比(同坐标序列才算相同帧)。注意: 帧间多边形**数量**
       允许变化(图层淡入淡出时 alpha<=0.003 的内容合法地不发射), 所以
       只断言"两帧都非空且逐点序列不同"。 */
    static const char *HH =
        "<html><head><style>html,body{margin:0}#lot{position:absolute;left:0;top:0;"
        "width:340;height:470}</style></head><body>"
        "<img id=\"lot\" src=\"examples/assets/dotlottie/TVFEMLriby.lottie\" hn-lottie></body></html>";
    static float poly0[200000], poly1[200000];
    int n0 = 0, n1 = 0;
    for (int pass = 0; pass < 2; pass++) {
        hn_context *cp = new_ctx();
        hn_context_set_doc(cp, hn_parse_html(HH, (int)strlen(HH)));
        hn_node *nn = hn_doc_find_by_id(hn_context_doc(cp), "lot");
        if (nn) nn->ext_clock = pass == 0 ? 0.0f : 500.0f;
        hn_context_layout(cp, 800, 800, &tb);
        const hn_display_list *dl = hn_context_display_list(cp);
        float *dst = pass == 0 ? poly0 : poly1;
        int *dn = pass == 0 ? &n0 : &n1;
        for (int i = 0; i < dl->count && *dn < 100000; i++)
            if (dl->cmds[i].kind == HN_CMD_POLYGON && dl->cmds[i].poly)
                for (int q = 0; q < dl->cmds[i].poly_n && *dn < 100000; q++) {
                    dst[*dn] = dl->cmds[i].poly[q * 2];
                    dst[*dn + 1] = dl->cmds[i].poly[q * 2 + 1];
                    *dn += 2;
                }
    }
    ck(n0 > 0 && n1 > 0, "两个时钟都有内容发射(顶点非空)");
    int same = n0 == n1;
    for (int i = 0; same && i < n0; i++) same = poly0[i] == poly1[i];
    ck(!same, "t=0 与 t=500ms 的顶点序列不同(帧推进)");

    printf("== [3] fit 各档(盒 340x470 @ (30,25); 画布 500x690) ==\n");
    /* contain 变换是仿射: out = (p - o)*k + box_pos。画布内容不铺满画布
       (火焰 bbox ⊂ 画布), 所以各档都对照**同一内容在 500x690 参考盒**
       (盒=画布 → k=1 且零偏移, 输出即画布坐标)下的输出做仿射关系断言,
       而不是硬编码内容 bbox。 */
    float rx0, ry0, rx1, ry1;
    int rok = render_bbox("", "position:absolute;left:0;top:0;width:500;height:690",
                          0.0f, &rx0, &ry0, &rx1, &ry1);
    ck(rok, "参考盒(=画布尺寸)渲染成功(输出即画布坐标)");
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
    /* contain(缺省): k = min(340/500, 470/690) = 340/500, letterbox 居中 */
    float k = 340.0f / 500.0f;
    float lx = (340.0f - 500.0f * k) * 0.5f;
    float ly = (470.0f - 690.0f * k) * 0.5f;
    int ok = render_bbox("", "position:absolute;left:30;top:25;width:340;height:470",
                         0.0f, &x0, &y0, &x1, &y1);
    ck(ok &&
       fabsf(x0 - (rx0 * k + 30.0f + lx)) < 0.6f && fabsf(y0 - (ry0 * k + 25.0f + ly)) < 0.6f &&
       fabsf(x1 - (rx1 * k + 30.0f + lx)) < 0.6f && fabsf(y1 - (ry1 * k + 25.0f + ly)) < 0.6f,
       "contain(缺省): 等比缩放 + letterbox 居中(仿射关系成立)");
    ck(ok && x0 >= 30.0f - 0.6f && y0 >= 25.0f - 0.6f &&
       x1 <= 370.0f + 0.6f && y1 <= 495.0f + 0.6f,
       "contain(缺省): 画面完全落在元素盒内(letterbox)");
    /* fill: 双轴独立拉伸, 无偏移(纵横比可破坏) */
    ok = render_bbox("hn-lottie-fit=\"fill\"",
                     "position:absolute;left:30;top:25;width:340;height:470",
                     0.0f, &x0, &y0, &x1, &y1);
    ck(ok &&
       fabsf(x0 - (rx0 * (340.0f / 500.0f) + 30.0f)) < 0.6f &&
       fabsf(y0 - (ry0 * (470.0f / 690.0f) + 25.0f)) < 0.6f &&
       fabsf(x1 - (rx1 * (340.0f / 500.0f) + 30.0f)) < 0.6f &&
       fabsf(y1 - (ry1 * (470.0f / 690.0f) + 25.0f)) < 0.6f,
       "fill: 双轴独立拉伸铺满(内容仿射对应参考输出)");
    /* cover: k = max(340/500, 470/690) = 470/690, 居中(短边方向出盒) */
    float kc = 470.0f / 690.0f;
    float cvx = (340.0f - 500.0f * kc) * 0.5f;
    float cvy = (470.0f - 690.0f * kc) * 0.5f;
    ok = render_bbox("hn-lottie-fit=\"cover\"",
                     "position:absolute;left:30;top:25;width:340;height:470",
                     0.0f, &x0, &y0, &x1, &y1);
    ck(ok &&
       fabsf(x0 - (rx0 * kc + 30.0f + cvx)) < 0.6f && fabsf(y0 - (ry0 * kc + 25.0f + cvy)) < 0.6f &&
       fabsf(x1 - (rx1 * kc + 30.0f + cvx)) < 0.6f && fabsf(y1 - (ry1 * kc + 25.0f + cvy)) < 0.6f,
       "cover: 等比铺满(k=短边), 居中裁切");
    /* none: 1:1, 画布中心对盒中心 → 与参考输出差固定平移
       (盒中心 - 画布中心): dx = 200-250 = -50, dy = 260-345 = -85 */
    float nx0, ny0, nx1, ny1;
    render_bbox("hn-lottie-fit=\"none\"",
                "position:absolute;left:30;top:25;width:340;height:470",
                0.0f, &nx0, &ny0, &nx1, &ny1);
    ck(fabsf(nx0 - (rx0 - 50.0f)) < 0.6f && fabsf(ny0 - (ry0 - 85.0f)) < 0.6f &&
       fabsf(nx1 - (rx1 - 50.0f)) < 0.6f && fabsf(ny1 - (ry1 - 85.0f)) < 0.6f,
       "none: 1:1 绘制且画布中心对盒中心(与参考输出差固定平移)");

    printf("== [4] zip 内幕(内嵌存储法小包) ==\n");
    const char *files_m[3][2] = {
        { "manifest.json",
          "{\"animations\":[{\"id\":\"b\",\"mode\":\"normal\",\"direction\":1}],\"version\":\"1.0\"}" },
        { "animations/a.json",
          "{\"w\":100,\"h\":100,\"fr\":60,\"ip\":0,\"op\":60,\"layers\":[{\"ty\":4,\"ip\":0,\"op\":60}]}" },
        { "animations/b.json",
          "{\"w\":40,\"h\":30,\"fr\":60,\"ip\":0,\"op\":60,\"layers\":[{\"ty\":4,\"ip\":0,\"op\":60}]}" },
    };
    size_t zn = make_zip(files_m, 3, 0);
    g_inject[g_inject_n].path = "t/manifested.lottie";
    g_inject[g_inject_n].bytes = g_zipbuf;
    g_inject[g_inject_n].n = zn;
    g_inject_n++;
    hn_context *cm = new_ctx();
    struct hn_lottie *lm = hn_lottie_load(cm, "t/manifested.lottie");
    ck(lm != NULL, "存储法(无压缩)小包可加载");
    float lw = 0, lh = 0;
    hn_lottie_size(lm, &lw, &lh);
    ck(lw == 40.0f && lh == 30.0f,
       "manifest 优先: 选中 animations/b.json(而非字典序 a.json)");
    hn_lottie_free(lm);

    /* 无 manifest → 回退第一个 animations/ 下 .json */
    const char *files_nm[2][2] = {
        { "animations/x.json",
          "{\"w\":70,\"h\":80,\"fr\":60,\"ip\":0,\"op\":60,\"layers\":[{\"ty\":4,\"ip\":0,\"op\":60}]}" },
        { "manifest.json", "{\"animations\":[]}" },   /* 空 animations → 回退 */
    };
    zn = make_zip(files_nm, 2, 0);
    g_inject[g_inject_n].path = "t/nofallback.lottie";
    g_inject[g_inject_n].bytes = g_zipbuf;
    g_inject[g_inject_n].n = zn;
    g_inject_n++;
    struct hn_lottie *ln = hn_lottie_load(cm, "t/nofallback.lottie");
    ck(ln != NULL, "manifest 空 animations → 回退 animations/ 首个 .json");
    hn_lottie_size(ln, &lw, &lh);
    ck(lw == 70.0f && lh == 80.0f, "回退选中的是 animations/x.json");
    hn_lottie_free(ln);

    /* 坏包: 截断 EOCD */
    g_inject[g_inject_n].path = "t/truncated.lottie";
    g_inject[g_inject_n].bytes = g_zipbuf;
    g_inject[g_inject_n].n = zn - 10;
    g_inject_n++;
    struct hn_lottie *lb = hn_lottie_load(cm, "t/truncated.lottie");
    ck(lb == NULL, "截断包(EOCD 损坏)→ NULL 不崩");
    hn_lottie_free(l);
    hn_live2d_cache_clear();

    printf("\n通过 %d / 失败 %d\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}

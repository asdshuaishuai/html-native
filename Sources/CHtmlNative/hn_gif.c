/* hn_gif.c — GIF87a/89a 动图解码 + 帧序列(纯 C, 零依赖)
 *
 * 为什么需要它: <img src="x.gif"> 之前会走到 PNG 解码失败 → 蓝灰占位网格 ——
 * 动图(GIF)在 UI 资产里密度不低(loading / 表情 / 指示器 / 步骤图), 而这个
 * 引擎承诺"零外部依赖、三平台同源", 引入 libgif/giflib 会终结该承诺, 因此
 * 自己解: LZW 解压 + 全局/局部色表 + 帧间 disposal 合成, 产物是每帧 RGBA8
 * 序列 + 每帧 delay。
 *
 * 播放模型: 与 Lottie / 网格变形共用同一帧时钟(节点的 ext_clock, 由
 * hn_context_anim_tick 推进) —— 节点声明了 GIF 就被标记为活跃, 帧循环
 * 持续重绘。当前帧 = "按 delay 累计"的**纯时间函数**(无累积状态), 与引擎
 * "按 t 求值"的动画口径一致: 截图跳时钟、循环回绕、倒退取帧都成立。
 *
 * 支持: GIF87a/89a、全局/局部色表、LZW、帧间 disposal 0/1/2/3、透明色
 *   索引、NETSCAPE 循环次数、交织(interlace)、多帧子块拼接。
 * 不支持(跳过而非报错): 纯文本扩展(plain text extension)、
 *   非 8 位色表(格式本身即 8 位, 无此情况)、
 *   "背景色恢复"以外的平台差异(见下: 声明了透明色的 GIF 画布起点按全透明)。
 *
 * 帧合成口径(disposal):
 *   0/1 = 保留(下一帧直接叠上);
 *   2   = 把本帧矩形恢复为画布起点色(背景色; 有透明声明时 = 透明);
 *   3   = 恢复为本帧绘制前的快照(逐帧保存一份, 仅 disposal=3 的帧付这份内存)。
 *   画布起点色: 有任一帧声明透明色 → 全透明(浏览器口径: 透明 GIF 的
 *   未覆盖区域必须透出下层); 否则用逻辑屏背景色(规范口径)。
 *   帧延迟为 0 时按 100ms 计(浏览器对 0 延迟的实际处理; 显式 >0 一律原样)。
 */
#include <stdlib.h>
#include <string.h>
#include "hn_internal.h"
#include "hn_gif.h"

/* ---------------- 字节游标(小端) ---------------- */

typedef struct { const unsigned char *p; size_t n, i; } gcur;

static int g_u8(gcur *c) { return c->i < c->n ? c->p[c->i++] : -1; }
static int g_u16(gcur *c) {
    int a = g_u8(c);
    int b = g_u8(c);
    return (a < 0 || b < 0) ? -1 : (a | (b << 8));
}
static int g_skip(gcur *c, size_t k) {
    if (c->i + k > c->n) { c->i = c->n; return 0; }
    c->i += k;
    return 1;
}

/* 跳过一个 GIF 子块串(长度字节 + 数据, 直到长度 0) */
static void g_skip_subblocks(gcur *c) {
    for (;;) {
        int len = g_u8(c);
        if (len <= 0) return;
        if (!g_skip(c, (size_t)len)) return;
    }
}

/* 把子块串拷进连续缓冲(调用方 free)。返回 1 成功。 */
static int g_read_subblocks(gcur *c, unsigned char **out, size_t *out_n) {
    size_t cap = 0, n = 0;
    unsigned char *buf = NULL;
    for (;;) {
        int len = g_u8(c);
        if (len < 0) break;
        if (len == 0) break;
        if (n + (size_t)len > cap) {
            size_t nc = cap ? cap * 2 : 1024;
            while (nc < n + (size_t)len) nc *= 2;
            unsigned char *nb = (unsigned char *)realloc(buf, nc);
            if (!nb) { free(buf); return 0; }
            buf = nb;
            cap = nc;
        }
        for (int k = 0; k < len; k++) {
            int v = g_u8(c);
            if (v < 0) { free(buf); return 0; }
            buf[n++] = (unsigned char)v;
        }
    }
    *out = buf;
    *out_n = n;
    return 1;
}

/* ---------------- LZW(GIF 变体) ---------------- */

#define GIF_LZW_MAX 4096

/* 从位流按 LSB 优先读 code_size 位; 越界返回 -1 */
static int g_read_bits(const unsigned char *d, size_t bytes, size_t *bitpos,
                       int size) {
    if (*bitpos + (size_t)size > bytes * 8) return -1;
    int v = 0;
    for (int i = 0; i < size; i++) {
        size_t b = *bitpos + (size_t)i;
        v |= (int)((d[b >> 3] >> (b & 7)) & 1u) << i;
    }
    *bitpos += (size_t)size;
    return v;
}

/* GIF LZW 解码: 字面量码 0..2^min-1, CLEAR = 2^min, END = CLEAR+1,
 * 字典项自 END+1 起。*out 已按 npix 预分配, 返回 1 = 恰好解出 npix 个索引。
 * 码长增长规则必须与编码器精确对齐 —— 差一位会让后半帧整片错乱,
 * 因此这里按"解码器建表后 next == 2^code_size 时增长"的经典口径实现。 */
static int gif_lzw(const unsigned char *in, size_t n, int min_code,
                   unsigned char *out, int npix) {
    if (!in || n == 0 || min_code < 2 || min_code > 8 || npix <= 0) return 0;
    const int CLEAR = 1 << min_code, END = CLEAR + 1;
    unsigned short *pre = (unsigned short *)malloc(sizeof(unsigned short) * GIF_LZW_MAX);
    unsigned char *suf = (unsigned char *)malloc(GIF_LZW_MAX);
    unsigned char *fch = (unsigned char *)malloc(GIF_LZW_MAX);
    unsigned char *stack = (unsigned char *)malloc(GIF_LZW_MAX);
    if (!pre || !suf || !fch || !stack) {
        free(pre); free(suf); free(fch); free(stack);
        return 0;
    }
    int code_size = min_code + 1;
    int next = END + 1;
    int prev = -1;
    int cc = 0;
    size_t bitpos = 0;
    int ok = 0;
    /* 字面量的"串首字符"= 自身(KwKwK 回指要用); 不初始化会读到未定义值 */
    for (int i = 0; i < CLEAR + 2; i++) fch[i] = (unsigned char)i;
    for (;;) {
        int code = g_read_bits(in, n, &bitpos, code_size);
        if (code < 0) break;
        if (code == CLEAR) {
            code_size = min_code + 1;
            next = END + 1;
            prev = -1;
            continue;
        }
        if (code == END) { ok = (cc >= npix); break; }
        int incode = code;
        int sp = 0;
        if (prev < 0) {
            /* CLEAR 之后的首码必为字面量(规范); 非字面量视为流损坏 */
            if (code >= CLEAR) break;
            out[cc++] = (unsigned char)code;
            if (cc >= npix) { ok = 1; break; }
            prev = code;
            continue;
        }
        if (code >= next) {
            /* KwKwK: 码尚未入表(= next), 语义是"上一串 + 上一串首字符" */
            if (code > next) break;
            stack[sp++] = fch[prev];
            code = prev;
        }
        while (code >= CLEAR) {
            if (code >= next || sp >= GIF_LZW_MAX - 1) { sp = -1; break; }
            stack[sp++] = suf[code];
            code = pre[code];
        }
        if (sp < 0) break;
        stack[sp++] = (unsigned char)code;
        unsigned char first_ch = stack[sp - 1];
        while (sp > 0) {
            out[cc++] = stack[--sp];
            if (cc >= npix) break;
        }
        if (cc >= npix) { ok = 1; break; }
        if (next < GIF_LZW_MAX) {
            pre[next] = (unsigned short)prev;
            suf[next] = first_ch;
            fch[next] = fch[prev];
            next++;
            /* 码长增长: 解码器比编码器晚一个条目建表, 因此判据是
               next == 2^code_size(不是 >, 也不是 >= 后再等一个) ——
               用 tools/gif_probe.c 的多编码器夹具(含 PIL/GIF89a 交织样本)
               逐索引比对过, 差一个码长会让后半帧整片错乱。 */
            if (next == (1 << code_size) && code_size < 12) code_size++;
        }
        prev = incode;
    }
    free(pre); free(suf); free(fch); free(stack);
    return ok;
}

/* ---------------- 帧结构 ---------------- */

typedef struct {
    int x, y, w, h;
    int disposal;            /* 0..3 */
    int tindex;              /* 透明色索引(-1 = 无) */
    int delay_cs;            /* 百分之一秒(原始值) */
    float delay_ms;          /* 规范值(0 → 100ms) */
    int has_lct;
    unsigned char lct[256 * 3];
    int lct_n;
    unsigned char *idx;      /* 帧局部索引(w*h; 已反交织) */
    unsigned char *rgba;     /* 合成后的整幅 RGBA(w*h*4) */
    unsigned char *saved;    /* disposal=3 用: 本帧绘制前的画布快照 */
} gif_frame;

struct hn_gif_s {
    int w, h;
    int bg_idx;
    int has_gct;
    unsigned char gct[256 * 3];
    int gct_n;
    int loops;               /* NETSCAPE 循环次数; 0 = 无限 */
    int has_transparency;    /* 任一帧声明了透明色 */
    gif_frame *frames;
    int n_frames;
};

int hn_gif_width(const hn_gif *g)  { return g ? g->w : 0; }
int hn_gif_height(const hn_gif *g) { return g ? g->h : 0; }
int hn_gif_frame_count(const hn_gif *g) { return g ? g->n_frames : 0; }
int hn_gif_loop_count(const hn_gif *g)  { return g ? g->loops : 0; }

float hn_gif_frame_delay_ms(const hn_gif *g, int i) {
    if (!g || i < 0 || i >= g->n_frames) return 0;
    return g->frames[i].delay_ms;
}

int hn_gif_frame_transparent(const hn_gif *g, int i) {
    if (!g || i < 0 || i >= g->n_frames) return -1;
    return g->frames[i].tindex;
}

/* ---------------- 合成 ---------------- */

/* 把画布上矩形区域恢复为"画布起点色" */
static void gif_restore_rect(hn_gif *g, unsigned char *canvas, int x, int y,
                             int w, int h) {
    if (w <= 0 || h <= 0) return;
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > g->w) w = g->w - x;
    if (y + h > g->h) h = g->h - y;
    if (w <= 0 || h <= 0) return;
    /* 起点色: 有透明声明 → 透明(alpha 0); 否则背景色(alpha 255) */
    unsigned char r = 0, gg = 0, b = 0;
    int a = 0;
    if (!g->has_transparency && g->has_gct && g->bg_idx < g->gct_n) {
        r = g->gct[g->bg_idx * 3];
        gg = g->gct[g->bg_idx * 3 + 1];
        b = g->gct[g->bg_idx * 3 + 2];
        a = 255;
    }
    for (int yy = 0; yy < h; yy++) {
        unsigned char *row = canvas + ((size_t)(y + yy) * (size_t)g->w + (size_t)x) * 4;
        for (int xx = 0; xx < w; xx++) {
            row[xx * 4] = r; row[xx * 4 + 1] = gg; row[xx * 4 + 2] = b;
            row[xx * 4 + 3] = (unsigned char)a;
        }
    }
}

/* 把一帧的索引图叠加到画布(透明索引跳过 = 保留画布旧内容) */
static void gif_draw_frame(hn_gif *g, unsigned char *canvas, const gif_frame *f) {
    const unsigned char *ct = f->has_lct ? f->lct : g->gct;
    int ct_n = f->has_lct ? f->lct_n : g->gct_n;
    for (int fy = 0; fy < f->h; fy++) {
        int cy = f->y + fy;
        if (cy < 0 || cy >= g->h) continue;
        for (int fx = 0; fx < f->w; fx++) {
            int cx = f->x + fx;
            if (cx < 0 || cx >= g->w) continue;
            int idx = f->idx[fy * f->w + fx];
            if (idx == f->tindex) continue;
            if (idx >= ct_n) continue;
            unsigned char *o = canvas + ((size_t)cy * (size_t)g->w + (size_t)cx) * 4;
            o[0] = ct[idx * 3];
            o[1] = ct[idx * 3 + 1];
            o[2] = ct[idx * 3 + 2];
            o[3] = 255;
        }
    }
}

/* 逐帧合成(顺序推进 + disposal 语义), 每帧留一份完整 RGBA。 */
static int gif_compose(hn_gif *g) {
    size_t wh = (size_t)g->w * (size_t)g->h;
    unsigned char *canvas = (unsigned char *)calloc(wh * 4, 1);
    if (!canvas) return 0;
    if (!g->has_transparency && g->has_gct && g->bg_idx < g->gct_n)
        gif_restore_rect(g, canvas, 0, 0, g->w, g->h);
    for (int i = 0; i < g->n_frames; i++) {
        gif_frame *f = &g->frames[i];
        if (i > 0) {
            const gif_frame *p = &g->frames[i - 1];
            if (p->disposal == 2) {
                gif_restore_rect(g, canvas, p->x, p->y, p->w, p->h);
            } else if (p->disposal == 3 && p->saved) {
                memcpy(canvas, p->saved, wh * 4);
            }
        }
        if (f->disposal == 3) {
            f->saved = (unsigned char *)malloc(wh * 4);
            if (!f->saved) { free(canvas); return 0; }
            memcpy(f->saved, canvas, wh * 4);
        }
        gif_draw_frame(g, canvas, f);
        f->rgba = (unsigned char *)malloc(wh * 4);
        if (!f->rgba) { free(canvas); return 0; }
        memcpy(f->rgba, canvas, wh * 4);
    }
    free(canvas);
    return 1;
}

/* ---------------- 解码 ---------------- */

#define GIF_MAX_DIM 8192
#define GIF_MAX_PIXELS (16u * 1024u * 1024u)   /* 单帧像素上限(防异常文件) */
#define GIF_MAX_FRAMES 2048
#define GIF_MAX_TOTAL (256u * 1024u * 1024u)   /* 全部合成帧的总字节上限 */

hn_gif *hn_gif_decode(const unsigned char *data, size_t n) {
    if (!data || n < 13) return NULL;
    int is87 = !memcmp(data, "GIF87a", 6);
    int is89 = !memcmp(data, "GIF89a", 6);
    if (!is87 && !is89) return NULL;

    gcur c;
    c.p = data; c.n = n; c.i = 6;
    int gw = g_u16(&c), gh = g_u16(&c);
    int packed = g_u8(&c);
    int bg_idx = g_u8(&c);
    (void)g_u8(&c);                                /* 像素宽高比 */
    if (gw <= 0 || gh <= 0 || gw > GIF_MAX_DIM || gh > GIF_MAX_DIM) return NULL;
    if ((unsigned)gw * (unsigned)gh > GIF_MAX_PIXELS) return NULL;

    hn_gif *g = (hn_gif *)calloc(1, sizeof(*g));
    if (!g) return NULL;
    g->w = gw;
    g->h = gh;
    g->bg_idx = bg_idx;

    if (packed & 0x80) {
        int ncol = 1 << ((packed & 7) + 1);
        if (ncol > 256) ncol = 256;
        if (c.i + (size_t)ncol * 3 > c.n) { free(g); return NULL; }
        memcpy(g->gct, c.p + c.i, (size_t)ncol * 3);
        g->gct_n = ncol;
        g->has_gct = 1;
        c.i += (size_t)ncol * 3;
    }

    /* 待应用的图形控制扩展(作用于**下一个**图像描述符) */
    int pend_disposal = 0, pend_tindex = -1, pend_delay = 0;

    int cap = 0;
    for (;;) {
        int blk = g_u8(&c);
        if (blk < 0) break;
        if (blk == 0x3B) break;                    /* trailer */
        if (blk == 0x21) {                         /* 扩展块 */
            int label = g_u8(&c);
            if (label < 0) break;
            if (label == 0xF9) {                   /* 图形控制扩展 */
                int size = g_u8(&c);
                if (size != 4) { g_skip_subblocks(&c); continue; }
                int p2 = g_u8(&c);
                int dlo = g_u8(&c), dhi = g_u8(&c);
                int ti = g_u8(&c);
                (void)g_u8(&c);                    /* 终止 0 */
                if (p2 < 0 || dlo < 0 || dhi < 0 || ti < 0) break;
                pend_disposal = (p2 >> 2) & 7;
                pend_delay = (dlo | (dhi << 8)) * 10;   /* 1/100s → ms */
                pend_tindex = (p2 & 1) ? ti : -1;
            } else if (label == 0xFF) {            /* 应用扩展 */
                int size = g_u8(&c);
                if (size == 11) {
                    unsigned char app[11];
                    int okread = 1;
                    for (int k = 0; k < 11; k++) {
                        int v = g_u8(&c);
                        if (v < 0) { okread = 0; break; }
                        app[k] = (unsigned char)v;
                    }
                    if (okread && !memcmp(app, "NETSCAPE2.0", 11)) {
                        /* 子块: [1, loop_lo, loop_hi] —— 首字节是子块 ID(1)，
                           不是计数值。把它当 lo 会让"无限循环"(0,0)读成 1 次 ——
                           实测: 夹具的 NETSCAPE 块 [1,0,0] 被解析成 loops=1,
                           循环回绕断言直接失败。 */
                        for (;;) {
                            int len = g_u8(&c);
                            if (len <= 0) break;
                            if (len >= 3) {
                                (void)g_u8(&c);            /* 子块 ID(1) */
                                int lo = g_u8(&c), hi = g_u8(&c);
                                int extra = len - 3;
                                if (lo >= 0 && hi >= 0) {
                                    int cnt = lo | (hi << 8);
                                    g->loops = cnt;   /* 0 = 无限 */
                                }
                                if (extra > 0) g_skip(&c, (size_t)extra);
                            } else {
                                g_skip(&c, (size_t)len);
                            }
                        }
                    } else {
                        g_skip_subblocks(&c);
                    }
                } else {
                    if (size > 0) g_skip(&c, (size_t)size);
                    g_skip_subblocks(&c);
                }
            } else {
                g_skip_subblocks(&c);              /* 注释/纯文本等: 跳过 */
            }
            continue;
        }
        if (blk != 0x2C) {                         /* 未知块: 防死循环直接终止 */
            break;
        }

        /* ---- 图像描述符 ---- */
        int fx = g_u16(&c), fy = g_u16(&c), fw = g_u16(&c), fh = g_u16(&c);
        int ip = g_u8(&c);
        if (fx < 0 || fy < 0 || fw <= 0 || fh <= 0 || ip < 0) break;
        if ((unsigned)fw * (unsigned)fh > GIF_MAX_PIXELS) break;
        if (g->n_frames >= GIF_MAX_FRAMES) break;

        /* 顺序必须是: 描述符 → (局部色表) → LZW 最小码长 → 数据子块。
           局部色表在 packed 之后、min_code 之前 —— 颠倒过来会把色表首字节
           当成 min_code, 整帧索引全错(表现为整幅变成色表第 0 项的颜色)。 */
        unsigned char lct[256 * 3];
        int lct_n = 0, has_lct = 0;
        if (ip & 0x80) {
            int ncol = 1 << ((ip & 7) + 1);
            if (ncol > 256) ncol = 256;
            if (c.i + (size_t)ncol * 3 > c.n) break;
            memcpy(lct, c.p + c.i, (size_t)ncol * 3);
            lct_n = ncol;
            has_lct = 1;
            c.i += (size_t)ncol * 3;
        }
        int min_code = g_u8(&c);
        if (min_code < 0) break;
        unsigned char *lzw = NULL;
        size_t lzw_n = 0;
        if (!g_read_subblocks(&c, &lzw, &lzw_n)) break;

        if (g->n_frames == cap) {
            int nc = cap ? cap * 2 : 8;
            gif_frame *nf = (gif_frame *)realloc(g->frames, sizeof(gif_frame) * (size_t)nc);
            if (!nf) { free(lzw); break; }
            memset(nf + cap, 0, sizeof(gif_frame) * (size_t)(nc - cap));
            g->frames = nf;
            cap = nc;
        }
        gif_frame *f = &g->frames[g->n_frames];
        memset(f, 0, sizeof(*f));
        f->x = fx; f->y = fy; f->w = fw; f->h = fh;
        f->disposal = pend_disposal;
        f->tindex = pend_tindex;
        f->delay_cs = pend_delay / 10;
        /* 0 延迟按浏览器口径计 100ms; 显式 >0 原样 */
        f->delay_ms = pend_delay > 0 ? (float)pend_delay : 100.0f;
        f->has_lct = has_lct;
        if (has_lct) { memcpy(f->lct, lct, (size_t)lct_n * 3); f->lct_n = lct_n; }
        if (f->tindex >= 0) g->has_transparency = 1;

        f->idx = (unsigned char *)malloc((size_t)fw * (size_t)fh);
        if (!f->idx) { free(lzw); break; }
        memset(f->idx, 0, (size_t)fw * (size_t)fh);
        if (!gif_lzw(lzw, lzw_n, min_code, f->idx, fw * fh)) {
            /* LZW 流损坏: 该帧按全透明占位(仍然保留帧号, 不破坏时间轴) */
            memset(f->idx, 0, (size_t)fw * (size_t)fh);
        }
        free(lzw);
        /* 交织: 4 趟(0,8)(4,8)(2,4)(1,2) 行序 → 顺序行 */
        if (ip & 0x40) {
            unsigned char *lin = (unsigned char *)malloc((size_t)fw * (size_t)fh);
            if (lin) {
                static const int sy[4] = { 0, 4, 2, 1 };
                static const int dy[4] = { 8, 8, 4, 2 };
                int o = 0;
                for (int p = 0; p < 4; p++)
                    for (int y = sy[p]; y < fh; y += dy[p])
                        for (int x = 0; x < fw; x++)
                            lin[(size_t)y * (size_t)fw + (size_t)x] = f->idx[o++];
                memcpy(f->idx, lin, (size_t)fw * (size_t)fh);
                free(lin);
            }
        }
        g->n_frames++;
        pend_disposal = 0;
        pend_tindex = -1;
        pend_delay = 0;
    }

    if (g->n_frames <= 0) { hn_gif_free(g); return NULL; }
    if ((size_t)g->w * (size_t)g->h * 4u * (size_t)g->n_frames > GIF_MAX_TOTAL) {
        hn_gif_free(g);
        return NULL;
    }
    if (!gif_compose(g)) { hn_gif_free(g); return NULL; }
    return g;
}

void hn_gif_free(hn_gif *g) {
    if (!g) return;
    for (int i = 0; i < g->n_frames; i++) {
        free(g->frames[i].idx);
        free(g->frames[i].rgba);
        free(g->frames[i].saved);
    }
    free(g->frames);
    free(g);
}

const unsigned char *hn_gif_frame_rgba(const hn_gif *g, int i) {
    if (!g || i < 0 || i >= g->n_frames) return NULL;
    return g->frames[i].rgba;
}

/* 当前帧号 = f(clock)。纯函数: 循环回绕/跳时钟/倒退都直接算,
   不依赖"上一帧"状态 —— 与 Lottie 的"按 t 求值"同一口径。 */
int hn_gif_frame_index(const hn_gif *g, float clock_ms) {
    if (!g || g->n_frames <= 0) return 0;
    if (g->n_frames == 1) return 0;
    double total = 0;
    for (int i = 0; i < g->n_frames; i++) total += (double)g->frames[i].delay_ms;
    if (total <= 0.0) return 0;
    double t = clock_ms > 0 ? (double)clock_ms : 0.0;
    if (g->loops > 0) {
        double span = total * (double)g->loops;
        if (t >= span) return g->n_frames - 1;     /* 播完停在末帧 */
    }
    /* 有限循环内也要按 total 取模 —— 只判"未越 span"而不取模, 第二遍起
       t 始终大于 total, 逐帧扫描必然落到末帧(实测: 3 次循环里第 2 遍
       的每一时刻都被报成末帧)。 */
    if (t >= total) t -= total * (double)(long long)(t / total);
    double acc = 0;
    for (int i = 0; i < g->n_frames; i++) {
        acc += (double)g->frames[i].delay_ms;
        if (t < acc) return i;
    }
    return g->n_frames - 1;
}

/* ---------------- <img> 挂载辅助 ---------------- */

/* 该元素要按 GIF 动图播放的源:
     hn-gif="a.gif"                 显式路径
     <img src="a.gif" hn-gif>       布尔属性(值为空串/true/1), 路径取 src
     <img src="a.gif">              自然: src 以 .gif 结尾(不区分大小写)
   仅 "false"/"0" 视为显式关闭。非 <img> 一律返回 NULL。 */
const char *hn_node_gif_src(hn_node *n) {
    if (!n || n->kind != HN_ELEM || !n->tag || strcmp(n->tag, "img")) return NULL;
    const char *v = hn_node_attr(n, "hn-gif");
    if (v) {
        if (!strcmp(v, "false") || !strcmp(v, "0")) return NULL;
        if (*v && strcmp(v, "true") && strcmp(v, "1")) return v;
        const char *src = hn_node_attr(n, "src");
        return (src && *src) ? src : NULL;
    }
    const char *src = hn_node_attr(n, "src");
    if (!src) return NULL;
    size_t sl = strlen(src);
    if (sl < 5) return NULL;
    const char *e = src + sl - 4;
    if ((e[0] == '.') &&
        (e[1] == 'g' || e[1] == 'G') &&
        (e[2] == 'i' || e[2] == 'I') &&
        (e[3] == 'f' || e[3] == 'F'))
        return src;
    return NULL;
}

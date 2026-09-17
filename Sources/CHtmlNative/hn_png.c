/* hn_png.c — PNG 解码(纯 C, 零依赖)
 *
 * 为什么需要它: 软件光栅后端(hnsoft)此前完全没有图像解码能力, 于是
 * Linux / Windows / 无头环境里所有图片都退化成"蓝色网格线"占位 ——
 * 也就是说跨平台能力在"有图"的场景下其实是不完整的。平台原生后端可以直接
 * 调系统 API(AppKit / GDI+), 但 hnsoft 的承诺是"只要有 C 编译器就能构建",
 * 因此必须自己解。
 *
 * 支持范围(覆盖绝大多数 UI 资源):
 *   位深 8 / 16(降到 8), 色彩类型 0 灰度 / 2 真彩 / 3 索引 / 4 灰度+alpha /
 *   6 真彩+alpha; 五种 PNG 滤镜(0..4); 多 IDAT 拼接; tRNS 透明扩展;
 *   Adam7 交织。
 * 不支持: 1/2/4 位深、APNG 动图(取第一帧)、色彩管理(按 sRGB)。
 *
 * inflate 是自带的(存储 / 固定哈夫曼 / 动态哈夫曼), 因为 zlib 不是标准 C 库 ——
 * 这也是本文件大半代码的来源。
 */
#include "hn_png.h"
#include <stdlib.h>
#include <string.h>

typedef struct { const unsigned char *p; size_t n, i; } rd;

static unsigned rd_u32(const unsigned char *q) {
    return ((unsigned)q[0] << 24) | ((unsigned)q[1] << 16) |
           ((unsigned)q[2] << 8) | (unsigned)q[3];
}

/* ---------------- inflate ---------------- */

typedef struct {
    const unsigned char *src;
    size_t n, i;
    unsigned bitbuf;
    int bitcnt, overrun;
    unsigned char *out;
    size_t outn, outcap;
} inf;

typedef struct { unsigned short count[16]; unsigned short symbol[288]; } huff;

static int in_bit(inf *s) {
    if (s->bitcnt == 0) {
        if (s->i >= s->n) { s->overrun = 1; return 0; }
        s->bitbuf = s->src[s->i++];
        s->bitcnt = 8;
    }
    int v = s->bitbuf & 1;
    s->bitbuf >>= 1;
    s->bitcnt--;
    return v;
}

static int in_bits(inf *s, int need) {
    int v = 0;
    for (int i = 0; i < need; i++) v |= in_bit(s) << i;
    return v;
}

static int huff_build(huff *h, const unsigned char *lengths, int n) {
    memset(h->count, 0, sizeof(h->count));
    for (int i = 0; i < n; i++) h->count[lengths[i]]++;
    h->count[0] = 0;
    unsigned short offs[16];
    offs[0] = 0;
    for (int i = 1; i < 16; i++) offs[i] = (unsigned short)(offs[i - 1] + h->count[i - 1]);
    for (int i = 0; i < n; i++)
        if (lengths[i]) {
            if (offs[lengths[i]] >= 288) return 0;
            h->symbol[offs[lengths[i]]++] = (unsigned short)i;
        }
    return 1;
}

static int huff_dec(inf *s, const huff *h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; len++) {
        code |= in_bit(s);
        int count = h->count[len];
        if (code - first < count) return h->symbol[index + (code - first)];
        index += count;
        first = (first + count) << 1;
        code <<= 1;
        if (s->overrun) return -1;
    }
    return -1;
}

static int out_byte(inf *s, unsigned char v) {
    if (s->outn == s->outcap) {
        size_t nc = s->outcap ? s->outcap * 2 : 4096;
        unsigned char *no = (unsigned char *)realloc(s->out, nc);
        if (!no) { s->overrun = 1; return 0; }
        s->out = no;
        s->outcap = nc;
    }
    s->out[s->outn++] = v;
    return 1;
}

static const unsigned short LEN_BASE[29] = {
    3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258
};
static const unsigned char LEN_EXTRA[29] = {
    0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0
};
static const unsigned short DIST_BASE[30] = {
    1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,
    3073,4097,6145,8193,12289,16385,24577
};
static const unsigned char DIST_EXTRA[30] = {
    0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13
};

static int inf_data(inf *s, const huff *lh, const huff *dh) {
    for (;;) {
        int sym = huff_dec(s, lh);
        if (sym < 0) return 0;
        if (sym < 256) {
            if (!out_byte(s, (unsigned char)sym)) return 0;
            continue;
        }
        if (sym == 256) return 1;
        sym -= 257;
        if (sym >= 29) return 0;
        int len = LEN_BASE[sym] + in_bits(s, LEN_EXTRA[sym]);
        int ds = huff_dec(s, dh);
        if (ds < 0 || ds >= 30) return 0;
        size_t dist = (size_t)DIST_BASE[ds] + (size_t)in_bits(s, DIST_EXTRA[ds]);
        if (dist > s->outn) return 0;
        size_t from = s->outn - dist;
        for (int i = 0; i < len; i++)
            if (!out_byte(s, s->out[from + (size_t)i])) return 0;
        if (s->overrun) return 0;
    }
}

static void fixed_tables(huff *lh, huff *dh) {
    unsigned char l[288];
    for (int i = 0; i < 144; i++) l[i] = 8;
    for (int i = 144; i < 256; i++) l[i] = 9;
    for (int i = 256; i < 280; i++) l[i] = 7;
    for (int i = 280; i < 288; i++) l[i] = 8;
    huff_build(lh, l, 288);
    unsigned char d[30];
    memset(d, 5, sizeof(d));
    huff_build(dh, d, 30);
}

static void dyn_tables(inf *s, huff *lh, huff *dh) {
    static const unsigned char ORDER[19] = {
        16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15
    };
    int hlit = in_bits(s, 5) + 257;
    int hdist = in_bits(s, 5) + 1;
    int hclen = in_bits(s, 4) + 4;
    if (hlit > 286 || hdist > 30 || hclen > 19) { s->overrun = 1; return; }
    unsigned char cl[19];
    memset(cl, 0, sizeof(cl));
    for (int i = 0; i < hclen; i++) cl[ORDER[i]] = (unsigned char)in_bits(s, 3);
    huff ch;
    if (!huff_build(&ch, cl, 19)) { s->overrun = 1; return; }
    unsigned char lengths[320];
    int n = 0, total = hlit + hdist;
    while (n < total) {
        int sym = huff_dec(s, &ch);
        if (sym < 0) return;
        if (sym < 16) {
            lengths[n++] = (unsigned char)sym;
        } else if (sym == 16) {
            if (n == 0) { s->overrun = 1; return; }
            int rep = 3 + in_bits(s, 2);
            unsigned char prev = lengths[n - 1];
            while (rep-- && n < total) lengths[n++] = prev;
        } else if (sym == 17) {
            int rep = 3 + in_bits(s, 3);
            while (rep-- && n < total) lengths[n++] = 0;
        } else {
            int rep = 11 + in_bits(s, 7);
            while (rep-- && n < total) lengths[n++] = 0;
        }
        if (s->overrun) return;
    }
    if (!huff_build(lh, lengths, hlit)) { s->overrun = 1; return; }
    if (!huff_build(dh, lengths + hlit, hdist)) { s->overrun = 1; return; }
}

static unsigned char *inflate_raw(const unsigned char *src, size_t n, size_t *out_len) {
    inf s;
    memset(&s, 0, sizeof(s));
    s.src = src;
    s.n = n;
    huff lh, dh;
    int guard_blocks = 0;
    while (guard_blocks++ < 100000) {
        int last = in_bit(&s);
        int type = in_bits(&s, 2);
        if (s.overrun) break;
        if (type == 0) {
            s.bitcnt = 0;                       /* 对齐到字节 */
            if (s.i + 4 > s.n) break;
            unsigned len = (unsigned)s.src[s.i] | ((unsigned)s.src[s.i + 1] << 8);
            s.i += 4;                           /* 跳过 LEN + NLEN */
            for (unsigned k = 0; k < len; k++) {
                if (s.i >= s.n) break;
                if (!out_byte(&s, s.src[s.i++])) break;
            }
        } else if (type == 1) {
            fixed_tables(&lh, &dh);
            if (!inf_data(&s, &lh, &dh)) break;
        } else if (type == 2) {
            dyn_tables(&s, &lh, &dh);
            if (s.overrun) break;
            if (!inf_data(&s, &lh, &dh)) break;
        } else {
            break;                              /* type 3 非法 */
        }
        if (last || s.overrun) break;
    }
    *out_len = s.outn;
    if (s.outn == 0) { free(s.out); return NULL; }
    return s.out;
}

/* ---------------- 反滤镜 ---------------- */

static unsigned paeth(int a, int b, int c) {
    int p = a + b - c;
    int pa = p > a ? p - a : a - p;
    int pb = p > b ? p - b : b - p;
    int pc = p > c ? p - c : c - p;
    if (pa <= pb && pa <= pc) return (unsigned)a;
    if (pb <= pc) return (unsigned)b;
    return (unsigned)c;
}

/* 解压后的原始扫描线数据(只读游标; 交织多趟共享同一份并按顺序推进) */
typedef struct { const unsigned char *p; size_t n; } inf_ro;

/* 从 raw 里逐行读取并反滤波, 返回 1 并在 *rows 给出行缓冲(第 0 行上方留一格全 0)。
   *consumed 回传已消费的字节数, 供交织多趟顺序推进。 */
static int unfilter(inf_ro *ro, int w, int h, int bpp,
                    unsigned char **rows, int *stride, size_t *consumed) {
    int st = w * bpp;
    unsigned char *buf = (unsigned char *)calloc((size_t)st * (size_t)(h + 1), 1);
    if (!buf) return 0;
    size_t pos = *consumed;
    for (int y = 0; y < h; y++) {
        unsigned char *cur = buf + (size_t)st * (size_t)(y + 1);
        unsigned char *prev = buf + (size_t)st * (size_t)y;
        if (pos >= ro->n) break;
        int ft = ro->p[pos++];
        size_t avail = ro->n - pos;
        size_t take = (size_t)st < avail ? (size_t)st : avail;
        memcpy(cur, ro->p + pos, take);
        pos += take;
        for (int i = 0; i < st; i++) {
            int a = i >= bpp ? cur[i - bpp] : 0;
            int b = prev[i];
            int c = (i >= bpp) ? prev[i - bpp] : 0;
            switch (ft) {
            case 1: cur[i] = (unsigned char)(cur[i] + a); break;
            case 2: cur[i] = (unsigned char)(cur[i] + b); break;
            case 3: cur[i] = (unsigned char)(cur[i] + ((a + b) >> 1)); break;
            case 4: cur[i] = (unsigned char)(cur[i] + (int)paeth(a, b, c)); break;
            default: break;                     /* 0 与未知滤镜: 原样 */
            }
        }
    }
    *rows = buf;
    *stride = st;
    *consumed = pos;
    return 1;
}

/* 一个样本 → RGBA8 */
typedef struct {
    int colortype, channels, samp;
    const unsigned char *pal; int pal_n;
    const unsigned char *trns_pal; int trns_pal_n;
    int trns_r, trns_g, trns_b, trns_gray;
} pxinfo;

static void sample_to_rgba(unsigned char *o, const unsigned char *line, int x,
                           const pxinfo *pi) {
    int off = x * pi->channels * pi->samp;
    const unsigned char *p = line + off;
    int hi = pi->samp - 1;                     /* 16 位取高字节 */
    switch (pi->colortype) {
    case 0: {                                  /* 灰度 */
        int g = pi->samp == 1 ? p[0] : p[hi];
        o[0] = o[1] = o[2] = (unsigned char)g;
        o[3] = (pi->trns_gray >= 0 && g == pi->trns_gray) ? 0 : 255;
        break;
    }
    case 2: {                                  /* 真彩 */
        int r = pi->samp == 1 ? p[0] : p[hi];
        int g = pi->samp == 1 ? p[1] : p[2 + hi];
        int b = pi->samp == 1 ? p[2] : p[4 + hi];
        o[0] = (unsigned char)r; o[1] = (unsigned char)g; o[2] = (unsigned char)b;
        int is_key = (pi->trns_r >= 0 && r == pi->trns_r && g == pi->trns_g && b == pi->trns_b);
        o[3] = is_key ? 0 : 255;
        break;
    }
    case 3: {                                  /* 索引 */
        int idx = p[0];
        o[0] = pi->pal[idx * 3]; o[1] = pi->pal[idx * 3 + 1]; o[2] = pi->pal[idx * 3 + 2];
        o[3] = (idx < pi->trns_pal_n) ? pi->trns_pal[idx] : 255;
        break;
    }
    case 4: {                                  /* 灰度 + alpha */
        int g = pi->samp == 1 ? p[0] : p[hi];
        int a = pi->samp == 1 ? p[1] : p[2 + hi];
        o[0] = o[1] = o[2] = (unsigned char)g;
        o[3] = (unsigned char)a;
        break;
    }
    default: {                                 /* 6: 真彩 + alpha */
        int r = pi->samp == 1 ? p[0] : p[hi];
        int g = pi->samp == 1 ? p[1] : p[2 + hi];
        int b = pi->samp == 1 ? p[2] : p[4 + hi];
        int a = pi->samp == 1 ? p[3] : p[6 + hi];
        o[0] = (unsigned char)r; o[1] = (unsigned char)g; o[2] = (unsigned char)b;
        o[3] = (unsigned char)a;
        break;
    }
    }
}

/* ---------------- 公共入口 ---------------- */

unsigned char *hn_png_decode(const unsigned char *data, size_t n, int *w, int *h) {
    if (!data || n < 8 || memcmp(data, "\x89PNG\r\n\x1a\n", 8) != 0) return NULL;
    size_t i = 8;
    int width = 0, height = 0, bitdepth = 0, colortype = 0, interlace = 0;
    unsigned char *idat = NULL;
    size_t idat_n = 0, idat_cap = 0;
    unsigned char palette[256 * 3];
    int pal_n = 0;
    int trns_r = -1, trns_g = -1, trns_b = -1, trns_gray = -1;
    unsigned char trns_pal[256];
    int trns_pal_n = 0;

    while (i + 8 <= n) {
        unsigned len = rd_u32(data + i);
        const unsigned char *type = data + i + 4;
        const unsigned char *body = data + i + 8;
        if (i + 12 + len > n) break;                    /* 截断的块 */
        if (len >= 4 && !memcmp(type, "IHDR", 4) && len >= 13) {
            width = (int)rd_u32(body);
            height = (int)rd_u32(body + 4);
            bitdepth = body[8];
            colortype = body[9];
            interlace = body[12];
        } else if (!memcmp(type, "PLTE", 4)) {
            int cnt = (int)(len / 3);
            if (cnt > 256) cnt = 256;
            memcpy(palette, body, (size_t)cnt * 3);
            pal_n = cnt;
        } else if (!memcmp(type, "tRNS", 4)) {
            if (colortype == 3) {
                int cnt = (int)len;
                if (cnt > 256) cnt = 256;
                memcpy(trns_pal, body, (size_t)cnt);
                trns_pal_n = cnt;
            } else if (colortype == 0 && len >= 2) {
                trns_gray = (body[0] << 8) | body[1];
            } else if (colortype == 2 && len >= 6) {
                trns_r = (body[0] << 8) | body[1];
                trns_g = (body[2] << 8) | body[3];
                trns_b = (body[4] << 8) | body[5];
            }
        } else if (!memcmp(type, "IDAT", 4)) {
            if (idat_n + len > idat_cap) {
                size_t nc = idat_cap ? idat_cap * 2 : 8192;
                while (nc < idat_n + len) nc *= 2;
                unsigned char *ni = (unsigned char *)realloc(idat, nc);
                if (!ni) { free(idat); return NULL; }
                idat = ni;
                idat_cap = nc;
            }
            memcpy(idat + idat_n, body, len);
            idat_n += len;
        } else if (!memcmp(type, "IEND", 4)) {
            break;
        }
        i += 12 + len;
    }
    if (width <= 0 || height <= 0 || width > 65535 || height > 65535) { free(idat); return NULL; }
    if (!idat) return NULL;
    /* 只支持 8/16 位与五类常见色彩类型 */
    if (bitdepth != 8 && bitdepth != 16) { free(idat); return NULL; }
    if (colortype != 0 && colortype != 2 && colortype != 3 &&
        colortype != 4 && colortype != 6) { free(idat); return NULL; }
    if (colortype == 3 && pal_n == 0) { free(idat); return NULL; }

    /* 索引色模式下位深可能是 1/2/4/8 —— 只认 8 */
    if (colortype == 3 && bitdepth != 8) { free(idat); return NULL; }

    size_t zlen = idat_n > 2 ? idat_n - 2 : 0;      /* 前 2 字节是 zlib 头 */
    size_t raw_n = 0;
    unsigned char *raw = inflate_raw(idat + 2, zlen, &raw_n);
    free(idat);
    if (!raw) return NULL;

    int channels = (colortype == 0) ? 1 : (colortype == 2) ? 3 :
                   (colortype == 3) ? 1 : (colortype == 4) ? 2 : 4;
    int samp = bitdepth / 8;
    int bpp = channels * samp;

    pxinfo pi;
    pi.colortype = colortype; pi.channels = channels; pi.samp = samp;
    pi.pal = palette; pi.pal_n = pal_n;
    pi.trns_pal = trns_pal; pi.trns_pal_n = trns_pal_n;
    pi.trns_r = trns_r; pi.trns_g = trns_g; pi.trns_b = trns_b; pi.trns_gray = trns_gray;

    unsigned char *out = (unsigned char *)calloc((size_t)width * (size_t)height * 4, 1);
    if (!out) { free(raw); return NULL; }

    inf_ro ro;
    ro.p = raw;
    ro.n = raw_n;
    size_t consumed = 0;

    if (interlace == 0) {
        unsigned char *rows = NULL;
        int stride = 0;
        if (!unfilter(&ro, width, height, bpp, &rows, &stride, &consumed)) {
            free(raw); free(out); return NULL;
        }
        for (int y = 0; y < height; y++)
            for (int x = 0; x < width; x++)
                sample_to_rgba(out + ((size_t)y * (size_t)width + (size_t)x) * 4,
                               rows + (size_t)stride * (size_t)(y + 1), x, &pi);
        free(rows);
    } else {
        /* Adam7: 字节流是 7 趟的滤波行按顺序拼接, 逐趟解再散回原位。
           注意每趟的"上一行"是本趟内的上一行(首行上方补 0), 而不是整图的上一行。 */
        static const unsigned char SX[7] = { 0, 4, 0, 2, 0, 1, 0 };
        static const unsigned char SY[7] = { 0, 0, 4, 0, 2, 0, 1 };
        static const unsigned char DX[7] = { 8, 8, 4, 4, 2, 2, 1 };
        static const unsigned char DY[7] = { 8, 8, 8, 4, 4, 2, 2 };
        for (int pass = 0; pass < 7; pass++) {
            int pw = (width - SX[pass] + DX[pass] - 1) / DX[pass];
            int ph = (height - SY[pass] + DY[pass] - 1) / DY[pass];
            if (pw <= 0 || ph <= 0) continue;
            unsigned char *rows = NULL;
            int stride = 0;
            if (!unfilter(&ro, pw, ph, bpp, &rows, &stride, &consumed)) {
                free(rows); free(raw); free(out); return NULL;
            }
            for (int y = 0; y < ph; y++) {
                int py = SY[pass] + y * DY[pass];
                if (py >= height) continue;
                for (int x = 0; x < pw; x++) {
                    int px = SX[pass] + x * DX[pass];
                    if (px >= width) continue;
                    sample_to_rgba(out + ((size_t)py * (size_t)width + (size_t)px) * 4,
                                   rows + (size_t)stride * (size_t)(y + 1), x, &pi);
                }
            }
            free(rows);
        }
    }
    free(raw);
    *w = width;
    *h = height;
    return out;
}

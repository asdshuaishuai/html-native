/* hn_zip.c — 最小 ZIP 容器读写(实现注记见 hn_zip.h)
 *
 * 结构(APPNOTE.TXT):
 *   [本地文件头 PK\x03\x04 + 数据] ... [中央目录 PK\x01\x02 ...] [EOCD PK\x05\x06]
 * 读取顺序与写入相反: 从尾部扫 EOCD → 拿中央目录位置/条目数 → 遍历中央
 * 目录拿各条目的名字/压缩方法/尺寸/本地头偏移 → 按需从本地头取数据。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn_zip.h"
#include "hn_png.h"          /* hn_inflate: ZIP method=8 = RFC 1951 裸 deflate */

#define HN_ZIP_EOCD_SIG  0x06054b50u
#define HN_ZIP_CDIR_SIG  0x02014b50u
#define HN_ZIP_LFH_SIG   0x04034b50u

static unsigned rd_le16(const unsigned char *p) { return p[0] | ((unsigned)p[1] << 8); }
static unsigned rd_le32(const unsigned char *p) {
    return p[0] | ((unsigned)p[1] << 8) | ((unsigned)p[2] << 16) | ((unsigned)p[3] << 24);
}
static void wr_le16(unsigned char *p, unsigned v) { p[0] = (unsigned char)(v & 0xFF); p[1] = (unsigned char)((v >> 8) & 0xFF); }
static void wr_le32(unsigned char *p, unsigned v) {
    p[0] = (unsigned char)(v & 0xFF); p[1] = (unsigned char)((v >> 8) & 0xFF);
    p[2] = (unsigned char)((v >> 16) & 0xFF); p[3] = (unsigned char)((v >> 24) & 0xFF);
}

/* ---- 读 ---- */

int hn_zip_open(const unsigned char *buf, size_t n, hn_zip *z) {
    memset(z, 0, sizeof(*z));
    if (n < 22) return 0;
    size_t scan_max = n - 22;
    size_t scan_floor = scan_max > 65557 ? scan_max - 65557 : 0;
    size_t e = scan_max + 1;
    while (e-- > scan_floor) {
        if (rd_le32(buf + e) == HN_ZIP_EOCD_SIG) break;
    }
    if (e < scan_floor || rd_le32(buf + e) != HN_ZIP_EOCD_SIG) return 0;
    int total = (int)rd_le16(buf + e + 10);         /* 全部条目数 */
    size_t cd_off = rd_le32(buf + e + 16);
    size_t cd_size = rd_le32(buf + e + 12);
    if (cd_off + (size_t)cd_size > n || total <= 0) return 0;
    z->buf = buf; z->n = n;
    z->ents = (hn_zip_entry *)calloc((size_t)total, sizeof(hn_zip_entry));
    if (!z->ents) return 0;
    size_t p = cd_off, end = cd_off + cd_size;
    for (int i = 0; i < total && p + 46 <= end; i++) {
        if (rd_le32(buf + p) != HN_ZIP_CDIR_SIG) break;
        hn_zip_entry *e2 = &z->ents[z->n_ents++];
        e2->method    = rd_le16(buf + p + 10);
        e2->comp_size = rd_le32(buf + p + 20);
        e2->lfh_off   = rd_le32(buf + p + 42);
        e2->name_len  = rd_le16(buf + p + 28);
        e2->name      = (const char *)buf + p + 46;
        p += 46 + e2->name_len + rd_le16(buf + p + 30) + rd_le16(buf + p + 32);
    }
    return z->n_ents > 0;
}

void hn_zip_free(hn_zip *z) {
    if (!z) return;
    free(z->ents);
    z->ents = NULL;
    z->n_ents = 0;
}

int hn_zip_name_is(const hn_zip_entry *e, const char *name) {
    size_t l = strlen(name);
    return e->name_len == l && !memcmp(e->name, name, l);
}

int hn_zip_find(const hn_zip *z, const char *name, hn_zip_entry **out) {
    for (int i = 0; i < z->n_ents; i++)
        if (hn_zip_name_is(&z->ents[i], name)) { *out = &z->ents[i]; return 1; }
    return 0;
}

unsigned char *hn_zip_extract(const hn_zip *z, const hn_zip_entry *e,
                              size_t *out_len) {
    *out_len = 0;
    if (e->lfh_off + 30 > z->n) return NULL;
    const unsigned char *lfh = z->buf + e->lfh_off;
    if (rd_le32(lfh) != HN_ZIP_LFH_SIG) return NULL;
    /* 数据区 = 本地头 30 + 名字 + 额外字段(本地头的 extra 可能与中央
       目录不同, 以本地头自己的字段为准) */
    size_t name_len = rd_le16(lfh + 26), extra_len = rd_le16(lfh + 28);
    size_t data_off = e->lfh_off + 30 + name_len + extra_len;
    if (data_off + e->comp_size > z->n) return NULL;
    const unsigned char *data = z->buf + data_off;
    if (e->method == 0) {                           /* 存储 */
        unsigned char *out = (unsigned char *)malloc(e->comp_size + 1);
        if (!out) return NULL;
        memcpy(out, data, e->comp_size);
        out[e->comp_size] = 0;
        *out_len = e->comp_size;
        return out;
    }
    if (e->method != 8) return NULL;                /* 其它方法不支持 */
    unsigned char *out = hn_inflate(data, e->comp_size, out_len);
    if (out) {                                      /* 补 NUL 便于直接当字符串解析 */
        unsigned char *n2 = (unsigned char *)realloc(out, *out_len + 1);
        if (!n2) { free(out); *out_len = 0; return NULL; }
        out = n2;
        out[*out_len] = 0;
    }
    return out;
}

/* ---- 写(存储; CRC-32/IEEE 0xEDB88320 反射, ZIP 规范口径) ---- */

static unsigned zip_crc32(const unsigned char *d, size_t n) {
    unsigned c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= d[i];
        for (int k = 0; k < 8; k++)
            c = (c >> 1) ^ (0xEDB88320u & (0u - (c & 1)));
    }
    return c ^ 0xFFFFFFFFu;
}

/* DOS 时间戳写 0(1980-01-01): 胶囊是内容寻址的, 时间戳不参与语义,
   恒定值让同内容产出逐字节一致的包(可 diff 可复现)。 */

int hn_zip_write(FILE *f, const hn_zip_member *m, int n) {
    if (!f || n <= 0) return -1;
    unsigned char lf[30], cd[46], eocd[22];
    unsigned char *name_buf = NULL;
    size_t name_cap = 0;
    unsigned *offsets = (unsigned *)calloc((size_t)n, sizeof(unsigned));
    if (!offsets) return -1;
    long base = ftell(f);
    if (base < 0) { free(offsets); return -1; }

    for (int i = 0; i < n; i++) {
        size_t nl = strlen(m[i].name);
        if (nl + 1 > name_cap) {
            name_cap = nl + 1;
            unsigned char *nb = (unsigned char *)realloc(name_buf, name_cap);
            if (!nb) { free(name_buf); free(offsets); return -1; }
            name_buf = nb;
        }
        memcpy(name_buf, m[i].name, nl);
        unsigned crc = zip_crc32(m[i].data, m[i].n);
        offsets[i] = (unsigned)(ftell(f) - base);

        memset(lf, 0, sizeof(lf));
        wr_le32(lf + 0, HN_ZIP_LFH_SIG);
        wr_le16(lf + 4, 20);                    /* version needed = 2.0 */
        wr_le16(lf + 8, 0);                     /* method = 存储 */
        wr_le32(lf + 14, crc);
        wr_le32(lf + 18, (unsigned)m[i].n);     /* 压缩尺寸 */
        wr_le32(lf + 22, (unsigned)m[i].n);     /* 原始尺寸 */
        wr_le16(lf + 26, (unsigned)nl);
        fwrite(lf, 1, 30, f);
        fwrite(name_buf, 1, nl, f);
        if (m[i].n) fwrite(m[i].data, 1, m[i].n, f);
    }

    long cd_off = ftell(f) - base;
    for (int i = 0; i < n; i++) {
        size_t nl = strlen(m[i].name);
        memcpy(name_buf, m[i].name, nl);
        unsigned crc = zip_crc32(m[i].data, m[i].n);
        memset(cd, 0, sizeof(cd));
        wr_le32(cd + 0, HN_ZIP_CDIR_SIG);
        wr_le16(cd + 4, 20);                    /* 制作版本 = 2.0 */
        wr_le16(cd + 6, 20);                    /* 需要版本 = 2.0 */
        wr_le16(cd + 10, 0);                    /* method = 存储 */
        wr_le32(cd + 16, crc);                  /* APPNOTE: crc@16 */
        wr_le32(cd + 20, (unsigned)m[i].n);     /* 压缩尺寸@20 */
        wr_le32(cd + 24, (unsigned)m[i].n);     /* 原始尺寸@24 */
        wr_le16(cd + 28, (unsigned)nl);         /* 名字长度@28 */
        wr_le32(cd + 42, offsets[i]);
        fwrite(cd, 1, 46, f);
        fwrite(name_buf, 1, nl, f);
    }
    long cd_size = ftell(f) - base - cd_off;

    memset(eocd, 0, sizeof(eocd));
    wr_le32(eocd + 0, HN_ZIP_EOCD_SIG);
    wr_le16(eocd + 8, (unsigned)n);
    wr_le16(eocd + 10, (unsigned)n);
    wr_le32(eocd + 12, (unsigned)cd_size);
    wr_le32(eocd + 16, (unsigned)cd_off);
    fwrite(eocd, 1, 22, f);

    free(name_buf);
    free(offsets);
    return (fflush(f) == 0) ? 0 : -1;
}

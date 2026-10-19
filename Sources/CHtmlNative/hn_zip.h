/* hn_zip.h — 最小 ZIP 容器读写(零依赖, 单源)
 *
 * 读: EOCD → 中央目录 → 按需取条目; 解压复用 hn_png.c 的 hn_inflate
 * (ZIP method=8 与 PNG IDAT 同是 RFC 1951 裸 deflate)。hn_lottie 的
 * .lottie 直读与本文件共用这一份实现。
 * 写: 全部 method=0(存储, 不压缩)—— 胶囊/.hnapp 打包用, 体积不是
 * 首要目标, 正确性与零依赖才是; CRC32 按规范计算, 系统 unzip 可校验。
 * 从简不做(与用途无冲突): ZIP64、加密、分卷、数据描述符。
 */
#ifndef HN_ZIP_H
#define HN_ZIP_H

#include <stddef.h>
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    const char *name;        /* 指向包内字节(不 NUL 结尾, 用 name_len) */
    unsigned    name_len;
    unsigned    method;      /* 0=存储 8=deflate */
    unsigned    comp_size;
    unsigned    lfh_off;     /* 本地文件头在包内的偏移 */
} hn_zip_entry;

typedef struct {
    const unsigned char *buf;    /* 调用方的缓冲(条目名/数据都指向它) */
    size_t               n;
    hn_zip_entry        *ents;   /* malloc 数组, hn_zip_free 释放 */
    int                  n_ents;
} hn_zip;

/* 从内存包打开(解析中央目录)。1 = 成功(至少一个条目); 0 = 非 ZIP/损坏。 */
int  hn_zip_open(const unsigned char *buf, size_t n, hn_zip *z);
void hn_zip_free(hn_zip *z);

/* 按名字找条目(名字精确比对, 区分大小写)。1 = 命中, *out 填好。 */
int  hn_zip_find(const hn_zip *z, const char *name, hn_zip_entry **out);

/* 条目名比对(等价 hn_zip_find 的谓词形态, 供遍历方使用)。 */
int  hn_zip_name_is(const hn_zip_entry *e, const char *name);

/* 提取条目内容(malloc, 调用方 free; 末尾补 NUL 便于当字符串)。失败 NULL。 */
unsigned char *hn_zip_extract(const hn_zip *z, const hn_zip_entry *e,
                              size_t *out_len);

/* ---- 写入(全部存储; CRC32 内部计算) ---- */
typedef struct {
    const char        *name;   /* ZIP 内路径("index.html"/"assets/x.png") */
    const unsigned char *data;
    size_t             n;
} hn_zip_member;

/* 把 n 个成员写成 ZIP 到 FILE(顺序即中央目录顺序)。0 = 成功。 */
int hn_zip_write(FILE *f, const hn_zip_member *m, int n);

#ifdef __cplusplus
}
#endif
#endif /* HN_ZIP_H */

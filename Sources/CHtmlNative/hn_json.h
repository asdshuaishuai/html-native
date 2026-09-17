/* hn_json.h — 极简 JSON 解析(纯 C, 零依赖)
 *
 * 为什么自己写: Lottie(bodymovin) 与 Live2D 的 motion3/physics3/model3
 * 都是 JSON; 引入 cJSON 等外部库会破坏"引擎零依赖、纯 C99、三平台同源"的承诺。
 * 这里只需解析+遍历(不构造/不序列化), 约 300 行足够。
 *
 * 设计: 就地解析(在输入缓冲上写 \0), 节点独立分配(地址稳定),
 * 容器的子节点用指针数组表达 —— 嵌套容器因此不会互相干扰。
 * 不支持: JSON5 扩展、大整数精度(内部用 double)、注释以外的容错。
 */
#ifndef HN_JSON_H
#define HN_JSON_H

#include <stddef.h>

typedef enum {
    HN_JSON_NULL = 0, HN_JSON_BOOL, HN_JSON_NUM, HN_JSON_STR,
    HN_JSON_ARR, HN_JSON_OBJ
} hn_json_kind;

typedef struct hn_json hn_json;

struct hn_json {
    hn_json_kind kind;
    /* 值 */
    double      num;
    int         boolean;
    const char *str;        /* STR: 已就地解码(转义已处理) */
    /* 容器(ARR/OBJ): 子节点指针数组。用指针数组而非连续节点块,
       这样嵌套容器的子节点不会与父容器的子节点交错。 */
    hn_json   **items;
    int         count;
    const char *key;        /* 在父对象中的成员名(仅 OBJ 的子节点有) */
};

/* 解析 JSON 文本。buf 会被就地修改(字符串解码 / 数字截断为 \0)。
 * 成功返回根节点, 失败返回 NULL。
 * arena_out 为内部资源句柄, 用完调用 hn_json_free。 */
hn_json *hn_json_parse(char *buf, size_t len, void **arena_out);
void     hn_json_free(void *arena);

/* 便捷访问 */
hn_json *hn_json_obj_get(const hn_json *obj, const char *key);
double   hn_json_num(const hn_json *v, double dflt);
const char *hn_json_str(const hn_json *v, const char *dflt);
int      hn_json_bool(const hn_json *v, int dflt);
hn_json *hn_json_at(const hn_json *arr, int i);

#endif /* HN_JSON_H */

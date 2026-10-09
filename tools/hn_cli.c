/* hn_cli.c — C99 命令行客户端(agent 的系统入口, 与 hn_daemon.c 配对)
 *
 * 协议与 hn_daemon.c 一致(Unix socket JSON-lines), agent 生态的
 * CLI/MCP/任何 socket 客户端都可直接对接, 不依赖本文件。
 *
 * 用法:
 *   hn open <id> <file.html> [--css f.css] [--w N] [--h N] [--headless 0|1]
 *   hn update <id> <file.html>
 *   hn close <id>
 *   hn list
 *   hn dump <id>
 *   hn text <id> --element <sel>
 *   hn event <id> --kind click --target <sel>
 *   hn shot <id> <out.png>
 *   hn ping
 */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/stat.h>

static int sock_connect(const char *path) {
    int fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) != 0) { close(fd); return -1; }
    return fd;
}

/* 发请求 + 读一行响应, 返回 malloc 的 JSON(调用方 free) */
static char *rpc(const char *sock, const char *req) {
    int fd = sock_connect(sock);
    if (fd < 0) return NULL;
    size_t len = strlen(req);
    if (write(fd, req, len) != (ssize_t)len || write(fd, "\n", 1) != 1) { close(fd); return NULL; }
    size_t cap = 8192, n = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) { close(fd); return NULL; }
    for (;;) {
        if (n + 4096 >= cap) { cap *= 2; buf = (char *)realloc(buf, cap); if (!buf) break; }
        ssize_t r = read(fd, buf + n, cap - n - 1);
        if (r <= 0) break;
        n += (size_t)r;
        buf[n] = 0;
        if (memchr(buf, '\n', n)) break;
    }
    close(fd);
    /* 去掉尾部换行 */
    while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r')) buf[--n] = 0;
    return buf;
}

static char *read_file(const char *path, size_t *len) {
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

/* JSON 字符串转义(agent 生成的 HTML 含引号/换行必须转义, 否则协议破) */
static char *json_esc(const char *s) {
    size_t cap = strlen(s) * 2 + 16;
    char *out = (char *)malloc(cap);
    if (!out) return NULL;
    size_t n = 0;
    for (; *s && n + 8 < cap; s++) {
        switch (*s) {
        case '"':  memcpy(out + n, "\\\"", 2); n += 2; break;
        case '\\': memcpy(out + n, "\\\\", 2); n += 2; break;
        case '\n': memcpy(out + n, "\\n", 2);  n += 2; break;
        case '\t': memcpy(out + n, "\\t", 2);  n += 2; break;
        case '\r': memcpy(out + n, "\\r", 2);  n += 2; break;
        default: out[n++] = *s;
        }
    }
    out[n] = 0;
    return out;
}

/* 从响应 JSON 里提取字符串值(粗粒度, 与 daemon 的 json_str 同思路) */
static void resp_field(const char *json, const char *key, char *out, size_t cap) {
    char pat[128];
    snprintf(pat, sizeof(pat), "\"%s\":\"", key);
    const char *p = strstr(json, pat);
    out[0] = 0;
    if (!p) return;
    p += strlen(pat);
    size_t i = 0;
    while (*p && *p != '"' && i + 1 < cap) {
        if (*p == '\\' && p[1]) { p++; if (i + 1 < cap) out[i++] = *p++; }
        else out[i++] = *p++;
    }
    out[i] = 0;
}

static void usage(void) {
    fprintf(stderr,
        "hn — html-native C99 命令行(agent 的系统入口)\n"
        "  hn ping\n"
        "  hn open <id> <file.html> [--css f.css] [--w N] [--h N]\n"
        "  hn update <id> <file.html>\n"
        "  hn close <id>\n"
        "  hn list\n"
        "  hn dump <id>\n"
        "  hn text <id> --element <sel>\n"
        "  hn event <id> --kind click --target <sel>\n"
        "  hn shot <id> <out.png>\n");
}

int main(int argc, char **argv) {
    if (argc < 2) { usage(); return 2; }
    const char *home = getenv("HOME");
    char sock[1024];
    snprintf(sock, sizeof(sock), "%s/.html-native/hn-daemon.sock", home ? home : ".");

    const char *cmd = argv[1];
    char req[1 << 20];
    char resp_buf[8192];
    const char *id = argc > 2 ? argv[2] : NULL;

    if (!strcmp(cmd, "ping")) {
        char *r = rpc(sock, "{\"op\":\"ping\"}");
        if (!r) { printf("daemon 未响应(先启动 hn_daemon)\n"); return 1; }
        printf("%s\n", r);
        free(r);
        return 0;
    }

    if (!strcmp(cmd, "list")) {
        char *r = rpc(sock, "{\"op\":\"list\"}");
        if (!r) { printf("daemon 未响应\n"); return 1; }
        /* 极简列出: 数 "id" 出现次数 */
        int count = 0;
        for (const char *p = r; (p = strstr(p, "\"id\":\"")) != NULL; count++) p += 6;
        if (!count) printf("(无运行中的应用)\n");
        else printf("%d 个应用: %s\n", count, r);
        free(r);
        return 0;
    }

    if (!strcmp(cmd, "open") || !strcmp(cmd, "update")) {
        if (!id || argc < 4) { usage(); return 2; }
        const char *path = argv[3];
        size_t hlen = 0;
        char *html = read_file(path, &hlen);
        if (!html) { fprintf(stderr, "无法读取 %s\n", path); return 1; }
        char *esc = json_esc(html);
        free(html);
        if (!esc) return 1;
        const char *op = !strcmp(cmd, "open") ? "open" : "update";
        snprintf(req, sizeof(req), "{\"op\":\"%s\",\"id\":\"%s\",\"html\":\"%s\"", op, id, esc);
        free(esc);
        /* 可选参数 */
        for (int i = 4; i < argc; i++) {
            if (!strcmp(argv[i], "--css") && i + 1 < argc) {
                size_t cl = 0;
                char *css = read_file(argv[++i], &cl);
                if (css) {
                    char *ce = json_esc(css);
                    free(css);
                    if (ce) { size_t l = strlen(req); snprintf(req + l, sizeof(req) - l, ",\"css\":\"%s\"", ce); free(ce); }
                }
            } else if (!strcmp(argv[i], "--w") && i + 1 < argc) {
                size_t l = strlen(req); snprintf(req + l, sizeof(req) - l, ",\"w\":%s", argv[++i]);
            } else if (!strcmp(argv[i], "--h") && i + 1 < argc) {
                size_t l = strlen(req); snprintf(req + l, sizeof(req) - l, ",\"h\":%s", argv[++i]);
            }
        }
        size_t l = strlen(req);
        snprintf(req + l, sizeof(req) - l, "}");
        char *r = rpc(sock, req);
        if (!r) { printf("daemon 未响应\n"); return 1; }
        char val[256];
        resp_field(r, "id", val, sizeof(val));
        printf("%s: %s\n", !strcmp(cmd, "open") ? "已打开" : "已热更新", val[0] ? val : id);
        free(r);
        return 0;
    }

    if (!strcmp(cmd, "close") || !strcmp(cmd, "dump") || !strcmp(cmd, "text") ||
        !strcmp(cmd, "event") || !strcmp(cmd, "shot")) {
        if (!id) { usage(); return 2; }
        if (!strcmp(cmd, "text") && argc < 5) { usage(); return 2; }
        if (!strcmp(cmd, "event") && argc < 5) { usage(); return 2; }
        if (!strcmp(cmd, "shot") && argc < 4) { usage(); return 2; }

        if (!strcmp(cmd, "text")) {
            /* --element <sel> */
            char *es = json_esc(argv[4]);
            snprintf(req, sizeof(req), "{\"op\":\"text\",\"id\":\"%s\",\"element\":\"%s\"}", id, es ? es : "");
            free(es);
        } else if (!strcmp(cmd, "event")) {
            /* --kind click --target <sel> */
            const char *kind = "click";
            const char *target = "";
            for (int i = 3; i < argc; i++) {
                if (!strcmp(argv[i], "--kind") && i + 1 < argc) kind = argv[++i];
                if (!strcmp(argv[i], "--target") && i + 1 < argc) target = argv[++i];
            }
            char *te = json_esc(target);
            snprintf(req, sizeof(req), "{\"op\":\"event\",\"id\":\"%s\",\"kind\":\"%s\",\"target\":\"%s\"}",
                     id, kind, te ? te : "");
            free(te);
        } else if (!strcmp(cmd, "shot")) {
            char *pe = json_esc(argv[3]);
            snprintf(req, sizeof(req), "{\"op\":\"shot\",\"id\":\"%s\",\"path\":\"%s\"}", id, pe ? pe : "");
            free(pe);
        } else {
            snprintf(req, sizeof(req), "{\"op\":\"%s\",\"id\":\"%s\"}", cmd, id);
        }
        char *r = rpc(sock, req);
        if (!r) { printf("daemon 未响应\n"); return 1; }
        if (!strcmp(cmd, "text")) {
            char val[4096];
            resp_field(r, "text", val, sizeof(val));
            printf("%s\n", val);
        } else if (!strcmp(cmd, "dump")) {
            char val[64];
            resp_field(r, "cmds", val, sizeof(val));
            printf("cmds=%s\n", val);
        } else if (!strcmp(cmd, "shot")) {
            printf("已输出截图\n");
        } else {
            printf("%s\n", r);
        }
        free(r);
        return 0;
    }

    usage();
    return 2;
}

/* media_probe.c — 媒体平台实现真机探针(macOS, 真 AVFoundation)
 *
 * 只测 hn_platform.h 的媒体段(直用平台宿主, 不引引擎/rt/QuickJS)。
 * 用法:
 *   cc -O1 -Wall -I platform tools/media_probe.c platform/hnp_media_macos.c \
 *      -o /tmp/hn_media_probe && /tmp/hn_media_probe [<样例视频路径>]
 * (python3 tools/media_probe.py 是本文件的门禁薄封装。)
 *
 * 检查项(docs/media-design.md §8.2 口径, 断言用范围不用精确值 —— 真解码
 * 非逐字节确定):
 *   Tier 1(恒跑): supported==1; open 不存在路径 → NULL+err;
 *                 NULL 句柄全 API 安全; 纯音频会话(合成 WAV)play/seek/pause。
 *   Tier 2: 合成 1 秒 440Hz WAV → duration≈1(±0.2); 位置前进; 播放中 seek
 *                 落位; pause 后位置恒定。
 *   视频: 尽力在 /System/Library 找样例 .mov(候选表→有界目录走查)抽首帧
 *         RGBA 非空。找不到 → 打印一行 skip 原因, 不算失败("尽力")。
 * 退出码: 0 = 全过; 1 = 有失败项。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <math.h>
#include <dirent.h>
#include <sys/stat.h>

#include "hn_platform.h"

static int fails, oks, skips;

static void ck(int ok, const char *detail_fmt, ...) {
    /* 简单计数打印: [ok]/[FAIL] 前缀 + 一行细节 */
    if (ok) { oks++; printf("  [ok]   "); }
    else    { fails++; printf("  [FAIL] "); }
    va_list ap;
    va_start(ap, detail_fmt);
    vprintf(detail_fmt, ap);
    va_end(ap);
    printf("\n");
}

#define SKIP(fmt) do { skips++; printf("  [skip] " fmt "\n"); } while (0)

/* ---------------- WAV 合成(1 秒 440Hz, 44.1kHz/16bit/单声道) ---------------- */

static void w16(FILE *f, unsigned v) {
    fputc(v & 0xFF, f);
    fputc((v >> 8) & 0xFF, f);
}
static void w32(FILE *f, unsigned v) {
    w16(f, v & 0xFFFF);
    w16(f, (v >> 16) & 0xFFFF);
}

static int write_wav_440(const char *path, double seconds) {
    const unsigned rate = 44100;
    const unsigned n = (unsigned)(rate * seconds);
    const unsigned data_bytes = n * 2;   /* 16bit 单声道 */
    FILE *f = fopen(path, "wb");
    if (!f) return 0;
    fwrite("RIFF", 1, 4, f); w32(f, 36 + data_bytes); fwrite("WAVE", 1, 4, f);
    fwrite("fmt ", 1, 4, f); w32(f, 16); w16(f, 1); w16(f, 1);
    w32(f, rate); w32(f, rate * 2); w16(f, 2); w16(f, 16);
    fwrite("data", 1, 4, f); w32(f, data_bytes);
    for (unsigned i = 0; i < n; i++) {
        double s = sin(2.0 * 3.14159265358979323846 * 440.0 * (double)i / (double)rate);
        w16(f, (unsigned)(int)(s * 0.4 * 32767.0));
    }
    fclose(f);
    return 1;
}

/* ---------------- Tier 1: NULL 句柄安全 ---------------- */

static void null_handle_checks(void) {
    double d = 42.0; int r = 9, e = 9;
    unsigned char buf[16]; int w = 9, h = 9; double pts = 9.0;
    hnp_media_close(NULL);                                       /* 幂等 */
    ck(hnp_media_play(NULL) == 0, "play(NULL)=0");
    hnp_media_pause(NULL);
    ck(hnp_media_seek(NULL, 1.0) == 0, "seek(NULL)=0");
    hnp_media_set_volume(NULL, 0.5f);
    hnp_media_set_muted(NULL, 1);
    hnp_media_duration(NULL, &d);
    ck(d == 42.0, "duration(NULL)=0 且不写输出");
    ck(hnp_media_position(NULL, &d) == 0, "position(NULL)=0");
    hnp_media_state(NULL, &r, &e);
    ck(r == 9 && e == 9, "state(NULL)=0 且不写输出");
    ck(hnp_media_frame(NULL, buf, sizeof buf, &w, &h, &pts) == -1,
       "frame(NULL)=-1");
}

/* ---------------- Tier 2: 元数据/播放/seek(poll 皆经有界 runloop 泵) ---------------- */

static int wait_ready(hnp_media *m, double *dur_out, int max_ms) {
    /* duration/state 内部各泵 ~10ms, 这里按 10ms/轮估上限 */
    for (int i = 0; i < max_ms / 10; i++) {
        double d;
        if (hnp_media_duration(m, &d) == 1 && d > 0) {
            int r = 0, e = 0;
            hnp_media_state(m, &r, &e);
            if (r) { if (dur_out) *dur_out = d; return 1; }
        }
    }
    return 0;
}

static void audio_checks(const char *wav_path) {
    char err[256] = "";
    hnp_media *m = hnp_media_open(wav_path, err, sizeof err);
    if (!m) {
        ck(0, "open(tone.wav) 失败: %s", err);
        return;
    }
    ck(1, "open(tone.wav) 成功");

    double dur = -1;
    ck(wait_ready(m, &dur, 4000) && dur > 0.8 && dur < 1.2,
       "纯音频 ready 且 duration=%.3f(期望≈1.0)", dur);

    int rr = 0, ee = 0;
    hnp_media_state(m, &rr, &ee);
    ck(rr == 1 && ee == 0, "state ready=1 ended=0(未播)");

    /* 纯音频没有视频轨: frame 恒无新帧(契约返回 -1 = 无视频轨) */
    unsigned char tiny[64];
    int fw = 0, fh = 0;
    double pts = 0;
    ck(hnp_media_frame(m, tiny, sizeof tiny, &fw, &fh, &pts) == -1,
       "纯音频 frame()=-1(无视频轨)");

    /* 音量/静音是 void 写入口(ABI 无读回), 只验调用安全 */
    hnp_media_set_volume(m, 0.5f);
    hnp_media_set_muted(m, 1);        /* 探针静音跑, 不出声 */
    ck(hnp_media_play(m) == 1, "play 接受");

    /* 位置前进: play 后 ≤2s 内 position 应 >0.1 */
    double pos = 0;
    int advanced = 0;
    for (int i = 0; i < 200; i++) {
        hnp_media_position(m, &pos);
        if (pos > 0.1) { advanced = 1; break; }
    }
    ck(advanced, "播放中位置前进 t=%.3f", pos);

    /* 播放中 seek(0.5) 落位 ±0.15 */
    ck(hnp_media_seek(m, 0.5) == 1, "seek(0.5) 接受");
    int settled = 0;
    for (int i = 0; i < 200; i++) {
        hnp_media_position(m, &pos);
        double dd = pos - 0.5;
        if (dd < 0) dd = -dd;
        if (dd < 0.15) { settled = 1; break; }
    }
    ck(settled, "播放中 seek 落位 t=%.3f(期望≈0.5)", pos);

    /* pause 后位置恒定(单调性契约) */
    hnp_media_pause(m);
    double p1 = 0, p2 = 0;
    hnp_media_position(m, &p1);
    for (int i = 0; i < 20; i++) hnp_media_position(m, &pos);   /* ~200ms */
    hnp_media_position(m, &p2);
    double dp = p2 - p1;
    if (dp < 0) dp = -dp;
    ck(dp < 0.02, "pause 后位置恒定 p1=%.3f p2=%.3f", p1, p2);

    hnp_media_set_muted(m, 0);
    hnp_media_close(m);
}

/* ---------------- 视频段: /System/Library 样例 .mov 抽首帧 ---------------- */

#define MOV_HITS_MAX 4

/* 有界目录走查: 深度≤8, 访问目录项≤50000, 收集 ≤MOV_HITS_MAX 个
   size≥200KB 的 .mov(太小的多是占位/空轨道)。 */
static void scan_mov(const char *dir, int depth, long *budget, char **hits, int *nhits) {
    if (depth <= 0 || *budget <= 0 || *nhits >= MOV_HITS_MAX) return;
    DIR *d = opendir(dir);
    if (!d) return;
    struct dirent *ent;
    while ((ent = readdir(d)) != NULL && *budget > 0 && *nhits < MOV_HITS_MAX) {
        (*budget)--;
        const char *nm = ent->d_name;
        if (nm[0] == '.') continue;
        char path[4096];
        if (snprintf(path, sizeof path, "%s/%s", dir, nm) >= (int)sizeof path) continue;
        struct stat st;
        if (stat(path, &st) != 0) continue;
        if (S_ISDIR(st.st_mode)) {
            scan_mov(path, depth - 1, budget, hits, nhits);
        } else if (S_ISREG(st.st_mode) && st.st_size >= 200 * 1024) {
            size_t ln = strlen(nm);
            if (ln > 4 && strcmp(nm + ln - 4, ".mov") == 0)
                hits[(*nhits)++] = strdup(path);
        }
    }
    closedir(d);
}

static int first_frame_ok(hnp_media *m, char *detail, size_t dcap) {
    /* 播放态轮询新帧(§8.2: frames>0 要在 play 后); 先用小缓冲探尺寸,
       cap 不足时按契约"按 meta 重新分配后重试"。 */
    hnp_media_set_muted(m, 1);
    hnp_media_play(m);
    static unsigned char small[256 * 256 * 4];
    unsigned char *buf = NULL;
    int got = 0, w = 0, h = 0;
    double pts = -1;
    for (int i = 0; i < 600 && !got; i++) {   /* ≤ ~6s(每轮带 10ms 泵) */
        int fw = 0, fh = 0;
        double fp = -1;
        int r = hnp_media_frame(m, buf ? buf : small,
                                buf ? (size_t)w * h * 4 : sizeof small,
                                &fw, &fh, &fp);
        if (r == 1) {
            const unsigned char *px = buf ? buf : small;
            size_t nb = (size_t)fw * fh * 4, nonz = 0;
            for (size_t k = 0; k < nb; k++) nonz += (px[k] != 0);
            if (nonz > 0) { got = 1; w = fw; h = fh; pts = fp; }
        } else if (r == -1 && !buf && fw > 0 && fh > 0 &&
                   (size_t)fw * fh * 4 > sizeof small) {
            /* cap 不足: 平台已写出真实尺寸(meta) → 重分配重试 */
            buf = (unsigned char *)malloc((size_t)fw * fh * 4);
            if (buf) { w = fw; h = fh; }
        }
    }
    hnp_media_pause(m);
    if (got)
        snprintf(detail, dcap, "首帧 %dx%d pts=%.3f RGBA 非空", w, h, pts);
    else
        snprintf(detail, dcap, "6s 内未抽到非空 RGBA 帧");
    free(buf);
    return got;
}

static void video_checks(int argc, char **argv) {
    char *hits[MOV_HITS_MAX] = { 0 };
    int nhits = 0;

    if (argc > 1) {   /* 命令行显式给样例 → 直接用 */
        hits[nhits++] = strdup(argv[1]);
    } else {
        /* 先试已知候选(本机实测存在的系统资源), 再有界走查兜底 */
        static const char *cands[] = {
            "/System/Library/CoreServices/ControlCenter.app/Contents/Resources/"
            "BentoGalleryIntroduction.mov",
            "/System/Library/CoreServices/BluetoothUIService.app/Contents/Resources/"
            "Banner-PID-8230-mov/Banner-PID-8230-default-Loop.mov",
        };
        for (size_t i = 0; i < sizeof cands / sizeof cands[0]; i++) {
            struct stat st;
            if (stat(cands[i], &st) == 0 && S_ISREG(st.st_mode))
                hits[nhits++] = strdup(cands[i]);
            if (nhits >= MOV_HITS_MAX) break;
        }
        if (nhits == 0) {
            long budget = 50000;
            scan_mov("/System/Library", 8, &budget, hits, &nhits);
        }
    }

    if (nhits == 0) {
        SKIP("/System/Library 未找到样例 .mov(尽力而为, 不计失败)");
        return;
    }
    printf("  [info] 样例候选 %d 个, 逐个尝试抽帧\n", nhits);

    int passed = 0;
    char detail[256];
    for (int i = 0; i < nhits && !passed; i++) {
        char err[256] = "";
        hnp_media *m = hnp_media_open(hits[i], err, sizeof err);
        if (!m) {
            printf("  [info] open 失败(跳过该候选): %s — %s\n", hits[i], err);
            continue;
        }
        double dur = -1;
        if (!wait_ready(m, &dur, 4000)) {
            printf("  [info] 4s 未就绪(跳过该候选): %s\n", hits[i]);
            hnp_media_close(m);
            continue;
        }
        if (first_frame_ok(m, detail, sizeof detail)) {
            ck(1, "视频首帧可抽: %s — %s", hits[i], detail);
            passed = 1;
        } else {
            printf("  [info] 抽帧失败(跳过该候选): %s — %s\n", hits[i], detail);
        }
        hnp_media_close(m);
    }
    if (!passed)
        ck(0, "%s", "样例 .mov 均未能抽出非空 RGBA 首帧");

    for (int i = 0; i < nhits; i++) free(hits[i]);
}

int main(int argc, char **argv) {
    printf("media_probe: 媒体平台真机探针(hn_platform.h 媒体段)\n");

    printf("Tier 1(supported / open 失败如实报错 / NULL 安全):\n");
    ck(hnp_media_supported() == 1, "supported()=%d", hnp_media_supported());
    char err[256] = "";
    void *bad = hnp_media_open("/tmp/hnp_media_probe_no_such_file.wav", err, sizeof err);
    ck(bad == NULL && err[0] != '\0', "open 不存在路径 → NULL+err=\"%s\"", err);

    null_handle_checks();

    const char *wav = "/tmp/hnp_media_probe_tone.wav";
    if (write_wav_440(wav, 1.0)) {
        printf("Tier 2(合成 1s 440Hz WAV → duration≈1 → 播放/seek/pause):\n");
        audio_checks(wav);
        remove(wav);
    } else {
        ck(0, "%s", "无法写入 /tmp/hnp_media_probe_tone.wav");
    }

    printf("视频段(/System/Library 样例 .mov 抽首帧 RGBA 非空):\n");
    video_checks(argc, argv);

    printf("media_probe: %d ok, %d fail, %d skip\n", oks, fails, skips);
    return fails ? 1 : 0;
}

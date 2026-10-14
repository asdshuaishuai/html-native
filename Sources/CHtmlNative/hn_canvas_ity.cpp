// hn_canvas_ity.cpp — canvas 后端之 canvas_ity 实现(ISC, 单头 vendored)
// ================================================================
// 消费"已解析的账本 op"(hn_internal.h 的 hn_canvas_op), 绘制进
// canvas_ity::canvas(梯形 AA + Gamma 校正混合的系统级质量 2D 光栅),
// 帧完成以非预乘 RGBA8 输出(与 HN_CMD_BITMAP 契约零转换)。
//
// v1 支持: FILL_RECT(含阴影)/STROKE_PATH/FILL_PATH/GRAD_RECT/GRAD_PATH
//          /CLIP_PUSH/CLIP_POP/CLEAR(整幅透明)。
// v1 暂不支持(遇到打一次 stderr 并跳过, 不静默刷屏): TEXT(可经 set_font
//          + TTF 字节补齐)/IMAGE/PDATA/PATTERN_RECT/PATH/CLEAR_RECT
//          (source-over 无擦除语义)/CLIP_PATH(子路径组)/blend≠source-over。
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

extern "C" {
#include "hn_canvas_backend.h"
#include "hn_internal.h"
}
#define CANVAS_ITY_IMPLEMENTATION
#include "canvas_ity.hpp"

namespace {

struct ity_backend : hn_canvas_backend {
    canvas_ity::canvas cv;
    unsigned char *frame;
    bool warned[8];
    explicit ity_backend(int w, int h)
        : cv(w, h), frame(nullptr) {
        vt = &VT;
        width = w; height = h;
        for (auto &f : warned) f = false;
    }
    static const hn_canvas_backend_vt VT;
};

constexpr int W_WARN = (int)(sizeof(((ity_backend *)nullptr)->warned) /
                            sizeof(((ity_backend *)nullptr)->warned[0]));
void warn_once(bool *warned, int slot, const char *msg) {
    if (slot >= 0 && slot < W_WARN) {
        if (warned[slot]) return;
        warned[slot] = true;
    }
    fprintf(stderr, "[canvas-ity] 暂不支持: %s(该 op 已跳过)\n", msg);
}

hn_color unpack_color(unsigned int v) { return (hn_color)v; }

void fill_brush(canvas_ity::canvas &cv, const hn_canvas_op *o) {
    hn_color c = o->color;
    cv.set_color(canvas_ity::fill_style, (float)((c >> 16) & 255) / 255.0f,
                 (float)((c >> 8) & 255) / 255.0f,
                 (float)(c & 255) / 255.0f, (float)((c >> 24) & 255) / 255.0f);
    if (o->n_stops >= 2 && o->stops) {
        for (int i = 0; i < o->n_stops; i++) {
            hn_grad_stop st = o->stops[i];
            unsigned col = (unsigned)st.c;
            cv.add_color_stop(canvas_ity::fill_style, st.t, (float)((col >> 16) & 255) / 255.0f,
                              (float)((col >> 8) & 255) / 255.0f,
                              (float)(col & 255) / 255.0f,
                              (float)((col >> 24) & 255) / 255.0f);
        }
    }
}

void stroke_brush(canvas_ity::canvas &cv, const hn_canvas_op *o) {
    hn_color c = o->color;
    cv.set_color(canvas_ity::stroke_style, (float)((c >> 16) & 255) / 255.0f,
                 (float)((c >> 8) & 255) / 255.0f,
                 (float)(c & 255) / 255.0f, (float)((c >> 24) & 255) / 255.0f);
    cv.set_line_width(o->stroke_w);
}

void path_points(canvas_ity::canvas &cv, const float *pts, int n, bool closed) {
    cv.begin_path();
    if (n > 0) {
        cv.move_to(pts[0], pts[1]);
        for (int i = 1; i < n; i++) cv.line_to(pts[i * 2], pts[i * 2 + 1]);
    }
    if (closed) cv.close_path();
}

void apply_gradient(canvas_ity::canvas &cv, const hn_canvas_op *o, bool fill) {
    canvas_ity::brush_type t = fill ? canvas_ity::fill_style : canvas_ity::stroke_style;
    if (o->grad_radial)
        cv.set_radial_gradient(canvas_ity::stroke_style, o->gx0, o->gy0, o->gr0, o->gx1, o->gy1, o->gr1);
    else
        cv.set_linear_gradient(canvas_ity::stroke_style, o->gx0, o->gy0, o->gx1, o->gy1);
    hn_color c0 = o->color, c1 = o->color2;
    if (!(o->n_stops >= 2 && o->stops)) {
        cv.add_color_stop(t, 0.0f, (float)((c0 >> 16) & 255) / 255.0f,
                          (float)((c0 >> 8) & 255) / 255.0f,
                          (float)(c0 & 255) / 255.0f,
                          (float)((c0 >> 24) & 255) / 255.0f);
        cv.add_color_stop(t, 1.0f, (float)((c1 >> 16) & 255) / 255.0f,
                          (float)((c1 >> 8) & 255) / 255.0f,
                          (float)(c1 & 255) / 255.0f,
                          (float)((c1 >> 24) & 255) / 255.0f);
    } else {
        for (int i = 0; i < o->n_stops; i++) {
            hn_grad_stop st = o->stops[i];
            unsigned col = (unsigned)st.c;
            cv.add_color_stop(t, st.t, (float)((col >> 16) & 255) / 255.0f,
                              (float)((col >> 8) & 255) / 255.0f,
                              (float)(col & 255) / 255.0f,
                              (float)((col >> 24) & 255) / 255.0f);
        }
    }
}

int ity_resize(hn_canvas_backend *self, int w, int h) {
    auto *b = (ity_backend *)self;
    b->~ity_backend();
    new (b) ity_backend(w, h);
    b->width = w; b->height = h;
    return 1;
}

void ity_op(hn_canvas_backend *self, const hn_canvas_op *o) {
    auto *b = (ity_backend *)self;
    canvas_ity::canvas &cv = b->cv;
    switch (o->kind) {
    case HN_CV_FILL_RECT: {
        hn_color c = o->color;
        cv.set_color(canvas_ity::fill_style, (float)((c >> 16) & 255) / 255.0f,
                     (float)((c >> 8) & 255) / 255.0f,
                     (float)(c & 255) / 255.0f, (float)((c >> 24) & 255) / 255.0f);
        if (o->shadow) {
            hn_color sc = o->sh_color;
            cv.set_shadow_color((float)((sc >> 16) & 255) / 255.0f,
                                (float)((sc >> 8) & 255) / 255.0f,
                                (float)(sc & 255) / 255.0f,
                                (float)((sc >> 24) & 255) / 255.0f);
            cv.set_shadow_blur(o->sh_blur);
        } else {
            cv.set_shadow_color(0.f, 0.f, 0.f, 0.f);
        }
        cv.fill_rectangle(o->x, o->y, o->w, o->h);
        break;
    }
    case HN_CV_STROKE_PATH:
        path_points(cv, o->pts, o->n_pts, false);
        stroke_brush(cv, o);
        cv.stroke();
        break;
    case HN_CV_FILL_PATH:
        path_points(cv, o->pts, o->n_pts, true);
        fill_brush(cv, o);
        cv.fill();
        break;
    case HN_CV_GRAD_RECT:
        apply_gradient(cv, o, true);
        cv.fill_rectangle(o->x, o->y, o->w, o->h);
        break;
    case HN_CV_GRAD_PATH:
        path_points(cv, o->pts, o->n_pts, true);
        apply_gradient(cv, o, true);
        cv.fill();
        break;
    case HN_CV_CLIP_PUSH:
        cv.save();
        cv.begin_path();
        cv.rectangle(o->x, o->y, o->x + o->w, o->y + o->h);
        cv.clip();
        break;
    case HN_CV_CLIP_POP:
        cv.restore();
        break;
    default: {
        int slot;
        switch (o->kind) {
        case HN_CV_TEXT:         slot = 0; break;
        case HN_CV_IMAGE:        slot = 1; break;
        case HN_CV_PDATA:        slot = 2; break;
        case HN_CV_PATTERN_RECT:
        case HN_CV_PATTERN_PATH: slot = 3; break;
        case HN_CV_CLEAR_RECT:   slot = 4; break;
        case HN_CV_CLIP_PATH:    slot = 5; break;
        default:                 slot = 6; break;
        }
        warn_once(b->warned, slot, "该 op 见文件头 v1 清单");
        break;
    }
    }
}

int ity_frame(hn_canvas_backend *self, const unsigned char **rgba,
              int *width, int *height, int *stride) {
    auto *b = (ity_backend *)self;
    size_t n = (size_t)b->width * (size_t)b->height * 4;
    free(b->frame);
    b->frame = (unsigned char *)malloc(n);
    if (!b->frame) return 0;
    b->cv.get_image_data(b->frame, b->width, b->height, b->width * 4, 0, 0);
    *rgba = b->frame;
    *width = b->width;
    *height = b->height;
    *stride = b->width * 4;
    return 1;
}

void ity_dispose(hn_canvas_backend *self) {
    auto *b = (ity_backend *)self;
    free(b->frame);
    b->~ity_backend();
    free(b);
}

const hn_canvas_backend_vt ity_backend::VT = {
    ity_resize, ity_op, ity_frame, ity_dispose,
};

} // namespace

hn_canvas_backend *hn_canvas_backend_ity_create(int width, int height) {
    if (width <= 0 || height <= 0) return NULL;
    auto *b = new ity_backend(width, height);
    return (hn_canvas_backend *)b;
}

/* ================= paint 分支接线 =================
 * HN_CANVAS_BACKEND=ity 时启用; 按画布 id 缓存后端(尺寸变化重建),
 * 重放账本全部 op → get_image_data 出帧 → HN_CMD_BITMAP(盒映射缩放)。
 * 任何一步失败返回 0, 调用方回落账本重放(页面不坏)。 */
static int ity_enabled(void) {
    static int v = -1;
    if (v < 0) {
        const char *e = getenv("HN_CANVAS_BACKEND");
        v = (e && !strcmp(e, "ity")) ? 1 : 0;
    }
    return v;
}

#define ITY_IDS 8
struct ity_slot { char id[128]; hn_canvas_backend *b; int w, h; };
static ity_slot g_ity_slots[ITY_IDS];

int hn_canvas_paint_ity(hn_context *c, const char *id, const hn_canvas_ledger *l,
                        float cw, float ch, float bw, float bh,
                        float sx, float sy, float alpha) {
    if (!c || !id || !l || l->n_ops <= 0) return 0;
    int W = (int)cw, H = (int)ch;
    if (W <= 0 || H <= 0) return 0;

    ity_slot *slot = NULL;
    for (int i = 0; i < ITY_IDS; i++)
        if (g_ity_slots[i].id[0] && !strcmp(g_ity_slots[i].id, id)) { slot = &g_ity_slots[i]; break; }
    if (slot && (slot->w != W || slot->h != H)) {
        slot->b->vt->dispose(slot->b);
        slot->b = NULL;
        slot->id[0] = 0;
        slot = NULL;
    }
    if (!slot) {
        for (int i = 0; i < ITY_IDS; i++)
            if (!g_ity_slots[i].id[0]) { slot = &g_ity_slots[i]; break; }
        if (!slot) { slot = &g_ity_slots[0];
            slot->b->vt->dispose(slot->b); slot->id[0] = 0; }
        hn_canvas_backend *b = hn_canvas_backend_ity_create(W, H);
        if (!b) return 0;
        slot->b = b; slot->w = W; slot->h = H;
        snprintf(slot->id, sizeof(slot->id), "%s", id);
    }

    for (int i = 0; i < l->n_ops; i++)
        slot->b->vt->op(slot->b, &l->ops[i]);

    const unsigned char *px = NULL;
    int fw = 0, fh = 0, fstride = 0;
    if (!slot->b->vt->frame(slot->b, &px, &fw, &fh, &fstride)) {
        slot->id[0] = 0;   /* 帧失败: 弃后端, 下次重建 */
        return 0;
    }

    hn_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = HN_CMD_BITMAP;
    cmd.bitmap = px;
    cmd.bitmap_stride = fstride;
    cmd.bitmap_w = fw; cmd.bitmap_h = fh;
    cmd.x = sx; cmd.y = sy; cmd.w = bw; cmd.h = bh;
    cmd.fill = 0xFFFFFF00u | (unsigned)(alpha * 255.0f);
    hn_paint_push(c, &cmd);
    return 1;
}

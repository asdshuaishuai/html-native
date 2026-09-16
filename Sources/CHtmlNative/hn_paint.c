/* hn_paint.c — DOM → 绘制指令列表(文档序深度优先, 父先子后)
 * 滚动: overflow 容器把子树绘制偏移 (scroll_x, scroll_y) 并用
 * CLIP_PUSH/CLIP_POP 裁剪到自身盒内; 偏移沿树下累积。
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "hn_internal.h"

static hn_color mul_alpha(hn_color col, float a) {
    unsigned al = (unsigned)(col & 0xFFu);
    al = (unsigned)((float)al * a);
    if (al > 255) al = 255;
    return (col & 0xFFFFFF00u) | al;
}

/* 动画 4 分量(rgba 0..1) → 打包色 */
static hn_color anim_color(const float v[4]) {
    int r = (int)(v[0] * 255.0f + 0.5f), g = (int)(v[1] * 255.0f + 0.5f);
    int b = (int)(v[2] * 255.0f + 0.5f), a = (int)(v[3] * 255.0f + 0.5f);
    if (r < 0) r = 0; if (r > 255) r = 255;
    if (g < 0) g = 0; if (g > 255) g = 255;
    if (b < 0) b = 0; if (b > 255) b = 255;
    if (a < 0) a = 0; if (a > 255) a = 255;
    return ((hn_color)r << 24) | ((hn_color)g << 16) | ((hn_color)b << 8) | (hn_color)a;
}

static void push_cmd(hn_context *c, hn_cmd *cmd);

/* 有效圆角: % 值按盒短边解析(border-radius:50% 即圆) */
static float eff_radius(const hn_style *st, float w, float h) {
    if (!st->radius_pct || st->radius <= 0) return st->radius;
    float m = w < h ? w : h;
    return m * st->radius / 100.0f;
}

/* 文本装饰线: underline / line-through / overline(随文本基线定位) */
static void push_deco(hn_context *c, const hn_style *st, float x, float baseline,
                      float w, float alpha, float sx, float sy) {
    if (!st->text_deco || w <= 0) return;
    float ys[3]; int ny = 0;
    if (st->text_deco & 1) ys[ny++] = baseline + st->font_size * 0.14f;
    if (st->text_deco & 2) ys[ny++] = baseline - st->font_size * 0.30f;
    if (st->text_deco & 4) ys[ny++] = baseline - st->font_size * 0.92f;
    hn_color col = mul_alpha(st->color, alpha);
    for (int i = 0; i < ny; i++) {
        hn_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.kind = HN_CMD_RECT;
        cmd.x = x - sx;
        cmd.y = ys[i] - sy;
        cmd.w = w;
        cmd.h = st->font_size > 14 ? 1.5f : 1.0f;
        cmd.fill = col;
        push_cmd(c, &cmd);
    }
}

/* 列表标记: ul → 实心圆/方/空心圆, ol → 十进制序号(右对齐于内容左缘) */
static void paint_marker(hn_context *c, hn_node *n, const hn_style *st,
                         float alpha, float sx, float sy) {
    hn_color col = mul_alpha(st->color, alpha);
    int ordered = !strcmp(n->parent->tag, "ol");
    if (ordered) {
        int idx = 1;
        for (hn_node *s = n->prev; s; s = s->prev)
            if (s->kind == HN_ELEM && s->tag && !strcmp(s->tag, "li")) idx++;
        char *num = hn_arena_alloc(c->tmp, 12);   /* 存活至下次 layout, 与指令列表同生命周期 */
        if (!num) return;
        snprintf(num, 12, "%d.", idx);
        hn_font_desc fd = { st->font_size, st->font_weight, st->font_italic, st->letter_spacing, st->font_family };
        float w = 0;
        if (c->tb && c->tb->measure) w = c->tb->measure(c->tb->ctx, &fd, num, strlen(num));
        hn_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.kind = HN_CMD_TEXT;
        cmd.text = num;
        cmd.text_len = strlen(num);
        cmd.tx = n->bx - 7 - w - sx;
        cmd.baseline = n->by + st->font_size * 1.02f - sy;
        cmd.font.size_px = st->font_size;
        cmd.font.weight = st->font_weight;
        cmd.font.italic = st->font_italic;
        cmd.font.letter_spacing = st->letter_spacing;
        cmd.fill = col;
        push_cmd(c, &cmd);
        return;
    }
    /* 无序标记: 0=disc(默认) 2=square 3=circle(空心) */
    hn_cmd cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.kind = HN_CMD_RECT;
    cmd.w = cmd.h = st->list_style == 2 ? 5 : 5;
    cmd.x = n->bx - 14 - sx;
    cmd.y = n->by + st->font_size * 0.45f - sy;
    if (st->list_style == 3) {          /* 空心圆 */
        cmd.radius = 2.5f;
        cmd.stroke = col;
        cmd.stroke_w = 1;
    } else if (st->list_style == 2) {   /* 方块 */
        cmd.fill = col;
    } else {                            /* 实心圆 */
        cmd.radius = 2.5f;
        cmd.fill = col;
    }
    push_cmd(c, &cmd);
}

/* 绘制某节点的全部行内片段(文本节点用父样式, 元素用自身样式) */
static void paint_runs(hn_context *c, hn_node *n, const hn_style *st,
                       float alpha, float sx, float sy) {
    for (int i = 0; i < n->n_runs; i++) {
        hn_run *r = &n->runs[i];
        if (r->end <= r->begin) continue;
        hn_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.kind = HN_CMD_TEXT;
        cmd.text = n->text + r->begin;
        cmd.text_len = r->end - r->begin;
        cmd.tx = r->x - sx;
        cmd.baseline = r->baseline - sy;
        cmd.font.size_px = st->font_size;
        cmd.font.weight = st->font_weight;
        cmd.font.italic = st->font_italic;
        cmd.font.letter_spacing = st->letter_spacing;
        cmd.fill = mul_alpha(st->color, alpha);
        push_cmd(c, &cmd);
        push_deco(c, st, r->x, r->baseline, r->width, alpha, sx, sy);
    }
}

static void push_cmd(hn_context *c, hn_cmd *cmd) {
    if (c->n_cmds == c->cap_cmds) {
        int nc = c->cap_cmds ? c->cap_cmds * 2 : 64;
        hn_cmd *nv = realloc(c->cmds, sizeof(hn_cmd) * (size_t)nc);
        if (!nv) abort();
        c->cmds = nv;
        c->cap_cmds = nc;
    }
    c->cmds[c->n_cmds++] = *cmd;
}

static void paint_walk(hn_context *c, hn_node *n, const hn_style *pst,
                       float alpha, float sx, float sy) {
    if (alpha <= 0.001f) return;

    if (n->kind == HN_TEXT) {
        paint_runs(c, n, pst, alpha, sx, sy);
        return;
    }

    if (n->style.display == HN_DISP_NONE) return;
    const hn_style *st = &n->style;

    /* 列表标记: li 且父为 ul/ol(背景之上、内容之左) */
    if (n->tag && !strcmp(n->tag, "li") && n->parent && n->parent->tag
        && st->list_style != 1
        && (!strcmp(n->parent->tag, "ul") || !strcmp(n->parent->tag, "ol"))) {
        paint_marker(c, n, st, alpha, sx, sy);
    }

    /* 图片元素 */
    if (n->tag && !strcmp(n->tag, "img")) {
        const char *src = hn_node_attr(n, "src");
        if (src) {
            hn_cmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.kind = HN_CMD_IMAGE;
            cmd.x = n->bx - sx; cmd.y = n->by - sy;
            cmd.w = n->bw; cmd.h = n->bh;
            cmd.radius = eff_radius(st, cmd.w, cmd.h);
            cmd.text = src;
            cmd.text_len = strlen(src);
            push_cmd(c, &cmd);
        }
        return;
    }

    /* 输入控件: 盒(背景/边框/圆角) + 值文本 + 插入符 */
    if (hn_node_is_input(n)) {
        int f = (st->background & 0xFFu) || st->has_gradient;
        int b = st->border_w > 0 && (st->border_color & 0xFFu);
        if (f || b) {
            hn_cmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.kind = HN_CMD_RECT;
            cmd.x = n->bx - sx; cmd.y = n->by - sy; cmd.w = n->bw; cmd.h = n->bh;
            cmd.radius = eff_radius(st, cmd.w, cmd.h);
            cmd.fill = mul_alpha(st->background, alpha);
            cmd.stroke = mul_alpha(st->border_color, alpha);
            cmd.stroke_w = st->border_w;
            if (st->has_shadow) {
                cmd.shadow = 1;
                cmd.shadow_color = mul_alpha(st->sh_color, alpha);
                cmd.shadow_blur = st->sh_blur;
                cmd.shadow_ox = st->sh_ox;
                cmd.shadow_oy = st->sh_oy;
            }
            push_cmd(c, &cmd);
        }
        size_t vlen = 0;
        const char *val = hn_node_value(n, &vlen);
        int is_ph = (!val || vlen == 0);
        if (is_ph) {
            const char *ph = hn_node_attr(n, "placeholder");
            if (ph && *ph) { val = ph; vlen = strlen(ph); }
        }
        /* 值文本用 run(由 layout_input 产出); placeholder 无 run 时现场测量绘制 */
        if (!is_ph && n->n_runs > 0) {
            for (int i = 0; i < n->n_runs; i++) {
                hn_run *r = &n->runs[i];
                if (r->end <= r->begin || (size_t)r->end > vlen) continue;
                hn_cmd cmd;
                memset(&cmd, 0, sizeof(cmd));
                cmd.kind = HN_CMD_TEXT;
                cmd.text = val + r->begin;
                cmd.text_len = r->end - r->begin;
                cmd.tx = r->x - sx;
                cmd.baseline = r->baseline - sy;
                cmd.font.size_px = st->font_size;
                cmd.font.weight = st->font_weight;
                cmd.font.italic = st->font_italic;
                cmd.font.letter_spacing = st->letter_spacing;
                cmd.fill = mul_alpha(st->color, alpha);
                push_cmd(c, &cmd);
            }
        } else if (is_ph && val && vlen) {
            hn_cmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.kind = HN_CMD_TEXT;
            cmd.text = val;
            cmd.text_len = vlen;
            float a = st->font_size * 0.8f, d = st->font_size * 0.2f, l = 0;
            if (c->tb && c->tb->metrics) {
                hn_font_desc fd = { st->font_size, st->font_weight, st->font_italic, st->letter_spacing, st->font_family };
                c->tb->metrics(c->tb->ctx, &fd, &a, &d, &l);
            }
            cmd.tx = n->bx + st->padding[3] - sx;
            cmd.baseline = n->by + st->padding[0] + (st->font_size * st->line_height - (a + d)) * 0.5f + a - sy;
            cmd.font.size_px = st->font_size;
            cmd.font.weight = st->font_weight;
            cmd.font.italic = st->font_italic;
            cmd.font.letter_spacing = st->letter_spacing;
            hn_color dim = st->color;
            cmd.fill = mul_alpha(dim, alpha * 0.45f);
            push_cmd(c, &cmd);
        }
        /* 插入符: 聚焦且有值状态时(x/y 取 caret 所在 run 的行位置) */
        if (c->focus_node == n && c->caret_on) {
            float cx = n->bx + st->padding[3] - sx;
            float cy = n->by + st->padding[0] - sy + 1;
            if (n->n_runs > 0) {
                /* 找到 caret 所在 run, 累加其前段宽度; 多行值跟随所在行盒 */
                for (int i = 0; i < n->n_runs; i++) {
                    hn_run *r = &n->runs[i];
                    if ((int)r->begin <= n->caret && (int)r->end >= n->caret) {
                        float frac = (r->end > r->begin)
                            ? (float)(n->caret - (int)r->begin) / (float)(r->end - r->begin) : 0;
                        cx = r->x + r->width * frac - sx;
                        if (r->h > 0) cy = r->y_top - sy + 1;
                        break;
                    }
                }
            }
            hn_cmd cc;
            memset(&cc, 0, sizeof(cc));
            cc.kind = HN_CMD_RECT;
            cc.x = cx;
            cc.y = cy;
            cc.w = 1.5f;
            cc.h = st->font_size * 1.15f;
            cc.fill = mul_alpha(st->color, alpha);
            push_cmd(c, &cc);
        }
        return;
    }

    /* 行内元素: 先画自身盒(背景/边框), 再画自己的文本片段 */
    if (st->display == HN_DISP_INLINE && n->n_runs > 0) {
        int f = (st->background & 0xFFu) || st->has_gradient;
        int b = st->border_w > 0 && (st->border_color & 0xFFu);
        if (f || b) {
            hn_cmd cmd;
            memset(&cmd, 0, sizeof(cmd));
            cmd.kind = HN_CMD_RECT;
            cmd.x = n->bx - sx; cmd.y = n->by - sy; cmd.w = n->bw; cmd.h = n->bh;
            cmd.radius = st->radius;
            cmd.fill = mul_alpha(st->background, alpha);
            cmd.stroke = mul_alpha(st->border_color, alpha);
            cmd.stroke_w = st->border_w;
            push_cmd(c, &cmd);
        }
        paint_runs(c, n, st, alpha, sx, sy);
        return;
    }

    int has_fill = (st->background & 0xFFu) || st->has_gradient;
    int has_border = st->border_w > 0 && (st->border_color & 0xFFu);
    if (has_fill || has_border) {
        hn_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.kind = HN_CMD_RECT;
        cmd.x = n->bx - sx; cmd.y = n->by - sy; cmd.w = n->bw; cmd.h = n->bh;
        cmd.radius = eff_radius(st, cmd.w, cmd.h);
        cmd.fill = mul_alpha(st->background, alpha);
        cmd.stroke = mul_alpha(st->border_color, alpha);
        cmd.stroke_w = st->border_w;
        if (st->has_gradient) {
            cmd.gradient = 1;
            cmd.grad_from = mul_alpha(st->grad_from, alpha);
            cmd.grad_to = mul_alpha(st->grad_to, alpha);
            cmd.grad_angle = st->grad_angle;
        }
        if (st->has_shadow) {
            cmd.shadow = 1;
            cmd.shadow_color = mul_alpha(st->sh_color, alpha);
            cmd.shadow_blur = st->sh_blur;
            cmd.shadow_ox = st->sh_ox;
            cmd.shadow_oy = st->sh_oy;
        }
        push_cmd(c, &cmd);
    }

    /* overflow 容器: 裁剪 + 子树滚动偏移 */
    int clipped = st->overflow != 0;
    if (clipped) {
        hn_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.kind = HN_CMD_CLIP_PUSH;
        cmd.x = n->bx - sx; cmd.y = n->by - sy;
        cmd.w = n->bw; cmd.h = n->bh;
        cmd.radius = eff_radius(st, cmd.w, cmd.h);
        push_cmd(c, &cmd);
    }
    float csx = sx + n->scroll_x, csy = sy + n->scroll_y;

    float child_alpha = alpha * st->opacity;
    for (hn_node *ch = n->first; ch; ch = ch->next)
        paint_walk(c, ch, st, child_alpha, csx, csy);

    if (clipped) {
        hn_cmd cmd;
        memset(&cmd, 0, sizeof(cmd));
        cmd.kind = HN_CMD_CLIP_POP;
        push_cmd(c, &cmd);

        /* 滚动指示条(仅已滚动时出现) */
        float max_s = n->content_h - n->bh;
        if (max_s > 0.001f && n->scroll_y > 0.001f) {
            float track_h = n->bh - 4;
            float thumb_h = n->bh * n->bh / n->content_h;
            if (thumb_h < 24) thumb_h = 24;
            if (thumb_h > track_h) thumb_h = track_h;
            float ty = n->by - sy + 2 + (n->scroll_y / max_s) * (track_h - thumb_h);
            hn_cmd sb;
            memset(&sb, 0, sizeof(sb));
            sb.kind = HN_CMD_RECT;
            sb.x = n->bx - sx + n->bw - 5;
            sb.y = ty;
            sb.w = 3;
            sb.h = thumb_h;
            sb.radius = 1.5f;
            sb.fill = 0xFFFFFF55;
            push_cmd(c, &sb);
        }
    }
}

void hn_paint_root(hn_context *c) {
    c->n_cmds = 0;
    if (c->doc) paint_walk(c, c->doc->root, NULL, 1.0f, 0, 0);
}

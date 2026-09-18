/* hncairo_probe.c — cairo 绘制后端的断言探针
 *
 * 为什么存在: 绘制后端从"每个平台手写一份"换成"一个跨平台框架"之后, 必须
 * 证明这个框架真的能画出显示列表里的每一类指令 —— 包括 hnsoft 此前做不到的
 * 两件事: **圆角裁剪**和**真实模糊阴影**。少了这一层, "换框架了"只是听起来
 * 变好, 实际可能只是把 bug 换了张脸。
 *
 * 用法: cc -O2 hncairo_probe.c hn_cairo.c hn_png.c $(pkg-config --cflags --libs cairo freetype2) -lm
 * 全部断言通过时退出码 0。
 *
 * 探针自身踩过的坑(都写在这里, 免得下次重踩):
 *  - 断言"角落 alpha==0"在**实色画布**上是错的: 被切掉露出来的是底色, 不是透明。
 *  - 渐变采样点必须贴轴的两端; 取矩形中部会落在渐变中段, 颜色不再是端色。
 *  - 抗锯齿要在**斜边**上找: 竖直边恰好落在像素边界时没有部分覆盖。
 *  - 显示列表的 poly 是**一条**扁平折线(给 Lottie 形状层), 塞"两个矩形"进去
 *    造的是自交 8 边形而不是两个子路径。验填充规则要用 {5/2} 自交星形。
 */
#include "hn_cairo.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <cairo.h>
#include <math.h>

static int fails = 0;
static void ck(int c, const char *l) { printf("%s %s\n", c ? "  v" : "  X", l); if (!c) fails++; }

static unsigned char *P; static int PW, PH;
static unsigned px(int x, int y) {
    const unsigned char *p = P + ((size_t)y * PW + x) * 4;
    return ((unsigned)p[3] << 24) | ((unsigned)p[0] << 16) | ((unsigned)p[1] << 8) | p[2];
}

/* 输入底色与期望像素是两个值: hn_color 是 #RRGGBBAA 打包, 而 px() 读回的是
   #AARRGGBB。混用会得到"近乎透明的偏红底色", 所有露底色断言假失败。 */
#define BG_IN   0x0B0E13FFu
#define BG_PX   0xFF0B0E13u

/* 渲染一组指令并读像素。bg 用 hn_color(#RRGGBBAA)口径。 */
static unsigned char *draw(hn_cmd *cmds, int n, hn_color bg) {
    free(P);
    hn_display_list dl = { cmds, n };
    P = hncairo_render(&dl, PW, PH, bg);
    return P;
}

int main(void) {
    PW = 240; PH = 200;
    hn_cmd cmds[16]; int n = 0;
    memset(cmds, 0, sizeof(cmds));

    printf("== 显示列表 → cairo ==\n");

    /* 1) 圆角矩形 + 实色 */
    cmds[n] = (hn_cmd){0}; cmds[n].kind = HN_CMD_RECT;
    cmds[n].x = 10; cmds[n].y = 10; cmds[n].w = 80; cmds[n].h = 60; cmds[n].radius = 16;
    cmds[n].fill = 0x3366FFFFu; n++;

    /* 2) 渐变矩形: grad_angle=90deg(向右)蓝→红 */
    cmds[n] = (hn_cmd){0}; cmds[n].kind = HN_CMD_RECT;
    cmds[n].x = 110; cmds[n].y = 10; cmds[n].w = 100; cmds[n].h = 60;
    cmds[n].gradient = 1; cmds[n].grad_angle = 90;
    cmds[n].grad_from = 0x0000FFFFu; cmds[n].grad_to = 0xFF0000FFu; n++;

    /* 3) 阴影矩形(blur 12)—— hnsoft 只有"6 层扩边"近似, 没有真模糊 */
    cmds[n] = (hn_cmd){0}; cmds[n].kind = HN_CMD_RECT;
    cmds[n].x = 40; cmds[n].y = 100; cmds[n].w = 90; cmds[n].h = 50; cmds[n].radius = 10;
    cmds[n].fill = 0xFFFFFFFFu;
    cmds[n].shadow = 1; cmds[n].shadow_color = 0x000000C8u;
    cmds[n].shadow_blur = 12; cmds[n].shadow_oy = 6; n++;

    /* 4) 圆角裁剪 + 内部大色块: 角落必须被切掉(hnsoft 只能做轴对齐矩形裁剪) */
    cmds[n] = (hn_cmd){0}; cmds[n].kind = HN_CMD_CLIP_PUSH;
    cmds[n].x = 150; cmds[n].y = 90; cmds[n].w = 70; cmds[n].h = 70; cmds[n].radius = 20; n++;
    cmds[n] = (hn_cmd){0}; cmds[n].kind = HN_CMD_RECT;
    cmds[n].x = 140; cmds[n].y = 80; cmds[n].w = 200; cmds[n].h = 200;
    cmds[n].fill = 0x00FF00FFu; n++;
    cmds[n] = (hn_cmd){0}; cmds[n].kind = HN_CMD_CLIP_POP; n++;

    /* 5) 四边形(3D 投影后的面片) */
    cmds[n] = (hn_cmd){0}; cmds[n].kind = HN_CMD_QUAD;
    cmds[n].qx[0] = 10; cmds[n].qy[0] = 180;
    cmds[n].qx[1] = 80; cmds[n].qy[1] = 180;
    cmds[n].qx[2] = 66; cmds[n].qy[2] = 199;
    cmds[n].qx[3] = 24; cmds[n].qy[3] = 199;
    cmds[n].fill = 0xFFFF00FFu; n++;

    if (!draw(cmds, n, BG_IN)) { printf("X hncairo_render 返回 NULL\n"); return 1; }

    ck(px(50, 40) == 0xFF3366FFu, "圆角矩形: 中心为 #3366FF");
    /* 实色画布上"被切掉"表现为等于底色, 不是 alpha=0 */
    ck(px(11, 11) == BG_PX, "圆角矩形: 角落被圆角切掉(露出底色)");

    /* 渐变: 采样点贴轴两端 */
    unsigned gl = px(115, 40), gr = px(205, 40);
    ck((gl & 0xFF) > 200 && ((gl >> 16) & 0xFF) < 60, "渐变 90deg: 左端为蓝");
    ck(((gr >> 16) & 0xFF) > 200 && (gr & 0xFF) < 60, "渐变 90deg: 右端为红");
    printf("       (左=%08X 右=%08X)\n", gl, gr);

    /* 阴影: 实色底上 alpha 恒 255, 要判**明度渐变** */
    int darkened = 0, midlum = 0;
    for (int y = 152; y < 176; y++) {
        unsigned c = px(85, y);
        int lum = (int)((c >> 16) & 0xFF) + (int)((c >> 8) & 0xFF) + (int)(c & 0xFF);
        if (lum < (0x0B + 0x0E + 0x13) - 12) darkened++;
        if (lum > 12 && lum < (0x0B + 0x0E + 0x13) - 12) midlum++;
    }
    ck(darkened >= 3, "阴影: 矩形下方被压暗(投影生效)");
    ck(midlum >= 2, "阴影: 边缘是连续明度渐变(真模糊, 非硬边)");
    printf("       (%d 行被压暗, %d 行处于中间明度)\n", darkened, midlum);

    /* 圆角裁剪 */
    ck(px(185, 125) == 0xFF00FF00u, "圆角裁剪: 区域内被填绿");
    ck(px(151, 91) == BG_PX, "圆角裁剪: 圆角处被切掉(hnsoft 只能做矩形裁剪)");

    /* 四边形 */
    ck(px(45, 190) == 0xFFFFFF00u, "四边形: 内部为黄");
    ck(px(45, 178) == BG_PX, "四边形: 顶点外不受影响");

    /* 关键回归: 指令之间不能互相污染。
       cairo_new_sub_path 是"追加子路径", 不清空路径的话, 后一条指令的 fill
       会把前面所有形状一起涂掉 —— 症状是"第二个矩形一出现, 第一个就变色"。 */
    ck(px(50, 40) == 0xFF3366FFu, "回归: 后续指令不污染先画的形状(路径不累积)");
    ck(px(160, 40) != 0xFFFFFFFFu, "回归: 阴影矩形的白色不溢出到别处");

    /* 抗锯齿: 实色画布上 alpha 恒为 255, 所以"部分覆盖"表现为**颜色介于
       纯黄与底色之间**, 而不是中间 alpha。沿四边形的斜边走一遍找这种像素。
       竖直边不能用来判 —— 它恰好落在像素边界上时覆盖是 0% 或 100%。 */
    int aa = 0;
    for (int i = 0; i < 60; i++) {
        double t = (double)i / 60.0;
        int sx = (int)lround(10 + 14 * t), sy = (int)lround(180 + 19 * t);
        if (sx < 1 || sx >= PW - 1 || sy < 1 || sy >= PH - 1) continue;
        unsigned c = px(sx, sy);
        if (c == 0xFFFFFF00u || c == BG_PX) continue;   /* 纯黄或纯底色 */
        aa++;
    }
    ck(aa > 0, "抗锯齿: 斜边上存在中间色(非硬边)");

    /* PNG 往返(cairo 自带真 zlib 压缩) */
    size_t plen = 0;
    unsigned char *png = hncairo_encode_png(P, PW, PH, &plen);
    ck(png && plen > 100, "PNG 编码: 产出非空");
    if (png) {
        const char *path = "/tmp/hncairo_probe.png";
        FILE *f = fopen(path, "wb");
        if (f) { fwrite(png, 1, plen, f); fclose(f); }
        printf("       (PNG %zu 字节, %dx%d → %s)\n", plen, PW, PH, path);
        free(png);
    }

    /* 多边形填充规则: {5/2} 自交星形。even-odd 下中心五边形是洞,
       非零环绕下是实心 —— 这是证明规则真的被切换的唯一办法。
       (显示列表的 poly 是一条扁平折线, 不是多个子路径; 用"两个矩形"验
        even-odd 造的是自交 8 边形, 那条意外的边会被误判成 bug。) */
    printf("== 多边形填充规则({5/2} 自交星形) ==\n");
    double cx = 120, cy = 120, R = 60;
    for (int eo = 0; eo < 2; eo++) {
        double ang[5];
        for (int i = 0; i < 5; i++) ang[i] = (-90.0 + i * 72.0) * M_PI / 180.0;
        int order[5] = { 0, 2, 4, 1, 3 };
        static float sp[10];
        for (int i = 0; i < 5; i++) {
            sp[i*2]   = (float)(cx + R * cos(ang[order[i]]));
            sp[i*2+1] = (float)(cy + R * sin(ang[order[i]]));
        }
        hn_cmd one = (hn_cmd){0};
        one.kind = HN_CMD_POLYGON; one.poly = sp; one.poly_n = 5;
        one.even_odd = (unsigned char)eo; one.fill = 0xFF00FFFFu;
        if (!draw(&one, 1, BG_IN)) { ck(0, "多边形: 渲染失败"); continue; }
        if (eo) {
            ck(px(120, 85) == 0xFFFF00FFu, "even-odd: 星臂被填");
            ck(px(120, 120) == BG_PX, "even-odd: 中心五边形是洞(露底色)");
        } else {
            ck(px(120, 85) == 0xFFFF00FFu, "非零环绕: 星臂被填");
            ck(px(120, 120) == 0xFFFF00FFu, "非零环绕: 同一条路径中心变实心");
        }
    }

    /* 透明背板: 逐像素 alpha 必须真的可用 —— 这是"桌面小组件/透明卡片"的
       前提。底色给全透明, 只有圆角矩形本体应该是不透明的。 */
    printf("== 透明背板 ==\n");
    {
        hn_cmd one = (hn_cmd){0};
        one.kind = HN_CMD_RECT; one.x = 10; one.y = 10;
        one.w = 80; one.h = 80; one.radius = 20; one.fill = 0xFF0000FFu;
        PW = 100; PH = 100;
        if (draw(&one, 1, 0x00000000u)) {
            int tr = 0, op = 0, pa = 0, tot = PW * PH;
            for (int y = 0; y < PH; y++)
                for (int x = 0; x < PW; x++) {
                    const unsigned char *q = P + ((size_t)y * PW + x) * 4;
                    if (q[3] == 0) tr++;
                    else if (q[3] == 255) op++;
                    else pa++;
                }
            printf("       (全透明 %.1f%% / 不透明 %.1f%% / 半透明 %.1f%%)\n",
                   tr * 100.0 / tot, op * 100.0 / tot, pa * 100.0 / tot);
            ck(tr > 100, "透明背板: 圆角之外完全透明(真逐像素 alpha)");
            ck(op > 5000, "透明背板: 矩形本体不透明");
            ck(pa > 0, "透明背板: 圆角边缘有半透明过渡");
        } else { ck(0, "透明背板: 渲染失败"); }
    }

    /* MESH: UV 贴图(Live2D 类网格变形)。用 cairo 现造一张 64x64 四色源图,
       验证逐三角形的 UV 仿射真的把源图对到了屏幕上。
       两个采样上的坑(都踩过):
         - mesh_verts 是**行主序**(index = row*(cols+1)+col → TL,TR,BL,BR)。
           按 TL,TR,BR,BL 给会让实现的 quad 组合拿到错位顶点。
         - 采样点必须躲开三角形接缝。1x1 网格的对角线恰好穿过源图四个象限的
           中心, 落在缝上的点会被两个各自 clip+各自仿射的三角形各覆盖一部分,
           出来是混色。所以取象限内偏离对角线的点。
         - 源图不能太小(2x2): UV=0 映射到贴图**边缘**, 双线性过滤会把外部
           透明拉进来 —— 那是标准行为, 不是实现问题。 */
    printf("== MESH UV 贴图 ==\n");
    {
        const int SW = 64, SH = 64;
        const char *pngpath = "/tmp/hncairo_probe_mesh.png";
        cairo_surface_t *src = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, SW, SH);
        cairo_t *sc = cairo_create(src);
        cairo_set_source_rgba(sc, 1, 0, 0, 1); cairo_rectangle(sc, 0, 0, SW/2, SH/2); cairo_fill(sc);
        cairo_set_source_rgba(sc, 0, 1, 0, 1); cairo_rectangle(sc, SW/2, 0, SW/2, SH/2); cairo_fill(sc);
        cairo_set_source_rgba(sc, 0, 0, 1, 1); cairo_rectangle(sc, 0, SH/2, SW/2, SH/2); cairo_fill(sc);
        cairo_set_source_rgba(sc, 1, 1, 0, 1); cairo_rectangle(sc, SW/2, SH/2, SW/2, SH/2); cairo_fill(sc);
        cairo_surface_write_to_png(src, pngpath);
        cairo_destroy(sc); cairo_surface_destroy(src);

        PW = 200; PH = 200;
        /* 行主序: TL, TR, BL, BR */
        float verts[8] = { 40, 40, 160, 40, 40, 160, 160, 160 };
        float uv[8]    = { 0, 0,   1, 0,   0, 1,    1, 1 };
        hn_cmd one = (hn_cmd){0};
        one.kind = HN_CMD_MESH; one.mesh_verts = verts; one.mesh_uv = uv;
        one.mesh_cols = 1; one.mesh_rows = 1; one.text = pngpath;
        if (draw(&one, 1, BG_IN)) {
            int red[2] = {55,85}, grn[2] = {145,55}, blu[2] = {55,145}, yel[2] = {130,115};
            ck(px(red[0], red[1]) == 0xFFFF0000u, "MESH: UV≈(0.1,0.4) 取到红");
            ck(px(grn[0], grn[1]) == 0xFF00FF00u, "MESH: UV≈(0.9,0.1) 取到绿");
            ck(px(blu[0], blu[1]) == 0xFF0000FFu, "MESH: UV≈(0.1,0.9) 取到蓝");
            ck(px(yel[0], yel[1]) == 0xFFFFFF00u, "MESH: UV≈(0.75,0.6) 取到黄");
            ck(px(20, 20) == BG_PX, "MESH: 网格之外不受影响");
        } else { ck(0, "MESH: 渲染失败"); }
    }

    printf("== %s (%d 项失败) ==\n", fails ? "失败" : "全部通过", fails);
    free(P);
    return fails ? 1 : 0;
}

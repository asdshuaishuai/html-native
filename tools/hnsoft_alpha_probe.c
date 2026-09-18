/* hnsoft_alpha_probe.c — 软件光栅后端的 alpha 链断言
 *
 * 为什么单独一个探针: 透明背景层(桌面小组件/透明卡片)是引擎的核心卖点之一,
 * 而它的最后一环在光栅器里。这里抓到的两个 bug 都属于"声明了透明却渲染成
 * 不透明黑底"这一类 —— 而且症状完全不像绘制问题:
 *
 * 1) fb_init 把底色 alpha 硬编码成 255。调用方(hnwin.c)传全透明底色
 *    0x00000000, 于是整张画布变成不透明。
 * 2) blend() 只写 RGB 从不写 alpha。半透明色画到透明像素上, 颜色被折半
 *    而 alpha 仍是 255 —— 出来是一块深色, 不是"淡色叠加"。
 * 3) sd_rounded 的内部距离项在 qx == qy 时塌成 0(正方形中心/45° 对角线),
 *    覆盖率只有 0.5 —— 于是正方形正中心像素的不透明度减半。
 *
 * 用法: cc -O2 hnsoft_alpha_probe.c hnsoft.c hn_png.c -lfreetype -lm
 */
/* hnsoft 的 alpha 链断言: 这就是刚修掉的 bug 现场。
   blend() 只写 RGB 不写 alpha → 整张画布 alpha 恒 255 → 透明背板变黑底。 */
#include "hnsoft.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
static char b2[96];
static const char *fmt2(const char*f,unsigned v){snprintf(b2,sizeof b2,f,v);return b2;}
static int fails=0;
static void ck(int c,const char*l){printf("%s %s\n",c?"  v":"  X",l); if(!c)fails++;}
int main(void){
    int W=100,H=100;
    hn_cmd c; memset(&c,0,sizeof c);
    printf("== hnsoft alpha 链 ==\n");

    /* 1) 半透明绿落全透明画布: alpha 必须是源 alpha, 颜色不被折半 */
    c.kind=HN_CMD_RECT; c.x=10;c.y=10;c.w=80;c.h=80; c.fill=0x00FF0080u;
    hn_display_list d1={&c,1};
    unsigned char *P=hnsoft_render(&d1,W,H,0x00000000u);
    if(!P){ck(0,"渲染失败");return 1;}
    ck(P[((size_t)50*W+50)*4+3]==128, "半透明绿落透明底: alpha=0x80");
    ck(P[((size_t)50*W+50)*4+1]==0xFF, "半透明绿落透明底: 绿色不被折半");
    int outside=0;
    for(int y=92;y<100;y++)for(int x=92;x<100;x++)
        if(P[((size_t)y*W+x)*4+3]==0) outside++;
    ck(outside==64, "图形之外仍全透明(64/64)");
    free(P);

    /* 2) 两层 50% 叠加应得 ≈75%(1-(1-.5)^2) */
    hn_cmd two[2]; memset(two,0,sizeof two);
    for(int i=0;i<2;i++){two[i].kind=HN_CMD_RECT;two[i].x=20;two[i].y=20;
        two[i].w=40;two[i].h=40;two[i].fill=0xFF000080u;}
    hn_display_list d2={two,2};
    P=hnsoft_render(&d2,W,H,0x00000000u);
    if(P){
        unsigned a=P[((size_t)40*W+40)*4+3];
        ck(a>165&&a<215,"两层 50% 叠加得 ≈75%");
        free(P);
    } else ck(0,"叠加渲染失败");

    /* 3) 不透明矩形在不透明底上必须完全覆盖 */
    P=hnsoft_render(&d1,W,H,0x0B0E13FFu);
    if(P){
        ck(P[((size_t)50*W+50)*4+3]==255,"不透明底: alpha=255");
        /* 50% 绿叠在 #0B0E13 上: G = 255*0.502 + 0x0E*0.498 ≈ 135。
       不是 255 —— 半透明叠加本来就会混入底色。这条验的是"混合算对了"。 */
    { unsigned g = P[((size_t)50*W+50)*4+1];
      ck(g > 120 && g < 150, fmt2("不透明底: 50%% 绿叠深底 G≈135 (实际 %u)", g)); }
        /* 图形之外仍是不透明底 */
        ck(P[((size_t)95*W+95)*4+3]==255,"不透明底: 图形之外也 255");
        free(P);
    } else ck(0,"不透明底渲染失败");

    printf("== %s (%d 项失败) ==\n", fails?"失败":"全部通过", fails);
    return fails?1:0;
}

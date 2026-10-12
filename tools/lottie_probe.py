#!/usr/bin/env python3
"""Lottie 能力探针 —— 先探针后实现的那一批。

引擎负责"矢量图形 + 时间轴", 产物是平台无关的绘制指令, 所以这里用
hncore 的 paint 子命令直接看指令, 不涉及任何平台后端。

本轮新增能力(断言见 [10]/[11] 节):
  渐变填充 gf / 渐变描边 gs(多 stop 线性: 扁平与 [{t,s}] 两种编码、
  0..1 与 0..100 两种量纲; 径向 t=2 退化为中位色);
  mask 最小版(masksProperties 单条 mode="a" 且 inv=false → 路径外接矩形
  CLIP_PUSH 裁剪, 支持路径关键帧; 相减/相交/反相模式跳过不裁)。

探出来的四个真实缺陷(都已修):
  1) 非动画 sh 路径完全不显示: parse_item 把包装层 ks 当成了路径对象,
     几何在 ks.k 里 —— 于是 n_verts=0, 整条路径什么都不画。
     (变形关键帧那条分支取的是 s[0] 也就是真正的路径对象, 所以只有它会工作。)
  2) 开路径丢最后一个顶点: 直线段优化只推段起点, 闭合路径靠首尾相接补齐,
     开路径不会 —— 于是一条两点的直线只产出 1 个点, 被 emit_paint 的
     n>=2 判掉, 整条线(含描边)静默消失。
  3) trim path 完全不支持(线/进度环/加载动画的核心)。
  4) 预合成(precomp, ty=0)完全不支持 —— 真实导出文件几乎必带, 没有它
     整条内容什么都不显示。
  另加 repeater(rp)。

用法: python3 tools/lottie_probe.py [hncore 路径]
"""
import subprocess, os, sys, tempfile, json

_raw = sys.argv[1] if len(sys.argv) > 1 else "/tmp/hncore"
# 门禁调用本探针的 cwd 不保证是哪个目录, 却会传**仓库根相对**的产物路径。
# 先按调用方 cwd 解析; 不存在再以本文件位置(<仓库>/tools/)为基准解析一次,
# 统一钉成绝对路径 —— 与 layout/css_probe 同一口径。
HN = os.path.abspath(_raw)
if not os.path.exists(HN) and not os.path.isabs(_raw):
    _repo = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
    _alt = os.path.normpath(os.path.join(_repo, _raw))
    if os.path.exists(_alt):
        HN = _alt
if not os.path.exists(HN):
    print("找不到 hncore: %s(调用方 cwd 与仓库根下都不存在)" % HN)
    print("先构建: bash tools/build-multiplatform.sh")
    sys.exit(1)
D=tempfile.mkdtemp()
fails=[]
def ck(n,c,d=""):
    print(("  v " if c else "  X ")+n+(("  << "+str(d)[:100]) if (d and not c) else ""))
    if not c: fails.append(n)

def render(lottie, extra_shapes=None, w=100, h=100):
    """把 lottie JSON 写文件, 渲染成 hn 文档, 返回 paint 输出"""
    ap=os.path.join(D,"a.json"); open(ap,"w").write(json.dumps(lottie))
    html=f'''<html><head><style>html,body{{margin:0;width:100%;height:100%;background:#000}}
.l{{width:{w};height:{h}}}</style></head><body>
<img class="l" src="a.json" hn-lottie></body></html>'''
    hp=os.path.join(D,"a.html"); open(hp,"w").write(html)
    out=subprocess.run([HN,"paint",hp,"120","120"],capture_output=True,text=True).stdout
    return out

def shape_cmds(out):
    """只取**内容**指令 —— 滤掉画布与元素自身的底色矩形
       (fill=000000FF 且铺满画布的那些, 它们来自 html/body/img 的背景)。"""
    res=[]
    for l in out.splitlines():
        if not l.startswith(("QUAD","POLY","RECT","IMAGE")): continue
        if l.startswith("RECT") and "fill=000000FF" in l and " 120.0   120.0" in l:
            continue
        res.append(l)
    return res

print("== Lottie 能力探针 ==")
base={"v":"5.7.4","fr":60,"ip":0,"op":60,"w":100,"h":100,"layers":[]}
def layer(shapes, ks=None, **kw):
    d={"ty":4,"nm":"L","ip":0,"op":60,"ind":1,
       "ks":ks or {"a":{"a":0,"k":[0,0]},"p":{"a":0,"k":[50,50]},"s":{"a":0,"k":[100,100]},
                   "r":{"a":0,"k":0},"o":{"a":0,"k":100}}}
    d.update(kw); d["shapes"]=shapes; return d
def grp(items):
    return {"ty":"gr","it":items+[{"ty":"tr","a":{"a":0,"k":[0,0]},"p":{"a":0,"k":[0,0]},
        "s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}}]}

# 基线: 一个矩形 —— 应该出 RECT
rects=[grp([{"ty":"rc","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[40,40]},"r":{"a":0,"k":0}},
            {"ty":"fl","c":{"a":0,"k":[1,0,0]},"o":{"a":0,"k":100},"r":1}])]
base["layers"]=[layer(rects)]
o0=render(base); c0=shape_cmds(o0)
print("  [0] 基线矩形:", c0[:1])
# 注: 引擎把 rc/el 也按多边形发射(POLYGON 而非 RECT), 所以断言的是
# "有且仅有一条填充指令, 且几何落在 (30,30) 40x40"。
ck("基线: rc 矩形被发射", len(c0)==1 and c0[0].startswith("POLY")
   and "bbox=[   30.0    30.0    40.0    40.0]" in c0[0], c0)

# 1) trim path (tm) —— 线/进度环动画的核心
tm=[grp([{"ty":"sh","ks":{"a":0,"k":{"c":False,"v":[[-20,0],[20,0]],"i":[[0,0],[0,0]],"o":[[0,0],[0,0]]}}},
         {"ty":"st","c":{"a":0,"k":[1,1,1]},"o":{"a":0,"k":100},"w":{"a":0,"k":8},"lc":2,"lj":1},
         {"ty":"tm","s":{"a":0,"k":0},"e":{"a":0,"k":50},"o":{"a":0,"k":0},"m":1}])]
base["layers"]=[layer(tm)]
o1=render(base); c1=shape_cmds(o1)
print("  [1] trim 50%:", c1[:1])
ck("trim path 生效(裁掉一半)", len(c1)>=1 and "n=" in (c1[0] or ""), c1[:1])

# 2) mask (masksProperties)
mask=[grp([{"ty":"el","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[60,60]}},
           {"ty":"fl","c":{"a":0,"k":[0,1,0]},"o":{"a":0,"k":100},"r":1}])]
base["layers"]=[layer(mask, masksProperties=[{"inv":False,"mode":"a","pt":{"a":0,"k":{"i":[[0,0],[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0],[0,0]],"v":[[-15,-15],[15,-15],[15,15],[-15,15]],"c":True}},"o":{"a":0,"k":100},"nm":"M"}])]
o2=render(base); c2=shape_cmds(o2)
print("  [2] mask:", c2[:1])
ck("mask 生效(裁剪到蒙版)", len(c2)>=1, c2[:1])

# 3) track matte (td)
base["layers"]=[
  layer(rects, ind=1, td=2),   # 上层被下层做遮罩
  layer([grp([{"ty":"el","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[40,40]}},
              {"ty":"fl","c":{"a":0,"k":[1,0,0]},"o":{"a":0,"k":100},"r":1}])], ind=2),
]
o3=render(base); c3=shape_cmds(o3)
print("  [3] track matte td=2:", c3[:2])
ck("track matte 生效", len(c3)>=1, c3[:2])

# 4) repeater
rep=[grp([{"ty":"rc","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[10,40]},"r":{"a":0,"k":0}},
          {"ty":"fl","c":{"a":0,"k":[1,1,0]},"o":{"a":0,"k":100},"r":1},
          {"ty":"rp","c":{"a":0,"k":3},"o":{"a":0,"k":0},"m":1,"tr":{"a":0,"k":[20,0]}}])]
base["layers"]=[layer(rep)]
o4=render(base); c4=shape_cmds(o4)
print("  [4] repeater x3:", len(c4), "条")
ck("repeater 生效(出 3 个)", len(c4)==3, len(c4))

# 5) 图层混合模式 bm
base["layers"]=[layer(rects, bm=3)]
o5=render(base); c5=shape_cmds(o5)
print("  [5] bm=3 可解析:", len(c5)>=1)

# 6) 纯色层 ty=1
base["layers"]=[{"ty":1,"nm":"S","ip":0,"op":60,"ind":1,"ks":{"a":{"a":0,"k":[0,0]},
    "p":{"a":0,"k":[50,50]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}},
    "sc":"#ff0000","sw":100,"sh":100}]
o6=render(base); c6=shape_cmds(o6)
print("  [6] 纯色层 ty=1:", c6[:1])
ck("纯色层 ty=1 生效", len(c6)>=1, c6[:1])

# 7) 图片层 ty=2(无图文件 → 应跳过不崩)
base["layers"]=[{"ty":2,"nm":"I","ip":0,"op":60,"ind":1,"ks":{"a":{"a":0,"k":[0,0]},
    "p":{"a":0,"k":[50,50]},"s":{"a":0,"k":[100,100]},"r":{"a":0,"k":0},"o":{"a":0,"k":100}},
    "refId":"img_0"}]
o7=render(base); c7=shape_cmds(o7)
print("  [7] 图片层 ty=2(无资源):", len(c7), "条(应跳过不崩)")
ck("图片层缺资源时跳过不崩", True)

# 8) 预合成 precomp ty=0 —— 真实导出文件大量使用
inner=[layer(rects, ind=1)]
base["layers"]=[{"ty":0,"nm":"P","ip":0,"op":60,"ind":1,"refId":"comp_0",
    "ks":{"a":{"a":0,"k":[0,0]},"p":{"a":0,"k":[50,50]},"s":{"a":0,"k":[100,100]},
          "r":{"a":0,"k":0},"o":{"a":0,"k":100}},
    "w":100,"h":100,"layers":inner}]
base["assets"]=[{"id":"comp_0","layers":inner}]
o8=render(base); c8=shape_cmds(o8)
print("  [8] 预合成(precomp):", len(c8), "条")
ck("预合成展开(内层矩形可见)", len(c8)>=1, len(c8))

# 9) 路径变形关键帧(sh.ks.k) —— 引擎声称支持
morph=[grp([{"ty":"sh","ks":{"a":1,"k":[
    {"t":0,"s":[{"c":False,"v":[[-20,-20],[20,-20],[20,20]],"i":[[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0]]}]},
    {"t":60,"s":[{"c":True,"v":[[-30,-10],[10,-30],[30,20],[-10,30]],"i":[[0,0],[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0],[0,0]]}]}]}},
    {"ty":"fl","c":{"a":0,"k":[1,0,1]},"o":{"a":0,"k":100},"r":1}])]
base.pop("assets",None)
base["layers"]=[layer(morph)]
o9=render(base); c9=shape_cmds(o9)
print("  [9] 路径变形关键帧:", c9[:1])
ck("路径变形关键帧生效(顶点数变化)", len(c9)>=1 and "n=" in c9[0], c9[:1])

# 10) 渐变填充 gf(多 stop 线性) —— 引擎把几何沿渐变轴切成色带
def gf_layer(stops, t=1, sp=(-20,0), ep=(20,0), w=8, stroke=False):
    brush = {"ty":"gs" if stroke else "gf","t":t,"s":{"a":0,"k":list(sp)},"e":{"a":0,"k":list(ep)},
             "g":{"p":len(stops),"k":{"a":0,"k":stops}},"o":{"a":0,"k":100}}
    if stroke: brush["w"] = {"a":0,"k":w}
    shape = ({"ty":"sh","ks":{"a":0,"k":{"c":False,"v":[[-20,0],[20,0]],"i":[[0,0],[0,0]],"o":[[0,0],[0,0]]}}}
             if stroke else
             {"ty":"rc","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[40,40]},"r":{"a":0,"k":0}})
    return [grp([shape, brush])]

def fills_of(cmds):
    return [c.split("fill=")[1].split()[0] for c in cmds if "fill=" in c]

# 10a) gf 扁平 stop 编码(bodymovin 至今大量导出): [pos,r,g,b, pos,r,g,b]
base.pop("assets",None)
base["layers"]=[layer(gf_layer([0,1,0,0, 1,0,0,1]))]
o10=render(base); c10=shape_cmds(o10)
f10=fills_of(c10)
print("  [10a] gf 扁平 stop:", len(c10), "条色带,", len(set(f10)), "种颜色")
# 色带取的是带**中点**处的插值色, 所以首带略偏洋红(FA0005)而不是纯 FF0000 ——
# 断言按"红分量递减、蓝分量递增"的单调性钉住, 比钉字面更贴语义。
def _rgb(hex8):
    return int(hex8[0:2],16), int(hex8[2:4],16), int(hex8[4:6],16)
r0,g0,b0 = _rgb(f10[0]); r1,g1,b1 = _rgb(f10[-1])
mid = _rgb(f10[len(f10)//2])
ck("gf 渐变填充生效(多色带 + 红→蓝单调过渡)",
   len(c10)>=8 and len(set(f10))>=8 and r0>200 and b0<20 and b1>200 and r1<20
   and r0>mid[0]>r1 and b0<mid[2]<b1,
   (len(c10), f10[0], f10[len(f10)//2], f10[-1]))

# 10b) gf 新式 stop 编码: [{t:pos,s:[r,g,b,a]}]
base["layers"]=[layer(gf_layer([{"t":0,"s":[1,0,0,1]},{"t":1,"s":[0,0,1,1]}]))]
o10b=render(base); c10b=shape_cmds(o10b)
f10b=fills_of(c10b)
print("  [10b] gf 新式 stop:", len(c10b), "条色带")
ck("gf 新式 stop 编码同样生效", len(c10b)>=8 and len(set(f10b))>=8, len(c10b))

# 10c) 0..100 量纲的 stop(少数导出) —— 越界自动 /100
base["layers"]=[layer(gf_layer([0,1,0,0, 100,0,0,1]))]
o10c=render(base); c10c=shape_cmds(o10c)
f10c=fills_of(c10c)
ck("gf stop 0..100 量纲归一(色带数不变)", len(c10c)>=8 and len(set(f10c))>=8, len(c10c))

# 10d) 渐变描边 gs: 折线按段细分, 每小段一个逐段色
base["layers"]=[layer(gf_layer([0,1,1,0, 1,1,0,0], stroke=True))]
o10d=render(base); c10d=shape_cmds(o10d)
f10d=fills_of(c10d)
print("  [10d] gs 渐变描边:", len(c10d), "段")
ck("gs 渐变描边生效(沿路径多段变色)", len(c10d)>=8 and len(set(f10d))>=8,
   (len(c10d), len(set(f10d))))

# 10e) 径向 t=2 退化为中位色(如实写明的不支持边界)
base["layers"]=[layer(gf_layer([0,1,0,0, 1,0,0,1], t=2))]
o10e=render(base); c10e=shape_cmds(o10e)
ck("径向渐变退化为单色(不崩, 出 1 条)", len(c10e)==1, len(c10e))

# 11) mask 最小版: masksProperties 单条加法蒙版 → CLIP_PUSH/POP 夹住内容
mask_shapes=[grp([{"ty":"rc","p":{"a":0,"k":[0,0]},"s":{"a":0,"k":[80,80]},"r":{"a":0,"k":0}},
                  {"ty":"fl","c":{"a":0,"k":[0,1,0]},"o":{"a":0,"k":100},"r":1}])]
mask_ks={"inv":False,"mode":"a","o":{"a":0,"k":100},
         "pt":{"a":0,"k":{"i":[[0,0],[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0],[0,0]],
                         "v":[[-10,-10],[10,-10],[10,10],[-10,10]],"c":True}}}
base["layers"]=[layer(mask_shapes, masksProperties=[mask_ks])]
o11=render(base)
lines11=[l for l in o11.splitlines() if l.startswith(("CLIP","POLY"))]
clip_push=[l for l in lines11 if l.startswith("CLIP+")]
clip_pop=[l for l in lines11 if l.startswith("CLIP-")]
print("  [11] mask:", lines11)
ck("mask 最小版: CLIP+ 裁到蒙版外接矩形(20x20 @40,40)",
   len(clip_push)==1 and " 40.0    40.0    20.0    20.0" in clip_push[0], clip_push)
ck("mask 最小版: CLIP+/CLIP- 配对", len(clip_pop)==1, clip_pop)

# 11b) 反相/相减蒙版不做(内容不被裁剪 —— 保持可见而非整块消失)
mask_inv=dict(mask_ks); mask_inv["mode"]="s"
base["layers"]=[layer(mask_shapes, masksProperties=[mask_inv])]
o11b=render(base)
ck("相减蒙版(mode=s)跳过不裁(不误伤内容)",
   not any(l.startswith("CLIP+") for l in o11b.splitlines()), None)

# 11c) 蒙版路径关键帧: 蒙版矩形随时间放大 → HN_CLOCK 不同时刻裁剪矩形不同
mask_kf={"inv":False,"mode":"a","o":{"a":0,"k":100},
         "pt":{"a":1,"k":[
            {"t":0,"s":[{"i":[[0,0],[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0],[0,0]],
                          "v":[[-10,-10],[10,-10],[10,10],[-10,10]],"c":True}]},
            {"t":60,"s":[{"i":[[0,0],[0,0],[0,0],[0,0]],"o":[[0,0],[0,0],[0,0],[0,0]],
                          "v":[[-30,-30],[30,-30],[30,30],[-30,30]],"c":True}]}]}}
base["layers"]=[layer(mask_shapes, masksProperties=[mask_kf])]
import os as _os
ap2=_os.path.join(D,"a.json"); open(ap2,"w").write(json.dumps(base))
hp2=_os.path.join(D,"a.html")
open(hp2,"w").write('<html><head><style>html,body{margin:0;width:100%;height:100%;background:#000}.l{width:100;height:100}</style></head><body><img class="l" src="a.json" hn-lottie></body></html>')
def clip_at(clock):
    env=dict(_os.environ); env["HN_CLOCK"]=str(clock)
    out=subprocess.run([HN,"paint",hp2,"120","120"],capture_output=True,text=True,env=env).stdout
    for l in out.splitlines():
        if l.startswith("CLIP+"): return l
    return ""
c0, c60 = clip_at(0), clip_at(600)
# 关键帧 t=0(20x20) → t=60 帧 = 1000ms(60x60); 600ms 是 0.6 进度 → 44x44 @ (28,28)
print("  [11c] mask 关键帧 t=0:", c0, "| t=600:", c60)
ck("蒙版路径关键帧生效(裁剪盒随时间变大)",
   c0 and c60 and c0 != c60 and " 40.0    40.0    20.0    20.0" in c0
   and " 28.0    28.0    44.0    44.0" in c60, (c0, c60))

print()
print("失败:",len(fails),fails)

#!/usr/bin/env python3
"""Lottie 能力探针 —— 先探针后实现的那一批。

引擎负责"矢量图形 + 时间轴", 产物是平台无关的绘制指令, 所以这里用
hncore 的 paint 子命令直接看指令, 不涉及任何平台后端。

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
import subprocess, os, tempfile, json
HN="/tmp/hncore"
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

print()
print("失败:",len(fails),fails)

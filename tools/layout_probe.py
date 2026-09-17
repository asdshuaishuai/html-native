#!/usr/bin/env python3
"""布局/排版探针 —— 用期望值对照, 找出真实的排版/布局缺陷。

用法:
    python3 tools/layout_probe.py [hncore 路径]

数据源是 hncore(纯 C 引擎 CLI): 不经过 Swift 运行时, 因此探到的就是
引擎核心的行为, 也是 macOS / Linux / Windows 三平台共用的那部分。

存在的意义: 以下三类缺陷都属于"写了样式却毫无反应"——
不报错、不崩溃, 只能靠对照期望值才能发现:
  1. margin 长写(margin-top/left/…)全部失效(token 被 auto 判定吃掉)
  2. 相邻 margin 不折叠(间距凭空翻倍, 破坏垂直节奏)
  3. flex-shrink 不生效(子项溢出并互相重叠)
  4. justify-content: space-around / space-evenly 静默退化为 flex-start
所以每次改布局/样式代码都值得跑一遍。
"""
import subprocess, os, sys, tempfile

import sys
HN = sys.argv[1] if len(sys.argv) > 1 else "/tmp/hncore"
if not os.path.exists(HN):
    sys.exit("找不到 hncore: %s\n先构建: cc -std=c99 -O1 -I Sources/CHtmlNative/include "
             "-I Sources/CHtmlNative tools/hncore.c Sources/CHtmlNative/hn_*.c "
             "Sources/CHtmlNative/hnsoft.c -o /tmp/hncore" % HN)
D = tempfile.mkdtemp(prefix="hnprobe-")
passed, failed = [], []

def boxes(html, w=400, h=300):
    p = os.path.join(D, "case.html")
    with open(p, "w") as f:
        f.write(html)
    out = subprocess.run([HN, "boxes", p, str(w), str(h)],
                         capture_output=True, text=True).stdout
    res = {}
    for line in out.strip().splitlines()[1:]:     # 第 0 行是 header
        parts = line.split(",")
        if len(parts) < 6 or not parts[1]:
            continue
        # tag,id,x,y,w,h —— id 为空则跳过(无标识无法定位)
        try:
            x, y, bw, bh = (float(v) for v in parts[2:6])
        except ValueError:
            continue
        res[parts[1]] = (x, y, bw, bh)
    return res

def ck(name, box, want):
    if box == want:
        passed.append(name)
    else:
        failed.append((name, want, box))

def near(name, box, want, tol=1.0):
    if box == "MISSING":
        failed.append((name, want, "MISSING")); return
    if all(abs(a - b) <= tol for a, b in zip(box, want)):
        passed.append(name)
    else:
        failed.append((name, want, box))

def get(res, i):
    return res.get(i, "MISSING")

# ============ flex: 主轴对齐 ============
def flex_case(extra, ids, tag):
    style = "html,body{margin:0}.f{display:flex;width:300;height:100" + extra + "}.c{width:50;height:30}"
    body = "".join('<div class="c" id="%s"></div>' % i for i in ids)
    return '<html><head><style>%s</style></head><body><div class="f">%s</div></body></html>' % (style, body)

# 注意: .c 给了显式 height:30, 因此 align-items 默认 stretch 不拉伸(有定高就不拉)
r = boxes(flex_case(";justify-content:flex-start", ["a"], ""))
near("justify flex-start", get(r, "a"), (0, 0, 50, 30))

r = boxes(flex_case(";justify-content:center", ["a"], ""))
near("justify center", get(r, "a"), (125, 0, 50, 30))

r = boxes(flex_case(";justify-content:flex-end", ["a"], ""))
near("justify flex-end", get(r, "a"), (250, 0, 50, 30))

r = boxes(flex_case(";justify-content:space-between", ["a", "b", "c"], ""))
near("space-between a", get(r, "a"), (0, 0, 50, 30))
near("space-between b", get(r, "b"), (125, 0, 50, 30))
near("space-between c", get(r, "c"), (250, 0, 50, 30))

r = boxes(flex_case(";justify-content:space-around", ["a", "b"], ""))
# 两侧 = 间隙的一半: 间隙=(300-100)/2=100 → a 在 50, b 在 200
near("space-around a", get(r, "a"), (50, 0, 50, 30))
near("space-around b", get(r, "b"), (200, 0, 50, 30))

r = boxes(flex_case(";justify-content:space-evenly", ["a", "b"], ""))
# 三段各 100/3≈33.3 → a 在 33.3, b 在 216.7
near("space-evenly a", get(r, "a"), (66.67, 0, 50, 30))
near("space-evenly b", get(r, "b"), (183.33, 0, 50, 30))

# ============ flex: 交叉轴 ============
for al, exp in [("flex-start", (0, 0, 50, 30)), ("flex-end", (0, 70, 50, 30)),
                ("center", (0, 35, 50, 30)), ("stretch", (0, 0, 50, 30))]:
    r = boxes(flex_case(";align-items:" + al, ["a"], ""))
    near("align-items:" + al, get(r, "a"), exp)

# ============ flex: 列方向 ============
def col_case(extra, ids):
    style = ("html,body{margin:0}.f{display:flex;flex-direction:column;"
             "width:100;height:200" + extra + "}.c{width:50;height:30}")
    body = "".join('<div class="c" id="%s"></div>' % i for i in ids)
    return '<html><head><style>%s</style></head><body><div class="f">%s</div></body></html>' % (style, body)

r = boxes(col_case(";justify-content:flex-end", ["a", "b"]))
near("column flex-end a", get(r, "a"), (0, 140, 50, 30))
near("column flex-end b", get(r, "b"), (0, 170, 50, 30))

r = boxes(col_case(";justify-content:space-between", ["a", "b"]))
near("column space-between a", get(r, "a"), (0, 0, 50, 30))
near("column space-between b", get(r, "b"), (0, 170, 50, 30))

# ============ grow / shrink ============
r = boxes('<html><head><style>html,body{margin:0}'
          '.f{display:flex;width:300;height:100}.a{flex:1;height:20}.b{flex:3;height:20}'
          '</style></head><body><div class="f"><div class="a" id="a"></div>'
          '<div class="b" id="b"></div></div></body></html>')
near("grow 1:3 a", get(r, "a"), (0, 0, 75, 20))
near("grow 1:3 b", get(r, "b"), (75, 0, 225, 20))

r = boxes('<html><head><style>html,body{margin:0}'
          '.f{display:flex;width:200;height:100}'
          '.a{width:150;height:20;flex-shrink:1}.b{width:150;height:20;flex-shrink:1}'
          '</style></head><body><div class="f"><div class="a" id="a"></div>'
          '<div class="b" id="b"></div></div></body></html>')
near("shrink 1:1 a", get(r, "a"), (0, 0, 100, 20))
near("shrink 1:1 b", get(r, "b"), (100, 0, 100, 20))

r = boxes('<html><head><style>html,body{margin:0}'
          '.f{display:flex;width:200;height:100}'
          '.a{width:150;height:20;flex-shrink:1}.b{width:150;height:20;flex-shrink:3}'
          '</style></head><body><div class="f"><div class="a" id="a"></div>'
          '<div class="b" id="b"></div></div></body></html>')
# 溢出 100, 按 1:3 → a 缩 25, b 缩 75 → 125 / 75
near("shrink 1:3 a", get(r, "a"), (0, 0, 125, 20))
near("shrink 1:3 b", get(r, "b"), (125, 0, 75, 20))

# ============ gap ============
r = boxes(flex_case(";gap:20", ["a", "b"], ""))
near("gap a", get(r, "a"), (0, 0, 50, 30))
near("gap b", get(r, "b"), (70, 0, 50, 30))

# ============ margin 折叠 ============
r = boxes('<html><head><style>html,body{margin:0}'
          '.a{width:100;height:30;margin:20}'
          '</style></head><body><div class="a" id="a"></div><div class="a" id="b"></div></body></html>')
near("margin 起点 a", get(r, "a"), (20, 20, 100, 30))
near("margin 折叠 b(取最大值 40 而非相加 20)", get(r, "b"), (20, 70, 100, 30))

# ============ 百分比 ============
r = boxes('<html><head><style>html,body{margin:0}'
          '.p{width:400;height:200}.c{width:50%;height:50}'
          '</style></head><body><div class="p"><div class="c" id="a"></div></div></body></html>')
near("百分比宽", get(r, "a"), (0, 0, 200, 50))

# ============ box-sizing ============
r = boxes('<html><head><style>html,body{margin:0}'
          '.c{width:100;height:100;padding:10;box-sizing:border-box}'
          '</style></head><body><div class="c" id="a"></div></body></html>')
near("border-box", get(r, "a"), (0, 0, 100, 100))

r = boxes('<html><head><style>html,body{margin:0}'
          '.c{width:100;height:100;padding:10}'
          '</style></head><body><div class="c" id="a"></div></body></html>')
near("content-box", get(r, "a"), (0, 0, 120, 120))

# ============ min/max 约束 ============
r = boxes('<html><head><style>html,body{margin:0}'
          '.f{display:flex;width:300;height:100}.a{width:400;max-width:120;height:20}'
          '</style></head><body><div class="f"><div class="a" id="a"></div></div></body></html>')
near("max-width 生效", get(r, "a"), (0, 0, 120, 20))

r = boxes('<html><head><style>html,body{margin:0}'
          '.f{display:flex;width:300;height:100}.a{width:20;min-width:120;height:20}'
          '</style></head><body><div class="f"><div class="a" id="a"></div></div></body></html>')
near("min-width 生效", get(r, "a"), (0, 0, 120, 20))

# ============ 报告 ============
print("通过 %d / 失败 %d" % (len(passed), len(failed)))
for name, want, got in failed:
    print("  ✘ %-34s 期望 %s  实际 %s" % (name, want, got))
sys.exit(1 if failed else 0)

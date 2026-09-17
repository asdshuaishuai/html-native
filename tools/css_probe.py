#!/usr/bin/env python3
"""CSS/HTML 规范符合性探针 v2 —— 只测"该对却可能错了"的地方。

与 layout_probe.py 的分工: 那个测盒几何, 这个测**规则与解析**
(选择器优先级、简写展开、颜色格式、单位换算、继承、文本属性)。
全部用期望值对照, 找出静默错误。
"""
import subprocess, os, sys, tempfile

HN = sys.argv[1] if len(sys.argv) > 1 else "/tmp/hncore"
if not os.path.exists(HN):
    sys.exit("找不到 hncore: %s" % HN)
D = tempfile.mkdtemp(prefix="hncss-")
passed, failed = [], []

def run(html, w=400, h=300):
    p = os.path.join(D, "c.html")
    with open(p, "w") as f:
        f.write(html)
    out = subprocess.run([HN, "boxes", p, str(w), str(h)],
                         capture_output=True, text=True).stdout
    res = {}
    for line in out.strip().splitlines()[1:]:
        parts = line.split(",")
        if len(parts) < 6 or not parts[1]:
            continue
        try:
            x, y, bw, bh = (float(v) for v in parts[2:6])
        except ValueError:
            continue
        res.setdefault(parts[1], []).append((x, y, bw, bh))
    return res

def text_runs(html, w=400, h=300):
    p = os.path.join(D, "t.html")
    with open(p, "w") as f:
        f.write(html)
    out = subprocess.run([HN, "text", p, str(w), str(h)],
                         capture_output=True, text=True).stdout
    runs = []
    for line in out.strip().splitlines():
        if not line.startswith("run "):
            continue
        # hncore 输出 "run  x=   53.8  baseline=   14.4" —— 键与值之间被空格分开,
        # 直接 split 会得到 "x=" 与 "53.8" 两个 token。这里按 = 归并。
        d = {}
        toks = line.split()
        i = 0
        while i < len(toks):
            t = toks[i]
            if t.endswith("="):
                key = t[:-1]
                val = toks[i + 1] if i + 1 < len(toks) else ""
                i += 2
            elif "=" in t:
                key, val = t.split("=", 1)
                i += 1
            else:
                i += 1
                continue
            try:
                d[key] = float(val)
            except ValueError:
                pass
        runs.append(d)
    return runs

def ck(name, got, want, tol=1.0):
    if got == want or (isinstance(got, float) and isinstance(want, float)
                       and abs(got - want) <= tol):
        passed.append(name)
    else:
        failed.append((name, want, got))

def page(css, body):
    return '<html><head><style>html,body{margin:0}%s</style></head><body>%s</body></html>' % (css, body)

# ============ margin/padding 简写展开 ============
# 盒的 h 是 height 声明值(20); margin 不进入自身盒子, padding 会
for css, want, label in [
    (".a{margin:10 20}",            (20, 10, 100, 20), "简写 2 值 (上下10 左右20)"),
    (".a{margin:10 20 30}",         (20, 10, 100, 20), "简写 3 值 (上10 左右20 下30)"),
    (".a{margin:10 20 30 40}",      (40, 10, 100, 20), "简写 4 值 (TRBL)"),
    (".a{padding:10 20}",           (0, 0, 140, 40),  "padding 2 值 (10 上下, 20 左右)"),
]:
    css2 = css + ".a{width:100;height:20}"
    r = run(page(css2, '<div class="a" id="x"></div>'))
    ck(label, r.get("x", [None])[0], want)

# ============ 单位换算 ============
for css, want, label in [
    (".a{font-size:20;width:10em}",   200, "em 基于本元素字号"),
    (".a{font-size:20;margin-top:50%}", 200, "margin % 基于容器宽(400×50%)"),
    (".a{font-size:20;width:50pt}",   66.67, "pt → px (×96/72)"),
]:
    css2 = css + ".a{height:10}"
    r = run(page(css2, '<div class="p" style="width:400;height:400"><div class="a" id="x"></div></div>'))
    b = r.get("x", [None])[0]
    if b:
        ck(label, b[2] if "width" in css or "em" in css or "pt" in css else b[1], want, 1.5)

# rem 基于根字号
r = run(page("html{font-size:25}.a{height:4rem;width:4rem}", '<div class="a" id="x"></div>'))
ck("rem 基于根字号", r.get("x", [None])[0], (0, 0, 100, 100))

# ============ 颜色解析(经 background 落到指令) ============
def rect_fill(css_decl, extra=".a{width:100;height:50}"):
    html = page(extra + ".a{%s}" % css_decl, '<div class="a" id="x"></div>')
    out = subprocess.run([HN, "paint", os.path.join(D, "p.html"), "400", "300"],
                         capture_output=True, text=True).stdout
    return None

# paint 需要文件, 单独写
def paint_fill(css_decl):
    p = os.path.join(D, "p.html")
    with open(p, "w") as f:
        f.write(page(".a{width:100;height:50;%s}" % css_decl, '<div class="a" id="x"></div>'))
    out = subprocess.run([HN, "paint", p, "400", "300"],
                         capture_output=True, text=True).stdout
    for line in out.splitlines():
        if line.startswith("RECT") and "100.0" in line and "50.0" in line:
            # fill=%08X
            import re
            m = re.search(r"fill=([0-9A-Fa-f]{8})", line)
            if m:
                return m.group(1).upper()
    return None

for decl, want, label in [
    ("background:#f00",        "FF0000FF", "hex 3 位"),
    ("background:#ff0000",     "FF0000FF", "hex 6 位"),
    ("background:#ff000080",   "FF000080", "hex 8 位(带 alpha)"),
    ("background:#ff0000ff",   "FF0000FF", "hex 8 位(全不透明)"),
    ("background:rgb(0,128,255)",  "0080FFFF", "rgb()"),
    ("background:rgba(0,128,255,0.5)", "0080FF7F", "rgba() 50%"),
    ("background:rgba(0 128 255 / 0.5)", "0080FF7F", "rgba() 空格+斜杠写法"),
    ("background:rgb(0 128 255)", "0080FFFF", "rgb() 空格写法"),
    ("background:rebeccapurple",  "663399FF", "命名色 rebeccapurple"),
]:
    ck(label, paint_fill(decl), want)

# ============ 选择器优先级 ============
# 同优先级后者胜; id > class > tag
r = run(page("#x{background:#f00;width:50;height:20}.a{width:200;height:20}",
             '<div class="a" id="x"></div>'))
ck("id 选择器优先于 class", r.get("x", [None])[0], (0, 0, 50, 20))

# 后定义的同优先级胜出
r = run(page(".a{width:50;height:20}.b{width:200;height:20}",
             '<div class="a b" id="x"></div>'))
ck("同特异性后定义者胜", r.get("x", [None])[0], (0, 0, 200, 20))

# class 优先于 tag
r = run(page("div{width:50;height:20}.a{width:200;height:20}",
             '<div class="a" id="x"></div>'))
ck("class 优先于 tag", r.get("x", [None])[0], (0, 0, 200, 20))

# 内联 style 最高
r = run(page(".a{width:50;height:20}",
             '<div class="a" id="x" style="width:200;height:20"></div>'))
ck("内联 style 最高优先", r.get("x", [None])[0], (0, 0, 200, 20))

# ============ 组合器 ============
r = run(page(".p > .c{width:200;height:20}.q .c{width:50;height:20}",
             '<div class="p"><div class="c" id="x"></div></div>'))
ck("子代组合器 >", r.get("x", [None])[0], (0, 0, 200, 20))

r = run(page(".p .c{width:200;height:20}",
             '<div class="p"><div class="wrap"><div class="c" id="x"></div></div></div>'))
ck("后代组合器(任意深度)", r.get("x", [None])[0], (0, 0, 200, 20))

# ============ nth-child ============
r = run(page(".c:nth-child(2){width:200;height:20}.c{width:50;height:20}",
             '<div class="p"><div class="c" id="a"></div><div class="c" id="b"></div></div>'))
b = r.get("b", [None])[0]
ck(":nth-child(2) 命中第二个", b, (0, 20, 200, 20))

r = run(page(".c:nth-child(even){width:200;height:20}.c{width:50;height:20}",
             '<div class="p"><div class="c" id="a"></div><div class="c" id="b"></div>'
             '<div class="c" id="c"></div><div class="c" id="d"></div></div>'))
ck(":nth-child(even) 命中 2/4", r.get("b", [None])[0], (0, 20, 200, 20))
ck(":nth-child(even) 跳过 1", r.get("a", [None])[0], (0, 0, 50, 20))
ck(":nth-child(even) 命中 4", r.get("d", [None])[0], (0, 60, 200, 20))

# ============ flex: align-self 覆盖 align-items ============
r = run(page(".f{display:flex;width:300;height:100;align-items:flex-start}"
             ".c{width:50;height:30}.s{align-self:flex-end}",
             '<div class="f"><div class="c s" id="x"></div></div>'))
ck("align-self 覆盖 align-items", r.get("x", [None])[0], (0, 70, 50, 30))

# ============ flex: order ============
r = run(page(".f{display:flex;width:300;height:50}.c{width:50;height:20}"
             ".z{order:-1}",
             '<div class="f"><div class="c" id="a"></div><div class="c z" id="b"></div></div>'))
ck("order:-1 排到最前", r.get("b", [None])[0], (0, 0, 50, 20))

# ============ flex-basis ============
r = run(page(".f{display:flex;width:300;height:50}"
             ".a{flex-basis:200;height:20}.b{flex-grow:1;height:20}",
             '<div class="f"><div class="a" id="x"></div><div class="b" id="y"></div></div>'))
b = r.get("x", [None])[0]
if b:
    ck("flex-basis:200 作初始主轴尺寸", b[2], 200, 2)
ck("flex-grow 吃剩余", r.get("y", [None])[0], (200, 0, 100, 20))

# ============ 继承 ============
r = run(page(".p{font-size:30;color:#f00;line-height:2}"
             ".c{width:100;font-size:inherit}",
             '<div class="p"><div class="c" id="x">文</div></div>'))
# 行高 2 × 30 = 60 → 盒高至少 60
b = r.get("x", [None])[0]
if b:
    ck("line-height 倍数随字号生效", b[3], 60, 4)

# font-size 用百分比继承
r = run(page("html{font-size:20}.p{font-size:200%;line-height:1}"
             ".c{width:100}",
             '<div class="p"><div class="c" id="x">文</div></div>'))
b = r.get("x", [None])[0]
if b:
    ck("font-size:200% 基于父字号", b[3], 40, 4)

# ============ 文本属性 ============
# white-space:nowrap 不换行
runs = text_runs(page(".p{width:40;font-size:14}",
                      '<div class="p">一二三四五六七八</div>'))
ck("默认自动换行(多行)", len(runs) > 1, True)

runs = text_runs(page(".p{width:40;font-size:14;white-space:nowrap}",
                      '<div class="p">一二三四五六七八</div>'))
ck("white-space:nowrap 不换行", len(runs), 1)

# text-align:center 使行起点 > 0
runs = text_runs(page(".p{width:200;font-size:14;text-align:center}",
                      '<div class="p">居中文本</div>'))
if runs:
    ck("text-align:center 行有左边距", runs[0].get("x", 0) > 5, True)

runs = text_runs(page(".p{width:200;font-size:14;text-align:right}",
                      '<div class="p">右对齐</div>'))
if runs:
    w = runs[0].get("w", 0)
    x = runs[0].get("x", 0)
    ck("text-align:right 贴右边", abs((x + w) - 200) < 3, True)

# ============ HTML 语义: 嵌套列表 ============
r = run(page("ul,ol{margin:0;padding-left:20}li{list-style:none;width:50;height:16}",
             '<ul><li id="a">一<ul><li id="b">二</li></ul></li></ul>'))
b = r.get("b", [None])[0]
if b:
    ck("嵌套列表缩进累加(父20+子20)", b[0], 40, 2)

# ============ overflow 容器不压缩内容 ============
r = run(page(".c{width:100;height:100;overflow:hidden}"
             ".k{width:80;height:80;margin:0}",
             '<div class="c" id="box"><div class="k" id="x"></div></div>'))
ck("overflow 容器尺寸不受内容影响", r.get("box", [None])[0], (0, 0, 100, 100))

# ============ 报告 ============
print("通过 %d / 失败 %d" % (len(passed), len(failed)))
for name, want, got in failed:
    print("  ✘ %-36s 期望 %s  实际 %s" % (name, want, got))
sys.exit(1 if failed else 0)

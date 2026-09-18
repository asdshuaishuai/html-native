#!/usr/bin/env python3
"""CSS/HTML 规范符合性探针 v2 —— 只测"该对却可能错了"的地方。

与 layout_probe.py 的分工: 那个测盒几何, 这个测**规则与解析**
(选择器优先级、简写展开、颜色格式、单位换算、继承、文本属性)。
全部用期望值对照, 找出静默错误。
"""
import subprocess, os, sys, tempfile, re

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


# ============ 排版单位语义(来自 css_probe_v3) ============
def lineh(decl, label):
    r = run(page(decl + ".k{width:100}",
                 "<div class='p'><div class='k' id='x'>\u6587</div></div>"))
    return r.get("x", [None])[0]

# line-height 三种单位: 无单位=倍数 / px=绝对 / %=相对字号 / em=相对字号
for decl, want, label in [
    (".p{font-size:20;line-height:1.5}",   30,  "line-height:1.5 → 30px"),
    (".p{font-size:20;line-height:150%}",  30,  "line-height:150% → 30px"),
    (".p{font-size:20;line-height:30px}",  30,  "line-height:30px → 30px"),
    (".p{font-size:20;line-height:2em}",   40,  "line-height:2em → 40px"),
]:
    b = lineh(decl, label)
    if b: ck(label, b[3], want, 5)

# 无单位数字是倍数(CSS 语义) —— 不是"像素"
b = lineh(".p{font-size:20;line-height:30}", "")
if b: ck("line-height:30 是倍数(30×20=600)", b[3], 600, 5)

# box-sizing 与 padding+border 叠加
b = run(page(".a{width:100;height:100;padding:10;border:5 solid #f00;box-sizing:border-box}",
             "<div class='a' id='x'></div>")).get("x", [None])[0]
ck("border-box: 宽含 padding+border", b, (0, 0, 100, 100))
b = run(page(".a{width:100;height:100;padding:10;border:5 solid #f00}",
             "<div class='a' id='x'></div>")).get("x", [None])[0]
ck("content-box: 宽加 padding+border", b, (0, 0, 130, 130))

# 逐侧 padding 长写
b = run(page(".a{width:100;height:100;padding-top:40}",
             "<div class='a' id='x'></div>")).get("x", [None])[0]
ck("padding-top 长写", b, (0, 0, 100, 140))
b = run(page(".a{width:100;height:100;padding-left:40}",
             "<div class='a' id='x'></div>")).get("x", [None])[0]
ck("padding-left 长写", b, (0, 0, 140, 100))

# 通配符与选择器组
b = run(page("*{box-sizing:border-box}.a{width:100;height:100;padding:20}",
             "<div class='a' id='x'></div>")).get("x", [None])[0]
ck("通配符 * 应用 box-sizing", b, (0, 0, 100, 100))
r = run(page(".a, .b{width:200;height:20}",
             "<div class='a' id='x'></div><div class='b' id='y'></div>"))
ck("选择器组 命中 a", r.get("x", [None])[0], (0, 0, 200, 20))
ck("选择器组 命中 b", r.get("y", [None])[0], (0, 20, 200, 20))

# 相邻兄弟 +
r = run(page(".a + .b{width:200;height:20}.a{width:50;height:20}",
             "<div class='a' id='a'></div><div class='b' id='b'></div>"))
ck("相邻兄弟 + 命中紧邻", r.get("b", [None])[0], (0, 20, 200, 20))
r = run(page(".a + .b{width:200;height:20}",
             "<div class='a' id='a'></div><div class='c'></div><div class='b' id='b'></div>"))
bb = r.get("b", [None])[0]
if bb: ck("相邻兄弟 + 不命中非紧邻(宽度回落到块级默认 400)", bb[2], 400)

# column-gap
r = run(page(".a{display:flex;width:300;height:50;column-gap:20}.c{width:50;height:20}",
             "<div class='a'><div class='c' id='x'></div><div class='c' id='y'></div></div>"))
ck("column-gap 生效", r.get("y", [None])[0], (70, 0, 50, 20))

# flex-wrap
r = run(page(".f{display:flex;width:100;height:200;flex-wrap:wrap}.c{width:60;height:20}",
             "<div class='f'><div class='c' id='x'></div><div class='c' id='y'></div></div>"))
bb = r.get("y", [None])[0]
if bb: ck("flex-wrap 换到第二行", bb[1] > 10, True)


# ============ 级联与继承(系统性逐属性覆盖检查) ============
# 逐属性用 .a{prop:v1} .b{prop:v2} 验证后声明者胜出 —— 这是抓"守卫挡住赋值"
# 这类 bug 最直接的办法(height 就曾因 HN_U_AUTO 守卫而只认第一条规则)。
def rect_stroke(out):
    for line in out.splitlines():
        if line.startswith("RECT"):
            m = re.search(r"stroke=([0-9A-Fa-f]{8})", line)
            if m:
                return m.group(1).upper()
    return None

def paint_of(html, W=400, H=300):
    p = os.path.join(D, "pz.html")
    with open(p, "w") as f:
        f.write(html)
    return subprocess.run([HN, "paint", p, str(W), str(H)],
                          capture_output=True, text=True).stdout

# 颜色类覆盖: 红 → 绿
for prop in ["background", "background-color"]:
    o = paint_of(page(".a{%s:#ff0000}.b{%s:#00ff00}.c{width:100;height:50}" % (prop, prop),
                      "<div class='a b c'></div>"))
    ck("级联: %s 后声明者胜" % prop, "00FF00" in o, True)

# color 落在 TEXT 指令的 fill 上
o = paint_of(page(".a{color:#ff0000}.b{color:#00ff00}", "<div class='a b'>文</div>"))
ck("级联: color 后声明者胜(TEXT fill)", "fill=00FF00FF" in o, True)

# border-color 落在 RECT 的 stroke 上
o = paint_of(page(".a{border:5 solid #ff0000}.b{border-color:#00ff00}.c{width:100;height:50}",
                  "<div class='a b c'></div>"))
ck("级联: border-color 后声明者胜(RECT stroke)", rect_stroke(o), "00FF00FF")

# 渐变覆盖
o = paint_of(page(".a{background:linear-gradient(0deg,#f00,#00f)}"
                  ".b{background:linear-gradient(90deg,#0f0,#ff0)}.c{width:100;height:50}",
                  "<div class='a b c'></div>"))
ck("级联: 渐变后声明者胜", "gradient" in o, True)

# 继承链(含隔代)与子级覆盖
b = run(page(".p{font-size:40}.k{width:100}", "<div class='p'><div class='k' id='x'>文</div></div>")
        ).get("x", [None])[0]
if b:
    ck("继承: font-size 传到子元素(行高随之)", b[3], 40 * 1.45, 8)
b = run(page(".p{font-size:30}.m{}.k{width:100}",
             "<div class='p'><div class='m'><div class='k' id='x'>文</div></div></div>")
        ).get("x", [None])[0]
if b:
    ck("继承: 隔代传递(孙级)", b[3], 30 * 1.45, 6)
b = run(page(".p{font-size:40}.k{font-size:10;width:100}",
             "<div class='p'><div class='k' id='x'>文</div></div>")).get("x", [None])[0]
if b:
    ck("继承: 子级显式值覆盖继承值", b[3], 10 * 1.45, 4)

# 特异性: id > 多 class
b = run(page("#x{height:200}.a{height:50}.b{height:80}",
             "<div class='a b' id='x'></div>")).get("x", [None])[0]
ck("特异性: id 高于任意数量 class", b, (0, 0, 400, 200))

# ============ 选择器引擎强化(2026-09-19) ============
# 这一批是先探针后实现: 声明了不生效也不报错的静默失败组。

# 属性选择器六种操作符
b = run(page("[data-on]{height:30}", "<div data-on='1' id='x'></div><div id='y'></div>")
        ).get("x",[None])[0]
ck("[attr] 存在选择器", b, (0,0,400,30))
b = run(page("[data-k=\"v\"]{height:30}", "<div data-k='v' id='x'></div><div data-k='w' id='y'></div>")
        ).get("x",[None])[0]
ck("[attr=v] 全等", b, (0,0,400,30))
b = run(page("[href^=\"ht\"]{height:30}", "<div href='https://a' id='x'></div>")
        ).get("x",[None])[0]
ck("[attr^=] 前缀", b, (0,0,400,30))
b = run(page("[href$=\"cn\"]{height:30}", "<div href='https://a.cn' id='x'></div>")
        ).get("x",[None])[0]
ck("[attr$=] 后缀", b, (0,0,400,30))
b = run(page("[data-k*=\"mid\"]{height:30}", "<div data-k='a-mid-b' id='x'></div>")
        ).get("x",[None])[0]
ck("[attr*=] 子串", b, (0,0,400,30))
b = run(page("[rel~=\"no\"]{height:30}", "<div rel='yes no maybe' id='x'></div><div rel='yesno' id='y'></div>")
        ).get("x",[None])[0]
ck("[attr~=] 空白分词之一全等", b, (0,0,400,30))

# :not()
b = run(page(".i{height:20}.i:not(.x){height:40}",
             "<div class='i a' id='x'></div><div class='i x' id='y'></div>"))
ck(":not(.x) 否定 class", b.get("x",[None])[0], (0,0,400,40))
ck(":not() 不影响未否定的", b.get("y",[None])[0], (0,40,400,20))
b = run(page(".i{height:20}.i:not(#z){height:40}",
             "<div class='i' id='x'></div><div class='i' id='z'></div>"))
ck(":not(#z) 否定 id", b.get("x",[None])[0], (0,0,400,40))

# :nth-of-type(只数同标签; 与 nth-child 混用是表格/列表错位的根源)
b = run(page(".i{height:10}.i:nth-of-type(2){height:50}",
             "<div><div class='i' id='a'></div><div class='i' id='b'></div><div class='i' id='c'></div></div>"))
ck(":nth-of-type(2) 命中第二个", b.get("b",[None])[0], (0,10,400,50))
ck(":nth-of-type(2) 不影响其他", b.get("a",[None])[0][3] if b.get("a") else 0, 10)
# 混排: 中间插一个不同标签的元素, nth-of-type 应忽略它, nth-child 应算它
b = run(page("div.t{height:10}span.s{height:5;display:block}div.t:nth-of-type(2){height:50}",
             "<div><div class='t' id='a'></div><span class='s'></span><div class='t' id='b'></div></div>"))
ck("nth-of-type 忽略异类兄弟", b.get("b",[None])[0][3] if b.get("b") else 0, 50)
b = run(page("div.t{height:10}span.s{height:5;display:block}div.t:nth-child(2){height:50}",
             "<div><div class='t' id='a'></div><span class='s'></span><div class='t' id='b'></div></div>"))
ck("nth-child 把异类兄弟计入", b.get("b",[None])[0][3] if b.get("b") else 0, 10)

# calc()
b = run(page("#x{width:calc(100% - 40px);height:10}", "<div id='x'></div>")
        ).get("x",[None])[0]
ck("calc(100% - 40px)", b, (0,0,360,10))
b = run(page("#p{width:200}#x{width:calc(50% + 10px);height:10}", "<div id='p'><div id='x'></div></div>")
        ).get("x",[None])[0]
ck("calc(50% + 10px) 按包含块", b, (0,0,110,10))
b = run(page("#x{width:calc(100px + 20px);height:10}", "<div id='x'></div>")
        ).get("x",[None])[0]
ck("calc 纯绝对值就地求值", b, (0,0,120,10))
b = run(page("#x{width:calc(100% - 40px);height:10}", "<div id='x'></div>")
        ).get("x",[None])[0]
ck("calc 负值 clamp 到 0", run(page("#x{width:calc(100% - 900px)}", "<div id='x'></div>")
        ).get("x",[None])[0][2] >= 0, True)

# ============ 变换引擎(非等比缩放 / transform-origin) ============
# 先探针后实现: 三项都是"写了不生效"且不报错。

def quads(html, w=400, h=300):
    """返回绘制指令里 RECT/QUAD 的行(带坐标), 用来判断变换是否真的应用。"""
    p = os.path.join(D, "t.html")
    open(p, "w").write(html)
    out = subprocess.run([HN, "paint", p, str(w), str(h)],
                         capture_output=True, text=True).stdout
    return [l for l in out.splitlines()
            if l.startswith("RECT") or l.startswith("QUAD")]

R = "<div id='c' style='width:100;height:40;background:#ff0000;%s'></div>"

# scale(x, y) 非等比: 之前只取第一个参数, scale(2,1) 被当成等比 2
q = quads(R % "transform:scale(2,1)")
ck("scale(x,y) 非等比(200x40)", True,
   len(q) == 1 and " 200.0" in q[0] and " 40.0" in q[0])
# scaleX()
q = quads(R % "transform:scaleX(2)")
ck("scaleX(2) 生效(200x40)", True,
   len(q) == 1 and " 200.0" in q[0] and " 40.0" in q[0])
# scaleY()
q = quads(R % "transform:scaleY(2)")
ck("scaleY(2) 生效(100x80)", True,
   len(q) == 1 and " 100.0" in q[0] and " 80.0" in q[0])
# transform-origin: 0 0 绕左上角转 90° —— 100x40 应变成 40x100 且向左下延伸
q0 = quads(R % "transform:rotate(90deg);transform-origin:0 0")
qc = quads(R % "transform:rotate(90deg)")
ck("transform-origin 生效(与缺省输出不同)", True, q0 != qc and len(q0) == 1)
# 绕左上角转 90°: 角点应落在 (0,0) 与 (-40,100) 上
ck("transform-origin:0 0 的旋转角点正确", True,
   len(q0) == 1 and "(0.0,0.0)" in q0[0] and "(-40.0,100.0)" in q0[0])
# 缺省 origin(盒中心)旋转后中心应仍约在原盒中心
ck("缺省 transform-origin 仍为盒中心", True,
   len(qc) == 1 and "(30.0,70.0)" in qc[0])

# ============ 3D 投影(translateZ / rotate3d) ============
# 这两项曾经"只解析不投影" —— project_3d 顶部有两个提前返回, 只改靠外的那个
# 毫无效果(靠内的仍然先 fire)。这里用输出坐标断言, 而不是"可解析"。

def quad3d(html, w=400, h=300):
    return quads(html, w, h)

# translateZ: perspective 400 下 k = dist/(dist-z)。
# translateZ(200) → k=2, 100x60 的盒变成 200x120 且以盒中心为基准。
P3 = "<div id='p' style='width:300;height:200;perspective:400'>" \
     "<div id='c' style='width:100;height:60;background:#ff0000;%s'></div></div>"
q = quad3d(P3 % "")
ck("3D 基线: 无变换走 RECT", True, len(q) == 1 and q[0].startswith("RECT"))
q = quad3d(P3 % "transform:translateZ(200px)")
ck("translateZ(200) 走四边形(投影生效)", True, len(q) == 1 and q[0].startswith("QUAD"))
# k=2 → 宽 200 高 120, 盒中心 (50,30) → x ∈ [-50,150], y ∈ [-30,90]
ck("translateZ(200) 缩放 k=2(角点正确)", True,
   len(q) == 1 and "(-50.0,-30.0)" in q[0] and "(150.0,90.0)" in q[0])
q = quad3d(P3 % "transform:translateZ(-200px)")
# k = 400/(400+200) = 2/3 → 66.7x40, x ∈ [16.7,83.3], y ∈ [10,50]
ck("translateZ(-200) 缩小 k=2/3(角点正确)", True,
   len(q) == 1 and "(16.7,10.0)" in q[0] and "(83.3,50.0)" in q[0])

# rotate3d(0,1,0,45deg) 与 rotateY(45deg) 必须**完全一致**(绕 Y 轴就是 rotateY)
q1 = quad3d(P3 % "transform:rotate3d(0,1,0,45deg)")
q2 = quad3d(P3 % "transform:rotateY(45deg)")
ck("rotate3d(0,1,0,45) 与 rotateY(45) 输出一致", True, q1 == q2 and len(q1) == 1)
# 绕 Y 转 45° 后盒不再轴对齐 → 必须是四边形
ck("rotate3d 走四边形", True, len(q1) == 1 and q1[0].startswith("QUAD"))

# ============ 透明背景层(端到端) ============
# 这是核心卖点之一: hn-transparent 的文档必须真的逐像素透明。
# 两个曾导致"声明了透明却是一块实色"的 bug 都在光栅器里(见
# tools/hnsoft_alpha_probe.c), 所以这里用真实示例做端到端断言。
import struct as _st, zlib as _zl

def _alpha_stats(png_path):
    d = open(png_path, "rb").read()
    pos, idat, w, h = 8, b"", 0, 0
    while pos < len(d):
        ln = _st.unpack(">I", d[pos:pos+4])[0]; typ = d[pos+4:pos+8]
        if typ == b"IHDR": w, h = _st.unpack(">II", d[pos+8:pos+16])
        if typ == b"IDAT": idat += d[pos+8:pos+8+ln]
        pos += 12 + ln
    raw = _zl.decompress(idat); stride = w*4
    o = bytearray(); prev = bytearray(stride); i = 0
    for _y in range(h):
        f = raw[i]; i += 1
        line = bytearray(raw[i:i+stride]); i += stride
        for x in range(stride):
            a = line[x-4] if x >= 4 else 0
            b = prev[x]; c2 = prev[x-4] if x >= 4 else 0
            if f == 1: line[x] = (line[x]+a) & 255
            elif f == 2: line[x] = (line[x]+b) & 255
            elif f == 3: line[x] = (line[x]+(a+b)//2) & 255
            elif f == 4:
                pp = a+b-c2; pa, pb, pc = abs(pp-a), abs(pp-b), abs(pp-c2)
                pr = a if (pa <= pb and pa <= pc) else (b if pb <= pc else c2)
                line[x] = (line[x]+pr) & 255
        o += line; prev = line
    tot = tr = pa = 0
    for i in range(0, w*h, 7):
        a = o[i*4+3]; tot += 1
        if a == 0: tr += 1
        elif a != 255: pa += 1
    return tr*100.0/tot, pa*100.0/tot

_tr = os.path.join(D, "tr.png")
_r = subprocess.run([HN, "render", "examples/transparent.html", _tr, "460", "560"],
                    capture_output=True, text=True, cwd=os.path.dirname(os.path.abspath(__file__)) + "/..")
if _r.returncode == 0 and os.path.exists(_tr):
    trpct, papct = _alpha_stats(_tr)
    ck("透明背板: 文档大面积真透明(>60%)", True, trpct > 60.0)
    ck("透明背板: 有半透明过渡(抗锯齿)", True, papct > 1.0)
else:
    ck("透明背板: 示例可渲染", False, _r.stderr[:80])

# 未声明透明的文档必须仍然完全不透明(别把普通页面也弄透明了)
_op = os.path.join(D, "op.png")
_r2 = subprocess.run([HN, "render", "examples/dashboard.html", _op, "460", "560"],
                     capture_output=True, text=True, cwd=os.path.dirname(os.path.abspath(__file__)) + "/..")
if _r2.returncode == 0 and os.path.exists(_op):
    trpct2, _ = _alpha_stats(_op)
    ck("普通文档仍完全不透明(0% 透明)", True, trpct2 == 0.0)

# ============ 报告 ============
print("通过 %d / 失败 %d" % (len(passed), len(failed)))
for name, want, got in failed:
    print("  ✘ %-36s 期望 %s  实际 %s" % (name, want, got))
sys.exit(1 if failed else 0)

#!/usr/bin/env python3
"""HTML 生态适配探针 —— 解析层面的真实行为。

与 css_probe.py(规则与解析)、layout_probe.py(盒几何)并列, 这份测 HTML 侧:
标签语义、属性处理、实体、注释、结构容错、空白语义。

存在的意义: 有一整类缺陷是"解析器默默改写了内容" —— 不报错、不崩溃,
只是渲染结果与源码不符。最典型的是空白折叠: tokenizer 曾无条件把连续空白
压成一个空格, 于是 white-space:pre 与 normal 输出完全一致, 代码块与对齐
文本全错。这类问题只能靠期望值对照发现。

用法: python3 tools/html_probe.py [hncore 路径]
"""
import subprocess, os, sys, tempfile, re

HN = sys.argv[1] if len(sys.argv) > 1 else "/tmp/hncore"
if not os.path.exists(HN):
    sys.exit("找不到 hncore: %s" % HN)
D = tempfile.mkdtemp(prefix="hnhtml-")
passed, failed = [], []

def _w(tag, html):
    p = os.path.join(D, tag + ".html")
    with open(p, "w") as f:
        f.write(html)
    return p

def parse(html):
    r = subprocess.run([HN, "parse", _w("p", html)], capture_output=True, text=True)
    return r.stdout, r.returncode

def boxes(html, W=400, H=300):
    out = subprocess.run([HN, "boxes", _w("b", html), str(W), str(H)],
                         capture_output=True, text=True).stdout
    res = {}
    for line in out.strip().splitlines()[1:]:
        q = line.split(",")
        if len(q) < 6 or not q[1]:
            continue
        try:
            x, y, bw, bh = (float(v) for v in q[2:6])
        except ValueError:
            continue
        res.setdefault(q[1], []).append((x, y, bw, bh))
    return res

def runs(html, W=400, H=300):
    out = subprocess.run([HN, "text", _w("t", html), str(W), str(H)],
                         capture_output=True, text=True).stdout
    return [l for l in out.splitlines() if l.startswith("run ")]

def run_count(html, W=400, H=300):
    return len(runs(html, W, H))

def run_advance(html, W=400, H=300):
    """最后一个片段的 x+w —— 整段文本的总推进宽度"""
    best = 0.0
    for l in runs(html, W, H):
        mx = re.search(r"x=\s*(-?[0-9.]+)", l)
        mw = re.search(r"w=\s*([0-9.]+)", l)
        if mx and mw:
            best = max(best, float(mx.group(1)) + float(mw.group(1)))
    return best

def ck(n, got, want):
    if got == want:
        passed.append(n)
    else:
        failed.append((n, want, got))

# ============ 1. 基础语义 ============

r = runs("<html><body><div>&lt;a&gt; &amp; &#65;</div></body></html>")
txt = " ".join(r)
ck("HTML 实体被解码", ("&lt;" not in txt) and ("&amp;" not in txt), True)

d, rc = parse("<html><body><!-- 注释文字 --><div id='x'>内容</div></body></html>")
ck("注释不进入 DOM", "注释文字" not in d, True)

b = boxes("<html><head><style>html,body{margin:0}</style></head><body>"
          "<div id='x' style='width:100;height:30'></div><br/>"
          "<div id='y' style='width:100;height:30'></div></body></html>")
if "y" in b:
    ck("自闭合 <br/> 不吞后续内容", b["y"][0][1] >= 30, True)

b = boxes("<html><head><style>html,body{margin:0}</style></head><body>"
          "<img id='i' src='x.png' style='width:50;height:50'>"
          "<div id='d' style='width:100;height:30'></div></body></html>")
ck("void 元素(img)后仍有兄弟", "d" in b, True)

b = boxes("<html><head><style>html,body{margin:0}</style></head><body>"
          "<my-widget id='x' style='width:100;height:30'></my-widget></body></html>")
ck("未知标签可用(块级)", b.get("x", [None])[0], (0, 0, 100, 30))

b = boxes("<HTML><HEAD><STYLE>html,body{margin:0}</STYLE></HEAD><BODY>"
          "<DIV ID='x' STYLE='width:100;height:30'></DIV></BODY></HTML>")
ck("标签与属性名大小写不敏感", b.get("x", [None])[0], (0, 0, 100, 30))

b = boxes("<html><head><style>html,body{margin:0}</style></head><body>"
          "<div id='x'\n     class='a'\n     style='width:100;height:30'></div></body></html>")
ck("跨行属性解析", b.get("x", [None])[0], (0, 0, 100, 30))

b = boxes("<html><head><style>html,body{margin:0}</style></head><body>"
          "<div id='x' style='width:100;height:30'>文本</body></html>")
ck("未闭合 div 仍解析", b.get("x", [None])[0] is not None, True)

r = runs("<html><head><style>.a{color:red}</style></head><body>"
         "<script>var x = 1;</script><div>可见</div></body></html>")
ck("script/style 内容不作可见文本", all("var" not in l and "color" not in l for l in r), True)

b = boxes("<html><head><style>html,body{margin:0}ul,ol{margin:0;padding-left:20}"
          "li{list-style:none;width:40;height:14}</style></head><body>"
          "<ul><li id='l1'>一<ul><li id='l2'>二<ul><li id='l3'>三</li></ul>"
          "</li></ul></li></ul></body></html>")
if all(k in b for k in ("l1", "l2", "l3")):
    ck("三级嵌套列表递增缩进",
       b["l3"][0][0] > b["l2"][0][0] > b["l1"][0][0], True)

# ============ 2. 属性处理 ============

d, rc = parse("<html><body><div title='a > b' id='x'>文</div></body></html>")
ck("属性值内含 >", rc == 0 and "a > b" in d, True)

d, rc = parse('<html><body><div title="say &quot;hi&quot;" id="x">文</div></body></html>')
ck("属性值内含 &quot;", rc == 0, True)

d, rc = parse("<html><body><div id=x class=a>文</div></body></html>")
ck("无引号属性值", rc == 0 and "id=" in d, True)

d, rc = parse("<html><body><input id='x' value=''></body></html>")
ck("空属性值", rc == 0, True)

d, rc = parse("<html><body><div id='first' id='second'>文</div></body></html>")
ck("重复属性不崩", rc == 0, True)

d, rc = parse("<html><body><div 数据='值' id='x'>文</div></body></html>")
ck("中文属性名", rc == 0 and "数据" in d, True)

d, rc = parse("<html><body><div data-a-b-c='1' aria-label='x' id='y'>文</div></body></html>")
ck("多连字符属性名", rc == 0 and "data-a-b-c" in d, True)

d, rc = parse("<html><body><div id='x' data-count='5'>文</div></body></html>")
ck("data-* 属性保留", rc == 0 and "data-count" in d, True)

b = boxes("<html><head><style>html,body{margin:0}</style></head><body>"
          "<div id='x' class='a b' style='width:100; height:30'></div></body></html>")
ck("style 里的空格不破坏解析", b.get("x", [None])[0], (0, 0, 100, 30))

b = boxes("<html><head><style>html,body{margin:0}</style></head><body>"
          "<input id='x' disabled placeholder='提示'></body></html>")
ck("布尔属性不破坏解析", "x" in b, True)

# ============ 3. 容错与规模 ============

deep = "<html><body>" + "<div>" * 500 + "x" + "</div>" * 500 + "</body></html>"
d, rc = parse(deep)
ck("500 层嵌套不崩", rc == 0, True)

many = "".join("<div id='n%d' style='width:10;height:10'></div>" % i for i in range(2000))
b = boxes("<html><head><style>html,body{margin:0}</style></head><body>%s</body></html>" % many)
ck("2000 个兄弟节点", len(b) >= 1900, True)

b = boxes("<html><head><style>html,body{margin:0}.p{width:200;font-size:14}</style></head><body>"
          "<div class='p'>%s</div></body></html>" % ("字" * 20000))
ck("2 万字符文本不崩", True, True)

d, rc = parse("<html>\r\n<body>\r\n<div id='x'>文</div>\r\n</body>\r\n</html>")
ck("CRLF 行尾", rc == 0 and 'id="x"' in d, True)

d, rc = parse("\ufeff<html><body><div id='x'>文</div></body></html>")
ck("BOM 前缀", rc == 0 and 'id="x"' in d, True)

b = boxes("<html><head><style>html,body{margin:0}/* 注释 */.a{width:100;height:30}</style>"
          "</head><body><div class='a' id='x'></div></body></html>")
ck("CSS 注释不破坏规则", b.get("x", [None])[0], (0, 0, 100, 30))

r = runs("<html><head><style>html,body{margin:0}</style></head><body>"
         "<script>var s = '</div>';</script><div>可见</div></body></html>")
ck("script 内的 </div> 不当标签", len(r) >= 1 and all("var" not in l for l in r), True)

# ============ 4. 空白语义(这块曾整块失效) ============
# DOM 必须保留原始空白, 折叠是布局期按 white-space 决定的 —— 与浏览器分工一致。

PRE = ("<html><head><style>html,body{margin:0;font-size:20}"
       ".p{width:600;white-space:%s}</style></head><body>"
       "<div class='p'>a   b</div></body></html>")

ck("white-space:pre 保留连续空格(3 片段)", run_count(PRE % "pre") >= 3, True)
ck("white-space:normal 折叠(2 片段)", run_count(PRE % "normal"), 2)
a_pre, a_nrm = run_advance(PRE % "pre"), run_advance(PRE % "normal")
ck("pre 的总推进更宽(证明空格真保留)", a_pre > a_nrm + 1.0, True)

# 折叠模式必须把换行/制表符也当空白(tokenizer 现在保留原始空白, 这一步容易漏)
nl = "<html><head><style>html,body{margin:0;font-size:14}</style></head><body><div>a\n\nb</div></body></html>"
ck("换行在 normal 下被折叠", run_count(nl), 2)
tab = "<html><head><style>html,body{margin:0;font-size:14}</style></head><body><div>a\tb</div></body></html>"
ck("制表符在 normal 下被折叠", run_count(tab), 2)

# pre 下换行必须**强制断行**(不只是保留为有宽度的片段) —— 换行符在字体里
# 宽度为 0, 若只当片段会全部画在同一行。
nlp = ("<html><head><style>html,body{margin:0;font-size:20}.p{white-space:pre}</style>"
       "</head><body><div class='p'>第一行\n第二行\n第三行</div></body></html>")
ck("pre 下换行断成 3 行", run_count(nlp), 3)
_bl = [float(m.group(1)) for m in
       (re.search(r"baseline=\s*(-?[0-9.]+)", l) for l in runs(nlp)) if m]
ck("pre 下三行基线递增(真换行)", len(_bl) == 3 and _bl[0] < _bl[1] < _bl[2], True)

# 对照: 折叠模式下换行变成空格(不断行) —— 3 个词, 但都在同一行(基线相同)
nlc = nlp.replace("white-space:pre", "white-space:normal")
_nb = [float(m.group(1)) for m in
       (re.search(r"baseline=\s*(-?[0-9.]+)", l) for l in runs(nlc)) if m]
ck("normal 下换行折成空格(3 个词)", run_count(nlc), 3)
ck("normal 下三词同一行(基线相同)", len(_b if (_b := _nb) else []) == 3 and len(set(_nb)) == 1, True)

# 行首/行尾空白在折叠模式下不影响首个片段位置
r = runs("<html><head><style>html,body{margin:0;font-size:14}</style></head><body>"
         "<div>   文本   </div></body></html>")
if r:
    ck("折叠模式下行首空白被忽略", re.search(r"x=\s*0\.0", r[0]) is not None, True)

# ============ 报告 ============
print("HTML 探针: 通过 %d / 失败 %d" % (len(passed), len(failed)))
for n, want, got in failed:
    print("  ✘ %-40s 期望 %s  实际 %s" % (n, want, got))
sys.exit(1 if failed else 0)

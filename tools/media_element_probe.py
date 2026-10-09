#!/usr/bin/env python3
"""媒体元素探针 —— <video>/<audio> 的 DOM 解析 / 状态机 / paint 指令。

用法:
    python3 tools/media_element_probe.py [hncore 路径]

数据源是 hncore(纯 C 引擎 CLI + 内置合成宿主, docs/media-design.md §8.1):
梯度帧按时间变色(逐字节可断言), 假时钟只认 tick(HN_CLOCK 16ms 分步),
因此断言是确定性的 —— 真解码(AVFoundation)的对应面由 tools/media_probe.c
在真机另测, 两者互补。

覆盖(契约 §3/§5/§8.1):
  DOM 解析   video/audio 属性(src/autoplay/loop/muted/volume/width/height)
  布局       替换元素尺寸链(样式 > 属性 > 会话帧固有 > 缺省 16:9; audio 高 0)
  状态机     IDLE/LOADING/READY/PLAYING/PAUSED/ENDED + autoplay/loop/
             seek 钳制/音量静音声明链
  paint      BITMAP 几何 = 元素盒, src=64x64, pts≈时钟, 像素=梯度公式;
             audio 永不发 BITMAP; 无帧画深底占位 RECT
"""
import os
import re
import subprocess
import sys
import tempfile

_raw = sys.argv[1] if len(sys.argv) > 1 else "dist/hncore-macos-arm64"
# 与 layout_probe 同一口径: 先按调用方 cwd 解析, 不存在再以仓库根为基准。
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


def _crash(t, v, _tb):
    print("探针异常退出: %s: %s" % (t.__name__, v))


sys.excepthook = _crash
D = tempfile.mkdtemp(prefix="hnmediaprobe-")
passed, failed = [], []


def ck(name, ok, got=""):
    if ok:
        passed.append(name)
    else:
        failed.append((name, "断言成立", got))


def run(html, env=None, w=400, h=300, cmd="paint"):
    """写临时文档 → 跑 hncore → 返回 stdout(附 HN_* 环境钮)。"""
    p = os.path.join(D, "case.html")
    with open(p, "w") as f:
        f.write(html)
    e = dict(os.environ)
    e.update(env or {})
    r = subprocess.run([HN, cmd, p, str(w), str(h)],
                       capture_output=True, text=True, env=e)
    return r.stdout


def media_lines(out):
    """MEDIA 行 → {id: {state,t,dur,vol,muted,loop}}(文档序, 后行覆盖同行 id)。"""
    res = {}
    for m in re.finditer(r"MEDIA\s+id=(\S+) state=(\S+) t=([\d.]+) dur=([\d.\-]+) "
                         r"vol=([\d.]+) muted=(\d) loop=(\d)", out):
        res[m.group(1)] = {
            "state": m.group(2), "t": float(m.group(3)), "dur": float(m.group(4)),
            "vol": float(m.group(5)), "muted": int(m.group(6)), "loop": int(m.group(7)),
        }
    return res


def bitmap_lines(out):
    """BITMAP 行 → [{x,y,w,h,sw,sh,pts,p00,pNN}](文档序)。
    字段间是列对齐的多个空格, 统一用 \\s+。"""
    res = []
    for m in re.finditer(
            r"BITMAP\s+([\d.\-]+)\s+([\d.\-]+)\s+([\d.\-]+)\s+([\d.\-]+)\s+"
            r"src=(\d+)x(\d+)\s+pts=([\d.\-]+)\s+p00=(\S+)\s+pNN=(\S+)", out):
        res.append({
            "x": float(m.group(1)), "y": float(m.group(2)),
            "w": float(m.group(3)), "h": float(m.group(4)),
            "sw": int(m.group(5)), "sh": int(m.group(6)),
            "pts": float(m.group(7)),
            "p00": tuple(int(v) for v in m.group(8).split(",")),
            "pNN": tuple(int(v) for v in m.group(9).split(",")),
        })
    return res


def boxes(html, w=400, h=300, env=None):
    out = run(html, env, w, h, cmd="boxes")
    res = {}
    for line in out.strip().splitlines()[1:]:
        parts = line.split(",")
        if len(parts) < 6 or not parts[1]:
            continue
        try:
            res[parts[1]] = tuple(float(v) for v in parts[2:6])
        except ValueError:
            continue
    return res


def near(name, a, b, tol):
    ck(name, all(abs(x - y) <= tol for x, y in zip(a, b)),
       "got %s want %s±%s" % (a, b, tol))


# ============ 1. DOM 解析(属性一个不少) ============
html = ('<html><body>'
        '<video id="v" src="clip.mp4" autoplay loop muted width="320" height="180"'
        ' data-duration="2"></video>'
        '<audio id="a" src="tone.m4a" volume="0.5" data-duration="1.5"></audio>'
        '</body></html>')
dom = run(html, cmd="parse")
ck("解析: video 属性齐全",
   'id="v"' in dom and 'src="clip.mp4"' in dom and 'autoplay=""' in dom
   and 'loop=""' in dom and 'muted=""' in dom
   and 'width="320"' in dom and 'height="180"' in dom, dom)
ck("解析: audio 属性齐全",
   'id="a"' in dom and 'src="tone.m4a"' in dom and 'volume="0.5"' in dom, dom)
# 无 id 的媒体元素也在(display:block 缺省 inline-block 由 UA CSS 给)
ck("解析: video/audio 是普通元素(非 void, 带闭合语义)", dom.count("<video") == 1 and dom.count("<audio") == 1)

# ============ 2. 布局: 替换元素尺寸链 ============
bx = boxes('<html><head><style>html,body{margin:0}video,audio{display:block}</style>'
           '</head><body><video id="v" src="c.mp4" width="320" height="180"></video>'
           '<video id="v2" src="c.mp4" width="160"></video>'
           '<audio id="a" src="t.m4a"></audio></body></html>')
near("布局: video 走 width/height 属性", bx.get("v", ("M",) * 4), (0, 0, 320, 180), 0.6)
if "v2" in bx:
    # 只有 width 属性: 高 = 宽 * 9/16(缺省 16:9)
    near("布局: video 缺省 16:9(160x90)", bx["v2"], (0, 180, 160, 90), 0.6)
else:
    ck("布局: video 缺省 16:9 存在", False, "v2 MISSING")
if "a" in bx:
    near("布局: audio 兜底高 0", bx["a"], (0, 270, 300, 0), 0.6)
else:
    ck("布局: audio 盒存在", False, "a MISSING")

# ============ 3. 状态机 ============
# 3.1 无 autoplay、无时钟: 会话惰性建立但停在 IDLE + 占位 RECT(非静默空白)
out = run('<html><body><video id="v" src="c.mp4" width="120" height="68"></video></body></html>')
ml = media_lines(out)
ck("状态机: 无 autoplay 停在 IDLE", ml.get("v", {}).get("state") == "IDLE", ml)
ck("状态机: IDLE 时长未知(-1)", ml.get("v", {}).get("dur") == -1.0, ml)
ck("绘制: IDLE 画占位 RECT(深底)", "fill=14171EFF" in out and "BITMAP" not in out, out)

# 3.2 autoplay、无时钟: open 了但元数据未就绪 → LOADING
out = run('<html><body><video id="v" src="c.mp4" autoplay></video></body></html>')
ck("状态机: autoplay 后 LOADING(时钟未推进)",
   media_lines(out).get("v", {}).get("state") == "LOADING", media_lines(out))

# 3.3 autoplay + 时钟推进: LOADING→READY→PLAYING, cur_time 随假时钟
out = run('<html><body><video id="v" src="c.mp4" autoplay data-duration="2"></video></body></html>',
          {"HN_CLOCK": "416"})
ml = media_lines(out)
ck("状态机: tick 后 PLAYING", ml.get("v", {}).get("state") == "PLAYING", ml)
ck("状态机: 时长来自 data-duration", ml.get("v", {}).get("dur") == 2.0, ml)
# 首个 tick 被 LOADING→READY 消耗(就绪后才起播), 25 个播放 tick = 400ms
near("状态机: cur_time≈假时钟", (ml.get("v", {}).get("t", -9),),
     (0.400,), 0.017)

# 3.4 播完(非循环)→ ENDED, 时间冻结在时长
out = run('<html><body><video id="v" src="c.mp4" autoplay data-duration="0.1"></video></body></html>',
          {"HN_CLOCK": "416"})
ml = media_lines(out)
ck("状态机: 播完 ENDED", ml.get("v", {}).get("state") == "ENDED", ml)
near("状态机: ENDED 时间=时长", (ml.get("v", {}).get("t", -9),), (0.100,), 0.001)

# 3.5 loop: 回绕后仍 PLAYING, 时间落在时长内
out = run('<html><body><video id="v" src="c.mp4" autoplay loop data-duration="0.1"></video></body></html>',
          {"HN_CLOCK": "416"})
ml = media_lines(out)
ck("状态机: loop 回绕后仍 PLAYING", ml.get("v", {}).get("state") == "PLAYING", ml)
ck("状态机: loop 时间落在时长内", 0 <= ml.get("v", {}).get("t", 9) < 0.1, ml)

# 3.6 seek 钳制(环境钮驱动引擎 API, 与 JS hnMediaSeek 同一入口)
base = '<html><body><video id="v" src="c.mp4" autoplay data-duration="0.5"></video></body></html>'
out = run(base, {"HN_CLOCK": "160", "HN_MEDIA_SEEK": "99"})
ck("seek: 越界钳制到时长", abs(media_lines(out).get("v", {}).get("t", -9) - 0.5) < 0.001,
   media_lines(out))
out = run(base, {"HN_CLOCK": "160", "HN_MEDIA_SEEK": "-3"})
ck("seek: 负值钳制到 0", abs(media_lines(out).get("v", {}).get("t", -9) - 0.0) < 0.001,
   media_lines(out))

# 3.7 pause → PAUSED(时间停在暂停时刻)
out = run(base, {"HN_CLOCK": "160", "HN_MEDIA_PAUSE": "1"})
ml = media_lines(out)
ck("状态机: pause → PAUSED", ml.get("v", {}).get("state") == "PAUSED", ml)
near("状态机: PAUSED 时间冻结", (ml.get("v", {}).get("t", -9),), (0.144,), 0.017)

# 3.8 音量/静音声明链: 元素属性 → 引擎钳制 → 宿主落位(回读)
out = run('<html><body><audio id="a" src="t.m4a" autoplay volume="0.5" muted'
          ' data-duration="2"></audio></body></html>', {"HN_CLOCK": "160"})
ml = media_lines(out)
ck("音量: volume=0.5 落到宿主", ml.get("a", {}).get("vol") == 0.5, ml)
ck("音量: muted 属性生效", ml.get("a", {}).get("muted") == 1, ml)
# 环境钮设音量(验引擎 0..1 钳制路径)
out = run('<html><body><video id="v" src="c.mp4" autoplay data-duration="2"></video></body></html>',
          {"HN_CLOCK": "160", "HN_MEDIA_VOLUME": "0.25"})
ck("音量: 运行期设 0.25", media_lines(out).get("v", {}).get("vol") == 0.25, media_lines(out))
out = run('<html><body><video id="v" src="c.mp4" autoplay data-duration="2"></video></body></html>',
          {"HN_CLOCK": "160", "HN_MEDIA_VOLUME": "7"})
ck("音量: 越界钳制到 1", media_lines(out).get("v", {}).get("vol") == 1.0, media_lines(out))
# 时长环境钮覆盖(真宿主读元数据的确定性替身)
out = run(base, {"HN_CLOCK": "160", "HN_MEDIA_DURATION": "1.5"})
ck("时长: HN_MEDIA_DURATION 覆盖", media_lines(out).get("v", {}).get("dur") == 1.5,
   media_lines(out))

# ============ 4. paint 指令 ============
blk = ('<html><head><style>html,body{margin:0}video{display:block}</style></head>'
       '<body><video id="v" src="c.mp4" autoplay data-duration="2"'
       ' width="200" height="150"></video></body></html>')
out = run(blk, {"HN_CLOCK": "416"})
bl = bitmap_lines(out)
ck("paint: 发出 BITMAP", len(bl) == 1, bl)
if bl:
    near("paint: BITMAP 几何 = 元素盒", (bl[0]["x"], bl[0]["y"], bl[0]["w"], bl[0]["h"]),
         (0, 0, 200, 150), 0.6)
    ck("paint: 帧源尺寸 64x64(合成宿主)", (bl[0]["sw"], bl[0]["sh"]) == (64, 64), bl[0])
    near("paint: BITMAP pts ≈ 时钟", (bl[0]["pts"],), (0.400,), 0.017)
    # 梯度帧逐字节: R=x*255/64, G=y*255/64, B=(int(pts*10)*37)%256
    # pts=0.400 → 桶 4 → B=148
    ck("paint: p00 逐字节(0,0,148,255)", bl[0]["p00"] == (0, 0, 148, 255), bl[0]["p00"])
    ck("paint: pNN 逐字节(251,251,148,255)", bl[0]["pNN"] == (251, 251, 148, 255), bl[0]["pNN"])
# pts 前进 → B 桶变色(800ms 时钟 → 49 播放 tick = 784ms → 桶 7 → B=3)
out = run(blk, {"HN_CLOCK": "800"})
bl = bitmap_lines(out)
if bl:
    ck("paint: pts 前进(784ms → 桶 7 → B=3)",
       bl[0]["p00"][2] == 3 and abs(bl[0]["pts"] - 0.784) <= 0.017, bl[0])
else:
    ck("paint: pts 前进(BITMAP 存在)", False, "no BITMAP")

# 4.1 audio 永不发 BITMAP(纯音频会话), 但状态机照常推进
out = run('<html><body><audio id="a" src="t.m4a" autoplay data-duration="2"></audio></body></html>',
          {"HN_CLOCK": "416"})
ck("audio: 无 BITMAP", "BITMAP" not in out, out)
ck("audio: 状态照常 PLAYING", media_lines(out).get("a", {}).get("state") == "PLAYING",
   media_lines(out))

# 4.2 双元素: 两个 BITMAP(几何各随其盒), MEDIA 行一个不少
out = run('<html><head><style>html,body{margin:0}video{display:block}</style></head><body>'
          '<video id="v1" src="c.mp4" autoplay width="120" height="68"></video>'
          '<video id="v2" src="c.mp4" autoplay width="80" height="45"></video>'
          '</body></html>', {"HN_CLOCK": "416"})
bl = bitmap_lines(out)
ck("paint: 两个 video 两条 BITMAP", len(bl) == 2, bl)
if len(bl) == 2:
    ck("paint: BITMAP#1 几何 120x68", (bl[0]["w"], bl[0]["h"]) == (120, 68), bl[0])
    ck("paint: BITMAP#2 几何 80x45", (bl[1]["w"], bl[1]["h"]) == (80, 45), bl[1])

# 4.3 确定性: 同文档同环境钮两次运行输出逐字节一致
outs = [run(blk, {"HN_CLOCK": "416"}) for _ in range(2)]
ck("确定性: 两次运行逐字节一致", outs[0] == outs[1])

# 4.4 无媒体元素的文档: 输出零 BITMAP/MEDIA 行(零激活)
out = run('<html><body><div id="d" style="width:50;height:30"></div></body></html>',
          {"HN_CLOCK": "416"})
ck("无媒体文档: 零 BITMAP/MEDIA 行", "BITMAP" not in out and "MEDIA" not in out, out)

# ============ 报告 ============
print("通过 %d / 失败 %d" % (len(passed), len(failed)))
for name, want, got in failed:
    print("  ✘ %-34s 期望 %s  实际 %s" % (name, want, got))
sys.exit(1 if failed else 0)

#!/usr/bin/env python3
# tools/gif_probe.py — GIF 动图全链路探针薄封装。

"""编译并运行 tools/gif_probe.c(引擎 + hnsoft, 与 hncore 同一条链接口径)。

夹具是**内嵌在 C 探针里的字节数组**, 由下面这段纯 Python LZW 编码器生成
(不依赖 PIL —— 门禁在任意机器上都能复现同一份字节)。生成器原样留档,
要改夹具时改这里, 并把输出的 C 数组替换回 tools/gif_probe.c 的夹具段。

---- 夹具生成器(标准 GIF89a 编码路径) ----
import struct

def lzw_encode(indices, min_code):
    clear = 1 << min_code; end = clear + 1
    code_size = min_code + 1
    d = {(i,): i for i in range(clear)}
    next_code = end + 1
    bits = []
    def emit(code, size):
        for i in range(size): bits.append((code >> i) & 1)
    emit(clear, code_size)
    buf = []
    for px in indices:
        cand = tuple(buf) + (px,)
        if cand in d: buf = list(cand)
        else:
            emit(d[tuple(buf)], code_size)
            d[cand] = next_code; next_code += 1
            if next_code > (1 << code_size) and code_size < 12: code_size += 1
            buf = [px]
            if next_code >= 4096:
                emit(clear, code_size)
                d = {(i,): i for i in range(clear)}
                next_code = end + 1; code_size = min_code + 1
    if buf: emit(d[tuple(buf)], code_size)
    emit(end, code_size)
    out = bytearray()
    for i in range(0, len(bits), 8):
        b = 0
        for j, bit in enumerate(bits[i:i+8]): b |= bit << j
        out.append(b)
    return bytes(out)

def make_gif(w, h, palette, frames, bg=0, loops=0):
    nbits = max(1, (len(palette)-1).bit_length()); ncol = 1 << nbits
    out = bytearray(b'GIF89a') + struct.pack('<HH', w, h) + bytes([0x80 | (nbits-1), bg, 0])
    for i in range(ncol):
        r,g,b = palette[i] if i < len(palette) else (0,0,0); out += bytes([r,g,b])
    if loops is not None:
        out += b'\\x21\\xff\\x0bNETSCAPE2.0\\x03\\x01' + struct.pack('<H', loops) + b'\\x00'
    for f in frames:
        gce = 1 if f.get('trans') is not None else 0
        out += bytes([0x21,0xf9,4,(f.get('disposal',0)<<2)|gce,
                      f['delay_cs'] & 0xff,(f['delay_cs']>>8)&0xff,(f.get('trans') or 0)&0xff,0])
        out += bytes([0x2c]) + struct.pack('<HHHH',0,0,w,h) + bytes([0x80|(nbits-1)])
        for i in range(ncol):
            r,g,b = palette[i] if i < len(palette) else (0,0,0); out += bytes([r,g,b])
        comp = lzw_encode(f['idx'], max(2,nbits))
        out += bytes([max(2,nbits)])
        for i in range(0,len(comp),255):
            ch=comp[i:i+255]; out += bytes([len(ch)])+ch
        out += b'\\x00'
    out += b'\\x3b'
    return bytes(out)

pal = [(255,0,0),(0,0,255),(0,255,0),(255,255,0)]
# DISP1: 两帧 disposal=1; DISP2: 三帧 disposal=2+1; TRANS: 首帧透明; LOOP3: 循环 3 次
---- 生成器结束 ----

校验(开发期做的, 不进门禁): 上述夹具与 PIL 解码结果逐像素一致 ——
包括 disposal 合成、透明索引与 NETSCAPE 循环计数字节序。

用法: python3 tools/gif_probe.py [二进制路径]
  二进制路径可给可不给: 给了且可执行且**确是本探针产物** → 直接运行;
  没给/不可用/不是探针 → 忽略该参数, 本机 cc 编译到临时目录再运行。
  身份判据: 探针 banner 字面量 "gif_probe" 必入 __TEXT。
"""

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENG = os.path.join(ROOT, "Sources", "CHtmlNative")

ENGINE_SRC = [
    "hn_arena.c", "hn_html.c", "hn_md.c", "hn_css.c", "hn_style.c",
    "hn_layout.c", "hn_paint.c", "hn_context.c", "hn_theme.c",
    "hn_json.c", "hn_lottie.c", "hn_mesh.c", "hn_png.c", "hn_gif.c",
    "hn_fmt.c",
    "hn_canvas.c", "hn_media.c", "hnsoft.c",
]

SOURCES = (
    [os.path.join(ROOT, "tools", "gif_probe.c")]
    + [os.path.join(ENG, f) for f in ENGINE_SRC]
)

INC = [
    "-I", os.path.join(ENG, "include"),
    "-I", ENG,
    "-I", "/opt/homebrew/include/freetype2",
]
LIBS = ["-L/opt/homebrew/lib", "-lfreetype", "-lm"]


def compile_probe(out_path: str) -> bool:
    cc = os.environ.get("CC", "cc")
    cmd = [cc, "-std=c99", "-O2", "-Wall", "-ffp-contract=off", *INC, *SOURCES, *LIBS,
           "-o", out_path]
    print("gif_probe.py: 编译:", " ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write("gif_probe.py: 编译失败\n")
        if r.stderr:
            sys.stderr.write(r.stderr[-4000:])
    return r.returncode == 0


def main() -> int:
    for s in SOURCES:
        if not os.path.isfile(s):
            print(f"gif_probe.py: FAIL 缺源文件 {s}")
            return 1

    bin_path = (sys.argv[1] if len(sys.argv) > 1 else "") or None
    if bin_path and os.path.isfile(bin_path) and os.access(bin_path, os.X_OK):
        with open(bin_path, "rb") as f:
            is_probe = b"gif_probe" in f.read()
        if not is_probe:
            print(f"gif_probe.py: {bin_path} 不是 gif_probe 产物, 忽略并重编")
            bin_path = None
    else:
        bin_path = None

    if bin_path is None:
        tmp = tempfile.mkdtemp(prefix="hngif-")
        bin_path = os.path.join(tmp, "hn_gif_probe")
        if not compile_probe(bin_path):
            return 1

    r = subprocess.run([bin_path], cwd=ROOT, capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    if r.stderr:
        sys.stderr.write(r.stderr)
    if r.returncode != 0:
        print(f"gif_probe.py: FAIL 退出码 {r.returncode}")
    else:
        print("gif_probe.py: OK")
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# tools/md_probe.py — Markdown 直染探针薄封装。
#
# 只做三件事: cc 编译 tools/md_probe.c(引擎 + hnsoft + FreeType, 与
# render_probe 同一链接口径) → 运行 → 透传退出码。语义断言全部在 .c 里,
# 本脚本不解析输出。不依赖 QuickJS/wasm3(直染是纯引擎语义, 不涉脚本桥)。
#
# 用法: python3 tools/md_probe.py [二进制路径]
#   二进制路径可给可不给: 给了且可执行且**确是本探针产物** → 直接运行
#   (门禁可复用已编译产物); 没给/不可用/不是探针 → 忽略该参数,
#   本机 cc 编译到临时目录再运行。
#   身份判据: 探针 banner 字面量 "md_probe" 必入 __TEXT。
#
# 手工编译(与脚本同一命令):
#   cc -std=c99 -O2 -Wall -ffp-contract=off \
#      -I Sources/CHtmlNative/include -I Sources/CHtmlNative \
#      -I /opt/homebrew/include/freetype2 \
#      tools/md_probe.c Sources/CHtmlNative/hn_*.c Sources/CHtmlNative/hnsoft.c \
#      -L/opt/homebrew/lib -lfreetype -lm -o /tmp/hn_md_probe
#   /tmp/hn_md_probe
#
# 门禁口径: 缺源/编译失败/运行失败/退出码非 0 → exit 1; 全过 → exit 0。

import glob
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ENG = os.path.join(ROOT, "Sources", "CHtmlNative")

ENGINE_SRC = [
    "hn_arena.c", "hn_html.c", "hn_md.c", "hn_css.c", "hn_style.c",
    "hn_layout.c", "hn_paint.c", "hn_context.c", "hn_theme.c", "hn_canvas.c",
    "hn_json.c", "hn_lottie.c", "hn_mesh.c", "hn_png.c", "hn_gif.c", "hn_media.c",
    "hn_fmt.c",
    "hnsoft.c",
]

SOURCES = [os.path.join(ROOT, "tools", "md_probe.c")] + \
          [os.path.join(ENG, f) for f in ENGINE_SRC]

INC = [
    "-I", os.path.join(ENG, "include"),
    "-I", ENG,
    "-I", "/opt/homebrew/include/freetype2",
]
LIBS = ["-L/opt/homebrew/lib", "-lfreetype", "-lm"]


def find_freetype_inc():
    """FreeType 头目录(homebrew 常见前缀); 找不到返回 None(编译会自己报)。"""
    for p in ("/opt/homebrew/include/freetype2", "/usr/local/include/freetype2"):
        if os.path.isfile(os.path.join(p, "ft2build.h")):
            return p
    return None


def compile_probe(out_path: str) -> bool:
    cc = os.environ.get("CC", "cc")
    ft = find_freetype_inc()
    inc = list(INC)
    if ft:
        inc += ["-I", ft]
    cmd = [cc, "-std=c99", "-O2", "-Wall", "-ffp-contract=off",
           *inc, *SOURCES, *LIBS, "-o", out_path]
    print("md_probe.py: 编译:", " ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write("md_probe.py: 编译失败\n")
        if r.stderr:
            sys.stderr.write(r.stderr[-4000:])
    return r.returncode == 0


def main() -> int:
    for s in SOURCES:
        if not os.path.isfile(s):
            print(f"md_probe.py: FAIL 缺源文件 {s}")
            return 1

    bin_path = (sys.argv[1] if len(sys.argv) > 1 else "") or None
    if bin_path and os.path.isfile(bin_path) and os.access(bin_path, os.X_OK):
        with open(bin_path, "rb") as f:
            is_probe = b"md_probe" in f.read()
        if not is_probe:
            print(f"md_probe.py: {bin_path} 不是 md_probe 产物, 忽略并重编")
            bin_path = None
    else:
        bin_path = None

    if bin_path is None:
        tmp = tempfile.mkdtemp(prefix="hnmd-")
        bin_path = os.path.join(tmp, "hn_md_probe")
        if not compile_probe(bin_path):
            return 1

    r = subprocess.run([bin_path], cwd=ROOT, capture_output=True, text=True)
    sys.stdout.write(r.stdout)
    if r.stderr:
        sys.stderr.write(r.stderr)
    if r.returncode != 0:
        print(f"md_probe.py: FAIL 退出码 {r.returncode}")
    else:
        print("md_probe.py: OK")
    return r.returncode


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env python3
# tools/quad3d_probe.py — quad3d_probe.c 的门禁薄封装。
#
# 只做三件事: 编译 tools/quad3d_probe.c + 引擎源 → 运行 → 透传退出码。
# 全部语义断言在 .c 里(display list / hnsoft 位图层面的确定性结论),
# 本脚本不解析输出。
#
# cairo 变体: pkg-config 找得到 cairo 时, 额外编一份 -DQUAD3D_HAVE_CAIRO
# + hn_cairo.c 再跑一遍 —— 用例 G(cairo 缝线)只在该变体里真正执行;
# 主运行里 G 恒为 skip。两份任一退出码非 0 → 本脚本 FAIL。
#
# 用法: python3 tools/quad3d_probe.py [二进制路径]
#   二进制路径可给可不给: 给了且可执行且**确是本探针产物** → 直接运行
#   (门禁可复用已编译产物, 跳过编译); 没给/不可用/不是探针 → 忽略该参数,
#   本机 cc 编译到临时目录再运行。
#   身份判据: 探针 banner 字面量 "quad3d_probe"(main 开头)必入 __TEXT ——
#   门禁习惯把 dist 引擎二进制(hncore)也传进来, 盲跑只会撞 usage/exit 2,
#   所以复用前先验身份(与 media_probe.py 同一口径)。
#
# FreeType: 有则真文本度量, 无则 -DHN_NO_TEXT 走引擎内置等宽估算 ——
# 断言不依赖文本度量, 两种构建语义一致。
#
# 门禁口径: 缺源/编译失败/运行失败/退出码非 0 → exit 1; 全过 → exit 0。

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROBE = os.path.join(ROOT, "tools", "quad3d_probe.c")
ENGINE = os.path.join(ROOT, "Sources", "CHtmlNative")

SOURCES = [PROBE] + [
    os.path.join(ENGINE, f)
    for f in ("hn_arena.c", "hn_html.c", "hn_css.c", "hn_style.c",
              "hn_layout.c", "hn_paint.c", "hn_context.c", "hn_theme.c",
              "hn_json.c", "hn_lottie.c", "hn_mesh.c", "hn_media.c",
              "hn_png.c", "hnsoft.c")
]

# cairo 变体(用例 G): 本机 pkg-config 找得到 cairo 才多编一份
# -DQUAD3D_HAVE_CAIRO + hn_cairo.c —— 探针里的 G 才真正执行; 找不到就跳过
# (探针自己打印 skip, 不算失败)。
CAIRO_SRC = os.path.join(ENGINE, "hn_cairo.c")


def pkg_config_cairo() -> list:
    """cairo 的 cflags+libs; 本机没有则返回空表"""
    try:
        r = subprocess.run(["pkg-config", "--cflags", "--libs", "cairo"],
                           capture_output=True, text=True)
    except OSError:
        return []
    return r.stdout.split() if r.returncode == 0 else []

# freetype2 头在 homebrew 的子目录里(与 tools/build-multiplatform.sh 同口径)
FT_INC = ""
FT_LIB = []
if os.path.isdir("/opt/homebrew/include/freetype2"):
    FT_INC = "-I/opt/homebrew/include/freetype2"
    FT_LIB = ["-L/opt/homebrew/lib", "-lfreetype"]
elif os.path.isdir("/usr/include/freetype2"):
    FT_INC = "-I/usr/include/freetype2"
    FT_LIB = ["-lfreetype"]


def cc_cmd(out_path: str, with_text: bool) -> list:
    cc = os.environ.get("CC", "cc")
    cmd = [cc, "-O2", "-std=c99",
           "-I", os.path.join(ENGINE, "include"),
           "-I", ENGINE]
    if with_text and FT_INC:
        cmd.append(FT_INC)
    else:
        cmd.append("-DHN_NO_TEXT")   # 无 FreeType: hnsoft 内置等宽估算
    cmd += SOURCES
    if with_text and FT_LIB:
        cmd += FT_LIB
    cmd += ["-lm", "-o", out_path]
    return cmd


def compile_probe(out_path: str, with_cairo: bool = False) -> bool:
    cairo = pkg_config_cairo()
    if with_cairo and not cairo:
        print("quad3d_probe.py: 本机无 cairo(pkg-config), 跳过 cairo 变体", flush=True)
        return False
    for with_text in (True, False):
        cmd = cc_cmd(out_path, with_text)
        if with_cairo:
            cmd += ["-DQUAD3D_HAVE_CAIRO"] + cairo + [CAIRO_SRC]
        print("quad3d_probe.py: 编译:", " ".join(cmd), flush=True)
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode == 0:
            return True
        sys.stderr.write("quad3d_probe.py: 编译失败%s\n"
                         % ("(带 FreeType)" if with_text else "(HN_NO_TEXT)"))
        if r.stderr:
            sys.stderr.write(r.stderr[-4000:])
    return False


def main() -> int:
    for s in SOURCES:
        if not os.path.isfile(s):
            print(f"quad3d_probe.py: FAIL 缺源文件 {s}")
            return 1

    bin_path = (sys.argv[1] if len(sys.argv) > 1 else "") or None
    if bin_path and os.path.isfile(bin_path) and os.access(bin_path, os.X_OK):
        with open(bin_path, "rb") as f:
            is_probe = b"quad3d_probe" in f.read()
        if is_probe:
            print(f"quad3d_probe.py: 复用现成二进制 {bin_path}", flush=True)
        else:
            print(f"quad3d_probe.py: 参数 {bin_path} 无探针标识(是别的产物), "
                  "忽略之, 走本机编译", flush=True)
            bin_path = None
    elif bin_path:
        print(f"quad3d_probe.py: 参数 {bin_path} 不是可执行文件, 忽略之, 走本机编译", flush=True)
        bin_path = None

    if bin_path is None:
        out = os.path.join(tempfile.gettempdir(), "quad3d_probe_bin")
        if not compile_probe(out):
            print("quad3d_probe.py: FAIL 编译失败")
            return 1
        bin_path = out

    print(f"quad3d_probe.py: 运行 {bin_path}", flush=True)
    r = subprocess.run([bin_path])
    rc = r.returncode
    if rc != 0:
        print(f"quad3d_probe.py: 主变体退出码 {rc}(非 0) —— 存在失败断言"
              "(修复 3D 边界/遗留清单前这是预期; 继续跑 cairo 变体以暴露全部结果)")

    # cairo 变体(用例 G): 本机有 cairo 才多跑一份; 任一退出码非 0 → FAIL。
    # 主变体已红也照跑 —— 一轮门禁同时暴露两份结果。
    if pkg_config_cairo():
        cout = os.path.join(tempfile.gettempdir(), "quad3d_probe_bin_cairo")
        if not compile_probe(cout, with_cairo=True):
            print("quad3d_probe.py: FAIL cairo 变体编译失败")
            return 1
        print(f"quad3d_probe.py: 运行 {cout}(cairo 变体, 用例 G 生效)", flush=True)
        r2 = subprocess.run([cout])
        if r2.returncode != 0:
            print(f"quad3d_probe.py: FAIL cairo 变体退出码 {r2.returncode}(非 0)")
            return 1
    else:
        print("quad3d_probe.py: 本机无 cairo —— 用例 G 在探针里以 skip 记")

    if rc != 0:
        print(f"quad3d_probe.py: FAIL 主变体退出码 {rc}(非 0)")
        return 1
    print("quad3d_probe.py: PASS(两份变体退出码均 0)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

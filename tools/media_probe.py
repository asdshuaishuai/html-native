#!/usr/bin/env python3
# tools/media_probe.py — 媒体真机探针薄封装(docs/media-design.md §8.2)。
#
# 只做三件事: 编译 tools/media_probe.c + platform/hnp_media_macos.c →
# 运行 → 断言退出码 0。语义断言全部在 .c 里, 本脚本不解析输出。
#
# 用法: python3 tools/media_probe.py [二进制路径]
#   二进制路径可给可不给: 给了且可执行且**确是本探针产物** → 直接运行
#   (门禁可复用已编译产物, 此时跳过编译); 没给/不可用/不是探针 → 忽略
#   该参数, 本机 cc 编译到临时目录再运行。
#   身份判据: 探针 banner 字面量 "media_probe"(media_probe.c main 开头)必入
#   __TEXT —— 门禁习惯把 dist 引擎二进制(hncore)也一并传进来, 盲跑只会
#   撞 usage / exit 2, 所以复用前先验身份。
#
# 门禁口径: 缺源/编译失败/运行失败/退出码非 0 → exit 1; 全过 → exit 0。

import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SOURCES = [
    os.path.join(ROOT, "tools", "media_probe.c"),
    os.path.join(ROOT, "platform", "hnp_media_macos.c"),
]


def compile_probe(out_path: str) -> bool:
    cc = os.environ.get("CC", "cc")
    cmd = [cc, "-O1", "-Wall",
           "-I", os.path.join(ROOT, "platform"),
           *SOURCES, "-o", out_path]
    print("media_probe.py: 编译:", " ".join(cmd))
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.stderr.write("media_probe.py: 编译失败\n")
        if r.stderr:
            sys.stderr.write(r.stderr[-4000:])
    return r.returncode == 0


def main() -> int:
    if sys.platform != "darwin":
        # 本探针是 macOS(AVFoundation)专属; 其他平台没有 hnp_media 实现
        # (supported()=0 留位), 被调到即是门禁配置错误。
        print(f"media_probe.py: FAIL 平台 {sys.platform} 非 macOS, "
              "本探针不适用")
        return 1

    for s in SOURCES:
        if not os.path.isfile(s):
            print(f"media_probe.py: FAIL 缺源文件 {s}")
            return 1

    bin_path = (sys.argv[1] if len(sys.argv) > 1 else "") or None  # 空串归一为 None
    if bin_path and os.path.isfile(bin_path) and os.access(bin_path, os.X_OK):
        with open(bin_path, "rb") as f:
            is_probe = b"media_probe" in f.read()
        if is_probe:
            print(f"media_probe.py: 复用现成二进制 {bin_path}")
        else:
            print(f"media_probe.py: 参数 {bin_path} 无探针标识(是别的产物), "
                  "忽略之, 走本机编译")
            bin_path = None
    elif bin_path:
        print(f"media_probe.py: 参数 {bin_path} 不是可执行文件, 忽略之, 走本机编译")
        bin_path = None

    if bin_path is None:
        out = os.path.join(tempfile.gettempdir(), "hnp_media_probe_bin")
        if not compile_probe(out):
            print("media_probe.py: FAIL 编译失败")
            return 1
        bin_path = out

    print(f"media_probe.py: 运行 {bin_path}")
    r = subprocess.run([bin_path])
    if r.returncode != 0:
        print(f"media_probe.py: FAIL 探针退出码 {r.returncode}(非 0)")
        return 1
    print("media_probe.py: PASS(退出码 0)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

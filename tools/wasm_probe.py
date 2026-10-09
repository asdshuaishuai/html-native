#!/usr/bin/env python3
# tools/wasm_probe.py — WASM 缝探针薄封装(WASM 实现缝, media-design §7 验证路径)。
#
# 只做三件事: 编译 tools/wasm_probe.c + vendor/wasm3 11 源 + hn_wasm.c →
# 运行 → 断言退出码 0(即 add(2,3)==5 等全部断言在 .c 里, 本脚本不解析
# 输出语义 —— 与 media_probe.py 同一口径)。
#
# 用法: python3 tools/wasm_probe.py [探针二进制] [demo .wasm 路径]
#   第 1 参数给了且可执行且带探针标识("wasm_probe" 字面量在 __TEXT)→ 直接
#   复用(门禁可复用已编译产物); 否则本机 cc 编译到临时目录再运行。
#   第 2 参数传给探针(demo 模块缺省 examples/wasm/add.wasm, 产物已入库)。
#
# 门禁口径: 缺源/缺 demo 模块/编译失败/运行失败/退出码非 0 → exit 1。

import glob
import os
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

C_SOURCES = [os.path.join(ROOT, "tools", "wasm_probe.c")]
C_SOURCES += sorted(glob.glob(os.path.join(ROOT, "vendor", "wasm3", "m3_*.c")))
C_SOURCES += [os.path.join(ROOT, "Sources", "CHtmlNative", "hn_wasm.c")]

# vendor 快照恰为契约 §7 的最小 11 源; glob 数目漂移说明 vendor 目录被动过
EXPECT_WASM3 = 11
DEMO_WASM = os.path.join(ROOT, "examples", "wasm", "add.wasm")


def main() -> int:
    wasm3 = [s for s in C_SOURCES if os.path.basename(s).startswith("m3_")]
    print(f"wasm_probe.py: wasm3 源 {len(wasm3)} 个(契约最小集 {EXPECT_WASM3})")
    if len(wasm3) != EXPECT_WASM3:
        print(f"wasm_probe.py: FAIL vendor/wasm3 源数 {len(wasm3)} != {EXPECT_WASM3}"
              "(vendor 目录与契约 §7 最小集不符)")
        return 1
    for s in C_SOURCES + [DEMO_WASM]:
        if not os.path.isfile(s):
            print(f"wasm_probe.py: FAIL 缺文件 {s}")
            return 1

    bin_path = sys.argv[1] if len(sys.argv) > 1 else None
    if bin_path and os.path.isfile(bin_path) and os.access(bin_path, os.X_OK):
        with open(bin_path, "rb") as f:
            is_probe = b"wasm_probe" in f.read()
        if is_probe:
            print(f"wasm_probe.py: 复用现成二进制 {bin_path}")
        else:
            print(f"wasm_probe.py: 参数 {bin_path} 无探针标识(是别的产物), "
                  "忽略之, 走本机编译")
            bin_path = None
    elif bin_path:
        print(f"wasm_probe.py: 参数 {bin_path} 不是可执行文件, 忽略之, 走本机编译")
        bin_path = None

    if bin_path is None:
        cc = os.environ.get("CC", "cc")
        out = os.path.join(tempfile.gettempdir(), "hn_wasm_probe_bin")
        cmd = [cc, "-std=c99", "-O1", "-Wall",
               "-I", os.path.join(ROOT, "Sources", "CHtmlNative", "include"),
               "-I", os.path.join(ROOT, "vendor", "wasm3"),
               *C_SOURCES, "-o", out]
        print("wasm_probe.py: 编译:", " ".join(cmd))
        r = subprocess.run(cmd, capture_output=True, text=True)
        if r.returncode != 0:
            print("wasm_probe.py: FAIL 编译失败")
            if r.stderr:
                print(r.stderr[-2000:])
            return 1
        if r.stderr.strip():
            print("wasm_probe.py: 编译警告:\n" + r.stderr)
        bin_path = out

    print(f"wasm_probe.py: 运行 {bin_path} {DEMO_WASM}")
    r = subprocess.run([bin_path, DEMO_WASM])
    if r.returncode != 0:
        print(f"wasm_probe.py: FAIL 探针退出码 {r.returncode}(非 0)")
        return 1
    print("wasm_probe.py: PASS(退出码 0, add(2,3)==5 已钉死)")
    return 0


if __name__ == "__main__":
    sys.exit(main())

#!/usr/bin/env bash
# build-wk-macos.sh — 构建 macOS WebView 画板壳(纯 C99, dlopen WebKit/AppKit, 零链接框架)
# -fblocks: WKSnapshot 的 completionHandler 需要(clang 的 C 扩展, 不引入 ObjC 编译单元)
set -euo pipefail
cd "$(dirname "$0")/.."
mkdir -p dist
case "$(uname -m)" in
    arm64|aarch64) OUT=dist/hnweb-wk-macos-arm64 ;;
    x86_64)        OUT=dist/hnweb-wk-macos-x86_64 ;;
    *)             OUT="dist/hnweb-wk-macos-$(uname -m)" ;;
esac
cc -std=c99 -fblocks -O2 -Wall -ffp-contract=off tools/hnweb_wk_macos.c -o "$OUT"
echo "✓ $OUT"
